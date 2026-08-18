/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "ota_port.h"

static const char *TAG = "espos_ota";

static const char *state_name(esp_ota_img_states_t s)
{
    switch (s) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "undefined";
    }
}

void espos_ota_port_info(espos_ota_port_info_t *out)
{
    memset(out, 0, sizeof(*out));
    const esp_app_desc_t *d = esp_app_get_description();
    snprintf(out->version, sizeof(out->version), "%s", d->version);
    snprintf(out->project, sizeof(out->project), "%s", d->project_name);
    snprintf(out->idf, sizeof(out->idf), "%s", d->idf_ver);
    snprintf(out->date, sizeof(out->date), "%s", d->date);
    snprintf(out->time, sizeof(out->time), "%s", d->time);
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (run) {
        snprintf(out->slot, sizeof(out->slot), "%s", run->label);
        esp_ota_img_states_t st;
        if (esp_ota_get_state_partition(run, &st) == ESP_OK) {
            snprintf(out->state, sizeof(out->state), "%s", state_name(st));
            out->pending_verify = st == ESP_OTA_IMG_PENDING_VERIFY;
        } else {
            snprintf(out->state, sizeof(out->state), "undefined");
        }
    }
    if (next) {
        snprintf(out->other_slot, sizeof(out->other_slot), "%s", next->label);
        esp_app_desc_t od;
        if (esp_ota_get_partition_description(next, &od) == ESP_OK) {
            snprintf(out->other_version, sizeof(out->other_version), "%s", od.version);
        }
        esp_ota_img_states_t st;
        if (esp_ota_get_state_partition(next, &st) == ESP_OK && (st == ESP_OTA_IMG_INVALID || st == ESP_OTA_IMG_ABORTED)) {
            out->rolled_back = true;
        }
    }
}

static void http_cfg(esp_http_client_config_t *c, const char *url, bool allow_insecure)
{
    memset(c, 0, sizeof(*c));
    c->url = url;
    c->timeout_ms = 15000;
    c->keep_alive_enable = true;
    c->buffer_size = 2048;
    c->buffer_size_tx = 1024;
    if (allow_insecure) {
        c->skip_cert_common_name_check = true;
        c->crt_bundle_attach = NULL;
    } else {
        c->crt_bundle_attach = esp_crt_bundle_attach;
    }
}

esp_err_t espos_ota_port_install(const char *url, bool allow_insecure, const char *expect_project,
                                 espos_ota_progress_cb_t cb, void *arg, char *err_text, size_t err_size)
{
    esp_http_client_config_t hc;
    http_cfg(&hc, url, allow_insecure);
    esp_https_ota_config_t oc = {
        .http_config = &hc,
    };
    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&oc, &h);
    if (err != ESP_OK) {
        snprintf(err_text, err_size, "connect failed: %s", esp_err_to_name(err));
        return err;
    }
    esp_app_desc_t desc;
    err = esp_https_ota_get_img_desc(h, &desc);
    if (err != ESP_OK) {
        snprintf(err_text, err_size, "not a firmware image: %s", esp_err_to_name(err));
        esp_https_ota_abort(h);
        return err;
    }
    if (expect_project && *expect_project && strcmp(desc.project_name, expect_project) != 0) {
        snprintf(err_text, err_size, "image is '%s', this device runs '%s'", desc.project_name, expect_project);
        esp_https_ota_abort(h);
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "downloading %s %s (%s)", desc.project_name, desc.version, url);
    int total = esp_https_ota_get_image_size(h);
    while (1) {
        err = esp_https_ota_perform(h);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        if (cb) {
            cb((size_t)esp_https_ota_get_image_len_read(h), total > 0 ? (size_t)total : 0, arg);
        }
    }
    if (err != ESP_OK) {
        snprintf(err_text, err_size, "download failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(h);
        return err;
    }
    if (!esp_https_ota_is_complete_data_received(h)) {
        snprintf(err_text, err_size, "incomplete image");
        esp_https_ota_abort(h);
        return ESP_ERR_INVALID_SIZE;
    }
    if (cb) {
        cb((size_t)esp_https_ota_get_image_len_read(h), (size_t)esp_https_ota_get_image_len_read(h), arg);
    }
    /* finish() validates the image (SHA-256, and the signature when
     * SECURE_SIGNED_ON_UPDATE is on) and sets the boot partition. */
    err = esp_https_ota_finish(h);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            snprintf(err_text, err_size, "image rejected: bad signature or corrupt (%s)", esp_err_to_name(err));
        } else {
            snprintf(err_text, err_size, "finish failed: %s", esp_err_to_name(err));
        }
        return err;
    }
    ESP_LOGI(TAG, "image %s installed, boot partition switched", desc.version);
    return ESP_OK;
}

esp_err_t espos_ota_port_fetch(const char *url, bool allow_insecure, size_t max, char **out, size_t *len, int *status)
{
    esp_http_client_config_t hc;
    http_cfg(&hc, url, allow_insecure);
    hc.keep_alive_enable = false;
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) {
        return ESP_ERR_NO_MEM;
    }
    *out = NULL;
    *len = 0;
    *status = 0;
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(c);
        return err;
    }
    int64_t clen = esp_http_client_fetch_headers(c);
    *status = esp_http_client_get_status_code(c);
    if (*status != 200) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }
    size_t cap = clen > 0 && (size_t)clen < max ? (size_t)clen + 1 : 2048;
    char *buf = malloc(cap);
    if (!buf) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_ERR_NO_MEM;
    }
    size_t n = 0;
    while (1) {
        if (n + 1 >= cap) {
            if (cap >= max) {
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            size_t ncap = cap * 2 > max ? max : cap * 2;
            char *nb = realloc(buf, ncap);
            if (!nb) {
                err = ESP_ERR_NO_MEM;
                break;
            }
            buf = nb;
            cap = ncap;
        }
        int r = esp_http_client_read(c, buf + n, (int)(cap - n - 1));
        if (r < 0) {
            err = ESP_FAIL;
            break;
        }
        if (r == 0) {
            break;
        }
        n += (size_t)r;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    buf[n] = '\0';
    *out = buf;
    *len = n;
    return ESP_OK;
}

esp_err_t espos_ota_port_mark_valid(void)
{
    return esp_ota_mark_app_valid_cancel_rollback();
}

esp_err_t espos_ota_port_mark_invalid_and_reboot(void)
{
    return esp_ota_mark_app_invalid_rollback_and_reboot();
}

void espos_ota_port_reboot(void)
{
    esp_restart();
}

uint32_t espos_ota_port_uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

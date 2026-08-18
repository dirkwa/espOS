/*
 * SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * Host stand-in: downloads really happen (esp_http_client works on linux)
 * but nothing is written; the "flash" state is a few variables driven by
 * environment variables so the harness can test the policy:
 *   ESPOS_SIM_OTA_PENDING=1     boot as PENDING_VERIFY (rollback armed)
 *   ESPOS_SIM_OTA_PROJECT=name  project name the sim image "is" (default espos)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_http_client.h"
#include "esp_log.h"

#include "ota_port.h"

static const char *TAG = "espos_ota";
static bool s_pending = false, s_init = false, s_rolled_back = false, s_invalidated = false;
static int s_reboots;

static void init(void)
{
    if (s_init) {
        return;
    }
    s_init = true;
    const char *p = getenv("ESPOS_SIM_OTA_PENDING");
    s_pending = p && *p == '1';
}

void espos_ota_port_info(espos_ota_port_info_t *out)
{
    init();
    memset(out, 0, sizeof(*out));
    const char *v = getenv("ESPOS_SIM_OTA_VERSION");
    snprintf(out->version, sizeof(out->version), "%s", v && *v ? v : "0.6.0");
    const char *pn = getenv("ESPOS_SIM_OTA_PROJECT");
    snprintf(out->project, sizeof(out->project), "%s", pn && *pn ? pn : "espos");
    snprintf(out->idf, sizeof(out->idf), "sim");
    snprintf(out->slot, sizeof(out->slot), "ota_0");
    snprintf(out->other_slot, sizeof(out->other_slot), "ota_1");
    snprintf(out->state, sizeof(out->state), "%s", s_invalidated ? "invalid" : s_pending ? "pending_verify" : "valid");
    out->pending_verify = s_pending && !s_invalidated;
    out->rolled_back = s_rolled_back;
}

esp_err_t espos_ota_port_install(const char *url, bool allow_insecure, const char *expect_project,
                                 espos_ota_progress_cb_t cb, void *arg, char *err_text, size_t err_size)
{
    (void)allow_insecure;
    esp_http_client_config_t hc = { .url = url, .timeout_ms = 10000, .buffer_size = 2048 };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) {
        snprintf(err_text, err_size, "no client");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        snprintf(err_text, err_size, "connect failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return err;
    }
    int64_t clen = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        snprintf(err_text, err_size, "HTTP %d", status);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }
    char buf[1024];
    size_t got = 0;
    bool checked = false;
    while (1) {
        int r = esp_http_client_read(c, buf, sizeof(buf));
        if (r < 0) {
            snprintf(err_text, err_size, "download failed");
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
            return ESP_FAIL;
        }
        if (r == 0) {
            break;
        }
        if (!checked) {
            /* The sim "image format": first line "ESPOS-IMAGE <project> <version>" */
            checked = true;
            char proj[32] = "", ver[32] = "";
            if (sscanf(buf, "ESPOS-IMAGE %31s %31s", proj, ver) != 2) {
                snprintf(err_text, err_size, "not a firmware image");
                esp_http_client_close(c);
                esp_http_client_cleanup(c);
                return ESP_ERR_INVALID_ARG;
            }
            if (expect_project && *expect_project && strcmp(proj, expect_project) != 0) {
                snprintf(err_text, err_size, "image is '%s', this device runs '%s'", proj, expect_project);
                esp_http_client_close(c);
                esp_http_client_cleanup(c);
                return ESP_ERR_INVALID_ARG;
            }
            if (strstr(buf, "BADSIG")) {
                /* consume the rest, then fail like esp_https_ota_finish would */
                while (esp_http_client_read(c, buf, sizeof(buf)) > 0) {}
                snprintf(err_text, err_size, "image rejected: bad signature or corrupt (ESP_ERR_OTA_VALIDATE_FAILED)");
                esp_http_client_close(c);
                esp_http_client_cleanup(c);
                return ESP_ERR_INVALID_CRC;   /* stands in for ESP_ERR_OTA_VALIDATE_FAILED */
            }
        }
        got += (size_t)r;
        if (cb) {
            cb(got, clen > 0 ? (size_t)clen : 0, arg);
        }
        struct timespec ts = { 0, 20 * 1000 * 1000 };
        nanosleep(&ts, NULL);   /* let the harness observe progress */
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (clen > 0 && got != (size_t)clen) {
        snprintf(err_text, err_size, "incomplete image");
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "sim: image installed (%u bytes), boot partition switched", (unsigned)got);
    return ESP_OK;
}

esp_err_t espos_ota_port_fetch(const char *url, bool allow_insecure, size_t max, char **out, size_t *len, int *status)
{
    (void)allow_insecure;
    esp_http_client_config_t hc = { .url = url, .timeout_ms = 10000, .buffer_size = 2048 };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    *out = NULL;
    *len = 0;
    *status = 0;
    if (!c) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(c);
        return err;
    }
    esp_http_client_fetch_headers(c);
    *status = esp_http_client_get_status_code(c);
    if (*status != 200) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }
    char *buf = malloc(max);
    size_t n = 0;
    while (buf && n + 1 < max) {
        int r = esp_http_client_read(c, buf + n, (int)(max - n - 1));
        if (r <= 0) {
            break;
        }
        n += (size_t)r;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    buf[n] = '\0';
    *out = buf;
    *len = n;
    return ESP_OK;
}

esp_err_t espos_ota_port_mark_valid(void)
{
    init();
    ESP_LOGI(TAG, "sim: image marked valid");
    s_pending = false;
    return ESP_OK;
}

esp_err_t espos_ota_port_mark_invalid_and_reboot(void)
{
    init();
    ESP_LOGW(TAG, "sim: image marked invalid, rolling back (reboot #%d)", ++s_reboots);
    s_invalidated = true;
    s_rolled_back = true;
    return ESP_OK;
}

void espos_ota_port_reboot(void)
{
    ESP_LOGW(TAG, "sim: reboot #%d requested", ++s_reboots);
}

uint32_t espos_ota_port_uptime_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    static time_t base = -1;
    if (base < 0) {
        base = ts.tv_sec;
    }
    return (uint32_t)(ts.tv_sec - base);
}

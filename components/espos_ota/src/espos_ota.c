/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * OTA task: manifest checks, installs, and the confirm/rollback policy for
 * a freshly booted image. One task, one command queue; blocking downloads
 * happen on this task so the HTTP server never stalls.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "espos_cfg_keys.h"
#include "espos_config.h"
#include "espos_httpd_sse.h"
#include "espos_ota.h"
#include "espos_ota_manifest.h"
#include "espos_wifi.h"
#include "ota_port.h"

static const char *TAG = "espos_ota";

#define CMD_CHECK        1
#define CMD_INSTALL_URL  2
#define CMD_INSTALL_AVAIL 3
#define CMD_CONFIRM      4
#define CMD_ROLLBACK     5
#define CMD_STOP         6

#define MANIFEST_MAX     (16 * 1024)
#define BOOT_CHECK_DELAY_S 20      /* let WiFi come up before the first manifest fetch */
#define NO_STA_GRACE_S    60       /* confirm without station config after this */

typedef struct {
    int cmd;
    char url[ESPOS_OTA_URL_MAX];
} cmd_t;

static struct {
    SemaphoreHandle_t lock;
    QueueHandle_t q;
    TaskHandle_t task;
    espos_ota_state_t state;
    char last_error[96];
    espos_ota_build_t avail;
    bool have_avail;
    size_t received, total;
    uint32_t last_check_at;    /* uptime s, 0 = never */
    uint32_t next_check_at;
    bool ever_connected;
    bool confirmed_this_boot;
    /* config snapshot */
    char manifest_url[168];
    char channel[16];
    bool auto_check, auto_install, allow_insecure;
    int32_t check_h, confirm_tmo_s;
    bool cfg_dirty;
} s;

static void lock(void) { xSemaphoreTake(s.lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s.lock); }

const char *espos_ota_state_name(espos_ota_state_t st)
{
    switch (st) {
    case ESPOS_OTA_IDLE: return "idle";
    case ESPOS_OTA_CHECKING: return "checking";
    case ESPOS_OTA_AVAILABLE: return "available";
    case ESPOS_OTA_DOWNLOADING: return "downloading";
    case ESPOS_OTA_VERIFYING: return "verifying";
    case ESPOS_OTA_READY: return "ready";
    case ESPOS_OTA_FAILED: return "failed";
    default: return "?";
    }
}

static void load_config(void)
{
    lock();
    espos_config_get_str(ESPOS_CFG_NS_OTA, ESPOS_CFG_OTA_MANIFEST_URL, s.manifest_url, sizeof(s.manifest_url), NULL);
    espos_config_get_str(ESPOS_CFG_NS_OTA, ESPOS_CFG_OTA_CHANNEL, s.channel, sizeof(s.channel), NULL);
    espos_config_get_bool(ESPOS_CFG_NS_OTA, ESPOS_CFG_OTA_AUTO_CHECK, &s.auto_check);
    espos_config_get_bool(ESPOS_CFG_NS_OTA, ESPOS_CFG_OTA_AUTO_INSTALL, &s.auto_install);
    espos_config_get_bool(ESPOS_CFG_NS_OTA, ESPOS_CFG_OTA_ALLOW_INSECURE, &s.allow_insecure);
    espos_config_get_i32(ESPOS_CFG_NS_OTA, ESPOS_CFG_OTA_CHECK_H, &s.check_h);
    espos_config_get_i32(ESPOS_CFG_NS_OTA, ESPOS_CFG_OTA_CONFIRM_TMO_S, &s.confirm_tmo_s);
    if (!s.channel[0]) {
        strcpy(s.channel, "stable");
    }
    s.cfg_dirty = false;
    unlock();
}

static void on_config(const char *ns, const char *key, void *arg)
{
    (void)key; (void)arg;
    if (strcmp(ns, ESPOS_CFG_NS_OTA) == 0) {
        s.cfg_dirty = true;
    }
}

static void publish(void)
{
    char *j = espos_ota_status_json();
    if (j) {
        espos_httpd_sse_publish("ota", j);
        free(j);
    }
}

static void set_state(espos_ota_state_t st, const char *err)
{
    lock();
    s.state = st;
    if (err) {
        snprintf(s.last_error, sizeof(s.last_error), "%s", err);
    } else if (st != ESPOS_OTA_FAILED) {
        s.last_error[0] = '\0';
    }
    unlock();
    publish();
}

static void progress_cb(size_t received, size_t total, void *arg)
{
    (void)arg;
    bool notify = false;
    lock();
    /* publish every ~32 KiB or on completion, not on every chunk */
    if (received == total || received / 32768 != s.received / 32768) {
        notify = true;
    }
    s.received = received;
    s.total = total;
    unlock();
    if (notify) {
        publish();
    }
}

static void do_check(void)
{
    char url[168], channel[16];
    bool insecure, auto_install;
    lock();
    snprintf(url, sizeof(url), "%s", s.manifest_url);
    snprintf(channel, sizeof(channel), "%s", s.channel);
    insecure = s.allow_insecure;
    auto_install = s.auto_install;
    s.last_check_at = espos_ota_port_uptime_s();
    s.next_check_at = s.last_check_at + (uint32_t)s.check_h * 3600;
    unlock();
    if (!url[0]) {
        set_state(ESPOS_OTA_FAILED, "no manifest URL configured");
        return;
    }
    set_state(ESPOS_OTA_CHECKING, NULL);
    char *body = NULL;
    size_t len = 0;
    int status = 0;
    esp_err_t err = espos_ota_port_fetch(url, insecure, MANIFEST_MAX, &body, &len, &status);
    if (err != ESP_OK) {
        char e[96];
        if (status) {
            snprintf(e, sizeof(e), "manifest: HTTP %d", status);
        } else {
            snprintf(e, sizeof(e), "manifest: %s", esp_err_to_name(err));
        }
        set_state(ESPOS_OTA_FAILED, e);
        return;
    }
    espos_ota_port_info_t info;
    espos_ota_port_info(&info);
    espos_ota_build_t b;
    err = espos_ota_manifest_pick(body, len, url, info.project, CONFIG_IDF_TARGET, channel, info.version, &b);
    free(body);
    if (err == ESP_ERR_NOT_FOUND) {
        lock();
        s.have_avail = false;
        unlock();
        char e[96];
        snprintf(e, sizeof(e), "no build for %s/%s in the manifest", CONFIG_IDF_TARGET, channel);
        set_state(ESPOS_OTA_FAILED, e);
        return;
    }
    if (err != ESP_OK) {
        set_state(ESPOS_OTA_FAILED, "manifest: malformed");
        return;
    }
    lock();
    s.avail = b;
    s.have_avail = true;
    unlock();
    if (!b.newer) {
        ESP_LOGI(TAG, "manifest: %s is the newest for %s/%s, running %s", b.version, CONFIG_IDF_TARGET, channel, info.version);
        set_state(ESPOS_OTA_IDLE, NULL);
        return;
    }
    ESP_LOGI(TAG, "manifest: %s available (running %s)%s", b.version, info.version, auto_install ? ", installing" : "");
    set_state(ESPOS_OTA_AVAILABLE, NULL);
    if (auto_install) {
        cmd_t c = { .cmd = CMD_INSTALL_AVAIL };
        xQueueSend(s.q, &c, 0);
    }
}

static void do_install(const char *url)
{
    bool insecure;
    espos_ota_port_info_t info;
    espos_ota_port_info(&info);
    lock();
    insecure = s.allow_insecure;
    s.received = 0;
    s.total = 0;
    unlock();
    set_state(ESPOS_OTA_DOWNLOADING, NULL);
    char err_text[96] = "";
    esp_err_t err = espos_ota_port_install(url, insecure, info.project, progress_cb, NULL, err_text, sizeof(err_text));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "install failed: %s", err_text);
        set_state(ESPOS_OTA_FAILED, err_text);
        return;
    }
    set_state(ESPOS_OTA_READY, NULL);
    ESP_LOGW(TAG, "update installed, rebooting");
    vTaskDelay(pdMS_TO_TICKS(1500));    /* let the status/SSE reach the client */
    espos_ota_port_reboot();
    /* Only the host simulator returns from here. */
    set_state(ESPOS_OTA_IDLE, NULL);
}

/* Confirm-or-rollback policy for a PENDING_VERIFY image; called once a second. */
static void tick_confirm(void)
{
    espos_ota_port_info_t info;
    espos_ota_port_info(&info);
    if (!info.pending_verify || s.confirmed_this_boot) {
        return;
    }
    espos_wifi_status_t w;
    bool wifi_ok = espos_wifi_get_status(&w) == ESP_OK;
    uint32_t up = espos_ota_port_uptime_s();
    bool connected = wifi_ok && w.sm.state == ESPOS_WIFI_ST_CONNECTED;
    bool no_station = wifi_ok && (w.sm.state == ESPOS_WIFI_ST_UNCONFIGURED || w.sm.state == ESPOS_WIFI_ST_DISABLED);
    if (connected || (no_station && up >= NO_STA_GRACE_S)) {
        ESP_LOGI(TAG, "new image confirmed (%s), rollback cancelled", connected ? "network up" : "no station configured");
        if (espos_ota_port_mark_valid() == ESP_OK) {
            s.confirmed_this_boot = true;
            publish();
        }
        return;
    }
    if (up >= (uint32_t)s.confirm_tmo_s) {
        ESP_LOGE(TAG, "new image could not reach the network in %u s: rolling back", (unsigned)up);
        set_state(ESPOS_OTA_FAILED, "rollback: network not reached in time");
        espos_ota_port_mark_invalid_and_reboot();
        s.confirmed_this_boot = true;   /* sim: do not repeat */
    }
}

static void ota_task(void *arg)
{
    (void)arg;
    load_config();
    uint32_t boot_check_at = espos_ota_port_uptime_s() + BOOT_CHECK_DELAY_S;
    bool boot_check_done = false;
    while (1) {
        cmd_t c;
        if (xQueueReceive(s.q, &c, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (s.cfg_dirty) {
                load_config();      /* a PUT just before the command must count */
            }
            switch (c.cmd) {
            case CMD_CHECK: do_check(); break;
            case CMD_INSTALL_URL: do_install(c.url); break;
            case CMD_INSTALL_AVAIL: {
                char url[ESPOS_OTA_URL_MAX];
                lock();
                bool have = s.have_avail;
                snprintf(url, sizeof(url), "%s", s.avail.url);
                unlock();
                if (have) {
                    do_install(url);
                } else {
                    set_state(ESPOS_OTA_FAILED, "nothing to install: check first");
                }
                break;
            }
            case CMD_CONFIRM:
                if (espos_ota_port_mark_valid() == ESP_OK) {
                    s.confirmed_this_boot = true;
                }
                publish();
                break;
            case CMD_ROLLBACK:
                ESP_LOGW(TAG, "rollback requested");
                set_state(ESPOS_OTA_FAILED, "rollback requested");
                espos_ota_port_mark_invalid_and_reboot();
                break;
            case CMD_STOP:
                s.task = NULL;
                vTaskDelete(NULL);
                return;
            }
            continue;
        }
        if (s.cfg_dirty) {
            load_config();
        }
        tick_confirm();
        /* periodic manifest checks: after boot once WiFi is up, then every check_h */
        espos_wifi_status_t w;
        bool up = espos_wifi_get_status(&w) == ESP_OK && w.sm.state == ESPOS_WIFI_ST_CONNECTED;
        uint32_t now = espos_ota_port_uptime_s();
        lock();
        bool auto_check = s.auto_check && s.manifest_url[0];
        bool due = auto_check && up && ((!boot_check_done && now >= boot_check_at) || (boot_check_done && s.next_check_at && now >= s.next_check_at));
        bool busy = s.state == ESPOS_OTA_DOWNLOADING || s.state == ESPOS_OTA_VERIFYING || s.state == ESPOS_OTA_READY;
        unlock();
        if (due && !busy) {
            boot_check_done = true;
            do_check();
        }
    }
}

esp_err_t espos_ota_register_api(void);

esp_err_t espos_ota_start(void)
{
    if (s.task) {
        return ESP_OK;
    }
    if (!s.lock) {
        s.lock = xSemaphoreCreateMutex();
        s.q = xQueueCreate(4, sizeof(cmd_t));
        if (!s.lock || !s.q) {
            return ESP_ERR_NO_MEM;
        }
        espos_config_subscribe(on_config, NULL);
        esp_err_t err = espos_ota_register_api();
        if (err != ESP_OK) {
            return err;
        }
    }
    espos_ota_port_info_t info;
    espos_ota_port_info(&info);
    ESP_LOGI(TAG, "running %s %s in %s (%s)%s", info.project, info.version, info.slot, info.state,
             info.rolled_back ? " — previous update was rolled back" : "");
    /* 8 KB words on the simulator would overflow the 16-bit depth; bytes on chips. */
    const uint32_t stack = configMINIMAL_STACK_SIZE > 3072 ? configMINIMAL_STACK_SIZE * 2 : 8192;
    if (xTaskCreate(ota_task, "espos_ota", stack, NULL, tskIDLE_PRIORITY + 3, &s.task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void espos_ota_stop(void)
{
    if (s.task) {
        cmd_t c = { .cmd = CMD_STOP };
        xQueueSend(s.q, &c, portMAX_DELAY);
        while (s.task) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

static esp_err_t send_cmd(int cmd, const char *url)
{
    if (!s.q) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    bool busy = s.state == ESPOS_OTA_CHECKING || s.state == ESPOS_OTA_DOWNLOADING || s.state == ESPOS_OTA_VERIFYING || s.state == ESPOS_OTA_READY;
    unlock();
    if (busy && cmd != CMD_CONFIRM) {
        return ESP_ERR_INVALID_STATE;
    }
    cmd_t c = { .cmd = cmd };
    if (url) {
        snprintf(c.url, sizeof(c.url), "%s", url);
    }
    return xQueueSend(s.q, &c, 0) == pdTRUE ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t espos_ota_check_now(void)
{
    return send_cmd(CMD_CHECK, NULL);
}

esp_err_t espos_ota_install_url(const char *url)
{
    if (!url || strlen(url) >= ESPOS_OTA_URL_MAX || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_cmd(CMD_INSTALL_URL, url);
}

esp_err_t espos_ota_install_available(void)
{
    lock();
    bool have = s.have_avail;
    unlock();
    if (!have) {
        return ESP_ERR_NOT_FOUND;
    }
    return send_cmd(CMD_INSTALL_AVAIL, NULL);
}

esp_err_t espos_ota_confirm(void)
{
    return send_cmd(CMD_CONFIRM, NULL);
}

esp_err_t espos_ota_rollback(void)
{
    return send_cmd(CMD_ROLLBACK, NULL);
}

static void json_str(char *dst, size_t n, const char *src)
{
    size_t o = 0;
    for (; *src && o + 2 < n; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            if (o + 3 >= n) {
                break;
            }
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c < 0x20) {
            dst[o++] = ' ';
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

char *espos_ota_status_json(void)
{
    espos_ota_port_info_t info;
    espos_ota_port_info(&info);
    char *out = malloc(2048);
    if (!out) {
        return NULL;
    }
    lock();
    char err[200], url[520], notes[280], murl[340];
    json_str(err, sizeof(err), s.last_error);
    json_str(url, sizeof(url), s.avail.url);
    json_str(notes, sizeof(notes), s.avail.notes);
    json_str(murl, sizeof(murl), s.manifest_url);
    uint32_t now = espos_ota_port_uptime_s();
    int n;
    char last[16] = "null", next[16] = "null";
    if (s.last_check_at) {
        snprintf(last, sizeof(last), "%u", (unsigned)(now - s.last_check_at));
    }
    if (s.next_check_at && s.auto_check) {
        snprintf(next, sizeof(next), "%d", (int)(s.next_check_at - now));
    }
    n = snprintf(out, 2048,
                 "{\"state\":\"%s\",\"last_error\":\"%s\","
                 "\"running\":{\"version\":\"%s\",\"project\":\"%s\",\"target\":\"%s\",\"slot\":\"%s\",\"image_state\":\"%s\","
                 "\"pending_verify\":%s,\"confirmed\":%s,\"other_slot\":\"%s\",\"other_version\":\"%s\",\"rolled_back\":%s,"
                 "\"built\":\"%s %s\",\"idf\":\"%s\"},"
                 "\"manifest\":{\"url\":\"%s\",\"channel\":\"%s\",\"auto_check\":%s,\"auto_install\":%s,"
                 "\"last_check_s\":%s,\"next_check_s\":%s},"
                 "\"progress\":{\"received\":%u,\"total\":%u}",
                 espos_ota_state_name(s.state), err,
                 info.version, info.project, CONFIG_IDF_TARGET, info.slot, info.state,
                 info.pending_verify && !s.confirmed_this_boot ? "true" : "false",
                 s.confirmed_this_boot || !info.pending_verify ? "true" : "false",
                 info.other_slot, info.other_version, info.rolled_back ? "true" : "false",
                 info.date, info.time, info.idf,
                 murl, s.channel, s.auto_check ? "true" : "false", s.auto_install ? "true" : "false",
                 last, next,
                 (unsigned)s.received, (unsigned)s.total);
    if (s.have_avail) {
        n += snprintf(out + n, 2048 - n, ",\"available\":{\"version\":\"%s\",\"url\":\"%s\",\"size\":%u,\"sha256\":\"%s\",\"notes\":\"%s\",\"newer\":%s}",
                      s.avail.version, url, (unsigned)s.avail.size, s.avail.sha256, notes, s.avail.newer ? "true" : "false");
    } else {
        n += snprintf(out + n, 2048 - n, ",\"available\":null");
    }
    snprintf(out + n, 2048 - n, "}");
    unlock();
    return out;
}

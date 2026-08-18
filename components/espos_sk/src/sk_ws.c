/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * WebSocket delta stream: one task keeps ws://<server>/signalk/v1/stream
 * open with the access token, sends batched deltas from the delta engine
 * (draining the offline buffer after a reconnect), reconciles declared
 * metadata on every connect and publishes device health. Built on IDF's
 * esp_transport_ws (no extra dependency); the transport answers pings.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"
#include "sdkconfig.h"

#include "espos_cfg_keys.h"
#include "espos_config.h"
#include "espos_httpd_sse.h"
#include "espos_sk.h"
#include "espos_sk_delta.h"
#include "espos_sk_priv.h"
#include "espos_wifi.h"
#include "espos_wifi_sm.h"

static const char *TAG = "espos_skws";

typedef struct {
    char path[ESPOS_SK_PATH_MAX];
    char *meta_json;
    uint32_t period_ms;
    bool reconciled;
} meta_entry_t;

static struct {
    TaskHandle_t task;
    SemaphoreHandle_t lock;          /* delta engine + meta table + status */
    espos_sk_delta_t *delta;
    meta_entry_t meta[ESPOS_SK_MAX_META];
    size_t meta_n;
    bool meta_dirty;                 /* something to reconcile on the live connection */
    volatile bool stop;
    volatile bool cfg_dirty;
    /* config */
    bool enabled;
    uint32_t batch_ms, drain_per_s, health_ms;
    size_t buffer_msgs, buffer_bytes;
    /* status */
    espos_sk_ws_status_t st;
    uint32_t connected_since_ms;
    uint32_t backoff_round;
    uint32_t retry_at_ms;
    char label[40];
} s;

static uint32_t now_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }
static void lock(void) { xSemaphoreTake(s.lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s.lock); }

static void publish_status(void)
{
    char *json = espos_sk_ws_status_json();
    if (json) {
        espos_httpd_sse_publish("sk_ws", json);
        free(json);
    }
}

/* ------------------------------------------------------------ config */

static void load_cfg(void)
{
    bool en = true;
    espos_config_get_bool(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_WS_ENABLED, &en);
    int32_t v = 100;
    espos_config_get_i32(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_BATCH_MS, &v);
    uint32_t batch = (uint32_t)v;
    v = 128;
    espos_config_get_i32(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_BUFFER_MSGS, &v);
    size_t msgs = (size_t)v;
    v = 32;
    espos_config_get_i32(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_BUFFER_KB, &v);
    size_t bytes = (size_t)v * 1024;
    v = 20;
    espos_config_get_i32(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_DRAIN_PER_S, &v);
    uint32_t drain = (uint32_t)v;
    v = 10;
    espos_config_get_i32(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_HEALTH_S, &v);
    uint32_t health = (uint32_t)v * 1000;
    char h[33] = { 0 };
    espos_config_get_str(ESPOS_CFG_NS_WIFI, ESPOS_CFG_WIFI_HOSTNAME, h, sizeof(h), NULL);
    lock();
    s.enabled = en;
    s.batch_ms = batch;
    s.drain_per_s = drain;
    s.health_ms = health;
    if (h[0]) {
        snprintf(s.label, sizeof(s.label), "%s", h);
    } else {
        snprintf(s.label, sizeof(s.label), "espos-%s", espos_wifi_short_id());
    }
    /* Buffer geometry changes rebuild the engine (buffered messages are lost). */
    if (!s.delta || msgs != s.buffer_msgs || bytes != s.buffer_bytes) {
        s.buffer_msgs = msgs;
        s.buffer_bytes = bytes;
        espos_sk_delta_cfg_t c = { .label = s.label, .batch_ms = batch, .max_msgs = msgs, .max_bytes = bytes, .drain_per_s = drain };
        espos_sk_delta_t *nd = espos_sk_delta_create(&c);
        if (nd) {
            espos_sk_delta_destroy(s.delta);
            s.delta = nd;
        }
    } else {
        espos_sk_delta_set_label(s.delta, s.label);
        espos_sk_delta_set_timing(s.delta, batch, drain);
    }
    unlock();
}

void espos_sk_ws_config_changed(void)
{
    s.cfg_dirty = true;
}

/* ----------------------------------------------------------- publish */

static esp_err_t publish(const char *path, const char *value_json)
{
    if (!s.lock || !s.delta) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    esp_err_t err = espos_sk_delta_publish(s.delta, path, value_json, now_ms());
    unlock();
    return err;
}

esp_err_t espos_sk_publish_number(const char *path, double value)
{
    char v[32];
    espos_sk_json_number(v, sizeof(v), value);
    return publish(path, v);
}

esp_err_t espos_sk_publish_string(const char *path, const char *value)
{
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }
    char v[ESPOS_SK_VALUE_MAX];
    if (espos_sk_json_string(v, sizeof(v), value) <= 0 || strlen(value) * 2 + 2 >= sizeof(v)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return publish(path, v);
}

esp_err_t espos_sk_publish_bool(const char *path, bool value)
{
    return publish(path, value ? "true" : "false");
}

esp_err_t espos_sk_publish_json(const char *path, const char *value_json)
{
    return publish(path, value_json);
}

esp_err_t espos_sk_declare_meta(const char *path, const char *meta_json, uint32_t period_ms)
{
    if (!path || !*path || strlen(path) >= ESPOS_SK_PATH_MAX || !meta_json) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *j = cJSON_Parse(meta_json);
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        return ESP_ERR_INVALID_ARG;
    }
    if (period_ms) {
        cJSON_DeleteItemFromObject(j, "timeout");
        cJSON_AddNumberToObject(j, "timeout", (double)period_ms * 2.5 / 1000.0);
    }
    char *txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!txt) {
        return ESP_ERR_NO_MEM;
    }
    if (!s.lock) {
        s.lock = xSemaphoreCreateMutex();
    }
    lock();
    meta_entry_t *e = NULL;
    for (size_t i = 0; i < s.meta_n; i++) {
        if (strcmp(s.meta[i].path, path) == 0) {
            e = &s.meta[i];
            break;
        }
    }
    if (!e) {
        if (s.meta_n >= ESPOS_SK_MAX_META) {
            unlock();
            free(txt);
            return ESP_ERR_NO_MEM;
        }
        e = &s.meta[s.meta_n++];
        snprintf(e->path, sizeof(e->path), "%s", path);
    }
    free(e->meta_json);
    e->meta_json = txt;
    e->period_ms = period_ms;
    e->reconciled = false;
    s.meta_dirty = true;
    unlock();
    return ESP_OK;
}

/* Reconcile every not-yet-reconciled path: GET the server's meta, PUT ours
 * only if the server has none. Runs on the ws task with a live token. */
static void reconcile_meta(const espos_sk_server_t *srv, const char *token)
{
    for (size_t i = 0; i < ESPOS_SK_MAX_META; i++) {
        char path[ESPOS_SK_PATH_MAX];
        char *meta = NULL;
        lock();
        if (i >= s.meta_n || s.meta[i].reconciled) {
            unlock();
            continue;
        }
        strcpy(path, s.meta[i].path);
        meta = strdup(s.meta[i].meta_json);
        unlock();
        if (!meta) {
            continue;
        }
        char *existing = NULL;
        int st = espos_sk_http_get_meta(srv, token, path, &existing);
        bool done = false;
        if (st == 401 || st == 403) {
            espos_sk_report_unauthorized();
        } else if (existing) {
            ESP_LOGI(TAG, "meta %s: server has its own, keeping it", path);
            done = true;
        } else if (st == 200 || st == 404) {
            int put = espos_sk_http_put_meta(srv, token, path, meta);
            if (put >= 200 && put < 300) {
                ESP_LOGI(TAG, "meta %s: sent", path);
                done = true;
            } else if (put == 401 || put == 403) {
                espos_sk_report_unauthorized();
            } else {
                ESP_LOGW(TAG, "meta %s: PUT → %d", path, put);
            }
        } else {
            ESP_LOGW(TAG, "meta %s: GET → %d", path, st);
        }
        free(existing);
        free(meta);
        if (done) {
            lock();
            s.meta[i].reconciled = true;
            unlock();
        }
    }
    lock();
    bool any = false;
    for (size_t i = 0; i < s.meta_n; i++) {
        any |= !s.meta[i].reconciled;
    }
    s.meta_dirty = any;
    unlock();
}

/* ------------------------------------------------------------ health */

static void publish_health(void)
{
    char base[64];
    snprintf(base, sizeof(base), "espos.%s.", s.label);
    static bool declared;
    if (!declared) {
        declared = true;
        char p[ESPOS_SK_PATH_MAX];
        uint32_t period = s.health_ms;
        snprintf(p, sizeof(p), "%suptime", base);
        espos_sk_declare_meta(p, "{\"units\":\"s\",\"description\":\"Time since boot\"}", period);
        snprintf(p, sizeof(p), "%sfreeHeap", base);
        espos_sk_declare_meta(p, "{\"description\":\"Free heap (bytes)\"}", period);
        snprintf(p, sizeof(p), "%sminFreeHeap", base);
        espos_sk_declare_meta(p, "{\"description\":\"Minimum free heap since boot (bytes)\"}", period);
        snprintf(p, sizeof(p), "%srssi", base);
        espos_sk_declare_meta(p, "{\"units\":\"dB\",\"description\":\"WiFi RSSI\"}", period);
        snprintf(p, sizeof(p), "%swifiReconnects", base);
        espos_sk_declare_meta(p, "{\"description\":\"WiFi reconnects since boot\"}", period);
        snprintf(p, sizeof(p), "%sskReconnects", base);
        espos_sk_declare_meta(p, "{\"description\":\"SignalK stream reconnects since boot\"}", period);
        snprintf(p, sizeof(p), "%sresetReason", base);
        espos_sk_declare_meta(p, "{\"description\":\"Reason of the last reset\"}", period);
    }
    char p[ESPOS_SK_PATH_MAX];
    snprintf(p, sizeof(p), "%suptime", base);
    espos_sk_publish_number(p, now_ms() / 1000);
    snprintf(p, sizeof(p), "%sfreeHeap", base);
    espos_sk_publish_number(p, esp_get_free_heap_size());
    snprintf(p, sizeof(p), "%sminFreeHeap", base);
    espos_sk_publish_number(p, esp_get_minimum_free_heap_size());
    espos_wifi_status_t ws;
    if (espos_wifi_get_status(&ws) == ESP_OK) {
        snprintf(p, sizeof(p), "%srssi", base);
        espos_sk_publish_number(p, ws.rssi);
        snprintf(p, sizeof(p), "%swifiReconnects", base);
        espos_sk_publish_number(p, ws.sm.connect_count > 0 ? ws.sm.connect_count - 1 : 0);
    }
    snprintf(p, sizeof(p), "%sskReconnects", base);
    espos_sk_publish_number(p, s.st.reconnects > 0 ? s.st.reconnects - 1 : 0);
    snprintf(p, sizeof(p), "%sresetReason", base);
    static const char *const reasons[] = { "unknown", "poweron", "external", "software", "panic", "int_wdt",
                                           "task_wdt", "wdt", "deepsleep", "brownout", "sdio", "usb", "jtag",
                                           "efuse", "power_glitch", "cpu_lockup" };
    int r = (int)esp_reset_reason();
    espos_sk_publish_string(p, (r >= 0 && r < (int)(sizeof(reasons) / sizeof(reasons[0]))) ? reasons[r] : "unknown");
}

/* -------------------------------------------------------------- task */

static void set_error(const char *msg)
{
    lock();
    snprintf(s.st.last_error, sizeof(s.st.last_error), "%s", msg ? msg : "");
    unlock();
}

static void ws_task(void *arg)
{
    (void)arg;
    esp_transport_handle_t tcp = NULL, ws = NULL;
    bool connected = false;
    uint32_t next_health = 0;
    char *buf = malloc(2048);
    char headers[ESPOS_SK_TOKEN_MAX + 32];
    espos_sk_server_t cur_srv = { 0 };
    char cur_token[ESPOS_SK_TOKEN_MAX] = { 0 };

    while (!s.stop) {
        if (s.cfg_dirty) {
            s.cfg_dirty = false;
            load_cfg();
        }
        /* preconditions: enabled, wifi up, server + token */
        espos_sk_server_t srv;
        char token[ESPOS_SK_TOKEN_MAX];
        espos_wifi_status_t wst;
        bool wifi_up = espos_wifi_get_status(&wst) == ESP_OK && wst.sm.state == ESPOS_WIFI_ST_CONNECTED;
        /* token "" is fine when the server runs without security (OPEN); the
         * token machine only exposes a server once it is usable */
        bool have = espos_sk_get_server(&srv) == ESP_OK && espos_sk_get_token(token, sizeof(token)) == ESP_OK &&
                    espos_sk_stream_allowed();
        bool ready = s.enabled && wifi_up && have;
        if (connected) {
            bool changed = strcmp(srv.host, cur_srv.host) != 0 || srv.port != cur_srv.port || strcmp(token, cur_token) != 0;
            if (!ready || changed) {
                ESP_LOGI(TAG, "closing stream (%s)", !s.enabled ? "disabled" : !wifi_up ? "wifi down" : changed ? "server/token changed" : "no server");
                esp_transport_close(ws);
                connected = false;
                lock();
                s.st.connected = false;
                if (s.delta) {
                    espos_sk_delta_flush(s.delta, now_ms());
                }
                unlock();
                publish_status();
            }
        }
        if (!connected) {
            /* offline: still close batch windows so values land in the ring
             * (a stale path keeps its history instead of coalescing forever) */
            uint32_t idle = 250;
            lock();
            if (s.delta) {
                free(espos_sk_delta_take(s.delta, now_ms(), false));
                uint32_t due = espos_sk_delta_next_due_ms(s.delta, now_ms(), false);
                if (due < idle) {
                    idle = due ? due : 1;
                }
            }
            unlock();
            if (!ready) {
                lock();
                s.st.next_retry_s = 0;
                unlock();
                vTaskDelay(pdMS_TO_TICKS(idle));
                continue;
            }
            uint32_t t = now_ms();
            if (s.retry_at_ms && (int32_t)(s.retry_at_ms - t) > 0) {
                lock();
                s.st.next_retry_s = (s.retry_at_ms - t) / 1000;
                unlock();
                vTaskDelay(pdMS_TO_TICKS(idle));
                continue;
            }
            /* connect */
            if (!tcp) {
                tcp = esp_transport_tcp_init();
                ws = esp_transport_ws_init(tcp);
            }
            esp_transport_ws_set_path(ws, "/signalk/v1/stream?subscribe=none");
            if (token[0]) {
                snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", token);
            } else {
                headers[0] = '\0';
            }
            esp_transport_ws_set_headers(ws, headers[0] ? headers : NULL);
            ESP_LOGI(TAG, "connecting to ws://%s:%u/signalk/v1/stream", srv.host, srv.port);
            int rc = esp_transport_connect(ws, srv.host, srv.port, 8000);
            if (rc < 0) {
                int http = esp_transport_ws_get_upgrade_request_status(ws);
                esp_transport_close(ws);
                char msg[64];
                if (http == 401 || http == 403) {
                    snprintf(msg, sizeof(msg), "stream refused (HTTP %d): token rejected", http);
                    espos_sk_report_unauthorized();
                } else if (http > 0) {
                    snprintf(msg, sizeof(msg), "stream refused (HTTP %d)", http);
                } else {
                    snprintf(msg, sizeof(msg), "connect failed");
                }
                ESP_LOGW(TAG, "%s", msg);
                set_error(msg);
                uint32_t d = espos_wifi_backoff_ms(s.backoff_round, 60000, (uint32_t)rand());
                if (s.backoff_round < 30) {
                    s.backoff_round++;
                }
                s.retry_at_ms = now_ms() + d;
                if (!s.retry_at_ms) {
                    s.retry_at_ms = 1;
                }
                publish_status();
                continue;
            }
            connected = true;
            cur_srv = srv;
            strcpy(cur_token, token);
            s.backoff_round = 0;
            s.retry_at_ms = 0;
            lock();
            s.st.connected = true;
            s.st.reconnects++;
            s.st.next_retry_s = 0;
            s.st.last_error[0] = '\0';
            s.connected_since_ms = now_ms();
            unlock();
            ESP_LOGI(TAG, "stream connected");
            publish_status();
            /* meta first, so the server knows units before values arrive */
            reconcile_meta(&srv, token);
        }

        /* connected: send what is due, read what comes in */
        if (s.meta_dirty) {
            reconcile_meta(&srv, token);
        }
        uint32_t t = now_ms();
        if (s.health_ms && (int32_t)(t - next_health) >= 0) {
            publish_health();
            next_health = t + s.health_ms;
        }
        char *msg = NULL;
        lock();
        if (s.delta) {
            msg = espos_sk_delta_take(s.delta, t, true);
        }
        unlock();
        if (msg) {
            int w = esp_transport_ws_send_raw(ws, WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN, msg, (int)strlen(msg), 3000);
            if (w < 0) {
                ESP_LOGW(TAG, "send failed; reconnecting");
                lock();
                espos_sk_delta_requeue(s.delta, msg);
                s.st.send_errors++;
                s.st.connected = false;
                unlock();
                set_error("send failed");
                esp_transport_close(ws);
                connected = false;
                s.retry_at_ms = now_ms() + 1000;
                publish_status();
                continue;
            }
            free(msg);
            lock();
            s.st.sent++;
            unlock();
            continue; /* look for more right away */
        }
        /* nothing to send: wait for input or the next deadline */
        uint32_t wait = 250;
        lock();
        uint32_t due = s.delta ? espos_sk_delta_next_due_ms(s.delta, t, true) : UINT32_MAX;
        unlock();
        if (due < wait) {
            wait = due;
        }
        if (s.health_ms) {
            int32_t hl = (int32_t)(next_health - t);
            if (hl >= 0 && (uint32_t)hl < wait) {
                wait = (uint32_t)hl;
            }
        }
        int pr = esp_transport_poll_read(ws, (int)wait);
        if (pr > 0) {
            int r = esp_transport_read(ws, buf, 2047, 100);
            if (r > 0) {
                buf[r] = '\0';
                ESP_LOGD(TAG, "rx %d bytes: %.200s", r, buf);
                /* the server tells us when a message was refused (auth,
                 * malformed): surface that instead of silently retrying */
                if (strstr(buf, "errorMessage") || strstr(buf, "\"error\"")) {
                    ESP_LOGW(TAG, "server: %.160s", buf);
                    lock();
                    snprintf(s.st.last_error, sizeof(s.st.last_error), "%.60s", buf);
                    unlock();
                }
            }
            if (r < 0) {
                ESP_LOGW(TAG, "stream closed by server");
                lock();
                s.st.connected = false;
                unlock();
                set_error("closed by server");
                esp_transport_close(ws);
                connected = false;
                s.retry_at_ms = now_ms() + 1000;
                publish_status();
            }
            /* server hello / anything else: not needed */
        } else if (pr < 0) {
            lock();
            s.st.connected = false;
            unlock();
            set_error("connection error");
            esp_transport_close(ws);
            connected = false;
            s.retry_at_ms = now_ms() + 1000;
            publish_status();
        }
    }
    if (connected) {
        esp_transport_close(ws);
    }
    if (ws) {
        esp_transport_destroy(ws);
    }
    if (tcp) {
        esp_transport_destroy(tcp);
    }
    free(buf);
    s.task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------ status */

esp_err_t espos_sk_ws_get_status(espos_sk_ws_status_t *out)
{
    if (!out || !s.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    *out = s.st;
    out->enabled = s.enabled;
    if (s.st.connected) {
        out->connected_s = (now_ms() - s.connected_since_ms) / 1000;
    }
    if (s.delta) {
        espos_sk_delta_stats_t ds;
        espos_sk_delta_stats(s.delta, &ds);
        out->pending = ds.pending;
        out->buffered = ds.buffered;
        out->buffered_bytes = ds.buffered_bytes;
        out->dropped = ds.dropped;
    }
    out->meta_declared = s.meta_n;
    out->meta_reconciled = 0;
    for (size_t i = 0; i < s.meta_n; i++) {
        out->meta_reconciled += s.meta[i].reconciled;
    }
    unlock();
    return ESP_OK;
}

char *espos_sk_ws_status_json(void)
{
    espos_sk_ws_status_t st;
    if (espos_sk_ws_get_status(&st) != ESP_OK) {
        return NULL;
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "enabled", st.enabled);
    cJSON_AddBoolToObject(j, "connected", st.connected);
    if (st.connected) {
        cJSON_AddNumberToObject(j, "connected_s", st.connected_s);
    } else if (st.next_retry_s) {
        cJSON_AddNumberToObject(j, "next_retry_s", st.next_retry_s);
    }
    cJSON_AddNumberToObject(j, "reconnects", st.reconnects);
    cJSON_AddNumberToObject(j, "sent", st.sent);
    cJSON_AddNumberToObject(j, "send_errors", st.send_errors);
    cJSON_AddNumberToObject(j, "pending", (double)st.pending);
    cJSON_AddNumberToObject(j, "buffered", (double)st.buffered);
    cJSON_AddNumberToObject(j, "buffered_bytes", (double)st.buffered_bytes);
    cJSON_AddNumberToObject(j, "dropped", st.dropped);
    cJSON_AddStringToObject(j, "last_error", st.last_error);
    cJSON *m = cJSON_AddObjectToObject(j, "meta");
    cJSON_AddNumberToObject(m, "declared", (double)st.meta_declared);
    cJSON_AddNumberToObject(m, "reconciled", (double)st.meta_reconciled);
    char *txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return txt;
}

/* --------------------------------------------------------- lifecycle */

esp_err_t espos_sk_ws_start(void)
{
    if (s.task) {
        return ESP_OK;
    }
    if (!s.lock) {
        s.lock = xSemaphoreCreateMutex();
        if (!s.lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    load_cfg();
    s.stop = false;
    if (xTaskCreate(ws_task, "espos_skws", 8192, NULL, tskIDLE_PRIORITY + 3, &s.task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void espos_sk_ws_stop(void)
{
    s.stop = true;
    for (int i = 0; i < 100 && s.task; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

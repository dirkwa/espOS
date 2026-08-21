/*
 * SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * The gateway: signalk-server's BLE provider protocol.
 *
 * Two channels, both bearing the token espos_sk already holds:
 *   POST .../ble/gateway/advertisements   batched advertisements
 *   WS   .../ble/gateway/ws?token=<jwt>   hello/status out, gatt_* in
 *
 * Wire-format invariants (contract with signalk-server's ble-schemas.ts):
 *   - snake_case keys throughout;
 *   - advertisement adv_data is UPPERCASE hex, GATT data is lowercase;
 *   - the JWT rides in the WS query string, not a header;
 *   - an empty token means the Authorization header is omitted entirely,
 *     which is valid when the server runs without security.
 */

#include "sdkconfig.h"

#if defined(CONFIG_BT_BLUEDROID_ENABLED)

#include <string.h>

#include "ble_gattc.h"
#include "ble_proto.h"
#include "ble_types.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "espos_ble.h"
#include "espos_config.h"
#include "espos_cfg_keys.h"
#include "espos_sk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "espos_ble";

#define ADV_PATH "/signalk/v2/api/ble/gateway/advertisements"
#define WS_PATH "/signalk/v2/api/ble/gateway/ws"

/* Advertisements per POST. Bounded so one flush cannot build an unbounded
 * JSON document out of a busy marina. */
#define POST_BATCH_MAX 40

typedef struct {
    bool active;
    char session_id[40];
    int conn_handle;
    /* Init writes run in order, each starting when the previous completes. */
    espos_ble_init_write_t *init_writes;
    size_t init_count, init_index;
    int64_t init_deadline_us;
    /* Characteristics to enable notifications on. */
    char (*notify_uuids)[ESPOS_BLE_UUID_MAX];
    size_t notify_count;
} session_t;

#define MAX_SESSIONS ESPOS_BLE_GATTC_MAX_CONN

static struct {
    bool running;
    espos_ble_advq_t q;
    espos_ble_adv_t *storage;
    SemaphoreHandle_t lock;
    esp_websocket_client_handle_t ws;
    bool ws_connected;
    TaskHandle_t task;
    session_t sessions[MAX_SESSIONS];

    /* config */
    bool active_scan, control_ws;
    uint32_t scan_int, scan_win, post_int, status_int, max_gatt;

    /* counters */
    uint32_t adv_received, adv_posted, post_ok, post_fail;
} g;

/* ---------------------------------------------------------------- */
/* Advertisement intake (runs on the BT stack task)                   */
/* ---------------------------------------------------------------- */

static void on_advertisement(const espos_ble_adv_t *adv, void *arg)
{
    (void)arg;
    g.adv_received++;
    /* A short timeout, never portMAX_DELAY: this runs on the Bluetooth stack
     * task, and blocking it stalls the whole radio. Dropping one
     * advertisement is much cheaper than that. */
    if (xSemaphoreTake(g.lock, pdMS_TO_TICKS(5)) != pdTRUE) return;
    espos_ble_advq_push(&g.q, adv);
    xSemaphoreGive(g.lock);
}

/* ---------------------------------------------------------------- */
/* HTTP POST of pending advertisements                                */
/* ---------------------------------------------------------------- */

static char *build_adv_body(const espos_ble_adv_t *ads, size_t n)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    espos_sk_ws_status_t ws;
    (void)ws;
    cJSON_AddStringToObject(root, "gateway_id", espos_ble_mac());
    cJSON_AddStringToObject(root, "mac", espos_ble_mac());
    cJSON_AddNumberToObject(root, "uptime", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());

    cJSON *devs = cJSON_AddArrayToObject(root, "devices");
    for (size_t i = 0; i < n && devs; i++) {
        cJSON *d = cJSON_CreateObject();
        if (!d) break;
        cJSON_AddStringToObject(d, "mac", ads[i].address);
        cJSON_AddNumberToObject(d, "rssi", ads[i].rssi);
        if (ads[i].name[0]) cJSON_AddStringToObject(d, "name", ads[i].name);
        if (ads[i].adv_data_len) {
            char hex[ESPOS_BLE_ADV_DATA_MAX * 2 + 1];
            /* UPPERCASE here; the GATT path uses lowercase. */
            espos_ble_hex_encode_upper(ads[i].adv_data, ads[i].adv_data_len, hex);
            cJSON_AddStringToObject(d, "adv_data", hex);
        }
        cJSON_AddItemToArray(devs, d);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

/* Scratch for one POST. Deliberately static rather than stack: the batch
 * alone is POST_BATCH_MAX * sizeof(espos_ble_adv_t) (~5 KB), which overflows
 * the gateway task's stack the first time a POST actually runs - and that
 * only happens once a token exists, so it hides until the device is fully
 * provisioned. Only the gateway task touches these. */
static espos_ble_adv_t s_post_batch[POST_BATCH_MAX];
static char s_token[512];
static char s_auth[540];
static char s_url[160];

static void post_pending(void)
{
    espos_sk_server_t srv;
    if (espos_sk_get_server(&srv) != ESP_OK || !srv.host[0]) return;

    espos_ble_adv_t *batch = s_post_batch;
    size_t n = 0;
    if (xSemaphoreTake(g.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        n = espos_ble_advq_drain(&g.q, batch, POST_BATCH_MAX);
        xSemaphoreGive(g.lock);
    }
    if (!n) return;

    char *body = build_adv_body(batch, n);
    if (!body) return;

    snprintf(s_url, sizeof(s_url), "http://%s:%u" ADV_PATH, srv.host, srv.port);

    esp_http_client_config_t cfg = {
        .url = s_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 3000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { free(body); return; }

    esp_http_client_set_header(c, "Content-Type", "application/json");
    if (espos_sk_get_token(s_token, sizeof(s_token)) == ESP_OK && s_token[0]) {
        snprintf(s_auth, sizeof(s_auth), "Bearer %s", s_token);
        esp_http_client_set_header(c, "Authorization", s_auth);
    } /* else: no header at all - correct when the server has security off */
    esp_http_client_set_post_field(c, body, strlen(body));

    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    free(body);

    if (err == ESP_OK && status == 200) {
        g.adv_posted += n;
        g.post_ok++;
    } else {
        g.post_fail++;
        if (status == 401 || status == 403) {
            /* Let espos_sk decide what to do about the token rather than
             * re-running an access request of our own. */
            ESP_LOGW(TAG, "POST rejected (%d) - reporting to espos_sk", status);
            espos_sk_report_unauthorized();
        } else {
            ESP_LOGW(TAG, "POST failed: err=%s status=%d", esp_err_to_name(err), status);
        }
    }
}

/* ---------------------------------------------------------------- */
/* Control WebSocket                                                  */
/* ---------------------------------------------------------------- */

static void ws_send_json(cJSON *doc)
{
    if (!g.ws_connected || !g.ws) return;
    char *txt = cJSON_PrintUnformatted(doc);
    if (!txt) return;
    esp_websocket_client_send_text(g.ws, txt, strlen(txt), pdMS_TO_TICKS(1000));
    free(txt);
}

static void send_hello(void)
{
    cJSON *d = cJSON_CreateObject();
    if (!d) return;
    cJSON_AddStringToObject(d, "type", "hello");
    cJSON_AddStringToObject(d, "gateway_id", espos_ble_mac());
    cJSON_AddStringToObject(d, "mac", espos_ble_mac());
    cJSON_AddStringToObject(d, "firmware", "espos-ble-gateway");
    cJSON_AddNumberToObject(d, "max_gatt_connections", g.max_gatt);
    cJSON_AddNumberToObject(d, "active_gatt_connections",
                            espos_ble_gatt_active_count());
    ws_send_json(d);
    cJSON_Delete(d);
}

static void send_status(void)
{
    cJSON *d = cJSON_CreateObject();
    if (!d) return;
    cJSON_AddStringToObject(d, "type", "status");
    cJSON_AddStringToObject(d, "gateway_id", espos_ble_mac());
    cJSON_AddNumberToObject(d, "uptime", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(d, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(d, "scan_hits", espos_ble_scan_hits());
    cJSON_AddNumberToObject(d, "post_success", g.post_ok);
    cJSON_AddNumberToObject(d, "post_fail", g.post_fail);
    cJSON_AddNumberToObject(d, "active_gatt_connections",
                            espos_ble_gatt_active_count());
    cJSON_AddNumberToObject(d, "max_gatt_connections", g.max_gatt);
    ws_send_json(d);
    cJSON_Delete(d);
}

static session_t *session_by_id(const char *id)
{
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        if (g.sessions[i].active && strcmp(g.sessions[i].session_id, id) == 0) {
            return &g.sessions[i];
        }
    }
    return NULL;
}

static session_t *session_by_handle(int h)
{
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        if (g.sessions[i].active && g.sessions[i].conn_handle == h) {
            return &g.sessions[i];
        }
    }
    return NULL;
}

static void session_free(session_t *s)
{
    free(s->notify_uuids);
    free(s->init_writes);
    memset(s, 0, sizeof(*s));
}

static void send_gatt_event(const char *type, const char *session_id,
                            const char *uuid, const char *data_hex,
                            const char *error, int reason)
{
    cJSON *d = cJSON_CreateObject();
    if (!d) return;
    cJSON_AddStringToObject(d, "type", type);
    cJSON_AddStringToObject(d, "session_id", session_id);
    if (uuid) cJSON_AddStringToObject(d, "uuid", uuid);
    if (data_hex) cJSON_AddStringToObject(d, "data", data_hex);
    if (error) cJSON_AddStringToObject(d, "error", error);
    if (reason >= 0) cJSON_AddNumberToObject(d, "reason", reason);
    ws_send_json(d);
    cJSON_Delete(d);
}

/* Drive the init-write chain. Each write starts when the previous completes -
 * except a write-without-response, which produces no completion event at all,
 * so ble_gattc synthesises one. The deadline is the backstop: without it a
 * peripheral that simply never answers leaves the session stuck in init
 * forever, never subscribing and never reporting. */
/* Enable notifications before any init write goes out.
 *
 * Order matters: a BMS answers its first command over the notify pipe, so
 * subscribing afterwards races the reply and usually loses it - the write
 * lands, the peripheral answers into a pipe nobody is listening on, and the
 * session sits there looking connected but silent. */
static void run_subscribes(session_t *s)
{
    for (size_t i = 0; i < s->notify_count; i++) {
        esp_err_t err = espos_ble_gatt_subscribe(s->conn_handle, s->notify_uuids[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "subscribe %s: %s", s->notify_uuids[i], esp_err_to_name(err));
            send_gatt_event("gatt_error", s->session_id, s->notify_uuids[i], NULL,
                            "subscribe failed", -1);
        }
    }
}

static void run_init_writes(session_t *s)
{
    if (s->init_index >= s->init_count) {
        send_gatt_event("gatt_connected", s->session_id, NULL, NULL, NULL, -1);
        return;
    }
    espos_ble_init_write_t *iw = &s->init_writes[s->init_index];
    s->init_deadline_us = esp_timer_get_time() + 3000000; /* 3 s */
    esp_err_t err = espos_ble_gatt_write(s->conn_handle, iw->char_uuid,
                                         iw->data, iw->data_len, iw->mode);
    if (err != ESP_OK) {
        send_gatt_event("gatt_error", s->session_id, iw->char_uuid, NULL,
                        "init write failed", -1);
        s->init_index++;
        run_init_writes(s);
    }
}

static void handle_gatt_subscribe(cJSON *doc)
{
    const cJSON *sid = cJSON_GetObjectItemCaseSensitive(doc, "session_id");
    const cJSON *mac = cJSON_GetObjectItemCaseSensitive(doc, "mac");
    const cJSON *svc = cJSON_GetObjectItemCaseSensitive(doc, "service");
    if (!cJSON_IsString(sid) || !cJSON_IsString(mac)) return;

    session_t *s = NULL;
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        if (!g.sessions[i].active) { s = &g.sessions[i]; break; }
    }
    if (!s) {
        send_gatt_event("gatt_error", sid->valuestring, NULL, NULL,
                        "no free session slot", -1);
        return;
    }

    memset(s, 0, sizeof(*s));
    snprintf(s->session_id, sizeof(s->session_id), "%s", sid->valuestring);

    const cJSON *notify = cJSON_GetObjectItemCaseSensitive(doc, "notify");
    if (cJSON_IsArray(notify)) {
        int n = cJSON_GetArraySize(notify);
        if (n > 0) {
            s->notify_uuids = calloc((size_t)n, ESPOS_BLE_UUID_MAX);
            if (s->notify_uuids) {
                for (int i = 0; i < n; i++) {
                    cJSON *e = cJSON_GetArrayItem(notify, i);
                    if (!cJSON_IsString(e)) continue;
                    snprintf(s->notify_uuids[s->notify_count], ESPOS_BLE_UUID_MAX,
                             "%s", e->valuestring);
                    s->notify_count++;
                }
            }
        }
    }

    const cJSON *init = cJSON_GetObjectItemCaseSensitive(doc, "init");
    if (cJSON_IsArray(init)) {
        int n = cJSON_GetArraySize(init);
        if (n > 0) {
            s->init_writes = calloc((size_t)n, sizeof(*s->init_writes));
            if (s->init_writes) {
                for (int i = 0; i < n; i++) {
                    cJSON *e = cJSON_GetArrayItem(init, i);
                    const cJSON *u = cJSON_GetObjectItemCaseSensitive(e, "uuid");
                    const cJSON *d = cJSON_GetObjectItemCaseSensitive(e, "data");
                    if (!cJSON_IsString(u) || !cJSON_IsString(d)) continue;
                    espos_ble_init_write_t *iw = &s->init_writes[s->init_count];
                    snprintf(iw->char_uuid, sizeof(iw->char_uuid), "%s", u->valuestring);
                    int len = espos_ble_hex_decode(d->valuestring, iw->data,
                                                   sizeof(iw->data));
                    if (len < 0) continue;
                    iw->data_len = (size_t)len;
                    /* Absent means write-with-response; see ble_proto. */
                    iw->mode = espos_ble_parse_write_mode(e);
                    s->init_count++;
                }
            }
        }
    }

    s->conn_handle = espos_ble_gatt_connect(
        mac->valuestring, cJSON_IsString(svc) ? svc->valuestring : NULL);
    if (s->conn_handle < 0) {
        send_gatt_event("gatt_error", s->session_id, NULL, NULL,
                        "connect failed", -1);
        session_free(s);
        return;
    }
    s->active = true;
}

static void handle_gatt_write(cJSON *doc)
{
    const cJSON *sid = cJSON_GetObjectItemCaseSensitive(doc, "session_id");
    const cJSON *uuid = cJSON_GetObjectItemCaseSensitive(doc, "uuid");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(doc, "data");
    if (!cJSON_IsString(sid) || !cJSON_IsString(uuid) || !cJSON_IsString(data)) return;

    session_t *s = session_by_id(sid->valuestring);
    if (!s) return;

    uint8_t buf[ESPOS_BLE_WRITE_DATA_MAX];
    int len = espos_ble_hex_decode(data->valuestring, buf, sizeof(buf));
    if (len < 0) {
        send_gatt_event("gatt_error", s->session_id, uuid->valuestring, NULL,
                        "bad hex payload", -1);
        return;
    }
    espos_ble_gatt_write(s->conn_handle, uuid->valuestring, buf, (size_t)len,
                         espos_ble_parse_write_mode(doc));
}

static void handle_gatt_close(cJSON *doc)
{
    const cJSON *sid = cJSON_GetObjectItemCaseSensitive(doc, "session_id");
    if (!cJSON_IsString(sid)) return;
    session_t *s = session_by_id(sid->valuestring);
    if (!s) return;
    espos_ble_gatt_disconnect(s->conn_handle);
    session_free(s);
}

static void handle_ws_text(const char *data, size_t len)
{
    cJSON *doc = cJSON_ParseWithLength(data, len);
    if (!doc) return;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(doc, "type");
    if (cJSON_IsString(type)) {
        if (!strcmp(type->valuestring, "gatt_subscribe")) handle_gatt_subscribe(doc);
        else if (!strcmp(type->valuestring, "gatt_write")) handle_gatt_write(doc);
        else if (!strcmp(type->valuestring, "gatt_close")) handle_gatt_close(doc);
        /* hello_ack needs no action beyond confirming the server heard us. */
    }
    cJSON_Delete(doc);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_websocket_event_data_t *ev = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        g.ws_connected = true;
        ESP_LOGI(TAG, "control WS connected");
        send_hello();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        g.ws_connected = false;
        break;
    case WEBSOCKET_EVENT_DATA:
        /* op_code 1 is a text frame; ignore pings/pongs/binary. */
        if (ev->op_code == 0x01 && ev->data_len > 0) {
            handle_ws_text(ev->data_ptr, ev->data_len);
        }
        break;
    default:
        break;
    }
}

/* Host:port the live control socket was opened against, so a server change is
 * noticed rather than silently ignored for the lifetime of the handle. */
static char s_ws_host[ESPOS_SK_HOST_MAX];
static uint16_t s_ws_port;

static void ws_connect(void)
{
    if (!g.control_ws) return;
    espos_sk_server_t srv;
    if (espos_sk_get_server(&srv) != ESP_OK || !srv.host[0]) return;

    if (g.ws) {
        /* Already connected to this server: nothing to do. Pointed at a
         * different one (the user re-pinned it, or discovery moved): tear the
         * old socket down so the code below re-dials the new address. */
        if (strcmp(s_ws_host, srv.host) == 0 && s_ws_port == srv.port) return;
        ESP_LOGI(TAG, "control WS server changed %s:%u -> %s:%u",
                 s_ws_host, s_ws_port, srv.host, srv.port);
        esp_websocket_client_stop(g.ws);
        esp_websocket_client_destroy(g.ws);
        g.ws = NULL;
        g.ws_connected = false;
    }

    s_token[0] = '\0';
    espos_sk_get_token(s_token, sizeof(s_token));

    /* The JWT rides in the query string: the server reads it there, and a raw
     * WS upgrade carries no Authorization header anyway. */
    char *url = malloc(700);
    if (!url) return;
    snprintf(url, 700, "ws://%s:%u" WS_PATH "?token=%s", srv.host, srv.port, s_token);

    esp_websocket_client_config_t cfg = {
        .uri = url,
        .task_stack = 6144,
        /* Roomy enough that a large gatt_subscribe arrives in one frame; the
         * implementation this replaces used 1 KB and could not reassemble. */
        .buffer_size = 4096,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
    };
    g.ws = esp_websocket_client_init(&cfg);
    free(url);
    if (!g.ws) return;
    esp_websocket_register_events(g.ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    esp_websocket_client_start(g.ws);
    snprintf(s_ws_host, sizeof(s_ws_host), "%s", srv.host);
    s_ws_port = srv.port;
}

/* ---------------------------------------------------------------- */
/* GATT callbacks                                                     */
/* ---------------------------------------------------------------- */

static void on_gatt_connected(int h, void *arg)
{
    (void)arg;
    session_t *s = session_by_handle(h);
    if (!s) return;
    run_subscribes(s);
    run_init_writes(s);
}

static void on_gatt_disconnected(int h, int reason, void *arg)
{
    (void)arg;
    session_t *s = session_by_handle(h);
    if (!s) return;
    send_gatt_event("gatt_disconnected", s->session_id, NULL, NULL, NULL, reason);
    session_free(s);
}

static void on_gatt_notify(int h, const char *uuid, const uint8_t *data,
                           size_t len, void *arg)
{
    (void)arg;
    session_t *s = session_by_handle(h);
    if (!s) return;
    char *hex = malloc(len * 2 + 1);
    if (!hex) return;
    /* lowercase on the GATT channel */
    espos_ble_hex_encode_lower(data, len, hex);
    send_gatt_event("gatt_data", s->session_id, uuid, hex, NULL, -1);
    free(hex);
}

static void on_gatt_read(int h, const char *uuid, const uint8_t *data,
                         size_t len, void *arg)
{
    on_gatt_notify(h, uuid, data, len, arg);
}

static void on_gatt_write_done(int h, const char *uuid, bool ok, void *arg)
{
    (void)uuid; (void)arg;
    session_t *s = session_by_handle(h);
    if (!s || s->init_index >= s->init_count) return;
    if (!ok) ESP_LOGW(TAG, "init write %u failed", (unsigned)s->init_index);
    s->init_index++;
    run_init_writes(s);
}

static void on_gatt_error(int h, const char *error, void *arg)
{
    (void)arg;
    session_t *s = session_by_handle(h);
    if (!s) return;
    send_gatt_event("gatt_error", s->session_id, NULL, NULL, error, -1);
    session_free(s);
}

/* ---------------------------------------------------------------- */
/* Task                                                               */
/* ---------------------------------------------------------------- */

static void gateway_task(void *arg)
{
    (void)arg;
    int64_t next_post = 0, next_status = 0;

    while (g.running) {
        int64_t now = esp_timer_get_time();
        espos_sk_ws_status_t ws;
        bool sk_up = (espos_sk_ws_get_status(&ws) == ESP_OK) && ws.connected;

        /* Gate on the SignalK link: without it there is nowhere to post and
         * no token worth using. Advertisements keep buffering meanwhile. */
        if (sk_up) {
            if (now >= next_post) {
                post_pending();
                next_post = now + (int64_t)g.post_int * 1000;
            }
            if (g.control_ws) {
                ws_connect();
                if (now >= next_status) {
                    send_status();
                    next_status = now + (int64_t)g.status_int * 1000;
                }
            }
        } else if (g.ws) {
            /* SignalK went away: drop the control socket too, rather than
             * leaving a handle pointed at a server that is gone. Without this
             * the client keeps retrying an address the device has since been
             * moved off, and /ble/status keeps reporting ws_connected from a
             * stale handle. It is re-dialled from scratch above once the SK
             * link is back, so a server change is picked up. */
            ESP_LOGI(TAG, "SignalK link down - closing the control WS");
            esp_websocket_client_stop(g.ws);
            esp_websocket_client_destroy(g.ws);
            g.ws = NULL;
            g.ws_connected = false;
        }

        /* Init-write watchdog: a peripheral that never answers must not pin a
         * session in init forever. */
        for (size_t i = 0; i < MAX_SESSIONS; i++) {
            session_t *s = &g.sessions[i];
            if (!s->active || s->init_index >= s->init_count) continue;
            if (s->init_deadline_us && now > s->init_deadline_us) {
                ESP_LOGW(TAG, "init write %u timed out", (unsigned)s->init_index);
                s->init_index++;
                run_init_writes(s);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------- */
/* Public API                                                         */
/* ---------------------------------------------------------------- */

esp_err_t espos_ble_start(void)
{
    if (g.running) return ESP_OK;

    bool enabled = true;
    espos_config_get_bool(ESPOS_CFG_NS_BLE, ESPOS_CFG_BLE_ENABLED, &enabled);
    if (!enabled) {
        ESP_LOGI(TAG, "disabled by config");
        return ESP_OK;
    }

    int32_t v;
    g.active_scan = false;
    espos_config_get_bool(ESPOS_CFG_NS_BLE, ESPOS_CFG_BLE_ACTIVE_SCAN, &g.active_scan);
    v = 320;  espos_config_get_i32(ESPOS_CFG_NS_BLE, ESPOS_CFG_BLE_SCAN_INT_MS, &v);   g.scan_int = (uint32_t)v;
    v = 160;  espos_config_get_i32(ESPOS_CFG_NS_BLE, ESPOS_CFG_BLE_SCAN_WIN_MS, &v);   g.scan_win = (uint32_t)v;
    v = 2000; espos_config_get_i32(ESPOS_CFG_NS_BLE, ESPOS_CFG_BLE_POST_INT_MS, &v);   g.post_int = (uint32_t)v;
    v = 30000;espos_config_get_i32(ESPOS_CFG_NS_BLE, ESPOS_CFG_BLE_STATUS_INT_MS, &v); g.status_int = (uint32_t)v;
    v = 3;    espos_config_get_i32(ESPOS_CFG_NS_BLE, ESPOS_CFG_BLE_MAX_GATT_SESS, &v); g.max_gatt = (uint32_t)v;
    g.control_ws = true;
    espos_config_get_bool(ESPOS_CFG_NS_BLE, ESPOS_CFG_BLE_CONTROL_WS, &g.control_ws);

    int32_t cap = 500;
    espos_config_get_i32(ESPOS_CFG_NS_BLE, ESPOS_CFG_BLE_MAX_PEND_ADS, &cap);
    g.storage = calloc((size_t)cap, sizeof(espos_ble_adv_t));
    if (!g.storage) return ESP_ERR_NO_MEM;
    espos_ble_advq_init(&g.q, g.storage, (size_t)cap);

    g.lock = xSemaphoreCreateMutex();
    if (!g.lock) { free(g.storage); return ESP_ERR_NO_MEM; }

    espos_ble_callbacks_t cb = {
        .on_advertisement = on_advertisement,
        .on_gatt_connected = on_gatt_connected,
        .on_gatt_disconnected = on_gatt_disconnected,
        .on_gatt_notify = on_gatt_notify,
        .on_gatt_read = on_gatt_read,
        .on_gatt_write_done = on_gatt_write_done,
        .on_gatt_error = on_gatt_error,
    };
    esp_err_t err = espos_ble_backend_init(&cb);
    if (err != ESP_OK) {
        vSemaphoreDelete(g.lock);
        free(g.storage);
        return err;
    }

    espos_ble_scan_start(g.active_scan, (uint16_t)g.scan_int, (uint16_t)g.scan_win);

    g.running = true;
    /* 8 KB: cJSON serialisation plus esp_http_client's own frame need real
     * headroom, and a stack-protection fault here is a reboot loop rather
     * than a degraded mode. The big scratch buffers are static (see
     * s_post_batch) so this covers the call depth, not the payloads. */
    if (xTaskCreate(gateway_task, "espos_ble", 8192, NULL, 5, &g.task) != pdPASS) {
        g.running = false;
        return ESP_ERR_NO_MEM;
    }
    /* Non-fatal: the gateway still works without its status endpoint. */
    esp_err_t api_err = espos_ble_register_api();
    if (api_err != ESP_OK) {
        ESP_LOGW(TAG, "status endpoint unavailable: %s", esp_err_to_name(api_err));
    }

    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

esp_err_t espos_ble_stop(void)
{
    if (!g.running) return ESP_OK;
    g.running = false;
    espos_ble_scan_stop();
    if (g.ws) {
        esp_websocket_client_stop(g.ws);
        esp_websocket_client_destroy(g.ws);
        g.ws = NULL;
    }
    return ESP_OK;
}

esp_err_t espos_ble_get_status(espos_ble_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->enabled = g.running;
    out->scanning = espos_ble_is_scanning();
    snprintf(out->mac, sizeof(out->mac), "%s", espos_ble_mac());
    out->scan_hits = espos_ble_scan_hits();
    out->adv_received = g.adv_received;
    out->adv_posted = g.adv_posted;
    out->adv_dropped = g.q.dropped;
    out->adv_pending = espos_ble_advq_count(&g.q);
    out->post_success = g.post_ok;
    out->post_fail = g.post_fail;
    out->ws_connected = g.ws_connected;
    out->gatt_sessions = espos_ble_gatt_active_count();
    out->gatt_max = g.max_gatt;
    return ESP_OK;
}

#endif /* CONFIG_BT_BLUEDROID_ENABLED */

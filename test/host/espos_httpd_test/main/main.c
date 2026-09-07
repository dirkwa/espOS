/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host harness: config store on emulated NVS + real REST server. The port
 * comes from ESPOS_TEST_PORT (default 18080). run_test.py drives it.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_private/partition_linux.h"
#include "nvs_flash.h"

#include "espos_cfg_keys.h"
#include "espos_config.h"
#include "espos_httpd.h"
#include "espos_wifi.h"
#include "espos_sk.h"
#include "espos_ota.h"

static const char *TAG = "harness";
static volatile sig_atomic_t s_terminate;

static void on_sigterm(int sig)
{
    (void)sig;
    s_terminate = 1;
}

/* Remove the emulated-flash temp file. Runs on esp_restart() (shutdown
 * handler) and on SIGTERM from the runner. */
static void cleanup_flash_file(void)
{
    esp_partition_get_file_mmap_ctrl_input()->remove_dump = true;
    esp_partition_file_munmap();
}

static void on_change(const char *ns, const char *key, void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "config changed: %s.%s", ns, key);
}

/* ---- M7 probe: subscribe to a few families and expose what arrives ---- */
#include "freertos/semphr.h"
#include "espos_httpd.h"
#include "espos_sk_parse.h"
#include "espos_sk_http.h"
#include "cJSON.h"

typedef struct {
    char path[96];
    char value[256];
    char meta[256];
    char src[32];
} rx_t;
static rx_t s_rx[64];
static size_t s_rx_n, s_rx_total;
static SemaphoreHandle_t s_rx_lock;
static char s_put_result[256] = "";

static void on_update(const espos_sk_update_t *u, void *arg)
{
    (void)arg;
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    s_rx_total++;
    if (s_rx_n < 64) {
        rx_t *r = &s_rx[s_rx_n++];
        snprintf(r->path, sizeof(r->path), "%s", u->path);
        snprintf(r->value, sizeof(r->value), "%s", u->value_json ? u->value_json : "");
        snprintf(r->meta, sizeof(r->meta), "%s", u->meta_json ? u->meta_json : "");
        snprintf(r->src, sizeof(r->src), "%s", u->source ? u->source : "");
    }
    xSemaphoreGive(s_rx_lock);
}

static esp_err_t rx_get(httpd_req_t *req)
{
    char *out = malloc(64 * 700 + 64);
    if (!out) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "no_mem", "");
    }
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    int n = snprintf(out, 64, "{\"count\":%u,\"items\":[", (unsigned)s_rx_total);
    for (size_t i = 0; i < s_rx_n; i++) {
        n += snprintf(out + n, 700, "%s{\"path\":\"%s\",\"value\":%s,\"meta\":%s,\"source\":\"%s\"}", i ? "," : "",
                      s_rx[i].path, s_rx[i].value[0] ? s_rx[i].value : "null", s_rx[i].meta[0] ? s_rx[i].meta : "null", s_rx[i].src);
    }
    snprintf(out + n, 8, "]}");
    xSemaphoreGive(s_rx_lock);
    esp_err_t r = espos_httpd_send_json(req, NULL, out);
    free(out);
    return r;
}

static esp_err_t rx_clear(httpd_req_t *req)
{
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    s_rx_n = 0;
    s_rx_total = 0;
    xSemaphoreGive(s_rx_lock);
    return espos_httpd_send_json(req, NULL, "{\"status\":\"cleared\"}");
}

static int s_sub_handles[8];
static int s_sub_n;

/* POST {"pattern":"navigation.*","period_ms":500} → subscribe; {"unsubscribe":<handle>} */
static esp_err_t sub_post(httpd_req_t *req)
{
    char *body = NULL;
    size_t len = 0;
    if (espos_httpd_read_body(req, &body, &len) != ESP_OK) {
        return ESP_FAIL;
    }
    cJSON *j = cJSON_ParseWithLength(body, len);
    free(body);
    const cJSON *pat = cJSON_GetObjectItem(j, "pattern");
    const cJSON *per = cJSON_GetObjectItem(j, "period_ms");
    const cJSON *un = cJSON_GetObjectItem(j, "unsubscribe");
    char out[64];
    if (cJSON_IsNumber(un)) {
        esp_err_t e = espos_sk_unsubscribe(un->valueint);
        snprintf(out, sizeof(out), "{\"result\":\"%s\"}", esp_err_to_name(e));
    } else if (cJSON_IsString(pat)) {
        int h = espos_sk_subscribe(pat->valuestring, cJSON_IsNumber(per) ? (uint32_t)per->valuedouble : 0, on_update, NULL);
        if (h > 0 && s_sub_n < 8) {
            s_sub_handles[s_sub_n++] = h;
        }
        snprintf(out, sizeof(out), "{\"handle\":%d}", h);
    } else {
        cJSON_Delete(j);
        return espos_httpd_send_error(req, "400 Bad Request", "validation", "pattern or unsubscribe");
    }
    cJSON_Delete(j);
    return espos_httpd_send_json(req, NULL, out);
}

static void on_put_result(const char *request_id, const char *state, int status_code, const char *message, void *arg)
{
    (void)arg;
    snprintf(s_put_result, sizeof(s_put_result), "{\"request_id\":\"%s\",\"state\":\"%s\",\"status_code\":%d,\"message\":\"%s\"}",
             request_id, state, status_code, message);
}

/* POST {"path":..,"value":<json>} → espos_sk_put; {"raw": "<frame>"} → send_raw */
static esp_err_t put_post(httpd_req_t *req)
{
    char *body = NULL;
    size_t len = 0;
    if (espos_httpd_read_body(req, &body, &len) != ESP_OK) {
        return ESP_FAIL;
    }
    cJSON *j = cJSON_ParseWithLength(body, len);
    free(body);
    const cJSON *path = cJSON_GetObjectItem(j, "path");
    const cJSON *val = cJSON_GetObjectItem(j, "value");
    const cJSON *raw = cJSON_GetObjectItem(j, "raw");
    esp_err_t e;
    if (cJSON_IsString(raw)) {
        e = espos_sk_send_raw(raw->valuestring);
    } else if (cJSON_IsString(path) && val) {
        char *v = cJSON_PrintUnformatted(val);
        s_put_result[0] = '\0';
        e = espos_sk_put(path->valuestring, v, on_put_result, NULL);
        free(v);
    } else {
        cJSON_Delete(j);
        return espos_httpd_send_error(req, "400 Bad Request", "validation", "path+value or raw");
    }
    cJSON_Delete(j);
    char out[64];
    snprintf(out, sizeof(out), "{\"result\":\"%s\"}", esp_err_to_name(e));
    return espos_httpd_send_json(req, e == ESP_OK ? NULL : "409 Conflict", out);
}

static esp_err_t put_result_get(httpd_req_t *req)
{
    return espos_httpd_send_json(req, NULL, s_put_result[0] ? s_put_result : "null");
}

/* ---- HTTP helper probe: espos_sk_http_* against MockSignalK ----
 *
 * POST {"op":"get"|"put"|"post"|"delete","path":"/signalk"[,"json":"{…}","max_body":N,"timeout_ms":N,"no_auth":true,"no_report":true]}
 *      → {"err":"ESP_OK","status":200,"len":123,"truncated":false,"body":"…"}
 * POST {"op":"value"|"meta","path":"navigation.speedOverGround"} → {"err":…,"json":<value or null>}
 * POST {"op":"url"|"ws_url","path":"/x"}                          → {"err":…,"url":"…"}
 *
 * Blocks the httpd task for the whole request, which is fine in a harness
 * and exactly what espos_sk_http.h tells an application not to do. */
static esp_err_t http_probe_post(httpd_req_t *req)
{
    char *body = NULL;
    size_t len = 0;
    if (espos_httpd_read_body(req, &body, &len) != ESP_OK) {
        return ESP_FAIL;
    }
    cJSON *j = cJSON_ParseWithLength(body, len);
    free(body);
    const cJSON *op = cJSON_GetObjectItem(j, "op");
    const cJSON *path = cJSON_GetObjectItem(j, "path");
    if (!cJSON_IsString(op) || !cJSON_IsString(path)) {
        cJSON_Delete(j);
        return espos_httpd_send_error(req, "400 Bad Request", "validation", "op and path");
    }
    const char *o = op->valuestring;
    const char *p = path->valuestring;
    cJSON *out = cJSON_CreateObject();
    if (strcmp(o, "url") == 0 || strcmp(o, "ws_url") == 0) {
        char url[ESPOS_SK_URL_MAX];
        esp_err_t e = strcmp(o, "url") == 0 ? espos_sk_url(p, url, sizeof(url)) : espos_sk_ws_url(p, url, sizeof(url));
        cJSON_AddStringToObject(out, "err", esp_err_to_name(e));
        cJSON_AddStringToObject(out, "url", e == ESP_OK ? url : "");
    } else if (strcmp(o, "value") == 0 || strcmp(o, "meta") == 0) {
        char *js = NULL;
        esp_err_t e = strcmp(o, "value") == 0 ? espos_sk_get_value(p, &js) : espos_sk_get_meta(p, &js);
        cJSON_AddStringToObject(out, "err", esp_err_to_name(e));
        if (js) {
            cJSON_AddRawToObject(out, "json", js);
        } else {
            cJSON_AddNullToObject(out, "json");
        }
        free(js);
    } else {
        const cJSON *json = cJSON_GetObjectItem(j, "json");
        const cJSON *mb = cJSON_GetObjectItem(j, "max_body");
        const cJSON *tm = cJSON_GetObjectItem(j, "timeout_ms");
        espos_sk_http_opts_t opts = {
            .max_body = cJSON_IsNumber(mb) ? (size_t)mb->valuedouble : 0,
            .timeout_ms = cJSON_IsNumber(tm) ? (uint32_t)tm->valuedouble : 0,
            .no_auth = cJSON_IsTrue(cJSON_GetObjectItem(j, "no_auth")),
            .no_report_unauthorized = cJSON_IsTrue(cJSON_GetObjectItem(j, "no_report")),
        };
        const char *jb = cJSON_IsString(json) ? json->valuestring : NULL;
        espos_sk_http_resp_t r;
        esp_err_t e;
        if (strcmp(o, "get") == 0) {
            e = espos_sk_http_get(p, &opts, &r);
        } else if (strcmp(o, "put") == 0) {
            e = espos_sk_http_put(p, jb, &opts, &r);
        } else if (strcmp(o, "post") == 0) {
            e = espos_sk_http_post(p, jb, &opts, &r);
        } else if (strcmp(o, "delete") == 0) {
            e = espos_sk_http_delete(p, &opts, &r);
        } else {
            cJSON_Delete(out);
            cJSON_Delete(j);
            return espos_httpd_send_error(req, "400 Bad Request", "validation", "op");
        }
        cJSON_AddStringToObject(out, "err", esp_err_to_name(e));
        cJSON_AddNumberToObject(out, "status", r.status);
        cJSON_AddNumberToObject(out, "len", (double)r.len);
        cJSON_AddBoolToObject(out, "truncated", r.truncated);
        cJSON_AddStringToObject(out, "body", r.body ? r.body : "");
        espos_sk_http_resp_free(&r);
    }
    cJSON_Delete(j);
    char *txt = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (!txt) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "no_mem", "");
    }
    esp_err_t rc = espos_httpd_send_json(req, NULL, txt);
    free(txt);
    return rc;
}

static void harness_sk_inbound_init(void)
{
    s_rx_lock = xSemaphoreCreateMutex();
    static const httpd_uri_t uris[] = {
        { .uri = "/__harness/sk/rx", .method = HTTP_GET, .handler = rx_get },
        { .uri = "/__harness/sk/rx", .method = HTTP_DELETE, .handler = rx_clear },
        { .uri = "/__harness/sk/sub", .method = HTTP_POST, .handler = sub_post },
        { .uri = "/__harness/sk/put", .method = HTTP_POST, .handler = put_post },
        { .uri = "/__harness/sk/put", .method = HTTP_GET, .handler = put_result_get },
        { .uri = "/__harness/sk/http", .method = HTTP_POST, .handler = http_probe_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(espos_httpd_register(&uris[i]));
    }
}

void app_main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0); /* the runner reads us through a pipe */
    signal(SIGTERM, on_sigterm);
    signal(SIGINT, on_sigterm);
    /* A write to a socket whose peer went away raises SIGPIPE on Linux (no
     * such thing on lwIP); without this the harness dies silently the first
     * time an SSE client disconnects. */
    signal(SIGPIPE, SIG_IGN);
    esp_register_shutdown_handler(cleanup_flash_file);
    const char *port_env = getenv("ESPOS_TEST_PORT");
    int port = port_env ? atoi(port_env) : 18080;
    if (getenv("ESPOS_TEST_FRESH")) {
        (void)nvs_flash_erase_partition("nvs");
    }
    ESP_ERROR_CHECK(espos_config_init(NULL, NULL));
    ESP_ERROR_CHECK(espos_config_subscribe(on_change, NULL));
    /* The harness overrides the configured port so the runner can pick one. */
    ESP_ERROR_CHECK(espos_config_set_i32(ESPOS_CFG_NS_HTTPD, ESPOS_CFG_HTTPD_PORT, port));
    ESP_ERROR_CHECK(espos_httpd_start());
    ESP_ERROR_CHECK(espos_wifi_start()); /* simulated driver on the host, see port_sim.c */
    ESP_ERROR_CHECK(espos_sk_start());   /* real HTTP; servers from ESPOS_SIM_SK_SERVERS */
    ESP_ERROR_CHECK(espos_ota_start());  /* sim port: downloads counted, no flash */
    harness_sk_inbound_init();           /* subscriptions, PUT and HTTP-helper probe endpoints */
    /* Announce readiness with a raw write loop: stdio gives up on EINTR
     * (which the simulator's tick signals can cause) and would silently drop
     * the line. */
    char ready[64];
    int n = snprintf(ready, sizeof(ready), "ESPOS_HARNESS_READY port=%d\n", port);
    fflush(stdout);
    for (int off = 0; off < n;) {
        ssize_t w = write(STDOUT_FILENO, ready + off, (size_t)(n - off));
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            break;
        }
        off += (int)w;
    }
    while (!s_terminate) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    cleanup_flash_file();
    exit(0);
}

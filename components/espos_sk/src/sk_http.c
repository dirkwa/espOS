/*
 * SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * Blocking HTTP calls of the access-request protocol (esp_http_client).
 * Bodies are small JSON documents; responses are capped at 4 KiB.
 *
 *   POST /signalk/v1/access/requests {clientId, description, permissions}
 *        202 {state:"PENDING", href}      400 {message}   403 / 404 / 503
 *   GET  <href>   200 {state:"PENDING"|"COMPLETED", accessRequest:{permission, token}}
 *                 500 "Unable to check request: not found" when the server forgot it
 *   GET  /signalk/v1/api/self  (Bearer)   200 "vessels.<self>"   401 / 403
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#if CONFIG_ESPOS_SK_TLS
#include "esp_crt_bundle.h"
#endif
#include "esp_log.h"
#include "sdkconfig.h"

#include "espos_sk_priv.h"

static const char *TAG = "espos_sk";
#define BODY_MAX 4096
#define TIMEOUT_MS 6000

typedef struct {
    char *buf;
    size_t len;
} body_t;

static esp_err_t on_event(esp_http_client_event_t *evt)
{
    body_t *b = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && b && evt->data_len > 0) {
        size_t room = BODY_MAX - 1 - b->len;
        size_t n = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
        memcpy(b->buf + b->len, evt->data, n);
        b->len += n;
        b->buf[b->len] = '\0';
    }
    return ESP_OK;
}

/* Perform one request; returns HTTP status (0 on transport failure) and the
 * body in *body (malloc'ed, NUL-terminated, may be empty). */
static int perform(const espos_sk_server_t *srv, esp_http_client_method_t method, const char *path,
                   const char *json_body, const char *bearer, char **body_out)
{
    char url[8 + ESPOS_SK_HOST_MAX + 8 + ESPOS_SK_HREF_MAX]; /* scheme + host + :port + path */
    snprintf(url, sizeof(url), "%s://%s:%u%s", srv->tls ? "https" : "http", srv->host,
             (unsigned)srv->port, path);
    body_t b = { .buf = calloc(1, BODY_MAX), .len = 0 };
    *body_out = NULL;
    if (!b.buf) {
        return 0;
    }
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = TIMEOUT_MS,
        .event_handler = on_event,
        .user_data = &b,
        .disable_auto_redirect = true,
        .keep_alive_enable = false,
        .buffer_size_tx = 1536, /* room for "Authorization: Bearer <jwt up to 1 KiB>" */
#if CONFIG_ESPOS_SK_TLS
        /* Server certificates are checked against the bundled Mozilla roots.
         * A boat server with a self-signed certificate will be refused, which
         * is the honest outcome: espOS has nowhere to pin a private CA yet,
         * and quietly accepting any certificate would make the setting a
         * decoration. See docs/signalk.md. */
        .crt_bundle_attach = srv->tls ? esp_crt_bundle_attach : NULL,
#endif
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        free(b.buf);
        return 0;
    }
    esp_http_client_set_header(c, "Accept", "application/json");
    if (json_body) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_post_field(c, json_body, (int)strlen(json_body));
    }
    char auth[ESPOS_SK_TOKEN_MAX + 8];
    if (bearer && bearer[0]) {
        snprintf(auth, sizeof(auth), "Bearer %s", bearer);
        esp_http_client_set_header(c, "Authorization", auth);
    }
    esp_err_t err = esp_http_client_perform(c);
    int status = 0;
    if (err == ESP_OK) {
        status = esp_http_client_get_status_code(c);
    } else {
        /* esp_http_client treats 401 as "needs auth" and fails perform()
         * before reading the body; the status is still there and, for us,
         * a perfectly good answer (token rejected / not enabled). */
        int sc = esp_http_client_get_status_code(c);
        if (sc == 401 || sc == 403) {
            status = sc;
        } else {
            ESP_LOGW(TAG, "%s %s: %s", method == HTTP_METHOD_POST ? "POST" : "GET", url, esp_err_to_name(err));
        }
    }
    esp_http_client_cleanup(c);
    *body_out = b.buf;
    return status;
}

static void copy_str(char *dst, size_t size, const cJSON *j)
{
    if (cJSON_IsString(j) && j->valuestring) {
        snprintf(dst, size, "%s", j->valuestring);
    } else {
        dst[0] = '\0';
    }
}

/* Parse a request/poll reply document into the result. */
static void parse_reply(const char *body, espos_sk_http_result_t *out)
{
    cJSON *j = cJSON_Parse(body);
    if (!j) {
        /* plain-text bodies ("Unable to check request: not found") */
        snprintf(out->message, sizeof(out->message), "%s", body);
        return;
    }
    copy_str(out->state, sizeof(out->state), cJSON_GetObjectItem(j, "state"));
    copy_str(out->href, sizeof(out->href), cJSON_GetObjectItem(j, "href"));
    copy_str(out->message, sizeof(out->message), cJSON_GetObjectItem(j, "message"));
    cJSON *ar = cJSON_GetObjectItem(j, "accessRequest");
    if (cJSON_IsObject(ar)) {
        copy_str(out->permission, sizeof(out->permission), cJSON_GetObjectItem(ar, "permission"));
        copy_str(out->token, sizeof(out->token), cJSON_GetObjectItem(ar, "token"));
    }
    /* a COMPLETED reply may carry the failing statusCode inside the body */
    cJSON *sc = cJSON_GetObjectItem(j, "statusCode");
    if (cJSON_IsNumber(sc) && strcmp(out->state, "COMPLETED") == 0 && sc->valueint >= 400 && out->permission[0] == '\0') {
        if (!out->message[0]) {
            snprintf(out->message, sizeof(out->message), "request rejected (%d)", sc->valueint);
        }
    }
    cJSON_Delete(j);
}

void espos_sk_http_request(const espos_sk_server_t *srv, const espos_sk_tok_cfg_t *cfg, espos_sk_http_result_t *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "clientId", cfg->client_id);
    cJSON_AddStringToObject(j, "description", cfg->description);
    cJSON_AddStringToObject(j, "permissions", cfg->permissions);
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!body) {
        return;
    }
    char *resp = NULL;
    out->http_status = perform(srv, HTTP_METHOD_POST, "/signalk/v1/access/requests", body, NULL, &resp);
    free(body);
    if (resp) {
        parse_reply(resp, out);
        free(resp);
    }
    if (out->http_status == 404) {
        /* 404 means "security disabled" only on a SignalK server; a random
         * HTTP host says 404 too. GET /signalk tells them apart. */
        char *probe = NULL;
        int st = perform(srv, HTTP_METHOD_GET, "/signalk", NULL, NULL, &probe);
        bool is_sk = st == 200 && probe && strstr(probe, "endpoints");
        free(probe);
        if (!is_sk) {
            out->http_status = 599;
        }
    }
    ESP_LOGI(TAG, "access request → %d %s %s", out->http_status, out->state, out->href[0] ? out->href : out->message);
}

void espos_sk_http_poll(const espos_sk_server_t *srv, const char *href, espos_sk_http_result_t *out)
{
    memset(out, 0, sizeof(*out));
    char *resp = NULL;
    out->http_status = perform(srv, HTTP_METHOD_GET, href, NULL, NULL, &resp);
    if (resp) {
        parse_reply(resp, out);
        free(resp);
    }
    ESP_LOGD(TAG, "poll → %d %s %s", out->http_status, out->state, out->permission);
}

void espos_sk_http_verify(const espos_sk_server_t *srv, const char *token, espos_sk_http_result_t *out)
{
    memset(out, 0, sizeof(*out));
    char *resp = NULL;
    out->http_status = perform(srv, HTTP_METHOD_GET, "/signalk/v1/api/self", NULL, token, &resp);
    if (resp && out->http_status == 200) {
        /* body is a JSON string: "vessels.urn:mrn:..." */
        cJSON *j = cJSON_Parse(resp);
        if (cJSON_IsString(j) && j->valuestring) {
            const char *v = j->valuestring;
            if (strncmp(v, "vessels.", 8) == 0) {
                v += 8;
            }
            snprintf(out->self, sizeof(out->self), "%s", v);
        }
        cJSON_Delete(j);
    }
    free(resp);
    ESP_LOGD(TAG, "verify → %d %s", out->http_status, out->self);
}

/* -------------------------------------------------------------- meta */

static void path_to_url(const char *path, char *out, size_t size)
{
    /* dots become slashes: environment.wind.speedApparent → environment/wind/speedApparent */
    size_t o = snprintf(out, size, "/signalk/v1/api/vessels/self/");
    for (; *path && o + 1 < size; path++) {
        out[o++] = *path == '.' ? '/' : *path;
    }
    snprintf(out + o, size - o, "/meta");
}

int espos_sk_http_get_meta(const espos_sk_server_t *srv, const char *token, const char *path, char **out_meta)
{
    char url[192];
    path_to_url(path, url, sizeof(url));
    *out_meta = NULL;
    char *resp = NULL;
    int status = perform(srv, HTTP_METHOD_GET, url, NULL, token, &resp);
    if (status == 200 && resp) {
        cJSON *j = cJSON_Parse(resp);
        if (cJSON_IsObject(j) && j->child) {
            *out_meta = resp; /* non-empty object: hand the text over */
            resp = NULL;
        }
        cJSON_Delete(j);
    }
    free(resp);
    return status;
}

int espos_sk_http_put_meta(const espos_sk_server_t *srv, const char *token, const char *path, const char *meta_json)
{
    char url[192];
    path_to_url(path, url, sizeof(url));
    size_t n = strlen(meta_json) + 16;
    char *body = malloc(n);
    if (!body) {
        return 0;
    }
    snprintf(body, n, "{\"value\":%s}", meta_json);
    char *resp = NULL;
    int status = perform(srv, HTTP_METHOD_PUT, url, body, token, &resp);
    free(body);
    free(resp);
    return status;
}

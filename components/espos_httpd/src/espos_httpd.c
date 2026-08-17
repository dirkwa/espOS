/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_event.h"
#include "esp_log.h"
#if !CONFIG_IDF_TARGET_LINUX
#include "esp_netif.h"
#endif
#include "sdkconfig.h"

#include "espos_config.h"
#include "espos_cfg_keys.h"
#include "espos_httpd.h"
#include "espos_httpd_priv.h"
#include "espos_httpd_sse.h"

static const char *TAG = "espos_httpd";
static httpd_handle_t s_server;

static void config_changed(const char *ns, const char *key, void *arg)
{
    (void)arg;
    char body[80];
    snprintf(body, sizeof(body), "{\"ns\":\"%s\",\"key\":\"%s\"}", ns, key);
    espos_httpd_sse_publish("config", body);
}

httpd_handle_t espos_httpd_handle(void)
{
    return s_server;
}

esp_err_t espos_httpd_register(const httpd_uri_t *uri)
{
    if (!s_server) {
        return ESP_ERR_INVALID_STATE;
    }
    return httpd_register_uri_handler(s_server, uri);
}

/* Map esp_http_server's own error responses onto the JSON error contract. */
static esp_err_t json_err_handler(httpd_req_t *req, httpd_err_code_t err)
{
    const char *status, *code, *msg;
    switch (err) {
    case HTTPD_404_NOT_FOUND:
        status = "404 Not Found"; code = "not_found"; msg = "no such resource"; break;
    case HTTPD_405_METHOD_NOT_ALLOWED:
        status = "405 Method Not Allowed"; code = "method_not_allowed"; msg = "method not allowed for this resource"; break;
    case HTTPD_400_BAD_REQUEST:
        status = "400 Bad Request"; code = "bad_request"; msg = "malformed request"; break;
    case HTTPD_408_REQ_TIMEOUT:
        status = "408 Request Timeout"; code = "timeout"; msg = "request timed out"; break;
    case HTTPD_411_LENGTH_REQUIRED:
        status = "411 Length Required"; code = "length_required"; msg = "Content-Length required"; break;
    case HTTPD_413_CONTENT_TOO_LARGE:
        status = "413 Payload Too Large"; code = "too_large"; msg = "request too large"; break;
    case HTTPD_414_URI_TOO_LONG:
        status = "414 URI Too Long"; code = "uri_too_long"; msg = "URI too long"; break;
    case HTTPD_431_REQ_HDR_FIELDS_TOO_LARGE:
        status = "431 Request Header Fields Too Large"; code = "headers_too_large"; msg = "request headers too large"; break;
    case HTTPD_501_METHOD_NOT_IMPLEMENTED:
        status = "501 Not Implemented"; code = "not_implemented"; msg = "method not implemented"; break;
    case HTTPD_505_VERSION_NOT_SUPPORTED:
        status = "505 HTTP Version Not Supported"; code = "version_not_supported"; msg = "HTTP version not supported"; break;
    default:
        status = "500 Internal Server Error"; code = "internal"; msg = "internal server error"; break;
    }
    /* Errors that abort request parsing leave the connection unusable. */
    if (err != HTTPD_404_NOT_FOUND && err != HTTPD_405_METHOD_NOT_ALLOWED) {
        httpd_resp_set_hdr(req, "Connection", "close");
    }
    return espos_httpd_send_error(req, status, code, msg);
}

esp_err_t espos_httpd_start(void)
{
    if (s_server) {
        return ESP_OK;
    }
    int32_t port = 80;
    (void)espos_config_get_i32(ESPOS_CFG_NS_HTTPD, ESPOS_CFG_HTTPD_PORT, &port);

    /* esp_http_server posts lifecycle events to the default loop; make sure
     * one exists so it does not log an error on every request. */
    esp_err_t lerr = esp_event_loop_create_default();
    if (lerr != ESP_OK && lerr != ESP_ERR_INVALID_STATE) {
        return lerr;
    }
#if !CONFIG_IDF_TARGET_LINUX
    /* Sockets need the lwIP stack; esp_netif_init() is idempotent and may
     * already have been called by whoever brought a network interface up. */
    lerr = esp_netif_init();
    if (lerr != ESP_OK && lerr != ESP_ERR_INVALID_STATE) {
        return lerr;
    }
#endif
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = (uint16_t)port;
    cfg.ctrl_port = 32768 + (uint16_t)(port & 0x0FFF); /* keep distinct per instance */
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = CONFIG_ESPOS_HTTPD_MAX_URI_HANDLERS;
    cfg.stack_size = CONFIG_ESPOS_HTTPD_STACK_SIZE;
#if CONFIG_IDF_TARGET_LINUX
    /* Host simulator: the depth is in StackType_t (8-byte) units and must fit
     * configSTACK_DEPTH_TYPE (16 bit); 32768 words = 256 KiB pthread stack. */
    if (cfg.stack_size < 32768) {
        cfg.stack_size = 32768;
    }
#endif
    cfg.task_priority = CONFIG_ESPOS_HTTPD_TASK_PRIORITY;
    cfg.lru_purge_enable = true;
    /* Event streams hold a socket each and sit outside the LRU purge; keep
     * a few for regular requests, but never exceed what lwIP can hand out
     * (esp_http_server needs three of CONFIG_LWIP_MAX_SOCKETS for itself,
     * the portal DNS responder one more). */
    cfg.max_open_sockets = 4 + CONFIG_ESPOS_HTTPD_SSE_MAX_CLIENTS;
#ifdef CONFIG_LWIP_MAX_SOCKETS
    if (cfg.max_open_sockets > CONFIG_LWIP_MAX_SOCKETS - 4) {
        cfg.max_open_sockets = CONFIG_LWIP_MAX_SOCKETS - 4;
    }
#endif

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start on port %ld failed: %s", (long)port, esp_err_to_name(err));
        s_server = NULL;
        return err;
    }
    for (int e = 0; e < HTTPD_ERR_CODE_MAX; e++) {
        httpd_register_err_handler(s_server, (httpd_err_code_t)e, json_err_handler);
    }
    ESP_ERROR_CHECK(espos_httpd_register_config_api(s_server));
    ESP_ERROR_CHECK(espos_httpd_register_system_api(s_server));
    ESP_ERROR_CHECK(espos_httpd_register_static(s_server));
    ESP_ERROR_CHECK(espos_httpd_register_sse(s_server));
    /* config changes are pushed to UIs as "config" events */
    (void)espos_config_subscribe(config_changed, NULL);
    ESP_LOGI(TAG, "listening on port %ld", (long)port);
    return ESP_OK;
}

esp_err_t espos_httpd_stop(void)
{
    if (!s_server) {
        return ESP_OK;
    }
    espos_config_unsubscribe(config_changed, NULL);
    espos_httpd_sse_shutdown();
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    return err;
}

/* -------------------------------------------------------------- helpers */

esp_err_t espos_httpd_send_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status ? status : HTTPD_200);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/* Minimal JSON string escaping for short ASCII messages. */
static void json_escape(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    for (; *in && o + 2 < out_size; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            if (o + 3 >= out_size) {
                break;
            }
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            if (o + 7 >= out_size) {
                break;
            }
            o += (size_t)snprintf(out + o, out_size - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

esp_err_t espos_httpd_send_error(httpd_req_t *req, const char *status, const char *code, const char *msg)
{
    char ecode[48], emsg[160], body[256];
    json_escape(code ? code : "error", ecode, sizeof(ecode));
    json_escape(msg ? msg : "", emsg, sizeof(emsg));
    snprintf(body, sizeof(body), "{\"error\":\"%s\",\"message\":\"%s\"}", ecode, emsg);
    return espos_httpd_send_json(req, status, body);
}

bool espos_httpd_require_json(httpd_req_t *req)
{
    char ct[64] = { 0 };
    esp_err_t err = httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct));
    /* ESP_ERR_HTTPD_RESULT_TRUNC still leaves the prefix in ct. */
    if ((err == ESP_OK || err == ESP_ERR_HTTPD_RESULT_TRUNC) &&
        strncasecmp(ct, "application/json", strlen("application/json")) == 0) {
        return true;
    }
    espos_httpd_send_error(req, "415 Unsupported Media Type", "unsupported_media_type",
                           "state-changing requests need Content-Type: application/json");
    return false;
}

esp_err_t espos_httpd_read_body(httpd_req_t *req, char **out, size_t *out_len)
{
    *out = NULL;
    if (out_len) {
        *out_len = 0;
    }
    size_t total = req->content_len;
    if (total > CONFIG_ESPOS_HTTPD_MAX_BODY) {
        /* Drain a bounded amount so the 413 reaches the client before the
         * socket closes (unread data would otherwise turn into a TCP RST).
         * Anything beyond the drain cap is abusive; the RST is acceptable. */
        char sink[256];
        size_t drained = 0;
        while (drained < total && drained < 4 * CONFIG_ESPOS_HTTPD_MAX_BODY) {
            size_t want = total - drained < sizeof(sink) ? total - drained : sizeof(sink);
            int r = httpd_req_recv(req, sink, want);
            if (r <= 0) {
                break;
            }
            drained += (size_t)r;
        }
        httpd_resp_set_hdr(req, "Connection", "close");
        espos_httpd_send_error(req, "413 Payload Too Large", "too_large", "request body too large");
        return ESP_ERR_INVALID_SIZE;
    }
    char *buf = malloc(total + 1);
    if (!buf) {
        espos_httpd_send_error(req, "500 Internal Server Error", "no_mem", "out of memory");
        return ESP_ERR_NO_MEM;
    }
    size_t got = 0;
    int timeouts = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 1) {
            continue; /* recv_wait_timeout expired once; give the client one more window */
        }
        if (r <= 0) {
            free(buf);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_set_hdr(req, "Connection", "close");
                espos_httpd_send_error(req, "408 Request Timeout", "timeout", "request body did not arrive");
                return ESP_ERR_TIMEOUT;
            }
            return ESP_FAIL;
        }
        got += (size_t)r;
    }
    buf[got] = '\0';
    *out = buf;
    if (out_len) {
        *out_len = got;
    }
    return ESP_OK;
}

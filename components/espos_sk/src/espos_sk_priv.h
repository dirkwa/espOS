/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "espos_sk.h"
#include "espos_sk_http.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Persistent device state (NVS namespace "skstate"): sk_store.c */
esp_err_t espos_sk_store_load(espos_sk_tok_store_t *out, char client_id[40]);
esp_err_t espos_sk_store_save(const espos_sk_tok_store_t *st);

/* HTTP (blocking, esp_http_client): sk_http.c */
void espos_sk_http_request(const espos_sk_server_t *srv, const espos_sk_tok_cfg_t *cfg, espos_sk_http_result_t *out);
void espos_sk_http_poll(const espos_sk_server_t *srv, const char *href, espos_sk_http_result_t *out);
void espos_sk_http_verify(const espos_sk_server_t *srv, const char *token, espos_sk_http_result_t *out);

/* The shared HTTP core (sk_http.c): one esp_http_client per call, perform()
 * only -- see espos_sk_http.h for why. srv, path and bearer are the caller's:
 * the token machine and the stream task pass what they were handed, the
 * public API (sk_http_api.c) snapshots the selected server and the current
 * token. timeout_ms and max_body must be set. Returns ESP_OK when a status
 * arrived (any status); r->body is then the caller's to free. */
typedef enum {
    ESPOS_SK_HTTP_GET = 0,
    ESPOS_SK_HTTP_PUT = 1,
    ESPOS_SK_HTTP_POST = 2,
    ESPOS_SK_HTTP_DELETE = 3,
} espos_sk_http_method_t;

typedef struct {
    const espos_sk_server_t *srv;
    espos_sk_http_method_t method;
    const char *path;        /* absolute: "/signalk/..." */
    const char *json_body;   /* NULL = no body */
    const char *bearer;      /* NULL or "" = no Authorization header */
    const char *accept;      /* NULL = application/json, "" = none */
    uint32_t timeout_ms;
    size_t max_body;
} espos_sk_http_req_t;

esp_err_t espos_sk_http_perform(const espos_sk_http_req_t *rq, espos_sk_http_resp_t *r);
/* "<scheme>://host:port/path"; ESP_ERR_INVALID_SIZE when it does not fit. */
esp_err_t espos_sk_http_build_url(const espos_sk_server_t *srv, const char *scheme, const char *path, char *out, size_t n);
/* "/signalk/v1/api/vessels/self/<a/b/c><suffix>" from a dotted path. */
esp_err_t espos_sk_http_self_path(const char *sk_path, const char *suffix, char *out, size_t n);

/* Discovery (blocking ~3 s): discovery.c / discovery_sim.c */
esp_err_t espos_sk_discovery_init(const char *hostname);
size_t espos_sk_discovery_run(espos_sk_discovered_t *out, size_t max);

/* HTTP endpoints: api_sk.c */
esp_err_t espos_sk_register_api(void);

/* WebSocket delta stream + meta reconciliation + health (sk_ws.c). */
esp_err_t espos_sk_ws_start(void);
void espos_sk_ws_stop(void);
void espos_sk_ws_config_changed(void);
/* Meta GET/PUT (sk_http.c). *out_meta receives the JSON object text (malloc'ed) or NULL if none. */
int espos_sk_http_get_meta(const espos_sk_server_t *srv, const char *token, const char *path, char **out_meta);
int espos_sk_http_put_meta(const espos_sk_server_t *srv, const char *token, const char *path, const char *meta_json);
/* JSON snippet for the status document. */
char *espos_sk_ws_status_json(void);
bool espos_sk_stream_allowed(void);

#ifdef __cplusplus
}
#endif

/* sk_inbound.c ↔ sk_ws.c */
void espos_sk_inbound_set_connected(bool connected);
char *espos_sk_inbound_take_frame(void);            /* malloc'ed; NULL when nothing to send */
bool espos_sk_inbound_handle_frame(const char *json, size_t len, char *err_out, size_t err_size); /* false = server error frame */
void espos_sk_inbound_tick(uint32_t now_ms);
void espos_sk_inbound_stats(espos_sk_ws_status_t *st);

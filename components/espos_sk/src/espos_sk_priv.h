/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "espos_sk.h"

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

/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_sk — SignalK server discovery and access-token management (M3);
 * WebSocket delta output and meta reconciliation follow in M4.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "espos_sk_token_sm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPOS_SK_MAX_SERVERS 6

typedef struct {
    char host[ESPOS_SK_HOST_MAX];   /* IPv4 dotted or hostname */
    uint16_t port;
    char self[ESPOS_SK_SELF_MAX];
    char name[48];                  /* mDNS instance name */
    char roles[32];
    char swname[24];
    char swvers[16];
    uint32_t seen_ms;
} espos_sk_discovered_t;

/** Start the SignalK task: loads config + persistent state, runs discovery
 * and the token machine, registers the /api/v1/sk endpoints. Needs espos_config,
 * espos_httpd and (on device) espos_wifi to be started. */
esp_err_t espos_sk_start(void);
esp_err_t espos_sk_stop(void);

/** Status document of docs/api.md (malloc'ed JSON). */
esp_err_t espos_sk_status_json(char **out_json);
/** Discovered servers as JSON array document {"servers":[...]} (malloc'ed). */
esp_err_t espos_sk_servers_json(char **out_json);

/* Commands (thread-safe, queued to the SK task). */
esp_err_t espos_sk_discover_now(void);
esp_err_t espos_sk_request_now(void);            /* re-request access (from denied/error) */
esp_err_t espos_sk_set_token(const char *token); /* manual token paste */
esp_err_t espos_sk_forget_token(void);           /* drop token + pending, request again */
/** Report that some SK call was rejected with 401/403 (M4 uses this). */
void espos_sk_report_unauthorized(void);

/** Copy the current usable token ("" if none). Thread-safe. */
esp_err_t espos_sk_get_token(char *buf, size_t size);
/** Copy the current server (host/port/self); ESP_ERR_NOT_FOUND if none. */
esp_err_t espos_sk_get_server(espos_sk_server_t *out);
const char *espos_sk_client_id(void);

#ifdef __cplusplus
}
#endif

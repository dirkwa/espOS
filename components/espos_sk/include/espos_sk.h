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

/* ------------------------------------------------------------ deltas */

/** Publish a value for a SignalK path (vessels.self). Values are batched
 * (sk.batch_ms), buffered while offline and streamed over the WebSocket.
 * Thread-safe; never blocks. */
esp_err_t espos_sk_publish_number(const char *path, double value);
esp_err_t espos_sk_publish_string(const char *path, const char *value);
esp_err_t espos_sk_publish_bool(const char *path, bool value);
/** value_json is a complete JSON value (object, array, null, …). */
esp_err_t espos_sk_publish_json(const char *path, const char *value_json);

/**
 * Declare metadata for a NON-standard path (never for spec paths — the
 * server knows those). meta_json is the full meta object, e.g.
 * {"units":"Hz","description":"…"}. period_ms > 0 adds "timeout" (in
 * seconds, 2.5× the period) as the one field the device really owns.
 * Reconciled on every (re)connect: GET the server's meta, PUT only if it
 * is empty — server-side edits win. Up to ESPOS_SK_MAX_META entries.
 */
#define ESPOS_SK_MAX_META 16
esp_err_t espos_sk_declare_meta(const char *path, const char *meta_json, uint32_t period_ms);

typedef struct {
    bool enabled;
    bool connected;
    uint32_t connected_s;
    uint32_t reconnects;      /* successful connections so far */
    uint32_t sent;            /* messages sent */
    uint32_t send_errors;
    uint32_t next_retry_s;    /* while disconnected */
    char last_error[64];
    size_t pending, buffered, buffered_bytes;
    uint32_t dropped;
    size_t meta_declared, meta_reconciled;
} espos_sk_ws_status_t;
esp_err_t espos_sk_ws_get_status(espos_sk_ws_status_t *out);

/** Copy the current usable token ("" if none). Thread-safe. */
esp_err_t espos_sk_get_token(char *buf, size_t size);
/** Copy the current server (host/port/self); ESP_ERR_NOT_FOUND if none. */
esp_err_t espos_sk_get_server(espos_sk_server_t *out);
const char *espos_sk_client_id(void);

#ifdef __cplusplus
}
#endif

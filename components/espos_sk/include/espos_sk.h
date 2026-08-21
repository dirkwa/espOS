/*
 * SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
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

/* A boat network can carry more SignalK servers than you would guess: a
 * plotter, a spare Pi, a laptop running one for development, plus every
 * neighbouring vessel in the marina. Measured on one: 9 advertisements.
 * When the list is smaller than what is advertised, which entries survive
 * depends on mDNS answer order, which is not stable — so the server the
 * device is actually paired with can silently drop out. */
#define ESPOS_SK_MAX_SERVERS 12

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
 * Raise or clear a SignalK notification under notifications.espos.<label>.<key>.
 *
 * For conditions the device knows about and an operator would want to see:
 * memory pressure, an overheating chip, a service the firmware depends on
 * having gone away. These otherwise surface as a device that has quietly
 * stopped doing its job, which is indistinguishable from a hardware fault
 * from the outside and is the expensive kind of problem to diagnose.
 *
 * Notifications are level-triggered and idempotent: raising the same state and
 * message twice sends one delta, so a caller may poll and re-raise freely.
 * Passing ESPOS_SK_ALERT_NORMAL clears the condition.
 *
 * `key` is a short stable identifier ("lowMemory", "wakeService"), not a
 * sentence -- it becomes part of the path, and the path is what a rule or a
 * dashboard keys on. `message` is the human-readable half and may change
 * without re-notifying.
 *
 * Thread-safe; never blocks. Buffered like any other delta while offline.
 */
typedef enum {
    ESPOS_SK_ALERT_NORMAL = 0,  /* condition cleared */
    ESPOS_SK_ALERT_WARN,
    ESPOS_SK_ALERT_ALARM,
} espos_sk_alert_t;

esp_err_t espos_sk_notify(const char *key, espos_sk_alert_t state, const char *message);

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

/* ------------------------------------------------------- inbound (M7) */

#include "espos_sk_parse.h"

/**
 * Subscribe to values (and meta) of vessels.self paths. `pattern` is an
 * exact path or a family: "notifications.*", "environment.*", "*".
 * period_ms is the server-side rate hint (0 = 1000). cb runs on the stream
 * task with strings valid only during the call — copy, do not block, do
 * not call espos_sk_* that could wait on the stream. Meta arrives as items
 * with meta_json set (stream opened with sendMeta=all). Subscriptions
 * survive reconnects and are (re)sent after every hello. Returns a
 * handle > 0, or <0 (-ESP_ERR_NO_MEM style) when the table is full.
 */
typedef void (*espos_sk_sub_cb_t)(const espos_sk_update_t *u, void *arg);
#define ESPOS_SK_MAX_SUBS 48
int espos_sk_subscribe(const char *pattern, uint32_t period_ms, espos_sk_sub_cb_t cb, void *arg);
esp_err_t espos_sk_unsubscribe(int handle);

/**
 * PUT a value (JSON text) to a vessels.self path over the stream. cb (may
 * be NULL) gets the server's response — state COMPLETED/FAILED with
 * statusCode — or state "TIMEOUT" after 10 s. Queued if the stream is
 * momentarily busy; ESP_ERR_INVALID_STATE when not connected, ESP_ERR_NO_MEM
 * when 8 requests are already in flight.
 */
typedef void (*espos_sk_put_cb_t)(const char *request_id, const char *state, int status_code, const char *message, void *arg);
esp_err_t espos_sk_put(const char *path, const char *value_json, espos_sk_put_cb_t cb, void *arg);

/** Send an arbitrary text frame on the stream (e.g. an inbound delta the
 * server should ingest as-is). ESP_ERR_INVALID_STATE when not connected. */
esp_err_t espos_sk_send_raw(const char *json);

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
    uint32_t received;        /* value/meta items delivered to subscribers */
    uint32_t frames;          /* text frames read */
    size_t subs;              /* active subscriptions */
    size_t puts_pending;
    uint32_t puts_sent, puts_failed;
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

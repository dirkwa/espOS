/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_sk_http — one correct way to talk HTTP to the selected SignalK
 * server: GET/PUT/POST/DELETE with the access token, URLs that follow the
 * server's scheme, and the two lookups a display needs most (a path's
 * current value and its meta).
 *
 * Why a helper and not "just use esp_http_client": four hand-rolled copies
 * of this code in one firmware had two of them rebooting the device. Both
 * faults were seen on the ESP32-P4 and both are in esp_http_client itself:
 *
 *   1. esp_http_client_open() / fetch_headers() / read() leaves the client's
 *      cache_data_in_fetch_hdr flag set, and a body that arrives in the same
 *      segment as the headers -- every small SignalK reply -- then trips
 *      assert(orig_raw_data == raw_data) in http_on_body. Only perform()
 *      clears the flag.
 *   2. Reusing one handle across esp_http_client_perform() calls desyncs the
 *      same two pointers and hits the same assert mid-batch.
 *
 * Contract (every function below):
 *   - a fresh esp_http_client per call and esp_http_client_perform() only;
 *     never open/fetch_headers/read, never a reused handle;
 *   - the body is collected in HTTP_EVENT_ON_DATA and bounded by max_body:
 *     beyond it `truncated` is set and the rest is discarded -- never parse
 *     a truncated body, treat it as "the reply was too big";
 *   - `Authorization: Bearer <token>` from espos_sk_get_token(), snapshotted
 *     per call; the token is never put in a query string. No token = no
 *     header, which is right for a server running without security;
 *   - scheme from the selected server's tls flag (http/ws, or https/wss with
 *     the same certificate-bundle rules as the delta stream, docs/signalk.md);
 *   - 401/403 while a token was sent -> espos_sk_report_unauthorized(), so
 *     the token machine re-verifies and re-requests, unless opted out;
 *   - ESP_ERR_INVALID_STATE when no server is selected;
 *   - requests in flight are bounded by Kconfig ESPOS_SK_HTTP_MAX_CONCURRENT
 *     (default 2): each costs a socket and, over TLS, ~20 KB of RAM. A caller
 *     over the limit waits up to its own timeout_ms for a slot.
 *
 * Threading: BLOCKING, for up to timeout_ms waiting for a slot plus
 * timeout_ms on the wire, on the caller's task, with ~2 KiB of its stack.
 * Call from an application task. Never from the SK stream task (the
 * espos_sk_subscribe / espos_sk_put callbacks), from an ESPOS_EVENT handler,
 * from a Bluetooth stack callback, or from an HTTP URI handler (it stalls
 * the web UI). All functions are thread-safe.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Fits any "scheme://host:port/path" espos_sk_url() builds: path up to ~300. */
#define ESPOS_SK_URL_MAX 384

typedef struct {
    int status;      /* HTTP status; 0 when no reply arrived (see err) */
    esp_err_t err;   /* the function's return value, kept with the reply */
    char *body;      /* malloc'ed, NUL-terminated, "" for an empty reply; NULL when err != ESP_OK */
    size_t len;      /* bytes in body (excluding the NUL) */
    bool truncated;  /* body hit max_body: len == max_body, the rest was discarded */
} espos_sk_http_resp_t;

/** Zeroed (or NULL) = 6000 ms, 16 KiB, Bearer on, 401/403 reported, Accept: application/json. */
typedef struct {
    uint32_t timeout_ms;          /* 0 = 6000 */
    size_t max_body;              /* 0 = 16384 */
    bool no_auth;                 /* omit the Authorization header even when a token exists */
    bool no_report_unauthorized;  /* a 401/403 does not touch the token machine */
    const char *accept;           /* NULL = "application/json"; "" = no Accept header */
} espos_sk_http_opts_t;

/**
 * One request to the selected server. `path` is absolute ("/signalk/v1/..."),
 * `json` (PUT/POST) is sent as application/json, NULL = no body.
 *
 * Returns ESP_OK whenever a reply arrived -- check r->status, a 404 or 500 is
 * a successful call. Otherwise ESP_ERR_INVALID_STATE (no server),
 * ESP_ERR_TIMEOUT (no slot within timeout_ms), ESP_ERR_NO_MEM, or the
 * transport error esp_http_client reported (unreachable, TLS refused, timed
 * out on the wire). Always espos_sk_http_resp_free(r) afterwards.
 */
esp_err_t espos_sk_http_get(const char *path, const espos_sk_http_opts_t *o, espos_sk_http_resp_t *r);
esp_err_t espos_sk_http_put(const char *path, const char *json, const espos_sk_http_opts_t *o, espos_sk_http_resp_t *r);
esp_err_t espos_sk_http_post(const char *path, const char *json, const espos_sk_http_opts_t *o, espos_sk_http_resp_t *r);
esp_err_t espos_sk_http_delete(const char *path, const espos_sk_http_opts_t *o, espos_sk_http_resp_t *r);
/** Frees the body and zeroes the struct; safe on a zeroed or failed reply. */
void espos_sk_http_resp_free(espos_sk_http_resp_t *r);

/**
 * "http(s)://host:port/path" for the selected server (a missing leading
 * slash is added; the path is otherwise used verbatim). ESP_ERR_NOT_FOUND
 * without a server, ESP_ERR_INVALID_SIZE when it does not fit in n.
 */
esp_err_t espos_sk_url(const char *path, char *out, size_t n);
/** "ws(s)://host:port/path", same rules. */
esp_err_t espos_sk_ws_url(const char *path, char *out, size_t n);

/**
 * Current value of a vessels.self path ("navigation.speedOverGround" ->
 * GET /signalk/v1/api/vessels/self/navigation/speedOverGround), handed back
 * as the JSON text of the node's "value" member (malloc'ed; a number,
 * string, object or "null" -- whatever the server holds). What a display
 * needs for a path whose value rarely changes: the stream only carries
 * changes, the REST node has the reading now.
 *
 * ESP_ERR_NOT_FOUND: 404, or the node has no "value" member;
 * ESP_ERR_NOT_ALLOWED: 401/403 (reported to the token machine);
 * ESP_ERR_INVALID_RESPONSE: not JSON; ESP_ERR_INVALID_SIZE: over 16 KiB;
 * ESP_FAIL: any other status; transport errors as above.
 */
esp_err_t espos_sk_get_value(const char *sk_path, char **out_json);
/**
 * The path's meta object (GET .../meta) as JSON text (malloc'ed).
 * ESP_ERR_NOT_FOUND when the server has none: a 404, or a 200 whose body
 * is not a JSON object (seen for paths a server exposes without meta).
 * Other errors as espos_sk_get_value().
 */
esp_err_t espos_sk_get_meta(const char *sk_path, char **out_json);

#ifdef __cplusplus
}
#endif

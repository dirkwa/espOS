/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * SignalK stream frame parser — pure C over cJSON, host-tested.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One value or meta item taken out of a delta message. Strings point
 * into parser-owned memory and are valid only during the callback. */
typedef struct {
    const char *context;      /* "vessels.self" / URN, or NULL */
    const char *path;
    const char *value_json;   /* the value as JSON text, NULL for a meta item */
    const char *meta_json;    /* the meta object as JSON text, NULL for a value item */
    const char *timestamp;    /* ISO-8601 or NULL */
    const char *source;       /* $source label or NULL */
    /* Set only for an item from a PUT request frame: the id the server wants
     * quoted back in the response. NULL for delta values and meta, which is
     * what tells a dispatcher that no response is owed. */
    const char *request_id;
} espos_sk_update_t;

typedef enum {
    ESPOS_SK_FRAME_UNKNOWN = 0,
    ESPOS_SK_FRAME_HELLO,     /* {"name","version","self","roles"} */
    ESPOS_SK_FRAME_DELTA,     /* {"context","updates":[…]} */
    ESPOS_SK_FRAME_RESPONSE,  /* {"requestId","state","statusCode",…} */
    ESPOS_SK_FRAME_ERROR,     /* {"errorMessage"} */
    /* An inbound PUT: the server asking this device to change something.
     * {"requestId","context","put":[{"path","value"}]} -- note that `put` is
     * an ARRAY here, which is what signalk-server's ws interface writes
     * (src/interfaces/ws.ts, handlePut). The single-object form
     * {"put":{"path","value"}} is what a CLIENT sends outbound; both are
     * accepted so the parser does not depend on which side wrote the frame. */
    ESPOS_SK_FRAME_PUT,
} espos_sk_frame_kind_t;

typedef struct {
    espos_sk_frame_kind_t kind;
    /* HELLO */
    const char *self;
    /* RESPONSE */
    const char *request_id;
    const char *state;        /* COMPLETED | PENDING | FAILED */
    int status_code;
    const char *message;
    /* ERROR */
    const char *error;
    void *_priv;              /* parser memory; freed by espos_sk_frame_free */
} espos_sk_frame_t;

/** Return false to stop iterating. */
typedef bool (*espos_sk_update_cb_t)(const espos_sk_update_t *u, void *arg);

/**
 * Parse one text frame. Fills *info (kind + fields, valid until
 * espos_sk_frame_free) and calls cb for every item it carries:
 *
 *   DELTA — every value and every meta item, in order, request_id NULL.
 *   PUT   — every {"path","value"} of the request, each with request_id
 *           set to the frame's requestId, so a handler knows what to
 *           answer. value_json is the JSON text of the new value.
 *
 * Returns the number of items delivered (0 for other frames or malformed
 * input; kind says which).
 */
size_t espos_sk_frame_parse(const char *json, size_t len, espos_sk_frame_t *info, espos_sk_update_cb_t cb, void *arg);
void espos_sk_frame_free(espos_sk_frame_t *info);

/**
 * Build the response frame for an inbound PUT: the JSON a device sends back
 * so signalk-server can resolve the request.
 *
 * `state` must be "COMPLETED" or "PENDING". signalk-server accepts nothing
 * else on this path -- its isWsRequestReply() (src/interfaces/ws.ts) tests
 * for exactly those two (or null) and silently ignores any other reply,
 * which reads to the client as a request that timed out 60 s later. A
 * failure is COMPLETED with a 4xx/5xx statusCode, NOT a "FAILED" state.
 *
 * Returns a malloc'ed string the caller frees, or NULL.
 */
char *espos_sk_put_response_frame(const char *request_id, const char *state, int status_code, const char *message);

/** Path pattern match: exact, or "prefix.*" / "prefix*" / "*". */
bool espos_sk_path_matches(const char *pattern, const char *path);

#ifdef __cplusplus
}
#endif

/*
 * SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
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
} espos_sk_update_t;

typedef enum {
    ESPOS_SK_FRAME_UNKNOWN = 0,
    ESPOS_SK_FRAME_HELLO,     /* {"name","version","self","roles"} */
    ESPOS_SK_FRAME_DELTA,     /* {"context","updates":[…]} */
    ESPOS_SK_FRAME_RESPONSE,  /* {"requestId","state","statusCode",…} */
    ESPOS_SK_FRAME_ERROR,     /* {"errorMessage"} */
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
 * espos_sk_frame_free) and, for a delta, calls cb for every value and
 * every meta item in order. Returns the number of items delivered (0 for
 * non-delta frames or malformed input; kind says which).
 */
size_t espos_sk_frame_parse(const char *json, size_t len, espos_sk_frame_t *info, espos_sk_update_cb_t cb, void *arg);
void espos_sk_frame_free(espos_sk_frame_t *info);

/** Path pattern match: exact, or "prefix.*" / "prefix*" / "*". */
bool espos_sk_path_matches(const char *pattern, const char *path);

#ifdef __cplusplus
}
#endif

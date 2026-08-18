/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "espos_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A decoded value in RAM, produced by the JSON layer or the typed setters. */
typedef struct {
    espos_cfg_type_t type;
    union {
        bool b;
        int32_t i;
        float f;
        const char *s;      /* NUL-terminated */
        struct { const uint8_t *p; size_t len; } blob;
    } v;
} espos_cfg_value_t;

/* Validate `val` against `key`. On failure writes a short reason into msg. */
bool espos_config_validate(const espos_cfg_key_t *key, const espos_cfg_value_t *val,
                           char *msg, size_t msg_size);

/* Batch write plan used by JSON import (all-or-nothing after validation). */
typedef struct {
    const espos_cfg_ns_t *ns;
    const espos_cfg_key_t *key;
    bool reset;               /* true → erase key (null in JSON) */
    espos_cfg_value_t val;    /* used if !reset; strings/blobs point into caller memory */
} espos_config_plan_entry_t;

/* Apply a validated plan under one lock. Fills changed[] with entries whose
 * effective value changed (indexes into plan) and returns their count. */
esp_err_t espos_config_apply_plan(const espos_config_plan_entry_t *plan, size_t n,
                                  size_t *changed_idx, size_t *changed_count,
                                  bool *restart_required);

/* Store lock, for callers that read several keys as one snapshot. */
void espos_config_lock(void);
void espos_config_unlock(void);
bool espos_config_is_inited(void);

/* Read the effective value into caller buffers (used by export). Lock held.
 * For STRING: sbuf must hold key->max_len + 1 bytes. For BLOB: bbuf must hold
 * key->max_len bytes, *blen receives the length. Never fails for declared keys. */
esp_err_t espos_config_read_effective(const espos_cfg_ns_t *ns, const espos_cfg_key_t *key,
                                      espos_cfg_value_t *out, char *sbuf, uint8_t *bbuf,
                                      size_t *blen, bool *is_set);

/* base64 helpers (RFC 4648, no line breaks). */
size_t espos_b64_encoded_len(size_t n);            /* incl. NUL */
size_t espos_b64_encode(const uint8_t *in, size_t n, char *out, size_t out_size); /* returns chars written excl. NUL, 0 on overflow */
/* Decodes into out; returns ESP_OK and *out_len, ESP_ERR_INVALID_ARG on bad input,
 * ESP_ERR_INVALID_SIZE if out is too small. Accepts optional '=' padding. */
esp_err_t espos_b64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_size, size_t *out_len);

#ifdef __cplusplus
}
#endif

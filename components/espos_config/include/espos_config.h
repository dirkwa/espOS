/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_config — NVS-backed, schema-described configuration store.
 *
 * Every value lives in an NVS namespace under a short key. What namespaces
 * and keys exist, their types, defaults and limits, is declared once in a
 * config descriptor (JSON) per component and turned into the tables in
 * espos_config_desc.h at build time. Use the generated espos_cfg_keys.h
 * constants rather than string literals.
 *
 * Reads never fail on missing or corrupt values: they fall back to the
 * compiled-in default. Writes are validated against the descriptor first and
 * committed per key; a JSON import validates the whole document before it
 * writes anything.
 *
 * Thread-safe: all calls take an internal mutex. Change callbacks run on the
 * caller's task with the mutex NOT held.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "espos_config_desc.h"
#include "espos_config_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sentinel returned in place of secret values on export; ignored on import. */
#define ESPOS_CONFIG_SECRET_SENTINEL "********"

/* ---------------------------------------------------------------- lifecycle */

/**
 * Initialise the store. Opens every declared namespace, then runs schema
 * migrations (see espos_config_register_migration) and stamps the current
 * version. Must be called before any other function.
 *
 * @param backend  NULL for the NVS backend, or an injected backend (tests).
 * @param backend_ctx  passed through to the backend.
 */
esp_err_t espos_config_init(const espos_config_backend_t *backend, void *backend_ctx);

/** Close namespaces and release resources (tests). */
void espos_config_deinit(void);

/** true if the backend had to wipe storage during init (values are defaults). */
bool espos_config_storage_was_reset(void);

/**
 * Erase the whole store. Does NOT reboot; the caller should call esp_restart()
 * promptly since the in-RAM state no longer matches storage.
 */
esp_err_t espos_config_factory_reset(void);

/* --------------------------------------------------------------- migrations */

/**
 * Migration step callback: move namespace `ns` from `from_version` to
 * `from_version + 1` using the raw accessors below. Return ESP_OK on success;
 * anything else aborts the chain (the stored version stays at from_version and
 * a warning is logged; reads still work via defaults on type mismatch).
 * Only the espos_config_migrate_* accessors may be used inside; the public
 * getters/setters return ESP_ERR_INVALID_STATE (the store is not up yet).
 */
typedef struct espos_config_migrate_ctx espos_config_migrate_ctx_t;
typedef esp_err_t (*espos_config_migrate_fn_t)(espos_config_migrate_ctx_t *ctx, void *arg);

/**
 * Register a migration for `ns` from `from_version` → `from_version + 1`.
 * Call before espos_config_init(). Steps without a registered function are
 * treated as additive (no-op) — new keys simply read their defaults.
 * @return ESP_ERR_NO_MEM if the table is full, ESP_ERR_INVALID_ARG if ns is
 *         unknown or from_version >= current version.
 */
esp_err_t espos_config_register_migration(const char *ns, uint16_t from_version,
                                          espos_config_migrate_fn_t fn, void *arg);

/* Raw accessors usable only inside a migration callback (no validation,
 * arbitrary types — a migration may read a key in its OLD type). */
esp_err_t espos_config_migrate_get(espos_config_migrate_ctx_t *ctx, const char *key,
                                   espos_cfg_type_t type, void *buf, size_t *len);
esp_err_t espos_config_migrate_set(espos_config_migrate_ctx_t *ctx, const char *key,
                                   espos_cfg_type_t type, const void *buf, size_t len);
esp_err_t espos_config_migrate_erase(espos_config_migrate_ctx_t *ctx, const char *key);
uint16_t espos_config_migrate_from_version(const espos_config_migrate_ctx_t *ctx);

/** Stored schema version of a namespace (0 = never stamped). */
esp_err_t espos_config_get_version(const char *ns, uint16_t *stored, uint16_t *current);

/* ---------------------------------------------------------------- accessors */

/* Reads: return ESP_OK with the stored value or the compiled-in default.
 * ESP_ERR_NOT_FOUND only if ns/key are not declared. */
esp_err_t espos_config_get_bool(const char *ns, const char *key, bool *out);
esp_err_t espos_config_get_i32(const char *ns, const char *key, int32_t *out);
esp_err_t espos_config_get_float(const char *ns, const char *key, float *out);
/** Copies the string into buf (always NUL-terminated if buf_size > 0). *out_len,
 * if non-NULL, receives the full length (excluding NUL). If the value does not
 * fit, returns ESP_ERR_INVALID_SIZE and buf holds a truncated copy. */
esp_err_t espos_config_get_str(const char *ns, const char *key, char *buf, size_t buf_size,
                               size_t *out_len);
/** Blob: *len is buf size in, bytes out. buf==NULL → size query (ESP_OK, *len set).
 * An absent blob is ESP_OK with *len = 0. */
esp_err_t espos_config_get_blob(const char *ns, const char *key, void *buf, size_t *len);

/* Writes: validated against the descriptor. ESP_ERR_INVALID_ARG on type/range/
 * length/enum violation, ESP_ERR_NOT_FOUND on undeclared key. Change callbacks
 * fire only if the effective value actually changed. */
esp_err_t espos_config_set_bool(const char *ns, const char *key, bool v);
esp_err_t espos_config_set_i32(const char *ns, const char *key, int32_t v);
esp_err_t espos_config_set_float(const char *ns, const char *key, float v);
esp_err_t espos_config_set_str(const char *ns, const char *key, const char *v);
esp_err_t espos_config_set_blob(const char *ns, const char *key, const void *buf, size_t len);

/** Erase a key so it reads its default again. */
esp_err_t espos_config_reset_key(const char *ns, const char *key);
/** Erase every key of a namespace (keeps the version stamp). */
esp_err_t espos_config_reset_ns(const char *ns);
/** true if the key currently has a stored (non-default) value. */
bool espos_config_is_set(const char *ns, const char *key);

/* --------------------------------------------------------------- descriptor */

const espos_cfg_ns_t *espos_config_find_ns(const char *ns);
const espos_cfg_key_t *espos_config_find_key(const espos_cfg_ns_t *ns, const char *key);

/* -------------------------------------------------------------- change feed */

typedef void (*espos_config_change_cb_t)(const char *ns, const char *key, void *arg);
/** Called after each committed change (per key), on the writer's task, with
 * the store lock released (a callback may read or write config). Small
 * fixed table. A callback that is already in flight on another task may
 * still complete after espos_config_unsubscribe() returns. */
esp_err_t espos_config_subscribe(espos_config_change_cb_t cb, void *arg);
esp_err_t espos_config_unsubscribe(espos_config_change_cb_t cb, void *arg);

/* ------------------------------------------------------------------- JSON */

/**
 * Serialise the effective configuration:  {"ns": {"key": value, ...}, ...}
 * Blobs are base64 strings. Secret values become ESPOS_CONFIG_SECRET_SENTINEL
 * unless include_secrets. `only_ns` restricts to one namespace (NULL = all).
 * The returned string is malloc'ed; caller frees.
 */
esp_err_t espos_config_export_json(const char *only_ns, bool include_secrets, char **out_json);

typedef struct {
    size_t changed;          /* keys whose effective value changed */
    bool restart_required;   /* at least one changed key is flagged restart_required */
    /* On validation failure: the first offending "ns.key" and a reason. */
    char error_path[40];
    char error_msg[96];
} espos_config_import_result_t;

/**
 * Apply a JSON document of the same shape as the export. Semantics:
 *   - namespaces/keys not present are left untouched
 *   - a key set to null is reset to its default
 *   - a secret whose value is the sentinel is left untouched
 *   - the whole document is validated first; on any error nothing is written
 *     and ESP_ERR_INVALID_ARG is returned with result->error_* filled in
 *   - unknown namespaces/keys are errors (unless `ignore_unknown`)
 * `out_report_json` (optional) receives a malloc'ed JSON report of the form
 *   {"changed":["ns.key",...],"restart_required":false}
 * on success, or {"error":"validation","path":"ns.key","message":"..."} on
 * ESP_ERR_INVALID_ARG. Caller frees.
 */
esp_err_t espos_config_import_json(const char *json, size_t json_len, bool ignore_unknown,
                                   espos_config_import_result_t *result, char **out_report_json);

#ifdef __cplusplus
}
#endif

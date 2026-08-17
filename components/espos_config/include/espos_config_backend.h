/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Storage backend interface for espos_config.
 *
 * The config core is pure C over this interface so it can be unit-tested on
 * the host with an in-memory backend, and so the NVS specifics live in one
 * file. Error codes are the generic esp_err_t values below — the NVS backend
 * translates ESP_ERR_NVS_* into them.
 *
 *   ESP_OK
 *   ESP_ERR_NOT_FOUND      key (or namespace) does not exist
 *   ESP_ERR_INVALID_STATE  key exists with a different type
 *   ESP_ERR_INVALID_SIZE   value does not fit the caller's buffer / exceeds limits
 *   ESP_ERR_NO_MEM         storage full
 *   ESP_FAIL               anything else
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "espos_config_desc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *espos_config_bh_t; /* backend handle for one open namespace */

typedef struct espos_config_backend {
    /* Bring storage up. Set *storage_reset=true if the backend had to wipe
     * corrupt/incompatible storage to do so (all values fall back to defaults). */
    esp_err_t (*init)(void *ctx, bool *storage_reset);
    /* Open a namespace read/write, creating it if absent. */
    esp_err_t (*open)(void *ctx, const char *ns, espos_config_bh_t *out);
    void (*close)(void *ctx, espos_config_bh_t h);

    /* Typed get. For STRING/BLOB: *len is the buffer size in, bytes out
     * (string length includes the NUL). buf==NULL queries the size only. */
    esp_err_t (*get)(void *ctx, espos_config_bh_t h, const char *key, espos_cfg_type_t type,
                     void *buf, size_t *len);
    /* Typed set. For STRING len is ignored (NUL-terminated); BLOB uses len. */
    esp_err_t (*set)(void *ctx, espos_config_bh_t h, const char *key, espos_cfg_type_t type,
                     const void *buf, size_t len);
    esp_err_t (*erase_key)(void *ctx, espos_config_bh_t h, const char *key);
    esp_err_t (*erase_all)(void *ctx, espos_config_bh_t h);
    esp_err_t (*commit)(void *ctx, espos_config_bh_t h);

    /* Wipe the entire store (factory reset). Handles are invalid afterwards. */
    esp_err_t (*erase_storage)(void *ctx);
} espos_config_backend_t;

/* NVS-backed backend (the default on device). ctx: partition label (const char*)
 * or NULL for CONFIG_ESPOS_CONFIG_NVS_PARTITION. */
const espos_config_backend_t *espos_config_backend_nvs(void);

/* In-memory backend for host tests / diagnostics. ctx: espos_config_mem_t*. */
typedef struct espos_config_mem espos_config_mem_t;
const espos_config_backend_t *espos_config_backend_mem(void);
espos_config_mem_t *espos_config_mem_create(void);
void espos_config_mem_destroy(espos_config_mem_t *m);
/* Fault injection: every set/erase/commit fails with this code while != ESP_OK. */
void espos_config_mem_set_write_fault(espos_config_mem_t *m, esp_err_t err);
/* Introspection for tests. */
size_t espos_config_mem_key_count(espos_config_mem_t *m, const char *ns);
bool espos_config_mem_has_key(espos_config_mem_t *m, const char *ns, const char *key);
/* Plant a raw value of an arbitrary type (simulates an older firmware's layout). */
esp_err_t espos_config_mem_plant(espos_config_mem_t *m, const char *ns, const char *key,
                                 espos_cfg_type_t type, const void *buf, size_t len);

#ifdef __cplusplus
}
#endif

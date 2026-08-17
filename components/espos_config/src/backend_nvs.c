/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * NVS backend for espos_config. Type mapping:
 *   bool   → u8        int → i32        float → u32 (IEEE-754 bits)
 *   string → nvs str   blob → nvs blob
 *
 * When CONFIG_NVS_ENCRYPTION is enabled the partition is transparently
 * encrypted by nvs_flash_init_partition() (keys in the nvs_keys partition).
 */
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "espos_config_backend.h"

static const char *TAG = "espos_config";

#ifndef CONFIG_ESPOS_CONFIG_NVS_PARTITION
#define CONFIG_ESPOS_CONFIG_NVS_PARTITION "nvs"
#endif

static const char *part_label(void *ctx)
{
    return ctx ? (const char *)ctx : CONFIG_ESPOS_CONFIG_NVS_PARTITION;
}

static esp_err_t map_err(esp_err_t e)
{
    switch (e) {
    case ESP_OK:
        return ESP_OK;
    case ESP_ERR_NVS_NOT_FOUND:
        return ESP_ERR_NOT_FOUND;
    case ESP_ERR_NVS_TYPE_MISMATCH:
        return ESP_ERR_INVALID_STATE;
    case ESP_ERR_NVS_INVALID_LENGTH:
    case ESP_ERR_NVS_VALUE_TOO_LONG:
        return ESP_ERR_INVALID_SIZE;
    case ESP_ERR_NVS_NOT_ENOUGH_SPACE:
        return ESP_ERR_NO_MEM;
    default:
        return ESP_FAIL;
    }
}

static esp_err_t be_init(void *ctx, bool *storage_reset)
{
    const char *label = part_label(ctx);
    *storage_reset = false;
    esp_err_t err = nvs_flash_init_partition(label);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND
#ifdef ESP_ERR_NVS_CORRUPT_KEY_PART
        || err == ESP_ERR_NVS_CORRUPT_KEY_PART
#endif
       ) {
        ESP_LOGW(TAG, "NVS partition '%s' unusable (%s); erasing", label, esp_err_to_name(err));
        esp_err_t e2 = nvs_flash_erase_partition(label);
        if (e2 != ESP_OK) {
            ESP_LOGE(TAG, "erase failed: %s", esp_err_to_name(e2));
            return e2;
        }
        err = nvs_flash_init_partition(label);
        if (err == ESP_OK) {
            *storage_reset = true;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init_partition('%s') failed: %s", label, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t be_open(void *ctx, const char *ns, espos_config_bh_t *out)
{
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open_from_partition(part_label(ctx), ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return map_err(err);
    }
    *out = (espos_config_bh_t)(uintptr_t)h;
    return ESP_OK;
}

static void be_close(void *ctx, espos_config_bh_t h)
{
    (void)ctx;
    nvs_close((nvs_handle_t)(uintptr_t)h);
}

static esp_err_t be_get(void *ctx, espos_config_bh_t bh, const char *key, espos_cfg_type_t type,
                        void *buf, size_t *len)
{
    (void)ctx;
    nvs_handle_t h = (nvs_handle_t)(uintptr_t)bh;
    esp_err_t err;
    switch (type) {
    case ESPOS_CFG_TYPE_BOOL: {
        uint8_t u;
        if (*len < 1) {
            return ESP_ERR_INVALID_SIZE;
        }
        err = nvs_get_u8(h, key, &u);
        if (err == ESP_OK) {
            *(uint8_t *)buf = u;
            *len = 1;
        }
        return map_err(err);
    }
    case ESPOS_CFG_TYPE_INT: {
        int32_t i;
        if (*len < 4) {
            return ESP_ERR_INVALID_SIZE;
        }
        err = nvs_get_i32(h, key, &i);
        if (err == ESP_OK) {
            memcpy(buf, &i, 4);
            *len = 4;
        }
        return map_err(err);
    }
    case ESPOS_CFG_TYPE_FLOAT: {
        uint32_t u;
        if (*len < 4) {
            return ESP_ERR_INVALID_SIZE;
        }
        err = nvs_get_u32(h, key, &u);
        if (err == ESP_OK) {
            memcpy(buf, &u, 4);
            *len = 4;
        }
        return map_err(err);
    }
    case ESPOS_CFG_TYPE_STRING:
        return map_err(nvs_get_str(h, key, buf, len));
    case ESPOS_CFG_TYPE_BLOB:
        return map_err(nvs_get_blob(h, key, buf, len));
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t raw_set(nvs_handle_t h, const char *key, espos_cfg_type_t type, const void *buf, size_t len)
{
    switch (type) {
    case ESPOS_CFG_TYPE_BOOL:
        return nvs_set_u8(h, key, *(const uint8_t *)buf ? 1 : 0);
    case ESPOS_CFG_TYPE_INT: {
        int32_t i;
        memcpy(&i, buf, 4);
        return nvs_set_i32(h, key, i);
    }
    case ESPOS_CFG_TYPE_FLOAT: {
        uint32_t u;
        memcpy(&u, buf, 4);
        return nvs_set_u32(h, key, u);
    }
    case ESPOS_CFG_TYPE_STRING:
        return nvs_set_str(h, key, (const char *)buf);
    case ESPOS_CFG_TYPE_BLOB:
        return nvs_set_blob(h, key, buf, len);
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t be_set(void *ctx, espos_config_bh_t bh, const char *key, espos_cfg_type_t type,
                        const void *buf, size_t len)
{
    (void)ctx;
    nvs_handle_t h = (nvs_handle_t)(uintptr_t)bh;
    esp_err_t err = raw_set(h, key, type, buf, len);
    if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
        /* Key exists with another type (e.g. a migration changing a key's
         * type): NVS refuses in-place; erase and write fresh. */
        err = nvs_erase_key(h, key);
        if (err == ESP_OK) {
            err = raw_set(h, key, type, buf, len);
        }
    }
    return map_err(err);
}

static esp_err_t be_erase_key(void *ctx, espos_config_bh_t bh, const char *key)
{
    (void)ctx;
    return map_err(nvs_erase_key((nvs_handle_t)(uintptr_t)bh, key));
}

static esp_err_t be_erase_all(void *ctx, espos_config_bh_t bh)
{
    (void)ctx;
    return map_err(nvs_erase_all((nvs_handle_t)(uintptr_t)bh));
}

static esp_err_t be_commit(void *ctx, espos_config_bh_t bh)
{
    (void)ctx;
    return map_err(nvs_commit((nvs_handle_t)(uintptr_t)bh));
}

static esp_err_t be_erase_storage(void *ctx)
{
    const char *label = part_label(ctx);
    esp_err_t err = nvs_flash_deinit_partition(label);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_INITIALIZED) {
        ESP_LOGW(TAG, "nvs_flash_deinit_partition('%s'): %s", label, esp_err_to_name(err));
    }
    err = nvs_flash_erase_partition(label);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_erase_partition('%s') failed: %s", label, esp_err_to_name(err));
    }
    return err;
}

static const espos_config_backend_t k_nvs_backend = {
    .init = be_init,
    .open = be_open,
    .close = be_close,
    .get = be_get,
    .set = be_set,
    .erase_key = be_erase_key,
    .erase_all = be_erase_all,
    .commit = be_commit,
    .erase_storage = be_erase_storage,
};

const espos_config_backend_t *espos_config_backend_nvs(void)
{
    return &k_nvs_backend;
}

/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * custom_settings — an application's own settings. They are declared once,
 * in config/app.json; from that one file come the key constants this code
 * uses (espos_cfg_keys.h), the validation the store applies, the REST
 * document under /api/v1/config and the form on the web UI's Config page.
 *
 * The example is a tank level sender with a simulated sensor: which tank,
 * how often, on or off, and the tank's name are the four settings.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "espos.h"
#include "espos_cfg_keys.h" /* generated from every registered descriptor */
#include "espos_config.h"
#include "espos_sk.h"

static const char *TAG = "settings";
static TaskHandle_t s_loop;

/* Runs on the writer's task — the HTTP handler behind PUT /api/v1/config —
 * once per changed key, with the store unlocked. It does not copy the values
 * itself: a string cannot be swapped under a reader atomically, so it wakes
 * the loop, the one task that reads them, and the loop reloads everything.
 * Other namespaces (wifi, sk) change too; those wake nobody here. */
static void on_config_change(const char *ns, const char *key, void *arg)
{
    (void)key;
    (void)arg;
    if (s_loop && strcmp(ns, ESPOS_CFG_NS_APP) == 0) xTaskNotifyGive(s_loop);
}

/* Descriptor version 1 stored the tank's name under "name"; version 2 calls
 * it "label", because "name" read like the Signal K path leaf it is published
 * to. Without this step every device already in the field would come up with
 * the default after the update: a rename is a data loss unless someone moves
 * the value. Steps run inside espos_config_init(), before anything reads the
 * store, and use the raw accessors because the old key is no longer declared. */
static esp_err_t migrate_1_to_2(espos_config_migrate_ctx_t *ctx, void *arg)
{
    (void)arg;
    char name[33];
    size_t n = sizeof(name);
    if (espos_config_migrate_get(ctx, "name", ESPOS_CFG_TYPE_STRING, name, &n) == ESP_OK) {
        espos_config_migrate_set(ctx, ESPOS_CFG_APP_LABEL, ESPOS_CFG_TYPE_STRING, name, n);
        espos_config_migrate_erase(ctx, "name");
    }
    return ESP_OK; /* a device that never had "name" reads label's default */
}

void app_main(void)
{
    /* Before espos_start(): the step must be known when the store comes up. */
    ESP_ERROR_CHECK(espos_config_register_migration(ESPOS_CFG_NS_APP, 1, migrate_1_to_2, NULL));
    ESP_ERROR_CHECK(espos_start(NULL));
    s_loop = xTaskGetCurrentTaskHandle();
    ESP_ERROR_CHECK(espos_config_subscribe(on_config_change, NULL));

    bool enabled = true;
    int32_t interval_ms = 2000;
    char label[33], tank[16], level_path[48], name_path[48];
    bool reload = true;
    for (float t = 0;; t += 0.05f) {
        if (reload) {
            /* Getters never fail for declared keys: a missing, corrupt or
             * out-of-range stored value reads as the compiled-in default. */
            espos_config_get_bool(ESPOS_CFG_NS_APP, ESPOS_CFG_APP_ENABLED, &enabled);
            espos_config_get_i32(ESPOS_CFG_NS_APP, ESPOS_CFG_APP_INTERVAL_MS, &interval_ms);
            espos_config_get_str(ESPOS_CFG_NS_APP, ESPOS_CFG_APP_LABEL, label, sizeof(label), NULL);
            espos_config_get_str(ESPOS_CFG_NS_APP, ESPOS_CFG_APP_TANK, tank, sizeof(tank), NULL);
            snprintf(level_path, sizeof(level_path), "tanks.%s.0.currentLevel", tank);
            snprintf(name_path, sizeof(name_path), "tanks.%s.0.name", tank);
            ESP_LOGI(TAG, "%s: %s, every %ld ms → %s", label, enabled ? "on" : "off", (long)interval_ms, level_path);
            /* The name changes only with the settings: out once here, not on every tick. */
            if (enabled) espos_sk_publish_string(name_path, label);
            reload = false;
        }
        if (enabled) {
            /* Simulated sensor: a slow swing around half full. currentLevel is
             * a ratio 0..1, like every Signal K quantity in its SI form. */
            espos_sk_publish_number(level_path, 0.5 + 0.3 * sinf(t));
        }
        /* Sleep one interval — or less: a saved change wakes the loop at
         * once, and the non-zero return says to reload. */
        reload = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(interval_ms)) > 0;
    }
}

/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espOS reference app: espos_start() brings the runtime up; the rest is the app.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "espos.h"
#include "espos_cfg_keys.h"
#include "espos_config.h"
#include "espos_sk.h"
#include "espos_wifi.h"
static const char *TAG = "app";
/* Word-sized: the writer's task stores, the loop reads, no lock needed. */
static bool s_enabled = true;
static int32_t s_interval_ms = 1000;
static float s_scale = 1.0f;

/* Reads the app namespace. Also the change callback (writer's task, an HTTP
 * handler usually), so a PUT takes effect without the loop polling every tick. */
static void load_cfg(const char *ns, const char *key, void *arg)
{
    (void)ns;
    (void)key;
    (void)arg;
    espos_config_get_bool(ESPOS_CFG_NS_APP, ESPOS_CFG_APP_ENABLED, &s_enabled);
    espos_config_get_i32(ESPOS_CFG_NS_APP, ESPOS_CFG_APP_INTERVAL_MS, &s_interval_ms);
    espos_config_get_float(ESPOS_CFG_NS_APP, ESPOS_CFG_APP_SCALE, &s_scale);
}

/* Stream task: log and return. A real app copies into a queue. */
static void on_watched(const espos_sk_update_t *u, void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "SK %s value=%.120s meta=%.120s", u->path, u->value_json ? u->value_json : "-", u->meta_json ? u->meta_json : "-");
}

void app_main(void)
{
#ifdef ESPOS_BROKEN_BUILD
    ESP_ERROR_CHECK(ESP_FAIL); /* rollback acceptance test (docs/ota.md): die before the image confirms itself */
#endif
    ESP_ERROR_CHECK(espos_start(NULL));
    load_cfg(NULL, NULL, NULL);
    ESP_ERROR_CHECK(espos_config_subscribe(load_cfg, NULL));
    char hb_path[64], watch[96] = "";
    snprintf(hb_path, sizeof(hb_path), "espos.%s.heartbeat", espos_wifi_short_id());
    espos_sk_declare_meta(hb_path, "{\"description\":\"Example app heartbeat counter\"}", 1000); /* non-standard path: the server cannot know it */
    espos_config_get_str(ESPOS_CFG_NS_APP, ESPOS_CFG_APP_WATCH_PATH, watch, sizeof(watch), NULL);
    int sub = watch[0] ? espos_sk_subscribe(watch, 1000, on_watched, NULL) : 0;
    ESP_LOGI(TAG, "watch_path %s (subscription %d)", watch[0] ? watch : "off", sub);
    for (uint32_t heartbeat = 0;; vTaskDelay(pdMS_TO_TICKS(s_interval_ms))) {
        if (s_enabled) {
            espos_sk_publish_number(hb_path, (double)(heartbeat++) * s_scale);
        }
    }
}

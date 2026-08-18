/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * espOS example application: bring up the config store and the HTTP server,
 * then log a heartbeat at the configured interval. This is the "app on top of
 * espOS" — everything reusable lives in components/.
 */
#include <inttypes.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "espos_cfg_keys.h"
#include "espos_config.h"
#include "espos_log.h"
#include "espos_httpd.h"
#include "espos_wifi.h"
#include "espos_sk.h"
#include "espos_ota.h"

static const char *TAG = "app";

static void on_config_change(const char *ns, const char *key, void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "config changed: %s.%s", ns, key);
}

void app_main(void)
{
#ifdef ESPOS_BROKEN_BUILD
    /* Test hook: an image that dies before it can confirm itself → rollback. */
    ESP_LOGE("app", "ESPOS_BROKEN_BUILD: aborting on purpose");
    abort();
#endif
    ESP_ERROR_CHECK(espos_log_init());     /* first: keep the boot log for /api/v1/logs */
    ESP_ERROR_CHECK(espos_config_init(NULL, NULL));
    ESP_ERROR_CHECK(espos_config_subscribe(on_config_change, NULL));

    ESP_ERROR_CHECK(espos_httpd_start());
    ESP_ERROR_CHECK(espos_wifi_start());
    ESP_ERROR_CHECK(espos_sk_start());
    ESP_ERROR_CHECK(espos_ota_start());

    /* Example app payload: a heartbeat counter under a custom path (with
     * meta, because the server cannot know a non-standard path) and the
     * configured scale — standard-looking but still ours. */
    char hb_path[64];
    snprintf(hb_path, sizeof(hb_path), "espos.%s.heartbeat", espos_wifi_short_id());
    espos_sk_declare_meta(hb_path, "{\"description\":\"Example app heartbeat counter\"}", 1000);
    uint32_t heartbeat = 0;
    char label[33];
    for (;;) {
        bool enabled = true;
        int32_t interval = 1000;
        float scale = 1.0f;
        espos_config_get_bool(ESPOS_CFG_NS_APP, ESPOS_CFG_APP_ENABLED, &enabled);
        espos_config_get_i32(ESPOS_CFG_NS_APP, ESPOS_CFG_APP_INTERVAL_MS, &interval);
        espos_config_get_float(ESPOS_CFG_NS_APP, ESPOS_CFG_APP_SCALE, &scale);
        espos_config_get_str(ESPOS_CFG_NS_APP, ESPOS_CFG_APP_LABEL, label, sizeof(label), NULL);
        if (enabled) {
            ESP_LOGI(TAG, "%s: heartbeat (interval %" PRId32 " ms, scale %.3f)", label, interval, (double)scale);
            espos_sk_publish_number(hb_path, (double)(heartbeat++) * scale);
        }
        vTaskDelay(pdMS_TO_TICKS(interval));
    }
}

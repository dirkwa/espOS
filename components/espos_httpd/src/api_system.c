/*
 * SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * /api/v1/system/{info,reboot,factory-reset}
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_LINUX
#include <time.h>
#else
#include "esp_timer.h"
#endif

#include "espos_config.h"
#include "espos_httpd.h"
#include "espos_httpd_priv.h"

static const char *TAG = "espos_httpd";

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    case ESP_RST_EFUSE: return "efuse";
    case ESP_RST_PWR_GLITCH: return "power_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    case ESP_RST_UNKNOWN:
    default: return "unknown";
    }
}

static const char *chip_model_str(esp_chip_model_t m)
{
    switch (m) {
    case CHIP_ESP32: return "esp32";
    case CHIP_ESP32S2: return "esp32s2";
    case CHIP_ESP32S3: return "esp32s3";
    case CHIP_ESP32C3: return "esp32c3";
    case CHIP_ESP32C2: return "esp32c2";
    case CHIP_ESP32C6: return "esp32c6";
    case CHIP_ESP32H2: return "esp32h2";
    case CHIP_ESP32P4: return "esp32p4";
    case CHIP_ESP32C61: return "esp32c61";
    case CHIP_ESP32C5: return "esp32c5";
    case CHIP_ESP32H21: return "esp32h21";
    case CHIP_ESP32H4: return "esp32h4";
    default: return "unknown";
    }
}

static int64_t uptime_s(void)
{
#if CONFIG_IDF_TARGET_LINUX
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
#else
    return esp_timer_get_time() / 1000000;
#endif
}

static esp_err_t info_get(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    char body[512];
    snprintf(body, sizeof(body),
             "{\"app\":\"%s\",\"version\":\"%s\",\"idf_version\":\"%s\","
             "\"chip\":\"%s\",\"chip_revision\":%u,\"cores\":%u,"
             "\"uptime_s\":%" PRId64 ",\"free_heap\":%" PRIu32 ",\"min_free_heap\":%" PRIu32 ","
             "\"reset_reason\":\"%s\",\"config_storage_reset\":%s,\"schema_etag\":\"%s\",\"ui_storage\":%s}",
             app->project_name, app->version, esp_get_idf_version(),
             chip_model_str(chip.model), (unsigned)chip.revision, (unsigned)chip.cores,
             uptime_s(), esp_get_free_heap_size(), esp_get_minimum_free_heap_size(),
             reset_reason_str(esp_reset_reason()),
             espos_config_storage_was_reset() ? "true" : "false",
             espos_cfg_schema_etag, espos_httpd_static_mounted() ? "true" : "false");
    return espos_httpd_send_json(req, NULL, body);
}

static volatile bool s_restart_pending;

bool espos_httpd_restart_pending(void)
{
    return s_restart_pending;
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGW(TAG, "restarting");
    esp_restart();
}

/* Reply first, restart 500 ms later so the response reaches the client. */
static esp_err_t schedule_restart(void)
{
    /* Generous stack: esp_restart runs the registered shutdown handlers. */
    if (s_restart_pending) {
        return ESP_OK;
    }
    const uint32_t stack = configMINIMAL_STACK_SIZE > 3072 ? configMINIMAL_STACK_SIZE * 4 : 3072;
    BaseType_t ok = xTaskCreate(restart_task, "espos_restart", stack, NULL, tskIDLE_PRIORITY + 5, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_restart_pending = true;
    return ESP_OK;
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    if (!espos_httpd_require_json(req)) {
        return ESP_OK;
    }
    esp_err_t err = schedule_restart();
    if (err != ESP_OK) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "restart_failed", esp_err_to_name(err));
    }
    return espos_httpd_send_json(req, "202 Accepted", "{\"status\":\"rebooting\"}");
}

static esp_err_t factory_reset_post(httpd_req_t *req)
{
    if (!espos_httpd_require_json(req)) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "factory reset requested");
    /* Arm the restart first: PUTs are refused (503) from this point on, so
     * nothing can be written between the erase and the reboot. */
    esp_err_t err = schedule_restart();
    if (err != ESP_OK) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "restart_failed", esp_err_to_name(err));
    }
    err = espos_config_factory_reset();
    if (err != ESP_OK) {
        /* The store may be half-erased; the armed reboot still happens. */
        return espos_httpd_send_error(req, "500 Internal Server Error", "factory_reset_failed",
                                      "erase failed; rebooting anyway");
    }
    return espos_httpd_send_json(req, "202 Accepted", "{\"status\":\"factory_reset\",\"rebooting\":true}");
}

esp_err_t espos_httpd_register_system_api(httpd_handle_t h)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/v1/system/info", .method = HTTP_GET, .handler = info_get },
        { .uri = "/api/v1/system/reboot", .method = HTTP_POST, .handler = reboot_post },
        { .uri = "/api/v1/system/factory-reset", .method = HTTP_POST, .handler = factory_reset_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(h, &uris[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

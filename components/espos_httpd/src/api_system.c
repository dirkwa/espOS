/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * /api/v1/system/{info,reboot,factory-reset}. info carries the health
 * policy's reset record (last_reset) when the previous boot ended in one.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"
#if !CONFIG_IDF_TARGET_LINUX
#include "esp_timer.h"
#endif
#include "cJSON.h"

#include "espos_config.h"
#include "espos_health.h"
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

/* "last_reset": the record espos_health's policy left when it restarted the
 * device, or null. Its message is a consumer's free text, so this goes through
 * cJSON rather than snprintf: a quote in it must not break the document. */
static void add_last_reset(cJSON *root)
{
    espos_health_reset_record_t rec;
    if (!espos_health_last_reset(&rec)) {
        cJSON_AddNullToObject(root, "last_reset");
        return;
    }
    cJSON *lr = cJSON_AddObjectToObject(root, "last_reset");
    if (!lr) {
        return;
    }
    cJSON_AddStringToObject(lr, "reason", reset_reason_str(esp_reset_reason()));
    cJSON_AddStringToObject(lr, "health_key", rec.key);
    cJSON_AddStringToObject(lr, "message", rec.message);
    cJSON_AddNumberToObject(lr, "min_free_heap_before", rec.min_free_heap);
    cJSON_AddNumberToObject(lr, "min_internal_before", rec.min_internal);
    cJSON_AddNumberToObject(lr, "largest_block_before", rec.largest_block);
    cJSON_AddNumberToObject(lr, "uptime_before_s", rec.uptime_s);
    if (rec.unix_ms > 0) {
        time_t secs = (time_t)(rec.unix_ms / 1000);
        struct tm tm;
        char iso[32];
        gmtime_r(&secs, &tm);
        strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm);
        cJSON_AddStringToObject(lr, "at", iso);
    } else {
        cJSON_AddNullToObject(lr, "at"); /* the clock was never set that boot */
    }
}

static esp_err_t info_get(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    cJSON *j = cJSON_CreateObject();
    if (!j) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "no_mem", "out of memory");
    }
    cJSON_AddStringToObject(j, "app", app->project_name);
    cJSON_AddStringToObject(j, "version", app->version);
    cJSON_AddStringToObject(j, "idf_version", esp_get_idf_version());
    cJSON_AddStringToObject(j, "chip", chip_model_str(chip.model));
    cJSON_AddNumberToObject(j, "chip_revision", chip.revision);
    cJSON_AddNumberToObject(j, "cores", chip.cores);
    cJSON_AddNumberToObject(j, "uptime_s", (double)uptime_s());
    cJSON_AddNumberToObject(j, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(j, "min_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddStringToObject(j, "reset_reason", reset_reason_str(esp_reset_reason()));
    cJSON_AddBoolToObject(j, "config_storage_reset", espos_config_storage_was_reset());
    cJSON_AddStringToObject(j, "schema_etag", espos_cfg_schema_etag);
    cJSON_AddBoolToObject(j, "ui_storage", espos_httpd_static_mounted());
    add_last_reset(j);
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!body) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "no_mem", "out of memory");
    }
    esp_err_t err = espos_httpd_send_json(req, NULL, body);
    cJSON_free(body);
    return err;
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

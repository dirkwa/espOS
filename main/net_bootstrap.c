/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * M1 scaffolding only. Deliberately minimal: fixed credentials from Kconfig,
 * no reason codes, no provisioning, plain reconnect. espos_wifi (M2) replaces
 * this file entirely.
 */
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "net_bootstrap.h"

static const char *TAG = "net_bootstrap";

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "disconnected (reason %u), retrying", d ? d->reason : 0);
        esp_wifi_connect();
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    const ip_event_got_ip_t *e = data;
    ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
}

esp_err_t net_bootstrap_start(void)
{
    if (strlen(CONFIG_ESPOS_EXAMPLE_WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "no SSID configured (menuconfig → espOS example app); staying offline");
        return ESP_OK;
    }
    /* esp_wifi keeps PHY calibration in the default "nvs" partition. Normally
     * espos_config already initialised it; this is a no-op then. */
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL));
    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, CONFIG_ESPOS_EXAMPLE_WIFI_SSID, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, CONFIG_ESPOS_EXAMPLE_WIFI_PASSWORD, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = strlen(CONFIG_ESPOS_EXAMPLE_WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "connecting to '%s'", CONFIG_ESPOS_EXAMPLE_WIFI_SSID);
    return ESP_OK;
}

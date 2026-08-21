/*
 * SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * Bluedroid backend: controller bring-up, GAP scanning, advertisement
 * extraction.
 *
 * One file covers both targets because only the controller differs. On the
 * ESP32-P4 there is no local radio at all: Bluedroid's HCI is routed at an
 * ESP32-C6 co-processor over esp_hosted's SDIO transport. Everything above
 * HCI - GAP, GATT, the scan loop - is identical.
 */

#include "sdkconfig.h"

#if defined(CONFIG_BT_BLUEDROID_ENABLED)

#include <string.h>

#include "ble_gattc.h"
#include "ble_proto.h"
#include "ble_types.h"
#include "esp_bt_device.h"
#include "esp_check.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_log.h"
#include "esp_timer.h"

#if defined(CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID)
#include "esp_hosted.h"
#include "esp_hosted_bluedroid.h"
#else
#include "esp_bt.h"
#endif

static const char *TAG = "espos_ble_backend";

static espos_ble_callbacks_t s_cb;
static bool s_scanning;
static uint32_t s_scan_hits;
static char s_mac[ESPOS_BLE_ADDR_LEN];
static esp_ble_scan_params_t s_scan_params;

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    /* Setting scan parameters only arms the scan; it starts here, once the
     * controller acknowledges them. */
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        if (param->scan_param_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "scan param set failed: %d", param->scan_param_cmpl.status);
            break;
        }
        esp_ble_gap_start_scanning(0); /* 0 = until stopped */
        break;

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "scan start failed: %d", param->scan_start_cmpl.status);
            s_scanning = false;
        } else {
            s_scanning = true;
            ESP_LOGI(TAG, "scanning");
        }
        break;

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        s_scanning = false;
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) break;
        s_scan_hits++;
        if (!s_cb.on_advertisement) break;

        espos_ble_adv_t adv;
        memset(&adv, 0, sizeof(adv));
        snprintf(adv.address, sizeof(adv.address),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 param->scan_rst.bda[0], param->scan_rst.bda[1],
                 param->scan_rst.bda[2], param->scan_rst.bda[3],
                 param->scan_rst.bda[4], param->scan_rst.bda[5]);
        adv.rssi = param->scan_rst.rssi;
        adv.received_us = esp_timer_get_time();

        uint8_t len = param->scan_rst.adv_data_len + param->scan_rst.scan_rsp_len;
        if (len > ESPOS_BLE_ADV_DATA_MAX) len = ESPOS_BLE_ADV_DATA_MAX;
        memcpy(adv.adv_data, param->scan_rst.ble_adv, len);
        adv.adv_data_len = len;

        uint8_t name_len = 0;
        uint8_t *name = esp_ble_resolve_adv_data(
            param->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
        if (name && name_len) {
            if (name_len >= ESPOS_BLE_NAME_MAX) name_len = ESPOS_BLE_NAME_MAX - 1;
            memcpy(adv.name, name, name_len);
            adv.name[name_len] = '\0';
        }

        /* Runs on the BT stack task: the callback must not block. */
        s_cb.on_advertisement(&adv, s_cb.arg);
        break;
    }

    default:
        break;
    }
}

static void gattc_trampoline(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                             esp_ble_gattc_cb_param_t *param)
{
    espos_ble_gattc_event(event, gattc_if, param);
}

static esp_err_t controller_up(void)
{
#if defined(CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID)
    /* ESP32-P4: the controller lives on the C6. Order is load-bearing -
     * enabling the remote controller is what populates the VHCI driver's
     * function pointers, so attaching the HCI driver first faults on the
     * first call through them (verified 2026-08-21). */
    ESP_RETURN_ON_ERROR(esp_hosted_init(), TAG, "esp_hosted_init");
    esp_hosted_connect_to_slave();
    ESP_RETURN_ON_ERROR(esp_hosted_bt_controller_init(), TAG, "bt_controller_init");
    ESP_RETURN_ON_ERROR(esp_hosted_bt_controller_enable(), TAG, "bt_controller_enable");

    hosted_hci_bluedroid_open();
    esp_bluedroid_hci_driver_operations_t ops = {
        .send = hosted_hci_bluedroid_send,
        .check_send_available = hosted_hci_bluedroid_check_send_available,
        .register_host_callback = hosted_hci_bluedroid_register_host_callback,
    };
    ESP_RETURN_ON_ERROR(esp_bluedroid_attach_hci_driver(&ops), TAG, "attach_hci");
#else
    /* Native controller. Classic BT memory is released because this is a
     * BLE-only gateway and that RAM is scarce. */
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_bt_controller_init(&cfg), TAG, "bt_controller_init");
    ESP_RETURN_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_BLE), TAG,
                        "bt_controller_enable");
#endif
    return ESP_OK;
}

esp_err_t espos_ble_backend_init(const espos_ble_callbacks_t *cb)
{
    if (cb) s_cb = *cb;

    ESP_RETURN_ON_ERROR(controller_up(), TAG, "controller");
    ESP_RETURN_ON_ERROR(esp_bluedroid_init(), TAG, "bluedroid_init");
    ESP_RETURN_ON_ERROR(esp_bluedroid_enable(), TAG, "bluedroid_enable");
    ESP_RETURN_ON_ERROR(esp_ble_gap_register_callback(gap_cb), TAG, "gap_register");

    const uint8_t *mac = esp_bt_dev_get_address();
    if (mac) {
        snprintf(s_mac, sizeof(s_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        ESP_LOGI(TAG, "BT MAC %s", s_mac);
    }

#ifdef CONFIG_BT_GATTC_ENABLE
    ESP_RETURN_ON_ERROR(esp_ble_gattc_register_callback(gattc_trampoline), TAG,
                        "gattc_register");
    ESP_RETURN_ON_ERROR(espos_ble_gattc_init(cb), TAG, "gattc_init");
#endif
    return ESP_OK;
}

esp_err_t espos_ble_backend_deinit(void)
{
    /* Deliberately does not tear Bluedroid down: IDF's deinit paths for
     * stateful subsystems do not reliably return to a state a later init can
     * build on, and a gateway that cannot re-init its radio is worse than one
     * that leaves it running. */
    espos_ble_scan_stop();
    return ESP_OK;
}

/* Scan interval/window are in 0.625 ms units, clamped to the spec range. */
static uint16_t ms_to_units(uint32_t ms)
{
    uint32_t units = (ms * 1000U) / 625U;
    if (units < 0x0004) units = 0x0004;
    if (units > 0x4000) units = 0x4000;
    return (uint16_t)units;
}

esp_err_t espos_ble_scan_start(bool active, uint16_t interval_ms, uint16_t window_ms)
{
    if (window_ms > interval_ms) window_ms = interval_ms;

    s_scan_params.scan_type = active ? BLE_SCAN_TYPE_ACTIVE : BLE_SCAN_TYPE_PASSIVE;
    s_scan_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    s_scan_params.scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
    s_scan_params.scan_interval = ms_to_units(interval_ms);
    s_scan_params.scan_window = ms_to_units(window_ms);
    /* Duplicates are wanted: the server times devices out on silence, so a
     * beacon that never changes must keep being reported. */
    s_scan_params.scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE;

    return esp_ble_gap_set_scan_params(&s_scan_params);
}

esp_err_t espos_ble_scan_stop(void)
{
    if (!s_scanning) return ESP_OK;
    return esp_ble_gap_stop_scanning();
}

bool espos_ble_is_scanning(void) { return s_scanning; }
uint32_t espos_ble_scan_hits(void) { return s_scan_hits; }
const char *espos_ble_mac(void) { return s_mac; }

#endif /* CONFIG_BT_BLUEDROID_ENABLED */

/*
 * SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * Bluedroid GATT client, shared by both backends: the ESP32-P4 (HCI at the C6
 * over esp_hosted) and native Bluedroid targets differ only in how the
 * controller is brought up, not in the GATTC API above it.
 */
#pragma once

#include "sdkconfig.h"

#ifdef CONFIG_BT_GATTC_ENABLE

#include "ble_types.h"
#include "esp_gattc_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bluedroid's own ceiling for simultaneous GATT connections. */
#define ESPOS_BLE_GATTC_MAX_CONN 3

/* Register with Bluedroid. Call once after esp_bluedroid_enable(). */
esp_err_t espos_ble_gattc_init(const espos_ble_callbacks_t *cb);

/* Forward every GATTC event here from the app's registered callback. */
void espos_ble_gattc_event(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_BT_GATTC_ENABLE */

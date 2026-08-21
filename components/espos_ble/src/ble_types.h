/*
 * SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * Types shared between the BLE backends and the gateway. Deliberately free of
 * ESP-IDF Bluetooth headers so the protocol logic above can be built and
 * tested on the linux host target, where no BLE stack exists.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 31 bytes is the legacy advertising payload limit; scan responses add a
 * second 31. Sized for both so a future active-scan path fits without
 * touching the struct. */
#define ESPOS_BLE_ADV_DATA_MAX 62
#define ESPOS_BLE_NAME_MAX 32
#define ESPOS_BLE_ADDR_LEN 18 /* "AA:BB:CC:DD:EE:FF" + NUL */

/* One advertisement, copied out of the stack callback. Values only - no
 * pointers into stack-owned buffers, because these are queued. */
typedef struct {
    char address[ESPOS_BLE_ADDR_LEN]; /* uppercase, colon-separated */
    char name[ESPOS_BLE_NAME_MAX];    /* "" when not advertised */
    int8_t rssi;
    uint8_t adv_data_len;
    uint8_t adv_data[ESPOS_BLE_ADV_DATA_MAX];
    int64_t received_us; /* esp_timer_get_time() when seen */
} espos_ble_adv_t;

/* GATT write mode. Some peripherals (JK-BMS, Daly-BMS and friends) reject
 * write-with-response on their command characteristic with "Write not
 * permitted" and accept only write-without-response; signalk-server sends the
 * per-write `with_response` flag for exactly this. Note a no-response write
 * produces NO completion event, so anything sequencing writes must not wait
 * for one. */
typedef enum {
    ESPOS_BLE_WRITE_WITH_RESPONSE = 0,
    ESPOS_BLE_WRITE_NO_RESPONSE = 1,
} espos_ble_write_mode_t;

/* Callbacks from the BLE backend. All run on the Bluetooth stack task and
 * must not block. */
typedef struct {
    void (*on_advertisement)(const espos_ble_adv_t *adv, void *arg);
    void (*on_gatt_connected)(int conn_id, void *arg);
    void (*on_gatt_disconnected)(int conn_id, int reason, void *arg);
    void (*on_gatt_notify)(int conn_id, const char *char_uuid,
                           const uint8_t *data, size_t len, void *arg);
    void (*on_gatt_read)(int conn_id, const char *char_uuid,
                         const uint8_t *data, size_t len, void *arg);
    void (*on_gatt_write_done)(int conn_id, const char *char_uuid, bool ok,
                               void *arg);
    void (*on_gatt_error)(int conn_id, const char *error, void *arg);
    void *arg;
} espos_ble_callbacks_t;

/* Backend interface. One implementation is compiled in per target: the
 * esp_hosted VHCI path on the ESP32-P4, native Bluedroid elsewhere. */
esp_err_t espos_ble_backend_init(const espos_ble_callbacks_t *cb);
esp_err_t espos_ble_backend_deinit(void);
esp_err_t espos_ble_scan_start(bool active, uint16_t interval_ms, uint16_t window_ms);
esp_err_t espos_ble_scan_stop(void);
bool espos_ble_is_scanning(void);
uint32_t espos_ble_scan_hits(void);
/* Controller address as "AA:BB:...", or "" if unavailable. */
const char *espos_ble_mac(void);

/* GATT client. conn_id identifies the connection; -1 on failure. */
int espos_ble_gatt_connect(const char *mac, const char *service_uuid);
esp_err_t espos_ble_gatt_subscribe(int conn_id, const char *char_uuid);
esp_err_t espos_ble_gatt_read(int conn_id, const char *char_uuid);
/* Returns ESP_OK when the write was sent and a completion event will follow;
 * ESP_ERR_NOT_FINISHED when it was sent as write-without-response, for which
 * the stack generates no completion at all - callers sequencing writes must
 * advance themselves on that. Other codes are real failures.
 *
 * Never calls back into espos_ble_callbacks_t before returning, so a caller
 * may hold its own lock across it. */
esp_err_t espos_ble_gatt_write(int conn_id, const char *char_uuid,
                               const uint8_t *data, size_t len,
                               espos_ble_write_mode_t mode);
esp_err_t espos_ble_gatt_disconnect(int conn_id);
uint32_t espos_ble_gatt_active_count(void);

#ifdef __cplusplus
}
#endif

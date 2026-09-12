/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * BLE provisioning: hand a device its WiFi credentials from a phone, with no
 * access point and no captive portal.
 *
 * WHAT THIS DOES NOT DO, and why it matters.
 *
 * Espressif's network_provisioning manager normally drives the station itself
 * -- esp_wifi_set_config(), esp_wifi_connect(), esp_wifi_start() and its own
 * retry logic. espOS already has a WiFi state machine that owns exactly those
 * calls and is host-tested against them, and two owners of one radio is a
 * fault that only shows up in the field.
 *
 * So this component uses the manager as a BLE TRANSPORT ONLY. Credentials
 * arriving over BLE are written into the `wifi` config namespace -- the same
 * keys the web UI writes -- and the existing state machine picks them up
 * through its config-change callback and connects exactly as it always does.
 * Nothing here calls esp_wifi_*.
 *
 * The consequence is deliberate: because the manager never attempts the
 * connection, it cannot report "wrong password" from its own connect path.
 * espos_prov answers the phone from espos_wifi_get_status() instead, which is
 * the same truth by a different route.
 *
 * THE SCANNER STOPS WHILE THIS RUNS. Bluedroid keeps exactly one GAP callback
 * (esp_ble_gap_register_callback is a setter, not a subscribe), and
 * protocomm's simple_ble registers its own. A BLE gateway scanning at the
 * same time would keep running and silently receive nothing. espos_prov
 * therefore suspends the scanner for the duration and resumes it --
 * reclaiming the GAP callback first -- when provisioning ends. A device being
 * provisioned has no network to publish to yet, so nothing is lost.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* BLE device name the phone sees. Empty picks "ESPOS_<id>", where <id> is
     * the same short device id used for the hostname and the portal SSID, so
     * one device is one recognisable name everywhere. */
    const char *service_name;
    /* Proof of possession, at most 23 characters; longer is rejected rather
     * than truncated. Empty makes the device generate a random one on first
     * use and keep it (config key `prov.pop`), printed on the log and served
     * by GET /api/v1/prov so it can be shown to whoever is holding the phone.
     *
     * It is deliberately NOT derived from the MAC or the device id: the
     * advertised name already carries the id, so a derived PoP would travel
     * over the air beside the thing it protects. A PoP printed on the
     * enclosure at manufacture is better still, and is what this field is
     * for. */
    const char *pop;
    /* Stop advertising after this many seconds. 0 takes
     * CONFIG_ESPOS_PROV_TIMEOUT_S. A device left advertising is a device
     * anyone in the marina can try to provision, so the window closes on its
     * own -- and closes early, a few seconds after credentials arrive, since
     * the job is then done. */
    uint32_t timeout_s;
} espos_prov_cfg_t;

/** Start BLE provisioning. NULL takes every default.
 *
 * Suspends BLE scanning if espos_ble is running (see the note above), starts
 * the GATT server, and returns as soon as it is advertising -- credentials
 * arrive later, on the provisioning task.
 *
 * ESP_ERR_INVALID_STATE if already running.
 */
esp_err_t espos_prov_start(const espos_prov_cfg_t *cfg);

/** Stop provisioning, tear down the GATT server and resume scanning. */
esp_err_t espos_prov_stop(void);

/** True between a successful start() and stop(). */
bool espos_prov_is_active(void);

/** True once credentials have been received and written to config. The WiFi
 * state machine decides what happens next; this only says the handover
 * happened. */
bool espos_prov_got_credentials(void);

/** Register GET /api/v1/prov (state, service name, PoP, QR payload).
 * Called by espos_prov_start(); exposed for tests. */
esp_err_t espos_prov_register_api(void);

#ifdef __cplusplus
}
#endif

/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * ble_gateway: bridges BLE sensors to signalk-server's BLE provider API.
 *
 * This is the whole application. Everything of substance lives in
 * espos_ble, and so does the boot order: espos_start() brings up the log
 * ring, config, the HTTP server, WiFi, Signal K, OTA and then the gateway,
 * in the one order that works (docs/concepts.md).
 *
 * Which of the optional stacks it starts is decided at CONFIGURE time, from
 * the component list in main/CMakeLists.txt and the Bluetooth settings in
 * sdkconfig.defaults -- not by anything written here. That is why adding a
 * gateway to a firmware is a build-file change rather than a code change.
 *
 * This example was a separate repository (dirkwa/espos-ble-gateway) until it
 * became twenty lines. What is worth keeping is not the code but the
 * configuration around it: the two sdkconfig fragments carry the BLE 4.2
 * choice and the ESP32-P4 controller init order, both of which cost real
 * bring-up time to find.
 */

#include "espos.h"

void app_main(void)
{
    ESP_ERROR_CHECK(espos_start(NULL));
}

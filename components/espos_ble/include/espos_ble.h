/*
 * SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * espos_ble — BLE gateway: bridges BLE devices to signalk-server's BLE
 * provider API.
 *
 * The device is a dumb, stateless bridge. It does NOT decode sensors and does
 * NOT publish SignalK deltas: raw advertisements and GATT bytes go to the
 * server, and signalk-server (with bt-sensors-plugin-sk) owns all decoding,
 * path naming and units. What each device is, which characteristics to read
 * and what to write arrives at runtime as `gatt_subscribe` commands.
 *
 * Two channels, both authenticated with the token espos_sk already holds:
 *
 *   POST /signalk/v2/api/ble/gateway/advertisements
 *        Batched advertisements, sent periodically.
 *   WS   /signalk/v2/api/ble/gateway/ws?token=<jwt>
 *        Control protocol: hello/status out, gatt_* commands in.
 *
 * Needs espos_config, espos_httpd and espos_sk started first.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the gateway: brings up the BLE stack, starts scanning, and runs the
 * POST + control-WS tasks. Reads its settings from the `ble` config
 * namespace. Idempotent. */
esp_err_t espos_ble_start(void);
esp_err_t espos_ble_stop(void);

/** Runtime counters, mirrored into GET /api/v1/ble/status and the `ble` SSE
 * event. */
typedef struct {
    bool enabled;
    bool scanning;
    char mac[18];             /* controller address, "" if unknown */
    uint32_t scan_hits;       /* advertisements seen by the scanner */
    uint32_t adv_received;    /* handed to the gateway */
    uint32_t adv_posted;      /* accepted by the server */
    uint32_t adv_dropped;     /* shed because the buffer was full */
    size_t adv_pending;       /* waiting for the next POST */
    uint32_t post_success;
    uint32_t post_fail;
    bool ws_connected;
    uint32_t gatt_sessions;   /* currently active */
    uint32_t gatt_max;        /* concurrent session ceiling */
} espos_ble_status_t;

esp_err_t espos_ble_get_status(espos_ble_status_t *out);

/** Status document for docs/api.md (malloc'ed JSON; caller frees). */
esp_err_t espos_ble_status_json(char **out_json);

/** Register GET /api/v1/ble/status and the `ble` SSE snapshot hook. Called by
 * espos_ble_start(); exposed for tests. */
esp_err_t espos_ble_register_api(void);

#ifdef __cplusplus
}
#endif

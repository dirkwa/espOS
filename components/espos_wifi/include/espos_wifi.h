/*
 * SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * espos_wifi — station connection manager with an explicit status model,
 * multi-network priority list, exponential backoff, and a SoftAP
 * provisioning portal. Configuration lives in the "wifi" namespace of
 * espos_config; status is exposed at GET /api/v1/wifi/status and pushed on
 * the SSE stream as "wifi" events.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "espos_wifi_sm.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bring up netif/event loop/driver, load the "wifi" namespace, register the
 * HTTP endpoints and start the state machine. Requires espos_config_init()
 * and espos_httpd_start() to have run. Idempotent.
 */
esp_err_t espos_wifi_start(void);
esp_err_t espos_wifi_stop(void);

/** Snapshot of the current status (thread-safe copy). rssi is refreshed
 * from the driver when connected. */
typedef struct {
    espos_wifi_sm_status_t sm;
    int8_t rssi;
    uint32_t backoff_remaining_ms;
    uint32_t connected_s;      /* seconds since GOT_IP, 0 if not connected */
    char hostname[33];
    char portal_ssid[33];
    char portal_ip[16];
} espos_wifi_status_t;

esp_err_t espos_wifi_get_status(espos_wifi_status_t *out);

/** Refresh the cached RSSI from the radio.
 *
 * **MAY BLOCK.** On co-processor boards (esp_hosted) this is a
 * synchronous RPC across the host<->radio link and takes the transport
 * timeout to fail when that link is wedged. Call it from an application
 * task that can afford to wait — never from a UI, render or event task.
 *
 * espos_wifi_get_status() deliberately does NOT do this: a status
 * snapshot must never depend on the radio answering. Without calling
 * this, `rssi` is the value captured at association, which is accurate
 * enough for a status line and costs nothing.
 *
 * ESP_ERR_NOT_SUPPORTED if the driver has no RSSI source,
 * ESP_ERR_INVALID_STATE if not connected, ESP_FAIL if the radio did not
 * answer. */
esp_err_t espos_wifi_refresh_rssi(void);

/** Serialise the status as the JSON document of docs/api.md (malloc'ed). */
esp_err_t espos_wifi_status_json(char **out_json);

/* Scan API: start is asynchronous; results are cached and a "wifi_scan"
 * SSE event fires when they are ready. */
esp_err_t espos_wifi_scan_start(void);
typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    uint8_t authmode;          /* wifi_auth_mode_t value */
} espos_wifi_scan_entry_t;
/** JSON: {"scanning":bool,"age_s":n,"results":[{"ssid","bssid","rssi","channel","auth"}]} */
esp_err_t espos_wifi_scan_json(char **out_json);

/** Device-unique short id, "1a2b" (last two MAC bytes), for default names. */
const char *espos_wifi_short_id(void);

/* Co-processor link watchdog (esp_hosted builds only; a no-op elsewhere).
 *
 * A wedged host<->co-processor transport takes the radio down while the
 * WiFi state machine still reports CONNECTED, so nothing above notices.
 * This watches the co-processor heartbeat — which travels over the same
 * RPC channel that dies — and reinitialises the transport when it stops,
 * without rebooting the device.
 *
 * Safe to call on every boot; returns ESP_ERR_NOT_SUPPORTED when the
 * build has no hosted co-processor. A non-OK return means wedges will
 * NOT be detected, not that WiFi is broken. */
esp_err_t espos_wifi_hosted_watchdog_start(void);

/* Times the transport was reinitialised since boot. 0 on non-hosted
 * builds. Surfaced in the WiFi status document so a link that keeps
 * wedging is visible without a serial console. */
uint32_t espos_wifi_hosted_recoveries(void);

#ifdef __cplusplus
}
#endif

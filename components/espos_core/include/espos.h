/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos — one call brings espOS up.
 *
 *     #include "espos.h"
 *     void app_main(void) { ESP_ERROR_CHECK(espos_start(NULL)); ... }
 *
 * espos_start() runs the component starts in the one order that works
 * (docs/concepts.md): the log ring first so the boot log is kept for
 * /api/v1/logs; the config store next because everything else reads it;
 * the HTTP server before WiFi so the provisioning portal has a page to
 * serve the moment its access point is up; WiFi before SignalK because
 * discovery is mDNS; then OTA and BLE, which need all of the above. Each
 * espos_*_start() checks its own prerequisites and fails with
 * ESP_ERR_INVALID_STATE when called out of order — espos_start() is how
 * an application never sees that error.
 *
 * Optional components (espos_sk, espos_ota, espos_ble) are started only
 * when the project builds them. That is decided at configure time from the
 * component list, not by the application.
 *
 * Threading: call once, from app_main() or any task. The calls block until
 * every component has started its own tasks, then return; before_network
 * runs on the caller's task. Thread-safe against a second caller.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* The name the device presents: the SignalK access-request description
     * and the hostname prefix. NULL = the project name (PROJECT_NAME). */
    const char *app_name;
    /* Runs after espos_init() — log and config are up — and before the
     * network stack: bring a display up here so it can show the portal
     * SSID. A non-OK return aborts espos_start() with that error. */
    esp_err_t (*before_network)(void *arg);
    void *arg; /* handed to before_network */
    /* Restart on health conditions the device cannot recover from. Default
     * true; also gated by the espOS core Kconfig option. The policy sink
     * is armed in espos_init() via espos_health_policy_start(); see docs/health.md. */
    bool health_watchdog;
} espos_start_opts_t;

#define ESPOS_START_OPTS_DEFAULT { .app_name = NULL, .before_network = NULL, .arg = NULL, .health_watchdog = true }

/**
 * Bring everything up: log → config → [before_network] → httpd → wifi →
 * sk → ota → ble, each of the last three only if built. NULL = defaults.
 * Idempotent: a second call returns ESP_OK and does nothing. On failure
 * the components started so far stay up and the error names the stage.
 */
esp_err_t espos_start(const espos_start_opts_t *opts);

/**
 * First half of espos_start(): the log ring and the config store, then
 * ESPOS_EVENT_CONFIG_READY. Idempotent; an application that initialised
 * the store itself beforehand is fine. For code that must run between
 * config and the network and cannot be expressed as before_network.
 */
esp_err_t espos_init(void);

/** Second half: httpd → wifi → [sk] → [ota] → [ble]. Requires the config
 * store to be up (espos_init()); ESP_ERR_INVALID_STATE otherwise. Idempotent. */
esp_err_t espos_start_network(void);

/** espOS's own version ("0.7.0"): version.txt at build time, or the
 * component manifest's version for a registry-installed copy. Not the
 * application's version — that is PROJECT_VER, on the boot banner. */
const char *espos_version(void);

/** espos_start_opts_t.app_name if one was given, else PROJECT_NAME. Valid
 * before espos_start() (the project name) and stable afterwards. */
const char *espos_app_name(void);

/**
 * The public C ABI: every header under the components' include directories, taken
 * together (docs/development.md, "Public API rules"). Bumped by any change
 * to one of them that is not purely additive — a function, type, macro or
 * enum value removed or renamed; a signature changed; a struct's members
 * changed in any way (appending one changes sizeof, which a caller compiled
 * against the old header has baked in); an enum or macro value changed; a
 * callback's threading contract changed; a new exception to the include
 * rule. Not bumped by a new function, macro or header, or an enum value
 * appended before its _MAX when no public struct is sized by that _MAX.
 * A binding generated from the headers records the value it was built
 * against and compares it with espos_abi_version() at run time.
 */
#define ESPOS_ABI_VERSION 1

/** ESPOS_ABI_VERSION of the espos_core actually linked, for code compiled
 * against another copy of the headers. Callable at any time, any task. */
int espos_abi_version(void);

#ifdef __cplusplus
}
#endif

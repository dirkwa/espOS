/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * The start sequence. Nothing here owns state beyond the options it was
 * started with; every component keeps its own and enforces its own order
 * guard, so this file is the recommended order written down once, not the
 * only way to get a working device.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "espos.h"
#include "espos_config.h"
#include "espos_event.h"
#include "espos_health.h"
#include "espos_httpd.h"
#include "espos_log.h"
#include "espos_net.h"
#include "espos_time.h"
/* espos_wifi is in the build on every target with a radio (or, ESP32-P4, a
 * co-processor) and excluded on the 802.15.4-only H-series; CONFIG_ESPOS_WIFI
 * lets a firmware that links it (an Ethernet gateway on a WiFi chip) leave
 * the station off without touching the component list. */
#if ESPOS_HAVE_WIFI && CONFIG_ESPOS_WIFI
#define START_WIFI 1
#include "espos_wifi.h"
#else
#define START_WIFI 0
#endif
#if ESPOS_HAVE_SK
#include "espos_sk.h"
#endif
#if ESPOS_HAVE_OTA
#include "espos_ota.h"
#endif
/* espos_ble compiles its gateway only with Bluedroid (ble_gateway.c); a
 * project that links the component without the stack has no
 * espos_ble_start() to call, so "built" means both. */
#if ESPOS_HAVE_BLE && defined(CONFIG_BT_BLUEDROID_ENABLED)
#define START_BLE 1
#include "espos_ble.h"
#else
#define START_BLE 0
#endif

static const char *TAG = "espos";

/* Filled in by CMakeLists.txt; the fallbacks only matter to a build that
 * compiles this file outside the component (a host unit test, say). */
#ifndef ESPOS_VERSION
#define ESPOS_VERSION "0.0.0-unknown"
#endif
#ifndef ESPOS_PROJECT_NAME
#define ESPOS_PROJECT_NAME "espos"
#endif
#ifndef ESPOS_PROJECT_VER
#define ESPOS_PROJECT_VER "?"
#endif

#define APP_NAME_MAX 32

static struct {
    SemaphoreHandle_t lock; /* two tasks calling espos_start() at once run it once */
    bool inited;
    bool network_started;
    bool started;
    bool health_watchdog; /* arm espos_health's policy in espos_init() */
    char app_name[APP_NAME_MAX + 1];
} s = { .health_watchdog = true }; /* the default when espos_init() is called without espos_start() */

/* netDown is a warning and never fatal, by construction. A router reboot, an
 * access-point roam or a trip out of range are normal, and the transport
 * reconnects by itself; a restart would throw away the UI, every socket and
 * any unsaved state to fix nothing, and while the network is marginal it
 * would repeat every strike window — a consumer once shipped exactly that
 * and got a panel rebooting every ~90 s. The link that only LOOKS connected
 * (a wedged co-processor transport) is caught by espos_sk's skLinkStalled,
 * which watches real traffic and is the case a restart does fix. */
static void on_network(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == ESPOS_EVENT_NETWORK_UP) {
        (void)espos_health_report_ex("netDown", ESPOS_HEALTH_NORMAL, "", 0);
    } else if (id == ESPOS_EVENT_NETWORK_DOWN) {
        (void)espos_health_report_ex("netDown", ESPOS_HEALTH_WARN, "network link lost", 0);
    }
}

/* One stage of the sequence: on failure say which one, so the boot log
 * reads "espos_wifi_start: ESP_ERR_NO_MEM" rather than a bare error code
 * from app_main's ESP_ERROR_CHECK. */
#define STAGE(call)                                                      \
    do {                                                                 \
        esp_err_t stage_err_ = (call);                                   \
        if (stage_err_ != ESP_OK) {                                      \
            ESP_LOGE(TAG, "%s: %s", #call, esp_err_to_name(stage_err_)); \
            return stage_err_;                                           \
        }                                                                \
    } while (0)

const char *espos_version(void)
{
    return ESPOS_VERSION;
}

int espos_abi_version(void)
{
    return ESPOS_ABI_VERSION;
}

const char *espos_app_name(void)
{
    return s.app_name[0] ? s.app_name : ESPOS_PROJECT_NAME;
}

esp_err_t espos_init(void)
{
    if (s.inited) {
        return ESP_OK;
    }
    /* Log ring first: the banner below is the first line a reader of
     * /api/v1/logs sees, and everything config says about itself follows. */
    STAGE(espos_log_init());
    ESP_LOGI(TAG, "espOS %s on %s — app %s %s", espos_version(), CONFIG_IDF_TARGET, espos_app_name(), ESPOS_PROJECT_VER);
    esp_err_t err = espos_config_init(NULL, NULL);
    if (err == ESP_ERR_INVALID_STATE && espos_config_is_ready()) {
        err = ESP_OK; /* the application brought the store up itself; fine */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "espos_config_init: %s", esp_err_to_name(err));
        return err;
    }
    s.inited = true;
    /* Before the network exists, so the first NETWORK_UP is heard: it is the
     * clear that retires a netDown a previous boot may have left on the server. */
    if (espos_event_subscribe(ESPOS_EVENT_NETWORK_UP, on_network, NULL) != ESP_OK ||
        espos_event_subscribe(ESPOS_EVENT_NETWORK_DOWN, on_network, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "netDown health condition unavailable (event subscribe failed)");
    }
#if CONFIG_ESPOS_CORE_HEALTH_WATCHDOG
    if (s.health_watchdog) {
        /* Not a STAGE: a device that boots without its watchdog beats one that
         * does not boot, and the log says which it is. */
        esp_err_t herr = espos_health_policy_start();
        if (herr != ESP_OK) {
            ESP_LOGE(TAG, "espos_health_policy_start: %s — running without the health watchdog", esp_err_to_name(herr));
        }
    } else {
        ESP_LOGW(TAG, "health watchdog off (espos_start_opts_t.health_watchdog)");
    }
#endif
    (void)espos_event_post(ESPOS_EVENT_CONFIG_READY, NULL, 0);
    return ESP_OK;
}

esp_err_t espos_start_network(void)
{
    if (!espos_config_is_ready()) {
        ESP_LOGE(TAG, "espos_start_network: call espos_init() first (or espos_start())");
        return ESP_ERR_INVALID_STATE;
    }
    if (s.network_started) {
        return ESP_OK;
    }
    /* httpd before net: /net/status and the portal's page live on it. net
     * before any transport: it owns the hostname the transport's DHCP request
     * carries, and the mDNS responder. time after net and before every
     * transport, so the first NETWORK_UP — whichever transport produces it —
     * is the one that starts SNTP. wifi (when built and enabled) before
     * sk: discovery is mDNS and needs a link. ota and ble need all of it. */
    STAGE(espos_httpd_start());
    STAGE(espos_net_start());
    /* Not a STAGE: a device that boots without knowing the time still does
     * everything else, and the log says which it is. The alternative — refusing
     * to come up because an NTP client could not be armed — would be a device
     * bricked by a configuration typo. */
    {
        esp_err_t terr = espos_time_start();
        if (terr != ESP_OK) {
            ESP_LOGE(TAG, "espos_time_start: %s — running without a wall clock", esp_err_to_name(terr));
        }
    }
#if START_WIFI
    STAGE(espos_wifi_start());
#else
    ESP_LOGW(TAG, "no WiFi in this build: the network comes up only if a transport reports into espos_net");
#endif
#if ESPOS_HAVE_SK
    /* Shows up in the server's access-request list as "<app> <hostname>"
     * unless sk.description is set — the operator approving it sees which
     * device asks. */
    (void)espos_sk_set_app_name(espos_app_name());
    STAGE(espos_sk_start());
#endif
#if ESPOS_HAVE_OTA
    STAGE(espos_ota_start());
#endif
#if START_BLE
    STAGE(espos_ble_start());
#if ESPOS_HAVE_WIFI
    /* The setup portal is raised inside espos_wifi_start(), which ran above,
     * so the gateway missed the PORTAL_UP event that tells it to stand down.
     * Catch up here.
     *
     * This is not a nicety on a co-processor part: the ESP32-P4's C6 is ONE
     * radio serving WiFi and BLE, and the gateway's default scan takes half
     * the airtime (160 ms window, 320 ms interval). Joining the portal then
     * takes minutes, or fails at DHCP -- measured on the bench, and reported
     * from a phone as "takes ages". A device showing its portal has nowhere
     * to publish advertisements to anyway. */
    espos_wifi_status_t wst;
    if (espos_wifi_get_status(&wst) == ESP_OK && wst.sm.portal_active) {
        espos_ble_scan_suspend("setup portal is up (shared radio)");
    }
#endif
#endif
    s.network_started = true;
    return ESP_OK;
}

esp_err_t espos_start(const espos_start_opts_t *opts)
{
    static const espos_start_opts_t defaults = ESPOS_START_OPTS_DEFAULT;
    if (!opts) {
        opts = &defaults;
    }
    if (!s.lock) {
        s.lock = xSemaphoreCreateMutex();
        if (!s.lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (s.started) {
        goto out;
    }
    if (opts->app_name && opts->app_name[0]) {
        snprintf(s.app_name, sizeof(s.app_name), "%s", opts->app_name);
    }
#if CONFIG_ESPOS_CORE_HEALTH_WATCHDOG
    s.health_watchdog = opts->health_watchdog;
#else
    s.health_watchdog = false;
#endif
    err = espos_init();
    if (err != ESP_OK) {
        goto out;
    }
    if (opts->before_network) {
        err = opts->before_network(opts->arg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "before_network: %s", esp_err_to_name(err));
            goto out;
        }
    }
    err = espos_start_network();
    if (err == ESP_OK) {
        s.started = true;
    }
out:
    xSemaphoreGive(s.lock);
    return err;
}

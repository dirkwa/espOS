/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * The one wall clock a device has: the policy instance, the sources that feed
 * it, and the notifications that go out when one does.
 *
 * The component owns no thread. SNTP calls in from IDF's own task, the
 * SignalK fallback from the stream task, a manual set from the HTTP server's;
 * all three go through espos_time_set() and out to the subscribers on
 * whichever task made the call.
 *
 * One invariant this file must keep: NOTHING here calls ESP_LOG while holding
 * s.lock. The log ring's hook asks this component for the wall clock to stamp
 * a line (espos_log's CONFIG_ESPOS_LOG_WALLCLOCK), so it takes s.lock while
 * holding its own — logging under s.lock would take them in the other order
 * and deadlock the first time a line is written from a task that is setting
 * the clock. Every log line below is therefore outside the locked region;
 * that is deliberate, not incidental.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "espos_cfg_keys.h"
#include "espos_config.h"
#include "espos_event.h"
#include "espos_httpd.h"
#include "espos_time.h"
#include "espos_time_policy.h"
#include "time_port.h"

static const char *TAG = "espos_time";

#define SUB_MAX CONFIG_ESPOS_TIME_MAX_SUBSCRIBERS

typedef struct {
    espos_time_cb_t cb;
    void *arg;
} sub_t;

static struct {
    SemaphoreHandle_t lock;
    bool started;
    espos_time_policy_t policy;
    sub_t subs[SUB_MAX];
    /* Configuration, read at start and followed live where that costs
     * nothing. The SNTP server list is not followed: re-arming the client
     * while it is mid-poll is more disruption than a restart, so those keys
     * are restart_required in the descriptor. */
    bool sntp_enabled;
    bool sntp_armed;   /* esp_netif_sntp_init() succeeded */
    bool sntp_started; /* polling began on a NETWORK_UP */
    char tz[ESPOS_TIME_TZ_MAX];
} s;

static void lock(void)
{
    if (s.lock) {
        xSemaphoreTake(s.lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s.lock) {
        xSemaphoreGive(s.lock);
    }
}

/* ------------------------------------------------------------- policy port */

static int64_t port_now_ms(void *ctx)
{
    (void)ctx;
    return espos_time_port_now_ms();
}

static const espos_time_policy_port_t POLICY_PORT = { .now_ms = port_now_ms };

/* ------------------------------------------------------------- accessors */

bool espos_time_is_synced(void)
{
    lock();
    bool v = s.started && espos_time_policy_is_synced(&s.policy);
    unlock();
    return v;
}

int64_t espos_time_now_ms(void)
{
    lock();
    int64_t v = s.started ? espos_time_policy_now_ms(&s.policy) : 0;
    unlock();
    return v;
}

int64_t espos_time_now_ms_or_zero(void *arg)
{
    (void)arg;
    return espos_time_now_ms();
}

espos_time_src_t espos_time_source(void)
{
    lock();
    espos_time_src_t v = s.started ? s.policy.src : ESPOS_TIME_SRC_NONE;
    unlock();
    return v;
}

const char *espos_time_src_str(espos_time_src_t src)
{
    switch (src) {
    case ESPOS_TIME_SRC_RTC: return "rtc";
    case ESPOS_TIME_SRC_SK: return "sk";
    case ESPOS_TIME_SRC_MANUAL: return "manual";
    case ESPOS_TIME_SRC_SNTP: return "sntp";
    case ESPOS_TIME_SRC_NONE:
    default: return "none";
    }
}

size_t espos_time_iso8601(char *buf, size_t n)
{
    return espos_time_iso8601_format(espos_time_now_ms(), buf, n);
}

size_t espos_time_iso8601_of(int64_t unix_ms, char *buf, size_t n)
{
    return espos_time_iso8601_format(unix_ms, buf, n);
}

/* Returns the buffer itself, not a copy, so it is deliberately never shortened
 * in place: espos_time_set_tz() overwrites it with snprintf, which NUL-
 * terminates, and a reader racing that write sees either string whole. Handing
 * back a pointer keeps this the same shape as espos_net_short_id(). */
const char *espos_time_tz(void)
{
    return s.tz[0] ? s.tz : "UTC0";
}

esp_err_t espos_time_parts(espos_time_parts_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    int64_t now = espos_time_now_ms();
    if (now <= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    espos_time_port_localtime(now, out);
    return ESP_OK;
}

/* ------------------------------------------------------------ subscribers */

esp_err_t espos_time_subscribe(espos_time_cb_t cb, void *arg)
{
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Subscribing is allowed before start(), so an application can register in
     * app_main and not race the first sync. The table is static, so there is
     * nothing to initialise for it. */
    lock();
    for (size_t i = 0; i < SUB_MAX; i++) {
        if (s.subs[i].cb == cb && s.subs[i].arg == arg) {
            unlock();
            return ESP_OK;
        }
    }
    for (size_t i = 0; i < SUB_MAX; i++) {
        if (!s.subs[i].cb) {
            s.subs[i].cb = cb;
            s.subs[i].arg = arg;
            unlock();
            return ESP_OK;
        }
    }
    unlock();
    return ESP_ERR_NO_MEM;
}

esp_err_t espos_time_unsubscribe(espos_time_cb_t cb, void *arg)
{
    lock();
    for (size_t i = 0; i < SUB_MAX; i++) {
        if (s.subs[i].cb == cb && s.subs[i].arg == arg) {
            s.subs[i].cb = NULL;
            s.subs[i].arg = NULL;
            unlock();
            return ESP_OK;
        }
    }
    unlock();
    return ESP_ERR_NOT_FOUND;
}

/* Run the subscribers with the lock RELEASED: a subscriber is documented to
 * run without an espos_time lock held, and one that reads the clock back — the
 * obvious thing to do from a sync callback — would otherwise deadlock. */
static void notify(espos_time_src_t src)
{
    sub_t copy[SUB_MAX];
    lock();
    memcpy(copy, s.subs, sizeof(copy));
    unlock();
    for (size_t i = 0; i < SUB_MAX; i++) {
        if (copy[i].cb) {
            copy[i].cb(src, copy[i].arg);
        }
    }
}

/* ------------------------------------------------------------------ set */

static esp_err_t set_internal(int64_t unix_ms, espos_time_src_t src, int64_t prior_age_ms)
{
    lock();
    if (!s.started) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = espos_time_policy_set_aged(&s.policy, unix_ms, src, prior_age_ms);
    unlock();
    if (err != ESP_OK) {
        return err;
    }
    /* The rest of the system reads the time through libc (mbedTLS checking a
     * certificate's validity, IDF's own log timestamps), so it has to agree
     * with the policy or a device would hold two different opinions about
     * what day it is. */
    espos_time_port_set_system_time(unix_ms);
    espos_time_port_rtc_store(unix_ms, src);
    ESP_LOGI(TAG, "clock set from %s", espos_time_src_str(src));
    notify(src);
    espos_event_time_t ev = { .source = (uint8_t)src, .unix_ms = unix_ms };
    (void)espos_event_post(ESPOS_EVENT_TIME_SYNCED, &ev, sizeof(ev));
    return ESP_OK;
}

esp_err_t espos_time_set(int64_t unix_ms, espos_time_src_t src)
{
    return set_internal(unix_ms, src, 0);
}

/* Runs on IDF's SNTP task. */
static void on_sntp(int64_t unix_ms)
{
    (void)set_internal(unix_ms, ESPOS_TIME_SRC_SNTP, 0);
}

/* ------------------------------------------------------------ timezone */

esp_err_t espos_time_set_tz(const char *posix_tz)
{
    const char *tz = (posix_tz && *posix_tz) ? posix_tz : "UTC0";
    if (strlen(tz) >= ESPOS_TIME_TZ_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    lock();
    snprintf(s.tz, sizeof(s.tz), "%s", tz);
    unlock();
    espos_time_port_set_tz(tz);
    /* Persisted so a display keeps its timezone across a reboot. A write of
     * the value already stored is a no-op in the config store, so following
     * our own change back through on_config_change costs nothing. */
    (void)espos_config_set_str(ESPOS_CFG_NS_TIME, ESPOS_CFG_TIME_TZ, tz);
    return ESP_OK;
}

/* --------------------------------------------------------------- events */

/* The first NETWORK_UP is what SNTP was waiting for. Later ones are ignored:
 * the client re-resolves and re-polls by itself, and on a DHCP-supplied server
 * list esp_netif_sntp refreshes it on IP_EVENT_STA_GOT_IP without our help. */
static void on_network(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    lock();
    bool go = s.sntp_armed && !s.sntp_started;
    if (go) {
        s.sntp_started = true;
    }
    unlock();
    if (!go) {
        return;
    }
    esp_err_t err = espos_time_port_sntp_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP start: %s", esp_err_to_name(err));
        lock();
        s.sntp_started = false; /* let the next NETWORK_UP try again */
        unlock();
        return;
    }
    ESP_LOGI(TAG, "SNTP polling started");
}

/* --------------------------------------------------------------- config */

static void on_config_change(const char *ns, const char *key, void *arg)
{
    (void)arg;
    if (strcmp(ns, ESPOS_CFG_NS_TIME) != 0) {
        return;
    }
    if (strcmp(key, ESPOS_CFG_TIME_TZ) == 0) {
        char tz[ESPOS_TIME_TZ_MAX];
        if (espos_config_get_str(ESPOS_CFG_NS_TIME, ESPOS_CFG_TIME_TZ, tz, sizeof(tz), NULL) == ESP_OK) {
            lock();
            snprintf(s.tz, sizeof(s.tz), "%s", tz[0] ? tz : "UTC0");
            unlock();
            espos_time_port_set_tz(s.tz);
        }
    }
    /* The SNTP keys are restart_required: re-arming a running client mid-poll
     * is more disruption than a reboot, and the descriptor says so. */
}

/* ------------------------------------------------------------ REST doc */

esp_err_t espos_time_status_json(char **out_json)
{
    if (!out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s.started) {
        return ESP_ERR_INVALID_STATE;
    }
    char s0[64] = { 0 };
    char s1[64] = { 0 };
    (void)espos_config_get_str(ESPOS_CFG_NS_TIME, ESPOS_CFG_TIME_SERVER0, s0, sizeof(s0), NULL);
    (void)espos_config_get_str(ESPOS_CFG_NS_TIME, ESPOS_CFG_TIME_SERVER1, s1, sizeof(s1), NULL);
    bool from_dhcp = true;
    (void)espos_config_get_bool(ESPOS_CFG_NS_TIME, ESPOS_CFG_TIME_FROM_DHCP, &from_dhcp);

    lock();
    bool synced = espos_time_policy_is_synced(&s.policy);
    espos_time_src_t src = s.policy.src;
    int64_t now = espos_time_policy_now_ms(&s.policy);
    bool sntp_on = s.sntp_enabled;
    bool sntp_running = s.sntp_started;
    char tz[ESPOS_TIME_TZ_MAX];
    snprintf(tz, sizeof(tz), "%s", s.tz[0] ? s.tz : "UTC0");
    unlock();

    char iso[ESPOS_TIME_ISO_MAX];
    espos_time_iso8601_format(now, iso, sizeof(iso));

    /* Hand-built rather than cJSON: the document is fixed-shape and the only
     * free text in it (the server names and the TZ string) is validated by the
     * config descriptor's maxLength and pattern before it can get here. */
    size_t need = 256 + sizeof(iso) + sizeof(s0) + sizeof(s1) + sizeof(tz);
    char *j = malloc(need);
    if (!j) {
        return ESP_ERR_NO_MEM;
    }
    int n = snprintf(j, need,
                     "{\"synced\":%s,\"source\":\"%s\",\"now\":%lld,\"iso\":\"%s\",\"tz\":\"%s\","
                     "\"sntp\":{\"enabled\":%s,\"running\":%s,\"from_dhcp\":%s,\"servers\":[",
                     synced ? "true" : "false", espos_time_src_str(src), (long long)now, iso, tz,
                     sntp_on ? "true" : "false", sntp_running ? "true" : "false", from_dhcp ? "true" : "false");
    bool first = true;
    if (s0[0]) {
        n += snprintf(j + n, need - (size_t)n, "\"%s\"", s0);
        first = false;
    }
    if (s1[0]) {
        n += snprintf(j + n, need - (size_t)n, "%s\"%s\"", first ? "" : ",", s1);
    }
    snprintf(j + n, need - (size_t)n, "]}}");
    *out_json = j;
    return ESP_OK;
}

/* ------------------------------------------------------------- lifecycle */

esp_err_t espos_time_register_api(void); /* api_time.c */

esp_err_t espos_time_start(void)
{
    if (s.started) {
        return ESP_OK;
    }
    if (!espos_config_is_ready()) {
        ESP_LOGE(TAG, "espos_time_start: call espos_init() first (or espos_start())");
        return ESP_ERR_INVALID_STATE;
    }
    if (!espos_httpd_handle()) {
        ESP_LOGE(TAG, "espos_time_start: call espos_httpd_start() first (or espos_start())");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s.lock) {
        s.lock = xSemaphoreCreateMutex();
        if (!s.lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    bool sntp_on = true;
    (void)espos_config_get_bool(ESPOS_CFG_NS_TIME, ESPOS_CFG_TIME_SNTP, &sntp_on);
    char tz[ESPOS_TIME_TZ_MAX] = { 0 };
    (void)espos_config_get_str(ESPOS_CFG_NS_TIME, ESPOS_CFG_TIME_TZ, tz, sizeof(tz), NULL);

    lock();
    espos_time_policy_init(&s.policy, &POLICY_PORT, NULL, CONFIG_ESPOS_TIME_RTC_STALE_H);
    s.sntp_enabled = sntp_on;
    snprintf(s.tz, sizeof(s.tz), "%s", tz[0] ? tz : "UTC0");
    s.started = true;
    unlock();
    espos_time_port_set_tz(s.tz);

    /* A time carried through a deep sleep, before anything else can set the
     * clock: it is the lowest-ranked source, so any live source replaces it
     * the moment one speaks. */
    int64_t rtc_ms = 0, age_ms = 0;
    espos_time_src_t rtc_src = ESPOS_TIME_SRC_RTC;
    if (espos_time_port_rtc_take(&rtc_ms, &rtc_src, &age_ms)) {
        if (set_internal(rtc_ms, ESPOS_TIME_SRC_RTC, age_ms) == ESP_OK) {
            char iso[ESPOS_TIME_ISO_MAX];
            espos_time_iso8601_format(rtc_ms, iso, sizeof(iso));
            ESP_LOGI(TAG, "clock restored from deep sleep: %s (%lld s old)", iso, (long long)(age_ms / 1000));
        }
    }

    esp_err_t err = espos_time_register_api();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "/time endpoints unavailable: %s", esp_err_to_name(err));
    }
    (void)espos_config_subscribe(on_config_change, NULL);

    if (sntp_on) {
        char server0[64] = { 0 };
        char server1[64] = { 0 };
        bool from_dhcp = true;
        (void)espos_config_get_str(ESPOS_CFG_NS_TIME, ESPOS_CFG_TIME_SERVER0, server0, sizeof(server0), NULL);
        (void)espos_config_get_str(ESPOS_CFG_NS_TIME, ESPOS_CFG_TIME_SERVER1, server1, sizeof(server1), NULL);
        (void)espos_config_get_bool(ESPOS_CFG_NS_TIME, ESPOS_CFG_TIME_FROM_DHCP, &from_dhcp);
        err = espos_time_port_sntp_init(server0, server1, from_dhcp, on_sntp);
        if (err == ESP_OK) {
            lock();
            s.sntp_armed = true;
            unlock();
            /* Polling starts on the first NETWORK_UP, not here: a request sent
             * before there is a route is a wasted retry cycle. espos_net posts
             * the event whichever transport carries the link. */
            if (espos_event_subscribe(ESPOS_EVENT_NETWORK_UP, on_network, NULL) != ESP_OK) {
                ESP_LOGW(TAG, "no NETWORK_UP subscription: SNTP will not start");
            }
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGD(TAG, "no SNTP client in this build");
        } else {
            ESP_LOGW(TAG, "SNTP init: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "SNTP disabled (time.sntp); the clock comes from SignalK or a manual set");
    }
    return ESP_OK;
}

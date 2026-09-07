/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * The SignalK clock fallback: while the device does not know what time it is,
 * take it from navigation.datetime on the stream.
 *
 * A boat's SignalK server usually has GPS time whether or not the boat has an
 * internet route, and a device that can reach the server can therefore learn
 * the time even where NTP never answers — an anchorage with no uplink, a
 * network whose gateway blocks port 123. That is the common case at sea, not
 * an edge case, which is why this is on by default.
 *
 * It lives here rather than in espos_time because the dependency only runs one
 * way: espos_sk already depends on espos_time (the delta engine's clock), so
 * espos_time subscribing to a SignalK path would close a cycle. espos_time
 * offers espos_time_set() and this file calls it — the source knows the clock,
 * the clock knows nothing about its sources.
 *
 * The subscription is dropped as soon as a better source succeeds. SNTP
 * outranks SK in espos_time's ranking anyway, so leaving it would be harmless;
 * dropping it also stops the server sending a value the device would only
 * throw away.
 */
#include <string.h>

#include "esp_log.h"

#include "espos_cfg_keys.h"
#include "espos_config.h"
#include "espos_event.h"
#include "espos_sk.h"
#include "espos_time.h"
#include "espos_time_policy.h"

static const char *TAG = "espos_sk";

static int s_handle;      /* > 0 while subscribed */
static bool s_registered; /* the TIME_SYNCED handler is installed */

static void unsubscribe(void)
{
    int h = s_handle;
    if (h > 0) {
        s_handle = 0;
        (void)espos_sk_unsubscribe(h);
    }
}

/* Runs on the stream task. Strings are valid for the call only. */
static void on_datetime(const espos_sk_update_t *u, void *arg)
{
    (void)arg;
    if (!u->value_json || !*u->value_json) {
        return;
    }
    /* The value arrives as a JSON string, quotes included: "2026-09-07T…Z".
     * Strip them rather than parse the JSON — the whole value is one scalar
     * and a parser here would cost more than it explains. */
    const char *v = u->value_json;
    char iso[40];
    if (*v == '"') {
        const char *end = strrchr(v, '"');
        if (end == v || (size_t)(end - v - 1) >= sizeof(iso)) {
            return;
        }
        size_t n = (size_t)(end - v - 1);
        memcpy(iso, v + 1, n);
        iso[n] = '\0';
    } else {
        if (strlen(v) >= sizeof(iso)) {
            return;
        }
        snprintf(iso, sizeof(iso), "%s", v);
    }
    int64_t ms = espos_time_iso8601_parse(iso);
    if (ms <= 0) {
        ESP_LOGD(TAG, "navigation.datetime not understood: %s", iso);
        return;
    }
    /* espos_time refuses this if a higher-ranked source got there first, which
     * is exactly the wanted behaviour and needs no check here. */
    esp_err_t err = espos_time_set(ms, ESPOS_TIME_SRC_SK);
    if (err == ESP_ERR_INVALID_STATE) {
        /* A better source has the clock: stop asking the server for one. */
        unsubscribe();
    } else if (err == ESP_OK) {
        ESP_LOGI(TAG, "clock from the SignalK server: %s", iso);
    }
}

/* The clock was set by something. If it was not us, we are done. */
static void on_time_synced(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    const espos_event_time_t *ev = data;
    if (ev && ev->source != (uint8_t)ESPOS_TIME_SRC_SK) {
        unsubscribe();
    }
}

void espos_sk_time_start(void)
{
    bool enabled = true;
    (void)espos_config_get_bool(ESPOS_CFG_NS_TIME, ESPOS_CFG_TIME_SK_FALLBACK, &enabled);
    if (!enabled) {
        return;
    }
    if (espos_time_is_synced()) {
        return; /* something already knows the time; nothing to fall back to */
    }
    if (s_handle > 0) {
        return;
    }
    /* 60 s: the date does not change fast, and a device that has gone this
     * long without a clock is not in a hurry for the millisecond. The first
     * value arrives with the subscription's initial state, so the wait is
     * one round trip, not a minute. */
    int h = espos_sk_subscribe("navigation.datetime", 60000, on_datetime, NULL);
    if (h <= 0) {
        ESP_LOGW(TAG, "no subscription slot for the clock fallback");
        return;
    }
    s_handle = h;
    if (!s_registered) {
        if (espos_event_subscribe(ESPOS_EVENT_TIME_SYNCED, on_time_synced, NULL) == ESP_OK) {
            s_registered = true;
        }
    }
    ESP_LOGI(TAG, "clock unset: following navigation.datetime until a better source appears");
}

void espos_sk_time_stop(void)
{
    unsubscribe();
    if (s_registered) {
        (void)espos_event_unsubscribe(ESPOS_EVENT_TIME_SYNCED, on_time_synced);
        s_registered = false;
    }
}

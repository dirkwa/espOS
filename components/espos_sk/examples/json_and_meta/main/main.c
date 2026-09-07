/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * json_and_meta: the three publish calls a plain number does not cover -- a
 * JSON object as a value, metadata for a path only this device knows, and a
 * notification for a condition of the device itself. Framed as a windlass
 * controller: where the anchor went down, how much rode is out, whether the
 * motor is overloaded.
 */
#include <stdbool.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "espos.h"
#include "espos_sk.h"

#define PERIOD_MS     5000
#define POSITION_PATH "navigation.anchor.position" /* spec path with an object value {latitude, longitude} */
#define RODE_PATH     "navigation.anchor.rodeDeployed" /* NOT in the spec: ours, so its meta is ours to declare */
#define OVERLOAD_KEY  "windlassOverload"            /* becomes notifications.espos.<hostname>.windlassOverload */

static const char *TAG = "json_and_meta";

/*
 * WHO OWNS META. Metadata -- units, description, zones, display hints --
 * belongs to whoever defines the path.
 *
 *  - Spec paths (POSITION_PATH): the server ships their meta and the user
 *    edits it there. A device declaring meta for one would overwrite that,
 *    so espos_sk_declare_meta() is never called for a spec path.
 *  - Custom paths (RODE_PATH): nothing else knows them. Declare the FULL
 *    object, units and description at least: what is PUT is the whole meta,
 *    not a merge, so {"units":"m"} alone leaves a path without a description
 *    in every dashboard.
 *  - period_ms adds "timeout" (2.5 x the period, in seconds), the one field
 *    only the device can know, since only it knows how often it publishes.
 *
 * Reconciled on every (re)connect: GET first, PUT only when the server has
 * none. An edit a user made on the server therefore always wins.
 */
static void declare_meta(void)
{
    ESP_ERROR_CHECK(espos_sk_declare_meta(RODE_PATH,
                                          "{\"units\":\"m\",\"description\":\"Anchor rode paid out, counted at the gypsy\"}",
                                          PERIOD_MS));
}

void app_main(void)
{
    ESP_ERROR_CHECK(espos_start(NULL));
    declare_meta(); /* before the first publish, so the first reconcile finds it */
    /* Constants standing in for the fix a real controller copies from the
     * boat's navigation.position the moment the chain starts running out
     * (espos_sk_get_value() from a worker task, never from a callback). */
    const double lat = 54.3233, lon = 10.1394;
    double rode = 0, step = 5; /* "paying out" 5 m per tick to 40 m, then hauling back in */
    for (;; vTaskDelay(pdMS_TO_TICKS(PERIOD_MS))) {
        /* Any complete JSON value is a value; an object here. The text goes
         * out verbatim, so build it yourself and keep it well-formed. */
        char pos[96];
        snprintf(pos, sizeof(pos), "{\"latitude\":%.6f,\"longitude\":%.6f}", lat, lon);
        espos_sk_publish_json(POSITION_PATH, pos);
        rode += step;
        if (rode >= 40 || rode <= 0) step = -step;
        espos_sk_publish_number(RODE_PATH, rode);
        /* A device condition. Level-triggered and idempotent: raise it on every
         * tick, only a change goes on the wire. The motor "overloads" while the
         * rode is between 20 and 30 m; NORMAL with an empty message clears it. */
        bool overload = rode >= 20 && rode < 30;
        espos_sk_notify(OVERLOAD_KEY, overload ? ESPOS_SK_ALERT_WARN : ESPOS_SK_ALERT_NORMAL,
                        overload ? "motor current above limit" : "");
        ESP_LOGI(TAG, "position %s, rode %.0f m, overload %s", pos, rode, overload ? "yes" : "no");
    }
}

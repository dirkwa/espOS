/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * /api/v1/time — read the clock, or set it by hand.
 *
 * Both are protected: espos_httpd_register() puts every endpoint behind the
 * authentication trampoline, and a device whose clock anyone on the network
 * could move is a device whose certificate checks and timestamps anyone can
 * invalidate.
 */
#include <stdlib.h>

#include "cJSON.h"

#include "espos_httpd.h"
#include "espos_time.h"

static esp_err_t time_get(httpd_req_t *req)
{
    char *json = NULL;
    esp_err_t err = espos_time_status_json(&json);
    if (err != ESP_OK || !json) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "status_failed", esp_err_to_name(err));
    }
    err = espos_httpd_send_json(req, NULL, json);
    free(json);
    return err;
}

/* PUT {"now": <unix ms>} and optionally {"tz": "<POSIX TZ>"}. A manual set
 * outranks the SignalK stream and an RTC carry, so an operator can correct a
 * device that picked up a bad time — but not SNTP, which is by definition
 * better than a human typing a number into a form. */
static esp_err_t time_put(httpd_req_t *req)
{
    if (!espos_httpd_require_json(req)) {
        return ESP_OK;
    }
    char *body = NULL;
    size_t len = 0;
    if (espos_httpd_read_body(req, &body, &len) != ESP_OK) {
        return ESP_FAIL;
    }
    cJSON *j = cJSON_ParseWithLength(body, len);
    free(body);
    if (!j) {
        return espos_httpd_send_error(req, "400 Bad Request", "bad_request", "not JSON");
    }
    const cJSON *now = cJSON_GetObjectItem(j, "now");
    const cJSON *tz = cJSON_GetObjectItem(j, "tz");
    if (!cJSON_IsNumber(now) && !cJSON_IsString(tz)) {
        cJSON_Delete(j);
        return espos_httpd_send_error(req, "400 Bad Request", "validation", "now (unix ms) or tz");
    }
    if (cJSON_IsString(tz)) {
        esp_err_t err = espos_time_set_tz(tz->valuestring);
        if (err != ESP_OK) {
            cJSON_Delete(j);
            return espos_httpd_send_error(req, "400 Bad Request", "validation", "tz too long");
        }
    }
    if (cJSON_IsNumber(now)) {
        /* valuedouble, not valueint: unix milliseconds passed 2^31 in 1970 and
         * cJSON's int field is 32-bit. A double holds every millisecond
         * exactly until well past the year 275000. */
        int64_t ms = (int64_t)now->valuedouble;
        esp_err_t err = espos_time_set(ms, ESPOS_TIME_SRC_MANUAL);
        cJSON_Delete(j);
        if (err == ESP_ERR_INVALID_ARG) {
            return espos_httpd_send_error(req, "400 Bad Request", "validation", "now must be a positive unix ms");
        }
        if (err == ESP_ERR_INVALID_STATE) {
            return espos_httpd_send_error(req, "409 Conflict", "outranked",
                                          "the clock is already set by a higher-ranked source");
        }
        if (err != ESP_OK) {
            return espos_httpd_send_error(req, "500 Internal Server Error", "set_failed", esp_err_to_name(err));
        }
    } else {
        cJSON_Delete(j);
    }
    return time_get(req);
}

esp_err_t espos_time_register_api(void)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/v1/time", .method = HTTP_GET, .handler = time_get },
        { .uri = "/api/v1/time", .method = HTTP_PUT, .handler = time_put },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = espos_httpd_register(&uris[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * The ESPOS_EVENT base and the two helpers around esp_event's default loop.
 */
#include "freertos/FreeRTOS.h"
#include "esp_event.h"

#include "espos_event.h"

ESP_EVENT_DEFINE_BASE(ESPOS_EVENT);

/* Bounded, not portMAX_DELAY: a full loop queue means the event task is
 * stuck, and a poster blocking on it would turn one stuck task into two. */
#define POST_WAIT_TICKS pdMS_TO_TICKS(50)

/* Whoever comes first creates the default loop; espos_wifi's driver init
 * and esp_http_server both call this too, so "already exists" is the
 * normal case, not an error. */
static esp_err_t ensure_loop(void)
{
    esp_err_t err = esp_event_loop_create_default();
    return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
}

esp_err_t espos_event_post(int32_t id, const void *data, size_t size)
{
    if (id <= 0 || id >= ESPOS_EVENT_MAX || (size && !data)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_loop();
    if (err != ESP_OK) {
        return err;
    }
    /* esp_event itself waits zero ticks when the caller is the loop task. */
    return esp_event_post(ESPOS_EVENT, id, data, size, POST_WAIT_TICKS);
}

esp_err_t espos_event_subscribe(int32_t id, esp_event_handler_t handler, void *arg)
{
    if (!handler) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_loop();
    if (err != ESP_OK) {
        return err;
    }
    return esp_event_handler_register(ESPOS_EVENT, id, handler, arg);
}

esp_err_t espos_event_unsubscribe(int32_t id, esp_event_handler_t handler)
{
    if (!handler) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_event_handler_unregister(ESPOS_EVENT, id, handler);
}

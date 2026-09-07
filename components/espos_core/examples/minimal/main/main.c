/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * minimal — the whole of an espOS application: one call brings the runtime
 * up, then the application publishes. A constant stands in for a sensor
 * read; replace the number and the path and this is a real device.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "espos.h"
#include "espos_sk.h"

static const char *TAG = "minimal";

/* A Signal K path: the server knows its unit (kelvin) and description, so
 * nothing has to be declared. A path of your own would need
 * espos_sk_declare_meta() once, before the first publish. */
#define PATH "environment.inside.temperature"

void app_main(void)
{
    /* log → config → httpd → wifi → sk → ota, in the one order that works.
     * Returns once every component has started its own tasks. The network
     * may still be coming up at that point — the portal, the server search
     * and the access request all happen in the background, narrated on the
     * monitor — and that is fine, see the loop. */
    ESP_ERROR_CHECK(espos_start(NULL));
    ESP_LOGI(TAG, "publishing %s every second", PATH);

    for (;;) {
        /* 293.65 K is 20.5 °C. Thread-safe and never blocks: until the
         * stream is up the value is batched and buffered, and the backlog
         * drains, oldest first, as soon as it is. */
        espos_sk_publish_number(PATH, 293.65);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

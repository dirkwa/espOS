/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * two_phase_boot — espos_init() and espos_start_network() instead of one
 * espos_start(), with the application's own work between them; and the
 * pattern for an espOS call that blocks: hand it to a worker task through a
 * queue, never call it from a callback.
 *
 * Phase one brings up the log ring and the config store; phase two the
 * network stack (httpd → wifi → sk → ota). In between the application owns
 * the CPU and can already read its settings: a display comes up and shows
 * the portal SSID before there is a portal, a peripheral settles before the
 * radio draws its start-up current, a bus is scanned while nothing else runs.
 */
#include <stdint.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "espos.h"
#include "espos_event.h"
#include "espos_sk.h"
#include "espos_sk_http.h"

static const char *TAG = "two_phase";

/* Everything the worker does arrives as one of these. Small, by value: the
 * event handler that posts one must neither allocate nor wait. */
typedef enum {
    JOB_READING,
    JOB_FETCH_POSITION,
} job_kind_t;
typedef struct {
    job_kind_t kind;
    double value;
} job_t;
static QueueHandle_t s_jobs;

/* The one task that talks to the server. Publishing never blocks and could
 * run anywhere; the REST lookup blocks for up to 2 × 6 s, and this task is
 * where that is allowed to happen. */
static void worker(void *arg)
{
    (void)arg;
    for (job_t job;;) {
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) != pdTRUE) continue;
        if (job.kind == JOB_READING) {
            espos_sk_publish_number("environment.outside.pressure", job.value);
            continue;
        }
        /* The stream carries changes only; a value that rarely changes — the
         * position of a moored boat — may not arrive for a long time, and the
         * REST node holds the current reading. One GET after every connect. */
        char *json = NULL;
        esp_err_t err = espos_sk_get_value("navigation.position", &json);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "navigation.position = %s", json);
        } else {
            ESP_LOGW(TAG, "navigation.position: %s", esp_err_to_name(err));
        }
        free(json); /* NULL on error, which free() accepts */
    }
}

/* Runs on the default event loop task, which WIFI_EVENT and IP_EVENT share:
 * a handler that blocks stalls the WiFi driver's own events. So: post a job
 * and return. A full queue drops it — the next connect posts another. */
static void on_stream_connected(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    job_t job = { .kind = JOB_FETCH_POSITION };
    xQueueSend(s_jobs, &job, 0);
}

void app_main(void)
{
    /* Phase one: log ring, config store, ESPOS_EVENT_CONFIG_READY. */
    ESP_ERROR_CHECK(espos_init());

    /* The "expensive local thing" — stand-in for a display or a sensor bus
     * that must exist before the network. Here: a log line and the worker.
     * espos_sk_get_value() wants ~2 KiB of stack on top of the task's own. */
    ESP_LOGI(TAG, "local hardware up (stand-in); starting the network now");
    s_jobs = xQueueCreate(8, sizeof(job_t));
    xTaskCreate(worker, "app_worker", 6144, NULL, 5, NULL);

    /* Subscribed before the network exists, so the very first connect is
     * heard; the event is posted again on every reconnect. */
    ESP_ERROR_CHECK(espos_event_subscribe(ESPOS_EVENT_SK_STREAM_CONNECTED, on_stream_connected, NULL));

    /* Phase two: httpd → wifi → sk → ota. The one-call form of this sequence is
     *     espos_start_opts_t o = ESPOS_START_OPTS_DEFAULT;
     *     o.before_network = local_hardware_up;   // esp_err_t (*)(void *arg)
     *     ESP_ERROR_CHECK(espos_start(&o));
     * which runs the hook at exactly this point. Two calls are for when the
     * work in between is not one function: it spawns tasks, needs app_main's
     * stack, or depends on something else that lives in main. */
    ESP_ERROR_CHECK(espos_start_network());

    /* Readings, one per second, from whatever the local hardware is; through
     * the queue, so one task owns publishing. espos_sk_publish_* buffers while
     * the server is unreachable, but the engine behind it exists only from
     * phase two on (ESP_ERR_INVALID_STATE before), so the readings start here. */
    for (uint32_t n = 0;; n++) {
        job_t job = { .kind = JOB_READING, .value = 101300.0 + (n % 20) * 10.0 }; /* Pa: Signal K is SI */
        xQueueSend(s_jobs, &job, 0);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

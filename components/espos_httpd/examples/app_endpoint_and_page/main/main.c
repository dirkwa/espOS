/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_endpoint_and_page — a firmware's own REST endpoints on the espOS HTTP
 * server, and a live value on the SSE stream the web UI already listens to.
 * The C side is complete on its own (curl is a client too); the page that
 * shows the value is described in the README, "A page in the web UI".
 *
 * Paths under /api/v1/app/ belong to the application; espOS never uses them.
 */
#include <stdatomic.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "espos.h"
#include "espos_httpd.h"
#include "espos_httpd_sse.h"

/* Incremented by the loop, zeroed by a POST on the server task, read by the
 * GET handler: atomic, so an increment and a reset cannot lose each other. */
static atomic_uint s_counter;

static void counter_json(char *buf, size_t n)
{
    snprintf(buf, n, "{\"app\":\"%s\",\"counter\":%u}", espos_app_name(), atomic_load(&s_counter));
}

/* GET /api/v1/app/status. URI handlers run on the esp_http_server task, one
 * task for every request the device gets: build the reply, send it, return.
 * Nothing that waits — not the SignalK HTTP helper, not a slow sensor read. */
static esp_err_t status_get(httpd_req_t *req)
{
    char json[96];
    counter_json(json, sizeof(json));
    return espos_httpd_send_json(req, NULL, json); /* NULL status = 200 OK */
}

/* POST /api/v1/app/reset. A state change, so the content-type guard comes
 * first: it is the whole API's CSRF defence (docs/rest-api.md) — a browser
 * cannot send application/json cross-origin without a preflight the device
 * never grants — and one handler that skips it is the hole in it. */
static esp_err_t reset_post(httpd_req_t *req)
{
    if (!espos_httpd_require_json(req)) {
        return ESP_OK; /* the 415 has been sent */
    }
    atomic_store(&s_counter, 0);
    char json[96];
    counter_json(json, sizeof(json));
    espos_httpd_sse_publish("app.counter", json); /* browsers see the reset now, not at the next tick */
    return espos_httpd_send_json(req, "202 Accepted", "{\"status\":\"reset\"}");
}

/* A browser that opens /api/v1/events after boot gets the current value at
 * once instead of waiting for the next tick — what every core component does
 * for its own event. Runs on the server task, for that one client. */
static void sse_hello(int client, void *arg)
{
    (void)arg;
    char json[96];
    counter_json(json, sizeof(json));
    espos_httpd_sse_send(client, "app.counter", json);
}

void app_main(void)
{
    ESP_ERROR_CHECK(espos_start(NULL));

    /* After espos_start(): there has to be a server to register on
     * (ESP_ERR_INVALID_STATE before). A table and a loop, the way the core
     * components register theirs. */
    static const httpd_uri_t uris[] = {
        { .uri = "/api/v1/app/status", .method = HTTP_GET, .handler = status_get },
        { .uri = "/api/v1/app/reset", .method = HTTP_POST, .handler = reset_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(espos_httpd_register(&uris[i]));
    }
    ESP_ERROR_CHECK(espos_httpd_sse_on_connect(sse_hello, NULL));

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        atomic_fetch_add(&s_counter, 1);
        char json[96];
        counter_json(json, sizeof(json));
        /* To every connected client, from any task; ESP_OK with none. A
         * stalled client costs at most 250 ms before it is dropped. */
        espos_httpd_sse_publish("app.counter", json);
    }
}

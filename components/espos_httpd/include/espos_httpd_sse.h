/*
 * SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * Server-Sent Events at GET /api/v1/events. Components publish named
 * events with a JSON payload; every connected browser/UI receives
 *
 *     event: <name>\n
 *     data: <json>\n\n
 *
 * The stream starts with "retry: 3000". A comment ping goes out every
 * CONFIG_ESPOS_HTTPD_SSE_PING_S seconds so dead sockets are noticed.
 * Publishers may call from any task; sends are serialised and each socket
 * has a 250 ms send timeout, so a stalled client costs a publisher at most
 * that before it is dropped. When all slots are taken the oldest stream is
 * evicted (clients reconnect).
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Broadcast to every connected client. Returns ESP_OK even with 0 clients. */
esp_err_t espos_httpd_sse_publish(const char *event, const char *json);

/** Send to one client (valid inside an on-connect callback). */
esp_err_t espos_httpd_sse_send(int client, const char *event, const char *json);

/** Called (on the server task) each time a client connects, so a component
 * can push its current snapshot to just that client. Small fixed table. */
typedef void (*espos_httpd_sse_connect_cb_t)(int client, void *arg);
esp_err_t espos_httpd_sse_on_connect(espos_httpd_sse_connect_cb_t cb, void *arg);

int espos_httpd_sse_client_count(void);

#ifdef __cplusplus
}
#endif

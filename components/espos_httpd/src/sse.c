/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SSE over esp_http_server: the request is detached with
 * httpd_req_async_handler_begin() so the server task is free again, and the
 * kept copy is written to from whichever task publishes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "espos_httpd.h"
#include "espos_httpd_priv.h"
#include "espos_httpd_sse.h"

static const char *TAG = "espos_sse";

#ifndef CONFIG_ESPOS_HTTPD_SSE_MAX_CLIENTS
#define CONFIG_ESPOS_HTTPD_SSE_MAX_CLIENTS 3
#endif
#ifndef CONFIG_ESPOS_HTTPD_SSE_PING_S
#define CONFIG_ESPOS_HTTPD_SSE_PING_S 15
#endif
#define MAX_CONNECT_CBS 4

typedef struct {
    httpd_req_t *req;   /* async copy, NULL if slot free */
    int fd;
    uint32_t since;     /* admission sequence: evicts the oldest stream when full
                         * (ticks tie for clients admitted within one tick) */
} client_t;

static struct {
    SemaphoreHandle_t lock;
    client_t clients[CONFIG_ESPOS_HTTPD_SSE_MAX_CLIENTS];
    struct { espos_httpd_sse_connect_cb_t cb; void *arg; } on_connect[MAX_CONNECT_CBS];
    TimerHandle_t ping;
    httpd_handle_t server;
    uint32_t admitted;
} s;

static void lock(void)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s.lock);
}

/* Lock held. */
static void drop_locked(int i)
{
    client_t *c = &s.clients[i];
    if (!c->req) {
        return;
    }
    httpd_req_t *req = c->req;
    int fd = c->fd;
    c->req = NULL;
    c->fd = -1;
    /* Shut the socket down (peer sees EOF, our side reads EOF), then hand it
     * back: the server loop finds it readable, recv() returns 0 and it reaps
     * the session cleanly. Not httpd_sess_trigger_close(): that queued close
     * could hit a reused fd belonging to a new connection. */
    shutdown(fd, SHUT_RDWR);
    httpd_req_async_handler_complete(req);
    ESP_LOGD(TAG, "client %d dropped", i);
}

/* Lock held. */
static esp_err_t send_locked(int i, const char *buf, size_t len)
{
    client_t *c = &s.clients[i];
    if (!c->req) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = httpd_resp_send_chunk(c->req, buf, (ssize_t)len);
    if (err != ESP_OK) {
        drop_locked(i);
    }
    return err;
}

static char *format_event(const char *event, const char *json, size_t *len)
{
    size_t n = strlen(event) + strlen(json) + 24;
    char *buf = malloc(n);
    if (!buf) {
        return NULL;
    }
    int w = snprintf(buf, n, "event: %s\ndata: %s\n\n", event, json);
    *len = (size_t)w;
    return buf;
}

esp_err_t espos_httpd_sse_publish(const char *event, const char *json)
{
    if (!s.lock || !event || !json) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = 0;
    char *buf = format_event(event, json, &len);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    lock();
    for (int i = 0; i < CONFIG_ESPOS_HTTPD_SSE_MAX_CLIENTS; i++) {
        if (s.clients[i].req) {
            send_locked(i, buf, len);
        }
    }
    unlock();
    free(buf);
    return ESP_OK;
}

esp_err_t espos_httpd_sse_send(int client, const char *event, const char *json)
{
    if (!s.lock || client < 0 || client >= CONFIG_ESPOS_HTTPD_SSE_MAX_CLIENTS || !event || !json) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = 0;
    char *buf = format_event(event, json, &len);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    lock();
    esp_err_t err = send_locked(client, buf, len);
    unlock();
    free(buf);
    return err;
}

esp_err_t espos_httpd_sse_on_connect(espos_httpd_sse_connect_cb_t cb, void *arg)
{
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < MAX_CONNECT_CBS; i++) {
        if (!s.on_connect[i].cb) {
            s.on_connect[i].cb = cb;
            s.on_connect[i].arg = arg;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

int espos_httpd_sse_client_count(void)
{
    if (!s.lock) {
        return 0;
    }
    int n = 0;
    lock();
    for (int i = 0; i < CONFIG_ESPOS_HTTPD_SSE_MAX_CLIENTS; i++) {
        n += s.clients[i].req != NULL;
    }
    unlock();
    return n;
}

static void ping_cb(TimerHandle_t t)
{
    (void)t;
    static const char ping[] = ": ping\n\n";
    lock();
    for (int i = 0; i < CONFIG_ESPOS_HTTPD_SSE_MAX_CLIENTS; i++) {
        if (s.clients[i].req) {
            send_locked(i, ping, sizeof(ping) - 1);
        }
    }
    unlock();
}

static esp_err_t events_get(httpd_req_t *req)
{
    httpd_req_t *copy = NULL;
    lock();
    int slot = -1, oldest = 0;
    for (int i = 0; i < CONFIG_ESPOS_HTTPD_SSE_MAX_CLIENTS; i++) {
        if (!s.clients[i].req) {
            slot = i;
            break;
        }
        if ((int32_t)(s.clients[i].since - s.clients[oldest].since) < 0) {
            oldest = i;
        }
    }
    if (slot < 0) {
        /* Full — most likely stale tabs whose close we have not seen yet
         * (async sockets sit outside select()). Evict the oldest stream;
         * a live client simply reconnects (retry: 3000). */
        ESP_LOGI(TAG, "stream table full, evicting client %d", oldest);
        drop_locked(oldest);
        slot = oldest;
    }
    esp_err_t err = httpd_req_async_handler_begin(req, &copy);
    if (err != ESP_OK) {
        unlock();
        return espos_httpd_send_error(req, "500 Internal Server Error", "async_failed", esp_err_to_name(err));
    }
    /* Bound how long a slow/absent peer can stall a publisher: sends on
     * this socket give up after 250 ms and the client is dropped. */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 250 * 1000 };
    setsockopt(httpd_req_to_sockfd(copy), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    httpd_resp_set_type(copy, "text/event-stream");
    httpd_resp_set_hdr(copy, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(copy, "Connection", "keep-alive");
    httpd_resp_set_hdr(copy, "X-Accel-Buffering", "no");
    static const char hello[] = "retry: 3000\n\n";
    err = httpd_resp_send_chunk(copy, hello, sizeof(hello) - 1); /* sends the headers too */
    if (err != ESP_OK) {
        httpd_req_async_handler_complete(copy);
        unlock();
        return ESP_FAIL;
    }
    s.clients[slot].req = copy;
    s.clients[slot].fd = httpd_req_to_sockfd(copy);
    s.clients[slot].since = ++s.admitted;
    unlock();
    ESP_LOGI(TAG, "client %d connected (fd %d)", slot, s.clients[slot].fd);
    for (int i = 0; i < MAX_CONNECT_CBS; i++) {
        if (s.on_connect[i].cb) {
            s.on_connect[i].cb(slot, s.on_connect[i].arg);
        }
    }
    return ESP_OK;
}

esp_err_t espos_httpd_register_sse(httpd_handle_t h)
{
    if (!s.lock) {
        s.lock = xSemaphoreCreateMutex();
        if (!s.lock) {
            return ESP_ERR_NO_MEM;
        }
        for (int i = 0; i < CONFIG_ESPOS_HTTPD_SSE_MAX_CLIENTS; i++) {
            s.clients[i].fd = -1;
        }
    }
    s.server = h;
    if (!s.ping) {
        s.ping = xTimerCreate("sse_ping", pdMS_TO_TICKS(CONFIG_ESPOS_HTTPD_SSE_PING_S * 1000), pdTRUE, NULL, ping_cb);
        if (!s.ping) {
            return ESP_ERR_NO_MEM;
        }
    }
    xTimerStart(s.ping, 0);
    static const httpd_uri_t uri = { .uri = "/api/v1/events", .method = HTTP_GET, .handler = events_get };
    return httpd_register_uri_handler(h, &uri);
}

void espos_httpd_sse_shutdown(void)
{
    if (!s.lock) {
        return;
    }
    if (s.ping) {
        xTimerStop(s.ping, 0);
    }
    lock();
    for (int i = 0; i < CONFIG_ESPOS_HTTPD_SSE_MAX_CLIENTS; i++) {
        drop_locked(i);
    }
    s.server = NULL;
    unlock();
}

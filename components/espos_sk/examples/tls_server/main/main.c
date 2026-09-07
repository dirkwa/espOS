/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * tls_server: talk to the SignalK server over https/wss. The application code
 * is the same as over plain http -- the scheme is a property of the selected
 * server, not of any call -- so what this example shows is the scheme being
 * decided and one diagnostic GET that says whether the certificate was
 * accepted.
 *
 * Nothing is seeded here any more. sk.scheme defaults to "auto", which reads
 * the scheme off the server's own mDNS advertisement (signalk-server publishes
 * _signalk-https._tcp when its ssl setting is on) or, for a manual host, off
 * one redirect probe. And sk.tls_trust defaults to "tofu", so the first
 * connection pins whatever the server presents rather than refusing the
 * self-signed certificate a boat server normally has. Forcing sk.scheme to
 * "https" would only make the device refuse to fall back on a server that has
 * since turned SSL off -- occasionally what you want, and then it is one
 * setting in the web UI, not something an example should decide for you.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "espos.h"
#include "espos_sk.h"
#include "espos_sk_http.h"
#include "espos_wifi.h"

static const char *TAG = "tls_server";

/* One GET as the TLS smoke test. Blocking, so from this task and never from a
 * callback. The URL says which scheme was used; a status means the handshake
 * passed. A refusal is now told apart from an unreachable server: r.cert_error
 * means the certificate is not the one this device trusts, which is a thing an
 * operator can act on (GET /api/v1/sk/tls compares it against the pinned one,
 * DELETE accepts the new one), not merely "the network is down". */
static void probe(const espos_sk_server_t *srv)
{
    char url[ESPOS_SK_URL_MAX];
    espos_sk_url("/signalk/v1/api", url, sizeof(url));
    espos_sk_http_resp_t r;
    esp_err_t err = espos_sk_http_get("/signalk/v1/api", NULL, &r);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "GET %s -> HTTP %d, %u bytes", url, r.status, (unsigned)r.len);
    } else if (r.cert_error) {
        ESP_LOGW(TAG, "GET %s: %s -- see GET /api/v1/sk/tls and the README", url, r.cert_reason);
    } else {
        ESP_LOGW(TAG, "GET %s failed: %s", url, esp_err_to_name(err));
    }
    espos_sk_http_resp_free(&r);
}

void app_main(void)
{
    ESP_ERROR_CHECK(espos_start(NULL));

    char hb[64];
    snprintf(hb, sizeof(hb), "espos.%s.heartbeat", espos_wifi_short_id());
    espos_sk_declare_meta(hb, "{\"description\":\"tls_server example heartbeat\"}", 1000); /* custom path: ours to describe */
    espos_sk_server_t srv, last = { 0 };
    for (uint32_t n = 0;; n++, vTaskDelay(pdMS_TO_TICKS(1000))) {
        espos_sk_publish_number(hb, n); /* buffered until the wss stream is up, then drained in order */
        if (espos_sk_get_server(&srv) != ESP_OK) continue;
        if (strcmp(srv.host, last.host) == 0 && srv.port == last.port && srv.tls == last.tls) continue;
        last = srv; /* a new server, or a new scheme for the same one: probe it once */
        ESP_LOGI(TAG, "server %s:%u over %s", srv.host, srv.port, srv.tls ? "https/wss" : "http/ws");
        probe(&srv);
    }
}

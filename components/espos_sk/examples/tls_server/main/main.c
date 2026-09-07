/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * tls_server: talk to the SignalK server over https/wss. The application code
 * is the same as over plain http -- the scheme is a property of the selected
 * server, not of any call -- so what this example adds is seeding the sk.tls
 * setting on first boot and one diagnostic GET that shows whether the
 * certificate chain verified. CONFIG_ESPOS_SK_TLS=y in sdkconfig.defaults
 * compiles the transports; without it sk.tls is inert and the device says so.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "espos.h"
#include "espos_cfg_keys.h"
#include "espos_config.h"
#include "espos_sk.h"
#include "espos_sk_http.h"
#include "espos_wifi.h"

static const char *TAG = "tls_server";

/* One GET as the TLS smoke test. Blocking, so from this task and never from a
 * callback. The URL says which scheme was used; a status means the handshake
 * passed, a transport error means it did not -- a certificate the bundle
 * cannot verify (self-signed, private CA) never gets as far as a status. */
static void probe(const espos_sk_server_t *srv)
{
    char url[ESPOS_SK_URL_MAX];
    espos_sk_url("/signalk/v1/api", url, sizeof(url));
    espos_sk_http_resp_t r;
    esp_err_t err = espos_sk_http_get("/signalk/v1/api", NULL, &r);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "GET %s -> HTTP %d, %u bytes", url, r.status, (unsigned)r.len);
    } else {
        ESP_LOGW(TAG, "GET %s failed: %s%s", url, esp_err_to_name(err), srv->tls ? " (self-signed certificate? see the README)" : "");
    }
    espos_sk_http_resp_free(&r);
}

void app_main(void)
{
    /* Two-phase boot on purpose: sk.tls is read when espos_sk starts and is
     * restart_required afterwards, so it has to be in the store before the
     * network half comes up. Seeded only while UNSET -- a user turning it
     * off in the web UI stays off. This is a default, not a policy. */
    ESP_ERROR_CHECK(espos_init());
    if (!espos_config_is_set(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_TLS)) {
        ESP_ERROR_CHECK(espos_config_set_bool(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_TLS, true));
        ESP_LOGI(TAG, "first boot: sk.tls seeded to true");
    }
    ESP_ERROR_CHECK(espos_start(NULL)); /* espos_init() already ran; this is the rest, in order */

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

/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_net core: the default-route machine behind a mutex, the device's
 * identity (base MAC, short id, hostname), the registered netifs, the
 * subscriber table, and the fan-out of a route change to ESPOS_EVENT,
 * subscribers and the "net" SSE event. Platform specifics live in the port.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "esp_log.h"

#include "espos_cfg_keys.h"
#include "espos_config.h"
#include "espos_event.h"
#include "espos_httpd.h"
#include "espos_httpd_sse.h"
#include "espos_mdns.h"
#include "espos_net.h"
#include "espos_net_sm.h"
#include "net_port.h"

static const char *TAG = "espos_net";

#define SUBSCRIBERS_MAX 8

/* Where the hostname lived until 0.7. Spelled as strings, not as generated
 * ESPOS_CFG_* constants: espos_net has to build without espos_wifi, and then
 * no descriptor declares the namespace — espos_config answers NOT_FOUND and
 * the migration below is a no-op. */
#define LEGACY_HOSTNAME_NS  "wifi"
#define LEGACY_HOSTNAME_KEY "hostname"

typedef struct {
    espos_net_cb_t cb;
    void *arg;
} subscriber_t;

static struct {
    SemaphoreHandle_t lock;
    const espos_net_port_t *port;
    espos_net_sm_t sm;
    bool started;
    bool api_registered;
    uint8_t mac[6];
    char short_id[ESPOS_NET_SHORT_ID_MAX];
    char hostname[ESPOS_NET_HOSTNAME_MAX];
    char ip6_ll[ESPOS_NET_IP6_MAX];
    void *netif[ESPOS_NET_IF_MAX]; /* esp_netif_t *, opaque; NULL until a transport registers */
    subscriber_t subs[SUBSCRIBERS_MAX];
} s;

/* Created on first use rather than in espos_net_start(): espos_net_subscribe()
 * and espos_net_register_if() are callable before it, so no single owner runs
 * first. The same shape as mdns.c's lock. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void lock(void)
{
    if (!s.lock) {
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        portENTER_CRITICAL(&s_mux);
        if (!s.lock) {
            s.lock = m;
            m = NULL;
        }
        portEXIT_CRITICAL(&s_mux);
        if (m) {
            vSemaphoreDelete(m);
        }
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s.lock);
}

static uint32_t sm_now(void *ctx)
{
    (void)ctx;
    return s.port->now_ms();
}

static const espos_net_sm_port_t k_sm_port = { .now_ms = sm_now };

/* ------------------------------------------------------------- identity */

const char *espos_net_short_id(void)
{
    return s.short_id;
}

/* net.hostname, with the one-time move of a 0.7 wifi.hostname. The old key is
 * retired whatever happens, so the settings page shows one hostname and a
 * value written there by an old script cannot linger. */
static void load_hostname(void)
{
    if (!espos_config_is_set(ESPOS_CFG_NS_NET, ESPOS_CFG_NET_HOSTNAME)) {
        char old[ESPOS_NET_HOSTNAME_MAX] = { 0 };
        if (espos_config_get_str(LEGACY_HOSTNAME_NS, LEGACY_HOSTNAME_KEY, old, sizeof(old), NULL) == ESP_OK && old[0]) {
            if (espos_config_set_str(ESPOS_CFG_NS_NET, ESPOS_CFG_NET_HOSTNAME, old) == ESP_OK) {
                ESP_LOGI(TAG, "hostname \"%s\" moved from wifi.hostname to net.hostname", old);
            } else {
                ESP_LOGW(TAG, "could not move wifi.hostname \"%s\" to net.hostname", old);
            }
        }
    }
    if (espos_config_is_set(LEGACY_HOSTNAME_NS, LEGACY_HOSTNAME_KEY)) {
        (void)espos_config_reset_key(LEGACY_HOSTNAME_NS, LEGACY_HOSTNAME_KEY);
    }
    char h[ESPOS_NET_HOSTNAME_MAX] = { 0 };
    espos_config_get_str(ESPOS_CFG_NS_NET, ESPOS_CFG_NET_HOSTNAME, h, sizeof(h), NULL);
    lock();
    if (h[0]) {
        strcpy(s.hostname, h);
    } else {
        snprintf(s.hostname, sizeof(s.hostname), "espos-%s", s.short_id);
    }
    unlock();
}

/* -------------------------------------------------------------- status */

/* Lock held. */
static void snapshot_locked(espos_net_status_t *out)
{
    espos_net_sm_status(&s.sm, out);
    memcpy(out->mac, s.mac, sizeof(out->mac));
    strcpy(out->hostname, s.hostname);
    if (out->up) {
        strcpy(out->ip6_ll, s.ip6_ll);
    }
}

esp_err_t espos_net_get_status(espos_net_status_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s.started) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    snapshot_locked(out);
    unlock();
    return ESP_OK;
}

bool espos_net_is_up(void)
{
    if (!s.started) {
        return false;
    }
    lock();
    bool up = s.sm.active != ESPOS_NET_IF_NONE;
    unlock();
    return up;
}

static char *status_to_json(const espos_net_status_t *st)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    char mac[18];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", st->mac[0], st->mac[1], st->mac[2], st->mac[3], st->mac[4], st->mac[5]);
    cJSON_AddBoolToObject(root, "up", st->up);
    cJSON_AddStringToObject(root, "iface", espos_net_if_str(st->iface));
    cJSON_AddStringToObject(root, "ip", st->ip);
    cJSON_AddStringToObject(root, "netmask", st->netmask);
    cJSON_AddStringToObject(root, "gateway", st->gateway);
    cJSON_AddStringToObject(root, "ip6_ll", st->ip6_ll);
    cJSON_AddStringToObject(root, "mac", mac);
    cJSON_AddStringToObject(root, "hostname", st->hostname);
    cJSON_AddStringToObject(root, "id", s.short_id);
    if (st->up && st->iface == ESPOS_NET_IF_WIFI_STA && st->rssi) {
        cJSON_AddNumberToObject(root, "rssi", st->rssi);
    } else {
        cJSON_AddNullToObject(root, "rssi");
    }
    cJSON_AddNumberToObject(root, "up_count", st->up_count);
    cJSON_AddNumberToObject(root, "up_s", st->up_since_ms / 1000);
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return txt;
}

esp_err_t espos_net_status_json(char **out_json)
{
    if (!out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;
    espos_net_status_t st;
    esp_err_t err = espos_net_get_status(&st);
    if (err != ESP_OK) {
        return err;
    }
    *out_json = status_to_json(&st);
    return *out_json ? ESP_OK : ESP_ERR_NO_MEM;
}

static void sse_hello(int client, void *arg)
{
    (void)arg;
    char *json = NULL;
    if (espos_net_status_json(&json) == ESP_OK) {
        espos_httpd_sse_send(client, "net", json);
        free(json);
    }
}

/* --------------------------------------------------------- subscribers */

esp_err_t espos_net_subscribe(espos_net_cb_t cb, void *arg)
{
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    subscriber_t *free_slot = NULL;
    for (size_t i = 0; i < SUBSCRIBERS_MAX; i++) {
        if (s.subs[i].cb == cb && s.subs[i].arg == arg) {
            unlock();
            return ESP_OK; /* already there: called once, as the header promises */
        }
        if (!s.subs[i].cb && !free_slot) {
            free_slot = &s.subs[i];
        }
    }
    if (!free_slot) {
        unlock();
        return ESP_ERR_NO_MEM;
    }
    free_slot->cb = cb;
    free_slot->arg = arg;
    unlock();
    return ESP_OK;
}

esp_err_t espos_net_unsubscribe(espos_net_cb_t cb, void *arg)
{
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < SUBSCRIBERS_MAX; i++) {
        if (s.subs[i].cb == cb && s.subs[i].arg == arg) {
            s.subs[i].cb = NULL;
            s.subs[i].arg = NULL;
            err = ESP_OK;
        }
    }
    unlock();
    return err;
}

/* ---------------------------------------------------------- transports */

esp_err_t espos_net_register_if(espos_net_if_t iface, void *esp_netif)
{
    if (iface <= ESPOS_NET_IF_NONE || iface >= ESPOS_NET_IF_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    s.netif[iface] = esp_netif;
    bool apply = s.started && esp_netif != NULL;
    char hostname[ESPOS_NET_HOSTNAME_MAX];
    strcpy(hostname, s.hostname);
    unlock();
    if (apply) {
        /* Before the transport connects, so the DHCP request carries it; a
         * transport registering before espos_net_start() gets it from there. */
        esp_err_t err = s.port->set_hostname(esp_netif, hostname);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "hostname on %s: %s", espos_net_if_str(iface), esp_err_to_name(err));
        }
    }
    return ESP_OK;
}

void espos_net_report(espos_net_if_t iface, bool up, const char *ip, const char *netmask, const char *gateway, int8_t rssi)
{
    if (!s.started) {
        return; /* a transport that reports before the seam exists has nothing to report to */
    }
    espos_net_status_t st;
    subscriber_t subs[SUBSCRIBERS_MAX];
    bool changed = false;
    lock();
    espos_net_edge_t edge = espos_net_sm_report(&s.sm, iface, up, ip, netmask, gateway, rssi, &changed);
    if (edge != ESPOS_NET_EDGE_NONE) {
        /* The netif is host-side (lwIP) on every target; asking it for the
         * link-local address does not touch a radio. Refreshed on edges only:
         * the address is fixed for the life of a link. */
        s.ip6_ll[0] = '\0';
        if (s.sm.active != ESPOS_NET_IF_NONE) {
            s.port->ip6_linklocal(s.netif[s.sm.active], s.ip6_ll);
        }
    }
    if (changed) {
        snapshot_locked(&st);
        memcpy(subs, s.subs, sizeof(subs));
    }
    unlock();
    if (!changed) {
        return;
    }

    /* Outside the lock from here: the event post, the subscribers and the
     * SSE send all leave this component, and a subscriber may well ask
     * espos_net_get_status(). */
    if (edge == ESPOS_NET_EDGE_DOWN || edge == ESPOS_NET_EDGE_CHANGED) {
        ESP_LOGI(TAG, "%s", edge == ESPOS_NET_EDGE_DOWN ? "network down" : "default route moved");
        (void)espos_event_post(ESPOS_EVENT_NETWORK_DOWN, NULL, 0);
    }
    if (edge == ESPOS_NET_EDGE_UP || edge == ESPOS_NET_EDGE_CHANGED) {
        ESP_LOGI(TAG, "network up via %s: %s (gateway %s)", espos_net_if_str(st.iface), st.ip, st.gateway[0] ? st.gateway : "none");
        espos_event_network_t ev = { 0 };
        snprintf(ev.ip, sizeof(ev.ip), "%s", st.ip);
        snprintf(ev.hostname, sizeof(ev.hostname), "%s", st.hostname);
        (void)espos_event_post(ESPOS_EVENT_NETWORK_UP, &ev, sizeof(ev));
    }
    if (edge != ESPOS_NET_EDGE_NONE) {
        for (size_t i = 0; i < SUBSCRIBERS_MAX; i++) {
            if (subs[i].cb) {
                subs[i].cb(&st, subs[i].arg);
            }
        }
    }
    char *json = status_to_json(&st);
    if (json) {
        espos_httpd_sse_publish("net", json);
        free(json);
    }
}

/* ------------------------------------------------------------ lifecycle */

esp_err_t espos_net_start(void)
{
    if (s.started) {
        return ESP_OK;
    }
    if (!espos_config_is_ready()) {
        ESP_LOGE(TAG, "espos_net_start: call espos_init() first (or espos_start())");
        return ESP_ERR_INVALID_STATE;
    }
    /* /net/status and the "net" SSE event live on the HTTP server. */
    if (!espos_httpd_handle()) {
        ESP_LOGE(TAG, "espos_net_start: call espos_httpd_start() first (or espos_start())");
        return ESP_ERR_INVALID_STATE;
    }
    s.port = espos_net_port();
    esp_err_t err = s.port->init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "netif init failed: %s", esp_err_to_name(err));
        return err;
    }
    uint8_t mac[6] = { 0 };
    if (s.port->read_mac(mac) == ESP_OK) {
        memcpy(s.mac, mac, sizeof(s.mac));
        snprintf(s.short_id, sizeof(s.short_id), "%02x%02x", mac[4], mac[5]);
    } else {
        ESP_LOGW(TAG, "no base MAC: device id is 0000");
        memset(s.mac, 0, sizeof(s.mac));
        strcpy(s.short_id, "0000");
    }
    load_hostname();

    lock();
    espos_net_sm_init(&s.sm, &k_sm_port, NULL);
    void *netifs[ESPOS_NET_IF_MAX];
    memcpy(netifs, s.netif, sizeof(netifs));
    char hostname[ESPOS_NET_HOSTNAME_MAX];
    strcpy(hostname, s.hostname);
    unlock();
    /* Interfaces a transport registered before us get the name now. */
    for (int i = ESPOS_NET_IF_NONE + 1; i < ESPOS_NET_IF_MAX; i++) {
        if (netifs[i]) {
            (void)s.port->set_hostname(netifs[i], hostname);
        }
    }

    if (!s.api_registered) {
        /* URI handlers and the SSE hook survive stop(); register once. */
        ESP_ERROR_CHECK(espos_net_register_api());
        espos_httpd_sse_on_connect(sse_hello, NULL);
        s.api_registered = true;
    }
    s.started = true;
    ESP_LOGI(TAG, "hostname %s (id %s)", hostname, s.short_id);
    /* The responder needs the netif layer and event loop the port just made
     * and takes records before any interface has an address (espos_mdns.h).
     * A failure costs discoverability, not the network; NOT_SUPPORTED in
     * builds without it. */
    (void)espos_mdns_start();
    return ESP_OK;
}

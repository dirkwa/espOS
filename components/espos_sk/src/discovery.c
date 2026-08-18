/*
 * SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * mDNS discovery of SignalK servers (_signalk-http._tcp) and advertisement
 * of this device. Device build; the host uses discovery_sim.c.
 */
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "lwip/inet.h"
#include "mdns.h"

#include "espos_sk_priv.h"
#include "espos_wifi.h"

static const char *TAG = "espos_sk";
static bool s_inited;

/* Our own IPv4 + netmask (0 if not connected). */
static void local_net(uint32_t *ip, uint32_t *mask)
{
    *ip = 0;
    *mask = 0;
    espos_wifi_status_t ws;
    if (espos_wifi_get_status(&ws) == ESP_OK && ws.sm.state == ESPOS_WIFI_ST_CONNECTED) {
        *ip = ipaddr_addr(ws.sm.ip.ip);
        *mask = ipaddr_addr(ws.sm.ip.netmask);
    }
}

esp_err_t espos_sk_discovery_init(const char *hostname)
{
    if (!s_inited) {
        esp_err_t err = mdns_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "mdns_init: %s", esp_err_to_name(err));
            return err;
        }
        s_inited = true;
    }
    mdns_hostname_set(hostname);
    mdns_instance_name_set(hostname);
    return ESP_OK;
}

static const char *txt_get(const mdns_result_t *r, const char *key)
{
    for (size_t i = 0; i < r->txt_count; i++) {
        if (r->txt[i].key && strcmp(r->txt[i].key, key) == 0) {
            return r->txt[i].value ? r->txt[i].value : "";
        }
    }
    return "";
}

size_t espos_sk_discovery_run(espos_sk_discovered_t *out, size_t max)
{
    if (!s_inited) {
        return 0;
    }
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr("_signalk-http", "_tcp", 3000, 20, &results);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns query: %s", esp_err_to_name(err));
        return 0;
    }
    uint32_t my_ip, my_mask;
    local_net(&my_ip, &my_mask);
    size_t n = 0;
    for (mdns_result_t *r = results; r && n < max; r = r->next) {
        espos_sk_discovered_t *d = &out[n];
        memset(d, 0, sizeof(*d));
        /* Dual-homed servers advertise several A records; prefer the one on
         * our own subnet, else the first IPv4, else <hostname>.local. */
        bool have_ip = false;
        for (int pass = 0; pass < 2 && !have_ip; pass++) {
            for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
                if (a->addr.type != ESP_IPADDR_TYPE_V4) {
                    continue;
                }
                uint32_t ip = a->addr.u_addr.ip4.addr;
                bool same_net = my_mask && ((ip & my_mask) == (my_ip & my_mask));
                if (pass == 0 && !same_net) {
                    continue;
                }
                snprintf(d->host, sizeof(d->host), IPSTR, IP2STR(&a->addr.u_addr.ip4));
                have_ip = true;
                break;
            }
        }
        if (!have_ip) {
            if (!r->hostname) {
                continue;
            }
            snprintf(d->host, sizeof(d->host), "%s.local", r->hostname);
        }
        d->port = r->port;
        snprintf(d->name, sizeof(d->name), "%s", r->instance_name ? r->instance_name : "");
        snprintf(d->self, sizeof(d->self), "%s", txt_get(r, "self"));
        snprintf(d->roles, sizeof(d->roles), "%s", txt_get(r, "roles"));
        snprintf(d->swname, sizeof(d->swname), "%s", txt_get(r, "swname"));
        snprintf(d->swvers, sizeof(d->swvers), "%s", txt_get(r, "swvers"));
        n++;
    }
    mdns_query_results_free(results);
    return n;
}

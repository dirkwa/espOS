/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * Device port: esp_netif, the default event loop and the eFuse base MAC.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "sdkconfig.h"

#include "net_port.h"

static esp_err_t p_init(void)
{
    /* Both idempotent: the WiFi port and esp_http_server call them too, and
     * whichever runs first does the work. */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return ESP_OK;
}

static esp_err_t p_read_mac(uint8_t mac[6])
{
    /* The base MAC from eFuse, not the station MAC from the driver: it is the
     * same on a WiFi, Ethernet or Thread build, and on the ESP32-P4 it is on
     * this chip rather than on the co-processor. */
    return esp_read_mac(mac, ESP_MAC_BASE);
}

static esp_err_t p_set_hostname(void *netif, const char *name)
{
    return esp_netif_set_hostname((esp_netif_t *)netif, name);
}

static void p_ip6_linklocal(void *netif, char out[ESPOS_NET_IP6_MAX])
{
    out[0] = '\0';
#if CONFIG_LWIP_IPV6
    esp_ip6_addr_t a;
    if (netif && esp_netif_get_ip6_linklocal((esp_netif_t *)netif, &a) == ESP_OK) {
        snprintf(out, ESPOS_NET_IP6_MAX, IPV6STR, IPV62STR(a));
    }
#else
    (void)netif;
#endif
}

static uint32_t p_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static const espos_net_port_t k_port = {
    .init = p_init,
    .read_mac = p_read_mac,
    .set_hostname = p_set_hostname,
    .ip6_linklocal = p_ip6_linklocal,
    .now_ms = p_now_ms,
};

const espos_net_port_t *espos_net_port(void)
{
    return &k_port;
}

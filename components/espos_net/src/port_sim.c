/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host (linux target) port: no netif, a fixed MAC. The MAC is the one the
 * simulated WiFi driver always reported (02:00:00:00:1a:2b), so the host
 * device keeps its id "1a2b" and every name derived from it — the REST
 * harness asserts hostname "espos-1a2b" and source label "espos-1a2b".
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "net_port.h"

static const char *TAG = "espos_net_sim";

static esp_err_t p_init(void)
{
    return ESP_OK;
}

static esp_err_t p_read_mac(uint8_t mac[6])
{
    memcpy(mac, (uint8_t[]) { 0x02, 0x00, 0x00, 0x00, 0x1a, 0x2b }, 6);
    return ESP_OK;
}

static esp_err_t p_set_hostname(void *netif, const char *name)
{
    (void)netif;
    ESP_LOGI(TAG, "hostname %s", name);
    return ESP_OK;
}

static void p_ip6_linklocal(void *netif, char out[ESPOS_NET_IP6_MAX])
{
    (void)netif;
    out[0] = '\0';
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

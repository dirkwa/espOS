/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include "esp_err.h"

#include "espos_net.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What espos_net needs from the platform, implemented by port_idf.c on a
 * device and port_sim.c on the linux target. Nothing here talks to a radio:
 * the netif and the eFuse are host-side on every target, the hosted ESP32-P4
 * included, so none of these calls can hang on a wedged co-processor link. */
typedef struct {
    esp_err_t (*init)(void);                                  /* esp_netif + default event loop, idempotent */
    esp_err_t (*read_mac)(uint8_t mac[6]);                    /* the base MAC */
    esp_err_t (*set_hostname)(void *netif, const char *name); /* on one registered esp_netif */
    void (*ip6_linklocal)(void *netif, char out[ESPOS_NET_IP6_MAX]); /* "" when none */
    uint32_t (*now_ms)(void);
} espos_net_port_t;

const espos_net_port_t *espos_net_port(void); /* the linked port */

/* HTTP endpoints (api_net.c). */
esp_err_t espos_net_register_api(void);

#ifdef __cplusplus
}
#endif

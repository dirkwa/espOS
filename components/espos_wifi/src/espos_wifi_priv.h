/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution */
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "espos_wifi.h"
#include "espos_wifi_sm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Implemented by the driver port (port_idf.c on device, port_sim.c on the
 * host). The port fills the SM callbacks and reports events back through
 * espos_wifi_dispatch(). */
typedef struct {
    esp_err_t (*init)(void);                     /* netif/driver up, not connected */
    esp_err_t (*deinit)(void);
    const espos_wifi_port_t *sm_port;            /* connect/disconnect/portal/timer/now/random */
    int8_t (*rssi)(void);                        /* current STA RSSI, 0 if unknown */
    esp_err_t (*scan_start)(void);               /* results via espos_wifi_scan_done() */
    esp_err_t (*get_mac)(uint8_t mac[6]);
    esp_err_t (*set_hostname)(const char *hostname);
    esp_err_t (*set_ps)(const char *mode);       /* "none" | "min" | "max" */
    const char *portal_ip;                       /* e.g. "192.168.4.1" */
} espos_wifi_driver_t;

const espos_wifi_driver_t *espos_wifi_driver(void);   /* the linked port */

/* Called by the port (any task); serialised internally. */
void espos_wifi_dispatch(espos_wifi_event_t ev, const void *arg);
void espos_wifi_scan_done(const espos_wifi_scan_entry_t *entries, size_t n);
/* Effective portal SSID/PSK for the port to bring the AP up with. */
void espos_wifi_portal_credentials(char ssid[33], char psk[65]);

/* HTTP endpoints (api_wifi.c). */
esp_err_t espos_wifi_register_api(void);

#ifdef __cplusplus
}
#endif

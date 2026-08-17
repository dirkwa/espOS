/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Device port: esp_wifi / esp_netif / esp_event glue for the state
 * machine. On ESP32-P4 the same calls reach the C6 co-processor through
 * esp_wifi_remote; nothing here knows the difference.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#include "espos_wifi_priv.h"

static const char *TAG = "espos_wifi";

#define PORTAL_IP "192.168.4.1"

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static TimerHandle_t s_timer;
static bool s_disconnect_requested;   /* our own esp_wifi_disconnect() is in flight */
static bool s_portal_up;
static int s_portal_clients;
static bool s_inited;

esp_err_t espos_wifi_portal_dns_start(const char *ip);
void espos_wifi_portal_dns_stop(void);

/* ----------------------------------------------------------- events */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    switch (id) {
    case WIFI_EVENT_STA_CONNECTED: {
        const wifi_event_sta_connected_t *e = data;
        espos_wifi_link_t link = { 0 };
        size_t n = e->ssid_len < 32 ? e->ssid_len : 32;
        memcpy(link.ssid, e->ssid, n);
        memcpy(link.bssid, e->bssid, 6);
        link.channel = e->channel;
        espos_wifi_dispatch(ESPOS_WIFI_EV_STA_CONNECTED, &link);
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *e = data;
        int reason = e->reason;
        if (s_disconnect_requested && reason == WIFI_REASON_ASSOC_LEAVE) {
            /* the echo of our own esp_wifi_disconnect(); the SM already moved on */
            s_disconnect_requested = false;
            ESP_LOGD(TAG, "swallowing our own disconnect echo");
            break;
        }
        ESP_LOGW(TAG, "disconnected: %d (%s)", reason, espos_wifi_reason_str(reason));
        espos_wifi_dispatch(ESPOS_WIFI_EV_STA_DISCONNECTED, &reason);
        break;
    }
    case WIFI_EVENT_SCAN_DONE: {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        if (n > 20) {
            n = 20;
        }
        wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
        espos_wifi_scan_entry_t out[20];
        size_t count = 0;
        if (recs && n) {
            uint16_t got = n;
            if (esp_wifi_scan_get_ap_records(&got, recs) == ESP_OK) {
                for (uint16_t i = 0; i < got && count < 20; i++) {
                    espos_wifi_scan_entry_t *o = &out[count++];
                    memset(o, 0, sizeof(*o));
                    strncpy(o->ssid, (const char *)recs[i].ssid, 32);
                    memcpy(o->bssid, recs[i].bssid, 6);
                    o->rssi = recs[i].rssi;
                    o->channel = recs[i].primary;
                    o->authmode = (uint8_t)recs[i].authmode;
                }
            }
        } else {
            esp_wifi_clear_ap_list();
        }
        free(recs);
        espos_wifi_scan_done(out, count);
        break;
    }
    case WIFI_EVENT_AP_STACONNECTED:
        s_portal_clients++;
        espos_wifi_dispatch(ESPOS_WIFI_EV_PORTAL_CLIENT, &s_portal_clients);
        break;
    case WIFI_EVENT_AP_STADISCONNECTED:
        if (s_portal_clients > 0) {
            s_portal_clients--;
        }
        espos_wifi_dispatch(ESPOS_WIFI_EV_PORTAL_CLIENT, &s_portal_clients);
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        espos_wifi_ip_t ip;
        snprintf(ip.ip, sizeof(ip.ip), IPSTR, IP2STR(&e->ip_info.ip));
        snprintf(ip.netmask, sizeof(ip.netmask), IPSTR, IP2STR(&e->ip_info.netmask));
        snprintf(ip.gateway, sizeof(ip.gateway), IPSTR, IP2STR(&e->ip_info.gw));
        ESP_LOGI(TAG, "got ip %s", ip.ip);
        espos_wifi_dispatch(ESPOS_WIFI_EV_GOT_IP, &ip);
    } else if (id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "lost ip");
        espos_wifi_dispatch(ESPOS_WIFI_EV_LOST_IP, NULL);
    }
}

/* ---------------------------------------------------------- SM port */

static esp_err_t p_connect(void *ctx, const espos_wifi_net_t *net)
{
    (void)ctx;
    s_disconnect_requested = false;
    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, net->ssid, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, net->psk, sizeof(cfg.sta.password));
    if (net->has_bssid) {
        cfg.sta.bssid_set = true;
        memcpy(cfg.sta.bssid, net->bssid, 6);
    }
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    /* Refuse to downgrade to open/WEP when a password is configured; WPA3
     * (SAE, H2E) is accepted when the AP offers it. */
    cfg.sta.threshold.authmode = net->psk[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "connecting to '%s'%s", net->ssid, net->has_bssid ? " (pinned BSSID)" : "");
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t p_disconnect(void *ctx)
{
    (void)ctx;
    s_disconnect_requested = true;
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK) {
        s_disconnect_requested = false;
    }
    return ESP_OK;
}

static esp_err_t p_portal_start(void *ctx)
{
    (void)ctx;
    if (s_portal_up) {
        return ESP_OK;
    }
    char ssid[33], psk[65];
    espos_wifi_portal_credentials(ssid, psk);
    wifi_config_t ap = { 0 };
    strncpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = (uint8_t)strlen(ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.beacon_interval = 100; /* explicit: a hosted co-processor does not default 0 → 100 */
    if (psk[0]) {
        strncpy((char *)ap.ap.password, psk, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ap.ap.pmf_cfg.capable = true;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_LOGD(TAG, "portal: switching to APSTA");
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    ESP_LOGD(TAG, "portal: set_mode → %s", esp_err_to_name(err));
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &ap);
        ESP_LOGD(TAG, "portal: set_config → %s", esp_err_to_name(err));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "portal start failed: %s", esp_err_to_name(err));
        esp_wifi_set_mode(WIFI_MODE_STA);
        return err;
    }
    s_portal_clients = 0;
    s_portal_up = true;
    espos_wifi_portal_dns_start(PORTAL_IP);
    ESP_LOGI(TAG, "portal up: SSID '%s' (%s), http://%s/", ssid, psk[0] ? "WPA2" : "open", PORTAL_IP);
    return ESP_OK;
}

static esp_err_t p_portal_stop(void *ctx)
{
    (void)ctx;
    if (!s_portal_up) {
        return ESP_OK;
    }
    espos_wifi_portal_dns_stop();
    s_portal_up = false;
    s_portal_clients = 0;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "portal down");
    return err;
}

static void timer_cb(TimerHandle_t t)
{
    (void)t;
    espos_wifi_dispatch(ESPOS_WIFI_EV_TIMER, NULL);
}

static void p_arm_timer(void *ctx, uint32_t ms)
{
    (void)ctx;
    if (!s_timer) {
        return;
    }
    TickType_t ticks = pdMS_TO_TICKS(ms);
    if (ticks == 0) {
        ticks = 1;
    }
    xTimerChangePeriod(s_timer, ticks, 0); /* also starts it */
}

static void p_cancel_timer(void *ctx)
{
    (void)ctx;
    if (s_timer) {
        xTimerStop(s_timer, 0);
    }
}

static uint32_t p_now_ms(void *ctx)
{
    (void)ctx;
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static uint32_t p_random(void *ctx)
{
    (void)ctx;
    return esp_random();
}

static const espos_wifi_port_t k_sm_port = {
    .connect = p_connect,
    .disconnect = p_disconnect,
    .portal_start = p_portal_start,
    .portal_stop = p_portal_stop,
    .arm_timer = p_arm_timer,
    .cancel_timer = p_cancel_timer,
    .now_ms = p_now_ms,
    .random = p_random,
    .status_changed = NULL, /* filled by the core */
};

/* ---------------------------------------------------------- driver */

static esp_err_t d_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        return err;
    }
    /* Credentials live in espos_config; do not let the driver persist its own copy. */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_ip_event, NULL));
    s_timer = xTimerCreate("espos_wifi", pdMS_TO_TICKS(1000), pdFALSE, NULL, timer_cb);
    if (!s_timer) {
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return err;
    }
    s_inited = true;
    return ESP_OK;
}

static esp_err_t d_deinit(void)
{
    if (!s_inited) {
        return ESP_OK;
    }
    p_portal_stop(NULL);
    esp_wifi_stop();
    s_inited = false;
    return ESP_OK;
}

static int8_t d_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

static esp_err_t d_scan_start(void)
{
    esp_err_t err = esp_wifi_scan_start(NULL, false);
    if (err == ESP_ERR_WIFI_STATE) {
        return ESP_ERR_INVALID_STATE;
    }
    return err;
}

static esp_err_t d_get_mac(uint8_t mac[6])
{
    /* Ask the driver first: on hosted setups (ESP32-P4 + C6) the WiFi MAC
     * lives on the co-processor and esp_read_mac() has nothing to read. */
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        return ESP_OK;
    }
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        return ESP_OK;
    }
    return esp_read_mac(mac, ESP_MAC_BASE);
}

static esp_err_t d_set_hostname(const char *hostname)
{
    esp_err_t err = ESP_OK;
    if (s_sta_netif) {
        err = esp_netif_set_hostname(s_sta_netif, hostname);
    }
    if (s_ap_netif) {
        esp_netif_set_hostname(s_ap_netif, hostname);
    }
    return err;
}

static esp_err_t d_set_ps(const char *mode)
{
    wifi_ps_type_t ps = WIFI_PS_NONE;
    if (strcmp(mode, "min") == 0) {
        ps = WIFI_PS_MIN_MODEM;
    } else if (strcmp(mode, "max") == 0) {
        ps = WIFI_PS_MAX_MODEM;
    }
    return esp_wifi_set_ps(ps);
}

static const espos_wifi_driver_t k_driver = {
    .init = d_init,
    .deinit = d_deinit,
    .sm_port = &k_sm_port,
    .rssi = d_rssi,
    .scan_start = d_scan_start,
    .get_mac = d_get_mac,
    .set_hostname = d_set_hostname,
    .set_ps = d_set_ps,
    .portal_ip = PORTAL_IP,
};

const espos_wifi_driver_t *espos_wifi_driver(void)
{
    return &k_driver;
}

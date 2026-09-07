/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * mDNS responder: <hostname>.local, the built-in _http._tcp / _espos._tcp
 * records, and a small table of application services that may be registered
 * before the responder exists. See include/espos_mdns.h for the contract.
 *
 * The responder (espressif/mdns) is brought up from espos_mdns_start() on the
 * caller's task rather than from the NETWORK_UP handler: mdns_hostname_set()
 * waits for the responder's task and mdns_service_add() takes a lock that
 * task holds while it parses packets, and an ESPOS_EVENT handler must do
 * neither. Nothing is lost by starting early — the responder accepts records
 * before the station has an address and announces them itself on GOT_IP
 * (it registers its own IP_EVENT handler), which is how SignalK discovery
 * has always used it. The event handlers here only track whether there is a
 * link to announce on and post MDNS_READY.
 */
#include "sdkconfig.h"

#include "espos_mdns.h"

#if CONFIG_ESPOS_WIFI_MDNS

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "mdns.h"

#include "espos_cfg_keys.h"
#include "espos_config.h"
#include "espos_event.h"
#include "espos_wifi.h"

static const char *TAG = "espos_mdns";

/* Set by CMakeLists.txt from what IDF knows about the application and from
 * espOS's version.txt (manifest version in a registry copy); the fallbacks
 * only exist so a build that lost the definitions still says something. */
#ifndef ESPOS_MDNS_PROJECT_NAME
#define ESPOS_MDNS_PROJECT_NAME "?"
#endif
#ifndef ESPOS_MDNS_PROJECT_VER
#define ESPOS_MDNS_PROJECT_VER "?"
#endif
#ifndef ESPOS_MDNS_ESPOS_VERSION
#define ESPOS_MDNS_ESPOS_VERSION "0.0.0-unknown"
#endif

/* One application service. TXT items are packed "key\0value\0key\0value\0",
 * n_txt pairs, so the entry is a single copy with no pointers to fix up. */
typedef struct {
    bool used;
    char type[ESPOS_MDNS_TYPE_MAX];
    char proto[ESPOS_MDNS_PROTO_MAX];
    uint16_t port;
    uint8_t n_txt;
    char txt[ESPOS_MDNS_TXT_MAX_BYTES];
} service_t;

static struct {
    SemaphoreHandle_t lock;
    bool up;      /* responder running, hostname set, built-ins registered */
    bool net_up;  /* NETWORK_UP seen and no NETWORK_DOWN since */
    bool subscribed;
    service_t services[CONFIG_ESPOS_WIFI_MDNS_MAX_SERVICES];
} s;

/* The mutex is created on first use rather than in espos_mdns_start():
 * espos_mdns_add_service() is documented as callable before the responder
 * exists, so there is no single owner that runs first. Two tasks racing here
 * both create one; the spinlock decides whose is kept. */
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

/* ------------------------------------------------------------ responder */

/* Register (type, proto) afresh: a record that already exists is withdrawn
 * first so a re-add carries the new port and TXT — the responder refuses a
 * duplicate outright (ESP_ERR_INVALID_ARG) and has no "replace". */
static esp_err_t responder_add(const char *type, const char *proto, uint16_t port, mdns_txt_item_t *txt, size_t n)
{
    if (mdns_service_exists(type, proto, NULL)) {
        (void)mdns_service_remove(type, proto);
    }
    esp_err_t err = mdns_service_add(NULL, type, proto, port, txt, n);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s.%s on port %u refused: %s", type, proto, port, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "advertising %s.%s on port %u", type, proto, port);
    }
    return err;
}

/* Lock held: hand a table entry to the responder. */
static esp_err_t apply_locked(const service_t *svc)
{
    mdns_txt_item_t items[ESPOS_MDNS_TXT_MAX_ITEMS];
    const char *p = svc->txt;
    for (size_t i = 0; i < svc->n_txt; i++) {
        items[i].key = p;
        p += strlen(p) + 1;
        items[i].value = p;
        p += strlen(p) + 1;
    }
    return responder_add(svc->type, svc->proto, svc->port, items, svc->n_txt);
}

static void register_builtin(void)
{
    int32_t port = 80;
    (void)espos_config_get_i32(ESPOS_CFG_NS_HTTPD, ESPOS_CFG_HTTPD_PORT, &port);
    mdns_txt_item_t http_txt[] = { { "path", "/" } };
    (void)responder_add("_http", "_tcp", (uint16_t)port, http_txt, sizeof(http_txt) / sizeof(http_txt[0]));
    /* What a browser needs to know before it fetches anything: which
     * firmware, which espOS, which chip, which device, where the API is and
     * that it needs no credentials (auth=0 until the REST API grows some). */
    mdns_txt_item_t espos_txt[] = {
        { "v", ESPOS_MDNS_PROJECT_VER },
        { "app", ESPOS_MDNS_PROJECT_NAME },
        { "espos", ESPOS_MDNS_ESPOS_VERSION },
        { "target", CONFIG_IDF_TARGET },
        { "id", espos_wifi_short_id() },
        { "api", "/api/v1" },
        { "auth", "0" },
    };
    (void)responder_add("_espos", "_tcp", (uint16_t)port, espos_txt, sizeof(espos_txt) / sizeof(espos_txt[0]));
}

/* ------------------------------------------------------------- events */

/* Default event loop task. Nothing here blocks: the responder follows the
 * interface on its own, this only keeps the readiness answer honest. */
static void on_network(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    bool link = (id == ESPOS_EVENT_NETWORK_UP);
    lock();
    s.net_up = link;
    bool ready = link && s.up;
    unlock();
    if (ready) {
        (void)espos_event_post(ESPOS_EVENT_MDNS_READY, NULL, 0);
    }
}

/* ---------------------------------------------------------------- API */

esp_err_t espos_mdns_start(void)
{
    /* Needs the netif and the default event loop, both espos_wifi's driver
     * init; the status call is the same probe espos_sk_start() uses. */
    espos_wifi_status_t ws;
    if (espos_wifi_get_status(&ws) != ESP_OK) {
        ESP_LOGE(TAG, "espos_mdns_start: call espos_wifi_start() first (or espos_start())");
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    bool up = s.up;
    unlock();
    if (up) {
        return ESP_OK;
    }
    const char *hostname = ws.hostname[0] ? ws.hostname : "espos";
    /* ESP_OK when a component brought the responder up before us (1.11.x);
     * INVALID_STATE is what older releases answered for the same thing. */
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "mdns_init: %s", esp_err_to_name(err));
        return err;
    }
    /* Blocks until the responder task took the name; every record below
     * needs it set (the responder refuses services without a hostname). */
    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_hostname_set(%s): %s", hostname, esp_err_to_name(err));
        return err;
    }
    (void)mdns_instance_name_set(hostname);

    if (!s.subscribed) {
        /* Before `up` is set: a link that comes up from here on is caught by
         * the handler, one that came up earlier is in the status snapshot
         * taken below. Either way MDNS_READY goes out exactly once per link. */
        (void)espos_event_subscribe(ESPOS_EVENT_NETWORK_UP, on_network, NULL);
        (void)espos_event_subscribe(ESPOS_EVENT_NETWORK_DOWN, on_network, NULL);
        s.subscribed = true;
    }

    lock();
    register_builtin();
    for (size_t i = 0; i < CONFIG_ESPOS_WIFI_MDNS_MAX_SERVICES; i++) {
        if (s.services[i].used && apply_locked(&s.services[i]) != ESP_OK) {
            s.services[i].used = false; /* refused by the responder: not queued, see the header */
        }
    }
    s.up = true;
    unlock();
    ESP_LOGI(TAG, "responding as %s.local", hostname);

    (void)espos_wifi_get_status(&ws);
    bool ready = false;
    if (ws.sm.state == ESPOS_WIFI_ST_CONNECTED) {
        lock();
        s.net_up = true;
        ready = true;
        unlock();
    }
    if (ready) {
        (void)espos_event_post(ESPOS_EVENT_MDNS_READY, NULL, 0);
    }
    return ESP_OK;
}

static bool valid_label(const char *str, size_t max)
{
    return str && str[0] == '_' && str[1] != '\0' && strlen(str) < max;
}

esp_err_t espos_mdns_add_service(const char *type, const char *proto, uint16_t port, const char *const *txt_kv, size_t n_txt)
{
    if (!valid_label(type, ESPOS_MDNS_TYPE_MAX) || !proto || port == 0 ||
        (strcmp(proto, "_tcp") != 0 && strcmp(proto, "_udp") != 0) || (n_txt > 0 && !txt_kv)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (n_txt > ESPOS_MDNS_TXT_MAX_ITEMS) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* Measure before touching the table so a refused request leaves no
     * half-written entry behind. "k=v" packs as "k\0v\0", one byte more; a
     * flag item "k" (no '=', value "") as "k\0\0", two more. */
    size_t need = 0;
    for (size_t i = 0; i < n_txt; i++) {
        const char *kv = txt_kv[i];
        if (!kv || kv[0] == '\0' || kv[0] == '=') {
            return ESP_ERR_INVALID_ARG;
        }
        need += strlen(kv) + (strchr(kv, '=') ? 1 : 2);
    }
    if (need > ESPOS_MDNS_TXT_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    lock();
    service_t *svc = NULL;
    for (size_t i = 0; i < CONFIG_ESPOS_WIFI_MDNS_MAX_SERVICES; i++) {
        service_t *e = &s.services[i];
        if (e->used && strcmp(e->type, type) == 0 && strcmp(e->proto, proto) == 0) {
            svc = e; /* replace: same slot, new port and TXT */
            break;
        }
        if (!e->used && !svc) {
            svc = e;
        }
    }
    if (!svc) {
        unlock();
        ESP_LOGW(TAG, "%s.%s: service table full (CONFIG_ESPOS_WIFI_MDNS_MAX_SERVICES=%d)", type, proto,
                 CONFIG_ESPOS_WIFI_MDNS_MAX_SERVICES);
        return ESP_ERR_NO_MEM;
    }
    memset(svc, 0, sizeof(*svc));
    svc->used = true;
    strcpy(svc->type, type);
    strcpy(svc->proto, proto);
    svc->port = port;
    svc->n_txt = (uint8_t)n_txt;
    char *w = svc->txt;
    for (size_t i = 0; i < n_txt; i++) {
        const char *kv = txt_kv[i];
        const char *eq = strchr(kv, '=');
        size_t klen = eq ? (size_t)(eq - kv) : strlen(kv);
        memcpy(w, kv, klen);
        w[klen] = '\0';
        w += klen + 1;
        if (eq) {
            strcpy(w, eq + 1);
            w += strlen(eq + 1) + 1;
        } else {
            *w++ = '\0';
        }
    }
    esp_err_t err = ESP_OK;
    if (s.up) {
        err = apply_locked(svc);
        if (err != ESP_OK) {
            svc->used = false;
        }
    } else {
        ESP_LOGD(TAG, "%s.%s queued until the responder is up", type, proto);
    }
    unlock();
    return err;
}

esp_err_t espos_mdns_remove_service(const char *type, const char *proto)
{
    if (!type || !proto) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < CONFIG_ESPOS_WIFI_MDNS_MAX_SERVICES; i++) {
        service_t *e = &s.services[i];
        if (e->used && strcmp(e->type, type) == 0 && strcmp(e->proto, proto) == 0) {
            e->used = false;
            err = ESP_OK;
            if (s.up) {
                /* NOT_FOUND / INVALID_ARG: the responder never had it or
                 * holds no services at all — withdrawn either way. */
                (void)mdns_service_remove(type, proto);
                ESP_LOGI(TAG, "withdrew %s.%s", type, proto);
            }
            break;
        }
    }
    unlock();
    return err;
}

bool espos_mdns_is_ready(void)
{
    if (!s.lock) {
        return false; /* nobody has started or queued anything yet */
    }
    lock();
    bool ready = s.up && s.net_up;
    unlock();
    return ready;
}

#else /* !CONFIG_ESPOS_WIFI_MDNS */

/* Built without the responder (CONFIG_ESPOS_WIFI_MDNS=n, or the linux target
 * where espressif/mdns does not exist): the API is present so callers need
 * no #ifdef, and answers that nothing is advertised. */
esp_err_t espos_mdns_start(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t espos_mdns_add_service(const char *type, const char *proto, uint16_t port, const char *const *txt_kv, size_t n_txt)
{
    (void)type;
    (void)proto;
    (void)port;
    (void)txt_kv;
    (void)n_txt;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t espos_mdns_remove_service(const char *type, const char *proto)
{
    (void)type;
    (void)proto;
    return ESP_ERR_NOT_SUPPORTED;
}
bool espos_mdns_is_ready(void) { return false; }

#endif /* CONFIG_ESPOS_WIFI_MDNS */

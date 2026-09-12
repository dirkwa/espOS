/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * BLE provisioning, built straight on protocomm.
 *
 * WHY NOT espressif/network_provisioning, which exists and does this?
 *
 * Because its manager drives the WiFi station itself. On receiving
 * credentials it calls esp_wifi_set_storage(FLASH), esp_wifi_set_config() and
 * arms a one-second connect timer -- all BEFORE it invokes the application's
 * callback (manager.c:1292-1318), with no Kconfig to turn that off. espOS
 * already has a host-tested state machine that owns those exact calls, so
 * adopting the manager would mean two owners of one radio and two stores of
 * one set of credentials.
 *
 * protocomm is the layer underneath: a secure, session-oriented request
 * channel over BLE GATT, with no opinion about WiFi. That is exactly, and
 * only, what we want. We take the credentials off the wire, write them into
 * the `wifi` config namespace -- the same keys the web UI writes -- and the
 * existing state machine connects as it always does. Nothing here calls
 * esp_wifi_*.
 *
 * The cost of not using the manager is that Espressif's "ESP BLE
 * Provisioning" phone app speaks the manager's protobuf schema and will not
 * talk to this. The endpoint here is plain JSON, which any BLE tool can
 * write; docs/provisioning.md says so plainly.
 */

#include "sdkconfig.h"

#if CONFIG_ESPOS_PROV

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_srp.h"
#include "espos_cfg_keys.h"
#include "espos_config.h"
#include "espos_prov.h"
#include "protocomm.h"
#include "protocomm_ble.h"
#include "protocomm_security.h"
#include "protocomm_security2.h"

#if __has_include("espos_ble.h")
#include "espos_ble.h"
#define HAVE_BLE_GATEWAY 1
#endif

static const char *TAG = "espos_prov";

/* Endpoint name -> characteristic UUID. protocomm needs one 16-bit UUID per
 * endpoint; these are arbitrary but must stay stable, because a client that
 * has talked to one espOS device expects the same layout on the next. */
#define UUID_SESSION 0xFF51
#define UUID_CONFIG  0xFF52

/* The provisioning service's 128-bit UUID, little-endian as protocomm wants
 * it. Randomly chosen once; changing it makes existing clients blind. */
static const uint8_t kServiceUuid[16] = {
    0x1c,
    0x4b,
    0x8f,
    0x2a,
    0x6d,
    0x11,
    0x47,
    0x9e,
    0xa3,
    0x5c,
    0x70,
    0xe8,
    0x92,
    0x0d,
    0x53,
    0xb6,
};

static struct {
    bool active;
    bool got_creds;
    protocomm_t *pc;
    /* Exactly protocomm's advertised-name capacity (MAX_BLE_DEVNAME_LEN+1):
     * a longer buffer here would be silently truncated on the way into
     * protocomm_ble_config_t, so the device would advertise a name nobody
     * configured. Sized to match, the limit is enforced once, at the door. */
    char service_name[MAX_BLE_DEVNAME_LEN + 1];
    char pop[24];
    char *salt;
    char *verifier;
    int verifier_len;
    protocomm_security2_params_t sec_params;
    bool suspended_scan;
} s;

/* ------------------------------------------------------------ helpers */

/* The device's short id: the same four hex digits used for the hostname and
 * the portal SSID, so one device presents one recognisable name everywhere. */
static void short_id(char *out, size_t len)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(out, len, "%02x%02x", mac[4], mac[5]);
}

/* ------------------------------------------------- the config endpoint */

/* Receives a JSON document and writes it into espOS config. The whole point
 * of the component: credentials in, config written, state machine connects.
 *
 * Accepts {"ssid": "...", "psk": "..."} as the common case, and the general
 * {"wifi": {"ssid0": "..."}} form that espos_config_import_json takes, so a
 * phone can provision more than the radio -- the SignalK server, the
 * hostname -- in the same write.
 */
static esp_err_t config_handler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen,
                                uint8_t **outbuf, ssize_t *outlen, void *priv)
{
    (void)session_id;
    (void)priv;

    /* Answer something even on the failure paths: a client left waiting on a
     * silent characteristic cannot tell a rejected password from a crashed
     * device. */
    const char *reply = "{\"ok\":false,\"error\":\"bad_request\"}";

    if (!inbuf || inlen <= 0) {
        goto respond;
    }

    cJSON *root = cJSON_ParseWithLength((const char *)inbuf, (size_t)inlen);
    if (!root) {
        reply = "{\"ok\":false,\"error\":\"not_json\"}";
        goto respond;
    }

    const cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (cJSON_IsString(ssid) && ssid->valuestring && ssid->valuestring[0]) {
        /* The shorthand form. Network 0 is the slot the setup portal writes
         * too, so provisioning and the portal cannot disagree about which
         * network a freshly configured device tries first. */
        const cJSON *psk = cJSON_GetObjectItem(root, "psk");
        err = espos_config_set_str(ESPOS_CFG_NS_WIFI, ESPOS_CFG_WIFI_SSID0, ssid->valuestring);
        if (err == ESP_OK) {
            err = espos_config_set_str(ESPOS_CFG_NS_WIFI, ESPOS_CFG_WIFI_PSK0,
                                       cJSON_IsString(psk) && psk->valuestring ? psk->valuestring : "");
        }
        if (err == ESP_OK) {
            /* Provisioning implies the station should come up, even on a
             * device someone had disabled it on. */
            err = espos_config_set_bool(ESPOS_CFG_NS_WIFI, ESPOS_CFG_WIFI_STA_ENABLED, true);
        }
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "credentials received for '%s'", ssid->valuestring);
        }
    } else {
        /* The general form: a whole config document. ignore_unknown is false
         * so a typo in a key is rejected rather than silently dropped -- over
         * BLE there is no second chance to notice, and a device that reports
         * success while ignoring half the document is worse than one that
         * says no. */
        char *doc = cJSON_PrintUnformatted(root);
        if (doc) {
            err = espos_config_import_json(doc, strlen(doc), /*ignore_unknown=*/false, NULL, NULL);
            free(doc);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "configuration document applied");
            }
        }
    }
    cJSON_Delete(root);

    if (err == ESP_OK) {
        s.got_creds = true;
        reply = "{\"ok\":true}";
    } else {
        ESP_LOGW(TAG, "could not apply configuration: %s", esp_err_to_name(err));
        reply = "{\"ok\":false,\"error\":\"rejected\"}";
    }

respond:
    /* protocomm frees this. */
    *outlen = (ssize_t)strlen(reply);
    *outbuf = malloc((size_t)*outlen);
    if (!*outbuf) {
        *outlen = 0;
        return ESP_ERR_NO_MEM;
    }
    memcpy(*outbuf, reply, (size_t)*outlen);
    return ESP_OK;
}

/* ------------------------------------------------------------ lifecycle */

esp_err_t espos_prov_start(const espos_prov_cfg_t *cfg)
{
    if (s.active) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s, 0, sizeof(s));

    char id[8];
    short_id(id, sizeof(id));

    if (cfg && cfg->service_name && cfg->service_name[0]) {
        snprintf(s.service_name, sizeof(s.service_name), "%s", cfg->service_name);
    } else {
        snprintf(s.service_name, sizeof(s.service_name), "ESPOS_%s", id);
    }
    if (cfg && cfg->pop && cfg->pop[0]) {
        snprintf(s.pop, sizeof(s.pop), "%s", cfg->pop);
    } else {
        /* Derived, not absent. A PoP the device picks is weak -- anyone who
         * can read the advertised name can guess the scheme -- but it still
         * forces an active attacker to know the MAC, and it is printed so a
         * human can type it. */
        snprintf(s.pop, sizeof(s.pop), "espos%s", id);
    }

    /* SRP6a salt and verifier for Security 2, derived at boot from the PoP.
     * Espressif's own note calls this the development pattern and prefers
     * salt+verifier embedded at manufacture; for a self-built marine device
     * the PoP is not a secret worth a provisioning server. */
    esp_err_t err = esp_srp_gen_salt_verifier(s.service_name, (int)strlen(s.service_name),
                                              s.pop, (int)strlen(s.pop),
                                              &s.salt, 16, &s.verifier, &s.verifier_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "srp salt/verifier: %s", esp_err_to_name(err));
        return err;
    }
    s.sec_params.salt = s.salt;
    s.sec_params.salt_len = 16;
    s.sec_params.verifier = s.verifier;
    s.sec_params.verifier_len = (uint16_t)s.verifier_len;

#ifdef HAVE_BLE_GATEWAY
    /* Before protocomm touches BLE. Bluedroid keeps exactly one GAP callback
     * and protocomm's simple_ble registers its own, so a scanner left running
     * would keep reporting "scanning" and receive nothing at all. */
    if (espos_ble_scan_suspend("BLE provisioning") == ESP_OK) {
        s.suspended_scan = true;
    }
#endif

    s.pc = protocomm_new();
    if (!s.pc) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    protocomm_ble_config_t ble = {
        .nu_lookup_count = 2,
        .nu_lookup = (protocomm_ble_name_uuid_t[]) {
            { "prov-session", UUID_SESSION },
            { "espos-config", UUID_CONFIG },
        },
    };
    snprintf(ble.device_name, sizeof(ble.device_name), "%s", s.service_name);
    memcpy(ble.service_uuid, kServiceUuid, sizeof(kServiceUuid));

    err = protocomm_ble_start(s.pc, &ble);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "protocomm_ble_start: %s", esp_err_to_name(err));
        goto fail;
    }

    err = protocomm_set_security(s.pc, "prov-session", &protocomm_security2, &s.sec_params);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_security: %s", esp_err_to_name(err));
        goto fail;
    }
    err = protocomm_add_endpoint(s.pc, "espos-config", config_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add_endpoint: %s", esp_err_to_name(err));
        goto fail;
    }

    s.active = true;
    ESP_LOGI(TAG, "provisioning over BLE as \"%s\", pop \"%s\"", s.service_name, s.pop);

    esp_err_t api_err = espos_prov_register_api();
    if (api_err != ESP_OK) {
        /* Non-fatal, as everywhere else: provisioning works without its
         * status endpoint, and the name and PoP are on the log. */
        ESP_LOGW(TAG, "status endpoint unavailable: %s", esp_err_to_name(api_err));
    }
    return ESP_OK;

fail:
    espos_prov_stop();
    return err;
}

esp_err_t espos_prov_stop(void)
{
    if (s.pc) {
        protocomm_ble_stop(s.pc);
        protocomm_delete(s.pc);
        s.pc = NULL;
    }
    free(s.salt);
    free(s.verifier);
    s.salt = s.verifier = NULL;

#ifdef HAVE_BLE_GATEWAY
    if (s.suspended_scan) {
        /* Resuming also reclaims the GAP callback protocomm took. Without
         * that the scanner restarts and never receives another
         * advertisement. */
        espos_ble_scan_resume();
        s.suspended_scan = false;
    }
#endif

    if (s.active) {
        ESP_LOGI(TAG, "provisioning stopped");
    }
    s.active = false;
    return ESP_OK;
}

bool espos_prov_is_active(void) { return s.active; }
bool espos_prov_got_credentials(void) { return s.got_creds; }

const char *espos_prov_service_name(void) { return s.service_name; }
const char *espos_prov_pop(void) { return s.pop; }

#endif /* CONFIG_ESPOS_PROV */

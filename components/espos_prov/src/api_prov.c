/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * GET /api/v1/prov -- what a person holding a phone needs to know.
 *
 * Reachable only over the network, which a device being provisioned does not
 * have yet; that is not a contradiction. It is for the other cases: a device
 * already on WiFi that is advertising for re-provisioning, and the setup
 * portal, which serves this page over its own access point. The PoP is in
 * here because a device-derived PoP is useless if nobody can read it.
 */

#include "sdkconfig.h"

#if CONFIG_ESPOS_PROV

#include "cJSON.h"
#include "espos_prov.h"

#if __has_include("espos_httpd.h")

#include "espos_httpd.h"
#include "espos_httpd_sse.h"

const char *espos_prov_service_name(void);
const char *espos_prov_pop(void);

static esp_err_t status_json(char **out)
{
    cJSON *d = cJSON_CreateObject();
    if (!d) return ESP_ERR_NO_MEM;
    cJSON_AddBoolToObject(d, "active", espos_prov_is_active());
    cJSON_AddBoolToObject(d, "got_credentials", espos_prov_got_credentials());
    cJSON_AddStringToObject(d, "service_name", espos_prov_service_name());
    cJSON_AddStringToObject(d, "pop", espos_prov_pop());
    /* Not Espressif's QR schema: that one names their provisioning transport
     * and their app, and this device speaks neither. Ours is the same idea
     * without the false advertisement. */
    cJSON_AddStringToObject(d, "scheme", "espos-ble-prov-1");
    *out = cJSON_PrintUnformatted(d);
    cJSON_Delete(d);
    return *out ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t prov_get(httpd_req_t *req)
{
    char *json = NULL;
    esp_err_t err = status_json(&json);
    if (err != ESP_OK) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "no_mem", "");
    }
    err = espos_httpd_send_json(req, NULL, json);
    free(json);
    return err;
}

esp_err_t espos_prov_register_api(void)
{
    static const httpd_uri_t uri = {
        .uri = "/api/v1/prov",
        .method = HTTP_GET,
        .handler = prov_get,
    };
    return espos_httpd_register(&uri);
}

#else /* no espos_httpd in this build */

esp_err_t espos_prov_register_api(void) { return ESP_OK; }

#endif

#endif /* CONFIG_ESPOS_PROV */

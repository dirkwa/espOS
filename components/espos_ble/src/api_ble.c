/*
 * SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * GET /api/v1/ble/status plus the `ble` SSE event. Registered on espOS's own
 * HTTP server rather than a second instance - there is no need for port
 * isolation here, and ESPOS_HTTPD_MAX_URI_HANDLERS leaves ample room.
 */

#include "sdkconfig.h"

#if defined(CONFIG_BT_BLUEDROID_ENABLED)

#include "cJSON.h"
#include "esp_log.h"
#include "espos_ble.h"
#include "espos_httpd.h"
#include "espos_httpd_sse.h"

static const char *TAG = "espos_ble_api";

esp_err_t espos_ble_status_json(char **out_json)
{
    if (!out_json) return ESP_ERR_INVALID_ARG;
    espos_ble_status_t st;
    esp_err_t err = espos_ble_get_status(&st);
    if (err != ESP_OK) return err;

    cJSON *d = cJSON_CreateObject();
    if (!d) return ESP_ERR_NO_MEM;
    cJSON_AddBoolToObject(d, "enabled", st.enabled);
    cJSON_AddBoolToObject(d, "scanning", st.scanning);
    cJSON_AddStringToObject(d, "mac", st.mac);
    cJSON_AddNumberToObject(d, "scan_hits", st.scan_hits);
    cJSON_AddNumberToObject(d, "adv_received", st.adv_received);
    cJSON_AddNumberToObject(d, "adv_posted", st.adv_posted);
    cJSON_AddNumberToObject(d, "adv_dropped", st.adv_dropped);
    cJSON_AddNumberToObject(d, "adv_pending", (double)st.adv_pending);
    cJSON_AddNumberToObject(d, "post_success", st.post_success);
    cJSON_AddNumberToObject(d, "post_fail", st.post_fail);
    cJSON_AddBoolToObject(d, "ws_connected", st.ws_connected);
    cJSON_AddNumberToObject(d, "gatt_sessions", st.gatt_sessions);
    cJSON_AddNumberToObject(d, "gatt_max", st.gatt_max);

    *out_json = cJSON_PrintUnformatted(d);
    cJSON_Delete(d);
    return *out_json ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t status_get(httpd_req_t *req)
{
    char *json = NULL;
    if (espos_ble_status_json(&json) != ESP_OK || !json) {
        return espos_httpd_send_error(req, "500 Internal Server Error",
                                      "internal", "could not build status");
    }
    esp_err_t err = espos_httpd_send_json(req, NULL, json);
    free(json);
    return err;
}

/* Push a snapshot to a newly connected client so the UI is populated at once
 * rather than after the first change. */
static void sse_hello(int client, void *arg)
{
    (void)arg;
    char *json = NULL;
    if (espos_ble_status_json(&json) == ESP_OK && json) {
        espos_httpd_sse_send(client, "ble", json);
        free(json);
    }
}

esp_err_t espos_ble_register_api(void)
{
    static const httpd_uri_t uris[] = {
        {.uri = "/api/v1/ble/status", .method = HTTP_GET, .handler = status_get},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = espos_httpd_register(&uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s: %s", uris[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    return espos_httpd_sse_on_connect(sse_hello, NULL);
}

#endif /* CONFIG_BT_BLUEDROID_ENABLED */

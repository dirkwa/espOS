/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * /api/v1/net/status — the espos_net status document.
 */
#include <stdlib.h>

#include "espos_httpd.h"
#include "espos_net.h"
#include "net_port.h"

static esp_err_t status_get(httpd_req_t *req)
{
    char *json = NULL;
    esp_err_t err = espos_net_status_json(&json);
    if (err != ESP_OK || !json) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "status_failed", esp_err_to_name(err));
    }
    err = espos_httpd_send_json(req, NULL, json);
    free(json);
    return err;
}

esp_err_t espos_net_register_api(void)
{
    static const httpd_uri_t uri = { .uri = "/api/v1/net/status", .method = HTTP_GET, .handler = status_get };
    return espos_httpd_register(&uri);
}

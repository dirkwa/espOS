/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * /api/v1/sk/{status,servers,discover,request,token,forget}
 */
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "espos_httpd.h"
#include "espos_httpd_sse.h"
#include "espos_sk.h"
#include "espos_sk_priv.h"

static esp_err_t send_doc(httpd_req_t *req, esp_err_t (*fn)(char **))
{
    char *json = NULL;
    esp_err_t err = fn(&json);
    if (err != ESP_OK || !json) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "sk_failed", esp_err_to_name(err));
    }
    err = espos_httpd_send_json(req, NULL, json);
    free(json);
    return err;
}

static esp_err_t status_get(httpd_req_t *req) { return send_doc(req, espos_sk_status_json); }
static esp_err_t servers_get(httpd_req_t *req) { return send_doc(req, espos_sk_servers_json); }

static esp_err_t discover_post(httpd_req_t *req)
{
    if (!espos_httpd_require_json(req)) {
        return ESP_OK;
    }
    esp_err_t err = espos_sk_discover_now();
    if (err != ESP_OK) {
        return espos_httpd_send_error(req, "503 Service Unavailable", "busy", esp_err_to_name(err));
    }
    return espos_httpd_send_json(req, "202 Accepted", "{\"status\":\"discovering\"}");
}

static esp_err_t request_post(httpd_req_t *req)
{
    if (!espos_httpd_require_json(req)) {
        return ESP_OK;
    }
    esp_err_t err = espos_sk_request_now();
    if (err != ESP_OK) {
        return espos_httpd_send_error(req, "503 Service Unavailable", "busy", esp_err_to_name(err));
    }
    return espos_httpd_send_json(req, "202 Accepted", "{\"status\":\"requesting\"}");
}

static esp_err_t forget_post(httpd_req_t *req)
{
    if (!espos_httpd_require_json(req)) {
        return ESP_OK;
    }
    esp_err_t err = espos_sk_forget_token();
    if (err != ESP_OK) {
        return espos_httpd_send_error(req, "503 Service Unavailable", "busy", esp_err_to_name(err));
    }
    return espos_httpd_send_json(req, "202 Accepted", "{\"status\":\"forgotten\"}");
}

static esp_err_t token_post(httpd_req_t *req)
{
    if (!espos_httpd_require_json(req)) {
        return ESP_OK;
    }
    char *body = NULL;
    size_t len = 0;
    if (espos_httpd_read_body(req, &body, &len) != ESP_OK) {
        return ESP_FAIL;
    }
    cJSON *j = cJSON_ParseWithLength(body, len);
    free(body);
    const cJSON *t = j ? cJSON_GetObjectItem(j, "token") : NULL;
    if (!cJSON_IsString(t) || !t->valuestring || !t->valuestring[0]) {
        cJSON_Delete(j);
        return espos_httpd_send_error(req, "400 Bad Request", "validation", "expected {\"token\": \"...\"}");
    }
    esp_err_t err = espos_sk_set_token(t->valuestring);
    cJSON_Delete(j);
    if (err == ESP_ERR_INVALID_ARG) {
        return espos_httpd_send_error(req, "400 Bad Request", "validation", "token too long");
    }
    if (err != ESP_OK) {
        return espos_httpd_send_error(req, "503 Service Unavailable", "busy", esp_err_to_name(err));
    }
    return espos_httpd_send_json(req, "202 Accepted", "{\"status\":\"verifying\"}");
}

static void sse_hello(int client, void *arg)
{
    (void)arg;
    char *json = NULL;
    if (espos_sk_status_json(&json) == ESP_OK) {
        espos_httpd_sse_send(client, "sk", json);
        free(json);
    }
    if (espos_sk_servers_json(&json) == ESP_OK) {
        espos_httpd_sse_send(client, "sk_servers", json);
        free(json);
    }
}

esp_err_t espos_sk_register_api(void)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/v1/sk/status", .method = HTTP_GET, .handler = status_get },
        { .uri = "/api/v1/sk/servers", .method = HTTP_GET, .handler = servers_get },
        { .uri = "/api/v1/sk/discover", .method = HTTP_POST, .handler = discover_post },
        { .uri = "/api/v1/sk/request", .method = HTTP_POST, .handler = request_post },
        { .uri = "/api/v1/sk/token", .method = HTTP_POST, .handler = token_post },
        { .uri = "/api/v1/sk/forget", .method = HTTP_POST, .handler = forget_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = espos_httpd_register(&uris[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return espos_httpd_sse_on_connect(sse_hello, NULL);
}

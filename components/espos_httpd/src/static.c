/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Static UI. M1 ships a placeholder page embedded in the binary; the real UI
 * bundle (M5) will be served from a LittleFS partition through the same
 * handler so URLs do not change.
 */
#include <string.h>

#include "espos_httpd.h"
#include "espos_httpd_priv.h"

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    /* EMBED_TXTFILES appends a NUL terminator; do not send it. */
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

esp_err_t espos_httpd_register_static(httpd_handle_t h)
{
    static const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = index_get };
    static const httpd_uri_t index = { .uri = "/index.html", .method = HTTP_GET, .handler = index_get };
    esp_err_t err = httpd_register_uri_handler(h, &root);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(h, &index);
    }
    return err;
}

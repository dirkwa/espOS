/*
 * SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * espos_httpd — the device's HTTP server (esp_http_server) and the versioned
 * REST API under /api/v1. Other espOS components register their endpoints
 * through espos_httpd_register(); the UI bundle is served from /.
 *
 * API contract: docs/api.md. Changing it is a cross-component decision.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the server on the configured port (httpd.port). Registers the
 * built-in endpoints (config, schema, system, static UI). Idempotent. */
esp_err_t espos_httpd_start(void);
esp_err_t espos_httpd_stop(void);
httpd_handle_t espos_httpd_handle(void);

/** Register an additional URI handler (thin wrapper, server must be started). */
esp_err_t espos_httpd_register(const httpd_uri_t *uri);

/* ------------------------------------------------- helpers for handlers */

/** Send `json` (NUL-terminated) with application/json and the given HTTP
 * status ("200 OK" etc.; NULL = 200). */
esp_err_t espos_httpd_send_json(httpd_req_t *req, const char *status, const char *json);

/** Send {"error": code, "message": msg} with the given status. */
esp_err_t espos_httpd_send_error(httpd_req_t *req, const char *status, const char *code, const char *msg);

/**
 * CSRF guard for state-changing endpoints: require `Content-Type:
 * application/json`. Browsers cannot send that cross-origin without a CORS
 * preflight (which we never answer), so a hostile web page cannot drive
 * PUT/POST endpoints through a user's browser. On failure sends 415 and
 * returns false (handler must return ESP_OK without responding again).
 */
bool espos_httpd_require_json(httpd_req_t *req);

/**
 * Read the whole request body into a malloc'ed, NUL-terminated buffer.
 * Bounded by CONFIG_ESPOS_HTTPD_MAX_BODY: on overflow sends 413 itself and
 * returns ESP_ERR_INVALID_SIZE (caller must return ESP_FAIL without
 * responding again). On socket error returns ESP_FAIL (no response sent).
 */
esp_err_t espos_httpd_read_body(httpd_req_t *req, char **out, size_t *out_len);

#ifdef __cplusplus
}
#endif

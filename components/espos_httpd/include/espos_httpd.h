/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_httpd — the device's HTTP server (esp_http_server) and the versioned
 * REST API under /api/v1. Other espOS components register their endpoints
 * through espos_httpd_register(); the UI bundle is served from /.
 *
 * Every registered endpoint is protected by the REST authentication
 * (docs/security.md) unless registered with ESPOS_HTTPD_PUBLIC: when
 * httpd.api_key is set, a request must carry `Authorization: Bearer <key>`
 * or the session cookie from POST /api/v1/auth/login, else it is answered
 * 401 before the handler runs. With no key set everything is open.
 *
 * URI handlers run on the esp_http_server task — one task for every request
 * the device gets: build the reply, send it, return.
 *
 * API contract: docs/rest-api.md. Changing it is a cross-component decision.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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

/** Register an additional URI handler (server must be started). The handler
 * is protected: see espos_httpd_register_ex(). */
esp_err_t espos_httpd_register(const httpd_uri_t *uri);

/* Registration flags for espos_httpd_register_ex(). Values are ABI. */
typedef enum {
    ESPOS_HTTPD_PROTECTED = 0,     /* the default: 401 without a valid credential when a key is set */
    ESPOS_HTTPD_PUBLIC = 1u << 0,  /* reachable by anyone: the UI, liveness, the login itself */
} espos_httpd_flags_t;

/**
 * Register a URI handler with flags. The httpd_uri_t is copied; the handler
 * is called with the user_ctx it registered. Protected handlers run only
 * after the request passed the authentication check (a 401/403/429 has been
 * sent otherwise); public ones always run and may ask
 * espos_httpd_request_authenticated() themselves. ESP_ERR_INVALID_STATE
 * before espos_httpd_start(), ESP_ERR_HTTPD_HANDLERS_FULL beyond
 * CONFIG_ESPOS_HTTPD_MAX_URI_HANDLERS.
 */
esp_err_t espos_httpd_register_ex(const httpd_uri_t *uri, uint32_t flags);

/**
 * Would this request pass the check a protected endpoint applies? True on an
 * open device (no key configured, unless the build requires one), for a
 * request from the setup portal's network, and for a valid Bearer key or
 * session cookie — a cookie on a state-changing request also needs a
 * matching Origin. Sends nothing. For public handlers that behave
 * differently for the operator.
 */
bool espos_httpd_request_authenticated(httpd_req_t *req);

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

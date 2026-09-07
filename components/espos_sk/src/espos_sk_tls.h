/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The device side of the trust store (sk_tls.c). Private on purpose: it names
 * mbedTLS types, and a public header may include no IDF header but esp_err.h.
 * The decision itself is public and platform-free -- espos_sk_tls_policy.h.
 *
 * Every symbol here exists only when CONFIG_ESPOS_SK_TLS is on; callers guard
 * with the same #if, which is also how a build without TLS keeps mbedTLS out
 * of the link entirely.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#include "sdkconfig.h"

#if CONFIG_ESPOS_SK_TLS

#include "mbedtls/x509_crt.h"

#include "espos_sk_tls_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

/* espos_sk_tls_attach() and espos_sk_tls_trust_mode() are public: any
 * component opening a TLS socket to the same server needs them
 * (espos_sk_tls_policy.h). */

/**
 * Take the device-wide handshake slot and check there is enough contiguous
 * internal RAM for one (CONFIG_ESPOS_SK_TLS_MIN_FREE_BLOCK_KB). Also discards
 * any stale capture, so a caller cannot commit what a previous attempt saw.
 *
 * @return ESP_OK (call espos_sk_tls_handshake_end() when done, whatever the
 *         outcome), ESP_ERR_TIMEOUT when another handshake holds the slot, or
 *         ESP_ERR_NO_MEM when memory is short (a tlsMemory health WARN is
 *         raised; retry after the caller's backoff).
 */
esp_err_t espos_sk_tls_handshake_begin(uint32_t timeout_ms);
void espos_sk_tls_handshake_end(void);

/**
 * Commit what the last handshake captured, now that the connection has proved
 * itself (HTTP 2xx / WebSocket 101). Nothing is written before this: a
 * machine-in-the-middle that completes a handshake but cannot answer as a
 * SignalK server never becomes the anchor. `server_self`, if known, is
 * recorded with it. No-op when nothing was captured or nothing changed.
 */
void espos_sk_tls_commit(const char *server_self);
/** Drop an uncommitted capture. Called before every handshake and after a failed one. */
void espos_sk_tls_discard(void);

/** Forget the anchor: the next successful connection pins afresh. */
esp_err_t espos_sk_tls_reset(void);

/** Configuration, applied by espos_sk.c's load_cfg(). `ca_pem` may be NULL (unchanged). */
void espos_sk_tls_set_trust(espos_sk_tls_trust_t trust, const char *ca_pem);

/** Validate an operator-supplied CA, for PUT /api/v1/sk/tls/ca and the config key. */
esp_err_t espos_sk_tls_parse_ca(const char *pem, mbedtls_x509_crt *out);

/** The GET /api/v1/sk/tls document (malloc'ed), and the SSE publish of it. */
char *espos_sk_tls_json(void);
void espos_sk_tls_publish(void);

/** Why the last handshake was refused ("" when none was). Read-only. */
const char *espos_sk_tls_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_ESPOS_SK_TLS */

/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * REST authentication policy — pure C. Decides whether one request may reach
 * a protected endpoint, keeps the login sessions and throttles failed keys:
 *
 *   portal ──────────────────────────────▶ allowed (the soft-AP network is exempt)
 *   no key configured ───────────────────▶ allowed, or 403 when the build requires a key
 *   Authorization: Bearer <key> ─ match ─▶ allowed        ─ miss ─▶ 401 (counted)
 *   Cookie: espos_sid=<id> ─ live session ▶ allowed; a state change also needs
 *                                          Origin (or Referer) host == Host, else 403
 *   nothing ─────────────────────────────▶ 401
 *   ESPOS_HTTPD_AUTH_FAIL_MAX misses within ESPOS_HTTPD_AUTH_FAIL_WINDOW_S
 *                                        ▶ every key check answers 429 for ESPOS_HTTPD_AUTH_LOCKOUT_S
 *
 * Key and session-id comparisons run in constant time over the maximum
 * length, so neither the bytes nor the length of a secret shows in the
 * response time. The machine never reads a header or a clock itself: the
 * caller hands it the parsed request and a port with the clock and the
 * entropy source, so it runs unchanged on the host under test
 * (test/host/espos_httpd_auth_test). espos_httpd owns the one instance a
 * device runs and serialises every call with its own lock; nothing here is
 * thread-safe on its own.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPOS_HTTPD_AUTH_KEY_MAX       64 /* bytes of an API key, excluding NUL (httpd.api_key maxLength) */
#define ESPOS_HTTPD_AUTH_KEY_MIN       8  /* shorter keys are accepted with a warning, not refused */
#define ESPOS_HTTPD_AUTH_SID_LEN       32 /* hex characters of a session id: 128 bits */
#define ESPOS_HTTPD_AUTH_FAIL_MAX      5  /* failed key checks that start a lockout */
#define ESPOS_HTTPD_AUTH_FAIL_WINDOW_S 60 /* counted within this window */
#define ESPOS_HTTPD_AUTH_LOCKOUT_S     30 /* how long every key check then answers 429 */

/* How a request authenticated itself. Values are ABI: append, never renumber. */
typedef enum {
    ESPOS_HTTPD_AUTH_NONE = 0,   /* no valid credential (the open device reports this too) */
    ESPOS_HTTPD_AUTH_BEARER = 1, /* Authorization: Bearer <key> matched */
    ESPOS_HTTPD_AUTH_COOKIE = 2, /* espos_sid named a live session */
    ESPOS_HTTPD_AUTH_PORTAL = 3, /* arrived on the soft-AP interface: exempt */
    ESPOS_HTTPD_AUTH_METHOD_MAX
} espos_httpd_auth_method_t;

typedef enum {
    ESPOS_HTTPD_AUTH_ALLOW = 0,
    ESPOS_HTTPD_AUTH_UNAUTHORIZED = 1,     /* 401: no or invalid credential */
    ESPOS_HTTPD_AUTH_FORBIDDEN_ORIGIN = 2, /* 403: cookie session, state change, Origin does not match Host */
    ESPOS_HTTPD_AUTH_UNCONFIGURED = 3,     /* 403: the build requires a key and none is configured */
    ESPOS_HTTPD_AUTH_THROTTLED = 4,        /* 429: too many failed key checks */
    ESPOS_HTTPD_AUTH_VERDICT_MAX
} espos_httpd_auth_verdict_t;

/* One login. The table is the caller's (sized by CONFIG_ESPOS_HTTPD_MAX_SESSIONS on a device). */
typedef struct {
    char id[ESPOS_HTTPD_AUTH_SID_LEN + 1];
    uint32_t expires_s;   /* port clock */
    uint32_t last_seen_s; /* LRU order when the table is full */
    bool used;
} espos_httpd_auth_session_t;

/* Everything the machine needs from the outside world. */
typedef struct {
    uint32_t (*now_s)(void *ctx);  /* monotonic seconds, wraps; differences only */
    uint32_t (*random)(void *ctx); /* 32 bits from a real entropy source (esp_random) */
} espos_httpd_auth_port_t;

/* What the decision needs to know about one request. Strings are borrowed
 * for the duration of the call and may be NULL when the header is absent. */
typedef struct {
    const char *bearer;     /* the token of an Authorization: Bearer header */
    const char *cookie_sid; /* the value of the espos_sid cookie */
    const char *host;       /* the Host header */
    const char *origin;     /* the Origin header, else the Referer header */
    bool state_changing;    /* anything but GET, HEAD and OPTIONS */
    bool from_portal;       /* the local socket address is the soft-AP's */
} espos_httpd_auth_request_t;

typedef struct espos_httpd_auth_policy {
    const espos_httpd_auth_port_t *port;
    void *ctx;
    espos_httpd_auth_session_t *sessions;
    size_t session_count;
    uint32_t ttl_s;   /* lifetime of a new session */
    bool require_key; /* CONFIG_ESPOS_HTTPD_AUTH_REQUIRED: refuse until a key exists */
    char key[ESPOS_HTTPD_AUTH_KEY_MAX + 1];
    /* The failure throttle: how many misses since the window opened, and
     * until when the lockout they caused lasts. Global, not per client —
     * a device has no way to tell clients apart that is worth the RAM. */
    uint32_t fail_count;
    uint32_t fail_first_s;
    uint32_t lockout_until_s;
    bool locked;
} espos_httpd_auth_policy_t;

void espos_httpd_auth_policy_init(espos_httpd_auth_policy_t *p, const espos_httpd_auth_port_t *port, void *ctx,
                                  espos_httpd_auth_session_t *sessions, size_t session_count, uint32_t ttl_s,
                                  bool require_key);

/** Install the configured key ("" or NULL = open). Every session is dropped:
 * whoever is logged in with the old key logs in again with the new one. */
void espos_httpd_auth_policy_set_key(espos_httpd_auth_policy_t *p, const char *key);
void espos_httpd_auth_policy_set_ttl(espos_httpd_auth_policy_t *p, uint32_t ttl_s);
/** A key is set. */
bool espos_httpd_auth_policy_configured(const espos_httpd_auth_policy_t *p);
/** Protected endpoints need a credential: a key is set, or the build requires one. */
bool espos_httpd_auth_policy_required(const espos_httpd_auth_policy_t *p);

/**
 * Compare a presented key in constant time. A miss counts toward the
 * throttle; a hit resets the count. ALLOW, UNAUTHORIZED, or THROTTLED while
 * a lockout lasts (nothing is compared then, a correct key included).
 * UNAUTHORIZED without counting when no key is configured.
 */
espos_httpd_auth_verdict_t espos_httpd_auth_policy_check_key(espos_httpd_auth_policy_t *p, const char *presented);
bool espos_httpd_auth_policy_throttled(const espos_httpd_auth_policy_t *p);
/** Seconds until key checks are answered again; 0 when not locked. */
uint32_t espos_httpd_auth_policy_retry_after_s(const espos_httpd_auth_policy_t *p);

/** Mint a session id into id_out (needs ESPOS_HTTPD_AUTH_SID_LEN + 1 bytes).
 * Evicts the least recently used session when the table is full; false only
 * when the table has no slots at all or the buffer is too small. */
bool espos_httpd_auth_policy_session_open(espos_httpd_auth_policy_t *p, char *id_out, size_t id_size);
/** Live session with that id? Stamps it as seen; expired ones are freed on the way. */
bool espos_httpd_auth_policy_session_valid(espos_httpd_auth_policy_t *p, const char *id);
void espos_httpd_auth_policy_session_close(espos_httpd_auth_policy_t *p, const char *id);
void espos_httpd_auth_policy_sessions_clear(espos_httpd_auth_policy_t *p);
size_t espos_httpd_auth_policy_sessions_live(espos_httpd_auth_policy_t *p);

/**
 * The decision for a protected endpoint. `method` (optional) receives how
 * the request authenticated, NONE when it did not — also the answer for
 * GET /api/v1/auth/status. A bearer is a key check (counted, throttled); a
 * request that presents a wrong bearer is refused even if it also carries a
 * live cookie, because it asked to be judged by the key.
 */
espos_httpd_auth_verdict_t espos_httpd_auth_policy_decide(espos_httpd_auth_policy_t *p,
                                                          const espos_httpd_auth_request_t *rq,
                                                          espos_httpd_auth_method_t *method);

/**
 * The authority of an Origin ("http://host:port") or Referer
 * ("http://host:port/path?q") equals the Host header, case-insensitively;
 * a default ":80" is ignored on either side. False when either is missing —
 * a browser always sends Origin on a state-changing request, so a cookie
 * request without one is not a browser doing what its user meant.
 */
bool espos_httpd_auth_origin_matches(const char *origin_or_referer, const char *host);

const char *espos_httpd_auth_method_str(espos_httpd_auth_method_t m);

#ifdef __cplusplus
}
#endif

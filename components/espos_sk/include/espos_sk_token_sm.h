/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SignalK access-token state machine — pure C. Drives the access-request
 * flow of the SignalK security API:
 *
 *   IDLE ──POST /signalk/v1/access/requests──▶ REQUESTED (poll href, backoff 5 s → 60 s)
 *          ├─ 404 security disabled ─────────▶ OPEN (no token needed)
 *          └─ 403 device requests disabled ──▶ DENIED
 *   REQUESTED ─ COMPLETED/APPROVED ▶ store token ▶ VERIFYING ▶ APPROVED
 *             ─ COMPLETED/DENIED   ▶ DENIED (no auto retry)
 *             ─ href gone (404/500 not found) ▶ IDLE (re-request)
 *   APPROVED  ─ periodic GET /signalk/v1/api/self with the token
 *             ─ 401/403 ▶ token invalidated ▶ IDLE
 *   VERIFYING ─ 200 ▶ APPROVED (self URN learned)   ─ 401/403 ▶ IDLE
 *
 * The machine never touches storage or HTTP itself: it asks the port to
 * do things (start a request, poll, verify, save/clear) and is fed the
 * outcomes as events. It runs unchanged on the host under test.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPOS_SK_HOST_MAX 64
#define ESPOS_SK_SELF_MAX 128
#define ESPOS_SK_HREF_MAX 128
#define ESPOS_SK_TOKEN_MAX 1024
#define ESPOS_SK_MSG_MAX 96

typedef enum {
    ESPOS_SK_TOK_NO_SERVER = 0, /* nothing to talk to yet */
    ESPOS_SK_TOK_IDLE,          /* server known; a request will be sent */
    ESPOS_SK_TOK_REQUESTED,     /* pending approval; polling href */
    ESPOS_SK_TOK_VERIFYING,     /* have a token; confirming it works */
    ESPOS_SK_TOK_APPROVED,      /* token works */
    ESPOS_SK_TOK_DENIED,        /* admin denied (or device requests disabled); waits for user */
    ESPOS_SK_TOK_OPEN,          /* server security disabled: no token needed */
    ESPOS_SK_TOK_ERROR,         /* transient trouble (server unreachable); retrying with backoff */
} espos_sk_tok_state_t;

typedef struct {
    char host[ESPOS_SK_HOST_MAX];
    uint16_t port;
    char self[ESPOS_SK_SELF_MAX];   /* vessel self URN if known (mDNS TXT / learned), else "" */
} espos_sk_server_t;

typedef struct {
    char client_id[40];             /* persistent UUID */
    char description[80];
    char permissions[12];           /* readonly | readwrite | admin */
    uint32_t check_interval_ms;     /* token re-verification while APPROVED */
} espos_sk_tok_cfg_t;

/* Persistent state handed in at start and written back through the port. */
typedef struct {
    char token[ESPOS_SK_TOKEN_MAX];
    char token_self[ESPOS_SK_SELF_MAX];   /* the server the token belongs to */
    char pending_href[ESPOS_SK_HREF_MAX];
    char pending_host[ESPOS_SK_HOST_MAX];
    uint16_t pending_port;
    char pending_self[ESPOS_SK_SELF_MAX];
} espos_sk_tok_store_t;

typedef enum {
    ESPOS_SK_EV_START,            /* arg: NULL */
    ESPOS_SK_EV_STOP,
    ESPOS_SK_EV_SERVER,           /* arg: const espos_sk_server_t* (NULL = no server) */
    ESPOS_SK_EV_REQUEST_RESULT,   /* arg: const espos_sk_http_result_t* */
    ESPOS_SK_EV_POLL_RESULT,      /* arg: const espos_sk_http_result_t* */
    ESPOS_SK_EV_VERIFY_RESULT,    /* arg: const espos_sk_http_result_t* */
    ESPOS_SK_EV_TIMER,
    ESPOS_SK_EV_MANUAL_TOKEN,     /* arg: const char* token */
    ESPOS_SK_EV_RETRY,            /* user asks to request again (from DENIED/ERROR) */
    ESPOS_SK_EV_UNAUTHORIZED,     /* some other SK call got 401/403 with our token */
    ESPOS_SK_EV_CONFIG,           /* arg: const espos_sk_tok_cfg_t* */
} espos_sk_tok_event_t;

typedef struct {
    int http_status;              /* 0 = transport failure (unreachable) */
    /* request / poll */
    char state[12];               /* "PENDING" | "COMPLETED" | "" */
    char permission[12];          /* "APPROVED" | "DENIED" | "" */
    char href[ESPOS_SK_HREF_MAX];
    char token[ESPOS_SK_TOKEN_MAX];
    char message[ESPOS_SK_MSG_MAX];
    /* verify */
    char self[ESPOS_SK_SELF_MAX];
} espos_sk_http_result_t;

/* Port: everything the machine needs from the outside world. Actions are
 * asynchronous — the port answers with the *_RESULT events. */
typedef struct {
    void (*http_request)(void *ctx, const espos_sk_server_t *srv, const espos_sk_tok_cfg_t *cfg);
    void (*http_poll)(void *ctx, const espos_sk_server_t *srv, const char *href);
    void (*http_verify)(void *ctx, const espos_sk_server_t *srv, const char *token);
    void (*store_save)(void *ctx, const espos_sk_tok_store_t *st);
    void (*arm_timer)(void *ctx, uint32_t ms);
    void (*cancel_timer)(void *ctx);
    uint32_t (*now_ms)(void *ctx);
    uint32_t (*random)(void *ctx);
    void (*status_changed)(void *ctx);
} espos_sk_tok_port_t;

typedef struct {
    espos_sk_tok_state_t state;
    espos_sk_server_t server;
    bool has_server;
    bool has_token;
    char pending_href[ESPOS_SK_HREF_MAX];
    uint32_t next_action_ms;      /* when the next poll/retry/check is due (port clock) */
    uint32_t poll_interval_ms;    /* current poll backoff */
    uint32_t requested_since_ms;
    uint32_t approved_since_ms;
    uint32_t last_check_ms;
    int last_http_status;
    char last_error[ESPOS_SK_MSG_MAX];
    uint32_t request_count, approve_count, deny_count, unauthorized_count;
    bool busy;                    /* an HTTP action is in flight */
} espos_sk_tok_status_t;

typedef struct espos_sk_tok_sm {
    const espos_sk_tok_port_t *port;
    void *ctx;
    espos_sk_tok_cfg_t cfg;
    espos_sk_tok_store_t store;
    espos_sk_tok_status_t st;
    bool started;
    bool reeval;                  /* server/token changed while an action was in flight */
    uint32_t error_backoff_ms;
    uint32_t timer_due_ms;
} espos_sk_tok_sm_t;

void espos_sk_tok_init(espos_sk_tok_sm_t *sm, const espos_sk_tok_port_t *port, void *ctx,
                       const espos_sk_tok_cfg_t *cfg, const espos_sk_tok_store_t *store);
void espos_sk_tok_event(espos_sk_tok_sm_t *sm, espos_sk_tok_event_t ev, const void *arg);
const espos_sk_tok_status_t *espos_sk_tok_status(const espos_sk_tok_sm_t *sm);
/* Current token ("" if none/unusable). Valid to read on the machine's task. */
const char *espos_sk_tok_token(const espos_sk_tok_sm_t *sm);
const char *espos_sk_tok_state_str(espos_sk_tok_state_t s);

#ifdef __cplusplus
}
#endif

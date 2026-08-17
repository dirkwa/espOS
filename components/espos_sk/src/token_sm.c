/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SignalK access-token state machine (see espos_sk_token_sm.h). Runs on one
 * task; the port performs HTTP asynchronously and reports back with events.
 */
#include <string.h>
#include <stdio.h>
#include "espos_sk_token_sm.h"

#define POLL_MIN_MS      5000u
#define POLL_MAX_MS      60000u
#define ERR_MIN_MS       10000u
#define ERR_MAX_MS       300000u
#define DUP_RETRY_MS     60000u
#define OPEN_CHECK_MS    60000u

static void notify(espos_sk_tok_sm_t *sm)
{
    if (sm->port->status_changed) {
        sm->port->status_changed(sm->ctx);
    }
}

static uint32_t now(espos_sk_tok_sm_t *sm)
{
    return sm->port->now_ms(sm->ctx);
}

static void set_error(espos_sk_tok_sm_t *sm, const char *msg)
{
    snprintf(sm->st.last_error, sizeof(sm->st.last_error), "%s", msg ? msg : "");
}

static void arm(espos_sk_tok_sm_t *sm, uint32_t ms)
{
    sm->st.next_action_ms = now(sm) + ms;
    sm->timer_due_ms = sm->st.next_action_ms ? sm->st.next_action_ms : 1;
    sm->port->arm_timer(sm->ctx, ms);
}

static void cancel(espos_sk_tok_sm_t *sm)
{
    sm->timer_due_ms = 0;
    sm->st.next_action_ms = 0;
    sm->port->cancel_timer(sm->ctx);
}

static void save(espos_sk_tok_sm_t *sm)
{
    sm->port->store_save(sm->ctx, &sm->store);
    sm->st.has_token = sm->store.token[0] != '\0';
    snprintf(sm->st.pending_href, sizeof(sm->st.pending_href), "%s", sm->store.pending_href);
}

static void clear_pending(espos_sk_tok_sm_t *sm)
{
    sm->store.pending_href[0] = '\0';
    sm->store.pending_host[0] = '\0';
    sm->store.pending_self[0] = '\0';
    sm->store.pending_port = 0;
}

static void clear_token(espos_sk_tok_sm_t *sm)
{
    sm->store.token[0] = '\0';
    sm->store.token_self[0] = '\0';
}

/* ---------------------------------------------------------- actions */

static void do_request(espos_sk_tok_sm_t *sm)
{
    sm->st.state = ESPOS_SK_TOK_IDLE;
    sm->st.busy = true;
    sm->st.request_count++;
    cancel(sm);
    sm->port->http_request(sm->ctx, &sm->st.server, &sm->cfg);
    notify(sm);
}

static void do_poll(espos_sk_tok_sm_t *sm)
{
    sm->st.state = ESPOS_SK_TOK_REQUESTED;
    sm->st.busy = true;
    cancel(sm);
    sm->port->http_poll(sm->ctx, &sm->st.server, sm->store.pending_href);
    notify(sm);
}

static void do_verify(espos_sk_tok_sm_t *sm, bool as_check)
{
    if (!as_check) {
        sm->st.state = ESPOS_SK_TOK_VERIFYING;
    }
    sm->st.busy = true;
    cancel(sm);
    sm->port->http_verify(sm->ctx, &sm->st.server, sm->store.token);
    notify(sm);
}

static void enter_error(espos_sk_tok_sm_t *sm, const char *msg)
{
    sm->st.state = ESPOS_SK_TOK_ERROR;
    set_error(sm, msg);
    uint32_t d = sm->error_backoff_ms;
    /* ±20 % jitter */
    uint32_t j = sm->port->random(sm->ctx) % (d / 5 + 1);
    arm(sm, d - d / 10 + j);
    if (sm->error_backoff_ms < ERR_MAX_MS) {
        sm->error_backoff_ms *= 2;
        if (sm->error_backoff_ms > ERR_MAX_MS) {
            sm->error_backoff_ms = ERR_MAX_MS;
        }
    }
    notify(sm);
}

/* Decide what to do for the current server given what we have stored. */
static void evaluate(espos_sk_tok_sm_t *sm)
{
    if (!sm->started) {
        return;
    }
    if (!sm->st.has_server) {
        sm->st.state = ESPOS_SK_TOK_NO_SERVER;
        cancel(sm);
        notify(sm);
        return;
    }
    const espos_sk_server_t *srv = &sm->st.server;
    /* Resume a pending request if it belongs to this server. */
    if (sm->store.pending_href[0]) {
        bool same = (srv->self[0] && sm->store.pending_self[0] && strcmp(srv->self, sm->store.pending_self) == 0) ||
                    (strcmp(srv->host, sm->store.pending_host) == 0 && srv->port == sm->store.pending_port);
        if (same) {
            sm->st.poll_interval_ms = POLL_MIN_MS;
            if (!sm->st.requested_since_ms) {
                sm->st.requested_since_ms = now(sm); /* resumed after a reboot: age unknown */
            }
            do_poll(sm);
            return;
        }
        clear_pending(sm); /* was for another server */
        save(sm);
    }
    /* Try a stored token if it is (or may be) for this server. */
    if (sm->store.token[0]) {
        bool applicable = sm->store.token_self[0] == '\0' || srv->self[0] == '\0' ||
                          strcmp(sm->store.token_self, srv->self) == 0;
        if (applicable) {
            do_verify(sm, false);
            return;
        }
        /* token belongs to a different server (self mismatch): keep it stored
         * (that server may come back) but request access to this one */
    }
    do_request(sm);
}

/* ------------------------------------------------------------ public */

const char *espos_sk_tok_state_str(espos_sk_tok_state_t s)
{
    switch (s) {
    case ESPOS_SK_TOK_NO_SERVER: return "no_server";
    case ESPOS_SK_TOK_IDLE: return "requesting";
    case ESPOS_SK_TOK_REQUESTED: return "pending";
    case ESPOS_SK_TOK_VERIFYING: return "verifying";
    case ESPOS_SK_TOK_APPROVED: return "approved";
    case ESPOS_SK_TOK_DENIED: return "denied";
    case ESPOS_SK_TOK_OPEN: return "open";
    case ESPOS_SK_TOK_ERROR: return "error";
    }
    return "unknown";
}

void espos_sk_tok_init(espos_sk_tok_sm_t *sm, const espos_sk_tok_port_t *port, void *ctx,
                       const espos_sk_tok_cfg_t *cfg, const espos_sk_tok_store_t *store)
{
    memset(sm, 0, sizeof(*sm));
    sm->port = port;
    sm->ctx = ctx;
    sm->cfg = *cfg;
    if (store) {
        sm->store = *store;
    }
    sm->st.state = ESPOS_SK_TOK_NO_SERVER;
    sm->st.has_token = sm->store.token[0] != '\0';
    snprintf(sm->st.pending_href, sizeof(sm->st.pending_href), "%s", sm->store.pending_href);
    sm->error_backoff_ms = ERR_MIN_MS;
    sm->st.poll_interval_ms = POLL_MIN_MS;
}

const espos_sk_tok_status_t *espos_sk_tok_status(const espos_sk_tok_sm_t *sm)
{
    return &sm->st;
}

const char *espos_sk_tok_token(const espos_sk_tok_sm_t *sm)
{
    return (sm->st.state == ESPOS_SK_TOK_APPROVED) ? sm->store.token : "";
}

void espos_sk_tok_event(espos_sk_tok_sm_t *sm, espos_sk_tok_event_t ev, const void *arg)
{
    switch (ev) {
    case ESPOS_SK_EV_START:
        if (sm->started) {
            return;
        }
        sm->started = true;
        evaluate(sm);
        return;

    case ESPOS_SK_EV_STOP:
        sm->started = false;
        sm->st.busy = false;
        sm->reeval = false;
        cancel(sm);
        sm->st.state = ESPOS_SK_TOK_NO_SERVER;
        notify(sm);
        return;

    case ESPOS_SK_EV_CONFIG:
        if (arg) {
            bool interval_changed = sm->cfg.check_interval_ms != ((const espos_sk_tok_cfg_t *)arg)->check_interval_ms;
            sm->cfg = *(const espos_sk_tok_cfg_t *)arg;
            if (interval_changed && sm->st.state == ESPOS_SK_TOK_APPROVED && !sm->st.busy) {
                arm(sm, sm->cfg.check_interval_ms); /* apply the new check cadence now */
            }
        }
        return;

    case ESPOS_SK_EV_SERVER: {
        const espos_sk_server_t *srv = arg;
        bool same_addr = srv && sm->st.has_server && strcmp(srv->host, sm->st.server.host) == 0 &&
                         srv->port == sm->st.server.port;
        bool self_compatible = srv && (srv->self[0] == '\0' || sm->st.server.self[0] == '\0' ||
                                       strcmp(srv->self, sm->st.server.self) == 0);
        if (same_addr && self_compatible) {
            /* same server; a self URN may have become known via mDNS */
            if (srv->self[0] && !sm->st.server.self[0]) {
                snprintf(sm->st.server.self, sizeof(sm->st.server.self), "%s", srv->self);
                if (sm->store.token[0] && !sm->store.token_self[0]) {
                    snprintf(sm->store.token_self, sizeof(sm->store.token_self), "%s", srv->self);
                    save(sm);
                }
                notify(sm);
            }
            return;
        }
        if (srv) {
            sm->st.server = *srv;
            sm->st.has_server = true;
        } else {
            memset(&sm->st.server, 0, sizeof(sm->st.server));
            sm->st.has_server = false;
        }
        sm->error_backoff_ms = ERR_MIN_MS;
        set_error(sm, "");
        if (sm->st.busy) {
            /* an action for the old server is in flight: its result must not
             * be attributed to the new one — re-evaluate when it lands */
            sm->reeval = true;
            notify(sm);
            return;
        }
        evaluate(sm);
        return;
    }

    case ESPOS_SK_EV_REQUEST_RESULT: {
        const espos_sk_http_result_t *r = arg;
        if (sm->reeval && sm->st.busy) {
            /* stale answer for a previous server/token: drop it, start over */
            sm->reeval = false;
            sm->st.busy = false;
            evaluate(sm);
            return;
        }
        if (sm->st.state != ESPOS_SK_TOK_IDLE || !sm->st.busy) {
            return; /* stale */
        }
        sm->st.busy = false;
        sm->st.last_http_status = r->http_status;
        if (r->http_status == 202 && r->href[0]) {
            snprintf(sm->store.pending_href, sizeof(sm->store.pending_href), "%s", r->href);
            snprintf(sm->store.pending_host, sizeof(sm->store.pending_host), "%s", sm->st.server.host);
            snprintf(sm->store.pending_self, sizeof(sm->store.pending_self), "%s", sm->st.server.self);
            sm->store.pending_port = sm->st.server.port;
            save(sm);
            sm->st.state = ESPOS_SK_TOK_REQUESTED;
            sm->st.requested_since_ms = now(sm);
            sm->st.poll_interval_ms = POLL_MIN_MS;
            sm->error_backoff_ms = ERR_MIN_MS;
            set_error(sm, "");
            arm(sm, POLL_MIN_MS);
            notify(sm);
            return;
        }
        if (r->http_status == 404) {
            /* "Server security is not enabled": nothing to request */
            sm->st.state = ESPOS_SK_TOK_OPEN;
            sm->error_backoff_ms = ERR_MIN_MS;
            set_error(sm, "");
            arm(sm, OPEN_CHECK_MS);
            notify(sm);
            return;
        }
        if (r->http_status == 599) {
            enter_error(sm, "host answers but is not a SignalK server");
            return;
        }
        if (r->http_status == 403) {
            sm->st.state = ESPOS_SK_TOK_DENIED;
            sm->st.deny_count++;
            set_error(sm, "device access requests are disabled on the server");
            cancel(sm);
            notify(sm);
            return;
        }
        if (r->http_status == 400) {
            /* typically "already requested access": an unanswered request we
             * lost track of. Once the admin decides it, a new one goes through. */
            sm->st.state = ESPOS_SK_TOK_ERROR;
            set_error(sm, r->message[0] ? r->message : "request rejected (400)");
            arm(sm, DUP_RETRY_MS);
            notify(sm);
            return;
        }
        if (r->http_status == 0) {
            enter_error(sm, "server unreachable");
        } else {
            char m[ESPOS_SK_MSG_MAX];
            snprintf(m, sizeof(m), "request failed (HTTP %d)", r->http_status);
            enter_error(sm, m);
        }
        return;
    }

    case ESPOS_SK_EV_POLL_RESULT: {
        const espos_sk_http_result_t *r = arg;
        if (sm->reeval && sm->st.busy) {
            /* stale answer for a previous server/token: drop it, start over */
            sm->reeval = false;
            sm->st.busy = false;
            evaluate(sm);
            return;
        }
        if (sm->st.state != ESPOS_SK_TOK_REQUESTED || !sm->st.busy) {
            return;
        }
        sm->st.busy = false;
        sm->st.last_http_status = r->http_status;
        if ((r->http_status == 200 || r->http_status == 202) && r->state[0] == '\0') {
            enter_error(sm, "unexpected reply while polling (not a SignalK server?)");
            return;
        }
        if (r->http_status == 200 || r->http_status == 202) {
            sm->error_backoff_ms = ERR_MIN_MS; /* the server answers again */
            if (strcmp(r->state, "COMPLETED") == 0) {
                if (strcmp(r->permission, "APPROVED") == 0 && r->token[0]) {
                    snprintf(sm->store.token, sizeof(sm->store.token), "%s", r->token);
                    snprintf(sm->store.token_self, sizeof(sm->store.token_self), "%s", sm->st.server.self);
                    clear_pending(sm);
                    save(sm);
                    sm->st.approve_count++;
                    set_error(sm, "");
                    do_verify(sm, false);
                    return;
                }
                /* DENIED, or COMPLETED with an error statusCode */
                clear_pending(sm);
                save(sm);
                sm->st.state = ESPOS_SK_TOK_DENIED;
                sm->st.deny_count++;
                set_error(sm, r->message[0] ? r->message :
                          (strcmp(r->permission, "DENIED") == 0 ? "access denied by the server admin" : "request rejected"));
                cancel(sm);
                notify(sm);
                return;
            }
            /* PENDING: keep polling with backoff */
            uint32_t next = sm->st.poll_interval_ms + sm->st.poll_interval_ms / 2;
            if (next > POLL_MAX_MS) {
                next = POLL_MAX_MS;
            }
            sm->st.poll_interval_ms = next;
            arm(sm, next);
            notify(sm);
            return;
        }
        if (r->http_status == 404 || (r->http_status == 500 && strstr(r->message, "not found"))) {
            /* the server forgot the request (restart, prune): start over */
            clear_pending(sm);
            save(sm);
            set_error(sm, "server lost the request; requesting again");
            do_request(sm);
            return;
        }
        if (r->http_status == 0) {
            enter_error(sm, "server unreachable");
        } else {
            char m[ESPOS_SK_MSG_MAX];
            snprintf(m, sizeof(m), "poll failed (HTTP %d)", r->http_status);
            enter_error(sm, m);
        }
        return;
    }

    case ESPOS_SK_EV_VERIFY_RESULT: {
        const espos_sk_http_result_t *r = arg;
        if (sm->reeval && sm->st.busy) {
            /* stale answer for a previous server/token: drop it, start over */
            sm->reeval = false;
            sm->st.busy = false;
            evaluate(sm);
            return;
        }
        bool checking = sm->st.state == ESPOS_SK_TOK_APPROVED || sm->st.state == ESPOS_SK_TOK_OPEN;
        if ((sm->st.state != ESPOS_SK_TOK_VERIFYING && !checking) || !sm->st.busy) {
            return;
        }
        sm->st.busy = false;
        sm->st.last_http_status = r->http_status;
        sm->st.last_check_ms = now(sm);
        if (r->http_status == 200) {
            if (sm->st.state == ESPOS_SK_TOK_OPEN) {
                arm(sm, OPEN_CHECK_MS); /* still open */
                notify(sm);
                return;
            }
            if (r->self[0]) {
                bool changed = strcmp(sm->store.token_self, r->self) != 0;
                snprintf(sm->store.token_self, sizeof(sm->store.token_self), "%s", r->self);
                if (sm->st.server.self[0] == '\0') {
                    snprintf(sm->st.server.self, sizeof(sm->st.server.self), "%s", r->self);
                }
                if (changed) {
                    save(sm);
                }
            }
            if (sm->st.state != ESPOS_SK_TOK_APPROVED) {
                sm->st.approved_since_ms = now(sm);
            }
            sm->st.state = ESPOS_SK_TOK_APPROVED;
            sm->error_backoff_ms = ERR_MIN_MS;
            set_error(sm, "");
            arm(sm, sm->cfg.check_interval_ms);
            notify(sm);
            return;
        }
        if (r->http_status == 401 || r->http_status == 403) {
            if (sm->st.state == ESPOS_SK_TOK_OPEN) {
                /* security got enabled: go get a token */
                set_error(sm, "");
                do_request(sm);
                return;
            }
            sm->st.unauthorized_count++;
            clear_token(sm);
            save(sm);
            set_error(sm, "token rejected by the server; requesting access again");
            do_request(sm);
            return;
        }
        if (r->http_status == 0) {
            enter_error(sm, "server unreachable");
        } else {
            char m[ESPOS_SK_MSG_MAX];
            snprintf(m, sizeof(m), "check failed (HTTP %d)", r->http_status);
            enter_error(sm, m);
        }
        return;
    }

    case ESPOS_SK_EV_TIMER:
        if (!sm->started || sm->st.busy || sm->timer_due_ms == 0 ||
            (int32_t)(now(sm) - sm->timer_due_ms) < 0) {
            return;
        }
        sm->timer_due_ms = 0;
        switch (sm->st.state) {
        case ESPOS_SK_TOK_REQUESTED:
            do_poll(sm);
            break;
        case ESPOS_SK_TOK_APPROVED:
            do_verify(sm, true);
            break;
        case ESPOS_SK_TOK_OPEN:
            /* GET /self succeeds without a token when allow_readonly is on,
             * so a re-POST is the only reliable probe: 404 = still open,
             * 202 = security got enabled and we are already in the queue. */
            do_request(sm);
            break;
        case ESPOS_SK_TOK_ERROR:
            evaluate(sm); /* resume whatever is right for THIS server */
            break;
        default:
            break;
        }
        return;

    case ESPOS_SK_EV_MANUAL_TOKEN: {
        const char *tok = arg;
        if (!tok || !tok[0]) {
            return;
        }
        snprintf(sm->store.token, sizeof(sm->store.token), "%s", tok);
        snprintf(sm->store.token_self, sizeof(sm->store.token_self), "%s",
                 sm->st.has_server ? sm->st.server.self : "");
        clear_pending(sm);
        save(sm);
        sm->error_backoff_ms = ERR_MIN_MS;
        set_error(sm, "");
        if (!sm->st.has_server) {
            notify(sm); /* kept; verified as soon as a server is known */
            return;
        }
        if (sm->st.busy) {
            sm->reeval = true; /* verify once the in-flight action has answered */
            notify(sm);
            return;
        }
        do_verify(sm, false);
        return;
    }

    case ESPOS_SK_EV_RETRY:
        if (!sm->started || !sm->st.has_server || sm->st.busy) {
            return;
        }
        sm->error_backoff_ms = ERR_MIN_MS;
        set_error(sm, "");
        if (sm->st.state == ESPOS_SK_TOK_DENIED || sm->st.state == ESPOS_SK_TOK_OPEN) {
            clear_pending(sm);
            save(sm);
            do_request(sm);
        } else {
            evaluate(sm);
        }
        return;

    case ESPOS_SK_EV_UNAUTHORIZED:
        if (sm->st.state == ESPOS_SK_TOK_APPROVED && !sm->st.busy) {
            sm->st.unauthorized_count++;
            clear_token(sm);
            save(sm);
            set_error(sm, "token rejected by the server; requesting access again");
            do_request(sm);
        }
        return;
    }
}

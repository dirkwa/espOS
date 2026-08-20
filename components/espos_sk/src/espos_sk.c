/*
 * SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * espos_sk core: one task owns discovery, server selection and the token
 * state machine. Everything else (HTTP handlers, config changes, other
 * components) talks to it through a command queue and reads a snapshot
 * under a mutex.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "sdkconfig.h"

#include "espos_cfg_keys.h"
#include "espos_config.h"
#include "espos_httpd_sse.h"
#include "espos_sk.h"
#include "espos_sk_priv.h"
#include "espos_wifi.h"

static const char *TAG = "espos_sk";

typedef enum { CMD_CONFIG, CMD_DISCOVER, CMD_REQUEST, CMD_TOKEN, CMD_FORGET, CMD_UNAUTHORIZED, CMD_STOP } cmd_type_t;
typedef struct {
    cmd_type_t type;
    char *str; /* CMD_TOKEN: malloc'ed token */
} cmd_t;

typedef enum { ACT_NONE, ACT_REQUEST, ACT_POLL, ACT_VERIFY } action_t;

static struct {
    TaskHandle_t task;
    QueueHandle_t cmds;
    SemaphoreHandle_t lock;          /* protects snapshot + servers */
    bool started;
    bool api_registered;

    /* task-private */
    espos_sk_tok_sm_t sm;
    espos_sk_tok_cfg_t cfg;
    action_t action;
    char action_href[ESPOS_SK_HREF_MAX];
    char action_token[ESPOS_SK_TOKEN_MAX];
    uint32_t timer_due_ms;           /* 0 = none */
    uint32_t discover_due_ms;
    bool discovery_enabled;
    uint32_t discover_interval_ms;
    char cfg_self[ESPOS_SK_SELF_MAX];
    bool cfg_pin;
    char cfg_host[ESPOS_SK_HOST_MAX];
    uint16_t cfg_port;
    bool have_server;
    espos_sk_server_t server;
    char server_source[12];          /* "manual" | "discovered" | "" */

    /* shared snapshot */
    espos_sk_tok_status_t snap;
    char snap_token[ESPOS_SK_TOKEN_MAX];
    char client_id[40];
    espos_sk_discovered_t servers[ESPOS_SK_MAX_SERVERS];
    uint32_t avoid_until_ms[ESPOS_SK_MAX_SERVERS]; /* unreachable discovered servers are skipped for a while */
    size_t server_count;
    uint32_t last_discovery_ms;
    bool wifi_was_connected;
    char snap_source[12];
    char hostname[33];
} s;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* A deadline that is never 0 (0 means "none"; the tick count starts at 0). */
static uint32_t at(uint32_t ms)
{
    uint32_t t = now_ms() + ms;
    return t ? t : 1;
}

static void lock(void) { xSemaphoreTake(s.lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s.lock); }

/* -------------------------------------------------------- SM port */

static void p_request(void *ctx, const espos_sk_server_t *srv, const espos_sk_tok_cfg_t *cfg)
{
    (void)ctx; (void)srv; (void)cfg;
    s.action = ACT_REQUEST;
}
static void p_poll(void *ctx, const espos_sk_server_t *srv, const char *href)
{
    (void)ctx; (void)srv;
    s.action = ACT_POLL;
    snprintf(s.action_href, sizeof(s.action_href), "%s", href);
}
static void p_verify(void *ctx, const espos_sk_server_t *srv, const char *token)
{
    (void)ctx; (void)srv;
    s.action = ACT_VERIFY;
    snprintf(s.action_token, sizeof(s.action_token), "%s", token);
}
static void p_save(void *ctx, const espos_sk_tok_store_t *st)
{
    (void)ctx;
    espos_sk_store_save(st);
}
static void p_arm(void *ctx, uint32_t ms)
{
    (void)ctx;
    s.timer_due_ms = at(ms);
}
static void p_cancel(void *ctx) { (void)ctx; s.timer_due_ms = 0; }
static uint32_t p_now(void *ctx) { (void)ctx; return now_ms(); }
static uint32_t p_random(void *ctx) { (void)ctx; return esp_random(); }

static char *status_json_from(const espos_sk_tok_status_t *st, const char *source);

static void p_status_changed(void *ctx)
{
    (void)ctx;
    lock();
    s.snap = s.sm.st;
    snprintf(s.snap_token, sizeof(s.snap_token), "%s", espos_sk_tok_token(&s.sm));
    snprintf(s.snap_source, sizeof(s.snap_source), "%s", s.server_source);
    unlock();
    char *json = status_json_from(&s.sm.st, s.server_source);
    if (json) {
        espos_httpd_sse_publish("sk", json);
        free(json);
    }
}

static const espos_sk_tok_port_t k_port = {
    .http_request = p_request, .http_poll = p_poll, .http_verify = p_verify, .store_save = p_save,
    .arm_timer = p_arm, .cancel_timer = p_cancel, .now_ms = p_now, .random = p_random,
    .status_changed = p_status_changed,
};

/* ------------------------------------------------------ config load */

static void load_cfg(void)
{
    espos_config_get_bool(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_DISCOVERY, &s.discovery_enabled);
    espos_config_get_str(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_SERVER_SELF, s.cfg_self, sizeof(s.cfg_self), NULL);
    espos_config_get_bool(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_SERVER_PIN, &s.cfg_pin);
    espos_config_get_str(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_SERVER_HOST, s.cfg_host, sizeof(s.cfg_host), NULL);
    int32_t v = 80;
    espos_config_get_i32(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_SERVER_PORT, &v);
    s.cfg_port = (uint16_t)v;
    v = 60;
    espos_config_get_i32(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_DISCOVER_S, &v);
    s.discover_interval_ms = (uint32_t)v * 1000;

    espos_sk_tok_cfg_t c = { 0 };
    snprintf(c.client_id, sizeof(c.client_id), "%s", s.client_id);
    char d[65] = { 0 };
    espos_config_get_str(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_DESCRIPTION, d, sizeof(d), NULL);
    if (d[0]) {
        snprintf(c.description, sizeof(c.description), "%s", d);
    } else {
        snprintf(c.description, sizeof(c.description), "espOS %s", s.hostname);
    }
    espos_config_get_str(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_PERMISSIONS, c.permissions, sizeof(c.permissions), NULL);
    v = 60;
    espos_config_get_i32(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_CHECK_S, &v);
    c.check_interval_ms = (uint32_t)v * 1000;
    lock();
    s.cfg = c; /* also read by status_json_from() on other tasks */
    unlock();
    espos_sk_tok_event(&s.sm, ESPOS_SK_EV_CONFIG, &c);
}

/* ---------------------------------------------------- server choice */

/* Pick the server per config: manual host wins; else the discovered server
 * with the preferred self; else the first discovered "master"; else the
 * first discovered. Task-private. */
static void select_server(void)
{
    espos_sk_server_t chosen = { 0 };
    const char *source = "";
    bool have = false;
    if (s.cfg_host[0]) {
        snprintf(chosen.host, sizeof(chosen.host), "%s", s.cfg_host);
        chosen.port = s.cfg_port;
        /* self may be known from a discovered entry with the same host:port */
        lock();
        for (size_t i = 0; i < s.server_count; i++) {
            if (strcmp(s.servers[i].host, chosen.host) == 0 && s.servers[i].port == chosen.port) {
                snprintf(chosen.self, sizeof(chosen.self), "%s", s.servers[i].self);
                break;
            }
        }
        unlock();
        source = "manual";
        have = true;
    } else {
        lock();
        int idx = -1;
        uint32_t t = now_ms();
        if (s.cfg_self[0]) {
            for (size_t i = 0; i < s.server_count; i++) {
                if (strcmp(s.servers[i].self, s.cfg_self) == 0) {
                    idx = (int)i;
                    break;
                }
            }
        } else {
            /* Sticky and deterministic: (1) the server our token / pending
             * request belongs to, (2) reachable "master" servers by self URN,
             * (3) any reachable server by self URN. mDNS answer order is not
             * stable, so never "first in the list". */
            const char *anchor = s.sm.store.token[0] ? s.sm.store.token_self :
                                 (s.sm.store.pending_href[0] ? s.sm.store.pending_self : "");
            for (size_t i = 0; i < s.server_count && anchor[0]; i++) {
                if (strcmp(s.servers[i].self, anchor) == 0) {
                    idx = (int)i;
                    break;
                }
            }
            for (int pass = 0; pass < 2 && idx < 0; pass++) {
                for (size_t i = 0; i < s.server_count; i++) {
                    bool avoided = s.avoid_until_ms[i] && (int32_t)(s.avoid_until_ms[i] - t) > 0;
                    bool master = strstr(s.servers[i].roles, "master") != NULL;
                    if (avoided || (pass == 0 && !master)) {
                        continue;
                    }
                    if (idx < 0 || strcmp(s.servers[i].self, s.servers[idx].self) < 0) {
                        idx = (int)i;
                    }
                }
            }
        }
        /* Pinned: keep the server already in use even when this round of
         * discovery did not see it. Re-election is what moves a helm
         * display onto a different vessel's server mid-passage; some
         * installations would rather wait for their own to answer again. */
        if (idx < 0 && s.cfg_pin && s.have_server && s.server.host[0]) {
            chosen = s.server;
            source = "pinned";
            have = true;
        }
        if (idx >= 0) {
            snprintf(chosen.host, sizeof(chosen.host), "%s", s.servers[idx].host);
            chosen.port = s.servers[idx].port;
            snprintf(chosen.self, sizeof(chosen.self), "%s", s.servers[idx].self);
            source = "discovered";
            have = true;
        }
        unlock();
    }
    bool changed = have != s.have_server || (have && (strcmp(chosen.host, s.server.host) != 0 ||
                   chosen.port != s.server.port || strcmp(chosen.self, s.server.self) != 0));
    bool source_changed = strcmp(source, s.server_source) != 0;
    s.have_server = have;
    s.server = chosen;
    snprintf(s.server_source, sizeof(s.server_source), "%s", source);
    if (!changed && source_changed) {
        p_status_changed(NULL); /* same server, different provenance: refresh the snapshot */
    }
    if (changed) {
        if (have) {
            ESP_LOGI(TAG, "server: %s:%u (%s%s%s)", chosen.host, chosen.port, source,
                     chosen.self[0] ? " " : "", chosen.self);
        } else {
            ESP_LOGI(TAG, "no server (waiting for discovery or manual host)");
        }
        espos_sk_tok_event(&s.sm, ESPOS_SK_EV_SERVER, have ? &chosen : NULL);
    }
}

static void run_discovery(void)
{
    /* heap, not stack: two arrays of ~2 KiB each */
    espos_sk_discovered_t *found = calloc(ESPOS_SK_MAX_SERVERS, sizeof(*found));
    espos_sk_discovered_t *merged = calloc(ESPOS_SK_MAX_SERVERS, sizeof(*merged));
    if (!found || !merged) {
        free(found);
        free(merged);
        return;
    }
    size_t n = espos_sk_discovery_run(found, ESPOS_SK_MAX_SERVERS);
    uint32_t t = now_ms();
    lock();
    /* Merge: fresh results replace, but keep recently seen entries that
     * dropped out of one query (mDNS is lossy) for two intervals. */
    uint32_t avoid[ESPOS_SK_MAX_SERVERS] = { 0 };
    size_t m = 0;
    /* The server our token belongs to is kept unconditionally.
     *
     * Fresh results used to fill the array first, so on a network
     * advertising ESPOS_SK_MAX_SERVERS or more the carry-over loop below
     * — the whole defence against mDNS being lossy — could not run at
     * all. One missed answer then evicted the anchored server, the
     * anchor lookup in select_server() found nothing, and a perfectly
     * reachable server was dropped with "no server". Reserving its slot
     * first means a full list can never cost us the one entry that
     * matters. */
    const char *keep = s.sm.store.token[0] ? s.sm.store.token_self :
                       (s.sm.store.pending_href[0] ? s.sm.store.pending_self : "");
    if (keep[0]) {
        for (size_t i = 0; i < s.server_count; i++) {
            if (strcmp(s.servers[i].self, keep) != 0) {
                continue;
            }
            /* Prefer this round's fresher copy if mDNS did answer for it. */
            bool fresh = false;
            for (size_t k = 0; k < n; k++) {
                if (strcmp(found[k].self, keep) == 0) {
                    merged[m] = found[k];
                    merged[m].seen_ms = t;
                    fresh = true;
                    break;
                }
            }
            if (!fresh) {
                merged[m] = s.servers[i];
            }
            avoid[m] = s.avoid_until_ms[i];
            m++;
            break;
        }
    }
    for (size_t i = 0; i < n && m < ESPOS_SK_MAX_SERVERS; i++) {
        if (keep[0] && m > 0 && strcmp(found[i].self, keep) == 0) {
            continue;   /* already reserved above */
        }
        merged[m] = found[i];
        merged[m].seen_ms = t;
        for (size_t k = 0; k < s.server_count; k++) { /* carry the avoid stamp over */
            if (strcmp(s.servers[k].host, found[i].host) == 0 && s.servers[k].port == found[i].port) {
                avoid[m] = s.avoid_until_ms[k];
            }
        }
        m++;
    }
    for (size_t i = 0; i < s.server_count && m < ESPOS_SK_MAX_SERVERS; i++) {
        bool dup = false;
        for (size_t k = 0; k < m; k++) {   /* against what we kept, including the reserved anchor */
            if (strcmp(merged[k].host, s.servers[i].host) == 0 && merged[k].port == s.servers[i].port) {
                dup = true;
                break;
            }
        }
        if (!dup && (int32_t)(t - s.servers[i].seen_ms) < (int32_t)(2 * s.discover_interval_ms + 5000)) {
            avoid[m] = s.avoid_until_ms[i];
            merged[m++] = s.servers[i];
        }
    }
    memcpy(s.servers, merged, m * sizeof(merged[0]));
    memcpy(s.avoid_until_ms, avoid, sizeof(avoid));
    s.server_count = m;
    s.last_discovery_ms = t;
    unlock();
    free(found);
    free(merged);
    ESP_LOGI(TAG, "discovery: %u server(s)", (unsigned)m);
    char *json = NULL;
    if (espos_sk_servers_json(&json) == ESP_OK) {
        espos_httpd_sse_publish("sk_servers", json);
        free(json);
    }
    select_server();
}

/* ------------------------------------------------------------ task */

static void handle_cmd(const cmd_t *c)
{
    switch (c->type) {
    case CMD_CONFIG:
        load_cfg();
        if (!s.discovery_enabled) {
            lock();
            s.server_count = 0; /* discovered entries are no longer valid choices */
            unlock();
        }
        select_server();
        if (s.discovery_enabled && s.discover_due_ms == 0) {
            s.discover_due_ms = at(0);
        }
        break;
    case CMD_DISCOVER:
        if (s.discovery_enabled) {
            run_discovery();
        }
        s.discover_due_ms = at(s.discover_interval_ms);
        break;
    case CMD_REQUEST:
        espos_sk_tok_event(&s.sm, ESPOS_SK_EV_RETRY, NULL);
        break;
    case CMD_TOKEN:
        espos_sk_tok_event(&s.sm, ESPOS_SK_EV_MANUAL_TOKEN, c->str);
        break;
    case CMD_FORGET: {
        /* Drop the token. A pending request stays: the server holds it
         * anyway and would refuse a duplicate; polling just resumes. */
        s.sm.store.token[0] = '\0';
        s.sm.store.token_self[0] = '\0';
        espos_sk_store_save(&s.sm.store);
        s.sm.st.has_token = false;
        espos_sk_tok_event(&s.sm, ESPOS_SK_EV_RETRY, NULL);
        p_status_changed(NULL); /* even without a server the snapshot must show the loss */
        break;
    }
    case CMD_UNAUTHORIZED:
        espos_sk_tok_event(&s.sm, ESPOS_SK_EV_UNAUTHORIZED, NULL);
        break;
    case CMD_STOP:
        break;
    }
}

/* A discovered server we cannot reach (wrong subnet, gone) must not hold
 * the machine hostage when others are available: sidestep it for a while. */
static void maybe_rotate_unreachable(void)
{
    /* cfg_self and cfg_pin both mean "this server or none": rotating away
     * would defeat the setting the moment the server had a bad minute. */
    if (s.sm.st.state != ESPOS_SK_TOK_ERROR || s.sm.st.last_http_status != 0 ||
        strcmp(s.server_source, "discovered") != 0 || s.cfg_self[0] || s.cfg_pin) {
        return;
    }
    lock();
    uint32_t t = now_ms();
    size_t alternatives = 0;
    for (size_t i = 0; i < s.server_count; i++) {
        bool is_current = strcmp(s.servers[i].host, s.server.host) == 0 && s.servers[i].port == s.server.port;
        bool avoided = s.avoid_until_ms[i] && (int32_t)(s.avoid_until_ms[i] - t) > 0;
        if (!is_current && !avoided) {
            alternatives++;
        }
    }
    if (alternatives == 0) {
        unlock();
        return; /* nothing better to try; the machine's own backoff handles the outage */
    }
    for (size_t i = 0; i < s.server_count; i++) {
        if (strcmp(s.servers[i].host, s.server.host) == 0 && s.servers[i].port == s.server.port) {
            s.avoid_until_ms[i] = at(5 * 60 * 1000);
            ESP_LOGW(TAG, "%s:%u unreachable; trying another server for 5 min", s.server.host, s.server.port);
        }
    }
    unlock();
    select_server();
}

static void run_action(void)
{
    action_t a = s.action;
    s.action = ACT_NONE;
    espos_sk_http_result_t *r = calloc(1, sizeof(*r));
    if (!r) {
        return;
    }
    espos_sk_server_t srv = s.server;
    switch (a) {
    case ACT_REQUEST:
        espos_sk_http_request(&srv, &s.cfg, r);
        espos_sk_tok_event(&s.sm, ESPOS_SK_EV_REQUEST_RESULT, r);
        break;
    case ACT_POLL:
        espos_sk_http_poll(&srv, s.action_href, r);
        espos_sk_tok_event(&s.sm, ESPOS_SK_EV_POLL_RESULT, r);
        break;
    case ACT_VERIFY:
        espos_sk_http_verify(&srv, s.action_token, r);
        espos_sk_tok_event(&s.sm, ESPOS_SK_EV_VERIFY_RESULT, r);
        break;
    default:
        break;
    }
    free(r);
    maybe_rotate_unreachable();
}

static void sk_task(void *arg)
{
    (void)arg;
    espos_sk_tok_store_t *store = calloc(1, sizeof(*store));
    char cid[40];
    espos_sk_store_load(store, cid);
    lock();
    strcpy(s.client_id, cid);
    unlock();
    load_cfg();
    espos_sk_tok_init(&s.sm, &k_port, NULL, &s.cfg, store);
    free(store);
    espos_sk_tok_event(&s.sm, ESPOS_SK_EV_START, NULL);
    s.discover_due_ms = at(2000); /* first pass shortly; re-triggered when WiFi connects */
    select_server();
    p_status_changed(NULL);

    for (;;) {
        /* 0. network came up: discover right away (mDNS is useless before) */
        espos_wifi_status_t ws;
        if (espos_wifi_get_status(&ws) == ESP_OK) {
            bool up = ws.sm.state == ESPOS_WIFI_ST_CONNECTED;
            if (up && !s.wifi_was_connected && s.discovery_enabled) {
                s.discover_due_ms = at(0);
            }
            s.wifi_was_connected = up;
        }
        /* 1. run whatever the machine asked for (blocking HTTP) */
        while (s.action != ACT_NONE) {
            run_action();
        }
        /* 2. timers */
        uint32_t t = now_ms();
        if (s.timer_due_ms && (int32_t)(t - s.timer_due_ms) >= 0) {
            s.timer_due_ms = 0;
            espos_sk_tok_event(&s.sm, ESPOS_SK_EV_TIMER, NULL);
            continue;
        }
        if (s.discovery_enabled && s.discover_due_ms && (int32_t)(t - s.discover_due_ms) >= 0) {
            run_discovery();
            s.discover_due_ms = at(s.discover_interval_ms);
            continue;
        }
        /* 3. wait for a command or the next deadline */
        uint32_t wait = 1000;
        if (s.timer_due_ms) {
            int32_t d = (int32_t)(s.timer_due_ms - t);
            if (d < (int32_t)wait) wait = d > 0 ? (uint32_t)d : 0;
        }
        if (s.discovery_enabled && s.discover_due_ms) {
            int32_t d = (int32_t)(s.discover_due_ms - t);
            if (d < (int32_t)wait) wait = d > 0 ? (uint32_t)d : 0;
        }
        cmd_t c;
        if (xQueueReceive(s.cmds, &c, pdMS_TO_TICKS(wait)) == pdTRUE) {
            if (c.type == CMD_STOP) {
                espos_sk_tok_event(&s.sm, ESPOS_SK_EV_STOP, NULL);
                break;
            }
            handle_cmd(&c);
            free(c.str);
        }
    }
    s.task = NULL;
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------- JSON */

static char *status_json_from(const espos_sk_tok_status_t *st, const char *source)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    uint32_t t = now_ms();
    cJSON *tok = cJSON_AddObjectToObject(root, "token");
    cJSON_AddStringToObject(tok, "state", espos_sk_tok_state_str(st->state));
    cJSON_AddBoolToObject(tok, "has_token", st->has_token);
    cJSON_AddBoolToObject(tok, "busy", st->busy);
    if (st->pending_href[0]) {
        cJSON_AddStringToObject(tok, "pending_href", st->pending_href);
        cJSON_AddNumberToObject(tok, "pending_s", (t - st->requested_since_ms) / 1000);
    }
    if (st->state == ESPOS_SK_TOK_APPROVED) {
        cJSON_AddNumberToObject(tok, "approved_s", (t - st->approved_since_ms) / 1000);
    }
    if (st->next_action_ms) {
        int32_t d = (int32_t)(st->next_action_ms - t);
        cJSON_AddNumberToObject(tok, "next_action_s", d > 0 ? d / 1000 : 0);
    }
    if (st->last_check_ms) {
        cJSON_AddNumberToObject(tok, "last_check_s", (t - st->last_check_ms) / 1000);
    }
    cJSON_AddNumberToObject(tok, "last_http_status", st->last_http_status);
    cJSON_AddStringToObject(tok, "last_error", st->last_error);
    cJSON *cnt = cJSON_AddObjectToObject(tok, "counts");
    cJSON_AddNumberToObject(cnt, "requests", st->request_count);
    cJSON_AddNumberToObject(cnt, "approved", st->approve_count);
    cJSON_AddNumberToObject(cnt, "denied", st->deny_count);
    cJSON_AddNumberToObject(cnt, "unauthorized", st->unauthorized_count);
    cJSON *srv = cJSON_AddObjectToObject(root, "server");
    if (st->has_server) {
        cJSON_AddStringToObject(srv, "host", st->server.host);
        cJSON_AddNumberToObject(srv, "port", st->server.port);
        cJSON_AddStringToObject(srv, "self", st->server.self);
        cJSON_AddStringToObject(srv, "source", source);
        lock();
        for (size_t i = 0; i < s.server_count; i++) {
            if (strcmp(s.servers[i].host, st->server.host) == 0 && s.servers[i].port == st->server.port) {
                cJSON_AddStringToObject(srv, "name", s.servers[i].name);
                cJSON_AddStringToObject(srv, "swname", s.servers[i].swname);
                cJSON_AddStringToObject(srv, "swvers", s.servers[i].swvers);
                break;
            }
        }
        unlock();
    } else {
        cJSON_AddStringToObject(srv, "source", "none");
    }
    char *wsj = espos_sk_ws_status_json();
    if (wsj) {
        cJSON *w = cJSON_Parse(wsj);
        free(wsj);
        if (w) {
            cJSON_AddItemToObject(root, "ws", w);
        }
    }
    cJSON *disc = cJSON_AddObjectToObject(root, "discovery");
    lock();
    cJSON_AddStringToObject(root, "client_id", s.client_id);
    cJSON_AddStringToObject(root, "description", s.cfg.description);
    cJSON_AddStringToObject(root, "permissions", s.cfg.permissions);
    cJSON_AddBoolToObject(disc, "enabled", s.discovery_enabled);
    cJSON_AddNumberToObject(disc, "count", (double)s.server_count);
    if (s.last_discovery_ms) {
        cJSON_AddNumberToObject(disc, "last_s", (t - s.last_discovery_ms) / 1000);
    } else {
        cJSON_AddNullToObject(disc, "last_s");
    }
    unlock();
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return txt;
}

esp_err_t espos_sk_status_json(char **out_json)
{
    if (!out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;
    if (!s.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    espos_sk_tok_status_t st = s.snap;
    char source[12];
    strcpy(source, s.snap_source);
    unlock();
    *out_json = status_json_from(&st, source);
    return *out_json ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t espos_sk_servers_json(char **out_json)
{
    if (!out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;
    if (!s.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *arr = cJSON_AddArrayToObject(root, "servers");
    uint32_t t = now_ms();
    lock();
    for (size_t i = 0; arr && i < s.server_count; i++) {
        const espos_sk_discovered_t *d = &s.servers[i];
        cJSON *e = cJSON_CreateObject();
        if (!e) {
            break;
        }
        cJSON_AddStringToObject(e, "host", d->host);
        cJSON_AddNumberToObject(e, "port", d->port);
        cJSON_AddStringToObject(e, "self", d->self);
        cJSON_AddStringToObject(e, "name", d->name);
        cJSON_AddStringToObject(e, "roles", d->roles);
        cJSON_AddStringToObject(e, "swname", d->swname);
        cJSON_AddStringToObject(e, "swvers", d->swvers);
        cJSON_AddNumberToObject(e, "seen_s", (t - d->seen_ms) / 1000);
        cJSON_AddBoolToObject(e, "selected", s.snap.has_server && strcmp(s.snap.server.host, d->host) == 0 &&
                              s.snap.server.port == d->port);
        cJSON_AddItemToArray(arr, e);
    }
    if (s.last_discovery_ms) {
        cJSON_AddNumberToObject(root, "last_s", (t - s.last_discovery_ms) / 1000);
    } else {
        cJSON_AddNullToObject(root, "last_s");
    }
    unlock();
    *out_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *out_json ? ESP_OK : ESP_ERR_NO_MEM;
}

/* -------------------------------------------------------- commands */

static esp_err_t post_cmd(cmd_type_t type, char *str)
{
    if (!s.cmds) {
        free(str);
        return ESP_ERR_INVALID_STATE;
    }
    cmd_t c = { .type = type, .str = str };
    if (xQueueSend(s.cmds, &c, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(str);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t espos_sk_discover_now(void) { return post_cmd(CMD_DISCOVER, NULL); }
esp_err_t espos_sk_request_now(void) { return post_cmd(CMD_REQUEST, NULL); }
esp_err_t espos_sk_forget_token(void) { return post_cmd(CMD_FORGET, NULL); }
void espos_sk_report_unauthorized(void) { (void)post_cmd(CMD_UNAUTHORIZED, NULL); }

esp_err_t espos_sk_set_token(const char *token)
{
    if (!token || !token[0] || strlen(token) >= ESPOS_SK_TOKEN_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    char *copy = strdup(token);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    return post_cmd(CMD_TOKEN, copy);
}

esp_err_t espos_sk_get_token(char *buf, size_t size)
{
    if (!buf || !size || !s.lock) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    snprintf(buf, size, "%s", s.snap_token);
    unlock();
    return ESP_OK;
}

esp_err_t espos_sk_get_server(espos_sk_server_t *out)
{
    if (!out || !s.lock) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    bool have = s.snap.has_server;
    if (have) {
        *out = s.snap.server;
    }
    unlock();
    return have ? ESP_OK : ESP_ERR_NOT_FOUND;
}

const char *espos_sk_client_id(void)
{
    return s.client_id;
}

/* The stream may run when the token is approved, or when the server has
 * security disabled (OPEN: no token needed). */
bool espos_sk_stream_allowed(void)
{
    if (!s.lock) {
        return false;
    }
    lock();
    bool ok = s.snap.state == ESPOS_SK_TOK_APPROVED || s.snap.state == ESPOS_SK_TOK_OPEN;
    unlock();
    return ok;
}

/* -------------------------------------------------------- lifecycle */

static void on_config_change(const char *ns, const char *key, void *arg)
{
    (void)key;
    (void)arg;
    if (strcmp(ns, ESPOS_CFG_NS_SK) == 0) {
        post_cmd(CMD_CONFIG, NULL);
        espos_sk_ws_config_changed();
    } else if (strcmp(ns, ESPOS_CFG_NS_WIFI) == 0) {
        espos_sk_ws_config_changed(); /* hostname → source label */
    }
}

esp_err_t espos_sk_start(void)
{
    if (s.started) {
        return ESP_OK;
    }
    if (!s.lock) {
        s.lock = xSemaphoreCreateMutex();
        s.cmds = xQueueCreate(8, sizeof(cmd_t));
        if (!s.lock || !s.cmds) {
            return ESP_ERR_NO_MEM;
        }
    }
    /* hostname for the request description / mDNS advertisement */
    char h[33] = { 0 };
    espos_config_get_str(ESPOS_CFG_NS_WIFI, ESPOS_CFG_WIFI_HOSTNAME, h, sizeof(h), NULL);
    if (h[0]) {
        snprintf(s.hostname, sizeof(s.hostname), "%s", h);
    } else {
        /* espos_wifi derives "espos-<mac4>"; keep in step by asking it */
        extern const char *espos_wifi_short_id(void);
        snprintf(s.hostname, sizeof(s.hostname), "espos-%s", espos_wifi_short_id());
    }
    espos_sk_discovery_init(s.hostname);
    if (!s.api_registered) {
        ESP_ERROR_CHECK(espos_sk_register_api());
        s.api_registered = true;
    }
    espos_config_subscribe(on_config_change, NULL);
    s.started = true;
    if (xTaskCreate(sk_task, "espos_sk", 12288, NULL, tskIDLE_PRIORITY + 3, &s.task) != pdPASS) {
        s.started = false;
        return ESP_ERR_NO_MEM;
    }
    return espos_sk_ws_start();
}

esp_err_t espos_sk_stop(void)
{
    if (!s.started) {
        return ESP_OK;
    }
    espos_config_unsubscribe(on_config_change, NULL);
    espos_sk_ws_stop();
    post_cmd(CMD_STOP, NULL);
    for (int i = 0; i < 100 && s.task; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s.started = false;
    return ESP_OK;
}

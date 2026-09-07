/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * The default-route machine (espos_net_sm.h) and the shared backoff curve.
 * Pure C: no netif, no driver, no lock — espos_net.c serialises calls, the
 * host test drives it directly.
 */
#include <stdio.h>
#include <string.h>

#include "espos_net_sm.h"

/* Wired beats wireless beats mesh: an Ethernet link is the one a boat's
 * installer ran a cable for, the WiFi station is the fallback, and a Thread
 * border route is the last resort because it is the slowest and the least
 * likely to reach a SignalK server directly. The rule is deliberately static
 * — no metric, no probing — so a consumer can predict it from the docs. */
static const espos_net_if_t k_preference[] = { ESPOS_NET_IF_ETH, ESPOS_NET_IF_WIFI_STA, ESPOS_NET_IF_THREAD };

static uint32_t now(const espos_net_sm_t *sm)
{
    return sm->port->now_ms(sm->ctx);
}

static void copy_ip(char dst[ESPOS_NET_IP_MAX], const char *src)
{
    snprintf(dst, ESPOS_NET_IP_MAX, "%s", src ? src : "");
}

const char *espos_net_if_str(espos_net_if_t iface)
{
    switch (iface) {
    case ESPOS_NET_IF_WIFI_STA: return "wifi_sta";
    case ESPOS_NET_IF_ETH: return "eth";
    case ESPOS_NET_IF_THREAD: return "thread";
    default: return "none";
    }
}

uint32_t espos_net_backoff_ms(uint32_t round, uint32_t cap_ms, uint32_t rnd)
{
    uint64_t d = 1000;
    for (uint32_t i = 0; i < round && d < cap_ms; i++) {
        d *= 2;
    }
    if (d > cap_ms) {
        d = cap_ms;
    }
    /* ±25 % jitter, never below 250 ms */
    uint32_t span = (uint32_t)(d / 2);          /* 50 % window */
    uint32_t j = span ? (rnd % (span + 1)) : 0; /* 0..span */
    uint64_t out = d - span / 2 + j;            /* d·0.75 .. d·1.25 */
    if (out < 250) {
        out = 250;
    }
    return (uint32_t)out;
}

void espos_net_sm_init(espos_net_sm_t *sm, const espos_net_sm_port_t *port, void *ctx)
{
    memset(sm, 0, sizeof(*sm));
    sm->port = port;
    sm->ctx = ctx;
    sm->active = ESPOS_NET_IF_NONE;
}

espos_net_if_t espos_net_sm_select(const espos_net_sm_t *sm)
{
    for (size_t i = 0; i < sizeof(k_preference) / sizeof(k_preference[0]); i++) {
        if (sm->ifs[k_preference[i]].up) {
            return k_preference[i];
        }
    }
    return ESPOS_NET_IF_NONE;
}

espos_net_edge_t espos_net_sm_report(espos_net_sm_t *sm, espos_net_if_t iface, bool up, const char *ip,
                                     const char *netmask, const char *gateway, int8_t rssi, bool *changed)
{
    if (changed) {
        *changed = false;
    }
    if (iface <= ESPOS_NET_IF_NONE || iface >= ESPOS_NET_IF_MAX) {
        return ESPOS_NET_EDGE_NONE;
    }
    espos_net_sm_if_t *cur = &sm->ifs[iface];
    espos_net_sm_if_t next = { .up = up };
    if (up) {
        copy_ip(next.ip, ip);
        copy_ip(next.netmask, netmask);
        copy_ip(next.gateway, gateway);
        next.rssi = rssi;
        /* keep the original up time across address and RSSI refreshes */
        next.up_since_ms = cur->up ? cur->up_since_ms : now(sm);
    }
    bool same_route = cur->up == next.up && strcmp(cur->ip, next.ip) == 0 && strcmp(cur->netmask, next.netmask) == 0 &&
                      strcmp(cur->gateway, next.gateway) == 0;
    if (same_route && cur->rssi == next.rssi) {
        return ESPOS_NET_EDGE_NONE; /* the transport repeated itself */
    }
    *cur = next;
    if (changed) {
        *changed = true;
    }

    espos_net_if_t was = sm->active;
    espos_net_if_t sel = espos_net_sm_select(sm);
    if (was == ESPOS_NET_IF_NONE && sel != ESPOS_NET_IF_NONE) {
        sm->active = sel;
        sm->up_count++;
        sm->up_since_ms = now(sm);
        return ESPOS_NET_EDGE_UP;
    }
    if (was != ESPOS_NET_IF_NONE && sel == ESPOS_NET_IF_NONE) {
        sm->active = ESPOS_NET_IF_NONE;
        sm->up_since_ms = 0;
        return ESPOS_NET_EDGE_DOWN;
    }
    if (was != sel) {
        /* a better interface came up, or the one we used went away and a
         * lesser one takes over: sockets bound to the old address are dead
         * either way, so consumers see DOWN then UP */
        sm->active = sel;
        sm->up_count++;
        sm->up_since_ms = now(sm);
        return ESPOS_NET_EDGE_CHANGED;
    }
    if (iface == sm->active && !same_route) {
        /* same interface, new address (a DHCP lease that came back different) */
        sm->up_count++;
        sm->up_since_ms = now(sm);
        return ESPOS_NET_EDGE_CHANGED;
    }
    /* an RSSI refresh on the active interface, or anything on a standby one */
    return ESPOS_NET_EDGE_NONE;
}

void espos_net_sm_status(const espos_net_sm_t *sm, espos_net_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->iface = sm->active;
    out->up_count = sm->up_count;
    if (sm->active == ESPOS_NET_IF_NONE) {
        return;
    }
    const espos_net_sm_if_t *a = &sm->ifs[sm->active];
    out->up = true;
    strcpy(out->ip, a->ip);
    strcpy(out->netmask, a->netmask);
    strcpy(out->gateway, a->gateway);
    /* RSSI is a WiFi notion; a wired transport reporting one would only confuse a status line */
    out->rssi = sm->active == ESPOS_NET_IF_WIFI_STA ? a->rssi : 0;
    out->up_since_ms = now(sm) - sm->up_since_ms;
}

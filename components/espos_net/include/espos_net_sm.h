/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_net default-route machine — pure C, no netif or driver calls. One
 * record per interface, one rule for which of them carries the default route
 * (Ethernet over WiFi station over Thread), and the edge a report produced:
 * came up, went down, or moved. The clock is injected so the machine runs
 * unchanged on the host (test/host/espos_net_test); espos_net.c owns the one
 * instance a device runs and turns edges into events, callbacks and SSE.
 *
 * Threading: the caller serialises every call (espos_net.c holds its mutex);
 * the port's now_ms runs on the caller's task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#include "espos_net.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What one report did to the default route. Values are ABI. */
typedef enum {
    ESPOS_NET_EDGE_NONE = 0,    /* nothing, or a change on a non-default interface, or an RSSI refresh */
    ESPOS_NET_EDGE_UP = 1,      /* no route → a route */
    ESPOS_NET_EDGE_DOWN = 2,    /* a route → none */
    ESPOS_NET_EDGE_CHANGED = 3, /* still up, but on another interface or another address */
    ESPOS_NET_EDGE_MAX = 4,
} espos_net_edge_t;

/* One interface as last reported. */
typedef struct {
    bool up;
    char ip[ESPOS_NET_IP_MAX];
    char netmask[ESPOS_NET_IP_MAX];
    char gateway[ESPOS_NET_IP_MAX];
    int8_t rssi;
    uint32_t up_since_ms; /* port clock when this interface last came up */
} espos_net_sm_if_t;

/* Everything the machine needs from the outside world. */
typedef struct {
    uint32_t (*now_ms)(void *ctx); /* monotonic, wraps; differences only */
} espos_net_sm_port_t;

typedef struct espos_net_sm {
    const espos_net_sm_port_t *port;
    void *ctx;
    espos_net_sm_if_t ifs[ESPOS_NET_IF_MAX]; /* indexed by espos_net_if_t; [NONE] unused */
    espos_net_if_t active;                  /* the interface carrying the default route */
    uint32_t up_count;                      /* UP and CHANGED edges since init */
    uint32_t up_since_ms;                   /* port clock of the last UP/CHANGED edge */
} espos_net_sm_t;

void espos_net_sm_init(espos_net_sm_t *sm, const espos_net_sm_port_t *port, void *ctx);

/**
 * Record a transport's report and re-select the default route. Returns the
 * edge; `changed` (optional) is set when any stored field differs from before
 * — true for an RSSI refresh too, which is not an edge but is worth a status
 * push. NULL strings read as "". An iface of NONE or ≥ MAX is ignored (NONE
 * edge, no change).
 */
espos_net_edge_t espos_net_sm_report(espos_net_sm_t *sm, espos_net_if_t iface, bool up, const char *ip,
                                     const char *netmask, const char *gateway, int8_t rssi, bool *changed);

/**
 * The route half of espos_net_status_t: up, iface, addresses, rssi (0 unless
 * the route is the WiFi station), up_count, up_since_ms as a duration. The
 * identity fields (mac, hostname, ip6_ll) are zeroed; espos_net.c fills them.
 */
void espos_net_sm_status(const espos_net_sm_t *sm, espos_net_status_t *out);

/** The preference rule on its own — which up interface would carry the route.
 * Exposed for tests. */
espos_net_if_t espos_net_sm_select(const espos_net_sm_t *sm);

#ifdef __cplusplus
}
#endif

/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_mdns — the device's mDNS responder, and the one place a component or
 * an application registers a service it wants found on the LAN.
 *
 * espOS names the device <net.hostname>.local and advertises
 *
 *   _http._tcp   on httpd.port   TXT path=/
 *   _espos._tcp  on httpd.port   TXT v=<app version> app=<app name>
 *                                    espos=<espOS version> target=<chip>
 *                                    id=<espos_net_short_id()> api=/api/v1 auth=0
 *
 * so a browser finds every espOS device with one query and knows what it is
 * talking to before it fetches anything. Anything else — a SignalK player, a
 * candump server — is added with espos_mdns_add_service(); that call may be
 * made at any time, before the network or the responder exists, and the entry
 * is kept and registered when they do.
 *
 * Lives in espos_net: the responder follows whatever interface carries the
 * default route (docs/net.md), so it is the same API on a WiFi, Ethernet or
 * Thread build. It moved here from espos_wifi unchanged; espos_wifi still
 * REQUIRES espos_net, so a consumer that found it through espos_wifi's
 * include path still does.
 *
 * Threading: espos_mdns_start(), espos_mdns_add_service() and
 * espos_mdns_remove_service() run on the caller's task and MAY BLOCK for a
 * few milliseconds on the responder's own task or lock — call them from an
 * application task or from app_main(), never from an ESPOS_EVENT handler or a
 * URI handler. espos_mdns_is_ready() only takes the table mutex.
 *
 * Readiness is also published as ESPOS_EVENT_MDNS_READY (espos_event.h):
 * posted on every ESPOS_EVENT_NETWORK_UP once the responder runs, and once
 * from espos_mdns_start() when the link is already up. Subscribe to that, or
 * poll espos_mdns_is_ready(); both mean "a query or an announcement can reach
 * the network now".
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Limits of one registered service; a request beyond them is refused, never
 * truncated. Values are part of the ABI. */
#define ESPOS_MDNS_TYPE_MAX      32   /* service type incl. NUL: "_signalk-player" */
#define ESPOS_MDNS_PROTO_MAX     8    /* "_tcp" | "_udp" */
#define ESPOS_MDNS_TXT_MAX_ITEMS 8    /* "k=v" items per service */
#define ESPOS_MDNS_TXT_MAX_BYTES 256  /* all keys + values of one service, NULs included */

/**
 * Bring the responder up: hostname, instance name, the built-in services
 * above, then every service queued with espos_mdns_add_service(). Requires
 * espos_net_start() (netif layer, event loop, hostname); ESP_ERR_INVALID_STATE
 * with one log line otherwise. Idempotent. espos_net_start() calls it, so an
 * application never needs to — the call exists for firmware that drives the
 * start sequence by hand.
 *
 * ESP_ERR_NOT_SUPPORTED when built without CONFIG_ESPOS_NET_MDNS or on the
 * linux target: the device is then not advertised, nothing else is affected.
 */
esp_err_t espos_mdns_start(void);

/**
 * Advertise `type`.`proto` (e.g. "_signalk-player", "_tcp") on `port` with the
 * TXT items `txt_kv` = {"k=v", "flag", ...} (n_txt of them, 0 and NULL for
 * none). Strings are copied. Callable any time: before the responder exists
 * the entry waits in a table of CONFIG_ESPOS_NET_MDNS_MAX_SERVICES slots and
 * is registered by espos_mdns_start(); afterwards it is registered at once.
 * Adding a (type, proto) that is already in the table replaces its port and
 * TXT (the old record is withdrawn, the new one announced).
 *
 * ESP_ERR_INVALID_ARG for a malformed type/proto/item, ESP_ERR_INVALID_SIZE
 * when the TXT items do not fit the limits above, ESP_ERR_NO_MEM when the
 * table is full, and the responder's own error (also ESP_ERR_NO_MEM once
 * CONFIG_MDNS_MAX_SERVICES is reached) when it refuses the record — in that
 * case the entry is dropped, not queued.
 */
esp_err_t espos_mdns_add_service(const char *type, const char *proto, uint16_t port, const char *const *txt_kv, size_t n_txt);

/** Withdraw a service added with espos_mdns_add_service(). ESP_ERR_NOT_FOUND
 * when no such (type, proto) was added through this API; the built-in
 * services cannot be removed. */
esp_err_t espos_mdns_remove_service(const char *type, const char *proto);

/** True while the responder runs and the default route is up: a query
 * (mdns_query_ptr) or an announcement can reach the network. Drops on
 * ESPOS_EVENT_NETWORK_DOWN. Always false when built without the responder. */
bool espos_mdns_is_ready(void);

#ifdef __cplusplus
}
#endif

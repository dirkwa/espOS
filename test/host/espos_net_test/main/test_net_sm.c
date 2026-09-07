/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * Default-route machine tests: which interface carries the route when several
 * report, what edge each report produces, what the merged status says, and
 * the backoff curve that moved here from espos_wifi (same vectors as
 * test/host/espos_wifi_test, so the two stay provably identical until the
 * WiFi copy is removed in 0.9).
 */
#include <string.h>
#include "unity.h"
#include "espos_net_sm.h"

static uint32_t F_now;

static uint32_t f_now(void *ctx)
{
    (void)ctx;
    return F_now;
}

static const espos_net_sm_port_t k_port = { .now_ms = f_now };
static espos_net_sm_t sm;

static void fresh(void)
{
    F_now = 1000;
    espos_net_sm_init(&sm, &k_port, NULL);
}

static espos_net_edge_t up(espos_net_if_t i, const char *ip, int8_t rssi, bool *changed)
{
    return espos_net_sm_report(&sm, i, true, ip, "255.255.255.0", "10.0.0.1", rssi, changed);
}

static espos_net_edge_t down(espos_net_if_t i, bool *changed)
{
    return espos_net_sm_report(&sm, i, false, NULL, NULL, NULL, 0, changed);
}

/* ------------------------------------------------------- initial state */

TEST_CASE("fresh machine: no route, empty status", "[net_sm]")
{
    fresh();
    espos_net_status_t st;
    espos_net_sm_status(&sm, &st);
    TEST_ASSERT_FALSE(st.up);
    TEST_ASSERT_EQUAL(ESPOS_NET_IF_NONE, st.iface);
    TEST_ASSERT_EQUAL_STRING("", st.ip);
    TEST_ASSERT_EQUAL_STRING("", st.netmask);
    TEST_ASSERT_EQUAL_STRING("", st.gateway);
    TEST_ASSERT_EQUAL(0, st.rssi);
    TEST_ASSERT_EQUAL_UINT32(0, st.up_count);
    TEST_ASSERT_EQUAL_UINT32(0, st.up_since_ms);
    TEST_ASSERT_EQUAL(ESPOS_NET_IF_NONE, espos_net_sm_select(&sm));
}

/* -------------------------------------------------------------- edges */

TEST_CASE("wifi up: UP edge, status carries its addresses", "[net_sm]")
{
    fresh();
    bool changed = false;
    TEST_ASSERT_EQUAL(ESPOS_NET_EDGE_UP, up(ESPOS_NET_IF_WIFI_STA, "10.0.0.2", -55, &changed));
    TEST_ASSERT_TRUE(changed);
    espos_net_status_t st;
    F_now = 6000;
    espos_net_sm_status(&sm, &st);
    TEST_ASSERT_TRUE(st.up);
    TEST_ASSERT_EQUAL(ESPOS_NET_IF_WIFI_STA, st.iface);
    TEST_ASSERT_EQUAL_STRING("10.0.0.2", st.ip);
    TEST_ASSERT_EQUAL_STRING("255.255.255.0", st.netmask);
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", st.gateway);
    TEST_ASSERT_EQUAL(-55, st.rssi);
    TEST_ASSERT_EQUAL_UINT32(1, st.up_count);
    TEST_ASSERT_EQUAL_UINT32(5000, st.up_since_ms); /* a duration, from the injected clock */
}

TEST_CASE("wifi down: DOWN edge, status cleared, up_count kept", "[net_sm]")
{
    fresh();
    up(ESPOS_NET_IF_WIFI_STA, "10.0.0.2", -55, NULL);
    bool changed = false;
    TEST_ASSERT_EQUAL(ESPOS_NET_EDGE_DOWN, down(ESPOS_NET_IF_WIFI_STA, &changed));
    TEST_ASSERT_TRUE(changed);
    espos_net_status_t st;
    espos_net_sm_status(&sm, &st);
    TEST_ASSERT_FALSE(st.up);
    TEST_ASSERT_EQUAL(ESPOS_NET_IF_NONE, st.iface);
    TEST_ASSERT_EQUAL_STRING("", st.ip);
    TEST_ASSERT_EQUAL(0, st.rssi);
    TEST_ASSERT_EQUAL_UINT32(1, st.up_count);
    TEST_ASSERT_EQUAL_UINT32(0, st.up_since_ms);
}

TEST_CASE("repeating the same report is not an edge and not a change", "[net_sm]")
{
    fresh();
    up(ESPOS_NET_IF_WIFI_STA, "10.0.0.2", -55, NULL);
    bool changed = true;
    TEST_ASSERT_EQUAL(ESPOS_NET_EDGE_NONE, up(ESPOS_NET_IF_WIFI_STA, "10.0.0.2", -55, &changed));
    TEST_ASSERT_FALSE(changed);
    /* and a "down" for an interface that is already down */
    fresh();
    changed = true;
    TEST_ASSERT_EQUAL(ESPOS_NET_EDGE_NONE, down(ESPOS_NET_IF_WIFI_STA, &changed));
    TEST_ASSERT_FALSE(changed);
    espos_net_status_t st;
    espos_net_sm_status(&sm, &st);
    TEST_ASSERT_EQUAL_UINT32(0, st.up_count);
}

TEST_CASE("RSSI refresh on the route: a change but no edge", "[net_sm]")
{
    fresh();
    up(ESPOS_NET_IF_WIFI_STA, "10.0.0.2", -55, NULL);
    bool changed = false;
    TEST_ASSERT_EQUAL(ESPOS_NET_EDGE_NONE, up(ESPOS_NET_IF_WIFI_STA, "10.0.0.2", -61, &changed));
    TEST_ASSERT_TRUE(changed);
    espos_net_status_t st;
    espos_net_sm_status(&sm, &st);
    TEST_ASSERT_EQUAL(-61, st.rssi);
    TEST_ASSERT_EQUAL_UINT32(1, st.up_count); /* the route never went anywhere */
}

TEST_CASE("new address on the same interface: CHANGED, up_count and clock restart", "[net_sm]")
{
    fresh();
    up(ESPOS_NET_IF_WIFI_STA, "10.0.0.2", -55, NULL);
    F_now = 20000;
    TEST_ASSERT_EQUAL(ESPOS_NET_EDGE_CHANGED, up(ESPOS_NET_IF_WIFI_STA, "10.0.0.9", -55, NULL));
    espos_net_status_t st;
    F_now = 21000;
    espos_net_sm_status(&sm, &st);
    TEST_ASSERT_EQUAL_STRING("10.0.0.9", st.ip);
    TEST_ASSERT_EQUAL_UINT32(2, st.up_count);
    TEST_ASSERT_EQUAL_UINT32(1000, st.up_since_ms);
}

TEST_CASE("two up/down cycles count two ups", "[net_sm]")
{
    fresh();
    up(ESPOS_NET_IF_WIFI_STA, "10.0.0.2", -55, NULL);
    down(ESPOS_NET_IF_WIFI_STA, NULL);
    TEST_ASSERT_EQUAL(ESPOS_NET_EDGE_UP, up(ESPOS_NET_IF_WIFI_STA, "10.0.0.2", -50, NULL));
    espos_net_status_t st;
    espos_net_sm_status(&sm, &st);
    TEST_ASSERT_EQUAL_UINT32(2, st.up_count);
}

TEST_CASE("an invalid interface is ignored", "[net_sm]")
{
    fresh();
    bool changed = true;
    TEST_ASSERT_EQUAL(ESPOS_NET_EDGE_NONE, up(ESPOS_NET_IF_NONE, "10.0.0.2", 0, &changed));
    TEST_ASSERT_FALSE(changed);
    changed = true;
    TEST_ASSERT_EQUAL(ESPOS_NET_EDGE_NONE, up(ESPOS_NET_IF_MAX, "10.0.0.2", 0, &changed));
    TEST_ASSERT_FALSE(changed);
    TEST_ASSERT_EQUAL(ESPOS_NET_IF_NONE, espos_net_sm_select(&sm));
}

TEST_CASE("NULL strings read as empty", "[net_sm]")
{
    fresh();
    TEST_ASSERT_EQUAL(ESPOS_NET_EDGE_UP, espos_net_sm_report(&sm, ESPOS_NET_IF_ETH, true, "192.168.1.5", "255.255.255.0", NULL, 0, NULL));
    espos_net_status_t st;
    espos_net_sm_status(&sm, &st);
    TEST_ASSERT_EQUAL_STRING("192.168.1.5", st.ip);
    TEST_ASSERT_EQUAL_STRING("", st.gateway);
}

/* ------------------------------------------------ default-route selection */

TEST_CASE("Ethernet arriving while WiFi carries the route: CHANGED to eth", "[net_sm]")
{
    fresh();
    up(ESPOS_NET_IF_WIFI_STA, "10.0.0.2", -55, NULL);
    F_now = 5000;
    TEST_ASSERT_EQUAL(ESPOS_NET_EDGE_CHANGED, up(ESPOS_NET_IF_ETH, "192.168.1.5", 0, NULL));
    espos_net_status_t st;
    espos_net_sm_status(&sm, &st);
    TEST_ASSERT_EQUAL(ESPOS_NET_IF_ETH, st.iface);
    TEST_ASSERT_EQUAL_STRING("192.168.1.5", st.ip);
    TEST_ASSERT_EQUAL_UINT32(2, st.up_count);
}

TEST_CASE("Ethernet lost while WiFi is still up: CHANGED back to wifi, not DOWN", "[net_sm]")
{
    fresh();
    up(ESPOS_NET_IF_WIFI_STA, "10.0.0.2", -55, NULL);
    up(ESPOS_NET_IF_ETH, "192.168.1.5", 0, NULL);
    TEST_ASSERT_EQUAL(ESPOS_NET_EDGE_CHANGED, down(ESPOS_NET_IF_ETH, NULL));
    espos_net_status_t st;
    espos_net_sm_status(&sm, &st);
    TEST_ASSERT_TRUE(st.up);
    TEST_ASSERT_EQUAL(ESPOS_NET_IF_WIFI_STA, st.iface);
    TEST_ASSERT_EQUAL_STRING("10.0.0.2", st.ip);
    TEST_ASSERT_EQUAL(-55, st.rssi);
    /* and only when the last one goes does the route go */
    TEST_ASSERT_EQUAL(ESPOS_NET_EDGE_DOWN, down(ESPOS_NET_IF_WIFI_STA, NULL));
}

TEST_CASE("WiFi arriving while Thread carries the route takes over; Thread stays standby", "[net_sm]")
{
    fresh();
    TEST_ASSERT_EQUAL(ESPOS_NET_EDGE_UP, up(ESPOS_NET_IF_THREAD, "fdde::1", 0, NULL));
    TEST_ASSERT_EQUAL(ESPOS_NET_IF_THREAD, espos_net_sm_select(&sm));
    TEST_ASSERT_EQUAL(ESPOS_NET_EDGE_CHANGED, up(ESPOS_NET_IF_WIFI_STA, "10.0.0.2", -60, NULL));
    TEST_ASSERT_EQUAL(ESPOS_NET_IF_WIFI_STA, espos_net_sm_select(&sm));
}

TEST_CASE("all three up: eth wins; preference is static", "[net_sm]")
{
    fresh();
    up(ESPOS_NET_IF_THREAD, "fdde::1", 0, NULL);
    up(ESPOS_NET_IF_WIFI_STA, "10.0.0.2", -60, NULL);
    up(ESPOS_NET_IF_ETH, "192.168.1.5", 0, NULL);
    TEST_ASSERT_EQUAL(ESPOS_NET_IF_ETH, espos_net_sm_select(&sm));
    /* the order they came up in does not matter */
    fresh();
    up(ESPOS_NET_IF_ETH, "192.168.1.5", 0, NULL);
    up(ESPOS_NET_IF_WIFI_STA, "10.0.0.2", -60, NULL);
    up(ESPOS_NET_IF_THREAD, "fdde::1", 0, NULL);
    TEST_ASSERT_EQUAL(ESPOS_NET_IF_ETH, espos_net_sm_select(&sm));
}

TEST_CASE("a standby interface changing is neither an edge nor the status", "[net_sm]")
{
    fresh();
    up(ESPOS_NET_IF_ETH, "192.168.1.5", 0, NULL);
    up(ESPOS_NET_IF_WIFI_STA, "10.0.0.2", -60, NULL); /* standby: eth is preferred */
    bool changed = false;
    TEST_ASSERT_EQUAL(ESPOS_NET_EDGE_NONE, up(ESPOS_NET_IF_WIFI_STA, "10.0.0.7", -40, &changed));
    TEST_ASSERT_TRUE(changed); /* recorded, for when it takes over */
    espos_net_status_t st;
    espos_net_sm_status(&sm, &st);
    TEST_ASSERT_EQUAL(ESPOS_NET_IF_ETH, st.iface);
    TEST_ASSERT_EQUAL_STRING("192.168.1.5", st.ip);
    TEST_ASSERT_EQUAL_UINT32(1, st.up_count);
    /* eth goes: the recorded standby state is what takes over */
    TEST_ASSERT_EQUAL(ESPOS_NET_EDGE_CHANGED, down(ESPOS_NET_IF_ETH, NULL));
    espos_net_sm_status(&sm, &st);
    TEST_ASSERT_EQUAL_STRING("10.0.0.7", st.ip);
    TEST_ASSERT_EQUAL(-40, st.rssi);
}

TEST_CASE("rssi is 0 on a non-WiFi route even if the transport reported one", "[net_sm]")
{
    fresh();
    up(ESPOS_NET_IF_ETH, "192.168.1.5", -30, NULL);
    espos_net_status_t st;
    espos_net_sm_status(&sm, &st);
    TEST_ASSERT_EQUAL(0, st.rssi);
}

TEST_CASE("interface names as the REST document spells them", "[net_sm]")
{
    TEST_ASSERT_EQUAL_STRING("none", espos_net_if_str(ESPOS_NET_IF_NONE));
    TEST_ASSERT_EQUAL_STRING("wifi_sta", espos_net_if_str(ESPOS_NET_IF_WIFI_STA));
    TEST_ASSERT_EQUAL_STRING("eth", espos_net_if_str(ESPOS_NET_IF_ETH));
    TEST_ASSERT_EQUAL_STRING("thread", espos_net_if_str(ESPOS_NET_IF_THREAD));
    TEST_ASSERT_EQUAL_STRING("none", espos_net_if_str(ESPOS_NET_IF_MAX));
}

/* ------------------------------------------------------------- backoff */

TEST_CASE("backoff curve moved from espos_wifi: same vectors", "[net_sm]")
{
    TEST_ASSERT_EQUAL_UINT32(750, espos_net_backoff_ms(0, 60000, 0));        /* 1 s · 0.75 */
    TEST_ASSERT_EQUAL_UINT32(1250, espos_net_backoff_ms(0, 60000, 500));     /* 1 s · 1.25 */
    TEST_ASSERT_EQUAL_UINT32(1500, espos_net_backoff_ms(1, 60000, 0));       /* 2 s · 0.75 */
    TEST_ASSERT_EQUAL_UINT32(6000, espos_net_backoff_ms(3, 60000, 0));       /* 8 s · 0.75 */
    TEST_ASSERT_EQUAL_UINT32(45000, espos_net_backoff_ms(20, 60000, 0));     /* capped at 60 s · 0.75 */
    TEST_ASSERT_EQUAL_UINT32(75000, espos_net_backoff_ms(20, 60000, 30000)); /* 60 s · 1.25 */
    for (uint32_t r = 0; r < 8; r++) {
        for (uint32_t j = 0; j < 5000; j += 137) {
            uint32_t d = espos_net_backoff_ms(r, 60000, j);
            uint32_t base = (1000u << r) > 60000 ? 60000 : (1000u << r);
            TEST_ASSERT_TRUE(d >= base * 3 / 4 && d <= base * 5 / 4);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(250, espos_net_backoff_ms(0, 100, 0)); /* floor */
}

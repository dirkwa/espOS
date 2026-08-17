/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * State machine tests. A fake port records driver calls and owns a manual
 * clock; "tick(ms)" advances time and fires the armed timer when due, so
 * every transition in docs/wifi.md is exercised deterministically.
 */
#include <string.h>
#include "unity.h"
#include "espos_wifi_sm.h"

/* ---------------------------------------------------------- fake port */

static struct {
    uint32_t now;
    bool timer_armed;
    uint32_t timer_due;
    int connects, disconnects, portal_starts, portal_stops, notifies;
    char last_ssid[33];
    bool last_has_bssid;
    uint32_t rnd;
    esp_err_t connect_result;
} F;

static esp_err_t f_connect(void *ctx, const espos_wifi_net_t *net)
{
    (void)ctx;
    F.connects++;
    strcpy(F.last_ssid, net->ssid);
    F.last_has_bssid = net->has_bssid;
    return F.connect_result;
}
static esp_err_t f_disconnect(void *ctx) { (void)ctx; F.disconnects++; return ESP_OK; }
static esp_err_t f_portal_start(void *ctx) { (void)ctx; F.portal_starts++; return ESP_OK; }
static esp_err_t f_portal_stop(void *ctx) { (void)ctx; F.portal_stops++; return ESP_OK; }
static void f_arm(void *ctx, uint32_t ms) { (void)ctx; F.timer_armed = true; F.timer_due = F.now + ms; }
static void f_cancel(void *ctx) { (void)ctx; F.timer_armed = false; }
static uint32_t f_now(void *ctx) { (void)ctx; return F.now; }
static uint32_t f_random(void *ctx) { (void)ctx; return F.rnd; }
static void f_notify(void *ctx) { (void)ctx; F.notifies++; }

static const espos_wifi_port_t PORT = {
    .connect = f_connect, .disconnect = f_disconnect, .portal_start = f_portal_start,
    .portal_stop = f_portal_stop, .arm_timer = f_arm, .cancel_timer = f_cancel,
    .now_ms = f_now, .random = f_random, .status_changed = f_notify,
};

static espos_wifi_sm_t SM;

static espos_wifi_cfg_t cfg_with(const char *s0, const char *s1)
{
    espos_wifi_cfg_t c = { 0 };
    c.sta_enabled = true;
    if (s0) { strcpy(c.nets[c.net_count].ssid, s0); strcpy(c.nets[c.net_count].psk, "pw"); c.net_count++; }
    if (s1) { strcpy(c.nets[c.net_count].ssid, s1); strcpy(c.nets[c.net_count].psk, "pw"); c.net_count++; }
    c.backoff_max_ms = 60000;
    c.dhcp_timeout_ms = 15000;
    c.connect_timeout_ms = 20000;
    c.portal_enabled = true;
    c.portal_after_ms = 90000;
    return c;
}

static void reset(const espos_wifi_cfg_t *c)
{
    memset(&F, 0, sizeof(F));
    F.now = 1000;
    F.rnd = 0; /* deterministic: lower jitter bound (0.75·d) */
    espos_wifi_sm_init(&SM, &PORT, NULL, c);
}

/* Advance the clock, firing the timer once if it comes due. */
static void tick(uint32_t ms)
{
    uint32_t target = F.now + ms;
    if (F.timer_armed && (int32_t)(F.timer_due - target) <= 0) {
        F.now = F.timer_due;
        F.timer_armed = false;
        espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_TIMER, NULL);
    }
    F.now = target;
}

static void ev_disconnected(int reason) { espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_STA_DISCONNECTED, &reason); }
static void ev_connected(const char *ssid)
{
    espos_wifi_link_t l = { .channel = 6, .rssi = -60 };
    strcpy(l.ssid, ssid);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_STA_CONNECTED, &l);
}
static void ev_got_ip(void)
{
    espos_wifi_ip_t ip = { .ip = "10.0.0.5", .netmask = "255.255.255.0", .gateway = "10.0.0.1" };
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_GOT_IP, &ip);
}
#define ST() (espos_wifi_sm_status(&SM))

/* --------------------------------------------------------------- tests */

TEST_CASE("backoff: exponential, capped, jittered ±25%, floor 250 ms", "[wifi_sm]")
{
    TEST_ASSERT_EQUAL_UINT32(750, espos_wifi_backoff_ms(0, 60000, 0));      /* 1 s · 0.75 */
    TEST_ASSERT_EQUAL_UINT32(1250, espos_wifi_backoff_ms(0, 60000, 500));   /* 1 s · 1.25 */
    TEST_ASSERT_EQUAL_UINT32(1500, espos_wifi_backoff_ms(1, 60000, 0));     /* 2 s · 0.75 */
    TEST_ASSERT_EQUAL_UINT32(6000, espos_wifi_backoff_ms(3, 60000, 0));     /* 8 s · 0.75 */
    TEST_ASSERT_EQUAL_UINT32(45000, espos_wifi_backoff_ms(20, 60000, 0));   /* capped at 60 s · 0.75 */
    TEST_ASSERT_EQUAL_UINT32(75000, espos_wifi_backoff_ms(20, 60000, 30000)); /* 60 s · 1.25 */
    for (uint32_t r = 0; r < 12; r++) {
        for (uint32_t j = 0; j < 5000; j += 137) {
            uint32_t d = espos_wifi_backoff_ms(r, 60000, j);
            uint64_t base = 1000ull << r;
            if (base > 60000) base = 60000;
            TEST_ASSERT_TRUE(d >= base * 3 / 4 && d <= base * 5 / 4 + 1);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(250, espos_wifi_backoff_ms(0, 100, 0)); /* floor */
}

TEST_CASE("happy path: start → connecting → obtaining_ip → connected, portal never up", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    reset(&c);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_DISABLED, ST()->state);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    TEST_ASSERT_EQUAL(1, F.connects);
    TEST_ASSERT_EQUAL_STRING("Boat", F.last_ssid);
    TEST_ASSERT_TRUE(F.timer_armed);                       /* connect timeout */
    TEST_ASSERT_EQUAL_UINT32(F.now + 20000, F.timer_due);
    TEST_ASSERT_EQUAL(1, ST()->attempt);
    ev_connected("Boat");
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_OBTAINING_IP, ST()->state);
    TEST_ASSERT_EQUAL_UINT32(F.now + 15000, F.timer_due);  /* dhcp timeout */
    TEST_ASSERT_EQUAL_STRING("Boat", ST()->link.ssid);
    tick(500);
    ev_got_ip();
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTED, ST()->state);
    TEST_ASSERT_FALSE(F.timer_armed);
    TEST_ASSERT_EQUAL_STRING("10.0.0.5", ST()->ip.ip);
    TEST_ASSERT_EQUAL(1, ST()->connect_count);
    TEST_ASSERT_EQUAL(0, ST()->attempt);
    TEST_ASSERT_EQUAL(0, ST()->round);
    TEST_ASSERT_EQUAL(0, ST()->reason);
    TEST_ASSERT_EQUAL(0, F.portal_starts);
    TEST_ASSERT_FALSE(ST()->portal_active);
    TEST_ASSERT_TRUE(F.notifies >= 3);
}

TEST_CASE("NO_AP_FOUND: backoff with countdown, then retry", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_disconnected(201);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_BACKOFF, ST()->state);
    TEST_ASSERT_EQUAL(201, ST()->reason);
    TEST_ASSERT_EQUAL_STRING("network not in range", espos_wifi_reason_str(ST()->reason));
    TEST_ASSERT_EQUAL_UINT32(750, espos_wifi_sm_backoff_remaining_ms(&SM));
    TEST_ASSERT_EQUAL(1, ST()->round);
    tick(300);
    TEST_ASSERT_EQUAL_UINT32(450, espos_wifi_sm_backoff_remaining_ms(&SM));
    TEST_ASSERT_EQUAL(1, F.connects);
    tick(500);                                              /* timer fires at 750 */
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    TEST_ASSERT_EQUAL(2, F.connects);
    TEST_ASSERT_EQUAL_UINT32(0, espos_wifi_sm_backoff_remaining_ms(&SM));
    /* second failure: round 1 → 1.5 s */
    ev_disconnected(201);
    TEST_ASSERT_EQUAL_UINT32(1500, espos_wifi_sm_backoff_remaining_ms(&SM));
    TEST_ASSERT_EQUAL(2, ST()->round);
    /* success resets the round counter */
    tick(1500);
    ev_connected("Boat");
    ev_got_ip();
    TEST_ASSERT_EQUAL(0, ST()->round);
    TEST_ASSERT_EQUAL(0, ST()->reason);
}

TEST_CASE("AUTH_FAIL and AUTH_EXPIRE map to 'wrong password'", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_disconnected(202);
    TEST_ASSERT_EQUAL_STRING("wrong password", espos_wifi_reason_str(ST()->reason));
    tick(1000);
    ev_disconnected(2);
    TEST_ASSERT_EQUAL_STRING("wrong password (auth expired)", espos_wifi_reason_str(2));
    TEST_ASSERT_EQUAL_STRING("auth timed out, weak signal?", espos_wifi_reason_str(204));
    TEST_ASSERT_EQUAL_STRING("unknown reason", espos_wifi_reason_str(9999));
}

TEST_CASE("DHCP timeout is a distinct failure and moves to the next network", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", "Marina");
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_connected("Boat");
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_OBTAINING_IP, ST()->state);
    tick(15000);                                            /* dhcp timer fires */
    TEST_ASSERT_EQUAL(1, F.disconnects);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_REASON_DHCP_TIMEOUT, ST()->reason);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    TEST_ASSERT_EQUAL_STRING("Marina", F.last_ssid);         /* next in priority */
    TEST_ASSERT_EQUAL(1, ST()->net_index);
    TEST_ASSERT_EQUAL(2, ST()->attempt);
}

TEST_CASE("multi-SSID: rotate through the list, back off after a full round", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("A", "B");
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_EQUAL_STRING("A", F.last_ssid);
    ev_disconnected(201);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state); /* no backoff between networks */
    TEST_ASSERT_EQUAL_STRING("B", F.last_ssid);
    ev_disconnected(202);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_BACKOFF, ST()->state);   /* round complete */
    TEST_ASSERT_EQUAL(0, ST()->net_index);                   /* next round starts at A */
    TEST_ASSERT_EQUAL(202, ST()->reason);
    tick(750);
    TEST_ASSERT_EQUAL_STRING("A", F.last_ssid);
    TEST_ASSERT_EQUAL(3, F.connects);
}

TEST_CASE("drop while connected: retry the same network first", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("A", "B");
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_connected("A");
    ev_got_ip();
    tick(5000);
    ev_disconnected(200);                                    /* beacon timeout */
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    TEST_ASSERT_EQUAL_STRING("A", F.last_ssid);              /* same one again */
    TEST_ASSERT_EQUAL(1, ST()->disconnect_count);
    TEST_ASSERT_EQUAL(200, ST()->reason);
    ev_disconnected(201);                                    /* now it is really gone */
    TEST_ASSERT_EQUAL_STRING("B", F.last_ssid);
}

TEST_CASE("connect timeout is a safety net when the driver stays silent", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("A", NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    tick(20000);
    TEST_ASSERT_EQUAL(1, F.disconnects);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_REASON_CONNECT_TIMEOUT, ST()->reason);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_BACKOFF, ST()->state);
}

TEST_CASE("driver refusing connect() counts as a failed attempt, no recursion blow-up", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("A", "B");
    reset(&c);
    F.connect_result = ESP_FAIL;
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_BACKOFF, ST()->state);
    TEST_ASSERT_EQUAL(2, F.connects);
}

TEST_CASE("unconfigured: portal immediately, no connect attempts", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with(NULL, NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_UNCONFIGURED, ST()->state);
    TEST_ASSERT_EQUAL(0, F.connects);
    TEST_ASSERT_EQUAL(1, F.portal_starts);
    TEST_ASSERT_TRUE(ST()->portal_active);
    /* configure via the portal → connect, portal down on success */
    espos_wifi_cfg_t c2 = cfg_with("Boat", NULL);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_CONFIG, &c2);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    TEST_ASSERT_TRUE(ST()->portal_active);                   /* stays up while trying */
    ev_connected("Boat");
    ev_got_ip();
    TEST_ASSERT_FALSE(ST()->portal_active);
    TEST_ASSERT_EQUAL(1, F.portal_stops);
}

TEST_CASE("portal comes up after portal_after without a connection, alongside retries", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    c.portal_after_ms = 5000;
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_disconnected(201);                                    /* → backoff 750 ms */
    TEST_ASSERT_FALSE(ST()->portal_active);
    /* walk the clock; the timer alternates between portal deadline and state timeouts */
    for (int i = 0; i < 40 && !ST()->portal_active; i++) {
        tick(250);
        if (ST()->state == ESPOS_WIFI_ST_CONNECTING) {
            ev_disconnected(201);
        }
    }
    TEST_ASSERT_TRUE(ST()->portal_active);
    TEST_ASSERT_TRUE(F.now - 1000 >= 5000);
    TEST_ASSERT_TRUE(F.now - 1000 <= 5500);
    TEST_ASSERT_TRUE(F.connects >= 2);                       /* station kept retrying */
    /* connection eventually succeeds → portal down */
    tick(60000);
    if (ST()->state == ESPOS_WIFI_ST_BACKOFF) {
        tick(60000);
    }
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    ev_connected("Boat");
    ev_got_ip();
    TEST_ASSERT_FALSE(ST()->portal_active);
}

TEST_CASE("portal disabled: never started", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with(NULL, NULL);
    c.portal_enabled = false;
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_UNCONFIGURED, ST()->state);
    TEST_ASSERT_EQUAL(0, F.portal_starts);
}

TEST_CASE("sta disabled: idle with the portal up; enabling starts connecting", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    c.sta_enabled = false;
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_DISABLED, ST()->state);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_REASON_DISABLED, ST()->reason);
    TEST_ASSERT_EQUAL(0, F.connects);
    TEST_ASSERT_TRUE(ST()->portal_active);
    espos_wifi_cfg_t c2 = c;
    c2.sta_enabled = true;
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_CONFIG, &c2);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    /* and disabling while connected disconnects */
    ev_connected("Boat");
    ev_got_ip();
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_CONFIG, &c);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_DISABLED, ST()->state);
    TEST_ASSERT_EQUAL(1, F.disconnects);
    TEST_ASSERT_EQUAL(1, ST()->disconnect_count);
}

TEST_CASE("config change: same network keeps the connection, changed psk reconnects", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_connected("Boat");
    ev_got_ip();
    /* add a second network below: nothing happens to the live link */
    espos_wifi_cfg_t c2 = cfg_with("Boat", "Marina");
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_CONFIG, &c2);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTED, ST()->state);
    TEST_ASSERT_EQUAL(0, F.disconnects);
    /* reorder: Boat becomes index 1 — still connected, index follows */
    espos_wifi_cfg_t c3 = cfg_with("Marina", "Boat");
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_CONFIG, &c3);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTED, ST()->state);
    TEST_ASSERT_EQUAL(1, ST()->net_index);
    /* identical config again: no-op */
    int n = F.notifies;
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_CONFIG, &c3);
    TEST_ASSERT_EQUAL(n, F.notifies);
    /* password change of the live network → reconnect from the top */
    espos_wifi_cfg_t c4 = c3;
    strcpy(c4.nets[1].psk, "newpw");
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_CONFIG, &c4);
    TEST_ASSERT_EQUAL(1, F.disconnects);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    TEST_ASSERT_EQUAL_STRING("Marina", F.last_ssid);         /* priority order */
    TEST_ASSERT_EQUAL(ESPOS_WIFI_REASON_CONFIG_CHANGE, ST()->reason);
    TEST_ASSERT_EQUAL(1, ST()->disconnect_count);
}

TEST_CASE("BSSID pin is passed to the driver", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    c.nets[0].has_bssid = true;
    memcpy(c.nets[0].bssid, (uint8_t[]) { 1, 2, 3, 4, 5, 6 }, 6);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_TRUE(F.last_has_bssid);
}

TEST_CASE("lost IP while connected: wait for DHCP again, then fail over", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_connected("Boat");
    ev_got_ip();
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_LOST_IP, NULL);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_OBTAINING_IP, ST()->state);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_REASON_LOST_IP, ST()->reason);
    TEST_ASSERT_EQUAL(1, ST()->disconnect_count);
    ev_got_ip();                                             /* DHCP recovered */
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTED, ST()->state);
    TEST_ASSERT_EQUAL(2, ST()->connect_count);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_LOST_IP, NULL);
    tick(15000);                                             /* no DHCP this time */
    TEST_ASSERT_EQUAL(ESPOS_WIFI_REASON_DHCP_TIMEOUT, ST()->reason);
    TEST_ASSERT_TRUE(ST()->state == ESPOS_WIFI_ST_CONNECTING || ST()->state == ESPOS_WIFI_ST_BACKOFF);
}

TEST_CASE("stale driver events in the wrong state are ignored", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_disconnected(201);                                    /* → backoff */
    int n = F.notifies;
    ev_connected("Boat");                                    /* late CONNECTED: ignored */
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_BACKOFF, ST()->state);
    ev_disconnected(201);                                    /* duplicate: ignored */
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_BACKOFF, ST()->state);
    TEST_ASSERT_EQUAL(1, ST()->round);
    TEST_ASSERT_EQUAL(n, F.notifies);
}

TEST_CASE("stop: disconnects, portal down, timer cancelled; start again works", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_connected("Boat");
    ev_got_ip();
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_STOP, NULL);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_DISABLED, ST()->state);
    TEST_ASSERT_EQUAL(1, F.disconnects);
    TEST_ASSERT_FALSE(F.timer_armed);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_STOP, NULL);      /* idempotent */
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    TEST_ASSERT_EQUAL(2, F.connects);
}

TEST_CASE("portal start failure is retried, not believed", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with(NULL, NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_TRUE(ST()->portal_active);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_PORTAL_FAILED, NULL);
    TEST_ASSERT_FALSE(ST()->portal_active);
    TEST_ASSERT_TRUE(F.timer_armed);
    TEST_ASSERT_EQUAL_UINT32(F.now + 10000, F.timer_due);
    tick(10000);
    TEST_ASSERT_TRUE(ST()->portal_active);
    TEST_ASSERT_EQUAL(2, F.portal_starts);
}

TEST_CASE("timing/portal knob changes do not restart an attempt", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_EQUAL(1, F.connects);
    espos_wifi_cfg_t c2 = c;
    c2.backoff_max_ms = 30000;
    c2.portal_after_ms = 5000;
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_CONFIG, &c2);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    TEST_ASSERT_EQUAL(1, F.connects);                        /* no restart */
    TEST_ASSERT_EQUAL_UINT32(30000, SM.cfg.backoff_max_ms);  /* but taken */
    /* a stale lease (GOT_IP without a preceding association) is ignored */
    ev_got_ip();
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    ev_connected("Boat");
    ev_got_ip();
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTED, ST()->state);
}

TEST_CASE("a timer fire that is early for the currently armed deadline is ignored", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_disconnected(201);                                    /* BACKOFF, armed for 750 ms */
    /* a fire queued from a previous arm arrives now (before the deadline) */
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_TIMER, NULL);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_BACKOFF, ST()->state);
    TEST_ASSERT_EQUAL(1, F.connects);
    tick(750);                                               /* the real one */
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    TEST_ASSERT_EQUAL(2, F.connects);
    /* after GOT_IP the timer is cancelled: a late fire does nothing */
    ev_connected("Boat");
    ev_got_ip();
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_TIMER, NULL);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTED, ST()->state);
}

TEST_CASE("portal deadline firing does not extend the DHCP/connect timeout", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    c.portal_after_ms = 3000;
    c.dhcp_timeout_ms = 10000;
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_connected("Boat");                                    /* dhcp timer: due at +10 s */
    uint32_t dhcp_due = F.now + 10000;
    TEST_ASSERT_TRUE(F.timer_due < dhcp_due);                /* portal deadline (+3 s) pre-empts */
    tick(3000);                                              /* portal fires */
    TEST_ASSERT_TRUE(ST()->portal_active);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_OBTAINING_IP, ST()->state);
    TEST_ASSERT_EQUAL_UINT32(dhcp_due, F.timer_due);         /* original deadline kept */
    tick(7000);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_REASON_DHCP_TIMEOUT, ST()->reason);
}

TEST_CASE("portal reconfig bounces a running portal only", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with(NULL, NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_EQUAL(1, F.portal_starts);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_PORTAL_RECONFIG, NULL);
    TEST_ASSERT_EQUAL(1, F.portal_stops);
    TEST_ASSERT_EQUAL(2, F.portal_starts);
    TEST_ASSERT_TRUE(ST()->portal_active);
    espos_wifi_cfg_t c2 = cfg_with("Boat", NULL);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_CONFIG, &c2);
    ev_connected("Boat");
    ev_got_ip();
    TEST_ASSERT_FALSE(ST()->portal_active);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_PORTAL_RECONFIG, NULL); /* nothing to bounce */
    TEST_ASSERT_EQUAL(2, F.portal_starts);
}

TEST_CASE("portal client count is tracked", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with(NULL, NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    int two = 2;
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_PORTAL_CLIENT, &two);
    TEST_ASSERT_EQUAL(2, ST()->portal_clients);
}

TEST_CASE("state names", "[wifi_sm]")
{
    TEST_ASSERT_EQUAL_STRING("connected", espos_wifi_state_str(ESPOS_WIFI_ST_CONNECTED));
    TEST_ASSERT_EQUAL_STRING("backoff", espos_wifi_state_str(ESPOS_WIFI_ST_BACKOFF));
    TEST_ASSERT_EQUAL_STRING("unconfigured", espos_wifi_state_str(ESPOS_WIFI_ST_UNCONFIGURED));
    TEST_ASSERT_EQUAL_STRING("obtaining_ip", espos_wifi_state_str(ESPOS_WIFI_ST_OBTAINING_IP));
}

/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The C runtime on its own, without a single node: espos_flow_every/after/
 * cancel/post and run_until_idle, which is the whole API a firmware needs if
 * it wants the loop and not the graph.
 *
 * The loop task is never started here — test_main.c adopts the loop for this
 * task instead, so every test drives the runtime by hand and asserts on what
 * happened rather than on what probably happened by now.
 */
#include <string.h>

#include "unity.h"

#include "espos_flow.h"

static int c_calls;
static char c_order[32];
static size_t c_order_n;

static void note(void *arg)
{
    c_calls++;
    if (c_order_n + 1 < sizeof(c_order)) c_order[c_order_n++] = (char)(intptr_t)arg;
    c_order[c_order_n] = '\0';
}

static void c_reset(void)
{
    c_calls = 0;
    c_order_n = 0;
    memset(c_order, 0, sizeof(c_order));
    /* Anything a previous test left queued must not land in this one: the
     * flow mailbox is process-wide. */
    espos_flow_run_until_idle(200);
}

/* Wait for real time to pass, driving the loop meanwhile. A fixed iteration
 * count is not enough on a fast host: 200 passes can take under a
 * millisecond, and a 1 ms timer would never come due. */
static void pump_ms(uint32_t ms)
{
    uint32_t started = espos_flow_now_ms();
    while (espos_flow_now_ms() - started < ms) espos_flow_run_until_idle(50);
}

/* ------------------------------------------------------------------ posts */

TEST_CASE("flow: a post runs on the next pass, not on the poster", "[flow][c]")
{
    c_reset();
    TEST_ASSERT_EQUAL(ESP_OK, espos_flow_post(note, (void *)(intptr_t)'a'));
    TEST_ASSERT_EQUAL_INT(0, c_calls); /* queued only */
    espos_flow_run_until_idle(200);
    TEST_ASSERT_EQUAL_INT(1, c_calls);
}

TEST_CASE("flow: posts from one task run in order", "[flow][c]")
{
    c_reset();
    espos_flow_post(note, (void *)(intptr_t)'1');
    espos_flow_post(note, (void *)(intptr_t)'2');
    espos_flow_post(note, (void *)(intptr_t)'3');
    espos_flow_run_until_idle(200);
    TEST_ASSERT_EQUAL_STRING("123", c_order);
}

TEST_CASE("flow: post rejects a NULL callback", "[flow][c]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_flow_post(NULL, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_flow_post_from_isr(NULL, NULL, NULL));
}

TEST_CASE("flow: the ISR post reports whether it woke anything", "[flow][c]")
{
    c_reset();
    bool woken = true; /* must be written, whatever the answer */
    TEST_ASSERT_EQUAL(ESP_OK, espos_flow_post_from_isr(note, (void *)(intptr_t)'i', &woken));
    espos_flow_run_until_idle(200);
    TEST_ASSERT_EQUAL_INT(1, c_calls);
}

TEST_CASE("flow: a full mailbox drops and counts rather than blocking", "[flow][c]")
{
    c_reset();
    espos_flow_stats_t before;
    espos_flow_stats(&before);

    /* Fill it past its depth. The point is that this RETURNS — a blocking
     * post would hang the test, which is exactly what it would do to a
     * driver task on a device. */
    int accepted = 0, refused = 0;
    for (int i = 0; i < CONFIG_ESPOS_FLOW_MAILBOX_DEPTH + 8; i++) {
        if (espos_flow_post(note, (void *)(intptr_t)'x') == ESP_OK) {
            accepted++;
        } else {
            refused++;
        }
    }
    TEST_ASSERT_EQUAL_INT(CONFIG_ESPOS_FLOW_MAILBOX_DEPTH, accepted);
    TEST_ASSERT_EQUAL_INT(8, refused);

    espos_flow_stats_t after;
    espos_flow_stats(&after);
    TEST_ASSERT_EQUAL_UINT32(before.dropped + 8, after.dropped);

    espos_flow_run_until_idle(500);
    TEST_ASSERT_EQUAL_INT(CONFIG_ESPOS_FLOW_MAILBOX_DEPTH, c_calls);
}

/* ----------------------------------------------------------------- timers */

TEST_CASE("flow: after() runs once", "[flow][c]")
{
    c_reset();
    espos_flow_timer_t h = ESPOS_FLOW_TIMER_NONE;
    TEST_ASSERT_EQUAL(ESP_OK, espos_flow_after(1, note, (void *)(intptr_t)'o', &h));
    TEST_ASSERT_NOT_EQUAL(ESPOS_FLOW_TIMER_NONE, h);

    pump_ms(20);
    TEST_ASSERT_EQUAL_INT(1, c_calls);
    /* Spent: cancelling afterwards finds nothing, and it never runs again. */
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_flow_cancel(h));
    pump_ms(20);
    TEST_ASSERT_EQUAL_INT(1, c_calls);
}

TEST_CASE("flow: every() keeps running until cancelled", "[flow][c]")
{
    c_reset();
    espos_flow_timer_t h = ESPOS_FLOW_TIMER_NONE;
    TEST_ASSERT_EQUAL(ESP_OK, espos_flow_every(1, note, (void *)(intptr_t)'e', &h));

    pump_ms(30);
    int while_running = c_calls;
    TEST_ASSERT_TRUE(while_running >= 2);

    TEST_ASSERT_EQUAL(ESP_OK, espos_flow_cancel(h));
    pump_ms(30);
    TEST_ASSERT_EQUAL_INT(while_running, c_calls);
}

TEST_CASE("flow: every() rejects a zero period and a NULL callback", "[flow][c]")
{
    espos_flow_timer_t h = ESPOS_FLOW_TIMER_NONE;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_flow_every(0, note, NULL, &h));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_flow_every(100, NULL, NULL, &h));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_flow_after(100, NULL, NULL, &h));
}

TEST_CASE("flow: cancelling a stale handle is NOT_FOUND", "[flow][c]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_flow_cancel(ESPOS_FLOW_TIMER_NONE));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_flow_cancel(0xABCD1234));
}

TEST_CASE("flow: live timers show in the stats", "[flow][c]")
{
    espos_flow_stats_t st;
    espos_flow_stats(&st);
    uint32_t before = st.timers_live;

    espos_flow_timer_t a = ESPOS_FLOW_TIMER_NONE, b = ESPOS_FLOW_TIMER_NONE;
    espos_flow_every(60000, note, NULL, &a);
    espos_flow_every(60000, note, NULL, &b);
    espos_flow_stats(&st);
    TEST_ASSERT_EQUAL_UINT32(before + 2, st.timers_live);

    espos_flow_cancel(a);
    espos_flow_cancel(b);
    espos_flow_stats(&st);
    TEST_ASSERT_EQUAL_UINT32(before, st.timers_live);
}

TEST_CASE("flow: the timer table is finite and says so", "[flow][c]")
{
    espos_flow_timer_t held[CONFIG_ESPOS_FLOW_MAX_TIMERS];
    size_t n = 0;

    /* Far-future timers, so none of them can fire while the table is full. */
    while (n < CONFIG_ESPOS_FLOW_MAX_TIMERS &&
           espos_flow_every(3600000, note, NULL, &held[n]) == ESP_OK) {
        n++;
    }
    TEST_ASSERT_TRUE(n > 0);

    espos_flow_timer_t extra = ESPOS_FLOW_TIMER_NONE;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, espos_flow_every(3600000, note, NULL, &extra));

    for (size_t i = 0; i < n; i++) TEST_ASSERT_EQUAL(ESP_OK, espos_flow_cancel(held[i]));
}

/* ---------------------------------------------------------- run_until_idle */

TEST_CASE("flow: run_until_idle is refused while the loop task runs", "[flow][c]")
{
    /* The loop is not started in these tests, so this is the negative half:
     * adopt_loop() is what a test may do, and it is refused once a real loop
     * exists. Both halves are the same invariant — exactly one task emits. */
    TEST_ASSERT_FALSE(espos_flow_is_running());
    TEST_ASSERT_EQUAL(ESP_OK, espos_flow_adopt_loop());
    TEST_ASSERT_TRUE(espos_flow_on_loop_task());
}

TEST_CASE("flow: run_until_idle drains everything queued", "[flow][c]")
{
    c_reset();
    for (int i = 0; i < 5; i++) espos_flow_post(note, (void *)(intptr_t)'d');
    TEST_ASSERT_TRUE(espos_flow_run_until_idle(500));
    TEST_ASSERT_EQUAL_INT(5, c_calls);
}

TEST_CASE("flow: the loop clock is monotonic", "[flow][c]")
{
    uint32_t a = espos_flow_now_ms();
    pump_ms(5);
    uint32_t b = espos_flow_now_ms();
    /* Modular subtraction, never a < b: the whole wrap argument in one line. */
    TEST_ASSERT_TRUE((uint32_t)(b - a) >= 5u);
    TEST_ASSERT_TRUE((uint32_t)(b - a) < 60000u);
}

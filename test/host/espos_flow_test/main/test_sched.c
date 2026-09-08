/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The timer wheel, driven by a clock the test moves by hand. No sleeping, no
 * FreeRTOS, no real time: a whole day of timers runs in microseconds and the
 * millisecond the 32-bit counter wraps can be single-stepped.
 *
 * The wrap cases are the reason this file is long. A device that runs a
 * season crosses 0xFFFFFFFF ms after 49.7 days, and every scheduler bug that
 * only appears then is a bug found on a boat.
 */
#include <string.h>

#include "unity.h"

#include "espos_sched.h"

#define SLOTS 8

static espos_sched_t s;
static espos_sched_timer_t slots[SLOTS];

/* Callbacks record their order of arrival so a test can assert on sequence,
 * not just on count. */
static char log_buf[64];
static size_t log_n;
static int fire_count;

static void log_char(void *arg)
{
    if (log_n + 1 < sizeof(log_buf)) log_buf[log_n++] = (char)(intptr_t)arg;
    log_buf[log_n] = '\0';
    fire_count++;
}

static void reset(void)
{
    memset(log_buf, 0, sizeof(log_buf));
    log_n = 0;
    fire_count = 0;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sched_init(&s, slots, SLOTS));
}

static espos_sched_handle_t add_at(uint32_t now, uint32_t delay, uint32_t period, char tag)
{
    espos_sched_handle_t h = ESPOS_SCHED_HANDLE_NONE;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sched_add(&s, now, delay, period, log_char, (void *)(intptr_t)tag, &h));
    TEST_ASSERT_NOT_EQUAL(ESPOS_SCHED_HANDLE_NONE, h);
    return h;
}

/* ------------------------------------------------------------------ basics */

TEST_CASE("sched: init clears the table", "[sched]")
{
    reset();
    TEST_ASSERT_EQUAL(0, espos_sched_count(&s));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, espos_sched_next_due(&s, 0));
}

TEST_CASE("sched: init rejects bad arguments", "[sched]")
{
    espos_sched_t t;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_sched_init(NULL, slots, SLOTS));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_sched_init(&t, NULL, SLOTS));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_sched_init(&t, slots, 0));
}

TEST_CASE("sched: a one-shot fires once, at its deadline", "[sched]")
{
    reset();
    add_at(1000, 500, 0, 'a');

    TEST_ASSERT_EQUAL(0, espos_sched_fire(&s, 1499)); /* one ms early */
    TEST_ASSERT_EQUAL(1, espos_sched_fire(&s, 1500));
    TEST_ASSERT_EQUAL_STRING("a", log_buf);
    /* Spent: it does not fire again, and the slot is free. */
    TEST_ASSERT_EQUAL(0, espos_sched_fire(&s, 9999));
    TEST_ASSERT_EQUAL(0, espos_sched_count(&s));
}

TEST_CASE("sched: delay 0 is due immediately", "[sched]")
{
    reset();
    add_at(1000, 0, 0, 'a');
    TEST_ASSERT_EQUAL_UINT32(0, espos_sched_next_due(&s, 1000));
    TEST_ASSERT_EQUAL(1, espos_sched_fire(&s, 1000));
}

TEST_CASE("sched: a periodic timer keeps firing", "[sched]")
{
    reset();
    add_at(0, 100, 100, 'p');

    for (uint32_t t = 100; t <= 500; t += 100) {
        TEST_ASSERT_EQUAL(1, espos_sched_fire(&s, t));
    }
    TEST_ASSERT_EQUAL_STRING("ppppp", log_buf);
    TEST_ASSERT_EQUAL(1, espos_sched_count(&s));
}

TEST_CASE("sched: add rejects a NULL callback and a too-distant deadline", "[sched]")
{
    reset();
    espos_sched_handle_t h;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_sched_add(&s, 0, 100, 0, NULL, NULL, &h));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      espos_sched_add(&s, 0, ESPOS_SCHED_MAX_DELAY_MS + 1u, 0, log_char, NULL, &h));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      espos_sched_add(&s, 0, 100, ESPOS_SCHED_MAX_DELAY_MS + 1u, log_char, NULL, &h));
    /* The boundary itself is allowed. */
    TEST_ASSERT_EQUAL(ESP_OK, espos_sched_add(&s, 0, ESPOS_SCHED_MAX_DELAY_MS, 0, log_char, NULL, &h));
}

TEST_CASE("sched: the table fills and says so", "[sched]")
{
    reset();
    for (int i = 0; i < SLOTS; i++) add_at(0, 100, 0, (char)('a' + i));
    TEST_ASSERT_EQUAL(SLOTS, espos_sched_count(&s));

    espos_sched_handle_t h = ESPOS_SCHED_HANDLE_NONE;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, espos_sched_add(&s, 0, 100, 0, log_char, NULL, &h));
    TEST_ASSERT_EQUAL(ESPOS_SCHED_HANDLE_NONE, h);
}

/* ------------------------------------------------------------------- order */

TEST_CASE("sched: due timers fire earliest deadline first", "[sched]")
{
    reset();
    /* Added out of order on purpose: c is due first. */
    add_at(0, 300, 0, 'a');
    add_at(0, 100, 0, 'c');
    add_at(0, 200, 0, 'b');

    TEST_ASSERT_EQUAL(3, espos_sched_fire(&s, 1000));
    TEST_ASSERT_EQUAL_STRING("cba", log_buf);
}

TEST_CASE("sched: equal deadlines fire in insertion order", "[sched]")
{
    reset();
    add_at(0, 100, 0, '1');
    add_at(0, 100, 0, '2');
    add_at(0, 100, 0, '3');

    TEST_ASSERT_EQUAL(3, espos_sched_fire(&s, 100));
    TEST_ASSERT_EQUAL_STRING("123", log_buf);
}

TEST_CASE("sched: only what is due fires", "[sched]")
{
    reset();
    add_at(0, 100, 0, 'a');
    add_at(0, 5000, 0, 'z');

    TEST_ASSERT_EQUAL(1, espos_sched_fire(&s, 200));
    TEST_ASSERT_EQUAL_STRING("a", log_buf);
    TEST_ASSERT_EQUAL(1, espos_sched_count(&s));
}

/* ---------------------------------------------------------------- next_due */

TEST_CASE("sched: next_due counts down to the earliest deadline", "[sched]")
{
    reset();
    add_at(1000, 500, 0, 'a');
    add_at(1000, 200, 0, 'b');

    TEST_ASSERT_EQUAL_UINT32(200, espos_sched_next_due(&s, 1000));
    TEST_ASSERT_EQUAL_UINT32(100, espos_sched_next_due(&s, 1100));
    TEST_ASSERT_EQUAL_UINT32(0, espos_sched_next_due(&s, 1200));
    /* Overdue reads as 0, not as nearly four billion. */
    TEST_ASSERT_EQUAL_UINT32(0, espos_sched_next_due(&s, 5000));
}

TEST_CASE("sched: next_due is UINT32_MAX with nothing pending", "[sched]")
{
    reset();
    espos_sched_handle_t h = add_at(0, 100, 0, 'a');
    TEST_ASSERT_EQUAL_UINT32(100, espos_sched_next_due(&s, 0));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sched_cancel(&s, h));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, espos_sched_next_due(&s, 0));
}

/* ------------------------------------------------------------------ cancel */

TEST_CASE("sched: a cancelled timer never fires", "[sched]")
{
    reset();
    espos_sched_handle_t h = add_at(0, 100, 0, 'a');
    add_at(0, 100, 0, 'b');

    TEST_ASSERT_EQUAL(ESP_OK, espos_sched_cancel(&s, h));
    TEST_ASSERT_EQUAL(1, espos_sched_fire(&s, 200));
    TEST_ASSERT_EQUAL_STRING("b", log_buf);
}

TEST_CASE("sched: cancelling twice is NOT_FOUND, not a double free", "[sched]")
{
    reset();
    espos_sched_handle_t h = add_at(0, 100, 0, 'a');
    TEST_ASSERT_EQUAL(ESP_OK, espos_sched_cancel(&s, h));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_sched_cancel(&s, h));
    TEST_ASSERT_EQUAL(0, espos_sched_count(&s));
}

TEST_CASE("sched: cancelling a spent one-shot is NOT_FOUND", "[sched]")
{
    reset();
    espos_sched_handle_t h = add_at(0, 100, 0, 'a');
    TEST_ASSERT_EQUAL(1, espos_sched_fire(&s, 100));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_sched_cancel(&s, h));
}

TEST_CASE("sched: a stale handle does not cancel the timer that reused its slot", "[sched]")
{
    reset();
    espos_sched_handle_t old = add_at(0, 100, 0, 'a');
    TEST_ASSERT_EQUAL(ESP_OK, espos_sched_cancel(&s, old));

    /* The next add takes the freed slot but a new generation. */
    add_at(0, 100, 0, 'b');
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_sched_cancel(&s, old));
    TEST_ASSERT_EQUAL(1, espos_sched_fire(&s, 100));
    TEST_ASSERT_EQUAL_STRING("b", log_buf);
}

TEST_CASE("sched: cancel of a never-issued handle is NOT_FOUND", "[sched]")
{
    reset();
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_sched_cancel(&s, ESPOS_SCHED_HANDLE_NONE));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_sched_cancel(&s, 0xDEADBEEF));
}

/* ------------------------------------------- cancelling from a callback */

static espos_sched_handle_t self_h;

static void cancel_self(void *arg)
{
    log_char(arg);
    TEST_ASSERT_EQUAL(ESP_OK, espos_sched_cancel(&s, self_h));
}

TEST_CASE("sched: a periodic callback may cancel itself", "[sched]")
{
    reset();
    espos_sched_handle_t h = ESPOS_SCHED_HANDLE_NONE;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sched_add(&s, 0, 100, 100, cancel_self, (void *)(intptr_t)'x', &h));
    self_h = h;

    TEST_ASSERT_EQUAL(1, espos_sched_fire(&s, 100));
    TEST_ASSERT_EQUAL_STRING("x", log_buf);
    /* Gone, and the slot is reaped once fire() unwound. */
    TEST_ASSERT_EQUAL(0, espos_sched_count(&s));
    TEST_ASSERT_EQUAL(0, espos_sched_fire(&s, 200));
}

static espos_sched_handle_t victim_h;

static void cancel_other(void *arg)
{
    log_char(arg);
    TEST_ASSERT_EQUAL(ESP_OK, espos_sched_cancel(&s, victim_h));
}

TEST_CASE("sched: a callback may cancel another timer due in the same sweep", "[sched]")
{
    reset();
    espos_sched_handle_t h = ESPOS_SCHED_HANDLE_NONE;
    /* 'k' is due first and cancels 'v', which is due in the same sweep. */
    TEST_ASSERT_EQUAL(ESP_OK, espos_sched_add(&s, 0, 100, 0, cancel_other, (void *)(intptr_t)'k', &h));
    victim_h = add_at(0, 200, 0, 'v');

    TEST_ASSERT_EQUAL(1, espos_sched_fire(&s, 1000));
    TEST_ASSERT_EQUAL_STRING("k", log_buf);
}

static void rearm_zero(void *arg)
{
    log_char(arg);
    espos_sched_handle_t h = ESPOS_SCHED_HANDLE_NONE;
    espos_sched_add(&s, 0, 0, 0, rearm_zero, arg, &h);
}

TEST_CASE("sched: a callback re-arming at delay 0 does not spin the sweep", "[sched]")
{
    reset();
    espos_sched_handle_t h = ESPOS_SCHED_HANDLE_NONE;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sched_add(&s, 0, 0, 0, rearm_zero, (void *)(intptr_t)'r', &h));

    /* One per sweep, however many are already due: the newcomer waits. */
    TEST_ASSERT_EQUAL(1, espos_sched_fire(&s, 0));
    TEST_ASSERT_EQUAL(1, espos_sched_fire(&s, 0));
    TEST_ASSERT_EQUAL_STRING("rr", log_buf);
}

static void reenter_fire(void *arg)
{
    log_char(arg);
    /* Refused, not recursive. */
    TEST_ASSERT_EQUAL(0, espos_sched_fire(&s, 100000));
}

TEST_CASE("sched: fire() refuses to re-enter", "[sched]")
{
    reset();
    espos_sched_handle_t h = ESPOS_SCHED_HANDLE_NONE;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sched_add(&s, 0, 100, 0, reenter_fire, (void *)(intptr_t)'f', &h));
    TEST_ASSERT_EQUAL(1, espos_sched_fire(&s, 100));
    TEST_ASSERT_EQUAL_STRING("f", log_buf);
}

/* -------------------------------------------------------------- no drift */

TEST_CASE("sched: a periodic deadline advances from the deadline, not from now", "[sched]")
{
    reset();
    add_at(0, 100, 100, 'p');

    /* The loop was 30 ms late. The next deadline is still 200, not 230. */
    TEST_ASSERT_EQUAL(1, espos_sched_fire(&s, 130));
    TEST_ASSERT_EQUAL_UINT32(70, espos_sched_next_due(&s, 130));
    TEST_ASSERT_EQUAL(1, espos_sched_fire(&s, 200));
    TEST_ASSERT_EQUAL_UINT32(100, espos_sched_next_due(&s, 200));
}

TEST_CASE("sched: missed periods are skipped, not fired back to back", "[sched]")
{
    reset();
    add_at(0, 100, 100, 'p');

    /* The loop was blocked for a second: eight periods went by. One fires. */
    TEST_ASSERT_EQUAL(1, espos_sched_fire(&s, 1000));
    TEST_ASSERT_EQUAL_STRING("p", log_buf);
    /* And the next deadline is a whole period ahead of now, not in the past. */
    TEST_ASSERT_EQUAL_UINT32(100, espos_sched_next_due(&s, 1000));
}

/* ------------------------------------------------------- 32-bit wrap-around */

TEST_CASE("sched: reached() is a modular comparison", "[sched][wrap]")
{
    TEST_ASSERT_TRUE(espos_sched_reached(100, 100));
    TEST_ASSERT_TRUE(espos_sched_reached(101, 100));
    TEST_ASSERT_FALSE(espos_sched_reached(99, 100));

    /* Across the wrap: 10 is AFTER 0xFFFFFFF0, by 26 ms. */
    TEST_ASSERT_TRUE(espos_sched_reached(10u, 0xFFFFFFF0u));
    TEST_ASSERT_FALSE(espos_sched_reached(0xFFFFFFF0u, 10u));
}

TEST_CASE("sched: a deadline across the wrap fires at the right moment", "[sched][wrap]")
{
    reset();
    /* Now is 10 ms before the wrap; the timer is due 100 ms later, which is
     * 90 ms after the counter restarts at 0. */
    const uint32_t now = 0xFFFFFFF6u; /* UINT32_MAX - 9 */
    add_at(now, 100, 0, 'w');

    TEST_ASSERT_EQUAL(0, espos_sched_fire(&s, 0xFFFFFFFFu)); /* the last ms before wrap */
    TEST_ASSERT_EQUAL(0, espos_sched_fire(&s, 0));           /* the wrap itself */
    TEST_ASSERT_EQUAL(0, espos_sched_fire(&s, 89));
    TEST_ASSERT_EQUAL(1, espos_sched_fire(&s, 90));
    TEST_ASSERT_EQUAL_STRING("w", log_buf);
}

TEST_CASE("sched: next_due is correct across the wrap", "[sched][wrap]")
{
    reset();
    const uint32_t now = 0xFFFFFFF6u;
    add_at(now, 100, 0, 'w');

    TEST_ASSERT_EQUAL_UINT32(100, espos_sched_next_due(&s, now));
    TEST_ASSERT_EQUAL_UINT32(91, espos_sched_next_due(&s, 0xFFFFFFFFu));
    TEST_ASSERT_EQUAL_UINT32(90, espos_sched_next_due(&s, 0));
    TEST_ASSERT_EQUAL_UINT32(1, espos_sched_next_due(&s, 89));
    TEST_ASSERT_EQUAL_UINT32(0, espos_sched_next_due(&s, 90));
}

TEST_CASE("sched: a periodic timer keeps its cadence through the wrap", "[sched][wrap]")
{
    reset();
    /* Due at 0xFFFFFFFF, then every 100 ms: 99, 199, ... after the wrap. */
    const uint32_t start = 0xFFFFFF9Fu; /* 0xFFFFFFFF - 96 */
    add_at(start, 96, 100, 'p');

    TEST_ASSERT_EQUAL(1, espos_sched_fire(&s, 0xFFFFFFFFu));
    TEST_ASSERT_EQUAL_UINT32(100, espos_sched_next_due(&s, 0xFFFFFFFFu));
    TEST_ASSERT_EQUAL(0, espos_sched_fire(&s, 98));
    TEST_ASSERT_EQUAL(1, espos_sched_fire(&s, 99));
    TEST_ASSERT_EQUAL(0, espos_sched_fire(&s, 198));
    TEST_ASSERT_EQUAL(1, espos_sched_fire(&s, 199));
    TEST_ASSERT_EQUAL_STRING("ppp", log_buf);
}

TEST_CASE("sched: ordering survives a sweep straddling the wrap", "[sched][wrap]")
{
    reset();
    const uint32_t now = 0xFFFFFFF0u;
    add_at(now, 40, 0, 'c'); /* due at 0x18 (after wrap) */
    add_at(now, 8, 0, 'a');  /* due at 0xFFFFFFF8 (before wrap) */
    add_at(now, 24, 0, 'b'); /* due at 0x08 (after wrap) */

    /* One sweep at a moment past all three: still earliest-deadline-first,
     * with "earliest" meaning earliest in modular time, not smallest. */
    TEST_ASSERT_EQUAL(3, espos_sched_fire(&s, 100));
    TEST_ASSERT_EQUAL_STRING("abc", log_buf);
}

TEST_CASE("sched: a full day of ticks stays on cadence", "[sched][wrap]")
{
    reset();
    add_at(0, 1000, 1000, 't');

    /* 86400 one-second ticks, driven at exactly the deadline each time. */
    uint32_t now = 0;
    for (int i = 0; i < 86400; i++) {
        now += 1000;
        TEST_ASSERT_EQUAL(1, espos_sched_fire(&s, now));
    }
    TEST_ASSERT_EQUAL(86400, fire_count);
    TEST_ASSERT_EQUAL_UINT32(1000, espos_sched_next_due(&s, now));
}

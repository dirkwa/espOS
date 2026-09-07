/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>

#include "unity.h"
#include "espos_time_policy.h"

/* A monotonic clock the test drives by hand, so a day can pass in one line. */
static int64_t s_mono_ms;

static int64_t fake_now(void *ctx)
{
    (void)ctx;
    return s_mono_ms;
}

static const espos_time_policy_port_t PORT = { .now_ms = fake_now };

/* 2026-09-07T10:12:13.456Z, the instant every formatting case below uses. */
#define T0      1788775933456LL
#define HOUR_MS (3600LL * 1000)

static void setup(espos_time_policy_t *p, uint32_t stale_h)
{
    s_mono_ms = 0;
    espos_time_policy_init(p, &PORT, NULL, stale_h);
}

TEST_CASE("an unset clock reads zero and is not synced", "[time]")
{
    espos_time_policy_t p;
    setup(&p, 24);
    TEST_ASSERT_FALSE(espos_time_policy_is_synced(&p));
    TEST_ASSERT_EQUAL_INT64(0, espos_time_policy_now_ms(&p));
    TEST_ASSERT_EQUAL_INT64(0, espos_time_policy_at_ms(&p, 5000));
    TEST_ASSERT_EQUAL(ESPOS_TIME_SRC_NONE, p.src);
    /* An unsynced clock formats as the empty string, not as 1970: a consumer
     * that forgets to check produces an obviously missing value. */
    char iso[ESPOS_TIME_ISO_MAX];
    TEST_ASSERT_EQUAL(0, espos_time_iso8601_format(espos_time_policy_now_ms(&p), iso, sizeof(iso)));
    TEST_ASSERT_EQUAL_STRING("", iso);
}

TEST_CASE("the clock advances with the monotonic counter once it is set", "[time]")
{
    espos_time_policy_t p;
    setup(&p, 24);
    s_mono_ms = 5000;
    TEST_ESP_OK(espos_time_policy_set(&p, T0, ESPOS_TIME_SRC_SNTP));
    TEST_ASSERT_TRUE(espos_time_policy_is_synced(&p));
    TEST_ASSERT_EQUAL_INT64(T0, espos_time_policy_now_ms(&p));
    s_mono_ms = 5000 + 90 * 1000;
    TEST_ASSERT_EQUAL_INT64(T0 + 90 * 1000, espos_time_policy_now_ms(&p));
    TEST_ASSERT_EQUAL(ESPOS_TIME_SRC_SNTP, p.src);
    TEST_ASSERT_EQUAL_UINT32(1, p.sets);
}

TEST_CASE("ranking: a lower source never overrides a higher one, a source refreshes itself", "[time]")
{
    espos_time_policy_t p;
    setup(&p, 24);
    /* Nothing set yet: even the lowest source is welcome. */
    TEST_ESP_OK(espos_time_policy_set(&p, T0, ESPOS_TIME_SRC_RTC));
    TEST_ASSERT_EQUAL(ESPOS_TIME_SRC_RTC, p.src);
    /* Every source above RTC may take over, in order. */
    TEST_ESP_OK(espos_time_policy_set(&p, T0 + 1000, ESPOS_TIME_SRC_SK));
    TEST_ASSERT_EQUAL(ESPOS_TIME_SRC_SK, p.src);
    TEST_ESP_OK(espos_time_policy_set(&p, T0 + 2000, ESPOS_TIME_SRC_MANUAL));
    TEST_ASSERT_EQUAL(ESPOS_TIME_SRC_MANUAL, p.src);
    TEST_ESP_OK(espos_time_policy_set(&p, T0 + 3000, ESPOS_TIME_SRC_SNTP));
    TEST_ASSERT_EQUAL(ESPOS_TIME_SRC_SNTP, p.src);
    TEST_ASSERT_EQUAL_INT64(T0 + 3000, espos_time_policy_now_ms(&p));

    /* Now everything below SNTP is refused, and the clock does not move. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, espos_time_policy_set(&p, T0 + 99999, ESPOS_TIME_SRC_MANUAL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, espos_time_policy_set(&p, T0 + 99999, ESPOS_TIME_SRC_SK));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, espos_time_policy_set(&p, T0 + 99999, ESPOS_TIME_SRC_RTC));
    TEST_ASSERT_EQUAL_INT64(T0 + 3000, espos_time_policy_now_ms(&p));
    TEST_ASSERT_EQUAL(ESPOS_TIME_SRC_SNTP, p.src);

    /* But SNTP re-syncing an hour later is not a downgrade. */
    TEST_ESP_OK(espos_time_policy_set(&p, T0 + HOUR_MS, ESPOS_TIME_SRC_SNTP));
    TEST_ASSERT_EQUAL_INT64(T0 + HOUR_MS, espos_time_policy_now_ms(&p));
}

TEST_CASE("set rejects a non-positive instant and an out-of-range source", "[time]")
{
    espos_time_policy_t p;
    setup(&p, 24);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_time_policy_set(&p, 0, ESPOS_TIME_SRC_SNTP));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_time_policy_set(&p, -1, ESPOS_TIME_SRC_SNTP));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_time_policy_set(&p, T0, ESPOS_TIME_SRC_NONE));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_time_policy_set(&p, T0, ESPOS_TIME_SRC_MAX));
    TEST_ASSERT_FALSE(espos_time_policy_is_synced(&p));
}

TEST_CASE("staleness: an RTC time ages out of 'synced' but is still readable", "[time]")
{
    espos_time_policy_t p;
    setup(&p, 24);
    TEST_ESP_OK(espos_time_policy_set(&p, T0, ESPOS_TIME_SRC_RTC));
    TEST_ASSERT_TRUE(espos_time_policy_is_synced(&p));

    s_mono_ms = 23 * HOUR_MS;
    TEST_ASSERT_TRUE(espos_time_policy_is_synced(&p));      /* not yet */
    s_mono_ms = 24 * HOUR_MS;
    TEST_ASSERT_FALSE(espos_time_policy_is_synced(&p));     /* exactly at the limit */
    s_mono_ms = 48 * HOUR_MS;
    TEST_ASSERT_FALSE(espos_time_policy_is_synced(&p));

    /* Stale is not gone: the time is still the best guess the device has, and
     * a log line with an approximate stamp beats one with none. */
    TEST_ASSERT_EQUAL_INT64(T0 + 48 * HOUR_MS, espos_time_policy_now_ms(&p));
    /* And a real source still replaces it, stale or not. */
    TEST_ESP_OK(espos_time_policy_set(&p, T0 + 48 * HOUR_MS, ESPOS_TIME_SRC_SNTP));
    TEST_ASSERT_TRUE(espos_time_policy_is_synced(&p));
}

TEST_CASE("staleness: only RTC ages, and stale_after_h of 0 disables the rule", "[time]")
{
    espos_time_policy_t p;
    setup(&p, 24);
    /* An SNTP sync from a week ago is still 'synced': it was learned from a
     * live source, and nothing has said otherwise since. */
    TEST_ESP_OK(espos_time_policy_set(&p, T0, ESPOS_TIME_SRC_SNTP));
    s_mono_ms = 7 * 24 * HOUR_MS;
    TEST_ASSERT_TRUE(espos_time_policy_is_synced(&p));

    setup(&p, 0); /* the rule off entirely */
    TEST_ESP_OK(espos_time_policy_set(&p, T0, ESPOS_TIME_SRC_RTC));
    s_mono_ms = 365 * 24 * HOUR_MS;
    TEST_ASSERT_TRUE(espos_time_policy_is_synced(&p));
}

TEST_CASE("staleness counts the age a deep-sleep value already had", "[time]")
{
    espos_time_policy_t p;
    setup(&p, 24);
    /* The chip slept for 20 hours; the instant in RTC memory is that old
     * before this boot has run for a second. Two more hours of uptime and it
     * is 22 h old — still fresh; six and it is past the day. */
    TEST_ESP_OK(espos_time_policy_set_aged(&p, T0, ESPOS_TIME_SRC_RTC, 20 * HOUR_MS));
    TEST_ASSERT_TRUE(espos_time_policy_is_synced(&p));
    s_mono_ms = 2 * HOUR_MS;
    TEST_ASSERT_TRUE(espos_time_policy_is_synced(&p));
    s_mono_ms = 4 * HOUR_MS;
    TEST_ASSERT_FALSE(espos_time_policy_is_synced(&p));

    /* A negative prior age is treated as none rather than as credit. */
    setup(&p, 24);
    TEST_ESP_OK(espos_time_policy_set_aged(&p, T0, ESPOS_TIME_SRC_RTC, -5 * HOUR_MS));
    s_mono_ms = 23 * HOUR_MS;
    TEST_ASSERT_TRUE(espos_time_policy_is_synced(&p));
}

TEST_CASE("at_ms dates a stamp taken before the clock was ever set", "[time]")
{
    espos_time_policy_t p;
    setup(&p, 24);
    /* The device boots, records three measurements over the first ten seconds
     * with no clock at all, and only learns the time an hour later. Each stamp
     * must still map to the second it was actually taken. */
    TEST_ASSERT_EQUAL_INT64(0, espos_time_policy_at_ms(&p, 1000));
    s_mono_ms = HOUR_MS;
    TEST_ESP_OK(espos_time_policy_set(&p, T0 + HOUR_MS, ESPOS_TIME_SRC_SNTP));
    TEST_ASSERT_EQUAL_INT64(T0 + 1000, espos_time_policy_at_ms(&p, 1000));
    TEST_ASSERT_EQUAL_INT64(T0 + 5000, espos_time_policy_at_ms(&p, 5000));
    TEST_ASSERT_EQUAL_INT64(T0 + 10000, espos_time_policy_at_ms(&p, 10000));
    /* Including the instant of the sync itself. */
    TEST_ASSERT_EQUAL_INT64(T0 + HOUR_MS, espos_time_policy_at_ms(&p, HOUR_MS));
}

TEST_CASE("iso8601: format, milliseconds, leap years and the empty cases", "[time]")
{
    char b[ESPOS_TIME_ISO_MAX];
    TEST_ASSERT_EQUAL(24, espos_time_iso8601_format(T0, b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("2026-09-07T10:12:13.456Z", b);

    /* The epoch itself is the "no time" sentinel and formats as nothing. */
    TEST_ASSERT_EQUAL(0, espos_time_iso8601_format(0, b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("", b);
    TEST_ASSERT_EQUAL(0, espos_time_iso8601_format(-1, b, sizeof(b)));

    /* One millisecond past the epoch is a real instant. */
    TEST_ASSERT_EQUAL(24, espos_time_iso8601_format(1, b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("1970-01-01T00:00:00.001Z", b);

    /* Milliseconds are zero-padded, not truncated. */
    espos_time_iso8601_format(1788775933006LL, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("2026-09-07T10:12:13.006Z", b);
    espos_time_iso8601_format(1788775933000LL, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("2026-09-07T10:12:13.000Z", b);

    /* Leap day, and the day after a leap day. */
    espos_time_iso8601_format(1709164800000LL, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("2024-02-29T00:00:00.000Z", b);
    espos_time_iso8601_format(1709251199999LL, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("2024-02-29T23:59:59.999Z", b);
    /* 2100 is NOT a leap year — the rule the naive "divisible by four" gets
     * wrong, which is the reason this is arithmetic and not a lookup. The
     * second before March is the 28th of February, not the 29th. */
    espos_time_iso8601_format(4107542399999LL, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("2100-02-28T23:59:59.999Z", b);
    espos_time_iso8601_format(4107542400000LL, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("2100-03-01T00:00:00.000Z", b);

    /* A buffer too small writes nothing rather than something truncated. */
    char small[10];
    TEST_ASSERT_EQUAL(0, espos_time_iso8601_format(T0, small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("", small);
    TEST_ASSERT_EQUAL(0, espos_time_iso8601_format(T0, NULL, 0));
}

TEST_CASE("iso8601 parse: the shapes a SignalK server sends, and what it refuses", "[time]")
{
    TEST_ASSERT_EQUAL_INT64(T0, espos_time_iso8601_parse("2026-09-07T10:12:13.456Z"));
    /* No fractional part. */
    TEST_ASSERT_EQUAL_INT64(T0 - 456, espos_time_iso8601_parse("2026-09-07T10:12:13Z"));
    /* Fewer and more fractional digits: three are kept, the rest dropped. */
    TEST_ASSERT_EQUAL_INT64(T0 - 56, espos_time_iso8601_parse("2026-09-07T10:12:13.4Z"));
    TEST_ASSERT_EQUAL_INT64(T0, espos_time_iso8601_parse("2026-09-07T10:12:13.456789Z"));
    /* Lower-case separator and a space, both seen in the wild. */
    TEST_ASSERT_EQUAL_INT64(T0, espos_time_iso8601_parse("2026-09-07t10:12:13.456Z"));
    TEST_ASSERT_EQUAL_INT64(T0, espos_time_iso8601_parse("2026-09-07 10:12:13.456z"));
    /* A numeric offset is converted to UTC, with and without the colon. */
    TEST_ASSERT_EQUAL_INT64(T0, espos_time_iso8601_parse("2026-09-07T12:12:13.456+02:00"));
    TEST_ASSERT_EQUAL_INT64(T0, espos_time_iso8601_parse("2026-09-07T12:12:13.456+0200"));
    TEST_ASSERT_EQUAL_INT64(T0, espos_time_iso8601_parse("2026-09-07T05:42:13.456-04:30"));
    /* No zone at all: read as UTC, which is what SignalK means anyway. */
    TEST_ASSERT_EQUAL_INT64(T0, espos_time_iso8601_parse("2026-09-07T10:12:13.456"));

    /* Everything that is not a timestamp is 0, the component's "no time". */
    TEST_ASSERT_EQUAL_INT64(0, espos_time_iso8601_parse(NULL));
    TEST_ASSERT_EQUAL_INT64(0, espos_time_iso8601_parse(""));
    TEST_ASSERT_EQUAL_INT64(0, espos_time_iso8601_parse("null"));
    TEST_ASSERT_EQUAL_INT64(0, espos_time_iso8601_parse("2026-09-07"));
    TEST_ASSERT_EQUAL_INT64(0, espos_time_iso8601_parse("2026-09-07T10:12"));
    TEST_ASSERT_EQUAL_INT64(0, espos_time_iso8601_parse("2026-13-07T10:12:13Z"));  /* month 13 */
    TEST_ASSERT_EQUAL_INT64(0, espos_time_iso8601_parse("2026-09-07T25:12:13Z"));  /* hour 25 */
    TEST_ASSERT_EQUAL_INT64(0, espos_time_iso8601_parse("2026-09-07T10:12:13.Z")); /* dot, no digits */
    TEST_ASSERT_EQUAL_INT64(0, espos_time_iso8601_parse("2026-09-07T10:12:13Z junk"));
    TEST_ASSERT_EQUAL_INT64(0, espos_time_iso8601_parse("1970-01-01T00:00:00.000Z")); /* the sentinel */
}

TEST_CASE("iso8601 round-trips through parse and format", "[time]")
{
    static const int64_t instants[] = {
        1LL,
        1000LL,
        T0,
        1709164800000LL,
        4107542400000LL,
        2524608000123LL,
    };
    char a[ESPOS_TIME_ISO_MAX];
    char b[ESPOS_TIME_ISO_MAX];
    for (size_t i = 0; i < sizeof(instants) / sizeof(instants[0]); i++) {
        TEST_ASSERT_GREATER_THAN(0, espos_time_iso8601_format(instants[i], a, sizeof(a)));
        int64_t back = espos_time_iso8601_parse(a);
        TEST_ASSERT_EQUAL_INT64(instants[i], back);
        TEST_ASSERT_GREATER_THAN(0, espos_time_iso8601_format(back, b, sizeof(b)));
        TEST_ASSERT_EQUAL_STRING(a, b);
    }
}

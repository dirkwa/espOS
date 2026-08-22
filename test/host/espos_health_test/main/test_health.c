/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * espos_health: the condition table and the sink registry.
 *
 * The two properties everything else leans on are that a condition is a level
 * (re-report freely, sinks hear about changes only) and that a sink which
 * registers late still learns the current state. Get the first wrong and a
 * polling caller floods the SignalK server; get the second wrong and a
 * condition raised during boot is invisible for as long as it lasts, which is
 * precisely the case espos_health exists for.
 */
#include <string.h>

#include "espos_health.h"
#include "unity.h"

/* ------------------------------------------------------------- recorder */

#define MAX_CALLS 16

typedef struct {
    char key[ESPOS_HEALTH_KEY_MAX];
    espos_health_state_t state;
    char message[ESPOS_HEALTH_MSG_MAX];
} call_t;

static call_t s_calls[MAX_CALLS];
static size_t s_n;

static void recorder(const char *key, espos_health_state_t state, const char *message, void *arg)
{
    (void)arg;
    if (s_n >= MAX_CALLS) return;
    snprintf(s_calls[s_n].key, sizeof(s_calls[s_n].key), "%s", key);
    s_calls[s_n].state = state;
    snprintf(s_calls[s_n].message, sizeof(s_calls[s_n].message), "%s", message ? message : "");
    s_n++;
}

/* A second sink with its own identity, to prove both are called. */
static size_t s_other_n;
static void other_sink(const char *key, espos_health_state_t state, const char *message, void *arg)
{
    (void)key; (void)state; (void)message; (void)arg;
    s_other_n++;
}

static void fresh(void)
{
    espos_health_reset();
    memset(s_calls, 0, sizeof(s_calls));
    s_n = 0;
    s_other_n = 0;
}

/* --------------------------------------------------------------- tests */

TEST_CASE("a sink hears a raised condition", "[health]")
{
    fresh();
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_add_sink(recorder, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_report("n2kBus", ESPOS_HEALTH_WARN, "no frames for 30 s"));

    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)s_n);
    TEST_ASSERT_EQUAL_STRING("n2kBus", s_calls[0].key);
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_WARN, s_calls[0].state);
    TEST_ASSERT_EQUAL_STRING("no frames for 30 s", s_calls[0].message);
}

/* The property a polling caller depends on. */
TEST_CASE("re-reporting an unchanged condition stays quiet", "[health]")
{
    fresh();
    espos_health_add_sink(recorder, NULL);
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, espos_health_report("lowMemory", ESPOS_HEALTH_WARN, "18 KB free"));
    }
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)s_n);
}

TEST_CASE("a changed message is a change", "[health]")
{
    fresh();
    espos_health_add_sink(recorder, NULL);
    espos_health_report("lowMemory", ESPOS_HEALTH_WARN, "18 KB free");
    espos_health_report("lowMemory", ESPOS_HEALTH_WARN, "12 KB free");
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)s_n);
    TEST_ASSERT_EQUAL_STRING("12 KB free", s_calls[1].message);
}

TEST_CASE("clearing is delivered like any other change", "[health]")
{
    fresh();
    espos_health_add_sink(recorder, NULL);
    espos_health_report("wakeService", ESPOS_HEALTH_WARN, "unreachable");
    espos_health_report("wakeService", ESPOS_HEALTH_NORMAL, "");
    espos_health_report("wakeService", ESPOS_HEALTH_NORMAL, "");   /* still quiet */

    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)s_n);
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL, s_calls[1].state);
}

/* A first report of NORMAL is not a no-op: after a reboot the far end may
 * still hold an alert this device raised before it restarted, and this is the
 * clear that retires it. */
TEST_CASE("a first NORMAL for an unknown key is still delivered", "[health]")
{
    fresh();
    espos_health_add_sink(recorder, NULL);
    espos_health_report("staleAlarm", ESPOS_HEALTH_NORMAL, "");
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)s_n);
    TEST_ASSERT_EQUAL_STRING("staleAlarm", s_calls[0].key);
}

/* espos_sk registers its sink only once the stream is up, long after
 * espos_voice may have reported a dead wake service. */
TEST_CASE("a late sink is told the current state on registration", "[health]")
{
    fresh();
    espos_health_report("wakeService", ESPOS_HEALTH_WARN, "unreachable");
    espos_health_report("lowMemory", ESPOS_HEALTH_NORMAL, "");
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)s_n);   /* nobody listening yet */

    TEST_ASSERT_EQUAL(ESP_OK, espos_health_add_sink(recorder, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)s_n);
    TEST_ASSERT_EQUAL_STRING("wakeService", s_calls[0].key);
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_WARN, s_calls[0].state);
    TEST_ASSERT_EQUAL_STRING("lowMemory", s_calls[1].key);
}

TEST_CASE("every registered sink is called", "[health]")
{
    fresh();
    espos_health_add_sink(recorder, NULL);
    espos_health_add_sink(other_sink, NULL);
    espos_health_report("n2kBus", ESPOS_HEALTH_ALARM, "bus off");
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)s_n);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)s_other_n);
}

TEST_CASE("the same sink cannot register twice", "[health]")
{
    fresh();
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_add_sink(recorder, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, espos_health_add_sink(recorder, NULL));

    /* Same function, different arg, is a different sink. */
    int ctx = 1;
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_add_sink(recorder, &ctx));
    espos_health_report("x", ESPOS_HEALTH_WARN, "");
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)s_n);
}

TEST_CASE("a removed sink hears nothing further", "[health]")
{
    fresh();
    espos_health_add_sink(recorder, NULL);
    espos_health_report("x", ESPOS_HEALTH_WARN, "one");
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_remove_sink(recorder, NULL));
    espos_health_report("x", ESPOS_HEALTH_WARN, "two");
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)s_n);

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_health_remove_sink(recorder, NULL));
}

/* Truncation would be worse than an error: a clipped key never matches on the
 * next report, so every call would consume another slot. */
TEST_CASE("an over-long key or message is rejected, not clipped", "[health]")
{
    fresh();
    char long_key[ESPOS_HEALTH_KEY_MAX + 8];
    memset(long_key, 'k', sizeof(long_key) - 1);
    long_key[sizeof(long_key) - 1] = '\0';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, espos_health_report(long_key, ESPOS_HEALTH_WARN, ""));

    char long_msg[ESPOS_HEALTH_MSG_MAX + 8];
    memset(long_msg, 'm', sizeof(long_msg) - 1);
    long_msg[sizeof(long_msg) - 1] = '\0';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, espos_health_report("k", ESPOS_HEALTH_WARN, long_msg));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_health_report("", ESPOS_HEALTH_WARN, ""));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_health_report(NULL, ESPOS_HEALTH_WARN, ""));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)espos_health_snapshot(NULL, 0));
}

TEST_CASE("a NULL message reads back as empty", "[health]")
{
    fresh();
    espos_health_add_sink(recorder, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_report("x", ESPOS_HEALTH_WARN, NULL));
    TEST_ASSERT_EQUAL_STRING("", s_calls[0].message);
}

TEST_CASE("the table fills and then refuses further keys", "[health]")
{
    fresh();
    char key[ESPOS_HEALTH_KEY_MAX];
    for (int i = 0; i < CONFIG_ESPOS_HEALTH_MAX_CONDITIONS; i++) {
        snprintf(key, sizeof(key), "k%d", i);
        TEST_ASSERT_EQUAL(ESP_OK, espos_health_report(key, ESPOS_HEALTH_WARN, ""));
    }
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, espos_health_report("overflow", ESPOS_HEALTH_WARN, ""));

    /* A key already in the table still works once it is full. */
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_report("k0", ESPOS_HEALTH_NORMAL, ""));
    TEST_ASSERT_EQUAL_UINT32(CONFIG_ESPOS_HEALTH_MAX_CONDITIONS,
                             (uint32_t)espos_health_snapshot(NULL, 0));
}

TEST_CASE("snapshot copies at most what it is given room for", "[health]")
{
    fresh();
    espos_health_report("a", ESPOS_HEALTH_WARN, "one");
    espos_health_report("b", ESPOS_HEALTH_ALARM, "two");

    espos_health_condition_t got[1];
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)espos_health_snapshot(got, 1));
    TEST_ASSERT_EQUAL_STRING("a", got[0].key);
    TEST_ASSERT_EQUAL_STRING("one", got[0].message);
}

/* What a single status LED wants to know. */
TEST_CASE("worst reports the highest state currently held", "[health]")
{
    fresh();
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL, espos_health_worst());

    espos_health_report("a", ESPOS_HEALTH_WARN, "");
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_WARN, espos_health_worst());

    espos_health_report("b", ESPOS_HEALTH_ALARM, "");
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_ALARM, espos_health_worst());

    espos_health_report("b", ESPOS_HEALTH_NORMAL, "");
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_WARN, espos_health_worst());

    espos_health_report("a", ESPOS_HEALTH_NORMAL, "");
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL, espos_health_worst());
}

TEST_CASE("state_str names the three levels", "[health]")
{
    TEST_ASSERT_EQUAL_STRING("normal", espos_health_state_str(ESPOS_HEALTH_NORMAL));
    TEST_ASSERT_EQUAL_STRING("warn", espos_health_state_str(ESPOS_HEALTH_WARN));
    TEST_ASSERT_EQUAL_STRING("alarm", espos_health_state_str(ESPOS_HEALTH_ALARM));
}

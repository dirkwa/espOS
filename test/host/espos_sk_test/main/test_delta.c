/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "espos_sk_delta.h"

static espos_sk_delta_t *mk(size_t max_msgs, size_t max_bytes, uint32_t drain)
{
    espos_sk_delta_cfg_t c = { .label = "espos-1a2b", .batch_ms = 100, .max_msgs = max_msgs, .max_bytes = max_bytes, .drain_per_s = drain };
    espos_sk_delta_t *d = espos_sk_delta_create(&c);
    TEST_ASSERT_NOT_NULL(d);
    return d;
}

TEST_CASE("batching: values within the window become one delta, last value per path wins", "[delta]")
{
    espos_sk_delta_t *d = mk(8, 0, 20);
    TEST_ESP_OK(espos_sk_delta_publish(d, "environment.wind.speedApparent", "3.5", 1000));
    TEST_ESP_OK(espos_sk_delta_publish(d, "environment.wind.angleApparent", "0.7", 1020));
    TEST_ESP_OK(espos_sk_delta_publish(d, "environment.wind.speedApparent", "3.6", 1050)); /* replaces */
    TEST_ASSERT_NULL(espos_sk_delta_take(d, 1090, true));                                   /* window open */
    TEST_ASSERT_EQUAL_UINT32(10, espos_sk_delta_next_due_ms(d, 1090, true));
    char *m = espos_sk_delta_take(d, 1100, true);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_STRING(
        "{\"context\":\"vessels.self\",\"updates\":[{\"source\":{\"label\":\"espos-1a2b\"},\"values\":["
        "{\"path\":\"environment.wind.speedApparent\",\"value\":3.6},"
        "{\"path\":\"environment.wind.angleApparent\",\"value\":0.7}]}]}", m);
    free(m);
    TEST_ASSERT_NULL(espos_sk_delta_take(d, 1200, true));
    espos_sk_delta_stats_t st;
    espos_sk_delta_stats(d, &st);
    TEST_ASSERT_EQUAL(1, st.built);
    TEST_ASSERT_EQUAL(1, st.taken);
    TEST_ASSERT_EQUAL(0, st.buffered);
    espos_sk_delta_destroy(d);
}

TEST_CASE("offline: messages buffer, oldest dropped at capacity, drained in order and rate-limited", "[delta]")
{
    espos_sk_delta_t *d = mk(3, 0, 10); /* 3 messages, 10/s */
    for (int i = 0; i < 5; i++) {
        char v[8];
        snprintf(v, sizeof(v), "%d", i);
        TEST_ESP_OK(espos_sk_delta_publish(d, "a.b", v, 1000 + i * 1000));
        TEST_ASSERT_NULL(espos_sk_delta_take(d, 1000 + i * 1000 + 100, false)); /* offline: builds, buffers */
    }
    espos_sk_delta_stats_t st;
    espos_sk_delta_stats(d, &st);
    TEST_ASSERT_EQUAL(3, st.buffered);
    TEST_ASSERT_EQUAL(2, st.dropped);                                             /* 0 and 1 gone */
    /* reconnect at t=10000: first message immediately, then ≥100 ms apart */
    char *m = espos_sk_delta_take(d, 10000, true);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NOT_NULL(strstr(m, "\"value\":2}"));
    free(m);
    TEST_ASSERT_NULL(espos_sk_delta_take(d, 10050, true));                        /* too soon */
    TEST_ASSERT_EQUAL_UINT32(50, espos_sk_delta_next_due_ms(d, 10050, true));
    m = espos_sk_delta_take(d, 10100, true);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NOT_NULL(strstr(m, "\"value\":3}"));
    free(m);
    m = espos_sk_delta_take(d, 10200, true);
    TEST_ASSERT_NOT_NULL(strstr(m, "\"value\":4}"));
    free(m);
    TEST_ASSERT_NULL(espos_sk_delta_take(d, 10300, true));
    /* live traffic after the backlog is not throttled */
    TEST_ESP_OK(espos_sk_delta_publish(d, "a.b", "5", 10300));
    m = espos_sk_delta_take(d, 10400, true);
    TEST_ASSERT_NOT_NULL(m);
    free(m);
    TEST_ESP_OK(espos_sk_delta_publish(d, "a.b", "6", 10401));
    m = espos_sk_delta_take(d, 10501, true);
    TEST_ASSERT_NOT_NULL(m);                                                       /* 101 ms later, no gap needed */
    free(m);
    espos_sk_delta_destroy(d);
}

TEST_CASE("byte cap, requeue after a failed send, forced flush", "[delta]")
{
    espos_sk_delta_t *d = mk(100, 400, 20);
    for (int i = 0; i < 6; i++) {
        TEST_ESP_OK(espos_sk_delta_publish(d, "some.path", "1234567890", 1000 + i * 200));
        espos_sk_delta_flush(d, 0);
    }
    espos_sk_delta_stats_t st;
    espos_sk_delta_stats(d, &st);
    TEST_ASSERT_TRUE(st.buffered_bytes <= 400);
    TEST_ASSERT_TRUE(st.buffered < 6);
    TEST_ASSERT_TRUE(st.dropped > 0);
    size_t before = st.buffered;
    char *m = espos_sk_delta_take(d, 5000, true);
    TEST_ASSERT_NOT_NULL(m);
    espos_sk_delta_requeue(d, m);                                                  /* send failed */
    espos_sk_delta_stats(d, &st);
    TEST_ASSERT_EQUAL(before, st.buffered);
    char *again = espos_sk_delta_take(d, 6000, true);
    TEST_ASSERT_NOT_NULL(again);                                                   /* same one first */
    free(again);
    /* an oversized single message is dropped, not buffered */
    char big[ESPOS_SK_VALUE_MAX];
    memset(big, '9', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    for (int i = 0; i < 3; i++) {
        char p[32];
        snprintf(p, sizeof(p), "p.%d", i);
        TEST_ESP_OK(espos_sk_delta_publish(d, p, big, 7000));
    }
    espos_sk_delta_flush(d, 7000);                                                 /* > 400 bytes */
    espos_sk_delta_stats(d, &st);
    TEST_ASSERT_TRUE(st.buffered_bytes <= 400);
    espos_sk_delta_destroy(d);
}

TEST_CASE("publish validation and pending overflow", "[delta]")
{
    espos_sk_delta_t *d = mk(8, 0, 20);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_sk_delta_publish(d, "", "1", 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_sk_delta_publish(d, "a", "", 0));
    char longp[ESPOS_SK_PATH_MAX + 4];
    memset(longp, 'a', sizeof(longp) - 1);
    longp[sizeof(longp) - 1] = '\0';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, espos_sk_delta_publish(d, longp, "1", 0));
    for (int i = 0; i < ESPOS_SK_PENDING_MAX + 2; i++) {
        char p[32];
        snprintf(p, sizeof(p), "path.%d", i);
        TEST_ESP_OK(espos_sk_delta_publish(d, p, "1", 100));
    }
    espos_sk_delta_stats_t st;
    espos_sk_delta_stats(d, &st);
    TEST_ASSERT_EQUAL(1, st.built);                                                /* window closed early */
    TEST_ASSERT_EQUAL(2, st.pending);
    espos_sk_delta_destroy(d);
}

TEST_CASE("json helpers", "[delta]")
{
    char b[64];
    espos_sk_json_number(b, sizeof(b), 0.1);
    TEST_ASSERT_EQUAL_STRING("0.1", b);
    espos_sk_json_number(b, sizeof(b), 3.0);
    TEST_ASSERT_EQUAL_STRING("3", b);
    espos_sk_json_number(b, sizeof(b), (double)0.1f);
    TEST_ASSERT_EQUAL_STRING("0.1", b);
    espos_sk_json_number(b, sizeof(b), 1e21);
    TEST_ASSERT_EQUAL_STRING("1e+21", b);
    espos_sk_json_number(b, sizeof(b), 0.0 / 0.0);
    TEST_ASSERT_EQUAL_STRING("null", b);
    espos_sk_json_string(b, sizeof(b), "he said \"hi\"\n");
    TEST_ASSERT_EQUAL_STRING("\"he said \\\"hi\\\"\\u000a\"", b);
}

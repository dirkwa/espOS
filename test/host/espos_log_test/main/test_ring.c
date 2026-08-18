/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution */
#include <string.h>
#include <stdio.h>

#include "unity.h"
#include "esp_log.h"
#include "espos_log.h"

static const char *TAG = "ringtest";

typedef struct {
    char last[128];
    uint32_t last_seq;
    int n;
    uint32_t seqs[64];
} acc_t;

static bool collect(uint32_t seq, const char *line, size_t len, void *arg)
{
    acc_t *a = arg;
    if (a->n < 64) {
        a->seqs[a->n] = seq;
    }
    a->n++;
    snprintf(a->last, sizeof(a->last), "%.*s", (int)len, line);
    a->last_seq = seq;
    return true;
}

static void reset(void)
{
    espos_log_deinit();
    TEST_ASSERT_EQUAL(ESP_OK, espos_log_init());
}

TEST_CASE("lines are stored with increasing sequence numbers", "[log]")
{
    reset();
    espos_log_stats_t st;
    espos_log_stats(&st);
    uint32_t base = st.next;
    ESP_LOGI(TAG, "one");
    ESP_LOGI(TAG, "two");
    ESP_LOGW(TAG, "three");
    espos_log_stats(&st);
    TEST_ASSERT_EQUAL(base + 3, st.next);
    acc_t a = { 0 };
    TEST_ASSERT_EQUAL(3, espos_log_read(base - 1, 0, collect, &a));
    TEST_ASSERT_EQUAL(base, a.seqs[0]);
    TEST_ASSERT_EQUAL(base + 2, a.last_seq);
    TEST_ASSERT_NOT_NULL(strstr(a.last, "ringtest: three"));
    TEST_ASSERT_EQUAL('W', a.last[0]);
}

TEST_CASE("after and limit page through the ring", "[log]")
{
    reset();
    espos_log_stats_t st;
    espos_log_stats(&st);
    uint32_t base = st.next;
    for (int i = 0; i < 10; i++) {
        ESP_LOGI(TAG, "line %d", i);
    }
    acc_t a = { 0 };
    TEST_ASSERT_EQUAL(4, espos_log_read(base + 2, 4, collect, &a));
    TEST_ASSERT_EQUAL(base + 3, a.seqs[0]);
    TEST_ASSERT_EQUAL(base + 6, a.last_seq);
    TEST_ASSERT_NOT_NULL(strstr(a.last, "line 6"));
    a.n = 0;
    TEST_ASSERT_EQUAL(0, espos_log_read(base + 9, 0, collect, &a));
    TEST_ASSERT_EQUAL(0, a.n);
}

TEST_CASE("the ring wraps and drops the oldest lines", "[log]")
{
    reset();
    /* 2048-byte ring, ~50-byte lines: 200 lines must overwrite. */
    for (int i = 0; i < 200; i++) {
        ESP_LOGI(TAG, "wrap line number %03d ........", i);
    }
    espos_log_stats_t st;
    espos_log_stats(&st);
    TEST_ASSERT_GREATER_THAN(0, st.dropped);
    TEST_ASSERT_LESS_OR_EQUAL(2048, st.used);
    TEST_ASSERT_TRUE(st.first > 1);
    /* Everything still in the ring is intact and contiguous. */
    acc_t a = { 0 };
    size_t n = espos_log_read(0, 0, collect, &a);
    TEST_ASSERT_EQUAL(st.next - st.first, n);
    TEST_ASSERT_EQUAL(st.first, a.seqs[0]);
    TEST_ASSERT_EQUAL(st.next - 1, a.last_seq);
    TEST_ASSERT_NOT_NULL(strstr(a.last, "wrap line number 199"));
    /* Reading from a seq that was dropped starts at first. */
    a.n = 0;
    espos_log_read(1, 1, collect, &a);
    TEST_ASSERT_EQUAL(st.first, a.seqs[0]);
}

TEST_CASE("long lines are truncated, colour codes stripped, CR dropped", "[log]")
{
    reset();
    espos_log_stats_t st;
    espos_log_stats(&st);
    uint32_t base = st.next;
    ESP_LOGI(TAG, "0123456789012345678901234567890123456789012345678901234567890123456789ABCDEFGHIJ");
    acc_t a = { 0 };
    espos_log_read(base - 1, 1, collect, &a);
    TEST_ASSERT_EQUAL(64, strlen(a.last));
    /* Raw vprintf through the hook: colours and CRLF are cleaned. */
    esp_log_write(ESP_LOG_INFO, TAG, "\033[0;32mI (1) x: colour\033[0m\r\n");
    a.n = 0;
    espos_log_read(base, 1, collect, &a);
    TEST_ASSERT_EQUAL_STRING("I (1) x: colour", a.last);
}

TEST_CASE("level names round-trip", "[log]")
{
    TEST_ASSERT_EQUAL(ESP_OK, espos_log_set_level("ringtest", "debug"));
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, esp_log_level_get("ringtest"));
    TEST_ASSERT_EQUAL_STRING("debug", espos_log_level_name(ESP_LOG_DEBUG));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_log_set_level("ringtest", "loud"));
    TEST_ASSERT_EQUAL(ESP_OK, espos_log_set_level("ringtest", "info"));
}

/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Log ring: a byte ring of [u16 len][text] records behind esp_log's
 * vprintf hook. The hook formats once into a stack buffer, strips ANSI
 * colour codes, splits on newlines and stores each line under the next
 * sequence number, then forwards the original call to the previous
 * vprintf so the console still sees everything.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "freertos/timers.h"
#include "sdkconfig.h"

#include "espos_log.h"

#define RING_SIZE   CONFIG_ESPOS_LOG_RING_SIZE
#define LOG_LINE_MAX    CONFIG_ESPOS_LOG_LINE_MAX
#define NOTIFY_MS   500

static struct {
    SemaphoreHandle_t lock;
    uint8_t *buf;
    size_t head;            /* offset of the oldest record */
    size_t tail;            /* offset where the next record goes */
    size_t used;
    uint32_t first;
    uint32_t next;
    uint32_t dropped;
    bool dirty;
    char partial[CONFIG_ESPOS_LOG_LINE_MAX];
    size_t partial_len;
    vprintf_like_t prev;
    TimerHandle_t timer;
    espos_log_notify_cb_t notify;
    void *notify_arg;
} s;

static void ring_write(size_t off, const void *src, size_t n)
{
    const uint8_t *p = src;
    size_t first = RING_SIZE - off;
    if (n <= first) {
        memcpy(s.buf + off, p, n);
    } else {
        memcpy(s.buf + off, p, first);
        memcpy(s.buf, p + first, n - first);
    }
}

static void ring_read(size_t off, void *dst, size_t n)
{
    uint8_t *p = dst;
    size_t first = RING_SIZE - off;
    if (n <= first) {
        memcpy(p, s.buf + off, n);
    } else {
        memcpy(p, s.buf + off, first);
        memcpy(p + first, s.buf, n - first);
    }
}

static uint16_t rec_len_at(size_t off)
{
    uint16_t len;
    ring_read(off, &len, sizeof(len));
    return len;
}

/* Lock held. */
static void pop_oldest(void)
{
    uint16_t len = rec_len_at(s.head);
    s.head = (s.head + sizeof(len) + len) % RING_SIZE;
    s.used -= sizeof(len) + len;
    s.first++;
    s.dropped++;
}

/* Lock held. */
static void push(const char *line, size_t len)
{
    if (len > (size_t)LOG_LINE_MAX) {
        len = LOG_LINE_MAX;
    }
    size_t need = sizeof(uint16_t) + len;
    while (RING_SIZE - s.used < need) {
        pop_oldest();
    }
    uint16_t l = (uint16_t)len;
    ring_write(s.tail, &l, sizeof(l));
    ring_write((s.tail + sizeof(l)) % RING_SIZE, line, len);
    s.tail = (s.tail + need) % RING_SIZE;
    s.used += need;
    s.next++;
    s.dirty = true;
}

/* Copy src into dst without ANSI escape sequences (ESC '[' ... letter). */
static size_t strip_ansi(char *dst, const char *src, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (src[i] == '\033' && i + 1 < n && src[i + 1] == '[') {
            i += 2;
            while (i < n && !((src[i] >= 'A' && src[i] <= 'Z') || (src[i] >= 'a' && src[i] <= 'z'))) {
                i++;
            }
            continue;   /* skips the final letter too */
        }
        dst[o++] = src[i];
    }
    return o;
}

static int hook(const char *fmt, va_list args)
{
    char raw[LOG_LINE_MAX + 1];
    char clean[LOG_LINE_MAX + 1];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(raw, sizeof(raw), fmt, copy);
    va_end(copy);
    if (n > 0 && s.buf) {
        size_t len = (size_t)n < sizeof(raw) - 1 ? (size_t)n : sizeof(raw) - 1;
        len = strip_ansi(clean, raw, len);
        /* The lock can be unavailable when logging from a context that
         * must not block (scheduler not running yet); drop the line then. */
        if (xSemaphoreTake(s.lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            /* Log v2 emits prefix, message and newline as separate calls;
             * gather until a newline so a record is always a whole line. */
            /* A message that did not fit was cut off — including its newline
             * in all likelihood — so a truncated call ends the line too. */
            bool truncated = (size_t)n >= sizeof(raw);
            for (size_t i = 0; i < len + (truncated ? 1 : 0); i++) {
                char c = i < len ? clean[i] : '\n';
                if (c == '\n') {
                    size_t l = s.partial_len;
                    while (l > 0 && s.partial[l - 1] == '\r') {
                        l--;
                    }
                    if (l > 0) {
                        push(s.partial, l);
                    }
                    s.partial_len = 0;
                } else if (s.partial_len < (size_t)LOG_LINE_MAX) {
                    s.partial[s.partial_len++] = c;
                }
            }
            xSemaphoreGive(s.lock);
        }
    }
    return s.prev ? s.prev(fmt, args) : n;
}

static void timer_cb(TimerHandle_t t)
{
    (void)t;
    espos_log_notify_cb_t cb = NULL;
    void *cb_arg = NULL;
    uint32_t next = 0;
    if (xSemaphoreTake(s.lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    if (s.dirty) {
        s.dirty = false;
        cb = s.notify;
        cb_arg = s.notify_arg;
        next = s.next;
    }
    xSemaphoreGive(s.lock);
    if (cb) {
        cb(next, cb_arg);
    }
}

esp_err_t espos_log_init(void)
{
    if (s.buf) {
        return ESP_OK;
    }
    s.lock = xSemaphoreCreateMutex();
    s.buf = malloc(RING_SIZE);
    if (!s.lock || !s.buf) {
        free(s.buf);
        s.buf = NULL;
        return ESP_ERR_NO_MEM;
    }
    s.head = s.tail = s.used = 0;
    s.first = s.next = 1;
    s.dropped = 0;
    s.timer = xTimerCreate("espos_log", pdMS_TO_TICKS(NOTIFY_MS), pdTRUE, NULL, timer_cb);
    if (!s.timer || xTimerStart(s.timer, 0) != pdPASS) {
        free(s.buf);
        s.buf = NULL;
        return ESP_ERR_NO_MEM;
    }
    s.prev = esp_log_set_vprintf(hook);
    return ESP_OK;
}

void espos_log_deinit(void)
{
    if (!s.buf) {
        return;
    }
    esp_log_set_vprintf(s.prev ? s.prev : vprintf);
    xTimerStop(s.timer, portMAX_DELAY);
    xTimerDelete(s.timer, portMAX_DELAY);
    xSemaphoreTake(s.lock, portMAX_DELAY);
    uint8_t *b = s.buf;
    s.buf = NULL;
    free(b);
    xSemaphoreGive(s.lock);
}

void espos_log_stats(espos_log_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    out->size = RING_SIZE;
    if (!s.buf) {
        return;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    out->first = s.first;
    out->next = s.next;
    out->used = s.used;
    out->dropped = s.dropped;
    xSemaphoreGive(s.lock);
}

size_t espos_log_read(uint32_t after, size_t limit, espos_log_visit_cb_t cb, void *arg)
{
    if (!s.buf) {
        return 0;
    }
    char line[LOG_LINE_MAX + 1];
    size_t visited = 0;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    size_t off = s.head;
    for (uint32_t seq = s.first; seq < s.next; seq++) {
        uint16_t len = rec_len_at(off);
        size_t text = (off + sizeof(len)) % RING_SIZE;
        if ((int32_t)(seq - after) > 0) {
            ring_read(text, line, len);
            line[len] = '\0';
            visited++;
            if (!cb(seq, line, len, arg) || (limit && visited >= limit)) {
                break;
            }
        }
        off = (text + len) % RING_SIZE;
    }
    xSemaphoreGive(s.lock);
    return visited;
}

void espos_log_set_notify(espos_log_notify_cb_t cb, void *arg)
{
    if (!s.lock) {
        return;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.notify = cb;
    s.notify_arg = arg;
    xSemaphoreGive(s.lock);
}

static const struct { const char *name; esp_log_level_t level; } LEVELS[] = {
    { "none", ESP_LOG_NONE }, { "error", ESP_LOG_ERROR }, { "warn", ESP_LOG_WARN },
    { "info", ESP_LOG_INFO }, { "debug", ESP_LOG_DEBUG }, { "verbose", ESP_LOG_VERBOSE },
};

const char *espos_log_level_name(int level)
{
    for (size_t i = 0; i < sizeof(LEVELS) / sizeof(LEVELS[0]); i++) {
        if ((int)LEVELS[i].level == level) {
            return LEVELS[i].name;
        }
    }
    return "unknown";
}

esp_err_t espos_log_set_level(const char *tag, const char *level)
{
    if (!tag || !*tag || !level) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < sizeof(LEVELS) / sizeof(LEVELS[0]); i++) {
        if (strcasecmp(LEVELS[i].name, level) == 0) {
            esp_log_level_set(tag, LEVELS[i].level);
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

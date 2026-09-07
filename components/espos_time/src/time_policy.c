/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <string.h>

#include "espos_time_policy.h"

void espos_time_policy_init(espos_time_policy_t *p, const espos_time_policy_port_t *port, void *ctx,
                            uint32_t stale_after_h)
{
    memset(p, 0, sizeof(*p));
    p->port = port;
    p->ctx = ctx;
    p->stale_after_h = stale_after_h;
    p->src = ESPOS_TIME_SRC_NONE;
}

esp_err_t espos_time_policy_set(espos_time_policy_t *p, int64_t unix_ms, espos_time_src_t src)
{
    return espos_time_policy_set_aged(p, unix_ms, src, 0);
}

esp_err_t espos_time_policy_set_aged(espos_time_policy_t *p, int64_t unix_ms, espos_time_src_t src,
                                     int64_t prior_age_ms)
{
    if (unix_ms <= 0 || src <= ESPOS_TIME_SRC_NONE || src >= ESPOS_TIME_SRC_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    /* The enum IS the ranking, which is why its values were chosen in that
     * order: a source at or above the incumbent may write, anything below is
     * refused. "At" rather than "above" so a source can keep refreshing its
     * own clock — SNTP re-syncing every hour is the normal case, not a
     * downgrade. */
    if (p->src != ESPOS_TIME_SRC_NONE && src < p->src) {
        return ESP_ERR_INVALID_STATE;
    }
    int64_t mono = p->port->now_ms(p->ctx);
    p->epoch_at_zero_ms = unix_ms - mono;
    p->set_at_mono_ms = mono;
    p->prior_age_ms = prior_age_ms > 0 ? prior_age_ms : 0;
    p->src = src;
    p->sets++;
    return ESP_OK;
}

int64_t espos_time_policy_now_ms(const espos_time_policy_t *p)
{
    if (p->src == ESPOS_TIME_SRC_NONE) {
        return 0;
    }
    return p->epoch_at_zero_ms + p->port->now_ms(p->ctx);
}

int64_t espos_time_policy_at_ms(const espos_time_policy_t *p, int64_t mono_ms)
{
    if (p->src == ESPOS_TIME_SRC_NONE) {
        return 0;
    }
    return p->epoch_at_zero_ms + mono_ms;
}

bool espos_time_policy_is_synced(const espos_time_policy_t *p)
{
    if (p->src == ESPOS_TIME_SRC_NONE) {
        return false;
    }
    /* Only an RTC value ages. Everything else was learned this boot from a
     * source that was live at the time, so it is as good as it will get until
     * that source speaks again. */
    if (p->src == ESPOS_TIME_SRC_RTC && p->stale_after_h > 0) {
        int64_t age_ms = p->prior_age_ms + (p->port->now_ms(p->ctx) - p->set_at_mono_ms);
        if (age_ms >= (int64_t)p->stale_after_h * 3600 * 1000) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------- ISO 8601 */

/* Days from the civil epoch (1970-01-01) to y-m-d, and back. Howard Hinnant's
 * days_from_civil / civil_from_days: exact for the whole int64 range, no
 * lookup tables, no libc. The era arithmetic shifts the year so that March is
 * month 0, which makes the leap day the last day of the "year" and removes
 * every special case from the month-length sum. */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);              /* 0-399 */
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; /* 0-365 */
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;  /* 0-146096 */
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yr = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = yr + (*m <= 2);
}

size_t espos_time_iso8601_format(int64_t unix_ms, char *buf, size_t n)
{
    if (!buf || n < ESPOS_TIME_ISO_MAX) {
        if (buf && n) {
            buf[0] = '\0';
        }
        return 0;
    }
    if (unix_ms <= 0) {
        buf[0] = '\0';
        return 0;
    }
    /* Floor division, not truncation: a negative instant is not reachable
     * through the guard above, but the arithmetic stays correct either way and
     * the millisecond remainder is never negative. */
    int64_t secs = unix_ms / 1000;
    int32_t ms = (int32_t)(unix_ms % 1000);
    if (ms < 0) {
        ms += 1000;
        secs--;
    }
    int64_t days = secs / 86400;
    int32_t sod = (int32_t)(secs % 86400);
    if (sod < 0) {
        sod += 86400;
        days--;
    }
    int64_t y;
    unsigned mo, d;
    civil_from_days(days, &y, &mo, &d);
    int n_written = snprintf(buf, n, "%04lld-%02u-%02uT%02d:%02d:%02d.%03dZ", (long long)y, mo, d,
                             (int)(sod / 3600), (int)((sod / 60) % 60), (int)(sod % 60), (int)ms);
    if (n_written < 0 || (size_t)n_written >= n) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n_written;
}

/* Read exactly `digits` decimal digits; returns false if any is not one. */
static bool take_digits(const char **s, int digits, int *out)
{
    int v = 0;
    for (int i = 0; i < digits; i++) {
        char c = **s;
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + (c - '0');
        (*s)++;
    }
    *out = v;
    return true;
}

int64_t espos_time_iso8601_parse(const char *s)
{
    if (!s) {
        return 0;
    }
    int year, month, day, hour, minute, second;
    if (!take_digits(&s, 4, &year) || *s++ != '-' || !take_digits(&s, 2, &month) || *s++ != '-' ||
        !take_digits(&s, 2, &day)) {
        return 0;
    }
    /* Both separators a SignalK server might use. */
    if (*s != 'T' && *s != 't' && *s != ' ') {
        return 0;
    }
    s++;
    if (!take_digits(&s, 2, &hour) || *s++ != ':' || !take_digits(&s, 2, &minute) || *s++ != ':' ||
        !take_digits(&s, 2, &second)) {
        return 0;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
        return 0;
    }
    int ms = 0;
    if (*s == '.' || *s == ',') {
        s++;
        /* Any number of fractional digits: take the first three as
         * milliseconds and discard the rest, which is what a server sending
         * microseconds expects of a millisecond consumer. */
        int seen = 0;
        if (*s < '0' || *s > '9') {
            return 0; /* a separator with no digits after it is malformed */
        }
        while (*s >= '0' && *s <= '9') {
            if (seen < 3) {
                ms = ms * 10 + (*s - '0');
                seen++;
            }
            s++;
        }
        for (; seen < 3; seen++) {
            ms *= 10;
        }
    }
    int32_t offset_s = 0;
    if (*s == 'Z' || *s == 'z') {
        s++;
    } else if (*s == '+' || *s == '-') {
        int sign = (*s == '-') ? -1 : 1;
        s++;
        int oh, om;
        if (!take_digits(&s, 2, &oh)) {
            return 0;
        }
        if (*s == ':') {
            s++;
        }
        if (!take_digits(&s, 2, &om)) {
            return 0;
        }
        offset_s = sign * (oh * 3600 + om * 60);
    } else if (*s != '\0') {
        return 0;
    }
    if (*s != '\0') {
        return 0; /* trailing junk: not a timestamp we understand */
    }
    int64_t days = days_from_civil(year, (unsigned)month, (unsigned)day);
    int64_t secs = days * 86400 + hour * 3600 + minute * 60 + second - offset_s;
    int64_t ums = secs * 1000 + ms;
    /* 0 is the "no time" sentinel of this whole component, so an instant that
     * lands exactly on the epoch is reported as unparseable rather than as a
     * clock nobody set. Nothing at sea is timestamped 1970-01-01T00:00:00Z on
     * purpose. */
    return ums > 0 ? ums : 0;
}

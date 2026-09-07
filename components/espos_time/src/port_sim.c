/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * time_port.h on the linux target. There is no SNTP client and no memory that
 * outlives the process, so espos_time on the host starts unsynced and stays
 * that way until something calls espos_time_set() — which is exactly the state
 * the REST harness and the unit tests want to exercise. The monotonic clock
 * and the timezone are real; nothing else here has anything to stand on.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"

#include "time_port.h"

static const char *TAG = "espos_time";

int64_t espos_time_port_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

esp_err_t espos_time_port_sntp_init(const char *server0, const char *server1, bool from_dhcp,
                                    void (*cb)(int64_t unix_ms))
{
    (void)server0;
    (void)server1;
    (void)from_dhcp;
    (void)cb;
    ESP_LOGD(TAG, "sim: no SNTP client on the host");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t espos_time_port_sntp_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void espos_time_port_sntp_deinit(void)
{
}

/* Deliberately a no-op: the harness runs as an ordinary user process and
 * settimeofday() would need root and would move the developer's own clock. */
void espos_time_port_set_system_time(int64_t unix_ms)
{
    (void)unix_ms;
}

void espos_time_port_set_tz(const char *posix_tz)
{
    setenv("TZ", posix_tz && *posix_tz ? posix_tz : "UTC0", 1);
    tzset();
}

/* The same breakdown as port_idf.c, deliberately: newlib's struct tm has no
 * tm_gmtoff, so the offset is derived rather than read, and the host must not
 * take a different path or it would test something the device never runs. */
void espos_time_port_localtime(int64_t unix_ms, espos_time_parts_t *out)
{
    time_t secs = (time_t)(unix_ms / 1000);
    struct tm local;
    struct tm utc;
    memset(&local, 0, sizeof(local));
    memset(&utc, 0, sizeof(utc));
    localtime_r(&secs, &local);
    gmtime_r(&secs, &utc);
    out->year = local.tm_year + 1900;
    out->month = (uint8_t)(local.tm_mon + 1);
    out->day = (uint8_t)local.tm_mday;
    out->hour = (uint8_t)local.tm_hour;
    out->minute = (uint8_t)local.tm_min;
    out->second = (uint8_t)local.tm_sec;
    out->millisecond = (uint16_t)(unix_ms % 1000);
    out->wday = (uint8_t)local.tm_wday;
    out->yday = (uint16_t)local.tm_yday;
    int32_t local_sod = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
    int32_t utc_sod = utc.tm_hour * 3600 + utc.tm_min * 60 + utc.tm_sec;
    int32_t day_diff = 0;
    if (local.tm_year != utc.tm_year) {
        day_diff = local.tm_year > utc.tm_year ? 1 : -1;
    } else if (local.tm_yday != utc.tm_yday) {
        day_diff = local.tm_yday > utc.tm_yday ? 1 : -1;
    }
    out->utc_offset_s = local_sod - utc_sod + day_diff * 86400;
}

void espos_time_port_rtc_store(int64_t unix_ms, espos_time_src_t src)
{
    (void)unix_ms;
    (void)src;
}

bool espos_time_port_rtc_take(int64_t *unix_ms, espos_time_src_t *src, int64_t *age_ms)
{
    (void)unix_ms;
    (void)src;
    (void)age_ms;
    return false;
}

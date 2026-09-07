/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * time_port.h on a chip: esp_timer for the monotonic base, esp_netif_sntp for
 * the network clock, settimeofday so the rest of the system agrees with us,
 * and RTC no-init memory to carry the time across a deep sleep.
 */
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "time_port.h"

static const char *TAG = "espos_time";

/* Chips with neither RTC fast nor RTC slow memory (none of the current
 * espOS targets, but the attribute is a hard static_assert where it is
 * missing) simply have no carry across a deep sleep. */
#if CONFIG_SOC_RTC_FAST_MEM_SUPPORTED || CONFIG_SOC_RTC_SLOW_MEM_SUPPORTED
#define HAVE_RTC_CARRY 1
#else
#define HAVE_RTC_CARRY 0
#endif

int64_t espos_time_port_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* ------------------------------------------------------------- SNTP */

static void (*s_sync_cb)(int64_t unix_ms);
static bool s_sntp_inited;

/* Runs on IDF's SNTP task. esp_netif_sntp has already applied the time to the
 * system clock by the time this is called; we only forward it. */
static void on_sntp_sync(struct timeval *tv)
{
    if (s_sync_cb && tv) {
        s_sync_cb((int64_t)tv->tv_sec * 1000 + tv->tv_usec / 1000);
    }
}

esp_err_t espos_time_port_sntp_init(const char *server0, const char *server1, bool from_dhcp,
                                    void (*cb)(int64_t unix_ms))
{
    if (s_sntp_inited) {
        espos_time_port_sntp_deinit();
    }
    const char *servers[2];
    size_t n = 0;
    if (server0 && *server0) {
        servers[n++] = server0;
    }
    if (server1 && *server1 && n < 2) {
        servers[n++] = server1;
    }
    if (n == 0 && !from_dhcp) {
        return ESP_ERR_INVALID_ARG; /* nothing to ask */
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(0, {});
    cfg.num_of_servers = n;
    for (size_t i = 0; i < n; i++) {
        cfg.servers[i] = servers[i];
    }
    /* start=false: esp_netif_sntp_init() runs during espos_time_start(), which
     * is before any transport has an address. Polling begins on the first
     * NETWORK_UP instead, so the first packet has a route to travel on.
     * wait_for_sync=false: nothing here ever blocks waiting for a server. */
    cfg.start = false;
    cfg.wait_for_sync = false;
    cfg.sync_cb = on_sntp_sync;
    cfg.server_from_dhcp = from_dhcp;
    /* A DHCP-supplied server replaces the preconfigured list, so it has to be
     * re-applied every time a lease arrives — a router reboot, a roam to
     * another access point — or the device keeps asking a server the new
     * network never offered. */
    cfg.renew_servers_after_new_IP = from_dhcp;
    cfg.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
    cfg.index_of_first_server = n;
    s_sync_cb = cb;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        s_sync_cb = NULL;
        return err;
    }
    s_sntp_inited = true;
    return ESP_OK;
}

esp_err_t espos_time_port_sntp_start(void)
{
    if (!s_sntp_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_netif_sntp_start();
}

void espos_time_port_sntp_deinit(void)
{
    if (s_sntp_inited) {
        esp_netif_sntp_deinit();
        s_sntp_inited = false;
        s_sync_cb = NULL;
    }
}

/* ------------------------------------------------------- system clock */

void espos_time_port_set_system_time(int64_t unix_ms)
{
    struct timeval tv = {
        .tv_sec = (time_t)(unix_ms / 1000),
        .tv_usec = (suseconds_t)((unix_ms % 1000) * 1000),
    };
    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGW(TAG, "settimeofday failed");
    }
}

void espos_time_port_set_tz(const char *posix_tz)
{
    setenv("TZ", posix_tz && *posix_tz ? posix_tz : "UTC0", 1);
    tzset();
}

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
    /* Newlib's struct tm has no tm_gmtoff, so the offset is the difference
     * between the two breakdowns of the same instant. Comparing seconds-in-day
     * alone would be wrong across a date boundary (a +13 zone is already
     * tomorrow), so the day difference — always -1, 0 or +1 — is folded in. */
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

/* --------------------------------------------------- deep-sleep carry */

#if HAVE_RTC_CARRY

/* The magic is checked because RTC memory is uninitialised at power-on: it
 * holds whatever the last power cycle left, and reading that as a timestamp
 * would give the device a confident, arbitrary idea of the date. */
#define RTC_MAGIC 0x54494d45u /* "TIME" */

static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    int64_t unix_ms;      /* the instant when the chip went to sleep */
    uint8_t src;          /* which source it came from */
    uint32_t check;       /* magic ^ low word of unix_ms: catches a half-written record */
} s_rtc;

static uint32_t rtc_check(uint32_t magic, int64_t unix_ms, uint8_t src)
{
    return magic ^ (uint32_t)(unix_ms & 0xffffffffu) ^ ((uint32_t)src << 24);
}

void espos_time_port_rtc_store(int64_t unix_ms, espos_time_src_t src)
{
    if (unix_ms <= 0) {
        return;
    }
    s_rtc.magic = RTC_MAGIC;
    s_rtc.unix_ms = unix_ms;
    s_rtc.src = (uint8_t)src;
    s_rtc.check = rtc_check(RTC_MAGIC, unix_ms, (uint8_t)src);
}

bool espos_time_port_rtc_take(int64_t *unix_ms, espos_time_src_t *src, int64_t *age_ms)
{
    /* Only a deep-sleep wake. After a power-on, a panic or an OTA reboot the
     * record may look intact and still be arbitrarily old — a device off for a
     * month would come up certain it is still last month. */
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {
        return false;
    }
    if (s_rtc.magic != RTC_MAGIC || s_rtc.check != rtc_check(RTC_MAGIC, s_rtc.unix_ms, s_rtc.src) ||
        s_rtc.unix_ms <= 0) {
        return false;
    }
    /* How much time the sleep itself accounted for. IDF keeps the system clock
     * running across deep sleep from the RTC timer, so gettimeofday() on this
     * boot already includes the nap; the stored instant is the floor and the
     * difference is what the chip slept. When the system clock was never set
     * (nothing ran settimeofday before the sleep) there is nothing to compare
     * against and the sleep counts as instantaneous — an underestimate that
     * only ever makes the restored time look FRESHER than it is for the
     * staleness rule, which is why that rule also measures from adoption. */
    struct timeval tv;
    int64_t sys_ms = 0;
    if (gettimeofday(&tv, NULL) == 0) {
        sys_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    }
    int64_t slept = sys_ms > s_rtc.unix_ms ? sys_ms - s_rtc.unix_ms : 0;
    *age_ms = slept;
    *unix_ms = s_rtc.unix_ms + slept;
    *src = ESPOS_TIME_SRC_RTC;
    return true;
}

#else /* no RTC memory on this chip */

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

#endif

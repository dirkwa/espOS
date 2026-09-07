/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * What espos_time needs from the platform: the monotonic clock, the SNTP
 * client, the timezone, and the scrap of memory that survives a deep sleep.
 * port_idf.c on chips, port_sim.c on the linux target, where there is no
 * SNTP client and no memory that outlives the process.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "espos_time.h"

/** Monotonic milliseconds since boot. 64-bit: the wall clock is built on it
 * and it must not wrap. */
int64_t espos_time_port_now_ms(void);

/**
 * Arm the SNTP client with up to two servers (either may be NULL or "") and,
 * when `from_dhcp`, whatever the DHCP lease offered. Does NOT start polling —
 * espos_time_port_sntp_start() does, on the first NETWORK_UP — because
 * esp_netif_sntp_init() with start=true would send its first packet before
 * there is a route. `cb` runs on IDF's SNTP task with the synced instant.
 * ESP_ERR_NOT_SUPPORTED where there is no client (the host).
 */
esp_err_t espos_time_port_sntp_init(const char *server0, const char *server1, bool from_dhcp,
                                    void (*cb)(int64_t unix_ms));
esp_err_t espos_time_port_sntp_start(void);
void espos_time_port_sntp_deinit(void);

/** Hand the system libc clock the time espos_time settled on, so anything
 * calling gettimeofday() (mbedTLS certificate validity, IDF's own log
 * timestamps) agrees with us. */
void espos_time_port_set_system_time(int64_t unix_ms);

/** setenv("TZ") + tzset(), so localtime_r() below follows the string. */
void espos_time_port_set_tz(const char *posix_tz);

/** Break `unix_ms` down in the timezone last given to _set_tz(). */
void espos_time_port_localtime(int64_t unix_ms, espos_time_parts_t *out);

/* ---------------------------------------------------- deep-sleep carry */

/**
 * Remember the current instant in memory that survives a deep sleep, so a
 * device that naps between measurements wakes up still knowing roughly what
 * time it is instead of starting from nothing.
 */
void espos_time_port_rtc_store(int64_t unix_ms, espos_time_src_t src);

/**
 * The instant a previous boot stored, if this boot woke from deep sleep and
 * the stored record is intact. False otherwise — including every power-on,
 * where the RTC memory holds whatever the last power cycle left and must not
 * be trusted. `age_ms` is how long the chip was asleep, as far as it can tell.
 */
bool espos_time_port_rtc_take(int64_t *unix_ms, espos_time_src_t *src, int64_t *age_ms);

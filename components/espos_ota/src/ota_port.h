/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * What espos_ota needs from the platform; port_idf.c on chips, port_sim.c
 * on the linux target (no flash, downloads are only counted).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    char version[32];
    char project[32];
    char idf[32];
    char date[16];
    char time[16];
    char slot[17];            /* running partition label */
    char other_slot[17];      /* the update target */
    char state[16];           /* valid | pending_verify | new | invalid | aborted | undefined */
    bool pending_verify;      /* rollback armed for this boot */
    bool rolled_back;         /* an invalid image is recorded in the other slot */
    char other_version[32];   /* version in the other slot, if readable */
} espos_ota_port_info_t;

void espos_ota_port_info(espos_ota_port_info_t *out);

typedef void (*espos_ota_progress_cb_t)(size_t received, size_t total, void *arg);

/**
 * Download + write + validate the image at url. Blocks. On success the boot
 * partition is switched to the new image. err_text receives a short human
 * reason on failure. Progress is reported through cb.
 */
esp_err_t espos_ota_port_install(const char *url, bool allow_insecure, const char *expect_project,
                                 espos_ota_progress_cb_t cb, void *arg, char *err_text, size_t err_size);

/** Fetch a small text resource (manifest). malloc'ed, NUL-terminated. */
esp_err_t espos_ota_port_fetch(const char *url, bool allow_insecure, size_t max, char **out, size_t *len, int *status);

esp_err_t espos_ota_port_mark_valid(void);
esp_err_t espos_ota_port_mark_invalid_and_reboot(void);
void espos_ota_port_reboot(void);
uint32_t espos_ota_port_uptime_s(void);

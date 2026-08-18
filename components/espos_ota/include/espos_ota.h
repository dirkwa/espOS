/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_ota — signed over-the-air updates with rollback.
 *
 * - Update from a URL (POST /api/v1/ota {"url"}) or from a version manifest
 *   (ota.manifest_url + channel; docs/ota.md), checked after boot and every
 *   ota.check_h hours; installed automatically only if ota.auto_install.
 * - Every image is signature-checked on write (SECURE_SIGNED_ON_UPDATE);
 *   the public key is compiled into the running firmware.
 * - A new image boots PENDING_VERIFY. It confirms itself once WiFi is
 *   connected (or, with no station configured, after a grace period); if it
 *   cannot within ota.confirm_tmo_s it is marked invalid and the device
 *   reboots into the previous slot. A panic before confirmation rolls back
 *   through the bootloader.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t espos_ota_start(void);
void espos_ota_stop(void);

typedef enum {
    ESPOS_OTA_IDLE = 0,
    ESPOS_OTA_CHECKING,       /* fetching the manifest */
    ESPOS_OTA_AVAILABLE,      /* a newer build is known (not installing) */
    ESPOS_OTA_DOWNLOADING,
    ESPOS_OTA_VERIFYING,      /* image written, validating / setting boot partition */
    ESPOS_OTA_READY,          /* installed; rebooting shortly */
    ESPOS_OTA_FAILED,         /* last_error set; back to idle on the next action */
} espos_ota_state_t;

const char *espos_ota_state_name(espos_ota_state_t s);

/** Fetch the manifest now (async; result in status / SSE "ota"). */
esp_err_t espos_ota_check_now(void);
/** Install from an explicit URL (async). ESP_ERR_INVALID_STATE while busy. */
esp_err_t espos_ota_install_url(const char *url);
/** Install the build found by the last check. ESP_ERR_NOT_FOUND if none. */
esp_err_t espos_ota_install_available(void);
/** Confirm the running image (cancels a pending rollback). */
esp_err_t espos_ota_confirm(void);
/** Mark the running image invalid and reboot into the other slot. */
esp_err_t espos_ota_rollback(void);

/** Status document of docs/api.md (malloc'ed). */
char *espos_ota_status_json(void);

#ifdef __cplusplus
}
#endif

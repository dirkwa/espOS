/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * The strong half of espos_ota's server-URL hook.
 *
 * espos_ota declares it weak and returns ESP_ERR_NOT_SUPPORTED, so a firmware
 * with no SignalK client still builds and says so plainly when configured to
 * ask a server it cannot talk to. This overrides it when espos_sk IS in the
 * build, which is what makes ota.manifest_src = "signalk" work.
 *
 * The override direction avoids a cycle: espos_ota would otherwise have to
 * depend on espos_sk for one convenience, and a headless OTA-only firmware is
 * a legitimate thing.
 *
 * espos_sk builds WHOLE_ARCHIVE for the same reason espos_time does: nothing
 * calls into this file, so without it the linker never pulls the object out
 * of the archive and the weak stub wins silently. That exact bug shipped once
 * already (/system/info reporting no clock while /time reported sntp), which
 * is why it is stated here rather than assumed.
 */
#include <stddef.h>

#include "esp_err.h"
#include "espos_sk_http.h"

esp_err_t espos_ota_server_url_hook(const char *path, char *out, size_t n)
{
    return espos_sk_url(path, out, n);
}

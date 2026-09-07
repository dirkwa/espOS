/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * Config-descriptor migrations for the `sk` namespace.
 *
 * Its own file for one reason: this is the only part of espos_sk that runs
 * before anything else. A migration has to be registered before
 * espos_config_init(), which espos_init() calls, and espos_sk has no hook that
 * early -- so the C runtime is the hook. Keeping it apart from sk_store.c also
 * keeps that file linkable by the Unity host test, which builds a handful of
 * espos_sk sources directly and has no espos_config to talk to.
 */
#include <string.h>

#include "esp_log.h"

#include "espos_cfg_keys.h"
#include "espos_config.h"

static const char *TAG = "espos_sk";

/* ---------------------------------------------------------- sk v1 -> v2
 *
 * `tls` (bool) became `scheme` (auto | http | https).
 *
 * true maps to "https": a device that had TLS switched on must not quietly
 * drop to plaintext on the next boot, and "https" is the reading that keeps it
 * talking to the same server the same way. Not "auto" -- auto would probe, and
 * a server that has since stopped advertising TLS would silently take the
 * device back to plaintext, which is precisely the change nobody asked for.
 *
 * false maps to "auto", which is the new default. false was never a decision;
 * it was the old default nobody touched, and auto is what that now means.
 */
static esp_err_t migrate_v1_to_v2(espos_config_migrate_ctx_t *ctx, void *arg)
{
    (void)arg;
    bool tls = false;
    size_t n = sizeof(tls);
    if (espos_config_migrate_get(ctx, "tls", ESPOS_CFG_TYPE_BOOL, &tls, &n) == ESP_OK && tls) {
        static const char scheme[] = "https";
        (void)espos_config_migrate_set(ctx, ESPOS_CFG_SK_SCHEME, ESPOS_CFG_TYPE_STRING, scheme, sizeof(scheme));
        ESP_LOGI(TAG, "config migration: sk.tls was on, so sk.scheme is now \"https\"");
    }
    /* The key is gone from the descriptor; leaving the stored value behind
     * would keep costing an NVS entry for ever and confuse anyone reading the
     * partition. */
    (void)espos_config_migrate_erase(ctx, "tls");
    return ESP_OK;
}

/* Before app_main, and so before espos_config_init(). The same reason
 * espos_health builds its mutex in a constructor; a step registered any later
 * would be treated as additive and `scheme` would silently read its default,
 * turning a TLS install back into a plaintext one on the upgrade boot. */
static void __attribute__((constructor)) register_sk_migrations(void)
{
    /* No ESP_LOGx here: the log ring is not up this early. A failure would
     * mean the descriptor version and this step disagree, which is a
     * programming error the config store already complains about at init. */
    (void)espos_config_register_migration(ESPOS_CFG_NS_SK, 1, migrate_v1_to_v2, NULL);
}

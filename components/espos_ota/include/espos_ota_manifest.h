/*
 * SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * Version manifest (docs/ota.md) — pure C, host-tested.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPOS_OTA_VERSION_MAX 32
#define ESPOS_OTA_URL_MAX     256
#define ESPOS_OTA_NOTES_MAX   128

typedef struct {
    char version[ESPOS_OTA_VERSION_MAX];
    char url[ESPOS_OTA_URL_MAX];      /* absolute; relative manifest URLs are resolved */
    char sha256[65];                  /* hex or "" */
    char notes[ESPOS_OTA_NOTES_MAX];
    size_t size;                      /* 0 = unknown */
    bool newer;                       /* strictly newer than the running version */
} espos_ota_build_t;

/**
 * Compare two version strings: numeric dot-separated core (1.2.10 > 1.2.9),
 * a "-prerelease" suffix sorts before the same core without one, then plain
 * strcmp on the remainder. Unparsable strings compare as strings.
 * Returns <0, 0, >0.
 */
int espos_ota_version_cmp(const char *a, const char *b);

/**
 * Pick the best build for (app, target, channel) from manifest JSON:
 * highest version among matching entries; missing "channel" means "stable",
 * missing "app" matches any. Relative "url" is resolved against
 * manifest_url. Returns ESP_OK with *out filled, ESP_ERR_NOT_FOUND when no
 * entry matches, ESP_ERR_INVALID_ARG on malformed JSON / schema.
 */
esp_err_t espos_ota_manifest_pick(const char *json, size_t len, const char *manifest_url,
                                  const char *app, const char *target, const char *channel,
                                  const char *running_version, espos_ota_build_t *out);

/** Resolve rel against base (RFC 3986-lite: absolute, "/path", "path"). */
bool espos_ota_resolve_url(const char *base, const char *rel, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

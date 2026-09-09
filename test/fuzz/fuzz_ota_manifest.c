/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_ota_manifest_pick() and espos_ota_resolve_url() — the JSON that
 * decides what firmware a device installs next.
 *
 * Worth fuzzing for a reason beyond "it parses network input": it fills
 * FIXED-SIZE buffers (version[32], url[256], sha256[65], notes[128]) from
 * strings a manifest chose, and it resolves a relative URL against a base.
 * Both are the shape that hides an off-by-one, and the consequence here is
 * not a wrong reading -- it is which image the device fetches.
 *
 * The harness checks the invariant the header promises rather than only
 * looking for crashes: on ESP_OK every buffer is NUL-terminated within its
 * bound. A truncation that forgets the terminator is not a crash, so a
 * sanitizer alone would not catch it; the abort() below does.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "espos_ota_manifest.h"

/* Fixed-size field is NUL-terminated somewhere inside its own bound. */
static void must_terminate(const char *field, size_t cap)
{
    if (memchr(field, '\0', cap) == NULL) {
        abort(); /* unterminated: the next strlen() runs off the struct */
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* First byte selects the caller's context, so one corpus exercises both
     * "no app filter" and a named app, both channels, and a running version
     * that is and is not newer. The rest is the manifest. */
    if (size < 1) {
        return 0;
    }
    const uint8_t sel = data[0];
    const uint8_t *json = data + 1;
    const size_t json_len = size - 1;

    const char *app = (sel & 1) ? "cockpit" : NULL;
    const char *target = (sel & 2) ? "esp32p4" : "esp32c6";
    const char *channel = (sel & 4) ? "beta" : "stable";
    const char *running = (sel & 8) ? "1.0.0" : "99.0.0";
    const char *base = (sel & 16) ? "https://example.invalid/ota/manifest.json" : "http://h/m.json";

    /* Exact-sized copy: the parser takes a length, so a heap block that ends
     * at json_len turns a one-byte overread into a report rather than a
     * silent read of the fuzzer's own memory. */
    char *buf = (char *)malloc(json_len ? json_len : 1);
    if (!buf) {
        return 0;
    }
    memcpy(buf, json, json_len);

    espos_ota_build_t out;
    memset(&out, 0xAA, sizeof(out)); /* poison: nothing may be read as a string unless set */

    esp_err_t err = espos_ota_manifest_pick(buf, json_len, base, app, target, channel, running, &out);
    if (err == ESP_OK) {
        must_terminate(out.version, sizeof(out.version));
        must_terminate(out.url, sizeof(out.url));
        must_terminate(out.sha256, sizeof(out.sha256));
        must_terminate(out.notes, sizeof(out.notes));
    }
    free(buf);

    /* The URL resolver, on its own: it writes into a caller buffer and is the
     * half that turns a manifest's "url" into what the device actually GETs.
     * NUL-terminate a copy for it -- unlike the parser it takes C strings. */
    if (json_len > 1) {
        size_t half = json_len / 2;
        char *rel = (char *)malloc(half + 1);
        if (rel) {
            memcpy(rel, json, half);
            rel[half] = '\0';
            char resolved[ESPOS_OTA_URL_MAX];
            memset(resolved, 0xAA, sizeof(resolved));
            if (espos_ota_resolve_url(base, rel, resolved, sizeof(resolved))) {
                must_terminate(resolved, sizeof(resolved));
            }
            free(rel);
        }
    }

    /* Version comparison, over the same bytes: it is called on strings that
     * came out of a manifest, so it sees whatever survived parsing. */
    if (json_len > 2) {
        char *v = (char *)malloc(json_len + 1);
        if (v) {
            memcpy(v, json, json_len);
            v[json_len] = '\0';
            (void)espos_ota_version_cmp(v, "1.2.3");
            (void)espos_ota_version_cmp("1.2.3", v);
            (void)espos_ota_version_cmp(v, v);
            free(v);
        }
    }

    return 0;
}

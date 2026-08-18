/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "espos_ota_manifest.h"

/* Parse up to 4 numeric components; returns count, sets *rest to the
 * suffix ("-beta.1" or "" or garbage). Leading 'v' is skipped. */
static int parse_core(const char *s, long *n, const char **rest)
{
    if (*s == 'v' || *s == 'V') {
        s++;
    }
    int cnt = 0;
    while (cnt < 4 && isdigit((unsigned char)*s)) {
        char *end;
        n[cnt++] = strtol(s, &end, 10);
        s = end;
        if (*s == '.' && isdigit((unsigned char)s[1])) {
            s++;
        } else {
            break;
        }
    }
    *rest = s;
    return cnt;
}

int espos_ota_version_cmp(const char *a, const char *b)
{
    long na[4] = { 0 }, nb[4] = { 0 };
    const char *ra, *rb;
    int ca = parse_core(a, na, &ra);
    int cb = parse_core(b, nb, &rb);
    if (ca == 0 || cb == 0) {
        return strcmp(a, b);
    }
    for (int i = 0; i < 4; i++) {
        if (na[i] != nb[i]) {
            return na[i] < nb[i] ? -1 : 1;
        }
    }
    /* Same core: a prerelease ("-...") is older than a release; "+build" is ignored. */
    bool pa = ra[0] == '-', pb = rb[0] == '-';
    if (pa != pb) {
        return pa ? -1 : 1;
    }
    if (pa) {
        return strcmp(ra, rb);
    }
    return 0;
}

bool espos_ota_resolve_url(const char *base, const char *rel, char *out, size_t out_size)
{
    if (!rel || !*rel) {
        return false;
    }
    if (strstr(rel, "://")) {
        return snprintf(out, out_size, "%s", rel) < (int)out_size;
    }
    if (!base || !strstr(base, "://")) {
        return false;
    }
    const char *scheme_end = strstr(base, "://") + 3;
    const char *host_end = strchr(scheme_end, '/');
    if (rel[0] == '/') {
        int n = host_end ? (int)(host_end - base) : (int)strlen(base);
        return snprintf(out, out_size, "%.*s%s", n, base, rel) < (int)out_size;
    }
    const char *last = strrchr(base, '/');
    if (!last || last < scheme_end) {
        return snprintf(out, out_size, "%s/%s", base, rel) < (int)out_size;
    }
    return snprintf(out, out_size, "%.*s/%s", (int)(last - base), base, rel) < (int)out_size;
}

static const char *str_or(const cJSON *o, const char *k, const char *dflt)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : dflt;
}

esp_err_t espos_ota_manifest_pick(const char *json, size_t len, const char *manifest_url,
                                  const char *app, const char *target, const char *channel,
                                  const char *running_version, espos_ota_build_t *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *schema = cJSON_GetObjectItem(root, "schema");
    if (cJSON_IsNumber(schema) && schema->valueint != 1) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    const char *mapp = str_or(root, "app", NULL);
    if (mapp && app && strcmp(mapp, app) != 0) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }
    const cJSON *builds = cJSON_GetObjectItem(root, "builds");
    if (!cJSON_IsArray(builds)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *best = NULL;
    const cJSON *b;
    cJSON_ArrayForEach(b, builds) {
        if (!cJSON_IsObject(b)) {
            continue;
        }
        const char *bt = str_or(b, "target", NULL);
        const char *bv = str_or(b, "version", NULL);
        const char *bu = str_or(b, "url", NULL);
        const char *bc = str_or(b, "channel", "stable");
        const char *ba = str_or(b, "app", NULL);
        if (!bt || !bv || !bu || strcmp(bt, target) != 0 || strcmp(bc, channel) != 0) {
            continue;
        }
        if (ba && app && strcmp(ba, app) != 0) {
            continue;
        }
        if (!best || espos_ota_version_cmp(bv, str_or(best, "version", "")) > 0) {
            best = b;
        }
    }
    if (!best) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }
    snprintf(out->version, sizeof(out->version), "%s", str_or(best, "version", ""));
    if (!espos_ota_resolve_url(manifest_url, str_or(best, "url", ""), out->url, sizeof(out->url))) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(out->sha256, sizeof(out->sha256), "%s", str_or(best, "sha256", ""));
    snprintf(out->notes, sizeof(out->notes), "%s", str_or(best, "notes", ""));
    const cJSON *sz = cJSON_GetObjectItem(best, "size");
    out->size = cJSON_IsNumber(sz) && sz->valuedouble > 0 ? (size_t)sz->valuedouble : 0;
    out->newer = running_version && *running_version ? espos_ota_version_cmp(out->version, running_version) > 0 : true;
    cJSON_Delete(root);
    return ESP_OK;
}

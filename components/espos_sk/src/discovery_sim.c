/*
 * SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * Host stand-in for mDNS discovery: servers come from the environment,
 *   ESPOS_SIM_SK_SERVERS="host,port,self,name;host,port,self,name"
 * (self/name optional; URNs contain colons, hence commas). Empty or unset =
 * nothing discovered.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "espos_sk_priv.h"

esp_err_t espos_sk_discovery_init(const char *hostname)
{
    (void)hostname;
    return ESP_OK;
}

size_t espos_sk_discovery_run(espos_sk_discovered_t *out, size_t max)
{
    const char *env = getenv("ESPOS_SIM_SK_SERVERS");
    if (!env || !*env) {
        return 0;
    }
    char *copy = strdup(env);
    if (!copy) {
        return 0;
    }
    size_t n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(copy, ";", &save); tok && n < max; tok = strtok_r(NULL, ";", &save)) {
        espos_sk_discovered_t *d = &out[n];
        memset(d, 0, sizeof(*d));
        char *f = tok;
        char *host = strsep(&f, ",");
        char *port = f ? strsep(&f, ",") : NULL;
        char *self = f ? strsep(&f, ",") : NULL;
        char *name = f;
        if (!host || !port) {
            continue;
        }
        snprintf(d->host, sizeof(d->host), "%s", host);
        d->port = (uint16_t)atoi(port);
        if (self) {
            snprintf(d->self, sizeof(d->self), "%s", self);
        }
        snprintf(d->name, sizeof(d->name), "%s", name ? name : "sim");
        snprintf(d->roles, sizeof(d->roles), "master, main");
        snprintf(d->swname, sizeof(d->swname), "signalk-server");
        snprintf(d->swvers, sizeof(d->swvers), "sim");
        n++;
    }
    free(copy);
    return n;
}

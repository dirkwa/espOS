/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution */
#pragma once
#include <stdbool.h>
#include <string.h>
#include "unity.h"
#include "espos_config.h"
#include "espos_config_backend.h"
#include "espos_cfg_keys.h"

/* Fresh in-memory store, initialised. Returns the backend ctx (owned by the
 * fixture; freed by fixture_teardown). */
espos_config_mem_t *fixture_setup(void);
void fixture_teardown(espos_config_mem_t *m);

/* Change-notification recorder shared by tests. */
#define REC_MAX 32
typedef struct {
    int count;
    char path[REC_MAX][40];
} change_rec_t;
void rec_cb(const char *ns, const char *key, void *arg);
bool rec_has(const change_rec_t *r, const char *path);

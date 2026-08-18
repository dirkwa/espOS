/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution */
#include <stdio.h>
#include <stdlib.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "esp_private/partition_linux.h"
#include "test_common.h"

espos_config_mem_t *fixture_setup(void)
{
    espos_config_mem_t *m = espos_config_mem_create();
    TEST_ASSERT_NOT_NULL(m);
    TEST_ESP_OK(espos_config_init(espos_config_backend_mem(), m));
    return m;
}

void fixture_teardown(espos_config_mem_t *m)
{
    espos_config_deinit();
    espos_config_mem_destroy(m);
}

void rec_cb(const char *ns, const char *key, void *arg)
{
    change_rec_t *r = arg;
    if (r->count < REC_MAX) {
        snprintf(r->path[r->count], sizeof(r->path[0]), "%s.%s", ns, key);
    }
    r->count++;
}

bool rec_has(const change_rec_t *r, const char *path)
{
    for (int i = 0; i < r->count && i < REC_MAX; i++) {
        if (strcmp(r->path[i], path) == 0) {
            return true;
        }
    }
    return false;
}

void setUp(void) {}
void tearDown(void)
{
    /* Make sure a failing test cannot leak an initialised store into the next. */
    espos_config_deinit();
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = UNITY_END();
    /* The emulated flash is a temp file under /tmp; do not leave it behind.
     * (esp_partition_file_mmap() clears the ctrl_input struct after mapping,
     * so the flag has to be set right before unmapping.) */
    esp_partition_get_file_mmap_ctrl_input()->remove_dump = true;
    esp_partition_file_munmap();
    fflush(stdout);
    exit(failures ? 1 : 0);
}

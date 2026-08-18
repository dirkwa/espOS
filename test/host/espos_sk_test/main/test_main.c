/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution */
#include <stdio.h>
#include <stdlib.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "esp_private/partition_linux.h"

void setUp(void) {}
void tearDown(void) {}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = UNITY_END();
    esp_partition_get_file_mmap_ctrl_input()->remove_dump = true;
    esp_partition_file_munmap();
    fflush(stdout);
    exit(failures ? 1 : 0);
}

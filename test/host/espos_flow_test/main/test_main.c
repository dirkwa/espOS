/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The graph tests emit from the test's own task rather than from a started
 * loop, which is what makes them deterministic — post, run, assert, with no
 * sleeping and nothing racing. espos_flow_adopt_loop() is the supported way
 * to say so: it hands the loop's identity to this task while no loop task
 * exists, so CONFIG_ESPOS_FLOW_CHECK_TASK is satisfied by the truth rather
 * than switched off. Once espos_flow_start() has run it refuses, which is the
 * behaviour the check is there for.
 */
#include <stdio.h>
#include <stdlib.h>
#include "unity.h"
#include "unity_test_runner.h"

#include "espos_flow.h"

void setUp(void) {}
void tearDown(void) {}

void app_main(void)
{
    ESP_ERROR_CHECK(espos_flow_adopt_loop());

    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = UNITY_END();

    espos_flow_release_loop();
    fflush(stdout);
    exit(failures ? 1 : 0);
}

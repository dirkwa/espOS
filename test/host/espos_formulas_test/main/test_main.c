/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * espos_formulas is pure arithmetic with nothing to start. The transform
 * tests need one thing: this task must BE the flow loop, so that emit()'s
 * wrong-task check is satisfied by the truth rather than switched off, and so
 * that nothing sleeps or races. espos_flow_adopt_loop() says exactly that,
 * and refuses once a real loop task exists.
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

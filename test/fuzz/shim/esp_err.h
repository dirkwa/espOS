/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * The three lines of esp_err.h the fuzz harnesses need.
 *
 * IDF's own header includes esp_compiler.h and pulls the component tree in
 * behind it. The harnesses build the parsers directly with the host compiler
 * -- that is the whole point of those parsers being IDF-free -- so this
 * supplies the type and the two codes they use and nothing else.
 *
 * The values match IDF's. They have to: a harness that checked for a
 * different ESP_ERR_NOT_FOUND would pass while the firmware failed.
 */
#pragma once

#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK                0
#define ESP_FAIL              (-1)
#define ESP_ERR_NO_MEM        0x101
#define ESP_ERR_INVALID_ARG   0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE  0x104
#define ESP_ERR_NOT_FOUND     0x105

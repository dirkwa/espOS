/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "esp_err.h"

/* M1 scaffolding: bring up WiFi STA with compile-time credentials so the
 * REST API is reachable. Replaced by espos_wifi in M2. */
esp_err_t net_bootstrap_start(void);

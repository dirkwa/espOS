/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include "espos_httpd.h"

esp_err_t espos_httpd_register_config_api(httpd_handle_t h);
esp_err_t espos_httpd_register_system_api(httpd_handle_t h);
esp_err_t espos_httpd_register_static(httpd_handle_t h);

/* true once a reboot/factory-reset has been accepted; writes are refused. */
bool espos_httpd_restart_pending(void);

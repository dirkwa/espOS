# SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
idf_component_register(
    SRCS main.c
    INCLUDE_DIRS "."
    PRIV_REQUIRES espos_config espos_httpd espos_wifi espos_sk espos_ota nvs_flash esp_partition esp_system)

espos_config_add_descriptor(config/app.json)

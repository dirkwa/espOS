# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# esp_driver_pcnt is named even for chips without a PCNT unit (ESP32-C3):
# the component exists on every target and compiles to nothing there, while
# an `if(CONFIG_SOC_PCNT_SUPPORTED)` around it would be empty on the first
# CMake pass, before sdkconfig exists. main.c chooses with SOC_PCNT_SUPPORTED.
idf_component_register(
    SRCS main.c
    PRIV_REQUIRES espos_core espos_config espos_sk esp_driver_pcnt esp_driver_gpio esp_timer)

espos_config_add_descriptor(config/app.json)

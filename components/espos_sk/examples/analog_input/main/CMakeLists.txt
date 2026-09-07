# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# espos_sk is named so espos_start() finds and starts it: a consumer builds
# with MINIMAL_BUILD, and only what main requires exists. esp_adc is the
# IDF 6 oneshot driver together with the calibration schemes.
idf_component_register(
    SRCS main.c
    PRIV_REQUIRES espos_core espos_config espos_sk esp_adc)

espos_config_add_descriptor(config/app.json)

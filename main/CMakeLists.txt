# SPDX-License-Identifier: Apache-2.0
idf_component_register(
    SRCS main.c
    INCLUDE_DIRS "."
    PRIV_REQUIRES espos_config espos_log espos_httpd espos_wifi espos_sk)

espos_config_add_descriptor(config/app.json)

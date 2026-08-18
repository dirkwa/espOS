# SPDX-License-Identifier: Apache-2.0
idf_component_register(
    SRCS main.c
    INCLUDE_DIRS "."
    PRIV_REQUIRES espos_config espos_log espos_httpd espos_wifi espos_sk espos_ota)

espos_config_add_descriptor(config/app.json)

# `idf.py -DESPOS_BROKEN_BUILD=1 build` makes an image that aborts in app_main:
# the M6 rollback acceptance test (docs/ota.md). Never set by default.
if(ESPOS_BROKEN_BUILD)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ESPOS_BROKEN_BUILD=1)
endif()

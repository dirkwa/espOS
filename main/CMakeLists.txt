# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# espos_core starts whatever is built. The optional components are named here
# so the reference project keeps building every one of them (CI covers all
# targets that way), not because main.c calls into them.
idf_component_register(
    SRCS main.c
    INCLUDE_DIRS "."
    PRIV_REQUIRES espos_core espos_config espos_sk espos_wifi
                  espos_ota espos_ble espos_n2k espos_voice espos_audio)

espos_config_add_descriptor(config/app.json)

# `idf.py -DESPOS_BROKEN_BUILD=1 build` makes an image that aborts in app_main:
# the M6 rollback acceptance test (docs/ota.md). Never set by default.
if(ESPOS_BROKEN_BUILD)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ESPOS_BROKEN_BUILD=1)
endif()

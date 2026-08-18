# SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
set(ota_src "${CMAKE_CURRENT_LIST_DIR}/../../../../components/espos_ota")
idf_component_register(
    SRCS test_main.c test_manifest.c "${ota_src}/src/manifest.c"
    INCLUDE_DIRS "." "${ota_src}/include"
    PRIV_REQUIRES unity espressif__cjson
    WHOLE_ARCHIVE)

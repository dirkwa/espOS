# SPDX-License-Identifier: Apache-2.0
set(sk_src "${CMAKE_CURRENT_LIST_DIR}/../../../../components/espos_sk")
idf_component_register(
    SRCS test_main.c test_token_sm.c test_store.c test_delta.c "${sk_src}/src/token_sm.c" "${sk_src}/src/sk_store.c" "${sk_src}/src/sk_delta.c"
    INCLUDE_DIRS "." "${sk_src}/include" "${sk_src}/src"
    PRIV_REQUIRES unity nvs_flash esp_partition esp_hw_support
    WHOLE_ARCHIVE)

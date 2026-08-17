# SPDX-License-Identifier: Apache-2.0
set(sk_src "${CMAKE_CURRENT_LIST_DIR}/../../../../components/espos_sk")
idf_component_register(
    SRCS test_main.c test_token_sm.c "${sk_src}/src/token_sm.c"
    INCLUDE_DIRS "." "${sk_src}/include"
    PRIV_REQUIRES unity
    WHOLE_ARCHIVE)

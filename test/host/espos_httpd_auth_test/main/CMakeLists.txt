# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# The policy source is compiled straight from the component rather than by
# requiring espos_httpd: auth_policy.c depends on nothing but its own header
# and libc, while the component as a whole would pull esp_http_server, the
# config store on emulated NVS and a registry download of cJSON into a test
# of some sixty asserts. What runs is still the file that ships.
idf_component_register(
    SRCS test_main.c test_auth_policy.c
         "${CMAKE_CURRENT_LIST_DIR}/../../../../components/espos_httpd/src/auth_policy.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../../../../components/espos_httpd/include"
    PRIV_REQUIRES unity
    WHOLE_ARCHIVE)

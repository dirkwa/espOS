# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
# Compiles the machine's source directly: no netif, config or httpd needed.
set(net_src "${CMAKE_CURRENT_LIST_DIR}/../../../../components/espos_net")
idf_component_register(
    SRCS test_main.c test_net_sm.c "${net_src}/src/net_sm.c"
    INCLUDE_DIRS "." "${net_src}/include"
    PRIV_REQUIRES unity
    WHOLE_ARCHIVE)

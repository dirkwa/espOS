# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# Just the policy source, the way espos_sk_test compiles the token machine and
# the delta engine: what is under test here is pure C with an injected clock,
# and pulling the whole component in would drag espos_httpd and the config
# store along for no coverage.
set(time_src "${CMAKE_CURRENT_LIST_DIR}/../../../../components/espos_time")
idf_component_register(
    SRCS test_main.c test_policy.c "${time_src}/src/time_policy.c"
    INCLUDE_DIRS "." "${time_src}/include" "${time_src}/src"
    PRIV_REQUIRES unity
    WHOLE_ARCHIVE)

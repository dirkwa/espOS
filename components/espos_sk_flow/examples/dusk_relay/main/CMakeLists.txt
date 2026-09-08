# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# main.cpp, not main.c: the graph is the C++ half. app_main stays extern "C",
# which is the only concession the language change asks for.
idf_component_register(SRCS main.cpp
    PRIV_REQUIRES espos_core espos_flow espos_sensors espos_sk espos_sk_flow)

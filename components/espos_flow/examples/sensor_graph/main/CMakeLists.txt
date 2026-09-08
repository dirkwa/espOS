# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# main.cpp, not main.c: the graph is the C++ half of espos_flow. app_main
# still has to be extern "C", which is the only concession the language
# change asks for.
idf_component_register(SRCS main.cpp PRIV_REQUIRES espos_core espos_flow espos_sk)

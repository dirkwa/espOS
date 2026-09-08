# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# Two kinds of test in one project.
#
# test_sched.c compiles espos_sched.c directly, the way espos_sk_test compiles
# the token machine: the wheel is pure C with an injected clock, and driving
# it by hand is what makes a 32-bit wrap testable in microseconds.
#
# test_graph.cpp links the REAL component and drives it through
# espos_flow_run_until_idle() — edges, Join's ages and the mailbox are the
# runtime, not a model of it, and the linux target runs them unchanged.
set(flow_src "${CMAKE_CURRENT_LIST_DIR}/../../../../components/espos_flow")
idf_component_register(
    SRCS test_main.c test_sched.c test_flow_c.c test_graph.cpp
    INCLUDE_DIRS "." "${flow_src}/include"
    PRIV_REQUIRES unity espos_flow
    WHOLE_ARCHIVE)

# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# Two layers in one project.
#
# test_units / test_curve / test_marine cover espos_formulas, which is
# header-only and has no dependencies at all: the reference values are checked
# against exactly the code the firmware runs, with nothing mocked.
#
# test_transforms covers the NODES in espos_flow/transforms/, driven through
# the real espos_flow on the linux target -- the arithmetic is already proved
# above, so what is under test there is the node behaviour that only exists
# once a value is flowing (max_skips, hysteresis direction, ring wrap, gating).
#
# ESPOS_FLOW_HAVE_CONFIG is deliberately NOT defined here: the transforms must
# work with no config store, which is what a host test proves and what a
# firmware built without espos_config gets.
idf_component_register(
    SRCS test_main.c test_units.cpp test_curve.cpp test_marine.cpp
         test_transforms.cpp
    INCLUDE_DIRS "."
    PRIV_REQUIRES unity espos_formulas espos_flow
    WHOLE_ARCHIVE)

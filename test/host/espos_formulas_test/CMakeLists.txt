# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# Host (linux target) unit tests for espos_formulas: the marine arithmetic
# checked against PUBLISHED reference values -- Arden Buck's own table, NOAA's
# heat-index chart, resistive-divider algebra worked by hand, every unit
# conversion against its SI definition, and Curve at, between, below and above
# its sample points. Build & run:
#
#   ../../../scripts/build.sh --preview set-target linux && ../../../scripts/build.sh build && ./build/espos_formulas_test.elf
#
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../components")
project(espos_formulas_test)

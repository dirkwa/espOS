# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# Host (linux target) unit tests for espos_time's pure policy: source ranking,
# staleness, ISO 8601 formatting and parsing. Build & run:
#
#   ../../../scripts/build.sh --preview set-target linux && ../../../scripts/build.sh build && ./build/espos_time_test.elf
#
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../components")
project(espos_time_test)

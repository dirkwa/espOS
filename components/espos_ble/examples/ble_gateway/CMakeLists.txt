# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# A complete IDF project. espOS is four directories up and comes in through
# the shared prologue, exactly as a firmware that vendors espOS as a submodule
# includes espos/cmake/espos_project.cmake.
#
# COMPONENTS espos_ble is what puts the gateway in the build: the prologue
# excludes every optional espOS component a project does not name, because the
# component manager resolves every visible manifest before MINIMAL_BUILD trims
# the graph -- without the list, esp-sr and esp-dl are downloaded and compiled
# for a headless gateway that links none of them.
cmake_minimum_required(VERSION 3.22)
include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/espos_project.cmake")
espos_project_prologue(NAME "ble-gateway"
                       PARTITIONS "${CMAKE_CURRENT_LIST_DIR}/../../../../partitions/16mb.csv"
                       COMPONENTS espos_ble)
project(ble_gateway)
espos_project_ui_partition()

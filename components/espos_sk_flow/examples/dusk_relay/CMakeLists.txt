# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# A complete IDF project. espOS is four directories up and comes in through
# the shared prologue, exactly as a firmware that vendors espOS as a submodule
# includes espos/cmake/espos_project.cmake.
cmake_minimum_required(VERSION 3.22)
include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/espos_project.cmake")
espos_project_prologue(NAME "dusk_relay")
project(dusk_relay)
espos_project_ui_partition()

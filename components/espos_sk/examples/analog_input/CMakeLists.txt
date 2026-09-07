# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# A complete IDF project. espOS is four directories up and comes in through
# the shared prologue, exactly as a firmware that vendors espOS as a submodule
# includes espos/cmake/espos_project.cmake: the IDF version policy,
# sdkconfig.d/espos.defaults, the 4 MB partition table, the components on
# EXTRA_COMPONENT_DIRS and the app-signing key are all decided there. No
# COMPONENTS are named because SignalK is in the core set.
cmake_minimum_required(VERSION 3.22)
include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/espos_project.cmake")
espos_project_prologue(NAME "analog_input")
project(analog_input)
espos_project_ui_partition()

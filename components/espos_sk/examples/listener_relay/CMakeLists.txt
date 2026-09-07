# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# A complete IDF project on the shared prologue (see analog_input for the
# reasoning); espOS is four directories up. No COMPONENTS: SignalK is core.
cmake_minimum_required(VERSION 3.22)
include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/espos_project.cmake")
espos_project_prologue(NAME "listener_relay")
project(listener_relay)
espos_project_ui_partition()

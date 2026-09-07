# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# Two-phase boot: espos_init() + espos_start_network() with the
# application's own work in between. The build side is the same as every
# espOS project's — the shared prologue does the IDF check, the sdkconfig
# defaults, the partition table, the components and the signing key.
cmake_minimum_required(VERSION 3.22)
include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/espos_project.cmake")
espos_project_prologue(NAME "two_phase_boot")
project(two_phase_boot)
espos_project_ui_partition()

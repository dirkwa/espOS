# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# A status LED driven from the health table, and an application condition
# the watchdog policy acts on. The build side is the same as every espOS
# project's — the shared prologue does the IDF check, the sdkconfig
# defaults, the partition table, the components and the signing key.
cmake_minimum_required(VERSION 3.22)
include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/espos_project.cmake")
espos_project_prologue(NAME "health_and_led")
project(health_and_led)
espos_project_ui_partition()

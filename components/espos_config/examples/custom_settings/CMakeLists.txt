# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# An application with settings of its own. The descriptor is registered in
# main/CMakeLists.txt; the build side here is the same as every espOS
# project's — the shared prologue does the IDF check, the sdkconfig
# defaults, the partition table, the components and the signing key.
cmake_minimum_required(VERSION 3.22)
include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/espos_project.cmake")
espos_project_prologue(NAME "custom_settings")
project(custom_settings)
espos_project_ui_partition()

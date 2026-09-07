# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# The example app. Everything a firmware needs before project() — the IDF
# version policy, espOS's sdkconfig defaults and partition table, its
# components, the app-signing key and the re-sign workaround — lives in
# cmake/espos_project.cmake, which every project built on espOS includes;
# espOS uses the same path so CI exercises it. There is no sdkconfig.defaults
# here: sdkconfig.d/espos.defaults IS the project's defaults, the same file
# every consumer inherits.
cmake_minimum_required(VERSION 3.22)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/espos_project.cmake")
# PARTITIONS is the default, spelled out: the example app's table is the one
# CI's size budget and the flashing docs assume.
espos_project_prologue(NAME "espOS" PARTITIONS "${CMAKE_CURRENT_LIST_DIR}/partitions/4mb.csv")

project(espos)

# Committed UI bundle (ui/dist-gz) → LittleFS "storage" partition image,
# flashed with `idf.py flash`. A missing bundle is a broken checkout and a
# hard error; `npm run build` in ui/ only when the UI itself changes.
espos_project_ui_partition(NAME "espOS")

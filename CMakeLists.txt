# SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
#
# The example app. Everything a firmware needs before project() — the IDF
# version pin, espOS's components, the app-signing key and the re-sign
# workaround — lives in cmake/espos_project.cmake, which every project built
# on espOS includes; espOS uses the same path so CI exercises it.
cmake_minimum_required(VERSION 3.22)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/espos_project.cmake")
espos_project_prologue(NAME "espOS")

project(espos)

# UI bundle → LittleFS "storage" partition image, flashed with `idf.py flash`.
# Built by `npm run build` in ui/ (dist-gz/); without it the device serves the
# embedded placeholder page and the image step is skipped (not an error).
espos_project_ui_partition(NAME "espOS")

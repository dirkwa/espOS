# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# The smallest espOS firmware. Everything a project needs before project() —
# the IDF version policy, espOS's sdkconfig defaults and partition table, its
# components, the app-signing key — is the shared prologue; nothing is copied
# into this directory. Used as a template outside the espOS tree, the include
# becomes "${CMAKE_CURRENT_LIST_DIR}/espos/cmake/espos_project.cmake" with
# espOS as the `espos/` submodule (docs/development.md).
cmake_minimum_required(VERSION 3.22)
include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/espos_project.cmake")
# No COMPONENTS argument: the core set (config, httpd, wifi, sk, ota, ...)
# needs no listing there; only the optional radios and codecs (espos_ble,
# espos_n2k, espos_voice, espos_audio) are named. PARTITIONS is left at the
# default 4 MB table.
espos_project_prologue(NAME "minimal")
project(minimal)
# The committed web UI bundle → LittleFS "storage" image, written by `idf.py flash`.
espos_project_ui_partition()

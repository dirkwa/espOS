# SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
#
# Host (linux target) unit tests for espos_config. Build & run:
#
#   idf.py --preview set-target linux && idf.py build && ./build/espos_config_test.elf
#
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../components")
project(espos_config_test)

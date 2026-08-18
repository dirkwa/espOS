# SPDX-License-Identifier: Apache-2.0
#
# Host unit tests for the espos_log ring (vprintf hook, wrap, paging).
#   idf.py --preview set-target linux && idf.py build && ./build/espos_log_test.elf
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../components")
set(COMPONENTS main)
project(espos_log_test)

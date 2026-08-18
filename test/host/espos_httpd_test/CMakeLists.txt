# SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
#
# Host (linux target) integration test for the REST API: the real espos_httpd
# on the FreeRTOS POSIX simulator with the NVS backend on emulated flash.
#
#   idf.py --preview set-target linux && idf.py build
#   python3 run_test.py          # starts build/espos_httpd_test.elf and drives it
#
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../components")
project(espos_httpd_test)

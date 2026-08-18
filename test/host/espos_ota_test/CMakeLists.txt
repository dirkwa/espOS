# SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
#
# Host unit tests for the espos_ota manifest / version logic (pure C).
#   idf.py --preview set-target linux && idf.py build && ./build/espos_ota_test.elf
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)
project(espos_ota_test)

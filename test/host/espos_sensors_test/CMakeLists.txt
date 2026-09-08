# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# Host (linux target) unit tests for espos_sensors' arithmetic: the ADC's
# uncalibrated counts-to-volts conversion, the pulse counter's wrap handling
# and its counts-to-frequency, and the tachometer's pulses-per-revolution.
#
# Those four are the only places in the component where a wrong answer is
# SILENT -- everything else either works or returns an error -- so they are
# pure functions in espos_sensor_math.c and tested here without hardware.
# The drivers call them; this is not a parallel implementation.
#
#   ../../../scripts/build.sh --preview set-target linux && ../../../scripts/build.sh build && ./build/espos_sensors_test.elf
#
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../components")
project(espos_sensors_test)

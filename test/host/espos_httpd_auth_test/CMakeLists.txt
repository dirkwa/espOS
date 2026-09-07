# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# Host (linux target) unit tests for espos_httpd's authentication policy —
# sessions, the failure throttle and the decision — driven with a fake clock
# and a fake entropy source, so expiry and lockouts take no real time. The
# REST harness next door (espos_httpd_test/run_test.py, AuthTests) covers the
# HTTP side against real headers. Build & run:
#
#   ../../../scripts/build.sh --preview set-target linux && ../../../scripts/build.sh build && ./build/espos_httpd_auth_test.elf
#
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)
project(espos_httpd_auth_test)

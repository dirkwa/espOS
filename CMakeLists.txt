# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.22)

# Pinned in .idf-version; the build fails loudly on a mismatched IDF so that a
# stale environment does not produce a subtly different binary.
file(READ "${CMAKE_CURRENT_LIST_DIR}/.idf-version" ESPOS_IDF_VERSION)
string(STRIP "${ESPOS_IDF_VERSION}" ESPOS_IDF_VERSION)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(ESPOS_IDF_ACTUAL "v${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}")
if(NOT ESPOS_IDF_ACTUAL STREQUAL ESPOS_IDF_VERSION)
    if(ESPOS_ALLOW_IDF_MISMATCH)
        message(WARNING "espOS: .idf-version pins ${ESPOS_IDF_VERSION} but IDF_PATH is ${ESPOS_IDF_ACTUAL}")
    else()
        message(FATAL_ERROR "espOS: .idf-version pins ${ESPOS_IDF_VERSION} but IDF_PATH is ${ESPOS_IDF_ACTUAL}. "
                            "Source the matching export.sh, or pass -DESPOS_ALLOW_IDF_MISMATCH=1.")
    endif()
endif()

project(espos)

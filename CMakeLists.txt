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

# App-signing key (see sdkconfig.defaults / docs/ota.md). Never committed.
# Missing → generate a DEVELOPMENT key so the build works out of the box;
# devices flashed with it will only accept updates signed by that same key.
set(ESPOS_SIGNING_KEY "${CMAKE_CURRENT_LIST_DIR}/secure_boot_signing_key.pem")
if(NOT EXISTS "${ESPOS_SIGNING_KEY}")
    message(WARNING "espOS: no secure_boot_signing_key.pem — generating a development RSA-3072 signing key. "
                    "For real deployments create and keep your own (docs/ota.md).")
    execute_process(
        COMMAND ${PYTHON} -m espsecure generate_signing_key --version 2 --scheme rsa3072 "${ESPOS_SIGNING_KEY}"
        RESULT_VARIABLE espos_key_rc OUTPUT_QUIET)
    if(NOT espos_key_rc EQUAL 0)
        message(FATAL_ERROR "espOS: could not generate secure_boot_signing_key.pem (espsecure missing?)")
    endif()
endif()

project(espos)

# UI bundle → LittleFS "storage" partition image, flashed with `idf.py flash`.
# Built by `npm run build` in ui/ (dist-gz/); without it the device serves the
# embedded placeholder page and the image step is skipped (not an error).
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/ui/dist-gz/index.html.gz")
    littlefs_create_partition_image(storage "${CMAKE_CURRENT_LIST_DIR}/ui/dist-gz" FLASH_IN_PROJECT)
    message(STATUS "espOS: UI bundle from ui/dist-gz will be flashed to 'storage'")
else()
    message(STATUS "espOS: ui/dist-gz missing — run 'npm run build' in ui/ to include the web UI")
endif()

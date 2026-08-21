# SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
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

# Re-sign when the KEY changes, not just when the code does.
#
# ESP-IDF's signing step depends on the unsigned binary alone
# (esptool_py/project_include.cmake: DEPENDS "${build_dir}/.bin_timestamp"),
# so swapping secure_boot_signing_key.pem and rebuilding silently keeps the
# signature made with the PREVIOUS key — no source changed, so nothing
# re-links, and the build log looks completely normal. You get a device that
# installs fine over USB and then rejects every OTA, with no way to tell why
# short of running `espsecure verify-signature` by hand.
#
# Fingerprint the key (the file's hash — never its contents, which would
# reach the build directory) and force a re-link whenever it differs.
file(SHA256 "${ESPOS_SIGNING_KEY}" ESPOS_SIGNING_KEY_HASH)
set(ESPOS_KEY_STAMP "${CMAKE_BINARY_DIR}/espos_signing_key.stamp")
set(ESPOS_KEY_STAMP_OLD "")
if(EXISTS "${ESPOS_KEY_STAMP}")
    file(READ "${ESPOS_KEY_STAMP}" ESPOS_KEY_STAMP_OLD)
endif()
if(NOT ESPOS_KEY_STAMP_OLD STREQUAL ESPOS_SIGNING_KEY_HASH)
    if(NOT ESPOS_KEY_STAMP_OLD STREQUAL "")
        message(STATUS "espOS: signing key changed — forcing a re-link so the "
                       "image is signed with the current key.")
        # Removing the unsigned binary's stamp is what actually re-triggers
        # IDF's sign-data step; deleting only the .bin leaves it satisfied.
        file(REMOVE "${CMAKE_BINARY_DIR}/.bin_timestamp"
                    "${CMAKE_BINARY_DIR}/.signed_bin_timestamp")
    endif()
    file(WRITE "${ESPOS_KEY_STAMP}" "${ESPOS_SIGNING_KEY_HASH}")
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

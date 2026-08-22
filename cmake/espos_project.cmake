# SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
#
# Shared project prologue/epilogue for firmwares built on espOS.
#
# Every espOS firmware needs the same four things before `project()`: the
# pinned IDF version enforced, espOS's components on EXTRA_COMPONENT_DIRS, an
# app-signing key, and the re-link-on-key-change workaround below. That was
# ~55 lines copy-pasted into each project root, which is exactly the code that
# must not drift: when the signing half of it goes wrong the symptom is a
# device that flashes fine over USB and then rejects every OTA.
#
# Usage in a project's root CMakeLists.txt:
#
#     cmake_minimum_required(VERSION 3.22)
#     include("${CMAKE_CURRENT_LIST_DIR}/espos/cmake/espos_project.cmake")
#     espos_project_prologue(NAME "ble-gateway")
#     project(ble_gateway)
#     espos_project_ui_partition()
#
# espOS itself includes it as `cmake/espos_project.cmake` — same code path, so
# the shared prologue is exercised by espOS's own CI on every target.
#
# This file includes ESP-IDF's project.cmake itself; do not include both.

include_guard(GLOBAL)

# <espos>/cmake/espos_project.cmake → <espos>
get_filename_component(ESPOS_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(NOT EXISTS "${ESPOS_DIR}/components/espos_config/CMakeLists.txt")
    message(FATAL_ERROR "espOS: '${ESPOS_DIR}/components' has no espos_config — "
                        "if espOS is a submodule, run `git submodule update --init`.")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

#
# espos_project_prologue([NAME <label>]
#                        [IDF_VERSION_FILE <path>]
#                        [SIGNING_KEY <path>])
#
# NAME              label used in messages (default: the project directory name)
# IDF_VERSION_FILE  version pin to enforce (default: the project's own
#                   .idf-version if it has one, else espOS's). When a project
#                   carries its own pin AND espOS's differs, that is a hard
#                   error rather than a silent choice between the two.
# SIGNING_KEY       app-signing key (default: <project>/secure_boot_signing_key.pem)
#
# A macro, not a function: EXTRA_COMPONENT_DIRS has to land in the caller's
# scope, where `project()` will read it.
#
macro(espos_project_prologue)
    cmake_parse_arguments(_ESPOS "" "NAME;IDF_VERSION_FILE;SIGNING_KEY" "" ${ARGN})
    if(_ESPOS_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "espos_project_prologue: unknown argument(s): ${_ESPOS_UNPARSED_ARGUMENTS}")
    endif()

    if(NOT _ESPOS_NAME)
        get_filename_component(_ESPOS_NAME "${CMAKE_SOURCE_DIR}" NAME)
    endif()

    _espos_check_idf_version("${_ESPOS_NAME}" "${_ESPOS_IDF_VERSION_FILE}")

    # In espOS's own tree components/ is already the project's component dir;
    # naming it again would register every component twice.
    if(NOT "${ESPOS_DIR}" STREQUAL "${CMAKE_SOURCE_DIR}")
        list(APPEND EXTRA_COMPONENT_DIRS "${ESPOS_DIR}/components")
    endif()

    if(NOT _ESPOS_SIGNING_KEY)
        set(_ESPOS_SIGNING_KEY "${CMAKE_SOURCE_DIR}/secure_boot_signing_key.pem")
    endif()
    _espos_signing_key("${_ESPOS_NAME}" "${_ESPOS_SIGNING_KEY}")
endmacro()

#
# Enforce the pinned ESP-IDF version so a stale environment cannot produce a
# subtly different binary. -DESPOS_ALLOW_IDF_MISMATCH=1 downgrades it to a
# warning (the pre-split projects each spelled this flag differently:
# BLEGW_/COCKPIT_/ESPOS_ALLOW_IDF_MISMATCH — it is ESPOS_ everywhere now).
#
function(_espos_check_idf_version name version_file)
    set(espos_pin "${ESPOS_DIR}/.idf-version")
    set(project_pin "${CMAKE_SOURCE_DIR}/.idf-version")

    if(version_file)
        if(NOT EXISTS "${version_file}")
            message(FATAL_ERROR "${name}: IDF_VERSION_FILE '${version_file}' does not exist.")
        endif()
    elseif(EXISTS "${project_pin}")
        set(version_file "${project_pin}")
        # Two pins that disagree is how a project ends up building against an
        # IDF its espOS was never tested on. Fail rather than pick one.
        if(EXISTS "${espos_pin}" AND NOT "${espos_pin}" STREQUAL "${project_pin}")
            file(READ "${project_pin}" a)
            file(READ "${espos_pin}" b)
            string(STRIP "${a}" a)
            string(STRIP "${b}" b)
            if(NOT a STREQUAL b)
                message(FATAL_ERROR "${name}: .idf-version pins ${a} but the espOS submodule pins ${b}. "
                                    "Align them (espOS's pin is the one its CI tests), or drop this "
                                    "project's .idf-version to follow espOS.")
            endif()
        endif()
    else()
        set(version_file "${espos_pin}")
    endif()

    file(READ "${version_file}" want)
    string(STRIP "${want}" want)
    set(have "v${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}")
    if(NOT have STREQUAL want)
        if(ESPOS_ALLOW_IDF_MISMATCH)
            message(WARNING "${name}: ${version_file} pins ${want} but IDF_PATH is ${have}")
        else()
            message(FATAL_ERROR "${name}: ${version_file} pins ${want} but IDF_PATH is ${have}. "
                                "Source the matching export.sh, or pass -DESPOS_ALLOW_IDF_MISMATCH=1.")
        endif()
    endif()
endfunction()

#
# App-signing key for espOS OTA (docs/ota.md); never committed.
# Missing → generate a DEVELOPMENT key so a fresh checkout builds; devices
# flashed with it will only accept updates signed by that same key.
#
function(_espos_signing_key name key)
    if(NOT EXISTS "${key}")
        message(WARNING "${name}: no ${key} — generating a development RSA-3072 signing key. "
                        "For real deployments create and keep your own (espos/docs/ota.md).")
        execute_process(
            COMMAND ${PYTHON} -m espsecure generate_signing_key --version 2 --scheme rsa3072 "${key}"
            RESULT_VARIABLE rc OUTPUT_QUIET)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "${name}: could not generate ${key} (espsecure missing?)")
        endif()
    endif()

    # Re-sign when the KEY changes, not just when the code does.
    #
    # ESP-IDF's signing step depends on the unsigned binary alone
    # (esptool_py/project_include.cmake: DEPENDS "${build_dir}/.bin_timestamp"),
    # so swapping the key and rebuilding silently keeps the signature made with
    # the PREVIOUS key — no source changed, so nothing re-links, and the build
    # log looks completely normal. You get a device that installs fine over USB
    # and then rejects every OTA, with no way to tell why short of running
    # `espsecure verify-signature` by hand.
    #
    # Fingerprint the key (the file's hash — never its contents, which would
    # reach the build directory) and force a re-link whenever it differs.
    file(SHA256 "${key}" hash)
    set(stamp "${CMAKE_BINARY_DIR}/espos_signing_key.stamp")
    set(old "")
    if(EXISTS "${stamp}")
        file(READ "${stamp}" old)
    endif()
    if(NOT old STREQUAL hash)
        if(NOT old STREQUAL "")
            message(STATUS "${name}: signing key changed — forcing a re-link so the "
                           "image is signed with the current key.")
            # Removing the unsigned binary's stamp is what actually re-triggers
            # IDF's sign-data step; deleting only the .bin leaves it satisfied.
            file(REMOVE "${CMAKE_BINARY_DIR}/.bin_timestamp"
                        "${CMAKE_BINARY_DIR}/.signed_bin_timestamp")
        endif()
        file(WRITE "${stamp}" "${hash}")
    endif()
endfunction()

#
# espos_project_ui_partition([PARTITION <name>] [DIR <dir>] [NAME <label>])
#
# Pack the espOS web UI into a LittleFS image flashed with `idf.py flash`.
# Call AFTER project() — littlefs_create_partition_image comes from the
# managed component. Default DIR is espOS's own ui/dist-gz, i.e. what
# `npm run build` in <espos>/ui produces.
#
function(espos_project_ui_partition)
    cmake_parse_arguments(_UI "" "PARTITION;DIR;NAME" "" ${ARGN})
    if(NOT _UI_PARTITION)
        set(_UI_PARTITION storage)
    endif()
    if(NOT _UI_DIR)
        set(_UI_DIR "${ESPOS_DIR}/ui/dist-gz")
    endif()
    if(NOT _UI_NAME)
        get_filename_component(_UI_NAME "${CMAKE_SOURCE_DIR}" NAME)
    endif()

    if(EXISTS "${_UI_DIR}/index.html.gz")
        littlefs_create_partition_image(${_UI_PARTITION} "${_UI_DIR}" FLASH_IN_PROJECT)
        message(STATUS "${_UI_NAME}: UI bundle from ${_UI_DIR} will be flashed to '${_UI_PARTITION}'")
    else()
        # WARNING, not STATUS: a STATUS line disappears into cmake's output and
        # the device then silently serves the placeholder page instead of the
        # real config UI, which looks like a firmware bug rather than a missing
        # build step.
        message(WARNING "${_UI_NAME}: ${_UI_DIR} missing — the device will serve the placeholder page, "
                        "not the espOS web UI. Build it with `npm ci && npm run build` in ${ESPOS_DIR}/ui.")
    endif()
endfunction()

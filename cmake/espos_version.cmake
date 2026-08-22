# SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
#
# Work out what version this build is, and put it where IDF will use it.
#
# version.txt alone answers "which release is this meant to be", which is not
# the question anyone asks of a device in the field. That question is "which
# build is on it", and between two releases every build answers `0.6.0` — the
# firmware on the bench and the firmware from three weeks ago are
# indistinguishable in `GET /api/v1/system/info`, in the OTA status and in the
# log banner.
#
# So: `git describe` when the checkout has tags, version.txt when it has not
# (a tarball, or a repo whose release has not been tagged yet). That is also
# the shape docs/api.md has always documented — "0.1.0-3-gabc1234".
#
# Include before project(); IDF honours a PROJECT_VER set beforehand.

include_guard(GLOBAL)

macro(espos_project_version)
    # The project being built, which for a firmware is its own repository and
    # not the espOS submodule inside it -- a consumer's version is its own.
    # CMAKE_SOURCE_DIR rather than CMAKE_CURRENT_LIST_DIR because a macro body
    # expands in the caller's context, so the latter would name whichever file
    # happened to invoke it.
    set(_espos_ver_file "${CMAKE_SOURCE_DIR}/version.txt")
    set(_espos_ver_base "")
    if(EXISTS "${_espos_ver_file}")
        file(READ "${_espos_ver_file}" _espos_ver_base)
        string(STRIP "${_espos_ver_base}" _espos_ver_base)
    endif()

    set(PROJECT_VER "${_espos_ver_base}")

    find_package(Git QUIET)
    if(GIT_FOUND)
        # --always so a checkout with no tags still yields the commit; --dirty
        # so a build with uncommitted changes says so on the device rather
        # than claiming to be the release it was branched from.
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --dirty --always
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE _espos_ver_git
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _espos_ver_rc)
        if(_espos_ver_rc EQUAL 0 AND _espos_ver_git)
            string(REGEX REPLACE "^v" "" _espos_ver_git "${_espos_ver_git}")
            # --always makes describe fall back to a bare commit hash when the
            # repository has no tags at all, and a hash is not a version: the
            # OTA manifest compares these numerically to decide whether an
            # update is newer. Take describe's answer only when it actually
            # starts with a release number.
            if(_espos_ver_git MATCHES "^([0-9]+\\.[0-9]+\\.[0-9]+)")
                set(PROJECT_VER "${_espos_ver_git}")

                # A tag that disagrees with version.txt means one of them was
                # forgotten, and the device would report whichever this file
                # happened to prefer. Say so at build time instead.
                if(_espos_ver_base AND NOT CMAKE_MATCH_1 STREQUAL _espos_ver_base)
                    message(WARNING
                        "espOS: version.txt says ${_espos_ver_base} but the nearest tag is "
                        "v${CMAKE_MATCH_1}. Bump one of them (scripts/release.sh does both).")
                endif()
            else()
                message(STATUS
                    "espOS: no release tag reachable from HEAD (${_espos_ver_git}) — reporting "
                    "${PROJECT_VER} from version.txt. Tag releases (docs/releasing.md) and "
                    "builds between them become distinguishable.")
            endif()
        endif()
    endif()

    if(NOT PROJECT_VER)
        set(PROJECT_VER "0.0.0-unknown")
        message(WARNING "espOS: no version.txt and no git — building as ${PROJECT_VER}")
    endif()
    message(STATUS "espOS: version ${PROJECT_VER}")
endmacro()

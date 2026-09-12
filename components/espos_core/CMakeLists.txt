# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# espos_core: one call brings espOS up in the right order (include/espos.h).
# The core components are hard requirements; SignalK, OTA and BLE are started
# only when the project builds them — decided here, at configure time, from
# the component list, so the application never spells the list itself.
idf_build_get_property(target IDF_TARGET)
set(reqs espos_log espos_config espos_health espos_httpd espos_net espos_event espos_time)

# WiFi is part of the core set on every chip with a radio (or, on the ESP32-P4,
# a co-processor) and absent on the 802.15.4-only H-series, where esp_wifi does
# not exist and espos_wifi cannot build. espos_project_prologue() records the
# decision as the ESPOS_WIFI build property and excludes the component; a
# project that bypasses the prologue gets the same rule from the target name.
# A hard REQUIRES rather than idf_component_optional_requires(): under a
# consumer's MINIMAL_BUILD only what is required gets compiled, and "WiFi
# vanished because nothing asked for it" is not a failure mode a firmware
# should have. Everything above the seam depends on espos_net, not on this.
idf_build_get_property(_espos_wifi ESPOS_WIFI)
if("${_espos_wifi}" STREQUAL "")
    set(_espos_no_wifi_targets esp32h2 esp32h21 esp32h4)
    if(target IN_LIST _espos_no_wifi_targets)
        set(_espos_wifi OFF)
    else()
        set(_espos_wifi ON)
    endif()
endif()
if(_espos_wifi)
    list(APPEND reqs espos_wifi)
endif()

idf_component_register(
    SRCS src/espos_core.c
    INCLUDE_DIRS include
    REQUIRES ${reqs})

# Optional stages: link them only when they are in the build, and tell the
# source which ones are, so espos_start() can call into them without an
# #include that would fail in a firmware built without them.
idf_component_optional_requires(PRIVATE espos_sk espos_ota espos_ble espos_prov)
idf_build_get_property(comps BUILD_COMPONENTS)
if(espos_wifi IN_LIST comps)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ESPOS_HAVE_WIFI=1)
endif()
if(espos_sk IN_LIST comps)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ESPOS_HAVE_SK=1)
endif()
if(espos_ota IN_LIST comps)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ESPOS_HAVE_OTA=1)
endif()
# espos_ble registers only its protocol codec on the linux target; there is
# no espos_ble_start() to call there. On chips the gateway also needs
# Bluedroid, which the source checks (CONFIG_BT_BLUEDROID_ENABLED) — a
# firmware may link the component without enabling the stack.
if(espos_ble IN_LIST comps AND NOT target STREQUAL "linux")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ESPOS_HAVE_BLE=1)
endif()

# Same shape for BLE provisioning. It also gates on CONFIG_ESPOS_PROV in the
# source: unlike the gateway, linking the component is not the same as wanting
# it started -- it suspends BLE scanning while it runs.
if(espos_prov IN_LIST comps AND NOT target STREQUAL "linux")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ESPOS_HAVE_PROV=1)
endif()

# espOS's own version, as distinct from the application's PROJECT_VER: in the
# tree it is version.txt two levels up; a copy installed from the component
# registry has no such file and carries the version in its manifest instead.
set(espos_version_file "${CMAKE_CURRENT_LIST_DIR}/../../version.txt")
if(EXISTS "${espos_version_file}")
    file(READ "${espos_version_file}" espos_version)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${espos_version_file}")
else()
    file(READ "${CMAKE_CURRENT_LIST_DIR}/idf_component.yml" espos_manifest)
    string(REGEX MATCH "\nversion: \"([^\"]+)\"" _ "${espos_manifest}")
    set(espos_version "${CMAKE_MATCH_1}")
endif()
string(STRIP "${espos_version}" espos_version)
if(NOT espos_version)
    set(espos_version "0.0.0-unknown")
endif()
# The application's name and version, the way esp_app_format reads them.
idf_build_get_property(project_name PROJECT_NAME)
idf_build_get_property(project_ver PROJECT_VER)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ESPOS_VERSION="${espos_version}"
    ESPOS_PROJECT_NAME="${project_name}"
    ESPOS_PROJECT_VER="${project_ver}")

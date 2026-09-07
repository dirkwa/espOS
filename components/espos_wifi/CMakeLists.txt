# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
idf_build_get_property(target IDF_TARGET)
set(srcs src/wifi_sm.c src/wifi_reason.c src/espos_wifi.c src/api_wifi.c src/mdns.c)
set(reqs espos_config espos_httpd)
set(priv_reqs espressif__cjson espos_event)
if(target STREQUAL "linux")
    # Host: scripted driver stand-in, no esp_wifi/lwIP on the simulator.
    # espressif/mdns does not exist for this target either; mdns.c compiles
    # to stubs (CONFIG_ESPOS_WIFI_MDNS depends on !IDF_TARGET_LINUX).
    list(APPEND srcs src/port_sim.c src/hosted_watchdog.c)
else()
    list(APPEND srcs src/port_idf.c src/portal_dns.c src/hosted_watchdog.c)
    list(APPEND priv_reqs esp_wifi esp_netif esp_event esp_timer lwip)
    # The responder is a public requirement, not a private one: espos_mdns.h
    # is a thin queue in front of it, and the components that use it reach
    # for mdns.h themselves for what the wrapper does not cover (espos_sk
    # browses with mdns_query_ptr). REQUIRES hands them the include path;
    # linking alone (PRIV_REQUIRES) would not.
    list(APPEND reqs espressif__mdns)
    # esp_hosted is only present on co-processor targets (P4 + C6 and
    # friends); the watchdog compiles to stubs without it. Declaring the
    # dependency unconditionally would break every native-radio build, so
    # key off the managed component actually being checked out.
    idf_build_get_property(project_dir PROJECT_DIR)
    if(EXISTS "${project_dir}/managed_components/espressif__esp_hosted/CMakeLists.txt")
        list(APPEND priv_reqs espressif__esp_hosted)
    endif()
endif()
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS include
    REQUIRES ${reqs}
    PRIV_REQUIRES ${priv_reqs})

espos_config_add_descriptor(config/wifi.json)

# TXT records of _espos._tcp (src/mdns.c): the application's name and version
# the way esp_app_format reads them, and espOS's own version — version.txt two
# levels up in the tree, the manifest's version in a copy installed from the
# component registry. The same lookup espos_core makes for its banner,
# repeated here rather than shared: espos_wifi must not depend on espos_core,
# and a cmake/ helper would not travel with a registry copy of this component.
set(_espos_wifi_version_file "${CMAKE_CURRENT_LIST_DIR}/../../version.txt")
if(EXISTS "${_espos_wifi_version_file}")
    file(READ "${_espos_wifi_version_file}" _espos_wifi_espos_version)
    # No CMAKE_CONFIGURE_DEPENDS on the file here: espos_core already adds
    # one, and the Ninja generator writes a phony edge per directory that
    # lists a file, so a second listing fails the generate step with
    # "version.txt is defined as an output multiple times".
else()
    file(READ "${CMAKE_CURRENT_LIST_DIR}/idf_component.yml" _espos_wifi_manifest)
    string(REGEX MATCH "\nversion: \"([^\"]+)\"" _ "${_espos_wifi_manifest}")
    set(_espos_wifi_espos_version "${CMAKE_MATCH_1}")
endif()
string(STRIP "${_espos_wifi_espos_version}" _espos_wifi_espos_version)
if(NOT _espos_wifi_espos_version)
    set(_espos_wifi_espos_version "0.0.0-unknown")
endif()
idf_build_get_property(_espos_wifi_project_name PROJECT_NAME)
idf_build_get_property(_espos_wifi_project_ver PROJECT_VER)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ESPOS_MDNS_ESPOS_VERSION="${_espos_wifi_espos_version}"
    ESPOS_MDNS_PROJECT_NAME="${_espos_wifi_project_name}"
    ESPOS_MDNS_PROJECT_VER="${_espos_wifi_project_ver}")

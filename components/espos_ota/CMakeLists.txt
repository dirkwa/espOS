# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
idf_build_get_property(target IDF_TARGET)
set(srcs src/manifest.c src/espos_ota.c src/api_ota.c)
set(priv_reqs espressif__cjson esp_http_client espos_event)
if(target STREQUAL "linux")
    list(APPEND srcs src/port_sim.c)
else()
    list(APPEND srcs src/port_idf.c)
    list(APPEND priv_reqs esp_https_ota app_update esp_app_format esp_timer mbedtls)
endif()
# "Is the network up" comes from espos_net, so OTA works over any transport.
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS include
    REQUIRES espos_config espos_httpd espos_net
    PRIV_REQUIRES ${priv_reqs})

# The one WiFi-specific question left in the confirm policy — no station
# configured at all, so confirm a new image after a grace period rather than
# roll back — is asked only when espos_wifi is in the build. Optional, so a
# firmware without WiFi (Ethernet, the H-series) links OTA unchanged.
idf_component_optional_requires(PRIVATE espos_wifi)
idf_build_get_property(comps BUILD_COMPONENTS)
if(espos_wifi IN_LIST comps)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ESPOS_OTA_HAVE_WIFI=1)
endif()

espos_config_add_descriptor(config/ota.json)

# SPDX-License-Identifier: Apache-2.0
idf_build_get_property(target IDF_TARGET)
set(srcs src/manifest.c src/espos_ota.c src/api_ota.c)
set(priv_reqs espressif__cjson esp_http_client)
if(target STREQUAL "linux")
    list(APPEND srcs src/port_sim.c)
else()
    list(APPEND srcs src/port_idf.c)
    list(APPEND priv_reqs esp_https_ota app_update esp_app_format esp_timer mbedtls)
endif()
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS include
    REQUIRES espos_config espos_httpd espos_wifi
    PRIV_REQUIRES ${priv_reqs})

espos_config_add_descriptor(config/ota.json)

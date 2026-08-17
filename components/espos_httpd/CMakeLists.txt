# SPDX-License-Identifier: Apache-2.0
idf_build_get_property(target IDF_TARGET)
set(priv_reqs esp_timer esp_app_format esp_system esp_event)
if(NOT target STREQUAL "linux")
    list(APPEND priv_reqs esp_netif)
endif()
idf_component_register(
    SRCS src/espos_httpd.c src/api_config.c src/api_system.c src/static.c src/sse.c
    INCLUDE_DIRS include
    REQUIRES esp_http_server espos_config
    PRIV_REQUIRES ${priv_reqs}
    EMBED_TXTFILES www/index.html)

espos_config_add_descriptor(config/httpd.json)

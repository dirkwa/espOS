# SPDX-License-Identifier: Apache-2.0
idf_build_get_property(target IDF_TARGET)
set(srcs src/token_sm.c src/sk_store.c src/sk_http.c src/espos_sk.c src/api_sk.c)
set(priv_reqs espressif__cjson esp_http_client nvs_flash)
if(target STREQUAL "linux")
    list(APPEND srcs src/discovery_sim.c)
else()
    list(APPEND srcs src/discovery.c)
    list(APPEND priv_reqs espressif__mdns esp_netif lwip)
endif()
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS include
    REQUIRES espos_config espos_httpd espos_wifi
    PRIV_REQUIRES ${priv_reqs})

espos_config_add_descriptor(config/sk.json)

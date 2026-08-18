# SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
idf_build_get_property(target IDF_TARGET)
set(srcs src/wifi_sm.c src/wifi_reason.c src/espos_wifi.c src/api_wifi.c)
set(priv_reqs espressif__cjson)
if(target STREQUAL "linux")
    # Host: scripted driver stand-in, no esp_wifi/lwIP on the simulator.
    list(APPEND srcs src/port_sim.c)
else()
    list(APPEND srcs src/port_idf.c src/portal_dns.c)
    list(APPEND priv_reqs esp_wifi esp_netif esp_event lwip)
endif()
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS include
    REQUIRES espos_config espos_httpd
    PRIV_REQUIRES ${priv_reqs})

espos_config_add_descriptor(config/wifi.json)

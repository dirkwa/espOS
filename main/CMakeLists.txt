# SPDX-License-Identifier: Apache-2.0
set(srcs main.c)
# The M1 network bootstrap needs native WiFi. Chips without it (ESP32-P4)
# stay offline until espos_wifi (M2) brings hosted-WiFi support. Only the
# source list may depend on Kconfig — requirements are resolved before it.
if(CONFIG_SOC_WIFI_SUPPORTED)
    list(APPEND srcs net_bootstrap.c)
endif()
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES espos_config espos_httpd esp_wifi esp_netif esp_event nvs_flash)

espos_config_add_descriptor(config/app.json)

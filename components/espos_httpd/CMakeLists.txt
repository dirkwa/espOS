# SPDX-License-Identifier: Apache-2.0
idf_component_register(
    SRCS src/espos_httpd.c src/api_config.c src/api_system.c src/static.c
    INCLUDE_DIRS include
    REQUIRES esp_http_server espos_config
    PRIV_REQUIRES esp_timer esp_app_format esp_system
    EMBED_TXTFILES www/index.html)

espos_config_add_descriptor(config/httpd.json)

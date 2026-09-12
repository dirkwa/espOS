# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
    SRCS "src/espos_prov.c" "src/api_prov.c"
    INCLUDE_DIRS "include"
    REQUIRES espos_config
    PRIV_REQUIRES protocomm esp_wifi
)

# Optional at link time: provisioning works without either, but when they are
# present it serves GET /api/v1/prov and suspends the BLE scanner for the
# duration. The sources test __has_include, so a build without them compiles
# the same file with those paths removed.
idf_component_optional_requires(PRIVATE espos_httpd espos_ble)

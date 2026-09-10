# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# NMEA 2000 over TWAI: receiver, transmitter, and a candump-format TCP
# server so the bus is reachable from a laptop (`candump`, canboatjs, a
# SignalK n2k-ip-gateway source). Board-agnostic — the application picks
# the TX/RX pins and bitrate; nothing here assumes a particular panel.
#
# Built on IDF 6's esp_twai node API. The peripheral is touched only by
# src/twai_node.cpp, which owns the node handle the receiver and the
# transmitter share; everything else — including the candump codec, which
# is why this component now has host tests — is driver-free.
idf_component_register(
    SRCS src/twai_node.cpp src/twai_receiver.cpp src/twai_transmitter.cpp
         src/api_n2k.cpp
         src/candump_tcp_server.cpp src/candump_format.cpp
    INCLUDE_DIRS include
    REQUIRES esp_driver_twai esp_timer
    PRIV_REQUIRES lwip espressif__mdns esp_driver_gpio)

# GET /api/v1/n2k is worth having but not worth requiring: a headless N2K
# gateway with no web server is a legitimate firmware, and api_n2k.cpp guards
# itself with __has_include so it compiles to a stub there. The optional
# dependency is what puts espos_httpd's headers on the include path when it
# IS in the build -- without it __has_include answers no even though the
# component is present.
idf_component_optional_requires(PRIVATE espos_httpd espressif__cjson)

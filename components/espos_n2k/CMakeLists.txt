# SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
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
         src/candump_tcp_server.cpp src/candump_format.cpp
    INCLUDE_DIRS include
    REQUIRES esp_driver_twai esp_timer
    PRIV_REQUIRES lwip espressif__mdns esp_driver_gpio)

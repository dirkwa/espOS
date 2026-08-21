# SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
#
# NMEA 2000 over TWAI: receiver, transmitter, and a candump-format TCP
# server so the bus is reachable from a laptop (`candump`, canboatjs, a
# SignalK n2k-ip-gateway source). Board-agnostic — the application picks
# the TX/RX pins and bitrate; nothing here assumes a particular panel.
idf_component_register(
    SRCS src/twai_receiver.cpp src/twai_transmitter.cpp
         src/candump_tcp_server.cpp src/candump_format.cpp
    INCLUDE_DIRS include
    REQUIRES driver esp_timer
    PRIV_REQUIRES lwip espressif__mdns esp_driver_gpio)

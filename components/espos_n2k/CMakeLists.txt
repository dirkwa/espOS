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

# IDF 6 deprecated driver/twai.h in favour of esp_twai.h, and espOS builds
# with -Werror, so the deprecation #warning fails the build. Silence it for
# THIS component's own sources rather than making every consumer set
# CONFIG_TWAI_SUPPRESS_DEPRECATE_WARN: a component that only compiles when
# its user knows to add a sdkconfig flag is not reusable, which is the whole
# point of moving this here.
#
# Migrating to the esp_twai API is the real fix and is worth doing on its
# own; it changes handle lifetime and the receive callback signature, so it
# does not belong in a move that is otherwise a rename.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-cpp)

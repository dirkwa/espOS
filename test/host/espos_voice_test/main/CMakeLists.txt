# SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
#
# Compile the protocol sources directly rather than depending on espos_voice:
# the component needs esp-sr and an AudioDriver, and registers as an empty
# stub on every target except esp32s3/esp32p4 — including linux. The framing
# and event code is pure C++ and has no such needs.
set(voice_src "${CMAKE_CURRENT_LIST_DIR}/../../../../components/espos_voice")
idf_component_register(
    SRCS test_main.c test_framing.cpp test_events.cpp
         "${voice_src}/src/protocol/framing.cpp"
         "${voice_src}/src/protocol/events.cpp"
    INCLUDE_DIRS "." "${voice_src}/include"
    PRIV_REQUIRES unity log espressif__cjson
    WHOLE_ARCHIVE)

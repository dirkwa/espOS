# SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
#
# Wyoming-protocol voice satellite: a TCP server that lets a
# signalk-wyoming / Home Assistant orchestrator play TTS to, and stream
# mic audio from, whatever espos_audio::AudioDriver the application
# supplies, with on-device wake word via esp-sr WakeNet.
#
# esp-sr is a hard requirement where it exists: WyomingSatellite owns a
# WakeEngine directly. Making it optional would mean an abstract wake
# interface and a null implementation — worth doing if a board ever wants
# the satellite without on-device wake, but not before there is one.
#
# esp-sr only supports esp32s3 and esp32p4 (see idf_component.yml), so on
# every other target this component registers as an empty stub rather than
# failing the build. Applications on those targets simply have no voice.
idf_build_get_property(target IDF_TARGET)
if(target STREQUAL "esp32s3" OR target STREQUAL "esp32p4")
    idf_component_register(
        SRCS src/wyoming_satellite.cpp src/wake_engine.cpp
             src/protocol/events.cpp src/protocol/framing.cpp
        INCLUDE_DIRS include
        REQUIRES espos_audio
        # espos_health for conditions: a wake service that goes away otherwise
        # leaves a panel that silently stops waking, which is indistinguishable
        # from a dead mic. Reporting that is core behaviour, not something each
        # application should re-derive from a callback -- but it is no reason
        # to depend on SignalK, which is what espos_sk here used to mean. What
        # becomes of a condition is the sink's business: espos_sk publishes a
        # notification when a firmware has it, and a firmware without it can
        # still light an LED.
        PRIV_REQUIRES lwip espressif__cjson espressif__esp-sr espos_health)
else()
    idf_component_register()
endif()

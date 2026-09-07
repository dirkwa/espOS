# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# Core: what the device knows is wrong with it, who gets told, and the
# watchdog policy that acts on it. Depends on nothing but FreeRTOS, the log
# and the system heap/timer, so raising a condition never drags in the
# SignalK stack (espos_sk registers itself as a sink instead). The policy is
# pure C (health_policy.c) behind a port; the host builds port_sim.c.
idf_build_get_property(target IDF_TARGET)
set(srcs src/espos_health.c src/health_policy.c src/health_watchdog.c)
set(priv_reqs log)
if(target STREQUAL "linux")
    list(APPEND srcs src/port_sim.c)
    list(APPEND priv_reqs esp_system)
else()
    list(APPEND srcs src/port_idf.c)
    list(APPEND priv_reqs esp_system esp_timer heap)
endif()
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS include
    PRIV_REQUIRES ${priv_reqs})

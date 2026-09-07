# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# espos_time: the device's wall clock. Core, because everything that wants to
# say WHEN something happened reads it — the delta engine's timestamps, the
# log's prefix, a reset record's `at`.
#
# It depends on espos_httpd (the /time endpoints) and espos_config, and NOT on
# espos_sk: the SignalK fallback is wired from the other side, because
# espos_sk already depends on this component and the reverse would close a
# cycle. What espos_time offers instead is espos_time_set(), which any source
# can call.
#
# The ranking, staleness and ISO 8601 rules are pure C (time_policy.c) behind
# a port; the host builds port_sim.c, which has no SNTP client and no memory
# that survives a process.
idf_build_get_property(target IDF_TARGET)
set(srcs src/time_policy.c src/espos_time.c src/api_time.c src/log_wallclock.c)
set(reqs espos_config espos_httpd)
set(priv_reqs espos_event espressif__cjson)
if(target STREQUAL "linux")
    list(APPEND srcs src/port_sim.c)
else()
    list(APPEND srcs src/port_idf.c)
    # esp_netif carries esp_netif_sntp.h and the client itself; lwip supplies
    # the SNTP implementation behind it; esp_hw_support has the deep-sleep and
    # RTC-memory attributes the carry needs.
    list(APPEND priv_reqs esp_netif lwip esp_timer esp_hw_support esp_system)
endif()
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS include
    REQUIRES ${reqs}
    PRIV_REQUIRES ${priv_reqs})

espos_config_add_descriptor(config/time.json)

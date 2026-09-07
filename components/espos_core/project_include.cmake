# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# Consumer sdkconfig lint. Included by the IDF build system for every project
# that has espos_core in its component list, after sdkconfig has been
# generated and before any component CMakeLists.txt runs; the CONFIG_* values
# are plain CMake variables here (build.cmake includes sdkconfig.cmake first,
# the same way IDF's own partition_table/project_include.cmake reads
# CONFIG_PARTITION_TABLE_CUSTOM).
#
# A firmware that includes cmake/espos_project.cmake inherits every value
# checked below from sdkconfig.d/espos.defaults and never trips this. It is
# for the project that installs espOS's components from the registry and
# writes its own CMakeLists.txt: without the shared defaults its sdkconfig is
# IDF's, and the four settings below are the ones whose absence does not show
# at build time but as a device that wedges or overflows a stack in the
# field. Each failure names the exact line to add.
#
# Every check skips when its value is not in this sdkconfig at all (another
# target, a component not in the build) rather than guess.

if(NOT DEFINED CONFIG_IDF_TARGET OR CONFIG_IDF_TARGET STREQUAL "linux")
    # Host tests run on pthread stacks and have no radio; nothing here applies.
    return()
endif()

set(_espos_lint_fix
    "Add the line to your sdkconfig.defaults and delete the stale sdkconfig so the "
    "defaults are re-applied — or include espOS's cmake/espos_project.cmake, whose "
    "espos_project_prologue() puts sdkconfig.d/espos.defaults on SDKCONFIG_DEFAULTS "
    "for you.")
string(CONCAT _espos_lint_fix ${_espos_lint_fix})

# WiFi/IP events and FreeRTOS timers run the state machine, JSON building and
# SSE sends; the IDF defaults (2304 / 2048) are too tight (espos.defaults).
foreach(_espos_stack CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH)
    if(DEFINED ${_espos_stack} AND ${_espos_stack} LESS 4096)
        message(FATAL_ERROR
            "espos_core: ${_espos_stack}=${${_espos_stack}} but espOS runs its WiFi/SignalK "
            "state machines, JSON building and SSE sends on that task and needs at least 4096 "
            "(a smaller stack overflows on the first busy event, not at boot).\n"
            "    ${_espos_stack}=4096\n"
            "${_espos_lint_fix}")
    endif()
endforeach()

if(CONFIG_IDF_TARGET STREQUAL "esp32p4")
    # From espos.defaults.esp32p4: "Keep the hosted transport mempool — the
    # transport's large DMA buffer pool — out of internal RAM [...]
    # CONSTRAINT: only safe with 64-byte L2 cache lines — the 1600-byte
    # transport stride is 64-aligned but NOT 128-aligned, so with
    # CONFIG_CACHE_L2_CACHE_LINE_128B the SDIO driver rejects PSRAM buffers
    # (ESP_ERR_INVALID_ARG; esp-hosted-mcu#219)."
    if(CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM AND DEFINED CONFIG_CACHE_L2_CACHE_LINE_64B
       AND NOT CONFIG_CACHE_L2_CACHE_LINE_64B)
        message(FATAL_ERROR
            "espos_core: CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y without "
            "CONFIG_CACHE_L2_CACHE_LINE_64B=y. The hosted transport's 1600-byte buffer stride is "
            "64-aligned but not 128-aligned, so with 128-byte L2 cache lines the SDIO driver "
            "rejects the PSRAM buffers (ESP_ERR_INVALID_ARG, esp-hosted-mcu#219) and the "
            "co-processor link wedges. A 256 KB L2 cache forces 128-byte lines; use 128 KB.\n"
            "    CONFIG_CACHE_L2_CACHE_LINE_64B=y\n"
            "${_espos_lint_fix}")
    endif()

    # From espos.defaults.esp32p4: "IDF defaults this to 6, but silently
    # raises it to 16 as soon as a project enables PSRAM
    # (SPIRAM_TRY_ALLOCATE_WIFI_LWIP) [...]. A window of 16 overruns the
    # SDIO Rx path on this transport: it wedges under sustained inbound TCP
    # [...] (espressif/esp-hosted-mcu#184). Measured on a Waveshare 7B with
    # repeated ~30 KB HTTP reads: at 16 the link wedged after 85 requests /
    # 77 s; at 6 it survived 400 consecutive requests over 283 s."
    if(CONFIG_SPIRAM AND DEFINED CONFIG_WIFI_RMT_RX_BA_WIN AND NOT CONFIG_WIFI_RMT_RX_BA_WIN EQUAL 6)
        message(FATAL_ERROR
            "espos_core: CONFIG_SPIRAM=y with CONFIG_WIFI_RMT_RX_BA_WIN=${CONFIG_WIFI_RMT_RX_BA_WIN}. "
            "Enabling PSRAM makes IDF raise the remote radio's receive block-ack window to 16, "
            "which overruns the SDIO Rx path to the C6 under sustained inbound TCP: the link "
            "wedges (\"H_SDIO_DRV: task still writing Rx data to queue!\") and WiFi stays dead "
            "until reboot (esp-hosted-mcu#184). Measured: 16 wedged after 85 requests, 6 survived "
            "400. The knob is WIFI_RMT_*, not ESP_WIFI_RX_BA_WIN — the radio is remote.\n"
            "    CONFIG_WIFI_RMT_RX_BA_WIN=6\n"
            "${_espos_lint_fix}")
    endif()
endif()

unset(_espos_lint_fix)
unset(_espos_stack)

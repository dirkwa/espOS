# SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
#
# Core: what the device knows is wrong with it, and who gets told. Depends on
# nothing but FreeRTOS and the log, so raising a condition never drags in the
# SignalK stack (espos_sk registers itself as a sink instead).
idf_component_register(
    SRCS src/espos_health.c
    INCLUDE_DIRS include
    PRIV_REQUIRES log)

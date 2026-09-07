# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# Naming a component here is what puts it in the build (projects on espOS
# build with IDF's MINIMAL_BUILD), and espos_start() starts whatever is
# built. espos_core is the boot; espos_sk is SignalK, what this example
# publishes to; espos_ota is over-the-air updates from the web UI. Drop
# espos_ota and the device simply has no OTA — nothing else changes.
idf_component_register(SRCS main.c PRIV_REQUIRES espos_core espos_sk espos_ota)

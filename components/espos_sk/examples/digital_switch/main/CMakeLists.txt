# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# No settings of its own, so no descriptor is registered; the pins are
# compile-time constants in main.c.
idf_component_register(
    SRCS main.c
    PRIV_REQUIRES espos_core espos_sk esp_driver_gpio esp_timer)

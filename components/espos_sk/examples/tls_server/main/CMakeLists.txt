# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
idf_component_register(
    SRCS main.c
    PRIV_REQUIRES espos_core espos_config espos_sk espos_wifi)

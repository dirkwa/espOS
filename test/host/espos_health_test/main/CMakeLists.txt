# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# The real component, not a copy of its sources: espos_health depends on
# nothing that the linux target lacks, so what runs here is what ships.
idf_component_register(
    SRCS test_main.c test_health.c test_policy.c
    INCLUDE_DIRS "."
    PRIV_REQUIRES espos_health unity
    WHOLE_ARCHIVE)

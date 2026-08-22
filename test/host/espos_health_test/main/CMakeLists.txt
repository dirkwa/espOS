# SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
#
# The real component, not a copy of its sources: espos_health depends on
# nothing that the linux target lacks, so what runs here is what ships.
idf_component_register(
    SRCS test_main.c test_health.c
    INCLUDE_DIRS "."
    PRIV_REQUIRES espos_health unity
    WHOLE_ARCHIVE)

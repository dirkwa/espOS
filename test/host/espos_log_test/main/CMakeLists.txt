# SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
idf_component_register(
    SRCS test_main.c test_ring.c
    INCLUDE_DIRS "."
    PRIV_REQUIRES unity espos_log
    WHOLE_ARCHIVE)

# SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
#
# Compile the codec directly rather than depending on espos_n2k: the
# component needs the TWAI peripheral and lwIP, which the linux target has
# not got. The codec itself needs neither — that is the point of it no
# longer naming the driver's frame type.
set(n2k_src "${CMAKE_CURRENT_LIST_DIR}/../../../../components/espos_n2k")
idf_component_register(
    SRCS test_main.c test_candump.cpp "${n2k_src}/src/candump_format.cpp"
    INCLUDE_DIRS "." "${n2k_src}/include"
    PRIV_REQUIRES unity
    WHOLE_ARCHIVE)

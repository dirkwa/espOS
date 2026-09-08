# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# espos_sensor_math.c is compiled directly rather than linked through the
# component: the rest of espos_sensors is IDF drivers (esp_adc, pulse_cnt,
# ledc, i2c) that do not exist on the linux target, and the arithmetic is
# deliberately free of all of them.
set(sensors_src "${CMAKE_CURRENT_LIST_DIR}/../../../../components/espos_sensors")
idf_component_register(
    SRCS test_main.c test_sensor_math.c "${sensors_src}/src/espos_sensor_math.c"
    INCLUDE_DIRS "." "${sensors_src}/include"
    PRIV_REQUIRES unity
    WHOLE_ARCHIVE)

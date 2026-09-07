# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# espos_health for the sink and the condition; esp_driver_gpio and esp_timer
# for the LED. espos_sk is named so the condition also reaches the server as
# notifications.espos.<label>.sensorBus (espos_sk is a health sink like the
# LED is); espos_ota so the device takes updates over the air.
idf_component_register(SRCS main.c PRIV_REQUIRES espos_core espos_health espos_sk espos_ota esp_driver_gpio esp_timer)

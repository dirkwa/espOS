# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# espos_sensors: the hardware side of the graph -- C driver helpers over the
# IDF 6 drivers, and the espos::flow nodes built on them (docs/sensors.md).
#
# Split the same way espos_flow is. src/ is C and usable on its own by a
# firmware that wants "an ADC reading in volts" without the graph; include/
# espos_sensors/*.hpp is header-only C++ that turns each of those into a node.
#
# The 1-Wire half is optional and OFF by default: it pulls two components
# from the registry (espressif/ds18b20 and its onewire_bus dependency), and a
# firmware with no DS18B20 on it should not download them or link the RMT
# driver. CONFIG_ESPOS_SENSORS_ONEWIRE turns it on.
idf_build_get_property(target IDF_TARGET)

set(srcs src/espos_sensor_math.c src/espos_adc.c src/espos_gpio_in.c src/espos_pcnt.c src/espos_pwm.c src/espos_i2c_bus.c)
set(reqs esp_adc esp_driver_gpio esp_driver_ledc esp_driver_i2c esp_timer esp_hw_support)

# The pulse counter uses the PCNT peripheral where the SoC has one and falls
# back to counting edges in a GPIO ISR where it does not (the ESP32-C3 has no
# PCNT unit). esp_driver_pcnt is named even there: the component exists on
# every target and compiles to nothing on a chip without the peripheral,
# while an `if(CONFIG_SOC_PCNT_SUPPORTED)` around it would be empty on the
# FIRST CMake pass, before sdkconfig exists -- the same trap the
# pulse_counter example documents. espos_pcnt.c chooses with
# SOC_PCNT_SUPPORTED, which is a soc_caps.h macro and always correct.
list(APPEND reqs esp_driver_pcnt)

# Same first-pass caveat, and the reason the 1-Wire source is added by a
# generator-safe check instead: CONFIG_ is readable here only on the second
# pass, so the file is always compiled and its BODY is guarded by the option.
list(APPEND srcs src/espos_onewire.c)

# REQUIRES, not PRIV_REQUIRES, for the four that PUBLIC HEADERS include:
# espos_flow (the node types), esp_timer and esp_system (system.hpp reads the
# clock and the reset reason) and heap (heap_caps_get_free_size). A public
# header whose dependency is private compiles here and fails in the firmware
# that includes it, which is the worst place to find out.
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS include
    REQUIRES espos_flow esp_timer esp_system heap
    PRIV_REQUIRES ${reqs})

# The 1-Wire driver is not in idf_component.yml: see the note there. A
# firmware that turns CONFIG_ESPOS_SENSORS_ONEWIRE on adds espressif/ds18b20
# to its OWN manifest, and this makes its headers visible here when it does.
#
# idf_component_optional_requires is what "use it if the firmware pulled it
# in" looks like: naming espressif__ds18b20 in PRIV_REQUIRES would make it a
# hard dependency of every firmware, which is the download the option exists
# to avoid. espos_onewire.c is empty without the option, so a firmware that
# has neither compiles fine.
idf_component_optional_requires(PRIVATE espressif__ds18b20 espressif__onewire_bus)

/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_onewire — a 1-Wire bus and the DS18B20s on it.
 *
 * The DS18B20 is the temperature sensor on boats: a few euro, waterproof
 * versions on a lead, and a dozen of them share three wires. Engine room,
 * fridge, freezer, cabin, water tank -- one bus.
 *
 * Wrapped over the registry components `espressif/ds18b20` and
 * `espressif/onewire_bus`, which IDF's own example uses. The wrapper exists
 * for two reasons that matter on a real boat:
 *
 *   * Enumeration by address, kept stable. Each sensor has a unique 64-bit
 *     ROM id, and that id -- not the order it answered in -- is what a
 *     Signal K path must key on. Sensors answer in address order, but a
 *     sensor that fails to answer one scan shifts every "index" after it,
 *     and the engine-room reading silently becomes the fridge.
 *   * A conversion that does not block the flow task for 750 ms. At 12-bit
 *     resolution the chip needs three quarters of a second to convert, and
 *     the honest way to use that in a graph is trigger-now / read-later.
 *
 * Needs an external 4.7 kOhm pull-up from the data line to 3V3. The internal
 * one cannot supply a DS18B20 and the bus reads as empty without it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct espos_onewire_bus *espos_onewire_bus_handle_t;
typedef struct espos_ds18b20 *espos_ds18b20_handle_t;

/** Open a 1-Wire bus on one GPIO. */
esp_err_t espos_onewire_open(int gpio, espos_onewire_bus_handle_t *out);
void espos_onewire_close(espos_onewire_bus_handle_t bus);

/**
 * Find every DS18B20 on the bus. Fills `addrs` with up to `max` ROM ids and
 * writes how many were found to *out_n. Log these once and put them in the
 * configuration: an address is what a sensor IS, and it is printed as 16 hex
 * digits.
 */
esp_err_t espos_onewire_search(espos_onewire_bus_handle_t bus, uint64_t *addrs, size_t max, size_t *out_n);

/**
 * Open one sensor by address. Address 0 means "the only sensor on the bus"
 * and is a convenience for the single-sensor case; it fails when the bus has
 * more than one, rather than picking one at random.
 */
esp_err_t espos_ds18b20_open(espos_onewire_bus_handle_t bus, uint64_t addr, espos_ds18b20_handle_t *out);
void espos_ds18b20_close(espos_ds18b20_handle_t dev);

/**
 * Resolution, 9..12 bits. Fewer bits convert faster: 9 bits is 0.5 C in
 * ~94 ms, 12 bits is 0.0625 C in ~750 ms. For a cabin or engine temperature
 * 10 or 11 bits is the sensible trade; 12 is the chip default.
 */
esp_err_t espos_ds18b20_set_resolution(espos_ds18b20_handle_t dev, int bits);

/**
 * Start a conversion on every sensor on the bus at once (the 1-Wire SKIP ROM
 * broadcast), then read each one when it is done. This is the pattern to
 * use: converting sensors one at a time costs the conversion time PER
 * sensor, so eight sensors at 12 bits take six seconds instead of one.
 *
 * espos_ds18b20_convert_ms() is how long to wait before reading.
 */
esp_err_t espos_onewire_convert_all(espos_onewire_bus_handle_t bus);
uint32_t espos_ds18b20_convert_ms(int bits);

/**
 * The temperature from the LAST conversion, in kelvin -- the Signal K unit,
 * so nothing downstream has to convert. Does not trigger one: call
 * espos_onewire_convert_all() and read after the conversion time.
 *
 * A sensor that has been unplugged reads 85 C exactly (the chip's power-on
 * value) and that is reported as ESP_ERR_INVALID_RESPONSE rather than a
 * plausible-looking number, because a disconnected engine sensor reading a
 * steady 85 C is the kind of fault that gets believed.
 */
esp_err_t espos_ds18b20_read_kelvin(espos_ds18b20_handle_t dev, float *out_kelvin);

/** The sensor's ROM id. */
uint64_t espos_ds18b20_address(espos_ds18b20_handle_t dev);

#ifdef __cplusplus
}
#endif

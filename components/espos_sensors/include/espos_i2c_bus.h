/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_i2c_bus — one I2C bus and the devices on it (i2c_master).
 *
 * espOS does not ship a driver for each breakout board and should not: there
 * are hundreds, they change, and a BME280 driver in this repository is a
 * BME280 driver nobody maintains. What espOS owns is the bus -- opened once,
 * shared, and safe to use from the flow task -- plus the register reads that
 * every one of those drivers is made of.
 *
 * The documented way to wrap a breakout is `Poll<T>` around a function that
 * calls espos_i2c_read_reg(): see docs/sensors.md. That is a dozen lines for
 * a new sensor and it stays in the firmware that needs it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct espos_i2c_dev *espos_i2c_dev_handle_t;

typedef struct {
    int port;         /* I2C peripheral number; 0 on most chips, and the default */
    int sda_gpio;
    int scl_gpio;
    uint32_t freq_hz; /* 0 = 100000. 400000 works on short, well-pulled-up wiring */
    bool pullups;     /* enable the weak internal pull-ups: for a breakout that has none.
                       * They are far too weak (~45 kOhm) for a long bus -- fit 4.7k
                       * resistors instead and leave this off. */
} espos_i2c_bus_cfg_t;

/**
 * Open a bus. Opening the same port twice returns ESP_ERR_INVALID_STATE:
 * the bus is shared, so the second caller should just add its device.
 */
esp_err_t espos_i2c_bus_open(const espos_i2c_bus_cfg_t *cfg);

/** Add a device on an already-open bus. `addr` is the 7-bit address. */
esp_err_t espos_i2c_dev_add(int port, uint8_t addr, espos_i2c_dev_handle_t *out);
void espos_i2c_dev_remove(espos_i2c_dev_handle_t dev);

/**
 * Register access, the shape almost every I2C sensor speaks: write the
 * register number, then read `len` bytes. Blocking, with a timeout in
 * milliseconds (0 = 100).
 */
esp_err_t espos_i2c_read_reg(espos_i2c_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len, uint32_t timeout_ms);
esp_err_t espos_i2c_write_reg(espos_i2c_dev_handle_t dev, uint8_t reg, const uint8_t *buf, size_t len, uint32_t timeout_ms);

/** Raw transfer, for a device that does not use registers. Either half may be NULL. */
esp_err_t espos_i2c_xfer(espos_i2c_dev_handle_t dev, const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len, uint32_t timeout_ms);

/** Whether a device answers at all: the first thing to check when a sensor reads zeros. */
esp_err_t espos_i2c_probe(int port, uint8_t addr, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

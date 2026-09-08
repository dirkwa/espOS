/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

#include "espos_i2c_bus.h"

static const char *TAG = "espos_i2c";

#define MAX_PORTS          2
#define DEFAULT_TIMEOUT_MS 100

struct espos_i2c_dev {
    i2c_master_dev_handle_t dev;
};

static i2c_master_bus_handle_t s_bus[MAX_PORTS];
/* The bus clock, remembered per port: i2c_master sets the speed per DEVICE,
 * not per bus, so a device added later has to be given the frequency the bus
 * was opened with. Without this a caller asking for 400 kHz silently gets
 * 100 kHz -- the sensor still works, just four times slower, which is the
 * kind of thing nobody notices until a display stutters. */
static uint32_t s_freq[MAX_PORTS];

static uint32_t or_default(uint32_t v, uint32_t d) { return v ? v : d; }

esp_err_t espos_i2c_bus_open(const espos_i2c_bus_cfg_t *cfg)
{
    if (!cfg || cfg->port < 0 || cfg->port >= MAX_PORTS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_bus[cfg->port]) {
        /* Shared on purpose: two sensors on one bus is the normal case, and
         * the second one should add a device rather than reconfigure the
         * pins under the first. */
        return ESP_ERR_INVALID_STATE;
    }
    i2c_master_bus_config_t bcfg = {
        .i2c_port = cfg->port,
        .sda_io_num = cfg->sda_gpio,
        .scl_io_num = cfg->scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = cfg->pullups },
    };
    esp_err_t err = i2c_new_master_bus(&bcfg, &s_bus[cfg->port]);
    if (err != ESP_OK) {
        return err;
    }
    s_freq[cfg->port] = or_default(cfg->freq_hz, 100000);
    ESP_LOGI(TAG, "bus %d on SDA %d SCL %d at %u Hz", cfg->port, cfg->sda_gpio, cfg->scl_gpio,
             (unsigned)s_freq[cfg->port]);
    return ESP_OK;
}

esp_err_t espos_i2c_dev_add(int port, uint8_t addr, espos_i2c_dev_handle_t *out)
{
    if (port < 0 || port >= MAX_PORTS || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_bus[port]) {
        return ESP_ERR_INVALID_STATE;
    }
    struct espos_i2c_dev *d = calloc(1, sizeof(*d));
    if (!d) {
        return ESP_ERR_NO_MEM;
    }
    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = s_freq[port],
    };
    esp_err_t err = i2c_master_bus_add_device(s_bus[port], &dcfg, &d->dev);
    if (err != ESP_OK) {
        free(d);
        return err;
    }
    *out = d;
    return ESP_OK;
}

void espos_i2c_dev_remove(espos_i2c_dev_handle_t dev)
{
    if (!dev) {
        return;
    }
    i2c_master_bus_rm_device(dev->dev);
    free(dev);
}

esp_err_t espos_i2c_xfer(espos_i2c_dev_handle_t dev, const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len, uint32_t timeout_ms)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    int to = (int)or_default(timeout_ms, DEFAULT_TIMEOUT_MS);
    if (tx && tx_len && rx && rx_len) {
        return i2c_master_transmit_receive(dev->dev, tx, tx_len, rx, rx_len, to);
    }
    if (tx && tx_len) {
        return i2c_master_transmit(dev->dev, tx, tx_len, to);
    }
    if (rx && rx_len) {
        return i2c_master_receive(dev->dev, rx, rx_len, to);
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t espos_i2c_read_reg(espos_i2c_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    return espos_i2c_xfer(dev, &reg, 1, buf, len, timeout_ms);
}

esp_err_t espos_i2c_write_reg(espos_i2c_dev_handle_t dev, uint8_t reg, const uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    /* Register byte and payload must go out as ONE transaction: a stop
     * between them leaves the device addressed at a register it will not
     * remember. 16 bytes covers every configuration write; more than that
     * is a bulk transfer and belongs in espos_i2c_xfer. */
    uint8_t tmp[17];
    if (len > sizeof(tmp) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    tmp[0] = reg;
    if (buf && len) {
        memcpy(tmp + 1, buf, len);
    }
    return espos_i2c_xfer(dev, tmp, len + 1, NULL, 0, timeout_ms);
}

esp_err_t espos_i2c_probe(int port, uint8_t addr, uint32_t timeout_ms)
{
    if (port < 0 || port >= MAX_PORTS || !s_bus[port]) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_probe(s_bus[port], addr, (int)or_default(timeout_ms, DEFAULT_TIMEOUT_MS));
}

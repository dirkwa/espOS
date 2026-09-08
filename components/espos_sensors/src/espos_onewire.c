/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 */
/* The whole file is behind CONFIG_ESPOS_SENSORS_ONEWIRE. It is compiled on
 * every build (a CONFIG_ check in CMakeLists is empty on the first pass,
 * before sdkconfig exists) and turns into nothing when the option is off --
 * which also means the espressif/ds18b20 headers are only needed by a
 * firmware that asked for them. */
#include "sdkconfig.h"

#if CONFIG_ESPOS_SENSORS_ONEWIRE

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "ds18b20.h"
#include "onewire_bus.h"
#include "onewire_device.h"

#include "espos_onewire.h"

static const char *TAG = "espos_1wire";

/* The chip's power-on scratchpad value. A sensor whose data line has come
 * adrift answers with this forever, and 85 C is a plausible engine reading. */
#define DS18B20_POWER_ON_C 85.0f
#define KELVIN_OFFSET      273.15f

struct espos_onewire_bus {
    onewire_bus_handle_t bus;
};

struct espos_ds18b20 {
    ds18b20_device_handle_t dev;
    uint64_t addr;
};

esp_err_t espos_onewire_open(int gpio, espos_onewire_bus_handle_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    struct espos_onewire_bus *b = calloc(1, sizeof(*b));
    if (!b) {
        return ESP_ERR_NO_MEM;
    }
    onewire_bus_config_t bcfg = { .bus_gpio_num = gpio };
    /* 10 bytes covers a scratchpad read (9) plus the slack the RMT receiver
     * wants; the driver sizes its symbol buffer from this. */
    onewire_bus_rmt_config_t rcfg = { .max_rx_bytes = 10 };
    esp_err_t err = onewire_new_bus_rmt(&bcfg, &rcfg, &b->bus);
    if (err != ESP_OK) {
        free(b);
        return err;
    }
    ESP_LOGI(TAG, "1-Wire bus on GPIO %d", gpio);
    *out = b;
    return ESP_OK;
}

void espos_onewire_close(espos_onewire_bus_handle_t bus)
{
    if (!bus) {
        return;
    }
    onewire_bus_del(bus->bus);
    free(bus);
}

esp_err_t espos_onewire_search(espos_onewire_bus_handle_t bus, uint64_t *addrs, size_t max, size_t *out_n)
{
    if (!bus || !addrs || !out_n) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_n = 0;
    onewire_device_iter_handle_t iter = NULL;
    esp_err_t err = onewire_new_device_iter(bus->bus, &iter);
    if (err != ESP_OK) {
        return err;
    }
    onewire_device_t dev;
    while (*out_n < max && onewire_device_iter_get_next(iter, &dev) == ESP_OK) {
        addrs[*out_n] = dev.address;
        ESP_LOGI(TAG, "device %016llx", (unsigned long long)dev.address);
        (*out_n)++;
    }
    onewire_del_device_iter(iter);
    return ESP_OK;
}

esp_err_t espos_ds18b20_open(espos_onewire_bus_handle_t bus, uint64_t addr, espos_ds18b20_handle_t *out)
{
    if (!bus || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    if (addr == 0) {
        /* "the only sensor": resolve it, and refuse rather than guess when
         * there is more than one -- picking one at random is how a fridge
         * reading ends up on the engine path. */
        uint64_t found[2];
        size_t n = 0;
        esp_err_t err = espos_onewire_search(bus, found, 2, &n);
        if (err != ESP_OK) {
            return err;
        }
        if (n == 0) {
            return ESP_ERR_NOT_FOUND;
        }
        if (n > 1) {
            ESP_LOGE(TAG, "%u sensors on the bus: name one by address", (unsigned)n);
            return ESP_ERR_INVALID_ARG;
        }
        addr = found[0];
    }

    struct espos_ds18b20 *d = calloc(1, sizeof(*d));
    if (!d) {
        return ESP_ERR_NO_MEM;
    }
    onewire_device_t od = { .bus = bus->bus, .address = addr };
    ds18b20_config_t cfg = {};
    esp_err_t err = ds18b20_new_device_from_enumeration(&od, &cfg, &d->dev);
    if (err != ESP_OK) {
        free(d);
        return err;
    }
    d->addr = addr;
    *out = d;
    return ESP_OK;
}

void espos_ds18b20_close(espos_ds18b20_handle_t dev)
{
    if (!dev) {
        return;
    }
    ds18b20_del_device(dev->dev);
    free(dev);
}

esp_err_t espos_ds18b20_set_resolution(espos_ds18b20_handle_t dev, int bits)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    ds18b20_resolution_t r;
    switch (bits) {
    case 9:
        r = DS18B20_RESOLUTION_9B;
        break;
    case 10:
        r = DS18B20_RESOLUTION_10B;
        break;
    case 11:
        r = DS18B20_RESOLUTION_11B;
        break;
    case 12:
        r = DS18B20_RESOLUTION_12B;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    return ds18b20_set_resolution(dev->dev, r);
}

esp_err_t espos_onewire_convert_all(espos_onewire_bus_handle_t bus)
{
    if (!bus) {
        return ESP_ERR_INVALID_ARG;
    }
    return ds18b20_trigger_temperature_conversion_for_all(bus->bus);
}

uint32_t espos_ds18b20_convert_ms(int bits)
{
    /* Datasheet maxima, rounded up: the chip may finish sooner but reading
     * early gives the previous conversion, which on a moving temperature is
     * a lag nobody sees and nobody can explain. */
    switch (bits) {
    case 9:
        return 100;
    case 10:
        return 190;
    case 11:
        return 380;
    default:
        return 760;
    }
}

esp_err_t espos_ds18b20_read_kelvin(espos_ds18b20_handle_t dev, float *out_kelvin)
{
    if (!dev || !out_kelvin) {
        return ESP_ERR_INVALID_ARG;
    }
    float c = 0.0f;
    esp_err_t err = ds18b20_get_temperature(dev->dev, &c);
    if (err != ESP_OK) {
        return err;
    }
    if (c == DS18B20_POWER_ON_C) {
        /* Exactly 85.0000 is the reset value, not a measurement: a real
         * sensor at 85 C reads 84.9375 or 85.0625 at 12-bit resolution. */
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out_kelvin = c + KELVIN_OFFSET;
    return ESP_OK;
}

uint64_t espos_ds18b20_address(espos_ds18b20_handle_t dev) { return dev ? dev->addr : 0; }

#endif /* CONFIG_ESPOS_SENSORS_ONEWIRE */

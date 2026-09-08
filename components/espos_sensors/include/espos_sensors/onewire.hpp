// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// espos::sensors — DS18B20 temperatures as graph nodes.
//
// Only compiled when CONFIG_ESPOS_SENSORS_ONEWIRE is on, which is also what
// makes espressif/ds18b20 a dependency of the firmware. See docs/sensors.md.
//
// The shape here is trigger-then-read, and it is the interesting part: a
// 12-bit conversion takes 750 ms, so a node that converted and read in one
// call would block the flow task -- and with it every other sensor in the
// firmware -- for three quarters of a second. Instead one timer broadcasts a
// conversion to every sensor on the bus at once, and a second timer, one
// conversion-time later, reads them. Eight sensors then cost ONE conversion
// window rather than eight.
#pragma once

#include "sdkconfig.h"

#if CONFIG_ESPOS_SENSORS_ONEWIRE

#include <cstdint>

#include "espos_flow/flow.hpp"
#include "espos_onewire.h"

namespace espos::sensors {

// One DS18B20, emitting KELVIN -- the Signal K unit, so nothing downstream
// has to convert and no path ever carries celsius by accident.
//
// Owned by a OneWireBus, which does the conversion timing for the whole bus.
class Ds18b20 : public espos::flow::NodeBase,
                public espos::flow::Producer<float> {
 public:
  // `addr` is the sensor's 64-bit ROM id, which is what a sensor IS. Pass 0
  // only when the bus has exactly one sensor: an index would renumber every
  // sensor after one that failed to answer, and the engine-room reading
  // would quietly become the fridge.
  Ds18b20(const char* id, espos_onewire_bus_handle_t bus, uint64_t addr)
      : NodeBase(id) {
    open_err_ = espos_ds18b20_open(bus, addr, &dev_);
  }

  esp_err_t set_resolution(int bits) {
    return dev_ ? espos_ds18b20_set_resolution(dev_, bits) : open_err_;
  }

  // Read what the last conversion produced and emit it. A disconnected
  // sensor reads exactly 85 C (the chip's power-on value) and emits NOTHING
  // rather than a plausible number -- a steady 85 C on an engine path is the
  // kind of fault that gets believed until something melts.
  void read_and_emit() {
    if (!dev_) return;
    float k = 0.0f;
    if (espos_ds18b20_read_kelvin(dev_, &k) != ESP_OK) {
      errors_++;
      return;
    }
    this->emit(k);
  }

  uint64_t address() const { return espos_ds18b20_address(dev_); }
  uint32_t errors() const { return errors_; }
  esp_err_t open_error() const { return open_err_; }

 protected:
  const char* node_id_for_error() const override { return id(); }

 private:
  espos_ds18b20_handle_t dev_ = nullptr;
  esp_err_t open_err_ = ESP_OK;
  uint32_t errors_ = 0;
};

// The bus, and the conversion clock for every sensor on it.
//
// `Max` is the number of sensors this bus can carry; the array is inline, so
// a bus sized for two costs two pointers.
template <std::size_t Max = 8>
class OneWireBus {
 public:
  // GPIO needs an external 4.7k pull-up to 3V3. The internal pull-up cannot
  // supply a DS18B20 and the bus simply reads as empty -- the single most
  // common "my sensors do not appear".
  explicit OneWireBus(int gpio) { open_err_ = espos_onewire_open(gpio, &bus_); }

  espos_onewire_bus_handle_t handle() const { return bus_; }
  esp_err_t open_error() const { return open_err_; }

  // Every sensor found, most useful at boot: log the addresses once and put
  // them in the configuration.
  esp_err_t search(uint64_t* addrs, std::size_t max, std::size_t* out_n) {
    return espos_onewire_search(bus_, addrs, max, out_n);
  }

  // Register a node so the bus converts and reads it.
  esp_err_t add(Ds18b20& sensor) {
    if (n_ >= Max) return ESP_ERR_NO_MEM;
    sensors_[n_++] = &sensor;
    return ESP_OK;
  }

  // Start the cycle: convert every `period_ms`, read `convert_ms` later.
  // period_ms must be longer than the conversion time or the reads never
  // catch up; 2000 ms at 12 bits is comfortable.
  esp_err_t start(uint32_t period_ms, int bits = 12) {
    if (open_err_ != ESP_OK) return open_err_;
    convert_ms_ = espos_ds18b20_convert_ms(bits);
    if (period_ms <= convert_ms_) return ESP_ERR_INVALID_ARG;
    for (std::size_t i = 0; i < n_; i++) sensors_[i]->set_resolution(bits);
    return espos_flow_every(period_ms, &OneWireBus::convert_tick, this,
                            &timer_);
  }

 private:
  // Broadcast a conversion to the whole bus, then arm a one-shot to read.
  // The read cannot happen here: it is 750 ms away, and the flow task has
  // other sensors to serve in the meantime.
  static void convert_tick(void* self) {
    OneWireBus* b = static_cast<OneWireBus*>(self);
    if (espos_onewire_convert_all(b->bus_) != ESP_OK) return;
    espos_flow_timer_t t;
    espos_flow_after(b->convert_ms_, &OneWireBus::read_tick, b, &t);
  }

  static void read_tick(void* self) {
    OneWireBus* b = static_cast<OneWireBus*>(self);
    for (std::size_t i = 0; i < b->n_; i++) b->sensors_[i]->read_and_emit();
  }

  espos_onewire_bus_handle_t bus_ = nullptr;
  esp_err_t open_err_ = ESP_OK;
  Ds18b20* sensors_[Max] = {};
  std::size_t n_ = 0;
  uint32_t convert_ms_ = 760;
  espos_flow_timer_t timer_ = ESPOS_FLOW_TIMER_NONE;
};

}  // namespace espos::sensors

#endif  // CONFIG_ESPOS_SENSORS_ONEWIRE

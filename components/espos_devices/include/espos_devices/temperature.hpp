// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// OneWireTemperature — a DS18B20 on a Signal K temperature path.
//
//   espos::sensors::OneWireBus bus(4);
//   espos::devices::OneWireTemperature engine(g, bus, {
//       .id = "engrm", .addr = 0x28FF1234567890AB,
//       .path = "environment.inside.engineRoom.temperature",
//   });
//   bus.start(5000);            // one cycle drives every sensor on the wire
//
// The bus is the caller's, not the device's, because one wire carries many
// sensors and each conversion is a bus-wide operation: a device that owned
// its own bus would give you one GPIO per thermometer.
//
// **Pass the ROM address, not an index.** A 1-Wire search returns sensors in
// whatever order they answer, so an index renumbers every sensor after one
// that fails to reply -- and the engine-room reading quietly becomes the
// fridge. The address is what a sensor IS; find it once with
// OneWireBus::search() and write it into the firmware.
#pragma once

#include <cstddef>

#include "espos_devices/detail.hpp"
#include "espos_flow/graph.hpp"
#include "espos_sensors/onewire.hpp"
#include "espos_sk_flow/sk.hpp"

// 1-Wire is off by default (CONFIG_ESPOS_SENSORS_ONEWIRE), and the driver
// types do not exist without it -- so neither does this device. The guard is
// here rather than left to the caller because espos_devices.hpp includes
// every device: without it, a firmware that wants a tank sender would fail to
// compile over a thermometer it never asked for.
#if CONFIG_ESPOS_SENSORS_ONEWIRE

namespace espos::devices
{

struct OneWireTemperatureConfig {
    const char *id = "temp";
    // The sensor's 64-bit ROM id. 0 is legal only on a bus with exactly one
    // sensor; see the note above about why it is a bad habit on any other.
    uint64_t addr = 0;
    // A spec path, so no metadata is sent. Signal K temperatures are KELVIN,
    // which is what the driver emits -- no conversion here, deliberately.
    const char *path = "environment.inside.temperature";
};

// Templated on the bus's capacity because OneWireBus is: `Max` is how many
// sensors that wire can carry, and the array is inline. A caller writes
// OneWireTemperature(g, bus, {...}) and the deduction guide below picks it up
// -- the parameter never appears in application code.
template <std::size_t Max>
class OneWireTemperature
{
  public:
    OneWireTemperature(espos::flow::Graph &g,
                       espos::sensors::OneWireBus<Max> &bus,
                       const OneWireTemperatureConfig &cfg)
        : sensor_(g.make<espos::sensors::Ds18b20>(cfg.id, bus.handle(),
                                                  cfg.addr)),
          out_(g.make<espos::sk::Output<float>>(cfg.path))
    {
        sensor_ >> out_;
        // Registering with the bus is what makes it converted and read; a sensor
        // that is only wired into the graph produces nothing, silently.
        bus.add(sensor_);
    }

    // The reading, for anything else that should follow it -- a Hysteresis into
    // an over-temperature notification is the usual second consumer.
    espos::flow::Producer<float> &kelvin() { return sensor_; }

    espos::sensors::Ds18b20 &sensor() { return sensor_; }
    espos::sk::Output<float> &output() { return out_; }

    esp_err_t open_error() const { return sensor_.open_error(); }

  private:
    espos::sensors::Ds18b20 &sensor_;
    espos::sk::Output<float> &out_;
};

// So application code says OneWireTemperature(g, bus, {...}) and never names
// the bus's capacity twice.
template <std::size_t Max>
OneWireTemperature(espos::flow::Graph &, espos::sensors::OneWireBus<Max> &,
                   const OneWireTemperatureConfig &)
    -> OneWireTemperature<Max>;

}  // namespace espos::devices

#endif  // CONFIG_ESPOS_SENSORS_ONEWIRE

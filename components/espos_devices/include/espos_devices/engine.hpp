// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// EngineRpm and EngineHours — the two things every engine installation
// publishes, and the two that are easiest to get subtly wrong.
//
//   espos::devices::EngineRpm rpm(g, {.id = "rpm", .gpio = 21,
//                                     .pulses_per_rev = 2.0f});
//   espos::devices::EngineHours hours(g, {.id = "hrs"});
//   rpm.running() >> hours.input();   // hours accrue while the engine turns
//   rpm.start();
//
// **Signal K wants revolutions per SECOND on propulsion.*.revolutions**, not
// RPM. The path is defined in SI units like every other numeric path in the
// spec, and publishing 2400 where the server expects 40 is the single most
// common mistake on an engine gateway -- it looks right on a gauge that
// happens to scale it and wrong everywhere else. That conversion is done
// here, once, rather than left to a magic constant in each firmware.
#pragma once

#include "espos_devices/detail.hpp"
#include "espos_flow/graph.hpp"
#include "espos_flow/transforms.hpp"
#include "espos_sensors/sensors.hpp"
#include "espos_sk_flow/sk.hpp"

namespace espos::devices
{

struct EngineRpmConfig {
    const char *id = "rpm";
    int gpio = -1;
    const char *path = "propulsion.main.revolutions";

    // Pulses per revolution of the crankshaft. An alternator W terminal gives
    // pole-pairs times the pulley ratio and is rarely a round number, which is
    // why this is a float and belongs on a config page: it is calibrated
    // against a hand tachometer, not read off a datasheet.
    float pulses_per_rev = 1.0f;

    // The engine is considered running above this, in revolutions per second.
    // 5 rev/s = 300 rpm, comfortably above a starter and below any idle.
    float running_above_hz = 5.0f;

    uint32_t period_ms = 1000;
    uint32_t max_glitch_ns = 1000;  // ignition noise is the usual problem
};

// Pulse rate on a pin -> revolutions per second on a Signal K path.
class EngineRpm
{
  public:
    EngineRpm(espos::flow::Graph &g, const EngineRpmConfig &cfg)
        : pulses_(g.make<espos::sensors::PulseCounter>(
              cfg.id, cfg.gpio, cfg.period_ms, cfg.max_glitch_ns)),
          // Hz on the pin / pulses per rev = revolutions per second, which is
          // what the spec asks for. A multiplier, not a divisor, so the config
          // page shows one number rather than a reciprocal nobody can sanity
          // check.
          scale_(g.make<espos::flow::Linear<float>>(
              detail::SubId(cfg.id, 's'),
              cfg.pulses_per_rev > 0.0f ? 1.0f / cfg.pulses_per_rev : 1.0f,
              0.0f)),
          out_(g.make<espos::sk::Output<float>>(cfg.path)),
          // Above the threshold the engine is turning. Hysteresis rather than a
          // comparison: an engine hovering at the idle boundary would otherwise
          // start and stop the hour meter every second.
          running_(g.make<espos::flow::Hysteresis<float, bool>>(
              detail::SubId(cfg.id, 'r'), cfg.running_above_hz * 0.8f,
              cfg.running_above_hz, false, true))
    {
        pulses_ >> scale_ >> out_;
        scale_ >> running_;
    }

    esp_err_t start() { return pulses_.start(); }
    void stop() { pulses_.stop(); }

    // Calibration belongs on a config page: pulses per revolution is measured
    // against a hand tachometer, and getting it wrong is invisible until
    // somebody compares two instruments.
    esp_err_t register_config(const char *title = "Engine RPM")
    {
        return scale_.register_config(title);
    }

    // True while the engine turns. Wire it into EngineHours, a Notify, or
    // anything else that cares.
    espos::flow::Producer<bool> &running() { return running_; }

    espos::sensors::PulseCounter &pulses() { return pulses_; }
    espos::flow::Linear<float> &scale() { return scale_; }
    espos::sk::Output<float> &output() { return out_; }

  private:
    espos::sensors::PulseCounter &pulses_;
    espos::flow::Linear<float> &scale_;
    espos::sk::Output<float> &out_;
    espos::flow::Hysteresis<float, bool> &running_;
};

struct EngineHoursConfig {
    const char *id = "hrs";
    // Signal K's runTime is SECONDS, which is what RunHours already emits --
    // no conversion, deliberately. An hour meter reads hours, and that
    // temptation to multiply is exactly how a runTime ends up 3600x wrong and
    // still plausible on a gauge that scales it.
    const char *path = "propulsion.main.runTime";
    uint32_t emit_interval_ms = 60000;
};

// Accumulated running time on a Signal K path.
//
// **The total does NOT survive a reboot on its own.** RunHours has no config
// registration: it counts from zero at every start. A firmware that wants a
// real hour meter has to persist total_s() itself and restore it with
// set_total_s() at boot -- there is no way to do that here without inventing
// a storage policy that belongs to the application. Said plainly because an
// hour meter that silently resets is worse than none: it reads plausibly.
class EngineHours
{
  public:
    EngineHours(espos::flow::Graph &g, const EngineHoursConfig &cfg)
        : hours_(g.make<espos::flow::RunHours>(cfg.id, cfg.emit_interval_ms)),
          out_(g.make<espos::sk::Output<float>>(cfg.path))
    {
        hours_ >> out_;
    }

    // Feed this the engine's running state -- EngineRpm::running(), an oil
    // pressure switch, an ignition sense line.
    espos::flow::Consumer<bool> &input() { return hours_; }

    // Restore a total at boot, and read it back to persist. The pair is what a
    // firmware needs to make the meter survive a power cycle.
    void set_total_s(float seconds) { hours_.set_total_s(seconds); }
    float total_s() const { return hours_.total_s(); }

    espos::flow::RunHours &hours() { return hours_; }
    espos::sk::Output<float> &output() { return out_; }

  private:
    espos::flow::RunHours &hours_;
    espos::sk::Output<float> &out_;
};

}  // namespace espos::devices

// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// TankLevel — a resistive tank sender on a Signal K path.
//
// This is the shape SensESP asked for in #264 and never had: a device class
// that owns its nodes, so a firmware says what the thing IS rather than how
// it is wired.
//
//   espos::devices::TankLevel fresh(g, {
//       .id = "fresh", .gpio = 4,
//       .path = "tanks.freshWater.0.currentLevel",
//       .empty_ohms = 190.0f, .full_ohms = 3.0f,
//       .divider_ohms = 1000.0f,
//   });
//   fresh.start();
//
// Against the same device wired by hand that is four nodes and five edges,
// and the wiring is the part a firmware gets wrong -- an ADC that emits
// volts into a transform expecting ohms is a tank that reads plausibly and
// is wrong all season.
//
// What it does NOT hide: every node is still reachable (`analog()`,
// `level()`), because a device class that cannot be taken apart is a worse
// deal than the wiring it replaced. Add a filter, retitle a parameter, wire
// a second consumer -- all of it stays possible.
#pragma once

#include "espos_devices/detail.hpp"
#include "espos_flow/graph.hpp"
#include "espos_flow/transforms.hpp"
#include "espos_sensors/sensors.hpp"
#include "espos_sk_flow/sk.hpp"

namespace espos::devices
{

struct TankLevelConfig {
    const char *id = "tank";
    int gpio = -1;
    // A path from the Signal K specification, so no metadata is sent: the
    // server already knows currentLevel is a ratio. A path of your own needs
    // sk::Output's Meta overload instead, which this deliberately does not
    // take -- see the note in espos_sk_flow/sk.hpp.
    const char *path = "tanks.freshWater.0.currentLevel";

    // The sender's resistance at the two ends of its travel. Marine senders
    // are commonly 240-33 (US) or 10-180 (European), and plenty are reversed:
    // empty > full is normal, not a mistake.
    float empty_ohms = 190.0f;
    float full_ohms = 3.0f;

    // The fixed leg of the divider the sender sits in, and the voltage across
    // the pair. Both are properties of the board, not of the tank.
    float divider_ohms = 1000.0f;
    float supply_volts = 3.3f;

    uint32_t period_ms = 2000;  // a tank does not move quickly
    uint8_t samples = 16;
};

class TankLevel
{
  public:
    TankLevel(espos::flow::Graph &g, const TankLevelConfig &cfg)
        : cfg_(cfg),
          analog_(g.make<espos::sensors::Analog>(cfg.id, cfg.gpio, cfg.period_ms,
                                                 ESPOS_ADC_ATTEN_12DB,
                                                 cfg.samples)),
          ohms_(g.make<espos::flow::DividerR2>(
              detail::SubId(cfg.id, 'r'), cfg.supply_volts, cfg.divider_ohms)),
          level_(g.make<espos::flow::TankLevel>(detail::SubId(cfg.id, 'l'),
                                                cfg.empty_ohms, cfg.full_ohms)),
          out_(g.make<espos::sk::Output<float>>(cfg.path))
    {
        // volts -> ohms -> ratio -> the server. The chain is the whole point:
        // each step is a unit conversion someone would otherwise fold into a
        // magic constant that nobody can check later.
        analog_ >> ohms_ >> level_ >> out_;
    }

    // Arm the poll. Separate from construction for the reason every node in
    // this tree is: a device built in a static initialiser must not start a
    // timer before the flow runtime exists.
    esp_err_t start() { return analog_.start(); }
    void stop() { analog_.stop(); }

    // Put the sender's two calibration points on a config page, so a tank can
    // be calibrated with a full tank and an empty one rather than a rebuild.
    esp_err_t register_config(const char *title = "Tank calibration")
    {
        return level_.register_config(title, "ohm");
    }

    // The nodes, for a firmware that wants to do more than the default. A
    // device class that cannot be opened is worse than no device class.
    espos::sensors::Analog &analog() { return analog_; }
    espos::flow::DividerR2 &ohms() { return ohms_; }
    espos::flow::TankLevel &level() { return level_; }
    espos::sk::Output<float> &output() { return out_; }

    esp_err_t open_error() const { return analog_.open_error(); }

  private:
    TankLevelConfig cfg_;
    espos::sensors::Analog &analog_;
    espos::flow::DividerR2 &ohms_;
    espos::flow::TankLevel &level_;
    espos::sk::Output<float> &out_;
};

}  // namespace espos::devices

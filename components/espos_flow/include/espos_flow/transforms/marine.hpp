// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// espos::flow — the marine transforms: the espos_formulas arithmetic, wired as
// nodes with live parameters.
//
// The maths lives in `components/espos_formulas` (pure, host-tested against
// published reference values); this file is the thin layer that gives it an
// id, a place in a chain and a config page. Keeping the two apart is what
// makes the arithmetic testable without a device and the nodes trivial enough
// to read at a glance.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

#include "espos_flow/node.hpp"
#include "espos_flow/transforms/param.hpp"
#include "espos_formulas/curve.hpp"
#include "espos_formulas/marine.hpp"
#include "espos_formulas/units.hpp"

namespace espos::flow {

// espos::units is a sibling of espos::flow, not nested in it; naming it once
// here keeps the nodes below reading as `units::rad_to_deg` rather than
// `::espos::units::rad_to_deg`.
namespace units = ::espos::units;

// ────────────────────────────────────────────────────────────────── Curve
//
// Piecewise-linear calibration over a table the user edits IN THE WEB UI.
// SensESP's CurveInterpolator, and the feature that most often turns a
// half-finished installation into a finished one.
//
// The table is a `format: "table"` string config key (docs/config.md): a JSON
// array of {"in": x, "out": y} rows that the UI renders as a row editor, NVS
// stores as readable text, and an export round-trips diffably. So calibrating
// a tank is: fill in ten litres, read the raw value off the device page, type
// the pair, repeat. No rebuild, no cable, no laptop.
//
// Endpoints CLAMP rather than extrapolate — see espos_formulas/curve.hpp for
// why (SensESP #1005 published a NaN below the lowest sample).
//
// `MaxSamples` is a template parameter because the table lives inside the
// node. 32 covers a tank curve with room to spare; the config key's 3999-byte
// budget is the other limit, at roughly 250 rows.
template <std::size_t MaxSamples = 32>
class Curve : public Symmetric<float> {
 public:
  explicit Curve(const char* id) : Symmetric<float>(id) {}

  // Build with a compiled-in table: a thermistor preset from
  // espos_formulas/marine.hpp, or a manufacturer's curve. A user's stored
  // table replaces it once register_config() loads one.
  Curve(const char* id, const formulas::CurveSample* samples, std::size_t count)
      : Symmetric<float>(id) {
    set_samples(samples, count);
  }

  void set(const float& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    std::optional<float> out = formulas::curve_eval(samples_, count_, v);
    // An empty table emits nothing rather than zero: a tank that has never
    // been calibrated should read as "no data", not as "empty".
    if (out.has_value()) this->emit(*out);
  }

  void set_samples(const formulas::CurveSample* samples, std::size_t count) {
    count_ = count < MaxSamples ? count : MaxSamples;
    for (std::size_t i = 0; i < count_; i++) samples_[i] = samples[i];
    formulas::curve_sort(samples_, count_);
  }

  std::size_t sample_count() const { return count_; }
  const formulas::CurveSample* samples() const { return samples_; }

  // Parse the table config key's JSON: [{"in":0,"out":0},{"in":10,"out":0.5}].
  //
  // A hand-rolled scan rather than a JSON parser, for two reasons: the shape
  // is fixed and trivial, and pulling cJSON into espos_flow would give the
  // graph runtime a dependency on a parser that a firmware doing no
  // interpolation would still link. It reads NUMBERS in order, two per row —
  // so it accepts the object form above and the shorter [[0,0],[10,0.5]] one
  // equally, which is what a user typing into a text box will produce anyway.
  //
  // Returns the number of samples parsed. A malformed table yields whatever
  // complete pairs it found, which degrades to "fewer points" rather than to
  // "no calibration at all".
  std::size_t set_table_json(const char* json) {
    count_ = 0;
    if (json == nullptr) return 0;
    const char* p = json;
    float pending = 0.0f;
    bool have_first = false;
    while (*p != '\0' && count_ < MaxSamples) {
      // Skip to the next number, ignoring keys, punctuation and whitespace.
      if (!((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' ||
            (*p == '.' && p[1] >= '0' && p[1] <= '9'))) {
        p++;
        continue;
      }
      char* end = nullptr;
      const float v = std::strtof(p, &end);
      if (end == p) {
        p++;
        continue;
      }
      p = end;
      if (!have_first) {
        pending = v;
        have_first = true;
      } else {
        samples_[count_].input = pending;
        samples_[count_].output = v;
        count_++;
        have_first = false;
      }
    }
    formulas::curve_sort(samples_, count_);
    return count_;
  }

  // Serialise the table back to the config key's JSON form.
  // `buf` needs about 32 bytes per sample.
  std::size_t table_json(char* buf, std::size_t n) const {
    if (buf == nullptr || n == 0) return 0;
    std::size_t w = 0;
    auto put = [&](const char* s) {
      while (*s && w + 1 < n) buf[w++] = *s++;
    };
    put("[");
    for (std::size_t i = 0; i < count_; i++) {
      char row[64];
      std::snprintf(row, sizeof(row), "%s{\"in\":%g,\"out\":%g}", i ? "," : "",
                    static_cast<double>(samples_[i].input),
                    static_cast<double>(samples_[i].output));
      put(row);
    }
    put("]");
    buf[w] = '\0';
    return w;
  }

  // Put the table on a config page and adopt a stored one.
  esp_err_t register_config(const char* title = "Calibration curve",
                            const char* in_label = "input",
                            const char* out_label = "output") {
    columns_[0] = in_label;
    columns_[1] = out_label;
    params_.add_table("curve", "Samples", columns_, 2);
    esp_err_t err = params_.register_ns(this->id(), title, "Curve");
    if (err != ESP_OK) return err;
    // A stored table replaces the compiled-in one; an absent key reads the
    // "[]" default, which would wipe a preset, so an empty parse is ignored.
    char json[512] = {};
    params_.load_str("curve", json, sizeof(json));
    if (json[0] != '\0') {
      const std::size_t before = count_;
      if (set_table_json(json) == 0 && before > 0) {
        // Nothing parsed but we had a preset: keep it rather than emitting
        // nothing forever because a config key was empty.
        return ESP_OK;
      }
    }
    return ESP_OK;
  }

 private:
  formulas::CurveSample samples_[MaxSamples] = {};
  std::size_t count_ = 0;
  const char* columns_[2] = {"input", "output"};
  ParamSet<1> params_;
};

// ─────────────────────────────────────────────────────────────── DewPoint
//
// Temperature and humidity in, dew point out. Arden Buck, in kelvin
// throughout (Signal K's unit).
//
// Two inputs, so it is a node with a second consumer slot rather than a
// Symmetric: `temp` is the primary input and drives the output, `humidity()`
// is the auxiliary. That asymmetry is deliberate — a BME280 reads both in one
// transaction and publishes temperature at the sensor's rate, so recomputing
// on every humidity update as well would double the output rate for no new
// information.
//
// Emits nothing until both have arrived, and nothing when RH is zero (there is
// no dew point for perfectly dry air; SensESP returned -inf).
class DewPoint : public Transform<float, float> {
 public:
  explicit DewPoint(const char* id) : Transform<float, float>(id) {}

  void set(const float& temp_k) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    temp_k_ = temp_k;
    have_temp_ = true;
    compute();
  }

  class Humidity : public Consumer<float> {
   public:
    using consumes_type = float;
    void set(const float& rh) override {
      if (!owner) return;
      owner->rh_ = rh;
      owner->have_rh_ = true;
    }
    DewPoint* owner = nullptr;
  };

  // Wire the humidity producer here: `bme.humidity >> dew.humidity()`.
  Humidity& humidity() {
    rh_in_.owner = this;
    return rh_in_;
  }

 private:
  void compute() {
    if (!have_temp_ || !have_rh_) return;
    std::optional<float> dp = formulas::dew_point_k(temp_k_, rh_);
    if (dp.has_value()) this->emit(*dp);
  }

  float temp_k_ = 0.0f;
  float rh_ = 0.0f;
  bool have_temp_ = false;
  bool have_rh_ = false;
  Humidity rh_in_;
};

// ────────────────────────────────────────────────────────────── HeatIndex
//
// The same two inputs, NOAA's regression, "what it feels like" in kelvin.
class HeatIndex : public Transform<float, float> {
 public:
  // `effect` true emits the DIFFERENCE (how much hotter it feels) rather than
  // the absolute index — SensESP's HeatIndexEffect, which is usually the more
  // useful channel because it is a fact about the space rather than about the
  // weather.
  explicit HeatIndex(const char* id, bool effect = false)
      : Transform<float, float>(id), effect_(effect) {}

  void set(const float& temp_k) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    temp_k_ = temp_k;
    have_temp_ = true;
    if (!have_rh_) return;
    this->emit(effect_ ? formulas::heat_index_effect_k(temp_k_, rh_)
                       : formulas::heat_index_k(temp_k_, rh_));
  }

  class Humidity : public Consumer<float> {
   public:
    using consumes_type = float;
    void set(const float& rh) override {
      if (!owner) return;
      owner->rh_ = rh;
      owner->have_rh_ = true;
    }
    HeatIndex* owner = nullptr;
  };

  Humidity& humidity() {
    rh_in_.owner = this;
    return rh_in_;
  }

 private:
  bool effect_;
  float temp_k_ = 0.0f;
  float rh_ = 0.0f;
  bool have_temp_ = false;
  bool have_rh_ = false;
  Humidity rh_in_;
};

// ───────────────────────────────────────────────────────────── AirDensity
//
// Temperature, pressure and humidity to kg/m3. Three inputs: temperature is
// the primary, pressure and humidity are auxiliaries.
class AirDensity : public Transform<float, float> {
 public:
  explicit AirDensity(const char* id) : Transform<float, float>(id) {}

  void set(const float& temp_k) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    temp_k_ = temp_k;
    if (!(pressure_pa_ > 0.0f) || !(temp_k_ > 0.0f)) return;
    this->emit(formulas::air_density(temp_k_, pressure_pa_, rh_));
  }

  class Pressure : public Consumer<float> {
   public:
    using consumes_type = float;
    void set(const float& pa) override {
      if (owner) owner->pressure_pa_ = pa;
    }
    AirDensity* owner = nullptr;
  };
  class Humidity : public Consumer<float> {
   public:
    using consumes_type = float;
    void set(const float& rh) override {
      if (owner) owner->rh_ = rh;
    }
    AirDensity* owner = nullptr;
  };

  Pressure& pressure() {
    p_in_.owner = this;
    return p_in_;
  }
  Humidity& humidity() {
    rh_in_.owner = this;
    return rh_in_;
  }

 private:
  float temp_k_ = 0.0f;
  // Sea-level standard, so a device with no barometer still produces a usable
  // density rather than nothing. Documented as an assumption, not hidden:
  // 1013.25 hPa is right within a percent for most weather.
  float pressure_pa_ = 101325.0f;
  float rh_ = 0.0f;
  Pressure p_in_;
  Humidity rh_in_;
};

// ─────────────────────────────────────────────────── DividerR1 / R2 / Scale
//
// A measured voltage back to what the sender is doing. See
// espos_formulas/marine.hpp for the circuit and for which form to pick;
// briefly, R2 is the one tank and temperature senders need (they are the
// low-side element with one terminal on the hull).
//
// All three take the ADC voltage as their input and their circuit constants as
// live parameters, because the resistor you soldered is 1% at best and the
// supply rail is not exactly 3.3 V. Both are things a user measures once and
// types in.
class DividerR2 : public Symmetric<float> {
 public:
  DividerR2(const char* id, float v_in, float r1_ohm)
      : Symmetric<float>(id), v_in_(v_in), r1_(r1_ohm) {}

  void set(const float& v_out) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    std::optional<float> r = formulas::divider_r2(v_out, v_in_, r1_);
    // An open sender emits nothing rather than infinity: a disconnected wire
    // must not read as a full tank.
    if (r.has_value()) this->emit(*r);
  }

  esp_err_t register_config(const char* title = "Divider") {
    params_.add_float("vin", "Supply voltage", "V", v_in_);
    params_.add_float("r1", "Fixed resistor", "ohm", r1_);
    esp_err_t err = params_.register_ns(this->id(), title, "Resistive divider");
    if (err != ESP_OK) return err;
    params_.load_float("vin", &v_in_);
    params_.load_float("r1", &r1_);
    return ESP_OK;
  }

 private:
  float v_in_;
  float r1_;
  ParamSet<2> params_;
};

class DividerR1 : public Symmetric<float> {
 public:
  DividerR1(const char* id, float v_in, float r2_ohm)
      : Symmetric<float>(id), v_in_(v_in), r2_(r2_ohm) {}

  void set(const float& v_out) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    std::optional<float> r = formulas::divider_r1(v_out, v_in_, r2_);
    if (r.has_value()) this->emit(*r);
  }

  esp_err_t register_config(const char* title = "Divider") {
    params_.add_float("vin", "Supply voltage", "V", v_in_);
    params_.add_float("r2", "Fixed resistor", "ohm", r2_);
    esp_err_t err = params_.register_ns(this->id(), title, "Resistive divider");
    if (err != ESP_OK) return err;
    params_.load_float("vin", &v_in_);
    params_.load_float("r2", &r2_);
    return ESP_OK;
  }

 private:
  float v_in_;
  float r2_;
  ParamSet<2> params_;
};

// Undo a known attenuation: what a 12 V bus monitor needs, where the divider
// exists only to bring 14 V inside the ADC's 3.3 V range.
class DividerScale : public Symmetric<float> {
 public:
  DividerScale(const char* id, float r1_ohm, float r2_ohm)
      : Symmetric<float>(id), r1_(r1_ohm), r2_(r2_ohm) {}

  void set(const float& v_out) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    this->emit(formulas::divider_scale(v_out, r1_, r2_));
  }

  esp_err_t register_config(const char* title = "Divider") {
    params_.add_float("r1", "Upper resistor", "ohm", r1_);
    params_.add_float("r2", "Lower resistor", "ohm", r2_);
    esp_err_t err = params_.register_ns(this->id(), title, "Voltage divider");
    if (err != ESP_OK) return err;
    params_.load_float("r1", &r1_);
    params_.load_float("r2", &r2_);
    return ESP_OK;
  }

 private:
  float r1_;
  float r2_;
  ParamSet<2> params_;
};

// ──────────────────────────────────────────────────────────── Frequency
//
// Pulse counts to a rate. The input is a COUNT for the elapsed period, which
// is what a pulse-counter peripheral or an ISR-fed Mailbox produces; the node
// divides by the period and by the pulses-per-unit multiplier.
//
// `pulses_per_unit` of 6 turns an alternator's W terminal into engine
// revolutions per second — see espos_formulas/marine.hpp. Signal K wants Hz,
// not RPM: units::hz_to_rpm exists for a display, not for a publish.
class Frequency : public Transform<int32_t, float> {
 public:
  Frequency(const char* id, uint32_t period_ms, float pulses_per_unit = 1.0f)
      : Transform<int32_t, float>(id),
        period_s_(static_cast<float>(period_ms) / 1000.0f),
        ppu_(pulses_per_unit) {}

  void set(const int32_t& count) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    std::optional<float> hz = formulas::frequency_hz(
        static_cast<uint32_t>(count < 0 ? 0 : count), period_s_, ppu_);
    if (hz.has_value()) this->emit(*hz);
  }

  esp_err_t register_config(const char* title = "Frequency") {
    params_.add_float("ppu", "Pulses per revolution", "", ppu_);
    esp_err_t err = params_.register_ns(this->id(), title, "Frequency");
    if (err != ESP_OK) return err;
    params_.load_float("ppu", &ppu_);
    return ESP_OK;
  }

 private:
  float period_s_;
  float ppu_;
  ParamSet<1> params_;
};

// ──────────────────────────────────────────────────────── AngleOffset
//
// Add a calibration offset to an angle and wrap the result.
//
// SensESP's AngleCorrection, plus the interval choice it lacked: `min` of 0
// gives [0, 2pi) for a heading, and -pi gives [-pi, pi) for a relative angle
// like apparent wind or rudder, where the sign is the information.
//
// The offset is stored in RADIANS (SI) and shown in DEGREES: the config key
// carries `displayMultiplier` 180/pi, so a user types "the sensor is mounted
// 12 degrees off the bow" and NVS holds 0.2094.
class AngleOffset : public Symmetric<float> {
 public:
  AngleOffset(const char* id, float offset_rad = 0.0f, float min_rad = 0.0f)
      : Symmetric<float>(id), offset_(offset_rad), min_(min_rad) {}

  void set(const float& rad) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    this->emit(formulas::angle_offset(rad, offset_, min_));
  }

  void set_offset(float rad) { offset_ = rad; }
  float offset() const { return offset_; }

  esp_err_t register_config(const char* title = "Angle offset") {
    params_.add_float("off", "Offset", "rad", offset_, "",
                      units::rad_to_deg(1.0f));
    esp_err_t err = params_.register_ns(this->id(), title, "Angle offset");
    if (err != ESP_OK) return err;
    params_.load_float("off", &offset_);
    return ESP_OK;
  }

 private:
  float offset_;
  float min_;
  ParamSet<1> params_;
};

// Wrap without an offset: normalise an angle that arrived outside its
// interval. Useful after arithmetic — a heading plus a rate-of-turn
// integration will drift out of [0, 2pi) and needs folding back.
class WrapAngle : public Symmetric<float> {
 public:
  explicit WrapAngle(const char* id, float min_rad = 0.0f)
      : Symmetric<float>(id), min_(min_rad) {}

  void set(const float& rad) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    this->emit(formulas::wrap_angle(rad, min_));
  }

 private:
  float min_;
};

// ─────────────────────────────────────────────────────────── TankLevel
//
// A sender reading to a Signal K ratio 0..1, linear and clamped.
//
// `empty` and `full` are the sender's readings at those states and may be in
// either order — a US resistive sender is 240 ohm empty and 33 ohm full, so
// `full < empty` is the common case.
//
// For a tank that is not a box (and most are not — they follow the hull),
// `Curve` is the right node instead: the relationship between sender angle and
// litres is whatever shape the hull is. TankLevel is the two-point
// approximation for a tank that really is prismatic, and the starting point
// before someone does the bucket work.
class TankLevel : public Symmetric<float> {
 public:
  TankLevel(const char* id, float empty, float full)
      : Symmetric<float>(id), empty_(empty), full_(full) {}

  void set(const float& reading) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    this->emit(formulas::tank_level(reading, empty_, full_));
  }

  esp_err_t register_config(const char* title = "Tank calibration",
                            const char* unit = "ohm") {
    params_.add_float("empty", "Reading when empty", unit, empty_);
    params_.add_float("full", "Reading when full", unit, full_);
    esp_err_t err = params_.register_ns(this->id(), title, "Tank level");
    if (err != ESP_OK) return err;
    params_.load_float("empty", &empty_);
    params_.load_float("full", &full_);
    return ESP_OK;
  }

 private:
  float empty_;
  float full_;
  ParamSet<2> params_;
};

// ──────────────────────────────────────────────────────────── BatterySoc
//
// Resting terminal voltage to an estimated state of charge, 0..1.
//
// Read the caveat in espos_formulas/marine.hpp before wiring this: voltage SoC
// is only meaningful at rest, and for LiFePO4 it is marginal even then,
// because the discharge curve is deliberately flat. Publish it as an estimate,
// alarm on the ends where the curve is steep, and use a shunt if the number
// has to be trusted.
//
// It is here because "roughly how full is the bank" while the boat sits on a
// mooring is genuinely useful and a voltage divider can already answer it.
class BatterySoc : public Symmetric<float> {
 public:
  BatterySoc(
      const char* id,
      formulas::BatteryChemistry chem = formulas::BatteryChemistry::kLifepo4,
      float nominal_v = 12.0f)
      : Symmetric<float>(id), chem_(chem), nominal_v_(nominal_v) {}

  void set(const float& volts) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    this->emit(formulas::battery_soc(volts, chem_, nominal_v_));
  }

  void set_chemistry(formulas::BatteryChemistry c) { chem_ = c; }

 private:
  formulas::BatteryChemistry chem_;
  float nominal_v_;
};

}  // namespace espos::flow

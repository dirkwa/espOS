// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// espos::flow — the bridge from a transform's parameters to the web UI.
//
// A calibration that cannot be changed without a rebuild is not a calibration.
// SensESP understood this — `Linear(1.7, -0.16, "/indoor/linear")` put the
// multiplier and the offset on a config page — and it is the single feature
// that most often decides whether a boater can finish an installation on the
// dock or has to go home for a laptop and a cable.
//
// espos_config already has the machinery (docs/config.md): a node builds an
// `espos_cfg_ns_t`, hands it to `espos_config_register_ns()`, and from that
// moment its parameters are typed, validated, exported, schema'd and stored in
// NVS under `f_<id>` exactly like a compiled namespace. What is missing is the
// tedious half — declaring the key table, loading the stored values at start,
// writing them back when the UI changes one. That is what this header is.
//
// ── The shape ────────────────────────────────────────────────────────────
//
//   class Linear : public Symmetric<float> {
//     ...
//     ParamSet<2> params_;   // storage for the descriptor, in the node
//   };
//
// A node declares a `ParamSet<N>`, describes each key once in its constructor,
// and calls `register_config()`. The ParamSet owns every string and the key
// array itself, which is what makes the ownership rule in
// `espos_config_register_ns()` ("the descriptor is borrowed, never copied")
// safe: the descriptor lives inside the node, and the node lives forever.
//
// Nodes stay usable with no config at all. A firmware that never calls
// `register_config()` links none of this, and a host test constructs a Linear
// and drives it without an NVS anywhere. That matters: the transforms are the
// part with the interesting arithmetic, and their tests should not need a
// config store to run.
//
// ── Threading ────────────────────────────────────────────────────────────
//
// `espos_config`'s change callbacks run on whichever task called the setter —
// the HTTP task, for a web-UI edit. A node's parameters are read on the flow
// task. Both are plain scalar loads and stores of at most 4 bytes, which are
// atomic on every target espOS supports, and a parameter changing between two
// reads within one transform is harmless: the next reading uses the new value,
// which is what the user asked for. Nothing here takes a lock, and nothing
// here may block.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "espos_flow/node.hpp"

// espos_config is optional. A firmware that does not build it still compiles
// every transform; it just cannot put their parameters on a config page.
// ESPOS_FLOW_HAVE_CONFIG is set by the component's CMakeLists when
// espos_config is in BUILD_COMPONENTS, the same pattern espos_flow already
// uses for espos_health.
#if ESPOS_FLOW_HAVE_CONFIG
#include "espos_config.h"
#include "espos_config_desc.h"
#endif

namespace espos::flow {

#if ESPOS_FLOW_HAVE_CONFIG

// Storage for a node's runtime config descriptor: the key table, the namespace
// struct and the namespace name, all inside the node so their lifetime is the
// node's. N is the number of parameters, known at compile time because a
// transform's parameter list is.
template <std::size_t N>
class ParamSet {
 public:
  static constexpr std::size_t kMax = N;

  // Declare a float parameter. `name` must be a string literal or otherwise
  // outlive the node — nothing is copied, per espos_config's ownership rule.
  // `display_mul` is the UI scaling from docs/config.md: stored SI, displayed
  // in whatever a human types. 0 means "identity".
  void add_float(const char* name, const char* title, const char* unit,
                 float def, const char* group = "", float display_mul = 0.0f) {
    if (count_ >= N) return;
    espos_cfg_key_t& k = keys_[count_++];
    k = espos_cfg_key_t{};
    k.name = name;
    k.title = title;
    k.description = "";
    k.unit = unit;
    k.type = ESPOS_CFG_TYPE_FLOAT;
    k.def.f = def;
    k.display.group = group;
    k.display.display_mul = display_mul;
  }

  void add_int(const char* name, const char* title, const char* unit,
               int32_t def, int32_t min, int32_t max, const char* group = "",
               float display_mul = 0.0f) {
    if (count_ >= N) return;
    espos_cfg_key_t& k = keys_[count_++];
    k = espos_cfg_key_t{};
    k.name = name;
    k.title = title;
    k.description = "";
    k.unit = unit;
    k.type = ESPOS_CFG_TYPE_INT;
    k.def.i = def;
    k.min.i = min;
    k.max.i = max;
    k.has_min = true;
    k.has_max = true;
    k.display.group = group;
    k.display.display_mul = display_mul;
  }

  void add_bool(const char* name, const char* title, bool def,
                const char* group = "") {
    if (count_ >= N) return;
    espos_cfg_key_t& k = keys_[count_++];
    k = espos_cfg_key_t{};
    k.name = name;
    k.title = title;
    k.description = "";
    k.unit = "";
    k.type = ESPOS_CFG_TYPE_BOOL;
    k.def.b = def;
    k.display.group = group;
  }

  // A read-only key: shown, never editable. RunHours' accumulated total is
  // one — a user must see it and must not be able to type a new engine age.
  void add_float_ro(const char* name, const char* title, const char* unit,
                    float def, float display_mul = 0.0f) {
    if (count_ >= N) return;
    add_float(name, title, unit, def, "", display_mul);
    keys_[count_ - 1].flags |= ESPOS_CFG_FLAG_READ_ONLY;
  }

  // A table parameter: a string key holding a JSON array of rows, rendered by
  // the UI as a row editor (docs/config.md, `format: "table"`). This is how
  // Curve's sample table is calibrated on a phone at the fuel dock.
  //
  // `columns` must outlive the node, like every other string here. `max_len`
  // defaults to the full NVS string budget, which is about 250 numeric rows.
  void add_table(const char* name, const char* title,
                 const char* const* columns, std::size_t column_count,
                 std::size_t max_len = 3999) {
    if (count_ >= N) return;
    espos_cfg_key_t& k = keys_[count_++];
    k = espos_cfg_key_t{};
    k.name = name;
    k.title = title;
    k.description = "";
    k.unit = "";
    k.type = ESPOS_CFG_TYPE_STRING;
    k.def.s = "[]";
    k.max_len = max_len;
    k.display.table_columns = columns;
    k.display.table_column_count = column_count;
  }

  // Register the namespace. `id` is the node id (≤ 12 characters, so that
  // "f_" + id fits NVS's 15); `title` is the tab label.
  //
  // Returns whatever espos_config_register_ns() returns. A failure is already
  // logged and raises the `flowConfig` health condition there, so a caller
  // that ignores the result still gets a loud error rather than a node whose
  // settings quietly do not appear.
  esp_err_t register_ns(const char* id, const char* title,
                        const char* description) {
    esp_err_t err = espos_config_flow_ns_name(id, ns_name_, sizeof(ns_name_));
    if (err != ESP_OK) return err;
    ns_ = espos_cfg_ns_t{};
    ns_.name = ns_name_;
    ns_.title = title;
    ns_.version = 1;
    ns_.keys = keys_;
    ns_.key_count = count_;
    ns_.description = description;
    err = espos_config_register_ns(&ns_);
    if (err == ESP_OK) registered_ = true;
    return err;
  }

  // The NVS namespace name, or "" before register_ns() succeeded. Getters and
  // setters below tolerate the empty name so a node with no config still
  // works — they simply do nothing.
  const char* ns_name() const { return registered_ ? ns_name_ : ""; }
  bool registered() const { return registered_; }
  std::size_t count() const { return count_; }

  // Read a stored value, or leave *out alone when there is no config.
  void load_float(const char* key, float* out) const {
    if (registered_) espos_config_get_float(ns_name_, key, out);
  }
  void load_int(const char* key, int32_t* out) const {
    if (registered_) espos_config_get_i32(ns_name_, key, out);
  }
  void load_bool(const char* key, bool* out) const {
    if (registered_) espos_config_get_bool(ns_name_, key, out);
  }
  void load_str(const char* key, char* buf, std::size_t n) const {
    if (registered_) espos_config_get_str(ns_name_, key, buf, n, nullptr);
  }

  esp_err_t store_float(const char* key, float v) const {
    return registered_ ? espos_config_set_float(ns_name_, key, v) : ESP_OK;
  }
  esp_err_t store_str(const char* key, const char* v) const {
    return registered_ ? espos_config_set_str(ns_name_, key, v) : ESP_OK;
  }

 private:
  espos_cfg_key_t keys_[N] = {};
  espos_cfg_ns_t ns_ = {};
  char ns_name_[ESPOS_CFG_NS_NAME_MAX + 1] = {};
  std::size_t count_ = 0;
  bool registered_ = false;
};

#else  // !ESPOS_FLOW_HAVE_CONFIG

// Same surface, no store. Every transform compiles and runs; register_config()
// is a no-op that reports ESP_ERR_NOT_SUPPORTED, and the compiled-in defaults
// (or whatever the constructor was given) are what the node uses. This is what
// the host test builds against, and what a firmware built without
// espos_config gets.
template <std::size_t N>
class ParamSet {
 public:
  static constexpr std::size_t kMax = N;

  void add_float(const char*, const char*, const char*, float, const char* = "",
                 float = 0.0f) {}
  void add_int(const char*, const char*, const char*, int32_t, int32_t, int32_t,
               const char* = "", float = 0.0f) {}
  void add_bool(const char*, const char*, bool, const char* = "") {}
  void add_float_ro(const char*, const char*, const char*, float,
                    float = 0.0f) {}
  void add_table(const char*, const char*, const char* const*, std::size_t,
                 std::size_t = 3999) {}

  esp_err_t register_ns(const char*, const char*, const char*) {
    return ESP_ERR_NOT_SUPPORTED;
  }

  const char* ns_name() const { return ""; }
  bool registered() const { return false; }
  std::size_t count() const { return 0; }

  void load_float(const char*, float*) const {}
  void load_int(const char*, int32_t*) const {}
  void load_bool(const char*, bool*) const {}
  void load_str(const char*, char*, std::size_t) const {}
  esp_err_t store_float(const char*, float) const { return ESP_OK; }
  esp_err_t store_str(const char*, const char*) const { return ESP_OK; }
};

#endif  // ESPOS_FLOW_HAVE_CONFIG

}  // namespace espos::flow

// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// espos::devices — whole marine devices, in one include.
//
//   #include "espos_devices.hpp"
//
// Each class owns its nodes and wires them in its constructor, so a firmware
// says what the thing IS rather than how it is plumbed (SensESP #264). Every
// node stays reachable: a device class that cannot be taken apart is a worse
// deal than the wiring it replaced.
//
// See docs/devices.md for what each one publishes and why.
#pragma once

#include "espos_devices/engine.hpp"
#include "espos_devices/switches.hpp"
#include "espos_devices/tank.hpp"
#include "espos_devices/temperature.hpp"

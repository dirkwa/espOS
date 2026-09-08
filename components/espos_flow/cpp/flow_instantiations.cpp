// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// The one translation unit that instantiates the common node templates.
// espos_flow/flow.hpp declares the same list `extern template`, so every
// other TU in the firmware references these definitions instead of emitting
// its own copy and leaving the linker to fold them.
//
// This is the direct answer to SensESP #339, where template instantiation was
// the largest single contributor to image size. Keep the list to types a
// Signal K device actually publishes: every entry here is flash spent whether
// the firmware uses it or not — though only for a firmware that links the
// component at all, since an unreferenced object file is dropped.
#include <cstdint>
#include <optional>
#include <string>

#include "espos_flow/flow.hpp"

namespace espos::flow {

template class Producer<float>;
template class Producer<double>;
template class Producer<int32_t>;
template class Producer<bool>;
template class Producer<std::string>;
template class Producer<std::optional<float>>;

template class Consumer<float>;
template class Consumer<double>;
template class Consumer<int32_t>;
template class Consumer<bool>;
template class Consumer<std::string>;
template class Consumer<std::optional<float>>;

template class Value<float>;
template class Value<double>;
template class Value<int32_t>;
template class Value<bool>;
template class Value<std::string>;

template class Transform<float, float>;
template class Transform<double, double>;
template class Transform<int32_t, int32_t>;
template class Transform<bool, bool>;
template class Transform<float, bool>;
template class Transform<float, int32_t>;

template class Constant<float>;
template class Constant<int32_t>;
template class Constant<bool>;

}  // namespace espos::flow

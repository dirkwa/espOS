// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// Shared internals for the device classes. Not part of the API.
#pragma once

#include <cstddef>

#include "espos_flow/node.hpp"

namespace espos::devices::detail
{

// A device owns several nodes and each needs its own id. A node id is capped
// at 12 characters and becomes an NVS namespace (`f_<id>`), so a part is the
// device's id plus ONE letter -- `<id>_scale` would truncate, and two parts
// of the same device would then collide silently and share a config
// namespace.
//
// Returned by value: NodeBase copies the id into its own storage, so the
// temporary only has to outlive the make<>() call.
struct SubId {
    char text[espos::flow::kIdMax + 1] = {};

    SubId(const char *id, char suffix)
    {
        std::size_t n = 0;
        while (n < espos::flow::kIdMax - 1 && id && id[n]) {
            text[n] = id[n];
            n++;
        }
        text[n++] = suffix;
        text[n] = '\0';
    }

    operator const char *() const { return text; }
};

}  // namespace espos::devices::detail

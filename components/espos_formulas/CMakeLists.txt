# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# espos_formulas: the marine arithmetic (docs/transforms.md).
#
# Header-only, and deliberately so. Every function is `inline` or `constexpr`
# arithmetic over floats; there is nothing to put in a .c file that the
# compiler would not have folded away anyway, and a header-only component
# costs a firmware that does not call it exactly zero bytes.
#
# It has NO dependencies at all -- not espos_flow, not espos_config, not IDF
# beyond the C++ standard library. That is the point: the host test compiles
# these headers on the linux target with nothing mocked, so the reference
# values it checks are checked against the same code the firmware runs.
#
# An IDF component with no SRCS becomes an INTERFACE library, which propagates
# the include directory to anything that REQUIREs it and links nothing.
idf_component_register(INCLUDE_DIRS include)

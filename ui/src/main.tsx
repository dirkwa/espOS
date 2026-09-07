// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
// espOS's own bundle: the core pages and nothing else. A firmware that wants
// pages of its own has its own entry point calling registerPage() before
// mount() — docs/ui.md.
import { mount } from "./mount";

mount();

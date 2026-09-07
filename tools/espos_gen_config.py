#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
# Compatibility shim: the generator moved into its component so espos_config
# is self-contained when installed from the registry. Out-of-tree callers of
# this path keep working; new ones should use the component path directly.
import os
import sys
_REAL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "components", "espos_config", "tools", "espos_gen_config.py")
print("espos_gen_config: tools/espos_gen_config.py is deprecated, use components/espos_config/tools/espos_gen_config.py", file=sys.stderr)
with open(_REAL, encoding="utf-8") as f:
    exec(compile(f.read(), _REAL, "exec"), {"__name__": "__main__", "__file__": _REAL})

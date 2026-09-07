#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
"""
Check that every CONFIG_ESPOS_* symbol the documentation names exists.

Documentation drifts in a way the build cannot notice: a Kconfig knob is
renamed, or never lands, and the paragraph that names it keeps reading as if
it were real (docs/signalk.md once documented CONFIG_ESPOS_SK_MAX_NOTIFY,
which no component ever defined). CONFIG_ names in prose are not compiled,
so this script compares the two sources of truth already in the tree:

  * every `config ESPOS_...` in components/*/Kconfig is a real symbol;
  * every CONFIG_ESPOS_[A-Z0-9_]+ token in README.md, docs/**/*.md and
    components/**/*.md must be one of them.

A token that ends in an underscore ("CONFIG_ESPOS_HTTPD_*" in prose) is a
prefix and passes when at least one symbol starts with it.

Unknown tokens are printed as file:line: TOKEN and the exit status is 1;
0 when the docs are clean. Standard library only, so it runs where the IDF
environment does not exist (CI's docs job, a pre-commit hook).

  python3 tools/check_kconfig_docs.py            # from anywhere in the tree
  python3 tools/check_kconfig_docs.py --root DIR
"""

import argparse
import re
import sys
from pathlib import Path

SYMBOL_RE = re.compile(r"^\s*(?:menu)?config\s+(ESPOS_[A-Z0-9_]+)\s*$")
TOKEN_RE = re.compile(r"\bCONFIG_(ESPOS_[A-Z0-9_]+)")


def kconfig_symbols(root: Path) -> set[str]:
    """Every symbol declared by a component Kconfig (Kconfig.projbuild too)."""
    symbols: set[str] = set()
    for kconfig in sorted(root.glob("components/*/Kconfig*")):
        for line in kconfig.read_text(encoding="utf-8").splitlines():
            m = SYMBOL_RE.match(line)
            if m:
                symbols.add(m.group(1))
    return symbols


def doc_files(root: Path) -> list[Path]:
    files = [root / "README.md"]
    files += sorted(root.glob("docs/**/*.md"))
    # Component-level notes, but not a UI bundle or a vendored package that
    # happens to ship a README.
    files += sorted(
        p for p in root.glob("components/**/*.md")
        if not any(part in ("www", "node_modules", "build") for part in p.parts)
    )
    return [f for f in files if f.is_file()]


def is_known(token: str, symbols: set[str]) -> bool:
    if token in symbols:
        return True
    if token.endswith("_"):
        return any(s.startswith(token) for s in symbols)
    return False


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="repository root (default: the parent of tools/)",
    )
    args = ap.parse_args()
    root = args.root.resolve()

    symbols = kconfig_symbols(root)
    if not symbols:
        print(f"{root}: no `config ESPOS_*` symbols found under components/*/Kconfig",
              file=sys.stderr)
        return 2

    unknown = 0
    references = 0
    files = doc_files(root)
    for path in files:
        for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            for m in TOKEN_RE.finditer(line):
                references += 1
                token = m.group(1)
                if not is_known(token, symbols):
                    unknown += 1
                    print(f"{path.relative_to(root)}:{lineno}: CONFIG_{token} "
                          f"is not defined in any components/*/Kconfig")

    if unknown:
        print(f"{unknown} unknown CONFIG_ESPOS_ reference(s) "
              f"(checked {references} in {len(files)} files against {len(symbols)} symbols)",
              file=sys.stderr)
        return 1
    print(f"ok: {references} CONFIG_ESPOS_ reference(s) in {len(files)} files, "
          f"all among the {len(symbols)} Kconfig symbols")
    return 0


if __name__ == "__main__":
    sys.exit(main())

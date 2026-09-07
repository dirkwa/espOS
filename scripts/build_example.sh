#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# Build one example project for one target, the way CI does it: inside the
# example directory, with idf.py's default build/ and sdkconfig (both
# git-ignored there). On a shared development host prefer scripts/build.sh
# with -B pointing outside the tree; this script exists so the CI workflow
# needs no quoting at all.
#
# Usage: scripts/build_example.sh <example-dir> <target>
set -euo pipefail
dir="${1:?example directory}"
target="${2:?idf target}"
cd "$(dirname "$0")/.."
[ -f "$dir/CMakeLists.txt" ] || { echo "build_example.sh: no CMakeLists.txt in $dir" >&2; exit 2; }
cd "$dir"
idf.py set-target "$target"
idf.py build

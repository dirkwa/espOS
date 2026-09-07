#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# Every component must pack for the Espressif Component Registry, or the
# release job that uploads them fails after the tag exists; and every manifest
# must carry version.txt's version (releases are lockstep).
#
# Usage: scripts/check_manifests.sh [dest-dir]     (default: build/registry-pack)
# Needs the ESP-IDF environment (compote is part of it).
set -euo pipefail
cd "$(dirname "$0")/.."
v=$(cat version.txt)
dest="${1:-build/registry-pack}"
mkdir -p "$dest"
for c in components/espos_*/; do
  n=$(basename "$c")
  grep -qxF "version: \"$v\"" "$c/idf_component.yml" || {
    echo "$n: manifest version differs from version.txt ($v)" >&2; exit 1; }
  compote component pack --project-dir "$c" --name "$n" --dest-dir "$dest" >/dev/null
done
echo "check_manifests.sh: $(ls "$dest"/*.tgz | wc -l) components pack at version $v"

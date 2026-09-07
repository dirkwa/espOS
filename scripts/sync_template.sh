#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# Generate the espos-template repository content from the `minimal` example.
# The example is the single source of truth (CI builds it on every target);
# the template is what `git clone --recursive` hands a newcomer: the same
# project with espOS as the `espos/` submodule instead of a relative include.
#
# Usage: scripts/sync_template.sh <dest-dir> [espos-ref]
#   dest-dir   a checkout of espos-template (or an empty directory)
#   espos-ref  tag or commit the .gitmodules/README should name (default: HEAD)
# The script writes files only; committing is the caller's job.
set -euo pipefail
dest="${1:?destination directory}"
ref="${2:-$(git rev-parse --short HEAD)}"
here="$(cd "$(dirname "$0")/.." && pwd)"
src="$here/components/espos_core/examples/minimal"
mkdir -p "$dest/main" "$dest/.github/workflows"

# main/: identical to the example, the firmware name aside.
cp "$src/main/main.c" "$src/main/CMakeLists.txt" "$src/main/idf_component.yml" "$dest/main/"

# Root CMakeLists: the include path is the only difference to the example.
sed -e 's#include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/espos_project.cmake")#include("${CMAKE_CURRENT_LIST_DIR}/espos/cmake/espos_project.cmake")#' \
    -e 's#espos_project_prologue(NAME "minimal")#espos_project_prologue(NAME "my-device")#' \
    -e 's#^project(minimal)#project(my_device)#' \
    -e '/^# into this directory\. Used as a template outside the espOS tree, the include$/,/^# espOS as the `espos\/` submodule (docs\/development\.md)\.$/d' \
    "$src/CMakeLists.txt" > "$dest/CMakeLists.txt"

cp "$here/.idf-version" "$dest/.idf-version"
cp "$here/LICENSE" "$dest/LICENSE"
cat > "$dest/.gitmodules" <<GM
[submodule "espos"]
	path = espos
	url = https://github.com/signalk-espOS/espOS.git
GM
cat > "$dest/.gitignore" <<GI
build/
build-*/
sdkconfig
sdkconfig.old
sdkconfig.local
managed_components/
dependencies.lock
*.pem
GI
cat > "$dest/.github/workflows/ci.yml" <<CI
name: CI

on:
  push:
    branches: [main]
  pull_request:

jobs:
  firmware:
    name: build \${{ matrix.target }}
    runs-on: ubuntu-latest
    timeout-minutes: 45
    strategy:
      fail-fast: false
      matrix:
        target: [esp32, esp32s3, esp32c3, esp32c6, esp32p4]
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive
          fetch-depth: 0
      - name: read pinned IDF version
        id: idf
        run: echo "version=\$(cat espos/.idf-version)" >> "\$GITHUB_OUTPUT"
      - uses: espressif/esp-idf-ci-action@v1
        with:
          esp_idf_version: \${{ steps.idf.outputs.version }}
          target: \${{ matrix.target }}
          command: idf.py -DSDKCONFIG=build/sdkconfig build
CI
cat > "$dest/README.md" <<RM
# my-device — an espOS firmware

Start here for a Signal K device on an ESP32. This repository is
[espOS](https://github.com/signalk-espOS/espOS)'s \`minimal\` example with espOS
as a submodule: one call boots the runtime (WiFi with a setup portal, the web
UI, Signal K discovery and access request, signed OTA), then the application
publishes.

\`\`\`sh
. \$IDF_PATH/export.sh                          # ESP-IDF 6.0.x (espos/.idf-version says which)
git clone --recursive https://github.com/signalk-espOS/espos-template my-device
cd my-device
idf.py set-target esp32c6                      # esp32 | esp32s3 | esp32c3 | esp32c6 | esp32p4
idf.py build flash monitor
\`\`\`

The monitor narrates the rest: join the \`espOS-xxxx\` access point and enter
your WiFi, watch the device find your Signal K server, approve its request
under **Security → Access Requests**, and see \`environment.inside.temperature\`
in the Data Browser. Details, settings and next steps:
[Getting started](https://signalk-espos.github.io/espOS/getting-started/).

## Make it yours

- \`main/main.c\` — replace the constant with your sensor read; add a
  \`config/app.json\` descriptor for settings (the web UI renders it).
- \`CMakeLists.txt\` — \`NAME\`, \`project()\`, \`PARTITIONS\` for bigger flash,
  \`COMPONENTS\` for the optional espOS parts (\`espos_ble\`, \`espos_n2k\`, …).
- \`main/CMakeLists.txt\` — the espOS components your firmware uses.
- Update espOS: \`git -C espos fetch --tags && git -C espos checkout <tag>\`,
  then commit the new submodule pointer.

Generated from espOS ${ref} by \`scripts/sync_template.sh\`; edit the example
there, not the copy here.
RM
echo "sync_template.sh: template written to $dest (espOS $ref)"

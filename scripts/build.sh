#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
# Run an ESP-IDF build without starving the host.
#
# A full build saturates all cores on a 4-core Pi and the interactive session
# stops getting scheduled -- at -j3 the host has frozen hard enough to need a
# power cycle. nice/ionice plus HALF the cores fix that, and ONE lock shared by
# every espOS project on the machine keeps two builds from ever running at the
# same time (two builds at -j2 are the same freeze as one at -j4).
#
# Usage:  scripts/build.sh [idf.py args...]          (default: build)
#         scripts/build.sh -B build-esp32p4 -DIDF_TARGET=esp32p4 build
#         scripts/build.sh --preview set-target linux
#         BUILD_JOBS=4 scripts/build.sh build           (bigger machine only)
#
# Consumers call it through their submodule: espos/scripts/build.sh. Works from
# any project directory, including test/host/* projects.
set -euo pipefail

# Not /tmp and not XDG_RUNTIME_DIR: both are tmpfs on this host, and nothing a
# build touches should live in the RAM the compiler is short of. TMPDIR is
# redirected for the same reason: mkstemp(), python's tempfile and IDF's linux
# partition emulation all honour it and otherwise land in RAM.
export TMPDIR="${TMPDIR:-$HOME/dev/tmp}"
mkdir -p "$TMPDIR"
LOCK_DIR="${ESPOS_BUILD_LOCK_DIR:-$HOME/.cache}"
mkdir -p "$LOCK_DIR"
LOCK="$LOCK_DIR/.idf-build-$(id -u).lock"          # global: one IDF build per user
exec 9>"$LOCK"
if ! flock -n 9; then
  if [ "${BUILD_NOWAIT:-0}" = "1" ]; then
    echo "==> another build is running (BUILD_NOWAIT=1) -- aborting" >&2
    exit 1
  fi
  echo "==> waiting for the running build to finish..." >&2
  flock 9
fi

if [ -z "${IDF_PATH:-}" ]; then
  here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
  echo "==> IDF_PATH not set: source the export.sh of ESP-IDF $(cat "$here/.idf-version" 2>/dev/null || echo '(see .idf-version)')" >&2
  exit 1
fi

# ccache when the host has it: IDF wires it in itself when IDF_CCACHE_ENABLE is
# set (idf.py adds the compiler launcher on the FIRST configure of a build
# directory, so an existing build dir keeps building without it until it is
# recreated). Opt-in by presence -- the reference Pi does not have ccache
# installed, and forcing it there would only turn every build into an error.
# An explicit IDF_CCACHE_ENABLE=0 in the environment still wins.
if command -v ccache >/dev/null 2>&1; then
  export IDF_CCACHE_ENABLE="${IDF_CCACHE_ENABLE:-1}"
fi

# IDF 6's idf.py has no -j option; parallelism is ninja's. Configure via
# idf.py (component manager, sdkconfig), then compile with a capped ninja.
# Half the cores, floor of 1: on a 4-core Pi that is 2. Raise deliberately with
# BUILD_JOBS on a bigger machine -- do not raise it here.
JOBS="${BUILD_JOBS:-$(( $(nproc) / 2 ))}"
[ "$JOBS" -lt 1 ] && JOBS=1
# One job when memory is short: a single esp-sr/esp-dl C++ object can take
# ~700 MB to compile, and two of them next to a running application stack
# have frozen this 8 GB host outright (2026-09-07).
if [ -z "${BUILD_JOBS:-}" ]; then
  avail_mb=$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo)
  if [ "${avail_mb:-0}" -lt 3000 ]; then
    echo "==> only ${avail_mb} MB available: compiling with 1 job" >&2
    JOBS=1
  fi
fi

# Find the build directory (-B <dir> or -B<dir>), default build/.
BUILD_DIR=build
args=("$@")
for ((i = 0; i < ${#args[@]}; i++)); do
  case "${args[$i]}" in
    -B) BUILD_DIR="${args[$((i + 1))]}" ;;
    -B*) BUILD_DIR="${args[$i]#-B}" ;;
  esac
done

# Anything that is not a plain build (set-target, menuconfig, flash, size, a
# build with extra actions) goes to idf.py as-is, still niced and locked.
# The value after -B is a directory, however it is named: `-B build` is the
# most common spelling of the default and must not read as the build action.
is_build=0; skip=0
for a in "${args[@]}"; do
  if [ $skip -eq 1 ]; then skip=0; continue; fi
  case "$a" in -B) skip=1 ;; build) is_build=1 ;; esac
done
if [ $# -eq 0 ]; then is_build=1; args=(build); fi
if [ $is_build -eq 0 ]; then
  exec nice -n 15 ionice -c 3 idf.py "${args[@]}"
fi

# Strip the "build" action, keep every option for reconfigure.
cfg=(); skip=0
for a in "${args[@]}"; do
  if [ $skip -eq 1 ]; then skip=0; cfg+=("$a"); continue; fi
  case "$a" in -B) skip=1; cfg+=("$a") ;; build) ;; *) cfg+=("$a") ;; esac
done
nice -n 15 ionice -c 3 idf.py "${cfg[@]}" reconfigure
exec nice -n 15 ionice -c 3 ninja -C "$BUILD_DIR" -j "$JOBS"

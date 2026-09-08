#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# Build and run every host-test project under test/host/ on the linux target.
#
#   ./test/host/run_all.sh                  # all of them
#   ./test/host/run_all.sh espos_sk_test    # just these
#
# Projects are DISCOVERED, not listed: a new test directory runs in CI the
# moment it exists. The previous arrangement — an explicit chain of `cd &&
# idf.py build && ./x.elf` in the workflow — is how espos_ble_test came to be
# documented as a host test while never actually running in CI.
#
# A project is any test/host/<name>/ with a CMakeLists.txt. After the build it
# runs ./run_test.py when the project has one (the httpd test drives the real
# REST server over HTTP from python), otherwise the Unity ELF from build/.
#
# Every project is attempted even after one fails, so a run reports all the
# broken ones at once; the exit status is non-zero if any failed.
set -uo pipefail

cd "$(dirname "$0")"
here=$(pwd)

# One run at a time, per user. Two concurrent runs share every project's
# build/ directory: they rebuild and delete each other's ELFs, and the losing
# run reports failures and errors that have nothing to do with the code (it
# has already happened twice in this repo, once as "6 failures + 16 errors"
# that were entirely a second run removing the harness binary mid-test).
# This is a separate lock from scripts/build.sh's: a host run and a firmware
# build touch different trees and need not exclude each other.
_lock_dir="${ESPOS_BUILD_LOCK_DIR:-$HOME/.cache}"
mkdir -p "$_lock_dir"
exec 8>"$_lock_dir/.espos-host-tests-$(id -u).lock"
if ! flock -n 8; then
    if [ "${HOST_TESTS_NOWAIT:-0}" = "1" ]; then
        echo "run_all.sh: another host-test run is in progress (HOST_TESTS_NOWAIT=1) -- aborting" >&2
        exit 1
    fi
    echo "==> waiting for the running host-test suite to finish..." >&2
    flock 8
fi

if [ "$#" -gt 0 ]; then
    projects=("$@")
else
    projects=()
    for d in */; do
        [ -f "${d}CMakeLists.txt" ] && projects+=("${d%/}")
    done
fi

if [ "${#projects[@]}" -eq 0 ]; then
    echo "run_all.sh: no host-test projects found in ${here}" >&2
    exit 1
fi

if [ -z "${IDF_PATH:-}" ]; then
    echo "run_all.sh: IDF_PATH is unset — source the pinned ESP-IDF's export.sh first" >&2
    exit 1
fi

# Same compile budget as scripts/build.sh: IDF 6's idf.py has no -j, so the
# parallelism is ninja's, and ninja's default is every core. On the 4-core
# development host that has frozen the machine; half the cores, floor 1.
# Scratch stays out of /tmp for the same reason (it is RAM there).
export TMPDIR="${TMPDIR:-$HOME/dev/tmp}"
mkdir -p "$TMPDIR"
JOBS="${BUILD_JOBS:-$(( $(nproc) / 2 ))}"
[ "$JOBS" -lt 1 ] && JOBS=1

passed=()
failed=()

for name in "${projects[@]}"; do
    if [ ! -f "${here}/${name}/CMakeLists.txt" ]; then
        echo "run_all.sh: no such host-test project: ${name}" >&2
        failed+=("${name} (missing)")
        continue
    fi

    echo "==> ${name}: build"
    cd "${here}/${name}" || { failed+=("${name} (cd)"); continue; }

    # --preview because the linux target is still a preview target in IDF 6.
    if ! nice -n 15 ionice -c 3 idf.py --preview set-target linux >/dev/null; then
        failed+=("${name} (set-target)")
        continue
    fi
    if ! { nice -n 15 ionice -c 3 idf.py reconfigure >/dev/null && nice -n 15 ionice -c 3 ninja -C build -j "$JOBS"; }; then
        failed+=("${name} (build)")
        continue
    fi

    echo "==> ${name}: run"
    if [ -f run_test.py ]; then
        if python3 run_test.py; then passed+=("${name}"); else failed+=("${name} (run_test.py)"); fi
        continue
    fi

    # Unity ELF: build/<project>.elf, but do not assume the project name
    # matches the directory — take whatever single ELF the build produced.
    mapfile -t elves < <(find build -maxdepth 1 -name '*.elf' -type f)
    if [ "${#elves[@]}" -ne 1 ]; then
        echo "run_all.sh: expected exactly one ELF in ${name}/build, found ${#elves[@]}" >&2
        failed+=("${name} (no elf)")
        continue
    fi
    if "${elves[0]}"; then passed+=("${name}"); else failed+=("${name} (tests)"); fi
done

cd "${here}"
echo
echo "host tests: ${#passed[@]} passed, ${#failed[@]} failed"
for p in "${passed[@]:-}"; do [ -n "$p" ] && echo "  ok    $p"; done
for f in "${failed[@]:-}"; do [ -n "$f" ] && echo "  FAIL  $f"; done
[ "${#failed[@]}" -eq 0 ]

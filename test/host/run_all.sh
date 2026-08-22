#!/usr/bin/env bash
# SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
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
    if ! idf.py --preview set-target linux >/dev/null; then
        failed+=("${name} (set-target)")
        continue
    fi
    if ! idf.py build; then
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

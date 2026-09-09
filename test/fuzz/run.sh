#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# Build and run the fuzz harnesses on the host.
#
#   ./test/fuzz/run.sh                 # every harness, 60 s each
#   ./test/fuzz/run.sh sk_frame        # just one
#   FUZZ_SECONDS=600 ./test/fuzz/run.sh
#
# Deliberately NOT an IDF project. These parsers are pure C -- no IDF headers
# beyond esp_err.h, which is why they were written that way -- so building
# them directly with the host compiler keeps the harness to one file and a
# command, and means a contributor needs clang and nothing else.
#
# With clang: libFuzzer, coverage-guided, plus ASan and UBSan.
# With gcc:   no libFuzzer, so the corpus is replayed once under ASan+UBSan.
#             That still catches what a corpus already knows about and keeps
#             the harnesses building on a machine without clang; it does not
#             discover anything new. CI uses clang.
set -uo pipefail

cd "$(dirname "$0")"
here=$(pwd)
root=$(cd ../.. && pwd)

export TMPDIR="${TMPDIR:-$HOME/dev/tmp}"
mkdir -p "$TMPDIR"

SECONDS_PER="${FUZZ_SECONDS:-60}"
out="$here/build"
mkdir -p "$out"

# cJSON: reuse the copy a host-test project already fetched if there is one,
# otherwise fetch the pinned version. Two of the three parsers need it.
cjson=$(find "$root/test/host" -path '*espressif__cjson/cJSON/cJSON.c' 2>/dev/null | head -1)
if [ -z "$cjson" ]; then
    # No host test has been built, so fetch the pinned cJSON on its own. The
    # version is the one components/espos_sk/idf_component.yml pins, upstream
    # rather than Espressif's mirror because this needs no component manager
    # and therefore no IDF checkout -- which is what lets CI run the fuzzers
    # in a job that installs clang and nothing else.
    cjson_ver=1.7.19
    cjson_dir="$TMPDIR/espos-fuzz-cjson/cJSON-$cjson_ver"
    if [ ! -f "$cjson_dir/cJSON.c" ]; then
        echo "==> fetching cJSON $cjson_ver"
        mkdir -p "$TMPDIR/espos-fuzz-cjson"
        if ! curl -fsSL "https://github.com/DaveGamble/cJSON/archive/refs/tags/v$cjson_ver.tar.gz" \
             | tar -xz -C "$TMPDIR/espos-fuzz-cjson"; then
            echo "run.sh: could not fetch cJSON; build a host test first, e.g." >&2
            echo "  cd test/host/espos_sk_test && ../../../scripts/build.sh --preview set-target linux && ../../../scripts/build.sh build" >&2
            exit 1
        fi
    fi
    cjson="$cjson_dir/cJSON.c"
else
    cjson_dir=$(dirname "$cjson")
fi

if command -v clang >/dev/null 2>&1; then
    CC=clang; CXX=clang++
    FUZZ_FLAGS="-fsanitize=fuzzer,address,undefined"
    MODE=libfuzzer
else
    CC=gcc; CXX=g++
    # gcc has no libFuzzer: link a driver that replays the corpus instead.
    FUZZ_FLAGS="-fsanitize=address,undefined"
    MODE=replay
fi
echo "==> $MODE ($CC)"

common=(
    -g -O1 -fno-omit-frame-pointer
    -I"$cjson_dir"
    -I"$root/components/espos_sk/include"
    -I"$root/components/espos_ota/include"
    -I"$root/components/espos_n2k/include"
    -I"$here/shim"
)

# One harness: <name> <source> <extra sources...>
build_one() {
    local name=$1 src=$2; shift 2
    local compiler=$CC
    case "$src" in *.cpp) compiler=$CXX;; esac

    local driver=()
    if [ "$MODE" = replay ]; then
        driver=("$here/replay_main.c")
    fi

    echo "  building $name"
    # shellcheck disable=SC2086
    "$compiler" $FUZZ_FLAGS "${common[@]}" -o "$out/$name" "$src" "$@" "${driver[@]}" 2>&1 | head -20
    [ -x "$out/$name" ]
}

run_one() {
    local name=$1
    local corpus="$here/corpus/$name"
    mkdir -p "$corpus"
    echo "==> $name"
    if [ "$MODE" = libfuzzer ]; then
        ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
            "$out/$name" "$corpus" -max_total_time="$SECONDS_PER" -max_len=8192 \
            -print_final_stats=1 -artifact_prefix="$corpus/crash-" || return 1
    else
        ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
            "$out/$name" "$corpus" || return 1
    fi
}

declare -A SOURCES=(
    [sk_frame]="$here/fuzz_sk_frame.c $root/components/espos_sk/src/sk_parse.c $cjson"
    [ota_manifest]="$here/fuzz_ota_manifest.c $root/components/espos_ota/src/manifest.c $cjson"
    [candump]="$here/fuzz_candump.cpp $root/components/espos_n2k/src/candump_format.cpp"
)

targets=("$@")
if [ ${#targets[@]} -eq 0 ]; then
    targets=(sk_frame ota_manifest candump)
fi

fail=0
for t in "${targets[@]}"; do
    if [ -z "${SOURCES[$t]:-}" ]; then
        echo "run.sh: no such harness '$t' (have: ${!SOURCES[*]})" >&2
        fail=1
        continue
    fi
    # shellcheck disable=SC2086
    if ! build_one "$t" ${SOURCES[$t]}; then
        echo "  FAIL build $t" >&2
        fail=1
        continue
    fi
    if ! run_one "$t"; then
        echo "  FAIL $t" >&2
        fail=1
    fi
done

if [ $fail -ne 0 ]; then
    echo "fuzz: FAILED" >&2
    exit 1
fi
echo "fuzz: ok"

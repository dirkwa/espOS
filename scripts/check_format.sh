#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# What CI's clang-format job checks, runnable before pushing.
#
#   ./scripts/check_format.sh          # report
#   ./scripts/check_format.sh --fix    # reformat in place
#
# The job checks every C/C++ file the branch changed against origin/main, not
# the files you happen to remember editing. That difference has turned three
# pushes red: each time the offender was a file written from scratch, where
# "it looks like the rest of the tree" is exactly the assumption clang-format
# exists to check.
#
# Same diff expression as .github/workflows/ci.yml -- if the two ever disagree
# this script is wrong, not CI.
set -uo pipefail

cd "$(dirname "$0")/.."
export TMPDIR="${TMPDIR:-$HOME/dev/tmp}"

fix=0
[ "${1:-}" = "--fix" ] && fix=1

if ! command -v clang-format >/dev/null 2>&1; then
    echo "check_format.sh: clang-format not on PATH (try: export PATH=\"\$HOME/.local/bin:\$PATH\")" >&2
    exit 1
fi

base="${FORMAT_BASE:-origin/main}"
if ! git rev-parse --verify -q "$base" >/dev/null; then
    echo "check_format.sh: no such ref '$base' -- fetch first, or set FORMAT_BASE" >&2
    exit 1
fi

files=$(git diff --name-only --diff-filter=ACMR "$base...HEAD" -- | grep -E '\.(c|h|cpp|hpp)$' || true)
# Uncommitted work counts too: the point is to catch this before pushing, and
# a file staged but not committed is still going to CI on the next push.
staged=$(git diff --name-only --diff-filter=ACMR HEAD -- | grep -E '\.(c|h|cpp|hpp)$' || true)
files=$(printf '%s\n%s\n' "$files" "$staged" | sort -u | grep -v '^$' || true)

if [ -z "$files" ]; then
    echo "check_format.sh: no C/C++ files changed against $base"
    exit 0
fi

count=$(printf '%s\n' "$files" | wc -l)
if [ "$fix" = 1 ]; then
    printf '%s\n' "$files" | xargs clang-format -i
    echo "check_format.sh: reformatted $count file(s); review the diff"
    exit 0
fi

if printf '%s\n' "$files" | xargs clang-format --dry-run -Werror 2>"$TMPDIR/.fmt.$$"; then
    echo "check_format.sh: $count file(s) checked, all formatted"
    rm -f "$TMPDIR/.fmt.$$"
    exit 0
fi

echo "check_format.sh: not formatted --" >&2
grep -oE '^[^:]+\.(c|h|cpp|hpp)' "$TMPDIR/.fmt.$$" | sort -u | sed 's/^/  /' >&2
echo "run: ./scripts/check_format.sh --fix" >&2
rm -f "$TMPDIR/.fmt.$$"
exit 1

#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# Doxyfile's INPUT is a literal list -- mkdoxy parses the file itself and does
# not expand globs -- so a new component's headers are simply absent from the
# API reference, with nothing failing. This compares the list to the tree.
set -euo pipefail
cd "$(dirname "$0")/.."
tree=$(printf '%s\n' components/*/include | sort)
fail=0
check() {   # $1 = file, $2 = human name
    local listed missing
    listed=$(grep -oE 'components/[a-z0-9_]+/include' "$1" | sort -u)
    missing=$(comm -23 <(echo "$tree") <(echo "$listed"))
    if [ -n "$missing" ]; then
        echo "$2 is missing:" >&2
        echo "$missing" >&2
        fail=1
    fi
}
# Both matter, and mkdoxy's src-dirs is the one that decides what the site
# shows: it overrides the Doxyfile's INPUT. A component missing from either
# is simply absent from the API reference, with nothing failing.
check Doxyfile "Doxyfile INPUT"
check mkdocs.yml "mkdocs.yml mkdoxy src-dirs"
if [ "$fail" -ne 0 ]; then
    echo "Add them, and give each new header a row in docs/api-c.md." >&2
    exit 1
fi
echo "check_doxyfile_inputs.sh: all $(echo "$tree" | wc -l) component include dirs are in Doxyfile and mkdocs.yml"

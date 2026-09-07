#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# Fail when something that must never be committed is tracked by git.
#
#   scripts/check_no_secrets.sh      # exit 0 when clean, 1 with the offending paths
#
# .gitignore keeps these out of `git add .`, but an ignore is advice, not a
# gate: `git add -f`, a rename, or a file that predates the rule all get
# through, and a signing key or a provisioning image with WiFi passwords in
# it is public the moment it is pushed. So this looks at the index -- what a
# commit would contain -- not at the working tree.
#
# The patterns mirror .gitignore: *.pem (the app-signing key and any other
# key), *.nvs.csv / *.nvs.bin (factory-provisioning inputs and images),
# *.secret, and anything under a provisioning/ or secrets/ directory at any
# depth.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

pattern='(^|/)(provisioning|secrets)/|\.(pem|nvs\.csv|nvs\.bin|secret)$'

# -z on both sides: paths are NUL-separated, so a newline in a file name
# cannot split a match in two or hide it.
hits=$(git ls-files -z | grep -zE "$pattern" | tr '\0' '\n' || true)

if [ -n "$hits" ]; then
    echo "check_no_secrets.sh: files that must not be committed are tracked:" >&2
    while IFS= read -r f; do
        [ -n "$f" ] && echo "  $f" >&2
    done <<< "$hits"
    echo "Remove them from the index (git rm --cached <file>) and, if they were" >&2
    echo "ever pushed, treat the secret as leaked: rotate it." >&2
    exit 1
fi

echo "check_no_secrets.sh: ok, no key, provisioning or secret files are tracked"

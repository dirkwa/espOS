#!/usr/bin/env bash
# SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
#
# Cut an espOS release: bump version.txt, commit, and tag.
#
#   scripts/release.sh 0.7.0
#   scripts/release.sh 0.7.0 --dry-run
#
# It does NOT push. Review `git show` and `git tag -n99 v<version>`, then push
# the branch and the tag yourself.
#
# Why a script for two commands: a tag and version.txt that disagree make a
# device report a version that matches no release, and the build only warns
# about it. Doing both from one place is the cheapest way to keep them equal.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

usage() { echo "usage: scripts/release.sh <major.minor.patch> [--dry-run]" >&2; exit 2; }

version="${1:-}"
dry_run=""
[ "$#" -ge 1 ] || usage
[ "$#" -le 2 ] || usage
if [ "$#" -eq 2 ]; then
    [ "$2" = "--dry-run" ] || usage
    dry_run=1
fi

if ! printf '%s' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
    echo "release.sh: '$version' is not major.minor.patch" >&2
    exit 1
fi

tag="v$version"

if git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
    echo "release.sh: $tag already exists" >&2
    exit 1
fi

# A release cut from a dirty tree is a release nobody can reproduce.
if [ -n "$(git status --porcelain)" ]; then
    echo "release.sh: working tree is not clean" >&2
    git status --short >&2
    exit 1
fi

current=$(git rev-parse --abbrev-ref HEAD)
echo "release.sh: $current at $(git rev-parse --short HEAD) → $tag"

previous=$(git describe --tags --abbrev=0 2>/dev/null || true)
if [ -n "$previous" ]; then
    echo
    echo "Changes since $previous:"
    git log --no-merges --pretty='  %s' "$previous..HEAD"
    echo
fi

if [ -n "$dry_run" ]; then
    echo "release.sh: --dry-run, stopping before the commit"
    exit 0
fi

printf '%s\n' "$version" > version.txt
git add version.txt
git commit -q -m "chore: release $version"

# Annotated, not lightweight: an annotated tag carries who cut it and when,
# and is what `git describe` prefers.
if [ -n "$previous" ]; then
    git tag -a "$tag" -m "espOS $version" -m "$(git log --no-merges --pretty='- %s' "$previous..HEAD^")"
else
    git tag -a "$tag" -m "espOS $version"
fi

echo
echo "release.sh: tagged $tag at $(git rev-parse --short HEAD)"
echo "Review, then push both:  git push origin $current && git push origin $tag"
echo "Consumers: bump their espos submodule to this commit and say so —"
echo "  git -C espos fetch --tags && git -C espos checkout $tag"
echo "  git commit -m 'chore: bump espos to $tag'"

# Releasing

espOS is consumed as a git submodule (`espos/` in a firmware project), so a
"release" is a tag other repositories can point at, and `version.txt` is what
the device reports.

## Cutting one

```sh
scripts/release.sh 0.7.0 --dry-run    # shows what would change
scripts/release.sh 0.7.0              # bumps version.txt, commits, tags
git push origin main && git push origin v0.7.0
```

The script refuses a dirty tree, refuses a tag that exists, and writes an
*annotated* tag — `git describe` prefers annotated tags, and the firmware's
reported version comes from `git describe` (below). The tag message carries
the commit subjects since the previous tag, or the whole history when there
is no previous tag.

Then publish the tag on GitHub, so the Releases tab answers "what is the
current version, and what changed" for anyone who is not already a consumer:

```sh
gh release create v0.7.0 --title "espOS 0.7.0" --notes "..."
```

There are no binaries to attach — espOS is source consumed as a submodule,
and the tag remains the deliverable. The release is a readable front page for
it, not a separate artifact. Lead the notes with anything that requires a
consumer to change its own code.

## What a device reports

`espos_project_prologue()` sets `PROJECT_VER` from `git describe --tags
--dirty --always`, falling back to `version.txt` when the checkout has no
tags (a tarball, or a release that has not been tagged yet). Firmwares built
on espOS get the same treatment for their own version, since they call the
same prologue. So:

| Build | `GET /api/v1/system/info` reports |
|---|---|
| the tagged commit | `0.7.0` |
| three commits later | `0.7.0-3-gabc1234` |
| with uncommitted changes | `0.7.0-3-gabc1234-dirty` |
| no tags at all | `0.7.0` (from version.txt) |

That distinction is the whole point of tagging. Without it every build
between two releases reports the same number, and "which firmware is on that
box" has no answer short of comparing binaries. The build warns when the
nearest tag and `version.txt` disagree.

## Consumers

A firmware project pins espOS by submodule commit. Bump it to a *tag*, and
say which one:

```sh
git -C espos fetch --tags
git -C espos checkout v0.7.0
git commit -am "chore: bump espos to v0.7.0"
```

The submodule still records a SHA — that is how submodules work — but the
commit message makes the release readable in `git log`, and
`git -C espos describe --tags` on any checkout then answers which espOS is
in it. A bump commit that says `bump espos to c6fd455` answers nothing
without a second repository to hand.

Consumers version themselves independently; espOS's version is not theirs.

## Versioning

Semantic-ish, judged against what a *consumer firmware* sees:

* **patch** — fixes, docs, internal changes. A consumer bumps and rebuilds.
* **minor** — new components, new config keys, new API endpoints. Additive:
  a consumer bumps and rebuilds, and may then use the new thing.
* **major** — a consumer has to change its own code: a removed or renamed
  public function, a changed struct field, a config key that no longer
  exists, an `/api/v1` change.

espOS is pre-1.0, so minor is doing the work major will do later. Say plainly
in the release notes when a bump requires consumer changes — that is the
number people actually need.

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
gh release create v0.7.0 --title "espOS 0.7.0" --generate-notes
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

## Registry publishing

Every `components/espos_*` directory is also a component on the [Espressif
Component Registry](https://components.espressif.com), under the
`signalk-espos` namespace: `signalk-espos/espos_config`, `signalk-espos/espos_sk`, and so on. A firmware
that does not want the submodule adds what it needs and the component
manager pulls the rest:

```sh
idf.py add-dependency "signalk-espos/espos_sk^0.7"
```

`espos_sk`'s manifest names `espos_config`, `espos_httpd`, `espos_wifi` and
`espos_health` as dependencies, so that one line installs the core. The
registry names each download `signalk-espos__<name>` in the build; a component's
own `REQUIRES espos_config` still resolves, because the component manager
maps a short name onto the namespaced component when only that one exists.

### One version for everything

All eleven manifests carry `version:` equal to `version.txt`, and every
dependency between espOS components is `^<that version>` with an
`override_path` to the sibling directory. The `override_path` is what an
in-tree build — and a firmware that vendors espOS as a submodule — uses: the
manager takes the checkout next to the manifest and never asks the registry
for an espOS component. The version range is what a registry consumer sees,
and lockstep versions keep it from ever mixing two espOS releases in one
firmware.

Lockstep has to be maintained by the release, not by hand. `scripts/release.sh`
must, for every `components/*/idf_component.yml`:

* set the top-level `version:` to the release version;
* set each `signalk-espos/espos_*` dependency's `version:` to `^<release version>`
  (pre-1.0, `^0.7.0` excludes `0.8.0`, so a minor bump that leaves the
  ranges behind publishes components that cannot be installed together);
* `git add` the manifests with `version.txt`, so the release commit carries
  all twelve files.

The manifests are the registry's contract; a manifest that fails to pack
fails the release. CI runs `compote component pack` for every component on
each pull request, and the tag check that compares the tag to `version.txt`
covers the manifests as well.

### Publishing a release

`.github/workflows/publish.yml` runs on the `v*` tag. It re-checks that the
tag, `version.txt` and every manifest agree — a registry version is
immutable, and that check is worth repeating rather than assuming CI's copy
ran — then uploads each component with `compote component upload`, taking
the registry token from the `IDF_COMPONENT_API_TOKEN` repository secret.
(The component manager reads it from the environment under that exact name;
there is no `--token` flag.)

**Before the first publish, the `signalk-espos` namespace has to exist on the
registry.** It does not yet: a dry run today ends with

```
ERROR: Namespace "signalk-espos" not found
```

which is the same message a nonsense namespace produces, so it is a
registration gap rather than an authentication one. Claim the namespace at
[components.espressif.com](https://components.espressif.com) with the account
that owns the token. Everything else is ready — the archive packs and the
manifests validate up to that point.

Upload order is computed, not written down: the registry resolves a
component's dependencies when it accepts the upload, so a component must not
arrive before the ones it names. `scripts/registry_order.py` topologically
sorts the manifests, and `--check` fails the release *before* anything is
uploaded, since half the components published is the one state that cannot be
rolled back. This paragraph used to carry a hand-written list of eleven
components while the tree had nineteen; every component added after it was
written was missing from it.

A registry version is immutable; `compote component upload --dry-run` (needs
the token) validates without creating one, and is the right rehearsal for a
first publish or a manifest change. A published version that turns out wrong
is yanked with a message, never deleted, and fixed by the next patch release.

What the registry ships is the packed archive: the component directory
minus `build/`, `sdkconfig*`, `managed_components/`, `dependencies.lock`
and the manager's own defaults (`.git`, `__pycache__`, ...). Anything a
component needs at build time — `espos_config`'s generator under `tools/`,
`espos_httpd`'s `www/index.html`, the `config/*.json` descriptors — lives
inside the component directory for exactly this reason.

## The template repository

[signalk-espOS/espos-template](https://github.com/signalk-espOS/espos-template)
is generated, never edited by hand: `scripts/sync_template.sh <checkout> <tag>`
copies `components/espos_core/examples/minimal` into a checkout of the template
with espOS as the `espos/` submodule, and the release then bumps that
submodule to the tag and commits ("chore: sync to espOS vX.Y.Z"). Change the
example, not the template; CI builds the example on every target, the
template's own CI builds the copy once it is pushed.

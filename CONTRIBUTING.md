# Contributing

espOS is the plumbing every SignalK ESP32 device needs — WiFi, config, web
UI, SignalK, OTA and the optional gateways — consumed as a git submodule by
firmware projects. This is what you need to know to change it. The build and
test details live in [docs/development.md](docs/development.md); this file
is about how a change gets in.

## Where code goes

* **Framework fixes belong in espOS, not in consumer repos.** If a firmware
  built on espOS needs to patch, copy or work around something under
  `components/espos_*`, that is a bug or a missing hook here: fix it here,
  then bump the submodule in the consumer
  ([docs/releasing.md](docs/releasing.md)). A workaround that lives in a
  consumer is a fix nobody else gets and a merge conflict on the next bump.
* Board-specific code — display HALs, audio codecs, pin maps — stays in the
  firmware that knows the board. `espos_audio::AudioDriver` is the pattern:
  espOS publishes the contract, the board implements it.
* One component per concern (`components/espos_<name>/`), each with
  `include/` for the public API, `src/`, `config/` descriptors where it has
  settings, and a host-test project under `test/host/espos_<name>_test/`.
  Look at `espos_health` for the smallest complete example.
* `main/` is the example application, not a place for framework code.

## Building

Build through the wrapper, never bare `idf.py build`:

```sh
. $IDF_PATH/export.sh                    # the version in .idf-version, exactly
scripts/build.sh                         # default target, build/
scripts/build.sh -B build-esp32c6 -DSDKCONFIG=build-esp32c6/sdkconfig -DIDF_TARGET=esp32c6 build
```

`scripts/build.sh` caps the compile at half the cores and holds one lock per
machine, because two IDF builds at once (or one at full parallelism) freeze a
small host. Anything that is not a build — `set-target`, `menuconfig`,
`flash`, `size` — passes through it to `idf.py` unchanged. Consumers call
the same script through their submodule (`espos/scripts/build.sh`).

The IDF version is pinned in `.idf-version` and the top-level CMake refuses
another one. Do not "fix" a build by bumping the pin in passing; that is its
own change with its own testing on every target.

## Host tests

Everything with logic in it runs on the linux target without hardware:

```sh
test/host/run_all.sh                    # every project under test/host/
test/host/run_all.sh espos_sk_test      # one of them
```

Projects are discovered, not listed: a new `test/host/<name>/` with a
`CMakeLists.txt` runs in CI the moment it exists. When to add or extend one:

* a **state machine, parser, wire format or store** gets Unity tests in the
  component's test project — every transition, every malformed input you
  can think of;
* **REST behaviour** gets a case in `test/host/espos_httpd_test/run_test.py`,
  which drives the real server over HTTP;
* a **new component** gets a new test project, and a `port_sim.c` (or the
  C++ equivalent) where it talks to hardware, so the logic above the port is
  testable — `espos_wifi` and `espos_ota` show the split.

CI has no boards. Whatever is not covered by a host test is tested only by
the person with the hardware — say in the PR which board that was.

## Commits and pull requests

* **Conventional Commits**: `type(scope): subject`, imperative, lower case,
  no trailing period. Types: `feat`, `fix`, `perf`, `refactor`, `docs`,
  `test`, `build`, `ci`, `chore`. The scope is the component without the
  `espos_` prefix (`fix(wifi): …`, `feat(sk): …`), or `ui`, `docs`, `build`,
  `release`. A change a consumer has to react to says so in the body and
  gets the `breaking` label. Squash-merge PRs so the subject is the merged
  commit; `scripts/release.sh` turns those subjects into the tag notes and
  `.github/release.yml` sorts the PRs into the release page by label.
* **Sign off every commit** (`git commit -s`). The `Signed-off-by` line is
  your statement under the [Developer Certificate of
  Origin](https://developercertificate.org) that you may contribute the
  change under Apache-2.0. There is no CLA.
* **Docs change with the code**, in the same PR. `docs/rest-api.md` is a
  contract — an endpoint or field change is discussed before it is written.
  A new config key is documented in `docs/config.md`, a new Kconfig option
  in the component's doc. User-visible changes get a line under
  `Unreleased` in `CHANGELOG.md`.
* **Ask before adding a registry dependency.** Open an issue or a Discussion
  before a new entry in any `idf_component.yml` (or `ui/package.json`); the
  manifests explain why each existing dependency is there, and a new one
  does the same. Components pin through version ranges, the application
  pins exactly through the committed `dependencies.lock`.
* Small, reviewable PRs. The template asks what changed and why, which host
  tests ran, which hardware it was tried on, and which docs moved — answer
  all four.

## Style

* C: C11, 4-space indent, `espos_` prefix on everything public,
  `esp_err_t` returns, comments that explain *why*. The root `.clang-format`
  describes the existing code; run `clang-format -i` on the files you touch
  and nothing else — CI checks only the files a PR changes, and a
  reformatting commit hides the change it travels with.
* C++ (`espos_n2k`, `espos_voice`, `espos_audio`): Google style, 2-space
  indent, their own `.clang-format`. Do not mix the two in one component.
* `.clang-tidy` is advisory (nothing is an error); worth a run on new code.
* Every new file carries the two SPDX header lines (copyright and license
  identifier, copied from any neighbouring file); `reuse lint` must stay
  clean. Files that cannot carry a header are listed in `REUSE.toml`.
* Warnings are errors (IDF 6 default); keep every target's build clean, not
  just the one on your desk.

## The web UI

`ui/` is a Preact + Vite app served gzipped from the LittleFS partition
([docs/ui.md](docs/ui.md)):

```sh
cd ui && npm ci
npm run dev                              # Vite + ui/mock/server.mjs, no device needed
ESPOS_API=http://<device-ip> npm run dev # against a real device or the host harness
npm run mock                             # the API mock alone, on :8484
npm run build                            # → dist-gz/, packed into storage.bin by the firmware build
```

`ui/mock/server.mjs` implements `docs/rest-api.md` well enough to exercise
every page and has no dependencies. When the contract changes, the mock
changes in the same PR — otherwise the UI is developed against an API that
no device has.

## Nothing machine-specific in committed files

No absolute paths, no `sdkconfig` (only `sdkconfig.d/*.defaults*`), no serial
port names, no local IP addresses or hostnames, no keys, no provisioning
images. `scripts/check_no_secrets.sh` fails CI if a key or a provisioning
file is tracked; `.gitignore` already keeps them out of `git add .`, the
script is there for `git add -f` and renames. A machine-specific value that
a build needs goes into an environment variable or a git-ignored file, and
the docs say which.

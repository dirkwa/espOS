<!--
Title as a Conventional Commit: `fix(wifi): ...`, `feat(sk): ...`, `docs: ...`
(types and scopes in CONTRIBUTING.md). Add the type as a label too -- the
release notes are sorted by label (.github/release.yml).
-->

## What and why

<!-- What changes for a device or a consumer firmware, and what made it necessary. Link the issue. -->

## Host tests run

<!-- Paste the summary line of `test/host/run_all.sh` (or the projects you ran).
     A new state machine / parser / wire format / REST behaviour comes with a test; say where it is. -->

- [ ] `test/host/run_all.sh` passes
- [ ] new or changed host test: <!-- test/host/<project>/... -- or "none needed, because ..." -->

## Hardware tested on

<!-- target + board + IDF version, and what you exercised; or "host-only change". -->

## Docs

- [ ] `docs/` updated where behaviour changed (`docs/rest-api.md` is a contract)
- [ ] `CHANGELOG.md` has an entry under Unreleased (user-visible changes only)
- [ ] new files carry the SPDX header (`reuse lint`), commits are signed off (`git commit -s`)

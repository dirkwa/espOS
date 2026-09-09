# Fuzzing the network parsers

```sh
./test/fuzz/run.sh                 # every harness, 60 s each
./test/fuzz/run.sh candump         # just one
FUZZ_SECONDS=600 ./test/fuzz/run.sh
```

Needs `clang` for the real thing. With only `gcc` the harnesses still build
and replay the corpus under ASan and UBSan, which is a useful regression check
after a parser change but discovers nothing new.

## Why these three

espOS is C, and the [language decision](../../docs/decisions.md) that settled
on C rather than Rust is only defensible if the code a hostile input reaches is
exercised by something that does not share the author's assumptions. Fuzzing
plus sanitizers is that something. These are the parsers a hostile input
reaches:

| Harness | Parser | Reachable from |
|---|---|---|
| `sk_frame` | `espos_sk_frame_parse()` | every text frame the Signal K server sends, on a socket the device opened itself |
| `ota_manifest` | `espos_ota_manifest_pick()`, `espos_ota_resolve_url()` | the JSON that decides which firmware to install next |
| `candump` | `candump_decode()`, `candump_resync_offset()` | any TCP peer that connects to the NMEA 2000 gateway |

All three are pure C or C++ with no ESP-IDF dependency beyond `esp_err.h` —
which is *why* they were written that way — so the harnesses build them
directly with the host compiler. There is no IDF project here, no component
manager and no `sdkconfig`.

## What the harnesses check beyond "did it crash"

A crash is the easy case. Each harness also asserts the contract, because the
failures that matter on a boat are quieter than a crash:

- **`sk_frame`** parses and then frees, twice. A leak on a malformed frame is
  a device that dies in a week and looks like a memory problem rather than a
  parsing one. The callback reads every string it is handed, so a pointer that
  outlived its arena is a use-after-free the sanitizer can see.
- **`ota_manifest`** poisons the output struct and checks every fixed-size
  field is NUL-terminated *within its bound* on success. An unterminated
  truncation is not a crash, so a sanitizer alone would miss it.
- **`candump`** checks a decoded frame's length is in range, that re-encoding
  it round-trips, and that `resync_offset` never rewinds, never runs past the
  end, and never resumes mid-line — at every possible cut of the buffer.

## Two bugs this found

Both were reachable from the network and neither showed up in review or in the
unit tests, which only ever ask the questions someone thought to ask.

**A heap overread in `candump_decode`** (ASan). The data loop advanced two
characters per byte, but `sscanf("%2x")` succeeds on a *single* hex digit — it
reads a trailing `"A"` as `0x0A` and reports success. A line ending on an odd
digit therefore stepped the pointer over the NUL and kept reading. Fixed by
requiring both nibbles explicitly, which also removed the `sscanf`.

**A signed overflow in the timestamp** (UBSan). `sec * 1000000` overflows
`int64` for a large enough seconds field, and signed overflow is undefined
behaviour, not merely a wrong time. A client is free to send
`(12345678903456.0)`. Both halves are now clamped.

Both have unit tests in `test/host/espos_n2k_test` as well, so they fail fast
and locally rather than only under a fuzzer.

## Corpus

`corpus/<harness>/` holds seeds — real frames, real manifests, real candump
lines. They are committed because a good seed is the difference between a
fuzzer finding something in seconds and finding it in hours: it has to reach
valid-looking input before mutation is interesting.

libFuzzer writes anything new it discovers back into the same directory. A
crash lands as `crash-*` next to the input that produced it; CI uploads those
as an artifact, so a red fuzz job comes with a reproducer rather than only a
stack trace.

The first byte of an `ota_manifest` input selects the caller's context (which
app, target, channel and running version), so one corpus exercises all of
them.

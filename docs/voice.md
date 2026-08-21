# Voice satellite

`espos_voice` is a [Wyoming-protocol](https://github.com/rhasspy/wyoming)
satellite: a TCP server (default port 10700) that lets a
signalk-wyoming or Home Assistant orchestrator play TTS to the device
and stream microphone audio back, with on-device wake-word detection via
esp-sr WakeNet.

espOS supplies the protocol, the server and the wake engine. It does not
supply an audio codec: the application implements
`espos_audio::AudioDriver` for whatever the board has and passes it in.

```cpp
#include "espos_audio/audio_driver.h"
#include "espos_voice/wyoming_satellite.h"

static MyBoardAudio audio;          // : public espos_audio::AudioDriver
static espos_voice::WyomingSatellite sat(&audio, cfg);
audio.init();
sat.start();
```

## espos_audio

A header-only contract, deliberately minimal: `init()`, `ready()`,
`sample_rate()`, `play_pcm()`, plus optional capture and mic-probe hooks.
`espos_audio::NullAudio` is a no-op implementation for boards with no
audio, so callers never need a null check.

`play_pcm()` is non-blocking by contract — it copies and enqueues — so it
is safe to call from a UI or render task. A driver that blocks there will
stall the display.

## Targets

esp-sr supports only `esp32s3` and `esp32p4`. On every other target
`espos_voice` registers as an empty component rather than failing the
build, so a project can depend on it unconditionally and simply have no
voice where the silicon cannot run WakeNet.

## Wake word

The satellite owns a `WakeEngine` directly, so on-device wake is not
optional today. Detection pauses the engine for the duration of an
utterance and re-arms it afterwards; see the note in `wake_engine.cpp`
about restoring the shared I2S capture clock after playback, which is
what stops WakeNet going deaf after the first TTS reply.

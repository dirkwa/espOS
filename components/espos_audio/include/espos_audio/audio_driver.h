/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution */
#pragma once

#include <cstddef>
#include <cstdint>

namespace espos_audio {

/// Abstract mono audio-out and mic-in device: one interface, one
/// implementation per board. The application supplies the driver (an
/// ES8311 codec, an I2S DAC, whatever the board has); espOS components
/// take it by pointer and never assume a particular codec.
///
/// play_pcm() is non-blocking by contract, so it is safe to call from a
/// UI or render task. espos_voice uses this same interface for both TTS
/// playback and mic capture.
class AudioDriver {
 public:
  virtual ~AudioDriver() = default;

  virtual void init() = 0;

  /// True once init() brought the codec up successfully. False if the
  /// board has no audio or init failed (play_pcm then no-ops). Lets a
  /// status endpoint report audio health without racing the boot log.
  virtual bool ready() const = 0;

  /// Sample rate the driver expects play_pcm() buffers to be in.
  /// Callers synthesise / resample to this. 16 kHz is plenty for
  /// alert tones and speech.
  virtual uint32_t sample_rate() const = 0;

  /// Enqueue `frames` signed-16-bit mono samples for playback. Copies
  /// the buffer and returns immediately; the samples play on the
  /// driver's audio task. Passing a null buffer or zero frames is a
  /// no-op. If the queue is full the buffer is dropped (a chime is
  /// disposable — never block the caller waiting for audio).
  virtual void play_pcm(const int16_t* samples, size_t frames) = 0;

  /// Output volume 0-100. Applied at the codec. Default no-op.
  virtual void set_volume(uint8_t /*pct*/) {}

  /// Mute / unmute without tearing down the codec. When muted the
  /// power amplifier is held disabled so a quiet helm stays quiet.
  /// Default no-op.
  virtual void set_enabled(bool /*on*/) {}

  // --- Streaming playback (voice / TTS) -----------------------------------
  //
  // A continuous stream, unlike play_pcm()'s disposable fixed clips: no
  // length cap, no per-chunk silence tail (that would gap the audio), and
  // write_stream() BLOCKS to apply backpressure instead of dropping. The
  // three calls bracket one audio-start / audio-chunk* / audio-stop span.
  //
  // Default no-ops so speaker-less boards still compile.

  /// Begin a stream at `rate` Hz / `bits` per sample / `channels`. The
  /// driver may reconfigure the codec to match `rate`. Returns false if
  /// the format can't be played (caller should abandon the stream).
  virtual bool begin_stream(uint32_t /*rate*/, uint8_t /*bits*/,
                            uint8_t /*channels*/) {
    return false;
  }

  /// Write `frames` signed-16-bit mono samples into the active stream.
  /// BLOCKS until the codec accepts them (this is the flow control that
  /// paces the sender). Returns frames written, 0 if no stream is active.
  virtual size_t write_stream(const int16_t* /*samples*/, size_t /*frames*/) {
    return 0;
  }

  /// End the active stream: drain the codec so the last chunk doesn't loop
  /// on the DMA ring, then leave the codec open for the next stream/clip.
  virtual void end_stream() {}

  // --- Microphone capture (voice-in) --------------------------------------
  //
  // Default: no capture. A board with a wired mic overrides these.

  /// True if this board can capture (a mic is wired to the codec ADC and
  /// init brought it up). record_pcm() no-ops on a capture-less board.
  virtual bool can_capture() const { return false; }

  /// Sample rate of record_pcm() buffers. 16 kHz matches Whisper's native
  /// rate, so a Wyoming mic stream needs no resampling.
  virtual uint32_t capture_rate() const { return 16000; }

  /// Read up to `max_frames` signed-16-bit mono samples into `out`,
  /// BLOCKING until that many are captured (or a codec error). Returns the
  /// number of frames read (0 on error / no capture). Call in a loop from a
  /// dedicated task while streaming mic audio.
  virtual size_t record_pcm(int16_t* /*out*/, size_t /*max_frames*/) {
    return 0;
  }

  /// Start / stop the ADC capture path. start_capture() must be called
  /// before record_pcm(); stop_capture() releases the ADC when voice-in
  /// ends so idle draws no capture bandwidth. Idempotent. Default no-op.
  virtual void start_capture() {}
  virtual void stop_capture() {}

  // --- Dual-mic capture (on-device wake feed only) ------------------------
  //
  // The Waveshare panel has TWO live mics (ES7210 MIC1 + MIC2). The mono
  // record_pcm() path above feeds STT/PTT one mic; esp-sr's AFE can take BOTH
  // (format "MM") for noise suppression / beamforming, which lifts far-field
  // wake SNR. These give the wake engine a SEPARATE 2-channel capture handle so
  // the mono STT path stays byte-identical. The two handles open the SAME I2S
  // RX, so they must never be open at once — the wake engine pauses (releasing
  // the 2ch handle) before the STT pipeline runs, so they don't overlap.
  //
  // Default: unsupported (single-mic boards keep the mono "M" AFE).

  /// True if this board exposes a 2-channel [MIC1,MIC2] capture path
  /// (start_capture2/record_pcm2/stop_capture2). When false the wake engine
  /// stays on the single-mic mono feed. Default false.
  virtual bool supports_dual_mic() const { return false; }

  /// Open / close the 2-channel capture handle. Same refcounted, idempotent
  /// contract as start_capture()/stop_capture(), on an independent handle.
  /// Default no-op.
  virtual void start_capture2() {}
  virtual void stop_capture2() {}

  /// Read up to `max_frames` 2-channel interleaved [MIC1,MIC2] int16 frames
  /// into `out` (so `out` holds `2 * max_frames` samples), BLOCKING until that
  /// many frames are captured (or a codec error). Returns the number of FRAMES
  /// read (0 on error / unsupported). start_capture2() must precede it.
  virtual size_t record_pcm2(int16_t* /*out*/, size_t /*max_frames*/) {
    return 0;
  }
  /// Analog mic preamp gain in dB, applied on the next capture open. Boards
  /// without a settable PGA ignore it. Exposed because the right value is an
  /// EMPIRICAL trade-off: too low and the wake detector gets nothing, too high
  /// and the preamp compresses/colours the audio until a wake model scores it
  /// the same as silence. Sweep it, listen, then pick.
  virtual void set_mic_gain_db(float /*db*/) {}
  virtual float mic_gain_db() const { return 0.0f; }

  // --- Diagnostic: per-input mic level probe --------------------------------
  //
  // Which physical ADC input each mic is wired to isn't documented for this
  // board, so this measures all four ES7210 inputs to find the live mic(s).
  // Levels for MIC1..MIC4 (index 0..3): RMS and peak magnitude of a short
  // capture. A live mic tracks speech; an unpopulated input flatlines near the
  // noise floor. Returns false if the board has no probe path. Must NOT be
  // called while normal capture is running (it re-opens the ADC).
  struct MicLevels {
    uint16_t rms[4] = {0, 0, 0, 0};
    uint16_t peak[4] = {0, 0, 0, 0};
  };
  virtual bool probe_mic_channels(MicLevels& /*out*/) { return false; }
};

}  // namespace espos_audio

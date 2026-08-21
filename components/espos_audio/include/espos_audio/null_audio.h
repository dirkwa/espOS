/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * AudioDriver that does nothing: keeps the chime/voice controls compiling
 * and honest ("no audio") until the ES7210/ES8311 HAL is ported (phase 2).
 */
#pragma once

#include "espos_audio/audio_driver.h"

namespace espos_audio {

class NullAudio : public AudioDriver {
 public:
  void init() override {}
  bool ready() const override { return false; }
  uint32_t sample_rate() const override { return 22050; }
  void play_pcm(const int16_t*, size_t) override {}
};

}  // namespace espos_audio

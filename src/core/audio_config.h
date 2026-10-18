#pragma once

#include <cstdint>

namespace anc {

// Target sample rate for the internal (mono float) DSP path.
inline constexpr uint32_t kPreferredSampleRate = 48000;

struct StreamConfig {
    uint32_t sampleRate = kPreferredSampleRate;
    uint16_t channels = 1;
    uint16_t bitsPerSample = 32;
    bool isFloat = true;
};

}  // namespace anc

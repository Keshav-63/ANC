#include "dsp/pass_through.h"

#include <cmath>

namespace anc {

void PassThrough::prepare(double /*sampleRate*/, uint32_t /*maxFrames*/) {}

void PassThrough::process(const float* in, float* out, uint32_t frames) {
    if (muted_.load(std::memory_order_relaxed)) {
        for (uint32_t i = 0; i < frames; ++i) out[i] = 0.0f;
        return;
    }
    const float sign = invert_.load(std::memory_order_relaxed) ? -1.0f : 1.0f;
    const float gain = gain_.load(std::memory_order_relaxed) * sign;
    uint64_t clips = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        const float v = in[i] * gain;
        if (std::fabs(v) > 1.0f) ++clips;
        out[i] = v;
    }
    if (clips) clipFrames_.fetch_add(clips, std::memory_order_relaxed);
}

}  // namespace anc

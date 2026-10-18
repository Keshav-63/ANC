#pragma once

#include <atomic>

#include "core/audio_processor.h"

namespace anc {

// Baseline processor for the Phase 1 pipeline: mono gain, optional polarity
// inversion and an explicit mute. Parameters are atomics so a non-real-time
// thread can adjust them without locks.
class PassThrough : public AudioProcessor {
public:
    void prepare(double sampleRate, uint32_t maxFrames) override;
    void process(const float* in, float* out, uint32_t frames) override;
    const char* name() const override { return "pass-through"; }

    void setGain(float gain) { gain_.store(gain, std::memory_order_relaxed); }
    float gain() const { return gain_.load(std::memory_order_relaxed); }
    void setInvert(bool invert) { invert_.store(invert, std::memory_order_relaxed); }
    void setMuted(bool muted) { muted_.store(muted, std::memory_order_relaxed); }
    bool muted() const { return muted_.load(std::memory_order_relaxed); }

    uint64_t clipFrames() const { return clipFrames_.load(std::memory_order_relaxed); }
    void resetClipCounter() { clipFrames_.store(0, std::memory_order_relaxed); }

private:
    std::atomic<float> gain_{1.0f};
    std::atomic<bool> invert_{false};
    std::atomic<bool> muted_{false};
    std::atomic<uint64_t> clipFrames_{0};
};

}  // namespace anc

#pragma once

#include <atomic>
#include <cstdint>

namespace anc {

// Phase-continuous sine generator with a click-free amplitude ramp.
//
// The phase accumulator is preserved across frequency changes so that no
// waveform discontinuities are introduced. Amplitude changes are smoothed
// over `rampSamples` samples. Not a processor by itself; used by other
// processors and by tests.
class ToneGenerator {
public:
    void prepare(double sampleRate, double rampSeconds = 0.02);

    // Real-time safe parameter updates (audio thread reads atomics).
    void setFrequency(double hz) { frequencyHz_.store(hz, std::memory_order_relaxed); }
    void setAmplitude(float linear) { targetAmplitude_.store(linear, std::memory_order_relaxed); }
    void setTargetAmplitudeImmediate(float linear) {
        targetAmplitude_.store(linear, std::memory_order_relaxed);
        currentAmplitude_ = linear;
    }

    // Per-sample generation (mono).
    float next();

    // Block generation: adds/overwrites `dst` with the tone.
    void render(float* dst, uint32_t frames, bool add = false);

    double phase() const { return phase_; }
    void resetPhase(double phaseRadians = 0.0) { phase_ = phaseRadians; }

private:
    double sampleRate_ = 48000.0;
    double phase_ = 0.0;  // radians [0, 2pi)
    double rampStep_ = 1.0 / 960.0;
    float currentAmplitude_ = 0.0f;
    std::atomic<double> frequencyHz_{0.0};
    std::atomic<float> targetAmplitude_{0.0f};
};

}  // namespace anc

#include "dsp/tone_generator.h"

#include <cmath>

namespace anc {

namespace {
constexpr double kTwoPi = 6.283185307179586476925286766559;
}

void ToneGenerator::prepare(double sampleRate, double rampSeconds) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    const double rampSamples = rampSeconds * sampleRate_;
    rampStep_ = rampSamples > 0.0 ? 1.0 / rampSamples : 1.0;
}

float ToneGenerator::next() {
    const float target = targetAmplitude_.load(std::memory_order_relaxed);
    if (currentAmplitude_ < target) {
        currentAmplitude_ = static_cast<float>(currentAmplitude_ + rampStep_);
        if (currentAmplitude_ > target) currentAmplitude_ = target;
    } else if (currentAmplitude_ > target) {
        currentAmplitude_ = static_cast<float>(currentAmplitude_ - rampStep_);
        if (currentAmplitude_ < target) currentAmplitude_ = target;
    }

    const float value = currentAmplitude_ * static_cast<float>(std::sin(phase_));

    const double increment =
        kTwoPi * frequencyHz_.load(std::memory_order_relaxed) / sampleRate_;
    phase_ += increment;
    while (phase_ >= kTwoPi) phase_ -= kTwoPi;
    while (phase_ < 0.0) phase_ += kTwoPi;
    return value;
}

void ToneGenerator::render(float* dst, uint32_t frames, bool add) {
    for (uint32_t i = 0; i < frames; ++i) {
        const float v = next();
        dst[i] = add ? dst[i] + v : v;
    }
}

}  // namespace anc

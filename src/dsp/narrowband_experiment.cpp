#include "dsp/narrowband_experiment.h"

#include <algorithm>
#include <cmath>

namespace anc {

namespace {
constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr float kOutputLimit = 0.85f;  // hard safety limit for the experiment output
}

void NarrowbandProcessor::prepare(double sampleRate, uint32_t /*maxFrames*/) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    appliedFreq_ = -1.0;
    phase_ = 0.0;
    demI_ = 0.0;
    demQ_ = 0.0;
    const double bandwidth = trackerBandwidthHz_.load(std::memory_order_relaxed);
    trackerCoeff_ = 1.0 - std::exp(-kTwoPi * bandwidth / sampleRate_);
    trackerCoeffDirty_.store(false, std::memory_order_relaxed);
}

void NarrowbandProcessor::process(const float* in, float* out, uint32_t frames) {
    const Mode mode = static_cast<Mode>(mode_.load(std::memory_order_relaxed));
    const double frequency = frequencyHz_.load(std::memory_order_relaxed);
    const float driveGain = driveGain_.load(std::memory_order_relaxed);
    const float cancelGain = cancelGain_.load(std::memory_order_relaxed);
    const double cancelPhase = cancelPhaseDeg_.load(std::memory_order_relaxed) * kTwoPi / 360.0;

    if (trackerCoeffDirty_.load(std::memory_order_relaxed)) {
        const double bandwidth = trackerBandwidthHz_.load(std::memory_order_relaxed);
        trackerCoeff_ = 1.0 - std::exp(-kTwoPi * bandwidth / sampleRate_);
        trackerCoeffDirty_.store(false, std::memory_order_relaxed);
    }

    if (frequency != appliedFreq_) {
        appliedFreq_ = frequency;
        phaseInc_ = kTwoPi * frequency / sampleRate_;
    }

    double peak = 0.0;
    uint64_t clips = 0;
    double magAccum = 0.0;
    uint32_t magCount = 0;

    for (uint32_t i = 0; i < frames; ++i) {
        const double c = std::cos(phase_);
        const double s = std::sin(phase_);

        // Quadrature demodulation of the input at `frequency`.
        // demI -> A*cos(psi), demQ -> A*sin(psi) for in ~ A*cos(theta+psi).
        demI_ += trackerCoeff_ * (2.0 * static_cast<double>(in[i]) * c - demI_);
        demQ_ += trackerCoeff_ * (2.0 * static_cast<double>(in[i]) * s - demQ_);
        const double mag = std::sqrt(demI_ * demI_ + demQ_ * demQ_);
        magAccum += mag;
        ++magCount;

        double v = 0.0;
        switch (mode) {
            case Mode::Off:
                v = 0.0;
                break;
            case Mode::DriveOnly:
                v = static_cast<double>(driveGain) * s;
                break;
            case Mode::DriveCancel:
                v = static_cast<double>(driveGain) * s -
                    static_cast<double>(cancelGain) * std::sin(phase_ + cancelPhase);
                break;
            case Mode::TrackedCancel:
                // Reproduce the input's in-phase component: I*cos - Q*sin,
                // emitted with opposite polarity and controller gain.
                v = -static_cast<double>(cancelGain) * (demI_ * c - demQ_ * s);
                break;
        }

        if (v > kOutputLimit || v < -kOutputLimit) {
            ++clips;
            v = v > 0.0 ? kOutputLimit : -kOutputLimit;
        }
        out[i] = static_cast<float>(v);
        const double a = std::fabs(v);
        if (a > peak) peak = a;

        phase_ += phaseInc_;
        if (phase_ >= kTwoPi) phase_ -= kTwoPi;
        while (phase_ < 0.0) phase_ += kTwoPi;
    }

    const double meanMag = magCount > 0 ? magAccum / static_cast<double>(magCount) : 0.0;
    residualMag_.store(meanMag, std::memory_order_relaxed);
    residualDbfs_.store(meanMag > 1e-9 ? 20.0 * std::log10(meanMag) : -180.0,
                        std::memory_order_relaxed);
    outputPeak_.store(peak, std::memory_order_relaxed);
    if (clips) {
        clipCount_.fetch_add(clips, std::memory_order_relaxed);
    }
}

}  // namespace anc

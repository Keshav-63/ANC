#pragma once

#include <atomic>
#include <cstdint>

#include "core/audio_processor.h"

namespace anc {

// Level 2 narrowband experiment processor (stationary tone only).
//
// Modes:
//   Off           - output silence.
//   DriveOnly     - emit a pure sine at `frequency`. Used as the "noise"
//                   source in the self-contained loopback experiment, or as
//                   a baseline reference tone.
//   DriveCancel   - emit the drive sine plus a deliberately calibrated
//                   anti-tone with independent gain and phase. The host
//                   sweeps phase/gain and picks the residual minimum.
//   TrackedCancel - internal-model controller: a quadrature demodulator
//                   tracks the amplitude/phase of the input component at
//                   `frequency`, and the processor emits -gain * bandpass(in),
//                   i.e. an anti-tone locked to the measured residual.
//
// Regardless of mode, the quadrature tracker always measures the residual
// amplitude of the INPUT signal at `frequency`; the host reads it via
// residualMagnitude()/residualDbfs(). Output is hard-limited for safety.
class NarrowbandProcessor : public AudioProcessor {
public:
    enum class Mode : uint32_t { Off = 0, DriveOnly = 1, DriveCancel = 2, TrackedCancel = 3 };

    void prepare(double sampleRate, uint32_t maxFrames) override;
    void process(const float* in, float* out, uint32_t frames) override;
    const char* name() const override { return "narrowband"; }

    // Real-time safe parameter updates (call from the host thread).
    void setMode(Mode mode) { mode_.store(static_cast<uint32_t>(mode), std::memory_order_relaxed); }
    Mode mode() const { return static_cast<Mode>(mode_.load(std::memory_order_relaxed)); }
    void setFrequency(double hz) { frequencyHz_.store(hz, std::memory_order_relaxed); }
    double frequency() const { return frequencyHz_.load(std::memory_order_relaxed); }
    void setDriveGain(float linear) { driveGain_.store(linear, std::memory_order_relaxed); }
    void setCancelGain(float linear) { cancelGain_.store(linear, std::memory_order_relaxed); }
    void setCancelPhaseDeg(double deg) { cancelPhaseDeg_.store(deg, std::memory_order_relaxed); }
    void setTrackerBandwidthHz(double hz) {
        trackerBandwidthHz_.store(hz, std::memory_order_relaxed);
        trackerCoeffDirty_.store(true, std::memory_order_relaxed);
    }

    // Measurement readouts (safe from the host thread).
    double residualMagnitude() const { return residualMag_.load(std::memory_order_relaxed); }
    double residualDbfs() const { return residualDbfs_.load(std::memory_order_relaxed); }
    double lastOutputPeak() const { return outputPeak_.load(std::memory_order_relaxed); }
    uint64_t clipCount() const { return clipCount_.load(std::memory_order_relaxed); }

    double sampleRate() const { return sampleRate_; }

private:
    std::atomic<uint32_t> mode_{static_cast<uint32_t>(Mode::Off)};
    std::atomic<double> frequencyHz_{200.0};
    std::atomic<float> driveGain_{0.0f};
    std::atomic<float> cancelGain_{0.0f};
    std::atomic<double> cancelPhaseDeg_{180.0};
    std::atomic<double> trackerBandwidthHz_{2.0};
    std::atomic<bool> trackerCoeffDirty_{true};

    std::atomic<double> residualMag_{0.0};
    std::atomic<double> residualDbfs_{-300.0};
    std::atomic<double> outputPeak_{0.0};
    std::atomic<uint64_t> clipCount_{0};

    // Audio-thread state.
    double sampleRate_ = 48000.0;
    double phase_ = 0.0;           // NCO phase [0, 2pi)
    double phaseInc_ = 0.0;
    double appliedFreq_ = -1.0;
    double trackerCoeff_ = 0.0;
    double demI_ = 0.0;
    double demQ_ = 0.0;
};

}  // namespace anc

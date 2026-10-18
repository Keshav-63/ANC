#pragma once

#include <vector>

namespace anc {

// Direct-form FIR filter with persistent state across blocks (streaming
// convolution). Allocation happens only in design/assign; process() is
// real-time safe. Coefficients are centered at tap (size-1)/2.
class FirFilter {
public:
    FirFilter() = default;

    void assign(std::vector<float> coefficients);
    void reset();

    void process(const float* in, float* out, uint32_t frames);

    size_t size() const { return coefficients_.size(); }
    const std::vector<float>& coefficients() const { return coefficients_; }

    // Linear-phase group delay in samples: (N-1)/2.
    double groupDelaySamples() const {
        return coefficients_.empty() ? 0.0
                                     : 0.5 * static_cast<double>(coefficients_.size() - 1);
    }

    // Magnitude response at a single frequency (test helper).
    double magnitudeAt(double frequencyHz, double sampleRate) const;

    // --- Design helpers -----------------------------------------------------
    // Windowed-sinc low-pass response with a Blackman window. `taps` is
    // forced odd; `cutoffHz` is the -6 dB point.
    static std::vector<float> designLowPass(double cutoffHz, double sampleRate, size_t taps);
    // Normalized moving average (DC gain = 1).
    static std::vector<float> designMovingAverage(size_t taps);

private:
    std::vector<float> coefficients_;
    std::vector<float> delayLine_;  // circular, size == coefficients_
    size_t writeIndex_ = 0;
};

}  // namespace anc

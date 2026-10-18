#include "dsp/fir_filter.h"

#include <cmath>

namespace anc {

void FirFilter::assign(std::vector<float> coefficients) {
    coefficients_ = std::move(coefficients);
    delayLine_.assign(coefficients_.size(), 0.0f);
    writeIndex_ = 0;
}

void FirFilter::reset() {
    std::fill(delayLine_.begin(), delayLine_.end(), 0.0f);
    writeIndex_ = 0;
}

void FirFilter::process(const float* in, float* out, uint32_t frames) {
    if (coefficients_.empty()) {
        for (uint32_t i = 0; i < frames; ++i) out[i] = in[i];
        return;
    }
    const size_t n = coefficients_.size();
    for (uint32_t f = 0; f < frames; ++f) {
        delayLine_[writeIndex_] = in[f];
        float acc = 0.0f;
        // Convolve newest (writeIndex_) backwards with coefficients_[0..].
        size_t idx = writeIndex_;
        for (size_t k = 0; k < n; ++k) {
            acc += coefficients_[k] * delayLine_[idx];
            idx = (idx == 0) ? n - 1 : idx - 1;
        }
        out[f] = acc;
        writeIndex_ = (writeIndex_ + 1) % n;
    }
}

double FirFilter::magnitudeAt(double frequencyHz, double sampleRate) const {
    double re = 0.0, im = 0.0;
    for (size_t k = 0; k < coefficients_.size(); ++k) {
        const double angle =
            -2.0 * 3.14159265358979323846 * frequencyHz * static_cast<double>(k) / sampleRate;
        re += coefficients_[k] * std::cos(angle);
        im += coefficients_[k] * std::sin(angle);
    }
    return std::sqrt(re * re + im * im);
}

std::vector<float> FirFilter::designLowPass(double cutoffHz, double sampleRate, size_t taps) {
    if (taps < 3) taps = 3;
    if (taps % 2 == 0) ++taps;

    std::vector<float> h(taps);
    const double fc = cutoffHz / sampleRate;  // normalized (cycles/sample)
    const int m = static_cast<int>((taps - 1) / 2);
    double sum = 0.0;
    for (int n = 0; n < static_cast<int>(taps); ++n) {
        const int k = n - m;
        const double sinc = (k == 0) ? 2.0 * fc
                                     : std::sin(2.0 * 3.14159265358979323846 * fc * k) /
                                           (3.14159265358979323846 * k);
        // Blackman window
        const double w =
            0.42 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * n / (taps - 1)) +
            0.08 * std::cos(4.0 * 3.14159265358979323846 * n / (taps - 1));
        h[n] = static_cast<float>(sinc * w);
        sum += h[n];
    }
    // Normalize DC gain to 1.
    if (sum != 0.0) {
        for (auto& v : h) v = static_cast<float>(v / sum);
    }
    return h;
}

std::vector<float> FirFilter::designMovingAverage(size_t taps) {
    if (taps < 1) taps = 1;
    return std::vector<float>(taps, 1.0f / static_cast<float>(taps));
}

}  // namespace anc

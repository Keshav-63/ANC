// Phase 3 test suite: FIR filter behaviour.
//
// - impulse response equals the coefficient set
// - analytic frequency response matches the DFT of the coefficients
// - designed low-pass meets its -6 dB definition at the cutoff frequency
// - block-wise streaming equals whole-signal convolution (state persistence)
// - tone generator frequency/phase accuracy

#include <algorithm>
#include <cmath>
#include <vector>

#include "dsp/fir_filter.h"
#include "dsp/tone_generator.h"
#include "test_framework.h"

using namespace anc;

namespace {

constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

double goertzelAmplitude(const std::vector<float>& x, double freq, size_t startSample) {
    // Quadrature (lock-in) amplitude estimate of the component at `freq`.
    double re = 0.0, im = 0.0;
    for (size_t i = startSample; i < x.size(); ++i) {
        const double t = static_cast<double>(i) / kRate;
        re += static_cast<double>(x[i]) * std::cos(2.0 * kPi * freq * t);
        im += static_cast<double>(x[i]) * std::sin(2.0 * kPi * freq * t);
    }
    const double n = static_cast<double>(x.size() - startSample);
    return 2.0 * std::sqrt(re * re + im * im) / n;
}

}  // namespace

ANCTEST(fir_impulse_response_is_coefficients) {
    const auto coeffs = FirFilter::designLowPass(1000.0, kRate, 33);
    FirFilter filter;
    filter.assign(coeffs);

    std::vector<float> impulse(coeffs.size() + 10, 0.0f);
    impulse[0] = 1.0f;
    std::vector<float> response(impulse.size(), 0.0f);
    filter.process(impulse.data(), response.data(), static_cast<uint32_t>(impulse.size()));

    double maxDiff = 0.0;
    for (size_t i = 0; i < coeffs.size(); ++i) {
        maxDiff = std::max(maxDiff, std::fabs(static_cast<double>(response[i] - coeffs[i])));
    }
    for (size_t i = coeffs.size(); i < response.size(); ++i) {
        maxDiff = std::max(maxDiff, std::fabs(static_cast<double>(response[i])));
    }
    ctx.expectNear(maxDiff, 0.0, 1e-7, "impulse response equals coefficient set");
}

ANCTEST(fir_analytic_vs_measured_response) {
    // Feed single-frequency sines and compare the measured output amplitude
    // against magnitudeAt(), the analytic evaluation used everywhere else.
    const auto coeffs = FirFilter::designLowPass(2000.0, kRate, 129);
    FirFilter filter;
    filter.assign(coeffs);

    for (double f : {100.0, 500.0, 1000.0, 2000.0, 4000.0}) {
        const size_t n = static_cast<size_t>(1.0 * kRate);
        std::vector<float> in(n), out(n);
        for (size_t i = 0; i < n; ++i) {
            in[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * f * i / kRate));
        }
        filter.reset();
        filter.process(in.data(), out.data(), static_cast<uint32_t>(n));

        const double measured = goertzelAmplitude(out, f, static_cast<size_t>(0.2 * kRate)) / 0.5;
        const double analytic = filter.magnitudeAt(f, kRate);
        ctx.expectNear(measured, analytic, 0.02 * std::max(analytic, 0.02),
                       "response at " + std::to_string(static_cast<int>(f)) + " Hz");
    }
}

ANCTEST(fir_lowpass_cutoff_definition) {
    const auto coeffs = FirFilter::designLowPass(1000.0, kRate, 65);
    FirFilter filter;
    filter.assign(coeffs);
    const double atCutoff = filter.magnitudeAt(1000.0, kRate);
    // The windowed-sinc -6 dB point sits close to (but not exactly at) the
    // design cutoff; require half gain within a modest band around it.
    ctx.expectNear(atCutoff, 0.5, 0.12, "gain near the -6 dB cutoff frequency");
    ctx.expectNear(filter.magnitudeAt(0.0, kRate), 1.0, 1e-3, "DC gain");
    // Transition sanity: deep stopband well above the cutoff (the Blackman
    // window's transition band spans roughly +/- 2 kHz here).
    ctx.expect(filter.magnitudeAt(4000.0, kRate) < 0.02,
               "stopband attenuation well above the cutoff");
}

ANCTEST(fir_block_processing_persists_state) {
    const auto coeffs = FirFilter::designLowPass(3000.0, kRate, 97);
    const size_t n = static_cast<size_t>(0.5 * kRate);
    std::vector<float> in(n);
    for (size_t i = 0; i < n; ++i) {
        in[i] = static_cast<float>(0.4 * std::sin(2.0 * kPi * 700.0 * i / kRate) +
                                   0.1 * std::sin(2.0 * kPi * 5100.0 * i / kRate));
    }

    FirFilter whole;
    whole.assign(coeffs);
    std::vector<float> wholeOut(n);
    whole.process(in.data(), wholeOut.data(), static_cast<uint32_t>(n));

    FirFilter blocked;
    blocked.assign(coeffs);
    std::vector<float> blockedOut(n);
    const size_t sizes[] = {1, 15, 480, 4801, 33, 7, 96000};
    size_t offset = 0, si = 0;
    while (offset < n) {
        const size_t k = std::min(sizes[si % std::size(sizes)], n - offset);
        blocked.process(in.data() + offset, blockedOut.data() + offset,
                        static_cast<uint32_t>(k));
        offset += k;
        ++si;
    }
    double maxDiff = 0.0;
    for (size_t i = 0; i < n; ++i) maxDiff = std::max(maxDiff, std::fabs(static_cast<double>(wholeOut[i] - blockedOut[i])));
    ctx.expectNear(maxDiff, 0.0, 1e-6, "streaming vs whole-signal convolution");
}

ANCTEST(tone_generator_frequency_and_ramp) {
    ToneGenerator gen;
    gen.prepare(kRate, 0.01);
    gen.setFrequency(500.0);
    gen.setAmplitude(0.5f);

    const size_t n = static_cast<size_t>(1.0 * kRate);
    std::vector<float> out(n);
    gen.render(out.data(), static_cast<uint32_t>(n), false);

    // Amplitude must reach the target after the ramp and the frequency must
    // be exact.
    const double amp = goertzelAmplitude(out, 500.0, static_cast<size_t>(0.1 * kRate));
    ctx.expectNear(amp, 0.5, 0.002, "tone amplitude after ramp");
    const double wrong = goertzelAmplitude(out, 500.0 * 1.01, static_cast<size_t>(0.1 * kRate));
    ctx.expect(wrong < 0.05, "off-frequency leakage is small");

    // Click-free startup: first samples must be small compared with the
    // steady-state amplitude (the ramp eases in from zero).
    double maxStart = 0.0;
    for (size_t i = 0; i < 12; ++i) maxStart = std::max(maxStart, std::fabs(static_cast<double>(out[i])));
    ctx.expect(maxStart < 0.05, "ramped startup starts small (no click)");

    // Phase continuity across a frequency change: no sample-to-sample jump
    // beyond the sine's own maximum slew.
    gen.setFrequency(1000.0);
    std::vector<float> after(480, 0.0f);
    gen.render(after.data(), 480, false);
    double maxDelta = 0.0;
    for (size_t i = 1; i < after.size(); ++i) {
        maxDelta = std::max(maxDelta, std::fabs(static_cast<double>(after[i] - after[i - 1])));
    }
    // Max slew of a 0.5 amplitude 1 kHz sine at 48 kHz is ~0.065; allow 4x.
    ctx.expect(maxDelta < 0.26, "no waveform discontinuity at frequency change");
}

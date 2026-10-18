// Phase 3 test suite: anti-phase mathematics.
//
// Verifies the residuals numerically:
//   - exact opposite phase sums to (numerical) zero
//   - timing error dt leaves residual 2A|sin(pi f dt)|
//   - gain mismatch g leaves residual A*|1-g|
//   - a deliberate 90 deg phase error leaves residual sqrt(2)*A
//   - behaviour at non-integer cycle boundaries

#include <algorithm>
#include <cmath>
#include <iterator>
#include <vector>

#include "dsp/pass_through.h"
#include "test_framework.h"

using namespace anc;

namespace {

constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(double freq, double seconds, double amplitude = 0.5) {
    const size_t n = static_cast<size_t>(seconds * kRate);
    std::vector<float> out(n);
    for (size_t i = 0; i < n; ++i) {
        out[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * freq * i / kRate));
    }
    return out;
}

// Linear-interpolation fractional delay: out[n] = in[n - delaySamples].
std::vector<float> fractionalDelay(const std::vector<float>& in, double delaySamples) {
    std::vector<float> out(in.size(), 0.0f);
    const long long whole = static_cast<long long>(std::floor(delaySamples));
    const double frac = delaySamples - static_cast<double>(whole);
    for (size_t n = 0; n < in.size(); ++n) {
        const long long i1 = static_cast<long long>(n) - whole;
        const long long i0 = i1 - 1;
        const double b = (i1 >= 0 && i1 < static_cast<long long>(in.size()))
                             ? in[static_cast<size_t>(i1)] : 0.0;
        const double a = (i0 >= 0 && i0 < static_cast<long long>(in.size()))
                             ? in[static_cast<size_t>(i0)] : 0.0;
        out[n] = static_cast<float>(b * (1.0 - frac) + a * frac);
    }
    return out;
}

// Offset-compensated RMS->amplitude estimate.
double amplitudeOf(const std::vector<float>& x, double skipSeconds = 0.15) {
    const size_t start = static_cast<size_t>(skipSeconds * kRate);
    double sum = 0.0;
    for (size_t i = start; i < x.size(); ++i) sum += static_cast<double>(x[i]) * x[i];
    return std::sqrt(2.0 * sum / static_cast<double>(x.size() - start));
}

}  // namespace

ANCTEST(phase_math_ideal_inverse_sums_to_zero) {
    for (double f : {50.0, 100.0, 200.0, 500.0}) {
        const auto x = sine(f, 0.5);
        std::vector<float> sum(x.size());
        for (size_t i = 0; i < x.size(); ++i) sum[i] = x[i] - x[i];
        ctx.expectNear(amplitudeOf(sum), 0.0, 1e-9, "ideal inverse residual at " + std::to_string(f));
    }
}

ANCTEST(phase_math_timing_error_matches_theory) {
    // Residual of x(t) minus an inverted, delayed copy: 2A|sin(pi f dt)|.
    for (double f : {50.0, 100.0, 200.0, 500.0}) {
        const auto x = sine(f, 0.8);
        for (double dtMs : {0.05, 0.1, 0.2, 0.5, 1.0}) {
            const double dt = dtMs / 1000.0;
            const auto delayed = fractionalDelay(x, dt * kRate);
            std::vector<float> sum(x.size());
            for (size_t i = 0; i < x.size(); ++i) sum[i] = x[i] - delayed[i];
            const double theory = 2.0 * 0.5 * std::fabs(std::sin(kPi * f * dt));
            const double measured = amplitudeOf(sum);
            ctx.expectNear(measured, theory, 0.02 * 0.5 + 0.003,
                           "timing residual " + std::to_string(f) + " Hz dt=" +
                               std::to_string(dtMs) + " ms");
        }
    }
}

ANCTEST(phase_math_gain_error_matches_theory) {
    const auto x = sine(200.0, 0.5);
    for (double g : {0.01, 0.05, 0.1, 0.3}) {
        std::vector<float> sum(x.size());
        for (size_t i = 0; i < x.size(); ++i) {
            sum[i] = x[i] - static_cast<float>((1.0 + g) * x[i]);
        }
        ctx.expectNear(amplitudeOf(sum), g * 0.5, 0.004, "gain mismatch g=" + std::to_string(g));
    }
}

ANCTEST(phase_math_quarter_cycle_error_is_sqrt2_times_amplitude) {
    const double f = 200.0;
    const auto x = sine(f, 0.5);
    const double dt = 0.25 / f;  // 90 degrees
    const auto delayed = fractionalDelay(x, dt * kRate);
    std::vector<float> sum(x.size());
    for (size_t i = 0; i < x.size(); ++i) sum[i] = x[i] - delayed[i];
    ctx.expectNear(amplitudeOf(sum), std::sqrt(2.0) * 0.5, 0.02,
                   "90 degree error residual");
}

ANCTEST(phase_math_non_integer_cycle_boundaries) {
    // 37.3 Hz over 3.7 s is not an integer number of cycles. Route the signal
    // through a real block processor (PassThrough with inversion) in odd,
    // non-cycle-aligned block sizes; the streamed inverse must cancel the
    // input to exact floating-point zero at every block edge.
    const double f = 37.3;
    const auto x = sine(f, 3.7, 0.5);

    PassThrough inverter;
    inverter.setInvert(true);
    inverter.prepare(kRate, 4096);

    std::vector<float> cancelled(x.size());
    const size_t blockSizes[] = {1, 3, 128, 4095, 7, 2000, 1, 96};
    size_t offset = 0, bi = 0;
    std::vector<float> out(4096);
    while (offset < x.size()) {
        const size_t n = std::min(blockSizes[bi % std::size(blockSizes)], x.size() - offset);
        inverter.process(x.data() + offset, out.data(), static_cast<uint32_t>(n));
        for (size_t i = 0; i < n; ++i) cancelled[offset + i] = x[offset + i] + out[i];
        offset += n;
        ++bi;
    }
    double maxResidual = 0.0;
    for (float v : cancelled) maxResidual = std::max(maxResidual, std::fabs(static_cast<double>(v)));
    ctx.expectNear(maxResidual, 0.0, 1e-9, "block-wise inverse at fractional cycle edges");

    // Same check at the very start of the stream (startup edge) with a
    // one-sample first block.
    PassThrough inverter2;
    inverter2.setInvert(true);
    inverter2.prepare(kRate, 8);
    float one[1] = {0.5f};
    float outOne[1] = {0.0f};
    inverter2.process(one, outOne, 1);
    ctx.expectNear(static_cast<double>(one[0] + outOne[0]), 0.0, 1e-9, "startup edge cancellation");
}

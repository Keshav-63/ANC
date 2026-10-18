// Phase 3 deliverable: dsp_sandbox
//
// Validates the anti-phase mathematics and the FIR implementation numerically
// before any acoustic experiment:
//   1. inverse-summation residual vs deliberate timing errors
//   2. residual vs gain errors and vs phase errors
//   3. FIR frequency-response verification (windowed-sinc low-pass)
//
// Writes CSVs under results/ for offline comparison with Python.

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "audio/wasapi_utils.h"
#include "dsp/fir_filter.h"
#include "dsp/tone_generator.h"

using namespace anc;

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 48000.0;

std::vector<float> makeSine(double freq, double seconds, double amplitude = 0.5) {
    const size_t n = static_cast<size_t>(seconds * kRate);
    std::vector<float> out(n);
    for (size_t i = 0; i < n; ++i) {
        out[i] = static_cast<float>(amplitude * std::sin(kTwoPi * freq * i / kRate));
    }
    return out;
}

// Linear-interpolation fractional delay: out[n] = in[n - delaySamples].
std::vector<float> delaySignal(const std::vector<float>& in, double delaySamples) {
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

// RMS amplitude -> sine amplitude (skip the first 0.1 s of transient).
double measureAmplitude(const std::vector<float>& signal, double skipSeconds = 0.1) {
    const size_t start = static_cast<size_t>(skipSeconds * kRate);
    if (signal.size() <= start + 100) return 0.0;
    double sum = 0.0;
    for (size_t i = start; i < signal.size(); ++i) sum += static_cast<double>(signal[i]) * signal[i];
    const double rms = std::sqrt(sum / static_cast<double>(signal.size() - start));
    return rms * std::sqrt(2.0);
}

int g_failures = 0;

void checkNear(const char* what, double measured, double expected, double tolerance) {
    const double diff = std::fabs(measured - expected);
    const bool pass = diff <= tolerance;
    if (!pass) ++g_failures;
    std::printf("  [%s] %-44s measured %.5f, expected %.5f (diff %.5f, tol %.5f)\n",
                pass ? "PASS" : "FAIL", what, measured, expected, diff, tolerance);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    (void)argc;
    (void)argv;
    SetConsoleOutputCP(CP_UTF8);

    std::error_code ec;
    std::filesystem::create_directories("results", ec);

    const double amplitude = 0.5;
    const double freqs[] = {50.0, 100.0, 200.0, 500.0};
    const double timingErrorsMs[] = {0.0, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0};

    // --- 1. Residual vs timing error -------------------------------------
    std::printf("timing-error residual (2*A*|sin(pi*f*dt)|)\n");
    {
        std::ofstream csv("results/dsp_inverse_residual.csv");
        csv << "freq_hz,dt_ms,phase_deg,theory_amp,measured_amp,diff,residual_db,"
               "cancellation_db\n";
        for (double f : freqs) {
            const auto signal = makeSine(f, 0.6, amplitude);
            for (double dtMs : timingErrorsMs) {
                const double dt = dtMs / 1000.0;
                const auto delayed = delaySignal(signal, dt * kRate);
                std::vector<float> sum(signal.size());
                // Anti-noise is the inverted, delayed reference: residual = x - x_delayed.
                for (size_t i = 0; i < signal.size(); ++i) sum[i] = signal[i] - delayed[i];
                const double measured = measureAmplitude(sum);
                const double theory = 2.0 * amplitude * std::fabs(std::sin(kPi * f * dt));
                const double phaseDeg = 360.0 * f * dt;
                csv << f << "," << dtMs << "," << phaseDeg << "," << theory << "," << measured
                    << "," << (measured - theory) << ","
                    << 20.0 * std::log10(measured / amplitude + 1e-12) << ","
                    << -20.0 * std::log10(measured / amplitude + 1e-12) << "\n";
                if (dtMs == 0.0 || dtMs == 0.1 || dtMs == 1.0) {
                    char label[96];
                    std::snprintf(label, sizeof(label), "%.0f Hz, dt=%.2f ms", f, dtMs);
                    checkNear(label, measured, theory, 0.02 * amplitude + 0.002);
                }
            }
        }
        std::printf("  wrote results/dsp_inverse_residual.csv\n");
    }

    // --- 2. Residual vs gain and phase errors ----------------------------
    std::printf("gain / phase error residual\n");
    {
        std::ofstream csv("results/dsp_error_residual.csv");
        csv << "type,value,theory_amp,measured_amp,diff\n";
        const double f = 200.0;
        const auto signal = makeSine(f, 0.6, amplitude);

        // Gain error: sum = A sin + (1+e) A sin(delayed by 0) inverted => e*A.
        for (double gainError : {0.0, 0.01, 0.05, 0.1, 0.25}) {
            std::vector<float> sum(signal.size());
            for (size_t i = 0; i < signal.size(); ++i) {
                sum[i] = signal[i] - static_cast<float>((1.0 + gainError) * signal[i]);
            }
            const double theory = gainError * amplitude;
            const double measured = measureAmplitude(sum);
            csv << "gain," << gainError << "," << theory << "," << measured << ","
                << (measured - theory) << "\n";
            char label[96];
            std::snprintf(label, sizeof(label), "gain error %.2f", gainError);
            checkNear(label, measured, theory, 0.002 * amplitude + 0.0005);
        }

        // Phase error of the anti-noise: residual 2A|sin(phi/2)|.
        for (double phaseDeg : {0.0, 5.0, 30.0, 90.0, 180.0}) {
            const double dt = phaseDeg / (360.0 * f);
            const auto delayed = delaySignal(signal, dt * kRate);
            std::vector<float> sum(signal.size());
            for (size_t i = 0; i < signal.size(); ++i) sum[i] = signal[i] - delayed[i];
            const double theory =
                2.0 * amplitude * std::fabs(std::sin(kPi * phaseDeg / 360.0));
            const double measured = measureAmplitude(sum);
            csv << "phase_deg," << phaseDeg << "," << theory << "," << measured << ","
                << (measured - theory) << "\n";
            char label[96];
            std::snprintf(label, sizeof(label), "phase error %.0f deg", phaseDeg);
            checkNear(label, measured, theory, 0.03 * amplitude + 0.002);
        }
        std::printf("  wrote results/dsp_error_residual.csv\n");
    }

    // --- 3. FIR frequency response ---------------------------------------
    std::printf("FIR low-pass response (1 kHz, 65 taps, Blackman)\n");
    {
        FirFilter filter;
        filter.assign(FirFilter::designLowPass(1000.0, kRate, 65));

        std::ofstream csv("results/dsp_fir_response.csv");
        csv << "freq_hz,mag_linear,mag_db\n";
        const double testFreqs[] = {50, 100, 200, 500, 800, 1000, 1200, 2000, 5000, 10000, 20000};
        for (double f : testFreqs) {
            const double mag = filter.magnitudeAt(f, kRate);
            csv << f << "," << mag << "," << 20.0 * std::log10(mag + 1e-12) << "\n";
        }

        // DC gain must be ~1 and the 4 kHz point must be well attenuated.
        const double dc = filter.magnitudeAt(0.0, kRate);
        const double at4k = filter.magnitudeAt(4000.0, kRate);
        const double at20k = filter.magnitudeAt(20000.0, kRate);
        checkNear("DC gain", dc, 1.0, 1e-3);
        std::printf("  [%s] %-44s %.4f (%.1f dB)\n", at4k < 0.01 ? "PASS" : "FAIL",
                    "4 kHz attenuation", at4k, 20.0 * std::log10(at4k));
        if (at4k >= 0.01) ++g_failures;
        std::printf("  [%s] %-44s %.4f (%.1f dB)\n", at20k < 0.005 ? "PASS" : "FAIL",
                    "20 kHz attenuation", at20k, 20.0 * std::log10(at20k));
        if (at20k >= 0.005) ++g_failures;

        // Streaming block processing must equal whole-signal convolution at
        // block boundaries (state persistence).
        const auto signal = makeSine(300.0, 0.5, 0.5);
        std::vector<float> whole(signal.size());
        filter.reset();
        filter.process(signal.data(), whole.data(), static_cast<uint32_t>(signal.size()));

        std::vector<float> blocked(signal.size());
        filter.reset();
        size_t offset = 0;
        const size_t blocks[] = {1, 7, 64, 4096, 3, 2000, 1, 1, 12345};
        size_t bi = 0;
        while (offset < signal.size()) {
            const size_t n = std::min(blocks[bi % std::size(blocks)], signal.size() - offset);
            filter.process(signal.data() + offset, blocked.data() + offset,
                           static_cast<uint32_t>(n));
            offset += n;
            ++bi;
        }
        double maxDiff = 0.0;
        for (size_t i = 0; i < signal.size(); ++i) {
            maxDiff = std::max(maxDiff, std::fabs(static_cast<double>(whole[i]) - blocked[i]));
        }
        checkNear("block-boundary equivalence (max diff)", maxDiff, 0.0, 1e-6);

        // Moving-average sanity: DC gain exactly 1, and the impulse response
        // is the coefficient set itself.
        FirFilter ma;
        ma.assign(FirFilter::designMovingAverage(8));
        checkNear("moving average DC gain", ma.magnitudeAt(0.0, kRate), 1.0, 1e-6);
        std::printf("  wrote results/dsp_fir_response.csv\n");
    }

    std::printf("\n%s\n", g_failures == 0 ? "dsp_sandbox: ALL CHECKS PASSED"
                                          : "dsp_sandbox: FAILURES DETECTED");
    return g_failures == 0 ? 0 : 1;
}

// Phase 4 deliverable: narrowband_experiment
//
// Runs the controlled stationary-tone experiment:
//   drive_cancel   - the earphone output itself is the "noise" (drive tone);
//                    the host sweeps the anti-tone phase, picks the residual
//                    minimum, then alternates baseline/cancel windows.
//   tracked_cancel - the controller tracks an external stationary tone at
//                    --freq (e.g. a phone playing a 200 Hz tone near the
//                    laptop) and emits a locked anti-tone.
//
// The residual amplitude at --freq is measured inside the DSP itself
// (quadrature tracker on the microphone signal) and independently verified
// afterwards from the recorded WAV by experiments/analyze_spectrum.py.
//
// Safety: output is hard-limited inside the processor; this harness trips the
// experiment off if clipping is detected repeatedly.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "audio/wasapi_streams.h"
#include "core/audio_metrics.h"
#include "dsp/narrowband_experiment.h"
#include "tools/cli.h"
#include "tools/run_reporter.h"

using namespace anc;

namespace {

using Mode = NarrowbandProcessor::Mode;

std::atomic<bool> g_stop{false};

BOOL WINAPI consoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_BREAK_EVENT) {
        g_stop.store(true);
        return TRUE;
    }
    return FALSE;
}

struct WindowResult {
    std::string label;
    double startSec = 0.0;
    double endSec = 0.0;
    double residualDbfs = -300.0;
    double residualMag = 0.0;
    double outPeak = 0.0;
    double cancelPhaseDeg = 0.0;
    bool tripped = false;
};

const char* modeName(Mode mode) {
    switch (mode) {
        case Mode::Off: return "off";
        case Mode::DriveOnly: return "drive";
        case Mode::DriveCancel: return "drive+cancel";
        case Mode::TrackedCancel: return "tracked-cancel";
    }
    return "?";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetConsoleCtrlHandler(consoleHandler, TRUE);
    const Args args(argc, argv);

    const std::wstring experiment = args.get(L"--experiment", L"drive_cancel");
    const double frequency = args.getDouble(L"--freq", 200.0);
    const float driveGain = args.getFloat(L"--drive-gain", 0.2f);
    const float cancelGain = args.getFloat(
        L"--cancel-gain", experiment == L"tracked_cancel" ? 0.5f : driveGain);
    const double windowSeconds = args.getDouble(L"--window-sec", 5.0);
    const int cycles = args.getInt(L"--cycles", 6);
    const bool sweepEnabled = !args.has(L"--no-sweep");
    const double sweepStepDeg = args.getDouble(L"--phase-step", 15.0);
    const double sweepWindowSeconds = args.getDouble(L"--sweep-window", 3.0);
    const double warmupSeconds = args.getDouble(L"--warmup", 2.0);
    const double initialPhaseDeg = args.getDouble(L"--cancel-phase", 180.0);

    const bool driveCancel = experiment == L"drive_cancel";

    std::printf(
        "narrowband experiment: %s at %.1f Hz\n"
        "  drive gain %.3f, cancel gain %.3f, windows %.1f s x %d\n"
        "  safety: output hard-limited; clipping trips the controller off\n"
        "  guidance: keep source/laptop/earphone positions fixed; hold the earphone\n"
        "            near the laptop microphone for the strongest residual signal\n\n",
        toUtf8(experiment).c_str(), frequency, driveGain, cancelGain, windowSeconds, cycles);

    DuplexEngine::Options options;
    std::string error;
    options.captureDeviceId = selectDeviceId(eCapture, args.get(L"--capture-device"), error);
    if (options.captureDeviceId.empty()) {
        std::printf("error: %s\n", error.c_str());
        return 1;
    }
    options.renderDeviceId = selectDeviceId(eRender, args.get(L"--render-device"), error);
    if (options.renderDeviceId.empty()) {
        std::printf("error: %s\n", error.c_str());
        return 1;
    }
    options.periodFrames = static_cast<uint32_t>(args.getInt(L"--period", 0));
    options.captureRaw = args.has(L"--capture-raw");
    options.recordInputPath = args.get(L"--record-input", L"results/narrowband_mic.wav");
    options.recordOutputPath = args.get(L"--record-output", L"results/narrowband_out.wav");

    NarrowbandProcessor processor;
    processor.setFrequency(frequency);
    processor.setDriveGain(driveGain);
    processor.setCancelGain(cancelGain);

    DuplexEngine engine;
    try {
        const DuplexPlan plan = engine.initialize(options);
        std::printf("capture: %s (%s)\nrender : %s (%s)\nperiod : %u frames\n",
                    toUtf8(plan.captureDeviceName).c_str(), plan.captureFormat.describe().c_str(),
                    toUtf8(plan.renderDeviceName).c_str(), plan.renderFormat.describe().c_str(),
                    plan.periodFrames);
        if (!plan.notes.empty()) std::printf("%s", plan.notes.c_str());
        engine.setProcessor(&processor);

        CsvLogger windowsCsv;
        const std::wstring windowsPath = args.get(L"--windows-csv", L"results/narrowband_windows.csv");
        if (!windowsCsv.open(windowsPath)) {
            std::printf("warning: could not open %s\n", toUtf8(windowsPath).c_str());
        }
        windowsCsv.writeLine(
            "window,label,mode,start_sec,end_sec,freq_hz,drive_gain,cancel_gain,cancel_phase_deg,"
            "residual_mag,residual_dbfs,out_peak,clip_total");

        RunReporter reporter;
        RunReporter::Config reporterConfig;
        reporterConfig.snapshotCsvPath = args.get(L"--metrics-csv");
        reporterConfig.timingCsvPath = args.get(L"--timing-csv");
        reporterConfig.console = false;

        engine.start();
        reporter.begin(engine, reporterConfig);

        const int64_t t0 = qpcTicks();
        int windowIndex = 0;
        uint64_t lastClipTotal = 0;
        bool tripped = false;

        // --- window runner -------------------------------------------------
        auto runWindow = [&](const std::string& label, Mode mode, double phaseDeg,
                             double seconds) -> WindowResult {
            WindowResult result;
            result.label = label;
            result.cancelPhaseDeg = phaseDeg;
            result.startSec = ticksToSeconds(qpcTicks() - t0);

            if (mode == Mode::DriveCancel && phaseDeg >= 0.0) {
                processor.setCancelPhaseDeg(phaseDeg);
            }
            processor.setMode(mode);
            // Give the audio thread a few blocks with the new mode so the
            // first readout sample below is not the tail of the previous
            // window (it would stale-bias peak and residual measurements).
            Sleep(50);

            double magSum = 0.0;
            double peak = 0.0;
            int samples = 0;
            const int64_t windowStart = qpcTicks();
            while (ticksToSeconds(qpcTicks() - windowStart) < seconds && !g_stop.load()) {
                magSum += processor.residualMagnitude();
                peak = std::max(peak, processor.lastOutputPeak());
                ++samples;

                // Safety watchdog: repeated clipping while emitting cancels.
                const uint64_t clipNow = processor.clipCount();
                if (mode != Mode::Off && mode != Mode::DriveOnly &&
                    clipNow - lastClipTotal > 5) {
                    tripped = true;
                    result.tripped = true;
                    processor.setMode(Mode::Off);
                    std::printf("!! safety trip: clipping detected, controller disabled\n");
                }
                lastClipTotal = clipNow;
                reporter.tick(engine);
                Sleep(100);
            }

            result.endSec = ticksToSeconds(qpcTicks() - t0);
            result.residualMag = samples > 0 ? magSum / samples : 0.0;
            result.residualDbfs =
                result.residualMag > 1e-9 ? 20.0 * std::log10(result.residualMag) : -300.0;
            result.outPeak = peak;

            char row[512];
            std::snprintf(row, sizeof(row), "%d,%s,%s,%.3f,%.3f,%.1f,%.3f,%.3f,%.1f,%.6f,%.2f,"
                                            "%.4f,%llu",
                          windowIndex++, label.c_str(), modeName(mode), result.startSec,
                          result.endSec, frequency, driveGain, cancelGain, phaseDeg,
                          result.residualMag, result.residualDbfs, result.outPeak,
                          static_cast<unsigned long long>(processor.clipCount()));
            windowsCsv.writeLine(row);
            std::printf("[%7.2fs] %-16s %-15s residual %7.2f dBFS  peak %.3f\n", result.endSec,
                        label.c_str(), modeName(mode), result.residualDbfs, result.outPeak);
            return result;
        };

        // --- run the schedule ----------------------------------------------
        double bestPhase = initialPhaseDeg;
        if (driveCancel) {
            runWindow("warmup", Mode::DriveOnly, -1.0, warmupSeconds);

            if (sweepEnabled) {
                std::printf("phase sweep (%.0f deg steps, %.1f s per step)...\n", sweepStepDeg,
                            sweepWindowSeconds);
                double best = 1e9;
                for (double phi = 0.0; phi < 360.0; phi += sweepStepDeg) {
                    const WindowResult r =
                        runWindow("sweep_" + std::to_string(static_cast<int>(phi)),
                                  Mode::DriveCancel, phi, sweepWindowSeconds);
                    if (!r.tripped && r.residualMag < best) {
                        best = r.residualMag;
                        bestPhase = phi;
                    }
                    if (g_stop.load()) break;
                }
                std::printf("best phase: %.0f deg (residual %.2f dBFS)\n", bestPhase,
                            20.0 * std::log10(best + 1e-12));
            }

            std::vector<WindowResult> baselines, cancels;
            for (int cycle = 0; cycle < cycles && !g_stop.load(); ++cycle) {
                baselines.push_back(runWindow("baseline", Mode::DriveOnly, -1.0, windowSeconds));
                cancels.push_back(
                    runWindow("cancel", Mode::DriveCancel, bestPhase, windowSeconds));
            }

            if (!baselines.empty() || !cancels.empty()) {
                double bSum = 0.0, cSum = 0.0;
                for (const auto& r : baselines) bSum += r.residualDbfs;
                for (const auto& r : cancels) cSum += r.residualDbfs;
                const double bAvg = baselines.empty() ? -300.0 : bSum / baselines.size();
                const double cAvg = cancels.empty() ? -300.0 : cSum / cancels.size();
                std::printf("\nresidual at mic (processor readout): baseline %.2f dBFS, "
                            "cancel %.2f dBFS -> %.2f dB reduction\n",
                            bAvg, cAvg, bAvg - cAvg);
                std::printf("note: the mic is NOT at the ear; this measures the residual at the\n"
                            "      microphone position only (see project plan section 4/5).\n");
            }
        } else {
            runWindow("warmup", Mode::Off, -1.0, warmupSeconds);
            std::vector<WindowResult> baselines, cancels;
            for (int cycle = 0; cycle < cycles && !g_stop.load(); ++cycle) {
                baselines.push_back(runWindow("baseline", Mode::Off, -1.0, windowSeconds));
                cancels.push_back(runWindow("cancel", Mode::TrackedCancel, -1.0, windowSeconds));
            }
            double bSum = 0.0, cSum = 0.0;
            for (const auto& r : baselines) bSum += r.residualDbfs;
            for (const auto& r : cancels) cSum += r.residualDbfs;
            if (!baselines.empty() && !cancels.empty()) {
                const double bAvg = bSum / baselines.size();
                const double cAvg = cSum / cancels.size();
                std::printf("\nresidual at mic (processor readout): baseline %.2f dBFS, "
                            "cancel %.2f dBFS -> %.2f dB reduction\n",
                            bAvg, cAvg, bAvg - cAvg);
            }
        }

        processor.setMode(Mode::Off);
        engine.stop();
        reporter.finish(engine);
        windowsCsv.close();
        std::printf("windows written to %s\n", toUtf8(windowsPath).c_str());
        std::printf("verify with: python experiments/analyze_spectrum.py --windows %s "
                    "--mic %s\n",
                    toUtf8(windowsPath).c_str(),
                    "results/narrowband_mic.wav");
        if (tripped) {
            std::printf("warning: the safety watchdog tripped at least once; reduce --cancel-gain\n");
            return 3;
        }
    } catch (const AncError& e) {
        std::printf("error: %s: %s\n", e.context.c_str(), e.message.c_str());
        return 1;
    }
    return 0;
}

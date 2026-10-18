// Phase 0 deliverable: audio_probe
//
// Enumerates WASAPI endpoints, reports supported formats and shared-mode
// engine periods, then attempts to initialize the capture and render streams
// concurrently in the intended low-latency configuration. Optionally runs a
// 1-second muted smoke test to verify event delivery.
//
// Writes results/device_probe.txt and prints a PASS/FAIL verdict.

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "audio/device_probe.h"
#include "audio/wasapi_streams.h"
#include "dsp/pass_through.h"
#include "tools/cli.h"

using namespace anc;

namespace {

std::string formatPlan(const DuplexPlan& plan) {
    char buf[1024];
    std::string out;
    std::snprintf(buf, sizeof(buf),
                  "capture device : %s\nrender device  : %s\ncapture format : %s\n"
                  "render format  : %s\nperiod         : %u frames (%.2f ms @ render rate)\n"
                  "capture buffer : %u frames\nrender buffer  : %u frames\n",
                  toUtf8(plan.captureDeviceName).c_str(), toUtf8(plan.renderDeviceName).c_str(),
                  plan.captureFormat.describe().c_str(), plan.renderFormat.describe().c_str(),
                  plan.periodFrames,
                  plan.renderFormat.sampleRate()
                      ? 1000.0 * plan.periodFrames / plan.renderFormat.sampleRate()
                      : 0.0,
                  plan.captureBufferFrames, plan.renderBufferFrames);
    out += buf;
    out += plan.notes;
    return out;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const Args args(argc, argv);

    const std::wstring captureSpec = args.get(L"--capture-device");
    const std::wstring renderSpec = args.get(L"--render-device");
    const int requestedPeriod = args.getInt(L"--period", 0);
    const bool attemptRaw = args.has(L"--attempt-raw");
    const bool noInit = args.has(L"--no-init");
    const bool noStartTest = args.has(L"--no-start-test");
    std::wstring outPath = args.get(L"--out", L"results/device_probe.txt");

    std::string report;
    char buf[512];

    std::snprintf(buf, sizeof(buf), "software-anc audio_probe report\n");
    report += buf;

    // --- Enumerate everything first. ---
    const auto captureEndpoints = enumerateAudioEndpoints(eCapture);
    const auto renderEndpoints = enumerateAudioEndpoints(eRender);
    report += "\n";
    report += formatEndpointTable(eCapture, captureEndpoints);
    report += "\n";
    report += formatEndpointTable(eRender, renderEndpoints);

    std::printf("%s", report.c_str());
    const size_t printedUpTo = report.size();

    int exitCode = 0;
    if (noInit) {
        report += "\n(initialization skipped: --no-init)\n";
    } else {
        report += "\n---- duplex initialization attempt ----\n";
        std::string error;
        const std::wstring captureId = selectDeviceId(eCapture, captureSpec, error);
        if (captureId.empty()) {
            report += "capture device selection failed: " + error + "\n";
            std::printf("%s", report.c_str());
            std::ofstream out{std::filesystem::path(outPath)};
            out << report;
            return 1;
        }
        const std::wstring renderId = selectDeviceId(eRender, renderSpec, error);
        if (renderId.empty()) {
            report += "render device selection failed: " + error + "\n";
            std::printf("%s", report.c_str());
            std::ofstream out{std::filesystem::path(outPath)};
            out << report;
            return 1;
        }

        DuplexEngine engine;
        PassThrough processor;
        processor.setMuted(true);  // the smoke test never plays mic audio back
        DuplexEngine::Options options;
        options.captureDeviceId = captureId;
        options.renderDeviceId = renderId;
        options.periodFrames = requestedPeriod > 0 ? static_cast<uint32_t>(requestedPeriod) : 0;
        options.captureRaw = attemptRaw;

        bool initialized = false;
        try {
            const DuplexPlan plan = engine.initialize(options);
            report += formatPlan(plan);
            initialized = true;

            if (!noStartTest) {
                report += "\n---- 1 s smoke test (muted output) ----\n";
                engine.setProcessor(&processor);
                engine.start();
                Sleep(1000);
                const EngineSnapshot snap = engine.snapshot();
                engine.stop();
                std::snprintf(buf, sizeof(buf),
                              "capture callbacks: %llu, render callbacks: %llu\n"
                              "frames captured: %llu, frames rendered: %llu\n"
                              "overruns: %llu, underruns (post-warmup): %llu, gaps: %llu frames\n",
                              static_cast<unsigned long long>(snap.captureCallbacks),
                              static_cast<unsigned long long>(snap.renderCallbacks),
                              static_cast<unsigned long long>(snap.framesCaptured),
                              static_cast<unsigned long long>(snap.framesRendered),
                              static_cast<unsigned long long>(snap.overrunFrames),
                              static_cast<unsigned long long>(snap.underrunFrames),
                              static_cast<unsigned long long>(snap.gapFrames));
                report += buf;
                if (snap.renderCallbacks == 0 || snap.captureCallbacks == 0) {
                    report += "smoke test failed: no callbacks received\n";
                    exitCode = 1;
                }
            }
            report += "\nPASS: capture and render streams initialized concurrently.\n";
        } catch (const AncError& e) {
            std::snprintf(buf, sizeof(buf), "\nFAIL: %s: %s\n", e.context.c_str(),
                          e.message.c_str());
            report += buf;
            exitCode = 1;
        }
        if (!initialized) exitCode = 1;
    }

    std::printf("\n%s", report.substr(printedUpTo).c_str());
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(outPath).parent_path(), ec);
    std::ofstream out{std::filesystem::path(outPath)};
    if (out) {
        out << report;
        std::printf("report written to %s\n", toUtf8(outPath).c_str());
    } else {
        std::printf("warning: could not write %s\n", toUtf8(outPath).c_str());
    }
    return exitCode;
}

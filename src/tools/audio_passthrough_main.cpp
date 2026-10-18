// Phase 1 deliverable: audio_passthrough
//
// Full-duplex event-driven WASAPI pipeline: microphone -> mono float DSP ->
// earphones. Modes:
//   passthrough (default) - monitor the mic through the headphones
//   loopback              - emit log chirps and record the mic (Phase 2
//                           acoustic latency cross-check)
//
// Always records metrics counters; optionally writes per-second CSVs, device
// clock anchors and WAV recordings.

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "audio/wasapi_streams.h"
#include "dsp/chirp_emitter.h"
#include "dsp/pass_through.h"
#include "tools/cli.h"
#include "tools/run_reporter.h"

using namespace anc;

namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI consoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_BREAK_EVENT) {
        g_stop.store(true);
        return TRUE;
    }
    return FALSE;
}

void printPlan(const DuplexPlan& plan) {
    std::printf("capture device : %s\n", toUtf8(plan.captureDeviceName).c_str());
    std::printf("render device  : %s\n", toUtf8(plan.renderDeviceName).c_str());
    std::printf("capture format : %s\n", plan.captureFormat.describe().c_str());
    std::printf("render format  : %s\n", plan.renderFormat.describe().c_str());
    std::printf("engine period  : %u frames (%.2f ms)\n", plan.periodFrames,
                plan.renderFormat.sampleRate()
                    ? 1000.0 * plan.periodFrames / plan.renderFormat.sampleRate()
                    : 0.0);
    std::printf("buffers        : capture %u frames, render %u frames\n",
                plan.captureBufferFrames, plan.renderBufferFrames);
    std::printf("stream latency : capture %.2f ms, render %.2f ms\n",
                plan.captureStreamLatencyMs, plan.renderStreamLatencyMs);
    if (!plan.notes.empty()) std::printf("%s", plan.notes.c_str());
}

std::string escapeJson(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += '?';
                } else {
                    out += c;
                }
        }
    }
    return out;
}

void writeRunInfoJson(const std::wstring& path, const DuplexPlan& plan, const RunInfo& info,
                      const std::string& processorName, const std::string& extraFields) {
    if (path.empty()) return;
    std::ofstream out{std::filesystem::path(path)};
    if (!out) return;
    out << "{\n";
    out << "  \"processor\": \"" << escapeJson(processorName) << "\",\n";
    out << "  \"capture_device\": \"" << escapeJson(toUtf8(plan.captureDeviceName)) << "\",\n";
    out << "  \"render_device\": \"" << escapeJson(toUtf8(plan.renderDeviceName)) << "\",\n";
    out << "  \"capture_rate\": " << info.captureRate << ",\n";
    out << "  \"render_rate\": " << info.renderRate << ",\n";
    out << "  \"capture_resampled\": " << (info.captureResampled ? "true" : "false") << ",\n";
    out << "  \"period_frames\": " << plan.periodFrames << ",\n";
    out << "  \"capture_buffer_frames\": " << plan.captureBufferFrames << ",\n";
    out << "  \"render_buffer_frames\": " << plan.renderBufferFrames << ",\n";
    out << "  \"start_qpc_sec\": " << (static_cast<double>(info.startQpc) / qpcFrequency())
        << ",\n";
    out << "  \"capture_first_packet_pos\": "
        << (info.captureFirstValid ? std::to_string(info.captureFirstPacketPos) : "-1") << ",\n";
    out << "  \"render_first_play_pos\": "
        << (info.renderFirstValid ? std::to_string(info.renderFirstPlayPos) : "-1") << ",\n";
    out << "  \"capture_stream_latency_ms\": " << plan.captureStreamLatencyMs << ",\n";
    out << "  \"render_stream_latency_ms\": " << plan.renderStreamLatencyMs << ",\n";
    out << "  \"notes\": \"" << escapeJson(plan.notes) << "\"";
    if (!extraFields.empty()) out << ",\n" << extraFields;
    out << "\n}\n";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetConsoleCtrlHandler(consoleHandler, TRUE);
    const Args args(argc, argv);

    const std::wstring mode = args.get(L"--mode", L"passthrough");
    const double durationRequested = args.getDouble(L"--duration", 10.0);
    const bool captureRaw = args.has(L"--capture-raw");

    DuplexEngine::Options options;
    std::string error;
    options.captureDeviceId =
        selectDeviceId(eCapture, args.get(L"--capture-device"), error);
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
    options.captureRaw = captureRaw;
    options.allowResample = !args.has(L"--no-resample");
    options.recordInputPath = args.get(L"--record-input");
    options.recordOutputPath = args.get(L"--record-output");

    // --- Select the processor. ---
    PassThrough passThrough;
    passThrough.setGain(args.getFloat(L"--gain", 1.0f));
    bool invert = args.has(L"--invert");
    passThrough.setInvert(invert);
    passThrough.setMuted(args.has(L"--mute"));

    ChirpEmitter::Config chirpConfig;
    chirpConfig.intervalSeconds = args.getDouble(L"--chirp-interval", 1.5);
    chirpConfig.chirpSeconds = args.getDouble(L"--chirp-length", 0.2);
    chirpConfig.startHz = args.getDouble(L"--chirp-start-hz", 200.0);
    chirpConfig.endHz = args.getDouble(L"--chirp-end-hz", 6000.0);
    chirpConfig.amplitude = args.getFloat(L"--chirp-amp", 0.1f);
    chirpConfig.initialDelaySeconds = args.getDouble(L"--chirp-delay", 0.5);
    chirpConfig.repeats = args.getInt(L"--chirp-repeats", 20);
    ChirpEmitter chirpEmitter(chirpConfig);

    const bool loopback = (mode == L"loopback");

    double duration = durationRequested;
    if (loopback && args.getDouble(L"--duration", -1.0) < 0.0) {
        duration = chirpConfig.initialDelaySeconds +
                   chirpConfig.intervalSeconds * chirpConfig.repeats + 1.0;
    }

    DuplexEngine engine;
    EngineSnapshot finalSnapshot{};
    RunInfo runInfo{};
    try {
        const DuplexPlan plan = engine.initialize(options);
        printPlan(plan);
        engine.setProcessor(loopback ? static_cast<AudioProcessor*>(&chirpEmitter)
                                     : static_cast<AudioProcessor*>(&passThrough));

        RunReporter reporter;
        RunReporter::Config reporterConfig;
        if (!args.has(L"--no-metrics")) {
            reporterConfig.snapshotCsvPath = args.get(L"--metrics-csv", L"results/latency_report.csv");
        }
        reporterConfig.timingCsvPath = args.get(L"--timing-csv");
        reporterConfig.console = !args.has(L"--quiet");

        std::printf("mode: %s, gain %.3f%s, duration %.1f s\n", loopback ? "loopback" : "pass-through",
                    passThrough.gain(), invert ? ", inverted" : "", duration);
        std::printf("starting streams...\n");

        engine.start();
        if (!reporter.begin(engine, reporterConfig)) {
            std::printf("warning: could not open metrics CSV\n");
        }

        const int64_t t0 = qpcTicks();
        while (!g_stop.load()) {
            const double elapsed = ticksToSeconds(qpcTicks() - t0);
            if (elapsed >= duration) break;
            reporter.tick(engine);
            Sleep(50);
        }
        engine.stop();
        finalSnapshot = engine.snapshot();
        runInfo = engine.runInfo();
        reporter.finish(engine);

        if (loopback) {
            std::printf("chirps emitted: %d / %d\n", chirpEmitter.chirpsEmitted(),
                        chirpEmitter.totalChirps());
        }
    } catch (const AncError& e) {
        std::printf("error: %s: %s\n", e.context.c_str(), e.message.c_str());
        return 1;
    }

    std::string extra;
    if (loopback) {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "  \"chirp\": {\"interval_sec\": %.3f, \"chirp_sec\": %.3f, "
                      "\"start_hz\": %.1f, \"end_hz\": %.1f, \"amplitude\": %.4f, "
                      "\"initial_delay_sec\": %.3f, \"repeats\": %d}",
                      chirpConfig.intervalSeconds, chirpConfig.chirpSeconds, chirpConfig.startHz,
                      chirpConfig.endHz, chirpConfig.amplitude, chirpConfig.initialDelaySeconds,
                      chirpConfig.repeats);
        extra = buf;
    }
    writeRunInfoJson(args.get(L"--info-json"), engine.plan(), runInfo,
                     engine.plan().captureFormat.sampleRate() ? (loopback ? chirpEmitter.name()
                                                                           : passThrough.name())
                                                              : "n/a",
                     extra);

    if (finalSnapshot.elapsedSeconds > 0.0 && finalSnapshot.overrunFrames == 0 &&
        finalSnapshot.underrunFrames == 0) {
        std::printf("result: clean run\n");
        return 0;
    }
    std::printf("result: run had dropouts (see summary above)\n");
    return 2;
}

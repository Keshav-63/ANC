#pragma once

#include <cstdio>
#include <string>

#include "audio/wasapi_streams.h"
#include "core/audio_metrics.h"

namespace anc {

// Per-second metrics CSV + console status + optional device-clock timing
// anchor log. All methods run on the tool's main (monitor) thread.
class RunReporter {
public:
    struct Config {
        std::wstring snapshotCsvPath;  // empty = no per-second CSV
        std::wstring timingCsvPath;    // empty = no timing anchor log
        bool console = true;
    };

    bool begin(DuplexEngine& engine, const Config& config) {
        config_ = config;
        beginQpc_ = qpcTicks();
        lastSnapshotQpc_ = beginQpc_;
        lastTimingQpc_ = beginQpc_;
        last_ = engine.snapshot();

        if (!config_.snapshotCsvPath.empty()) {
            if (!snapshotCsv_.open(config_.snapshotCsvPath)) return false;
            snapshotCsv_.writeLine(std::string("# software-anc metrics: ") +
                                   engine.plan().captureFormat.describe() + " -> " +
                                   engine.plan().renderFormat.describe());
            char buf[256];
            std::snprintf(buf, sizeof(buf), "# period %u frames, capture buffer %u, render buffer %u",
                          engine.plan().periodFrames, engine.plan().captureBufferFrames,
                          engine.plan().renderBufferFrames);
            snapshotCsv_.writeLine(buf);
            snapshotCsv_.writeLine(kSnapshotCsvHeader);
        }
        if (!config_.timingCsvPath.empty()) {
            if (!timingCsv_.open(config_.timingCsvPath)) return false;
            timingCsv_.writeLine("# device clock anchors (QPC seconds since boot)");
            timingCsv_.writeLine("sec,capture_pos_frames,capture_qpc_s,render_pos_frames,render_qpc_s");
        }
        return true;
    }

    // Call roughly 10 times per second.
    void tick(DuplexEngine& engine) {
        const int64_t now = qpcTicks();
        if (timingCsv_.isOpen() && now - lastTimingQpc_ >= secondsToTicks(0.1)) {
            lastTimingQpc_ = now;
            uint64_t capPos = 0, renPos = 0;
            int64_t capQpc = 0, renQpc = 0;
            const bool haveCap = engine.sampleCaptureClock(capPos, capQpc);
            const bool haveRen = engine.sampleRenderClock(renPos, renQpc);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%.3f,%s,%s,%s,%s",
                          ticksToSeconds(now - beginQpc_),
                          haveCap ? std::to_string(capPos).c_str() : "-",
                          haveCap ? std::to_string(ticksToSeconds(capQpc)).c_str() : "-",
                          haveRen ? std::to_string(renPos).c_str() : "-",
                          haveRen ? std::to_string(ticksToSeconds(renQpc)).c_str() : "-");
            timingCsv_.writeLine(buf);
        }

        if (now - lastSnapshotQpc_ >= secondsToTicks(1.0)) {
            lastSnapshotQpc_ = now;
            engine_ = &engine;
            last_ = engine.snapshot();
            if (snapshotCsv_.isOpen()) snapshotCsv_.writeLine(snapshotToCsvRow(last_));
            if (config_.console) printConsoleLine(last_);
        }
    }

    struct Summary {
        double durationSeconds = 0.0;
        EngineSnapshot final{};
        uint64_t inputDropped = 0;
        uint64_t outputDropped = 0;
        bool stable = false;
        std::string text;
    };

    Summary finish(DuplexEngine& engine) {
        Summary summary;
        summary.final = engine.snapshot();
        summary.durationSeconds = summary.final.elapsedSeconds;
        summary.inputDropped = engine.inputDroppedFrames();
        summary.outputDropped = engine.outputDroppedFrames();
        summary.stable = summary.final.overrunFrames == 0 && summary.final.underrunFrames == 0 &&
                         summary.final.gapFrames == 0 &&
                         summary.final.renderMissedDeadlines == 0;
        if (snapshotCsv_.isOpen()) snapshotCsv_.writeLine(snapshotToCsvRow(summary.final));

        char buf[2048];
        std::snprintf(
            buf, sizeof(buf),
            "\n==== run summary ====\n"
            "duration              : %.1f s\n"
            "capture callbacks     : %llu\n"
            "render callbacks      : %llu\n"
            "frames captured       : %llu\n"
            "frames rendered       : %llu\n"
            "overrun frames        : %llu\n"
            "underrun frames       : %llu (post-warmup)\n"
            "warmup underruns      : %llu\n"
            "render missed deadline: %llu (event wake with no free space; benign if tiny)\n"
            "position gaps         : %llu frames\n"
            "discontinuities       : %llu\n"
            "clipped frames        : %llu\n"
            "capture proc time     : avg %.1f us, p99.9 %.1f us, max %.1f us\n"
            "render proc time      : avg %.1f us, p99.9 %.1f us, max %.1f us\n"
            "e2e software estimate : last %.2f ms, median %.2f ms\n"
            "device stream latency : capture %.2f ms, render %.2f ms\n"
            "ring fill at end      : %u frames (%.2f ms)\n"
            "record drops          : input %llu frames, output %llu frames\n"
            "verdict               : %s\n",
            summary.durationSeconds,
            static_cast<unsigned long long>(summary.final.captureCallbacks),
            static_cast<unsigned long long>(summary.final.renderCallbacks),
            static_cast<unsigned long long>(summary.final.framesCaptured),
            static_cast<unsigned long long>(summary.final.framesRendered),
            static_cast<unsigned long long>(summary.final.overrunFrames),
            static_cast<unsigned long long>(summary.final.underrunFrames),
            static_cast<unsigned long long>(summary.final.warmupUnderrunFrames),
            static_cast<unsigned long long>(summary.final.renderMissedDeadlines),
            static_cast<unsigned long long>(summary.final.gapFrames),
            static_cast<unsigned long long>(summary.final.discontinuities),
            static_cast<unsigned long long>(summary.final.clipFrames),
            summary.final.captureProcAvgUs, summary.final.captureProcP999Us,
            summary.final.captureProcMaxUs,
            summary.final.renderProcAvgUs, summary.final.renderProcP999Us,
            summary.final.renderProcMaxUs,
            summary.final.e2eEstimateMs, summary.final.e2eMedianMs,
            summary.final.captureStreamLatencyMs, summary.final.renderStreamLatencyMs,
            summary.final.ringFillFrames, summary.final.ringFillMs,
            static_cast<unsigned long long>(summary.inputDropped),
            static_cast<unsigned long long>(summary.outputDropped),
            summary.stable ? "STABLE (no dropouts, no gaps)"
                           : "UNSTABLE (dropouts/gaps detected - try a larger --period)");
        summary.text = buf;
        std::printf("%s", summary.text.c_str());

        snapshotCsv_.close();
        timingCsv_.close();
        return summary;
    }

private:
    static void printConsoleLine(const EngineSnapshot& s) {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "[%6.1fs] cap %lluf/s  ren %lluf/s  ring %uf (%.1fms)  ovr %llu  unr %llu  "
                      "disc %llu  proc cap p99.9 %.0fus ren p99.9 %.0fus  e2e %s\n",
                      s.elapsedSeconds,
                      static_cast<unsigned long long>(s.framesCaptured),
                      static_cast<unsigned long long>(s.framesRendered), s.ringFillFrames,
                      s.ringFillMs,
                      static_cast<unsigned long long>(s.overrunFrames),
                      static_cast<unsigned long long>(s.underrunFrames),
                      static_cast<unsigned long long>(s.discontinuities), s.captureProcP999Us,
                      s.renderProcP999Us,
                      s.e2eEstimateMs >= 0.0 ? (std::to_string(s.e2eEstimateMs) + "ms").c_str()
                                             : "n/a");
        std::printf("%s", buf);
        std::fflush(stdout);
    }

    Config config_{};
    CsvLogger snapshotCsv_;
    CsvLogger timingCsv_;
    DuplexEngine* engine_ = nullptr;
    EngineSnapshot last_{};
    int64_t beginQpc_ = 0;
    int64_t lastSnapshotQpc_ = 0;
    int64_t lastTimingQpc_ = 0;
};

}  // namespace anc

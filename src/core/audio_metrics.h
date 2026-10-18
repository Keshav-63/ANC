#pragma once

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace anc {

// ---------------------------------------------------------------------------
// High-resolution clock helpers (QueryPerformanceCounter domain)
// ---------------------------------------------------------------------------

inline int64_t qpcFrequency() {
    static const int64_t freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<int64_t>(f.QuadPart);
    }();
    return freq;
}

inline int64_t qpcTicks() {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return static_cast<int64_t>(t.QuadPart);
}

inline double ticksToSeconds(int64_t ticks) {
    return static_cast<double>(ticks) / static_cast<double>(qpcFrequency());
}

inline int64_t secondsToTicks(double seconds) {
    return static_cast<int64_t>(seconds * static_cast<double>(qpcFrequency()));
}

// ---------------------------------------------------------------------------
// Fixed-bucket latency histogram (allocation-free, safe from audio threads)
// ---------------------------------------------------------------------------

class LatencyHistogram {
public:
    static constexpr size_t kBuckets = 20000;      // 0 .. 10 ms
    static constexpr double kBucketWidthUs = 0.5;  // 0.5 us per bucket

    void record(int64_t ticks) {
        const double us = ticksToSeconds(ticks) * 1e6;
        size_t b = us <= 0.0 ? 0 : static_cast<size_t>(us / kBucketWidthUs);
        if (b < kBuckets) {
            counts_[b].fetch_add(1, std::memory_order_relaxed);
        } else {
            overflow_.fetch_add(1, std::memory_order_relaxed);
        }
        totalCount_.fetch_add(1, std::memory_order_relaxed);
        totalTicks_.fetch_add(static_cast<uint64_t>(ticks), std::memory_order_relaxed);
        uint64_t prevMax = maxTicks_.load(std::memory_order_relaxed);
        while (static_cast<uint64_t>(ticks) > prevMax &&
               !maxTicks_.compare_exchange_weak(prevMax, static_cast<uint64_t>(ticks),
                                                std::memory_order_relaxed)) {
        }
    }

    uint64_t count() const { return totalCount_.load(std::memory_order_relaxed); }

    double averageUs() const {
        const uint64_t n = count();
        if (n == 0) return 0.0;
        return ticksToSeconds(static_cast<int64_t>(totalTicks_.load(std::memory_order_relaxed))) /
               static_cast<double>(n) * 1e6;
    }

    double maxUs() const {
        return ticksToSeconds(static_cast<int64_t>(maxTicks_.load(std::memory_order_relaxed))) * 1e6;
    }

    // Percentile in [0,1]. Scans the buckets; safe while recording.
    double percentileUs(double p) const {
        const uint64_t n = count();
        if (n == 0) return 0.0;
        const uint64_t target = static_cast<uint64_t>(
            static_cast<double>(n) * p < 1.0 ? 1.0 : static_cast<double>(n) * p);
        uint64_t acc = 0;
        for (size_t b = 0; b < kBuckets; ++b) {
            acc += counts_[b].load(std::memory_order_relaxed);
            if (acc >= target) return static_cast<double>(b) * kBucketWidthUs;
        }
        return 10000.0;  // everything in overflow
    }

private:
    std::array<std::atomic<uint32_t>, kBuckets> counts_{};
    std::atomic<uint64_t> overflow_{0};
    std::atomic<uint64_t> totalCount_{0};
    std::atomic<uint64_t> totalTicks_{0};
    std::atomic<uint64_t> maxTicks_{0};
};

// ---------------------------------------------------------------------------
// Engine snapshot: plain data filled by the engine, formatted by reporters
// ---------------------------------------------------------------------------

struct EngineSnapshot {
    double elapsedSeconds = 0.0;

    uint64_t captureCallbacks = 0;
    uint64_t renderCallbacks = 0;
    uint64_t framesCaptured = 0;
    uint64_t framesRendered = 0;

    uint64_t overrunFrames = 0;         // captured frames dropped (ring full)
    uint64_t underrunFrames = 0;        // silence inserted after warm-up
    uint64_t warmupUnderrunFrames = 0;  // silence inserted while priming
    uint64_t gapFrames = 0;             // device position jumps
    uint64_t discontinuities = 0;       // AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY
    uint64_t silencePackets = 0;        // AUDCLNT_BUFFERFLAGS_SILENT
    uint64_t clipFrames = 0;            // |sample| > 1.0 handed to the device
    uint64_t renderMissedDeadlines = 0; // no room in the render buffer

    uint32_t ringFillFrames = 0;
    double ringFillMs = 0.0;

    double captureProcAvgUs = 0.0;
    double captureProcP999Us = 0.0;
    double captureProcMaxUs = 0.0;
    double renderProcAvgUs = 0.0;
    double renderProcP999Us = 0.0;
    double renderProcMaxUs = 0.0;

    double e2eEstimateMs = -1.0;  // last software input->output estimate
    double e2eMedianMs = -1.0;    // rolling median of recent estimates

    double captureStreamLatencyMs = 0.0;
    double renderStreamLatencyMs = 0.0;

    uint32_t captureBufferFrames = 0;
    uint32_t renderBufferFrames = 0;
};

inline const char* kSnapshotCsvHeader =
    "sec,capture_callbacks,render_callbacks,frames_captured,frames_rendered,"
    "overrun_frames,underrun_frames,warmup_underrun_frames,gap_frames,discontinuities,"
    "silence_packets,clip_frames,render_missed_deadlines,ring_fill_frames,ring_fill_ms,"
    "capture_proc_avg_us,capture_proc_p999_us,capture_proc_max_us,"
    "render_proc_avg_us,render_proc_p999_us,render_proc_max_us,"
    "e2e_ms,e2e_med_ms,capture_latency_ms,render_latency_ms";

inline std::string snapshotToCsvRow(const EngineSnapshot& s) {
    char buf[1024];
    std::snprintf(
        buf, sizeof(buf),
        "%.3f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%.3f,"
        "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.3f,%.3f,%.3f,%.3f",
        s.elapsedSeconds,
        static_cast<unsigned long long>(s.captureCallbacks),
        static_cast<unsigned long long>(s.renderCallbacks),
        static_cast<unsigned long long>(s.framesCaptured),
        static_cast<unsigned long long>(s.framesRendered),
        static_cast<unsigned long long>(s.overrunFrames),
        static_cast<unsigned long long>(s.underrunFrames),
        static_cast<unsigned long long>(s.warmupUnderrunFrames),
        static_cast<unsigned long long>(s.gapFrames),
        static_cast<unsigned long long>(s.discontinuities),
        static_cast<unsigned long long>(s.silencePackets),
        static_cast<unsigned long long>(s.clipFrames),
        static_cast<unsigned long long>(s.renderMissedDeadlines),
        s.ringFillFrames, s.ringFillMs,
        s.captureProcAvgUs, s.captureProcP999Us, s.captureProcMaxUs,
        s.renderProcAvgUs, s.renderProcP999Us, s.renderProcMaxUs,
        s.e2eEstimateMs, s.e2eMedianMs,
        s.captureStreamLatencyMs, s.renderStreamLatencyMs);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Simple CSV logger (non-real-time threads only)
// ---------------------------------------------------------------------------

class CsvLogger {
public:
    ~CsvLogger() { close(); }

    bool open(const std::wstring& path) {
        close();
        file_ = _wfopen(path.c_str(), L"wb");
        return file_ != nullptr;
    }

    bool isOpen() const { return file_ != nullptr; }

    void writeLine(const std::string& line) {
        if (!file_) return;
        std::fwrite(line.data(), 1, line.size(), file_);
        std::fputc('\n', file_);
        std::fflush(file_);
    }

    void close() {
        if (file_) {
            std::fclose(file_);
            file_ = nullptr;
        }
    }

private:
    FILE* file_ = nullptr;
};

}  // namespace anc

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "audio/device_probe.h"
#include "audio/record_sink.h"
#include "audio/wasapi_utils.h"
#include "core/audio_metrics.h"
#include "core/audio_processor.h"
#include "core/ring_buffer.h"

struct IMMDevice;
struct IMMDeviceEnumerator;
struct IAudioClient;
struct IAudioClient3;
struct IAudioCaptureClient;
struct IAudioRenderClient;
struct IAudioClock;
struct IAudioClock2;

namespace anc {

// The negotiated duplex configuration (formats, periods, devices).
struct DuplexPlan {
    std::wstring captureDeviceId;
    std::wstring captureDeviceName;
    std::wstring renderDeviceId;
    std::wstring renderDeviceName;

    WaveFormat captureFormat;  // format the capture client opens with
    WaveFormat renderFormat;   // format the render client opens with
    uint32_t periodFrames = 0;   // engine period (frames)
    uint32_t captureBufferFrames = 0;
    uint32_t renderBufferFrames = 0;

    bool captureUsesConversion = false;  // AUTOCONVERTPCM active on capture
    bool captureRawMode = false;         // raw capture mode active
    bool client3Used = false;            // IAudioClient3 low-latency path

    uint32_t captureMinPeriod = 0, captureMaxPeriod = 0, captureDefaultPeriod = 0;
    uint32_t renderMinPeriod = 0, renderMaxPeriod = 0, renderDefaultPeriod = 0;

    double captureStreamLatencyMs = 0.0;
    double renderStreamLatencyMs = 0.0;

    std::string notes;  // human-readable negotiation log (multi-line)
};

// Immutable per-run facts used to map WAV samples onto device clock times.
struct RunInfo {
    bool valid = false;
    int64_t startQpc = 0;
    double captureRate = 0.0;
    double renderRate = 0.0;
    bool captureResampled = false;
    uint64_t captureFirstPacketPos = 0;  // device position of first captured frame
    uint64_t renderFirstPlayPos = 0;     // device position of first rendered frame
    bool captureFirstValid = false;
    bool renderFirstValid = false;
};

class DuplexEngine {
public:
    struct Options {
        std::wstring captureDeviceId;  // empty = default device
        std::wstring renderDeviceId;   // empty = default device
        uint32_t periodFrames = 0;     // 0 = lowest common stable period
        bool captureRaw = false;       // request raw (effect-bypassed) capture
        bool allowResample = true;     // allow AUTOCONVERTPCM if rates differ
        std::wstring recordInputPath;  // optional WAV of the mono capture signal
        std::wstring recordOutputPath; // optional WAV of the mono processed output
    };

    DuplexEngine();
    ~DuplexEngine();

    DuplexEngine(const DuplexEngine&) = delete;
    DuplexEngine& operator=(const DuplexEngine&) = delete;

    // Enumerates devices, plans formats/periods and initializes both streams.
    // Throws AncError on failure.
    DuplexPlan initialize(const Options& options);

    // Must be called before start(). Processor runs on the render thread.
    void setProcessor(AudioProcessor* processor);

    void start();
    void stop();

    const DuplexPlan& plan() const { return plan_; }
    RunInfo runInfo() const;

    EngineSnapshot snapshot() const;
    double elapsedSeconds() const;

    // Device clock sampling (monitor thread; used for timing logs/analysis).
    bool sampleCaptureClock(uint64_t& posFrames, int64_t& qpc) const;
    bool sampleRenderClock(uint64_t& posFrames, int64_t& qpc) const;

    uint64_t inputDroppedFrames() const { return inputSink_ ? inputSink_->droppedFrames() : 0; }
    uint64_t outputDroppedFrames() const { return outputSink_ ? outputSink_->droppedFrames() : 0; }

private:
    struct CaptureAnchor {
        uint64_t pushedBefore = 0;  // stream frame index of first frame pushed
        uint32_t framesTotal = 0;   // frames in the captured packet
        uint64_t endPos = 0;        // device position just after the packet
        int64_t qpcEnd = 0;         // QPC when the packet end was available
    };
    struct RenderAnchor {
        uint64_t inputIndex = 0;  // input stream frame index of first written frame
        uint64_t playPos = 0;     // device position at which it will play
        int64_t playQpc = 0;      // QPC at which it will play
        bool hasInput = false;
    };

    template <typename T, size_t N>
    struct AnchorLog {
        void push(const T& v) {
            const size_t i = writeCount_.load(std::memory_order_relaxed);
            entries_[i % N] = v;
            writeCount_.store(i + 1, std::memory_order_release);
        }
        // Copies the most recent `maxItems` entries (oldest first). Returns count.
        size_t latest(T* out, size_t maxItems) const {
            const size_t count = writeCount_.load(std::memory_order_acquire);
            const size_t n = count < maxItems ? count : maxItems;
            for (size_t k = 0; k < n; ++k) {
                out[k] = entries_[(count - n + k) % N];
            }
            return n;
        }
        std::atomic<size_t> writeCount_{0};
        T entries_[N]{};
    };

    void captureThreadProc();
    void renderThreadProc();
    void releaseInterfaces();

    static void sampleClockImpl(void* clock2Ptr, void* clockPtr, bool& ok, uint64_t& pos,
                                int64_t& qpc);

    // Negotiation helpers (setup path).
    DuplexPlan planStreams(const Options& options);
    bool tryOpenCapture(const Options& options, uint32_t periodFrames, bool raw,
                        const WaveFormat& format, bool allowConversion, std::string& errorOut);
    bool tryOpenRender(uint32_t periodFrames, std::string& errorOut);

    double estimateE2eMs(RenderAnchor& usedAnchor, CaptureAnchor& usedCapture) const;

    Options options_{};
    DuplexPlan plan_{};

    // COM objects (raw pointers managed in releaseInterfaces()).
    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* captureDevice_ = nullptr;
    IMMDevice* renderDevice_ = nullptr;
    IAudioClient* captureClient_ = nullptr;
    IAudioClient3* captureClient3_ = nullptr;
    IAudioCaptureClient* capture_ = nullptr;
    IAudioClock* captureClock_ = nullptr;
    IAudioClock2* captureClock2_ = nullptr;
    IAudioClient* renderClient_ = nullptr;
    IAudioClient3* renderClient3_ = nullptr;
    IAudioRenderClient* render_ = nullptr;
    IAudioClock* renderClock_ = nullptr;
    IAudioClock2* renderClock2_ = nullptr;

    HANDLE captureEvent_ = nullptr;
    HANDLE renderEvent_ = nullptr;
    HANDLE shutdownEvent_ = nullptr;

    std::thread captureThread_;
    std::thread renderThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> startedFlag_{false};
    int64_t startQpc_ = 0;

    AudioProcessor* processor_ = nullptr;
    uint32_t periodFrames_ = 0;

    // Data path.
    SpscRingBuffer<float> inputRing_{1 << 15};
    std::vector<float> captureScratch_;   // sized to capture buffer frames
    std::vector<float> renderScratchIn_;  // sized to render buffer frames
    std::vector<float> renderScratchOut_;

    std::unique_ptr<RecordSink> inputSink_;
    std::unique_ptr<RecordSink> outputSink_;

    // Metrics (written by audio threads).
    std::atomic<uint64_t> captureCallbacks_{0};
    std::atomic<uint64_t> renderCallbacks_{0};
    std::atomic<uint64_t> framesCaptured_{0};
    std::atomic<uint64_t> framesRendered_{0};
    std::atomic<uint64_t> overrunFrames_{0};
    std::atomic<uint64_t> underrunFrames_{0};
    std::atomic<uint64_t> warmupUnderrunFrames_{0};
    std::atomic<uint64_t> gapFrames_{0};
    std::atomic<uint64_t> discontinuities_{0};
    std::atomic<uint64_t> silencePackets_{0};
    std::atomic<uint64_t> clipFrames_{0};
    std::atomic<uint64_t> renderMissedDeadlines_{0};
    std::atomic<uint64_t> pushedTotal_{0};   // capture stream frames written to ring
    std::atomic<uint64_t> inputReadTotal_{0}; // input stream frames consumed by render
    std::atomic<bool> warmupDone_{false};
    std::atomic<uint32_t> warmupGoodPackets_{0};
    std::atomic<uint64_t> expectedCapPos_{0};
    std::atomic<bool> expectedCapValid_{false};
    std::atomic<uint64_t> captureFirstPos_{0};
    std::atomic<uint64_t> renderFirstPlayPos_{0};
    std::atomic<bool> captureFirstValid_{false};
    std::atomic<bool> renderFirstValid_{false};

    LatencyHistogram captureProcHist_;
    LatencyHistogram renderProcHist_;

    AnchorLog<CaptureAnchor, 512> captureAnchors_;
    AnchorLog<RenderAnchor, 512> renderAnchors_;

    // Rolling e2e estimates (monitor thread only).
    mutable double e2eRecent_[64]{};
    mutable size_t e2eRecentCount_ = 0;
    mutable size_t e2eRecentNext_ = 0;
};

}  // namespace anc

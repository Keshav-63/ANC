// DuplexEngine runtime path: stream start/stop, the two real-time threads,
// metrics and the software latency estimator.

#include "audio/wasapi_streams.h"

#include <audioclient.h>
#include <avrt.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace anc {

void DuplexEngine::start() {
    if (startedFlag_.load(std::memory_order_acquire)) return;

    if (processor_) {
        processor_->prepare(static_cast<double>(plan_.renderFormat.sampleRate()),
                            plan_.renderBufferFrames);
    }

    // Prime the render buffer with silence so the device never starves before
    // the first captured frames arrive.
    {
        BYTE* buffer = nullptr;
        if (SUCCEEDED(render_->GetBuffer(plan_.renderBufferFrames, &buffer))) {
            std::memset(buffer, 0, static_cast<size_t>(plan_.renderBufferFrames) *
                                       plan_.renderFormat.frameSize());
            render_->ReleaseBuffer(plan_.renderBufferFrames, 0);
        }
    }

    stopRequested_.store(false, std::memory_order_relaxed);
    warmupDone_.store(false, std::memory_order_relaxed);
    warmupGoodPackets_.store(0, std::memory_order_relaxed);
    startQpc_ = qpcTicks();

    // Start the servicing threads before the streams so no event is missed.
    captureThread_ = std::thread([this] { captureThreadProc(); });
    renderThread_ = std::thread([this] { renderThreadProc(); });

    HRESULT hr = renderClient_->Start();
    if (FAILED(hr)) {
        stopRequested_.store(true, std::memory_order_release);
        SetEvent(shutdownEvent_);
        SetEvent(shutdownEvent_);
        if (captureThread_.joinable()) captureThread_.join();
        if (renderThread_.joinable()) renderThread_.join();
        throw AncError{hr, "render Start", hrToString(hr)};
    }
    hr = captureClient_->Start();
    if (FAILED(hr)) {
        renderClient_->Stop();
        stopRequested_.store(true, std::memory_order_release);
        SetEvent(shutdownEvent_);
        SetEvent(shutdownEvent_);
        if (captureThread_.joinable()) captureThread_.join();
        if (renderThread_.joinable()) renderThread_.join();
        throw AncError{hr, "capture Start", hrToString(hr)};
    }

    running_.store(true, std::memory_order_release);
    startedFlag_.store(true, std::memory_order_release);
}

void DuplexEngine::stop() {
    const bool threadsAlive = captureThread_.joinable() || renderThread_.joinable();
    if (!threadsAlive && !startedFlag_.load(std::memory_order_acquire)) return;

    stopRequested_.store(true, std::memory_order_release);
    if (shutdownEvent_) {
        SetEvent(shutdownEvent_);
        SetEvent(shutdownEvent_);  // auto-reset event, one per waiting thread
    }
    if (captureThread_.joinable()) captureThread_.join();
    if (renderThread_.joinable()) renderThread_.join();

    if (startedFlag_.load(std::memory_order_acquire)) {
        if (captureClient_) captureClient_->Stop();
        if (renderClient_) renderClient_->Stop();
    }
    running_.store(false, std::memory_order_relaxed);

    if (inputSink_) inputSink_->close();
    if (outputSink_) outputSink_->close();
}

void DuplexEngine::captureThreadProc() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
    if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);

    HANDLE events[2] = {captureEvent_, shutdownEvent_};
    const WaveFormat& fmt = plan_.captureFormat;

    while (!stopRequested_.load(std::memory_order_acquire)) {
        const DWORD wait = WaitForMultipleObjects(2, events, FALSE, 2000);
        if (wait == WAIT_OBJECT_0 + 1) break;
        if (wait == WAIT_TIMEOUT) continue;

        const int64_t t0 = qpcTicks();
        UINT32 packetFrames = 0;
        while (!stopRequested_.load(std::memory_order_relaxed) &&
               SUCCEEDED(capture_->GetNextPacketSize(&packetFrames)) && packetFrames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devicePos = 0;
            // NOTE argument order: (ppData, pNumFramesToRead, pdwFlags,
            // pu64DevicePosition, pu64QPCPosition).
            if (FAILED(capture_->GetBuffer(&data, &frames, &flags, &devicePos, nullptr))) break;
            const int64_t qpcNow = qpcTicks();

            if (frames > captureScratch_.size()) frames = static_cast<UINT32>(captureScratch_.size());

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                std::memset(captureScratch_.data(), 0, frames * sizeof(float));
                silencePackets_.fetch_add(1, std::memory_order_relaxed);
            } else {
                convertToMono(data, frames, fmt, captureScratch_.data());
            }
            if (inputSink_) inputSink_->push(captureScratch_.data(), frames);

            // Gap detection on the device position timeline.
            if (!expectedCapValid_.load(std::memory_order_relaxed)) {
                expectedCapPos_.store(devicePos + frames, std::memory_order_relaxed);
                expectedCapValid_.store(true, std::memory_order_relaxed);
            } else {
                const uint64_t expected = expectedCapPos_.load(std::memory_order_relaxed);
                if (devicePos > expected) {
                    gapFrames_.fetch_add(devicePos - expected, std::memory_order_relaxed);
                }
                expectedCapPos_.store(devicePos + frames, std::memory_order_relaxed);
            }
            if (!captureFirstValid_.load(std::memory_order_relaxed)) {
                captureFirstPos_.store(devicePos, std::memory_order_relaxed);
                captureFirstValid_.store(true, std::memory_order_relaxed);
            }
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                discontinuities_.fetch_add(1, std::memory_order_relaxed);
            }

            const uint64_t pushedBefore = pushedTotal_.load(std::memory_order_relaxed);
            const size_t wrote = inputRing_.write(captureScratch_.data(), frames);
            if (wrote < frames) {
                overrunFrames_.fetch_add(frames - wrote, std::memory_order_relaxed);
            }
            pushedTotal_.store(pushedBefore + wrote, std::memory_order_release);

            CaptureAnchor anchor{};
            anchor.pushedBefore = pushedBefore;
            anchor.framesTotal = frames;
            anchor.endPos = devicePos + frames;
            anchor.qpcEnd = qpcNow;
            captureAnchors_.push(anchor);

            framesCaptured_.fetch_add(frames, std::memory_order_relaxed);
            capture_->ReleaseBuffer(frames);
        }
        captureCallbacks_.fetch_add(1, std::memory_order_relaxed);
        captureProcHist_.record(qpcTicks() - t0);
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
}

void DuplexEngine::renderThreadProc() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
    if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);

    HANDLE events[2] = {renderEvent_, shutdownEvent_};
    const double rate = static_cast<double>(plan_.renderFormat.sampleRate());
    const double ticksPerFrame = rate > 0.0 ? static_cast<double>(qpcFrequency()) / rate : 0.0;

    while (!stopRequested_.load(std::memory_order_acquire)) {
        const DWORD wait = WaitForMultipleObjects(2, events, FALSE, 2000);
        if (wait == WAIT_OBJECT_0 + 1) break;
        if (wait == WAIT_TIMEOUT) continue;

        const int64_t t0 = qpcTicks();

        UINT32 padding = 0;
        if (FAILED(renderClient_->GetCurrentPadding(&padding))) break;
        const UINT32 avail = plan_.renderBufferFrames - padding;
        if (avail == 0) {
            renderMissedDeadlines_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        const uint64_t inputIndex = inputReadTotal_.load(std::memory_order_relaxed);
        const size_t got = inputRing_.read(renderScratchIn_.data(), avail);
        if (got < avail) {
            std::memset(renderScratchIn_.data() + got, 0, (avail - got) * sizeof(float));
            if (warmupDone_.load(std::memory_order_relaxed)) {
                underrunFrames_.fetch_add(avail - got, std::memory_order_relaxed);
            } else {
                warmupUnderrunFrames_.fetch_add(avail - got, std::memory_order_relaxed);
            }
        }
        inputReadTotal_.store(inputIndex + got, std::memory_order_release);

        if (processor_) {
            processor_->process(renderScratchIn_.data(), renderScratchOut_.data(), avail);
        } else {
            std::memcpy(renderScratchOut_.data(), renderScratchIn_.data(), avail * sizeof(float));
        }

        uint64_t clips = 0;
        for (UINT32 i = 0; i < avail; ++i) {
            if (std::fabs(renderScratchOut_[i]) > 1.0f) ++clips;
        }
        if (clips) clipFrames_.fetch_add(clips, std::memory_order_relaxed);

        if (outputSink_) outputSink_->push(renderScratchOut_.data(), avail);

        BYTE* buffer = nullptr;
        if (FAILED(render_->GetBuffer(avail, &buffer))) break;
        convertFromMono(renderScratchOut_.data(), avail, plan_.renderFormat, buffer);

        // Sample the render clock to record when this block will play out.
        uint64_t renderPos = 0;
        int64_t renderQpc = 0;
        if (sampleRenderClock(renderPos, renderQpc)) {
            RenderAnchor anchor{};
            anchor.playPos = renderPos + padding;
            anchor.playQpc = renderQpc + static_cast<int64_t>(static_cast<double>(padding) * ticksPerFrame);
            if (got > 0) {
                anchor.hasInput = true;
                anchor.inputIndex = inputIndex;
            }
            if (!renderFirstValid_.load(std::memory_order_relaxed)) {
                renderFirstPlayPos_.store(anchor.playPos, std::memory_order_relaxed);
                renderFirstValid_.store(true, std::memory_order_relaxed);
            }
            renderAnchors_.push(anchor);
        }

        render_->ReleaseBuffer(avail, 0);
        framesRendered_.fetch_add(avail, std::memory_order_relaxed);
        renderCallbacks_.fetch_add(1, std::memory_order_relaxed);
        renderProcHist_.record(qpcTicks() - t0);

        if (!warmupDone_.load(std::memory_order_relaxed) && got == avail) {
            if (warmupGoodPackets_.fetch_add(1, std::memory_order_relaxed) >= 50) {
                warmupDone_.store(true, std::memory_order_relaxed);
            }
        }
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
}

double DuplexEngine::estimateE2eMs(RenderAnchor& usedAnchor, CaptureAnchor& usedCapture) const {
    RenderAnchor recent[32];
    const size_t renderCount = renderAnchors_.latest(recent, 32);
    bool foundRender = false;
    for (size_t i = renderCount; i-- > 0;) {
        if (recent[i].hasInput) {
            usedAnchor = recent[i];
            foundRender = true;
            break;
        }
    }
    if (!foundRender) return -1.0;

    CaptureAnchor anchors[256];
    const size_t captureCount = captureAnchors_.latest(anchors, 256);
    const CaptureAnchor* chosen = nullptr;
    for (size_t i = captureCount; i-- > 0;) {
        if (anchors[i].pushedBefore <= usedAnchor.inputIndex) {
            chosen = &anchors[i];
            break;
        }
    }
    if (!chosen) return -1.0;
    usedCapture = *chosen;

    const double rate = static_cast<double>(plan_.captureFormat.sampleRate());
    if (rate <= 0.0) return -1.0;
    const double ticksPerFrame = static_cast<double>(qpcFrequency()) / rate;

    const double offsetInPacket =
        static_cast<double>(usedAnchor.inputIndex - chosen->pushedBefore);
    const double lagFrames = static_cast<double>(chosen->framesTotal) - offsetInPacket;
    const double captureQpc = static_cast<double>(chosen->qpcEnd) - lagFrames * ticksPerFrame;

    const double e2eTicks = static_cast<double>(usedAnchor.playQpc) - captureQpc;
    return ticksToSeconds(static_cast<int64_t>(e2eTicks)) * 1000.0;
}

EngineSnapshot DuplexEngine::snapshot() const {
    EngineSnapshot s;
    s.elapsedSeconds =
        startQpc_ != 0 ? ticksToSeconds(qpcTicks() - startQpc_) : 0.0;

    s.captureCallbacks = captureCallbacks_.load(std::memory_order_relaxed);
    s.renderCallbacks = renderCallbacks_.load(std::memory_order_relaxed);
    s.framesCaptured = framesCaptured_.load(std::memory_order_relaxed);
    s.framesRendered = framesRendered_.load(std::memory_order_relaxed);
    s.overrunFrames = overrunFrames_.load(std::memory_order_relaxed);
    s.underrunFrames = underrunFrames_.load(std::memory_order_relaxed);
    s.warmupUnderrunFrames = warmupUnderrunFrames_.load(std::memory_order_relaxed);
    s.gapFrames = gapFrames_.load(std::memory_order_relaxed);
    s.discontinuities = discontinuities_.load(std::memory_order_relaxed);
    s.silencePackets = silencePackets_.load(std::memory_order_relaxed);
    s.clipFrames = clipFrames_.load(std::memory_order_relaxed);
    s.renderMissedDeadlines = renderMissedDeadlines_.load(std::memory_order_relaxed);

    s.ringFillFrames = static_cast<uint32_t>(inputRing_.availableToRead());
    const double renderRate = static_cast<double>(plan_.renderFormat.sampleRate());
    s.ringFillMs = renderRate > 0.0 ? 1000.0 * s.ringFillFrames / renderRate : 0.0;

    s.captureProcAvgUs = captureProcHist_.averageUs();
    s.captureProcP999Us = captureProcHist_.percentileUs(0.999);
    s.captureProcMaxUs = captureProcHist_.maxUs();
    s.renderProcAvgUs = renderProcHist_.averageUs();
    s.renderProcP999Us = renderProcHist_.percentileUs(0.999);
    s.renderProcMaxUs = renderProcHist_.maxUs();

    s.captureStreamLatencyMs = plan_.captureStreamLatencyMs;
    s.renderStreamLatencyMs = plan_.renderStreamLatencyMs;
    s.captureBufferFrames = plan_.captureBufferFrames;
    s.renderBufferFrames = plan_.renderBufferFrames;

    RenderAnchor anchor{};
    CaptureAnchor captureAnchor{};
    s.e2eEstimateMs = estimateE2eMs(anchor, captureAnchor);
    if (s.e2eEstimateMs >= 0.0) {
        e2eRecent_[e2eRecentNext_] = s.e2eEstimateMs;
        e2eRecentNext_ = (e2eRecentNext_ + 1) % std::size(e2eRecent_);
        if (e2eRecentCount_ < std::size(e2eRecent_)) ++e2eRecentCount_;

        double sorted[64];
        std::copy(e2eRecent_, e2eRecent_ + e2eRecentCount_, sorted);
        std::sort(sorted, sorted + e2eRecentCount_);
        s.e2eMedianMs = sorted[e2eRecentCount_ / 2];
    }
    return s;
}

}  // namespace anc

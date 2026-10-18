#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "audio/wav_writer.h"
#include "core/ring_buffer.h"

namespace anc {

// Real-time -> disk bridge. The audio thread pushes mono float frames
// (non-blocking, lock-free ring); a dedicated writer thread drains them into
// a float32 WAV file. Frames are dropped (and counted) if the writer thread
// cannot keep up, so the audio path never blocks on disk I/O.
class RecordSink {
public:
    RecordSink() : ring_(1 << 18) {}  // ~5.4 s at 48 kHz
    ~RecordSink() { close(); }

    RecordSink(const RecordSink&) = delete;
    RecordSink& operator=(const RecordSink&) = delete;

    bool open(const std::wstring& path, uint32_t sampleRate) {
        if (!writer_.open(path, sampleRate, 1)) return false;
        stop_.store(false, std::memory_order_relaxed);
        drainEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        thread_ = std::thread([this] { writerLoop(); });
        return true;
    }

    // Real-time safe.
    void push(const float* data, uint32_t frames) {
        const size_t written = ring_.write(data, frames);
        if (written < frames) dropped_ += (frames - written);
        if (written > 0 && drainEvent_) SetEvent(drainEvent_);
    }

    void close() {
        if (thread_.joinable()) {
            stop_.store(true, std::memory_order_relaxed);
            if (drainEvent_) SetEvent(drainEvent_);
            thread_.join();
        }
        drainRing();
        writer_.close();
        if (drainEvent_) {
            CloseHandle(drainEvent_);
            drainEvent_ = nullptr;
        }
    }

    uint64_t droppedFrames() const { return dropped_; }
    uint64_t framesWritten() const { return writer_.framesWritten(); }
    bool isOpen() const { return thread_.joinable(); }

private:
    void drainRing() {
        float buf[16384];
        size_t n;
        while ((n = ring_.read(buf, std::size(buf))) > 0) {
            writer_.writeFloat32(buf, n);
        }
    }

    void writerLoop() {
        std::vector<float> buf(16384);
        while (!stop_.load(std::memory_order_relaxed)) {
            size_t n = ring_.read(buf.data(), buf.size());
            if (n == 0) {
                WaitForSingleObject(drainEvent_, 50);
                continue;
            }
            writer_.writeFloat32(buf.data(), n);
        }
        drainRing();
    }

    SpscRingBuffer<float> ring_;
    WavWriter writer_;
    std::thread thread_;
    HANDLE drainEvent_ = nullptr;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> dropped_{0};
};

}  // namespace anc

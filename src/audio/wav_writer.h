#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace anc {

// Streaming float32 WAV writer. Sizes in the RIFF header are patched on
// close(). Not real-time safe: use from a dedicated writer thread.
class WavWriter {
public:
    WavWriter() = default;
    ~WavWriter() { close(); }

    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    bool open(const std::wstring& path, uint32_t sampleRate, uint16_t channels = 1);
    void writeFloat32(const float* data, size_t frames);
    void close();

    uint64_t framesWritten() const { return framesWritten_; }
    bool isOpen() const { return file_ != nullptr; }

private:
    void writeHeader();

    std::FILE* file_ = nullptr;
    uint32_t sampleRate_ = 0;
    uint16_t channels_ = 1;
    uint64_t framesWritten_ = 0;
    bool headerPatched_ = false;
};

}  // namespace anc

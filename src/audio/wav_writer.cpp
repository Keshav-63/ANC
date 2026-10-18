#include "audio/wav_writer.h"

#include <cstring>

namespace anc {

namespace {

void putU32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

void putU16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

}  // namespace

bool WavWriter::open(const std::wstring& path, uint32_t sampleRate, uint16_t channels) {
    close();
    file_ = _wfopen(path.c_str(), L"wb");
    if (!file_) return false;
    sampleRate_ = sampleRate;
    channels_ = channels;
    framesWritten_ = 0;
    headerPatched_ = false;
    writeHeader();
    return std::ferror(file_) == 0;
}

void WavWriter::writeHeader() {
    const uint16_t bitsPerSample = 32;
    const uint16_t blockAlign = static_cast<uint16_t>(channels_ * bitsPerSample / 8);

    uint8_t header[44] = {};
    std::memcpy(header + 0, "RIFF", 4);
    putU32(header + 4, 36);  // patched on close
    std::memcpy(header + 8, "WAVE", 4);
    std::memcpy(header + 12, "fmt ", 4);
    putU32(header + 16, 16);                       // fmt chunk size
    putU16(header + 20, 3);                        // WAVE_FORMAT_IEEE_FLOAT
    putU16(header + 22, channels_);
    putU32(header + 24, sampleRate_);
    putU32(header + 28, sampleRate_ * blockAlign);  // byte rate
    putU16(header + 32, blockAlign);
    putU16(header + 34, bitsPerSample);
    std::memcpy(header + 36, "data", 4);
    putU32(header + 40, 0);  // patched on close

    std::fwrite(header, 1, sizeof(header), file_);
}

void WavWriter::writeFloat32(const float* data, size_t frames) {
    if (!file_ || frames == 0) return;
    std::fwrite(data, sizeof(float) * channels_, frames, file_);
    framesWritten_ += frames;
}

void WavWriter::close() {
    if (!file_) return;
    const uint64_t dataBytes = framesWritten_ * channels_ * sizeof(float);
    const uint32_t dataSize = dataBytes > 0xFFFFFFF0ull ? 0xFFFFFFF0u
                                                        : static_cast<uint32_t>(dataBytes);

    if (!headerPatched_) {
        uint8_t buf[4];
        std::fseek(file_, 4, SEEK_SET);
        putU32(buf, 36 + dataSize);
        std::fwrite(buf, 1, 4, file_);
        std::fseek(file_, 40, SEEK_SET);
        putU32(buf, dataSize);
        std::fwrite(buf, 1, 4, file_);
        headerPatched_ = true;
    }

    std::fclose(file_);
    file_ = nullptr;
}

}  // namespace anc

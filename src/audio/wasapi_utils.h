#pragma once

#include <windows.h>
#include <mmreg.h>

#include <cstdint>
#include <string>
#include <vector>

namespace anc {

// ---------------------------------------------------------------------------
// HRESULT / string helpers
// ---------------------------------------------------------------------------

std::string hrToString(HRESULT hr);
std::string toUtf8(const std::wstring& wide);
std::wstring toWide(const std::string& utf8);

struct AncError {
    HRESULT hr = S_OK;
    std::string context;
    std::string message;
};

// ---------------------------------------------------------------------------
// WaveFormat: deep-copying owner of a WAVEFORMATEX / WAVEFORMATEXTENSIBLE
// ---------------------------------------------------------------------------

class WaveFormat {
public:
    WaveFormat() = default;

    explicit WaveFormat(const WAVEFORMATEX* src) {
        if (!src) return;
        const size_t size = sizeof(WAVEFORMATEX) + src->cbSize;
        data_.resize(size);
        std::memcpy(data_.data(), src, size);
    }

    static WaveFormat makeFloat(uint32_t sampleRate, uint16_t channels);
    static WaveFormat makePcm16(uint32_t sampleRate, uint16_t channels);

    bool valid() const { return data_.size() >= sizeof(WAVEFORMATEX); }

    const WAVEFORMATEX* p() const {
        return reinterpret_cast<const WAVEFORMATEX*>(data_.data());
    }
    WAVEFORMATEX* p() { return reinterpret_cast<WAVEFORMATEX*>(data_.data()); }

    uint16_t channels() const { return valid() ? p()->nChannels : 0; }
    uint32_t sampleRate() const { return valid() ? p()->nSamplesPerSec : 0; }
    uint16_t bitsPerSample() const { return valid() ? p()->wBitsPerSample : 0; }
    uint16_t blockAlign() const { return valid() ? p()->nBlockAlign : 0; }
    uint16_t validBitsPerSample() const;
    size_t frameSize() const { return valid() ? p()->nBlockAlign : 0; }
    bool isFloat() const;
    bool isExtensible() const { return valid() && p()->wFormatTag == WAVE_FORMAT_EXTENSIBLE; }

    std::string describe() const;

private:
    std::vector<uint8_t> data_;
};

// Convert a captured buffer of `frames` frames into mono float, averaging
// channels. Supports float32, PCM 16/24/32-bit.
void convertToMono(const uint8_t* src, uint32_t frames, const WaveFormat& fmt, float* dst);

// Convert mono float into the device format, replicating the mono signal to
// every channel. Supports float32, PCM 16/24/32-bit.
void convertFromMono(const float* src, uint32_t frames, const WaveFormat& fmt, uint8_t* dst);

inline bool isSupportedSampleFormat(const WaveFormat& fmt) {
    if (!fmt.valid()) return false;
    if (fmt.isFloat()) return true;
    return fmt.bitsPerSample() == 16 || fmt.bitsPerSample() == 24 || fmt.bitsPerSample() == 32;
}

}  // namespace anc

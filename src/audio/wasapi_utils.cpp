#include "audio/wasapi_utils.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace anc {

// Channel mask bits (SPEAKER_* constants normally come from ksmedia.h; only
// these three are needed here).
namespace {
constexpr DWORD kSpeakerFrontLeft = 0x1;
constexpr DWORD kSpeakerFrontRight = 0x2;
constexpr DWORD kSpeakerFrontCenter = 0x4;
}  // namespace

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT / _PCM (defined locally to avoid the
// initguid.h / uuid.lib dance).
static const GUID kSubtypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID kSubtypePcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

std::string hrToString(HRESULT hr) {
    char buf[128];

    switch (hr) {
        case AUDCLNT_E_NOT_INITIALIZED: return "AUDCLNT_E_NOT_INITIALIZED";
        case AUDCLNT_E_ALREADY_INITIALIZED: return "AUDCLNT_E_ALREADY_INITIALIZED";
        case AUDCLNT_E_WRONG_ENDPOINT_TYPE: return "AUDCLNT_E_WRONG_ENDPOINT_TYPE";
        case AUDCLNT_E_DEVICE_INVALIDATED: return "AUDCLNT_E_DEVICE_INVALIDATED";
        case AUDCLNT_E_NOT_STOPPED: return "AUDCLNT_E_NOT_STOPPED";
        case AUDCLNT_E_BUFFER_TOO_LARGE: return "AUDCLNT_E_BUFFER_TOO_LARGE";
        case AUDCLNT_E_OUT_OF_ORDER: return "AUDCLNT_E_OUT_OF_ORDER";
        case AUDCLNT_E_UNSUPPORTED_FORMAT: return "AUDCLNT_E_UNSUPPORTED_FORMAT";
        case AUDCLNT_E_INVALID_SIZE: return "AUDCLNT_E_INVALID_SIZE";
        case AUDCLNT_E_DEVICE_IN_USE: return "AUDCLNT_E_DEVICE_IN_USE";
        case AUDCLNT_E_BUFFER_OPERATION_PENDING: return "AUDCLNT_E_BUFFER_OPERATION_PENDING";
        case AUDCLNT_E_THREAD_NOT_REGISTERED: return "AUDCLNT_E_THREAD_NOT_REGISTERED";
        case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED: return "AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED";
        case AUDCLNT_E_ENDPOINT_CREATE_FAILED: return "AUDCLNT_E_ENDPOINT_CREATE_FAILED";
        case AUDCLNT_E_SERVICE_NOT_RUNNING: return "AUDCLNT_E_SERVICE_NOT_RUNNING";
        case AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED: return "AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED";
        case AUDCLNT_E_EXCLUSIVE_MODE_ONLY: return "AUDCLNT_E_EXCLUSIVE_MODE_ONLY";
        case AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL: return "AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL";
        case AUDCLNT_E_EVENTHANDLE_NOT_SET: return "AUDCLNT_E_EVENTHANDLE_NOT_SET";
        case AUDCLNT_E_INCORRECT_BUFFER_SIZE: return "AUDCLNT_E_INCORRECT_BUFFER_SIZE";
        case AUDCLNT_E_BUFFER_SIZE_ERROR: return "AUDCLNT_E_BUFFER_SIZE_ERROR";
        case AUDCLNT_E_CPUUSAGE_EXCEEDED: return "AUDCLNT_E_CPUUSAGE_EXCEEDED";
        case AUDCLNT_E_BUFFER_ERROR: return "AUDCLNT_E_BUFFER_ERROR";
        case AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED: return "AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED";
        case AUDCLNT_E_INVALID_DEVICE_PERIOD: return "AUDCLNT_E_INVALID_DEVICE_PERIOD";
        case AUDCLNT_E_INVALID_STREAM_FLAG: return "AUDCLNT_E_INVALID_STREAM_FLAG";
        case AUDCLNT_E_ENDPOINT_OFFLOAD_NOT_CAPABLE: return "AUDCLNT_E_ENDPOINT_OFFLOAD_NOT_CAPABLE";
        case AUDCLNT_E_OUT_OF_OFFLOAD_RESOURCES: return "AUDCLNT_E_OUT_OF_OFFLOAD_RESOURCES";
        case AUDCLNT_E_OFFLOAD_MODE_ONLY: return "AUDCLNT_E_OFFLOAD_MODE_ONLY";
        case AUDCLNT_E_NONOFFLOAD_MODE_ONLY: return "AUDCLNT_E_NONOFFLOAD_MODE_ONLY";
        case AUDCLNT_E_RESOURCES_INVALIDATED: return "AUDCLNT_E_RESOURCES_INVALIDATED";
        case AUDCLNT_E_RAW_MODE_UNSUPPORTED: return "AUDCLNT_E_RAW_MODE_UNSUPPORTED";
        case AUDCLNT_E_ENGINE_PERIODICITY_LOCKED: return "AUDCLNT_E_ENGINE_PERIODICITY_LOCKED";
        case AUDCLNT_E_ENGINE_FORMAT_LOCKED: return "AUDCLNT_E_ENGINE_FORMAT_LOCKED";
        case AUDCLNT_E_HEADTRACKING_ENABLED: return "AUDCLNT_E_HEADTRACKING_ENABLED";
        case AUDCLNT_E_HEADTRACKING_UNSUPPORTED: return "AUDCLNT_E_HEADTRACKING_UNSUPPORTED";
        case AUDCLNT_S_BUFFER_EMPTY: return "AUDCLNT_S_BUFFER_EMPTY";
        case AUDCLNT_S_THREAD_ALREADY_REGISTERED: return "AUDCLNT_S_THREAD_ALREADY_REGISTERED";
        case AUDCLNT_S_POSITION_STALLED: return "AUDCLNT_S_POSITION_STALLED";
        case E_ACCESSDENIED: return "E_ACCESSDENIED (check Windows microphone privacy settings)";
        case E_POINTER: return "E_POINTER";
        case E_INVALIDARG: return "E_INVALIDARG";
        case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
        case E_NOTFOUND: return "E_NOTFOUND (device not found)";
        case REGDB_E_CLASSNOTREG: return "REGDB_E_CLASSNOTREG";
        default: break;
    }

    const DWORD msgId = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        static_cast<DWORD>(hr), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), nullptr, 0, nullptr);
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    std::string result(buf);
    if (msgId != 0) {
        wchar_t* msg = nullptr;
        if (FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS |
                               FORMAT_MESSAGE_ALLOCATE_BUFFER,
                           nullptr, static_cast<DWORD>(hr),
                           MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                           reinterpret_cast<LPWSTR>(&msg), 0, nullptr)) {
            std::wstring w(msg);
            LocalFree(msg);
            while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ')) {
                w.pop_back();
            }
            result += " (" + toUtf8(w) + ")";
        }
    }
    return result;
}

std::string toUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                         nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), size);
    return out;
}

// ---------------------------------------------------------------------------
// WaveFormat
// ---------------------------------------------------------------------------

WaveFormat WaveFormat::makeFloat(uint32_t sampleRate, uint16_t channels) {
    WAVEFORMATEXTENSIBLE wf{};
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.Format.nChannels = channels;
    wf.Format.nSamplesPerSec = sampleRate;
    wf.Format.wBitsPerSample = 32;
    wf.Format.nBlockAlign = static_cast<WORD>(channels * 4);
    wf.Format.nAvgBytesPerSec = sampleRate * wf.Format.nBlockAlign;
    wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wf.Samples.wValidBitsPerSample = 32;
    wf.dwChannelMask = channels == 1 ? kSpeakerFrontCenter
                        : channels == 2 ? (kSpeakerFrontLeft | kSpeakerFrontRight)
                                        : 0;
    wf.SubFormat = kSubtypeIeeeFloat;
    return WaveFormat(&wf.Format);
}

WaveFormat WaveFormat::makePcm16(uint32_t sampleRate, uint16_t channels) {
    WAVEFORMATEXTENSIBLE wf{};
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.Format.nChannels = channels;
    wf.Format.nSamplesPerSec = sampleRate;
    wf.Format.wBitsPerSample = 16;
    wf.Format.nBlockAlign = static_cast<WORD>(channels * 2);
    wf.Format.nAvgBytesPerSec = sampleRate * wf.Format.nBlockAlign;
    wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wf.Samples.wValidBitsPerSample = 16;
    wf.dwChannelMask = channels == 1 ? kSpeakerFrontCenter
                        : channels == 2 ? (kSpeakerFrontLeft | kSpeakerFrontRight)
                                        : 0;
    wf.SubFormat = kSubtypePcm;
    return WaveFormat(&wf.Format);
}

uint16_t WaveFormat::validBitsPerSample() const {
    if (!valid()) return 0;
    if (isExtensible()) {
        return reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(data_.data())->Samples.wValidBitsPerSample;
    }
    return p()->wBitsPerSample;
}

bool WaveFormat::isFloat() const {
    if (!valid()) return false;
    if (p()->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (isExtensible()) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(data_.data());
        return IsEqualGUID(ext->SubFormat, kSubtypeIeeeFloat) != 0;
    }
    return false;
}

std::string WaveFormat::describe() const {
    if (!valid()) return "<invalid format>";
    char buf[192];
    std::snprintf(buf, sizeof(buf), "%u Hz, %u ch, %u-bit %s%s", sampleRate(), channels(),
                  bitsPerSample(), isFloat() ? "float" : "PCM",
                  isExtensible() ? " (extensible)" : "");
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Sample conversion
// ---------------------------------------------------------------------------

namespace {

inline float readSample(const uint8_t* p, const WaveFormat& fmt) {
    const uint16_t bits = fmt.bitsPerSample();
    if (fmt.isFloat()) {
        float v;
        std::memcpy(&v, p, sizeof(float));
        return v;
    }
    switch (bits) {
        case 16: {
            int16_t v;
            std::memcpy(&v, p, sizeof(v));
            return static_cast<float>(v) / 32768.0f;
        }
        case 24: {
            int32_t v = static_cast<int32_t>(p[0] | (p[1] << 8) | (p[2] << 16));
            if (v & 0x00800000) v |= static_cast<int32_t>(0xFF000000);  // sign extend
            return static_cast<float>(v) / 8388608.0f;
        }
        case 32: {
            int32_t v;
            std::memcpy(&v, p, sizeof(v));
            return static_cast<float>(v) / 2147483648.0f;
        }
        default:
            return 0.0f;
    }
}

inline void writeSample(uint8_t* p, const WaveFormat& fmt, float v) {
    if (fmt.isFloat()) {
        std::memcpy(p, &v, sizeof(float));
        return;
    }
    switch (fmt.bitsPerSample()) {
        case 16: {
            float c = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
            int16_t s = static_cast<int16_t>(c * 32767.0f);
            std::memcpy(p, &s, sizeof(s));
            break;
        }
        case 24: {
            float c = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
            int32_t s = static_cast<int32_t>(c * 8388607.0f);
            p[0] = static_cast<uint8_t>(s & 0xFF);
            p[1] = static_cast<uint8_t>((s >> 8) & 0xFF);
            p[2] = static_cast<uint8_t>((s >> 16) & 0xFF);
            break;
        }
        case 32: {
            float c = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
            int32_t s = static_cast<int32_t>(static_cast<double>(c) * 2147483647.0);
            std::memcpy(p, &s, sizeof(s));
            break;
        }
        default:
            break;
    }
}

}  // namespace

void convertToMono(const uint8_t* src, uint32_t frames, const WaveFormat& fmt, float* dst) {
    const uint16_t channels = fmt.channels();
    const size_t frameBytes = fmt.frameSize();
    const float inv = 1.0f / static_cast<float>(channels);
    for (uint32_t f = 0; f < frames; ++f) {
        const uint8_t* frame = src + static_cast<size_t>(f) * frameBytes;
        size_t sampleBytes = frameBytes / channels;
        float sum = 0.0f;
        for (uint16_t c = 0; c < channels; ++c) {
            sum += readSample(frame + static_cast<size_t>(c) * sampleBytes, fmt);
        }
        dst[f] = sum * inv;
    }
}

void convertFromMono(const float* src, uint32_t frames, const WaveFormat& fmt, uint8_t* dst) {
    const uint16_t channels = fmt.channels();
    const size_t frameBytes = fmt.frameSize();
    size_t sampleBytes = frameBytes / channels;
    for (uint32_t f = 0; f < frames; ++f) {
        uint8_t* frame = dst + static_cast<size_t>(f) * frameBytes;
        for (uint16_t c = 0; c < channels; ++c) {
            writeSample(frame + static_cast<size_t>(c) * sampleBytes, fmt, src[f]);
        }
    }
}

}  // namespace anc

// DuplexEngine setup path: device selection, format/period negotiation and
// stream initialization. Runtime (threads/metrics) lives in
// wasapi_streams_run.cpp.

#include "audio/wasapi_streams.h"

#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>

#include "core/audio_config.h"

using Microsoft::WRL::ComPtr;

namespace anc {
namespace {

ComPtr<IMMDeviceEnumerator> makeEnumerator() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) {
        return nullptr;
    }
    return enumerator;
}

ComPtr<IMMDevice> openDevice(IMMDeviceEnumerator* enumerator, EDataFlow flow,
                             const std::wstring& id) {
    ComPtr<IMMDevice> device;
    const HRESULT hr = id.empty() ? enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device)
                                  : enumerator->GetDevice(id.c_str(), &device);
    if (FAILED(hr)) return nullptr;
    return device;
}

WaveFormat queryMixFormat(IMMDevice* device) {
    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
        return {};
    }
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix)) || !mix) return {};
    WaveFormat format(mix);
    CoTaskMemFree(mix);
    return format;
}

struct Periods {
    bool ok = false;
    uint32_t def = 0, fundamental = 0, min = 0, max = 0;
};

Periods querySharedPeriods(IMMDevice* device, const WaveFormat& format) {
    Periods result;
    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
        return result;
    }
    ComPtr<IAudioClient3> client3;
    if (FAILED(client.As(&client3)) || !client3) return result;
    if (FAILED(client3->GetSharedModeEnginePeriod(format.p(), &result.def, &result.fundamental,
                                                  &result.min, &result.max))) {
        return result;
    }
    result.ok = true;
    return result;
}

DWORD streamFlagsFor(bool eventDriven, bool convert) {
    // NOTE: AUDCLNT_STREAMFLAGS_NOPERSIST is rejected by
    // IAudioClient3::InitializeSharedAudioStream (AUDCLNT_E_INVALID_STREAM_FLAG)
    // on some driver stacks (e.g. Realtek), so only the essential flags are set.
    DWORD flags = 0;
    if (eventDriven) flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (convert) flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    return flags;
}

uint32_t roundUpTo(uint32_t value, uint32_t multiple) {
    if (multiple == 0) return value;
    return ((value + multiple - 1) / multiple) * multiple;
}

void setRawMode(IAudioClient* client, bool raw) {
    if (!raw) return;
    ComPtr<IAudioClient2> client2;
    if (FAILED(client->QueryInterface(IID_PPV_ARGS(&client2)))) return;
    AudioClientProperties props{};
    props.cbSize = sizeof(props);
    props.bIsOffload = FALSE;
    props.eCategory = AudioCategory_Media;
    props.Options = AUDCLNT_STREAMOPTIONS_RAW;
    client2->SetClientProperties(&props);  // failure tolerated; init will fall back
}

}  // namespace

DuplexEngine::DuplexEngine() = default;
DuplexEngine::~DuplexEngine() {
    stop();
    releaseInterfaces();
}

double DuplexEngine::elapsedSeconds() const {
    if (!running_.load(std::memory_order_acquire) && startQpc_ == 0) return 0.0;
    return ticksToSeconds(qpcTicks() - startQpc_);
}

RunInfo DuplexEngine::runInfo() const {
    RunInfo info;
    info.valid = startedFlag_.load(std::memory_order_acquire);
    info.startQpc = startQpc_;
    info.captureRate = plan_.captureFormat.sampleRate();
    info.renderRate = plan_.renderFormat.sampleRate();
    info.captureResampled = plan_.captureUsesConversion;
    info.captureFirstPacketPos = captureFirstPos_.load(std::memory_order_acquire);
    info.renderFirstPlayPos = renderFirstPlayPos_.load(std::memory_order_acquire);
    info.captureFirstValid = captureFirstValid_.load(std::memory_order_acquire);
    info.renderFirstValid = renderFirstValid_.load(std::memory_order_acquire);
    return info;
}

DuplexPlan DuplexEngine::initialize(const Options& options) {
    options_ = options;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // no-op if already MTA

    ComPtr<IMMDeviceEnumerator> enumerator = makeEnumerator();
    if (!enumerator) {
        throw AncError{E_FAIL, "initialize", "failed to create the MMDeviceEnumerator"};
    }
    enumerator_ = enumerator.Detach();

    plan_ = planStreams(options_);

    // --- Open the render (playback) stream first; it defines the clock. ---
    std::string error;
    if (!tryOpenRender(plan_.periodFrames, error)) {
        throw AncError{E_FAIL, "render stream", error};
    }

    // --- Open the capture stream. ---
    bool captureOpened = false;
    if (options_.captureRaw && !plan_.captureUsesConversion) {
        if (tryOpenCapture(options_, plan_.periodFrames, /*raw=*/true, plan_.captureFormat,
                           plan_.captureUsesConversion, error)) {
            plan_.captureRawMode = true;
            captureOpened = true;
            plan_.notes += "capture: raw (effect-bypassed) mode active\n";
        } else {
            plan_.notes += "capture: raw mode rejected (" + error + "), falling back\n";
        }
    }
    if (!captureOpened) {
        if (!tryOpenCapture(options_, plan_.periodFrames, /*raw=*/false, plan_.captureFormat,
                            plan_.captureUsesConversion, error)) {
            throw AncError{E_FAIL, "capture stream", error};
        }
    }

    // --- Events, buffer sizes and services. ---
    captureEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    renderEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    shutdownEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!captureEvent_ || !renderEvent_ || !shutdownEvent_) {
        throw AncError{E_FAIL, "initialize", "failed to create stream events"};
    }

    UINT32 captureBuffer = 0, renderBuffer = 0;
    HRESULT hr = captureClient_->GetBufferSize(&captureBuffer);
    if (FAILED(hr)) throw AncError{hr, "capture GetBufferSize", hrToString(hr)};
    hr = renderClient_->GetBufferSize(&renderBuffer);
    if (FAILED(hr)) throw AncError{hr, "render GetBufferSize", hrToString(hr)};
    plan_.captureBufferFrames = captureBuffer;
    plan_.renderBufferFrames = renderBuffer;

    hr = captureClient_->SetEventHandle(captureEvent_);
    if (FAILED(hr)) throw AncError{hr, "capture SetEventHandle", hrToString(hr)};
    hr = renderClient_->SetEventHandle(renderEvent_);
    if (FAILED(hr)) throw AncError{hr, "render SetEventHandle", hrToString(hr)};

    hr = captureClient_->GetService(IID_PPV_ARGS(&capture_));
    if (FAILED(hr)) throw AncError{hr, "capture client service", hrToString(hr)};
    hr = renderClient_->GetService(IID_PPV_ARGS(&render_));
    if (FAILED(hr)) throw AncError{hr, "render client service", hrToString(hr)};

    if (SUCCEEDED(captureClient_->GetService(IID_PPV_ARGS(&captureClock_)))) {
        captureClock_->QueryInterface(IID_PPV_ARGS(&captureClock2_));
    }
    if (SUCCEEDED(renderClient_->GetService(IID_PPV_ARGS(&renderClock_)))) {
        renderClock_->QueryInterface(IID_PPV_ARGS(&renderClock2_));
    }

    REFERENCE_TIME latency = 0;
    if (SUCCEEDED(captureClient_->GetStreamLatency(&latency))) {
        plan_.captureStreamLatencyMs = static_cast<double>(latency) / 10000.0;
    }
    if (SUCCEEDED(renderClient_->GetStreamLatency(&latency))) {
        plan_.renderStreamLatencyMs = static_cast<double>(latency) / 10000.0;
    }

    periodFrames_ = plan_.periodFrames;

    // Scratch buffers for the audio threads (owned by the engine, never
    // allocated while streaming).
    captureScratch_.assign(captureBuffer, 0.0f);
    renderScratchIn_.assign(renderBuffer, 0.0f);
    renderScratchOut_.assign(renderBuffer, 0.0f);

    if (!options_.recordInputPath.empty()) {
        inputSink_ = std::make_unique<RecordSink>();
        if (!inputSink_->open(options_.recordInputPath, plan_.captureFormat.sampleRate())) {
            throw AncError{E_FAIL, "record input", "failed to open the input WAV file"};
        }
    }
    if (!options_.recordOutputPath.empty()) {
        outputSink_ = std::make_unique<RecordSink>();
        if (!outputSink_->open(options_.recordOutputPath, plan_.renderFormat.sampleRate())) {
            throw AncError{E_FAIL, "record output", "failed to open the output WAV file"};
        }
    }

    return plan_;
}

DuplexPlan DuplexEngine::planStreams(const Options& options) {
    DuplexPlan plan;

    ComPtr<IMMDevice> captureDevice =
        openDevice(enumerator_, eCapture, options.captureDeviceId);
    if (!captureDevice) throw AncError{E_NOTFOUND, "capture device", "could not open the capture endpoint"};
    ComPtr<IMMDevice> renderDevice = openDevice(enumerator_, eRender, options.renderDeviceId);
    if (!renderDevice) throw AncError{E_NOTFOUND, "render device", "could not open the render endpoint"};
    captureDevice_ = captureDevice.Detach();
    renderDevice_ = renderDevice.Detach();

    plan.captureDeviceId = options.captureDeviceId;
    plan.captureDeviceName = deviceFriendlyName(options.captureDeviceId);
    plan.renderDeviceId = options.renderDeviceId;
    plan.renderDeviceName = deviceFriendlyName(options.renderDeviceId);
    if (plan.captureDeviceName.empty()) plan.captureDeviceName = L"(default capture device)";
    if (plan.renderDeviceName.empty()) plan.renderDeviceName = L"(default render device)";

    const WaveFormat captureMix = queryMixFormat(captureDevice_);
    const WaveFormat renderMix = queryMixFormat(renderDevice_);
    if (!captureMix.valid()) throw AncError{E_FAIL, "capture format", "could not read the capture mix format"};
    if (!renderMix.valid()) throw AncError{E_FAIL, "render format", "could not read the render mix format"};

    char buf[512];
    std::snprintf(buf, sizeof(buf), "capture mix format: %s\nrender mix format : %s\n",
                  captureMix.describe().c_str(), renderMix.describe().c_str());
    plan.notes += buf;

    // Choose the capture stream format. If sample rates differ, ask WASAPI to
    // sample-rate-convert the capture stream to the render rate.
    plan.renderFormat = renderMix;
    plan.captureFormat = captureMix;
    if (captureMix.sampleRate() != renderMix.sampleRate()) {
        if (!options.allowResample) {
            throw AncError{E_FAIL, "format negotiation",
                           "capture and render sample rates differ and resampling is disabled"};
        }
        plan.captureFormat = WaveFormat::makeFloat(renderMix.sampleRate(), captureMix.channels());
        plan.captureUsesConversion = true;
        plan.notes += "capture: sample rate conversion requested (AUTOCONVERTPCM)\n";
    }
    if (plan.captureFormat.sampleRate() != kPreferredSampleRate ||
        plan.renderFormat.sampleRate() != kPreferredSampleRate) {
        plan.notes += "note: operating below the 48 kHz target; using the endpoints' native rates\n";
    }

    // Query shared-mode engine periods for the exact formats we will open.
    const Periods capturePeriods = querySharedPeriods(captureDevice_, plan.captureFormat);
    const Periods renderPeriods = querySharedPeriods(renderDevice_, plan.renderFormat);

    if (capturePeriods.ok) {
        plan.captureMinPeriod = capturePeriods.min;
        plan.captureMaxPeriod = capturePeriods.max;
        plan.captureDefaultPeriod = capturePeriods.def;
    }
    if (renderPeriods.ok) {
        plan.renderMinPeriod = renderPeriods.min;
        plan.renderMaxPeriod = renderPeriods.max;
        plan.renderDefaultPeriod = renderPeriods.def;
    }

    if (!capturePeriods.ok || !renderPeriods.ok) {
        plan.periodFrames = 0;  // legacy path; engine decides the period
        plan.notes += "engine periods: IAudioClient3 unavailable or query failed; using default period\n";
        return plan;
    }

    const uint32_t minFrames = std::max(capturePeriods.min, renderPeriods.min);
    const uint32_t maxFrames = std::min(capturePeriods.max, renderPeriods.max);

    uint32_t period = 0;
    if (options.periodFrames > 0) {
        period = options.periodFrames;
        if (period < minFrames) {
            std::snprintf(buf, sizeof(buf),
                          "requested period %u frames raised to the lowest stable %u frames\n",
                          options.periodFrames, minFrames);
            plan.notes += buf;
            period = minFrames;
        }
        if (period > maxFrames) {
            std::snprintf(buf, sizeof(buf),
                          "requested period %u frames exceeds device maximum; using %u frames\n",
                          options.periodFrames, maxFrames);
            plan.notes += buf;
            period = maxFrames;
        }
    } else {
        period = minFrames;
    }

    // Keep the period a multiple of both devices' fundamental periods when
    // that is possible inside the common range.
    const uint32_t fundamental = std::max(capturePeriods.fundamental, renderPeriods.fundamental);
    const uint32_t rounded = roundUpTo(period, fundamental);
    if (rounded != period && rounded <= maxFrames) {
        period = rounded;
    }
    if (period < minFrames || period > maxFrames) {
        period = std::min(std::max(capturePeriods.def, renderPeriods.def), maxFrames);
        plan.notes += "lowest common period unusable; fell back to the larger default period\n";
    }

    plan.periodFrames = period;
    plan.client3Used = true;

    auto ms = [](uint32_t frames, uint32_t rate) {
        return rate ? 1000.0 * static_cast<double>(frames) / rate : 0.0;
    };
    std::snprintf(buf, sizeof(buf),
                  "engine period: %u frames (%.2f ms) [capture min %u/max %u, render min %u/max %u]\n",
                  period, ms(period, plan.renderFormat.sampleRate()), capturePeriods.min,
                  capturePeriods.max, renderPeriods.min, renderPeriods.max);
    plan.notes += buf;
    return plan;
}

bool DuplexEngine::tryOpenRender(uint32_t periodFrames, std::string& errorOut) {
    ComPtr<IAudioClient> client;
    HRESULT hr = renderDevice_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                         &client);
    if (FAILED(hr)) {
        errorOut = "Activate: " + hrToString(hr);
        return false;
    }

    const DWORD flags = streamFlagsFor(/*eventDriven=*/true, /*convert=*/false);
    ComPtr<IAudioClient3> client3;
    const bool haveClient3 = SUCCEEDED(client.As(&client3)) && client3;

    if (haveClient3 && periodFrames > 0) {
        hr = client3->InitializeSharedAudioStream(flags, periodFrames, plan_.renderFormat.p(),
                                                  nullptr);
    } else {
        REFERENCE_TIME def = 0, min = 0;
        client->GetDevicePeriod(&def, &min);
        const REFERENCE_TIME duration = min > 0 ? min * 2 : 200000;  // 2 x min, fallback 20 ms
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, duration, 0,
                                plan_.renderFormat.p(), nullptr);
    }
    if (FAILED(hr)) {
        errorOut = "Initialize: " + hrToString(hr);
        return false;
    }

    renderClient_ = client.Detach();
    renderClient3_ = client3.Detach();  // may be null on the legacy path
    return true;
}

bool DuplexEngine::tryOpenCapture(const Options& options, uint32_t periodFrames, bool raw,
                                  const WaveFormat& format, bool allowConversion,
                                  std::string& errorOut) {
    (void)options;
    ComPtr<IAudioClient> client;
    HRESULT hr = captureDevice_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (FAILED(hr)) {
        errorOut = "Activate: " + hrToString(hr);
        return false;
    }

    setRawMode(client.Get(), raw);

    const DWORD flags = streamFlagsFor(/*eventDriven=*/true, /*convert=*/allowConversion);
    ComPtr<IAudioClient3> client3;
    const bool haveClient3 = SUCCEEDED(client.As(&client3)) && client3;

    if (haveClient3 && periodFrames > 0) {
        hr = client3->InitializeSharedAudioStream(flags, periodFrames, format.p(), nullptr);
    } else {
        REFERENCE_TIME def = 0, min = 0;
        client->GetDevicePeriod(&def, &min);
        const REFERENCE_TIME duration = min > 0 ? min * 2 : 200000;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, duration, 0, format.p(), nullptr);
    }
    if (FAILED(hr)) {
        errorOut = "Initialize: " + hrToString(hr) + (raw ? " [raw]" : "");
        return false;
    }

    captureClient_ = client.Detach();
    captureClient3_ = client3.Detach();
    return true;
}

void DuplexEngine::sampleClockImpl(void* clock2Ptr, void* clockPtr, bool& ok, uint64_t& pos,
                                   int64_t& qpc) {
    ok = false;
    pos = 0;
    qpc = 0;
    if (clock2Ptr) {
        auto* clock2 = static_cast<IAudioClock2*>(clock2Ptr);
        UINT64 p = 0, q = 0;
        if (SUCCEEDED(clock2->GetDevicePosition(&p, &q))) {
            ok = true;
            pos = p;
            qpc = static_cast<int64_t>(q);
        }
        return;
    }
    if (clockPtr) {
        auto* clock = static_cast<IAudioClock*>(clockPtr);
        UINT64 p = 0;
        if (SUCCEEDED(clock->GetPosition(&p, nullptr))) {
            ok = true;
            pos = p;
            qpc = qpcTicks();
        }
    }
}

bool DuplexEngine::sampleCaptureClock(uint64_t& posFrames, int64_t& qpcTicksOut) const {
    if (!captureClock_ && !captureClock2_) return false;
    bool ok = false;
    sampleClockImpl(captureClock2_, captureClock_, ok, posFrames, qpcTicksOut);
    return ok;
}

bool DuplexEngine::sampleRenderClock(uint64_t& posFrames, int64_t& qpcTicksOut) const {
    if (!renderClock_ && !renderClock2_) return false;
    bool ok = false;
    sampleClockImpl(renderClock2_, renderClock_, ok, posFrames, qpcTicksOut);
    return ok;
}

void DuplexEngine::setProcessor(AudioProcessor* processor) { processor_ = processor; }

void DuplexEngine::releaseInterfaces() {
    if (capture_) {
        capture_->Release();
        capture_ = nullptr;
    }
    if (render_) {
        render_->Release();
        render_ = nullptr;
    }
    if (captureClock2_) {
        captureClock2_->Release();
        captureClock2_ = nullptr;
    }
    if (renderClock2_) {
        renderClock2_->Release();
        renderClock2_ = nullptr;
    }
    if (captureClock_) {
        captureClock_->Release();
        captureClock_ = nullptr;
    }
    if (renderClock_) {
        renderClock_->Release();
        renderClock_ = nullptr;
    }
    if (captureClient3_) {
        captureClient3_->Release();
        captureClient3_ = nullptr;
    }
    if (renderClient3_) {
        renderClient3_->Release();
        renderClient3_ = nullptr;
    }
    if (captureClient_) {
        captureClient_->Release();
        captureClient_ = nullptr;
    }
    if (renderClient_) {
        renderClient_->Release();
        renderClient_ = nullptr;
    }
    if (captureDevice_) {
        captureDevice_->Release();
        captureDevice_ = nullptr;
    }
    if (renderDevice_) {
        renderDevice_->Release();
        renderDevice_ = nullptr;
    }
    if (enumerator_) {
        enumerator_->Release();
        enumerator_ = nullptr;
    }
    for (HANDLE* event : {&captureEvent_, &renderEvent_, &shutdownEvent_}) {
        if (*event) {
            CloseHandle(*event);
            *event = nullptr;
        }
    }
}

}  // namespace anc

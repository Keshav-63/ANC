#include "audio/device_probe.h"

#include <functiondiscoverykeys_devpkey.h>
#include <propidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace anc {
namespace {

// PKEY_AudioEndpoint_FormFactor {1da5d803-d492-4edd-8c23-e0c0ffe7f9f1}, 8
// (defined locally; the exported symbol is not in every SDK's uuid.lib)
const PROPERTYKEY kPkeyFormFactor = {
    {0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xe7, 0xf9, 0xf1}}, 8};

std::wstring propString(IPropertyStore* store, REFPROPERTYKEY key) {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring out;
    if (SUCCEEDED(store->GetValue(key, &pv))) {
        if (pv.vt == VT_LPWSTR && pv.pwszVal) out = pv.pwszVal;
    }
    PropVariantClear(&pv);
    return out;
}

std::wstring formFactorName(EndpointFormFactor ff) {
    switch (ff) {
        case RemoteNetworkDevice: return L"RemoteNetworkDevice";
        case Speakers: return L"Speakers";
        case LineLevel: return L"LineLevel";
        case Headphones: return L"Headphones";
        case Microphone: return L"Microphone";
        case Headset: return L"Headset";
        case Handset: return L"Handset";
        case UnknownDigitalPassthrough: return L"UnknownDigitalPassthrough";
        case SPDIF: return L"SPDIF";
        case DigitalAudioDisplayDevice: return L"DigitalAudioDisplayDevice";
        default: return L"Unknown";
    }
}

ComPtr<IMMDeviceEnumerator> makeEnumerator() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) enumerator.Reset();
    return enumerator;
}

ComPtr<IMMDevice> getDevice(IMMDeviceEnumerator* enumerator, EDataFlow flow,
                            const std::wstring& id) {
    ComPtr<IMMDevice> device;
    HRESULT hr = id.empty() ? enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device)
                            : enumerator->GetDevice(id.c_str(), &device);
    if (FAILED(hr)) device.Reset();
    return device;
}

WaveFormat getMixFormat(IMMDevice* device) {
    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
        return WaveFormat{};
    }
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix)) || mix == nullptr) return WaveFormat{};
    WaveFormat result(mix);
    CoTaskMemFree(mix);
    return result;
}

void queryEnginePeriods(IMMDevice* device, EndpointInfo& info) {
    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) return;

    ComPtr<IAudioClient3> client3;
    info.hasClient3 = SUCCEEDED(client.As(&client3)) && client3;

    REFERENCE_TIME def = 0, min = 0;
    if (SUCCEEDED(client->GetDevicePeriod(&def, &min))) {
        info.streamLatencyMs = 0.0;  // filled below
    }
    REFERENCE_TIME latency = 0;
    if (SUCCEEDED(client->GetStreamLatency(&latency))) {
        info.streamLatencyMs = static_cast<double>(latency) / 10000.0;  // 100ns -> ms
    }

    if (info.hasClient3 && info.mixFormat.valid()) {
        info.periodQueryResult = client3->GetSharedModeEnginePeriod(
            info.mixFormat.p(), &info.defaultPeriod, &info.fundamentalPeriod, &info.minPeriod,
            &info.maxPeriod);
    } else {
        info.periodQueryResult = E_NOINTERFACE;
    }
}

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

}  // namespace

std::vector<EndpointInfo> enumerateAudioEndpoints(EDataFlow flow) {
    std::vector<EndpointInfo> result;
    ComPtr<IMMDeviceEnumerator> enumerator = makeEnumerator();
    if (!enumerator) return result;

    ComPtr<IMMDevice> defaultDevice;
    std::wstring defaultId;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &defaultDevice))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(defaultDevice->GetId(&id))) {
            defaultId = id;
            CoTaskMemFree(id);
        }
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection))) {
        return result;
    }

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;

        EndpointInfo info;
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id))) {
            info.id = id;
            CoTaskMemFree(id);
        }
        info.isDefault = (!info.id.empty() && info.id == defaultId);

        ComPtr<IPropertyStore> store;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
            info.friendlyName = propString(store.Get(), PKEY_Device_FriendlyName);
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(store->GetValue(kPkeyFormFactor, &pv)) &&
                pv.vt == VT_UI4) {
                info.formFactorName =
                    formFactorName(static_cast<EndpointFormFactor>(pv.uintVal));
            }
            PropVariantClear(&pv);
        }

        info.mixFormat = getMixFormat(device.Get());
        queryEnginePeriods(device.Get(), info);
        result.push_back(std::move(info));
    }
    return result;
}

std::string formatEndpointTable(EDataFlow flow, const std::vector<EndpointInfo>& endpoints) {
    std::string out;
    out += (flow == eCapture) ? "Capture endpoints (microphones):\n"
                              : "Render endpoints (speakers / headphones):\n";
    if (endpoints.empty()) {
        out += "  (none found)\n";
        return out;
    }
    char buf[512];
    for (size_t i = 0; i < endpoints.size(); ++i) {
        const EndpointInfo& e = endpoints[i];
        std::snprintf(buf, sizeof(buf), "  #%zu %s\n      name  : %s\n      id    : %s\n",
                      i, e.isDefault ? "[default]" : "",
                      toUtf8(e.friendlyName).c_str(), toUtf8(e.id).c_str());
        out += buf;
        std::snprintf(buf, sizeof(buf), "      form  : %s\n",
                      e.formFactorName.empty() ? "(unknown)" : toUtf8(e.formFactorName).c_str());
        out += buf;
        std::snprintf(buf, sizeof(buf), "      format: %s\n", e.mixFormat.describe().c_str());
        out += buf;
        if (e.hasClient3 && SUCCEEDED(e.periodQueryResult)) {
            const uint32_t rate = e.mixFormat.sampleRate();
            auto ms = [rate](uint32_t frames) {
                return rate ? 1000.0 * frames / rate : 0.0;
            };
            std::snprintf(buf, sizeof(buf),
                          "      engine periods (frames @ %u Hz): default %u (%.2f ms), "
                          "fundamental %u, min %u (%.2f ms), max %u (%.2f ms)\n",
                          rate, e.defaultPeriod, ms(e.defaultPeriod), e.fundamentalPeriod,
                          e.minPeriod, ms(e.minPeriod), e.maxPeriod, ms(e.maxPeriod));
            out += buf;
        } else {
            std::snprintf(buf, sizeof(buf),
                          "      engine periods: unavailable (%s)\n",
                          hrToString(e.periodQueryResult).c_str());
            out += buf;
        }
        std::snprintf(buf, sizeof(buf), "      stream latency reported: %.3f ms\n",
                      e.streamLatencyMs);
        out += buf;
    }
    return out;
}

std::wstring selectDeviceId(EDataFlow flow, const std::wstring& spec, std::string& error) {
    error.clear();
    const std::vector<EndpointInfo> endpoints = enumerateAudioEndpoints(flow);
    if (endpoints.empty()) {
        error = "no active endpoints found";
        return {};
    }

    if (spec.empty()) {
        for (const auto& e : endpoints) {
            if (e.isDefault) return e.id;
        }
        error = "no default endpoint";
        return {};
    }

    if (spec.size() > 1 && spec[0] == L'#') {
        try {
            const int index = std::stoi(spec.substr(1));
            if (index < 0 || static_cast<size_t>(index) >= endpoints.size()) {
                error = "device index out of range (see audio_probe listing)";
                return {};
            }
            return endpoints[static_cast<size_t>(index)].id;
        } catch (...) {
            error = "invalid device index";
            return {};
        }
    }

    const std::wstring needle = lower(spec);
    for (const auto& e : endpoints) {
        if (lower(e.friendlyName).find(needle) != std::wstring::npos) return e.id;
    }
    error = "no endpoint name contains the given text";
    return {};
}

std::wstring deviceFriendlyName(const std::wstring& deviceId) {
    ComPtr<IMMDeviceEnumerator> enumerator = makeEnumerator();
    if (!enumerator) return {};
    if (deviceId.empty()) return {};
    ComPtr<IMMDevice> device = getDevice(enumerator.Get(), eRender, deviceId);
    if (!device) device = getDevice(enumerator.Get(), eCapture, deviceId);
    if (!device) return {};
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) return {};
    return propString(store.Get(), PKEY_Device_FriendlyName);
}

}  // namespace anc

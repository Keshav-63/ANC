#pragma once

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <string>
#include <vector>

#include "audio/wasapi_utils.h"

namespace anc {

struct EndpointInfo {
    std::wstring id;
    std::wstring friendlyName;
    std::wstring formFactorName;
    WaveFormat mixFormat;
    bool isDefault = false;
    bool hasClient3 = false;
    uint32_t defaultPeriod = 0;
    uint32_t fundamentalPeriod = 0;
    uint32_t minPeriod = 0;
    uint32_t maxPeriod = 0;
    HRESULT periodQueryResult = E_FAIL;
    double streamLatencyMs = 0.0;
};

// Requires COM to be initialized. Enumerates active endpoints of the given
// flow (eCapture or eRender), newest Windows device order.
std::vector<EndpointInfo> enumerateAudioEndpoints(EDataFlow flow);

std::string formatEndpointTable(EDataFlow flow, const std::vector<EndpointInfo>& endpoints);

// Resolve a user-supplied device specifier:
//   ""        -> default device
//   "#N"      -> index N in enumerateAudioEndpoints() order
//   "text"    -> first friendly name containing "text" (case-insensitive)
// Returns the device id, or empty string on failure (error set).
std::wstring selectDeviceId(EDataFlow flow, const std::wstring& spec, std::string& error);

// Friendly name of a device id (empty if not found).
std::wstring deviceFriendlyName(const std::wstring& deviceId);

}  // namespace anc

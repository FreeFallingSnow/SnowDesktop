#pragma once

#include "common/l10n.h"
#include "widget/data/widget_system_data_provider.h"
#include <winrt/base.h>
#include <cmath>
#include <optional>
#include <span>
#include <string>

namespace snowdesktop
{
inline std::wstring StatusBarNetworkTooltip(
    std::span<const widget_runtime::WidgetNetworkConnectionDataSnapshot> connections,
    std::wstring fallback)
{
    if (connections.empty()) return fallback;
    std::wstring text;
    for (const auto& connection : connections)
    {
        if (!text.empty()) text += L"\n\n";
        text += winrt::to_hstring(connection.name).c_str();
        text += L"\n";
        const auto& state = connection.connectivity;
        text += _LW(state == "internet" ? "statusBar.networkInternet" :
            state == "local" || state == "none" ? "statusBar.networkNoInternet" :
            state == "connected" ? "controlCenter.connected" :
            state == "disabled" ? "statusBar.networkDisabled" : "statusBar.networkDisconnected");
    }
    return text;
}

inline std::wstring StatusBarVolumeTooltip(
    const std::optional<widget_runtime::WidgetAudioOutputDefaultDataSnapshot>& device,
    const std::optional<widget_runtime::WidgetAudioOutputVolumeDataSnapshot>& audio,
    std::optional<double> pendingVolume = {})
{
    const bool matchingDevice = device && device->available && !device->name.empty() &&
        audio && audio->available && device->id == audio->endpointId;
    std::wstring text = matchingDevice ? std::wstring(winrt::to_hstring(device->name).c_str()) : _LW("statusBar.volume");
    text += L": ";
    if (!audio || !audio->available) return text + L"—";
    if (audio->muted && !pendingVolume) return text + _LW("statusBar.muted");
    const double volume = pendingVolume.value_or(audio->volume);
    return text + (std::isfinite(volume) && volume >= 0 && volume <= 1 ?
        std::to_wstring(static_cast<int>(std::lround(volume * 100))) + L"%" : L"—");
}
}

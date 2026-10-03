#pragma once
#include <array>
#include <string_view>

namespace snowdesktop
{
// Host-only control identifiers; these are not widget topics or capabilities.
struct SystemQuickControl
{
    std::string_view id;
    const char* label;
    const wchar_t* glyph;
    const char* topic;
    const char* task;
    const wchar_t* settings;
    bool menu;
};
inline constexpr std::array SystemQuickControls{
    SystemQuickControl{"projection", "controlCenter.projection", L"\uEBC6", "host.projection", "host.projection.set", L"ms-settings:display", true},
    SystemQuickControl{"hotspot", "controlCenter.hotspot", L"\uEC27", "host.hotspot", "host.hotspot.set", L"ms-settings:network-mobilehotspot", true},
    SystemQuickControl{"airplane", "controlCenter.airplane", L"\uE709", "host.airplane", "host.airplane.set", L"ms-settings:network-airplanemode", false},
    SystemQuickControl{"microphone", "controlCenter.microphoneMute", L"\uF781", "audio.input.volume", "audio.input.setMute", L"ms-settings:sound", false},
    SystemQuickControl{"awake", "controlCenter.keepAwake", L"\uE708", "host.awake", "host.awake.set", L"ms-settings:powersleep", false},
    SystemQuickControl{"power", "controlCenter.powerMode", L"\uE945", "system.power.plans", "", L"ms-settings:powersleep", true}
};
inline const SystemQuickControl* FindSystemQuickControl(std::string_view id)
{
    for (const auto& control : SystemQuickControls) if (control.id == id) return &control;
    return nullptr;
}
}

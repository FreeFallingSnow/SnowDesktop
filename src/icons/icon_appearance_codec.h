#pragma once
#include "icon_appearance.h"
#include "theme/icon_theme_codec.h"
#include <windows.h>

namespace snowdesktop::icon_appearance
{
inline std::wstring ToWide(const std::string& value)
{
    if (value.empty()) return {};
    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), length);
    return out;
}
inline bool ValidTarget(std::wstring_view target)
{
    if (target == L"control:start" || target == L"control:search") return true;
    for (const auto prefix : {L"item:", L"widget:", L"recycle-empty:", L"recycle-full:"})
        if (target.starts_with(prefix) && target.size() > std::wstring_view(prefix).size()) return target.size() <= 32768;
    return false;
}
// Theme packages deliberately exclude the processing mode. Local overrides
// persist it separately, including explicit disablement and preset identity.
inline std::string EncodeStyle(const IconBeautifySettings& settings)
{
    const auto theme = themes::EncodeIconTheme(settings);
    if (theme.empty()) return {};
    return "{\"mode\":" + std::to_string(settings.mode) + ",\"preset\":" +
        std::to_string(static_cast<int>(settings.preset)) + ",\"theme\":" + theme + "}";
}
inline bool DecodeStyle(const JsonValue& json, IconBeautifySettings& settings)
{
    const auto* mode = json.Find("mode");
    const auto* preset = json.Find("preset");
    const auto* theme = json.Find("theme");
    if (!mode || !mode->IsNumber() || (mode->number != 0 && mode->number != 1 && mode->number != 2) ||
        !preset || !preset->IsNumber() || !std::isfinite(preset->number) ||
        preset->number != std::floor(preset->number) ||
        (preset->number != 0 && preset->number != 1 && preset->number != 5 &&
         preset->number != 6 && preset->number != 7 && preset->number != 8) ||
        !theme || !themes::DecodeIconTheme(*theme, settings)) return false;
    settings.mode = static_cast<int>(mode->number);
    settings.preset = static_cast<IconBeautifyPreset>(static_cast<int>(preset->number));
    return true;
}
}

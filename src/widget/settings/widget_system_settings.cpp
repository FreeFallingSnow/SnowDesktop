#include "widget_system_settings.h"

#include <array>
#include <utility>

namespace snowdesktop::widget_runtime
{
std::optional<std::wstring_view> SystemSettingsUri(
    std::string_view page) noexcept
{
    static constexpr std::array mappings{
        std::pair{ std::string_view("notifications"),
            std::wstring_view(L"ms-settings:notifications") },
        std::pair{ std::string_view("audio"),
            std::wstring_view(L"ms-settings:sound") },
        std::pair{ std::string_view("display"),
            std::wstring_view(L"ms-settings:display") },
        std::pair{ std::string_view("network"),
            std::wstring_view(L"ms-settings:network-status") },
        std::pair{ std::string_view("bluetooth"),
            std::wstring_view(L"ms-settings:bluetooth") },
        std::pair{ std::string_view("power"),
            std::wstring_view(L"ms-settings:powersleep") },
        std::pair{ std::string_view("storage"),
            std::wstring_view(L"ms-settings:storagesense") },
        std::pair{ std::string_view("apps"),
            std::wstring_view(L"ms-settings:appsfeatures") },
        std::pair{ std::string_view("personalization"),
            std::wstring_view(L"ms-settings:personalization") },
    };
    for (const auto& [name, uri] : mappings)
    {
        if (page == name) return uri;
    }
    return std::nullopt;
}
}

#pragma once

#include <optional>
#include <string_view>

namespace snowdesktop::widget_runtime
{
std::optional<std::wstring_view> SystemSettingsUri(
    std::string_view page) noexcept;
}

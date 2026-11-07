#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace snowdesktop::runtime_modules
{
enum class Module : std::uint8_t { Taskbar, Tray, WindowMinimize, StartMenu, WallpaperPreview, Count };
inline constexpr std::size_t kCount = static_cast<std::size_t>(Module::Count);
inline constexpr std::array<std::string_view, kCount> kKeys{
    "taskbar", "tray", "windowMinimize", "startMenu", "wallpaperPreview"};
struct Policy
{
    std::array<bool, kCount> allowed{true, true, true, true, true};
    bool Allows(Module module) const noexcept;
    bool operator==(const Policy&) const = default;
};

}

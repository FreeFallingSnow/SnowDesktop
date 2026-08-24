#pragma once

#include "../settings_route.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace snowdesktop::winui
{

enum class SettingsUpdateState : std::uint8_t
{
    Unknown,
    Checking,
    UpToDate,
    UpdateAvailable,
    ManagedByStore,
    Failed,
};

enum class SettingsBackupState : std::uint8_t
{
    Unknown,
    Empty,
    Ready,
    Running,
    Succeeded,
    Failed,
};

/** Host-owned Home/About/Debug status for one matching settings session. */
struct HomeAboutStatusPatch
{
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    std::optional<std::wstring> applicationVersion;
    std::optional<std::size_t> installedWidgetCount;
    /** Deployment ownership controls whether the legacy update row exists. */
    std::optional<bool> packaged;
    std::optional<SettingsUpdateState> updateState;
    std::optional<std::wstring> availableVersion;
    std::optional<std::wstring> updateDetail;
    std::optional<SettingsBackupState> backupState;
    std::optional<std::size_t> backupCount;
    std::optional<std::wstring> backupDetail;
    /** Session-only scheduler state used by the conditional Debug page. */
    std::optional<bool> animationDiagnosticsEnabled;
    std::optional<std::wstring> animationDiagnosticsStatus;
};

/** Every external link from the legacy About page, without raw URLs in UI. */
enum class HomeAboutLink : std::uint8_t
{
    Bilibili,
    AuthorGitHub,
    Douyin,
    Xiaohongshu,
    ReleaseRepository,
    SourceRepository,
    QqGroup,
    EverythingSdk,
    DearImGui,
    Lua,
    Spdlog,
    PinyinData,
    TranslucentTb,
};

[[nodiscard]] constexpr std::wstring_view HomeAboutLinkUri(
    HomeAboutLink link) noexcept
{
    switch (link)
    {
    case HomeAboutLink::Bilibili:
        return L"https://space.bilibili.com/32837853";
    case HomeAboutLink::AuthorGitHub:
        return L"https://github.com/FreeFallingSnow/";
    case HomeAboutLink::Douyin:
        return L"https://www.douyin.com/user/"
               L"MS4wLjABAAAA-O94bwF3BK2sj9JOwM2R2zRlTOiYf4BbaSyIF9DZPyM";
    case HomeAboutLink::Xiaohongshu:
        return L"https://www.xiaohongshu.com/user/profile/"
               L"6819eed7000000000403bf0e";
    case HomeAboutLink::ReleaseRepository:
        return L"https://github.com/FreeFallingSnow/SnowDesktop_Release";
    case HomeAboutLink::SourceRepository:
        return L"https://github.com/FreeFallingSnow/SnowDesktop";
    case HomeAboutLink::QqGroup:
        return L"https://qm.qq.com/q/HyazkCIRig";
    case HomeAboutLink::EverythingSdk:
        return L"https://www.voidtools.com/support/everything/sdk/";
    case HomeAboutLink::DearImGui:
        return L"https://github.com/ocornut/imgui";
    case HomeAboutLink::Lua:
        return L"https://www.lua.org/";
    case HomeAboutLink::Spdlog:
        return L"https://github.com/gabime/spdlog";
    case HomeAboutLink::PinyinData:
        return L"https://github.com/mozillazg/pinyin-data";
    case HomeAboutLink::TranslucentTb:
        return L"https://github.com/TranslucentTB/TranslucentTB/tree/"
               L"322e2b7395a51975150126276308b415970e080b";
    }
    return {};
}

enum class HomeAboutCommand : std::uint8_t
{
    CheckForUpdates,
    CancelUpdateCheck,
    OpenProject,
    OpenLicense,
    OpenThirdPartyNotices,
};

} // namespace snowdesktop::winui

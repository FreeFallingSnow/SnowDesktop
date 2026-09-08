#pragma once

#include "../settings_route.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace snowdesktop::winui
{

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
    /** Deployment ownership controls whether the Store update button exists. */
    std::optional<bool> packaged;
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
    // Value 4 belonged to the retired release-repository link.
    SourceRepository = 5,
    QqGroup,
    EverythingSdk,
    DearImGui,
    Lua,
    PinyinData,
    TranslucentTb,
    OfficialWebsite,
};

[[nodiscard]] constexpr std::wstring_view HomeAboutLinkUri(
    HomeAboutLink link) noexcept
{
    switch (link)
    {
    case HomeAboutLink::OfficialWebsite:
        return L"https://snowdesktop.com/";
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
    // Value 1 belonged to the removed network-update cancellation.
    OpenProject = 2,
    OpenLicense,
    OpenThirdPartyNotices,
};

} // namespace snowdesktop::winui

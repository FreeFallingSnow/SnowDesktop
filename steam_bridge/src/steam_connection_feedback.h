// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace snowdesktop::steam_bridge
{
enum class SteamConnectionProblem : std::uint8_t
{
    None,
    ClientUnavailable,
    Offline,
    InitializationFailed,
    ClientOutdated,
    InterfaceUnavailable,
    AppIdMismatch,
    SteamworksUnavailable,
};

inline SteamConnectionProblem ClassifySteamConnectionProblem(
    std::string_view code, std::optional<std::uint32_t> steamInitResult = {})
{
    if (code == "steam_not_logged_on") return SteamConnectionProblem::Offline;
    if (code == "steam_interface_unavailable")
        return SteamConnectionProblem::InterfaceUnavailable;
    if (code == "steam_app_id_mismatch")
        return SteamConnectionProblem::AppIdMismatch;
    if (code == "steamworks_unavailable")
        return SteamConnectionProblem::SteamworksUnavailable;
    if (code == "steam_init_failed" || code == "steam_initialization_failed")
    {
        // ESteamAPIInitResult values. Missing/unknown results from older/newer
        // bridges stay generic; NoSteamClient does not prove Steam is stopped.
        if (steamInitResult == 2u) return SteamConnectionProblem::ClientUnavailable;
        if (steamInitResult == 3u) return SteamConnectionProblem::ClientOutdated;
        return SteamConnectionProblem::InitializationFailed;
    }
    return SteamConnectionProblem::None;
}

struct SteamConnectionFeedback
{
    const char* key;
    const char* chinese;
    const char* english;
    bool offerOpenSteam;
};

inline SteamConnectionFeedback ConnectionFeedback(SteamConnectionProblem problem)
{
    switch (problem)
    {
    case SteamConnectionProblem::ClientUnavailable:
        return {"workshop_manager.steam_unavailable_hint",
            "无法连接 Steam 客户端。请启动 Steam；若已打开，请确认两者使用同一 Windows 用户和相同权限，再重试。",
            "Cannot connect to the Steam client. Start Steam; if it is already open, make sure both apps use the same Windows user and privilege level, then retry.", true};
    case SteamConnectionProblem::Offline:
        return {"workshop_manager.steam_offline_hint",
            "Steam 当前离线或账户尚未登录。请恢复网络并登录 Steam，确认客户端不再显示“无连接”后重试。",
            "Steam is offline or the account is not signed in. Restore the connection and sign in to Steam, then retry once the client no longer shows No Connection.", true};
    case SteamConnectionProblem::ClientOutdated:
        return {"workshop_manager.steam_outdated_hint",
            "Steam 客户端版本不兼容，缺少所需接口。请在 Steam 菜单中检查客户端更新，更新后完全退出并重新打开 Steam，再从库中启动 SnowDesktop 重试。若仍失败，请提供“帮助 → 关于 Steam”中的客户端版本、构建日期及下方错误详情。",
            "The Steam client version is incompatible and lacks a required interface. Check for client updates in the Steam menu, then fully exit and reopen Steam. Launch SnowDesktop from your library and retry. If it still fails, share the client version and build date from Help → About Steam, along with the error details below.", true};
    case SteamConnectionProblem::InterfaceUnavailable:
        return {"workshop_manager.steam_interface_hint",
            "Steam 接口不可用。请更新并重启 Steam；若仍失败，请验证 SnowDesktop 文件完整性。",
            "Steam interfaces are unavailable. Update and restart Steam; if the problem persists, verify the integrity of SnowDesktop files.", true};
    case SteamConnectionProblem::AppIdMismatch:
        return {"workshop_manager.steam_app_mismatch_hint",
            "Steam 应用身份不匹配。请从 Steam 库启动 SnowDesktop，并验证文件完整性。",
            "The Steam app identity does not match. Launch SnowDesktop from your Steam library and verify its file integrity.", false};
    case SteamConnectionProblem::SteamworksUnavailable:
        return {"workshop_manager.steam_bridge_unavailable_hint",
            "当前 Steam Bridge 不支持 Steam 功能。请在 Steam 中验证 SnowDesktop 文件完整性。",
            "This Steam Bridge does not support Steam features. Verify the integrity of SnowDesktop files in Steam.", false};
    case SteamConnectionProblem::InitializationFailed:
        return {"workshop_manager.steam_initialization_hint",
            "Steam 初始化失败。请重启 Steam，并从 Steam 库启动 SnowDesktop；若仍失败，请提供下方错误详情。",
            "Steam initialization failed. Restart Steam and launch SnowDesktop from your Steam library; if it still fails, share the error details below.", true};
    case SteamConnectionProblem::None:
        return {"", "", "", false};
    }
    return {"", "", "", false};
}
}

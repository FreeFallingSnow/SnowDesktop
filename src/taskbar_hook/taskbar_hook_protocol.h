#pragma once

#include <windows.h>

#include <cstdint>

namespace snowdesktop::taskbar_hook
{
inline constexpr std::uint32_t kSharedStateMagic = 0x53445442; // "SDTB"
inline constexpr std::uint32_t kSharedStateVersion = 4;

inline constexpr wchar_t kSharedStateName[] =
    L"Local\\SnowDesktop.TaskbarBackdrop.State.v4";
inline constexpr wchar_t kReadyEventName[] =
    L"Local\\SnowDesktop.TaskbarBackdrop.Ready.v4";
inline constexpr wchar_t kApplyMessageName[] =
    L"SnowDesktop.TaskbarBackdrop.Apply.v4";

inline constexpr LONG kStatusIdle = 0;
inline constexpr LONG kStatusInjecting = 1;
inline constexpr LONG kStatusConnected = 2;
inline constexpr LONG kStatusApplied = 3;
inline constexpr LONG kStatusFailed = -1;

inline constexpr LONG kStyleGlassBackdrop = 1 << 0;
inline constexpr LONG kStyleAcrylicBackdrop = 1 << 1;

struct SharedState
{
    std::uint32_t magic = kSharedStateMagic;
    std::uint32_t version = kSharedStateVersion;
    std::uint32_t size = sizeof(SharedState);
    volatile LONG generation = 0;
    volatile LONG enabled = FALSE;
    volatile LONG style = 0;
    volatile LONG contentTheme = 0; // 0=dark(white text), 1=light(black text)
    volatile LONG systemUsesLightTheme = TRUE; // 1=system light, 0=system dark
    DWORD ownerProcessId = 0;
    DWORD explorerProcessId = 0;
    float red = 0.08f;
    float green = 0.10f;
    float blue = 0.13f;
    float alpha = 0.36f;
    float blurAmount = 24.0f;
    float borderRed = 1.0f;
    float borderGreen = 1.0f;
    float borderBlue = 1.0f;
    float borderAlpha = 0.40f;
    volatile LONG status = kStatusIdle;
    volatile LONG lastError = ERROR_SUCCESS;
    volatile LONG diagnosticStage = 0;
};

struct Snapshot
{
    LONG generation = 0;
    bool enabled = false;
    LONG style = 0;
    LONG contentTheme = 0;
    LONG systemUsesLightTheme = TRUE;
    DWORD ownerProcessId = 0;
    float red = 0.08f;
    float green = 0.10f;
    float blue = 0.13f;
    float alpha = 0.36f;
    float blurAmount = 24.0f;
    float borderRed = 1.0f;
    float borderGreen = 1.0f;
    float borderBlue = 1.0f;
    float borderAlpha = 0.40f;
};
}

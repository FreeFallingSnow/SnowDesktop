#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace snowdesktop::taskbar_hook
{
inline constexpr std::uint32_t kSharedStateMagic = 0x53445442; // "SDTB"
inline constexpr std::uint32_t kSharedStateVersion = 5;
inline constexpr std::size_t kMaximumTaskbarTargets = 32;

inline constexpr wchar_t kSharedStateName[] =
    L"Local\\SnowDesktop.TaskbarBackdrop.State.v5";
inline constexpr wchar_t kReadyEventName[] =
    L"Local\\SnowDesktop.TaskbarBackdrop.Ready.v5";
inline constexpr wchar_t kApplyMessageName[] =
    L"SnowDesktop.TaskbarBackdrop.Apply.v5";
inline constexpr wchar_t kTaskViewStateMessageName[] =
    L"SnowDesktop.Taskbar.Dynamic.TaskView.v1";
inline constexpr wchar_t kRegistryQueryMessageName[] =
    L"SnowDesktop.RegistryQuery.Apply.v1";
inline constexpr wchar_t kRegistryQueryMappingPrefix[] =
    L"Local\\SnowDesktop.RegistryQuery.State.v1";

inline constexpr std::uint32_t kRegistryQueryMagic = 0x53445251; // "SDRQ"
inline constexpr std::uint32_t kRegistryQueryVersion = 1;
inline constexpr std::size_t kMaximumRegistrySubKeyLength = 512;
inline constexpr std::size_t kMaximumRegistryValueNameLength = 128;
inline constexpr std::size_t kMaximumRegistryValueBytes = 32;
inline constexpr LONG kRegistryQueryPending = 0;
inline constexpr LONG kRegistryQueryCompleted = 1;

inline constexpr LONG kStatusIdle = 0;
inline constexpr LONG kStatusInjecting = 1;
inline constexpr LONG kStatusConnected = 2;
inline constexpr LONG kStatusApplied = 3;
inline constexpr LONG kStatusFailed = -1;

inline constexpr LONG kStyleGlassBackdrop = 1 << 0;
inline constexpr LONG kStyleAcrylicBackdrop = 1 << 1;

struct TargetAppearance
{
    std::uintptr_t taskbar = 0;
    LONG enabled = FALSE;
    LONG style = 0;
    LONG contentTheme = 0;
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

struct SharedState
{
    std::uint32_t magic = kSharedStateMagic;
    std::uint32_t version = kSharedStateVersion;
    std::uint32_t size = sizeof(SharedState);
    volatile LONG generation = 0;
    // enabled controls hook lifetime; defaultEnabled controls the visual
    // fallback used when no per-taskbar record matches.
    volatile LONG enabled = FALSE;
    volatile LONG defaultEnabled = FALSE;
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
    volatile LONG targetCount = 0;
    TargetAppearance targets[kMaximumTaskbarTargets]{};
    volatile LONG status = kStatusIdle;
    volatile LONG lastError = ERROR_SUCCESS;
    volatile LONG diagnosticStage = 0;
};

struct SharedRegistryQueryState
{
    std::uint32_t magic = kRegistryQueryMagic;
    std::uint32_t version = kRegistryQueryVersion;
    std::uint32_t size = sizeof(SharedRegistryQueryState);
    DWORD ownerProcessId = 0;
    wchar_t subKey[kMaximumRegistrySubKeyLength]{};
    wchar_t valueName[kMaximumRegistryValueNameLength]{};
    volatile LONG status = kRegistryQueryPending;
    LONG queryResult = ERROR_GEN_FAILURE;
    DWORD valueType = REG_NONE;
    DWORD valueSize = 0;
    BYTE value[kMaximumRegistryValueBytes]{};
};

struct Snapshot
{
    LONG generation = 0;
    bool enabled = false;
    bool defaultEnabled = false;
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
    LONG targetCount = 0;
    TargetAppearance targets[kMaximumTaskbarTargets]{};
};
}

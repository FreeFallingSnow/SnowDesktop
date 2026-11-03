#pragma once

#include <windows.h>

namespace snowdesktop::dock_window_rules
{
enum class DockClickAction
{
    None,
    Launch,
    Activate,
    Minimize,
    Restore,
};

enum class DockWindowIconSource
{
    None,
    AppUserModel,
    Executable,
    Window,
    GenericExecutable,
};

enum class DockWindowActivationObservationAction
{
    Stop,
    WaitForRestore,
    Activate,
};

struct DockRunningIndicatorColor
{
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    float alpha = 1.0f;
};

enum class DockWindowMinimizeRequestRoute
{
    None,
    PostedSystemCommand,
    AsyncShowFallback,
    DefaultSystemCommandFallback,
};

struct DockWindowActivationObservation
{
    static constexpr ULONGLONG RestoreTimeoutMs = 5000;
    static constexpr ULONGLONG ActivationTimeoutMs = 1000;

    bool awaitingRestore = false;
    ULONGLONG deadline = 0;
    DWORD processId = 0;
    DWORD threadId = 0;

    void Begin(bool restoring, ULONGLONG now, DWORD process, DWORD thread) noexcept;

    void Restored(ULONGLONG now) noexcept;

    bool Expired(ULONGLONG now) const noexcept;

    bool Matches(DWORD process, DWORD thread) const noexcept;
};
}

#include "taskbar_autohide_observer.h"
#include "taskbar_hook_lifecycle.h"
#include <commctrl.h>
#include <atomic>

namespace snowdesktop::taskbar_hook::autohide_observer
{
namespace
{
std::atomic<AutoHideTraceBuffer*> output{nullptr};

void Record(AutoHideTraceKind kind, HWND window, WPARAM wParam, LPARAM lParam) noexcept
{
    auto* buffer = output.load(std::memory_order_acquire);
    if (!buffer) return;
    const DWORD savedError = GetLastError();
    AutoHideTraceRecord record;
    record.tick = GetTickCount64();
    record.kind = kind;
    record.threadId = GetCurrentThreadId();
    record.taskbar = reinterpret_cast<std::uintptr_t>(window);
    record.foreground = reinterpret_cast<std::uintptr_t>(GetForegroundWindow());
    record.previous = static_cast<std::uintptr_t>(lParam);
    record.activation = LOWORD(wParam);
    record.geometryValid = GetWindowRect(window, &record.rect) && GetCursorPos(&record.cursor);
    AppendAutoHideTrace(*buffer, record);
    SetLastError(savedError);
}
}

bool Disable() noexcept
{
    output.store(nullptr, std::memory_order_release);
    return true;
}

void Configure(AutoHideTraceBuffer* buffer, HWND taskbar, bool enabled,
    bool protectActivation) noexcept
{
    if (hookInstanceRetired.load()) { Disable(); return; }
    output.store(enabled ? buffer : nullptr, std::memory_order_release);
    if (!taskbar) return;
    if (enabled && protectActivation)
        SetPropW(taskbar, kActivationProtectionProperty, reinterpret_cast<HANDLE>(1));
    else RemovePropW(taskbar, kActivationProtectionProperty);
}

LRESULT Dispatch(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message != WM_ACTIVATE || !output.load(std::memory_order_acquire))
        return DefSubclassProc(window, message, wParam, lParam);
    Record(AutoHideTraceKind::ActivateBefore, window, wParam, lParam);
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    Record(AutoHideTraceKind::ActivateAfter, window, wParam, lParam);
    return result;
}
}

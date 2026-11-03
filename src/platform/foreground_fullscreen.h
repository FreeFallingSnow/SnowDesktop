#pragma once

#include <windows.h>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace snowdesktop::fullscreen
{
// A native window is sampled once, then shared by all monitors in a scan.
// Invisible, minimized and ignored-process windows never enter the scan.
struct WindowSample
{
    HWND window = nullptr;
    RECT client{};
    LONG_PTR style = 0, extended = 0;
    bool maximized = false, cloaked = false, shell = false;
    RECT frame{};
    bool nonRude = false;
    DWORD process = 0;
};

struct WindowSnapshot
{
    HWND window = nullptr;
    HMONITOR monitor = nullptr;
    DWORD process = 0;
    RECT client{}, bounds{};
};

bool ClientCoversMonitor(const RECT& client, const RECT& monitor, LONG edgeInset = 0) noexcept;

bool IsWindowedMaximization(LONG_PTR style, bool maximized) noexcept;

bool IsCandidate(LONG_PTR style, LONG_PTR extended, bool maximized, bool shell) noexcept;

bool IsFullscreen(const WindowSample& window, const RECT& monitor) noexcept;

std::optional<WindowSample> SampleWindow(
    HWND window, DWORD ignoredProcess = GetCurrentProcessId()) noexcept;

std::optional<WindowSnapshot> ObserveWindow(
    HWND window, DWORD ignoredProcess = GetCurrentProcessId()) noexcept;

std::optional<WindowSnapshot> ObserveForeground() noexcept;

std::vector<std::optional<WindowSnapshot>> ObserveMonitors(
    std::span<const HMONITOR> monitors, DWORD ignoredProcess = GetCurrentProcessId());

std::optional<WindowSnapshot> ObserveMonitor(
    HMONITOR monitor, DWORD ignoredProcess = GetCurrentProcessId());

// Re-observe at the side-effect boundary, including any attached-input retry.
template<class Observe, class Activate>
bool GuardFocusRequest(bool deliberateFullscreenExit, Observe&& observe, Activate&& activate)
{
    if (!deliberateFullscreenExit && observe()) return false;
    std::forward<Activate>(activate)();
    return true;
}

} // namespace snowdesktop::fullscreen

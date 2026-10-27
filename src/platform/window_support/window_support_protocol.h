#pragma once

#include "window_capabilities.h"
#include <windows.h>
#include <array>
#include <type_traits>

namespace snowdesktop::window_support::protocol
{
inline constexpr std::uint32_t Version = 1;
inline constexpr std::uint32_t Magic = 0x57534453;
inline constexpr std::size_t MaximumTargets = 128;
inline constexpr std::size_t MaximumPointerEvents = 32;
inline constexpr wchar_t HelperFilename[] = L"SnowDesktopWindowHelper.exe";
inline constexpr wchar_t TaskFolder[] = L"\\SnowDesktop\\WindowSupport";
enum class Command : std::uint32_t
{
    Hello, Poll, TogglePin, PinAppearance, PeekTarget, CloseWindow, MinimizeWindow,
    RestoreWindow, ConfigureTargets, StartMinimize, StartRestore, CancelAnimation,
    CompleteAnimation, ClearPins, Disconnect,
};
struct WindowIdentity
{
    std::uint64_t window = 0;
    std::uint32_t process = 0;
    std::uint32_t thread = 0;
};
inline WindowIdentity Capture(HWND window) noexcept
{
    DWORD process = 0;
    const DWORD thread = window ? GetWindowThreadProcessId(window, &process) : 0;
    return {static_cast<std::uint64_t>(reinterpret_cast<UINT_PTR>(window)), process, thread};
}
inline HWND Window(const WindowIdentity& value) noexcept
{ return reinterpret_cast<HWND>(static_cast<UINT_PTR>(value.window)); }
inline bool Current(const WindowIdentity& value) noexcept
{
    const auto current = Capture(Window(value));
    return value.window && value.process && value.thread && IsWindow(Window(value)) &&
        current.process == value.process && current.thread == value.thread;
}
struct PinAppearance
{
    std::uint32_t enabled = 1;
    float width = 6.0f, red = 0.0f, green = 0.47f, blue = 0.84f, opacity = 0.5f;
};
struct Target
{
    WindowIdentity identity;
    RECT anchor{};
    std::uint64_t keepBelow = 0;
    std::uint32_t effect = 0;
    std::uint32_t animate = 0;
    std::uint32_t speed = 1, frameLimit = 0, dockPosition = 0;
};
struct Request
{
    std::uint32_t magic = Magic, version = Version;
    Command command = Command::Hello;
    std::uint32_t hostProcess = 0;
    std::uint64_t sequence = 0;
    WindowIdentity target;
    PinAppearance appearance;
    Target animation;
    std::uint32_t targetCount = 0;
    std::array<Target, MaximumTargets> targets{};
};
struct PointerEvent
{
    std::uint64_t sequence = 0;
    POINT point{};
    std::uint32_t message = 0, time = 0, mouseData = 0, flags = 0;
    std::uint32_t buttons = 0;
};
struct Response
{
    std::uint32_t magic = Magic, version = Version;
    std::uint64_t sequence = 0;
    CapabilityMask capabilities = 0;
    std::uint32_t error = ERROR_SUCCESS;
    std::uint32_t asynchronousError = ERROR_SUCCESS;
    std::uint32_t succeeded = 0;
    std::uint32_t buttons = 0;
    std::uint32_t pointerCount = 0;
    std::array<PointerEvent, MaximumPointerEvents> pointerEvents{};
    WindowIdentity animationTarget;
    std::uint64_t presentation = 0;
    std::uint32_t animationActive = 0, direction = 0;
    WindowIdentity externalMinimize;
};
static_assert(std::is_trivially_copyable_v<Request> && std::is_trivially_copyable_v<Response>);
}

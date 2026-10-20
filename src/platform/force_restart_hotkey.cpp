#include "force_restart_hotkey.h"
#include "single_instance.h"

#include <future>
#include <iterator>
#include <system_error>
#include <string>
#include <utility>

namespace snowdesktop
{
namespace
{
constexpr UINT kApply = WM_APP + 1;
constexpr wchar_t kWindowClass[] = L"SnowDesktop.ForceRestartHotkey.v1";
constexpr UINT kModifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN;
}

DWORD ForceRestartCurrentProcess()
{
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable,
        static_cast<DWORD>(std::size(executable)));
    if (!length) return GetLastError();
    if (length >= std::size(executable)) return ERROR_INSUFFICIENT_BUFFER;
    single_instance::PreparedRestart replacement;
    DWORD error = replacement.Prepare(std::wstring_view(executable, length));
    if (error != ERROR_SUCCESS) return error;
    error = replacement.Resume();
    if (error != ERROR_SUCCESS) return error;
    // ERROR_CANCELLED is intentional: the crash watchdog must not launch a
    // second replacement or count a user-requested restart as a crash.
    if (!TerminateProcess(GetCurrentProcess(), ERROR_CANCELLED)) return GetLastError();
    return ERROR_SUCCESS;
}

ForceRestartHotkey::ForceRestartHotkey(RestartAction restart)
    : restart_(std::move(restart)) {}

ForceRestartHotkey::~ForceRestartHotkey() { Stop(); }

UINT ForceRestartHotkey::Encode(UINT modifiers, UINT virtualKey) noexcept
{
    return virtualKey > 0 && virtualKey <= 0xff
        ? (virtualKey << 8) | (modifiers & kModifiers) : 0;
}

bool ForceRestartHotkey::Matches(UINT modifiers, UINT virtualKey) const noexcept
{
    const UINT chord = Encode(modifiers, virtualKey);
    return chord != 0 && registeredChord_.load() == chord;
}

void ForceRestartHotkey::Apply(bool enabled, UINT modifiers, UINT virtualKey)
{
    const UINT chord = enabled ? Encode(modifiers, virtualKey) : 0;
    if (!thread_.joinable())
    {
        if (!chord) return;
        std::promise<HWND> ready;
        auto result = ready.get_future();
        try
        {
            thread_ = std::thread([this, ready = std::move(ready)]() mutable {
                WNDCLASSW type{};
                type.hInstance = GetModuleHandleW(nullptr);
                type.lpfnWndProc = WindowProcedure;
                type.lpszClassName = kWindowClass;
                if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                {
                    lastError_ = GetLastError();
                    ready.set_value(nullptr);
                    return;
                }
                const HWND window = CreateWindowExW(0, kWindowClass, L"", 0,
                    0, 0, 0, 0, HWND_MESSAGE, nullptr, type.hInstance, this);
                if (!window) lastError_ = GetLastError();
                window_ = window;
                ready.set_value(window);
                if (!window) return;
                MSG message{};
                while (GetMessageW(&message, nullptr, 0, 0) > 0)
                    DispatchMessageW(&message);
                window_ = nullptr;
            });
        }
        catch (const std::system_error&)
        {
            lastError_ = ERROR_NOT_ENOUGH_MEMORY;
            return;
        }
        if (!result.get())
        {
            thread_.join();
            return;
        }
    }
    SendMessageW(Window(), kApply, chord, 0);
}

void ForceRestartHotkey::Stop() noexcept
{
    if (const HWND window = Window()) SendMessageW(window, WM_CLOSE, 0, 0);
    if (thread_.joinable()) thread_.join();
    settingsWindow_ = nullptr;
}

LRESULT CALLBACK ForceRestartHotkey::WindowProcedure(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* self = reinterpret_cast<ForceRestartHotkey*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        self = static_cast<ForceRestartHotkey*>(
            reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, wParam, lParam);
    switch (message)
    {
    case kApply:
    {
        const UINT chord = static_cast<UINT>(wParam);
        if (chord && chord == self->registeredChord_.load()) return 0;
        UnregisterHotKey(window, kForceRestartHotkeyId);
        self->registeredChord_ = 0;
        self->lastError_ = ERROR_SUCCESS;
        if (chord)
        {
            if (RegisterHotKey(window, kForceRestartHotkeyId,
                    (chord & kModifiers) | MOD_NOREPEAT, chord >> 8))
                self->registeredChord_ = chord;
            else self->lastError_ = GetLastError();
        }
        return 0;
    }
    case WM_HOTKEY:
        if (wParam == kForceRestartHotkeyId &&
            self->Matches(LOWORD(lParam), HIWORD(lParam)))
        {
            DWORD_PTR consumed = 0;
            const HWND settings = self->settingsWindow_.load();
            const UINT captureMessage = ForceRestartCaptureMessage();
            if (settings && captureMessage && SendMessageTimeoutW(settings,
                    captureMessage, LOWORD(lParam), HIWORD(lParam),
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &consumed) && consumed)
                return 0;
            try
            {
                self->lastError_ = self->restart_();
            }
            catch (...)
            {
                self->lastError_ = ERROR_UNHANDLED_EXCEPTION;
            }
            if (self->LastError() != ERROR_SUCCESS)
            {
                OutputDebugStringW(L"SnowDesktop: force restart failed; current host retained.\n");
                MessageBeep(MB_ICONERROR);
            }
        }
        return 0;
    case WM_CLOSE:
        UnregisterHotKey(window, kForceRestartHotkeyId);
        self->registeredChord_ = 0;
        DestroyWindow(window);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

ForceRestartHotkey& EmergencyRestartHotkey()
{
    static ForceRestartHotkey hotkey;
    return hotkey;
}
}

#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <thread>

namespace snowdesktop
{
// Shared with the settings process. Reply 1 only when a recorder consumed the
// chord; a bounded query keeps a hung settings process from blocking recovery.
inline UINT ForceRestartCaptureMessage()
{
    static const UINT message = RegisterWindowMessageW(
        L"SnowDesktop.ForceRestart.CaptureHotkey.v1");
    return message;
}

inline constexpr int kForceRestartHotkeyId = 1;

// Prepares/resumes a replacement that waits for this PID, then terminates only
// this host with a non-crash exit code. Does not flush settings or layout.
DWORD ForceRestartCurrentProcess();

class ForceRestartHotkey final
{
public:
    using RestartAction = std::function<DWORD()>;
    explicit ForceRestartHotkey(RestartAction restart = ForceRestartCurrentProcess);
    ~ForceRestartHotkey();
    ForceRestartHotkey(const ForceRestartHotkey&) = delete;
    ForceRestartHotkey& operator=(const ForceRestartHotkey&) = delete;

    void Apply(bool enabled, UINT modifiers, UINT virtualKey);
    void Stop() noexcept;
    void SetSettingsWindow(HWND window) noexcept { settingsWindow_.store(window); }
    bool Matches(UINT modifiers, UINT virtualKey) const noexcept;
    DWORD LastError() const noexcept { return lastError_.load(); }
    HWND Window() const noexcept { return window_.load(); }

private:
    static LRESULT CALLBACK WindowProcedure(HWND, UINT, WPARAM, LPARAM);
    static UINT Encode(UINT modifiers, UINT virtualKey) noexcept;
    RestartAction restart_;
    std::thread thread_;
    std::atomic<HWND> window_{nullptr};
    std::atomic<HWND> settingsWindow_{nullptr};
    std::atomic<UINT> registeredChord_{0};
    std::atomic<DWORD> lastError_{ERROR_SUCCESS};
};

ForceRestartHotkey& EmergencyRestartHotkey();
}

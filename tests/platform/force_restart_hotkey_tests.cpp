#include "platform/force_restart_hotkey.h"
#include "platform/single_instance.h"

#include <atomic>
#include <future>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

namespace
{
int failures = 0;
constexpr UINT kModifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
constexpr wchar_t kMarkerVariable[] = L"SNOWDESKTOP_TEST_FORCE_RESTART_EVENT";

void Check(bool result, const char* message)
{
    if (result) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

struct SettingsPeer
{
    std::atomic<bool> capturing{true};
    std::atomic<bool> blocked{false};
    std::atomic<UINT> capturedKey{0};
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE received = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HWND window = nullptr;
    std::thread thread;

    SettingsPeer()
    {
        std::promise<HWND> ready;
        auto result = ready.get_future();
        thread = std::thread([this, ready = std::move(ready)]() mutable {
            WNDCLASSW type{};
            type.hInstance = GetModuleHandleW(nullptr);
            type.lpfnWndProc = Procedure;
            type.lpszClassName = L"SnowDesktop.Test.RestartSettings";
            RegisterClassW(&type);
            window = CreateWindowExW(0, type.lpszClassName, L"", 0,
                0, 0, 0, 0, HWND_MESSAGE, nullptr, type.hInstance, this);
            ready.set_value(window);
            if (!window) return;
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0)
                DispatchMessageW(&message);
        });
        Check(result.get() != nullptr, "isolated settings peer starts");
    }
    ~SettingsPeer()
    {
        SetEvent(release);
        if (window) SendMessageW(window, WM_CLOSE, 0, 0);
        thread.join();
        CloseHandle(received);
        CloseHandle(release);
    }
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<SettingsPeer*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<SettingsPeer*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == snowdesktop::ForceRestartCaptureMessage())
        {
            self->capturedKey = static_cast<UINT>(lParam);
            SetEvent(self->received);
            if (self->blocked) WaitForSingleObject(self->release, 5000);
            return self->capturing ? 1 : 0;
        }
        if (message == WM_CLOSE)
        {
            DestroyWindow(window);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
};

void TestRegistrationAndRecoveryThread()
{
    std::atomic<int> restarts{0};
    HANDLE restarted = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // Only process termination is replaced here. Registration, the independent
    // message loop, settings delivery and bounded timeout are production code.
    snowdesktop::ForceRestartHotkey hotkey([&] {
        ++restarts;
        SetEvent(restarted);
        return ERROR_SUCCESS;
    });
    hotkey.Apply(true, kModifiers, VK_F20);
    Check(hotkey.Matches(kModifiers, VK_F20), "production registrar owns the configured shortcut");
    Check(!RegisterHotKey(nullptr, 9001, kModifiers, VK_F20),
        "a competing registration cannot acquire the reserved chord");
    UnregisterHotKey(nullptr, 9001);

    const auto trigger = [&](UINT key) {
        SendMessageW(hotkey.Window(), WM_HOTKEY, snowdesktop::kForceRestartHotkeyId,
            MAKELPARAM(kModifiers, key));
    };
    trigger(VK_F21);
    Check(restarts == 0, "unrelated or stale hotkey messages cannot restart the host");
    hotkey.Apply(true, kModifiers, VK_F21);
    Check(hotkey.Matches(kModifiers, VK_F21) && !hotkey.Matches(kModifiers, VK_F20),
        "changing the shortcut switches the active registration");
    const bool oldAvailable = RegisterHotKey(nullptr, 9001, kModifiers, VK_F20) != FALSE;
    Check(oldAvailable, "changing the shortcut releases the old chord");
    UnregisterHotKey(nullptr, 9001);

    SettingsPeer settings;
    hotkey.SetSettingsWindow(settings.window);
    trigger(VK_F21);
    Check(restarts == 0 && settings.capturedKey == VK_F21,
        "recording consumes the registered chord without restarting");
    settings.capturing = false;
    trigger(VK_F21);
    Check(restarts == 1, "an idle settings page allows the force restart action");
    ResetEvent(restarted);
    settings.blocked = true;
    // The caller performs no message pumping. A hung settings peer and idle
    // host UI thread cannot prevent the independently owned hotkey from firing.
    const ULONGLONG before = GetTickCount64();
    PostMessageW(hotkey.Window(), WM_HOTKEY, snowdesktop::kForceRestartHotkeyId,
        MAKELPARAM(kModifiers, VK_F21));
    const DWORD wait = WaitForSingleObject(restarted, 2000);
    Check(wait == WAIT_OBJECT_0 && GetTickCount64() - before < 1500 && restarts == 2,
        "blocked UI and settings threads cannot stall emergency recovery");
    SetEvent(settings.release);
    hotkey.SetSettingsWindow(nullptr);

    hotkey.Apply(false, kModifiers, VK_F21);
    trigger(VK_F21);
    Check(restarts == 2 && !hotkey.Matches(kModifiers, VK_F21),
        "disabling the shortcut prevents activation, including queued messages");
    hotkey.Apply(true, kModifiers, 0);
    Check(!hotkey.Matches(kModifiers, 0), "clearing the chord leaves it unregistered");
    hotkey.Apply(true, kModifiers, VK_F21);
    hotkey.Stop();
    Check(!hotkey.Window() && !hotkey.Matches(kModifiers, VK_F21),
        "shutdown releases the shortcut and worker window");
    const bool released = RegisterHotKey(nullptr, 9001, kModifiers, VK_F21) != FALSE;
    Check(released, "shutdown makes the chord available again");
    UnregisterHotKey(nullptr, 9001);
    CloseHandle(restarted);
}

void TestRealProcessRestart()
{
    const auto name = L"Local\\SnowDesktop.Test.ForceRestart." + std::to_wstring(GetCurrentProcessId());
    HANDLE marker = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --force-restart-test-child";
    SetEnvironmentVariableW(kMarkerVariable, name.c_str());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION child{};
    const bool created = CreateProcessW(executable, command.data(), nullptr, nullptr,
        FALSE, 0, nullptr, nullptr, &startup, &child) != FALSE;
    SetEnvironmentVariableW(kMarkerVariable, nullptr);
    Check(created, "isolated restart host launches");
    if (created)
    {
        const DWORD waited = WaitForSingleObject(child.hProcess, 15000);
        DWORD exitCode = STILL_ACTIVE;
        GetExitCodeProcess(child.hProcess, &exitCode);
        Check(waited == WAIT_OBJECT_0 && exitCode == ERROR_CANCELLED,
            "the real restart terminates only the old host with a non-crash exit code");
        Check(WaitForSingleObject(marker, 15000) == WAIT_OBJECT_0,
            "the real replacement starts after its predecessor exits");
        if (waited != WAIT_OBJECT_0) TerminateProcess(child.hProcess, ERROR_CANCELLED);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
    }
    CloseHandle(marker);
}
}

int wmain(int argc, wchar_t** argv)
{
    const DWORD predecessor = snowdesktop::single_instance::
        ParseRestartPredecessorProcessId(GetCommandLineW());
    if (predecessor)
    {
        if (!snowdesktop::single_instance::WaitForRestartPredecessor(predecessor, 10000)) return 3;
        wchar_t name[256]{};
        if (!GetEnvironmentVariableW(kMarkerVariable, name, 256)) return 4;
        HANDLE marker = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
        if (!marker) return 5;
        SetEvent(marker);
        CloseHandle(marker);
        return 0;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--force-restart-test-child")
    {
        snowdesktop::ForceRestartHotkey hotkey;
        hotkey.Apply(true, kModifiers, VK_F22);
        if (!hotkey.Matches(kModifiers, VK_F22)) return 6;
        PostMessageW(hotkey.Window(), WM_HOTKEY, snowdesktop::kForceRestartHotkeyId,
            MAKELPARAM(kModifiers, VK_F22));
        // Deliberately no host message loop: exercise real process replacement
        // while the initiating/UI thread is unresponsive.
        HANDLE never = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        WaitForSingleObject(never, 10000);
        CloseHandle(never);
        return 7;
    }
    TestRegistrationAndRecoveryThread();
    TestRealProcessRestart();
    return failures ? 1 : 0;
}

#pragma once
#include <windows.h>
#include "../platform/hook_registration_retry.h"
#include <thread>
#include <mutex>
#include <condition_variable>

namespace snowdesktop::dock_minimize
{
inline constexpr wchar_t kCloakDispatcherProperty[] = L"SnowDesktop.Dock.CloakDispatcher.v1";
inline constexpr wchar_t kCloakDispatchMessage[] = L"SnowDesktop.Dock.DispatchCloak.v1";
inline UINT CloakDispatchMessage() noexcept
{
    static const UINT message = RegisterWindowMessageW(kCloakDispatchMessage);
    return message;
}
struct HookApi
{
    decltype(&SetWindowsHookExW) install = SetWindowsHookExW;
    decltype(&UnhookWindowsHookEx) remove = UnhookWindowsHookEx;
};
// Runs in the injector's bitness. The target's CBT callback can send to the
// host while this separate thread sends the source-cloak command back.
class CloakDispatcher final
{
public:
    ~CloakDispatcher() { Stop(); }
    bool Start(HMODULE module, HOOKPROC procedure, HookApi api);
    void Stop() noexcept;
    HWND Window() const noexcept { return window_; }
    hook_registration::Statistics Statistics() const;
    std::size_t HookCount() const;
private:
    static LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Apply(HWND source, bool cloak) noexcept;
    void Run() noexcept;
    HMODULE module_ = nullptr;
    HOOKPROC procedure_ = nullptr;
    HookApi api_;
    HWND window_ = nullptr;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    bool started_ = false;
    hook_registration::Retry retry_;
    hook_registration::Statistics statistics_;
    std::size_t activeHooks_ = 0;
};
}

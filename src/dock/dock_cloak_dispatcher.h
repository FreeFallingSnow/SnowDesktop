#pragma once
#include <windows.h>
#include "../platform/hook_registration_retry.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <limits>

namespace snowdesktop::dock_minimize
{
inline constexpr ULONG_PTR kCloakRequestTag = 0x53444332;
struct CloakRequest
{
    std::uint32_t version = 2;
    std::uint32_t cloak = 0;
    std::uint64_t source = 0;
    std::uint64_t owner = 0;
    std::uint64_t process = 0;
    std::uint64_t token = 0;
};
static_assert(sizeof(CloakRequest) == 40);
inline bool ReadCloakRequest(const COPYDATASTRUCT* data, CloakRequest& request) noexcept
{
    if (!data || data->dwData != kCloakRequestTag || data->cbData != sizeof(request) || !data->lpData) return false;
    std::memcpy(&request, data->lpData, sizeof(request));
    const auto maximum = static_cast<std::uint64_t>((std::numeric_limits<UINT_PTR>::max)());
    return request.version == 2 && request.cloak <= 1 && request.source && request.owner && request.process && request.token &&
        request.source <= maximum && request.owner <= maximum && request.process <= MAXDWORD && request.token <= maximum;
}
inline bool SendCloakRequest(HWND target, const CloakRequest& request, DWORD timeout, DWORD_PTR& result) noexcept
{
    COPYDATASTRUCT data{kCloakRequestTag, sizeof(request), const_cast<CloakRequest*>(&request)};
    return SendMessageTimeoutW(target, WM_COPYDATA, static_cast<WPARAM>(request.owner),
        reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, timeout, &result) != 0;
}
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
    LRESULT Apply(const CloakRequest& request) noexcept;
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

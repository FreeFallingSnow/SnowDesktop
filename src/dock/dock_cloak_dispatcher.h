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
inline constexpr ULONG_PTR kCloakRequestTag = 0x53444335;
inline constexpr std::uint32_t kPrepareRestoreCloak = 2;
struct CloakRequest
{
    std::uint32_t version = 5;
    std::uint32_t cloak = 0;
    std::uint64_t source = 0;
    std::uint64_t owner = 0;
    std::uint64_t process = 0;
    std::uint64_t token = 0;
    // Nonzero, never reused by this sender. The low two bits bind the operation.
    std::uint64_t requestId = 0;
};
static_assert(sizeof(CloakRequest) == 48);
inline bool ReadCloakRequest(const COPYDATASTRUCT* data, CloakRequest& request) noexcept
{
    if (!data || data->dwData != kCloakRequestTag || data->cbData != sizeof(request) || !data->lpData) return false;
    std::memcpy(&request, data->lpData, sizeof(request));
    const auto maximum = static_cast<std::uint64_t>((std::numeric_limits<UINT_PTR>::max)());
    return request.version == 5 && request.cloak <= kPrepareRestoreCloak && request.source && request.owner && request.process && request.token &&
        request.source <= maximum && request.owner <= maximum && request.process <= MAXDWORD && request.token <= maximum &&
        request.requestId && request.requestId <= MAXDWORD && (request.requestId & 3u) == request.cloak;
}
inline bool SendCloakDispatchRequest(HWND dispatcher, const CloakRequest& request, DWORD timeout, DWORD_PTR& result) noexcept
{
    COPYDATASTRUCT data{kCloakRequestTag, sizeof(request), const_cast<CloakRequest*>(&request)};
    return SendMessageTimeoutW(dispatcher, WM_COPYDATA, static_cast<WPARAM>(request.owner),
        reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, timeout, &result) != 0;
}
inline constexpr wchar_t kCloakDispatcherProperty[] = L"SnowDesktop.Dock.CloakDispatcher.v1";
inline constexpr wchar_t kCloakDispatchMessage[] = L"SnowDesktop.Dock.DispatchCloak.v1";
inline constexpr wchar_t kForwardedRequest[] = L"SnowDesktop.Dock.CloakForward.Request.v1";
inline constexpr wchar_t kForwardedSource[] = L"SnowDesktop.Dock.CloakForward.Source.v1";
inline constexpr wchar_t kForwardedOwner[] = L"SnowDesktop.Dock.CloakForward.Owner.v1";
inline constexpr wchar_t kForwardedProcess[] = L"SnowDesktop.Dock.CloakForward.Process.v1";
inline constexpr wchar_t kForwardedToken[] = L"SnowDesktop.Dock.CloakForward.Token.v1";

inline UINT CloakDispatchMessage() noexcept
{
    static const UINT message = RegisterWindowMessageW(kCloakDispatchMessage);
    return message;
}

inline bool ReadForwardedCloakRequest(HWND source, HWND dispatcher, DWORD id, CloakRequest& request) noexcept
{
    if (!id || !dispatcher || GetPropW(source, kCloakDispatcherProperty) != dispatcher)
        return false;
    const HANDLE marker = ULongToHandle(id);
    if (GetPropW(dispatcher, kForwardedRequest) != marker ||
        GetPropW(dispatcher, kForwardedSource) != source)
        return false;
    request.source = reinterpret_cast<UINT_PTR>(source);
    request.owner = reinterpret_cast<UINT_PTR>(GetPropW(dispatcher, kForwardedOwner));
    request.process = HandleToULong(GetPropW(dispatcher, kForwardedProcess));
    request.token = reinterpret_cast<UINT_PTR>(GetPropW(dispatcher, kForwardedToken));
    request.requestId = id;
    request.cloak = id & 3u;
    // A timed-out sender can withdraw the request while the hook is reading.
    return request.owner && request.process && request.token && request.cloak <= kPrepareRestoreCloak &&
        GetPropW(dispatcher, kForwardedRequest) == marker &&
        GetPropW(source, kCloakDispatcherProperty) == dispatcher;
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

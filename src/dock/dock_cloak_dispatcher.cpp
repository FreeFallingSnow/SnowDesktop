#include "dock_cloak_dispatcher.h"
#include "dock_minimize_protocol.h"
#include "dock_window_source_cloak.h"

namespace snowdesktop::dock_minimize
{
bool CloakDispatcher::Start(HMODULE module, HOOKPROC procedure, HookApi api)
{
    if (thread_.joinable()) return window_ != nullptr;
    module_ = module; procedure_ = procedure; api_ = api;
    if (!module_ || !procedure_) return false;
    try { thread_ = std::thread([this] { Run(); }); }
    catch (...) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
    std::unique_lock lock(mutex_);
    ready_.wait(lock, [this] { return started_; });
    return window_ != nullptr;
}
void CloakDispatcher::Run() noexcept
{
    constexpr wchar_t name[] = L"SnowDesktop.Dock.CloakDispatcher.v1";
    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = name; type.lpfnWndProc = WindowProc;
    const bool registered = RegisterClassW(&type) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    const HWND window = registered ? CreateWindowExW(0, name, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, type.hInstance, this) : nullptr;
    {
        std::lock_guard lock(mutex_); window_ = window; started_ = true;
    }
    ready_.notify_one();
    if (!window) return;
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
}
LRESULT CALLBACK CloakDispatcher::WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_NCCREATE)
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* self = reinterpret_cast<CloakDispatcher*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    CloakRequest request;
    if (self && message == WM_COPYDATA && ReadCloakRequest(reinterpret_cast<const COPYDATASTRUCT*>(lp), request))
        return self->Apply(request);
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wp, lp);
}
LRESULT CloakDispatcher::Apply(const CloakRequest& request) noexcept
{
    const HWND source = reinterpret_cast<HWND>(static_cast<UINT_PTR>(request.source));
    const bool cloak = request.cloak != 0;
    if (GetPropW(source, kCloakDispatcherProperty) != window_) return E_ABORT;
    const HWND receiver = reinterpret_cast<HWND>(GetPropW(source, kTargetProperty));
    if (!HasLiveOwner(receiver)) return E_ABORT;
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(source, &process);
    if (!thread || !process) return E_INVALIDARG;
    const auto owner = dock_source_cloak::RequestOwner(request);
    if (!dock_source_cloak::SameOwner(source, owner) || (cloak && !dock_source_cloak::IsLiveOwner(owner))) return E_ABORT;
    {
        std::lock_guard lock(mutex_);
        if (!retry_.Ready(GetTickCount64())) return HRESULT_FROM_WIN32(retry_.Error());
    }
    SetLastError(0);
    const HHOOK hook = api_.install(WH_CALLWNDPROC, procedure_, module_, thread);
    const DWORD error = hook ? ERROR_SUCCESS : (GetLastError() ? GetLastError() : ERROR_GEN_FAILURE);
    {
        std::lock_guard lock(mutex_);
        ++statistics_.attempts; statistics_.lastThread = thread; statistics_.lastError = error;
        if (hook) { ++statistics_.successes; ++activeHooks_; retry_.Reset(); }
        else { ++statistics_.failures; retry_.Failed(error, GetTickCount64()); }
    }
    if (!hook) return HRESULT_FROM_WIN32(error);
    DWORD_PTR ignored = 0;
    SetLastError(0);
    // Forward the caller's frozen lease; never substitute a newer ReadOwner.
    const bool sent = SendCloakRequest(source, request, 120, ignored);
    const DWORD sendError = GetLastError();
    api_.remove(hook);
    { std::lock_guard lock(mutex_); --activeHooks_; }
    return sent ? S_OK : HRESULT_FROM_WIN32(sendError ? sendError : ERROR_TIMEOUT);
}
hook_registration::Statistics CloakDispatcher::Statistics() const
{
    std::lock_guard lock(mutex_); return statistics_;
}
std::size_t CloakDispatcher::HookCount() const
{
    std::lock_guard lock(mutex_); return activeHooks_;
}
void CloakDispatcher::Stop() noexcept
{
    if (window_) PostMessageW(window_, WM_CLOSE, 0, 0);
    if (thread_.joinable()) thread_.join();
    window_ = nullptr; module_ = nullptr; procedure_ = nullptr; started_ = false;
    retry_.Reset();
}
}

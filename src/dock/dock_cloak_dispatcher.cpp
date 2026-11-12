#include "dock_cloak_dispatcher.h"
#include "dock_minimize_protocol.h"
#include "dock_window_source_cloak.h"

namespace snowdesktop::dock_minimize
{
namespace
{
HRESULT ForwardCloakRequest(HWND dispatcher, HWND source, const CloakRequest& request, DWORD timeout) noexcept
{
    const UINT message = CloakDispatchMessage();
    if (!message) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

    // Publish the frozen lease before waking the source thread. A registered
    // message keeps this control request separate from application WM_COPYDATA.
    struct WithdrawRequest
    {
        HWND dispatcher;
        ~WithdrawRequest()
        {
            RemovePropW(dispatcher, kForwardedRequest);
            RemovePropW(dispatcher, kForwardedToken);
            RemovePropW(dispatcher, kForwardedProcess);
            RemovePropW(dispatcher, kForwardedOwner);
            RemovePropW(dispatcher, kForwardedSource);
        }
    } withdraw{dispatcher};
    SetLastError(0);
    if (!SetPropW(dispatcher, kForwardedSource, source) ||
        !SetPropW(dispatcher, kForwardedOwner, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(request.owner))) ||
        !SetPropW(dispatcher, kForwardedProcess, ULongToHandle(static_cast<DWORD>(request.process))) ||
        !SetPropW(dispatcher, kForwardedToken, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(request.token))) ||
        !SetPropW(dispatcher, kForwardedRequest, ULongToHandle(static_cast<DWORD>(request.requestId))))
    {
        const DWORD error = GetLastError();
        return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
    }

    DWORD_PTR ignored = 0;
    SetLastError(0);
    if (SendMessageTimeoutW(source, message, reinterpret_cast<WPARAM>(dispatcher),
            static_cast<LPARAM>(request.requestId), SMTO_ABORTIFHUNG, timeout, &ignored))
        return S_OK;
    const DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_TIMEOUT);
}
}

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
    // A sent message can reenter this dispatcher while a request is in flight.
    if (GetPropW(window_, kForwardedRequest)) return HRESULT_FROM_WIN32(ERROR_BUSY);
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
    const DWORD timeout = cloak ? CloakSendBudget(owner.window, 120) : 120;
    const HRESULT sent = timeout ? ForwardCloakRequest(window_, source, request, timeout) :
        HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    api_.remove(hook);
    { std::lock_guard lock(mutex_); --activeHooks_; }
    return sent;
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

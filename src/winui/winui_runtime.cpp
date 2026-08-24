#include "pch.h"

#include "winui_runtime.h"

#include "App.xaml.h"

#include <Microsoft.UI.Dispatching.Interop.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxh = winrt::Microsoft::UI::Xaml::Hosting;
namespace mud = winrt::Microsoft::UI::Dispatching;

namespace
{
std::wstring FormatError(const wchar_t* stage, HRESULT result)
{
    wchar_t code[24]{};
    swprintf_s(code, L"0x%08X", static_cast<unsigned int>(result));

    std::wstring message(stage ? stage : L"WinUI");
    message += L" (";
    message += code;
    message += L")";

    wchar_t* systemMessage = nullptr;
    DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(result),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&systemMessage),
        0,
        nullptr);
    if (length != 0 && systemMessage)
    {
        while (length > 0 &&
               (systemMessage[length - 1] == L'\r' ||
                systemMessage[length - 1] == L'\n'))
        {
            systemMessage[length - 1] = L'\0';
            --length;
        }
        message += L": ";
        message += systemMessage;
    }
    if (systemMessage)
        LocalFree(systemMessage);
    return message;
}
}

struct WinUiRuntime::Impl
{
    DWORD ownerThreadId = 0;
    bool initialized = false;
    bool ownsDispatcherQueue = false;
    bool addedControlParentStyle = false;
    HWND parentWindow = nullptr;
    HWND islandWindow = nullptr;
    HWND lastFocusedWindow = nullptr;
    LONG_PTR originalParentExStyle = 0;
    std::wstring lastError;

    mud::DispatcherQueue dispatcherQueue{nullptr};
    mud::DispatcherQueueController dispatcherController{nullptr};
    winrt::com_ptr<winrt::SnowDesktop::implementation::App> application;
    muxh::DesktopWindowXamlSource xamlSource{nullptr};
    winrt::event_token takeFocusRequestedToken{};

    [[nodiscard]] bool OnOwnerThread() const noexcept
    {
        return ownerThreadId != 0 &&
            ownerThreadId == GetCurrentThreadId();
    }

    void SetError(const wchar_t* stage, HRESULT result)
    {
        lastError = FormatError(stage, result);
    }

    void MoveFocusOut(
        const muxh::DesktopWindowXamlSourceTakeFocusRequestedEventArgs& args)
        noexcept
    {
        if (!parentWindow || !IsWindow(parentWindow))
            return;

        const bool moveBackward =
            args.Request().Reason() ==
            muxh::XamlSourceFocusNavigationReason::Last;
        HWND target = GetNextDlgTabItem(
            parentWindow, islandWindow, moveBackward ? TRUE : FALSE);
        if (!target || target == islandWindow)
            target = parentWindow;
        SetFocus(target);
    }
};

WinUiRuntime::WinUiRuntime()
    : impl_(std::make_unique<Impl>())
{
}

WinUiRuntime::~WinUiRuntime()
{
    Shutdown();
}

bool WinUiRuntime::Initialize() noexcept
{
    if (impl_->initialized)
    {
        if (impl_->OnOwnerThread())
            return true;
        impl_->SetError(L"WinUI runtime used from a different thread",
                        RPC_E_WRONG_THREAD);
        return false;
    }

    impl_->lastError.clear();
    impl_->ownerThreadId = GetCurrentThreadId();

    try
    {
        impl_->dispatcherQueue = mud::DispatcherQueue::GetForCurrentThread();
        if (!impl_->dispatcherQueue)
        {
            impl_->dispatcherController =
                mud::DispatcherQueueController::CreateOnCurrentThread();
            impl_->dispatcherQueue =
                impl_->dispatcherController.DispatcherQueue();
            impl_->ownsDispatcherQueue = true;
        }

        // A custom Application is required so Microsoft.UI.Xaml.Controls can
        // resolve metadata and theme resources inside a Win32 XAML Island.
        impl_->application =
            winrt::make_self<winrt::SnowDesktop::implementation::App>();
        impl_->initialized = true;
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(L"Initialize WinUI runtime", error.code());
    }
    catch (...)
    {
        impl_->SetError(L"Initialize WinUI runtime", E_FAIL);
    }

    impl_->application = nullptr;
    if (impl_->ownsDispatcherQueue && impl_->dispatcherController)
    {
        try
        {
            impl_->dispatcherController.ShutdownQueue();
        }
        catch (...)
        {
        }
    }
    impl_->dispatcherController = nullptr;
    impl_->dispatcherQueue = nullptr;
    impl_->ownsDispatcherQueue = false;
    impl_->ownerThreadId = 0;
    return false;
}

void WinUiRuntime::Shutdown() noexcept
{
    if (!impl_->initialized)
        return;
    if (!impl_->OnOwnerThread())
    {
        impl_->SetError(L"Shutdown WinUI runtime from a different thread",
                        RPC_E_WRONG_THREAD);
        return;
    }

    Detach();

    if (impl_->application)
    {
        impl_->application->Close();
        impl_->application = nullptr;
    }

    if (impl_->ownsDispatcherQueue && impl_->dispatcherController)
    {
        try
        {
            impl_->dispatcherController.ShutdownQueue();
        }
        catch (...)
        {
        }
    }

    impl_->dispatcherController = nullptr;
    impl_->dispatcherQueue = nullptr;
    impl_->ownsDispatcherQueue = false;
    impl_->initialized = false;
    impl_->ownerThreadId = 0;
}

bool WinUiRuntime::Attach(
    HWND parentWindow,
    const mux::UIElement& content) noexcept
{
    if (!impl_->initialized || !impl_->OnOwnerThread())
    {
        impl_->SetError(L"Attach XAML Island before WinUI initialization",
                        impl_->initialized ? RPC_E_WRONG_THREAD : E_UNEXPECTED);
        return false;
    }
    if (!parentWindow || !IsWindow(parentWindow) || !content)
    {
        impl_->SetError(L"Attach XAML Island with invalid content or HWND",
                        E_INVALIDARG);
        return false;
    }

    Detach();
    impl_->lastError.clear();
    impl_->parentWindow = parentWindow;

    try
    {
        impl_->originalParentExStyle =
            GetWindowLongPtrW(parentWindow, GWL_EXSTYLE);
        if ((impl_->originalParentExStyle & WS_EX_CONTROLPARENT) == 0)
        {
            SetWindowLongPtrW(
                parentWindow,
                GWL_EXSTYLE,
                impl_->originalParentExStyle | WS_EX_CONTROLPARENT);
            impl_->addedControlParentStyle = true;
        }

        impl_->xamlSource = muxh::DesktopWindowXamlSource{};
        impl_->xamlSource.Initialize(
            winrt::Microsoft::UI::GetWindowIdFromWindow(parentWindow));
        impl_->islandWindow = winrt::Microsoft::UI::GetWindowFromWindowId(
            impl_->xamlSource.SiteBridge().WindowId());
        if (!impl_->islandWindow)
            winrt::throw_hresult(E_HANDLE);

        LONG_PTR style =
            GetWindowLongPtrW(impl_->islandWindow, GWL_STYLE);
        style &= ~static_cast<LONG_PTR>(WS_POPUP);
        style |= WS_CHILD | WS_VISIBLE | WS_TABSTOP;
        SetWindowLongPtrW(impl_->islandWindow, GWL_STYLE, style);

        impl_->takeFocusRequestedToken =
            impl_->xamlSource.TakeFocusRequested(
                [state = impl_.get()](
                    const muxh::DesktopWindowXamlSource&,
                    const muxh::
                        DesktopWindowXamlSourceTakeFocusRequestedEventArgs&
                            args) { state->MoveFocusOut(args); });
        impl_->xamlSource.Content(content);
        ResizeToClient();
        ShowWindow(impl_->islandWindow, SW_SHOW);
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(L"Attach XAML Island", error.code());
    }
    catch (...)
    {
        impl_->SetError(L"Attach XAML Island", E_FAIL);
    }

    const std::wstring error = impl_->lastError;
    Detach();
    impl_->lastError = error;
    return false;
}

void WinUiRuntime::Detach() noexcept
{
    if (!impl_->OnOwnerThread())
        return;

    if (impl_->xamlSource)
    {
        try
        {
            if (impl_->takeFocusRequestedToken.value != 0)
            {
                impl_->xamlSource.TakeFocusRequested(
                    impl_->takeFocusRequestedToken);
            }
            impl_->takeFocusRequestedToken = {};
            impl_->xamlSource.Content(nullptr);
            impl_->xamlSource.Close();
        }
        catch (...)
        {
        }
    }
    impl_->xamlSource = nullptr;
    impl_->islandWindow = nullptr;
    impl_->lastFocusedWindow = nullptr;

    if (impl_->addedControlParentStyle && impl_->parentWindow &&
        IsWindow(impl_->parentWindow))
    {
        SetWindowLongPtrW(
            impl_->parentWindow,
            GWL_EXSTYLE,
            impl_->originalParentExStyle);
    }
    impl_->addedControlParentStyle = false;
    impl_->originalParentExStyle = 0;
    impl_->parentWindow = nullptr;
}

void WinUiRuntime::ResizeToClient() noexcept
{
    if (!impl_->OnOwnerThread() || !impl_->xamlSource ||
        !impl_->parentWindow || !IsWindow(impl_->parentWindow))
    {
        return;
    }

    RECT client{};
    if (!GetClientRect(impl_->parentWindow, &client))
        return;

    const auto width = static_cast<std::int32_t>(
        std::max<LONG>(0, client.right - client.left));
    const auto height = static_cast<std::int32_t>(
        std::max<LONG>(0, client.bottom - client.top));
    try
    {
        impl_->xamlSource.SiteBridge().MoveAndResize(
            {0, 0, width, height});
    }
    catch (...)
    {
    }
}

bool WinUiRuntime::PreTranslateMessage(MSG* message) noexcept
{
    if (!impl_->initialized || !impl_->OnOwnerThread() || !message)
        return false;
    if (!impl_->parentWindow || !impl_->islandWindow || !message->hwnd)
        return false;

    // ContentPreTranslateMessage is process-global. Feeding it input for the
    // desktop, Dock, or helper HWNDs lets the Island consume unrelated keys.
    // Include owned XAML popup windows through GA_ROOTOWNER while keeping the
    // settings top-level window as the only accepted root.
    const HWND target = message->hwnd;
    if (target != impl_->parentWindow && target != impl_->islandWindow &&
        !IsChild(impl_->parentWindow, target) &&
        GetAncestor(target, GA_ROOTOWNER) != impl_->parentWindow)
    {
        return false;
    }
    return ::ContentPreTranslateMessage(message);
}

bool WinUiRuntime::ProcessTabNavigation(MSG* message) noexcept
{
    if (!impl_->xamlSource || !impl_->OnOwnerThread() || !message ||
        message->message != WM_KEYDOWN || message->wParam != VK_TAB)
    {
        return false;
    }

    const HWND focused = GetFocus();
    if (focused && focused != impl_->parentWindow &&
        GetAncestor(focused, GA_ROOT) != impl_->parentWindow)
    {
        return false;
    }

    const bool moveBackward =
        (static_cast<unsigned short>(GetKeyState(VK_SHIFT)) & 0x8000u) != 0;
    const HWND next = GetNextDlgTabItem(
        impl_->parentWindow, focused, moveBackward ? TRUE : FALSE);
    if (next != impl_->islandWindow)
        return false;

    try
    {
        muxh::XamlSourceFocusNavigationRequest request{
            moveBackward
                ? muxh::XamlSourceFocusNavigationReason::Last
                : muxh::XamlSourceFocusNavigationReason::First};
        impl_->xamlSource.NavigateFocus(request);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void WinUiRuntime::HandleWindowMessage(
    UINT message, WPARAM wParam, LPARAM) noexcept
{
    if (!impl_->OnOwnerThread())
        return;

    switch (message)
    {
    case WM_SIZE:
    case WM_DPICHANGED:
        ResizeToClient();
        break;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE)
        {
            const HWND focused = GetFocus();
            impl_->lastFocusedWindow =
                focused && GetAncestor(focused, GA_ROOT) == impl_->parentWindow
                    ? focused
                    : nullptr;
        }
        else if (impl_->lastFocusedWindow &&
                 IsWindow(impl_->lastFocusedWindow))
        {
            SetFocus(impl_->lastFocusedWindow);
        }
        break;
    case WM_NCDESTROY:
        Detach();
        break;
    default:
        break;
    }
}

bool WinUiRuntime::IsInitialized() const noexcept
{
    return impl_->initialized;
}

bool WinUiRuntime::IsAttached() const noexcept
{
    return impl_->xamlSource != nullptr;
}

HWND WinUiRuntime::ParentWindow() const noexcept
{
    return impl_->parentWindow;
}

HWND WinUiRuntime::IslandWindow() const noexcept
{
    return impl_->islandWindow;
}

const std::wstring& WinUiRuntime::LastError() const noexcept
{
    return impl_->lastError;
}
}

#pragma once
#include <windows.h>
#include <cwchar>
#include <iterator>
#include <string_view>

namespace snowdesktop::shell_extensions
{
// Host-owned verbs refer to the selected desktop object only at the aggregate
// root. The same canonical verb inside a provider's cascade belongs to that
// provider, for example PowerShell 7's nested "runas" command.
inline bool IsRootMenuCommand(HMENU root, UINT command)
{
    for (int index = 0; index < GetMenuItemCount(root); ++index)
    {
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_ID;
        if (GetMenuItemInfoW(root, index, TRUE, &item) && item.wID == command)
            return true;
    }
    return false;
}

enum class PopupContents { Readable, Deferred, Native };

// Lazy Shell cascades can contain a dummy command with an ID but no text.
// Inspect the native popup before assigning tokens, otherwise the dummy looks
// executable and becomes an extra ellipsis row in the custom menu.
inline PopupContents InspectPopup(HMENU menu)
{
    if (!IsMenu(menu)) return PopupContents::Native;
    bool hasCommand = false, hasPlaceholder = false;
    for (int index = 0; index < GetMenuItemCount(menu); ++index)
    {
        wchar_t label[2048]{};
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_STRING | MIIM_FTYPE;
        item.dwTypeData = label;
        item.cch = static_cast<UINT>(std::size(label));
        if (!GetMenuItemInfoW(menu, index, TRUE, &item))
            return PopupContents::Native;
        if (item.fType & MFT_SEPARATOR)
            continue;
        if (item.fType & MFT_OWNERDRAW)
            return PopupContents::Native;
        const std::wstring_view text(label);
        if (text.empty() || text == L"…" || text == L"...")
            hasPlaceholder = true;
        else
            hasCommand = true;
    }
    if (!hasCommand) return PopupContents::Deferred;
    return hasPlaceholder ? PopupContents::Native : PopupContents::Readable;
}
inline bool RequiresNativePopup(HMENU menu)
{
    return InspectPopup(menu) != PopupContents::Readable;
}

namespace popup_reader_detail
{
struct Context
{
    HWND forwardingOwner;
    HMENU menu;
    UINT parentIndex;
    bool initialized = false;
    HWND nativeWindow = nullptr;
    WNDPROC nativeProc = nullptr;
};
inline thread_local Context *activeContext = nullptr;
inline LRESULT CALLBACK NativeProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    auto *context = activeContext;
    if (!context || context->nativeWindow != window || !context->nativeProc)
        return DefWindowProcW(window, message, wp, lp);
    const auto result = CallWindowProcW(context->nativeProc, window, message, wp, lp);
    if (message == WM_WINDOWPOSCHANGING)
    {
        auto *position = reinterpret_cast<WINDOWPOS *>(lp);
        position->flags &= ~SWP_SHOWWINDOW;
        position->flags |= SWP_HIDEWINDOW | SWP_NOACTIVATE;
    }
    return result;
}
inline LRESULT CALLBACK HidePopup(int code, WPARAM wp, LPARAM lp)
{
    if (code == HCBT_CREATEWND && activeContext && !activeContext->nativeWindow)
    {
        wchar_t name[32]{};
        const HWND window = reinterpret_cast<HWND>(wp);
        if (GetClassNameW(window, name, 32) && wcscmp(name, L"#32768") == 0)
        {
            // Attach before User32 can show its menu window. EndMenu during
            // WM_INITMENUPOPUP alone still permits one native show operation.
            activeContext->nativeWindow = window;
            activeContext->nativeProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window,
                GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(NativeProc)));
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
inline LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    auto *context = reinterpret_cast<Context *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        context = static_cast<Context *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
    }
    if (context && message == WM_INITMENUPOPUP && reinterpret_cast<HMENU>(wp) == context->menu)
    {
        SendMessageW(context->forwardingOwner, message, wp, MAKELPARAM(context->parentIndex, FALSE));
        context->initialized = true;
        // End during initialization. The scoped native window procedure also
        // suppresses User32's final show operation after this callback returns.
        EndMenu();
        return 0;
    }
    if (context && message == WM_TIMER && wp == 1)
    {
        EndMenu();
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
} // namespace popup_reader_detail

// Some IExplorerCommand adapters require the calling STA's real active menu
// state as well as WM_INITMENUPOPUP. Materialize only placeholder popups in the
// supervised helper; already readable and owner-drawn menus are left alone.
namespace popup_reader_detail
{
inline bool MaterializePopup(HMENU menu, HWND forwardingOwner, UINT parentIndex, bool retryPartial)
{
    const auto contents = InspectPopup(menu);
    if (!IsMenu(menu) || (contents != PopupContents::Deferred && !(retryPartial && contents == PopupContents::Native)) || !IsWindow(forwardingOwner) ||
        GetWindowThreadProcessId(forwardingOwner, nullptr) != GetCurrentThreadId()) return false;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    constexpr wchar_t className[] = L"SnowDesktopShellPopupReader";
    WNDCLASSW cls{};
    cls.lpfnWndProc = popup_reader_detail::Proc;
    cls.hInstance = instance;
    cls.lpszClassName = className;
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    popup_reader_detail::Context context{forwardingOwner, menu, parentIndex};
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, className, L"", WS_POPUP,
        -32000, -32000, 1, 1, nullptr, nullptr, instance, &context);
    if (!window) return false;
    auto *previous = popup_reader_detail::activeContext;
    popup_reader_detail::activeContext = &context;
    HHOOK hook = SetWindowsHookExW(WH_CBT, popup_reader_detail::HidePopup, nullptr, GetCurrentThreadId());
    // Bound an unexpected native-loop failure. Arbitrary extension callbacks
    // remain subject to the existing parent-supervised query deadline.
    // TPM_NONOTIFY would suppress WM_INITMENUPOPUP as well. TPM_RETURNCMD
    // already prevents WM_COMMAND; the initialization callback cancels.
    if (hook && SetTimer(window, 1, 1000, nullptr))
        TrackPopupMenuEx(menu, TPM_RETURNCMD, -32000, -32000, window, nullptr);
    if (context.nativeProc && IsWindow(context.nativeWindow))
        SetWindowLongPtrW(context.nativeWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(context.nativeProc));
    if (hook) UnhookWindowsHookEx(hook);
    popup_reader_detail::activeContext = previous;
    DestroyWindow(window);
    return context.initialized && !RequiresNativePopup(menu);
}
} // namespace popup_reader_detail
inline bool TryMaterializePopup(HMENU menu, HWND forwardingOwner, UINT parentIndex)
{
    return popup_reader_detail::MaterializePopup(menu, forwardingOwner, parentIndex, false);
}
// Most deferred handlers only need the STA menu loop. Creating a ShellView
// first can enumerate unavailable network resources even for a local file.
// Retain the live-view fallback for a popup that remains deferred or only
// partially materializes. Initially readable/owner-drawn menus are never probed.
// PowerShell's registered background adapters cache the first empty-folder
// result if initialized before their view site. Keep that narrow compatibility
// requirement; ordinary archive handlers need no view before materialization.
inline bool PopupNeedsInitialView(std::wstring_view verb)
{
    return verb == L"PowerShell7x64" || verb == L"PowerShell7x86";
}
template <class PrepareSite>
inline bool TryMaterializePopup(HMENU menu, HWND forwardingOwner, UINT parentIndex, PrepareSite &&prepareSite,
    bool prepareInitially = false)
{
    if (InspectPopup(menu) != PopupContents::Deferred || !IsWindow(forwardingOwner) ||
        GetWindowThreadProcessId(forwardingOwner, nullptr) != GetCurrentThreadId()) return false;
    if (prepareInitially) prepareSite();
    if (TryMaterializePopup(menu, forwardingOwner, parentIndex)) return true;
    if (prepareInitially || !RequiresNativePopup(menu) || !prepareSite()) return false;
    return popup_reader_detail::MaterializePopup(menu, forwardingOwner, parentIndex, true);
}
} // namespace snowdesktop::shell_extensions

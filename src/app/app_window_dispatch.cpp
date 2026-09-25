#include "app.h"
#include "../desktop_keyboard_rules.h"
#include "../animation_settings.h"

// Static window-procedure dispatch adapters.

void DesktopApp::BeginDesktopInteractionTrace(const wchar_t* reason)
{
    const ULONGLONG now = GetTickCount64();
    // Each click/menu/layer checkpoint receives a fresh budget, including
    // repeated clicks less than two seconds apart. Nested lifecycle messages
    // join the current trace in TraceDesktopWindowMessage instead of resetting it.
    ++desktopInteractionTraceId_;
    desktopInteractionTraceEvents_ = 0;
    desktopInteractionTraceFrames_ = 0;
    desktopInteractionTraceUntil_ = now + 2000;
    SYSTEM_POWER_STATUS power{};
    const bool powerKnown = GetSystemPowerStatus(&power) != FALSE;
    wchar_t event[256]{};
    swprintf_s(event,
        L"%ls powerKnown=%d ac=%u battery=%u saver=%u frameLimit=%d",
        reason, powerKnown ? 1 : 0,
        powerKnown ? static_cast<unsigned>(power.ACLineStatus) : 255U,
        powerKnown ? static_cast<unsigned>(power.BatteryLifePercent) : 255U,
        powerKnown ? static_cast<unsigned>(power.SystemStatusFlag) : 255U,
        snowdesktop::animation::RuntimeFrameLimit());
    TraceDesktopInteraction(event, hwnd_, 0, 0, 0, true);
}

void DesktopApp::TraceDesktopInteraction(const wchar_t* event, HWND subject,
    UINT message, WPARAM wp, LPARAM lp, bool force)
{
    const ULONGLONG now = GetTickCount64();
    // Synchronous log I/O must not turn this diagnostic into a source of lag.
    // Each input/menu checkpoint gets at most 64 window/focus records in 2 s.
    if (!force && (now > desktopInteractionTraceUntil_ ||
        desktopInteractionTraceEvents_ >= 64))
        return;
    ++desktopInteractionTraceEvents_;
    const HWND foreground = GetForegroundWindow();
    const DWORD foregroundThread = foreground
        ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    GUITHREADINFO gui{ sizeof(gui) };
    const bool focusKnown = foregroundThread && GetGUIThreadInfo(foregroundThread, &gui);
    const HWND parent = hwnd_ ? GetParent(hwnd_) : nullptr;
    RECT rect{};
    if (hwnd_) GetWindowRect(hwnd_, &rect);
    std::wostringstream line;
    line << L"DesktopInteraction id=" << desktopInteractionTraceId_
         << L" seq=" << desktopInteractionTraceEvents_ << L" tick=" << now
         << L" event=" << event << L" subject=" << subject
         << L" msg=0x" << std::hex << message << L" wp=0x" << wp
         << L" lp=0x" << lp << std::dec
         << L" render=" << hwnd_ << L" parent=" << parent
         << L" renderVisible=" << (hwnd_ && IsWindowVisible(hwnd_))
         << L" parentVisible=" << (parent && IsWindowVisible(parent))
         << L" prev=" << (hwnd_ ? GetWindow(hwnd_, GW_HWNDPREV) : nullptr)
         << L" input=" << inputHwnd_ << L" foreground=" << foreground
         << L" active=" << GetActiveWindow() << L" focus=" << GetFocus()
         << L" foregroundFocusKnown=" << focusKnown
         << L" foregroundFocus=" << gui.hwndFocus << L" capture=" << GetCapture()
         << L" rect=" << rect.left << L"," << rect.top << L"," << rect.right << L"," << rect.bottom
         << L" desktopVisible=" << customDesktopVisible_ << L" iconsHidden=" << desktopIconsHidden_
         << L" surface=" << dcompSurface_.Get() << L" commitPending=" << compositionCommitPending_
         << L" paintActive=" << compositionPaintInProgress_
         << L" recovery=" << graphicsDeviceRecovery_.Pending()
         << L" panels=" << desktopBackdropCompositor_.PanelCount();
    if ((message == WM_WINDOWPOSCHANGING || message == WM_WINDOWPOSCHANGED) && lp)
    {
        const auto& pos = *reinterpret_cast<const WINDOWPOS*>(lp);
        line << L" insertAfter=" << pos.hwndInsertAfter << L" posFlags=0x" << std::hex << pos.flags
             << std::dec << L" xywh=" << pos.x << L"," << pos.y << L"," << pos.cx << L"," << pos.cy;
    }
    WriteDiagnosticLogEntry(line.str().c_str(), DiagnosticLogLevel::Debug);
}

void DesktopApp::TraceDesktopWindowMessage(HWND subject, UINT message,
    WPARAM wp, LPARAM lp, bool afterDispatch)
{
    const bool input = message == WM_MOUSEACTIVATE || message == WM_LBUTTONDOWN ||
        message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDOWN || message == WM_CONTEXTMENU;
    const bool lifecycle = message == WM_SHOWWINDOW || message == WM_SIZE ||
        message == WM_DISPLAYCHANGE || message == WM_POWERBROADCAST;
    const bool state = message == WM_ACTIVATE || message == WM_ACTIVATEAPP ||
        message == WM_SETFOCUS || message == WM_KILLFOCUS ||
        message == WM_WINDOWPOSCHANGING || message == WM_WINDOWPOSCHANGED;
    if (!input && !lifecycle && !state) return;
    if (!afterDispatch && (input || message == WM_POWERBROADCAST ||
        (lifecycle && GetTickCount64() > desktopInteractionTraceUntil_)))
        BeginDesktopInteractionTrace(message == WM_POWERBROADCAST ? L"power-message" : L"window-message");
    TraceDesktopInteraction(afterDispatch ? L"message-after" : L"message-before",
        subject, message, wp, lp, lifecycle);
}

void DesktopApp::TraceDesktopPresentation(const wchar_t* event, HRESULT result,
    const RECT* dirty)
{
    if (GetTickCount64() > desktopInteractionTraceUntil_ ||
        desktopInteractionTraceFrames_ >= 12)
        return;
    ++desktopInteractionTraceFrames_;
    const RECT rect = dirty ? *dirty : RECT{};
    wchar_t detail[384]{};
    swprintf_s(detail,
        L"DesktopInteraction id=%llu tick=%llu event=%ls sample=%u hr=0x%08X "
        L"partial=%d dirty=%ld,%ld,%ld,%ld surface=%p pending=%d paintActive=%d recovery=%d",
        desktopInteractionTraceId_, GetTickCount64(), event,
        desktopInteractionTraceFrames_, static_cast<unsigned>(result),
        dirty ? 1 : 0, rect.left, rect.top, rect.right, rect.bottom,
        dcompSurface_.Get(), compositionCommitPending_ ? 1 : 0,
        compositionPaintInProgress_ ? 1 : 0, graphicsDeviceRecovery_.Pending() ? 1 : 0);
    WriteDiagnosticLogEntry(detail, DiagnosticLogLevel::Debug);
}

LRESULT CALLBACK DesktopApp::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    DesktopApp* app = nullptr;
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        app = static_cast<DesktopApp*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    else
    {
        app = reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (msg == WM_NCCREATE || msg == WM_CREATE)
    {
        wchar_t buf[128];
        wsprintfW(buf, L"WndProc msg=0x%04X app=%p", msg, app);
        WriteDiagnosticLogEntry(buf);
    }

    if (app)
    {
        app->TraceDesktopWindowMessage(hwnd, msg, wp, lp, false);
        const LRESULT result = app->HandleMessage(hwnd, msg, wp, lp);
        app->TraceDesktopWindowMessage(hwnd, msg, wp, lp, true);
        return result;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/**
 * @brief 快捷导航窗口的静态窗口过程
 *
 * @param hwnd 窗口句柄
 * @param msg  消息标识符
 * @param wp   WPARAM 参数
 * @param lp   LPARAM 参数
 * @return LRESULT 消息处理结果
 *
 * 在 WM_NCCREATE 时存储 DesktopApp 实例指针，
 * 后续消息转发至 HandleQuickNavigationMessage 处理。
 */
LRESULT CALLBACK DesktopApp::QuickNavigationWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    DesktopApp* app = nullptr;
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        app = static_cast<DesktopApp*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    else
    {
        app = reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (app)
        return app->HandleQuickNavigationMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK DesktopApp::FloatingDockWndProc(
    HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PersistentDockHost* host = nullptr;
    if (msg == WM_NCCREATE)
    {
        auto* create =
            reinterpret_cast<CREATESTRUCTW*>(lp);
        host = static_cast<PersistentDockHost*>(
            create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(host));
    }
    else
    {
        host = reinterpret_cast<PersistentDockHost*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (host && host->owner)
        return host->owner->HandleFloatingDockMessage(
            *host, hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK DesktopApp::FloatingPopupWndProc(
    HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    DesktopApp* app = nullptr;
    if (msg == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        app = static_cast<DesktopApp*>(create->lpCreateParams);
        SetWindowLongPtrW(
            hwnd, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(app));
    }
    else
    {
        app = reinterpret_cast<DesktopApp*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (app)
        return app->HandleFloatingPopupMessage(
            hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK DesktopApp::DragPreviewWndProc(
    HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    DesktopApp* app = nullptr;
    if (msg == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        app = static_cast<DesktopApp*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(app));
    }
    else
    {
        app = reinterpret_cast<DesktopApp*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (app)
        return app->HandleDragPreviewMessage(
            hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/**
 * @brief 独立键盘输入窗口的静态窗口过程。
 */
LRESULT CALLBACK DesktopApp::InputWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    DesktopApp* app = nullptr;
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        app = static_cast<DesktopApp*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    else
    {
        app = reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (app)
    {
        app->TraceDesktopWindowMessage(hwnd, msg, wp, lp, false);
        const LRESULT result = app->HandleInputMessage(hwnd, msg, wp, lp);
        app->TraceDesktopWindowMessage(hwnd, msg, wp, lp, true);
        return result;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT DesktopApp::HandleInputMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto focusedSearchWidget =
        [this]() -> ScrollingItemWidget* {
        for (auto& container : containers_)
        {
            auto* searchable =
                dynamic_cast<ScrollingItemWidget*>(
                    container.get());
            if (searchable &&
                searchable->IsSearchFocused())
                return searchable;
        }
        return nullptr;
    };

    switch (msg)
    {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTARROWS;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_IME_STARTCOMPOSITION:
        if (widgetEngine_ &&
            widgetEngine_->HasFocusedHostInput())
        {
            widgetEngine_->ClearHostInputComposition();
            UpdateHostInputImePosition();
            return 0;
        }
        if (auto* searchable = focusedSearchWidget())
        {
            searchable->ClearSearchComposition();
            UpdateHostInputImePosition();
            return 0;
        }
        break;
    case WM_IME_COMPOSITION:
    {
        const bool hostInputFocused =
            widgetEngine_ &&
            widgetEngine_->HasFocusedHostInput();
        ScrollingItemWidget* searchable =
            hostInputFocused
                ? nullptr : focusedSearchWidget();
        if (hostInputFocused || searchable)
        {
            const bool insertCompositionCharacter =
                (lp & CS_INSERTCHAR) != 0 &&
                (lp & (GCS_RESULTSTR | GCS_COMPSTR)) == 0 &&
                wp != 0;
            if (insertCompositionCharacter)
            {
                const std::wstring composition(
                    1, static_cast<wchar_t>(wp));
                const size_t cursor =
                    (lp & CS_NOMOVECARET) != 0
                        ? 0 : composition.size();
                if (hostInputFocused)
                {
                    widgetEngine_->SetHostInputComposition(
                        composition, cursor);
                }
                else
                {
                    searchable->SetSearchComposition(
                        composition, cursor);
                }
                UpdateHostInputImePosition();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }

            HIMC context = ImmGetContext(hwnd);
            if (context)
            {
                auto readCompositionString =
                    [&](DWORD index) {
                        std::wstring result;
                        const LONG byteCount =
                            ImmGetCompositionStringW(
                                context, index, nullptr, 0);
                        if (byteCount <= 0)
                            return result;
                        result.resize(static_cast<size_t>(
                            byteCount) / sizeof(wchar_t));
                        const LONG copied =
                            ImmGetCompositionStringW(
                                context, index, result.data(),
                                static_cast<DWORD>(byteCount));
                        if (copied >= 0)
                        {
                            result.resize(static_cast<size_t>(
                                copied) / sizeof(wchar_t));
                        }
                        else
                        {
                            result.clear();
                        }
                        return result;
                    };

                if ((lp & GCS_RESULTSTR) != 0)
                {
                    const std::wstring result =
                        readCompositionString(GCS_RESULTSTR);
                    if (hostInputFocused)
                    {
                        widgetEngine_->
                            CommitHostInputComposition(result);
                    }
                    else
                        searchable->
                            CommitSearchComposition(result);
                }
                if ((lp & (GCS_COMPSTR | GCS_CURSORPOS)) != 0)
                {
                    const std::wstring composition =
                        readCompositionString(GCS_COMPSTR);
                    const LONG imeCursor =
                        ImmGetCompositionStringW(
                            context, GCS_CURSORPOS, nullptr, 0);
                    const size_t cursor =
                        imeCursor >= 0
                            ? static_cast<size_t>(imeCursor)
                            : composition.size();
                    if (hostInputFocused)
                    {
                        widgetEngine_->
                            SetHostInputComposition(
                                composition, cursor);
                    }
                    else
                    {
                        searchable->SetSearchComposition(
                            composition, cursor);
                    }
                }
                else if (lp == 0)
                {
                    if (hostInputFocused)
                    {
                        widgetEngine_->
                            ClearHostInputComposition();
                    }
                    else
                        searchable->
                            ClearSearchComposition();
                }
                ImmReleaseContext(hwnd, context);
            }
            UpdateHostInputImePosition();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        break;
    }
    case WM_IME_ENDCOMPOSITION:
        if (widgetEngine_ &&
            widgetEngine_->HasFocusedHostInput())
        {
            widgetEngine_->ClearHostInputComposition();
            UpdateHostInputImePosition();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (auto* searchable = focusedSearchWidget())
        {
            searchable->ClearSearchComposition();
            UpdateHostInputImePosition();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_KEYDOWN:
    {
        const bool repeated =
            (static_cast<ULONG_PTR>(lp) & (ULONG_PTR{1} << 30)) != 0;
        if (TryHandlePageNavigationKey(wp, repeated))
            return 0;
        DispatchLuaWidgetViewKeyEvent(wp, true,
            repeated);
        if (widgetEngine_ && widgetEngine_->HandleHostInputKey(wp))
        {
            UpdateHostInputImePosition();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        OnKeyDown(wp, repeated);
        UpdateHostInputImePosition();
        return 0;
    }
    case WM_SYSKEYDOWN:
    {
        if (wp == VK_RETURN && OnKeyDown(wp,
                (static_cast<ULONG_PTR>(lp) & (ULONG_PTR{1} << 30)) != 0))
            return 0;
        using snowdesktop::desktop_keyboard_rules::AltF4Action;
        const AltF4Action altF4Action =
            snowdesktop::desktop_keyboard_rules::ResolveAltF4Action(
                hwnd == inputHwnd_,
                wp == VK_F4,
                (static_cast<ULONG_PTR>(lp) &
                    (ULONG_PTR{1} << 29)) != 0,
                (static_cast<ULONG_PTR>(lp) &
                    (ULONG_PTR{1} << 30)) != 0);
        if (altF4Action != AltF4Action::PassThrough)
        {
            if (altF4Action ==
                AltF4Action::RequestWindowsShutdownDialog)
                RequestWindowsShutdownDialog();
            return 0;
        }
        if (TryHandlePageNavigationKey(
                wp,
                (static_cast<ULONG_PTR>(lp) &
                    (ULONG_PTR{1} << 30)) != 0))
            return 0;
        if ((wp >= 'A' && wp <= 'Z') || (wp >= '0' && wp <= '9'))
            DispatchLuaWidgetViewKeyEvent(wp, true,
                (static_cast<ULONG_PTR>(lp) &
                    (ULONG_PTR{1} << 30)) != 0);
        break;
    }
    case WM_SYSCHAR:
    {
        if (wp > 0x7f) break;
        const char key = static_cast<char>(wp);
        if (!((key >= 'A' && key <= 'Z') ||
                (key >= 'a' && key <= 'z') ||
                (key >= '0' && key <= '9')))
            break;
        const bool repeated =
            (static_cast<ULONG_PTR>(lp) & (ULONG_PTR{1} << 30)) != 0;
        if (!OnKeyDown(static_cast<WPARAM>(key), repeated)) break;
        UpdateHostInputImePosition();
        return 0;
    }
    case WM_IME_CHAR:
    case WM_CHAR:
    {
        wchar_t ch = static_cast<wchar_t>(wp);
        if (widgetEngine_ && widgetEngine_->HandleHostInputChar(ch))
        {
            UpdateHostInputImePosition();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (ch >= 0x20 && ch != 0x7F)
        {
            for (auto& c : containers_)
            {
                auto* searchable = dynamic_cast<ScrollingItemWidget*>(c.get());
                if (searchable && searchable->IsSearchFocused())
                {
                    searchable->AppendSearchChar(ch);
                    UpdateHostInputImePosition();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    break;
                }
            }
        }
        return 0;
    }
    case WM_KEYUP:
        DispatchLuaWidgetViewKeyEvent(wp, false, false);
        RefreshDragHintFromKeyboard();
        return 0;
    case WM_SYSKEYUP:
        if ((wp >= 'A' && wp <= 'Z') || (wp >= '0' && wp <= '9'))
            DispatchLuaWidgetViewKeyEvent(wp, false, false);
        break;
    case WM_KILLFOCUS:
        CancelRenameClick();
        if (widgetEngine_)
        {
            widgetEngine_->ClearHostViewKeyState();
            widgetEngine_->CancelInteractionPointerPress();
        }
        break;
    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
        if (msg == WM_CANCELMODE) CancelRenameClick();
        ForgetLuaWidgetPanelCapture(hwnd);
        if (msg == WM_CANCELMODE ||
            !IsOwnedPointerCaptureWindow(
                reinterpret_cast<HWND>(lp)))
        {
            if (CanCancelPointerPressAfterCaptureLoss())
            {
                CancelPointerPressWithoutCaptureRelease();
            }
        }
        break;
    case WM_TIMER:
        OnTimer(wp);
        return 0;
    case kDesktopPassthroughExitMessage:
        if (desktopPassthroughIndicator_.OwnsWindow(reinterpret_cast<HWND>(wp)))
            EndDesktopPassthrough();
        return 0;
    case WM_HOTKEY:
        if (settingsWindow_ &&
            settingsWindow_->IsHotkeyCaptureActive())
        {
            settingsWindow_->CaptureRegisteredHotkey(
                LOWORD(lp), HIWORD(lp));
            return 0;
        }
        if (static_cast<int>(wp) == kQuickNavigationHotkeyId)
        {
            ToggleQuickNavigation();
            return 0;
        }
        if (static_cast<int>(wp) ==
            kFloatingDockHotkeyId)
        {
            ToggleFloatingDock();
            return 0;
        }
        if (static_cast<int>(wp) ==
            kDesktopPassthroughHotkeyId)
        {
            ToggleDesktopPassthrough();
            return 0;
        }
        break;
    case WM_DESTROY:
        if (navigationHotkeyHwnd_ == hwnd)
        {
            navigationHotkeyHwnd_ = nullptr;
            navigationHotkeyRegistered_ = false;
        }
        if (floatingDockHotkeyHwnd_ == hwnd)
        {
            floatingDockHotkeyHwnd_ = nullptr;
            floatingDockHotkeyRegistered_ = false;
        }
        if (desktopPassthroughHotkeyHwnd_ == hwnd)
        {
            EndDesktopPassthrough(false);
            desktopPassthroughHotkeyHwnd_ = nullptr;
            desktopPassthroughHotkeyRegistered_ = false;
        }
        if (floatingDockEdgeSwipeHwnd_ == hwnd)
        {
            floatingDockEdgeSwipeHwnd_ = nullptr;
            floatingDockEdgeSwipeDetector_.Reset();
        }
        if (inputHwnd_ == hwnd)
            inputHwnd_ = nullptr;
        if (floatingDockInputHwnd_ == hwnd)
            floatingDockInputHwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

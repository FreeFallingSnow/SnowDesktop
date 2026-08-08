#include "app.h"

// Static window-procedure dispatch adapters.

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

    // 只记录低频生命周期消息；逐帧记录 WM_PAINT 会造成同步文件 I/O
    // 和整份日志扫描。
    if (msg == WM_NCCREATE || msg == WM_CREATE || msg == WM_SIZE || msg == WM_SHOWWINDOW)
    {
        wchar_t buf[128];
        wsprintfW(buf, L"WndProc msg=0x%04X app=%p", msg, app);
        WriteDiagnosticLogEntry(buf);
    }

    if (app) return app->HandleMessage(hwnd, msg, wp, lp);
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
    DesktopApp* app = nullptr;
    if (msg == WM_NCCREATE)
    {
        auto* create =
            reinterpret_cast<CREATESTRUCTW*>(lp);
        app = static_cast<DesktopApp*>(
            create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(app));
    }
    else
    {
        app = reinterpret_cast<DesktopApp*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (app)
        return app->HandleFloatingDockMessage(
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
        return app->HandleInputMessage(hwnd, msg, wp, lp);
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
        if (widgetEngine_ && widgetEngine_->HandleHostInputKey(wp))
        {
            UpdateHostInputImePosition();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        OnKeyDown(wp);
        UpdateHostInputImePosition();
        return 0;
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
        RefreshDragHintFromKeyboard();
        return 0;
    case WM_TIMER:
        OnTimer(wp);
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
            BeginDesktopPassthroughHold();
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
            KillTimer(hwnd, kDesktopPassthroughHoldTimerId);
            desktopPassthroughHotkeyHwnd_ = nullptr;
            desktopPassthroughHotkeyRegistered_ = false;
            desktopPassthroughHoldActive_ = false;
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

#pragma once
// Inline implementations for DesktopApp — destructor, Run, WndProc, HandleMessage.
// This file is included by app_oo.h after the class definition.

// ── Inline implementations ──────────────────────────────────

inline DesktopApp::~DesktopApp()
{
    widgetEngine_.reset();
    settingsWindow_.reset();
    items_oo_.clear();
    containers_.clear();
    ClearMenuIcons();
    if (faMenuFont_)
    {
        DeleteObject(faMenuFont_);
        faMenuFont_ = nullptr;
    }
    if (faFontHandle_)
    {
        RemoveFontMemResourceEx(faFontHandle_);
        faFontHandle_ = nullptr;
    }
}

inline int DesktopApp::Run(HINSTANCE instance, int showCommand)
{
    auto L = [](const wchar_t* s) {
        HANDLE f = CreateFileW(L"SnowDesktop_crash.log", FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, s, static_cast<DWORD>(wcslen(s)*2), &w, nullptr);
            WriteFile(f, L"\r\n", 4, &w, nullptr); CloseHandle(f); }
    };

    L(L"Run start");

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    HRESULT hr = OleInitialize(nullptr);
    L(SUCCEEDED(hr) ? L"OleInit ok" : L"OleInit FAILED");

    instance_ = instance;

    // Find and hide Explorer icon layer
    desktopWindows_ = FindDesktopWindows();
    {
        wchar_t buf[256];
        wsprintfW(buf, L"Desktop: progman=%p defView=%p listView=%p host=%p",
            desktopWindows_.progman, desktopWindows_.defView,
            desktopWindows_.listView, desktopWindows_.host);
        L(buf);
    }
    if (desktopWindows_.listView && IsWindowVisible(desktopWindows_.listView))
    {
        SetPropW(desktopWindows_.listView, kHiddenBySnowDesktopProp, reinterpret_cast<HANDLE>(1));
        ShowWindow(desktopWindows_.listView, SW_HIDE);
        L(L"Explorer icon layer hidden");
    }
    else
    {
        L(L"Explorer icon layer not found or already hidden");
    }

    // Create desktop overlay window as child of desktop host
    virtualLeft_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
    virtualTop_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
    virtualWidth_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    virtualHeight_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    LoadLayoutSlots();
    UpdateLayoutWorkArea();

    HWND parent = desktopWindows_.host ? desktopWindows_.host : GetDesktopWindow();
    POINT origin{ virtualLeft_, virtualTop_ };
    ScreenToClient(parent, &origin);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"SnowDesktopWindow";
    RegisterClassExW(&wc);

    {
        WNDCLASSEXW hint{};
        hint.cbSize = sizeof(hint);
        hint.lpfnWndProc = DefWindowProcW;
        hint.hInstance = instance;
        hint.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        hint.hbrBackground = nullptr;
        hint.lpszClassName = kHintWindowClassName;
        RegisterClassExW(&hint);
    }

    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"SnowDesktop",
        WS_CHILD | WS_VISIBLE, origin.x, origin.y, virtualWidth_, virtualHeight_,
        parent, nullptr, instance, this);
    if (!hwnd_)
    {
        L(L"Child failed, fallback popup");
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"SnowDesktop",
            WS_POPUP | WS_VISIBLE, virtualLeft_, virtualTop_, virtualWidth_, virtualHeight_,
            nullptr, nullptr, instance, this);
        if (hwnd_ && parent && parent != GetDesktopWindow())
        {
            SetParent(hwnd_, parent);
            LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
            style &= ~WS_POPUP;
            style |= WS_CHILD | WS_VISIBLE;
            SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
            SetWindowPos(hwnd_, HWND_TOP, origin.x, origin.y, virtualWidth_, virtualHeight_,
                SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
    }
    if (!hwnd_) { L(L"CreateWindow FAILED"); return __LINE__; }
    SetWindowPos(hwnd_, HWND_TOP, origin.x, origin.y, virtualWidth_, virtualHeight_, SWP_NOACTIVATE);
    L(L"Window created");
    {
        wchar_t buf[256];
        wsprintfW(buf, L"Parent=%p origin=(%d,%d) size=%dx%d",
            parent, origin.x, origin.y, virtualWidth_, virtualHeight_);
        L(buf);
    }

    if (!InitGraphics()) { L(L"InitGraphics FAILED"); return __LINE__; }
    L(L"InitGraphics ok");

    // Create control window for tray icon ownership
    {
        WNDCLASSEXW cwc{};
        cwc.cbSize = sizeof(cwc);
        cwc.lpfnWndProc = ControlWndProc;
        cwc.hInstance = instance;
        cwc.lpszClassName = kControlWindowClassName;
        RegisterClassExW(&cwc);
    }
    controlHwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kControlWindowClassName, L"SnowDesktopControl", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, instance, this);
    taskbarRestartMsg_ = RegisterWindowMessageW(L"TaskbarCreated");

    // Create DComp target and initial surface
    if (FAILED(dcompDevice_->CreateTargetForHwnd(hwnd_, FALSE, &dcompTarget_)))
        { L(L"CreateTargetForHwnd FAILED"); return __LINE__; }
    if (FAILED(dcompDevice_->CreateVisual(&dcompVisual_)))
        { L(L"CreateVisual FAILED"); return __LINE__; }
    dcompTarget_->SetRoot(dcompVisual_.Get());
    if (FAILED(CreateOrResizeCompositionSurface()))
        { L(L"CreateCompositionSurface FAILED"); return __LINE__; }
    L(L"Composition target ready");

    LoadDesktopItems();
    L(L"LoadDesktopItems ok");

    // Auto-assign grid cells for items without layout
    {
        int idx = 0;
        for (auto& item : items_)
        {
            if (item.gridCell.pageId.empty() && !gridPages_.empty())
            {
                item.gridCell.pageId = gridPages_.front().id;
                item.gridCell.column = idx / std::max(1, gridPages_.front().rows);
                item.gridCell.row    = idx % std::max(1, gridPages_.front().rows);
                item.gridSpan = {1, 1};
            }
            ++idx;
        }
    }

    LayoutItems();
    L(L"Layout done");
    RebuildContainersAndItems();
    L(L"RebuildContainersAndItems ok");

    // App icon
    if (HICON appIcon = LoadAppIcon())
    {
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(appIcon));
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIcon));
    }

    AddTrayIcon();
    RegisterShellChangeNotifications();

    dropTargetRegistered_ = SUCCEEDED(RegisterDragDrop(hwnd_, static_cast<IDropTarget*>(this)));

    // Timers
    SetTimer(hwnd_, kRecycleBinPollTimerId, kRecycleBinPollIntervalMs, nullptr);
    SetTimer(controlHwnd_, kDesktopHostWatchTimerId, kDesktopHostWatchIntervalMs, nullptr);
    SetTimer(hwnd_, kWidgetRefreshTimerId, kWidgetRefreshIntervalMs, nullptr);

    settingsWindow_ = std::make_unique<SettingsWindow>();
    if (settingsWindow_->Init(instance, d3dDevice_.Get()))
    {
        settingsWindow_->SetReloadCallback([this]() { ReloadItems(); });
        settingsWindow_->SetExitCallback([this]() {
            if (hwnd_ && IsWindow(hwnd_))
                DestroyWindow(hwnd_);
        });
        settingsWindow_->SetInvalidateCallback([this]() {
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        });
    }
    else
    {
        settingsWindow_.reset();
    }

    widgetEngine_ = std::make_unique<WidgetEngine>();
    if (widgetEngine_->Init(d2dContext_.Get(), dwriteFactory_.Get()))
    {
        if (settingsWindow_)
            settingsWindow_->SetWidgetEngine(widgetEngine_.get());
    }
    else
    {
        widgetEngine_.reset();
    }

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
    L(L"Window shown, entering loop");

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (settingsWindow_ && settingsWindow_->IsVisible())
            settingsWindow_->Render();
    }
    OleUninitialize();
    return static_cast<int>(msg.wParam);
}

inline LRESULT CALLBACK DesktopApp::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
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

    // Log first few messages
    if (msg == WM_NCCREATE || msg == WM_CREATE || msg == WM_SIZE || msg == WM_PAINT || msg == WM_SHOWWINDOW)
    {
        auto L = [](const wchar_t* s) {
            HANDLE f = CreateFileW(L"SnowDesktop_crash.log", FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, s, static_cast<DWORD>(wcslen(s)*2), &w, nullptr);
                WriteFile(f, L"\r\n", 4, &w, nullptr); CloseHandle(f); }
        };
        wchar_t buf[128];
        wsprintfW(buf, L"WndProc msg=0x%04X app=%p", msg, app);
        L(buf);
    }

    if (app) return app->HandleMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

inline LRESULT DesktopApp::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (newMenuContextMenu_ && (msg == WM_INITMENUPOPUP || msg == WM_DRAWITEM || msg == WM_MEASUREITEM))
    {
        if (SUCCEEDED(newMenuContextMenu_->HandleMenuMsg(msg, wp, lp)))
            return 0;
    }
    if (activeContextMenu3_)
    {
        LRESULT result = 0;
        if (SUCCEEDED(activeContextMenu3_->HandleMenuMsg2(msg, wp, lp, &result)))
            return result;
    }
    else if (activeContextMenu2_)
    {
        if (SUCCEEDED(activeContextMenu2_->HandleMenuMsg(msg, wp, lp)))
            return 0;
    }

    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        EndPaint(hwnd_, &ps);
        OnPaint();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
    {
        virtualWidth_ = LOWORD(lp);
        virtualHeight_ = HIWORD(lp);
        dcompSurface_.Reset();
        UpdateLayoutWorkArea();
        LayoutItems();
        InvalidateRect(hwnd_, nullptr, TRUE);
        return 0;
    }
    case WM_LBUTTONDOWN:
        OnLeftButtonDown(wp, lp);
        return 0;
    case WM_MOUSEMOVE:
        OnMouseMove(wp, lp);
        return 0;
    case WM_LBUTTONUP:
        OnLeftButtonUp(wp, lp);
        return 0;
    case WM_MOUSEWHEEL:
        OnMouseWheel(wp, lp);
        return 0;
    case WM_RBUTTONUP:
        OnRightButtonUp(lp);
        return 0;
    case WM_LBUTTONDBLCLK:
    {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (popupWidgetIndex_ < widgets_.size())
        {
            RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
            if (PtInRect(&popup, pt))
            {
                std::vector<std::wstring> popupKeys = GetPopupItemKeys(widgets_[popupWidgetIndex_]);
                RECT content = GetCollectionPopupContentRect(popup);
                for (size_t i = 0; i < popupKeys.size(); ++i)
                {
                    RECT itemRect = GetCollectionPopupItemRect(popup, i);
                    RECT clipped = itemRect;
                    clipped.top = std::max(clipped.top, content.top);
                    clipped.bottom = std::min(clipped.bottom, content.bottom);
                    if (clipped.bottom <= clipped.top || !PtInRect(&clipped, pt)) continue;
                    size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
                    if (itemIndex != static_cast<size_t>(-1))
                    {
                        ShellExecuteW(nullptr, L"open", items_[itemIndex].parsingName.c_str(),
                            nullptr, nullptr, SW_SHOWNORMAL);
                        return 0;
                    }
                }
            }
        }

        size_t standaloneWidget = HitTestStandaloneWidgetIndex(pt);
        if (standaloneWidget != static_cast<size_t>(-1) &&
            widgets_[standaloneWidget].type == DesktopWidgetType::LuaScript)
        {
            ShowWidgetEditorHost(standaloneWidget);
            return 0;
        }

        for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(it->get());
            if (!wc) continue;
            for (auto& slot : wc->GetSlots())
            {
                if (!slot) continue;
                RECT slotBounds = slot->GetBounds();
                if (!PtInRect(&slotBounds, pt)) continue;
                if (auto* icon = dynamic_cast<DesktopIcon*>(slot->GetItem()))
                {
                    DesktopItem* item = icon->GetDesktopItem();
                    if (item)
                    {
                        ShellExecuteW(nullptr, L"open", item->parsingName.c_str(),
                            nullptr, nullptr, SW_SHOWNORMAL);
                        return 0;
                    }
                }
                if (auto* folderIcon = dynamic_cast<FolderEntryIcon*>(slot->GetItem()))
                {
                    FolderEntry* entry = folderIcon->GetFolderEntry();
                    if (entry)
                    {
                        ShellExecuteW(nullptr, L"open", entry->fullPath.c_str(),
                            nullptr, nullptr, SW_SHOWNORMAL);
                        return 0;
                    }
                }
            }
        }

        int hit = HitTestItem(pt);
        if (hit >= 0)
        {
            ShellExecuteW(nullptr, L"open", items_[hit].parsingName.c_str(),
                nullptr, nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTARROWS;
    case WM_KEYDOWN:
        OnKeyDown(wp);
        return 0;
    case WM_KEYUP:
        RefreshDragHintFromKeyboard();
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        virtualLeft_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
        virtualTop_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
        virtualWidth_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        virtualHeight_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (dcompVisual_)
            CreateOrResizeCompositionSurface();
        UpdateLayoutWorkArea();
        LayoutItems();
        InvalidateRect(hwnd_, nullptr, TRUE);
        return 0;
    case kShellChangeMessage:
        SetTimer(hwnd_, kShellChangeTimerId, kShellChangeDebounceMs, nullptr);
        return 0;
    case WM_TIMER:
        OnTimer(wp);
        return 0;
    case kTrayCallbackMessage:
        OnTrayCallback(lp);
        return 0;
    case WM_CLOSE:
        if (desktopWindows_.listView && IsWindow(desktopWindows_.listView))
        {
            ShowWindow(desktopWindows_.listView, SW_SHOW);
            RemovePropW(desktopWindows_.listView, kHiddenBySnowDesktopProp);
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        SaveLayoutSlots();
        RemoveTrayIcon();
        DestroyDragHintWindow();
        if (dropTargetRegistered_) { RevokeDragDrop(hwnd_); dropTargetRegistered_ = false; }
        KillTimer(hwnd_, kShellChangeTimerId);
        KillTimer(hwnd_, kRecycleBinPollTimerId);
        KillTimer(hwnd_, kWidgetRefreshTimerId);
        KillTimer(hwnd_, kCollectionPopupDwellTimerId);
        KillTimer(controlHwnd_, kDesktopHostWatchTimerId);
        if (desktopWindows_.listView && IsWindow(desktopWindows_.listView))
        {
            ShowWindow(desktopWindows_.listView, SW_SHOW);
            RemovePropW(desktopWindows_.listView, kHiddenBySnowDesktopProp);
        }
        if (shellChangeRegId_ != 0)
        {
            SHChangeNotifyDeregister(shellChangeRegId_);
            shellChangeRegId_ = 0;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

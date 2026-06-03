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

inline void DesktopApp::HideExplorerIcons()
{
    if (!desktopWindows_.listView || !IsWindow(desktopWindows_.listView))
        return;

    const bool hiddenBySnowDesktop = GetPropW(desktopWindows_.listView,
        kHiddenBySnowDesktopProp) != nullptr;
    const bool shouldHide = IsWindowVisible(desktopWindows_.listView) != FALSE ||
        hiddenBySnowDesktop;
    desktopWindows_.listViewWasVisible = shouldHide;
    if (!shouldHide)
        return;

    SetPropW(desktopWindows_.listView, kHiddenBySnowDesktopProp,
        reinterpret_cast<HANDLE>(1));
    ShowWindow(desktopWindows_.listView, SW_HIDE);
}

inline void DesktopApp::RestoreExplorerIcons()
{
    if (!desktopWindows_.listView || !IsWindow(desktopWindows_.listView))
        return;

    const bool hiddenBySnowDesktop = GetPropW(desktopWindows_.listView,
        kHiddenBySnowDesktopProp) != nullptr;
    if (desktopWindows_.listViewWasVisible || hiddenBySnowDesktop)
        ShowWindow(desktopWindows_.listView, SW_SHOW);
    RemovePropW(desktopWindows_.listView, kHiddenBySnowDesktopProp);
    desktopWindows_.listViewWasVisible = false;
}

inline void DesktopApp::RegisterOleDropTarget()
{
    if (!hwnd_ || !IsWindow(hwnd_) || dropTargetRegistered_)
        return;
    dropTargetRegistered_ = SUCCEEDED(RegisterDragDrop(hwnd_, static_cast<IDropTarget*>(this)));
}

inline void DesktopApp::ResetDesktopWindowResources()
{
    if (hwnd_ && IsWindow(hwnd_))
    {
        KillTimer(hwnd_, kShellChangeTimerId);
        KillTimer(hwnd_, kRecycleBinPollTimerId);
        KillTimer(hwnd_, kWidgetRefreshTimerId);
        KillTimer(hwnd_, kCollectionPopupDwellTimerId);
        if (dropTargetRegistered_)
            RevokeDragDrop(hwnd_);
    }
    dropTargetRegistered_ = false;

    if (shellChangeRegId_ != 0)
    {
        SHChangeNotifyDeregister(shellChangeRegId_);
        shellChangeRegId_ = 0;
    }

    DestroyDragHintWindow();
    dragRenderCache_.Reset();
    dcompSurface_.Reset();
    dcompVisual_.Reset();
    dcompTarget_.Reset();
    compositionWidth_ = 0;
    compositionHeight_ = 0;
    hwnd_ = nullptr;
}

inline void DesktopApp::AttachWindowToDesktopHost(HWND host)
{
    if (!hwnd_ || !IsWindow(hwnd_) || !host || !IsWindow(host))
        return;

    if (GetParent(hwnd_) != host)
        SetParent(hwnd_, host);

    LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
    style &= ~WS_POPUP;
    style |= WS_CHILD | WS_VISIBLE;
    SetWindowLongPtrW(hwnd_, GWL_STYLE, style);

    POINT origin{ virtualLeft_, virtualTop_ };
    ScreenToClient(host, &origin);
    SetWindowPos(hwnd_, HWND_TOP, origin.x, origin.y, virtualWidth_, virtualHeight_,
        SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
}

inline bool DesktopApp::CreateDesktopOverlayWindow()
{
    auto fail = [this]() {
        if (hwnd_ && IsWindow(hwnd_))
            DestroyWindow(hwnd_);
        ResetDesktopWindowResources();
        return false;
    };

    HWND parent = desktopWindows_.host && IsWindow(desktopWindows_.host)
        ? desktopWindows_.host
        : GetDesktopWindow();
    POINT origin{ virtualLeft_, virtualTop_ };
    ScreenToClient(parent, &origin);

    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"SnowDesktopWindow", L"SnowDesktop",
        WS_CHILD | WS_VISIBLE, origin.x, origin.y, virtualWidth_, virtualHeight_,
        parent, nullptr, instance_, this);
    if (!hwnd_)
    {
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"SnowDesktopWindow", L"SnowDesktop",
            WS_POPUP | WS_VISIBLE, virtualLeft_, virtualTop_, virtualWidth_, virtualHeight_,
            nullptr, nullptr, instance_, this);
        if (hwnd_ && parent && parent != GetDesktopWindow())
            AttachWindowToDesktopHost(parent);
    }
    if (!hwnd_)
        return false;

    SetWindowPos(hwnd_, HWND_TOP, origin.x, origin.y, virtualWidth_, virtualHeight_,
        SWP_NOACTIVATE);

    if (FAILED(dcompDevice_->CreateTargetForHwnd(hwnd_, FALSE, &dcompTarget_)))
        return fail();
    if (FAILED(dcompDevice_->CreateVisual(&dcompVisual_)))
        return fail();
    dcompTarget_->SetRoot(dcompVisual_.Get());
    if (FAILED(CreateOrResizeCompositionSurface()))
        return fail();

    if (HICON appIcon = LoadAppIcon())
    {
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(appIcon));
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIcon));
    }

    RegisterOleDropTarget();
    RegisterShellChangeNotifications();
    SetTimer(hwnd_, kRecycleBinPollTimerId, kRecycleBinPollIntervalMs, nullptr);
    SetTimer(hwnd_, kWidgetRefreshTimerId, kWidgetRefreshIntervalMs, nullptr);

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    InvalidateRect(hwnd_, nullptr, TRUE);
    UpdateWindow(hwnd_);
    return true;
}

inline void DesktopApp::RecoverDesktopHostAfterExplorerRestart()
{
    if (exitRequested_)
        return;

    DesktopWindows current = FindDesktopWindows();
    if (!current.host || !IsWindow(current.host))
        return;

    desktopWindows_ = current;
    if (hwnd_ && IsWindow(hwnd_))
    {
        AttachWindowToDesktopHost(desktopWindows_.host);
        RegisterOleDropTarget();
        RegisterShellChangeNotifications();
    }
    else
    {
        ResetDesktopWindowResources();
        if (!CreateDesktopOverlayWindow())
            return;
    }

    HideExplorerIcons();
    AddTrayIcon(true);
    if (controlHwnd_ && IsWindow(controlHwnd_))
        SetTimer(controlHwnd_, kDesktopHostWatchTimerId, kDesktopHostWatchIntervalMs, nullptr);

    if (!mouseDown_ && !reloading_)
        ReloadItems(false);
    else if (hwnd_)
        InvalidateRect(hwnd_, nullptr, TRUE);
}

inline void DesktopApp::WatchDesktopHost()
{
    if (exitRequested_)
        return;

    if (!hwnd_ || !IsWindow(hwnd_))
    {
        RecoverDesktopHostAfterExplorerRestart();
        return;
    }

    DesktopWindows current = FindDesktopWindows();
    HWND currentHost = current.host;
    HWND parent = GetParent(hwnd_);
    const bool parentMissing = parent == nullptr || !IsWindow(parent);
    const bool knownHostMissing = desktopWindows_.host == nullptr || !IsWindow(desktopWindows_.host);
    const bool knownListViewMissing = desktopWindows_.listView != nullptr &&
        !IsWindow(desktopWindows_.listView);
    const bool hostChanged = currentHost && IsWindow(currentHost) &&
        currentHost != desktopWindows_.host;
    const bool parentDetached = currentHost && IsWindow(currentHost) &&
        parent != currentHost;

    if (parentMissing || knownHostMissing || knownListViewMissing ||
        hostChanged || parentDetached)
    {
        RecoverDesktopHostAfterExplorerRestart();
        return;
    }

    if (current.listView && IsWindow(current.listView) && IsWindowVisible(current.listView))
    {
        desktopWindows_ = current;
        HideExplorerIcons();
    }
}

inline void DesktopApp::RequestExit()
{
    if (exitRequested_)
        return;
    exitRequested_ = true;
    RestoreExplorerIcons();
    if (hwnd_ && IsWindow(hwnd_))
    {
        DestroyWindow(hwnd_);
        return;
    }

    SaveLayoutSlots();
    RemoveTrayIcon();
    ResetDesktopWindowResources();
    if (controlHwnd_ && IsWindow(controlHwnd_))
        DestroyWindow(controlHwnd_);
    else
        PostQuitMessage(0);
}

inline int DesktopApp::Run(HINSTANCE instance, int showCommand)
{
    (void)showCommand;

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
    HideExplorerIcons();
    if (desktopWindows_.listView && desktopWindows_.listViewWasVisible)
        L(L"Explorer icon layer hidden");
    else
        L(L"Explorer icon layer not found or already hidden");

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
    RegisterOleDropTarget();

    // Timers
    SetTimer(hwnd_, kRecycleBinPollTimerId, kRecycleBinPollIntervalMs, nullptr);
    SetTimer(controlHwnd_, kDesktopHostWatchTimerId, kDesktopHostWatchIntervalMs, nullptr);
    SetTimer(hwnd_, kWidgetRefreshTimerId, kWidgetRefreshIntervalMs, nullptr);

    settingsWindow_ = std::make_unique<SettingsWindow>();
    if (settingsWindow_->Init(instance, d3dDevice_.Get()))
    {
        settingsWindow_->SetReloadCallback([this]() { ReloadItems(); });
        settingsWindow_->SetExitCallback([this]() { RequestExit(); });
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
        widgetEngine_->SetDesktopSnapshotProvider([this]() {
            return BuildLuaDesktopSnapshot(false);
        });
        widgetEngine_->SetSelectionProvider([this]() {
            return BuildLuaDesktopSnapshot(true);
        });
        widgetEngine_->SetWidgetTitleCallback([this](const std::wstring& widgetId, const std::wstring& title) {
            LuaSetWidgetTitle(widgetId, title);
        });
        widgetEngine_->SetInvalidateCallback([this]() {
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        });
        widgetEngine_->SetDesktopOpenCallback([this](const std::wstring& path) {
            return LuaOpenPath(path);
        });
        widgetEngine_->SetDesktopRevealCallback([this](const std::wstring& path) {
            return LuaRevealPath(path);
        });
        widgetEngine_->SetDesktopRefreshCallback([this]() {
            ReloadItems();
        });
        widgetEngine_->SetInlineTextEditCallback([this](const LuaInlineTextEditRequest& request) {
            BeginLuaInlineTextEdit(request);
        });
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
            if (HitTestStandaloneWidget(standaloneWidget, pt) == WidgetHit::Content && widgetEngine_)
            {
                RECT frame = GetStandaloneWidgetFrameRect(widgets_[standaloneWidget]);
                widgetEngine_->EnsureWidgetLoaded(widgets_[standaloneWidget].id,
                    widgets_[standaloneWidget].scriptPath);
                widgetEngine_->InvokeMouseEvent(widgets_[standaloneWidget].id, "onDoubleClick",
                    pt.x - frame.left, pt.y - frame.top, 1, 0);
            }
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
        RequestExit();
        return 0;
    case WM_DESTROY:
        if (luaInlineEdit_)
            CommitLuaInlineTextEdit(false);
        if (!exitRequested_)
        {
            ResetDesktopWindowResources();
            if (controlHwnd_ && IsWindow(controlHwnd_))
                SetTimer(controlHwnd_, kDesktopHostWatchTimerId, kDesktopHostWatchIntervalMs, nullptr);
            return 0;
        }
        SaveLayoutSlots();
        RemoveTrayIcon();
        ResetDesktopWindowResources();
        if (controlHwnd_ && IsWindow(controlHwnd_))
            KillTimer(controlHwnd_, kDesktopHostWatchTimerId);
        RestoreExplorerIcons();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

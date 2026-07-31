#include "app.h"

#include <commoncontrols.h>
#include <imm.h>
#include <new>

// Application bootstrap and top-level message loop.

int DesktopApp::Run(HINSTANCE instance, int showCommand)
{
    (void)showCommand;

    MigrateLegacyDataPaths();
    WriteDiagnosticLogEntry(L"Run start");

    {
        std::wstring langDir = GetExecutableDirectoryPath();
        langDir += L"\\lang";
        Locale::Instance().Init(langDir.c_str());
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    HRESULT hr = OleInitialize(nullptr);
    WriteDiagnosticLogEntry(SUCCEEDED(hr) ? L"OleInit ok" : L"OleInit FAILED");

    instance_ = instance;

    // Resolve the persisted desktop mode before touching Explorer's icon layer.
    LoadGeneralSettingsAndApply();

    // Find and optionally hide Explorer icon layer.
    desktopWindows_ = FindDesktopWindows();
    if (desktopWindows_.host && IsWindow(desktopWindows_.host))
        GetWindowThreadProcessId(desktopWindows_.host,
            &desktopHostExplorerProcessId_);
    {
        wchar_t buf[256];
        wsprintfW(buf, L"Desktop: progman=%p defView=%p listView=%p host=%p",
            desktopWindows_.progman, desktopWindows_.defView,
            desktopWindows_.listView, desktopWindows_.host);
        WriteDiagnosticLogEntry(buf);
    }
    if (customDesktopVisible_)
    {
        HideExplorerIcons();
        if (desktopWindows_.listView && desktopWindows_.listViewWasVisible)
            WriteDiagnosticLogEntry(L"Explorer icon layer hidden");
        else
            WriteDiagnosticLogEntry(L"Explorer icon layer not found or already hidden");
    }
    else
    {
        WriteDiagnosticLogEntry(L"Native desktop selected by persisted setting");
    }

    // Create desktop overlay window as child of desktop host
    virtualLeft_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
    virtualTop_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
    virtualWidth_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    virtualHeight_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    BeginIconLoadGeneration();
    LoadDockSettingsAndApply();
    LoadDockUsageStats();
    LoadLayoutSlots();
    UpdateLayoutWorkArea();
    displayTopologySignature_ = CaptureDisplayTopologySignature();

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
        WNDCLASSEXW input{};
        input.cbSize = sizeof(input);
        input.lpfnWndProc = InputWndProc;
        input.hInstance = instance;
        input.hbrBackground = nullptr;
        input.lpszClassName = kInputWindowClassName;
        RegisterClassExW(&input);
    }
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
    {
        WNDCLASSEXW nav{};
        nav.cbSize = sizeof(nav);
        nav.style = CS_DBLCLKS;
        nav.lpfnWndProc = QuickNavigationWndProc;
        nav.hInstance = instance;
        nav.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        nav.hbrBackground = nullptr;
        nav.lpszClassName = kQuickNavigationWindowClassName;
        RegisterClassExW(&nav);
    }
    {
        WNDCLASSEXW dock{};
        dock.cbSize = sizeof(dock);
        dock.style = CS_DBLCLKS;
        dock.lpfnWndProc = FloatingDockWndProc;
        dock.hInstance = instance;
        dock.hCursor =
            LoadCursorW(nullptr, IDC_ARROW);
        dock.hbrBackground = nullptr;
        dock.lpszClassName =
            kFloatingDockWindowClassName;
        RegisterClassExW(&dock);
    }

    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED,
        wc.lpszClassName, L"SnowDesktop",
        WS_POPUP, virtualLeft_, virtualTop_, virtualWidth_, virtualHeight_,
        nullptr, nullptr, instance, this);
    if (!hwnd_) { WriteDiagnosticLogEntry(L"CreateWindow FAILED"); return __LINE__; }
    AttachWindowToDesktopHost(parent);
    dockWindowPreview_ = std::make_unique<DockWindowPreview>();
    if (!dockWindowPreview_->Initialize(
            instance_,
            [this](HWND window) {
                ActivateDockWindowFromPreview(window);
            },
            [this](HWND window) {
                CloseDockWindowFromPreview(window);
            }))
        dockWindowPreview_.reset();
    dockWindowTransition_ =
        std::make_unique<DockWindowTransition>();
    if (!dockWindowTransition_->Initialize(instance_))
        dockWindowTransition_.reset();
    if (!CreateDesktopInputWindow(parent))
    {
        WriteDiagnosticLogEntry(L"CreateInputWindow FAILED");
        return __LINE__;
    }
    WriteDiagnosticLogEntry(L"Window created");
    {
        wchar_t buf[256];
        wsprintfW(buf, L"Parent=%p origin=(%d,%d) size=%dx%d exStyle=0x%08X",
            parent, origin.x, origin.y, virtualWidth_, virtualHeight_,
            static_cast<unsigned>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE)));
        WriteDiagnosticLogEntry(buf);
    }

    if (!InitGraphics()) { WriteDiagnosticLogEntry(L"InitGraphics FAILED"); return __LINE__; }
    WriteDiagnosticLogEntry(L"InitGraphics ok");

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
    systemTaskbarTaskViewStateMsg_ = RegisterWindowMessageW(
        L"SnowDesktop.Taskbar.Dynamic.TaskView.v1");

    // Create DComp target and initial surface
    if (FAILED(dcompDevice_->CreateTargetForHwnd(hwnd_, FALSE, &dcompTarget_)))
        { WriteDiagnosticLogEntry(L"CreateTargetForHwnd FAILED"); return __LINE__; }
    if (FAILED(dcompDevice_->CreateVisual(&dcompVisual_)))
        { WriteDiagnosticLogEntry(L"CreateVisual FAILED"); return __LINE__; }
    dcompTarget_->SetRoot(dcompVisual_.Get());
    if (FAILED(CreateOrResizeCompositionSurface()))
        { WriteDiagnosticLogEntry(L"CreateCompositionSurface FAILED"); return __LINE__; }
    WriteDiagnosticLogEntry(L"Composition target ready");
    if (customDesktopVisible_)
    {
        if (desktopBackdropCompositor_.Initialize(hwnd_))
        {
            nativeGlassPanelReadyLogged_ = false;
            WriteDiagnosticLogEntry(
                L"Native desktop CompositionBackdropBrush initialized");
        }
        else
        {
            std::wstring message =
                L"Native desktop CompositionBackdropBrush unavailable: ";
            message += desktopBackdropCompositor_.LastError();
            WriteDiagnosticLogEntry(message.c_str());
        }
    }

    LoadCategorySettingsAndApply();

    // Use the same placement pipeline as runtime refreshes so a desktop that
    // already contains more items than the visible grids can create virtual
    // overflow pages during the initial load.
    ReloadItems(false);
    if (legacyWidgetLayoutMigrationPending_)
    {
        SaveLayoutSlots();
        legacyWidgetLayoutMigrationPending_ = false;
    }
    StartIconLoader();
    WriteDiagnosticLogEntry(L"LoadDesktopItems ok");
    WriteDiagnosticLogEntry(L"Layout done");
    WriteDiagnosticLogEntry(L"RebuildContainersAndItems ok");

    // App icon
    if (HICON appIcon = LoadAppIcon())
    {
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(appIcon));
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIcon));
    }

    RegisterShellChangeNotifications();
    RegisterOleDropTarget();
    LoadNavigationSettingsAndApply();
    ApplyFloatingDockHotkey();
    ApplyDesktopPassthroughHotkey();

    // Timers
    SetTimer(hwnd_, kRecycleBinPollTimerId, kRecycleBinPollIntervalMs, nullptr);
    SetTimer(controlHwnd_, kDesktopHostWatchTimerId, kDesktopHostWatchIntervalMs, nullptr);
    SetTimer(hwnd_, kWidgetRefreshTimerId, kWidgetRefreshIntervalMs, nullptr);
    SetTimer(hwnd_, kTaskbarRevealGuardTimerId,
        kTaskbarRevealGuardIntervalMs, nullptr);
    StartDockForegroundMonitor();

    settingsWindow_ = std::make_unique<SettingsWindow>();
    if (settingsWindow_->Init(instance, d3dDevice_.Get()))
    {
        settingsWindow_->SetReloadCallback([this]() {
            bool migratedLayout = false;
            for (auto& widget : widgets_)
            {
                if (widget.type != DesktopWidgetType::LuaScript ||
                    !widget.packageId.empty() ||
                    widget.legacyScriptPath.empty())
                    continue;
                if (const auto packageId =
                    WidgetEngine::ResolveLegacyWidgetPackage(
                        widget.legacyScriptPath))
                {
                    widget.packageId = *packageId;
                    widget.legacyScriptPath.clear();
                    migratedLayout = true;
                }
            }
            if (migratedLayout)
                SaveLayoutSlots();
            ReloadItems();
            if (settingsWindow_)
                settingsWindow_->SyncDockEnabled(generalSettings_.dockEnabled);
        });
        settingsWindow_->SetExitCallback([this]() { RequestExit(); });
        settingsWindow_->SetRestartCallback([this]() { RequestRestart(); });
        settingsWindow_->SetInvalidateCallback([this]() {
            ApplyQuickNavigationAppearance();
            if (quickNavigationOpen_)
                InvalidateQuickNavigationWindow();
            if (dockSettings_.systemTaskbarFollowPersonalization ||
                dockSettings_.systemTaskbarVisibleWindow.themeMode ==
                    SystemTaskbarThemeMode::FollowGlobal ||
                dockSettings_.systemTaskbarMaximizedWindow.themeMode ==
                    SystemTaskbarThemeMode::FollowGlobal ||
                dockSettings_.systemTaskbarShellUi.themeMode ==
                    SystemTaskbarThemeMode::FollowGlobal)
                RefreshSystemTaskbarAppearance(false);
            if (hwnd_)
            {
                InvalidateAllWidgetSlots();
                InvalidateRect(hwnd_, nullptr, FALSE);
                UpdateWindow(hwnd_);
            }
        });
        settingsWindow_->SetGlassStatusProvider([this]() {
            return GetGlassBackendStatusText();
        });
        settingsWindow_->SetHotkeyAvailabilityCallback(
            [this](HotkeySettingTarget target,
                UINT modifiers, UINT virtualKey) {
                if (virtualKey == 0)
                    return false;
                const UINT normalizedModifiers =
                    modifiers &
                    (MOD_CONTROL | MOD_ALT |
                        MOD_SHIFT | MOD_WIN);
                const auto matches = [&](
                    UINT configuredModifiers,
                    UINT configuredVirtualKey) {
                    return normalizedModifiers ==
                            (configuredModifiers &
                                (MOD_CONTROL | MOD_ALT |
                                    MOD_SHIFT | MOD_WIN)) &&
                        virtualKey == configuredVirtualKey;
                };

                switch (target)
                {
                case HotkeySettingTarget::QuickNavigation:
                    if (navigationSettings_.enabled &&
                        matches(navigationSettings_.modifiers,
                            navigationSettings_.virtualKey))
                        return navigationHotkeyRegistered_;
                    break;
                case HotkeySettingTarget::DesktopPassthrough:
                    if (generalSettings_.
                            desktopPassthroughHotkeyEnabled &&
                        customDesktopVisible_ &&
                        matches(generalSettings_.
                                desktopPassthroughHotkeyModifiers,
                            generalSettings_.
                                desktopPassthroughHotkeyVirtualKey))
                        return desktopPassthroughHotkeyRegistered_;
                    break;
                case HotkeySettingTarget::FloatingDock:
                    if (generalSettings_.dockEnabled &&
                        dockSettings_.floatingShortcutMode &&
                        matches(
                            dockSettings_.floatingHotkeyModifiers,
                            dockSettings_.floatingHotkeyVirtualKey))
                        return floatingDockHotkeyRegistered_;
                    break;
                case HotkeySettingTarget::None:
                    return false;
                }

                HWND probeWindow =
                    controlHwnd_ && IsWindow(controlHwnd_)
                        ? controlHwnd_
                        : (inputHwnd_ && IsWindow(inputHwnd_)
                            ? inputHwnd_ : hwnd_);
                if (!probeWindow || !IsWindow(probeWindow))
                    return false;
                const BOOL registered = RegisterHotKey(
                    probeWindow, kSettingsHotkeyProbeId,
                    normalizedModifiers | MOD_NOREPEAT,
                    virtualKey);
                if (registered)
                {
                    UnregisterHotKey(
                        probeWindow, kSettingsHotkeyProbeId);
                    return true;
                }
                return false;
            });
        settingsWindow_->SetNavigationSettingsChangedCallback([this]() {
            LoadNavigationSettingsAndApply();
        });
        settingsWindow_->SetGeneralSettingsChangedCallback([this]() {
            LoadGeneralSettingsAndApply();
            if (quickNavigationOpen_)
                InvalidateQuickNavigationWindow();
        });
        settingsWindow_->SetLanguageChangedCallback([this]() {
            ApplyLanguageChange();
        });
        settingsWindow_->SetDockEnabledChangedCallback([this](bool enabled) {
            if (generalSettings_.dockEnabled == enabled) return;
            generalSettings_.dockEnabled = enabled;
            ApplyFloatingDockHotkey();
            UpdateLayoutWorkArea();
            if (!enabled)
                RestoreDockEntriesToDesktop();
            LayoutItems();
            SaveLayoutSlots();
            InvalidateDragStaticScene();
            if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
        });
        settingsWindow_->SetDockSettingsPreviewChangedCallback(
            [this](const DockSettings& settings) {
                DockSettings normalizedSettings = settings;
                NormalizeDockSettings(normalizedSettings);
                std::vector<RECT> previousDockRects;
                for (const auto& container : containers_)
                {
                    if (const auto* dock =
                            dynamic_cast<DockContainer*>(
                                container.get()))
                    {
                        previousDockRects.push_back(
                            dock->GetInteractiveBounds());
                    }
                }
                const bool layoutChanged =
                    normalizedSettings.position != dockSettings_.position ||
                    normalizedSettings.edgeAttached != dockSettings_.edgeAttached ||
                    normalizedSettings.floatingShortcutMode !=
                        dockSettings_.floatingShortcutMode ||
                    normalizedSettings.monitorScope != dockSettings_.monitorScope ||
                    normalizedSettings.showWindowsButton !=
                        dockSettings_.showWindowsButton ||
                    normalizedSettings.showFrequentItems !=
                        dockSettings_.showFrequentItems ||
                    normalizedSettings.frequentItemCount !=
                        dockSettings_.frequentItemCount ||
                    std::abs(
                        normalizedSettings.thicknessScale -
                        dockSettings_.thicknessScale) > 0.0001f;

                dockSettings_ = normalizedSettings;
                ApplyFloatingDockHotkey();
                dockSettingsLayoutCommitPending_ =
                    dockSettingsLayoutCommitPending_ ||
                    layoutChanged;
                if (layoutChanged)
                {
                    InvalidateDockContainers();
                    if (hwnd_)
                    {
                        for (const RECT& rect :
                            previousDockRects)
                        {
                            InvalidateRect(
                                hwnd_, &rect, TRUE);
                        }
                        InvalidateDockRects(TRUE);
                    }
                }
            });
        settingsWindow_->SetDockSettingsChangedCallback([this]() {
            const DockPosition previousPosition = dockSettings_.position;
            const bool previousEdgeAttached = dockSettings_.edgeAttached;
            const bool previousFloatingShortcutMode =
                dockSettings_.floatingShortcutMode;
            const DockMonitorScope previousMonitorScope = dockSettings_.monitorScope;
            const bool previousShowWindowsButton = dockSettings_.showWindowsButton;
            const bool previousShowFrequentItems = dockSettings_.showFrequentItems;
            const int previousFrequentItemCount = dockSettings_.frequentItemCount;
            const float previousThicknessScale = dockSettings_.thicknessScale;
            const bool previousSystemTaskbarAutoHide =
                dockSettings_.systemTaskbarAutoHide;
            const int previousSystemTaskbarAlignment =
                dockSettings_.systemTaskbarAlignment;
            LoadDockSettingsAndApply();
            if (dockSettingsLayoutCommitPending_ ||
                dockSettings_.position != previousPosition ||
                dockSettings_.edgeAttached != previousEdgeAttached ||
                dockSettings_.floatingShortcutMode !=
                    previousFloatingShortcutMode ||
                dockSettings_.monitorScope != previousMonitorScope ||
                dockSettings_.showWindowsButton != previousShowWindowsButton ||
                dockSettings_.showFrequentItems != previousShowFrequentItems ||
                dockSettings_.frequentItemCount != previousFrequentItemCount ||
                std::abs(dockSettings_.thicknessScale - previousThicknessScale) > 0.0001f ||
                dockSettings_.systemTaskbarAutoHide != previousSystemTaskbarAutoHide ||
                dockSettings_.systemTaskbarAlignment != previousSystemTaskbarAlignment)
            {
                UpdateLayoutWorkArea();
                LayoutItems();
                SaveLayoutSlots();
                InvalidateDragStaticScene();
            }
            dockSettingsLayoutCommitPending_ = false;
            if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
        });
        settingsWindow_->SetPersonalizationChangedCallback([this]() {
            RefreshSystemTaskbarAppearance(false);
        });
        settingsWindow_->SetDisplaySettingsChangedCallback([this]() {
            SetIconSpacing(settingsWindow_->GetIconSpacingScale());
            SetItemFontSize(settingsWindow_->GetItemFontSizeD());
            SetItemFontWeight(static_cast<DWRITE_FONT_WEIGHT>(static_cast<int>(settingsWindow_->GetItemFontWeightD())));
            SetShortcutArrowMode(settingsWindow_->GetShortcutArrowMode());
            SetIconBeautifySettings(settingsWindow_->GetIconBeautifyEnabled(),
                settingsWindow_->GetIconBeautifyMode(),
                settingsWindow_->GetIconBeautifyBgOpacity(),
                settingsWindow_->GetIconBeautifyGradientEnabled(),
                settingsWindow_->GetIconBeautifyBgStartR(),
                settingsWindow_->GetIconBeautifyBgStartG(),
                settingsWindow_->GetIconBeautifyBgStartB(),
                settingsWindow_->GetIconBeautifyBgEndR(),
                settingsWindow_->GetIconBeautifyBgEndG(),
                settingsWindow_->GetIconBeautifyBgEndB(),
                settingsWindow_->GetIconBeautifyGradientDirection());
        });
        settingsWindow_->SetCategorySettingsChangedCallback([this]() {
            LoadCategorySettingsAndApply();
        });

        settingsWindow_->SyncDisplaySettings(iconSpacingScale_, itemFontSize_,
            static_cast<float>(itemFontWeight_), shortcutArrowMode_, iconBeautifyEnabled_,
            iconBeautifyMode_,
            iconBeautifyBgOpacity_, iconBeautifyGradientEnabled_,
            iconBeautifyBgStartR_, iconBeautifyBgStartG_, iconBeautifyBgStartB_,
            iconBeautifyBgEndR_, iconBeautifyBgEndG_, iconBeautifyBgEndB_,
            iconBeautifyGradientDirection_);
        settingsWindow_->SyncDockEnabled(generalSettings_.dockEnabled);
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
        widgetEngine_->SetWidgetSelectedProvider(
            [this](const std::wstring& widgetId) {
                for (const auto& widget : widgets_)
                    if (widget.id == widgetId &&
                        widget.type == DesktopWidgetType::LuaScript)
                        return widget.selected;
                return false;
            });
        widgetEngine_->SetSelectedWidgetPackageProvider(
            [this]() {
                std::wstring selectedPackageId;
                int selectedCount = 0;
                for (const auto& widget : widgets_)
                {
                    if (!widget.selected)
                        continue;
                    ++selectedCount;
                    if (widget.type ==
                        DesktopWidgetType::LuaScript)
                        selectedPackageId = widget.packageId;
                }
                return selectedCount == 1
                    ? selectedPackageId
                    : std::wstring{};
            });
        widgetEngine_->SetApplicationSearchProvider(
            [this](const std::string& query, int maxResults) {
                return BuildLuaApplicationSearch(
                    query, maxResults);
            });
        widgetEngine_->SetEverythingSearchProvider([this](const std::string& query, int maxResults) {
            return BuildLuaEverythingSearch(query, maxResults);
        });
        widgetEngine_->SetWidgetTitleCallback([this](const std::wstring& widgetId, const std::wstring& title) {
            LuaSetWidgetTitle(widgetId, title);
        });
        widgetEngine_->SetInvalidateCallback([this](const std::wstring& widgetId) {
            if (!hwnd_) return;
            if (widgetId.empty())
            {
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            if (luaWidgetPanelRequest_.widgetId ==
                    widgetId)
            {
                RECT dirty =
                    GetLuaWidgetPanelRect();
                InflateRect(&dirty, 3, 3);
                InvalidateRect(
                    hwnd_, &dirty, FALSE);
            }
            for (const auto& widget : widgets_)
            {
                if (widget.id != widgetId || widget.type != DesktopWidgetType::LuaScript)
                    continue;
                RECT dirty = GetStandaloneWidgetFrameRect(widget);
                InflateRect(&dirty, 3, 3);
                InvalidateRect(hwnd_, &dirty, FALSE);
                return;
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
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
        widgetEngine_->SetHostInputFocusCallback([this]() {
            for (auto& container : containers_)
            {
                auto* searchable =
                    dynamic_cast<ScrollingItemWidget*>(container.get());
                if (searchable)
                    searchable->SetSearchFocused(false);
            }
            FocusDesktopInputWindow();
            UpdateHostInputImePosition();
        });
        widgetEngine_->SetNotifyCallback([this](const std::wstring& title, const std::wstring& message) {
            ShowBalloonNotification(title, message);
        });
        widgetEngine_->SetWidgetTimerRequestCallback([this](const std::wstring& widgetId, UINT intervalMs) -> UINT_PTR {
            if (!hwnd_) return 0;
            UINT_PTR tid = nextWidgetTimerId_++;
            if (SetTimer(hwnd_, tid, intervalMs, nullptr) == 0)
                return 0;
            widgetTimerIds_[tid] = widgetId;
            return tid;
        });
        widgetEngine_->SetWidgetTimerKillCallback([this](UINT_PTR timerId) {
            if (!hwnd_ || !timerId) return;
            KillTimer(hwnd_, timerId);
            widgetTimerIds_.erase(timerId);
        });
        widgetEngine_->SetOpenWidgetSettingsCallback([this](const std::wstring& widgetId, const std::wstring&) {
            for (size_t i = 0; i < widgets_.size(); ++i)
            {
                if (widgets_[i].id == widgetId && widgets_[i].type == DesktopWidgetType::LuaScript)
                {
                    ShowWidgetEditorHost(i);
                    break;
                }
            }
        });
        widgetEngine_->SetOpenWidgetPanelCallback(
            [this](const LuaWidgetPanelRequest& request) {
                OpenLuaWidgetPanel(request);
            });
        widgetEngine_->SetCloseWidgetPanelCallback(
            [this](const std::wstring& widgetId) {
                CloseLuaWidgetPanel(widgetId, "widget");
            });
        if (settingsWindow_)
        {
            settingsWindow_->SetWidgetEngine(widgetEngine_.get());
            if (!WidgetEngine::ListLegacyWidgetPackages().empty())
                settingsWindow_->ShowWidgetMigration();
        }
    }
    else
    {
        widgetEngine_.reset();
    }

    // Expose the tray menu only after SettingsWindow and all of its callbacks
    // are ready. Otherwise a click during startup can reach the temporary
    // settingsWindow_ == nullptr state and appear to require a second click.
    startupInitializationComplete_ = true;
    AddTrayIcon();
    if (showSettingsPending_ && settingsWindow_)
    {
        showSettingsPending_ = false;
        settingsWindow_->Show();
    }
    SetSoftwareDesktopEnabled(customDesktopVisible_, false);
    if (customDesktopVisible_)
        UpdateWindow(hwnd_);
    WriteDiagnosticLogEntry(customDesktopVisible_
        ? L"Window shown, entering loop"
        : L"Native desktop active, entering loop");

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (settingsWindow_ && settingsWindow_->IsVisible() &&
            settingsWindow_->NeedsRender())
            settingsWindow_->Render();
    }
    OleUninitialize();
    return static_cast<int>(msg.wParam);
}

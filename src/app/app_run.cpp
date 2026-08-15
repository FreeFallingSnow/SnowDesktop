#include "app.h"

#include <commoncontrols.h>
#include <imm.h>
#include <new>
#include <shobjidl.h>

namespace
{
LuaWidgetFilePickerResult ShowLuaWidgetFilePicker(HWND owner,
    const LuaWidgetFilePickerRequest& request)
{
    LuaWidgetFilePickerResult result;
    ComPtr<IFileDialog> dialog;
    const CLSID& classId = request.kind == LuaWidgetFilePickerKind::SaveFile
        ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
    if (FAILED(CoCreateInstance(classId, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog))) || !dialog)
    {
        result.error = "pickerUnavailable";
        return result;
    }

    DWORD options = 0;
    if (FAILED(dialog->GetOptions(&options)))
    {
        result.error = "pickerFailed";
        return result;
    }
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (request.kind == LuaWidgetFilePickerKind::OpenFile)
        options |= FOS_FILEMUSTEXIST;
    else if (request.kind == LuaWidgetFilePickerKind::SaveFile)
        options |= FOS_OVERWRITEPROMPT;
    else
        options |= FOS_PICKFOLDERS;
    if (FAILED(dialog->SetOptions(options)))
    {
        result.error = "pickerFailed";
        return result;
    }

    std::vector<std::wstring> patterns;
    std::vector<COMDLG_FILTERSPEC> filters;
    if (request.kind != LuaWidgetFilePickerKind::Folder &&
        !request.extensions.empty())
    {
        patterns.reserve(request.extensions.size());
        for (const auto& extension : request.extensions)
            patterns.push_back(L"*." + extension);
        filters.reserve(patterns.size());
        for (const auto& pattern : patterns)
            filters.push_back({ pattern.c_str(), pattern.c_str() });
        if (FAILED(dialog->SetFileTypes(
                static_cast<UINT>(filters.size()), filters.data())))
        {
            result.error = "pickerFailed";
            return result;
        }
    }

    if (request.kind == LuaWidgetFilePickerKind::SaveFile)
    {
        ComPtr<IFileSaveDialog> saveDialog;
        if (FAILED(dialog.As(&saveDialog)) || !saveDialog)
        {
            result.error = "pickerFailed";
            return result;
        }
        if (!request.suggestedName.empty())
            (void)saveDialog->SetFileName(request.suggestedName.c_str());
        if (!request.extensions.empty())
            (void)saveDialog->SetDefaultExtension(
                request.extensions.front().c_str());
    }

    const HRESULT shown = dialog->Show(owner);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED))
    {
        result.canceled = true;
        return result;
    }
    if (FAILED(shown))
    {
        result.error = "pickerFailed";
        return result;
    }

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item)) || !item)
    {
        result.error = "invalidSelection";
        return result;
    }
    PWSTR selected = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &selected)) ||
        !selected)
    {
        if (selected) CoTaskMemFree(selected);
        result.error = "invalidSelection";
        return result;
    }
    result.path = std::filesystem::path(selected);
    CoTaskMemFree(selected);
    return result;
}
}

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

    if (!uiAnimationScheduler_.Initialize())
    {
        WriteDiagnosticLogEntry(
            L"UiAnimationScheduler initialization failed");
        OleUninitialize();
        return __LINE__;
    }

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
                ActivateDockWindowFromPreviewAnimated(window);
            },
            [this](HWND window) {
                CloseDockWindowFromPreview(window);
            }))
        dockWindowPreview_.reset();
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
    dockWindowTransition_ =
        std::make_unique<DockWindowTransition>();
    if (!dockWindowTransition_->Initialize(
            instance_, &uiAnimationScheduler_,
            d2dDevice_.Get(), dcompDevice_.Get()))
        dockWindowTransition_.reset();

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
    GetDemoIdentityIconDirectory();
    StartDemoIconLoader();

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
    StartRecycleBinWatcher();
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
    if (!settingsWindow_->Init(instance, d3dDevice_.Get()))
        WriteDiagnosticLogEntry(
            L"SettingsWindow startup initialization deferred");
    if (settingsWindow_)
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
        settingsWindow_->SetAnimationDiagnosticsToggleCallback(
            [this](bool enabled) {
                uiAnimationScheduler_.SetDiagnosticsEnabled(enabled);
            });
        settingsWindow_->SetAnimationDiagnosticsProvider([this]() {
            const auto metrics = uiAnimationScheduler_.Metrics();
            wchar_t text[768]{};
            swprintf_s(
                text,
                L"Target %.1f Hz | effective %.1f Hz\n"
                L"frames requested %llu | delivered %llu | skipped %llu\n"
                L"active animations %zu | timers %zu\n"
                L"interval p50/p95/p99 %.2f / %.2f / %.2f ms\n"
                L"UI p50/p95/p99 %.2f / %.2f / %.2f ms\n"
                L"Commit p50/p95/p99 %.2f / %.2f / %.2f ms",
                metrics.targetRefreshHz,
                metrics.effectiveRefreshHz,
                static_cast<unsigned long long>(
                    metrics.requestedFrames),
                static_cast<unsigned long long>(
                    metrics.deliveredFrames),
                static_cast<unsigned long long>(
                    metrics.skippedFrames),
                metrics.activeAnimations,
                metrics.activeTimers,
                metrics.frameIntervalP50Ms,
                metrics.frameIntervalP95Ms,
                metrics.frameIntervalP99Ms,
                metrics.uiWorkP50Ms,
                metrics.uiWorkP95Ms,
                metrics.uiWorkP99Ms,
                metrics.commitP50Ms,
                metrics.commitP95Ms,
                metrics.commitP99Ms);
            return std::wstring(text);
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
                    UpdateLayoutWorkArea();
                    LayoutItems();
                    InvalidateDragStaticScene();
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
            SetComponentSpacing(
                settingsWindow_->GetComponentSpacingScale());
            SetItemFontSize(settingsWindow_->GetItemFontSizeCu());
            SetListItemFontSize(
                settingsWindow_->GetListItemFontSizeCu());
            SetItemFontWeight(static_cast<DWRITE_FONT_WEIGHT>(static_cast<int>(settingsWindow_->GetItemFontWeightD())));
            SetShortcutArrowMode(settingsWindow_->GetShortcutArrowMode());
        });
        settingsWindow_->SetIconBeautifySettingsChangedCallback(
            [this](snowdesktop::IconBeautifyUpdateKind updateKind) {
                SetIconBeautifySettings(
                    settingsWindow_->GetIconBeautifySettings(), updateKind);
            });
        settingsWindow_->SetCategorySettingsChangedCallback([this]() {
            LoadCategorySettingsAndApply();
        });
        settingsWindow_->SetAddWidgetToDesktopCallback(
            [this](const std::wstring& packageId) {
                const size_t previousCount = widgets_.size();
                AddLuaWidgetAt(POINT{ -32000, -32000 }, packageId);
                return widgets_.size() > previousCount;
            });
        settingsWindow_->SetComponentSpacingMaximumProvider([this]() {
            return GetMaximumComponentSpacingScale();
        });

        settingsWindow_->SyncDisplaySettings(iconSpacingScale_,
            componentSpacingScale_, itemFontSizeCu_, listItemFontSizeCu_,
            static_cast<float>(itemFontWeight_), shortcutArrowMode_,
            iconBeautifySettings_);
        settingsWindow_->SyncDockEnabled(generalSettings_.dockEnabled);
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
        widgetEngine_->SetApplicationCatalogProvider([this]() {
            return BuildLuaApplicationCatalog();
        });
        widgetEngine_->SetApplicationIndexStatusProvider([this]() {
            return BuildLuaApplicationIndexStatus();
        });
        widgetEngine_->SetApplicationLaunchCallback(
            [this](const std::wstring& target) {
                return LuaOpenPath(target);
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
                if (customDesktopVisible_)
                    InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            bool invalidated = false;
            if (luaWidgetPanelRequest_.widgetId ==
                    widgetId &&
                !luaWidgetPanelAnimation_.IsHidden())
            {
                RECT dirty =
                    GetLuaWidgetPanelRect();
                InflateRect(&dirty, 3, 3);
                InvalidateRect(
                    hwnd_, &dirty, FALSE);
                invalidated = true;
            }
            for (const auto& widget : widgets_)
            {
                if (widget.id != widgetId || widget.type != DesktopWidgetType::LuaScript)
                    continue;
                if (customDesktopVisible_ &&
                    (!desktopIconsHidden_ ||
                        widget.keepWhenDesktopHidden))
                {
                    RECT dirty =
                        GetStandaloneWidgetFrameRect(widget);
                    if (!IsRectEmpty(&dirty))
                    {
                        InflateRect(&dirty, 3, 3);
                        InvalidateRect(hwnd_, &dirty, FALSE);
                        invalidated = true;
                    }
                }
                return;
            }
            if (!invalidated && customDesktopVisible_)
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
            RestoreInteractionInputFocus();
            UpdateHostInputImePosition();
        });
        widgetNotificationPresenter_.SetActionCallback(
            [this](const std::string& notificationId,
                const std::string& actionId) {
                if (widgetEngine_)
                    widgetEngine_->OnNotificationAction(
                        notificationId, actionId);
            });
        widgetEngine_->SetNotifyCallback([this](
            const snowdesktop::widget_runtime::
                WidgetNotificationHostRequest& request) {
            HWND owner = controlHwnd_ ? controlHwnd_ : hwnd_;
            using Operation = snowdesktop::widget_runtime::
                WidgetNotificationHostOperation;
            switch (request.operation)
            {
            case Operation::Show:
                if (!request.imagePath.empty() || request.progress ||
                    !request.actions.empty())
                    return widgetNotificationPresenter_.Show(owner, request);
                return trayIconController_.ShowBalloon(owner,
                    request.id, request.title, request.message);
            case Operation::Update:
                if (widgetNotificationPresenter_.Contains(request.id))
                    return widgetNotificationPresenter_.Update(owner, request);
                if (!request.imagePath.empty() || request.progress ||
                    !request.actions.empty())
                {
                    (void)trayIconController_.DismissBalloon(owner, request.id);
                    return widgetNotificationPresenter_.Show(owner, request);
                }
                return trayIconController_.UpdateBalloon(owner,
                    request.id, request.title, request.message);
            case Operation::Dismiss:
                if (widgetNotificationPresenter_.Contains(request.id))
                    return widgetNotificationPresenter_.Dismiss(request.id);
                return trayIconController_.DismissBalloon(
                    owner, request.id);
            }
            return false;
        });
        widgetEngine_->SetFilePickerCallback(
            [this](const LuaWidgetFilePickerRequest& request) {
                return ShowLuaWidgetFilePicker(hwnd_, request);
            });
        widgetEngine_->SetLogicalSlotPickerCallback(
            [this](const LogicalSlotPickerRequest& request) {
                return OpenLuaLogicalSlotPicker(request);
            });
        widgetEngine_->SetWidgetTimerRequestCallback([this](const std::wstring& widgetId, UINT intervalMs) -> UINT_PTR {
            if (!hwnd_) return 0;
            const snowdesktop::UiScheduleToken token =
                uiAnimationScheduler_.ScheduleInterval(
                    intervalMs,
                    [this, widgetId](
                        snowdesktop::UiScheduleToken dueToken) {
                        if (widgetEngine_)
                        {
                            widgetEngine_->OnWidgetTimer(
                                widgetId,
                                static_cast<UINT_PTR>(dueToken));
                        }
                    });
            if (!token)
                return 0;
            const UINT_PTR timerId =
                static_cast<UINT_PTR>(token);
            widgetTimerIds_[timerId] = widgetId;
            return timerId;
        });
        widgetEngine_->SetWidgetTimerKillCallback([this](UINT_PTR timerId) {
            if (!timerId) return;
            uiAnimationScheduler_.Cancel(
                static_cast<snowdesktop::UiScheduleToken>(timerId));
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
    StartSteamWorkshopWatcher();

    // Expose the tray menu only after SettingsWindow and all of its callbacks
    // are configured. Activation requests received during startup remain
    // pending until this point, including when native initialization is retried.
    startupInitializationComplete_ = true;
    AddTrayIcon();
    TryShowPendingSettingsWindow();
    SetSoftwareDesktopEnabled(customDesktopVisible_, false);
    if (customDesktopVisible_)
    {
        ReconcileDesktopHoverState(
            snowdesktop::desktop_hover_rules::
                ReconcileMode::AllowImmediateActivation);
        UpdateWindow(hwnd_);
        FlushPendingCompositionCommit();
    }
    WriteDiagnosticLogEntry(customDesktopVisible_
        ? L"Window shown, entering loop"
        : L"Native desktop active, entering loop");

    MSG msg{};
    bool running = true;
    while (running)
    {
        HANDLE animationWait = uiAnimationScheduler_.WaitHandle();
        const DWORD handleCount = animationWait ? 1U : 0U;
        const DWORD waitResult = MsgWaitForMultipleObjectsEx(
            handleCount,
            animationWait ? &animationWait : nullptr,
            INFINITE,
            QS_ALLINPUT,
            MWMO_INPUTAVAILABLE);
        if (waitResult == WAIT_FAILED)
            break;
        const bool animationWasReady =
            handleCount == 1 &&
            waitResult == WAIT_OBJECT_0;
        // Drain a bounded batch of queued input/window work first. When the
        // waitable animation timer and mouse input are both ready, Windows
        // reports the lower-indexed handle first; advancing animation here
        // and again after every message lets a costly frame repeatedly jump
        // ahead of pointer feedback.
        unsigned processedMessages = 0;
        while (processedMessages < 64 &&
            PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            // Pointer-driven desktop/Dock pixels must enter their own DComp
            // channel first. Quick Navigation is flushed independently so a
            // panel animation transaction cannot delay this presentation.
            FlushPendingCompositionCommit();
            FlushPendingQuickNavigationCompositionCommit();
            ++processedMessages;
        }
        if (animationWait &&
            (animationWasReady ||
                WaitForSingleObject(animationWait, 0) ==
                    WAIT_OBJECT_0))
        {
            // UiAnimationScheduler advances from the current monotonic time
            // and skips missed deadlines, so one callback batch per pump
            // iteration is sufficient and never creates catch-up bursts. The
            // initial wait result must be retained because the high-resolution
            // waitable timer is auto-reset and that wait consumes its signal.
            uiAnimationScheduler_.DispatchDue();
            FlushPendingCompositionCommit();
            FlushPendingQuickNavigationCompositionCommit();
        }
        if (settingsWindow_ && settingsWindow_->IsVisible() &&
            settingsWindow_->NeedsRender())
            settingsWindow_->Render();
    }
    uiAnimationScheduler_.Shutdown();
    OleUninitialize();
    return static_cast<int>(msg.wParam);
}

#include "app.h"
#include "../deployment_context.h"
#include "../drag_input_rules.h"
#include "../steam_app_identity.h"
#include "../steam_child_environment.h"
#include "../widget_engine_settings_backend.h"
#include "../widget_settings_service.h"

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

std::wstring QuoteProcessArgument(std::wstring_view argument)
{
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument)
    {
        if (character == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (character == L'\"')
        {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

std::filesystem::path SteamWorkshopManagerPath()
{
    return std::filesystem::path(GetExecutableDirectoryPath()) /
        L"SnowDesktopWorkshopManager.exe";
}

bool IsSteamWorkshopPublisherAvailable()
{
    if (!WidgetEngine::IsSteamWorkshopBridgeAvailable())
        return false;
    const std::filesystem::path manager = SteamWorkshopManagerPath();
    std::error_code error;
    if (!std::filesystem::is_regular_file(manager, error) || error)
        return false;
    const DWORD attributes = GetFileAttributesW(manager.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool LaunchSteamWorkshopPublisher(
    const std::filesystem::path& developmentRoot,
    const std::filesystem::path& projectDirectory)
{
    if (!IsSteamWorkshopPublisherAvailable())
        return false;
    const std::filesystem::path manager = SteamWorkshopManagerPath();
    const std::string effectiveLanguage =
        Locale::Instance().GetEffectiveLanguage();
    const std::wstring managerLanguage(
        effectiveLanguage.begin(), effectiveLanguage.end());
    std::wstring commandLine = QuoteProcessArgument(manager.wstring()) +
        L" --development-root " +
        QuoteProcessArgument(developmentRoot.wstring()) +
        L" --language " + QuoteProcessArgument(managerLanguage) +
        L" --settings-file " +
        QuoteProcessArgument(GetGeneralSettingsPath());
    if (!projectDirectory.empty())
    {
        commandLine += L" --project-directory " +
            QuoteProcessArgument(projectDirectory.wstring());
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> environment =
        snowdesktop::BuildSnowDesktopSteamChildEnvironment();
    if (environment.empty())
        return false;
    const std::wstring workingDirectory =
        manager.parent_path().wstring();
    if (!CreateProcessW(manager.c_str(), commandLine.data(), nullptr,
            nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
            environment.data(), workingDirectory.c_str(), &startup,
            &process))
    {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
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

    InitializeSettingsController();

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    const HRESULT oleInitializeResult = OleInitialize(nullptr);
    if (FAILED(oleInitializeResult))
    {
        WriteDiagnosticLogEntry(L"OleInit FAILED");
        return __LINE__;
    }
    WriteDiagnosticLogEntry(L"OleInit ok");

    // OleInitialize returns S_FALSE when this STA was already initialized, and
    // that successful call still requires a matching OleUninitialize. Keep the
    // pairing scoped to Run so every later bootstrap failure unwinds it too.
    struct OleUninitializeOnExit final
    {
        ~OleUninitializeOnExit() noexcept { OleUninitialize(); }
    };
    const OleUninitializeOnExit oleUninitializeOnExit;

    if (!uiAnimationScheduler_.Initialize())
    {
        WriteDiagnosticLogEntry(
            L"UiAnimationScheduler initialization failed");
        return __LINE__;
    }

    instance_ = instance;

    // Resolve the persisted desktop mode before touching Explorer's icon layer.
    LoadGeneralSettingsAndApply();

    // Creating WorkerW mutates Explorer's desktop window tree. Do this once
    // before the overlay is created; periodic discovery remains read-only.
    EnsureDesktopWorkerWindow();

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
    if (settingsController_)
    {
        // dockEnabled is persisted in the layout document rather than the
        // general-settings file. LoadLayoutSlots has just projected the
        // authoritative runtime value into generalSettings_; publish that
        // domain before any WinUI presenter can observe the controller's
        // earlier default-false snapshot.
        (void)settingsController_->SynchronizeGeneral(generalSettings_);

        snowdesktop::DesktopDisplaySettings desktopSettings;
        desktopSettings.dockEnabled = generalSettings_.dockEnabled;
        desktopSettings.iconSpacingScale = iconSpacingScale_;
        desktopSettings.itemIconSizeScale = itemIconSizeScale_;
        desktopSettings.itemFontSizeCu = itemFontSizeCu_;
        desktopSettings.listItemFontSizeCu = listItemFontSizeCu_;
        desktopSettings.itemFontWeight = static_cast<int>(itemFontWeight_);
        desktopSettings.shortcutArrowMode = shortcutArrowMode_;
        desktopSettings.iconBeautify = iconBeautifySettings_;
        (void)settingsController_->SynchronizeDesktop(
            std::move(desktopSettings));
    }
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
    {
        WNDCLASSEXW popup{};
        popup.cbSize = sizeof(popup);
        popup.style = CS_DBLCLKS;
        popup.lpfnWndProc = FloatingPopupWndProc;
        popup.hInstance = instance;
        popup.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        popup.hbrBackground = nullptr;
        popup.lpszClassName = kFloatingPopupWindowClassName;
        RegisterClassExW(&popup);
    }
    {
        WNDCLASSEXW preview{};
        preview.cbSize = sizeof(preview);
        preview.lpfnWndProc = DragPreviewWndProc;
        preview.hInstance = instance;
        preview.hbrBackground = nullptr;
        preview.lpszClassName = kDragPreviewWindowClassName;
        RegisterClassExW(&preview);
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

    snowdesktop::winui::SettingsWindowHostOptions settingsHostOptions;
    settingsHostOptions.windowTitle = _LW("app.settings.title");
    settingsHostOptions.localize = [](std::string_view key) {
        const std::string ownedKey(key);
        return std::wstring(Locale::Instance().TrW(ownedKey.c_str()));
    };
    settingsHostOptions.languageCatalog = []() {
        std::vector<std::pair<std::string, std::wstring>> languages;
        for (const auto& language :
            Locale::Instance().GetAvailableLanguages())
        {
            languages.emplace_back(language.code,
                Utf8ToWide(Locale::Instance().GetLanguageSelectionLabel(
                    language.code)));
        }
        return languages;
    };
    settingsHostOptions.searchInput = [this]() {
        snowdesktop::SettingsSearchIndexInput input;
        input.languageTag = Locale::Instance().GetEffectiveLanguage();
        if (!widgetSettingsBackend_)
            return input;
        // Indexing must not replace live WidgetSettingsService sessions or
        // advance their revisions. A short-lived reader evaluates the same
        // v2 dependency/visibility rules without publishing UI events.
        snowdesktop::widget_runtime::WidgetSettingsService searchReader(
            *widgetSettingsBackend_);
        for (const auto& widget : widgets_)
        {
            if (widget.type != DesktopWidgetType::LuaScript)
                continue;
            const auto loaded = searchReader.Load(widget.id);
            if (!loaded.Succeeded() || !loaded.snapshot)
            {
                continue;
            }
            const auto& snapshot = *loaded.snapshot;
            snowdesktop::WidgetSettingsSearchDescriptor searchable;
            searchable.instanceId = widget.id;
            searchable.widgetName = Utf8ToWide(snapshot.widgetName);
            if (searchable.widgetName.empty())
                searchable.widgetName = widget.title;

            std::unordered_map<std::string, std::wstring> groupLabels;
            for (const auto& group : snapshot.groups)
                groupLabels[group.id] = Utf8ToWide(group.label);
            std::unordered_set<std::string> indexedKeys;
            for (const auto& fieldState : snapshot.fields)
            {
                const auto& schema = fieldState.schema;
                if (!fieldState.visible || schema.key.empty() ||
                    schema.label.empty() ||
                    !indexedKeys.insert(schema.key).second)
                {
                    continue;
                }
                snowdesktop::WidgetSettingSearchFieldDescriptor field;
                field.key = schema.key;
                field.focusId = schema.key;
                field.label = Utf8ToWide(schema.label);
                field.description = Utf8ToWide(schema.description);
                const auto group = groupLabels.find(schema.group);
                if (group != groupLabels.end())
                    field.groupLabel = group->second;
                searchable.fields.push_back(std::move(field));
            }
            if (!searchable.fields.empty())
                input.widgets.push_back(std::move(searchable));
        }
        return input;
    };
    settingsHostOptions.homeAboutStatus = [this](
        std::uint64_t generation,
        std::uint64_t revision) {
        snowdesktop::winui::HomeAboutStatusPatch patch;
        patch.generation = generation;
        patch.revision = revision;
        patch.applicationVersion = Utf8ToWide(SNOWDESKTOP_VERSION);
        patch.installedWidgetCount = widgets_.size();
        patch.packaged = snowdesktop::deployment::IsPackaged();
        patch.updateState = settingsUpdateState_;
        patch.availableVersion = settingsUpdateAvailableVersion_;
        patch.updateDetail = settingsUpdateDetailKey_.empty()
            ? std::wstring{} : _LW(settingsUpdateDetailKey_.c_str());
        patch.animationDiagnosticsEnabled =
            uiAnimationScheduler_.DiagnosticsEnabled();
        patch.animationDiagnosticsStatus =
            BuildAnimationDiagnosticsStatus();
        return patch;
    };
    settingsHostOptions.startupConflict = [this]() {
        using snowdesktop::winui::GeneralStartupConflict;
        using snowdesktop::winui::GeneralStartupConflictKind;

        GeneralStartupConflict conflict;
        const snowdesktop::AutoStartQueryResult state =
            QueryAutoStartState();
        if (state.packaged && snowdesktop::HasActivePortableAutoStart(
                state.portableOwner, state.portableApproval))
        {
            conflict.kind =
                GeneralStartupConflictKind::PortableVersionOwnsStartup;
            conflict.ownerCommand = state.portableCommand;
        }
        else if (!state.packaged && state.installedPackageEnabled)
        {
            conflict.kind =
                GeneralStartupConflictKind::InstalledVersionOwnsStartup;
        }
        return conflict;
    };
    settingsHostOptions.refreshExternalState = [this]() {
        if (!settingsController_)
            return;
        if (const auto snapshot = settingsController_->Snapshot())
            PrepareSettingsUpdateSession(snapshot->generation);
        GeneralSettings general = generalSettings_;
        general.autoStartEnabled = QueryAutoStartEnabled();
        if (settingsController_->SynchronizeGeneral(general))
            generalSettings_.autoStartEnabled = general.autoStartEnabled;

        snowdesktop::DesktopDisplaySettings desktop;
        desktop.dockEnabled = generalSettings_.dockEnabled;
        desktop.iconSpacingScale = iconSpacingScale_;
        desktop.itemIconSizeScale = itemIconSizeScale_;
        desktop.itemFontSizeCu = itemFontSizeCu_;
        desktop.listItemFontSizeCu = listItemFontSizeCu_;
        desktop.itemFontWeight = static_cast<int>(itemFontWeight_);
        desktop.shortcutArrowMode = shortcutArrowMode_;
        desktop.iconBeautify = iconBeautifySettings_;
        (void)settingsController_->SynchronizeDesktop(std::move(desktop));
        SyncSystemTaskbarSettingsFromWindows();
    };
    settingsHostOptions.developerToolsVisible = [this]() {
        if (settingsController_)
        {
            const auto snapshot = settingsController_->Snapshot();
            if (snapshot && snapshot->sessionActive)
            {
                return snapshot->values.general.
                    widgetDeveloperToolsEnabled;
            }
        }
        return generalSettings_.widgetDeveloperToolsEnabled;
    };
    settingsHostOptions.debugVisible = []() { return false; };
    settingsHostOptions.ensureWidgetSettingsInstance = [this](
        std::wstring_view instanceId) {
        if (!widgetEngine_ || instanceId.empty())
            return false;
        const std::size_t index = FindWidgetIndexById(
            std::wstring(instanceId));
        if (index >= widgets_.size() ||
            widgets_[index].type != DesktopWidgetType::LuaScript)
        {
            return false;
        }
        return widgetEngine_->EnsureWidgetLoaded(
            widgets_[index].id, widgets_[index].packageId);
    };
    settingsHostOptions.backupDataPage.commitLayoutRestore = [this](
        snowdesktop::winui::LayoutRestorePayload payload) {
        return CommitLayoutRestore(std::move(payload));
    };

    settingsHostOptions.widgetsPage.locale = []() {
        return Locale::Instance().GetEffectiveLanguage();
    };
    settingsHostOptions.widgetsPage.instances = [this]() {
        std::vector<snowdesktop::winui::WidgetsPageHostInstance> instances;
        instances.reserve(widgets_.size());
        for (const auto& widget : widgets_)
        {
            if (widget.type != DesktopWidgetType::LuaScript)
                continue;
            snowdesktop::winui::WidgetsPageHostInstance instance;
            instance.instanceId = widget.id;
            instance.packageId = widget.packageId;
            instance.displayName = widget.title;
            if (widgetSettingsBackend_)
            {
                snowdesktop::widget_runtime::
                    WidgetSettingsBackendDescriptor descriptor;
                const auto described = widgetSettingsBackend_->Describe(
                    widget.id, descriptor);
                instance.settingsAvailable = described.Succeeded() &&
                    (!descriptor.manifestFields.empty() ||
                        !descriptor.scriptFields.empty());
            }
            instances.push_back(std::move(instance));
        }
        return instances;
    };
    settingsHostOptions.widgetsPage.developerOverridesVisible = [this]() {
        if (settingsController_)
        {
            const auto snapshot = settingsController_->Snapshot();
            if (snapshot && snapshot->sessionActive)
            {
                return snapshot->values.general.
                    widgetDeveloperToolsEnabled;
            }
        }
        return generalSettings_.widgetDeveloperToolsEnabled;
    };
    settingsHostOptions.widgetsPage.agentSkillTargetMask = [this]() {
        if (settingsController_)
        {
            const auto snapshot = settingsController_->Snapshot();
            if (snapshot)
                return snapshot->values.general.agentSkillTargetMask;
        }
        return generalSettings_.agentSkillTargetMask;
    };
    settingsHostOptions.widgetsPage.setAgentSkillTargetMask = [this](
        int mask) {
        if (!settingsController_)
            return false;
        const auto snapshot = settingsController_->Snapshot();
        if (!snapshot || !snapshot->sessionActive)
            return false;
        mask = std::clamp(mask, 0,
            GeneralSettings::kAllAgentSkillTargetsMask);
        GeneralSettings general = snapshot->values.general;
        general.agentSkillTargetMask = mask;
        settingsController_->UpdateGeneral(std::move(general),
            snowdesktop::SettingsUpdateMode::PreviewAndCommit);
        const auto updated = settingsController_->Snapshot();
        return updated && updated->values.general.agentSkillTargetMask == mask;
    };
    settingsHostOptions.widgetsPage.openDevelopmentFolder = [this]() {
        const auto paths = WidgetEngine::GetWidgetPackagePaths();
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(controlHwnd_, L"open",
                paths.development.c_str(), nullptr, nullptr,
                SW_SHOWNORMAL)) <= 32)
        {
            return snowdesktop::winui::WidgetsPageHostOperationResult::
                Failure(_LW(
                    "settings.about.link.openFailed"));
        }
        return snowdesktop::winui::WidgetsPageHostOperationResult::Success(
            false);
    };
    settingsHostOptions.widgetsPage.developmentProjectCreated = [this](
        const std::filesystem::path& projectRoot) {
        if (settingsController_)
        {
            const auto snapshot = settingsController_->Snapshot();
            if (snapshot && snapshot->sessionActive)
            {
                GeneralSettings general = snapshot->values.general;
                general.widgetDeveloperToolsEnabled = true;
                settingsController_->UpdateGeneral(
                    std::move(general),
                    snowdesktop::SettingsUpdateMode::PreviewAndCommit);
            }
        }
        if (!projectRoot.empty())
        {
            (void)ShellExecuteW(controlHwnd_, L"open",
                projectRoot.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (settingsWindow_)
        {
            (void)settingsWindow_->Open(
                snowdesktop::SettingsRoute::ForPage(
                    snowdesktop::SettingsPage::DeveloperTools));
        }
    };
    settingsHostOptions.widgetsPage.canPublishDevelopmentPackage = []() {
        return IsSteamWorkshopPublisherAvailable();
    };
    settingsHostOptions.widgetsPage.workshopAvailable = []() {
        return WidgetEngine::IsSteamWorkshopBridgeAvailable();
    };
    settingsHostOptions.widgetsPage.publishDevelopmentPackage = [](
        const std::filesystem::path& projectDirectory) {
        const auto paths = WidgetEngine::GetWidgetPackagePaths();
        if (!LaunchSteamWorkshopPublisher(
                paths.development, projectDirectory))
        {
            return snowdesktop::winui::WidgetsPageHostOperationResult::
                Failure(_LW(
                    "app.settings.widgets_publisher_launch_failed"));
        }
        return snowdesktop::winui::WidgetsPageHostOperationResult::Success(
            false);
    };
    settingsHostOptions.widgetsPage.openWorkshopItem = [this](
        std::string_view externalItemId) {
        const std::string publishedFileId =
            snowdesktop::widget::SteamPublishedFileId(externalItemId);
        if (publishedFileId.empty())
        {
            return snowdesktop::winui::WidgetsPageHostOperationResult::
                Failure(_LW("settings.widgets.workshop.openFailed"));
        }
        const std::wstring client =
            snowdesktop::SnowDesktopSteamCommunityItemClientUrl(
                publishedFileId);
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(controlHwnd_, L"open",
                client.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        {
            const std::wstring web =
                snowdesktop::SnowDesktopSteamCommunityItemUrl(
                    publishedFileId);
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(controlHwnd_, L"open",
                    web.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            {
                return snowdesktop::winui::WidgetsPageHostOperationResult::
                    Failure(_LW(
                        "settings.widgets.workshop.openFailed"));
            }
        }
        return snowdesktop::winui::WidgetsPageHostOperationResult::Success(
            false);
    };
    settingsHostOptions.widgetsPage.openWorkshop = [this](
        std::string_view) {
        const std::wstring client =
            snowdesktop::SnowDesktopSteamWorkshopClientUrl();
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(controlHwnd_, L"open",
                client.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        {
            const std::wstring web =
                snowdesktop::SnowDesktopSteamWorkshopUrl();
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(controlHwnd_, L"open",
                    web.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            {
                return snowdesktop::winui::WidgetsPageHostOperationResult::
                    Failure(_LW("settings.widgets.workshop.openFailed"));
            }
        }
        return snowdesktop::winui::WidgetsPageHostOperationResult::Success(
            false);
    };
    settingsHostOptions.widgetsPage.addPackageToDesktop = [this](
        std::wstring_view packageId) {
        const std::size_t before = widgets_.size();
        AddLuaWidgetAt(POINT{-32000, -32000}, std::wstring(packageId));
        if (widgets_.size() == before)
        {
            return snowdesktop::winui::WidgetsPageHostOperationResult::
                Failure(_LW("app.settings.widgets_add_to_desktop_failed"));
        }
        return snowdesktop::winui::WidgetsPageHostOperationResult::Success();
    };
    settingsHostOptions.widgetsPage.canSynchronizeSource = [](
        std::string_view sourceId) {
        return sourceId == "steam-workshop" &&
            WidgetEngine::IsSteamWorkshopBridgeAvailable();
    };
    settingsHostOptions.widgetsPage.synchronizeSource = [this](
        std::uint64_t generation,
        std::uint64_t taskId,
        std::string sourceId,
        snowdesktop::winui::WidgetsPageBackendOptions::AsyncCompletion done) {
        if (sourceId != "steam-workshop")
        {
            done(snowdesktop::winui::WidgetsPageHostOperationResult::Failure(
                _LW("settings.widgets.source.syncFailed")));
            return;
        }
        bool alreadyRunning = false;
        {
            std::lock_guard lock(
                steamWorkshopSubscriptionPollState_->mutex);
            alreadyRunning = steamWorkshopSubscriptionPollState_->
                queryInFlight.load();
            const std::uint64_t queryId = alreadyRunning
                ? steamWorkshopSubscriptionPollState_->activeQueryId
                : steamWorkshopSubscriptionPollState_->nextQueryId;
            SteamWorkshopSubscriptionPollState::SettingsCompletion
                completion;
            completion.generation = generation;
            completion.taskId = taskId;
            completion.queryId = queryId;
            completion.done = std::move(done);
            steamWorkshopSubscriptionPollState_->settingsCompletions.
                push_back(std::move(completion));
        }
        PollSteamWorkshopSubscriptions(!alreadyRunning);
    };
    settingsHostOptions.widgetsPage.unsubscribeWorkshop = [this](
        std::uint64_t generation,
        std::uint64_t taskId,
        std::string externalItemId,
        snowdesktop::winui::WidgetsPageBackendOptions::AsyncCompletion done) {
        const std::string publishedFileId = snowdesktop::widget::
            SteamPublishedFileId(externalItemId);
        if (publishedFileId.empty())
        {
            done(snowdesktop::winui::WidgetsPageHostOperationResult::Failure(
                _LW("settings.widgets.source.syncFailed")));
            return;
        }
        std::vector<std::string> expectedPackageIds;
        for (const auto& package : WidgetEngine::ListWidgetPackages())
        {
            if (package.builtin || package.development ||
                package.source.providerId != "steam-workshop" ||
                snowdesktop::widget::SteamPublishedFileId(
                    package.source.externalItemId) != publishedFileId)
            {
                continue;
            }
            expectedPackageIds.push_back(package.manifest.id);
        }
        const auto pollState = steamWorkshopSubscriptionPollState_;
        const HWND notifyWindow = hwnd_;
        const std::wstring reconciliationScheduleFailure = _LW(
            "settings.widgets.source.syncFailed");
        std::thread([generation, taskId,
                        externalItemId = std::move(externalItemId),
                        publishedFileId,
                        expectedPackageIds = std::move(expectedPackageIds),
                        pollState, notifyWindow,
                        reconciliationScheduleFailure,
                        done = std::move(done)]() mutable {
            std::string error;
            if (!WidgetEngine::UnsubscribeSteamWorkshopItem(
                    externalItemId, error))
            {
                if (done)
                {
                    done(snowdesktop::winui::
                            WidgetsPageHostOperationResult::Failure(
                                Utf8ToWide(error)));
                }
                return;
            }

            std::uint64_t queryId = 0;
            {
                std::lock_guard lock(pollState->mutex);
                // Always wait for a query that starts after Steam accepted
                // the unsubscribe request. An in-flight snapshot may have
                // been captured before the request and is not authoritative
                // for this operation.
                queryId = pollState->nextQueryId;
                SteamWorkshopSubscriptionPollState::SettingsCompletion
                    completion;
                completion.generation = generation;
                completion.taskId = taskId;
                completion.queryId = queryId;
                completion.expectedUnsubscribedPublishedFileId =
                    publishedFileId;
                completion.expectedRemovedPackageIds =
                    std::move(expectedPackageIds);
                completion.done = std::move(done);
                pollState->settingsCompletions.push_back(
                    std::move(completion));
                pollState->refreshPending.store(true);
            }

            if (notifyWindow && PostMessageW(notifyWindow,
                    kSteamWorkshopSubscriptionChangedMessage, 0, 0))
            {
                return;
            }

            snowdesktop::winui::WidgetsPageBackendOptions::AsyncCompletion
                failedCompletion;
            {
                std::lock_guard lock(pollState->mutex);
                auto& pending = pollState->settingsCompletions;
                const auto item = std::find_if(pending.begin(), pending.end(),
                    [generation, taskId, queryId](const auto& value) {
                        return value.generation == generation &&
                            value.taskId == taskId &&
                            value.queryId == queryId;
                    });
                if (item != pending.end())
                {
                    failedCompletion = std::move(item->done);
                    pending.erase(item);
                }
            }
            if (failedCompletion)
                failedCompletion(snowdesktop::winui::
                    WidgetsPageHostOperationResult::Failure(
                        reconciliationScheduleFailure));
        }).detach();
    };
    // Subscription queries do not expose a cooperative abort. The WinUI page
    // may detach its visible synchronization task, while its outstanding
    // operation ledger stays busy until this authoritative poll completes;
    // package mutations therefore cannot overlap the reconciliation.
    settingsHostOptions.widgetsPage.hostStateChanged = [this]() {
        ReloadItems(false);
        if (settingsWindow_)
            settingsWindow_->RefreshWidgetsPage();
    };

    settingsWindow_ = std::make_unique<SettingsWindow>();
    if (!settingsController_ ||
        !settingsWindow_->Init(instance, *settingsController_, nullptr,
            std::move(settingsHostOptions)))
    {
        std::wstring message =
            L"WinUI SettingsWindow startup initialization failed";
        if (settingsWindow_ && !settingsWindow_->LastError().empty())
        {
            message += L": ";
            message += settingsWindow_->LastError();
        }
        WriteDiagnosticLogEntry(message.c_str());
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
        widgetEngine_->SetInvalidateCallback([this](
                const std::wstring& widgetId,
                const std::optional<RECT>& requestedDirty,
                std::string_view surface) {
            if (!hwnd_) return;
            if (widgetId.empty())
            {
                if (customDesktopVisible_)
                    InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            bool invalidated = false;
            if (snowdesktop::widget_composition_layer_rules::
                    SurfaceIncludesAuxiliary(surface) &&
                luaWidgetPanelRequest_.widgetId ==
                    widgetId &&
                !luaWidgetPanelAnimation_.IsHidden())
            {
                if (IsLuaPanelHostedByFloatingWindow())
                {
                    InvalidateFloatingPopupWindow(false);
                }
                else
                {
                    RECT dirty = GetLuaWidgetPanelRect();
                    InflateRect(&dirty, 3, 3);
                    InvalidateRect(hwnd_, &dirty, FALSE);
                }
                invalidated = true;
            }
            for (const auto& widget : widgets_)
            {
                if (widget.id != widgetId || widget.type != DesktopWidgetType::LuaScript)
                    continue;
                if (snowdesktop::widget_composition_layer_rules::
                        SurfaceIncludesDesktop(surface) &&
                    customDesktopVisible_ &&
                    (!desktopIconsHidden_ ||
                        widget.keepWhenDesktopHidden))
                {
                    const RECT widgetFrame =
                        GetStandaloneWidgetFrameRect(widget);
                    RECT dirty = widgetFrame;
                    if (requestedDirty)
                    {
                        RECT clipped{};
                        if (!IntersectRect(&clipped, &*requestedDirty,
                                &widgetFrame))
                            return;
                        dirty = clipped;
                    }
                    if (!IsRectEmpty(&dirty))
                    {
                        if (QueueDesktopWidgetComposition(widgetId))
                        {
                            invalidated = true;
                        }
                        else
                        {
                            InflateRect(&dirty,
                                requestedDirty ? 1 : 3,
                                requestedDirty ? 1 : 3);
                            InvalidateRect(hwnd_, &dirty, FALSE);
                            invalidated = true;
                        }
                    }
                }
                return;
            }
            if (!invalidated && customDesktopVisible_)
                InvalidateRect(hwnd_, nullptr, FALSE);
        });
        widgetEngine_->SetNativeMarqueeSyncCallback([this](
                const std::wstring& widgetId,
                const std::vector<LuaWidget::NativeMarqueeText>& marquees,
                bool reducedMotion) {
            return QueueWidgetMarqueeComposition(
                widgetId, marquees, reducedMotion);
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
        const HWND widgetAudioWakeWindow =
            controlHwnd_ ? controlHwnd_ : hwnd_;
        widgetEngine_->SetAudioAnalysisWakeCallback(
            [widgetAudioWakeWindow]() {
                if (widgetAudioWakeWindow)
                {
                    (void)PostMessageW(widgetAudioWakeWindow,
                        kWidgetAudioAnalysisWakeMessage, 0, 0);
                }
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
        widgetSettingsBackend_ = snowdesktop::widget_runtime::
            CreateWidgetEngineSettingsBackend(*widgetEngine_);
        widgetSettingsService_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetSettingsService>(
                *widgetSettingsBackend_);
        if (settingsWindow_)
        {
            settingsWindow_->SetWidgetSettingsService(
                widgetSettingsService_.get());
            settingsWindow_->SetWidgetEngine(widgetEngine_.get());
        }
    }
    else
    {
        if (settingsWindow_)
            settingsWindow_->SetWidgetEngine(nullptr);
        widgetSettingsService_.reset();
        widgetSettingsBackend_.reset();
        widgetEngine_.reset();
    }
    widgetAccessibilityProvider_ = std::make_unique<
        snowdesktop::WidgetAccessibilityProviderHost>(
        [this]() {
            return widgetEngine_
                ? widgetEngine_->RuntimeAccessibilitySnapshots()
                : std::vector<LuaWidgetAccessibilitySnapshot>{};
        },
        [this](const std::wstring& widgetId,
            const std::string& nodeKey) {
            if (!hwnd_ || !IsWindow(hwnd_) || !widgetEngine_)
                return false;
            const size_t index = FindWidgetIndexById(widgetId);
            if (index >= widgets_.size() ||
                widgets_[index].type != DesktopWidgetType::LuaScript)
                return false;
            SelectWidgetOnly(index);
            ::SetFocus(hwnd_);
            const bool focused = nodeKey.empty() ||
                widgetEngine_->RuntimeSetAccessibilityFocus(
                    widgetId, nodeKey);
            if (focused)
                InvalidateRect(hwnd_, nullptr, FALSE);
            return focused;
        },
        [this](const LuaWidgetAccessibilityActionRequest& request) {
            if (!hwnd_ || !IsWindow(hwnd_) || !widgetEngine_)
                return false;
            const size_t index = FindWidgetIndexById(request.widgetId);
            if (index >= widgets_.size() ||
                widgets_[index].type != DesktopWidgetType::LuaScript)
                return false;
            SelectWidgetOnly(index);
            ::SetFocus(hwnd_);
            const bool accepted =
                widgetEngine_->RuntimePerformAccessibilityAction(request);
            if (accepted)
                InvalidateRect(hwnd_, nullptr, FALSE);
            return accepted;
        });
    widgetAccessibilityProvider_->AttachWindow(hwnd_);
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
            const bool nativeDragActive =
                snowdesktop::drag_input_rules::IsNativeDragActive(
                    dragSession_.IsActive(),
                    dragDropController_.IsTransportActive());
            const bool nativeDragMessageSurface =
                snowdesktop::drag_input_rules::
                    IsNativeDragMessageSurface(
                        msg.hwnd == hwnd_,
                        IsPersistentDockHostWindow(msg.hwnd),
                        floatingPopupHwnd_ != nullptr &&
                            msg.hwnd == floatingPopupHwnd_);
            snowdesktop::drag_input_rules::
                CoalesceQueuedMouseMoves(
                    nativeDragActive,
                    nativeDragMessageSurface,
                    msg,
                    [](MSG& next) {
                        return PeekMessageW(
                            &next, nullptr, 0, 0,
                            PM_NOREMOVE) != FALSE;
                    },
                    [](MSG& next) {
                        return PeekMessageW(
                            &next, nullptr, 0, 0,
                            PM_REMOVE) != FALSE;
                    },
                    [](const MSG& left, const MSG& right) {
                        return left.hwnd == right.hwnd;
                    },
                    [](const MSG& message) {
                        return message.message == WM_MOUSEMOVE;
                    });
            const bool settingsMessageHandled = settingsWindow_ &&
                (settingsWindow_->PreTranslateMessage(&msg) ||
                    settingsWindow_->ProcessTabNavigation(&msg));
            if (!settingsMessageHandled)
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
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
    }
    widgetAccessibilityProvider_.reset();
    ShutdownSettingsInfrastructure();
    uiAnimationScheduler_.Shutdown();
    return static_cast<int>(msg.wParam);
}

#include "app.h"
#include "../atomic_file.h"
#include "../deployment_context.h"
#include "../layout_storage.h"

// Settings application, desktop passthrough and retained-surface visibility.

snowdesktop::SettingsActionResult DesktopApp::CommitLayoutRestore(
    snowdesktop::winui::LayoutRestorePayload payload)
{
    using snowdesktop::SettingsActionResult;

    if (!settingsController_ || exitRequested_ || reloading_ ||
        shellFileOperationInFlight_ > 0 || dragSession_.HasContext() ||
        dragDropController_.IsTransportActive())
    {
        return SettingsActionResult::Failure(
            _LW("settings.backup.restoreLayout.busy"));
    }

    // Capture edits made while the worker validated the backup before the
    // live-file transaction begins.
    const SettingsActionResult flushed = settingsController_->FlushAll();
    if (!flushed.Succeeded())
        return flushed;

    std::string validationError;
    if (!snowdesktop::layout_storage::ValidateDocument(
            payload.layoutDocument, &validationError))
    {
        return SettingsActionResult::Failure(
            _LW("settings.backup.restoreLayout.commitFailed"));
    }

    const std::filesystem::path layoutPath = GetLayoutPath();
    const std::filesystem::path storagePath =
        GetDataFilePath(L"SnowDesktop.storage.json");
    std::string previousLayout;
    if (!snowdesktop::atomic_file::ReadAll(
            layoutPath, previousLayout, &validationError) ||
        !snowdesktop::layout_storage::ValidateDocument(
            previousLayout, &validationError))
    {
        return SettingsActionResult::Failure(
            _LW("settings.backup.restoreLayout.commitFailed"));
    }

    std::optional<std::string> previousStorage;
    const bool storageExisted =
        GetFileAttributesW(storagePath.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (storageExisted)
    {
        std::string contents;
        if (!snowdesktop::atomic_file::ReadAll(
                storagePath, contents, &validationError))
        {
            return SettingsActionResult::Failure(
                _LW("settings.backup.restoreLayout.commitFailed"));
        }
        previousStorage = std::move(contents);
    }

    std::string commitError;
    if (!snowdesktop::layout_storage::SaveDocument(
            layoutPath, payload.layoutDocument, &commitError))
    {
        return SettingsActionResult::Failure(
            _LW("settings.backup.restoreLayout.commitFailed"));
    }

    if (payload.storageDocument &&
        !snowdesktop::atomic_file::WriteAll(
            storagePath, *payload.storageDocument, {}, &commitError))
    {
        std::string rollbackError;
        const bool layoutRolledBack =
            snowdesktop::layout_storage::SaveDocument(
                layoutPath, previousLayout, &rollbackError);
        bool storageRolledBack = true;
        if (previousStorage)
        {
            storageRolledBack = snowdesktop::atomic_file::WriteAll(
                storagePath, *previousStorage, {}, &rollbackError);
        }
        else if (!storageExisted)
        {
            std::error_code removeError;
            std::filesystem::remove(storagePath, removeError);
            storageRolledBack = !removeError;
        }
        if (!layoutRolledBack || !storageRolledBack)
        {
            WriteDiagnosticLogEntry(L"Layout restore rollback failed",
                DiagnosticLogLevel::Error);
        }
        return SettingsActionResult::Failure(
            _LW("settings.backup.restoreLayout.commitFailed"));
    }

    // ReloadItems reads both restored documents and writes only the newly
    // reconstructed model. Synchronizing the mirrors prevents a later close
    // from persisting the pre-restore desktop values.
    ReloadItems(true);
    snowdesktop::DesktopDisplaySettings desktop;
    desktop.dockEnabled = generalSettings_.dockEnabled;
    desktop.iconSpacingScale = iconSpacingScale_;
    desktop.itemIconSizeScale = itemIconSizeScale_;
    desktop.itemFontSizeCu = itemFontSizeCu_;
    desktop.listItemFontSizeCu = listItemFontSizeCu_;
    desktop.itemFontWeight = static_cast<int>(itemFontWeight_);
    desktop.shortcutArrowMode = shortcutArrowMode_;
    desktop.iconBeautify = iconBeautifySettings_;
    const bool generalSynchronized =
        settingsController_->SynchronizeGeneral(generalSettings_);
    const bool desktopSynchronized =
        settingsController_->SynchronizeDesktop(std::move(desktop));
    if (!generalSynchronized || !desktopSynchronized)
    {
        WriteDiagnosticLogEntry(
            L"Layout restored but settings mirror synchronization failed",
            DiagnosticLogLevel::Error);
        return SettingsActionResult::Failure(
            _LW("settings.backup.restoreLayout.commitFailed"));
    }
    return SettingsActionResult::Success();
}

class DesktopApp::SettingsHostActionsAdapter final
    : public snowdesktop::SettingsHostActions
{
public:
    explicit SettingsHostActionsAdapter(DesktopApp& app) : app_(app) {}

    snowdesktop::SettingsActionResult OnSettingsPreview(
        const snowdesktop::SettingsSnapshot& snapshot,
        snowdesktop::SettingsDomain domains) override
    {
        using snowdesktop::HasSettingsDomain;
        using snowdesktop::SettingsDomain;

        if (HasSettingsDomain(domains, SettingsDomain::Personalization))
        {
            app_.personalizationSettings_ = snapshot.values.personalization;
            app_.ApplyQuickNavigationAppearance();
            app_.ApplyCollectionPopupAppearance();
            if (app_.dockSettings_.systemTaskbarFollowPersonalization)
                app_.RefreshSystemTaskbarAppearance(false);
            app_.InvalidateAllWidgetSlots();
            if (app_.hwnd_)
                InvalidateRect(app_.hwnd_, nullptr, FALSE);
        }
        if (HasSettingsDomain(domains, SettingsDomain::Dock))
        {
            // Auto-hide and alignment are committed through the Windows Shell
            // request queue.  Keep their last committed mirrors intact while
            // previewing the remaining Dock appearance and layout values so
            // the commit path can detect a requested system-state change.
            const bool committedTaskbarAutoHide =
                app_.dockSettings_.systemTaskbarAutoHide;
            const int committedTaskbarAlignment =
                app_.dockSettings_.systemTaskbarAlignment;
            app_.dockSettings_ = snapshot.values.dock;
            NormalizeDockSettings(app_.dockSettings_);
            app_.dockSettings_.systemTaskbarAutoHide =
                committedTaskbarAutoHide;
            app_.dockSettings_.systemTaskbarAlignment =
                committedTaskbarAlignment;
            app_.ApplyFloatingDockHotkey();
            app_.UpdateLayoutWorkArea();
            app_.LayoutItems();
            app_.InvalidateDragStaticScene();
            if (app_.hwnd_)
                InvalidateRect(app_.hwnd_, nullptr, TRUE);
        }
        if (HasSettingsDomain(domains, SettingsDomain::Desktop))
        {
            app_.PreviewIconSpacing(
                snapshot.values.desktop.iconSpacingScale);
            app_.PreviewItemIconSize(
                snapshot.values.desktop.itemIconSizeScale);
        }
        return snowdesktop::SettingsActionResult::Success(domains);
    }

    snowdesktop::SettingsActionResult OnSettingsCommitted(
        const snowdesktop::SettingsSnapshot& snapshot,
        snowdesktop::SettingsDomain domains) override
    {
        using snowdesktop::HasSettingsDomain;
        using snowdesktop::SettingsDomain;

        DockSettings requestedDockSettings = snapshot.values.dock;
        NormalizeDockSettings(requestedDockSettings);
        if (HasSettingsDomain(domains, SettingsDomain::Dock))
        {
            const bool autoHideChanged =
                app_.dockSettings_.systemTaskbarAutoHide !=
                    requestedDockSettings.systemTaskbarAutoHide;
            const bool alignmentChanged =
                app_.dockSettings_.systemTaskbarAlignment !=
                    requestedDockSettings.systemTaskbarAlignment;

            // Queue system-owned changes before mutating the application
            // mirror.  A rejected request leaves the Dock domain pending so
            // SettingsController can surface the error and retry safely.
            if (autoHideChanged &&
                !RequestSystemTaskbarAutoHideEnabled(
                    requestedDockSettings.systemTaskbarAutoHide))
            {
                return snowdesktop::SettingsActionResult::Failure(
                    L"The system taskbar auto-hide change could not be queued.",
                    SettingsDomain::Dock);
            }
            if (alignmentChanged &&
                !RequestSystemTaskbarAlignmentCentered(
                    requestedDockSettings.systemTaskbarAlignment == 1))
            {
                return snowdesktop::SettingsActionResult::Failure(
                    L"The system taskbar alignment change could not be queued.",
                    SettingsDomain::Dock);
            }
        }

        if (HasSettingsDomain(domains, SettingsDomain::Personalization))
        {
            app_.personalizationSettings_ = snapshot.values.personalization;
            app_.ApplyQuickNavigationAppearance();
            app_.ApplyCollectionPopupAppearance();
            app_.RefreshSystemTaskbarAppearance(false);
            app_.InvalidateAllWidgetSlots();
        }
        if (HasSettingsDomain(domains, SettingsDomain::Dock))
        {
            app_.dockSettings_ = requestedDockSettings;
            app_.ApplyFloatingDockHotkey();
            app_.UpdateLayoutWorkArea();
            app_.LayoutItems();
            app_.SaveLayoutSlots();
            app_.InvalidateDragStaticScene();
            app_.RefreshSystemTaskbarAppearance(true);
        }
        if (HasSettingsDomain(domains, SettingsDomain::Navigation))
        {
            app_.navigationSettings_ = snapshot.values.navigation;
            app_.ApplyNavigationHotkey();
        }
        if (HasSettingsDomain(domains, SettingsDomain::General))
        {
            const bool dockEnabledChanged =
                app_.generalSettings_.dockEnabled !=
                    snapshot.values.general.dockEnabled;
            const bool languageChanged = std::strcmp(
                app_.generalSettings_.language,
                snapshot.values.general.language) != 0;
            app_.generalSettings_ = snapshot.values.general;
            Locale::Instance().SetLanguage(app_.generalSettings_.language);
            app_.SetSoftwareDesktopEnabled(
                app_.generalSettings_.softwareDesktopEnabled, false);
            app_.ApplyDesktopPassthroughHotkey();
            app_.ApplyFloatingDockHotkey();
            if (dockEnabledChanged)
            {
                if (app_.settingsController_)
                {
                    auto desktop = snapshot.values.desktop;
                    desktop.dockEnabled =
                        app_.generalSettings_.dockEnabled;
                    (void)app_.settingsController_->SynchronizeDesktop(
                        std::move(desktop));
                }
                app_.UpdateLayoutWorkArea();
                if (!app_.generalSettings_.dockEnabled)
                    app_.RestoreDockEntriesToDesktop();
                app_.LayoutItems();
                app_.SaveLayoutSlots();
                app_.InvalidateDragStaticScene();
            }
            app_.ApplyQuickNavigationAppearance();
            app_.ApplyCollectionPopupAppearance();
            if (languageChanged)
                app_.ApplyLanguageChange();
        }
        if (HasSettingsDomain(domains, SettingsDomain::Category))
        {
            app_.categorySettings_ = snapshot.values.category;
            NormalizeCategorySettings(app_.categorySettings_);
            for (auto& container : app_.containers_)
            {
                if (auto* categories =
                        dynamic_cast<FileCategories*>(container.get()))
                    categories->InvalidateCategoryCache();
                else if (auto* mapping =
                             dynamic_cast<FolderMapping*>(container.get()))
                    mapping->InvalidateFilterCache();
                else if (auto* group =
                             dynamic_cast<FileGroup*>(container.get()))
                    group->InvalidateHostedView();
            }
        }
        if (HasSettingsDomain(domains, SettingsDomain::Desktop))
        {
            const auto& desktop = snapshot.values.desktop;
            app_.SetIconSpacing(desktop.iconSpacingScale);
            app_.SetItemIconSize(desktop.itemIconSizeScale);
            app_.SetItemFontSize(desktop.itemFontSizeCu);
            app_.SetListItemFontSize(desktop.listItemFontSizeCu);
            app_.SetItemFontWeight(static_cast<DWRITE_FONT_WEIGHT>(
                desktop.itemFontWeight));
            app_.SetShortcutArrowMode(desktop.shortcutArrowMode);
            app_.SetIconBeautifySettings(
                desktop.iconBeautify,
                snowdesktop::IconBeautifyUpdateKind::Commit);
        }
        if (app_.hwnd_)
            InvalidateRect(app_.hwnd_, nullptr, TRUE);
        return snowdesktop::SettingsActionResult::Success(domains);
    }

    snowdesktop::SettingsActionResult OnSettingsRouteChanged(
        const snowdesktop::SettingsRoute&) override
    {
        return snowdesktop::SettingsActionResult::Success();
    }

    snowdesktop::SettingsActionResult Invoke(
        const Request& request) override
    {
        switch (request.action)
        {
        case Action::ApplyLanguage:
            app_.ApplyLanguageChange();
            break;
        case Action::RegisterHotkeys:
            app_.ApplyNavigationHotkey();
            app_.ApplyDesktopPassthroughHotkey();
            app_.ApplyFloatingDockHotkey();
            break;
        case Action::ApplyDock:
            app_.ApplyFloatingDockHotkey();
            app_.UpdateLayoutWorkArea();
            app_.LayoutItems();
            break;
        case Action::ApplyTaskbar:
            app_.RefreshSystemTaskbarAppearance(true);
            break;
        case Action::ApplyDesktopLayout:
            app_.UpdateLayoutWorkArea();
            app_.LayoutItems();
            app_.SaveLayoutSlots();
            break;
        case Action::ApplyCategories:
            if (app_.hwnd_)
                InvalidateRect(app_.hwnd_, nullptr, FALSE);
            break;
        case Action::RefreshDesktop:
            app_.ReloadItems();
            break;
        case Action::RefreshWidgets:
            if (app_.widgetEngine_)
            {
                for (const auto& widget : app_.widgets_)
                {
                    if (widget.type == DesktopWidgetType::LuaScript)
                        app_.widgetEngine_->ReloadWidget(widget.id);
                }
            }
            break;
        case Action::AddWidgetToDesktop:
        {
            const size_t previousCount = app_.widgets_.size();
            app_.AddLuaWidgetAt(POINT{ -32000, -32000 }, request.value);
            if (app_.widgets_.size() == previousCount)
            {
                return snowdesktop::SettingsActionResult::Failure(
                    L"The widget could not be added to the desktop.");
            }
            break;
        }
        case Action::ReloadWidgetInstance:
            if (!app_.widgetEngine_ ||
                !app_.widgetEngine_->ReloadWidget(request.widgetInstanceId))
            {
                return snowdesktop::SettingsActionResult::Failure(
                    L"The widget instance could not be reloaded.");
            }
            break;
        case Action::RestartExplorer:
            if (!RestartWindowsExplorer())
            {
                return snowdesktop::SettingsActionResult::Failure(
                    L"Windows Explorer could not be restarted.");
            }
            break;
        case Action::RestartApplication:
            if (!app_.RequestRestart())
            {
                return snowdesktop::SettingsActionResult::Failure(
                    _LW("app.run.restart_failed"));
            }
            break;
        case Action::ExitApplication:
            app_.RequestExit();
            break;
        case Action::OpenDataDirectory:
        {
            const std::wstring path = GetDataDirectoryPath();
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(
                    app_.controlHwnd_, L"open", path.c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            {
                return snowdesktop::SettingsActionResult::Failure(
                    L"The data directory could not be opened.");
            }
            break;
        }
        case Action::CheckForUpdates:
        {
            const std::wstring target = snowdesktop::deployment::IsPackaged()
                ? snowdesktop::deployment::GetStoreProductPageUri()
                : L"https://github.com/FreeFallingSnow/"
                  L"SnowDesktop_Release/releases/latest";
            if (target.empty() || reinterpret_cast<INT_PTR>(ShellExecuteW(
                    app_.controlHwnd_, L"open", target.c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            {
                return snowdesktop::SettingsActionResult::Failure(
                    L"The update page could not be opened.");
            }
            break;
        }
        case Action::OpenProject:
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(
                    app_.controlHwnd_, L"open",
                    L"https://github.com/FreeFallingSnow/SnowDesktop",
                    nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            {
                return snowdesktop::SettingsActionResult::Failure(
                    L"The project page could not be opened.");
            }
            break;
        case Action::OpenLicense:
        case Action::OpenThirdPartyNotices:
        {
            const wchar_t* filename = request.action == Action::OpenLicense
                ? L"LICENSE" : L"THIRD_PARTY_NOTICES.md";
            std::filesystem::path target =
                std::filesystem::path(GetExecutableDirectoryPath()) /
                filename;
            if (!std::filesystem::exists(target))
            {
                target = request.action == Action::OpenLicense
                    ? L"https://github.com/FreeFallingSnow/"
                      L"SnowDesktop/blob/main/LICENSE"
                    : L"https://github.com/FreeFallingSnow/"
                      L"SnowDesktop/blob/main/THIRD_PARTY_NOTICES.md";
            }
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(
                    app_.controlHwnd_, L"open", target.c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            {
                return snowdesktop::SettingsActionResult::Failure(
                    L"The requested project notice could not be opened.");
            }
            break;
        }
        case Action::ProbeHotkeyAvailability:
            if (request.hotkeyTarget == HotkeyTarget::None)
            {
                return snowdesktop::SettingsActionResult::Failure(
                    L"Hotkey probes require a typed capture target.");
            }
            if (ProbeHotkeyAvailability(
                    request.hotkeyTarget,
                    request.modifiers,
                    request.virtualKey))
            {
                return snowdesktop::SettingsActionResult::Success();
            }
            return snowdesktop::SettingsActionResult::Failure(
                L"The hotkey is unavailable.");
        }
        return snowdesktop::SettingsActionResult::Success();
    }

private:
    bool ProbeHotkeyAvailability(
        HotkeyTarget target,
        UINT modifiers,
        UINT virtualKey) const
    {
        if (virtualKey == 0)
            return false;

        const UINT normalizedModifiers = modifiers &
            (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN);
        const auto matches = [normalizedModifiers, virtualKey](
            UINT configuredModifiers,
            UINT configuredVirtualKey) {
            return normalizedModifiers ==
                    (configuredModifiers &
                        (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN)) &&
                virtualKey == configuredVirtualKey;
        };

        switch (target)
        {
        case HotkeyTarget::QuickNavigation:
            if (app_.navigationSettings_.enabled &&
                matches(app_.navigationSettings_.modifiers,
                    app_.navigationSettings_.virtualKey))
            {
                return app_.navigationHotkeyRegistered_;
            }
            break;
        case HotkeyTarget::DesktopPassthrough:
            if (app_.generalSettings_.desktopPassthroughHotkeyEnabled &&
                app_.customDesktopVisible_ &&
                matches(app_.generalSettings_.
                        desktopPassthroughHotkeyModifiers,
                    app_.generalSettings_.
                        desktopPassthroughHotkeyVirtualKey))
            {
                return app_.desktopPassthroughHotkeyRegistered_;
            }
            break;
        case HotkeyTarget::FloatingDock:
            if (app_.generalSettings_.dockEnabled &&
                app_.dockSettings_.floatingShortcutMode &&
                matches(app_.dockSettings_.floatingHotkeyModifiers,
                    app_.dockSettings_.floatingHotkeyVirtualKey))
            {
                return app_.floatingDockHotkeyRegistered_;
            }
            break;
        case HotkeyTarget::PagePrevious:
        case HotkeyTarget::PageNext:
            return true;
        case HotkeyTarget::None:
            return false;
        }

        HWND probeWindow =
            app_.controlHwnd_ && IsWindow(app_.controlHwnd_)
                ? app_.controlHwnd_
                : (app_.inputHwnd_ && IsWindow(app_.inputHwnd_)
                    ? app_.inputHwnd_ : app_.hwnd_);
        if (!probeWindow || !IsWindow(probeWindow))
            return false;

        const BOOL registered = RegisterHotKey(
            probeWindow,
            kSettingsHotkeyProbeId,
            normalizedModifiers | MOD_NOREPEAT,
            virtualKey);
        if (!registered)
            return false;

        UnregisterHotKey(probeWindow, kSettingsHotkeyProbeId);
        return true;
    }

    DesktopApp& app_;
};

void DesktopApp::InitializeSettingsController()
{
    settingsHostActions_ =
        std::make_unique<SettingsHostActionsAdapter>(*this);
    settingsController_ = std::make_unique<snowdesktop::SettingsController>(
        snowdesktop::CreateNativeSettingsStore(),
        settingsHostActions_.get());

    const snowdesktop::SettingsActionResult result =
        settingsController_->Initialize();
    const auto snapshot = settingsController_->Snapshot();
    if (snapshot && snapshot->initialized)
    {
        personalizationSettings_ = snapshot->values.personalization;
        dockSettings_ = snapshot->values.dock;
        navigationSettings_ = snapshot->values.navigation;
        generalSettings_ = snapshot->values.general;
        categorySettings_ = snapshot->values.category;
    }
    if (!result.Succeeded())
    {
        std::wstring message =
            L"SettingsController initialized with recoverable load errors";
        if (!result.message.empty())
        {
            message += L": ";
            message += result.message;
        }
        WriteDiagnosticLogEntry(message.c_str());
    }
}

void DesktopApp::ShowSettingsWindow(snowdesktop::SettingsRoute route)
{
    settingsWindowOpenRequest_.Request(std::move(route));
    TryShowPendingSettingsWindow();
}

void DesktopApp::TryShowPendingSettingsWindow()
{
    if (!settingsWindowOpenRequest_.Pending() ||
        !startupInitializationComplete_)
        return;

    const snowdesktop::SettingsRoute route =
        settingsWindowOpenRequest_.Route();
    const bool shown = settingsWindow_ && settingsWindow_->Open(route);
    if (shown)
    {
        settingsWindowOpenRequest_.MarkShown();
        if (controlHwnd_ && IsWindow(controlHwnd_))
            KillTimer(controlHwnd_, kSettingsWindowRetryTimerId);
        WriteDiagnosticLogEntry(L"SettingsWindow shown");
        return;
    }

    if (settingsWindowOpenRequest_.RecordFailure(
            kSettingsWindowMaximumAutomaticRetries) &&
        controlHwnd_ && IsWindow(controlHwnd_))
    {
        if (SetTimer(controlHwnd_, kSettingsWindowRetryTimerId,
                kSettingsWindowRetryIntervalMs, nullptr) != 0)
        {
            WriteDiagnosticLogEntry(
                L"SettingsWindow show failed; retry scheduled");
            return;
        }
        wchar_t message[192]{};
        swprintf_s(message,
            L"SettingsWindow retry timer failed (error=%lu)",
            GetLastError());
        WriteDiagnosticLogEntry(message);
    }

    WriteDiagnosticLogEntry(
        L"SettingsWindow show failed; request remains pending");
}

/**
 * @brief 加载导航设置并应用热键注册
 */
void DesktopApp::LoadNavigationSettingsAndApply()
{
    NavigationSettings settings;
    LoadNavigationSettings(GetNavigationSettingsPath().c_str(), settings);
    navigationSettings_ = settings;
    ApplyNavigationHotkey();
}

bool DesktopApp::IsDesktopPassthroughHotkeyDown() const
{
    const auto keyDown = [](int virtualKey) {
        return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    };
    if (!keyDown(static_cast<int>(
            generalSettings_.desktopPassthroughHotkeyVirtualKey)))
        return false;

    const UINT modifiers =
        generalSettings_.desktopPassthroughHotkeyModifiers;
    if ((modifiers & MOD_CONTROL) != 0 && !keyDown(VK_CONTROL))
        return false;
    if ((modifiers & MOD_ALT) != 0 && !keyDown(VK_MENU))
        return false;
    if ((modifiers & MOD_SHIFT) != 0 && !keyDown(VK_SHIFT))
        return false;
    if ((modifiers & MOD_WIN) != 0 &&
        !keyDown(VK_LWIN) && !keyDown(VK_RWIN))
        return false;
    return true;
}

bool DesktopApp::IsDesktopPassthroughPointerDown() const
{
    constexpr int pointerKeys[] = {
        VK_LBUTTON, VK_RBUTTON, VK_MBUTTON,
        VK_XBUTTON1, VK_XBUTTON2
    };
    for (const int virtualKey : pointerKeys)
    {
        if ((GetAsyncKeyState(virtualKey) & 0x8000) != 0)
            return true;
    }
    return false;
}

void DesktopApp::EndDesktopPassthroughHold(
    bool restoreDesktop)
{
    if (desktopPassthroughHotkeyHwnd_ &&
        IsWindow(desktopPassthroughHotkeyHwnd_))
    {
        KillTimer(desktopPassthroughHotkeyHwnd_,
            kDesktopPassthroughHoldTimerId);
    }

    if (!desktopPassthroughHoldActive_)
        return;
    desktopPassthroughHoldActive_ = false;

    if (!restoreDesktop || !customDesktopVisible_ ||
        !hwnd_ || !IsWindow(hwnd_))
        return;

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    desktopBackdropCompositor_.SetVisible(true);
    ReconcileDesktopHoverState(
        snowdesktop::desktop_hover_rules::
            ReconcileMode::AllowImmediateActivation);
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void DesktopApp::BeginDesktopPassthroughHold()
{
    if (desktopPassthroughHoldActive_ ||
        !desktopPassthroughHotkeyRegistered_ ||
        !generalSettings_.desktopPassthroughHotkeyEnabled ||
        !customDesktopVisible_ ||
        !hwnd_ || !IsWindow(hwnd_) ||
        !desktopPassthroughHotkeyHwnd_ ||
        !IsWindow(desktopPassthroughHotkeyHwnd_))
        return;

    // Hiding in the middle of a desktop drag would prevent SnowDesktop from
    // receiving the matching button-up event and leave its interaction state
    // latched. The shortcut can be pressed again after the current gesture.
    if (IsDesktopPassthroughPointerDown() ||
        mouseDown_ || marqueeActive_ ||
        dragSession_.IsActive() ||
        dragDropController_.IsTransportActive() ||
        GetCapture() != nullptr)
        return;

    if (SetTimer(desktopPassthroughHotkeyHwnd_,
            kDesktopPassthroughHoldTimerId,
            kDesktopPassthroughHoldIntervalMs,
            nullptr) == 0)
        return;

    if (quickNavigationOpen_)
    {
        CloseQuickNavigation();
        FinalizeCloseQuickNavigation();
    }
    HideDockWindowPreview();
    HideDragHintWindow();

    desktopPassthroughHoldActive_ = true;
    CloseFloatingDockThen(
        [this]() {
            // The hotkey may have been released while the compositor hand-off
            // was pending. In that case the desktop must remain visible.
            if (!desktopPassthroughHoldActive_ ||
                !hwnd_ || !IsWindow(hwnd_))
                return;
            if (widgetEngine_)
                widgetEngine_->SetAllWidgetDesktopVisible(false);
            desktopBackdropCompositor_.SetVisible(false);
            ShowWindow(hwnd_, SW_HIDE);
        });
}

void DesktopApp::UnregisterDesktopPassthroughHotkey()
{
    EndDesktopPassthroughHold();
    if (desktopPassthroughHotkeyRegistered_ &&
        desktopPassthroughHotkeyHwnd_)
    {
        UnregisterHotKey(desktopPassthroughHotkeyHwnd_,
            kDesktopPassthroughHotkeyId);
    }
    desktopPassthroughHotkeyRegistered_ = false;
    desktopPassthroughHotkeyHwnd_ = nullptr;
}

void DesktopApp::ApplyDesktopPassthroughHotkey()
{
    UnregisterDesktopPassthroughHotkey();
    if (!generalSettings_.desktopPassthroughHotkeyEnabled ||
        !customDesktopVisible_ ||
        generalSettings_.desktopPassthroughHotkeyVirtualKey == 0)
        return;

    HWND target =
        controlHwnd_ && IsWindow(controlHwnd_)
            ? controlHwnd_
            : (inputHwnd_ && IsWindow(inputHwnd_)
                ? inputHwnd_ : hwnd_);
    if (!target)
        return;

    const UINT modifiers =
        generalSettings_.desktopPassthroughHotkeyModifiers |
        MOD_NOREPEAT;
    desktopPassthroughHotkeyRegistered_ =
        RegisterHotKey(target, kDesktopPassthroughHotkeyId,
            modifiers,
            generalSettings_.desktopPassthroughHotkeyVirtualKey) != FALSE;
    if (desktopPassthroughHotkeyRegistered_)
    {
        desktopPassthroughHotkeyHwnd_ = target;
        WriteDiagnosticLogEntry(
            L"Desktop passthrough hold hotkey registered");
    }
    else
    {
        WriteDiagnosticLogEntry(
            L"Desktop passthrough hold hotkey registration failed");
    }
}

void DesktopApp::LoadGeneralSettingsAndApply()
{
    const bool dockEnabled = generalSettings_.dockEnabled;
    const bool demoModeEnabled = generalSettings_.demoModeEnabled;
    GeneralSettings settings;
    LoadGeneralSettings(GetGeneralSettingsPath().c_str(), settings);
    generalSettings_ = settings;
    if (std::strcmp(generalSettings_.language, "system") != 0 &&
        !Locale::Instance().HasLanguage(generalSettings_.language))
    {
        std::strncpy(generalSettings_.language, "system",
            sizeof(generalSettings_.language) - 1);
        generalSettings_.language[sizeof(generalSettings_.language) - 1] = '\0';
    }
    Locale::Instance().SetLanguage(generalSettings_.language);
    generalSettings_.dockEnabled = dockEnabled;
    generalSettings_.quickNavTheme =
        NormalizeFourThemeSelection(generalSettings_.quickNavTheme);
    generalSettings_.collectionPopupTheme =
        NormalizeFourThemeSelection(
            generalSettings_.collectionPopupTheme);
    SetSoftwareDesktopEnabled(generalSettings_.softwareDesktopEnabled, false);
    ApplyQuickNavigationAppearance();
    ApplyCollectionPopupAppearance();
    if (demoModeEnabled != generalSettings_.demoModeEnabled)
    {
        InvalidateDragStaticScene();
        InvalidateDockContainers();
        if (hwnd_ && IsWindow(hwnd_))
            InvalidateRect(hwnd_, nullptr, TRUE);
        InvalidateFloatingDockWindow(false);
    }
}

void DesktopApp::ApplyQuickNavigationAppearance()
{
    const PersonalizationSettings globalAppearance = CurrentPersonalization();
    const int presetId = globalAppearance.backgroundPreset == kAppearancePresetCustom
        ? AppearancePresetFromFourThemeSelection(
            generalSettings_.quickNavTheme)
        : globalAppearance.backgroundPreset;
    const PersonalizationSettings appearance =
        MakeQuickNavigationAppearancePreset(presetId);

    const float luminance = appearance.widgetBgR * 0.2126f +
        appearance.widgetBgG * 0.7152f + appearance.widgetBgB * 0.0722f;
    quickNavLightTheme_ = (presetId == kAppearancePresetLight ||
        presetId == kAppearancePresetAcrylicLight) ||
        luminance >= 0.55f;
    quickNavGlassTheme_ = appearance.glassEnabled;
    quickNavBlurRadius_ = std::clamp(appearance.glassBlurRadius, 4.0f, 48.0f);
    quickNavAppearance_ = appearance;
    if (quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_))
        UpdateQuickNavigationBackdrop();
}

void DesktopApp::ApplyCollectionPopupAppearance()
{
    const PersonalizationSettings globalAppearance = CurrentPersonalization();

    const int selection =
        globalAppearance.backgroundPreset == kAppearancePresetCustom
        ? NormalizeFourThemeSelection(
            generalSettings_.collectionPopupTheme)
        : FourThemeSelectionFromAppearancePreset(
            NormalizeAppearancePresetId(
                globalAppearance.backgroundPreset));
    const int presetId =
        AppearancePresetFromFourThemeSelection(selection);
    collectionPopupAppearance_ =
        MakeQuickNavigationAppearancePreset(presetId);
    collectionPopupLightTheme_ =
        collectionPopupAppearance_.contentTheme == 1;
    collectionPopupGlassTheme_ =
        collectionPopupAppearance_.glassEnabled;
    collectionPopupBlurRadius_ = std::clamp(
        collectionPopupAppearance_.glassBlurRadius,
        4.0f, 48.0f);

    UpdateCollectionPopupBackdrop();
    if (GetOpenPopupWidget())
        InvalidateFloatingPopupWindow(true);
}

void DesktopApp::LoadDockSettingsAndApply()
{
    DockSettings settings;
    LoadDockSettings(GetDockSettingsPath().c_str(), settings);
    NormalizeDockSettings(settings);
    dockSettings_ = settings;
    SyncSystemTaskbarSettingsFromWindows();
    ApplyFloatingDockHotkey();
    systemTaskbarWindowStateChangedTick_.fetch_add(1,
        std::memory_order_relaxed);
    RefreshSystemTaskbarAppearance(true);
    if (hwnd_ && IsWindow(hwnd_))
        InvalidateRect(hwnd_, nullptr, FALSE);
}

void DesktopApp::SyncSystemTaskbarSettingsFromWindows()
{
    // During Explorer restart ABM_GETSTATE returns zero before Shell_TrayWnd
    // exists. Treat that interval as unavailable, not as an external request
    // to disable auto-hide and overwrite the saved software mirror.
    if (!FindWindowW(L"Shell_TrayWnd", nullptr))
        return;

    const bool autoHide = IsSystemTaskbarAutoHideEnabled();
    const bool centered = IsSystemTaskbarAlignmentCentered();
    if (dockSettings_.systemTaskbarAutoHide == autoHide &&
        dockSettings_.systemTaskbarAlignment == (centered ? 1 : 0))
        return;

    dockSettings_.systemTaskbarAutoHide = autoHide;
    dockSettings_.systemTaskbarAlignment = centered ? 1 : 0;
    SaveDockSettings(GetDockSettingsPath().c_str(), dockSettings_);
    if (settingsController_)
        (void)settingsController_->SynchronizeDock(dockSettings_);
}

void DesktopApp::LoadCategorySettingsAndApply()
{
    CategorySettings settings = CategorySettings::Defaults();
    LoadCategorySettings(GetCategorySettingsPath().c_str(), settings);
    categorySettings_ = settings;

    for (auto& c : containers_)
    {
        if (auto* fc = dynamic_cast<FileCategories*>(c.get()))
            fc->InvalidateCategoryCache();
        else if (auto* mapping =
                     dynamic_cast<FolderMapping*>(c.get()))
            mapping->InvalidateFilterCache();
        else if (auto* group =
                     dynamic_cast<FileGroup*>(c.get()))
            group->InvalidateHostedView();
    }
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, FALSE);
}

void DesktopApp::ApplyLanguageChange()
{
    LoadCategorySettingsAndApply();
    if (settingsWindow_)
        settingsWindow_->ApplyLanguageChange();
    if (quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_))
        SetWindowTextW(quickNavigationHwnd_, _LW("app.interact.snow_nav_title"));
    if (quickNavigationSearchEdit_ && IsWindow(quickNavigationSearchEdit_))
    {
        SendMessageW(quickNavigationSearchEdit_, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(_LW("app.nav.search_hint")));
    }

    bool titleChanged = false;
    for (auto& widget : widgets_)
    {
        std::wstring defaultTitle;
        switch (widget.type)
        {
        case DesktopWidgetType::Collection:
            defaultTitle = _LW("widget.collection");
            break;
        case DesktopWidgetType::CollectionGroup:
            defaultTitle = _LW("widget.collection_group");
            break;
        case DesktopWidgetType::FileGroup:
            defaultTitle = _LW("widget.file_group");
            break;
        case DesktopWidgetType::FileCategories:
            defaultTitle = _LW("widget.desktop_files");
            break;
        case DesktopWidgetType::Guide:
            defaultTitle = _LW("app.guide.title");
            break;
        case DesktopWidgetType::LuaScript:
            if (widgetEngine_ && !widget.packageId.empty())
            {
                if (!widgetEngine_->ReloadWidget(widget.id))
                    widgetEngine_->EnsureWidgetLoaded(widget.id, widget.packageId);
                widgetEngine_->NotifyLanguageChanged(widget.id);
                const auto& runtimeWidgets = widgetEngine_->GetWidgets();
                auto runtime = std::find_if(runtimeWidgets.begin(), runtimeWidgets.end(),
                    [&](const LuaWidget& loaded) {
                        return loaded.widgetId == widget.id;
                    });
                if (runtime != runtimeWidgets.end())
                    defaultTitle = Utf8ToWide(runtime->name);
            }
            break;
        case DesktopWidgetType::FolderMapping:
        default:
            break;
        }

        if (widget.customTitle.empty() &&
            !defaultTitle.empty() &&
            (widget.type != DesktopWidgetType::LuaScript ||
                widget.scriptTitle.empty()) &&
            widget.title != defaultTitle)
        {
            widget.title = std::move(defaultTitle);
            titleChanged = true;
        }
    }

    if (titleChanged)
        SaveLayoutSlots();
    if (quickNavigationOpen_)
        InvalidateQuickNavigationWindow();
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, TRUE);
}

void DesktopApp::ToggleDesktopIconsVisibility()
{
    desktopIconsHidden_ = !desktopIconsHidden_;
    // The control-window timer also maintains the Explorer taskbar hook and
    // the blurred desktop background. Keep it alive while icons are hidden.
    ClearHiddenHint();

    if (desktopIconsHidden_)
    {
        if (GetOpenPopupWidget() && !IsOpenPopupRetained())
            CloseCollectionPopup();
        if (!luaWidgetPanelRequest_.widgetId.empty())
        {
            const auto source = std::find_if(
                widgets_.begin(), widgets_.end(),
                [&](const DesktopWidget& widget) {
                    return widget.id ==
                        luaWidgetPanelRequest_.widgetId;
                });
            if (source == widgets_.end() ||
                !source->keepWhenDesktopHidden)
            {
                CloseLuaWidgetPanel(
                    luaWidgetPanelRequest_.widgetId,
                    "desktop-hidden");
            }
        }
    }

    if (hwnd_ && IsWindow(hwnd_))
        InvalidateRect(hwnd_, nullptr, TRUE);
}

bool DesktopApp::HasRetainedElements() const
{
    if (dockSettings_.keepWhenDesktopHidden)
    {
        for (const auto& container : containers_)
            if (dynamic_cast<DockContainer*>(container.get()))
                return true;
    }
    for (const auto& widgetData : widgets_)
        if (widgetData.keepWhenDesktopHidden &&
            !IsRectEmptyRect(widgetData.bounds))
            return true;
    return false;
}

bool DesktopApp::IsOpenPopupRetained() const
{
    if (!desktopIconsHidden_)
        return GetOpenPopupWidget() != nullptr;
    if (!GetOpenPopupWidget())
        return false;
    if (dockFolderPopupOpen_ || popupAnchoredToDock_)
        return dockSettings_.keepWhenDesktopHidden ||
            floatingDockVisible_;
    return popupWidgetIndex_ < widgets_.size() &&
        widgets_[popupWidgetIndex_].keepWhenDesktopHidden;
}

bool DesktopApp::IsRetainedContainer(
    const Container* container) const
{
    if (!container)
        return false;
    if (!desktopIconsHidden_)
        return true;
    if (dynamic_cast<const DockContainer*>(container))
        return dockSettings_.keepWhenDesktopHidden ||
            (floatingDockVisible_ &&
                container == floatingDockContainer_);
    if (container == dockFolderPopupContainer_.get())
        return dockSettings_.keepWhenDesktopHidden;
    const auto* widget =
        dynamic_cast<const WidgetContainer*>(container);
    const DesktopWidget* widgetData = widget
        ? widget->GetWidgetData()
        : nullptr;
    if (widgetData && popupAnchoredToDock_ &&
        dockSettings_.keepWhenDesktopHidden &&
        GetOpenPopupWidget() == widgetData)
        return true;
    return widgetData && widgetData->keepWhenDesktopHidden;
}

bool DesktopApp::IsPointOnRetainedElement(POINT pt) const
{
    if (IsOpenPopupRetained() &&
        IsPointInsideOpenPopup(pt))
        return true;
    if (const DockContainer* dock =
            GetDockContainerAtPoint(pt);
        dock &&
        (dockSettings_.keepWhenDesktopHidden ||
            (floatingDockVisible_ &&
                dock == floatingDockContainer_)))
        return true;
    for (const auto& widgetData : widgets_)
    {
        if (!widgetData.keepWhenDesktopHidden) continue;
        if (luaWidgetPanelRequest_.widgetId == widgetData.id &&
            luaWidgetPanelAnimation_.IsInteractive())
        {
            const RECT panel = GetLuaWidgetPanelRect();
            if (!IsRectEmptyRect(panel) && PtInRect(&panel, pt))
                return true;
        }
        const size_t standalone =
            HitTestStandaloneWidgetIndex(pt);
        if (standalone < widgets_.size() &&
            &widgets_[standalone] == &widgetData)
            return true;
        if (!IsRectEmptyRect(widgetData.bounds) &&
            PtInRect(&widgetData.bounds, pt))
            return true;
        for (const auto& c : containers_)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(c.get());
            if (!wc || wc->GetWidgetData() != &widgetData) continue;
            const RECT bodyRect = wc->GetBodyRect();
            if (PtInRect(&bodyRect, pt))
                return true;
            break;
        }
    }
    return false;
}

void DesktopApp::ShowHiddenHint()
{
    if (!generalSettings_.doubleClickHideDesktop) return;
    showHiddenHint_ = true;
    hiddenHintStartTick_ = GetTickCount();
    if (hwnd_ && IsWindow(hwnd_))
    {
        SetTimer(hwnd_, kHiddenHintTimerId, 100, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void DesktopApp::ClearHiddenHint()
{
    showHiddenHint_ = false;
    hiddenHintStartTick_ = 0;
    if (hwnd_ && IsWindow(hwnd_))
        KillTimer(hwnd_, kHiddenHintTimerId);
}

void DesktopApp::ShowWidgetAddedHint()
{
    showWidgetAddedHint_ = true;
    widgetAddedHintStartTick_ = GetTickCount();
    if (hwnd_ && IsWindow(hwnd_))
    {
        SetTimer(hwnd_, kWidgetAddedHintTimerId, 100, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void DesktopApp::ClearWidgetAddedHint()
{
    showWidgetAddedHint_ = false;
    widgetAddedHintStartTick_ = 0;
    if (hwnd_ && IsWindow(hwnd_))
        KillTimer(hwnd_, kWidgetAddedHintTimerId);
}

/**
 * @brief 刷新拖拽目标：根据鼠标位置更新目标容器、槽位和区域
 * @param clientPoint 客户端坐标点
 * @param mods 修饰键状态
 */

#include "app.h"

// Settings application, desktop passthrough and retained-surface visibility.

void DesktopApp::ShowSettingsWindow()
{
    settingsWindowOpenRequest_.Request();
    TryShowPendingSettingsWindow();
}

void DesktopApp::TryShowPendingSettingsWindow()
{
    if (!settingsWindowOpenRequest_.Pending() ||
        !startupInitializationComplete_)
        return;

    if (settingsWindow_ && settingsWindow_->Show())
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
    generalSettings_.quickNavTheme = std::clamp(generalSettings_.quickNavTheme, 0, 3);
    SetSoftwareDesktopEnabled(generalSettings_.softwareDesktopEnabled, false);
    ApplyQuickNavigationAppearance();
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
    PersonalizationSettings globalAppearance;
    if (settingsWindow_)
    {
        globalAppearance = settingsWindow_->GetPersonalization();
    }
    else
    {
        globalAppearance = PersonalizationSettings::DarkPreset();
        LoadPersonalization(GetPersonalizationPath().c_str(), globalAppearance);
    }
    constexpr int quickNavPresetIds[] = {
        kAppearancePresetDark, kAppearancePresetLight,
        kAppearancePresetAcrylicDark, kAppearancePresetAcrylicLight
    };
    const int presetId = globalAppearance.backgroundPreset == kAppearancePresetCustom
        ? quickNavPresetIds[std::clamp(generalSettings_.quickNavTheme, 0, 3)]
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
    if (settingsWindow_)
        settingsWindow_->SyncSystemTaskbarSettings(autoHide, centered);
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

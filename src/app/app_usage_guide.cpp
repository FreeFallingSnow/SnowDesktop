#include "app.h"
#include "../modern_menu.h"

void DesktopApp::LoadUsageGuidePreferences()
{
    std::string error;
    if (!snowdesktop::usage_guide::LoadExpanded(
        GetDataFilePath(L"SnowDesktop.guide.json"), usageGuideExpanded_, &error))
        WriteDiagnosticLogEntry((L"Usage guide preference load failed: " + Utf8ToWide(error)).c_str(),
            DiagnosticLogLevel::Warning);
}

snowdesktop::SettingsActionResult DesktopApp::SetUsageGuideExpanded(bool expanded)
{
    std::string error;
    const bool saved = snowdesktop::usage_guide::SaveExpanded(
        GetDataFilePath(L"SnowDesktop.guide.json"), expanded, &error);
    if (saved) usageGuideExpanded_ = expanded;
    else if (!error.empty())
        WriteDiagnosticLogEntry((L"Usage guide preference save failed: " + Utf8ToWide(error)).c_str(),
            DiagnosticLogLevel::Warning);
    PublishHomeAboutStatus(); // On failure, restore the previously saved value in the UI.
    return saved ? snowdesktop::SettingsActionResult::Success() :
        snowdesktop::SettingsActionResult::Failure(_LW("start.error.savePreference"));
}

void DesktopApp::ShowUsageGuideWelcome()
{
    // Dispatch after startup / settings RPC completes, never recursively in IPC.
    if (!startupInitializationComplete_ || settingsWindowOpenRequest_.Pending()) return;
    usageGuideWelcomeQueued_ = false;
    if (!usageGuideWelcomePending_) return;
    ShowSettingsWindow(snowdesktop::SettingsRoute::ForPage(
        snowdesktop::SettingsPage::General, "start.welcome"));
}

std::uint32_t DesktopApp::UsageGuideContext() const
{
    using namespace snowdesktop::usage_guide;
    std::uint32_t context = generalSettings_.dockEnabled ? DockEnabled : 0;
    if (navigationSettings_.enabled && navigationSettings_.virtualKey) context |= NavigationEnabled;
    unsigned standaloneCollections = 0;
    for (const auto& widget : widgets_)
    {
        const bool inDock = widget.gridCell.pageId == kDockPageId || IsDockExclusiveWidgetId(widget.id);
        if (widget.type == DesktopWidgetType::Collection && (!inDock || generalSettings_.dockEnabled))
            context |= CollectionAvailable;
        if (IsGroupedWidget(widget) || inDock || widget.type == DesktopWidgetType::Guide) continue;
        context |= StandaloneWidget;
        if (widget.type == DesktopWidgetType::Collection) ++standaloneCollections;
        if (widget.type == DesktopWidgetType::FileCategories || widget.type == DesktopWidgetType::FolderMapping)
            context |= StandaloneFileSource;
    }
    if (standaloneCollections) context |= StandaloneCollection;
    if (standaloneCollections >= 2) context |= TwoStandaloneCollections;
    return context;
}

snowdesktop::SettingsActionResult DesktopApp::StartUsageGuidePractice(snowdesktop::usage_guide::Topic topic)
{
    using snowdesktop::SettingsActionResult;
    const auto* lesson = snowdesktop::usage_guide::Find(topic);
    if (!lesson) return SettingsActionResult::Failure(_LW("start.error.unavailable"));
    // Settings lessons remain usable while desktop icons are hidden. Only
    // lessons that actually enter desktop practice require an idle desktop.
    if (!startupInitializationComplete_ || reloading_ || exitRequested_ ||
        (lesson->practice && (!desktopItemsReady_ || !customDesktopVisible_ || desktopIconsHidden_ ||
            !luaWidgetPanelRequest_.widgetId.empty() || shellFileOperationInFlight_ > 0 ||
            !pendingRenames_.empty() || dragSession_.HasContext() || dragDropController_.IsTransportActive())))
        return SettingsActionResult::Failure(_LW("start.error.unavailable"));
    if (usageGuidePractice_.active == topic) usageGuidePractice_.Resume(UsageGuideContext());
    else
    {
        if (!usageGuidePractice_.Begin(topic, UsageGuideContext())) return SettingsActionResult::Failure(_LW("start.error.unavailable"));
        usageGuideDetails_ = false; usageGuideDetailPage_ = 0;
    }
    ClearWidgetAddedHint();
    usageGuideWaitingForDesktop_ = true;
    RefreshUsageGuidePractice();
    return SettingsActionResult::Success();
}

void DesktopApp::RefreshUsageGuidePractice()
{
    usageGuidePauseRect_ = usageGuideSettingsRect_ = usageGuideNextRect_ = usageGuideMoreRect_ = usageGuideOpenSettingsRect_ = {};
    usageGuidePressedButton_ = 0;
    PublishHomeAboutStatus();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool DesktopApp::HandleUsageGuidePointerDown(POINT point)
{
    if (!usageGuidePractice_.Visible() || !customDesktopVisible_ || desktopIconsHidden_ || reloading_ ||
        !luaWidgetPanelRequest_.widgetId.empty() || shellPopupMenuLayerDepth_ > 0 ||
        snowdesktop::modern_menu::IsActive() ||
        (settingsWindow_ && IsWindowVisible(settingsWindow_->Window()))) return false;
    usageGuidePressedButton_ = PtInRect(&usageGuidePauseRect_, point) ? 1 :
        PtInRect(&usageGuideSettingsRect_, point) ? 2 :
        PtInRect(&usageGuideNextRect_, point) ? 3 :
        PtInRect(&usageGuideMoreRect_, point) ? 4 :
        PtInRect(&usageGuideOpenSettingsRect_, point) ? 5 : 0;
    if (!usageGuidePressedButton_) return false;
    SetCapture(hwnd_);
    return true;
}

bool DesktopApp::HandleUsageGuidePointerUp(POINT point)
{
    const int pressed = std::exchange(usageGuidePressedButton_, 0);
    if (!pressed) return false;
    if (GetCapture() == hwnd_) ReleaseCapture();
    if (!usageGuidePractice_.Visible()) return true;
    const auto topic = *usageGuidePractice_.active;
    const auto& lesson = *snowdesktop::usage_guide::Find(topic);
    if (pressed == 1 && PtInRect(&usageGuidePauseRect_, point))
    {
        usageGuidePractice_.Pause();
        usageGuideWaitingForDesktop_ = false;
        RefreshUsageGuidePractice();
    }
    else if (pressed == 2 && PtInRect(&usageGuideSettingsRect_, point))
    {
        usageGuidePractice_.Pause();
        RefreshUsageGuidePractice();
        ShowSettingsWindow(snowdesktop::SettingsRoute::ForPage(
            snowdesktop::SettingsPage::General, "start." + std::string(lesson.key)));
    }
    else if (pressed == 3 && PtInRect(&usageGuideNextRect_, point))
    {
        if (usageGuidePractice_.sectionEnd)
        {
            if (!usageGuidePractice_.ContinueSection(UsageGuideContext())) usageGuidePractice_.End();
        }
        else usageGuidePractice_.Advance(UsageGuideContext());
        usageGuideDetails_ = false; usageGuideDetailPage_ = 0;
        RefreshUsageGuidePractice();
    }
    else if (pressed == 4 && PtInRect(&usageGuideMoreRect_, point))
    {
        ClientToScreen(hwnd_, &point); ShowUsageGuideActions(point);
    }
    else if (pressed == 5 && PtInRect(&usageGuideOpenSettingsRect_, point))
    {
        const auto missing = snowdesktop::usage_guide::MissingPrerequisite(lesson, UsageGuideContext());
        usageGuideWaitingForDesktop_ = true;
        ShowSettingsWindow(missing && !snowdesktop::usage_guide::PreparationLesson(*missing) ?
            snowdesktop::usage_guide::PrerequisiteRoute(*missing) :
            snowdesktop::SettingsRoute::ForPage(lesson.settingsPage, lesson.settingsFocus));
    }
    return true;
}

void DesktopApp::ShowUsageGuideActions(POINT point)
{
    using namespace snowdesktop::usage_guide;
    if (!usageGuidePractice_.Visible()) return;
    const auto topic = *usageGuidePractice_.active;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, 1, _LW(usageGuideDetails_ ? "start.hideDetails" : "start.details"));
    if (usageGuideDetailPages_ > 1)
    {
        AppendMenuW(menu, MF_STRING | (usageGuideDetailPage_ ? 0 : MF_GRAYED), 5, _LW("start.previousPage"));
        AppendMenuW(menu, MF_STRING | (usageGuideDetailPage_ + 1 < usageGuideDetailPages_ ? 0 : MF_GRAYED), 6, _LW("start.nextPage"));
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (usageGuidePractice_.sectionEnd || Adjacent(topic, false) ? 0 : MF_GRAYED), 2, _LW("start.previous"));
    AppendMenuW(menu, MF_STRING | (usageGuidePractice_.sectionEnd ? MF_GRAYED : 0), 3, _LW("start.skip"));
    AppendMenuW(menu, MF_STRING, 4, _LW("start.endPractice"));
    const UINT command = ShowModernMenu(menu, point, hwnd_);
    DestroyMenu(menu);
    // The menu runs a nested event loop; initialization can end a session.
    if (!usageGuidePractice_.Visible() || usageGuidePractice_.active != topic) return;
    switch (command)
    {
    case 1: usageGuideDetails_ = !usageGuideDetails_; usageGuideDetailPage_ = 0; break;
    case 2: usageGuidePractice_.Previous(UsageGuideContext()); usageGuideDetails_ = false; usageGuideDetailPage_ = 0; break;
    case 3: usageGuidePractice_.Advance(UsageGuideContext(), true); usageGuideDetails_ = false; usageGuideDetailPage_ = 0; break;
    case 4: usageGuidePractice_.End(); usageGuideWaitingForDesktop_ = false; break;
    case 5: if (usageGuideDetailPage_) --usageGuideDetailPage_; break;
    case 6: if (usageGuideDetailPage_ + 1 < usageGuideDetailPages_) ++usageGuideDetailPage_; break;
    default: break;
    }
    RefreshUsageGuidePractice();
}


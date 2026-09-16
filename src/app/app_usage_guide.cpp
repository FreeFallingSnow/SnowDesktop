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

bool DesktopApp::IsUsageGuideVisible() const
{
    return usageGuideTopic_.has_value() && customDesktopVisible_ && !desktopIconsHidden_ && !reloading_ &&
        luaWidgetPanelRequest_.widgetId.empty() &&
        !(settingsWindow_ && IsWindowVisible(settingsWindow_->Window()));
}

bool DesktopApp::IsPointInUsageGuide(POINT point) const
{
    return IsUsageGuideVisible() && PtInRect(&usageGuideFrame_, point);
}

snowdesktop::SettingsActionResult DesktopApp::StartUsageGuidePractice(snowdesktop::usage_guide::Topic topic)
{
    using snowdesktop::SettingsActionResult;
    const auto* lesson = snowdesktop::usage_guide::Find(topic);
    // Settings-only articles never start a desktop indicator, even through IPC.
    if (!lesson || !lesson->practice || !startupInitializationComplete_ || reloading_ || exitRequested_ ||
        !desktopItemsReady_ || !customDesktopVisible_ || desktopIconsHidden_ ||
        !luaWidgetPanelRequest_.widgetId.empty() || shellFileOperationInFlight_ > 0 ||
        !pendingRenames_.empty() || dragSession_.HasContext() || dragDropController_.IsTransportActive())
        return SettingsActionResult::Failure(_LW("start.error.unavailable"));
    usageGuideTopic_ = topic;
    usageGuideDetails_ = false;
    usageGuideDetailPage_ = 0;
    ClearWidgetAddedHint();
    usageGuideWaitingForDesktop_ = true;
    RefreshUsageGuideReference();
    return SettingsActionResult::Success();
}

void DesktopApp::RefreshUsageGuideReference()
{
    usageGuidePauseRect_ = usageGuideSettingsRect_ = usageGuideMoreRect_ = usageGuideOpenSettingsRect_ = {};
    usageGuideFrame_ = usageGuideDragRect_ = {};
    usageGuidePressedButton_ = 0;
    usageGuidePlacement_.EndDrag();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool DesktopApp::HandleUsageGuidePointerDown(POINT point)
{
    if (!IsPointInUsageGuide(point) || shellPopupMenuLayerDepth_ > 0 ||
        snowdesktop::modern_menu::IsActive()) return false;
    usageGuidePressedButton_ = PtInRect(&usageGuidePauseRect_, point) ? 1 :
        PtInRect(&usageGuideSettingsRect_, point) ? 2 :
        PtInRect(&usageGuideMoreRect_, point) ? 4 :
        PtInRect(&usageGuideOpenSettingsRect_, point) ? 5 :
        PtInRect(&usageGuideDragRect_, point) ? 6 : 7;
    if (usageGuidePressedButton_ == 6)
    {
        ClientToScreen(hwnd_, &point);
        usageGuidePlacement_.BeginDrag(point);
    }
    // The complete panel owns its hit area: reading or dragging it must not
    // click/select the desktop objects underneath.
    SetCapture(hwnd_);
    return true;
}

bool DesktopApp::HandleUsageGuidePointerMove(POINT point)
{
    if (!usageGuidePressedButton_) return false;
    if (usageGuidePlacement_.dragOffset)
    {
        ClientToScreen(hwnd_, &point);
        if (usageGuidePlacement_.DragTo(point)) InvalidateRect(hwnd_, nullptr, FALSE);
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
    }
    return true;
}

bool DesktopApp::HandleUsageGuidePointerUp(POINT point)
{
    const int pressed = std::exchange(usageGuidePressedButton_, 0);
    if (!pressed) return false;
    usageGuidePlacement_.EndDrag();
    if (GetCapture() == hwnd_) ReleaseCapture();
    if (!IsUsageGuideVisible()) return true;
    const auto& lesson = *snowdesktop::usage_guide::Find(*usageGuideTopic_);
    if (pressed == 1 && PtInRect(&usageGuidePauseRect_, point))
    {
        usageGuideTopic_.reset();
        usageGuideWaitingForDesktop_ = false;
        RefreshUsageGuideReference();
    }
    else if (pressed == 2 && PtInRect(&usageGuideSettingsRect_, point))
    {
        const auto route = snowdesktop::SettingsRoute::ForPage(
            snowdesktop::SettingsPage::General, "start." + std::string(lesson.key));
        usageGuideTopic_.reset();
        usageGuideWaitingForDesktop_ = false;
        RefreshUsageGuideReference();
        ShowSettingsWindow(route);
    }
    else if (pressed == 4 && PtInRect(&usageGuideMoreRect_, point))
    {
        ClientToScreen(hwnd_, &point); ShowUsageGuideActions(point);
    }
    else if (pressed == 5 && PtInRect(&usageGuideOpenSettingsRect_, point))
    {
        const auto route = snowdesktop::SettingsRoute::ForPage(lesson.settingsPage, lesson.settingsFocus);
        usageGuideTopic_.reset();
        usageGuideWaitingForDesktop_ = false;
        RefreshUsageGuideReference();
        ShowSettingsWindow(route);
    }
    return true;
}

void DesktopApp::ShowUsageGuideActions(POINT point)
{
    if (!IsUsageGuideVisible()) return;
    const auto topic = usageGuideTopic_;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, 1, _LW(usageGuideDetails_ ? "start.hideDetails" : "start.details"));
    if (usageGuideDetailPages_ > 1)
    {
        AppendMenuW(menu, MF_STRING | (usageGuideDetailPage_ ? 0 : MF_GRAYED), 5, _LW("start.previousPage"));
        AppendMenuW(menu, MF_STRING | (usageGuideDetailPage_ + 1 < usageGuideDetailPages_ ? 0 : MF_GRAYED), 6, _LW("start.nextPage"));
    }
    const UINT command = ShowModernMenu(menu, point, hwnd_);
    DestroyMenu(menu);
    // A nested menu loop can close or replace the indicator during reload.
    if (!IsUsageGuideVisible() || usageGuideTopic_ != topic) return;
    switch (command)
    {
    case 1: usageGuideDetails_ = !usageGuideDetails_; usageGuideDetailPage_ = 0; break;
    case 5: if (usageGuideDetailPage_) --usageGuideDetailPage_; break;
    case 6: if (usageGuideDetailPage_ + 1 < usageGuideDetailPages_) ++usageGuideDetailPage_; break;
    default: return; // Cancelling a menu changes neither content nor placement.
    }
    RefreshUsageGuideReference();
}


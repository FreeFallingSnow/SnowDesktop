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
    if (!lesson || !snowdesktop::usage_guide::CanShowDesktop(*lesson, generalSettings_.dockEnabled) || !startupInitializationComplete_ || reloading_ || exitRequested_ ||
        !desktopItemsReady_ || !customDesktopVisible_ || desktopIconsHidden_ ||
        !luaWidgetPanelRequest_.widgetId.empty() || shellFileOperationInFlight_ > 0 ||
        !pendingRenames_.empty() || dragSession_.HasContext() || dragDropController_.IsTransportActive())
        return SettingsActionResult::Failure(_LW("start.error.unavailable"));
    usageGuideTopic_ = topic;
    usageGuideScroll_ = {};
    ClearWidgetAddedHint();
    usageGuideWaitingForDesktop_ = true;
    RefreshUsageGuideReference();
    return SettingsActionResult::Success();
}

void DesktopApp::RefreshUsageGuideReference()
{
    usageGuidePauseRect_ = usageGuideSettingsRect_ = usageGuideOpenSettingsRect_ = {};
    usageGuideFrame_ = usageGuideDragRect_ = usageGuideBodyRect_ = usageGuideScrollTrack_ = usageGuideScrollThumb_ = {};
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
        PtInRect(&usageGuideScrollTrack_, point) ? 8 :
        PtInRect(&usageGuideOpenSettingsRect_, point) ? 5 :
        PtInRect(&usageGuideDragRect_, point) ? 6 : 7;
    if (usageGuidePressedButton_ == 8)
    {
        const int thumbHeight = usageGuideScrollThumb_.bottom - usageGuideScrollThumb_.top;
        usageGuideScrollGrab_ = PtInRect(&usageGuideScrollThumb_, point) ? point.y - usageGuideScrollThumb_.top : thumbHeight / 2;
        HandleUsageGuidePointerMove(point);
    }
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
    if (usageGuidePressedButton_ == 8)
    {
        const int travel = usageGuideScrollTrack_.bottom - usageGuideScrollTrack_.top -
            (usageGuideScrollThumb_.bottom - usageGuideScrollThumb_.top);
        if (travel > 0) usageGuideScroll_.ToFraction(
            static_cast<double>(point.y - usageGuideScrollTrack_.top - usageGuideScrollGrab_) / travel);
        InvalidateRect(hwnd_, &usageGuideFrame_, FALSE);
    }
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

void DesktopApp::ScrollUsageGuide(int delta)
{
    if (!IsUsageGuideVisible() || usageGuidePressedButton_) return;
    usageGuideScroll_.By(-MulDiv(delta, 48, WHEEL_DELTA));
    InvalidateRect(hwnd_, &usageGuideFrame_, FALSE);
}

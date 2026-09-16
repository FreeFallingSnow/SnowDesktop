#include "app.h"

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
        if (widget.type == DesktopWidgetType::Collection) context |= CollectionAvailable;
        if (IsGroupedWidget(widget) || widget.gridCell.pageId == kDockPageId || widget.type == DesktopWidgetType::Guide) continue;
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
    if (!startupInitializationComplete_ || !desktopItemsReady_ ||
        !customDesktopVisible_ || desktopIconsHidden_ || reloading_ || exitRequested_ ||
        !luaWidgetPanelRequest_.widgetId.empty() ||
        shellFileOperationInFlight_ > 0 || !pendingRenames_.empty() ||
        dragSession_.HasContext() || dragDropController_.IsTransportActive())
        return SettingsActionResult::Failure(_LW("start.error.unavailable"));
    const auto* lesson = snowdesktop::usage_guide::Find(topic);
    if (!lesson) return SettingsActionResult::Failure(_LW("start.error.unavailable"));
    const auto context = UsageGuideContext();
    if (const auto missing = snowdesktop::usage_guide::MissingPrerequisite(*lesson, context))
        return SettingsActionResult::Failure(_LW(snowdesktop::usage_guide::PrerequisiteText(*missing)));
    if (!usageGuidePractice_.Begin(topic, context))
        return SettingsActionResult::Failure(_LW("start.error.unavailable"));
    ClearWidgetAddedHint();
    usageGuideWaitingForDesktop_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
    return SettingsActionResult::Success();
}

bool DesktopApp::HandleUsageGuidePointerDown(POINT point)
{
    if (!usageGuidePractice_.active || !customDesktopVisible_ || desktopIconsHidden_ ||
        !luaWidgetPanelRequest_.widgetId.empty()) return false;
    usageGuidePressedButton_ = PtInRect(&usageGuidePauseRect_, point) ? 1 :
        PtInRect(&usageGuideSettingsRect_, point) ? 2 : 0;
    if (!usageGuidePressedButton_) return false;
    SetCapture(hwnd_);
    return true;
}

bool DesktopApp::HandleUsageGuidePointerUp(POINT point)
{
    const int pressed = std::exchange(usageGuidePressedButton_, 0);
    if (!pressed) return false;
    if (GetCapture() == hwnd_) ReleaseCapture();
    if ((pressed == 1 && PtInRect(&usageGuidePauseRect_, point)) ||
        (pressed == 2 && PtInRect(&usageGuideSettingsRect_, point)))
    {
        const auto previous = usageGuidePractice_.End();
        usageGuideWaitingForDesktop_ = false;
        usageGuidePauseRect_ = usageGuideSettingsRect_ = {};
        InvalidateRect(hwnd_, nullptr, FALSE);
        if (pressed == 2 && previous)
        {
            const auto* lesson = snowdesktop::usage_guide::Find(*previous);
            ShowSettingsWindow(snowdesktop::SettingsRoute::ForPage(
                snowdesktop::SettingsPage::General, lesson ?
                    "start." + std::string(lesson->key) : "start.basics"));
        }
    }
    return true;
}


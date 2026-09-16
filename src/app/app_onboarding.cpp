#include "app.h"

void DesktopApp::LoadOnboarding()
{
    std::string error;
    onboardingWritable_ = snowdesktop::onboarding::Load(
        GetDataFilePath(L"SnowDesktop.onboarding.json"), onboarding_.regular, &error);
    if (!onboardingWritable_)
        WriteDiagnosticLogEntry((L"Onboarding state load failed: " + Utf8ToWide(error)).c_str(),
            DiagnosticLogLevel::Warning);
}

void DesktopApp::SaveOnboarding()
{
    onboardingPractice_.Advance(onboarding_.Current());
    if (!onboarding_.experiment && onboardingWritable_)
    {
        std::string error;
        if (!snowdesktop::onboarding::Save(GetDataFilePath(L"SnowDesktop.onboarding.json"),
                onboarding_.regular, &error))
            WriteDiagnosticLogEntry((L"Onboarding state save failed: " + Utf8ToWide(error)).c_str(),
                DiagnosticLogLevel::Warning);
    }
    PublishHomeAboutStatus();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void DesktopApp::ShowOnboardingWelcome()
{
    // Dispatch after startup / settings RPC completes, never recursively in IPC.
    if (!startupInitializationComplete_ || settingsWindowOpenRequest_.Pending()) return;
    onboardingWelcomeQueued_ = false;
    if (!onboarding_.Current().Visible() || !onboarding_.Current().welcomePending) return;
    ShowSettingsWindow(snowdesktop::SettingsRoute::ForPage(
        snowdesktop::SettingsPage::General, "start.basics"));
}

bool DesktopApp::HasOnboardingCollection() const
{
    const auto index = FindWidgetIndexById(Utf8ToWide(onboarding_.Current().collectionId));
    return index < widgets_.size() && widgets_[index].type == DesktopWidgetType::Collection &&
        !IsGroupedWidget(widgets_[index]) && widgets_[index].gridCell.pageId != kDockPageId;
}

void DesktopApp::RecordOnboardingWidgetCreated(const DesktopWidget& widget)
{
    auto& state = onboarding_.Current();
    bool changed = false;
    if (widget.type == DesktopWidgetType::Collection)
        changed = state.CollectionCreated(WideToUtf8(widget.id),
            HasOnboardingCollection() && onboardingPractice_.active != snowdesktop::onboarding::Task::Collection,
            onboardingMenuCreation_);
    else if (widget.type == DesktopWidgetType::FileCategories)
        changed = state.FilesCreated(onboardingMenuCreation_);
    if (changed) SaveOnboarding();
}

void DesktopApp::RecordOnboardingApplicationDrop(const std::wstring& collectionId,
    const std::wstring& key, bool newlyInserted)
{
    const auto index = FindItemIndexByKey(key);
    if (index >= items_.size() || !HasOnboardingCollection()) return;
    const auto& item = items_[index];
    const bool application = item.isApplicationShortcut ||
        _wcsicmp(PathFindExtensionW(item.parsingName.c_str()), L".exe") == 0;
    if (onboarding_.Current().ApplicationDropped(WideToUtf8(collectionId),
            application, newlyInserted)) SaveOnboarding();
}

snowdesktop::SettingsActionResult DesktopApp::StartOnboardingTask(snowdesktop::onboarding::Task task)
{
    using snowdesktop::SettingsActionResult;
    if (!startupInitializationComplete_ || !desktopItemsReady_ ||
        !customDesktopVisible_ || reloading_ || exitRequested_ ||
        shellFileOperationInFlight_ > 0 || !pendingRenames_.empty() ||
        dragSession_.HasContext() || dragDropController_.IsTransportActive() ||
        !onboardingPractice_.Begin(onboarding_.Current(), task, HasOnboardingCollection()))
        return SettingsActionResult::Failure(_LW("start.error.unavailable"));
    ClearWidgetAddedHint();
    onboardingPracticeWaitingForDesktop_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
    return SettingsActionResult::Success();
}

bool DesktopApp::HandleOnboardingPointerDown(POINT point)
{
    if (!onboardingPractice_.active || !onboarding_.Current().Visible() ||
        !customDesktopVisible_ || desktopIconsHidden_ || !luaWidgetPanelRequest_.widgetId.empty()) return false;
    onboardingPressedButton_ = PtInRect(&onboardingPauseRect_, point) ? 1 :
        PtInRect(&onboardingSettingsRect_, point) ? 2 : 0;
    if (!onboardingPressedButton_) return false;
    SetCapture(hwnd_);
    return true;
}

bool DesktopApp::HandleOnboardingPointerUp(POINT point)
{
    const int pressed = std::exchange(onboardingPressedButton_, 0);
    if (!pressed) return false;
    if (GetCapture() == hwnd_) ReleaseCapture();
    if ((pressed == 1 && PtInRect(&onboardingPauseRect_, point)) ||
        (pressed == 2 && PtInRect(&onboardingSettingsRect_, point)))
    {
        onboardingPractice_.Pause();
        onboardingPauseRect_ = onboardingSettingsRect_ = {};
        InvalidateRect(hwnd_, nullptr, FALSE);
        if (pressed == 2) ShowSettingsWindow(snowdesktop::SettingsRoute::ForPage(
            snowdesktop::SettingsPage::General, "start.basics"));
    }
    return true;
}

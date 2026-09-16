#include "app.h"
#include "../layout_storage.h"

void DesktopApp::LoadOnboarding()
{
    // Inspect durable user data before layout initialization can create it.
    // A cleared or invalid existing layout is still an existing user.
    std::error_code ec;
    const std::filesystem::path layout = GetDataFilePath(L"SnowDesktop.layout.json");
    bool existingUser = std::filesystem::exists(layout, ec);
    existingUser |= static_cast<bool>(ec);
    existingUser |= std::filesystem::exists(snowdesktop::layout_storage::BackupPath(layout), ec);
    existingUser |= static_cast<bool>(ec);
    existingUser |= std::filesystem::exists(GetDataFilePath(L"SnowDesktop.general.json"), ec);
    existingUser |= static_cast<bool>(ec);
    std::string error;
    onboardingWritable_ = snowdesktop::onboarding::Load(
        GetDataFilePath(L"SnowDesktop.onboarding.json"), existingUser,
        onboarding_.regular, &error);
    if (!onboardingWritable_)
        WriteDiagnosticLogEntry((L"Onboarding state load failed: " + Utf8ToWide(error)).c_str(),
            DiagnosticLogLevel::Warning);
}

void DesktopApp::SaveOnboarding()
{
    if (!onboarding_.experiment && onboardingWritable_)
    {
        std::string error;
        if (!snowdesktop::onboarding::Save(GetDataFilePath(L"SnowDesktop.onboarding.json"),
                onboarding_.regular, &error))
            WriteDiagnosticLogEntry((L"Onboarding state save failed: " + Utf8ToWide(error)).c_str(),
                DiagnosticLogLevel::Warning);
    }
    PublishHomeAboutStatus();
}

void DesktopApp::ShowOnboardingWelcome()
{
    // Called after startup presentation or from the outer pump after the Debug
    // IPC command returns, never recursively inside its settings RPC.
    if (!startupInitializationComplete_) return;
    onboardingWelcomeQueued_ = false;
    if (!onboarding_.Current().welcomePending || settingsWindowOpenRequest_.Pending()) return;
    ShowSettingsWindow(snowdesktop::SettingsRoute::ForPage(
        snowdesktop::SettingsPage::Home, "start.basics"));
}

void DesktopApp::RecordOnboardingWidgetCreated(const DesktopWidget& widget)
{
    auto& state = onboarding_.Current();
    bool changed = false;
    if (widget.type == DesktopWidgetType::Collection)
    {
        const auto previous = FindWidgetIndexById(Utf8ToWide(state.collectionId));
        changed = state.CollectionCreated(WideToUtf8(widget.id),
            previous < widgets_.size() &&
                widgets_[previous].type == DesktopWidgetType::Collection &&
                !IsGroupedWidget(widgets_[previous]) &&
                widgets_[previous].gridCell.pageId != kDockPageId);
    }
    else if (widget.type == DesktopWidgetType::FileCategories)
        changed = state.Record(snowdesktop::onboarding::kFiles);
    if (changed) SaveOnboarding();
}

void DesktopApp::RecordOnboardingApplicationDrop(const std::wstring& collectionId,
    const std::wstring& key, bool newlyInserted)
{
    const auto index = FindItemIndexByKey(key);
    if (index >= items_.size()) return;
    const auto& item = items_[index];
    const bool application = item.isApplicationShortcut ||
        _wcsicmp(PathFindExtensionW(item.parsingName.c_str()), L".exe") == 0;
    if (onboarding_.Current().ApplicationDropped(WideToUtf8(collectionId),
            application, newlyInserted)) SaveOnboarding();
}

snowdesktop::SettingsActionResult DesktopApp::StartOnboardingTask(
    snowdesktop::onboarding::Task task, bool defer)
{
    using snowdesktop::onboarding::Task;
    using snowdesktop::SettingsActionResult;
    auto& state = onboarding_.Current();
    if (defer)
    {
        if (state.Defer(task)) SaveOnboarding();
        return SettingsActionResult::Success();
    }
    if (!startupInitializationComplete_ || !desktopItemsReady_ ||
        !customDesktopVisible_ || reloading_ || exitRequested_ ||
        shellFileOperationInFlight_ > 0 || !pendingRenames_.empty() ||
        dragSession_.HasContext() || dragDropController_.IsTransportActive())
        return SettingsActionResult::Failure(_LW("start.error.unavailable"));

    const auto showWidget = [this](std::size_t index) {
        const auto id = widgets_[index].id;
        const auto pageId = widgets_[index].gridCell.pageId;
        const auto page = std::find(savedPageIds_.begin(), savedPageIds_.end(), pageId);
        if (page != savedPageIds_.end() && !FindGridPage(gridPages_, pageId))
        {
            const int pageIndex = static_cast<int>(page - savedPageIds_.begin());
            const int fixedPages = std::max(0, static_cast<int>(gridPages_.size()) - 1);
            if (pageIndex >= fixedPages) JumpToPageOffset(pageIndex - fixedPages);
        }
        SelectWidgetOnly(FindWidgetIndexById(id));
    };
    if (task == Task::Application || task == Task::Layout)
    {
        const auto index = FindWidgetIndexById(Utf8ToWide(state.collectionId));
        if (index >= widgets_.size() || widgets_[index].type != DesktopWidgetType::Collection ||
            IsGroupedWidget(widgets_[index]) || widgets_[index].gridCell.pageId == kDockPageId)
            return SettingsActionResult::Failure(_LW("start.error.collectionMissing"));
        showWidget(index);
        onboardingHintKey_ = task == Task::Application
            ? "start.application.hint" : "app.overlay.widget_move_hint";
        ShowWidgetAddedHint();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    else
    {
        POINT anchor{};
        GetCursorPos(&anchor);
        RECT settingsRect{};
        if (settingsWindow_ && GetWindowRect(settingsWindow_->Window(), &settingsRect))
            anchor = {(settingsRect.left + settingsRect.right) / 2,
                (settingsRect.top + settingsRect.bottom) / 2};
        std::unordered_set<std::wstring> previousIds;
        for (const auto& widget : widgets_) previousIds.insert(widget.id);
        if (task == Task::Collection) AddCollectionWidgetAt(anchor);
        else AddFileCategoryWidgetAt(anchor);
        const auto created = std::find_if(widgets_.begin(), widgets_.end(),
            [&](const DesktopWidget& widget) { return !previousIds.contains(widget.id) &&
                widget.type == (task == Task::Collection ? DesktopWidgetType::Collection
                    : DesktopWidgetType::FileCategories); });
        if (created == widgets_.end())
            return SettingsActionResult::Failure(_LW("start.error.noSpace"));
        // An explicit Add in the guide chooses the new practice collection.
        if (task == Task::Collection &&
            state.CollectionCreated(WideToUtf8(created->id), false)) SaveOnboarding();
        showWidget(static_cast<std::size_t>(created - widgets_.begin()));
        onboardingHintKey_ = task == Task::Collection
            ? "start.application.hint" : "start.files.hint";
        ShowWidgetAddedHint();
    }
    if (state.Resume(task)) SaveOnboarding();
    return SettingsActionResult::Success();
}

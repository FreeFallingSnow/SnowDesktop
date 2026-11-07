#include "ui/input/private_pointer.h"
#include "app/app.h"
#include "settings/settings_search_index.h"
#include "common/constants.h"
#include "app/navigation/quick_navigation_state.h"
#include "navigation/navigation_settings.h"
#include "common/l10n.h"
#include "resources/resource.h"
#include "settings/animation_settings.h"
#include "common/utils.h"
#include "ui/input/text_input_window.h"
#include "settings/settings_controller.h"
#include "quick_navigation_theme.h"
#include "quick_navigation_helpers.h"
#include "navigation/quick_navigation_query.h"
#include "navigation/quick_navigation_rules.h"
#include "platform/operation_feedback.h"
#include "shell/shell_launch_process.h"
#include "ui/menu/menu_fluent_glyphs.h"
#include <wincodec.h>
#include <shobjidl.h>

namespace
{
constexpr const char* kScopeKeys[] = {"quickNav.type.all", "quickNav.type.app", "quickNav.type.file",
    "quickNav.type.web", "quickNav.type.settings", "quickNav.type.run", "quickNav.type.calculator"};
constexpr const wchar_t* kScopeGlyphs[] = {L"\uF690", L"\uF123", L"\uF419", L"\uF45B", L"\uF6AA", L"\uF583", L"\uE233"};
std::wstring ScopeLabel(QuickNavigationSearchType type)
{
    return _LW(kScopeKeys[static_cast<size_t>(type)]);
}

std::optional<std::wstring> PickQuickNavigationFavoritePath(
    HWND owner, bool pickFolders)
{
    ComPtr<IFileOpenDialog> dialog;
    if (!owner || !IsWindow(owner) ||
        FAILED(CoCreateInstance(
            CLSID_FileOpenDialog, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) ||
        !dialog)
        return std::nullopt;

    DWORD options = 0;
    if (FAILED(dialog->GetOptions(&options)))
        return std::nullopt;
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    options |= pickFolders ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST;
    if (FAILED(dialog->SetOptions(options)))
        return std::nullopt;
    (void)dialog->SetTitle(_LW(pickFolders
        ? "app.nav.favorite_folder_picker"
        : "app.nav.favorite_file_picker"));

    const HRESULT shown =
        snowdesktop::operation_feedback::ShowModalWindow(
            dialog.Get(), owner);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return std::nullopt;
    if (FAILED(shown))
        return std::nullopt;

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item)) || !item)
        return std::nullopt;
    PWSTR selected = nullptr;
    if (FAILED(item->GetDisplayName(
            SIGDN_FILESYSPATH, &selected)) ||
        !selected)
    {
        if (selected)
            CoTaskMemFree(selected);
        return std::nullopt;
    }
    std::wstring path(selected);
    CoTaskMemFree(selected);
    return path.empty()
        ? std::nullopt
        : std::optional<std::wstring>(std::move(path));
}
}

bool DesktopApp::UseQuickNavigationList() const
{
    return !IsLuaLogicalSlotPickerOpen() && (quickNavigation_->collapsed || quickNavigation_->searchType != QuickNavigationSearchType::All ||
        !snowdesktop::quick_navigation_query::PrefixCandidates(navigationSettings_, GetQuickNavigationEffectiveSearchText()).empty());
}

QuickNavigationFavorite DesktopApp::CreateQuickNavigationFavoriteFromPath(
    const std::wstring& path, const std::wstring& displayName) const
{
    QuickNavigationFavorite favorite;
    if (path.empty())
        return favorite;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    favorite.type = attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
        ? QuickNavigationFavoriteType::Folder
        : QuickNavigationFavoriteType::File;
    favorite.target = WideToUtf8(path);
    favorite.name = WideToUtf8(
        displayName.empty()
            ? FileNameFromPath(path)
            : displayName);
    return favorite;
}

QuickNavigationFavorite
DesktopApp::CreateQuickNavigationFavoriteFromDesktopItem(
    size_t itemIndex) const
{
    QuickNavigationFavorite favorite;
    if (itemIndex >= items_.size())
        return favorite;
    const DesktopItem& item = items_[itemIndex];
    favorite.type = QuickNavigationFavoriteType::DesktopItem;
    favorite.target = WideToUtf8(
        item.parsingName.empty()
            ? item.layoutKey
            : item.parsingName);
    favorite.desktopKey = WideToUtf8(item.layoutKey);
    favorite.name = WideToUtf8(item.name);
    return favorite;
}

QuickNavigationFavorite
DesktopApp::CreateQuickNavigationFavoriteFromApp(
    const QuickNavigationAppEntry& entry) const
{
    QuickNavigationFavorite favorite;
    favorite.type = QuickNavigationFavoriteType::Application;
    favorite.target = WideToUtf8(entry.parsingName);
    favorite.name = WideToUtf8(entry.name);
    return favorite;
}

bool DesktopApp::IsQuickNavigationFavorite(
    const std::wstring& target) const
{
    if (target.empty())
        return false;
    const std::wstring normalized =
        ToUpperInvariant(target);
    return std::any_of(
        navigationSettings_.favorites.begin(),
        navigationSettings_.favorites.end(),
        [&](const QuickNavigationFavorite& favorite) {
            return ToUpperInvariant(
                Utf8ToWide(favorite.target)) == normalized;
        });
}

bool DesktopApp::AddQuickNavigationFavorite(
    QuickNavigationFavorite favorite)
{
    if (favorite.target.empty() ||
        favorite.name.empty() ||
        navigationSettings_.favorites.size() >= 200)
        return false;
    if (IsQuickNavigationFavorite(
            Utf8ToWide(favorite.target)))
        return true;
    navigationSettings_.favorites.push_back(
        std::move(favorite));
    NotifyQuickNavigationFavoritesChanged();
    return true;
}

bool DesktopApp::ToggleQuickNavigationFavorite(
    QuickNavigationFavorite favorite)
{
    if (favorite.target.empty())
        return false;
    const std::wstring normalized =
        ToUpperInvariant(Utf8ToWide(favorite.target));
    const auto it = std::find_if(
        navigationSettings_.favorites.begin(),
        navigationSettings_.favorites.end(),
        [&](const QuickNavigationFavorite& current) {
            return ToUpperInvariant(
                Utf8ToWide(current.target)) == normalized;
        });
    if (it != navigationSettings_.favorites.end())
    {
        navigationSettings_.favorites.erase(it);
        NotifyQuickNavigationFavoritesChanged();
        return true;
    }
    return AddQuickNavigationFavorite(std::move(favorite));
}

void DesktopApp::AppendQuickNavigationFavoriteMenuItem(
    HMENU menu, const QuickNavigationFavorite& favorite)
{
    if (!navigationSettings_.favoritesEnabled ||
        !menu || favorite.target.empty())
        return;
    const bool exists = IsQuickNavigationFavorite(
        Utf8ToWide(favorite.target));
    AppendMenuW(
        menu, MF_STRING,
        kContextFavoriteQuickNavigationCommand,
        exists
            ? _LW("app.menu.unfavorite_quick_navigation")
            : _LW("app.menu.favorite_quick_navigation"));
    SetMenuItemIcon(
        menu, kContextFavoriteQuickNavigationCommand,
        snowdesktop::menu_fluent_glyphs::kPin,
        MenuIconFont::FluentRegular);
}

void DesktopApp::NotifyQuickNavigationFavoritesChanged()
{
    SaveNavigationSettings(
        GetNavigationSettingsPath().c_str(),
        navigationSettings_);
    if (settingsController_)
        (void)settingsController_->SynchronizeNavigation(
            navigationSettings_);
    if (!quickNavigation_->open ||
        !quickNavigation_->hwnd ||
        !IsWindow(quickNavigation_->hwnd))
        return;
    PositionQuickNavigationWindow();
    InvalidateQuickNavigationWindow();
}

void DesktopApp::ShowQuickNavigationFavoriteAddMenu(
    POINT screenPoint)
{
    const HWND owner =
        quickNavigation_->hwnd && IsWindow(quickNavigation_->hwnd)
        ? quickNavigation_->hwnd : hwnd_;
    if (!owner || !IsWindow(owner))
        return;
    PrepareMenuIconsForPoint(screenPoint);
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;
    AppendMenuW(
        menu, MF_STRING,
        kQuickNavigationFavoriteFileCommand,
        _LW("app.nav.favorite_file"));
    AppendMenuW(
        menu, MF_STRING,
        kQuickNavigationFavoriteFolderCommand,
        _LW("app.nav.favorite_folder"));
    SetMenuItemIcon(
        menu, kQuickNavigationFavoriteFileCommand,
        L"\uF10C", MenuIconFont::FluentRegular);
    SetMenuItemIcon(
        menu, kQuickNavigationFavoriteFolderCommand,
        snowdesktop::menu_fluent_glyphs::kDesktopFiles,
        MenuIconFont::FluentRegular);
    snowdesktop::private_pointer::SetInputForeground(owner);
    const UINT command = ShowModernMenu(
        menu, screenPoint, owner);
    DestroyMenu(menu);
    ClearMenuIcons();
    RestoreDesktopWindowLayer();
    if (command == kQuickNavigationFavoriteFileCommand)
        ShowQuickNavigationFavoritePicker(false);
    else if (command == kQuickNavigationFavoriteFolderCommand)
        ShowQuickNavigationFavoritePicker(true);
}

void DesktopApp::ShowQuickNavigationFavoritePicker(bool pickFolders)
{
    const HWND owner =
        quickNavigation_->hwnd && IsWindow(quickNavigation_->hwnd)
        ? quickNavigation_->hwnd : hwnd_;
    if (!owner || !IsWindow(owner))
        return;

    const bool previousDialogActive =
        quickNavigation_->externalDialogActive;
    quickNavigation_->externalDialogActive = true;
    const auto selected = PickQuickNavigationFavoritePath(
        owner, pickFolders);
    quickNavigation_->externalDialogActive = previousDialogActive;

    if (selected)
    {
        QuickNavigationFavorite favorite;
        favorite.type = pickFolders
            ? QuickNavigationFavoriteType::Folder
            : QuickNavigationFavoriteType::File;
        favorite.target = WideToUtf8(*selected);
        favorite.name = WideToUtf8(
            FileNameFromPath(*selected));
        if (favorite.name.empty())
            favorite.name = favorite.target;
        if (!AddQuickNavigationFavorite(
                std::move(favorite)))
            MessageBeep(MB_ICONWARNING);
    }

    if (!quickNavigation_->open)
        return;
    (void)snowdesktop::private_pointer::SetInputForeground(
        owner);
    if (quickNavigation_->searchEdit &&
        IsWindow(quickNavigation_->searchEdit))
        (void)FocusKeyboardWindow(
            quickNavigation_->searchEdit, true,
            L"Quick navigation favorite picker", true);
    else
        (void)FocusKeyboardWindow(
            owner, true,
            L"Quick navigation favorite picker", true);
    InvalidateQuickNavigationWindow();
}

int DesktopApp::QuickNavigationGridCellWidth() const
{
    return std::max(96, navigationSettings_.layout.iconSize + navigationSettings_.layout.gridGap * 2);
}
int DesktopApp::QuickNavigationGridCellHeight() const
{
    const auto& layout = navigationSettings_.layout;
    // Fixed trailing space keeps the next row outside the opening viewport
    // with the shared defaults of all four quick-navigation themes.
    return layout.iconSize + 12 + (layout.fontSize + 4) * layout.labelLines + 16;
}
RECT DesktopApp::GetQuickNavigationToolbarRect(const RECT& overlay, int button) const
{
    const RECT search = GetQuickNavigationSearchRect(overlay);
    const int height = std::min(QuickNavScale(32), static_cast<int>(search.bottom - search.top) - QuickNavScale(8));
    const int top = search.top + (static_cast<int>(search.bottom - search.top) - height) / 2;
    if (button == 0)
        return MakeRect(search.left + QuickNavScale(8), top, search.left + QuickNavScale(quickNavigation_->searchType == QuickNavigationSearchType::All ? 40 : 68), top + height);
    const int right = search.right - QuickNavScale(button == 2 ? 8 : 46);
    return MakeRect(right - height, top, right, top + height);
}
RECT DesktopApp::GetQuickNavigationInputRect(const RECT& overlay) const
{
    const RECT search = GetQuickNavigationSearchRect(overlay);
    const RECT type = GetQuickNavigationToolbarRect(overlay, 0);
    const RECT settings = GetQuickNavigationToolbarRect(overlay, 1);
    return MakeRect(type.right + QuickNavScale(2),
        search.top, settings.left - QuickNavScale(8), search.bottom);
}
std::wstring DesktopApp::QuickNavigationTypeLabel() const
{
    if (quickNavigation_->searchType == QuickNavigationSearchType::Web && !quickNavigation_->searchEngine.empty())
        for (const auto& engine : navigationSettings_.engines)
            if (engine.id == quickNavigation_->searchEngine) return Utf8ToWide(engine.name);
    return ScopeLabel(quickNavigation_->searchType);
}
std::wstring DesktopApp::QuickNavigationViewLabel() const
{
    constexpr const char* keys[] = {"app.nav.view_tile", "app.nav.view_source", "app.nav.view_initial", "app.nav.view_frequency"};
    return _LW(keys[static_cast<size_t>(GetQuickNavigationViewMode())]);
}
QuickNavigationDesktopViewMode DesktopApp::GetQuickNavigationViewMode() const
{
    if (quickNavigation_->activeWidgetIndex == static_cast<size_t>(-1))
        return navigationSettings_.desktopViewMode;
    if (quickNavigation_->activeWidgetIndex == static_cast<size_t>(-2))
        return navigationSettings_.mappingViewMode;
    if (quickNavigation_->activeWidgetIndex < widgets_.size())
    {
        const auto it = navigationSettings_.widgetSortByInitial.find(
            WideToUtf8(widgets_[quickNavigation_->activeWidgetIndex].id));
        if (it != navigationSettings_.widgetSortByInitial.end() && it->second)
            return QuickNavigationDesktopViewMode::Initial;
    }
    return QuickNavigationDesktopViewMode::Tile;
}
std::vector<QuickNavigationDesktopViewMode> DesktopApp::GetQuickNavigationViewModeOptions() const
{
    if (quickNavigation_->activeWidgetIndex < widgets_.size())
        return {QuickNavigationDesktopViewMode::Tile, QuickNavigationDesktopViewMode::Initial};
    if (quickNavigation_->activeWidgetIndex == static_cast<size_t>(-1))
        return {QuickNavigationDesktopViewMode::Tile, QuickNavigationDesktopViewMode::Source,
            QuickNavigationDesktopViewMode::Initial, QuickNavigationDesktopViewMode::Frequency};
    return {QuickNavigationDesktopViewMode::Tile, QuickNavigationDesktopViewMode::Source,
        QuickNavigationDesktopViewMode::Initial};
}
void DesktopApp::ToggleQuickNavigationCollapsed()
{
    if (IsLuaLogicalSlotPickerOpen()) return;
    quickNavigation_->collapsed = !quickNavigation_->collapsed;
    navigationSettings_.lastCollapsed = quickNavigation_->collapsed;
    SaveNavigationSettings(GetNavigationSettingsPath().c_str(), navigationSettings_);
    if (settingsController_)
        (void)settingsController_->SynchronizeNavigation(navigationSettings_);
    quickNavigation_->initialJumpOpen = false;
    quickNavigation_->menu = QuickNavigationMenu::None;
    quickNavigation_->scrollOffset = 0;
    const auto rows = BuildQuickNavigationListRows();
    if (quickNavigation_->collapsed)
    {
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const auto& row = rows[i];
            if ((row.kind == QuickNavigationListRow::Kind::Item && IsQuickNavigationKeyboardTarget(QuickNavigationKeyboardTargetKind::Item, row.index)) ||
                (row.kind == QuickNavigationListRow::Kind::App && IsQuickNavigationKeyboardTarget(QuickNavigationKeyboardTargetKind::App, row.index)) ||
                (row.kind == QuickNavigationListRow::Kind::Everything && IsQuickNavigationKeyboardTarget(QuickNavigationKeyboardTargetKind::Everything, row.index)))
                quickNavigation_->listSelection = static_cast<int>(i);
        }
    }
    else if (quickNavigation_->listSelection >= 0 && static_cast<size_t>(quickNavigation_->listSelection) < rows.size())
    {
        const auto& row = rows[static_cast<size_t>(quickNavigation_->listSelection)];
        quickNavigation_->keyboardTargetIndex = row.index;
        quickNavigation_->keyboardTargetKind = row.kind == QuickNavigationListRow::Kind::Item ? QuickNavigationKeyboardTargetKind::Item :
            row.kind == QuickNavigationListRow::Kind::App ? QuickNavigationKeyboardTargetKind::App :
            row.kind == QuickNavigationListRow::Kind::Everything ? QuickNavigationKeyboardTargetKind::Everything : QuickNavigationKeyboardTargetKind::None;
    }
    const auto now = static_cast<std::uint64_t>(
        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds());
    quickNavigation_->animation.Advance(now);
    StopQuickNavigationAnimationTimeline();
    // A Dock warp's source crops belong to its opening geometry. Finish that
    // opening before beginning an independently interactive size transition.
    if (quickNavigation_->animation.IsOpening())
        quickNavigation_->animation.ShowImmediately();
    quickNavigation_->fixedTop = false;
    const RECT target = GetQuickNavigationRect();
    quickNavigation_->anchorTop = target.top;
    quickNavigation_->fixedTop = true;
    quickNavigation_->animation.Resize().Start(
        {quickNavigation_->rect.left, quickNavigation_->rect.top,
            quickNavigation_->rect.right, quickNavigation_->rect.bottom},
        {target.left, target.top, target.right, target.bottom}, now,
        snowdesktop::animation::RuntimeAnimationsEnabled(),
        snowdesktop::animation::RuntimeDurationScale());
    PositionQuickNavigationWindow();
    EnsureUiAnimationFrame();
    if (UseQuickNavigationList() && quickNavigation_->listSelection >= 0)
        EnsureQuickNavigationKeyboardTargetVisible(GetQuickNavigationListRowRect(static_cast<size_t>(quickNavigation_->listSelection)));
    InvalidateQuickNavigationWindow();
}
void DesktopApp::SetQuickNavigationSearchScope(QuickNavigationSearchType type, std::string engine)
{
    if (IsLuaLogicalSlotPickerOpen()) return;
    quickNavigation_->searchType = type == QuickNavigationSearchType::File ? QuickNavigationSearchType::All : type;
    quickNavigation_->searchEngine = std::move(engine);
    quickNavigation_->menu = QuickNavigationMenu::None;
    quickNavigation_->listSelection = -1;
    quickNavigation_->scrollOffset = 0;
    quickNavigation_->actionNotice.clear();
    ClearQuickNavigationEverythingResults();
    RefreshQuickNavigationEverythingResults();
    UpdateQuickNavigationSearchEditRect();
    PositionQuickNavigationWindow();
    InvalidateQuickNavigationWindow();
}
void DesktopApp::RefreshQuickNavigationTypedResults()
{
    quickNavigation_->actionNotice.clear();
    quickNavigation_->settingsResults.clear();
    quickNavigation_->listSelection = -1;
    if (quickNavigation_->searchType == QuickNavigationSearchType::Settings && !quickNavigation_->preview)
        quickNavigation_->settingsResults = snowdesktop::SettingsSearchIndex(BuildSettingsSearchInput()).Search(GetQuickNavigationEffectiveSearchText());
}
std::vector<DesktopApp::QuickNavigationListRow> DesktopApp::BuildQuickNavigationListRows() const
{
    using Kind = QuickNavigationListRow::Kind;
    namespace query = snowdesktop::quick_navigation_query;
    std::vector<QuickNavigationListRow> rows;
    const auto text = query::Trim(GetQuickNavigationEffectiveSearchText());
    if (text.empty())
    {
        if (quickNavigation_->collapsed && quickNavigation_->searchType == QuickNavigationSearchType::All)
        {
            rows.push_back({Kind::Header, 0, _LW("quickNav.search.actions"), {}, {}, false});
            for (const auto type : kQuickNavigationSearchTypes)
                if (type != QuickNavigationSearchType::All)
                    rows.push_back({Kind::Scope, static_cast<size_t>(type), ScopeLabel(type),
                        Utf8ToWide(navigationSettings_.prefixes[static_cast<size_t>(type) - 1]), {}, true});
        }
        return rows;
    }
    const auto header = [&](std::wstring title) { rows.push_back({Kind::Header, 0, std::move(title), {}, {}, false}); };
    const auto notice = [&](std::wstring title) { rows.push_back({Kind::Notice, 0, std::move(title), {}, {}, false}); };
    if (quickNavigation_->searchType == QuickNavigationSearchType::All)
    {
        const auto candidates = query::PrefixCandidates(navigationSettings_, text);
        if (!candidates.empty()) header(_LW("quickNav.search.actions"));
        for (const auto& candidate : candidates)
        {
            std::wstring label = ScopeLabel(candidate.scope.type);
            for (const auto& engine : navigationSettings_.engines)
                if (engine.id == candidate.scope.engine) { label = Utf8ToWide(engine.name); break; }
            rows.push_back({Kind::Scope, static_cast<size_t>(candidate.scope.type), std::move(label),
                Utf8ToWide(candidate.prefix), Utf8ToWide(candidate.scope.engine), true});
        }
    }
    if (quickNavigation_->searchType == QuickNavigationSearchType::Settings)
    {
        for (size_t i = 0; i < quickNavigation_->settingsResults.size(); ++i)
        {
            const auto& result = quickNavigation_->settingsResults[i];
            rows.push_back({Kind::Settings, i, result.label, result.context + L" · " + result.description, {}, true});
        }
    }
    else if (quickNavigation_->searchType == QuickNavigationSearchType::Web)
    {
        const auto engineId = quickNavigation_->searchEngine.empty() ? navigationSettings_.defaultEngine : quickNavigation_->searchEngine;
        for (const auto& engine : navigationSettings_.engines)
            if (engine.id == engineId)
                rows.push_back({Kind::Web, 0, Utf8ToWide(engine.name) + L" · " + text, _LW("quickNav.web.preview"), Utf8ToWide(query::SearchUrl(engine, text)), true});
    }
    else if (quickNavigation_->searchType == QuickNavigationSearchType::Run)
    {
        const auto command = query::ParseCommand(text);
        rows.push_back({Kind::Run, 0, command ? command->target : _LW("quickNav.run.invalid"),
            command ? command->parameters : _LW("quickNav.run.hint"), text, command.has_value()});
    }
    else if (quickNavigation_->searchType == QuickNavigationSearchType::Calculator)
    {
        const auto result = query::Calculator().Evaluate(text);
        if (result.error == query::CalculationError::None)
            rows.push_back({Kind::Calculator, 0, query::FormatCalculation(result.value), _LW("quickNav.calculator.copy"), query::FormatCalculation(result.value), true});
        else notice(_LW(result.error == query::CalculationError::DivisionByZero ? "quickNav.calculator.divisionByZero" :
            result.error == query::CalculationError::NonFinite ? "quickNav.calculator.nonFinite" : "quickNav.calculator.invalid"));
    }
    else
    {
        const auto model = BuildQuickNavigationContentModel();
        if (!model.entries.empty())
        {
            header(_LW("quickNav.desktop"));
            for (size_t i = 0; i < model.entries.size(); ++i)
            {
                const auto& entry = model.entries[i];
                rows.push_back({Kind::Item, i, entry.name, entry.source + L" · " + entry.path, {}, true});
            }
        }
        if (quickNavigation_->searchType == QuickNavigationSearchType::All || quickNavigation_->searchType == QuickNavigationSearchType::App)
        {
            if (!quickNavigation_->appsIndexed) notice(_LW("quickNav.apps.loading"));
            if (!quickNavigation_->appResultIndices.empty())
            {
                header(ScopeLabel(QuickNavigationSearchType::App));
                const size_t count = GetQuickNavigationVisibleAppResultCount();
                for (size_t i = 0; i < count; ++i)
                {
                    const auto index = quickNavigation_->appResultIndices[i];
                    const auto& entry = quickNavigation_->appEntries[index];
                    rows.push_back({Kind::App, i, entry.name, entry.parsingName, {}, true});
                }
                if (HasQuickNavigationAppExpandButton()) rows.push_back({Kind::ExpandApps, 0, _LW("quickNav.more"), {}, {}, true});
            }
        }
        if ((quickNavigation_->searchType == QuickNavigationSearchType::All || quickNavigation_->searchType == QuickNavigationSearchType::File) &&
            (!quickNavigation_->everythingResults.empty() || quickNavigation_->everythingSearchPending || !quickNavigation_->everythingSearchAvailable))
        {
            if (!quickNavigation_->everythingResults.empty())
                header(ScopeLabel(QuickNavigationSearchType::File));
            for (size_t i = 0; i < quickNavigation_->everythingResults.size(); ++i)
            {
                const auto& entry = quickNavigation_->everythingResults[i];
                rows.push_back({Kind::Everything, i, entry.name, entry.path, {}, true});
            }
            if (quickNavigation_->everythingSearchPending) notice(_LW("quickNav.files.loading"));
            else if (!quickNavigation_->everythingSearchAvailable) notice(GetQuickNavigationEverythingNoticeText());
            if (HasQuickNavigationEverythingLoadMoreButton()) rows.push_back({Kind::LoadMore, 0, _LW("quickNav.more"), {}, {}, true});
        }
    }
    if (std::none_of(rows.begin(), rows.end(), [](const auto& row) {return row.kind != Kind::Header;})) notice(_LW("quickNav.empty"));
    if (!quickNavigation_->actionNotice.empty()) notice(quickNavigation_->actionNotice);
    return rows;
}
int DesktopApp::QuickNavigationListRowHeight(const QuickNavigationListRow& row) const
{
    return QuickNavScale(row.kind == QuickNavigationListRow::Kind::Header ? 28 :
        row.kind == QuickNavigationListRow::Kind::Scope ? 48 : navigationSettings_.layout.resultRowHeight);
}
RECT DesktopApp::GetQuickNavigationListRowRect(size_t index) const
{
    const RECT content = GetQuickNavigationContentRect(quickNavigation_->rect);
    int top = content.top - quickNavigation_->scrollOffset;
    if (quickNavigation_->menu == QuickNavigationMenu::Views)
    {
        const auto anchor = GetQuickNavigationViewModeButtonRect(quickNavigation_->rect);
        top = std::max<LONG>(content.top + QuickNavScale(4), anchor.bottom + QuickNavScale(8)) + QuickNavScale(static_cast<int>(index) * 36);
        return QuickNavigationResultRowRect(MakeRect(content.left + QuickNavScale(4), top, std::min<LONG>(content.right - QuickNavScale(4), content.left + QuickNavScale(180)), top + QuickNavScale(36)), QuickNavScale(2));
    }
    if (quickNavigation_->menu != QuickNavigationMenu::None)
    {
        top += static_cast<int>(index) * QuickNavScale(navigationSettings_.layout.resultRowHeight);
        return QuickNavigationResultRowRect(MakeRect(content.left, top, content.right, top + QuickNavScale(navigationSettings_.layout.resultRowHeight)), QuickNavScale(2));
    }
    const auto rows = BuildQuickNavigationListRows();
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const int height = QuickNavigationListRowHeight(rows[i]);
        if (i == index)
        {
            const RECT slot = MakeRect(content.left, top, content.right, top + height);
            return rows[i].kind == QuickNavigationListRow::Kind::Header || rows[i].kind == QuickNavigationListRow::Kind::Notice ?
                slot : QuickNavigationResultRowRect(slot, QuickNavScale(2));
        }
        top += height;
    }
    return {};
}
void DesktopApp::DrawQuickNavigationList(ID2D1DeviceContext* context)
{
    using Kind = QuickNavigationListRow::Kind;
    const auto theme = ResolveQuickNavTheme(quickNavigation_->lightTheme, navigationSettings_, quickNavigation_->glassTheme);
    const auto rows = BuildQuickNavigationListRows();
    const auto model = BuildQuickNavigationContentModel();
    const RECT content = GetQuickNavigationContentRect(quickNavigation_->rect);
    int top = content.top - quickNavigation_->scrollOffset;
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const auto& row = rows[i];
        RECT bounds = MakeRect(content.left, top, content.right, top + QuickNavigationListRowHeight(row));
        top = bounds.bottom;
        if (row.kind != Kind::Header && row.kind != Kind::Notice)
            bounds = QuickNavigationResultRowRect(bounds, QuickNavScale(2));
        if (bounds.bottom <= content.top || bounds.top >= content.bottom) continue;
        const bool selected = quickNavigation_->listSelection == static_cast<int>(i) ||
            (quickNavigation_->listSelection < 0 && row.kind == Kind::Scope && i == 1);
        const bool hovered = row.enabled && PtInRect(&content, quickNavigation_->lastMousePoint) &&
            PtInRect(&bounds, quickNavigation_->lastMousePoint);
        if (row.kind == Kind::Header)
        {
            bounds.left += QuickNavScale(8);
            DrawD2DTextEllipsis(context, row.title, bounds, quickNavigation_->pathTextFormat.Get(), ToD2DColor(theme.headerText));
            continue;
        }
        DrawD2DRoundedRectangle(context, bounds, static_cast<float>(QuickNavScale(navigationSettings_.layout.itemRadius)),
            ToD2DColor(selected ? theme.selectedFill : hovered ? theme.appRowHoverFill : theme.resultFill),
            ToD2DColor(selected ? theme.selectedBorder : hovered ? theme.appRowHoverStroke : theme.resultBorder));
        const int size = QuickNavScale(36);
        RECT icon = MakeRect(bounds.left + QuickNavScale(8), (bounds.top + bounds.bottom - size) / 2, bounds.left + QuickNavScale(8) + size, (bounds.top + bounds.bottom + size) / 2);
        if (row.kind != Kind::Notice)
            DrawD2DRoundedRectangle(context, icon, static_cast<float>(QuickNavScale(navigationSettings_.layout.itemRadius)),
                ToD2DColor(theme.iconPlateFill),
                ToD2DColor(theme.iconPlateBorder));
        if (row.kind == Kind::App && row.index < quickNavigation_->appResultIndices.size())
            DrawQuickNavAppIcon(context, quickNavigation_->appEntries[quickNavigation_->appResultIndices[row.index]], icon);
        else if (row.kind == Kind::Everything && row.index < quickNavigation_->everythingResults.size())
            DrawQuickNavSysIcon(context, quickNavigation_->everythingResults[row.index].systemIconIndex, icon);
        else if (row.kind == Kind::Item && row.index < model.entries.size() && model.entries[row.index].itemIndex < items_.size())
        {
            const auto& item = items_[model.entries[row.index].itemIndex];
            if (item.iconBitmap) DrawIconBitmap(context, GetTargetIconBitmap(context, item.iconBitmap, item.layoutKey, item.iconIsMediaThumbnail), icon);
            else DrawQuickNavSysIcon(context, item.sysIconIndex, icon);
        }
        else if (row.kind == Kind::Item && row.index < model.entries.size())
        {
            const auto& entry = model.entries[row.index];
            if (entry.widgetIndex < widgets_.size() && entry.folderEntryIndex < widgets_[entry.widgetIndex].folderEntries.size())
            {
                const auto& item = widgets_[entry.widgetIndex].folderEntries[entry.folderEntryIndex];
                if (item.iconBitmap) DrawIconBitmap(context, GetTargetIconBitmap(context, item.iconBitmap, item.fullPath, item.iconIsMediaThumbnail), icon);
                else DrawQuickNavSysIcon(context,item.sysIconIndex,icon);
            }
            else if (entry.kind ==
                QuickNavigationEntry::Kind::PathFavorite)
            {
                DrawQuickNavSysIcon(
                    context,
                    GetQuickNavigationEverythingIconIndex(
                        entry.path, entry.isDirectory),
                    icon);
            }
            else if (entry.kind ==
                QuickNavigationEntry::Kind::ApplicationFavorite)
            {
                const auto* appEntry =
                    FindQuickNavigationAppEntryByParsingName(
                        entry.path);
                if (appEntry)
                    DrawQuickNavAppIcon(
                        context, *appEntry, icon);
            }
        }
        else if (row.kind == Kind::Scope)
            DrawQuickNavigationActionIcon(context, static_cast<QuickNavigationSearchType>(row.index), icon, 28);
        else if (row.kind != Kind::Notice)
            DrawQuickNavigationActionIcon(context, quickNavigation_->searchType, icon, 28);
        RECT title = bounds;
        title.left = row.kind == Kind::Notice ? bounds.left + QuickNavScale(8) : icon.right + QuickNavScale(8);
        title.right -= QuickNavScale(8);
        if (row.kind == Kind::Scope)
        {
            const int width = QuickNavScale(std::clamp(static_cast<int>(row.detail.size()) * 7 + 16, 40, 160));
            const int height = QuickNavScale(24);
            const RECT prefix = MakeRect(title.right - width, (bounds.top + bounds.bottom - height) / 2,
                title.right, (bounds.top + bounds.bottom + height) / 2);
            DrawD2DRoundedRectangle(context, prefix, static_cast<float>(QuickNavScale(4)),
                ToD2DColor(theme.searchBg, .3f), ToD2DColor(theme.searchBorder, .32f));
            RECT prefixText = prefix; prefixText.left += QuickNavScale(6); prefixText.right -= QuickNavScale(6);
            DrawQuickNavigationCenteredText(context, row.detail, prefixText, quickNavigation_->pathTextFormat.Get(), ToD2DColor(theme.appTypeText));
            title.right = prefix.left - QuickNavScale(12);
            DrawQuickNavigationCenteredText(context, row.title, title, quickNavigation_->itemTextFormat.Get(), ToD2DColor(selected ? theme.selectedText : theme.appNameText));
            continue;
        }
        if (!row.detail.empty()) title.bottom = bounds.top + (bounds.bottom - bounds.top) / 2 + QuickNavScale(2);
        DrawD2DTextEllipsis(context, row.title, title, quickNavigation_->itemTextFormat.Get(), ToD2DColor(row.enabled ? (selected ? theme.selectedText : theme.appNameText) : theme.emptyText));
        if (!row.detail.empty())
        {
            RECT detail = title; detail.top = title.bottom - QuickNavScale(2); detail.bottom = bounds.bottom - QuickNavScale(4);
            DrawD2DTextEllipsis(context, row.detail, detail, quickNavigation_->pathTextFormat.Get(), ToD2DColor(theme.appTypeText));
        }
    }
}
void DesktopApp::DrawQuickNavigationActionIcon(ID2D1DeviceContext* context, QuickNavigationSearchType type, RECT bounds, int sizeDip)
{
    if (!context) return;
    const auto index = static_cast<size_t>(type);
    if (index >= quickNavigation_->actionIconCache.size()) return;
    const int size = QuickNavScale(sizeDip);
    bounds = MakeRect((bounds.left + bounds.right - size) / 2, (bounds.top + bounds.bottom - size) / 2,
        (bounds.left + bounds.right - size) / 2 + size, (bounds.top + bounds.bottom - size) / 2 + size);
    const auto fallback = [&] {
        HIGHCONTRASTW contrast{sizeof(contrast)};
        const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON);
        const auto theme = ResolveQuickNavTheme(quickNavigation_->lightTheme, navigationSettings_, quickNavigation_->glassTheme);
        DrawQuickNavigationCenteredText(context, kScopeGlyphs[index], bounds, quickNavigation_->fluentTextFormat.Get(),
            ToD2DColor(highContrast ? snowdesktop::RgbaColor{GetSysColor(COLOR_WINDOWTEXT)} : theme.typeText), static_cast<float>(size));
    };
    HIGHCONTRASTW contrast{sizeof(contrast)};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON)) { fallback(); return; }
    if (quickNavigation_->actionIconContext != context)
    {
        for (auto& icon : quickNavigation_->actionIconCache) icon.Reset();
        quickNavigation_->actionIconContext = context;
    }
    auto& bitmap = quickNavigation_->actionIconCache[index];
    if (!bitmap)
    {
        constexpr int resources[] = {IDR_QUICK_NAV_ALL, IDR_QUICK_NAV_APP, 0, IDR_QUICK_NAV_WEB,
            IDR_QUICK_NAV_SETTINGS, IDR_QUICK_NAV_RUN, IDR_QUICK_NAV_CALCULATOR};
        const HMODULE module = GetModuleHandleW(nullptr);
        const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(resources[index]), RT_RCDATA);
        const HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
        auto* bytes = loaded ? static_cast<BYTE*>(LockResource(loaded)) : nullptr;
        const DWORD byteCount = resource ? SizeofResource(module, resource) : 0;
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        if (!bytes || !byteCount || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
            FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromMemory(bytes, byteCount)) ||
            FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
            FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
            FAILED(context->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap))) { fallback(); return; }
    }
    context->DrawBitmap(bitmap.Get(), ToD2DRect(bounds), 1.f, D2D1_INTERPOLATION_MODE_LINEAR);
}
void DesktopApp::DrawQuickNavigationCenteredText(ID2D1DeviceContext* context, const std::wstring& text,
    RECT bounds, IDWriteTextFormat* format, const D2D1_COLOR_F& color, float fontSize)
{
    if (!context || !dwriteFactory_ || !format || text.empty() || IsRectEmpty(&bounds)) return;
    // Fluent's font boxes can exceed the actual path, especially for chevrons.
    // Center the official outline itself; this also avoids baseline rounding
    // shifting small symbols independently of the search text and caret.
    if (format == quickNavigation_->fluentTextFormat.Get() && text.size() == 1)
    {
        ComPtr<IDWriteFontCollection> collection;
        ComPtr<IDWriteFontFamily> family;
        ComPtr<IDWriteFont> font;
        ComPtr<IDWriteFontFace> face;
        UINT32 familyIndex = 0;
        BOOL found = FALSE;
        if (SUCCEEDED(format->GetFontCollection(&collection)) && collection &&
            SUCCEEDED(collection->FindFamilyName(L"FluentSystemIcons-Regular", &familyIndex, &found)) && found &&
            SUCCEEDED(collection->GetFontFamily(familyIndex, &family)) &&
            SUCCEEDED(family->GetFirstMatchingFont(format->GetFontWeight(), format->GetFontStretch(), format->GetFontStyle(), &font)) &&
            SUCCEEDED(font->CreateFontFace(&face)))
        {
            const UINT32 codepoint = static_cast<UINT32>(text.front());
            UINT16 glyph = 0;
            ComPtr<ID2D1Factory> factory;
            ComPtr<ID2D1PathGeometry> geometry;
            ComPtr<ID2D1GeometrySink> sink;
            context->GetFactory(&factory);
            if (SUCCEEDED(face->GetGlyphIndices(&codepoint, 1, &glyph)) && glyph && factory &&
                SUCCEEDED(factory->CreatePathGeometry(&geometry)) && SUCCEEDED(geometry->Open(&sink)) &&
                SUCCEEDED(face->GetGlyphRunOutline(fontSize > 0.f ? fontSize : format->GetFontSize(),
                    &glyph, nullptr, nullptr, 1, FALSE, FALSE, sink.Get())) && SUCCEEDED(sink->Close()))
            {
                D2D1_RECT_F ink{};
                ComPtr<ID2D1SolidColorBrush> brush;
                if (SUCCEEDED(geometry->GetBounds(nullptr, &ink)) && ink.bottom > ink.top &&
                    SUCCEEDED(context->CreateSolidColorBrush(color, &brush)))
                {
                    D2D1_MATRIX_3X2_F original{};
                    context->GetTransform(&original);
                    context->PushAxisAlignedClip(ToD2DRect(bounds), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    context->SetTransform(D2D1::Matrix3x2F::Translation(
                        (static_cast<float>(bounds.left) + static_cast<float>(bounds.right) - ink.left - ink.right) * .5f,
                        (static_cast<float>(bounds.top) + static_cast<float>(bounds.bottom) - ink.top - ink.bottom) * .5f) * original);
                    context->FillGeometry(geometry.Get(), brush.Get());
                    context->SetTransform(original);
                    context->PopAxisAlignedClip();
                    return;
                }
            }
        }
    }
    ComPtr<IDWriteTextLayout> layout;
    const float width = static_cast<float>(bounds.right - bounds.left);
    const float height = static_cast<float>(bounds.bottom - bounds.top);
    if (FAILED(dwriteFactory_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
            format, width, height, &layout))) return;
    if (fontSize > 0.f) layout->SetFontSize(fontSize, {0, static_cast<UINT32>(text.size())});
    else
    {
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(dwriteFactory_->CreateEllipsisTrimmingSign(format, &sign))) layout->SetTrimming(&trimming, sign.Get());
    }
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DWRITE_OVERHANG_METRICS ink{};
    const float offsetY = SUCCEEDED(layout->GetOverhangMetrics(&ink)) ? (ink.top - ink.bottom) / 2.f : 0.f;
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(context->CreateSolidColorBrush(color, &brush))) return;
    context->PushAxisAlignedClip(ToD2DRect(bounds), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    context->DrawTextLayout(D2D1::Point2F(static_cast<float>(bounds.left),
        static_cast<float>(bounds.top) + offsetY), layout.Get(), brush.Get());
    context->PopAxisAlignedClip();
}
void DesktopApp::DrawQuickNavigationMenus(ID2D1DeviceContext* context)
{
    const auto theme = ResolveQuickNavTheme(quickNavigation_->lightTheme, navigationSettings_, quickNavigation_->glassTheme);
    const auto format = quickNavigation_->fluentTextFormat.Get();
    for (int button = 0; button < 3; ++button)
    {
        RECT bounds = GetQuickNavigationToolbarRect(quickNavigation_->rect, button);
        const bool hovered = PtInRect(&bounds, quickNavigation_->lastMousePoint) != FALSE;
        const bool chip = button == 0 && quickNavigation_->searchType != QuickNavigationSearchType::All;
        if (hovered) DrawD2DRoundedRectangle(context, bounds, static_cast<float>(QuickNavScale(navigationSettings_.layout.searchRadius)),
            ToD2DColor(theme.tabHoverFill), D2D1::ColorF(0,0,0,0.f));
        const wchar_t* glyph = button == 0 ? kScopeGlyphs[static_cast<size_t>(quickNavigation_->searchType)] : button == 1 ? L"\uF6AA" : (quickNavigation_->collapsed ? L"\uF2A4" : L"\uF2B7");
        RECT symbol = bounds;
        const RECT search = GetQuickNavigationSearchRect(quickNavigation_->rect);
        symbol.top = search.top; symbol.bottom = search.bottom;
        if (chip) symbol.left += QuickNavScale(28);
        if (chip) DrawQuickNavigationActionIcon(context, quickNavigation_->searchType, symbol, 24);
        else DrawQuickNavigationCenteredText(context, glyph, symbol, format,
            ToD2DColor(theme.searchPlaceholder), static_cast<float>(QuickNavScale(button == 2 ? 16 : 20)));
        if (chip)
        {
            RECT back = symbol; back.left = bounds.left; back.right = back.left + QuickNavScale(24);
            DrawQuickNavigationCenteredText(context, L"\uF15C", back, format, ToD2DColor(theme.searchPlaceholder), static_cast<float>(QuickNavScale(16)));
        }
    }
    if (quickNavigation_->menu == QuickNavigationMenu::None)
    {
        if (quickNavigation_->collapsed && GetQuickNavigationEffectiveSearchText().empty())
        {
            const int padding = QuickNavScale(std::max(4, navigationSettings_.layout.padding - 8));
            RECT hint = MakeRect(quickNavigation_->rect.left + padding, quickNavigation_->rect.bottom - QuickNavScale(32),
                quickNavigation_->rect.right - padding, quickNavigation_->rect.bottom - QuickNavScale(8));
            DrawD2DTextEllipsis(context, _LW(quickNavigation_->searchType == QuickNavigationSearchType::All ? "quickNav.search.shortcutHint" : "quickNav.search.scopedHint"),
                hint, quickNavigation_->pathTextFormat.Get(), ToD2DColor(theme.appTypeText));
        }
        return;
    }
    const auto viewModes = GetQuickNavigationViewModeOptions();
    const size_t count = quickNavigation_->menu == QuickNavigationMenu::Types ? kQuickNavigationSearchTypes.size() : viewModes.size();
    const RECT content = GetQuickNavigationContentRect(quickNavigation_->rect);
    context->PushAxisAlignedClip(ToD2DRect(content), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (quickNavigation_->menu == QuickNavigationMenu::Views)
    {
        RECT popup = GetQuickNavigationListRowRect(0); popup.bottom = GetQuickNavigationListRowRect(count - 1).bottom;
        InflateRect(&popup, QuickNavScale(4), QuickNavScale(4));
        // The menu overlays already-painted items on the same surface. An
        // opaque backing keeps their icons and labels out of the menu rows.
        auto popupFill = ToD2DColor(theme.searchBg);
        popupFill.a = 1.f;
        DrawD2DRoundedRectangle(context, popup, static_cast<float>(QuickNavScale(navigationSettings_.layout.itemRadius)),
            popupFill, ToD2DColor(theme.searchBorder, .45f));
    }
    for (size_t i = 0; i < count; ++i)
    {
        RECT bounds = GetQuickNavigationListRowRect(i);
        const bool selected = quickNavigation_->menuSelection == static_cast<int>(i);
        const bool hover = PtInRect(&content, quickNavigation_->lastMousePoint) &&
            PtInRect(&bounds, quickNavigation_->lastMousePoint);
        if (selected || hover) DrawD2DRoundedRectangle(context, bounds, static_cast<float>(QuickNavScale(navigationSettings_.layout.itemRadius)),
            ToD2DColor(selected ? theme.selectedFill : theme.appRowHoverFill), ToD2DColor(selected ? theme.selectedBorder : theme.appRowHoverStroke));
        const auto type = kQuickNavigationSearchTypes[i];
        RECT icon = bounds; icon.right = icon.left + QuickNavScale(44);
        if (quickNavigation_->menu == QuickNavigationMenu::Types)
            DrawQuickNavigationActionIcon(context, type, icon, 28);
        RECT label = bounds;
        label.left = quickNavigation_->menu == QuickNavigationMenu::Types ? icon.right + QuickNavScale(8) : bounds.left + QuickNavScale(10);
        label.right -= QuickNavScale(quickNavigation_->menu == QuickNavigationMenu::Types ? 90 : 10);
        constexpr const char* views[] = {"app.nav.view_tile", "app.nav.view_source", "app.nav.view_initial", "app.nav.view_frequency"};
        DrawD2DTextEllipsis(context, quickNavigation_->menu == QuickNavigationMenu::Types ? ScopeLabel(type) : _LW(views[static_cast<size_t>(viewModes[i])]), label, quickNavigation_->itemTextFormat.Get(), ToD2DColor(selected ? theme.selectedText : theme.appNameText));
        if (quickNavigation_->menu == QuickNavigationMenu::Types && i > 0)
        {
            RECT prefix = bounds; prefix.left = prefix.right - QuickNavScale(80); prefix.right -= QuickNavScale(12);
            DrawD2DText(context, Utf8ToWide(navigationSettings_.prefixes[static_cast<size_t>(type) - 1]), prefix, quickNavigation_->pathTextFormat.Get(), ToD2DColor(theme.appTypeText));
        }
    }
    context->PopAxisAlignedClip();
}
bool DesktopApp::HandleQuickNavigationToolbarClick(POINT point)
{
    for (int button = 0; button < 3; ++button)
    {
        const RECT bounds = GetQuickNavigationToolbarRect(quickNavigation_->rect, button);
        if (!PtInRect(&bounds, point)) continue;
        if (button == 2) ToggleQuickNavigationCollapsed();
        else if (button == 1) CloseQuickNavigationThen([this]() { ShowSettingsWindow(snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::QuickNavigation)); });
        else if (quickNavigation_->searchType != QuickNavigationSearchType::All && point.x < bounds.left + QuickNavScale(24)) SetQuickNavigationSearchScope(QuickNavigationSearchType::All);
        else
        {
            quickNavigation_->menu = quickNavigation_->menu == QuickNavigationMenu::Types ? QuickNavigationMenu::None : QuickNavigationMenu::Types;
            quickNavigation_->menuSelection = static_cast<int>(std::distance(kQuickNavigationSearchTypes.begin(), std::find(kQuickNavigationSearchTypes.begin(), kQuickNavigationSearchTypes.end(), quickNavigation_->searchType)));
            quickNavigation_->scrollOffset = 0;
            PositionQuickNavigationWindow();
            InvalidateQuickNavigationWindow();
        }
        return true;
    }
    return false;
}
bool DesktopApp::DismissQuickNavigationMenuAtPoint(POINT point)
{
    if (quickNavigation_->menu == QuickNavigationMenu::None) return false;
    const size_t count = quickNavigation_->menu == QuickNavigationMenu::Views ? GetQuickNavigationViewModeOptions().size() : kQuickNavigationSearchTypes.size();
    RECT popup = GetQuickNavigationListRowRect(0);
    popup.bottom = GetQuickNavigationListRowRect(count - 1).bottom;
    if (quickNavigation_->menu == QuickNavigationMenu::Views) InflateRect(&popup, QuickNavScale(4), QuickNavScale(4));
    const RECT content = GetQuickNavigationContentRect(quickNavigation_->rect);
    RECT visible{};
    if (IntersectRect(&visible, &popup, &content) && PtInRect(&visible, point)) return false;
    quickNavigation_->menu = QuickNavigationMenu::None;
    PositionQuickNavigationWindow();
    InvalidateQuickNavigationWindow();
    return true;
}
bool DesktopApp::HandleQuickNavigationListClick(POINT point, bool contextMenu, POINT screenPoint)
{
    using Kind = QuickNavigationListRow::Kind;
    const RECT content = GetQuickNavigationContentRect(quickNavigation_->rect);
    if (!PtInRect(&content, point)) return true;
    const auto rows = BuildQuickNavigationListRows();
    const auto viewModes = GetQuickNavigationViewModeOptions();
    const size_t count = quickNavigation_->menu == QuickNavigationMenu::Types ? kQuickNavigationSearchTypes.size() : quickNavigation_->menu == QuickNavigationMenu::Views ? viewModes.size() : rows.size();
    for (size_t i = 0; i < count; ++i)
    {
        const RECT bounds = GetQuickNavigationListRowRect(i);
        if (!PtInRect(&bounds, point)) continue;
        if (quickNavigation_->menu != QuickNavigationMenu::None)
        {
            if (quickNavigation_->menu == QuickNavigationMenu::Types) SetQuickNavigationSearchScope(kQuickNavigationSearchTypes[i]);
            else
            {
                quickNavigation_->menu = QuickNavigationMenu::None;
                SetQuickNavigationViewMode(viewModes[i]);
                PositionQuickNavigationWindow();
                InvalidateQuickNavigationWindow();
            }
        }
        else if (!contextMenu) { quickNavigation_->listSelection = static_cast<int>(i); ActivateQuickNavigationListRow(i); }
        else if (rows[i].kind == Kind::App) ShowQuickNavigationAppContextMenu(quickNavigation_->appEntries[quickNavigation_->appResultIndices[rows[i].index]], screenPoint);
        else if (rows[i].kind == Kind::Everything) ShowQuickNavigationEverythingContextMenu(quickNavigation_->everythingResults[rows[i].index], screenPoint);
        else if (rows[i].kind == Kind::Item)
        {
            const auto model = BuildQuickNavigationContentModel();
            const auto& entry = model.entries[rows[i].index];
            ClearSelection();
            if (entry.kind == QuickNavigationEntry::Kind::DesktopItem && entry.itemIndex < items_.size())
            { items_[entry.itemIndex].selected = true; ShowItemContextMenu(screenPoint,static_cast<int>(entry.itemIndex),false,true); }
            else if (entry.widgetIndex < widgets_.size() && entry.folderEntryIndex < widgets_[entry.widgetIndex].folderEntries.size())
            { widgets_[entry.widgetIndex].folderEntries[entry.folderEntryIndex].selected = true; ShowFolderEntryContextMenu(screenPoint,entry.widgetIndex,entry.folderEntryIndex,true); }
            else if (entry.kind == QuickNavigationEntry::Kind::PathFavorite)
            {
                QuickNavigationEverythingEntry favoriteEntry;
                favoriteEntry.name = entry.name;
                favoriteEntry.path = entry.path;
                favoriteEntry.isDirectory = entry.isDirectory;
                ShowQuickNavigationEverythingContextMenu(
                    favoriteEntry, screenPoint);
            }
            else if (entry.kind ==
                QuickNavigationEntry::Kind::ApplicationFavorite)
            {
                const auto* appEntry =
                    FindQuickNavigationAppEntryByParsingName(
                        entry.path);
                if (appEntry)
                    ShowQuickNavigationAppContextMenu(
                        *appEntry, screenPoint);
            }
        }
        return true;
    }
    return true;
}
bool DesktopApp::ActivateQuickNavigationListRow(size_t index)
{
    using Kind = QuickNavigationListRow::Kind;
    const auto rows = BuildQuickNavigationListRows();
    if (index >= rows.size() || !rows[index].enabled) return false;
    const auto row = rows[index];
    if (row.kind == Kind::Scope)
    {
        if (quickNavigation_->searchEdit && IsWindow(quickNavigation_->searchEdit)) SetWindowTextW(quickNavigation_->searchEdit, L"");
        quickNavigation_->searchText.clear(); quickNavigation_->effectiveSearchText.clear();
        SetQuickNavigationSearchScope(static_cast<QuickNavigationSearchType>(row.index), WideToUtf8(row.value));
    }
    else if (row.kind == Kind::Calculator)
    {
        quickNavigation_->actionNotice = _LW(CopyTextToClipboard(row.value) ? "quickNav.copied" : "quickNav.copyFailed");
        PositionQuickNavigationWindow();
        InvalidateQuickNavigationWindow();
    }
    else if (row.kind == Kind::Settings)
    {
        // Rebuild immediately before opening so removed/hidden widget fields cannot be executed.
        const auto desired = quickNavigation_->settingsResults[row.index];
        const auto current = snowdesktop::SettingsSearchIndex(BuildSettingsSearchInput()).Search(GetQuickNavigationEffectiveSearchText());
        const bool available = std::any_of(current.begin(), current.end(), [&](const auto& hit) { return hit.route == desired.route && hit.focusId == desired.focusId; });
        if (available) CloseQuickNavigationThen([this, route = desired.route]() { ShowSettingsWindow(route); });
        else { RefreshQuickNavigationTypedResults(); InvalidateQuickNavigationWindow(); }
    }
    else if (row.kind == Kind::App) CloseQuickNavigationThenLaunchApp(quickNavigation_->appEntries[quickNavigation_->appResultIndices[row.index]]);
    else if (row.kind == Kind::Item)
    {
        const auto model = BuildQuickNavigationContentModel();
        if (row.index >= model.entries.size()) return false;
        const auto entry = model.entries[row.index];
        if (entry.kind == QuickNavigationEntry::Kind::DesktopItem)
        {
            if (entry.itemIndex >= items_.size()) return false;
            const auto key = items_[entry.itemIndex].layoutKey;
            CloseQuickNavigationThen([this, key]() {
                const auto currentIndex = FindItemIndexByKey(key);
                if (currentIndex < items_.size()) LaunchDesktopItem(currentIndex, true);
            });
        }
        else if (entry.kind ==
            QuickNavigationEntry::Kind::PathFavorite)
        {
            CloseQuickNavigationThen(
                [this, path = entry.path]() {
                    LaunchPathWithShortcutPolicy(nullptr, path);
                });
        }
        else if (entry.kind ==
            QuickNavigationEntry::Kind::ApplicationFavorite)
        {
            const auto* appEntry =
                FindQuickNavigationAppEntryByParsingName(
                    entry.path);
            if (appEntry)
                CloseQuickNavigationThenLaunchApp(*appEntry);
        }
        else CloseQuickNavigationThen([this, path = entry.path]() { LaunchPathWithShortcutPolicy(nullptr, path); });
    }
    else if (row.kind == Kind::ExpandApps) { quickNavigation_->appsExpanded = true; PositionQuickNavigationWindow(); InvalidateQuickNavigationWindow(); }
    else if (row.kind == Kind::LoadMore) { quickNavigation_->everythingResultLimit += kQuickNavigationEverythingResultBatchSize; RefreshQuickNavigationEverythingResults(); PositionQuickNavigationWindow(); InvalidateQuickNavigationWindow(); }
    else
    {
        const auto value = row.kind == Kind::Everything ? quickNavigation_->everythingResults[row.index].path : row.value;
        CloseQuickNavigationThen([this, value, run = row.kind == Kind::Run]() {
            snowdesktop::shell_launch_process::Request request;
            request.owner = hwnd_; request.path = value;
            request.action = run ? snowdesktop::shell_launch_process::Action::RunCommand : snowdesktop::shell_launch_process::Action::OpenWithShortcutPolicy;
            (void)snowdesktop::shell_launch_process::Start(request);
        });
    }
    return true;
}
bool DesktopApp::HandleQuickNavigationSearchKey(WPARAM key)
{
    if (!quickNavigation_->open || snowdesktop::text_input::IsComposing(quickNavigation_->searchEdit) || !quickNavigation_->searchCompositionText.empty()) return false;
    if (key == 'E' && (snowdesktop::private_pointer::KeyState(VK_CONTROL) & 0x8000)) { ToggleQuickNavigationCollapsed(); return true; }
    if (key == VK_ESCAPE)
    {
        if (quickNavigation_->menu != QuickNavigationMenu::None) { quickNavigation_->menu = QuickNavigationMenu::None; PositionQuickNavigationWindow(); InvalidateQuickNavigationWindow(); return true; }
        if (quickNavigation_->searchType != QuickNavigationSearchType::All) { SetQuickNavigationSearchScope(QuickNavigationSearchType::All); return true; }
        return false;
    }
    if (key == VK_BACK && GetQuickNavigationEffectiveSearchText().empty() && quickNavigation_->searchType != QuickNavigationSearchType::All)
    { SetQuickNavigationSearchScope(QuickNavigationSearchType::All); return true; }
    if (key == VK_RETURN && !IsLuaLogicalSlotPickerOpen() && snowdesktop::quick_navigation_query::GetSubmitIntent(navigationSettings_,quickNavigation_->searchType,quickNavigation_->searchText,false,quickNavigation_->menu != QuickNavigationMenu::None) == snowdesktop::quick_navigation_query::SubmitIntent::ConfirmPrefix)
    {
        if (const auto scope = snowdesktop::quick_navigation_query::ResolvePrefix(navigationSettings_, quickNavigation_->searchText))
        {
            SetWindowTextW(quickNavigation_->searchEdit, L"");
            quickNavigation_->searchText.clear(); quickNavigation_->effectiveSearchText.clear();
            SetQuickNavigationSearchScope(scope->type, scope->engine); return true;
        }
    }
    if (key == VK_TAB && !IsLuaLogicalSlotPickerOpen() && !(snowdesktop::private_pointer::KeyState(VK_SHIFT) & 0x8000) &&
        quickNavigation_->menu == QuickNavigationMenu::None && quickNavigation_->searchType == QuickNavigationSearchType::All)
    {
        const auto rows = BuildQuickNavigationListRows();
        if (quickNavigation_->listSelection >= 0 && static_cast<size_t>(quickNavigation_->listSelection) < rows.size() &&
            rows[static_cast<size_t>(quickNavigation_->listSelection)].kind == QuickNavigationListRow::Kind::Scope)
            return ActivateQuickNavigationListRow(static_cast<size_t>(quickNavigation_->listSelection));
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i].kind == QuickNavigationListRow::Kind::Scope) return ActivateQuickNavigationListRow(i);
        return false;
    }
    if (key != VK_UP && key != VK_DOWN && key != VK_RETURN) return false;
    if (quickNavigation_->menu != QuickNavigationMenu::None)
    {
        const auto viewModes = GetQuickNavigationViewModeOptions();
        const int count = quickNavigation_->menu == QuickNavigationMenu::Types ? static_cast<int>(kQuickNavigationSearchTypes.size()) : static_cast<int>(viewModes.size());
        quickNavigation_->menuSelection = std::clamp(quickNavigation_->menuSelection, 0, count - 1);
        if (key == VK_RETURN)
        {
            if (quickNavigation_->menu == QuickNavigationMenu::Types) SetQuickNavigationSearchScope(kQuickNavigationSearchTypes[static_cast<size_t>(quickNavigation_->menuSelection)]);
            else { quickNavigation_->menu = QuickNavigationMenu::None; SetQuickNavigationViewMode(viewModes[static_cast<size_t>(quickNavigation_->menuSelection)]); PositionQuickNavigationWindow(); }
        }
        else quickNavigation_->menuSelection = (quickNavigation_->menuSelection + (key == VK_DOWN ? 1 : count - 1)) % count;
        InvalidateQuickNavigationWindow(); return true;
    }
    if (!UseQuickNavigationList()) return key != VK_RETURN && HandleQuickNavigationKeyboardInput(key);
    const auto rows = BuildQuickNavigationListRows();
    if (key == VK_RETURN)
    {
        if (quickNavigation_->listSelection >= 0 && ActivateQuickNavigationListRow(static_cast<size_t>(quickNavigation_->listSelection))) return true;
        for (size_t i = 0; i < rows.size(); ++i) if (rows[i].enabled) { ActivateQuickNavigationListRow(i); break; }
        return true;
    }
    if (rows.empty()) return true;
    const int count = static_cast<int>(rows.size());
    int next = quickNavigation_->listSelection;
    if (next < 0 && rows.size() > 1 && rows[1].kind == QuickNavigationListRow::Kind::Scope) next = 1;
    for (int attempts = 0; attempts < count; ++attempts)
    {
        next = key == VK_DOWN ? (next + 1) % count : (next <= 0 ? count - 1 : next - 1);
        if (rows[static_cast<size_t>(next)].enabled) { quickNavigation_->listSelection = next; break; }
    }
    if (quickNavigation_->listSelection >= 0) EnsureQuickNavigationKeyboardTargetVisible(GetQuickNavigationListRowRect(static_cast<size_t>(quickNavigation_->listSelection)));
    InvalidateQuickNavigationWindow(); return true;
}

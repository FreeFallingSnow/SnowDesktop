#include "app.h"
#include "quick_navigation_theme.h"
#include "../quick_navigation_query.h"
#include "../quick_navigation_rules.h"
#include "../shell_launch_process.h"

namespace
{
constexpr const char* kScopeKeys[] = {"quickNav.type.all", "quickNav.type.app", "quickNav.type.file",
    "quickNav.type.web", "quickNav.type.settings", "quickNav.type.run", "quickNav.type.calculator"};
constexpr const wchar_t* kScopeGlyphs[] = {L"\uF68F", L"\uF122", L"\uF378", L"\uE6B2", L"\uF6A9", L"\uF582", L"\uE233"};
std::wstring ScopeLabel(QuickNavigationSearchType type)
{
    return _LW(kScopeKeys[static_cast<size_t>(type)]);
}
}

bool DesktopApp::UseQuickNavigationList() const
{
    return !IsLuaLogicalSlotPickerOpen() && (quickNavigationCollapsed_ || quickNavigationSearchType_ != QuickNavigationSearchType::All);
}
int DesktopApp::QuickNavigationGridCellWidth() const
{
    return std::max(96, navigationSettings_.layout.iconSize + navigationSettings_.layout.gridGap * 2);
}
int DesktopApp::QuickNavigationGridCellHeight() const
{
    const auto& layout = navigationSettings_.layout;
    return layout.iconSize + 12 + (layout.fontSize + 4) * layout.labelLines + 8;
}
RECT DesktopApp::GetQuickNavigationToolbarRect(const RECT& overlay, int button) const
{
    const RECT search = GetQuickNavigationSearchRect(overlay);
    const int height = std::min(QuickNavScale(32), static_cast<int>(search.bottom - search.top) - QuickNavScale(8));
    const int top = search.top + (static_cast<int>(search.bottom - search.top) - height) / 2;
    if (button == 0)
        return MakeRect(search.left + QuickNavScale(8), top, search.left + QuickNavScale(quickNavigationSearchType_ == QuickNavigationSearchType::All ? 40 : 142), top + height);
    const int right = search.right - QuickNavScale(button == 2 ? 8 : 46);
    return MakeRect(right - height, top, right, top + height);
}
RECT DesktopApp::GetQuickNavigationInputRect(const RECT& overlay) const
{
    const RECT search = GetQuickNavigationSearchRect(overlay);
    const RECT type = GetQuickNavigationToolbarRect(overlay, 0);
    const RECT settings = GetQuickNavigationToolbarRect(overlay, 1);
    return MakeRect(type.right + QuickNavScale(8), search.top + QuickNavScale(4), settings.left - QuickNavScale(8), search.bottom - QuickNavScale(4));
}
std::wstring DesktopApp::QuickNavigationTypeLabel() const
{
    if (quickNavigationSearchType_ == QuickNavigationSearchType::Web && !quickNavigationSearchEngine_.empty())
        for (const auto& engine : navigationSettings_.engines)
            if (engine.id == quickNavigationSearchEngine_) return Utf8ToWide(engine.name);
    return ScopeLabel(quickNavigationSearchType_);
}
std::wstring DesktopApp::QuickNavigationViewLabel() const
{
    constexpr const char* keys[] = {"app.nav.view_tile", "app.nav.view_source", "app.nav.view_initial"};
    return _LW(keys[static_cast<size_t>(navigationSettings_.desktopViewMode)]);
}
void DesktopApp::ToggleQuickNavigationCollapsed()
{
    if (IsLuaLogicalSlotPickerOpen()) return;
    quickNavigationCollapsed_ = !quickNavigationCollapsed_;
    quickNavigationInitialJumpOpen_ = false;
    quickNavigationMenu_ = QuickNavigationMenu::None;
    quickNavigationScrollOffset_ = 0;
    const auto rows = BuildQuickNavigationListRows();
    if (quickNavigationCollapsed_)
    {
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const auto& row = rows[i];
            if ((row.kind == QuickNavigationListRow::Kind::Item && IsQuickNavigationKeyboardTarget(QuickNavigationKeyboardTargetKind::Item, row.index)) ||
                (row.kind == QuickNavigationListRow::Kind::App && IsQuickNavigationKeyboardTarget(QuickNavigationKeyboardTargetKind::App, row.index)) ||
                (row.kind == QuickNavigationListRow::Kind::Everything && IsQuickNavigationKeyboardTarget(QuickNavigationKeyboardTargetKind::Everything, row.index)))
                quickNavigationListSelection_ = static_cast<int>(i);
        }
    }
    else if (quickNavigationListSelection_ >= 0 && static_cast<size_t>(quickNavigationListSelection_) < rows.size())
    {
        const auto& row = rows[static_cast<size_t>(quickNavigationListSelection_)];
        quickNavigationKeyboardTargetIndex_ = row.index;
        quickNavigationKeyboardTargetKind_ = row.kind == QuickNavigationListRow::Kind::Item ? QuickNavigationKeyboardTargetKind::Item :
            row.kind == QuickNavigationListRow::Kind::App ? QuickNavigationKeyboardTargetKind::App :
            row.kind == QuickNavigationListRow::Kind::Everything ? QuickNavigationKeyboardTargetKind::Everything : QuickNavigationKeyboardTargetKind::None;
    }
    PositionQuickNavigationWindow();
    if (UseQuickNavigationList() && quickNavigationListSelection_ >= 0)
        EnsureQuickNavigationKeyboardTargetVisible(GetQuickNavigationListRowRect(static_cast<size_t>(quickNavigationListSelection_)));
    InvalidateQuickNavigationWindow();
}
void DesktopApp::SetQuickNavigationSearchScope(QuickNavigationSearchType type, std::string engine)
{
    if (IsLuaLogicalSlotPickerOpen()) return;
    quickNavigationSearchType_ = type;
    quickNavigationSearchEngine_ = std::move(engine);
    quickNavigationMenu_ = QuickNavigationMenu::None;
    quickNavigationListSelection_ = -1;
    quickNavigationScrollOffset_ = 0;
    quickNavigationActionNotice_.clear();
    ClearQuickNavigationEverythingResults();
    RefreshQuickNavigationEverythingResults();
    UpdateQuickNavigationSearchEditRect();
    PositionQuickNavigationWindow();
    InvalidateQuickNavigationWindow();
}
void DesktopApp::RefreshQuickNavigationTypedResults()
{
    quickNavigationActionNotice_.clear();
    quickNavigationSettingsResults_.clear();
    quickNavigationListSelection_ = -1;
    if (quickNavigationSearchType_ == QuickNavigationSearchType::Settings && !quickNavigationPreview_)
        quickNavigationSettingsResults_ = snowdesktop::SettingsSearchIndex(BuildSettingsSearchInput()).Search(GetQuickNavigationEffectiveSearchText());
}
std::vector<DesktopApp::QuickNavigationListRow> DesktopApp::BuildQuickNavigationListRows() const
{
    using Kind = QuickNavigationListRow::Kind;
    namespace query = snowdesktop::quick_navigation_query;
    std::vector<QuickNavigationListRow> rows;
    const auto text = query::Trim(GetQuickNavigationEffectiveSearchText());
    if (text.empty()) return rows;
    const auto header = [&](std::wstring title) { rows.push_back({Kind::Header, 0, std::move(title), {}, {}, false}); };
    const auto notice = [&](std::wstring title) { rows.push_back({Kind::Notice, 0, std::move(title), {}, {}, false}); };
    if (quickNavigationSearchType_ == QuickNavigationSearchType::Settings)
    {
        for (size_t i = 0; i < quickNavigationSettingsResults_.size(); ++i)
        {
            const auto& result = quickNavigationSettingsResults_[i];
            rows.push_back({Kind::Settings, i, result.label, result.context + L" · " + result.description, {}, true});
        }
    }
    else if (quickNavigationSearchType_ == QuickNavigationSearchType::Web)
    {
        const auto engineId = quickNavigationSearchEngine_.empty() ? navigationSettings_.defaultEngine : quickNavigationSearchEngine_;
        for (const auto& engine : navigationSettings_.engines)
            if (engine.id == engineId)
                rows.push_back({Kind::Web, 0, Utf8ToWide(engine.name) + L" · " + text, _LW("quickNav.web.preview"), Utf8ToWide(query::SearchUrl(engine, text)), true});
    }
    else if (quickNavigationSearchType_ == QuickNavigationSearchType::Run)
    {
        const auto command = query::ParseCommand(text);
        rows.push_back({Kind::Run, 0, command ? command->target : _LW("quickNav.run.invalid"),
            command ? command->parameters : _LW("quickNav.run.hint"), text, command.has_value()});
    }
    else if (quickNavigationSearchType_ == QuickNavigationSearchType::Calculator)
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
        if (quickNavigationSearchType_ == QuickNavigationSearchType::All || quickNavigationSearchType_ == QuickNavigationSearchType::App)
        {
            if (!quickNavigationAppsIndexed_) notice(_LW("quickNav.apps.loading"));
            if (!quickNavigationAppResultIndices_.empty())
            {
                header(ScopeLabel(QuickNavigationSearchType::App));
                const size_t count = GetQuickNavigationVisibleAppResultCount();
                for (size_t i = 0; i < count; ++i)
                {
                    const auto index = quickNavigationAppResultIndices_[i];
                    const auto& entry = quickNavigationAppEntries_[index];
                    rows.push_back({Kind::App, i, entry.name, entry.parsingName, {}, true});
                }
                if (HasQuickNavigationAppExpandButton()) rows.push_back({Kind::ExpandApps, 0, _LW("quickNav.more"), {}, {}, true});
            }
        }
        if (quickNavigationSearchType_ == QuickNavigationSearchType::All || quickNavigationSearchType_ == QuickNavigationSearchType::File)
        {
            header(ScopeLabel(QuickNavigationSearchType::File));
            for (size_t i = 0; i < quickNavigationEverythingResults_.size(); ++i)
            {
                const auto& entry = quickNavigationEverythingResults_[i];
                rows.push_back({Kind::Everything, i, entry.name, entry.path, {}, true});
            }
            if (quickNavigationEverythingSearchPending_) notice(_LW("quickNav.files.loading"));
            else if (!everythingSearchAvailable_) notice(GetQuickNavigationEverythingNoticeText());
            if (HasQuickNavigationEverythingLoadMoreButton()) rows.push_back({Kind::LoadMore, 0, _LW("quickNav.more"), {}, {}, true});
        }
    }
    if (std::none_of(rows.begin(), rows.end(), [](const auto& row) {return row.kind != Kind::Header;})) notice(_LW("quickNav.empty"));
    if (!quickNavigationActionNotice_.empty()) notice(quickNavigationActionNotice_);
    return rows;
}
RECT DesktopApp::GetQuickNavigationListRowRect(size_t index) const
{
    const RECT content = GetQuickNavigationContentRect(quickNavigationRect_);
    int top = content.top - quickNavigationScrollOffset_;
    if (quickNavigationMenu_ != QuickNavigationMenu::None)
    {
        top += static_cast<int>(index) * QuickNavScale(navigationSettings_.layout.resultRowHeight);
        return MakeRect(content.left, top, content.right, top + QuickNavScale(navigationSettings_.layout.resultRowHeight));
    }
    const auto rows = BuildQuickNavigationListRows();
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const int height = QuickNavScale(rows[i].kind == QuickNavigationListRow::Kind::Header ? 28 : navigationSettings_.layout.resultRowHeight);
        if (i == index) return MakeRect(content.left, top, content.right, top + height);
        top += height;
    }
    return {};
}
void DesktopApp::DrawQuickNavigationList(ID2D1DeviceContext* context)
{
    using Kind = QuickNavigationListRow::Kind;
    const auto theme = ResolveQuickNavTheme(quickNavLightTheme_, navigationSettings_);
    const auto rows = BuildQuickNavigationListRows();
    const auto model = BuildQuickNavigationContentModel();
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const auto& row = rows[i];
        RECT bounds = GetQuickNavigationListRowRect(i);
        if (bounds.bottom <= quickNavigationRect_.top || bounds.top >= quickNavigationRect_.bottom) continue;
        const bool selected = quickNavigationListSelection_ == static_cast<int>(i);
        const bool hovered = row.enabled && PtInRect(&bounds, quickNavigationLastMousePoint_);
        if (row.kind == Kind::Header)
        {
            bounds.left += QuickNavScale(10);
            DrawD2DTextEllipsis(context, row.title, bounds, quickNavPathTextFormat_.Get(), ToD2DColor(theme.headerText));
            continue;
        }
        DrawD2DRoundedRectangle(context, bounds, static_cast<float>(QuickNavScale(navigationSettings_.layout.itemRadius)),
            ToD2DColor(selected ? theme.selectedFill : hovered ? theme.appRowHoverFill : theme.resultFill, selected || hovered ? 1.f : 0.22f),
            ToD2DColor(selected ? theme.selectedBorder : hovered ? theme.appRowHoverStroke : theme.resultBorder, selected || hovered ? 1.f : 0.f));
        const int size = QuickNavScale(28);
        RECT icon = MakeRect(bounds.left + QuickNavScale(12), (bounds.top + bounds.bottom - size) / 2, bounds.left + QuickNavScale(12) + size, (bounds.top + bounds.bottom + size) / 2);
        if (row.kind == Kind::App && row.index < quickNavigationAppResultIndices_.size())
            DrawQuickNavAppIcon(context, quickNavigationAppEntries_[quickNavigationAppResultIndices_[row.index]], icon);
        else if (row.kind == Kind::Everything && row.index < quickNavigationEverythingResults_.size())
            DrawQuickNavSysIcon(context, quickNavigationEverythingResults_[row.index].systemIconIndex, icon);
        else if (row.kind == Kind::Item && row.index < model.entries.size() && model.entries[row.index].itemIndex < items_.size())
        {
            const auto& item = items_[model.entries[row.index].itemIndex];
            if (item.iconBitmap) DrawIconBitmap(context, GetOrCreateD2DBitmap(context, item.iconBitmap, ShouldBeautifyIconBitmap(false)), icon);
            else DrawQuickNavSysIcon(context, item.sysIconIndex, icon);
        }
        else if (row.kind == Kind::Item && row.index < model.entries.size())
        {
            const auto& entry = model.entries[row.index];
            if (entry.widgetIndex < widgets_.size() && entry.folderEntryIndex < widgets_[entry.widgetIndex].folderEntries.size())
            {
                const auto& item = widgets_[entry.widgetIndex].folderEntries[entry.folderEntryIndex];
                if (item.iconBitmap) DrawIconBitmap(context, GetOrCreateD2DBitmap(context,item.iconBitmap,ShouldBeautifyIconBitmap(false)),icon);
                else DrawQuickNavSysIcon(context,item.sysIconIndex,icon);
            }
        }
        else if (row.kind != Kind::Notice)
            DrawD2DText(context, kScopeGlyphs[static_cast<size_t>(quickNavigationSearchType_)], icon, quickNavFluentTextFormat_.Get(), ToD2DColor(theme.typeText));
        RECT title = bounds;
        title.left = row.kind == Kind::Notice ? bounds.left + QuickNavScale(12) : icon.right + QuickNavScale(12);
        title.right -= QuickNavScale(12);
        if (!row.detail.empty()) title.bottom = bounds.top + (bounds.bottom - bounds.top) / 2 + QuickNavScale(2);
        DrawD2DTextEllipsis(context, row.title, title, quickNavItemTextFormat_.Get(), ToD2DColor(row.enabled ? (selected ? theme.selectedText : theme.appNameText) : theme.emptyText));
        if (!row.detail.empty())
        {
            RECT detail = title; detail.top = title.bottom - QuickNavScale(2); detail.bottom = bounds.bottom - QuickNavScale(4);
            DrawD2DTextEllipsis(context, row.detail, detail, quickNavPathTextFormat_.Get(), ToD2DColor(theme.appTypeText));
        }
    }
}
void DesktopApp::DrawQuickNavigationMenus(ID2D1DeviceContext* context)
{
    const auto theme = ResolveQuickNavTheme(quickNavLightTheme_, navigationSettings_);
    const auto format = quickNavFluentTextFormat_.Get();
    for (int button = 0; button < 3; ++button)
    {
        RECT bounds = GetQuickNavigationToolbarRect(quickNavigationRect_, button);
        const bool hovered = PtInRect(&bounds, quickNavigationLastMousePoint_) != FALSE;
        const bool chip = button == 0 && quickNavigationSearchType_ != QuickNavigationSearchType::All;
        if (hovered || chip) DrawD2DRoundedRectangle(context, bounds, static_cast<float>(QuickNavScale(6)),
            ToD2DColor(chip ? theme.typeFill : theme.tabHoverFill), ToD2DColor(theme.searchBorder, 0.f));
        const wchar_t* glyph = button == 0 ? kScopeGlyphs[static_cast<size_t>(quickNavigationSearchType_)] : button == 1 ? L"\uF6A9" : (quickNavigationCollapsed_ ? L"\uF2A4" : L"\uF2B7");
        RECT symbol = bounds; if (chip) symbol.right = symbol.left + QuickNavScale(28);
        DrawD2DText(context, glyph, symbol, format, ToD2DColor(chip ? theme.typeText : theme.searchPlaceholder));
        if (chip)
        {
            RECT label = bounds; label.left += QuickNavScale(30); label.right -= QuickNavScale(24);
            DrawD2DTextEllipsis(context, QuickNavigationTypeLabel(), label, quickNavPathTextFormat_.Get(), ToD2DColor(theme.typeText));
            RECT dismiss = bounds; dismiss.left = dismiss.right - QuickNavScale(24);
            DrawD2DText(context, L"\uF369", dismiss, format, ToD2DColor(theme.typeText));
        }
    }
    if (quickNavigationMenu_ == QuickNavigationMenu::None) return;
    const size_t count = quickNavigationMenu_ == QuickNavigationMenu::Types ? 7 : 3;
    const RECT content = GetQuickNavigationContentRect(quickNavigationRect_);
    context->PushAxisAlignedClip(ToD2DRect(content), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (size_t i = 0; i < count; ++i)
    {
        RECT bounds = GetQuickNavigationListRowRect(i);
        const bool selected = quickNavigationMenuSelection_ == static_cast<int>(i);
        const bool hover = PtInRect(&bounds, quickNavigationLastMousePoint_) != FALSE;
        if (selected || hover) DrawD2DRoundedRectangle(context, bounds, static_cast<float>(QuickNavScale(navigationSettings_.layout.itemRadius)),
            ToD2DColor(selected ? theme.selectedFill : theme.appRowHoverFill), ToD2DColor(theme.selectedBorder, selected ? 1.f : 0.f));
        RECT icon = bounds; icon.right = icon.left + QuickNavScale(44);
        const auto glyph = quickNavigationMenu_ == QuickNavigationMenu::Types ? std::wstring(kScopeGlyphs[i]) :
            std::wstring(snowdesktop::quick_navigation_rules::QuickNavigationDesktopViewModeGlyph(static_cast<QuickNavigationDesktopViewMode>(i)));
        DrawD2DText(context, glyph, icon, format, ToD2DColor(theme.typeText));
        RECT label = bounds; label.left = icon.right + QuickNavScale(8); label.right -= QuickNavScale(90);
        constexpr const char* views[] = {"app.nav.view_tile", "app.nav.view_source", "app.nav.view_initial"};
        DrawD2DTextEllipsis(context, quickNavigationMenu_ == QuickNavigationMenu::Types ? ScopeLabel(static_cast<QuickNavigationSearchType>(i)) : _LW(views[i]), label, quickNavItemTextFormat_.Get(), ToD2DColor(theme.appNameText));
        if (quickNavigationMenu_ == QuickNavigationMenu::Types && i > 0)
        {
            RECT prefix = bounds; prefix.left = prefix.right - QuickNavScale(80); prefix.right -= QuickNavScale(12);
            DrawD2DText(context, Utf8ToWide(navigationSettings_.prefixes[i - 1]), prefix, quickNavPathTextFormat_.Get(), ToD2DColor(theme.appTypeText));
        }
    }
    context->PopAxisAlignedClip();
}
bool DesktopApp::HandleQuickNavigationToolbarClick(POINT point)
{
    for (int button = 0; button < 3; ++button)
    {
        const RECT bounds = GetQuickNavigationToolbarRect(quickNavigationRect_, button);
        if (!PtInRect(&bounds, point)) continue;
        if (button == 2) ToggleQuickNavigationCollapsed();
        else if (button == 1) CloseQuickNavigationThen([this]() { ShowSettingsWindow(snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::QuickNavigation)); });
        else if (quickNavigationSearchType_ != QuickNavigationSearchType::All && point.x >= bounds.right - QuickNavScale(24)) SetQuickNavigationSearchScope(QuickNavigationSearchType::All);
        else
        {
            quickNavigationMenu_ = quickNavigationMenu_ == QuickNavigationMenu::Types ? QuickNavigationMenu::None : QuickNavigationMenu::Types;
            quickNavigationMenuSelection_ = static_cast<int>(quickNavigationSearchType_);
            quickNavigationScrollOffset_ = 0;
            PositionQuickNavigationWindow();
            InvalidateQuickNavigationWindow();
        }
        return true;
    }
    return false;
}
bool DesktopApp::HandleQuickNavigationListClick(POINT point, bool contextMenu, POINT screenPoint)
{
    using Kind = QuickNavigationListRow::Kind;
    const RECT content = GetQuickNavigationContentRect(quickNavigationRect_);
    if (!PtInRect(&content, point)) return true;
    const auto rows = BuildQuickNavigationListRows();
    const size_t count = quickNavigationMenu_ == QuickNavigationMenu::Types ? 7 : quickNavigationMenu_ == QuickNavigationMenu::Views ? 3 : rows.size();
    for (size_t i = 0; i < count; ++i)
    {
        const RECT bounds = GetQuickNavigationListRowRect(i);
        if (!PtInRect(&bounds, point)) continue;
        if (quickNavigationMenu_ != QuickNavigationMenu::None)
        {
            if (quickNavigationMenu_ == QuickNavigationMenu::Types) SetQuickNavigationSearchScope(static_cast<QuickNavigationSearchType>(i));
            else { quickNavigationMenu_ = QuickNavigationMenu::None; SetQuickNavigationDesktopViewMode(static_cast<QuickNavigationDesktopViewMode>(i)); PositionQuickNavigationWindow(); }
        }
        else if (!contextMenu) { quickNavigationListSelection_ = static_cast<int>(i); ActivateQuickNavigationListRow(i); }
        else if (rows[i].kind == Kind::App) ShowQuickNavigationAppContextMenu(quickNavigationAppEntries_[quickNavigationAppResultIndices_[rows[i].index]], screenPoint);
        else if (rows[i].kind == Kind::Everything) ShowQuickNavigationEverythingContextMenu(quickNavigationEverythingResults_[rows[i].index], screenPoint);
        else if (rows[i].kind == Kind::Item)
        {
            const auto model = BuildQuickNavigationContentModel();
            const auto& entry = model.entries[rows[i].index];
            ClearSelection();
            if (entry.kind == QuickNavigationEntry::Kind::DesktopItem && entry.itemIndex < items_.size())
            { items_[entry.itemIndex].selected = true; ShowItemContextMenu(screenPoint,static_cast<int>(entry.itemIndex),false,true); }
            else if (entry.widgetIndex < widgets_.size() && entry.folderEntryIndex < widgets_[entry.widgetIndex].folderEntries.size())
            { widgets_[entry.widgetIndex].folderEntries[entry.folderEntryIndex].selected = true; ShowFolderEntryContextMenu(screenPoint,entry.widgetIndex,entry.folderEntryIndex,true); }
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
    if (row.kind == Kind::Calculator)
    {
        quickNavigationActionNotice_ = _LW(CopyTextToClipboard(row.value) ? "quickNav.copied" : "quickNav.copyFailed");
        if (quickNavigationCollapsed_) PositionQuickNavigationWindow();
        InvalidateQuickNavigationWindow();
    }
    else if (row.kind == Kind::Settings)
    {
        // Rebuild immediately before opening so removed/hidden widget fields cannot be executed.
        const auto desired = quickNavigationSettingsResults_[row.index];
        const auto current = snowdesktop::SettingsSearchIndex(BuildSettingsSearchInput()).Search(GetQuickNavigationEffectiveSearchText());
        const bool available = std::any_of(current.begin(), current.end(), [&](const auto& hit) { return hit.route == desired.route && hit.focusId == desired.focusId; });
        if (available) CloseQuickNavigationThen([this, route = desired.route]() { ShowSettingsWindow(route); });
        else { RefreshQuickNavigationTypedResults(); InvalidateQuickNavigationWindow(); }
    }
    else if (row.kind == Kind::App) CloseQuickNavigationThenLaunchApp(quickNavigationAppEntries_[quickNavigationAppResultIndices_[row.index]]);
    else if (row.kind == Kind::Item)
    {
        const auto model = BuildQuickNavigationContentModel();
        if (row.index >= model.entries.size()) return false;
        const auto entry = model.entries[row.index];
        if (entry.kind == QuickNavigationEntry::Kind::DesktopItem)
            CloseQuickNavigationThen([this, index = entry.itemIndex]() { LaunchDesktopItem(index, true); });
        else CloseQuickNavigationThen([this, path = entry.path]() { LaunchPathWithShortcutPolicy(nullptr, path); });
    }
    else if (row.kind == Kind::ExpandApps) { quickNavigationAppsExpanded_ = true; PositionQuickNavigationWindow(); InvalidateQuickNavigationWindow(); }
    else if (row.kind == Kind::LoadMore) { quickNavigationEverythingResultLimit_ += kQuickNavigationEverythingResultBatchSize; RefreshQuickNavigationEverythingResults(); InvalidateQuickNavigationWindow(); }
    else
    {
        const auto value = row.kind == Kind::Everything ? quickNavigationEverythingResults_[row.index].path : row.value;
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
    if (!quickNavigationOpen_ || snowdesktop::text_input::IsComposing(quickNavigationSearchEdit_) || !quickNavigationSearchCompositionText_.empty()) return false;
    if (key == 'E' && (GetKeyState(VK_CONTROL) & 0x8000)) { ToggleQuickNavigationCollapsed(); return true; }
    if (key == VK_ESCAPE)
    {
        if (quickNavigationMenu_ != QuickNavigationMenu::None) { quickNavigationMenu_ = QuickNavigationMenu::None; PositionQuickNavigationWindow(); InvalidateQuickNavigationWindow(); return true; }
        if (quickNavigationSearchType_ != QuickNavigationSearchType::All) { SetQuickNavigationSearchScope(QuickNavigationSearchType::All); return true; }
        return false;
    }
    if (key == VK_BACK && GetQuickNavigationEffectiveSearchText().empty() && quickNavigationSearchType_ != QuickNavigationSearchType::All)
    { SetQuickNavigationSearchScope(QuickNavigationSearchType::All); return true; }
    if (key == VK_RETURN && !IsLuaLogicalSlotPickerOpen() && snowdesktop::quick_navigation_query::GetSubmitIntent(navigationSettings_,quickNavigationSearchType_,quickNavigationSearchText_,false,quickNavigationMenu_ != QuickNavigationMenu::None) == snowdesktop::quick_navigation_query::SubmitIntent::ConfirmPrefix)
    {
        if (const auto scope = snowdesktop::quick_navigation_query::ResolvePrefix(navigationSettings_, quickNavigationSearchText_))
        {
            SetWindowTextW(quickNavigationSearchEdit_, L"");
            quickNavigationSearchText_.clear(); quickNavigationEffectiveSearchText_.clear();
            SetQuickNavigationSearchScope(scope->type, scope->engine); return true;
        }
    }
    if (key != VK_UP && key != VK_DOWN && key != VK_RETURN) return false;
    if (quickNavigationMenu_ != QuickNavigationMenu::None)
    {
        const int count = quickNavigationMenu_ == QuickNavigationMenu::Types ? 7 : 3;
        if (key == VK_RETURN)
        {
            if (quickNavigationMenu_ == QuickNavigationMenu::Types) SetQuickNavigationSearchScope(static_cast<QuickNavigationSearchType>(quickNavigationMenuSelection_));
            else { quickNavigationMenu_ = QuickNavigationMenu::None; SetQuickNavigationDesktopViewMode(static_cast<QuickNavigationDesktopViewMode>(quickNavigationMenuSelection_)); PositionQuickNavigationWindow(); }
        }
        else quickNavigationMenuSelection_ = (quickNavigationMenuSelection_ + (key == VK_DOWN ? 1 : count - 1)) % count;
        InvalidateQuickNavigationWindow(); return true;
    }
    if (!UseQuickNavigationList()) return key != VK_RETURN && HandleQuickNavigationKeyboardInput(key);
    const auto rows = BuildQuickNavigationListRows();
    if (key == VK_RETURN)
    {
        if (quickNavigationListSelection_ >= 0 && ActivateQuickNavigationListRow(static_cast<size_t>(quickNavigationListSelection_))) return true;
        for (size_t i = 0; i < rows.size(); ++i) if (rows[i].enabled) { ActivateQuickNavigationListRow(i); break; }
        return true;
    }
    if (rows.empty()) return true;
    const int count = static_cast<int>(rows.size());
    int next = quickNavigationListSelection_;
    for (int attempts = 0; attempts < count; ++attempts)
    {
        next = key == VK_DOWN ? (next + 1) % count : (next <= 0 ? count - 1 : next - 1);
        if (rows[static_cast<size_t>(next)].enabled) { quickNavigationListSelection_ = next; break; }
    }
    if (quickNavigationListSelection_ >= 0) EnsureQuickNavigationKeyboardTargetVisible(GetQuickNavigationListRowRect(static_cast<size_t>(quickNavigationListSelection_)));
    InvalidateQuickNavigationWindow(); return true;
}

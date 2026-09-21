#include "pch.h"
#include "context_menu_page_presenter.h"
#include "../shell_extension_service.h"
#include "../shell_extension_management.h"
#include <array>
#include <cwctype>
#include <set>
#include <shobjidl.h>
#include <winrt/Windows.Storage.Streams.h>
#include <wrl/client.h>

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = mux::Controls;
namespace ext = snowdesktop::shell_extensions;
struct ContextMenuPagePresenter::Impl : std::enable_shared_from_this<Impl>
{
    struct Row
    {
        ext::ManagementRow model;
        muxc::Expander expander;
        muxc::Border icon;
        muxc::TextBlock title, scope, mixed;
        muxc::ToggleSwitch toggle;
        muxc::StackPanel positionsPanel;
        struct Position { ext::Context context; muxc::Grid layout; muxc::ComboBox choice; std::function<void()> revoke; };
        std::vector<Position> positions;
        std::vector<std::function<void()>> revoke;
        std::string group;
    };
    LocalizeCallback localize;
    PersonalizationPageActions actions;
    mux::Style cardStyle;
    muxc::StackPanel root, content, groups;
    muxc::Border managementCard;
    muxc::SelectorBar tabs;
    muxc::SelectorBarItem objectsTab, backgroundTab;
    muxc::Grid toolbar;
    muxc::TextBox search;
    muxc::Button more;
    muxc::MenuFlyoutItem refresh, chooseObject, chooseFolder, chooseDesktop;
    muxc::SelectorBar views;
    muxc::SelectorBarItem byObject, byApplication, byExtension;
    ext::ManagementView mode = ext::ManagementView::Objects;
    std::string routeFocus;
    std::vector<ext::ManagementRow> filteredRows;
    muxc::TextBlock heading, status, refreshStatus;
    muxc::StackPanel refreshIndicator;
    muxc::ProgressRing refreshRing;
    mux::DispatcherTimer timer, rowTimer;
    std::vector<ext::ManagementRow> pendingRows;
    struct Card
    {
        muxc::StackPanel panel;
        muxc::TextBlock title, mixed;
        muxc::ToggleSwitch toggle;
        std::function<void()> revoke;
    };
    std::map<std::string, Card> sections;
    std::map<std::string, ext::ManagementCard> cardModels;
    std::vector<std::string> cardOrder;
    std::map<std::string, std::string> rowGroups;
    size_t nextRow = 0;
    std::vector<std::function<void()>> revoke;
    std::vector<Row> rows;
    ext::Preferences prefs;
    ext::Request request;
    ext::CatalogueView view;
    ext::Category category = ext::Category::Objects;
    ext::Category rowCategory = ext::Category::Objects;
    bool hasVisibleRows = false;
    bool extensionFocus = false;
    std::uint64_t generation = 0;
    bool active = false, closed = false, updating = false, initialized = false, routed = false, queryFailed = false;
    std::wstring L(std::string_view key) const { return localize ? localize(key) : std::wstring{}; }
    static void Column(muxc::Grid grid, double width, mux::GridUnitType unit)
    {
        muxc::ColumnDefinition column; column.Width({width, unit}); grid.ColumnDefinitions().Append(column);
    }
    explicit Impl(LocalizeCallback l, const mux::Style &style) : localize(std::move(l)), cardStyle(style)
    {
        root.Spacing(12); content.Spacing(12); groups.Spacing(12);
        if (cardStyle) managementCard.Style(cardStyle);
        heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        heading.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::StackPanel titleRow; titleRow.Orientation(muxc::Orientation::Horizontal); titleRow.Spacing(12);
        titleRow.Children().Append(heading);
        refreshIndicator.Orientation(muxc::Orientation::Horizontal); refreshIndicator.Spacing(6);
        refreshIndicator.VerticalAlignment(mux::VerticalAlignment::Center);
        refreshRing.Width(16); refreshRing.Height(16); refreshRing.MinWidth(0); refreshRing.MinHeight(0);
        refreshRing.IsIndeterminate(true);
        refreshStatus.FontSize(12); refreshStatus.Opacity(0.7); refreshStatus.VerticalAlignment(mux::VerticalAlignment::Center);
        refreshIndicator.Children().Append(refreshRing); refreshIndicator.Children().Append(refreshStatus);
        titleRow.Children().Append(refreshIndicator); content.Children().Append(titleRow);
        SetRefreshing(false);
        tabs.Items().Append(objectsTab); tabs.Items().Append(backgroundTab); tabs.SelectedItem(objectsTab);
        views.Items().Append(byObject); views.Items().Append(byApplication); views.Items().Append(byExtension);
        views.SelectedItem(byObject);
        muxc::Grid selectors; selectors.ColumnSpacing(16);
        Column(selectors, 1, mux::GridUnitType::Auto); Column(selectors, 1, mux::GridUnitType::Star);
        tabs.HorizontalAlignment(mux::HorizontalAlignment::Left);
        views.HorizontalAlignment(mux::HorizontalAlignment::Right);
        muxc::Grid::SetColumn(views, 1);
        selectors.Children().Append(tabs); selectors.Children().Append(views); content.Children().Append(selectors);
        Column(toolbar, 1, mux::GridUnitType::Star); Column(toolbar, 1, mux::GridUnitType::Auto);
        search.Margin({0, 0, 8, 0}); toolbar.Children().Append(search);
        muxc::Grid::SetColumn(more, 1); toolbar.Children().Append(more); content.Children().Append(toolbar);
        muxc::MenuFlyout advanced;
        advanced.Items().Append(chooseObject); advanced.Items().Append(chooseFolder); advanced.Items().Append(chooseDesktop);
        advanced.Items().Append(muxc::MenuFlyoutSeparator()); advanced.Items().Append(refresh); more.Flyout(advanced);
        status.TextWrapping(mux::TextWrapping::Wrap); status.Visibility(mux::Visibility::Collapsed); content.Children().Append(status);
        managementCard.Child(content); root.Children().Append(managementCard); root.Children().Append(groups);
        timer.Interval(std::chrono::milliseconds(300));
        rowTimer.Interval(std::chrono::milliseconds(16));
        auto append = rowTimer.Tick([this](auto &&, auto &&) { AppendRows(); });
        revoke.push_back([t = rowTimer, append] { t.Tick(append); });
        auto tick = timer.Tick([this](auto &&, auto &&) { Poll(); }); revoke.push_back([t = timer, tick] { t.Tick(tick); });
        auto changed = tabs.SelectionChanged([this](auto &&, auto &&) {
            if (closed || updating) return;
            category = tabs.SelectedItem() == backgroundTab ? ext::Category::Background : ext::Category::Objects;
            Text(); BuildRows();
        }); revoke.push_back([t = tabs, changed] { t.SelectionChanged(changed); });
        auto filter = search.TextChanged([this](auto &&, auto &&) { if (!closed && !updating) BuildRows(); });
        revoke.push_back([c = search, filter] { c.TextChanged(filter); });
        const auto changedView = views.SelectionChanged([this](auto &&, auto &&) {
            if (closed || updating) return;
            mode = views.SelectedItem() == byApplication ? ext::ManagementView::Applications :
                views.SelectedItem() == byExtension ? ext::ManagementView::Extensions : ext::ManagementView::Objects;
            ClearRows(); BuildRows();
        }); revoke.push_back([c = views, changedView] { c.SelectionChanged(changedView); });
        auto reload = refresh.Click([this](auto &&, auto &&) { if (active && !closed) Reload(true); });
        revoke.push_back([c = refresh, reload] { c.Click(reload); });
        auto pick = chooseObject.Click([this](auto &&, auto &&) { if (active && !closed) Pick(false); });
        revoke.push_back([c = chooseObject, pick] { c.Click(pick); });
        auto folders = chooseFolder.Click([this](auto &&, auto &&) { if (active && !closed) Pick(true); });
        revoke.push_back([c = chooseFolder, folders] { c.Click(folders); });
        auto desktop = chooseDesktop.Click([this](auto &&, auto &&) {
            request = {}; request.context = ext::Context::Desktop; request.background = true; routed = false; Reload(true);
        }); revoke.push_back([c = chooseDesktop, desktop] { c.Click(desktop); });
        Text();
    }
    void Text()
    {
        heading.Text(L("settings.contextMenu.extensions"));
        refreshStatus.Text(L("settings.contextMenu.refreshing"));
        mux::Automation::AutomationProperties::SetName(refreshRing, L("settings.contextMenu.refreshing"));
        objectsTab.Text(L("settings.contextMenu.objects")); backgroundTab.Text(L("settings.contextMenu.background"));
        search.PlaceholderText(L("settings.contextMenu.search")); refresh.Text(L("settings.contextMenu.refresh"));
        more.Content(winrt::box_value(L("settings.contextMenu.more")));
        byObject.Text(L("settings.contextMenu.viewObjects")); byApplication.Text(L("settings.contextMenu.viewApplications"));
        byExtension.Text(L("settings.contextMenu.viewExtensions"));
        byExtension.Visibility(category == ext::Category::Objects ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        if (category == ext::Category::Background && mode == ext::ManagementView::Extensions)
        {
            mode = ext::ManagementView::Objects;
            const bool before = std::exchange(updating, true); views.SelectedItem(byObject); updating = before;
        }
        chooseObject.Text(L("settings.contextMenu.inspectFiles")); chooseFolder.Text(L("settings.contextMenu.inspectFolders"));
        chooseDesktop.Text(L("settings.contextMenu.inspectDesktop"));
        chooseDesktop.Visibility(category == ext::Category::Background ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        chooseObject.Visibility(category == ext::Category::Objects ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    }
    void SetRefreshing(bool busy)
    {
        refreshRing.IsActive(busy);
        refreshIndicator.Visibility(busy ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    }
    void Cancel() { timer.Stop(); rowTimer.Stop(); SetRefreshing(false); }
    void Status(const std::wstring &text) { status.Text(text); status.Visibility(text.empty() ? mux::Visibility::Collapsed : mux::Visibility::Visible); }
    void Reload(bool refreshCatalogue = false)
    {
        if (!active || closed || !actions.contextMenu) return;
        try
        {
            view = actions.contextMenu(request, refreshCatalogue);
            queryFailed = false;
            if (!routed && !view.selection.paths.empty())
            {
                request = view.selection;
                category = !extensionFocus && (request.background || request.context == ext::Context::Desktop) ? ext::Category::Background : ext::Category::Objects;
                updating = true; tabs.SelectedItem(category == ext::Category::Objects ? objectsTab : backgroundTab); updating = false;
                routed = true; Text();
            }
            ext::MigrateAssociations(prefs, view.catalogue);
            BuildRows();
            if (view.scanning || view.menu.pending) timer.Start(); else timer.Stop();
        }
        catch (...) { queryFailed = true; SetRefreshing(false); Status(L("settings.contextMenu.failed")); timer.Stop(); }
    }
    void Poll()
    {
        if (!active || closed || !actions.contextMenu) return;
        try
        {
            auto next = actions.contextMenu(request, false);
            queryFailed = false;
            const bool changed = next.catalogue.revision != view.catalogue.revision;
            if (request.paths.empty() && next.selection.context == ext::Context::Desktop && !next.selection.paths.empty()) request = next.selection;
            view = std::move(next);
            if (changed) { ext::MigrateAssociations(prefs, view.catalogue); BuildRows(); }
            UpdateStatus();
            if (!view.scanning && !view.menu.pending) timer.Stop();
        }
        catch (...) { queryFailed = true; SetRefreshing(false); Status(L("settings.contextMenu.failed")); timer.Stop(); }
    }
    void Pick(bool folders)
    {
        const auto lifetime = shared_from_this();
        Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
        DWORD options = 0; dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_ALLOWMULTISELECT | (folders ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
        if (FAILED(dialog->Show(GetActiveWindow())) || closed || !active) return;
        Microsoft::WRL::ComPtr<IShellItemArray> selected;
        if (FAILED(dialog->GetResults(&selected))) return;
        DWORD count = 0; selected->GetCount(&count); request.paths.clear();
        for (DWORD i = 0; i < count && i < 256; ++i)
        {
            Microsoft::WRL::ComPtr<IShellItem> item; PWSTR path = nullptr;
            if (SUCCEEDED(selected->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            { request.paths.emplace_back(path); CoTaskMemFree(path); }
        }
        request.context = category == ext::Category::Background ? ext::Context::FolderBackground : ext::Context::Automatic;
        request.background = category == ext::Category::Background;
        // A background menu has exactly one directory; multi-select applies to objects.
        if (request.background && request.paths.size() > 1) request.paths.resize(1);
        Reload(true);
    }
    mux::UIElement Icon(const ext::Entry &entry)
    {
        if (entry.width > 0 && entry.height > 0 && entry.width <= 128 && entry.height <= 128 &&
            entry.pixels.size() == static_cast<size_t>(entry.width * entry.height * 4))
        {
            mux::Media::Imaging::WriteableBitmap bitmap(entry.width, entry.height);
            memcpy(bitmap.PixelBuffer().data(), entry.pixels.data(), entry.pixels.size());
            bitmap.Invalidate();
            muxc::Image image;
            image.Width(20);
            image.Height(20);
            image.Source(bitmap);
            return image;
        }
        muxc::FontIcon icon;
        icon.Glyph(L"\xE8A7");
        icon.FontSize(18);
        return icon;
    }
    void UpdateStatus()
    {
        // Report actual service work, not incremental construction of cached rows.
        const bool refreshing = active && !closed && !queryFailed && (view.scanning || view.menu.pending);
        SetRefreshing(refreshing);
        if (hasVisibleRows) Status(L"");
        else if (queryFailed) Status(L("settings.contextMenu.failed"));
        else if (HasFilters()) Status(L("settings.contextMenu.noMatches"));
        else if (refreshing) Status(L"");
        else Status(L(view.menu.error.empty() ? "settings.contextMenu.empty" : "settings.contextMenu.failed"));
    }
    bool HasFilters() const { return !search.Text().empty(); }
    std::wstring CardTitle(const ext::ManagementCard &card) const
    {
        return card.titleKey.empty() ? card.title : L(card.titleKey);
    }
    void SetCardResults(const std::string &id, bool shown)
    {
        if (closed || !active || !initialized || !cardModels.contains(id) || !actions.updateGeneral) return;
        const auto targets = cardModels.at(id).rows; const auto catalogue = view.catalogue;
        actions.updateGeneral(generation, SettingsUpdateMode::PreviewAndCommit, [targets, catalogue, scope = category, shown](auto &settings) {
            ext::MigrateAssociations(settings.shellExtensions, catalogue);
            ext::SetManagementResults(settings.shellExtensions, targets, scope, shown);
        });
    }
    void UpdateCardStates()
    {
        const bool before = std::exchange(updating, true);
        for (auto &[id, card] : sections) if (cardModels.contains(id))
        {
            const auto &model = cardModels.at(id); const auto state = ext::ManagementCardState(prefs, model, category);
            card.title.Text(CardTitle(model)); card.toggle.IsOn(state.value_or(true));
            card.toggle.IsEnabled(active && initialized && !model.rows.empty());
            card.mixed.Visibility(state ? mux::Visibility::Collapsed : mux::Visibility::Visible);
            mux::Automation::AutomationProperties::SetName(card.toggle, CardTitle(model) + L" " + L("settings.contextMenu.groupToggle"));
        }
        updating = before;
    }
    Row *FindRow(const std::string &id)
    {
        const auto it = std::find_if(rows.begin(), rows.end(), [&](const auto &row) { return row.model.id == id; });
        return it == rows.end() ? nullptr : &*it;
    }
    static void RemoveChild(muxc::Panel panel, mux::UIElement child)
    {
        uint32_t index = 0;
        if (panel.Children().IndexOf(child, index)) panel.Children().RemoveAt(index);
    }
    void RemoveRow(Row &row)
    {
        for (auto &r : row.revoke) r();
        for (auto &p : row.positions) p.revoke();
        if (sections.contains(row.group)) RemoveChild(sections.at(row.group).panel, row.expander);
    }
    void ClearRows()
    {
        rowTimer.Stop();
        for (auto &row : rows) RemoveRow(row);
        for (auto &[id, card] : sections) if (card.revoke) card.revoke();
        rows.clear(); groups.Children().Clear(); sections.clear(); cardModels.clear(); rowGroups.clear(); pendingRows.clear(); nextRow = 0;
    }
    void BuildRows()
    {
        rowTimer.Stop();
        // Only explicit category/localization changes reset the list. Polling,
        // refresh and search reconcile identities and preserve surviving rows.
        if (rowCategory != category) { ClearRows(); rowCategory = category; }
        filteredRows = ext::ManagementRows(view.catalogue, category, std::wstring(search.Text()));
        hasVisibleRows = !filteredRows.empty();
        auto cards = ext::ManagementCards(filteredRows, category, mode);
        std::vector<ext::ManagementRow> visible;
        cardModels.clear(); cardOrder.clear(); rowGroups.clear();
        for (auto &model : cards)
        {
            cardOrder.push_back(model.id);
            for (const auto &row : model.rows) { rowGroups[row.id] = model.id; visible.push_back(row); }
            cardModels.emplace(model.id, std::move(model));
        }
        pendingRows = ext::PlanManagementUpdates(rows, visible, [this](auto &row) { RemoveRow(row); });
        nextRow = 0;
        UpdateChoices();
        AppendRows();
        if (nextRow < pendingRows.size()) rowTimer.Start();
        UpdateStatus();
    }
    muxc::StackPanel Section(const std::string &group)
    {
        if (!sections.contains(group))
        {
            Card card; card.panel.Spacing(4);
            muxc::Grid header; Column(header, 1, mux::GridUnitType::Star); Column(header, 1, mux::GridUnitType::Auto);
            muxc::StackPanel names; names.VerticalAlignment(mux::VerticalAlignment::Center); names.Margin({0, 0, 16, 0});
            card.title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold()); card.title.TextWrapping(mux::TextWrapping::Wrap);
            names.Children().Append(card.title); card.mixed.Text(L("settings.contextMenu.groupMixed")); card.mixed.FontSize(12);
            card.mixed.Opacity(0.7); names.Children().Append(card.mixed); header.Children().Append(names);
            card.toggle.OnContent(winrt::box_value(L"")); card.toggle.OffContent(winrt::box_value(L"")); card.toggle.MinWidth(0);
            card.toggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
            card.toggle.VerticalAlignment(mux::VerticalAlignment::Center); muxc::Grid::SetColumn(card.toggle, 1);
            // The native switch already includes a 12-DIP trailing content gap.
            header.Children().Append(card.toggle); header.Margin({12, 8, 0, 12}); card.panel.Children().Append(header);
            muxc::ToolTipService::SetToolTip(card.toggle, winrt::box_value(L("settings.contextMenu.groupHint")));
            const auto changed = card.toggle.Toggled([this, group](auto &&, auto &&) {
                if (!updating && sections.contains(group)) SetCardResults(group, sections.at(group).toggle.IsOn());
            }); card.revoke = [toggle = card.toggle, changed] { toggle.Toggled(changed); };
            muxc::Border groupCard; if (cardStyle) groupCard.Style(cardStyle); groupCard.Child(card.panel);
            // Discovery can identify an application after the unknown card is
            // already visible. Insert the new card in view order without
            // recreating the existing cards or their expanded rows.
            uint32_t insertion = groups.Children().Size();
            bool following = false;
            for (const auto &id : cardOrder)
            {
                if (id == group) { following = true; continue; }
                if (!following || !sections.contains(id)) continue;
                const auto sibling = sections.at(id).panel.Parent().as<mux::UIElement>();
                if (groups.Children().IndexOf(sibling, insertion)) break;
            }
            groups.Children().InsertAt(insertion, groupCard); sections.emplace(group, std::move(card));
            UpdateCardStates();
        }
        return sections.at(group).panel;
    }
    void RemoveEmptySections()
    {
        for (auto it = sections.begin(); it != sections.end();)
            if (!cardModels.contains(it->first))
            {
                if (it->second.revoke) it->second.revoke();
                RemoveChild(groups, it->second.panel.Parent().as<mux::UIElement>());
                it = sections.erase(it);
            }
            else ++it;
    }
    void AppendRows()
    {
        if (closed || !active) { rowTimer.Stop(); return; }
        const auto started = GetTickCount64();
        for (unsigned added = 0; nextRow < pendingRows.size() && added < 8; ++added)
        {
            const auto &entry = pendingRows[nextRow++];
            if (auto *existing = FindRow(entry.id)) UpdateRow(*existing, entry);
            else
            {
                Row row; row.model = entry;
                muxc::Grid layout; Column(layout, 32, mux::GridUnitType::Pixel); Column(layout, 1, mux::GridUnitType::Star); Column(layout, 1, mux::GridUnitType::Auto);
                layout.HorizontalAlignment(mux::HorizontalAlignment::Stretch); layout.Children().Append(row.icon);
                muxc::StackPanel names; names.VerticalAlignment(mux::VerticalAlignment::Center);
                row.title.TextWrapping(mux::TextWrapping::Wrap); names.Children().Append(row.title);
                row.scope.FontSize(12); row.scope.Opacity(0.7); row.scope.TextWrapping(mux::TextWrapping::Wrap); names.Children().Append(row.scope);
                row.mixed.FontSize(12); row.mixed.Opacity(0.7); row.mixed.TextWrapping(mux::TextWrapping::Wrap); row.mixed.Text(L("settings.contextMenu.mixed")); names.Children().Append(row.mixed);
                names.Margin({0, 0, 16, 0}); muxc::Grid::SetColumn(names, 1); layout.Children().Append(names);
                row.toggle.OnContent(winrt::box_value(L"")); row.toggle.OffContent(winrt::box_value(L"")); row.toggle.MinWidth(0);
                row.toggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
                row.toggle.VerticalAlignment(mux::VerticalAlignment::Center);
                muxc::Grid::SetColumn(row.toggle, 2); layout.Children().Append(row.toggle);
                auto token = row.toggle.Toggled([this, id = entry.id](auto &&, auto &&) {
                    if (closed || updating || !active || !initialized || !actions.updateGeneral) return;
                    const auto *current = FindRow(id); if (!current) return;
                    const auto model = current->model; const bool shown = current->toggle.IsOn(); const auto catalogue = view.catalogue;
                    actions.updateGeneral(generation, SettingsUpdateMode::PreviewAndCommit, [model, scope = category, shown, catalogue](auto &settings) {
                        ext::MigrateAssociations(settings.shellExtensions, catalogue);
                        ext::SetManagementCommon(settings.shellExtensions, model, scope, shown);
                    });
                }); row.revoke.push_back([toggle = row.toggle, token] { toggle.Toggled(token); });
                row.expander.HorizontalAlignment(mux::HorizontalAlignment::Stretch); row.expander.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
                // Keep the native chevron hit area; only tighten its leading gap.
                row.expander.Resources().Insert(winrt::box_value(L"ExpanderChevronMargin"), winrt::box_value(mux::Thickness{4, 0, 8, 0}));
                row.expander.Header(layout); muxc::ToolTipService::SetToolTip(row.expander, winrt::box_value(L("settings.contextMenu.locations")));
                row.positionsPanel.Spacing(8);
                const auto expanding = row.expander.Expanding([this, id = entry.id](auto &&, auto &&) {
                    if (closed || !active) return;
                    if (auto *current = FindRow(id)) PopulatePositions(*current);
                });
                row.revoke.push_back([expander = row.expander, expanding] { expander.Expanding(expanding); });
                rows.push_back(std::move(row)); UpdateRow(rows.back(), entry);
            }
            if (GetTickCount64() - started >= 6) break;
        }
        if (nextRow >= pendingRows.size()) { rowTimer.Stop(); RemoveEmptySections(); }
    }
    void UpdateRow(Row &row, const ext::ManagementRow &entry)
    {
        const auto &group = rowGroups.at(entry.id);
        if (row.group != group)
        {
            if (sections.contains(row.group)) RemoveChild(sections.at(row.group).panel, row.expander);
            Section(group).Children().Append(row.expander); row.group = group;
        }
        // Replace only the changed row's display; the Expander and toggle retain
        // identity, focus and expansion state throughout incremental discovery.
        row.model = entry; row.title.Text(entry.display.label);
        row.icon.Child(Icon(entry.display));
        std::wstring types;
        if (mode != ext::ManagementView::Applications) for (const auto &app : entry.applications)
        {
            if (!types.empty()) types += L", ";
            types += app.id.empty() ? L("settings.contextMenu.unknownApplication") : app.name;
        }
        if (category == ext::Category::Objects && (entry.contexts & ext::ContextBit(ext::Context::File)))
        {
            std::wstring extensions;
            for (const auto &type : entry.types) if (!type.empty() && type.front() == L'.') { if (!extensions.empty()) extensions += L", "; extensions += type; }
            if (entry.types.empty() || std::find(entry.types.begin(), entry.types.end(), L"*") != entry.types.end()) extensions = L("settings.contextMenu.anyExtension");
            if (!extensions.empty()) { if (!types.empty()) types += L" · "; types += extensions; }
        }
        row.scope.Text(types); row.scope.Visibility(types.empty() ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        mux::Automation::AutomationProperties::SetName(row.toggle, entry.display.label);
        mux::Automation::AutomationProperties::SetName(row.expander, entry.display.label + L" — " + L("settings.contextMenu.locations"));
        if (row.expander.Content()) PopulatePositions(row);
        UpdateChoices(row);
    }
    void PopulatePositions(Row &row)
    {
        std::erase_if(row.positions, [&](auto &position) {
            if (row.model.contexts & ext::ContextBit(position.context)) return false;
            position.revoke(); RemoveChild(row.positionsPanel, position.layout); return true;
        });
        for (int i = category == ext::Category::Objects ? 0 : 2; i < (category == ext::Category::Objects ? 2 : 4); ++i)
        {
            if (!(row.model.contexts & (1u << i))) continue;
            const auto context = static_cast<ext::Context>(i);
            if (std::any_of(row.positions.begin(), row.positions.end(), [&](const auto &p) { return p.context == context; })) continue;
            Row::Position position; position.context = context;
            Column(position.layout, 1, mux::GridUnitType::Star); Column(position.layout, 1, mux::GridUnitType::Auto);
            muxc::TextBlock label; const char *keys[] = {"settings.contextMenu.files", "settings.contextMenu.folders", "settings.contextMenu.folderBackground", "settings.contextMenu.desktop"};
            label.Text(L(keys[i])); label.VerticalAlignment(mux::VerticalAlignment::Center); position.layout.Children().Append(label);
            auto choice = position.choice;
            for (const auto *key : {"settings.contextMenu.inherit", "settings.contextMenu.show", "settings.contextMenu.hide"}) choice.Items().Append(winrt::box_value(L(key)));
            choice.PlaceholderText(L("settings.contextMenu.mixed"));
            mux::Automation::AutomationProperties::SetName(choice, row.model.display.label + L" " + L(keys[i]));
            muxc::Grid::SetColumn(choice, 1); position.layout.Children().Append(choice); row.positionsPanel.Children().Append(position.layout);
            const auto changed = choice.SelectionChanged([this, id = row.model.id, context, choice](auto &&, auto &&) {
                if (closed || updating || !active || !initialized || !actions.updateGeneral || choice.SelectedIndex() < 0) return;
                const auto *current = FindRow(id); if (!current) return;
                const auto model = current->model;
                const auto visibility = static_cast<ext::Visibility>(choice.SelectedIndex()); const auto catalogue = view.catalogue;
                actions.updateGeneral(generation, SettingsUpdateMode::PreviewAndCommit, [model, context, visibility, catalogue](auto &settings) {
                    ext::MigrateAssociations(settings.shellExtensions, catalogue);
                    ext::SetManagementOverride(settings.shellExtensions, model, context, visibility);
                });
            }); position.revoke = [choice, changed] { choice.SelectionChanged(changed); };
            row.positions.push_back(std::move(position));
        }
        if (!row.expander.Content()) row.expander.Content(row.positionsPanel);
        UpdateChoices(row);
    }
    void UpdateChoices(Row &row)
    {
        const bool wasUpdating = std::exchange(updating, true);
        const auto common = ext::ManagementCommon(prefs, row.model, category);
        row.toggle.IsOn(common.value_or(false));
        row.mixed.Visibility(common ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        for (auto &position : row.positions)
        {
            const auto value = ext::ManagementOverride(prefs, row.model, position.context);
            position.choice.SelectedIndex(value ? static_cast<int>(*value) : -1);
        }
        updating = wasUpdating;
    }
    void UpdateChoices() { for (auto &row : rows) UpdateChoices(row); UpdateCardStates(); }
    void Apply(const SettingsSnapshot &snapshot)
    {
        if (closed) return;
        generation = snapshot.generation; initialized = snapshot.initialized; prefs = snapshot.values.general.shellExtensions;
        extensionFocus = snapshot.route.focusId == "contextMenu.extension";
        ext::MigrateAssociations(prefs, view.catalogue);
        UpdateChoices();
        if (routeFocus != snapshot.route.focusId)
        {
            routeFocus = snapshot.route.focusId;
            if (routeFocus == "contextMenu.application" || routeFocus == "contextMenu.extension")
            {
                mode = routeFocus == "contextMenu.application" ? ext::ManagementView::Applications : ext::ManagementView::Extensions;
                const bool before = std::exchange(updating, true);
                if (mode == ext::ManagementView::Extensions) { category = ext::Category::Objects; tabs.SelectedItem(objectsTab); }
                views.SelectedItem(mode == ext::ManagementView::Applications ? byApplication : byExtension); updating = before;
                Text(); ClearRows(); if (active) BuildRows();
            }
        }
        if (!snapshot.sessionActive) Deactivate();
    }
    void Deactivate() { active = false; Cancel(); }
    void Close()
    {
        if (closed) return;
        closed = true; Deactivate();
        ClearRows();
        for (auto &r : revoke) r(); revoke.clear(); actions = {};
    }
};
ContextMenuPagePresenter::ContextMenuPagePresenter(LocalizeCallback callback, const mux::Style &cardStyle)
    : impl_(std::make_shared<Impl>(std::move(callback), cardStyle))
{
}
ContextMenuPagePresenter::~ContextMenuPagePresenter()
{
    Close();
}
void ContextMenuPagePresenter::SetActions(PersonalizationPageActions actions)
{
    impl_->actions = std::move(actions);
}
mux::UIElement ContextMenuPagePresenter::Content() const
{
    return impl_->root;
}
void ContextMenuPagePresenter::ApplySnapshot(const SettingsSnapshot &snapshot)
{
    impl_->Apply(snapshot);
}
void ContextMenuPagePresenter::RefreshLocalizedText()
{
    impl_->Text();
    impl_->ClearRows();
    if (impl_->active) impl_->BuildRows();
}
void ContextMenuPagePresenter::Activate()
{
    if (impl_->closed)
        return;
    impl_->active = true;
    impl_->request = {};
    impl_->routed = false;
    impl_->Reload();
}
void ContextMenuPagePresenter::Deactivate()
{
    impl_->Deactivate();
}
void ContextMenuPagePresenter::Close()
{
    if (impl_)
        impl_->Close();
}
void ContextMenuPagePresenter::RegisterFocusTargets(const FocusRegistrar &registerFocus) const
{
    registerFocus("contextMenu.extensions", impl_->tabs);
    registerFocus("contextMenu.items", impl_->search);
    registerFocus("contextMenu.application", impl_->views);
    registerFocus("contextMenu.extension", impl_->views);
    registerFocus("contextMenu.batch", impl_->views);
}
} // namespace snowdesktop::winui

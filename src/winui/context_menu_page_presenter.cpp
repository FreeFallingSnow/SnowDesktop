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
        int group = -1;
    };
    LocalizeCallback localize;
    PersonalizationPageActions actions;
    mux::Style cardStyle;
    muxc::StackPanel root, content, groups;
    muxc::Border card;
    muxc::SelectorBar tabs;
    muxc::SelectorBarItem objectsTab, backgroundTab;
    muxc::Grid toolbar;
    muxc::TextBox search;
    muxc::Button refresh;
    muxc::ComboBox applicationFilter, extensionFilter;
    muxc::Grid filters, resultBar;
    muxc::Button bulk;
    muxc::MenuFlyoutItem showResults, hideResults;
    muxc::HyperlinkButton clearFilters;
    muxc::TextBlock resultCount;
    std::vector<std::pair<std::string, std::wstring>> applicationOptions;
    std::vector<std::pair<std::wstring, std::wstring>> extensionOptions;
    std::string selectedApplication;
    std::wstring selectedExtension;
    std::vector<ext::ManagementRow> filteredRows;
    muxc::HyperlinkButton chooseObject, chooseFolder, chooseDesktop;
    muxc::TextBlock heading, status;
    mux::DispatcherTimer timer, rowTimer;
    std::vector<ext::ManagementRow> pendingRows;
    std::map<int, muxc::StackPanel> sections;
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
    bool active = false, closed = false, updating = false, initialized = false, routed = false;
    std::wstring L(std::string_view key) const { return localize ? localize(key) : std::wstring{}; }
    static void Column(muxc::Grid grid, double width, mux::GridUnitType unit)
    {
        muxc::ColumnDefinition column; column.Width({width, unit}); grid.ColumnDefinitions().Append(column);
    }
    explicit Impl(LocalizeCallback l, const mux::Style &style) : localize(std::move(l)), cardStyle(style)
    {
        root.Spacing(12); content.Spacing(12); groups.Spacing(12);
        if (cardStyle) card.Style(cardStyle);
        heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        content.Children().Append(heading);
        tabs.Items().Append(objectsTab); tabs.Items().Append(backgroundTab); tabs.SelectedItem(objectsTab);
        content.Children().Append(tabs);
        Column(toolbar, 1, mux::GridUnitType::Star); Column(toolbar, 1, mux::GridUnitType::Auto);
        search.Margin({0, 0, 8, 0}); toolbar.Children().Append(search);
        muxc::Grid::SetColumn(refresh, 1); toolbar.Children().Append(refresh); content.Children().Append(toolbar);
        Column(filters, 1, mux::GridUnitType::Star); Column(filters, 1, mux::GridUnitType::Star);
        applicationFilter.HorizontalAlignment(mux::HorizontalAlignment::Stretch); applicationFilter.Margin({0, 0, 8, 0});
        extensionFilter.HorizontalAlignment(mux::HorizontalAlignment::Stretch); muxc::Grid::SetColumn(extensionFilter, 1);
        filters.Children().Append(applicationFilter); filters.Children().Append(extensionFilter); content.Children().Append(filters);
        Column(resultBar, 1, mux::GridUnitType::Star); Column(resultBar, 1, mux::GridUnitType::Auto); Column(resultBar, 1, mux::GridUnitType::Auto);
        resultCount.VerticalAlignment(mux::VerticalAlignment::Center); resultCount.TextWrapping(mux::TextWrapping::Wrap);
        resultBar.Children().Append(resultCount); muxc::Grid::SetColumn(clearFilters, 1); resultBar.Children().Append(clearFilters);
        muxc::Grid::SetColumn(bulk, 2); resultBar.Children().Append(bulk); content.Children().Append(resultBar);
        muxc::MenuFlyout batchMenu; batchMenu.Items().Append(showResults); batchMenu.Items().Append(hideResults); bulk.Flyout(batchMenu);
        muxc::StackPanel pickers; pickers.Orientation(muxc::Orientation::Horizontal); pickers.Spacing(8);
        pickers.Children().Append(chooseObject); pickers.Children().Append(chooseFolder); pickers.Children().Append(chooseDesktop); content.Children().Append(pickers);
        status.TextWrapping(mux::TextWrapping::Wrap); status.Visibility(mux::Visibility::Collapsed); content.Children().Append(status);
        card.Child(content); root.Children().Append(card); root.Children().Append(groups);
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
        const auto appChanged = applicationFilter.SelectionChanged([this](auto &&, auto &&) {
            if (closed || updating || applicationFilter.SelectedIndex() < 0) return;
            selectedApplication = applicationOptions.at(applicationFilter.SelectedIndex()).first; BuildRows();
        }); revoke.push_back([c = applicationFilter, appChanged] { c.SelectionChanged(appChanged); });
        const auto extensionChanged = extensionFilter.SelectionChanged([this](auto &&, auto &&) {
            if (closed || updating || extensionFilter.SelectedIndex() < 0) return;
            selectedExtension = extensionOptions.at(extensionFilter.SelectedIndex()).first; BuildRows();
        }); revoke.push_back([c = extensionFilter, extensionChanged] { c.SelectionChanged(extensionChanged); });
        for (const auto &choice : {applicationFilter, extensionFilter})
        {
            const auto closedList = choice.DropDownClosed([this](auto &&, auto &&) { if (!closed && active) BuildRows(); });
            revoke.push_back([choice, closedList] { choice.DropDownClosed(closedList); });
        }
        const auto clear = clearFilters.Click([this](auto &&, auto &&) {
            updating = true; selectedApplication.clear(); selectedExtension.clear(); search.Text(L""); updating = false; BuildRows();
        }); revoke.push_back([c = clearFilters, clear] { c.Click(clear); });
        const auto show = showResults.Click([this](auto &&, auto &&) { SetResults(true); });
        revoke.push_back([c = showResults, show] { c.Click(show); });
        const auto hide = hideResults.Click([this](auto &&, auto &&) { SetResults(false); });
        revoke.push_back([c = hideResults, hide] { c.Click(hide); });
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
        objectsTab.Text(L("settings.contextMenu.objects")); backgroundTab.Text(L("settings.contextMenu.background"));
        search.PlaceholderText(L("settings.contextMenu.search")); refresh.Content(winrt::box_value(L("settings.contextMenu.refresh")));
        applicationFilter.Header(winrt::box_value(L("settings.contextMenu.application")));
        extensionFilter.Header(winrt::box_value(L("settings.contextMenu.extension")));
        extensionFilter.Visibility(category == ext::Category::Objects ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        muxc::Grid::SetColumnSpan(applicationFilter, category == ext::Category::Objects ? 1 : 2);
        bulk.Content(winrt::box_value(L("settings.contextMenu.batch")));
        showResults.Text(L("settings.contextMenu.showResults")); hideResults.Text(L("settings.contextMenu.hideResults"));
        clearFilters.Content(winrt::box_value(L("settings.contextMenu.clearFilters")));
        muxc::ToolTipService::SetToolTip(bulk, winrt::box_value(L("settings.contextMenu.batchHint")));
        chooseObject.Content(winrt::box_value(L("settings.contextMenu.inspectFiles")));
        chooseFolder.Content(winrt::box_value(L("settings.contextMenu.inspectFolders")));
        chooseDesktop.Content(winrt::box_value(L("settings.contextMenu.inspectDesktop")));
        chooseDesktop.Visibility(category == ext::Category::Background ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        chooseObject.Visibility(category == ext::Category::Objects ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    }
    void Cancel() { timer.Stop(); rowTimer.Stop(); }
    void Status(const std::wstring &text) { status.Text(text); status.Visibility(text.empty() ? mux::Visibility::Collapsed : mux::Visibility::Visible); }
    void Reload(bool refreshCatalogue = false)
    {
        if (!active || closed || !actions.contextMenu) return;
        try
        {
            view = actions.contextMenu(request, refreshCatalogue);
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
        catch (...) { Status(L("settings.contextMenu.failed")); timer.Stop(); }
    }
    void Poll()
    {
        if (!active || closed || !actions.contextMenu) return;
        try
        {
            auto next = actions.contextMenu(request, false);
            const bool changed = next.catalogue.revision != view.catalogue.revision;
            if (request.paths.empty() && next.selection.context == ext::Context::Desktop && !next.selection.paths.empty()) request = next.selection;
            view = std::move(next);
            if (changed) { ext::MigrateAssociations(prefs, view.catalogue); BuildRows(); }
            UpdateStatus();
            if (!view.scanning && !view.menu.pending) timer.Stop();
        }
        catch (...) { Status(L("settings.contextMenu.failed")); timer.Stop(); }
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
        if (hasVisibleRows) Status(L"");
        else if (HasFilters()) Status(L("settings.contextMenu.noMatches"));
        else if (view.scanning || view.menu.pending) Status(L("settings.contextMenu.loading"));
        else Status(L(view.menu.error.empty() ? "settings.contextMenu.empty" : "settings.contextMenu.failed"));
    }
    bool HasFilters() const
    {
        return !selectedApplication.empty() || (category == ext::Category::Objects && !selectedExtension.empty()) || !search.Text().empty();
    }
    template<class Key>
    void SyncOptions(muxc::ComboBox choice, std::vector<std::pair<Key, std::wstring>> &current,
        std::vector<std::pair<Key, std::wstring>> desired, const Key &selected)
    {
        if (choice.IsDropDownOpen()) return;
        // Keep a selected filter if its last item disappears. Resetting it would
        // silently broaden a subsequent bulk operation to unrelated items.
        if (const auto old = std::find_if(current.begin(), current.end(), [&](const auto &item) { return item.first == selected; });
            old != current.end() && std::none_of(desired.begin(), desired.end(), [&](const auto &item) { return item.first == selected; })) desired.push_back(*old);
        const bool wasUpdating = std::exchange(updating, true);
        if (current != desired)
        {
            current = std::move(desired); choice.Items().Clear();
            for (const auto &item : current) choice.Items().Append(winrt::box_value(item.second));
        }
        const auto selectedItem = std::find_if(current.begin(), current.end(), [&](const auto &item) { return item.first == selected; });
        choice.SelectedIndex(selectedItem == current.end() ? -1 : static_cast<int>(std::distance(current.begin(), selectedItem)));
        updating = wasUpdating;
    }
    void FilterOptions()
    {
        const auto all = ext::ManagementRows(view.catalogue, category);
        std::map<std::string, std::wstring> applications;
        std::set<std::wstring> extensions;
        for (const auto &row : all)
        {
            for (const auto &app : row.applications)
                applications[app.id.empty() ? "@unknown" : app.id] = app.id.empty() ? L("settings.contextMenu.unknownApplication") : app.name;
            if (row.contexts & ext::ContextBit(ext::Context::File))
            {
                if (row.types.empty()) extensions.insert(L"*");
                for (const auto &type : row.types) if (type == L"*" || (!type.empty() && type.front() == L'.')) extensions.insert(type);
            }
        }
        std::vector<std::pair<std::string, std::wstring>> apps(applications.begin(), applications.end());
        std::stable_sort(apps.begin(), apps.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
        apps.insert(apps.begin(), {"", L("settings.contextMenu.allApplications")});
        SyncOptions(applicationFilter, applicationOptions, std::move(apps), selectedApplication);
        std::vector<std::pair<std::wstring, std::wstring>> types{{L"", L("settings.contextMenu.allExtensions")}};
        for (const auto &extension : extensions) types.emplace_back(extension, extension == L"*" ? L("settings.contextMenu.anyExtension") : extension);
        SyncOptions(extensionFilter, extensionOptions, std::move(types), selectedExtension);
    }
    void FilterActions()
    {
        auto text = L("settings.contextMenu.resultCount");
        if (const auto at = text.find(L"{count}"); at != std::wstring::npos) text.replace(at, 7, std::to_wstring(filteredRows.size()));
        resultCount.Text(text); bulk.IsEnabled(active && initialized && !filteredRows.empty()); clearFilters.IsEnabled(HasFilters());
    }
    void SetResults(bool shown)
    {
        if (closed || !active || !initialized || filteredRows.empty() || !actions.updateGeneral) return;
        const auto targets = filteredRows; const auto catalogue = view.catalogue;
        actions.updateGeneral(generation, SettingsUpdateMode::PreviewAndCommit, [targets, catalogue, scope = category, shown](auto &settings) {
            ext::MigrateAssociations(settings.shellExtensions, catalogue);
            ext::SetManagementResults(settings.shellExtensions, targets, scope, shown);
        });
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
        if (sections.contains(row.group)) RemoveChild(sections.at(row.group), row.expander);
    }
    void ClearRows()
    {
        rowTimer.Stop();
        for (auto &row : rows) RemoveRow(row);
        rows.clear(); groups.Children().Clear(); sections.clear(); pendingRows.clear(); nextRow = 0;
    }
    void BuildRows()
    {
        rowTimer.Stop();
        // Only explicit category/localization changes reset the list. Polling,
        // refresh and search reconcile identities and preserve surviving rows.
        if (rowCategory != category) { ClearRows(); rowCategory = category; }
        FilterOptions();
        filteredRows = ext::ManagementRows(view.catalogue, category, std::wstring(search.Text()), selectedApplication,
            category == ext::Category::Objects ? selectedExtension : L"");
        hasVisibleRows = !filteredRows.empty();
        pendingRows = ext::PlanManagementUpdates(rows, filteredRows, [this](auto &row) { RemoveRow(row); });
        FilterActions();
        nextRow = 0;
        UpdateChoices();
        AppendRows();
        if (nextRow < pendingRows.size()) rowTimer.Start();
        UpdateStatus();
    }
    muxc::StackPanel Section(int group)
    {
        if (!sections.contains(group))
        {
            muxc::StackPanel section; section.Spacing(4);
            const char *key = group == 0 ? "settings.contextMenu.common" : group == 3 ? "settings.contextMenu.types" :
                category == ext::Category::Objects ? (group == 1 ? "settings.contextMenu.files" : "settings.contextMenu.folders") :
                (group == 1 ? "settings.contextMenu.folderBackground" : "settings.contextMenu.desktop");
            muxc::TextBlock label; label.Text(L(key)); label.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold()); label.Margin({0, 0, 0, 8});
            section.Children().Append(label);
            muxc::Border groupCard; if (cardStyle) groupCard.Style(cardStyle); groupCard.Child(section);
            // New cards append while discovery is active; existing rows do not
            // jump because an earlier alphabetical group has just arrived.
            groups.Children().Append(groupCard); sections.emplace(group, section);
        }
        return sections.at(group);
    }
    void RemoveEmptySections()
    {
        for (auto it = sections.begin(); it != sections.end();)
            if (it->second.Children().Size() == 1)
            {
                RemoveChild(groups, it->second.Parent().as<mux::UIElement>());
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
                row.toggle.OnContent(winrt::box_value(L"")); row.toggle.OffContent(winrt::box_value(L"")); row.toggle.MinWidth(44);
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
        const auto group = ext::ManagementGroup(entry, category);
        if (row.group != group)
        {
            if (sections.contains(row.group)) RemoveChild(sections.at(row.group), row.expander);
            Section(group).Children().Append(row.expander); row.group = group;
        }
        // Replace only the changed row's display; the Expander and toggle retain
        // identity, focus and expansion state throughout incremental discovery.
        row.model = entry; row.title.Text(entry.display.label);
        row.icon.Child(Icon(entry.display));
        std::wstring types;
        for (const auto &app : entry.applications)
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
    void UpdateChoices() { for (auto &row : rows) UpdateChoices(row); }
    void Apply(const SettingsSnapshot &snapshot)
    {
        if (closed) return;
        generation = snapshot.generation; initialized = snapshot.initialized; prefs = snapshot.values.general.shellExtensions;
        extensionFocus = snapshot.route.focusId == "contextMenu.extension";
        ext::MigrateAssociations(prefs, view.catalogue);
        UpdateChoices();
        FilterActions();
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
    registerFocus("contextMenu.application", impl_->applicationFilter);
    registerFocus("contextMenu.extension", impl_->extensionFilter);
    registerFocus("contextMenu.batch", impl_->bulk);
}
} // namespace snowdesktop::winui

#include "pch.h"
#include "context_menu_page_presenter.h"
#include "../shell_extension_service.h"
#include <array>
#include <cwctype>
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
        std::string id;
        muxc::ToggleSwitch toggle;
        std::vector<std::pair<ext::Context, muxc::ComboBox>> positions;
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
    muxc::HyperlinkButton chooseObject, chooseFolder, chooseDesktop;
    muxc::TextBlock heading, status;
    mux::DispatcherTimer timer;
    std::vector<std::function<void()>> revoke, rowRevoke;
    std::vector<Row> rows;
    ext::Preferences prefs;
    ext::Request request;
    ext::CatalogueView view;
    ext::Category category = ext::Category::Objects;
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
        muxc::StackPanel pickers; pickers.Orientation(muxc::Orientation::Horizontal); pickers.Spacing(8);
        pickers.Children().Append(chooseObject); pickers.Children().Append(chooseFolder); pickers.Children().Append(chooseDesktop); content.Children().Append(pickers);
        status.TextWrapping(mux::TextWrapping::Wrap); status.Visibility(mux::Visibility::Collapsed); content.Children().Append(status);
        card.Child(content); root.Children().Append(card); root.Children().Append(groups);
        timer.Interval(std::chrono::milliseconds(300));
        auto tick = timer.Tick([this](auto &&, auto &&) { Poll(); }); revoke.push_back([t = timer, tick] { t.Tick(tick); });
        auto changed = tabs.SelectionChanged([this](auto &&, auto &&) {
            if (closed || updating) return;
            category = tabs.SelectedItem() == backgroundTab ? ext::Category::Background : ext::Category::Objects;
            Text(); BuildRows();
        }); revoke.push_back([t = tabs, changed] { t.SelectionChanged(changed); });
        auto filter = search.TextChanged([this](auto &&, auto &&) { if (!closed) BuildRows(); });
        revoke.push_back([c = search, filter] { c.TextChanged(filter); });
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
        chooseObject.Content(winrt::box_value(L("settings.contextMenu.inspectFiles")));
        chooseFolder.Content(winrt::box_value(L("settings.contextMenu.inspectFolders")));
        chooseDesktop.Content(winrt::box_value(L("settings.contextMenu.inspectDesktop")));
        chooseDesktop.Visibility(category == ext::Category::Background ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        chooseObject.Visibility(category == ext::Category::Objects ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    }
    void Cancel() { timer.Stop(); }
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
                category = request.background || request.context == ext::Context::Desktop ? ext::Category::Background : ext::Category::Objects;
                updating = true; tabs.SelectedItem(category == ext::Category::Objects ? objectsTab : backgroundTab); updating = false;
                routed = true; Text();
            }
            ext::MigrateAssociations(prefs, view.catalogue);
            Status(view.scanning && view.catalogue.rows.empty() ? L("settings.contextMenu.loading") : L"");
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
            const bool changed = next.catalogue.revision != view.catalogue.revision || next.catalogue.associations.size() != view.catalogue.associations.size() || next.menu.revision != view.menu.revision;
            if (request.paths.empty() && next.selection.context == ext::Context::Desktop && !next.selection.paths.empty()) request = next.selection;
            view = std::move(next);
            if (changed) { ext::MigrateAssociations(prefs, view.catalogue); BuildRows(); }
            if (!view.scanning && !view.menu.pending) { timer.Stop(); Status(view.menu.error.empty() ? L"" : L("settings.contextMenu.failed")); }
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
    void BuildRows()
    {
        for (auto &r : rowRevoke) r(); rowRevoke.clear(); rows.clear(); groups.Children().Clear();
        auto filter = std::wstring(search.Text()); for (auto &c : filter) c = towlower(c);
        std::vector<ext::Registration> entries = view.catalogue.rows;
        if (view.menu.snapshot)
            for (const auto &entry : view.menu.snapshot->entries)
                if (!entry.separator && entry.registration.empty())
                {
                    ext::Registration row; row.id = entry.provider; row.kind = ext::RegistrationKind::Observed; row.display = entry;
                    row.linked = true; row.contexts = view.menu.contexts; entries.push_back(std::move(row));
                }
        std::map<int, muxc::StackPanel> sections;
        const unsigned mask = category == ext::Category::Objects ? 3 : 12;
        std::stable_sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return a.display.label < b.display.label; });
        for (const auto &entry : entries)
        {
            if (!entry.systemEnabled || !(entry.contexts & mask)) continue;
            auto name = entry.display.label; for (auto &c : name) c = towlower(c);
            auto types = std::wstring{}; for (const auto &type : entry.types) if (type != L"*") types += type + L" ";
            if (!filter.empty() && name.find(filter) == std::wstring::npos && types.find(filter) == std::wstring::npos) continue;
            const bool specificType = !entry.types.empty() && std::find(entry.types.begin(), entry.types.end(), L"*") == entry.types.end();
            const int group = entry.kind == ext::RegistrationKind::Observed ? 4 : specificType ? 3 : (entry.contexts & mask) == mask ? 0 : (entry.contexts & (category == ext::Category::Objects ? 1 : 4)) ? 1 : 2;
            if (!sections.contains(group)) sections.emplace(group, muxc::StackPanel{});
            auto section = sections.at(group); section.Spacing(4);
            muxc::Grid layout; Column(layout, 32, mux::GridUnitType::Pixel); Column(layout, 1, mux::GridUnitType::Star); Column(layout, 1, mux::GridUnitType::Auto);
            layout.HorizontalAlignment(mux::HorizontalAlignment::Stretch); layout.Children().Append(Icon(entry.display));
            muxc::StackPanel names; names.VerticalAlignment(mux::VerticalAlignment::Center);
            muxc::TextBlock title; title.Text(entry.display.label); title.TextWrapping(mux::TextWrapping::Wrap); names.Children().Append(title);
            if (!entry.linked) { muxc::TextBlock pending; pending.Text(L("settings.contextMenu.pending")); pending.FontSize(12); pending.Opacity(0.65); names.Children().Append(pending); }
            names.Margin({0, 0, 16, 0}); muxc::Grid::SetColumn(names, 1); layout.Children().Append(names);
            Row row; row.id = entry.id; row.toggle.OnContent(winrt::box_value(L"")); row.toggle.OffContent(winrt::box_value(L"")); row.toggle.MinWidth(44);
            row.toggle.VerticalAlignment(mux::VerticalAlignment::Center); row.toggle.IsEnabled(entry.linked);
            row.toggle.IsOn(ext::CommonShown(prefs, row.id, category)); mux::Automation::AutomationProperties::SetName(row.toggle, entry.display.label);
            muxc::Grid::SetColumn(row.toggle, 2); layout.Children().Append(row.toggle);
            auto token = row.toggle.Toggled([this, id = row.id, toggle = row.toggle, scope = category](auto &&, auto &&) {
                if (closed || updating || !active || !initialized || !actions.updateGeneral) return;
                const bool shown = toggle.IsOn(); const auto catalogue = view.catalogue;
                actions.updateGeneral(generation, SettingsUpdateMode::PreviewAndCommit, [id, scope, shown, catalogue](auto &settings) {
                    ext::MigrateAssociations(settings.shellExtensions, catalogue);
                    ext::SetCommon(settings.shellExtensions, id, scope, shown);
                });
            }); rowRevoke.push_back([toggle = row.toggle, token] { toggle.Toggled(token); });
            muxc::Expander expander; expander.HorizontalAlignment(mux::HorizontalAlignment::Stretch); expander.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
            expander.Header(layout); muxc::ToolTipService::SetToolTip(expander, winrt::box_value(L("settings.contextMenu.locations")));
            mux::Automation::AutomationProperties::SetName(expander, entry.display.label + L" — " + L("settings.contextMenu.locations"));
            muxc::StackPanel positions; positions.Spacing(8);
            if (!types.empty()) { muxc::TextBlock typeText; typeText.Text(types); typeText.TextWrapping(mux::TextWrapping::Wrap); positions.Children().Append(typeText); }
            for (int i = category == ext::Category::Objects ? 0 : 2; i < (category == ext::Category::Objects ? 2 : 4); ++i)
            {
                if (!(entry.contexts & (1u << i))) continue;
                const auto context = static_cast<ext::Context>(i);
                muxc::Grid position; Column(position, 1, mux::GridUnitType::Star); Column(position, 1, mux::GridUnitType::Auto);
                muxc::TextBlock label; const char *keys[] = {"settings.contextMenu.files", "settings.contextMenu.folders", "settings.contextMenu.folderBackground", "settings.contextMenu.desktop"};
                label.Text(L(keys[i])); label.VerticalAlignment(mux::VerticalAlignment::Center); position.Children().Append(label);
                muxc::ComboBox choice;
                for (const auto *key : {"settings.contextMenu.inherit", "settings.contextMenu.show", "settings.contextMenu.hide"}) choice.Items().Append(winrt::box_value(L(key)));
                choice.SelectedIndex(static_cast<int>(ext::OverrideOf(prefs, row.id, context))); choice.IsEnabled(entry.linked);
                mux::Automation::AutomationProperties::SetName(choice, entry.display.label + L" " + L(keys[i]));
                muxc::Grid::SetColumn(choice, 1); position.Children().Append(choice); positions.Children().Append(position);
                const auto changed = choice.SelectionChanged([this, id = row.id, context, choice](auto &&, auto &&) {
                    if (closed || updating || !active || !initialized || !actions.updateGeneral || choice.SelectedIndex() < 0) return;
                    const auto visibility = static_cast<ext::Visibility>(choice.SelectedIndex()); const auto catalogue = view.catalogue;
                    actions.updateGeneral(generation, SettingsUpdateMode::PreviewAndCommit, [id, context, visibility, catalogue](auto &settings) {
                        ext::MigrateAssociations(settings.shellExtensions, catalogue); ext::SetOverride(settings.shellExtensions, id, context, visibility);
                    });
                }); rowRevoke.push_back([choice, changed] { choice.SelectionChanged(changed); }); row.positions.emplace_back(context, choice);
            }
            expander.Content(positions); section.Children().Append(expander); rows.push_back(std::move(row));
        }
        for (auto &[group, section] : sections)
        {
            const char *key = group == 0 ? "settings.contextMenu.common" : group == 3 ? "settings.contextMenu.types" : group == 4 ? "settings.contextMenu.currentSelection" :
                category == ext::Category::Objects ? (group == 1 ? "settings.contextMenu.files" : "settings.contextMenu.folders") :
                (group == 1 ? "settings.contextMenu.folderBackground" : "settings.contextMenu.desktop");
            muxc::TextBlock label; label.Text(L(key)); label.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold()); label.Margin({0, 0, 0, 8});
            section.Children().InsertAt(0, label);
            muxc::Border groupCard; if (cardStyle) groupCard.Style(cardStyle); groupCard.Child(section); groups.Children().Append(groupCard);
        }
        if (rows.empty() && !view.scanning) Status(L("settings.contextMenu.empty"));
    }
    void Apply(const SettingsSnapshot &snapshot)
    {
        if (closed) return;
        generation = snapshot.generation; initialized = snapshot.initialized; prefs = snapshot.values.general.shellExtensions;
        ext::MigrateAssociations(prefs, view.catalogue);
        updating = true;
        for (auto &row : rows)
        {
            row.toggle.IsOn(ext::CommonShown(prefs, row.id, category));
            for (auto &[context, choice] : row.positions) choice.SelectedIndex(static_cast<int>(ext::OverrideOf(prefs, row.id, context)));
        }
        updating = false;
        if (!snapshot.sessionActive) Deactivate();
    }
    void Deactivate() { active = false; Cancel(); }
    void Close()
    {
        if (closed) return;
        closed = true; Deactivate();
        for (auto &r : rowRevoke) r(); rowRevoke.clear();
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
}
} // namespace snowdesktop::winui

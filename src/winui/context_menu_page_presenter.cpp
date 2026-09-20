#include "pch.h"

#include "context_menu_page_presenter.h"
#include "../shell_extension_menu.h"
#include "../shell_extension_menu_cache.h"
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
    struct Tab
    {
        ext::Context context;
        const char *label;
        muxc::SelectorBarItem item;
    };
    struct Row
    {
        std::string id;
        muxc::ToggleSwitch toggle;
    };
    LocalizeCallback localize;
    PersonalizationPageActions actions;
    muxc::StackPanel root;
    muxc::Border card;
    muxc::StackPanel content;
    muxc::SelectorBar tabs;
    muxc::ScrollViewer tabsScroll;
    muxc::Grid toolbar;
    muxc::TextBox search;
    muxc::Button refresh;
    muxc::HyperlinkButton chooseObject;
    muxc::TextBlock heading, status;
    muxc::ListView list;
    mux::DispatcherTimer timer;
    std::array<Tab, 4> scopes{{
        {ext::Context::File, "settings.contextMenu.files"},
        {ext::Context::Folder, "settings.contextMenu.folders"},
        {ext::Context::FolderBackground, "settings.contextMenu.folderBackground"},
        {ext::Context::Desktop, "settings.contextMenu.desktop"},
    }};
    std::vector<std::function<void()>> revoke, rowRevoke;
    std::vector<Row> rows;
    ext::Preferences prefs;
    ext::Request request;
    std::vector<ext::Entry> entries;
    std::unique_ptr<ext::Session> session;
    ext::MenuSnapshotCache::Ticket cacheTicket;
    std::uint64_t queryGeneration = 0;
    std::uint64_t generation = 0;
    bool active = false, closed = false, updating = false, initialized = false;

    std::wstring L(std::string_view key) const
    {
        return localize ? localize(key) : std::wstring{};
    }
    static void Column(muxc::Grid grid, double width, mux::GridUnitType unit)
    {
        muxc::ColumnDefinition column;
        column.Width({width, unit});
        grid.ColumnDefinitions().Append(column);
    }
    explicit Impl(LocalizeCallback l, const mux::Style &cardStyle) : localize(std::move(l))
    {
        root.Spacing(12);
        if (cardStyle)
            card.Style(cardStyle);
        content.Spacing(12);
        heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        heading.TextWrapping(mux::TextWrapping::Wrap);
        content.Children().Append(heading);
        card.Child(content);
        root.Children().Append(card);
        tabsScroll.HorizontalScrollMode(muxc::ScrollMode::Enabled);
        tabsScroll.HorizontalScrollBarVisibility(muxc::ScrollBarVisibility::Hidden);
        tabsScroll.VerticalScrollMode(muxc::ScrollMode::Disabled);
        tabsScroll.VerticalScrollBarVisibility(muxc::ScrollBarVisibility::Disabled);
        tabsScroll.Content(tabs);
        content.Children().Append(tabsScroll);
        for (auto &scope : scopes)
            tabs.Items().Append(scope.item);
        tabs.SelectedItem(scopes.front().item);
        request.context = ext::Context::File;
        Column(toolbar, 1, mux::GridUnitType::Star);
        Column(toolbar, 1, mux::GridUnitType::Auto);
        search.Margin({0, 0, 8, 0});
        toolbar.Children().Append(search);
        muxc::Grid::SetColumn(refresh, 1);
        toolbar.Children().Append(refresh);
        content.Children().Append(toolbar);
        status.TextWrapping(mux::TextWrapping::Wrap);
        status.Visibility(mux::Visibility::Collapsed);
        content.Children().Append(status);
        list.SelectionMode(muxc::ListViewSelectionMode::None);
        list.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        list.ItemContainerStyle(mux::Markup::XamlReader::Load(LR"(<Style
 xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="ListViewItem">
 <Setter Property="HorizontalContentAlignment" Value="Stretch"/>
 <Setter Property="Padding" Value="12,6"/>
 </Style>)")
                                    .as<mux::Style>());
        content.Children().Append(list);
        // Optional inspection of a file type or a contextual folder, never a
        // prerequisite for using the four ordinary context lists.
        chooseObject.HorizontalAlignment(mux::HorizontalAlignment::Left);
        content.Children().Append(chooseObject);
        timer.Interval(std::chrono::milliseconds(100));
        auto tick = timer.Tick([this](auto &&, auto &&) { Poll(); });
        revoke.push_back([t = timer, tick] { t.Tick(tick); });
        auto changed = tabs.SelectionChanged([this](auto &&, auto &&) {
            if (closed || updating)
                return;
            for (const auto &scope : scopes)
                if (scope.item == tabs.SelectedItem())
                    request.context = scope.context;
            request.paths.clear();
            request.background =
                request.context == ext::Context::Desktop || request.context == ext::Context::FolderBackground;
            Text();
            Reload();
        });
        revoke.push_back([t = tabs, changed] { t.SelectionChanged(changed); });
        auto filter = search.TextChanged([this](auto &&, auto &&) {
            if (!closed)
                BuildRows();
        });
        revoke.push_back([c = search, filter] { c.TextChanged(filter); });
        auto reload = refresh.Click([this](auto &&, auto &&) {
            if (active && !closed)
            {
                ext::InvalidateMenuCache();
                Reload();
            }
        });
        revoke.push_back([c = refresh, reload] { c.Click(reload); });
        auto pick = chooseObject.Click([this](auto &&, auto &&) {
            if (active && !closed)
                Pick();
        });
        revoke.push_back([c = chooseObject, pick] { c.Click(pick); });
        Text();
    }
    void Text()
    {
        heading.Text(L("settings.contextMenu.extensions"));
        for (auto &scope : scopes)
            scope.item.Text(L(scope.label));
        search.PlaceholderText(L("settings.contextMenu.search"));
        refresh.Content(winrt::box_value(L("settings.contextMenu.refresh")));
        chooseObject.Content(
            winrt::box_value(L(request.context == ext::Context::File ? "settings.contextMenu.inspectFile"
                                                                     : "settings.contextMenu.inspectFolder")));
        chooseObject.Visibility(request.context == ext::Context::Desktop ? mux::Visibility::Collapsed
                                                                         : mux::Visibility::Visible);
    }
    void Cancel()
    {
        timer.Stop();
        session.reset();
    }
    void Status(const std::wstring &text)
    {
        status.Text(text);
        status.Visibility(text.empty() ? mux::Visibility::Collapsed : mux::Visibility::Visible);
    }
    void Reload()
    {
        Cancel();
        if (!active || closed)
            return;
        auto query = request;
        query.catalogueOnly = true;
        queryGeneration = ext::MenuCacheGeneration();
        cacheTicket = ext::SharedMenuCache().Capture(query);
        if (auto cached = ext::SharedMenuCache().Find(cacheTicket))
        {
            entries = std::move(cached->entries);
            Status(entries.empty() ? L("settings.contextMenu.empty") : L"");
        }
        else
        {
            entries.clear();
            Status(L("settings.contextMenu.loading"));
        }
        BuildRows();
        try
        {
            session = std::make_unique<ext::Session>(query);
            timer.Start();
        }
        catch (...)
        {
            Status(L("settings.contextMenu.failed"));
        }
    }
    void Poll()
    {
        if (!session)
            return;
        auto reply = session->Poll();
        if (!reply)
            return;
        timer.Stop();
        if (queryGeneration != ext::MenuCacheGeneration())
        {
            Reload();
            return;
        }
        if (reply->ok)
        {
            ext::SharedMenuCache().Store(cacheTicket, *reply);
            entries = std::move(reply->entries);
            Status(entries.empty() ? L("settings.contextMenu.empty") : L"");
            BuildRows();
        }
        else
            Status(L("settings.contextMenu.failed"));
        session.reset();
    }
    void Pick()
    {
        const auto lifetime = shared_from_this();
        const auto context = request.context;
        Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            return;
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                           (context == ext::Context::File ? FOS_FILEMUSTEXIST : FOS_PICKFOLDERS));
        if (FAILED(dialog->Show(GetActiveWindow())) || closed || !active || request.context != context)
            return;
        Microsoft::WRL::ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        {
            request.paths = {path};
            CoTaskMemFree(path);
            Reload();
        }
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
        for (auto &r : rowRevoke)
            r();
        rowRevoke.clear();
        rows.clear();
        list.Items().Clear();
        auto filter = std::wstring(search.Text());
        for (auto &c : filter)
            c = towlower(c);
        for (const auto &entry : entries)
        {
            if (entry.separator)
                continue;
            auto label = entry.label;
            for (auto &c : label)
                c = towlower(c);
            if (!filter.empty() && label.find(filter) == std::wstring::npos)
                continue;
            muxc::Grid layout;
            Column(layout, 32, mux::GridUnitType::Pixel);
            Column(layout, 1, mux::GridUnitType::Star);
            Column(layout, 1, mux::GridUnitType::Auto);
            layout.Children().Append(Icon(entry));
            muxc::TextBlock title;
            title.Text(entry.label);
            title.VerticalAlignment(mux::VerticalAlignment::Center);
            title.TextWrapping(mux::TextWrapping::Wrap);
            title.Margin({0, 0, 16, 0});
            muxc::Grid::SetColumn(title, 1);
            layout.Children().Append(title);
            Row row;
            row.id = entry.provider;
            row.toggle.OnContent(winrt::box_value(L""));
            row.toggle.OffContent(winrt::box_value(L""));
            row.toggle.MinWidth(44);
            row.toggle.VerticalAlignment(mux::VerticalAlignment::Center);
            row.toggle.IsOn(!ext::IsHidden(prefs, row.id, request.context));
            mux::Automation::AutomationProperties::SetName(row.toggle, entry.label);
            muxc::Grid::SetColumn(row.toggle, 2);
            layout.Children().Append(row.toggle);
            auto token = row.toggle.Toggled(
                [this, id = row.id, toggle = row.toggle, context = request.context](auto &&, auto &&) {
                    if (closed || updating || !active || !initialized || !actions.updateGeneral)
                        return;
                    const bool hidden = !toggle.IsOn();
                    actions.updateGeneral(generation, SettingsUpdateMode::PreviewAndCommit,
                                          [id, context, hidden](auto &settings) {
                                              ext::SetHidden(settings.shellExtensions, id, context, hidden);
                                          });
                });
            rowRevoke.push_back([toggle = row.toggle, token] { toggle.Toggled(token); });
            rows.push_back(std::move(row));
            list.Items().Append(layout);
        }
    }
    void Apply(const SettingsSnapshot &snapshot)
    {
        if (closed)
            return;
        if (generation != snapshot.generation)
            Cancel();
        generation = snapshot.generation;
        initialized = snapshot.initialized;
        prefs = snapshot.values.general.shellExtensions;
        updating = true;
        for (auto &row : rows)
            row.toggle.IsOn(!ext::IsHidden(prefs, row.id, request.context));
        updating = false;
        if (!snapshot.sessionActive)
            Deactivate();
    }
    void Deactivate()
    {
        active = false;
        Cancel();
    }
    void Close()
    {
        if (closed)
            return;
        closed = true;
        Deactivate();
        for (auto &r : rowRevoke)
            r();
        rowRevoke.clear();
        for (auto &r : revoke)
            r();
        revoke.clear();
        actions = {};
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
    const bool wasActive = impl_->active;
    impl_->active = true;
    if (!wasActive)
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

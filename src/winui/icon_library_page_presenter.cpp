#include "pch.h"
#include "icon_library_page_presenter.h"
#include "settings_presenter_controls.h"
#include <shobjidl.h>
#include <wrl/client.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Storage.h>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <utility>
#include <unordered_map>

namespace snowdesktop::winui
{
namespace x = winrt::Microsoft::UI::Xaml;
namespace c = x::Controls;
namespace m = x::Media;
namespace dt = winrt::Windows::ApplicationModel::DataTransfer;
namespace
{
std::vector<std::wstring> PickFiles(bool folder)
{
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    DWORD flags = 0; dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST | FOS_ALLOWMULTISELECT));
    const COMDLG_FILTERSPEC filter[]{{L"PNG, ICO, JPEG, BMP, SVG, WebP", L"*.png;*.ico;*.jpg;*.jpeg;*.bmp;*.svg;*.webp"}};
    if (!folder) dialog->SetFileTypes(1, filter);
    const HWND owner = GetActiveWindow(); DWORD process = 0;
    if (owner) GetWindowThreadProcessId(owner, &process);
    if (FAILED(dialog->Show(process == GetCurrentProcessId() ? owner : nullptr))) return {};
    Microsoft::WRL::ComPtr<IShellItemArray> items;
    if (FAILED(dialog->GetResults(&items))) return {};
    DWORD count = 0; items->GetCount(&count);
    std::vector<std::wstring> paths;
    for (DWORD i = 0; i < count; ++i)
    {
        Microsoft::WRL::ComPtr<IShellItem> item; PWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        { paths.emplace_back(path); CoTaskMemFree(path); }
    }
    return paths;
}
}

struct IconLibraryPagePresenter::Impl : std::enable_shared_from_this<Impl>
{
    static std::function<void()> ShowDialog(x::XamlRoot root, Localize localize,
        IconLibraryAction action, std::string selected, std::function<void(std::string)> completed);
    static winrt::fire_and_forget AwaitDialog(std::shared_ptr<Impl> view, c::ContentDialog dialog, std::function<void(std::string)> completed);
    Localize localize; IconLibraryAction action;
    IconLibrarySnapshot snapshot;
    c::StackPanel root, toolbar;
    c::TextBox search;
    c::GridView grid;
    c::InfoBar status;
    c::TextBlock empty;
    c::Button import, rename, remove, png, ico, retry;
    std::function<void()> cancelPicker;
    x::DispatcherTimer timer;
    bool active = false, picker = false, rendering = false, dockPickerStarted = false;
    std::wstring signature;
    std::unordered_map<std::string, IconLibraryEntry> visible;
    std::function<void()> selectionChanged;

    std::wstring L(std::string_view key) const { return localize ? localize(key) : std::wstring{}; }
    std::vector<std::string> Selected() const
    {
        std::vector<std::string> result;
        for (const auto& item : grid.SelectedItems()) result.push_back(winrt::to_string(winrt::unbox_value<winrt::hstring>(item)));
        return result;
    }
    c::StackPanel Tile(const IconLibraryEntry& entry)
    {
        c::StackPanel tile; tile.Width(132); tile.Spacing(4); tile.Padding({6,6,6,6});
        c::Grid checker; checker.Width(112); checker.Height(96);
        // Small neutral cells make transparent margins visible in both themes.
        for (int y = 0; y < 6; ++y) for (int xx = 0; xx < 7; ++xx)
        {
            c::Border cell; cell.Width(16); cell.Height(16); cell.HorizontalAlignment(x::HorizontalAlignment::Left);
            cell.VerticalAlignment(x::VerticalAlignment::Top); cell.Margin({double(xx * 16), double(y * 16), 0, 0});
            cell.Background(m::SolidColorBrush(winrt::Windows::UI::Color{255,
                static_cast<uint8_t>((xx + y) % 2 ? 184 : 224), static_cast<uint8_t>((xx + y) % 2 ? 184 : 224), static_cast<uint8_t>((xx + y) % 2 ? 184 : 224)}));
            checker.Children().Append(cell);
        }
        c::Image image; image.Stretch(m::Stretch::Uniform);
        if (!entry.previewPath.empty()) image.Source(m::Imaging::BitmapImage(winrt::Windows::Foundation::Uri(entry.previewPath)));
        checker.Children().Append(image); tile.Children().Append(checker);
        c::TextBlock name; name.Text(entry.name); name.TextTrimming(x::TextTrimming::CharacterEllipsis); tile.Children().Append(name);
        c::TextBlock detail; detail.FontSize(11); detail.Opacity(.7);
        detail.Text(entry.missing ? L("iconLibrary.missing") : std::to_wstring(entry.width) + L" × " + std::to_wstring(entry.height) + L" · " + winrt::to_hstring(entry.format).c_str());
        tile.Children().Append(detail);
        c::TextBlock uses; uses.FontSize(11); uses.Text(entry.uses.empty() ? L("iconLibrary.unused") : L("iconLibrary.inUse"));
        tile.Children().Append(uses);
        std::wstring accessible = entry.name;
        for (const auto& use : entry.uses) accessible += L"\n" + use;
        x::Automation::AutomationProperties::SetName(tile, accessible);
        c::ToolTipService::SetToolTip(tile, winrt::box_value(accessible));
        return tile;
    }
    void UpdateButtons()
    {
        const auto selected = Selected();
        import.IsEnabled(!snapshot.busy); rename.IsEnabled(!snapshot.busy && selected.size() == 1);
        remove.IsEnabled(!snapshot.busy && !selected.empty()); png.IsEnabled(!snapshot.busy && !selected.empty()); ico.IsEnabled(!snapshot.busy && !selected.empty());
        if (selectionChanged) selectionChanged();
    }
    void Render()
    {
        std::wstring next;
        const auto query = search.Text();
        std::wstring lowerQuery(query.c_str());
        std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), [](wchar_t v) { return static_cast<wchar_t>(std::towlower(v)); });
        for (const auto& entry : snapshot.entries)
            next += winrt::to_hstring(entry.id).c_str() + entry.name + entry.previewPath + std::to_wstring(entry.uses.size()) + std::to_wstring(entry.missing);
        next += lowerQuery;
        if (signature != next)
        {
            signature = std::move(next); const auto selected = Selected(); rendering = true;
            grid.Items().Clear(); visible.clear();
            for (const auto& entry : snapshot.entries)
            {
                auto name = entry.name; std::transform(name.begin(), name.end(), name.begin(), [](wchar_t v) { return static_cast<wchar_t>(std::towlower(v)); });
                if (!lowerQuery.empty() && name.find(lowerQuery) == name.npos) continue;
                visible.emplace(entry.id, entry); const auto item = winrt::box_value(winrt::to_hstring(entry.id)); grid.Items().Append(item);
                if (std::find(selected.begin(), selected.end(), entry.id) != selected.end()) grid.SelectedItems().Append(item);
            }
            rendering = false;
        }
        empty.Text(L(snapshot.entries.empty() ? "iconLibrary.empty" : "iconLibrary.noMatches"));
        empty.Visibility(grid.Items().Size() ? x::Visibility::Collapsed : x::Visibility::Visible);
        std::wstring message = snapshot.busy ? L("iconLibrary.working") : L(snapshot.error);
        for (const auto& failure : snapshot.failures) message += L"\n" + failure;
        status.Message(message); status.IsOpen(!message.empty());
        status.Severity(snapshot.error.empty() ? c::InfoBarSeverity::Informational : c::InfoBarSeverity::Error);
        retry.Visibility(snapshot.error.empty() ? x::Visibility::Collapsed : x::Visibility::Visible);
        retry.IsEnabled(!snapshot.busy);
        timer.Interval(snapshot.busy ? std::chrono::milliseconds(300) : std::chrono::milliseconds(2000));
        UpdateButtons();
    }
    bool Send(IconLibraryRequest request)
    {
        if (!action) return false;
        try { snapshot = action(std::move(request)); }
        catch (...) { snapshot.busy = false; snapshot.error = "iconLibrary.unavailable"; }
        Render(); return snapshot.succeeded;
    }
    void Import(std::vector<std::wstring> paths)
    {
        if (paths.empty() || snapshot.busy) return;
        IconLibraryRequest request; request.action = "import"; request.paths = std::move(paths); Send(std::move(request));
    }
    void Export(std::string format)
    {
        const auto ids = Selected(); if (ids.empty() || snapshot.busy) return;
        const auto paths = PickFiles(true); if (paths.empty()) return;
        IconLibraryRequest request; request.action = "export"; request.ids = ids; request.format = std::move(format); request.destination = paths.front(); Send(std::move(request));
    }
    winrt::fire_and_forget Rename()
    {
        const auto lifetime = shared_from_this(); const auto ids = Selected(); if (ids.size() != 1 || snapshot.busy) co_return;
        const auto entry = visible.find(ids[0]); if (entry == visible.end()) co_return;
        c::TextBox name; name.Header(winrt::box_value(L("iconLibrary.name"))); name.Text(entry->second.name); name.MaxLength(256);
        c::ContentDialog dialog; dialog.XamlRoot(root.XamlRoot()); dialog.Title(winrt::box_value(L("iconLibrary.rename")));
        dialog.PrimaryButtonText(L("app.settings.apply")); dialog.CloseButtonText(L("app.settings.cancel")); dialog.Content(name);
        try
        {
            if (co_await dialog.ShowAsync() == c::ContentDialogResult::Primary && active)
            { IconLibraryRequest request; request.action = "rename"; request.ids = ids; request.name = name.Text(); Send(std::move(request)); }
        }
        catch (...) { /* The owning settings island may have closed. */ }
    }
    winrt::fire_and_forget Remove()
    {
        const auto lifetime = shared_from_this(); const auto ids = Selected(); if (ids.empty() || snapshot.busy) co_return;
        std::wstring names;
        for (const auto& id : ids)
        {
            const auto entry = visible.find(id); if (entry == visible.end()) continue;
            if (!entry->second.uses.empty())
            {
                status.Message(L("iconLibrary.deleteInUse")); status.IsOpen(true); status.Severity(c::InfoBarSeverity::Warning); co_return;
            }
            names += (names.empty() ? L"" : L"\n") + entry->second.name;
        }
        c::ContentDialog dialog; dialog.XamlRoot(root.XamlRoot()); dialog.Title(winrt::box_value(L("iconLibrary.delete")));
        dialog.Content(winrt::box_value(names)); dialog.PrimaryButtonText(L("app.settings.delete")); dialog.CloseButtonText(L("app.settings.cancel"));
        dialog.DefaultButton(c::ContentDialogButton::Close);
        try
        {
            if (co_await dialog.ShowAsync() == c::ContentDialogResult::Primary && active)
            { IconLibraryRequest request; request.action = "delete"; request.ids = ids; Send(std::move(request)); }
        }
        catch (...) { }
    }
    winrt::fire_and_forget Drop(x::DragEventArgs args)
    {
        const auto lifetime = shared_from_this();
        const auto deferral = args.GetDeferral();
        try
        {
            std::vector<std::wstring> paths;
            const auto items = co_await args.DataView().GetStorageItemsAsync();
            if (active)
            {
                for (const auto& item : items) if (item.IsOfType(winrt::Windows::Storage::StorageItemTypes::File)) paths.emplace_back(item.Path());
                Import(std::move(paths));
                args.AcceptedOperation(dt::DataPackageOperation::Copy);
            }
        }
        catch (...) { }
        deferral.Complete();
    }
    void Build()
    {
        presenter_controls::BindInfoBarVisibility(status);
        root.Spacing(12); toolbar.Orientation(c::Orientation::Horizontal); toolbar.Spacing(8);
        search.PlaceholderText(L("iconLibrary.search")); search.Header(winrt::box_value(L("iconLibrary.search")));
        x::Automation::AutomationProperties::SetAutomationId(search, L"IconLibrarySearch");
        root.Children().Append(search); root.Children().Append(toolbar);
        const auto add = [&](c::Button button, const char* key, auto click) {
            button.Content(winrt::box_value(L(key))); toolbar.Children().Append(button); button.Click(click);
        };
        const auto weak = weak_from_this();
        add(import, "iconLibrary.import", [weak](auto&&, auto&&) { if (auto self = weak.lock()) self->Import(PickFiles(false)); });
        add(retry, "app.operation.retry", [weak](auto&&, auto&&) { if (auto self = weak.lock()) { IconLibraryRequest request; request.action = "refresh"; self->Send(request); } });
        add(rename, "iconLibrary.rename", [weak](auto&&, auto&&) { if (auto self = weak.lock()) self->Rename(); });
        add(remove, "iconLibrary.delete", [weak](auto&&, auto&&) { if (auto self = weak.lock()) self->Remove(); });
        add(png, "iconLibrary.exportPng", [weak](auto&&, auto&&) { if (auto self = weak.lock()) self->Export("png"); });
        add(ico, "iconLibrary.exportIco", [weak](auto&&, auto&&) { if (auto self = weak.lock()) self->Export("ico"); });
        if (picker) { rename.Visibility(x::Visibility::Collapsed); remove.Visibility(x::Visibility::Collapsed); png.Visibility(x::Visibility::Collapsed); ico.Visibility(x::Visibility::Collapsed); }
        grid.Height(picker ? 340 : 460); grid.SelectionMode(picker ? c::ListViewSelectionMode::Single : c::ListViewSelectionMode::Multiple);
        x::Automation::AutomationProperties::SetAutomationId(grid, L"IconLibraryGrid");
        grid.ContainerContentChanging([weak](auto&&, const c::ContainerContentChangingEventArgs& args) {
            if (auto self = weak.lock())
            {
                if (args.InRecycleQueue()) { args.ItemContainer().Content(nullptr); return; }
                const auto id = winrt::to_string(winrt::unbox_value<winrt::hstring>(args.Item()));
                if (const auto entry = self->visible.find(id); entry != self->visible.end())
                { args.ItemContainer().Content(self->Tile(entry->second)); args.Handled(true); }
            }
        });
        grid.SelectionChanged([weak](auto&&, auto&&) { if (auto self = weak.lock(); self && !self->rendering) self->UpdateButtons(); });
        search.TextChanged([weak](auto&&, auto&&) { if (auto self = weak.lock()) self->Render(); });
        root.Children().Append(status); root.Children().Append(empty); root.Children().Append(grid);
        root.AllowDrop(true);
        root.DragOver([weak](auto&&, const x::DragEventArgs& args) {
            if (auto self = weak.lock(); self && !self->snapshot.busy && args.DataView().Contains(dt::StandardDataFormats::StorageItems()))
                args.AcceptedOperation(dt::DataPackageOperation::Copy);
        });
        root.Drop([weak](auto&&, const x::DragEventArgs& args) { if (auto self = weak.lock()) self->Drop(args); });
        timer.Interval(std::chrono::milliseconds(500));
        timer.Tick([weak](auto&&, auto&&) { if (auto self = weak.lock(); self && self->active) self->Send({}); });
        toolbar.SizeChanged([weak](auto&&, const x::SizeChangedEventArgs&) {
            if (auto self = weak.lock()) self->toolbar.Orientation(self->root.ActualWidth() < 600 ? c::Orientation::Vertical : c::Orientation::Horizontal);
        });
    }
};

IconLibraryPagePresenter::IconLibraryPagePresenter(Localize localize, IconLibraryAction action) : impl_(std::make_shared<Impl>())
{ impl_->localize = std::move(localize); impl_->action = std::move(action); impl_->Build(); }
IconLibraryPagePresenter::~IconLibraryPagePresenter() { Deactivate(); }
c::StackPanel IconLibraryPagePresenter::Content() const { return impl_->root; }
void IconLibraryPagePresenter::Activate(std::wstring target)
{
    if (impl_->active) return;
    impl_->dockPickerStarted = false;
    impl_->active = true; IconLibraryRequest refresh; refresh.action = "refresh"; impl_->Send(refresh); impl_->timer.Start();
    if (target.empty()) return;
    const auto weak = std::weak_ptr<Impl>(impl_);
    const auto open = [weak, target = std::move(target)] {
        const auto self = weak.lock();
        if (!self || !self->active || self->dockPickerStarted || !self->root.XamlRoot()) return;
        self->dockPickerStarted = true;
        IconLibraryRequest read; read.target = target;
        if (!self->Send(read)) return;
        const auto session = self->snapshot.session;
        self->cancelPicker = Pick(self->root.XamlRoot(), self->localize, self->action, {}, [weak, target, session](std::string id) {
            if (auto current = weak.lock(); current && current->active)
            {
                IconLibraryRequest request; request.action = id.empty() ? "cancel" : "apply";
                request.target = target; request.session = session; if (!id.empty()) request.ids.push_back(std::move(id));
                current->Send(std::move(request));
            }
        });
    };
    if (impl_->root.XamlRoot()) open();
    else impl_->root.Loaded([open](auto&&, auto&&) { open(); });
}
void IconLibraryPagePresenter::Deactivate()
{
    impl_->active = false; impl_->timer.Stop();
    if (auto cancel = std::exchange(impl_->cancelPicker, {})) cancel();
}
void IconLibraryPagePresenter::RefreshLocalizedText()
{
    impl_->search.Header(winrt::box_value(impl_->L("iconLibrary.search"))); impl_->search.PlaceholderText(impl_->L("iconLibrary.search"));
    impl_->import.Content(winrt::box_value(impl_->L("iconLibrary.import"))); impl_->rename.Content(winrt::box_value(impl_->L("iconLibrary.rename")));
    impl_->remove.Content(winrt::box_value(impl_->L("iconLibrary.delete"))); impl_->png.Content(winrt::box_value(impl_->L("iconLibrary.exportPng")));
    impl_->ico.Content(winrt::box_value(impl_->L("iconLibrary.exportIco"))); impl_->signature.clear(); impl_->Render();
}
std::function<void()> IconLibraryPagePresenter::Impl::ShowDialog(x::XamlRoot root, IconLibraryPagePresenter::Localize localize,
    IconLibraryAction action, std::string selected, std::function<void(std::string)> completed)
{
    const auto view = std::make_shared<IconLibraryPagePresenter::Impl>();
    view->localize = localize; view->action = std::move(action); view->picker = true; view->Build(); view->active = true;
    c::ContentDialog dialog; dialog.XamlRoot(root); dialog.Title(winrt::box_value(localize("iconLibrary.choose")));
    dialog.PrimaryButtonText(localize("iconLibrary.use")); dialog.CloseButtonText(localize("app.settings.cancel"));
    dialog.IsPrimaryButtonEnabled(false); dialog.Content(view->root);
    const auto availableWidth = root ? root.Size().Width : 640.f;
    view->root.Width(std::max(180., std::min(640., double(availableWidth) - 96)));
    view->selectionChanged = [weak = std::weak_ptr<IconLibraryPagePresenter::Impl>(view), dialog] {
        if (const auto self = weak.lock())
        {
            const auto ids = self->Selected(); const auto entry = ids.size() == 1 ? self->visible.find(ids[0]) : self->visible.end();
            dialog.IsPrimaryButtonEnabled(!self->snapshot.busy && entry != self->visible.end() && !entry->second.missing);
        }
    };
    view->Send({});
    if (!selected.empty()) for (const auto& item : view->grid.Items())
        if (winrt::to_string(winrt::unbox_value<winrt::hstring>(item)) == selected) { view->grid.SelectedItem(item); break; }
    view->timer.Start();
    AwaitDialog(view, dialog, std::move(completed));
    return [dialog] { try { dialog.Hide(); } catch (...) {} };
}
winrt::fire_and_forget IconLibraryPagePresenter::Impl::AwaitDialog(std::shared_ptr<Impl> view, c::ContentDialog dialog,
    std::function<void(std::string)> completed)
{
    std::string result;
    try { if (co_await dialog.ShowAsync() == c::ContentDialogResult::Primary) { const auto ids = view->Selected(); if (ids.size() == 1) result = ids[0]; } }
    catch (...) { }
    view->active = false; view->timer.Stop(); view->selectionChanged = {};
    if (completed) completed(std::move(result));
}
std::function<void()> IconLibraryPagePresenter::Pick(x::XamlRoot root, Localize localize, IconLibraryAction action,
    std::string selected, std::function<void(std::string)> completed)
{
    if (!root) { if (completed) completed({}); return {}; }
    return Impl::ShowDialog(root, std::move(localize), std::move(action), std::move(selected), std::move(completed));
}
}

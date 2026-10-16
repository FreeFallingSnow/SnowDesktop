#include "pch.h"
#include "icon_library_page_presenter.h"
#include "settings_presenter_controls.h"
#include <shobjidl.h>
#include <wrl/client.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
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
    const COMDLG_FILTERSPEC filter[]{{L"PNG, ICO, JPEG, BMP, SVG, WebP, ZIP, APK", L"*.png;*.ico;*.jpg;*.jpeg;*.bmp;*.svg;*.webp;*.zip;*.apk"}};
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
    c::StackPanel root;
    c::CommandBar toolbar;
    c::ComboBox groups;
    c::AppBarButton newGroup, renameGroup, deleteGroup, moveGroup;
    std::string groupId;
    std::wstring groupSignature;
    bool updatingGroups = false;
    c::TextBox search;
    c::GridView grid;
    c::InfoBar status;
    c::TextBlock empty;
    c::AppBarButton import, rename, remove, png, ico, retry, selectAll;
    std::function<void()> cancelPicker;
    x::DispatcherTimer timer;
    bool active = false, picker = false, rendering = false, dockPickerStarted = false, importing = false;
    std::uint64_t pickerGeneration = 0;
    std::wstring signature;
    std::unordered_map<std::string, IconLibraryEntry> visible;
    std::function<void()> selectionChanged;

    std::wstring L(std::string_view key) const { return localize ? localize(key) : std::wstring{}; }
    std::vector<std::string> Selected() const
    {
        std::vector<std::string> result;
        for (const auto& item : grid.SelectedItems()) result.push_back(ItemId(item));
        return result;
    }
    static std::string ItemId(const winrt::Windows::Foundation::IInspectable& item)
    {
        const auto container = item.try_as<c::GridViewItem>();
        return container ? winrt::to_string(winrt::unbox_value_or<winrt::hstring>(container.Tag(), {})) : std::string{};
    }
    static winrt::fire_and_forget LoadPreview(c::Image image, c::TextBlock missing, std::wstring path)
    {
        const winrt::apartment_context foreground;
        winrt::Windows::Storage::Streams::IRandomAccessStreamWithContentType stream{nullptr};
        try
        {
            // Storage activation may synchronously pump COM messages. Starting
            // it inside GridViewItem.Loaded reenters XAML layout and can fail
            // fast before the coroutine reaches its first suspension.
            co_await winrt::resume_background();
            const auto file = co_await winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(path);
            stream = co_await file.OpenReadAsync();
        }
        catch (...) { }
        bool resumed=false;
        try
        {
            co_await foreground;
            resumed=true;
            if(!image.XamlRoot())co_return;
            if(!stream){missing.Visibility(x::Visibility::Visible);co_return;}
            m::Imaging::BitmapImage bitmap;
            co_await bitmap.SetSourceAsync(stream);
            if (bitmap.PixelWidth() <= 0 || bitmap.PixelHeight() <= 0) throw winrt::hresult_invalid_argument();
            image.Source(bitmap); missing.Visibility(x::Visibility::Collapsed);
        }
        catch (...)
        {
            if(resumed)try{image.Source(nullptr);missing.Visibility(x::Visibility::Visible);}catch(...){}
            // Island shutdown must not escape a fire-and-forget task.
        }
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
        x::Automation::AutomationProperties::SetAutomationId(image, L"IconLibraryPreview-" + winrt::to_hstring(entry.id));
        image.Tag(winrt::box_value(entry.missing));
        image.Width(112); image.Height(96);
        c::TextBlock missing; missing.Text(L("iconLibrary.missing")); missing.TextWrapping(x::TextWrapping::Wrap);
        missing.Visibility(entry.missing ? x::Visibility::Visible : x::Visibility::Collapsed);
        checker.Children().Append(image); checker.Children().Append(missing); tile.Children().Append(checker);
        if (!entry.previewPath.empty() && !entry.missing) LoadPreview(image, missing, entry.previewPath);
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
        newGroup.IsEnabled(!snapshot.busy);
        renameGroup.IsEnabled(!snapshot.busy && !groupId.empty() && groupId != "ungrouped");
        deleteGroup.IsEnabled(renameGroup.IsEnabled()); moveGroup.IsEnabled(!snapshot.busy && !selected.empty());
        selectAll.IsEnabled(!snapshot.busy&&grid.Items().Size()>0);
        if (selectionChanged) selectionChanged();
    }
    void Render()
    {
        std::string selectImported;
        if (importing && !snapshot.busy)
        {
            importing = false;
            if (!snapshot.importedGroupId.empty()) { groupId = snapshot.importedGroupId; groupSignature.clear(); signature.clear(); }
            if (picker && snapshot.imported.size() == 1) selectImported = snapshot.imported.front();
        }
        std::wstring next = std::to_wstring(snapshot.revision) + winrt::to_hstring(groupId).c_str();
        std::wstring nextGroups;
        for (const auto& group : snapshot.groups) nextGroups += winrt::to_hstring(group.id).c_str() + group.name;
        if (groupSignature != nextGroups || !groups.Items().Size())
        {
            groupSignature = std::move(nextGroups); updatingGroups = true; groups.Items().Clear();
            const auto append = [&](const std::string& id, const std::wstring& name) {
                c::ComboBoxItem item; item.Tag(winrt::box_value(winrt::to_hstring(id))); item.Content(winrt::box_value(name));
                groups.Items().Append(item); if (id == groupId) groups.SelectedItem(item);
            };
            append({}, L("iconLibrary.allGroups")); append("ungrouped", L("iconLibrary.ungrouped"));
            for (const auto& group : snapshot.groups) append(group.id, group.name);
            if (groups.SelectedIndex() < 0) { groupId.clear(); groups.SelectedIndex(0); }
            updatingGroups = false;
        }
        const auto query = search.Text();
        std::wstring lowerQuery(query.c_str());
        std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), [](wchar_t v) { return static_cast<wchar_t>(std::towlower(v)); });
        for (const auto& entry : snapshot.entries)
            next += winrt::to_hstring(entry.id).c_str() + entry.name + entry.previewPath + std::to_wstring(entry.uses.size()) + std::to_wstring(entry.missing);
        next += lowerQuery;
        if (signature != next)
        {
            signature = std::move(next); const auto selected = selectImported.empty() ? Selected() : std::vector<std::string>{selectImported}; rendering = true;
            grid.Items().Clear(); visible.clear();
            for (const auto& entry : snapshot.entries)
            {
                auto name = entry.name; std::transform(name.begin(), name.end(), name.begin(), [](wchar_t v) { return static_cast<wchar_t>(std::towlower(v)); });
                if (!lowerQuery.empty() && name.find(lowerQuery) == name.npos) continue;
                if (groupId == "ungrouped" ? !entry.groupIds.empty() :
                    (!groupId.empty() && std::find(entry.groupIds.begin(), entry.groupIds.end(), groupId) == entry.groupIds.end())) continue;
                visible.emplace(entry.id, entry);
                c::GridViewItem item; item.Tag(winrt::box_value(winrt::to_hstring(entry.id)));
                item.HorizontalContentAlignment(x::HorizontalAlignment::Stretch);
                item.Width(148); item.MinHeight(176);
                x::Automation::AutomationProperties::SetName(item, entry.name);
                const auto weak = weak_from_this();
                item.Loaded([weak, entry](const auto& sender, auto&&) {
                    if (const auto self = weak.lock())
                    { const auto container = sender.template as<c::GridViewItem>(); if (!container.Content()) container.Content(self->Tile(entry)); }
                });
                item.Unloaded([](const auto& sender, auto&&) { sender.template as<c::GridViewItem>().Content(nullptr); });
                grid.Items().Append(item);
                if (entry.id == selectImported || std::find(selected.begin(), selected.end(), entry.id) != selected.end()) grid.SelectedItems().Append(item);
            }
            rendering = false;
        }
        empty.Text(L("iconLibrary.noMatches"));
        empty.Visibility(!snapshot.entries.empty() && !grid.Items().Size() ? x::Visibility::Visible : x::Visibility::Collapsed);
        std::wstring message = snapshot.busy ? L("iconLibrary.working") : L(snapshot.error);
        for (size_t i = 0; i < std::min<size_t>(snapshot.failures.size(), 12); ++i) message += L"\n" + snapshot.failures[i];
        if (snapshot.failures.size() > 12) message += L"\n… (" + std::to_wstring(snapshot.failures.size()) + L")";
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
        importing = true;
        IconLibraryRequest request; request.action = "import"; request.paths = std::move(paths); request.groupId = groupId == "ungrouped" ? "" : groupId; Send(std::move(request));
    }
    void Export(std::string format)
    {
        const auto ids = Selected(); if (ids.empty() || snapshot.busy) return;
        const auto paths = PickFiles(true); if (paths.empty()) return;
        IconLibraryRequest request; request.action = "export"; request.ids = ids; request.format = std::move(format); request.destination = paths.front(); Send(std::move(request));
    }
    winrt::fire_and_forget EditGroup(bool create)
    {
        const auto lifetime = shared_from_this(); const auto id = groupId;
        if (snapshot.busy) co_return;
        c::TextBox name; name.MaxLength(256);
        if (!create) for (const auto& group : snapshot.groups) if (group.id == id) name.Text(group.name);
        x::Automation::AutomationProperties::SetName(name, L("iconLibrary.name"));
        c::ContentDialog dialog; dialog.XamlRoot(root.XamlRoot()); dialog.Title(winrt::box_value(L(create ? "iconLibrary.newGroup" : "iconLibrary.renameGroup")));
        dialog.Content(name); dialog.PrimaryButtonText(L("app.settings.apply")); dialog.CloseButtonText(L("app.settings.cancel"));
        try { if (co_await dialog.ShowAsync() == c::ContentDialogResult::Primary && active)
            { IconLibraryRequest request; request.action = create ? "createGroup" : "renameGroup"; request.groupId = id; request.name = name.Text(); Send(request); } }
        catch (...) { }
    }
    winrt::fire_and_forget DeleteGroup()
    {
        const auto lifetime = shared_from_this(); const auto id = groupId;
        if (snapshot.busy || id.empty() || id == "ungrouped") co_return;
        c::ContentDialog dialog; dialog.XamlRoot(root.XamlRoot()); dialog.Title(winrt::box_value(L("iconLibrary.deleteGroup")));
        c::StackPanel content;content.Spacing(8);
        c::CheckBox withIcons;withIcons.Content(winrt::box_value(L("iconLibrary.deleteGroupIcons")));
        c::TextBlock hint;hint.Text(L("iconLibrary.deleteGroupHint"));hint.TextWrapping(x::TextWrapping::Wrap);
        c::TextBlock uses;uses.TextWrapping(x::TextWrapping::Wrap);
        std::wstring affected;
        for(const auto& entry:snapshot.entries)
            if(std::find(entry.groupIds.begin(),entry.groupIds.end(),id)!=entry.groupIds.end())
                for(const auto& use:entry.uses)affected+=L"\n"+entry.name+L" · "+use;
        uses.Text(affected.empty()?L"":L("iconLibrary.deleteRestoreUses")+affected);
        c::ScrollViewer references;references.MaxHeight(180);references.Content(uses);references.Visibility(x::Visibility::Collapsed);
        const auto update=[withIcons,hint,references,affected,keep=L("iconLibrary.deleteGroupHint"),removeHint=L("iconLibrary.deleteGroupIconsHint")](auto&&,auto&&){
            const bool removeIcons=withIcons.IsChecked().Value();hint.Text(removeIcons?removeHint:keep);
            references.Visibility(removeIcons&&!affected.empty()?x::Visibility::Visible:x::Visibility::Collapsed);
        };
        withIcons.Checked(update);withIcons.Unchecked(update);
        content.Children().Append(withIcons);content.Children().Append(hint);content.Children().Append(references);
        dialog.Content(content); dialog.PrimaryButtonText(L("app.settings.delete"));
        dialog.CloseButtonText(L("app.settings.cancel")); dialog.DefaultButton(c::ContentDialogButton::Close);
        try { if (co_await dialog.ShowAsync() == c::ContentDialogResult::Primary && active)
            { IconLibraryRequest request; request.action = withIcons.IsChecked().Value()?"deleteGroupWithAssets":"deleteGroup"; request.groupId = id; Send(request); } }
        catch (...) { }
    }
    winrt::fire_and_forget MoveGroup()
    {
        const auto lifetime = shared_from_this(); const auto ids = Selected();
        if (snapshot.busy || ids.empty()) co_return;
        c::ComboBox choices; choices.HorizontalAlignment(x::HorizontalAlignment::Stretch);
        c::ComboBoxItem none; none.Tag(winrt::box_value(L"")); none.Content(winrt::box_value(L("iconLibrary.ungrouped"))); choices.Items().Append(none);
        for (const auto& group : snapshot.groups)
        { c::ComboBoxItem item; item.Content(winrt::box_value(group.name)); item.Tag(winrt::box_value(winrt::to_hstring(group.id))); choices.Items().Append(item); }
        choices.SelectedIndex(0);
        c::ContentDialog dialog; dialog.XamlRoot(root.XamlRoot()); dialog.Title(winrt::box_value(L("iconLibrary.moveGroup")));
        dialog.Content(choices); dialog.PrimaryButtonText(L("app.settings.apply")); dialog.CloseButtonText(L("app.settings.cancel"));
        try { if (co_await dialog.ShowAsync() == c::ContentDialogResult::Primary && active)
            { IconLibraryRequest request; request.action = "moveGroup"; request.ids = ids;
                request.groupId = winrt::to_string(winrt::unbox_value<winrt::hstring>(choices.SelectedItem().as<c::ComboBoxItem>().Tag())); Send(request); } }
        catch (...) { }
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
            names += (names.empty() ? L"" : L"\n") + entry->second.name;
            if(!entry->second.uses.empty())
            {
                names+=L"\n"+L("iconLibrary.deleteRestoreUses");
                for(const auto& use:entry->second.uses)names+=L"\n  "+use;
            }
        }
        c::ContentDialog dialog; dialog.XamlRoot(root.XamlRoot()); dialog.Title(winrt::box_value(L("iconLibrary.delete")));
        c::TextBlock detail;detail.Text(names);detail.TextWrapping(x::TextWrapping::Wrap);
        c::ScrollViewer details;details.MaxHeight(280);details.Content(detail);
        dialog.Content(details); dialog.PrimaryButtonText(L("app.settings.delete")); dialog.CloseButtonText(L("app.settings.cancel"));
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
        root.Spacing(8);
        toolbar.DefaultLabelPosition(c::CommandBarDefaultLabelPosition::Right);
        toolbar.IsDynamicOverflowEnabled(true); toolbar.HorizontalAlignment(x::HorizontalAlignment::Left);
        search.PlaceholderText(L("iconLibrary.search"));
        x::Automation::AutomationProperties::SetName(search, L("iconLibrary.search"));
        x::Automation::AutomationProperties::SetAutomationId(search, L"IconLibrarySearch");
        c::Grid filters; filters.ColumnSpacing(8);
        c::ColumnDefinition groupColumn; groupColumn.Width({picker ? 156. : 200., x::GridUnitType::Pixel});
        c::ColumnDefinition searchColumn; searchColumn.Width({1., x::GridUnitType::Star});
        filters.ColumnDefinitions().Append(groupColumn); filters.ColumnDefinitions().Append(searchColumn);
        groups.HorizontalAlignment(x::HorizontalAlignment::Stretch);
        filters.Children().Append(groups); c::Grid::SetColumn(search,1); filters.Children().Append(search);
        root.Children().Append(filters); root.Children().Append(toolbar);
        const auto add = [&](c::AppBarButton button, const char* key, auto click) {
            button.Label(L(key)); toolbar.PrimaryCommands().Append(button); button.Click(click);
        };
        const auto weak = weak_from_this();

        groups.MinWidth(120);
        x::Automation::AutomationProperties::SetName(groups, L("iconLibrary.groups"));
        x::Automation::AutomationProperties::SetAutomationId(groups, L"IconLibraryGroups");
        root.SizeChanged([weak](auto&&, const x::SizeChangedEventArgs& args) {
            if (const auto self=weak.lock()) self->toolbar.MaxWidth(std::max(0.,double(args.NewSize().Width)));
        });
        groups.SelectionChanged([weak](auto&&, auto&&) { if (auto self = weak.lock(); self && !self->updatingGroups)
            { const auto selected = self->groups.SelectedItem().try_as<c::ComboBoxItem>();
              self->groupId = selected ? winrt::to_string(winrt::unbox_value<winrt::hstring>(selected.Tag())) : std::string{}; self->signature.clear(); self->Render(); } });
        add(import, "iconLibrary.import", [weak](auto&&, auto&&) { if (auto self = weak.lock()) self->Import(PickFiles(false)); });
        add(selectAll,"iconLibrary.selectFiltered",[weak](auto&&,auto&&){if(const auto self=weak.lock())self->grid.SelectAll();});
        x::Automation::AutomationProperties::SetAutomationId(selectAll,L"IconLibrarySelectFiltered");
        add(retry, "app.operation.retry", [weak](auto&&, auto&&) { if (auto self = weak.lock()) { IconLibraryRequest request; request.action = "refresh"; self->Send(request); } });
        add(rename, "iconLibrary.rename", [weak](auto&&, auto&&) { if (auto self = weak.lock()) self->Rename(); });
        add(remove, "iconLibrary.delete", [weak](auto&&, auto&&) { if (auto self = weak.lock()) self->Remove(); });
        add(png, "iconLibrary.exportPng", [weak](auto&&, auto&&) { if (auto self = weak.lock()) self->Export("png"); });
        add(ico, "iconLibrary.exportIco", [weak](auto&&, auto&&) { if (auto self = weak.lock()) self->Export("ico"); });
        add(moveGroup, "iconLibrary.moveGroup", [weak](auto&&, auto&&) { if (auto self = weak.lock()) self->MoveGroup(); });
        add(newGroup, "iconLibrary.newGroup", [weak](auto&&, auto&&) { if (auto self = weak.lock()) self->EditGroup(true); });
        for (auto [button,key] : {std::pair{renameGroup,"iconLibrary.renameGroup"},{deleteGroup,"iconLibrary.deleteGroup"}})
        { button.Label(L(key)); toolbar.SecondaryCommands().Append(button); }
        renameGroup.Click([weak](auto&&, auto&&) { if (auto self=weak.lock()) self->EditGroup(false); });
        deleteGroup.Click([weak](auto&&, auto&&) { if (auto self=weak.lock()) self->DeleteGroup(); });
        if (picker) { selectAll.Visibility(x::Visibility::Collapsed); newGroup.Visibility(x::Visibility::Collapsed); renameGroup.Visibility(x::Visibility::Collapsed); deleteGroup.Visibility(x::Visibility::Collapsed); moveGroup.Visibility(x::Visibility::Collapsed); rename.Visibility(x::Visibility::Collapsed); remove.Visibility(x::Visibility::Collapsed); png.Visibility(x::Visibility::Collapsed); ico.Visibility(x::Visibility::Collapsed); }
        grid.Height(picker ? 340 : 460); grid.SelectionMode(picker ? c::ListViewSelectionMode::Single : c::ListViewSelectionMode::Multiple);
        x::Automation::AutomationProperties::SetAutomationId(grid, L"IconLibraryGrid");
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
        for (const auto& button : {import, newGroup}) button.Icon(c::SymbolIcon(c::Symbol::Add));
        for (const auto& button : {rename, renameGroup}) button.Icon(c::SymbolIcon(c::Symbol::Edit));
        for (const auto& button : {remove, deleteGroup}) button.Icon(c::SymbolIcon(c::Symbol::Delete));
        for (const auto& button : {png, ico}) button.Icon(c::SymbolIcon(c::Symbol::Save));
        retry.Icon(c::SymbolIcon(c::Symbol::Refresh)); moveGroup.Icon(c::SymbolIcon(c::Symbol::Folder));
        selectAll.Icon(c::SymbolIcon(c::Symbol::SelectAll));
    }
};

IconLibraryPagePresenter::IconLibraryPagePresenter(Localize localize, IconLibraryAction action) : impl_(std::make_shared<Impl>())
{ impl_->localize = std::move(localize); impl_->action = std::move(action); impl_->Build(); }
IconLibraryPagePresenter::~IconLibraryPagePresenter() { Deactivate(); }
c::StackPanel IconLibraryPagePresenter::Content() const { return impl_->root; }
void IconLibraryPagePresenter::Activate(std::wstring target)
{
    if (!impl_->active)
    {
        impl_->active = true; IconLibraryRequest refresh; refresh.action = "refresh"; impl_->Send(refresh); impl_->timer.Start();
    }
    if (target.empty()) return;
    if (impl_->dockPickerStarted) return;
    const auto generation=++impl_->pickerGeneration;
    const auto weak = std::weak_ptr<Impl>(impl_);
    const auto open = [weak, generation, target = std::move(target)] {
        const auto self = weak.lock();
        if (!self || !self->active || self->pickerGeneration!=generation || self->dockPickerStarted || !self->root.XamlRoot()) return;
        self->dockPickerStarted = true;
        IconLibraryRequest read; read.target = target;
        if (!self->Send(read)) { self->dockPickerStarted=false; return; }
        const auto session = self->snapshot.session;
        self->cancelPicker = Pick(self->root.XamlRoot(), self->localize, self->action, {}, [weak, target, session, generation](std::string id) {
            if (auto current = weak.lock(); current && current->active && current->pickerGeneration==generation)
            {
                current->dockPickerStarted = false;
                current->cancelPicker = {};
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
    ++impl_->pickerGeneration;
    if (auto cancel = std::exchange(impl_->cancelPicker, {})) cancel();
    impl_->dockPickerStarted = false;
}
void IconLibraryPagePresenter::RefreshLocalizedText()
{
    impl_->search.PlaceholderText(impl_->L("iconLibrary.search"));
    impl_->import.Label(impl_->L("iconLibrary.import")); impl_->rename.Label(impl_->L("iconLibrary.rename"));
    impl_->remove.Label(impl_->L("iconLibrary.delete")); impl_->png.Label(impl_->L("iconLibrary.exportPng"));
    for (auto [button, key] : {std::pair{impl_->newGroup, "iconLibrary.newGroup"}, {impl_->renameGroup, "iconLibrary.renameGroup"},
        {impl_->deleteGroup, "iconLibrary.deleteGroup"}, {impl_->moveGroup, "iconLibrary.moveGroup"}}) button.Label(impl_->L(key));
    impl_->groupSignature.clear(); impl_->groups.Items().Clear();
    impl_->retry.Label(impl_->L("app.operation.retry"));
    impl_->ico.Label(impl_->L("iconLibrary.exportIco")); impl_->signature.clear(); impl_->Render();
    impl_->selectAll.Label(impl_->L("iconLibrary.selectFiltered"));
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
    view->root.Width(std::max(180., std::min(440., double(availableWidth) - 96)));
    view->grid.Height(std::clamp(double(root.Size().Height) - 360., 120., 340.));
    view->selectionChanged = [weak = std::weak_ptr<IconLibraryPagePresenter::Impl>(view), dialog] {
        if (const auto self = weak.lock())
        {
            const auto ids = self->Selected(); const auto entry = ids.size() == 1 ? self->visible.find(ids[0]) : self->visible.end();
            dialog.IsPrimaryButtonEnabled(!self->snapshot.busy && entry != self->visible.end() && !entry->second.missing);
        }
    };
    view->Send({});
    if (!selected.empty()) for (const auto& item : view->grid.Items())
        if (ItemId(item) == selected) { view->grid.SelectedItem(item); break; }
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

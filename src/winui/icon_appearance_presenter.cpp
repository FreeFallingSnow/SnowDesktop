#include "pch.h"
#include "icon_appearance_presenter.h"
#include "desktop_page_presenter.h"
#include "settings_presenter_controls.h"
#include "icons/icon_image_codec.h"
#include "theme/theme_library_settings.h"
#include <robuffer.h>
#include <cwctype>
#include <utility>

namespace snowdesktop::winui
{
namespace x = winrt::Microsoft::UI::Xaml;
namespace c = x::Controls;
namespace m = x::Media;
namespace
{
std::wstring Fold(std::wstring text)
{
    for (auto& ch : text) ch = static_cast<wchar_t>(std::towlower(ch));
    return text;
}
void SetPreview(const c::Image& image, const icon_image::Image& source, const IconBeautifySettings& style)
{
    if (source.width <= 0 || source.height <= 0 || source.pixels.empty()) { image.Source(nullptr); return; }
    const auto edge = style.mode != 1 ? icon_beautify::DetectPlateFill(source.pixels, source.width, source.height) : std::nullopt;
    const auto pixels = icon_beautify::Render(source.pixels, source.width, source.height, style, edge);
    x::Media::Imaging::WriteableBitmap bitmap(source.width, source.height);
    std::uint8_t* buffer = nullptr;
    winrt::check_hresult(bitmap.PixelBuffer().as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&buffer));
    memcpy(buffer, pixels.data(), pixels.size() * sizeof(std::uint32_t));
    bitmap.Invalidate(); image.Source(bitmap);
}
}

struct IconAppearancePresenter::Impl : std::enable_shared_from_this<Impl>
{
    Localize localize; IconAppearanceAction action;
    c::StackPanel root, commands;
    c::TextBlock title, empty;
    c::TextBox query;
    c::ListView list;
    c::Button edit, inherit;
    c::InfoBar feedback;
    IconAppearanceSnapshot snapshot;
    c::ContentDialog dialog{nullptr};
    std::function<void()> cancelEditor;
    std::wstring pendingTarget, editingTarget;
    bool active = false, closed = false;

    Impl(Localize l, IconAppearanceAction a) : localize(std::move(l)), action(std::move(a))
    {
        root.Spacing(8); root.Margin({0,12,0,0});
        title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold()); title.TextWrapping(x::TextWrapping::Wrap);
        x::Automation::AutomationProperties::SetHeadingLevel(title, x::Automation::Peers::AutomationHeadingLevel::Level2);
        root.Children().Append(title);
        query.Name(L"IconAppearanceSearch"); x::Automation::AutomationProperties::SetAutomationId(query, L"IconAppearanceSearch"); query.HorizontalAlignment(x::HorizontalAlignment::Stretch);
        x::Automation::AutomationProperties::SetName(query, L("iconAppearance.search"));
        commands.Orientation(c::Orientation::Horizontal); commands.Spacing(8);
        edit.Name(L"IconAppearanceEdit"); x::Automation::AutomationProperties::SetAutomationId(edit, L"IconAppearanceEdit"); inherit.Name(L"IconAppearanceInherit"); x::Automation::AutomationProperties::SetAutomationId(inherit, L"IconAppearanceInherit");
        commands.Children().Append(edit); commands.Children().Append(inherit);
        c::Grid toolbar; toolbar.ColumnSpacing(8);
        c::ColumnDefinition searchColumn, actionsColumn; actionsColumn.Width(x::GridLengthHelper::Auto());
        toolbar.ColumnDefinitions().Append(searchColumn); toolbar.ColumnDefinitions().Append(actionsColumn);
        query.VerticalAlignment(x::VerticalAlignment::Center); commands.VerticalAlignment(x::VerticalAlignment::Center);
        c::Grid::SetColumn(commands, 1); toolbar.Children().Append(query); toolbar.Children().Append(commands); root.Children().Append(toolbar);
        list.Name(L"IconAppearanceList"); x::Automation::AutomationProperties::SetAutomationId(list, L"IconAppearanceList"); list.MaxHeight(420); list.SelectionMode(c::ListViewSelectionMode::Single);
        list.IsItemClickEnabled(true); list.HorizontalContentAlignment(x::HorizontalAlignment::Stretch);
        list.ItemTemplate(x::Markup::XamlReader::Load(LR"(<DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"><Grid Padding="8" ColumnSpacing="12"><Grid.ColumnDefinitions><ColumnDefinition Width="48"/><ColumnDefinition Width="*"/></Grid.ColumnDefinitions><Image x:Name="Preview" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" Width="48" Height="48" Stretch="Uniform"/><StackPanel Grid.Column="1" Spacing="4"><TextBlock x:Name="Name" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" TextTrimming="CharacterEllipsis"/><TextBlock x:Name="Detail" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" FontSize="12" TextWrapping="Wrap" Opacity="0.7"/></StackPanel></Grid></DataTemplate>)").as<x::DataTemplate>());
        c::Border surface; presenter_controls::StyleSettingsCard(surface); surface.Padding({0,0,0,0});
        c::StackPanel content; content.Children().Append(list); empty.Margin({16,16,16,16}); content.Children().Append(empty); surface.Child(content);
        root.Children().Append(surface);
        feedback.IsOpen(false); presenter_controls::BindInfoBarVisibility(feedback); root.Children().Append(feedback);
        LocalizeText();
    }
    std::wstring L(std::string_view key) const { return localize ? localize(key) : std::wstring{}; }
    void Hook()
    {
        const auto weak = weak_from_this();
        root.Loaded([weak](const auto&, const auto&) { if (const auto self = weak.lock()) self->OpenPending(); });
        query.TextChanged([weak](const auto&, const auto&) { if (const auto self = weak.lock()) self->Filter(); });
        list.SelectionChanged([weak](const auto&, const auto&) { if (const auto self = weak.lock()) self->Buttons(); });
        edit.Click([weak](const auto&, const auto&) { if (const auto self = weak.lock()) self->RequestEdit(self->Selected()); });
        inherit.Click([weak](const auto&, const auto&) { if (const auto self = weak.lock()) self->Inherit(self->Selected()); });
        list.ContainerContentChanging([weak](const auto&, const c::ContainerContentChangingEventArgs& args) {
            const auto self = weak.lock(); if (!self || self->closed || args.InRecycleQueue()) return;
            const auto target = std::wstring(winrt::unbox_value<winrt::hstring>(args.Item()));
            const auto found = std::find_if(self->snapshot.entries.begin(), self->snapshot.entries.end(), [&](const auto& entry) { return entry.target == target; });
            if (found == self->snapshot.entries.end()) return;
            const auto content = args.ItemContainer().ContentTemplateRoot().as<c::Grid>();
            content.FindName(L"Name").as<c::TextBlock>().Text(found->name);
            std::wstring themeName = self->L(found->settings.enabled ? "app.settings.custom" : "app.settings.beautify_preset_none");
            if (found->themeId.empty())
            {
                const auto preset = icon_beautify::IdentifyPreset(found->settings);
                const char* key = preset == IconBeautifyPreset::None ? "app.settings.beautify_preset_none" :
                    preset == IconBeautifyPreset::DefaultBeautify ? "app.settings.beautify_preset_default" :
                    preset == IconBeautifyPreset::FrostedGlass ? "app.settings.transparent_glass" :
                    preset == IconBeautifyPreset::FrostedGlassDark ? "app.settings.dark_glass" :
                    preset == IconBeautifyPreset::FrostedGlassLight ? "app.settings.light_glass" : "app.settings.custom";
                themeName = self->L(key);
            }
            else for (const auto& theme : self->snapshot.themes) if (theme.id == found->themeId) { themeName = theme.name; break; }
            const auto location = found->target.starts_with(L"control:") || found->target.starts_with(L"widget:") ? self->L("settings.nav.dock") :
                found->target.starts_with(L"recycle-") ? self->L("settings.contextMenu.desktop") : found->target.substr(found->target.find(L':') + 1);
            content.FindName(L"Detail").as<c::TextBlock>().Text(themeName +
                (found->settings.enabled ? L" · " + self->L(found->settings.mode == 2 ? "iconStyle.noUpscale" : found->settings.mode == 1 ? "iconStyle.shrinkAll" : "iconStyle.smart") : L"") +
                (found->available ? L"" : L" · " + self->L("iconAppearance.unavailable")) + L"\n" + location);
            const auto style = found->settings;
            auto image = content.FindName(L"Preview").as<c::Image>(); image.Source(nullptr);
            if (self->active && self->action)
            {
                IconAppearanceRequest request; request.action = "preview"; request.target = target;
                const auto preview = self->action(request);
                icon_image::Image source; std::string error;
                if (preview.succeeded && icon_image::DecodeBytes(preview.previewPng, 96, source, error)) SetPreview(image, source, style);
            }
            args.Handled(true);
        });
    }
    std::wstring Selected() const
    { return list.SelectedItem() ? std::wstring(winrt::unbox_value<winrt::hstring>(list.SelectedItem())) : std::wstring{}; }
    void Buttons()
    {
        const auto target = Selected();
        const auto entry = std::find_if(snapshot.entries.begin(), snapshot.entries.end(), [&](const auto& value) { return value.target == target; });
        edit.IsEnabled(entry != snapshot.entries.end() && entry->available); inherit.IsEnabled(!target.empty());
    }
    void LocalizeText()
    {
        title.Text(L("iconAppearance.list")); query.PlaceholderText(L("iconAppearance.search"));
        edit.Content(winrt::box_value(L("iconAppearance.editEntry"))); inherit.Content(winrt::box_value(L("iconAppearance.inherit")));
        empty.Text(L("iconAppearance.empty"));
        x::Automation::AutomationProperties::SetName(list, title.Text()); Buttons();
    }
    void Error(std::string_view key) { feedback.Message(L(key)); feedback.IsOpen(!key.empty()); }
    void Refresh()
    {
        if (!active || !action || closed) return;
        const auto result = action({});
        if (!result.succeeded) { Error(result.error); return; }
        snapshot = result; Filter();
    }
    void Filter()
    {
        const auto selected = Selected(); list.Items().Clear(); const auto filter = Fold(std::wstring(query.Text()));
        for (const auto& record : snapshot.entries)
            if (filter.empty() || Fold(record.name + L" " + record.target).find(filter) != std::wstring::npos)
                list.Items().Append(winrt::box_value(record.target));
        for (unsigned index = 0; index < list.Items().Size(); ++index)
            if (winrt::unbox_value<winrt::hstring>(list.Items().GetAt(index)) == selected) { list.SelectedIndex(static_cast<int>(index)); break; }
        empty.Visibility(list.Items().Size() ? x::Visibility::Collapsed : x::Visibility::Visible); Buttons();
    }
    void Inherit(const std::wstring& target)
    {
        if (target.empty() || !active || !action) return;
        IconAppearanceRequest request; request.action = "inherit"; request.target = target;
        const auto result = action(request); Error(result.error); if (result.succeeded) Refresh();
    }
    void RequestEdit(std::wstring target)
    {
        if (target.empty()) return;
        if (dialog && editingTarget == target) return;
        pendingTarget = std::move(target);
        if (dialog) { dialog.Hide(); return; }
        const auto weak = weak_from_this();
        root.DispatcherQueue().TryEnqueue([weak] { if (const auto self = weak.lock()) self->OpenPending(); });
    }
    void OpenPending()
    {
        if (!active || closed || dialog || pendingTarget.empty() || !root.XamlRoot()) return;
        const auto target = std::exchange(pendingTarget, {}); Edit(target);
    }
    void Edit(const std::wstring& target)
    {
        if (target.empty() || !active || !action || closed || dialog || !root.XamlRoot()) return;
        editingTarget = target;
        IconAppearanceRequest begin; begin.action = "begin"; begin.target = target;
        const auto captured = action(begin); if (!captured.succeeded) { Error(captured.error); return; }
        struct Draft
        {
            SettingsSnapshot settings;
            themes::Library library;
            std::string themeId;
            icon_image::Image source;
            std::unique_ptr<DesktopPagePresenter> editor;
            c::Image preview;
            c::ComboBox sourceMode;
            c::InfoBar error;
            bool ending = false;
        };
        const auto draft = std::make_shared<Draft>();
        draft->settings.initialized = draft->settings.sessionActive = true;
        draft->settings.generation = draft->settings.revision = 1; draft->settings.domainRevisions.desktop = 1;
        draft->settings.values.desktop.iconBeautify = captured.current.settings;
        draft->themeId = captured.current.themeId;
        for (const auto& choice : captured.themes)
        {
            themes::Theme theme; theme.id = choice.id; theme.name = winrt::to_string(choice.name); theme.kind = themes::Kind::Icon; theme.icon = choice.settings;
            draft->library.themes.emplace(choice.id, std::move(theme));
        }
        std::string error;
        if (!draft->themeId.empty()) (void)themes::Select(draft->library, "icon", draft->themeId, themes::Kind::Icon, themes::Components, error);
        (void)icon_image::DecodeBytes(captured.previewPng, 192, draft->source, error);
        draft->editor = std::make_unique<DesktopPagePresenter>(localize, x::Style{nullptr}); draft->editor->SetIconEditorMode();
        const auto sync = [draft] {
            if (draft->ending) return;
            ++draft->settings.revision; ++draft->settings.domainRevisions.desktop;
            draft->editor->ApplySnapshot(draft->settings);
            SetPreview(draft->preview, draft->source, draft->settings.values.desktop.iconBeautify);
        };
        DesktopPageActions actions;
        actions.updateDesktop = [draft, sync](auto, auto, auto update) {
            const auto before = draft->settings.values.desktop.iconBeautify;
            update(draft->settings.values.desktop);
            if (!themes::SameIconTheme(before, draft->settings.values.desktop.iconBeautify))
            { draft->themeId.clear(); themes::Detach(draft->library, "icon"); }
            sync();
        };
        actions.themeLibrary = [draft, sync](auto, const ThemeLibraryRequest& request) {
            ThemeLibraryResult result; result.succeeded = true;
            if (request.command == ThemeLibraryCommand::Apply)
            {
                std::string error;
                result.succeeded = themes::ApplyTarget(draft->library, "icon", request.id, draft->settings.values, error) &&
                    themes::Select(draft->library, "icon", request.id, themes::Kind::Icon, themes::Components, error);
                if (result.succeeded) { draft->themeId = request.id; sync(); }
            }
            else if (request.command == ThemeLibraryCommand::Detach) { draft->themeId.clear(); themes::Detach(draft->library, "icon"); }
            else if (request.command != ThemeLibraryCommand::Refresh) result.succeeded = false;
            result.library = draft->library; return result;
        };
        draft->editor->SetActions(std::move(actions)); draft->editor->ApplySnapshot(draft->settings); draft->editor->Activate();
        c::StackPanel body; body.Spacing(8);
        body.MinWidth(std::clamp(root.XamlRoot().Size().Width - 96., 280., 480.)); body.MaxWidth(640);
        draft->preview.Name(L"IconAppearancePreview"); x::Automation::AutomationProperties::SetAutomationId(draft->preview, L"IconAppearancePreview");
        draft->preview.Width(128); draft->preview.Height(128); draft->preview.Stretch(m::Stretch::Uniform);
        draft->preview.HorizontalAlignment(x::HorizontalAlignment::Center); body.Children().Append(draft->preview);
        SetPreview(draft->preview, draft->source, captured.current.settings);
        draft->sourceMode.Name(L"IconAppearanceSource"); x::Automation::AutomationProperties::SetAutomationId(draft->sourceMode, L"IconAppearanceSource");
        presenter_controls::SettingRow sourceRow; sourceRow.Initialize(draft->sourceMode, 220);
        sourceRow.SetText(L("iconAppearance.source")); presenter_controls::StyleSettingsCard(sourceRow.root);
        x::Automation::AutomationProperties::SetName(draft->sourceMode, L("iconAppearance.source"));
        draft->sourceMode.HorizontalAlignment(x::HorizontalAlignment::Stretch);
        draft->sourceMode.Items().Append(winrt::box_value(L("iconAppearance.follow")));
        draft->sourceMode.Items().Append(winrt::box_value(L("iconAppearance.independent")));
        draft->sourceMode.SelectedIndex(1); body.Children().Append(sourceRow.root);
        const auto editorContent = draft->editor->IconBeautificationContent(); body.Children().Append(editorContent);
        editorContent.Visibility(x::Visibility::Visible);
        draft->sourceMode.SelectionChanged([weakDraft = std::weak_ptr<Draft>(draft), editorContent](const auto&, const auto&) {
            if (const auto value = weakDraft.lock()) editorContent.Visibility(value->sourceMode.SelectedIndex() == 1 ? x::Visibility::Visible : x::Visibility::Collapsed);
        });
        draft->error.IsOpen(false); presenter_controls::BindInfoBarVisibility(draft->error); body.Children().Append(draft->error);
        c::ScrollViewer scroll; scroll.MaxHeight(540); scroll.HorizontalScrollBarVisibility(c::ScrollBarVisibility::Disabled); scroll.Content(body);
        dialog = c::ContentDialog{}; dialog.XamlRoot(root.XamlRoot()); dialog.RequestedTheme(root.ActualTheme()); dialog.Title(winrt::box_value(captured.current.name));
        dialog.Content(scroll); dialog.PrimaryButtonText(L("app.settings.apply")); dialog.CloseButtonText(L("app.settings.cancel"));
        dialog.DefaultButton(c::ContentDialogButton::Primary);
        x::Automation::AutomationProperties::SetAutomationId(dialog, L"IconAppearanceEditor");
        const auto weak = weak_from_this();
        dialog.PrimaryButtonClick([weak, draft, captured, target](const auto&, const c::ContentDialogButtonClickEventArgs& args) {
            const auto self = weak.lock(); if (!self || !self->active || self->closed) { args.Cancel(true); return; }
            draft->editor->Deactivate();
            IconAppearanceRequest request; request.action = draft->sourceMode.SelectedIndex() == 1 ? "apply" : "inherit";
            request.target = target; request.session = captured.session; request.revision = captured.revision;
            request.settings = draft->settings.values.desktop.iconBeautify; request.themeId = draft->themeId;
            const auto result = self->action(request);
            if (!result.succeeded) { args.Cancel(true); draft->error.Message(self->L(result.error)); draft->error.IsOpen(true); draft->editor->Activate(); }
        });
        dialog.Closed([weak, draft, captured, target](const auto&, const auto&) {
            const auto self = weak.lock();
            draft->ending = true; draft->editor->Close();
            // Break editor/event closures before releasing the dialog.
            draft->editor.reset();
            if (!self) return;
            IconAppearanceRequest cancel; cancel.action = "cancel"; cancel.target = target; cancel.session = captured.session;
            if (self->action) (void)self->action(cancel);
            self->dialog = nullptr; self->editingTarget.clear(); self->cancelEditor = {}; self->Refresh();
            const auto weakAgain = self->weak_from_this();
            self->root.DispatcherQueue().TryEnqueue([weakAgain] { if (const auto current = weakAgain.lock()) current->OpenPending(); });
        });
        const auto shown = dialog; cancelEditor = [shown] { shown.Hide(); };
        (void)dialog.ShowAsync();
    }
};

IconAppearancePresenter::IconAppearancePresenter(Localize localize, IconAppearanceAction action)
    : impl_(std::make_shared<Impl>(std::move(localize), std::move(action))) { impl_->Hook(); }
IconAppearancePresenter::~IconAppearancePresenter() { Deactivate(); impl_->closed = true; }
c::StackPanel IconAppearancePresenter::Content() const { return impl_->root; }
void IconAppearancePresenter::Activate(std::wstring target)
{ impl_->active = true; impl_->Refresh(); if (!target.empty()) impl_->RequestEdit(std::move(target)); }
void IconAppearancePresenter::Deactivate()
{ impl_->active = false; impl_->pendingTarget.clear(); if (impl_->cancelEditor) { auto cancel = impl_->cancelEditor; cancel(); } }
void IconAppearancePresenter::RefreshLocalizedText() { impl_->LocalizeText(); impl_->Filter(); }
}

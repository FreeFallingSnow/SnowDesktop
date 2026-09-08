#include "pch.h"
#include "large_icon_page_presenter.h"
#include "settings_presenter_controls.h"
#include "panel_gradient_editor.h"
#include "../large_icon_settings_rules.h"
#include "../large_icon_preset_rules.h"
#include "../large_icon_title_measure.h"
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.System.h>

namespace snowdesktop::winui
{
namespace x = winrt::Microsoft::UI::Xaml;
namespace c = x::Controls;
namespace m = x::Media;
using Field = large_icon_settings_rules::Field;

struct LargeIconPagePresenter::Impl : std::enable_shared_from_this<Impl>
{
    std::function<std::wstring(std::string_view)> localize;
    x::Style style{nullptr};
    LargeIconSettingsAction action;
    std::function<void()> nameChanged;
    LargeIconSettingsSnapshot snapshot;
    LargeIconConfig draft;
    c::StackPanel root, editors;
    c::ContentControl editorHost;
    c::TextBlock status;
    x::DispatcherTimer timer;
    presenter_controls::CoalescedPreviewTimer<LargeIconConfig> previews;
    std::shared_ptr<PanelGradientEditor> gradient;
    std::vector<std::function<void()>> synchronize, visibility;
    std::array<c::Image, 2> coverImages;
    std::array<c::TextBlock, 2> coverLabels;
    std::array<std::wstring, 2> coverPaths;
    bool active = false, sending = false, syncing = false, dirty = false;
    Microsoft::WRL::ComPtr<IDWriteFactory> titleFonts;

    std::wstring L(std::string_view key) const { return localize ? localize(key) : std::wstring{}; }
    static winrt::Windows::UI::Color Color(unsigned rgb)
    { return {255, static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb)}; }
    LargeIconConfig Defaults() const
    {
        LargeIconConfig result; JsonValue json;
        if (ParseJson(snapshot.defaultConfig, json)) DecodeLargeIconConfig(json, result);
        result.effect = large_icon_preset_rules::DefaultEffect(draft);
        return result;
    }
    bool Supported(int direction)
    {
        if (!titleFonts) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(titleFonts.GetAddressOf()));
        return large_icon_render_rules::CanSelectTitleDirection(draft, direction, snapshot.frameWidth, snapshot.frameHeight,
            snapshot.imageWidth, snapshot.imageHeight, snapshot.unitScale,
            large_icon_render_rules::MeasureTitleText(titleFonts.Get(), draft, snapshot.name, snapshot.unitScale));
    }
    void Reload()
    {
        JsonValue json;
        if (ParseJson(snapshot.config, json)) DecodeLargeIconConfig(json, draft);
        large_icon_preset_rules::PrepareForEditing(draft, snapshot.accent, snapshot.hasEdgeColor, snapshot.edgeColor);
        if (gradient) gradient->SetValue(draft.gradient, true);
    }
    void Sync()
    {
        syncing = true;
        status.Text(L(snapshot.error));
        status.Visibility(snapshot.error.empty() ? x::Visibility::Collapsed : x::Visibility::Visible);
        editorHost.IsEnabled(snapshot.available && snapshot.editable && snapshot.error != "largeIcon.stale");
        for (auto& fn : synchronize) fn();
        for (auto& fn : visibility) fn();
        if (gradient) gradient->SetValue(draft.gradient);
        for (int i = 0; i < 2; ++i)
        {
            const auto& path = i == 0 ? snapshot.landscapePath : snapshot.portraitPath;
            const auto& source = i == 0 ? snapshot.landscapeSource : snapshot.portraitSource;
            if (coverPaths[i] != path)
            {
                coverPaths[i] = path;
                if (path.empty()) coverImages[i].Source(nullptr);
                else coverImages[i].Source(m::Imaging::BitmapImage(winrt::Windows::Foundation::Uri(path)));
            }
            coverLabels[i].Text(source.empty() ? L(snapshot.loading ? "largeIcon.loading" : "largeIcon.unavailable") :
                L(source == "original" ? "largeIcon.source.original" : source == "local" ? "largeIcon.source.local" :
                    source == "steam-local" ? "largeIcon.source.steam-local" : source == "steam-online" ? "largeIcon.source.steam-online" : "largeIcon.source.cache"));
        }
        syncing = false;
    }
    void Preview()
    {
        if (!active || !snapshot.available || !snapshot.editable) return;
        dirty = true; Sync(); previews.Queue(draft);
    }
    bool Send(std::string operation)
    {
        if (!action || sending) return false;
        if (operation != "preview" && operation != "status") previews.Cancel();
        sending = true;
        const auto oldRevision = snapshot.revision;
        const auto oldError = snapshot.error;
        const auto oldSteam = snapshot.steam;
        const auto oldName = snapshot.name;
        const bool wasDirty = dirty;
        try { snapshot = action({snapshot.key, snapshot.session, snapshot.revision, operation, EncodeLargeIconConfig(draft), {}}); }
        catch (...) { snapshot.succeeded = false; snapshot.error = "largeIcon.unavailable"; }
        sending = false;
        if (snapshot.succeeded && operation == "preview") dirty = true;
        if (snapshot.succeeded && (operation == "commit" || operation == "cancel" || operation == "read")) { dirty = false; Reload(); }
        if (!snapshot.available || !snapshot.editable || snapshot.error == "largeIcon.stale") { previews.Cancel(); dirty = false; Reload(); }
        if (operation == "status" && snapshot.succeeded)
        {
            if (snapshot.revision != oldRevision) { dirty = false; Reload(); }
            else if (wasDirty && snapshot.editable && snapshot.available) dirty = true;
            if (snapshot.error.empty() && (oldError == "largeIcon.saveFailed" || oldError == "largeIcon.invalid"))
                snapshot.error = oldError;
        }
        if (oldSteam != snapshot.steam) Build();
        else Sync();
        if (snapshot.name != oldName && nameChanged) nameChanged();
        return snapshot.succeeded;
    }
    void Cancel()
    {
        if (dirty) { Send("cancel"); Reload(); Sync(); }
    }
    void Track(const x::FrameworkElement& element, Field field)
    {
        visibility.push_back([this, element, field] {
            element.Visibility(large_icon_settings_rules::Visible(field, draft, snapshot.hasEdgeColor) ? x::Visibility::Visible : x::Visibility::Collapsed);
        });
    }
    c::StackPanel Group(const char* key, Field field = Field::Always)
    {
        c::StackPanel group; group.Spacing(8); group.HorizontalAlignment(x::HorizontalAlignment::Stretch);
        c::TextBlock heading; heading.Text(L(key)); heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        c::Border card; card.Style(style); card.HorizontalAlignment(x::HorizontalAlignment::Stretch);
        c::StackPanel content; content.Spacing(4); content.HorizontalAlignment(x::HorizontalAlignment::Stretch);
        card.Child(content); group.Children().Append(heading); group.Children().Append(card);
        editors.Children().Append(group); Track(group, field); return content;
    }
    void Row(c::StackPanel panel, const char* key, const x::UIElement& control,
        std::function<void(Impl&)> reset, Field field = Field::Always, int level = 0)
    {
        c::Grid editor; editor.ColumnSpacing(8);
        c::ColumnDefinition value; value.Width(x::GridLengthHelper::FromValueAndType(1, x::GridUnitType::Star));
        c::ColumnDefinition button; button.Width(x::GridLengthHelper::Auto());
        editor.ColumnDefinitions().Append(value); editor.ColumnDefinitions().Append(button); editor.Children().Append(control);
        c::Button restore;
        presenter_controls::ConfigureRestoreDefaultButton(restore, L("app.settings.restore_default") + L" · " + L(key));
        c::Grid::SetColumn(restore, 1); editor.Children().Append(restore);
        std::weak_ptr<Impl> weak = shared_from_this();
        restore.Click([weak, reset](auto const&, auto const&) { if (auto self = weak.lock()) reset(*self); });
        presenter_controls::SettingRow row; row.Initialize(editor, presenter_controls::kSettingControlWidth + 40);
        row.SetText(L(key)); row.root.MinHeight(44); row.root.Margin({16. * level, 0, 0, 0});
        x::Automation::AutomationProperties::SetName(control, L(key));
        panel.Children().Append(row.root); Track(row.root, field);
        const std::string id = key;
        visibility.push_back([this, row, field, id]() mutable {
            bool enabled = large_icon_settings_rules::Enabled(field, draft, snapshot.hasEdgeColor, snapshot.accent != 0);
            std::wstring help;
            if (field == Field::ForegroundPosition && !enabled) help = L("largeIcon.positionEffectHelp");

            if (id == "largeIcon.radius") help = L("largeIcon.radiusHelp");
            if (id == "largeIcon.direction" && !Supported(draft.autoTitleDirection ? 2 : draft.titleDirection)) help = L("largeIcon.titleUnavailable");
            if (id == "largeIcon.autoTitleColor" && (draft.backgroundStyle >= -1 || (draft.backgroundStyle == -3 && draft.defaultBackground != 0))) help = L("largeIcon.themeTextHelp");
            if (field == Field::Crop)
            {
                const auto geometry = large_icon_render_rules::ResolveContent(draft, snapshot.frameWidth, snapshot.frameHeight,
                    snapshot.imageWidth, snapshot.imageHeight, snapshot.unitScale, false, 0);
                enabled = id == "largeIcon.positionX" ? snapshot.imageWidth > geometry.sourceWidth + .01 : snapshot.imageHeight > geometry.sourceHeight + .01;
                if (!enabled) help = L("largeIcon.noCropRoom");
            }
            row.SetEnabled(enabled); row.help.Text(help);
            row.help.Visibility(help.empty() ? x::Visibility::Collapsed : x::Visibility::Visible);
        });
    }
    template<class T> std::function<void(Impl&)> Reset(T LargeIconConfig::* member)
    {
        return [member](Impl& self) {
            const auto defaults = self.Defaults();
            if constexpr (std::is_same_v<T, int>)
                if (member == &LargeIconConfig::backgroundStyle)
                {
                    if (large_icon_preset_rules::ApplyBackground(self.draft, defaults.backgroundStyle, self.snapshot.editable,
                            self.snapshot.hasEdgeColor, self.snapshot.accent, self.snapshot.edgeColor)) self.Send("commit");
                    return;
                }
            self.draft.*member = defaults.*member;
            if constexpr (std::is_same_v<T, bool>)
            {
                if (member == &LargeIconConfig::themeColor && self.draft.themeColor) self.draft.themeGradient = false;
                if (member == &LargeIconConfig::themeGradient && self.draft.themeGradient) self.draft.themeColor = false;
            }
            if constexpr (std::is_same_v<T, int>)
                if (member == &LargeIconConfig::titleDirection) self.draft.autoTitleDirection = defaults.autoTitleDirection;
            if constexpr (std::is_same_v<T, double>)
                if (member == &LargeIconConfig::radiusPercent) self.draft.radius = defaults.radius;
            self.Send("commit");
        };
    }
    template<class T> double NumericValue(T LargeIconConfig::* member) const
    {
        if constexpr (std::is_same_v<T, double>)
            if (member == &LargeIconConfig::radiusPercent)
                return large_icon_render_rules::RadiusPercent(draft, snapshot.frameWidth, snapshot.frameHeight, snapshot.unitScale);
        return static_cast<double>(draft.*member);
    }
    template<class T> void Slider(c::StackPanel panel, const char* key, T LargeIconConfig::* member,
        double min, double max, double step = 1, double factor = 1, const wchar_t* unit = L"",
        Field field = Field::Always, int level = 0)
    {
        c::Grid pair; pair.ColumnSpacing(8);
        for (int i = 0; i < 3; ++i)
        {
            c::ColumnDefinition column;
            column.Width(i == 0 ? x::GridLengthHelper::FromValueAndType(1, x::GridUnitType::Star) :
                i == 1 ? x::GridLengthHelper::FromValueAndType(70, x::GridUnitType::Pixel) : x::GridLengthHelper::Auto());
            pair.ColumnDefinitions().Append(column);
        }
        c::Slider slider; slider.Minimum(min); slider.Maximum(max); slider.StepFrequency(step);
        slider.VerticalAlignment(x::VerticalAlignment::Center);
        c::NumberBox number; number.Minimum(min); number.Maximum(max); number.SmallChange(step);
        number.SpinButtonPlacementMode(c::NumberBoxSpinButtonPlacementMode::Hidden);
        c::TextBlock suffix; suffix.Text(unit); suffix.VerticalAlignment(x::VerticalAlignment::Center);
        c::Grid::SetColumn(number, 1); c::Grid::SetColumn(suffix, 2);
        pair.Children().Append(slider); pair.Children().Append(number); pair.Children().Append(suffix);
        x::Automation::AutomationProperties::SetName(slider, L(key)); x::Automation::AutomationProperties::SetName(number, L(key));
        std::weak_ptr<Impl> weak = shared_from_this();
        const auto change = [weak, member, min, max, step, factor](auto const&, auto const& args) {
            if (auto self = weak.lock(); self && !self->syncing && std::isfinite(args.NewValue()))
            {
                const double n = presenter_controls::QuantizeNumericValue(args.NewValue(), min, max, step);
                self->draft.*member = static_cast<T>(n / factor); self->Preview();
            }
        };
        slider.ValueChanged(change); number.ValueChanged(change);
        const auto commit = [weak](auto const&, auto const&) {
            if (auto self = weak.lock(); self && self->active && !self->syncing && self->dirty) self->Send("commit");
        };
        slider.PointerReleased(commit); slider.PointerCaptureLost(commit); slider.KeyUp(commit); slider.LostFocus(commit);
        number.KeyUp(commit); number.LostFocus(commit);
        synchronize.push_back([this, slider, number, member, factor] {
            const auto value = NumericValue(member) * factor; slider.Value(value); number.Value(value);
        });
        Row(panel, key, pair, Reset(member), field, level);
    }
    void Toggle(c::StackPanel panel, const char* key, bool LargeIconConfig::* member, Field field, int level)
    {
        c::ToggleSwitch input; input.MinWidth(0); input.HorizontalAlignment(x::HorizontalAlignment::Right);
        std::weak_ptr<Impl> weak = shared_from_this();
        input.Toggled([weak, member, input](auto const&, auto const&) {
            if (auto self = weak.lock(); self && !self->syncing)
            {
                self->draft.*member = input.IsOn();
                if (member == &LargeIconConfig::themeColor && input.IsOn()) self->draft.themeGradient = false;
                if (member == &LargeIconConfig::themeGradient && input.IsOn()) self->draft.themeColor = false;
                self->Send("commit");
            }
        });
        synchronize.push_back([this, input, member] { input.IsOn(draft.*member); });
        Row(panel, key, input, Reset(member), field, level);
    }
    void Choice(c::StackPanel panel, const char* key, int LargeIconConfig::* member,
        std::initializer_list<std::pair<int, const char*>> options, Field field = Field::Always, int level = 0)
    {
        c::ComboBox input; input.HorizontalAlignment(x::HorizontalAlignment::Right); input.MinWidth(200);
        std::vector<int> values;
        for (const auto& [value, text] : options)
        {
            c::ComboBoxItem item; item.Content(winrt::box_value(L(text))); input.Items().Append(item); values.push_back(value);
            if (member == &LargeIconConfig::titleDirection)
                visibility.push_back([this, item, value] { item.IsEnabled(Supported(value)); });
            if (member == &LargeIconConfig::effect && value == 2)
                visibility.push_back([this, item] { item.IsEnabled(!IsLargeIconFill(draft)); });
            if (member == &LargeIconConfig::backgroundStyle)
                visibility.push_back([this, item, value] {
                    item.Visibility(large_icon_preset_rules::BackgroundVisible(value, snapshot.hasEdgeColor, draft) ? x::Visibility::Visible : x::Visibility::Collapsed);
                });
        }
        std::weak_ptr<Impl> weak = shared_from_this();
        input.SelectionChanged([weak, member, input, values](auto const&, auto const&) {
            if (auto self = weak.lock(); self && !self->syncing)
            {
                const int index = input.SelectedIndex(); if (index < 0 || index >= static_cast<int>(values.size())) return;
                if (member == &LargeIconConfig::titleDirection && !self->Supported(values[index])) return;
                if (member == &LargeIconConfig::effect && values[index] == 2 && IsLargeIconFill(self->draft)) return;
                if (member == &LargeIconConfig::titleDirection)
                {
                    self->draft.autoTitleDirection = values[index] == 2;
                    if (values[index] != 2) self->draft.titleDirection = values[index];
                }
                else if (member == &LargeIconConfig::backgroundStyle)
                {
                    if (!large_icon_preset_rules::ApplyBackground(self->draft, values[index], self->snapshot.editable,
                        self->snapshot.hasEdgeColor, self->snapshot.accent, self->snapshot.edgeColor)) return;
                }
                else if (member == &LargeIconConfig::effect)
                {
                    if (!large_icon_preset_rules::ApplyEffect(self->draft, values[index], self->snapshot.editable)) return;
                }
                else self->draft.*member = values[index];
                self->Send("commit");
            }
        });
        synchronize.push_back([this, input, member, values] {
            const int value = member == &LargeIconConfig::titleDirection && draft.autoTitleDirection ? 2 :
                member == &LargeIconConfig::effect && IsLargeIconFill(draft) && draft.effect == 2 ? 0 : draft.*member;
            const auto found = std::find(values.begin(), values.end(), value);
            input.SelectedIndex(found == values.end() ? -1 : static_cast<int>(found - values.begin()));
        });
        Row(panel, key, input, Reset(member), field, level);
    }
    void ColorPicker(c::StackPanel panel, const char* key, unsigned LargeIconConfig::* member, Field field, int level)
    {
        c::Button button; button.HorizontalAlignment(x::HorizontalAlignment::Right);
        c::StackPanel content; content.Orientation(c::Orientation::Horizontal); content.Spacing(8);
        c::Border swatch; swatch.Width(20); swatch.Height(20); swatch.CornerRadius({4});
        c::TextBlock label; content.Children().Append(swatch); content.Children().Append(label); button.Content(content);
        c::ColorPicker picker; picker.IsAlphaEnabled(false); c::Flyout flyout;
        c::StackPanel body; body.Spacing(8); body.Children().Append(picker); flyout.Content(body); button.Flyout(flyout);
        std::weak_ptr<Impl> weak = shared_from_this();
        flyout.Opening([weak, body, picker](auto const&, auto const&) {
            body.Children().Clear(); body.Children().Append(picker);
            if (auto self = weak.lock()) self->ColorShortcuts(body, picker);
        });
        picker.ColorChanged([weak, member](auto const&, auto const& args) {
            if (auto self = weak.lock(); self && !self->syncing)
            { const auto color = args.NewColor(); self->draft.*member = color.R << 16 | color.G << 8 | color.B; self->Preview(); }
        });
        flyout.Closed([weak](auto const&, auto const&) { if (auto self = weak.lock(); self && self->dirty) self->Send("commit"); });
        picker.KeyDown([weak, flyout](auto const&, auto const& args) {
            if (args.Key() == winrt::Windows::System::VirtualKey::Escape)
                if (auto self = weak.lock()) { self->Cancel(); args.Handled(true); flyout.Hide(); }
        });
        synchronize.push_back([this, picker, swatch, label, member] {
            const auto color = Color(draft.*member); picker.Color(color); swatch.Background(m::SolidColorBrush(color));
            wchar_t text[8]; swprintf_s(text, L"#%06X", draft.*member); label.Text(text);
        });
        Row(panel, key, button, Reset(member), field, level);
    }
    void Action(c::StackPanel panel, const char* key, const char* label, std::function<void(Impl&)> callback, Field field, int level)
    {
        c::Button button; button.Content(winrt::box_value(L(label))); button.HorizontalAlignment(x::HorizontalAlignment::Right);
        std::weak_ptr<Impl> weak = shared_from_this();
        button.Click([weak, callback](auto const&, auto const&) { if (auto self = weak.lock()) callback(*self); });
        presenter_controls::SettingRow row; row.Initialize(button); row.SetText(L(key));
        row.SetControlAlignment(x::HorizontalAlignment::Right); row.root.Margin({level * 16., 0, 0, 0});
        panel.Children().Append(row.root); Track(row.root, field);
    }
    void ColorShortcuts(c::StackPanel panel, c::ColorPicker picker)
    {
        const auto add = [&](const char* key, unsigned value) {
            c::Button button; button.Content(winrt::box_value(L(key)));
            button.HorizontalAlignment(x::HorizontalAlignment::Stretch);
            button.Click([picker, value](auto const&, auto const&) { picker.Color(Color(value)); });
            panel.Children().Append(button);
        };
        if (snapshot.hasEdgeColor) add("largeIcon.useIconBackground", snapshot.edgeColor);
        if (snapshot.accent) add("largeIcon.useIconTheme", snapshot.accent);
    }
    c::StackPanel Section(c::StackPanel parent, const char* key, Field field)
    {
        c::StackPanel section; section.Spacing(4);
        c::TextBlock title; title.Text(L(key)); title.TextWrapping(x::TextWrapping::Wrap);
        title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold()); title.Margin({0,12,0,4});
        section.Children().Append(title);
        c::StackPanel body; body.Spacing(4); section.Children().Append(body);
        parent.Children().Append(section); Track(section, field); return body;
    }
    void FillKind(c::StackPanel panel)
    {
        c::ComboBox input; input.MinWidth(200); input.HorizontalAlignment(x::HorizontalAlignment::Right);
        for (auto key : {"largeIcon.solidBackground", "largeIcon.gradientBackground"})
        { c::ComboBoxItem item; item.Content(winrt::box_value(L(key))); input.Items().Append(item); }
        std::weak_ptr<Impl> weak = shared_from_this();
        input.SelectionChanged([weak, input](auto const&, auto const&) {
            if (auto self = weak.lock(); self && !self->syncing && input.SelectedIndex() >= 0)
            { self->draft.gradient.enabled = input.SelectedIndex() == 1; self->Send("commit"); }
        });
        synchronize.push_back([this, input] { input.SelectedIndex(draft.gradient.enabled ? 1 : 0); });
        Row(panel, "largeIcon.defaultBackground", input, [](auto& self) { self.draft.gradient.enabled = false; self.Send("commit"); }, Field::Custom);
    }
    void Build()
    {
        syncing = true; synchronize.clear(); visibility.clear(); root.Children().Clear(); editors.Children().Clear();
        root.Spacing(16); editors.Spacing(20);
        root.HorizontalAlignment(x::HorizontalAlignment::Stretch); editors.HorizontalAlignment(x::HorizontalAlignment::Stretch);
        editorHost.HorizontalAlignment(x::HorizontalAlignment::Stretch); editorHost.HorizontalContentAlignment(x::HorizontalAlignment::Stretch);
        status.TextWrapping(x::TextWrapping::Wrap);
        root.Children().Append(status); editorHost.Content(editors); root.Children().Append(editorHost);
        auto bg = Group("largeIcon.backgroundSection");
        Choice(bg, "largeIcon.backgroundStyle", &LargeIconConfig::backgroundStyle,
            {{-5,"largeIcon.default"},{-4,"largeIcon.platePreset"},{-2,"largeIcon.fill"},{-1,"largeIcon.follow"},
             {0,"app.settings.dark"},{1,"app.settings.light"},{6,"app.settings.dark_glass"},{7,"app.settings.light_glass"},
             {10,"app.settings.dark_acrylic"},{11,"app.settings.light_acrylic"},{9,"app.settings.custom"}});
        if (snapshot.steam) Choice(bg, "largeIcon.fillSource", &LargeIconConfig::content,
            {{0,"largeIcon.original"},{1,"largeIcon.image"},{2,"largeIcon.steam"}}, Field::Fill, 1);
        else Choice(bg, "largeIcon.fillSource", &LargeIconConfig::content, {{0,"largeIcon.original"},{1,"largeIcon.image"}}, Field::Fill, 1);
        Action(bg, "largeIcon.image", "largeIcon.import", [](auto& self) { self.Send("import"); }, Field::FillImage, 2);
        Slider(bg, "largeIcon.fillScale", &LargeIconConfig::fillScale, 25, 300, 1, 100, L"%", Field::Fill, 1);
        auto imageDetails = Section(bg, "largeIcon.imageDetails", Field::Fill);
        Choice(imageDetails, "largeIcon.fit", &LargeIconConfig::fit, {{1,"largeIcon.cover"},{0,"largeIcon.contain"}}, Field::Fill);
        Slider(imageDetails, "largeIcon.positionX", &LargeIconConfig::focusX, 0, 100, 1, 100, L"%", Field::Crop, 1);
        Slider(imageDetails, "largeIcon.positionY", &LargeIconConfig::focusY, 0, 100, 1, 100, L"%", Field::Crop, 1);
        Choice(imageDetails, "largeIcon.orientation", &LargeIconConfig::steamOrientation,
            {{0,"largeIcon.auto"},{1,"largeIcon.landscape"},{2,"largeIcon.portrait"}}, Field::Steam, 2);
        c::StackPanel gallery; gallery.Orientation(c::Orientation::Horizontal); gallery.Spacing(12); gallery.HorizontalAlignment(x::HorizontalAlignment::Right);
        for (int i = 0; i < 2; ++i)
        {
            coverImages[i] = c::Image{}; coverLabels[i] = c::TextBlock{}; coverPaths[i].clear();
            c::StackPanel asset; asset.Spacing(4); coverImages[i].Width(120); coverImages[i].Height(80); coverImages[i].Stretch(m::Stretch::Uniform);
            coverLabels[i].FontSize(12); coverLabels[i].TextWrapping(x::TextWrapping::Wrap); coverLabels[i].MaxWidth(120);
            c::Button choose; choose.Content(winrt::box_value(L(i == 0 ? "largeIcon.landscape" : "largeIcon.portrait")));
            std::weak_ptr<Impl> weak = shared_from_this();
            choose.Click([weak, i](auto const&, auto const&) { if (auto self = weak.lock()) { self->draft.steamOrientation = i + 1; self->Send("commit"); } });
            visibility.push_back([this, choose, i] { choose.IsEnabled(!(i == 0 ? snapshot.landscapePath : snapshot.portraitPath).empty()); });
            asset.Children().Append(coverImages[i]); asset.Children().Append(coverLabels[i]); asset.Children().Append(choose); gallery.Children().Append(asset);
        }
        imageDetails.Children().Append(gallery); Track(gallery, Field::Steam);
        Toggle(imageDetails, "largeIcon.localOnly", &LargeIconConfig::localOnly, Field::Steam, 1);
        Action(imageDetails, "largeIcon.refresh", "largeIcon.refresh", [](auto& self) { self.Send("refresh"); }, Field::Steam, 1);
        c::TextBlock fillHeading; fillHeading.Text(L("largeIcon.colorAndFill"));
        fillHeading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold()); fillHeading.Margin({0,8,0,4});
        bg.Children().Append(fillHeading); Track(fillHeading, Field::Custom);
        FillKind(bg);
        ColorPicker(bg, "largeIcon.manualColor", &LargeIconConfig::manualColor, Field::Solid, 0);
        Slider(bg, "largeIcon.opacity", &LargeIconConfig::opacity, 0, 100, 1, 100, L"%", Field::Solid);
        std::weak_ptr<Impl> weak = shared_from_this();
        gradient = PanelGradientEditor::Create(localize, [weak](const auto& value, bool commit) {
            if (auto self = weak.lock(); self && !self->syncing) { self->draft.gradient = value; if (commit) self->Send("commit"); else self->Preview(); }
        }, true, [weak](auto panel, auto picker) { if (auto self = weak.lock()) self->ColorShortcuts(panel, picker); }, true);
        gradient->SetValue(draft.gradient); bg.Children().Append(gradient->Content()); Track(gradient->Content(), Field::Gradient);
        Slider(bg, "largeIcon.gradientOpacity", &LargeIconConfig::gradientOpacity, 0, 100, 1, 100, L"%", Field::Gradient);
        auto material = Section(bg, "largeIcon.material", Field::Custom);
        Choice(material, "largeIcon.material", &LargeIconConfig::material,
            {{0,"largeIcon.plain"},{1,"largeIcon.glass"},{2,"largeIcon.acrylic"}}, Field::Custom);
        Slider(material, "app.settings.blur_radius", &LargeIconConfig::blurRadius, 4, 48, 1, 1, L"", Field::Blur, 1);
        auto edges = Section(bg, "largeIcon.borderAndHighlight", Field::Custom);
        Toggle(edges, "largeIcon.border", &LargeIconConfig::border, Field::Custom, 0);
        ColorPicker(edges, "largeIcon.borderColor", &LargeIconConfig::borderColor, Field::Border, 1);
        Slider(edges, "largeIcon.borderOpacity", &LargeIconConfig::borderOpacity, 0, 100, 1, 100, L"%", Field::Border, 1);
        Slider(edges, "largeIcon.borderWidth", &LargeIconConfig::borderWidth, .5, 4, .5, 1, L"", Field::Border, 1);
        Toggle(edges, "largeIcon.edgeHighlight", &LargeIconConfig::edgeHighlight, Field::Custom, 0);
        Slider(edges, "largeIcon.edgeStrength", &LargeIconConfig::edgeStrength, 0, 100, 1, 100, L"%", Field::Edge, 1);
        Slider(edges, "largeIcon.edgeWidth", &LargeIconConfig::edgeWidth, .5, 4, .5, 1, L"", Field::Edge, 1);
        auto text = Section(bg, "appearance.text", Field::Custom);
        Choice(text, "app.settings.text_color", &LargeIconConfig::componentTheme,
            {{0,"app.settings.light"},{1,"app.settings.dark"}}, Field::Custom);
        Toggle(bg, "largeIcon.followComponentRadius", &LargeIconConfig::followComponentRadius, Field::Always, 0);
        Slider(bg, "largeIcon.radius", &LargeIconConfig::radiusPercent, 0, 100, 1, 1, L"%", Field::Radius, 1);
        auto icon = Group("largeIcon.foreground", Field::Foreground);
        Choice(icon, "largeIcon.foregroundSource", &LargeIconConfig::foregroundContent, {{0,"largeIcon.original"},{1,"largeIcon.image"}});
        Action(icon, "largeIcon.image", "largeIcon.import", [](auto& self) { self.Send("import"); }, Field::ForegroundImage, 1);
        Slider(icon, "largeIcon.iconSize", &LargeIconConfig::contentScale, 10, 100, 1, 100, L"%");
        Slider(icon, "largeIcon.positionX", &LargeIconConfig::iconX, 0, 100, 1, 100, L"%", Field::ForegroundPosition);
        Slider(icon, "largeIcon.positionY", &LargeIconConfig::iconY, 0, 100, 1, 100, L"%", Field::ForegroundPosition);
        auto effects = Group("largeIcon.effectsSection");
        Choice(effects, "largeIcon.effect", &LargeIconConfig::effect,
            {{0,"largeIcon.noEffect"},{1,"largeIcon.tilt"},{2,"largeIcon.dynamicTitle"},
             {3,"largeIcon.zoom"},{4,"largeIcon.edgeGlow"},{5,"largeIcon.shine"}});
        Slider(effects, "largeIcon.amplitude", &LargeIconConfig::amplitude, 0, 100, 1, 50, L"%", Field::Tilt, 1);
        Slider(effects, "largeIcon.zoomAmount", &LargeIconConfig::zoomAmount, 0, 10, 1, 100, L"%", Field::Zoom, 1);
        Slider(effects, "largeIcon.amplitude", &LargeIconConfig::glowStrength, 0, 100, 1, 100, L"%", Field::Glow, 1);
        Slider(effects, "largeIcon.amplitude", &LargeIconConfig::shineStrength, 0, 100, 1, 100, L"%", Field::Shine, 1);
        Slider(effects, "largeIcon.shineDuration", &LargeIconConfig::shineDurationMs, 150, 1500, 50, 1, L"ms", Field::Shine, 1);
        Choice(effects, "largeIcon.direction", &LargeIconConfig::titleDirection, {{2,"largeIcon.auto"},{0,"largeIcon.left"},{1,"largeIcon.up"}}, Field::Title, 1);
        Slider(effects, "largeIcon.titleSize", &LargeIconConfig::revealTitleSize, 8, 72, 1, 1, L"", Field::Title, 1);
        Slider(effects, "largeIcon.titleWeight", &LargeIconConfig::titleWeight, 100, 900, 100, 1, L"", Field::Title, 1);
        Toggle(effects, "largeIcon.autoTitleColor", &LargeIconConfig::autoTitleColor, Field::Title, 1);
        ColorPicker(effects, "largeIcon.titleColor", &LargeIconConfig::titleColor, Field::ManualTitle, 2);
        auto visibilitySettings = Group("largeIcon.visibility");
        Toggle(visibilitySettings, "app.interact.hover_only", &LargeIconConfig::showOnHoverOnly, Field::Always, 0);
        Toggle(visibilitySettings, "app.interact.keep_when_hidden", &LargeIconConfig::keepWhenDesktopHidden, Field::Always, 0);
        Action(editors, "largeIcon.retry", "largeIcon.retry", [](auto& self) { self.Send("commit"); }, Field::Always, 0);
        const auto retry = editors.Children().GetAt(editors.Children().Size() - 1).as<x::FrameworkElement>();
        visibility.push_back([this, retry] { retry.Visibility(snapshot.error == "largeIcon.saveFailed" ? x::Visibility::Visible : x::Visibility::Collapsed); });
        Sync();
    }
};
LargeIconPagePresenter::LargeIconPagePresenter(std::function<std::wstring(std::string_view)> localize,
    x::Style style, LargeIconSettingsAction action, std::function<void()> nameChanged) : impl_(std::make_shared<Impl>())
{
    impl_->localize = std::move(localize); impl_->style = style; impl_->action = std::move(action);
    impl_->nameChanged = std::move(nameChanged);
    std::weak_ptr<Impl> weak = impl_;
    impl_->previews.Initialize([weak](const LargeIconConfig&) {
        if (auto self = weak.lock(); self && self->active && self->snapshot.editable) self->Send("preview");
    });
    impl_->root.KeyDown([weak](auto const&, auto const& args) {
        if (args.Key() == winrt::Windows::System::VirtualKey::Escape)
            if (auto self = weak.lock(); self && self->dirty) { self->Cancel(); args.Handled(true); }
    });
    impl_->timer.Interval(std::chrono::seconds(1));
    impl_->timer.Tick([weak](auto const&, auto const&) { if (auto self = weak.lock(); self && self->active) self->Send("status"); });
}
LargeIconPagePresenter::~LargeIconPagePresenter() { Deactivate(); impl_->previews.Close(); }
c::StackPanel LargeIconPagePresenter::Content() const { return impl_->root; }
std::wstring_view LargeIconPagePresenter::NameForKey(std::wstring_view key) const
{ return impl_->active && impl_->snapshot.key == key ? std::wstring_view(impl_->snapshot.name) : std::wstring_view{}; }
void LargeIconPagePresenter::Activate(std::wstring key)
{
    if (impl_->active && impl_->snapshot.key == key) return;
    Deactivate(); impl_->snapshot = {}; impl_->snapshot.key = std::move(key); impl_->active = true;
    impl_->Send("read"); impl_->Reload(); impl_->Build(); impl_->timer.Start();
}
void LargeIconPagePresenter::Deactivate()
{
    impl_->timer.Stop();
    if (impl_->active) { if (impl_->gradient) impl_->gradient->Flush(); if (impl_->dirty) impl_->Send("commit"); impl_->Send("cancel"); }
    impl_->active = false;
}
void LargeIconPagePresenter::RefreshLocalizedText() { if (impl_->active) { if (impl_->gradient) impl_->gradient->Flush(); impl_->Build(); } }
}

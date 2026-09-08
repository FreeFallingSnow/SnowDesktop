#pragma once
#include "appearance_sections.h"
#include "panel_gradient_editor.h"
#include "../personalization.h"

namespace snowdesktop::winui
{
// Value-only background editor for independent surfaces. Geometry and window
// behavior remain owned by the surface, so only supported appearance is shown.
class PanelAppearanceEditor : public std::enable_shared_from_this<PanelAppearanceEditor>
{
    using Panel = winrt::Microsoft::UI::Xaml::Controls::StackPanel;
    using Element = winrt::Microsoft::UI::Xaml::UIElement;
    using Edit = std::function<void(PersonalizationSettings&)>;
    using Condition = std::function<bool(const PersonalizationSettings&)>;
public:
    using Localize = PanelGradientEditor::Localize;
    using Change = std::function<void(const PersonalizationSettings&, bool)>;
    static std::shared_ptr<PanelAppearanceEditor> Create(Localize localize, Change change)
    {
        auto result = std::make_shared<PanelAppearanceEditor>();
        result->localize_ = std::move(localize); result->change_ = std::move(change);
        std::weak_ptr<PanelAppearanceEditor> weak = result;
        result->preview_.Initialize([weak](auto const& value) {
            if (auto self = weak.lock(); self && self->change_) self->change_(value, false);
        });
        result->Build(); return result;
    }
    ~PanelAppearanceEditor() { Close(); }
    Panel Content() const { return root_; }
    void SetValue(const PersonalizationSettings& value, bool force = false)
    {
        if (dirty_ && !force && value != value_) return;
        if (force || value != value_) { preview_.Cancel(); dirty_ = false; }
        value_ = value;
        if (force)
        {
            // Rebuild picker lifetimes on session replacement so a late close
            // or Escape cannot commit the previous session's original color.
            Build();
            return;
        }
        Sync();
    }
    void Flush()
    {
        if (syncing_ || closed_) return;
        for (auto& editor : colors_) editor->Dismiss();
        if (gradient_) gradient_->Flush();
        if (!dirty_) return;
        preview_.Cancel(); dirty_ = false;
        if (change_) change_(value_, true);
    }
    void Close() noexcept
    {
        closed_ = true; preview_.Close();
        if (gradient_) gradient_->Close();
        for (auto& editor : colors_) editor->Close();
        change_ = {}; localize_ = {};
    }
    void RefreshLocalizedText() { Flush(); Build(); }
private:
    Panel root_;
    AppearanceSections sections_;
    PersonalizationSettings value_;
    Localize localize_; Change change_;
    std::shared_ptr<PanelGradientEditor> gradient_;
    std::vector<std::unique_ptr<presenter_controls::ColorFlyoutEditor>> colors_;
    std::vector<std::function<void()>> sync_;
    presenter_controls::CoalescedPreviewTimer<PersonalizationSettings> preview_;
    bool syncing_ = false, dirty_ = false, closed_ = false;
    std::wstring L(const char* key) const { return localize_ ? localize_(key) : std::wstring{}; }
    void Sync()
    {
        syncing_ = true;
        gradient_->SetValue(value_.panelGradient);
        for (auto const& sync : sync_) sync();
        syncing_ = false;
    }
    void Apply(Edit edit, bool commit)
    {
        if (syncing_ || closed_) return;
        edit(value_); value_.backgroundPreset = kAppearancePresetCustom;
        dirty_ = !commit;
        if (commit) { preview_.Cancel(); if (change_) change_(value_, true); }
        else preview_.Queue(value_);
        Sync();
    }
    void Row(Panel parent, const char* key, Element control, Edit reset, Condition visible = {})
    {
        namespace x = winrt::Microsoft::UI::Xaml; namespace c = x::Controls;
        c::Grid group; group.ColumnSpacing(8);
        c::ColumnDefinition main; main.Width(x::GridLengthHelper::FromValueAndType(1, x::GridUnitType::Star));
        c::ColumnDefinition tail; tail.Width(x::GridLengthHelper::Auto());
        group.ColumnDefinitions().Append(main); group.ColumnDefinitions().Append(tail);
        group.Children().Append(control);
        c::Button restore; presenter_controls::ConfigureRestoreDefaultButton(restore, L("app.settings.restore_default") + L" · " + L(key));
        c::Grid::SetColumn(restore, 1); group.Children().Append(restore);
        std::weak_ptr<PanelAppearanceEditor> weak = shared_from_this();
        restore.Click([weak, reset](auto const&, auto const&) { if (auto self = weak.lock()) self->Apply(reset, true); });
        presenter_controls::SettingRow row; row.Initialize(group, presenter_controls::kSettingControlWidth + 40);
        row.SetText(L(key)); row.root.MinHeight(44); parent.Children().Append(row.root);
        x::Automation::AutomationProperties::SetName(control, L(key));
        if (visible) sync_.push_back([this, visible, root = row.root] { root.Visibility(visible(value_) ? x::Visibility::Visible : x::Visibility::Collapsed); });
    }
    void Number(Panel parent, const char* key, float PersonalizationSettings::* field,
        double minimum, double maximum, double step, double scale, const wchar_t* unit, Condition visible = {})
    {
        namespace x = winrt::Microsoft::UI::Xaml; namespace c = x::Controls;
        c::Grid pair; pair.ColumnSpacing(8);
        for (int i = 0; i < 3; ++i)
        {
            c::ColumnDefinition column;
            column.Width(i == 0 ? x::GridLengthHelper::FromValueAndType(1, x::GridUnitType::Star) : i == 1 ? x::GridLengthHelper::FromValueAndType(70, x::GridUnitType::Pixel) : x::GridLengthHelper::Auto());
            pair.ColumnDefinitions().Append(column);
        }
        c::Slider slider; slider.Minimum(minimum); slider.Maximum(maximum); slider.StepFrequency(step); slider.VerticalAlignment(x::VerticalAlignment::Center);
        c::NumberBox number; number.Minimum(minimum); number.Maximum(maximum); number.SmallChange(step); number.SpinButtonPlacementMode(c::NumberBoxSpinButtonPlacementMode::Hidden);
        c::TextBlock suffix; suffix.Text(unit); suffix.VerticalAlignment(x::VerticalAlignment::Center);
        c::Grid::SetColumn(number, 1); c::Grid::SetColumn(suffix, 2);
        pair.Children().Append(slider); pair.Children().Append(number); pair.Children().Append(suffix);
        x::Automation::AutomationProperties::SetName(slider, L(key)); x::Automation::AutomationProperties::SetName(number, L(key));
        std::weak_ptr<PanelAppearanceEditor> weak = shared_from_this();
        const auto changed = [weak, field, scale, minimum, maximum, step](auto const&, auto const& args) {
            if (auto self = weak.lock(); self && std::isfinite(args.NewValue()))
                self->Apply([=](auto& value) { value.*field = static_cast<float>(presenter_controls::QuantizeNumericValue(args.NewValue(), minimum, maximum, step) / scale); }, false);
        };
        slider.ValueChanged(changed); number.ValueChanged(changed);
        const auto flush = [weak](auto const&, auto const&) { if (auto self = weak.lock()) self->Flush(); };
        slider.PointerReleased(flush); slider.PointerCaptureLost(flush); slider.KeyUp(flush); slider.LostFocus(flush); number.KeyUp(flush); number.LostFocus(flush);
        sync_.push_back([this, field, scale, step, minimum, maximum, slider, number] {
            const auto value = presenter_controls::QuantizeNumericValue(value_.*field * scale, minimum, maximum, step);
            slider.Value(value); number.Value(value);
        });
        Row(parent, key, pair, [field](auto& value) { value.*field = PersonalizationSettings{}.*field; }, visible);
    }
    void Color(Panel parent, const char* key, float PersonalizationSettings::* red,
        float PersonalizationSettings::* green, float PersonalizationSettings::* blue, Condition visible = {})
    {
        namespace x = winrt::Microsoft::UI::Xaml;
        auto owned = std::make_unique<presenter_controls::ColorFlyoutEditor>(); auto* editor = owned.get();
        std::weak_ptr<PanelAppearanceEditor> weak = shared_from_this();
        editor->Initialize([weak, red, green, blue](auto color, SettingsUpdateMode mode) {
            if (auto self = weak.lock()) self->Apply([=](auto& value) { value.*red = color.R / 255.f; value.*green = color.G / 255.f; value.*blue = color.B / 255.f; }, mode == SettingsUpdateMode::PreviewAndCommit);
        });
        editor->SetText(L(key), {}, L("app.settings.cancel"));
        // Reuse the flyout's button in the common reset row.
        editor->row.controlHost.Content(nullptr);
        Row(parent, key, editor->button, [=](auto& value) { const PersonalizationSettings defaults; value.*red = defaults.*red; value.*green = defaults.*green; value.*blue = defaults.*blue; }, visible);
        sync_.push_back([this, editor, red, green, blue] {
            const auto byte = [](float n) { return static_cast<uint8_t>(std::lround(std::clamp(n, 0.f, 1.f) * 255)); };
            editor->SetColor({255, byte(value_.*red), byte(value_.*green), byte(value_.*blue)});
        });
        colors_.push_back(std::move(owned));
    }
    void Build()
    {
        namespace x = winrt::Microsoft::UI::Xaml; namespace c = x::Controls;
        syncing_ = true;
        for (auto& editor : colors_) editor->Close(); colors_.clear();
        if (gradient_) gradient_->Close();
        root_.Children().Clear(); sync_.clear(); root_.Spacing(8);
        sections_ = {}; sections_.Initialize(root_); sections_.RefreshLocalizedText(localize_);
        std::weak_ptr<PanelAppearanceEditor> weak = shared_from_this();
        gradient_ = PanelGradientEditor::Create(localize_, [weak](auto const& value, bool commit) {
            if (auto self = weak.lock()) self->Apply([&](auto& appearance) { appearance.panelGradient = value; }, commit);
        }, false, {}, true);
        sections_.colors.Children().Append(gradient_->Content());
        const auto solid = [](auto const& value) { return !value.panelGradient.enabled; };
        Color(sections_.colors, "app.settings.bg_color", &PersonalizationSettings::widgetBgR, &PersonalizationSettings::widgetBgG, &PersonalizationSettings::widgetBgB, solid);
        Number(sections_.colors, "app.settings.bg_opacity", &PersonalizationSettings::widgetAlpha, 0, 100, 1, 100, L"%", solid);
        c::ToggleSwitch glass; glass.HorizontalAlignment(x::HorizontalAlignment::Right);
        glass.Toggled([weak, glass](auto const&, auto const&) {
            if (auto self = weak.lock()) self->Apply([&](auto& value) { value.glassEnabled = glass.IsOn(); }, true);
        });
        sync_.push_back([this, glass] { glass.IsOn(value_.glassEnabled); });
        Row(sections_.material, "app.settings.glass_enabled", glass, [](auto& value) { value.glassEnabled = false; });
        c::ToggleSwitch acrylic; acrylic.HorizontalAlignment(x::HorizontalAlignment::Right);
        acrylic.Toggled([weak, acrylic](auto const&, auto const&) {
            if (auto self = weak.lock()) self->Apply([&](auto& value) { value.acrylicEnabled = acrylic.IsOn(); }, true);
        });
        sync_.push_back([this, acrylic] { acrylic.IsOn(value_.acrylicEnabled); });
        Row(sections_.material, "app.settings.acrylic_noise", acrylic, [](auto& value) { value.acrylicEnabled = false; }, [](auto const& value) { return value.glassEnabled; });
        Number(sections_.material, "app.settings.blur_radius", &PersonalizationSettings::glassBlurRadius, 4, 48, 1, 1, L"px", [](auto const& value) { return value.glassEnabled; });
        c::ComboBox theme; theme.HorizontalAlignment(x::HorizontalAlignment::Right);
        theme.Items().Append(winrt::box_value(L("app.settings.light"))); theme.Items().Append(winrt::box_value(L("app.settings.dark")));
        theme.SelectionChanged([weak, theme](auto const&, auto const&) { if (auto self = weak.lock()) self->Apply([&](auto& value) { value.contentTheme = std::clamp(theme.SelectedIndex(), 0, 1); }, true); });
        sync_.push_back([this, theme] { theme.SelectedIndex(value_.contentTheme); });
        Row(sections_.text, "app.settings.text_color", theme, [](auto& value) { value.contentTheme = 0; });
        Color(sections_.border, "app.settings.border_color", &PersonalizationSettings::widgetBorderR, &PersonalizationSettings::widgetBorderG, &PersonalizationSettings::widgetBorderB);
        Number(sections_.border, "largeIcon.borderOpacity", &PersonalizationSettings::widgetBorderAlpha, 0, 100, 1, 100, L"%");
        Number(sections_.border, "largeIcon.borderWidth", &PersonalizationSettings::widgetBorderWidth, .5, 4, .5, 1, L"px");
        c::ToggleSwitch highlight; highlight.HorizontalAlignment(x::HorizontalAlignment::Right);
        highlight.Toggled([weak, highlight](auto const&, auto const&) { if (auto self = weak.lock()) self->Apply([&](auto& value) { value.widgetEdgeHighlightEnabled = highlight.IsOn(); }, true); });
        sync_.push_back([this, highlight] { highlight.IsOn(value_.widgetEdgeHighlightEnabled); });
        Row(sections_.border, "largeIcon.edgeHighlight", highlight, [](auto& value) { value.widgetEdgeHighlightEnabled = PersonalizationSettings{}.widgetEdgeHighlightEnabled; });
        const auto edge = [](auto const& value) { return value.widgetEdgeHighlightEnabled; };
        Number(sections_.border, "largeIcon.edgeWidth", &PersonalizationSettings::widgetEdgeHighlightWidth, .5, 4, .5, 1, L"px", edge);
        Number(sections_.border, "largeIcon.edgeStrength", &PersonalizationSettings::widgetEdgeHighlightStrength, 0, 100, 1, 100, L"%", edge);
        Sync();
    }
};
}

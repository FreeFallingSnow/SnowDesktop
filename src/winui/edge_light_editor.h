#pragma once
#include "settings_presenter_controls.h"
#include "theme/edge_light_settings.h"
#include <winrt/Windows.UI.Text.h>

namespace snowdesktop::winui
{
// Shared controls for panels, icon plates and the Dock. Each callback carries
// values only; the presenter owns persistence and preview-session boundaries.
class EdgeLightEditor : public std::enable_shared_from_this<EdgeLightEditor>
{
    using Panel = winrt::Microsoft::UI::Xaml::Controls::StackPanel;
public:
    using Localize = std::function<std::wstring(std::string_view)>;
    using Change = std::function<void(const EdgeLightSettings&, bool)>;
    static std::shared_ptr<EdgeLightEditor> Create(Localize localize, Change change)
    {
        auto self = std::make_shared<EdgeLightEditor>();
        self->localize_ = std::move(localize); self->change_ = std::move(change);
        std::weak_ptr<EdgeLightEditor> weak = self;
        self->preview_.Initialize([weak](auto const& value) {
            if (auto editor = weak.lock(); editor && editor->change_) editor->change_(value, false);
        });
        self->Build(); return self;
    }
    Panel Content() const { return root_; }
    void SetValue(EdgeLightSettings value, bool force = false)
    {
        if (dirty_ && !force && value != value_) return;
        if (force) { preview_.Cancel(); dirty_ = false; }
        value_ = NormalizeEdgeLight(value); Sync();
    }
    void Flush()
    {
        if (!dirty_ || closed_) return;
        preview_.Cancel(); dirty_ = false;
        if (change_) change_(value_, true);
    }
    void Close() noexcept { closed_ = true; preview_.Close(); change_ = {}; localize_ = {}; }
    void Cancel() noexcept { preview_.Cancel(); dirty_ = false; }
    ~EdgeLightEditor() { Close(); }
    void RefreshLocalizedText() { Flush(); Build(); }
private:
    Panel root_;
    EdgeLightSettings value_;
    Localize localize_; Change change_;
    std::vector<std::function<void()>> sync_;
    presenter_controls::CoalescedPreviewTimer<EdgeLightSettings> preview_;
    bool syncing_ = false, dirty_ = false, closed_ = false;
    std::wstring L(std::string_view key) const { return localize_ ? localize_(key) : std::wstring{}; }
    void Sync() { syncing_ = true; for (auto const& sync : sync_) sync(); syncing_ = false; }
    void Apply(float EdgeLightSettings::* field, float value, bool commit)
    {
        if (syncing_ || closed_) return;
        value_.*field = value; value_ = NormalizeEdgeLight(value_); dirty_ = !commit;
        if (commit) { preview_.Cancel(); if (change_) change_(value_, true); }
        else preview_.Queue(value_);
        Sync();
    }
    void Number(Panel parent, const char* name, float EdgeLightSettings::* field,
        double minimum, double maximum, double step, double scale, const wchar_t* unit)
    {
        namespace x = winrt::Microsoft::UI::Xaml; namespace c = x::Controls;
        const std::string key = "edgeLight." + std::string(name);
        c::Grid pair; pair.ColumnSpacing(8);
        for (int i = 0; i < 4; ++i)
        {
            c::ColumnDefinition column;
            column.Width(i == 0 ? x::GridLengthHelper::FromValueAndType(1, x::GridUnitType::Star)
                : i == 1 ? x::GridLengthHelper::FromValueAndType(76, x::GridUnitType::Pixel) : x::GridLengthHelper::Auto());
            pair.ColumnDefinitions().Append(column);
        }
        c::Slider slider; slider.Minimum(minimum); slider.Maximum(maximum); slider.StepFrequency(step); slider.VerticalAlignment(x::VerticalAlignment::Center);
        c::NumberBox number; number.Minimum(minimum); number.Maximum(maximum); number.SmallChange(step);
        number.SpinButtonPlacementMode(c::NumberBoxSpinButtonPlacementMode::Hidden);
        number.ValidationMode(c::NumberBoxValidationMode::InvalidInputOverwritten);
        c::TextBlock suffix; suffix.Text(unit); suffix.VerticalAlignment(x::VerticalAlignment::Center); suffix.Opacity(.72);
        c::Button restore; presenter_controls::ConfigureRestoreDefaultButton(restore, L("app.settings.restore_default") + L" · " + L(key));
        pair.Children().Append(slider); c::Grid::SetColumn(number, 1); pair.Children().Append(number);
        c::Grid::SetColumn(suffix, 2); pair.Children().Append(suffix); c::Grid::SetColumn(restore, 3); pair.Children().Append(restore);
        presenter_controls::SettingRow row; row.Initialize(pair, presenter_controls::kSettingControlWidth + 40);
        row.SetText(L(key), L(key + ".description")); parent.Children().Append(row.root);
        x::Automation::AutomationProperties::SetName(slider, L(key)); x::Automation::AutomationProperties::SetName(number, L(key));
        std::weak_ptr<EdgeLightEditor> weak = shared_from_this();
        const auto changed = [weak, field, scale, minimum, maximum, step](auto const&, auto const& args) {
            if (auto self = weak.lock(); self && std::isfinite(args.NewValue()))
                self->Apply(field, static_cast<float>(presenter_controls::QuantizeNumericValue(args.NewValue(), minimum, maximum, step) / scale), false);
        };
        slider.ValueChanged(changed); number.ValueChanged(changed);
        const auto flush = [weak](auto const&, auto const&) { if (auto self = weak.lock()) self->Flush(); };
        slider.PointerReleased(flush); slider.PointerCaptureLost(flush); slider.KeyUp(flush); slider.LostFocus(flush); number.KeyUp(flush); number.LostFocus(flush);
        restore.Click([weak, field](auto const&, auto const&) { if (auto self = weak.lock()) self->Apply(field, EdgeLightSettings{}.*field, true); });
        sync_.push_back([this, slider, number, field, scale] { slider.Value(value_.*field * scale); presenter_controls::SyncNumberBoxValue(number, value_.*field * scale); });
    }
    void Build()
    {
        namespace c = winrt::Microsoft::UI::Xaml::Controls;
        syncing_ = true; root_.Children().Clear(); sync_.clear(); root_.Spacing(4);
        Number(root_, "direction", &EdgeLightSettings::direction, 0, 360, 1, 1, L"°");
        c::Expander advanced; advanced.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
        advanced.HorizontalContentAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
        advanced.Header(winrt::box_value(L("edgeLight.advanced"))); advanced.IsExpanded(false);
        Panel details; details.Spacing(4); advanced.Content(details); root_.Children().Append(advanced);
        const auto group = [&](const char* key) { c::TextBlock title; title.Text(L(key)); title.Margin({0,12,0,4}); title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold()); details.Children().Append(title); };
        group("edgeLight.lighting");
        Number(details,"spread",&EdgeLightSettings::spread,0,180,1,1,L"°");
        Number(details,"feather",&EdgeLightSettings::feather,.5,90,.5,1,L"°");
        Number(details,"ambient",&EdgeLightSettings::ambient,0,100,1,100,L"%");
        Number(details,"primary",&EdgeLightSettings::primary,0,100,1,100,L"%");
        Number(details,"opposite",&EdgeLightSettings::opposite,0,100,1,100,L"%");
        group("edgeLight.shape");
        Number(details,"minimumWidth",&EdgeLightSettings::minimumWidth,10,100,1,100,L"%");
        Number(details,"widthVariation",&EdgeLightSettings::widthVariation,0,100,1,100,L"%");
        group("edgeLight.glow");
        Number(details,"innerGlow",&EdgeLightSettings::innerGlow,0,4,.1,1,L"×");
        Number(details,"outerGlow",&EdgeLightSettings::outerGlow,0,3,.1,1,L"×");
        Number(details,"glowStrength",&EdgeLightSettings::glowStrength,0,100,1,100,L"%");
        Number(details,"glowFalloff",&EdgeLightSettings::glowFalloff,.5,8,.1,1,L"");
        Number(details,"glowThreshold",&EdgeLightSettings::glowThreshold,0,99,1,100,L"%");
        group("edgeLight.shadow");
        Number(details,"shadowStrength",&EdgeLightSettings::shadowStrength,0,100,1,100,L"%");
        Number(details,"shadowFalloff",&EdgeLightSettings::shadowFalloff,1,12,.05,1,L"");
        Sync();
    }
};
}

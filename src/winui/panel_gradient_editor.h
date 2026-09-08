#pragma once
#include "settings_presenter_controls.h"
#include "../panel_gradient.h"
#include <winrt/Windows.UI.Text.h>

namespace snowdesktop::winui
{
// Shared by global component appearance, per-component appearance and large
// icons. It owns controls and values only; the caller owns preview/commit I/O.
class PanelGradientEditor : public std::enable_shared_from_this<PanelGradientEditor>
{
    using X = winrt::Microsoft::UI::Xaml::UIElement;
    using Panel = winrt::Microsoft::UI::Xaml::Controls::StackPanel;
    using Edit = std::function<void(PanelGradient&)>;
    using Read = std::function<double(const PanelGradient&)>;
    using Write = std::function<void(PanelGradient&, double)>;
public:
    using Localize = std::function<std::wstring(std::string_view)>;
    using Change = std::function<void(const PanelGradient&, bool)>;
    using PickerExtras = std::function<void(Panel, winrt::Microsoft::UI::Xaml::Controls::ColorPicker)>;
    static std::shared_ptr<PanelGradientEditor> Create(Localize localize, Change change, bool required = false,
        PickerExtras pickerExtras = {}, bool compact = false)
    {
        auto result = std::make_shared<PanelGradientEditor>();
        result->required_ = required;
        result->pickerExtras_ = std::move(pickerExtras); result->compact_ = compact;
        result->localize_ = std::move(localize); result->change_ = std::move(change);
        std::weak_ptr<PanelGradientEditor> weak = result;
        result->preview_.Initialize([weak](const PanelGradient& value) {
            if (auto self = weak.lock(); self && self->change_) self->change_(value, false);
        });
        result->Build(); return result;
    }
    ~PanelGradientEditor() { Close(); }
    void Close() noexcept { preview_.Close(); change_ = {}; localize_ = {}; }
    Panel Content() const { return root_; }
    void SetValue(const PanelGradient& value, bool force = false)
    {
        if (!ValidatePanelGradient(value) || (dirty_ && !force && value != value_)) return;
        const bool rebuild = value.enabled != value_.enabled || value.stops.size() != value_.stops.size();
        if (force || value != value_) { preview_.Cancel(); dirty_ = false; }
        if (!dirty_) committed_ = value;
        value_ = value;
        if (rebuild) Build(); else Sync();
    }
    void Flush()
    {
        if (!dirty_) return;
        preview_.Cancel(); dirty_ = false; committed_ = value_;
        if (change_) change_(value_, true);
    }
    void Cancel()
    {
        if (!dirty_) return;
        preview_.Cancel();
        const auto original = committed_;
        dirty_ = false; value_ = original;
        if (change_) change_(original, true);
        Build();
    }
    void RefreshLocalizedText() { Build(); }
private:
    Panel root_;
    presenter_controls::CoalescedPreviewTimer<PanelGradient> preview_;
    PanelGradient value_, committed_;
    Localize localize_;
    Change change_;
    PickerExtras pickerExtras_;
    std::vector<std::function<void()>> sync_;
    bool syncing_ = false, dirty_ = false, expanded_ = false, required_ = false, compact_ = false;
    std::wstring L(std::string_view key) const { return localize_ ? localize_(key) : std::wstring{}; }
    void Sync()
    {
        syncing_ = true; for (const auto& sync : sync_) sync(); syncing_ = false;
    }
    void Apply(Edit edit, bool commit)
    {
        if (syncing_) return;
        auto next = value_; edit(next);
        if (!ValidatePanelGradient(next)) { Sync(); return; }
        const bool rebuild = next.enabled != value_.enabled || next.stops.size() != value_.stops.size();
        if (!dirty_) committed_ = value_;
        value_ = std::move(next); dirty_ = !commit;
        if (commit) committed_ = value_;
        if (commit) { preview_.Cancel(); if (change_) change_(value_, true); }
        else preview_.Queue(value_);
        if (rebuild) Build(); else Sync();
    }
    void Row(const char* key, const X& control, std::function<void()> reset, int level = 1)
    {
        namespace x = winrt::Microsoft::UI::Xaml;
        namespace c = x::Controls;
        c::Grid editor; editor.ColumnSpacing(8);
        c::ColumnDefinition value; value.Width(x::GridLengthHelper::FromValueAndType(1, x::GridUnitType::Star));
        c::ColumnDefinition button; button.Width(x::GridLengthHelper::Auto());
        editor.ColumnDefinitions().Append(value); editor.ColumnDefinitions().Append(button);
        editor.Children().Append(control);
        c::Button restore;
        presenter_controls::ConfigureRestoreDefaultButton(restore, L("app.settings.restore_default") + L" · " + L(key));
        c::Grid::SetColumn(restore, 1); editor.Children().Append(restore);
        restore.Click([reset = std::move(reset)](auto const&, auto const&) { reset(); });
        presenter_controls::SettingRow row;
        row.Initialize(editor, presenter_controls::kSettingControlWidth + 40);
        row.SetText(L(key)); row.root.MinHeight(44); row.root.Margin({level * 16., 0, 0, 0});
        x::Automation::AutomationProperties::SetName(control, L(key));
        root_.Children().Append(row.root);
    }
    void ActionRow(const char* key, const X& control, int level = 1)
    {
        namespace x = winrt::Microsoft::UI::Xaml;
        presenter_controls::SettingRow row;
        row.Initialize(control); row.SetText(L(key));
        row.SetControlAlignment(x::HorizontalAlignment::Right);
        row.root.MinHeight(44); row.root.Margin({level * 16., 0, 0, 0});
        x::Automation::AutomationProperties::SetName(control, L(key));
        root_.Children().Append(row.root);
    }
    void Numeric(const char* key, Read read, Write write, double min, double max, double step, double defaultValue, const wchar_t* unit = L"%", int level = 1)
    {
        namespace x = winrt::Microsoft::UI::Xaml;
        namespace c = x::Controls;
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
        x::Automation::AutomationProperties::SetName(slider, L(key));
        x::Automation::AutomationProperties::SetName(number, L(key));
        std::weak_ptr<PanelGradientEditor> weak = shared_from_this();
        slider.ValueChanged([weak, write](auto const&, auto const& args) {
            if (auto self = weak.lock()) self->Apply([&](auto& v) { write(v, args.NewValue()); }, false);
        });
        number.ValueChanged([weak, write, min, max, step](auto const&, auto const& args) {
            if (auto self = weak.lock(); self && std::isfinite(args.NewValue()))
                self->Apply([&](auto& v) { write(v, presenter_controls::QuantizeNumericValue(args.NewValue(), min, max, step)); }, false);
        });
        const auto flush = [weak](auto const&, auto const&) { if (auto self = weak.lock(); self && !self->syncing_) self->Flush(); };
        slider.PointerReleased(flush); slider.PointerCaptureLost(flush); slider.KeyUp(flush); slider.LostFocus(flush);
        number.KeyUp(flush); number.LostFocus(flush);
        sync_.push_back([this, read, slider, number] { slider.Value(read(value_)); number.Value(read(value_)); });
        Row(key, pair, [weak, write, defaultValue] {
            if (auto self = weak.lock()) self->Apply([&](auto& v) { write(v, defaultValue); }, true);
        }, level);
    }
    winrt::Microsoft::UI::Xaml::Controls::Button ColorButton(const char* key, size_t index)
    {
        namespace x = winrt::Microsoft::UI::Xaml;
        namespace c = x::Controls;
        namespace m = x::Media;
        c::Button button; button.HorizontalAlignment(x::HorizontalAlignment::Right);
        c::StackPanel content; content.Orientation(c::Orientation::Horizontal); content.Spacing(8);
        c::Border swatch; swatch.Width(20); swatch.Height(20); swatch.CornerRadius({4});
        c::TextBlock text; content.Children().Append(swatch); content.Children().Append(text); button.Content(content);
        c::Flyout flyout; c::ColorPicker picker; picker.IsAlphaEnabled(false);
        c::StackPanel body; body.Spacing(8); body.Children().Append(picker); flyout.Content(body); button.Flyout(flyout);
        std::weak_ptr<PanelGradientEditor> weak = shared_from_this();
        flyout.Opening([weak, body, picker](auto const&, auto const&) {
            body.Children().Clear(); body.Children().Append(picker);
            if (auto self = weak.lock(); self && self->pickerExtras_) self->pickerExtras_(body, picker);
        });
        x::Automation::AutomationProperties::SetName(button, L(key));
        c::ToolTipService::SetToolTip(button, winrt::box_value(L(key)));
        picker.ColorChanged([weak, index](auto const&, auto const& args) {
            if (auto self = weak.lock()) self->Apply([&](auto& g) {
                if (index < g.stops.size()) { const auto v = args.NewColor(); g.stops[index].color = v.R << 16 | v.G << 8 | v.B; }
            }, false);
        });
        flyout.Closed([weak](auto const&, auto const&) { if (auto self = weak.lock()) self->Flush(); });
        picker.KeyDown([weak, flyout](auto const&, auto const& args) {
            if (args.Key() == winrt::Windows::System::VirtualKey::Escape)
                if (auto self = weak.lock()) { self->Cancel(); args.Handled(true); flyout.Hide(); }
        });
        sync_.push_back([this, index, picker, swatch, text] {
            if (index >= value_.stops.size()) return;
            const unsigned rgb = value_.stops[index].color;
            winrt::Windows::UI::Color color{255, static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb)};
            picker.Color(color); swatch.Background(m::SolidColorBrush(color));
            wchar_t label[8]; swprintf_s(label, L"#%06X", rgb); text.Text(label);
        });
        return button;
    }
    void Color(const char* key, size_t index, unsigned defaultColor, int level = 1)
    {
        std::weak_ptr<PanelGradientEditor> weak = shared_from_this();
        Row(key, ColorButton(key, index), [weak, index, defaultColor] {
            if (auto self = weak.lock()) self->Apply([&](auto& v) { if (index < v.stops.size()) v.stops[index].color = defaultColor; }, true);
        }, level);
    }
    void Stop(size_t index, bool first, bool last)
    {
        Color(first ? "panelGradient.startColor" : last ? "panelGradient.endColor" : "panelGradient.stopColor",
            index, first ? 0xb6d6ef : last ? 0xd5c7ef : 0xc8d0ef, first || last ? 1 : 2);
        Numeric(first ? "panelGradient.startOpacity" : last ? "panelGradient.endOpacity" : "panelGradient.stopOpacity",
            [index](auto const& v) { return v.stops[index].opacity * 100; },
            [index](auto& v, double n) { if (index < v.stops.size()) v.stops[index].opacity = n / 100; }, 0, 100, 1, 65,
            L"%", first || last ? 1 : 2);
        if (first || last) return;
        Numeric("panelGradient.stopPosition", [index](auto const& v) { return v.stops[index].position * 100; },
            [index](auto& v, double n) {
                if (index > 0 && index + 1 < v.stops.size())
                {
                    const double left = v.stops[index - 1].position, right = v.stops[index + 1].position;
                    const double gap = std::min(.001, (right - left) / 4);
                    v.stops[index].position = std::clamp(n / 100, left + gap, right - gap);
                }
            }, 0, 100, .1, 50, L"%", 2);
        namespace x = winrt::Microsoft::UI::Xaml;
        x::Controls::Button remove; remove.Content(winrt::box_value(L("panelGradient.removeStop")));
        remove.HorizontalAlignment(x::HorizontalAlignment::Right);
        std::weak_ptr<PanelGradientEditor> weak = shared_from_this();
        remove.Click([weak, index](auto const&, auto const&) {
            if (auto self = weak.lock()) self->Apply([index](auto& v) {
                if (index > 0 && index + 1 < v.stops.size()) v.stops.erase(v.stops.begin() + index);
            }, true);
        });
        root_.Children().Append(remove);
    }
    void Build()
    {
        namespace x = winrt::Microsoft::UI::Xaml;
        namespace c = x::Controls;
        syncing_ = true; sync_.clear(); root_.Children().Clear(); root_.Spacing(4);
        std::weak_ptr<PanelGradientEditor> weak = shared_from_this();
        c::ToggleSwitch enabled; enabled.IsOn(value_.enabled); enabled.MinWidth(0);
        enabled.HorizontalAlignment(x::HorizontalAlignment::Right);
        enabled.Toggled([weak, enabled](auto const&, auto const&) {
            if (auto self = weak.lock()) self->Apply([&](auto& v) { v.enabled = enabled.IsOn(); }, true);
        });
        sync_.push_back([this, enabled] { enabled.IsOn(value_.enabled); });
        if (!required_) Row("panelGradient.enabled", enabled, [weak] {
            if (auto self = weak.lock()) self->Apply([](auto& v) { v.enabled = false; }, true);
        }, 0);
        if (value_.enabled)
        {
            if (!compact_) { Stop(0, true, false); Stop(value_.stops.size() - 1, false, true); }
            c::Button swap; swap.Content(winrt::box_value(L"⇄")); swap.HorizontalAlignment(x::HorizontalAlignment::Right);
            swap.Click([weak](auto const&, auto const&) {
                if (auto self = weak.lock()) self->Apply([](auto& v) {
                    std::swap(v.stops.front().color, v.stops.back().color);
                    std::swap(v.stops.front().opacity, v.stops.back().opacity);
                }, true);
            });
            if (compact_)
            {
                c::StackPanel colors; colors.Orientation(c::Orientation::Horizontal); colors.Spacing(6);
                colors.HorizontalAlignment(x::HorizontalAlignment::Right);
                colors.Children().Append(ColorButton("panelGradient.startColor", 0)); colors.Children().Append(swap);
                colors.Children().Append(ColorButton("panelGradient.endColor", value_.stops.size() - 1));
                x::Automation::AutomationProperties::SetName(swap, L("panelGradient.swap"));
                c::ToolTipService::SetToolTip(swap, winrt::box_value(L("panelGradient.swap")));
                Row("largeIcon.gradientColors", colors, [weak] {
                    if (auto self = weak.lock()) self->Apply([](auto& v) {
                        v.stops.front().color = 0xb6d6ef; v.stops.back().color = 0xd5c7ef;
                    }, true);
                }, 0);
            }
            Numeric("panelGradient.angle", [](auto const& v) { return v.angle; }, [](auto& v, double n) { v.angle = n; }, 0, 360, 1, 90, L"°", compact_ ? 0 : 1);
            if (!compact_) ActionRow("panelGradient.swap", swap);
            c::Button more; more.Content(winrt::box_value(L(expanded_ ? "largeIcon.hideDetails" : "largeIcon.showDetails")));
            more.HorizontalAlignment(x::HorizontalAlignment::Right);
            more.Click([weak](auto const&, auto const&) {
                if (auto self = weak.lock(); self && !self->syncing_)
                { self->expanded_ = !self->expanded_; self->Build(); }
            });
            ActionRow("panelGradient.more", more);
            if (expanded_)
            {
                if (compact_)
                {
                    Numeric("panelGradient.startOpacity", [](auto const& v) { return v.stops.front().opacity * 100; },
                        [](auto& v, double n) { v.stops.front().opacity = n / 100; }, 0, 100, 1, 65);
                    Numeric("panelGradient.endOpacity", [](auto const& v) { return v.stops.back().opacity * 100; },
                        [](auto& v, double n) { v.stops.back().opacity = n / 100; }, 0, 100, 1, 65);
                }
                Numeric("panelGradient.startPosition", [](auto const& v) { return v.start * 100; },
                    [](auto& v, double n) { v.start = std::min(n / 100, v.end - .001); }, 0, 100, .1, 0, L"%", 2);
                Numeric("panelGradient.endPosition", [](auto const& v) { return v.end * 100; },
                    [](auto& v, double n) { v.end = std::max(n / 100, v.start + .001); }, 0, 100, .1, 100, L"%", 2);
                for (size_t i = 1; i + 1 < value_.stops.size(); ++i) Stop(i, false, false);
                c::Button add; add.Content(winrt::box_value(L("panelGradient.addStop"))); add.IsEnabled(value_.stops.size() < 5);
                add.HorizontalAlignment(x::HorizontalAlignment::Right);
                add.Click([weak](auto const&, auto const&) {
                    if (auto self = weak.lock()) self->Apply([](auto& v) {
                        if (v.stops.size() >= 5) return;
                        size_t gap = 1;
                        for (size_t i = 2; i < v.stops.size(); ++i)
                            if (v.stops[i].position - v.stops[i - 1].position > v.stops[gap].position - v.stops[gap - 1].position) gap = i;
                        const double position = (v.stops[gap - 1].position + v.stops[gap].position) / 2;
                        v.stops.insert(v.stops.begin() + gap, {position, 0xc8d0ef, .65});
                    }, true);
                });
                ActionRow("panelGradient.addStop", add, 2);
                c::Button reset; reset.Content(winrt::box_value(L("app.settings.restore_default")));
                reset.Click([weak](auto const&, auto const&) {
                    if (auto self = weak.lock()) self->Apply([](auto& v) { v = {}; v.enabled = true; }, true);
                });
                ActionRow("panelGradient.reset", reset, 2);
            }
        }
        Sync();
    }
};
}

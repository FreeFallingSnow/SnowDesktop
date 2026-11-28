#pragma once

#include "dock_page_actions.h"
#include "dock/dock_layout_settings.h"
#include "settings_presenter_controls.h"

#include <array>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace snowdesktop::winui
{
template <typename Override, typename Layout>
class BarMonitorOverridesEditor
{
public:
    using Edit = std::function<void(Override&)>;
    using Apply = std::function<void(BarMonitorRole, Edit)>;
    using Localize = std::function<std::wstring(std::string_view)>;

    BarMonitorOverridesEditor(Localize localize, const DockPageActions& actions, Apply apply)
        : localize_(std::move(localize)), actions_(actions), apply_(std::move(apply))
    {
        using namespace presenter_controls;
        heading_.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        InitializeExpander(section_, heading_, groupsPanel_);
        root_ = SettingsDisclosureRow(section_);
        const auto expanding = section_.Expanding([this](const auto&, const auto&) {
            if (RefreshAccess() || groups_[0].value != Override{} || groups_[1].value != Override{}) return;
            section_.IsExpanded(false);
            if (available_ && actions_.openAdvancedFeatureUnlock) actions_.openAdvancedFeatureUnlock();
        });
        revoke_.emplace_back([control = section_, expanding] { control.Expanding(expanding); });
        for (auto& group : groups_)
        {
            group.heading.Margin({16, 0, 0, 0});
            InitializeExpander(group.expander, group.heading, group.body);
            group.body.Children().Append(group.fields);
            group.resetRow.Initialize(group.reset);
            group.body.Children().Append(group.resetRow.root);
            groupsPanel_.Children().Append(SettingsDisclosureRow(group.expander));
            const auto reset = group.reset.Click([this, role = group.role](const auto&, const auto&) {
                apply_(role, [](Override& value) { value = {}; });
            });
            revoke_.emplace_back([control = group.reset, reset] { control.Click(reset); });
            const auto groupExpanding = group.expander.Expanding([this, role = group.role](const auto&, const auto&) {
                auto& current = groups_[role == BarMonitorRole::Last ? 0 : 1];
                if (RefreshAccess() || current.value != Override{}) return;
                current.expander.IsExpanded(false);
                if (available_ && actions_.openAdvancedFeatureUnlock) actions_.openAdvancedFeatureUnlock();
            });
            revoke_.emplace_back([control = group.expander, groupExpanding] { control.Expanding(groupExpanding); });
        }
    }

    ~BarMonitorOverridesEditor()
    {
        for (auto& revoke : revoke_)
            try { revoke(); } catch (...) {}
    }

    presenter_controls::mux::FrameworkElement Content() const { return root_; }

    template <typename T>
    void AddChoice(const char* key, std::optional<T> Override::* member, T Layout::* baseMember,
        std::vector<std::pair<T, const char*>> choices,
        std::function<bool(const Layout&)> applicable = {}, std::optional<T> defaultValue = {})
    {
        using namespace presenter_controls;
        AddField<T>(key, member, baseMember, applicable, [&](const auto& set) {
            muxc::ComboBox combo;
            combo.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
            muxc::Button reset{nullptr};
            mux::FrameworkElement content = combo;
            if (defaultValue)
            {
                reset = muxc::Button{};
                const auto click = reset.Click([set, defaultValue](const auto&, const auto&) { set(defaultValue); });
                revoke_.emplace_back([reset, click] { reset.Click(click); });
                content = EditorWithReset(combo, reset);
            }
            const auto selected = combo.SelectionChanged([combo, choices, set](const auto&, const auto&) {
                const int index = combo.SelectedIndex();
                if (combo.IsEnabled() && index >= 0 && static_cast<size_t>(index) < choices.size())
                    set(choices[static_cast<size_t>(index)].first);
            });
            revoke_.emplace_back([combo, selected] { combo.SelectionChanged(selected); });
            return FieldEditor<T>{content, [this, combo, reset, choices, defaultValue](
                T value, bool enabled, const std::wstring& label) {
                combo.IsEnabled(enabled);
                muxa::AutomationProperties::SetName(combo, label);
                int index = 0;
                for (const auto& [option, text] : choices)
                {
                    const auto description = localize_(text);
                    if (index >= static_cast<int>(combo.Items().Size()))
                        combo.Items().Append(winrt::box_value(description));
                    else if (winrt::unbox_value<winrt::hstring>(combo.Items().GetAt(index)) != winrt::hstring{description})
                        combo.Items().SetAt(index, winrt::box_value(description));
                    if (option == value && combo.SelectedIndex() != index) combo.SelectedIndex(index);
                    ++index;
                }
                if (reset)
                {
                    ConfigureRestoreDefaultButton(reset, label + L" · " + localize_("app.settings.restore_default"));
                    reset.IsEnabled(enabled && value != *defaultValue);
                }
            }};
        });
    }

    void AddToggle(const char* key, std::optional<bool> Override::* member, bool Layout::* baseMember,
        bool invert = false)
    {
        using namespace presenter_controls;
        AddField<bool>(key, member, baseMember, {}, [&](const auto& set) {
            muxc::ToggleSwitch toggle;
            toggle.MinWidth(0);
            toggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
            const auto toggled = toggle.Toggled([toggle, invert, set](const auto&, const auto&) {
                if (toggle.IsEnabled()) set(toggle.IsOn() != invert);
            });
            revoke_.emplace_back([toggle, toggled] { toggle.Toggled(toggled); });
            return FieldEditor<bool>{toggle, [toggle, invert](bool value, bool enabled, const std::wstring& label) {
                toggle.IsEnabled(enabled);
                toggle.IsOn(value != invert);
                muxa::AutomationProperties::SetName(toggle, label);
            }};
        });
    }

    void AddPercentage(const char* key, std::optional<float> Override::* member, float Layout::* baseMember,
        double minimum, double maximum, double step, double defaultValue,
        std::function<bool(const Layout&)> applicable = {})
    {
        using namespace presenter_controls;
        AddField<float>(key, member, baseMember, applicable, [&](const auto& set) {
            muxc::Slider slider;
            slider.Minimum(minimum);
            slider.Maximum(maximum);
            slider.StepFrequency(step);
            slider.SmallChange(step);
            slider.VerticalAlignment(mux::VerticalAlignment::Center);
            muxc::NumberBox number;
            number.Width(kSettingNumberWidth);
            number.Minimum(minimum);
            number.Maximum(maximum);
            number.SmallChange(step);
            number.LargeChange(step * 5);
            number.SpinButtonPlacementMode(muxc::NumberBoxSpinButtonPlacementMode::Compact);
            muxc::TextBlock unit;
            unit.Text(L"%");
            unit.Opacity(.72);
            unit.VerticalAlignment(mux::VerticalAlignment::Center);
            muxc::Grid values;
            values.ColumnSpacing(kSettingControlGap);
            muxc::ColumnDefinition flexible;
            flexible.Width(mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star));
            values.ColumnDefinitions().Append(flexible);
            values.Children().Append(slider);
            AppendEditor(values, number);
            AppendEditor(values, unit);
            muxc::Button reset;
            const auto editors = EditorWithReset(values, reset);

            mux::DispatcherTimer idle;
            idle.Interval(std::chrono::milliseconds(250));
            const auto commit = [idle, slider, set] {
                if (!idle.IsEnabled()) return;
                idle.Stop();
                if (slider.IsEnabled()) set(static_cast<float>(slider.Value() / 100));
            };
            const auto tick = idle.Tick([commit](const auto&, const auto&) { commit(); });
            const auto changed = slider.ValueChanged([this, slider, number, idle](const auto&, const auto&) {
                if (syncing_ || !slider.IsEnabled()) return;
                syncing_ = true;
                SetNumberBoxValue(number, slider.Value());
                syncing_ = false;
                idle.Stop();
                idle.Start();
            });
            const auto captureLost = slider.PointerCaptureLost([commit](const auto&, const auto&) { commit(); });
            const auto lostFocus = slider.LostFocus([commit](const auto&, const auto&) { commit(); });
            const auto typed = number.ValueChanged([this, slider, number, minimum, maximum, step](const auto&, const auto&) {
                if (!syncing_ && number.IsEnabled() && std::isfinite(number.Value()))
                    slider.Value(QuantizeNumericValue(number.Value(), minimum, maximum, step));
            });
            const auto restore = reset.Click([slider, defaultValue, commit](const auto&, const auto&) {
                slider.Value(defaultValue);
                commit();
            });
            revoke_.emplace_back([idle, slider, number, reset, tick, changed, captureLost, lostFocus, typed, restore] {
                idle.Stop();
                idle.Tick(tick);
                slider.ValueChanged(changed);
                slider.PointerCaptureLost(captureLost);
                slider.LostFocus(lostFocus);
                number.ValueChanged(typed);
                reset.Click(restore);
            });
            return FieldEditor<float>{editors, [this, slider, number, reset, idle, minimum, maximum, step, defaultValue](
                float value, bool enabled, const std::wstring& label) {
                slider.IsEnabled(enabled);
                number.IsEnabled(enabled);
                if (!enabled) idle.Stop();
                const auto percent = QuantizeNumericValue(value * 100., minimum, maximum, step);
                if (!idle.IsEnabled())
                {
                    slider.Value(percent);
                    SyncNumberBoxValue(number, percent);
                }
                muxa::AutomationProperties::SetName(slider, label);
                muxa::AutomationProperties::SetName(number, label);
                ConfigureRestoreDefaultButton(reset, label + L" · " + localize_("app.settings.restore_default"));
                reset.IsEnabled(enabled && percent != defaultValue);
            }};
        });
    }

    void Update(const Override& last, const Override& other, const Layout& base,
        const Layout& lastEffective, const Layout& otherEffective)
    {
        groups_[0].value = last;
        groups_[0].effective = lastEffective;
        groups_[1].value = other;
        groups_[1].effective = otherEffective;
        base_ = base;
        Refresh();
    }

    void Refresh()
    {
        using namespace presenter_controls;
        syncing_ = true;
        const bool editable = RefreshAccess();
        heading_.Text(localize_("settings.bar.monitorOverrides"));
        muxa::AutomationProperties::SetName(section_, heading_.Text());
        for (auto& group : groups_)
        {
            const auto title = localize_(group.role == BarMonitorRole::Last ?
                "settings.bar.monitorOverrides.last" : "settings.bar.monitorOverrides.other");
            const auto state = localize_(group.value == Override{} ?
                "settings.bar.monitorOverrides.follow" : "settings.bar.monitorOverrides.custom");
            group.heading.Text(title + L"  ·  " + state);
            muxa::AutomationProperties::SetName(group.expander, group.heading.Text());
            group.reset.Content(winrt::box_value(localize_("settings.bar.monitorOverrides.reset")));
            group.resetRow.SetText(L"");
            StyleSettingsSubrow(group.resetRow.root, !editable);
            for (auto& display : group.refresh) display(group.value, group.effective, editable);
            UpdateSettingsSubrows(group.fields);
        }
        syncing_ = false;
    }

private:
    template <typename T>
    struct FieldEditor
    {
        presenter_controls::mux::FrameworkElement content;
        std::function<void(T, bool, const std::wstring&)> refresh;
    };

    template <typename T, typename Create>
    void AddField(const char* key, std::optional<T> Override::* member, T Layout::* baseMember,
        const std::function<bool(const Layout&)>& applicable, Create create)
    {
        using namespace presenter_controls;
        for (auto& group : groups_)
        {
            const auto apply = [this, role = group.role, member](std::optional<T> value) {
                if (syncing_ || !CanEdit()) return;
                apply_(role, [member, value](Override& row) { row.*member = value; });
            };
            const auto editor = create(apply);
            muxc::CheckBox follow;
            follow.MinWidth(0);
            follow.VerticalAlignment(mux::VerticalAlignment::Center);
            muxc::TextBlock followLabel;
            followLabel.VerticalAlignment(mux::VerticalAlignment::Center);
            muxc::Grid controls;
            controls.ColumnSpacing(kSettingControlGap);
            AppendEditor(controls, followLabel);
            AppendEditor(controls, follow);
            muxc::ColumnDefinition editorColumn;
            editorColumn.Width(mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star));
            muxc::Grid::SetColumn(editor.content, 2);
            controls.ColumnDefinitions().Append(editorColumn);
            controls.Children().Append(editor.content);
            SettingRow row;
            row.Initialize(controls, kSettingControlWidth + 108);
            group.fields.Children().Append(row.root);

            const auto click = follow.Click([this, follow, baseMember, apply](const auto&, const auto&) {
                apply(follow.IsChecked().Value() ? std::nullopt : std::optional<T>{base_.*baseMember});
            });
            revoke_.emplace_back([follow, click] { follow.Click(click); });
            group.refresh.emplace_back([this, row, follow, followLabel, editor, key, member, baseMember, applicable](
                const Override& value, const Layout& effective, bool editable) mutable {
                const auto label = localize_(key);
                const bool inherits = !(value.*member);
                row.SetText(label);
                row.root.Visibility(!applicable || applicable(effective) ? mux::Visibility::Visible : mux::Visibility::Collapsed);
                followLabel.Text(localize_("settings.bar.monitorOverrides.followToggle"));
                muxa::AutomationProperties::SetName(follow, label + L" · " + localize_("settings.bar.monitorOverrides.follow"));
                follow.IsEnabled(editable);
                follow.IsChecked(inherits);
                editor.refresh(effective.*baseMember, editable && !inherits, label);
            });
        }
    }

    static void AppendEditor(const presenter_controls::muxc::Grid& grid,
        const presenter_controls::mux::FrameworkElement& editor)
    {
        using namespace presenter_controls;
        muxc::ColumnDefinition column;
        column.Width(mux::GridLengthHelper::Auto());
        muxc::Grid::SetColumn(editor, static_cast<int>(grid.ColumnDefinitions().Size()));
        grid.ColumnDefinitions().Append(column);
        grid.Children().Append(editor);
    }

    static presenter_controls::muxc::Grid EditorWithReset(
        const presenter_controls::mux::FrameworkElement& control, const presenter_controls::muxc::Button& reset)
    {
        using namespace presenter_controls;
        muxc::Grid editors;
        editors.ColumnSpacing(kSettingControlGap);
        muxc::ColumnDefinition flexible;
        flexible.Width(mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star));
        editors.ColumnDefinitions().Append(flexible);
        editors.Children().Append(control);
        reset.VerticalAlignment(mux::VerticalAlignment::Center);
        AppendEditor(editors, reset);
        return editors;
    }

    static void InitializeExpander(const presenter_controls::muxc::Expander& expander,
        const presenter_controls::muxc::TextBlock& header, const presenter_controls::muxc::StackPanel& body)
    {
        using namespace presenter_controls;
        header.TextWrapping(mux::TextWrapping::Wrap);
        expander.Header(header);
        expander.Content(body);
        expander.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        expander.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        expander.Padding({0});
        expander.IsExpanded(false);
        const auto weakBody = winrt::make_weak(body);
        expander.SizeChanged([weakBody](const auto&, const mux::SizeChangedEventArgs& args) {
            if (const auto panel = weakBody.get())
                panel.Width(std::max(0.0, static_cast<double>(args.NewSize().Width)));
        });
    }

    bool CanEdit() const
    {
        const auto status = actions_.advancedFeatureStatus ? actions_.advancedFeatureStatus() : GeneralAdvancedFeatureStatus{};
        return status.bridgeAvailable && status.registered;
    }

    bool RefreshAccess()
    {
        using presenter_controls::mux::Visibility;
        const auto status = actions_.advancedFeatureStatus ? actions_.advancedFeatureStatus() : GeneralAdvancedFeatureStatus{};
        available_ = status.bridgeAvailable;
        const bool editable = available_ && status.registered;
        bool visible = available_;
        for (auto& group : groups_)
        {
            const bool custom = group.value != Override{};
            visible |= custom;
            group.expander.Visibility(available_ || custom ? Visibility::Visible : Visibility::Collapsed);
            group.fields.Visibility(editable ? Visibility::Visible : Visibility::Collapsed);
            group.resetRow.root.Visibility(custom ? Visibility::Visible : Visibility::Collapsed);
        }
        section_.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
        return editable;
    }

    struct Group
    {
        BarMonitorRole role;
        presenter_controls::muxc::Expander expander;
        presenter_controls::muxc::TextBlock heading;
        presenter_controls::muxc::StackPanel body, fields;
        presenter_controls::muxc::Button reset;
        presenter_controls::SettingRow resetRow;
        Override value;
        Layout effective;
        std::vector<std::function<void(const Override&, const Layout&, bool)>> refresh;
    };

    Localize localize_;
    const DockPageActions& actions_;
    Apply apply_;
    presenter_controls::muxc::Border root_{nullptr};
    presenter_controls::muxc::Expander section_;
    presenter_controls::muxc::StackPanel groupsPanel_;
    presenter_controls::muxc::TextBlock heading_;
    std::array<Group, 2> groups_{{{BarMonitorRole::Last}, {BarMonitorRole::Other}}};
    std::vector<std::function<void()>> revoke_;
    Layout base_;
    bool syncing_ = false, available_ = false;
};
}

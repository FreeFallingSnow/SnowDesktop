#pragma once

#include "dock_page_actions.h"
#include "dock/dock_layout_settings.h"
#include "settings_presenter_controls.h"

#include <array>
#include <cmath>
#include <functional>
#include <memory>
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
        heading_.Margin({16, 12, 16, 6});
        root_.Children().Append(heading_);
        for (auto& group : groups_)
        {
            group.expander.Content(group.body);
            group.body.Children().Append(group.fields);
            group.resetRow.Initialize(group.reset);
            group.body.Children().Append(group.resetRow.root);
            root_.Children().Append(presenter_controls::SettingsDisclosureRow(group.expander));
            const auto reset = group.reset.Click([this, role = group.role](const auto&, const auto&) {
                apply_(role, [](Override& value) { value = {}; });
            });
            revoke_.emplace_back([control = group.reset, reset] { control.Click(reset); });
            const auto expanding = group.expander.Expanding([this, role = group.role](const auto&, const auto&) {
                auto& current = groups_[role == BarMonitorRole::Last ? 0 : 1];
                RefreshAccess();
                if (!editable_ && current.value == Override{} && available_)
                {
                    current.expander.IsExpanded(false);
                    if (actions_.openAdvancedFeatureUnlock) actions_.openAdvancedFeatureUnlock();
                }
            });
            revoke_.emplace_back([control = group.expander, expanding] { control.Expanding(expanding); });
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
        std::function<bool(const Layout&)> applicable = {})
    {
        using namespace presenter_controls;
        for (auto& group : groups_)
        {
            muxc::ComboBox combo;
            combo.MinWidth(136);
            SettingRow row;
            row.Initialize(combo);
            row.SetControlAlignment(mux::HorizontalAlignment::Right);
            group.fields.Children().Append(row.root);
            group.refresh.emplace_back([this, row, combo, key, member, baseMember, choices, applicable](
                const Override& value, const Layout& base, const Layout& effective) mutable {
                const auto choiceLabel = [&](T requested) {
                    for (const auto& [choice, label] : choices)
                        if (choice == requested) return localize_(label);
                    return std::wstring{};
                };
                const auto label = localize_(key);
                row.SetText(label);
                winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(combo, label);
                row.root.Visibility(!applicable || applicable(effective) ? mux::Visibility::Visible : mux::Visibility::Collapsed);
                combo.Items().Clear();
                combo.Items().Append(winrt::box_value(localize_("settings.bar.monitorOverrides.follow") +
                    L" (" + choiceLabel(base.*baseMember) + L")"));
                int selected = 0;
                for (const auto& [choice, text] : choices)
                {
                    combo.Items().Append(winrt::box_value(localize_(text)));
                    if (value.*member == choice) selected = static_cast<int>(combo.Items().Size()) - 1;
                }
                combo.SelectedIndex(selected);
            });
            const auto selected = combo.SelectionChanged([this, role = group.role, member, choices](const auto& sender, const auto&) {
                const int index = sender.template as<muxc::ComboBox>().SelectedIndex();
                if (syncing_ || index < 0 || !CanEdit()) return;
                const auto value = index == 0 ? std::optional<T>{} : choices[static_cast<size_t>(index - 1)].first;
                apply_(role, [member, value](Override& row) { row.*member = value; });
            });
            revoke_.emplace_back([combo, selected] { combo.SelectionChanged(selected); });
        }
    }

    void AddToggle(const char* key, std::optional<bool> Override::* member, bool Layout::* baseMember)
    {
        AddChoice<bool>(key, member, baseMember, {{false, "app.interact.off"}, {true, "app.interact.on"}});
    }

    void AddPercentage(const char* key, std::optional<float> Override::* member,
        float Layout::* baseMember, double minimum, double maximum, bool appendUnit = true,
        std::function<bool(const Layout&)> applicable = {})
    {
        using namespace presenter_controls;
        for (auto& group : groups_)
        {
            muxc::NumberBox number;
            number.MinWidth(136);
            number.Minimum(minimum);
            number.Maximum(maximum);
            number.SmallChange(1);
            number.SpinButtonPlacementMode(muxc::NumberBoxSpinButtonPlacementMode::Compact);
            SettingRow row;
            row.Initialize(number);
            row.SetControlAlignment(mux::HorizontalAlignment::Right);
            group.fields.Children().Append(row.root);
            group.refresh.emplace_back([this, row, number, key, member, baseMember, appendUnit, applicable](
                const Override& value, const Layout& base, const Layout& effective) mutable {
                const auto label = localize_(key) + (appendUnit ? L" (%)" : L"");
                row.root.Visibility(!applicable || applicable(effective) ? mux::Visibility::Visible : mux::Visibility::Collapsed);
                row.SetText(label);
                winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(number, label);
                number.PlaceholderText(localize_("settings.bar.monitorOverrides.follow") +
                    L" (" + std::to_wstring(std::lround((base.*baseMember) * 100)) + L"%)");
                number.Value(value.*member ? std::round(*(value.*member) * 100.) : std::nan(""));
            });
            const auto changed = number.ValueChanged([this, role = group.role, member](const auto&, const auto& args) {
                if (syncing_ || !CanEdit()) return;
                const double input = args.NewValue();
                const auto value = std::isfinite(input) ? std::optional{static_cast<float>(input / 100.)} : std::nullopt;
                apply_(role, [member, value](Override& row) { row.*member = value; });
            });
            revoke_.emplace_back([number, changed] { number.ValueChanged(changed); });
        }
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
        syncing_ = true;
        heading_.Text(localize_("settings.bar.monitorOverrides"));
        for (auto& group : groups_)
        {
            const auto title = localize_(group.role == BarMonitorRole::Last ?
                "settings.bar.monitorOverrides.last" : "settings.bar.monitorOverrides.other");
            const auto state = localize_(group.value == Override{} ?
                "settings.bar.monitorOverrides.follow" : "settings.bar.monitorOverrides.custom");
            group.expander.Header(winrt::box_value(title + L"  ·  " + state));
            const auto reset = localize_("settings.bar.monitorOverrides.reset");
            group.reset.Content(winrt::box_value(reset));
            group.resetRow.SetText(L"");
            presenter_controls::StyleSettingsSubrow(group.resetRow.root, false);
            for (auto& display : group.refresh) display(group.value, base_, group.effective);
            presenter_controls::UpdateSettingsSubrows(group.fields);
        }
        RefreshAccess();
        syncing_ = false;
    }

private:
    bool CanEdit() const
    {
        const auto status = actions_.advancedFeatureStatus ? actions_.advancedFeatureStatus() : GeneralAdvancedFeatureStatus{};
        return status.bridgeAvailable && status.registered;
    }

    void RefreshAccess()
    {
        using presenter_controls::mux::Visibility;
        const auto status = actions_.advancedFeatureStatus ? actions_.advancedFeatureStatus() : GeneralAdvancedFeatureStatus{};
        available_ = status.bridgeAvailable;
        editable_ = available_ && status.registered;
        bool visible = available_;
        for (auto& group : groups_)
        {
            const bool custom = group.value != Override{};
            visible |= custom;
            group.expander.Visibility(available_ || custom ? Visibility::Visible : Visibility::Collapsed);
            group.fields.Visibility(editable_ ? Visibility::Visible : Visibility::Collapsed);
            group.resetRow.root.Visibility(custom ? Visibility::Visible : Visibility::Collapsed);
        }
        root_.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
    }

    struct Group
    {
        BarMonitorRole role;
        presenter_controls::muxc::Expander expander;
        presenter_controls::muxc::StackPanel body, fields;
        presenter_controls::muxc::Button reset;
        presenter_controls::SettingRow resetRow;
        Override value;
        Layout effective;
        std::vector<std::function<void(const Override&, const Layout&, const Layout&)>> refresh;
    };

    Localize localize_;
    const DockPageActions& actions_;
    Apply apply_;
    presenter_controls::muxc::StackPanel root_;
    presenter_controls::muxc::TextBlock heading_;
    std::array<Group, 2> groups_{{{BarMonitorRole::Last}, {BarMonitorRole::Other}}};
    std::vector<std::function<void()>> revoke_;
    Layout base_;
    bool syncing_ = false, available_ = false, editable_ = false;
};
}

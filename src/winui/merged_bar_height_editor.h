#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include "dock_page_presenter.h"
#include "settings_presenter_controls.h"
#include "../bar_settings_rules.h"

namespace snowdesktop::winui
{
// Every entry point edits the same layout field. Only settled input crosses
// into AppBar placement, so dragging cannot create a work-area feedback loop.
class MergedBarHeightEditor
{
    using Localize = DockPagePresenter::LocalizeCallback;
    using Row = presenter_controls::SettingRow;
    Localize localize_;
    DockPageActions actions_;
    Row row_;
    winrt::Microsoft::UI::Xaml::Controls::Slider slider_;
    winrt::Microsoft::UI::Xaml::Controls::NumberBox number_;
    winrt::Microsoft::UI::Xaml::Controls::Button reset_;
    winrt::Microsoft::UI::Xaml::DispatcherTimer timer_;
    winrt::event_token sliderToken_{}, numberToken_{}, resetToken_{}, timerToken_{};
    std::uint64_t generation_ = 0;
    bool syncing_ = false, dirty_ = false, closed_ = false;
    int value_ = 48;
public:
    explicit MergedBarHeightEditor(Localize localize) : localize_(std::move(localize))
    {
        namespace x = winrt::Microsoft::UI::Xaml;
        namespace c = x::Controls;
        c::Grid grid; grid.ColumnSpacing(8);
        for (int i = 0; i < 3; ++i)
        { c::ColumnDefinition column; column.Width(i == 0 ? x::GridLengthHelper::FromValueAndType(1, x::GridUnitType::Star) : x::GridLengthHelper::Auto()); grid.ColumnDefinitions().Append(column); }
        slider_.Minimum(32); slider_.Maximum(96); slider_.StepFrequency(1);
        number_.Minimum(32); number_.Maximum(96); number_.Width(90);
        number_.SpinButtonPlacementMode(c::NumberBoxSpinButtonPlacementMode::Compact);
        c::Grid::SetColumn(number_, 1); c::Grid::SetColumn(reset_, 2);
        grid.Children().Append(slider_); grid.Children().Append(number_); grid.Children().Append(reset_);
        row_.Initialize(grid);
        timer_.Interval(std::chrono::milliseconds(300));
        sliderToken_ = slider_.ValueChanged([this](const auto&, const auto&) { Change(slider_.Value()); });
        numberToken_ = number_.ValueChanged([this](const auto&, const auto&) { Change(number_.Value()); });
        resetToken_ = reset_.Click([this](const auto&, const auto&) { Change(48); Flush(); });
        timerToken_ = timer_.Tick([this](const auto&, const auto&) { Flush(); });
        RefreshLocalizedText();
    }
    ~MergedBarHeightEditor() { Close(); }
    auto Content() const { return row_.root; }
    auto FocusTarget() const { return slider_; }
    void SetActions(DockPageActions actions) { actions_ = std::move(actions); }
    void RefreshLocalizedText()
    {
        row_.SetText(localize_("settings.bars.height"), localize_("settings.bars.height.description"));
        reset_.Content(winrt::box_value(localize_("settings.bars.resetHeight")));
        winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(slider_, row_.label.Text());
        winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(number_, row_.label.Text());
    }
    void Update(const SettingsSnapshot& snapshot)
    {
        if (closed_) return;
        if (generation_ != snapshot.generation) { timer_.Stop(); dirty_ = false; }
        generation_ = snapshot.generation;
        const auto state = ResolveBarSettingsAvailability(snapshot.values.general.dockEnabled,
            snapshot.values.dock, snapshot.values.general.statusBar, GetSystemMetrics(SM_CMONITORS));
        row_.root.Visibility(state.anyMerged ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        if (!state.anyMerged) { timer_.Stop(); dirty_ = false; }
        if (!dirty_) SetValue(std::clamp(snapshot.values.dock.mergedBarHeight, 32, 96));
    }
    void Flush()
    {
        timer_.Stop();
        if (!dirty_ || closed_ || !generation_ || !actions_.updateDock) return;
        dirty_ = false;
        const int height = value_;
        actions_.updateDock(generation_, SettingsUpdateMode::PreviewAndCommit,
            [height](DockSettings& dock) { dock.mergedBarHeight = height; });
    }
    void Close() noexcept
    {
        if (closed_) return;
        closed_ = true;
        try { timer_.Stop(); timer_.Tick(timerToken_); slider_.ValueChanged(sliderToken_);
            number_.ValueChanged(numberToken_); reset_.Click(resetToken_); } catch (...) {}
    }
private:
    void SetValue(int value)
    {
        syncing_ = true; value_ = value; slider_.Value(value); number_.Value(value);
        reset_.IsEnabled(value != 48); syncing_ = false;
    }
    void Change(double value)
    {
        if (syncing_ || closed_ || !std::isfinite(value)) return;
        const int next = static_cast<int>(std::clamp(std::round(value), 32., 96.));
        if (next == value_) return;
        SetValue(next); dirty_ = true; timer_.Stop(); timer_.Start();
    }
};
}

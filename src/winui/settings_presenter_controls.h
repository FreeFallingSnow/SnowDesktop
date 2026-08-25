#pragma once

#include "../settings_controller.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <string>
#include <utility>

namespace snowdesktop::winui
{
namespace presenter_controls
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxa = winrt::Microsoft::UI::Xaml::Automation;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace muxi = winrt::Microsoft::UI::Xaml::Input;
namespace muxm = winrt::Microsoft::UI::Xaml::Media;

inline constexpr double kSettingControlWidth = 300.0;
inline constexpr double kSettingRowStackThreshold = 700.0;
inline constexpr std::chrono::milliseconds kContinuousPreviewInterval{33};

/**
 * Applies the compact Fluent treatment used by low-risk restore-default
 * actions. The visible affordance is intentionally icon-only, while the
 * localized action remains available to tooltips and accessibility clients.
 * Destructive or broad reset actions must keep their explicit text labels.
 */
inline void ConfigureRestoreDefaultButton(
    const muxc::Button& button,
    const std::wstring& accessibleText)
{
    if (!button)
        return;
    muxc::FontIcon icon{};
    icon.Glyph(L"\xE777");
    icon.FontSize(15.0);
    button.Content(icon);
    button.Width(32.0);
    button.Height(32.0);
    button.MinWidth(32.0);
    button.MinHeight(32.0);
    button.Padding({0.0, 0.0, 0.0, 0.0});
    button.VerticalContentAlignment(mux::VerticalAlignment::Center);
    button.HorizontalContentAlignment(mux::HorizontalAlignment::Center);
    muxc::ToolTipService::SetToolTip(
        button, winrt::box_value(accessibleText));
    muxa::AutomationProperties::SetName(button, accessibleText);
    muxa::AutomationProperties::SetHelpText(button, accessibleText);
}

/** Remove persisted float noise before a value reaches Slider/NumberBox. */
inline double QuantizeNumericValue(
    double value,
    double minimum,
    double maximum,
    double step) noexcept
{
    value = std::clamp(value, minimum, maximum);
    if (!std::isfinite(step) || step <= 0.0)
        return value;
    const double ticks = std::round((value - minimum) / step);
    return std::clamp(minimum + ticks * step, minimum, maximum);
}

/**
 * Keeps the native control responsive while limiting expensive host previews
 * to the last value observed in a display-sized interval.
 */
template <typename Value>
struct CoalescedPreviewTimer
{
    using PublishCallback = std::function<void(const Value&)>;

    mux::DispatcherTimer timer{nullptr};
    PublishCallback publish;
    Value latest{};
    bool pending = false;
    bool closed = false;
    winrt::event_token tickToken{};

    void Initialize(PublishCallback callback)
    {
        publish = std::move(callback);
        timer = mux::DispatcherTimer{};
        timer.Interval(kContinuousPreviewInterval);
        tickToken = timer.Tick([this](const auto&, const auto&) {
            timer.Stop();
            Flush();
        });
    }

    void Queue(const Value& value)
    {
        if (closed) return;
        latest = value;
        pending = true;
        if (!timer.IsEnabled())
            timer.Start();
    }

    void Flush()
    {
        if (closed || !pending) return;
        pending = false;
        if (publish)
            publish(latest);
    }

    void Cancel() noexcept
    {
        pending = false;
        try
        {
            if (timer) timer.Stop();
        }
        catch (...)
        {
        }
    }

    void Close() noexcept
    {
        if (closed) return;
        Cancel();
        closed = true;
        try
        {
            if (timer) timer.Tick(tickToken);
        }
        catch (...)
        {
        }
        publish = {};
    }
};

/**
 * Programmatic equivalent of the legacy BeginSettingRow contract.
 *
 * The label/help column stays on the left and a fixed-width editor stays on
 * the right while enough content width is available. Below the responsive
 * threshold the editor moves below the text so localized labels and controls
 * do not compete for the same row. Compact controls keep their requested
 * alignment; full-width editors stretch inside the available editor area.
 */
struct SettingRow
{
    muxc::Grid root{nullptr};
    muxc::StackPanel text{nullptr};
    muxc::TextBlock label{nullptr};
    muxc::TextBlock help{nullptr};
    muxc::ContentControl controlHost{nullptr};

    void Initialize(
        const mux::UIElement& control,
        double controlWidth = kSettingControlWidth)
    {
        root = muxc::Grid{};
        root.ColumnSpacing(20.0);
        root.HorizontalAlignment(mux::HorizontalAlignment::Stretch);

        muxc::ColumnDefinition labelColumn{};
        labelColumn.Width(mux::GridLengthHelper::FromValueAndType(
            1.0, mux::GridUnitType::Star));
        muxc::ColumnDefinition controlColumn{};
        controlColumn.Width(mux::GridLengthHelper::FromValueAndType(
            controlWidth, mux::GridUnitType::Pixel));
        root.ColumnDefinitions().Append(labelColumn);
        root.ColumnDefinitions().Append(controlColumn);
        muxc::RowDefinition labelRow{};
        labelRow.Height(mux::GridLengthHelper::Auto());
        root.RowDefinitions().Append(labelRow);
        muxc::RowDefinition controlRow{};
        controlRow.Height(mux::GridLengthHelper::Auto());
        root.RowDefinitions().Append(controlRow);

        text = muxc::StackPanel{};
        text.Spacing(3.0);
        text.VerticalAlignment(mux::VerticalAlignment::Center);
        label = muxc::TextBlock{};
        label.TextWrapping(mux::TextWrapping::Wrap);
        help = muxc::TextBlock{};
        help.TextWrapping(mux::TextWrapping::Wrap);
        help.Opacity(0.68);
        help.Visibility(mux::Visibility::Collapsed);
        text.Children().Append(label);
        text.Children().Append(help);

        controlHost = muxc::ContentControl{};
        controlHost.Content(control);
        controlHost.HorizontalContentAlignment(
            mux::HorizontalAlignment::Stretch);
        controlHost.VerticalContentAlignment(
            mux::VerticalAlignment::Center);

        muxc::Grid::SetColumn(text, 0);
        muxc::Grid::SetColumn(controlHost, 1);
        root.Children().Append(text);
        root.Children().Append(controlHost);

        // SettingRow is also used as a short-lived builder in several
        // presenters, while its XAML elements remain in the visual tree.
        // Capture weak projected objects rather than `this` so responsive
        // updates remain safe after the builder object goes out of scope.
        const auto weakControlColumn = winrt::make_weak(controlColumn);
        const auto weakText = winrt::make_weak(text);
        const auto weakControlHost = winrt::make_weak(controlHost);
        root.SizeChanged(
            [weakControlColumn, weakText, weakControlHost, controlWidth](
                const auto& sender,
                const mux::SizeChangedEventArgs& args) {
                const auto grid = sender.template try_as<muxc::Grid>();
                const auto responsiveControlColumn = weakControlColumn.get();
                const auto responsiveText = weakText.get();
                const auto responsiveControlHost = weakControlHost.get();
                if (!grid || !responsiveControlColumn || !responsiveText ||
                    !responsiveControlHost)
                {
                    return;
                }

                const bool stacked =
                    args.NewSize().Width < kSettingRowStackThreshold;
                responsiveControlColumn.Width(
                    mux::GridLengthHelper::FromValueAndType(
                        stacked ? 0.0 : controlWidth,
                        mux::GridUnitType::Pixel));
                grid.ColumnSpacing(stacked ? 0.0 : 20.0);
                grid.RowSpacing(stacked ? 10.0 : 0.0);
                muxc::Grid::SetColumn(responsiveText, 0);
                muxc::Grid::SetRow(responsiveText, 0);
                muxc::Grid::SetColumn(responsiveControlHost,
                    stacked ? 0 : 1);
                muxc::Grid::SetRow(responsiveControlHost,
                    stacked ? 1 : 0);
            });
    }

    void SetControlAlignment(mux::HorizontalAlignment alignment)
    {
        // ContentControl can still occupy the full editor column even when
        // its ContentPresenter aligns a compact child.  Shrink and align the
        // host as well so ToggleSwitch and other compact templates actually
        // meet the right edge shared by full-width editors.
        controlHost.HorizontalAlignment(alignment);
        controlHost.HorizontalContentAlignment(alignment);
        if (alignment == mux::HorizontalAlignment::Right)
        {
            // WinUI reserves 154 DIPs for ToggleSwitch by default while its
            // visible track/content remain left-aligned inside that width.
            // Remove only that reserved minimum so the intrinsic switch
            // group, including localized On/Off content, reaches the edge.
            if (const auto toggle =
                    controlHost.Content().try_as<muxc::ToggleSwitch>())
            {
                toggle.MinWidth(0.0);
            }
        }
    }

    void SetText(std::wstring labelText, std::wstring helpText = {})
    {
        label.Text(std::move(labelText));
        help.Text(std::move(helpText));
        help.Visibility(help.Text().empty()
                ? mux::Visibility::Collapsed
                : mux::Visibility::Visible);
        if (controlHost)
        {
            muxa::AutomationProperties::SetName(controlHost, label.Text());
            muxa::AutomationProperties::SetHelpText(controlHost, help.Text());
        }
    }

    void SetEnabled(bool enabled)
    {
        // Grid is a FrameworkElement rather than a Control.  Disable the
        // ContentControl host so every interactive editor below it leaves the
        // tab order and rejects pointer/keyboard input as one unit.
        controlHost.IsEnabled(enabled);
        root.Opacity(enabled ? 1.0 : 0.55);
    }
};

/** Compact color swatch whose full ColorPicker exists only inside a flyout. */
struct ColorFlyoutEditor
{
    enum class EditState
    {
        Inactive,
        Committed,
        PendingPreview,
        RolledBack,
    };

    using ChangedCallback = std::function<void(
        const winrt::Windows::UI::Color&, SettingsUpdateMode)>;

    SettingRow row;
    muxc::Button button{nullptr};
    muxc::Border swatch{nullptr};
    muxc::Flyout flyout{nullptr};
    muxc::ColorPicker picker{nullptr};
    muxc::Button cancel{nullptr};
    CoalescedPreviewTimer<winrt::Windows::UI::Color> preview;
    ChangedCallback changed;
    winrt::Windows::UI::Color original{};
    bool updating = false;
    bool open = false;
    bool canceled = false;
    bool rollbackApplied = false;
    bool closed = false;
    EditState editState = EditState::Inactive;

    winrt::event_token openingToken{};
    winrt::event_token closedToken{};
    winrt::event_token colorToken{};
    winrt::event_token pointerReleasedToken{};
    winrt::event_token lostFocusToken{};
    winrt::event_token keyDownToken{};
    winrt::event_token cancelToken{};

    void Initialize(ChangedCallback callback)
    {
        changed = std::move(callback);
        button = muxc::Button{};
        button.Width(44.0);
        button.Height(34.0);
        button.Padding({5.0, 5.0, 5.0, 5.0});
        button.HorizontalAlignment(mux::HorizontalAlignment::Right);
        button.UseSystemFocusVisuals(true);
        swatch = muxc::Border{};
        swatch.Width(28.0);
        swatch.Height(18.0);
        swatch.CornerRadius({3.0, 3.0, 3.0, 3.0});
        button.Content(swatch);

        picker = muxc::ColorPicker{};
        picker.IsAlphaEnabled(false);
        picker.IsAlphaSliderVisible(false);
        picker.IsAlphaTextInputVisible(false);
        picker.IsMoreButtonVisible(false);

        cancel = muxc::Button{};
        muxc::StackPanel actions{};
        actions.Orientation(muxc::Orientation::Horizontal);
        actions.HorizontalAlignment(mux::HorizontalAlignment::Right);
        actions.Spacing(8.0);
        actions.Children().Append(cancel);

        muxc::StackPanel panel{};
        panel.Spacing(10.0);
        panel.Children().Append(picker);
        panel.Children().Append(actions);
        flyout = muxc::Flyout{};
        flyout.Content(panel);
        button.Flyout(flyout);
        row.Initialize(button);
        row.SetControlAlignment(mux::HorizontalAlignment::Right);

        preview.Initialize([this](const auto& color) {
            if (changed)
                changed(color, SettingsUpdateMode::Preview);
        });

        openingToken = flyout.Opening([this](const auto&, const auto&) {
            if (closed) return;
            original = picker.Color();
            open = true;
            canceled = false;
            rollbackApplied = false;
            // The opening value is already authoritative. A later change
            // becomes PendingPreview until one of the continuous-control
            // commit boundaries accepts it.
            editState = EditState::Committed;
        });
        colorToken = picker.ColorChanged([this](const auto&, const auto&) {
            if (closed || updating || !open) return;
            UpdateSwatch();
            editState = EditState::PendingPreview;
            preview.Queue(picker.Color());
        });
        pointerReleasedToken = picker.PointerReleased(
            [this](const auto&, const auto&) { Commit(); });
        lostFocusToken = picker.LostFocus(
            [this](const auto&, const auto&) { Commit(); });
        keyDownToken = picker.KeyDown(
            [this](const auto&, const muxi::KeyRoutedEventArgs& args) {
                if (args.Key() == winrt::Windows::System::VirtualKey::Enter)
                    Commit();
            });
        cancelToken = cancel.Click([this](const auto&, const auto&) {
            if (closed || !open) return;
            canceled = true;
            Rollback();
            flyout.Hide();
        });
        closedToken = flyout.Closed([this](const auto&, const auto&) {
            if (closed) return;
            // Light-dismiss accepts the latest visible color. Cancel is the
            // only explicit rollback path.
            if (!canceled)
                Commit();
            open = false;
            editState = EditState::Inactive;
        });
        UpdateSwatch();
    }

    void SetText(
        std::wstring labelText,
        std::wstring helpText,
        std::wstring cancelText)
    {
        row.SetText(std::move(labelText), std::move(helpText));
        cancel.Content(winrt::box_value(std::move(cancelText)));
        muxa::AutomationProperties::SetName(button, row.label.Text());
        muxa::AutomationProperties::SetHelpText(button, row.help.Text());
    }

    void SetColor(const winrt::Windows::UI::Color& color)
    {
        if (closed) return;
        // ColorChanged previews are echoed through the controller snapshot.
        // Do not write that echo into the live ColorPicker: assigning Color
        // while its flyout owns focus causes WinUI to light-dismiss the flyout
        // and ends the drag after the first channel change.
        if (open) return;
        const bool previous = updating;
        updating = true;
        picker.Color(color);
        updating = previous;
        if (!open)
            original = color;
        UpdateSwatch();
    }

    [[nodiscard]] winrt::Windows::UI::Color Color() const
    {
        return picker.Color();
    }

    [[nodiscard]] bool IsOpen() const noexcept
    {
        return open;
    }

    void SetEnabled(bool enabled)
    {
        row.SetEnabled(enabled);
        button.IsEnabled(enabled);
    }

    void Commit()
    {
        if (closed || !open || editState != EditState::PendingPreview)
            return;
        preview.Cancel();
        if (changed)
            changed(picker.Color(), SettingsUpdateMode::PreviewAndCommit);
        editState = EditState::Committed;
    }

    void Rollback()
    {
        if (closed || !open || rollbackApplied)
            return;
        preview.Cancel();
        rollbackApplied = true;
        const bool previous = updating;
        updating = true;
        picker.Color(original);
        updating = previous;
        UpdateSwatch();
        if (changed)
            changed(original, SettingsUpdateMode::PreviewAndCommit);
        editState = EditState::RolledBack;
    }

    void UpdateSwatch()
    {
        if (!swatch || !picker) return;
        swatch.Background(muxm::SolidColorBrush(picker.Color()));
    }

    void Dismiss() noexcept
    {
        if (!open || !flyout) return;
        try
        {
            // Flyout::Hide completes asynchronously. Commit while the owning
            // presenter and its generation guard are still authoritative.
            if (!canceled)
                Commit();
            flyout.Hide();
        }
        catch (...)
        {
            try
            {
                if (!canceled)
                    Commit();
            }
            catch (...)
            {
            }
            open = false;
            editState = EditState::Inactive;
        }
    }

    void Close() noexcept
    {
        if (closed) return;
        try
        {
            if (open && !canceled)
                Commit();
        }
        catch (...)
        {
        }
        open = false;
        editState = EditState::Inactive;
        closed = true;
        preview.Close();
        try
        {
            flyout.Opening(openingToken);
            flyout.Closed(closedToken);
            picker.ColorChanged(colorToken);
            picker.PointerReleased(pointerReleasedToken);
            picker.LostFocus(lostFocusToken);
            picker.KeyDown(keyDownToken);
            cancel.Click(cancelToken);
        }
        catch (...)
        {
        }
        changed = {};
    }
};

} // namespace presenter_controls
} // namespace snowdesktop::winui

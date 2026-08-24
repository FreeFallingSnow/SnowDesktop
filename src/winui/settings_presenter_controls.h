#pragma once

#include "../settings_controller.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.UI.h>

#include <algorithm>
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
namespace muxm = winrt::Microsoft::UI::Xaml::Media;

inline constexpr double kSettingControlWidth = 520.0;
inline constexpr double kStackedSettingThreshold = 760.0;

/**
 * Programmatic equivalent of the legacy BeginSettingRow contract.
 *
 * On a wide page the label/help column stays on the left and a fixed-width
 * editor stays on the right.  On a narrow page the editor moves below the
 * label so translated labels never collide with the control.
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
        root.RowSpacing(8.0);
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
        muxc::RowDefinition controlRow{};
        controlRow.Height(mux::GridLengthHelper::Auto());
        root.RowDefinitions().Append(labelRow);
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

        const auto weakRoot = winrt::make_weak(root);
        const auto weakText = winrt::make_weak(text);
        const auto weakControl = winrt::make_weak(controlHost);
        root.SizeChanged(
            [weakRoot, weakText, weakControl, controlWidth](
                const auto&, const mux::SizeChangedEventArgs& args) {
                const auto currentRoot = weakRoot.get();
                const auto currentText = weakText.get();
                const auto currentControl = weakControl.get();
                if (!currentRoot || !currentText || !currentControl)
                    return;

                const bool stacked =
                    args.NewSize().Width < kStackedSettingThreshold;
                auto columns = currentRoot.ColumnDefinitions();
                if (stacked)
                {
                    columns.GetAt(0).Width(
                        mux::GridLengthHelper::FromValueAndType(
                            1.0, mux::GridUnitType::Star));
                    columns.GetAt(1).Width(
                        mux::GridLengthHelper::FromValueAndType(
                            0.0, mux::GridUnitType::Pixel));
                    muxc::Grid::SetColumn(currentText, 0);
                    muxc::Grid::SetColumnSpan(currentText, 2);
                    muxc::Grid::SetRow(currentText, 0);
                    muxc::Grid::SetColumn(currentControl, 0);
                    muxc::Grid::SetColumnSpan(currentControl, 2);
                    muxc::Grid::SetRow(currentControl, 1);
                    currentControl.MaxWidth(controlWidth);
                    currentControl.HorizontalAlignment(
                        mux::HorizontalAlignment::Stretch);
                }
                else
                {
                    columns.GetAt(0).Width(
                        mux::GridLengthHelper::FromValueAndType(
                            1.0, mux::GridUnitType::Star));
                    columns.GetAt(1).Width(
                        mux::GridLengthHelper::FromValueAndType(
                            controlWidth, mux::GridUnitType::Pixel));
                    muxc::Grid::SetColumn(currentText, 0);
                    muxc::Grid::SetColumnSpan(currentText, 1);
                    muxc::Grid::SetRow(currentText, 0);
                    muxc::Grid::SetColumn(currentControl, 1);
                    muxc::Grid::SetColumnSpan(currentControl, 1);
                    muxc::Grid::SetRow(currentControl, 0);
                    currentControl.MaxWidth(controlWidth);
                    currentControl.HorizontalAlignment(
                        mux::HorizontalAlignment::Stretch);
                }
            });
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
    using ChangedCallback = std::function<void(
        const winrt::Windows::UI::Color&, SettingsUpdateMode)>;

    SettingRow row;
    muxc::Button button{nullptr};
    muxc::Border swatch{nullptr};
    muxc::Flyout flyout{nullptr};
    muxc::ColorPicker picker{nullptr};
    muxc::Button apply{nullptr};
    muxc::Button cancel{nullptr};
    ChangedCallback changed;
    winrt::Windows::UI::Color original{};
    bool updating = false;
    bool open = false;
    bool accepted = false;
    bool rollbackApplied = false;
    bool closed = false;

    winrt::event_token openingToken{};
    winrt::event_token closedToken{};
    winrt::event_token colorToken{};
    winrt::event_token applyToken{};
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

        apply = muxc::Button{};
        cancel = muxc::Button{};
        muxc::StackPanel actions{};
        actions.Orientation(muxc::Orientation::Horizontal);
        actions.HorizontalAlignment(mux::HorizontalAlignment::Right);
        actions.Spacing(8.0);
        actions.Children().Append(apply);
        actions.Children().Append(cancel);

        muxc::StackPanel panel{};
        panel.Spacing(10.0);
        panel.Children().Append(picker);
        panel.Children().Append(actions);
        flyout = muxc::Flyout{};
        flyout.Content(panel);
        button.Flyout(flyout);
        row.Initialize(button);

        openingToken = flyout.Opening([this](const auto&, const auto&) {
            if (closed) return;
            original = picker.Color();
            open = true;
            accepted = false;
            rollbackApplied = false;
        });
        colorToken = picker.ColorChanged([this](const auto&, const auto&) {
            if (closed || updating || !open) return;
            UpdateSwatch();
            if (changed)
                changed(picker.Color(), SettingsUpdateMode::Preview);
        });
        applyToken = apply.Click([this](const auto&, const auto&) {
            if (closed || !open) return;
            accepted = true;
            if (changed)
            {
                changed(picker.Color(),
                    SettingsUpdateMode::PreviewAndCommit);
            }
            flyout.Hide();
        });
        cancelToken = cancel.Click([this](const auto&, const auto&) {
            if (closed || !open) return;
            Rollback();
            flyout.Hide();
        });
        closedToken = flyout.Closed([this](const auto&, const auto&) {
            if (closed) return;
            if (!accepted)
                Rollback();
            open = false;
        });
        UpdateSwatch();
    }

    void SetText(
        std::wstring labelText,
        std::wstring helpText,
        std::wstring applyText,
        std::wstring cancelText)
    {
        row.SetText(std::move(labelText), std::move(helpText));
        apply.Content(winrt::box_value(std::move(applyText)));
        cancel.Content(winrt::box_value(std::move(cancelText)));
        muxa::AutomationProperties::SetName(button, row.label.Text());
        muxa::AutomationProperties::SetHelpText(button, row.help.Text());
    }

    void SetColor(const winrt::Windows::UI::Color& color)
    {
        if (closed) return;
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

    void SetEnabled(bool enabled)
    {
        row.SetEnabled(enabled);
        button.IsEnabled(enabled);
    }

    void Rollback()
    {
        if (rollbackApplied) return;
        rollbackApplied = true;
        const bool previous = updating;
        updating = true;
        picker.Color(original);
        updating = previous;
        UpdateSwatch();
        if (changed)
            changed(original, SettingsUpdateMode::Preview);
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
            // Flyout::Hide completes asynchronously. Roll back while the
            // owning presenter is still active so its Preview callback can
            // restore the controller before navigation or teardown disables
            // event emission. The later Closed event is idempotent through
            // rollbackApplied.
            if (!accepted)
                Rollback();
            flyout.Hide();
        }
        catch (...)
        {
            try
            {
                if (!accepted)
                    Rollback();
            }
            catch (...)
            {
            }
            open = false;
        }
    }

    void Close() noexcept
    {
        if (closed) return;
        try
        {
            if (open && !accepted)
                Rollback();
        }
        catch (...)
        {
        }
        closed = true;
        try
        {
            flyout.Opening(openingToken);
            flyout.Closed(closedToken);
            picker.ColorChanged(colorToken);
            apply.Click(applyToken);
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

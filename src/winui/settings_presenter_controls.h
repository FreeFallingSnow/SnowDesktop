#pragma once

#include "settings/settings_controller.h"
#include "number_box_update_rules.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
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
#include <memory>
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
inline constexpr double kSettingNumberWidth = 96.0;
inline constexpr double kSettingResetWidth = 32.0;
inline constexpr double kSettingControlGap = 8.0;
inline constexpr double kSettingLabelMinWidth = 180.0;
inline constexpr double kSettingColumnGap = 24.0;
inline constexpr std::chrono::milliseconds kContinuousPreviewInterval{33};

// A closed InfoBar hides its template but still participates in StackPanel
// spacing unless the control itself is collapsed. Keep both states together.
inline void BindInfoBarVisibility(const muxc::InfoBar& bar)
{
    bar.Visibility(bar.IsOpen() ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    (void)bar.RegisterPropertyChangedCallback(muxc::InfoBar::IsOpenProperty(),
        [](const mux::DependencyObject& sender, const mux::DependencyProperty&) {
            const auto current = sender.as<muxc::InfoBar>();
            current.Visibility(current.IsOpen() ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        });
}

// Section headings have no surface. Keep their secondary actions equally
// lightweight while retaining native hyperlink focus and keyboard behavior.
inline void ConfigureSettingsHeadingLink(const muxc::HyperlinkButton& link)
{
    link.Padding({4.0, 0.0, 4.0, 0.0});
    link.MinHeight(32.0);
    link.VerticalAlignment(mux::VerticalAlignment::Center);
    link.HorizontalAlignment(mux::HorizontalAlignment::Right);
}

template <typename Surface>
inline void StyleSettingsCard(const Surface& surface)
{
    surface.Resources().Insert(winrt::box_value(L"SnowDesktopSettingsCard"), winrt::box_value(true));
    surface.Padding({16.0, 16.0, 16.0, 16.0});
    surface.MinHeight(68.0);
    surface.CornerRadius({4.0, 4.0, 4.0, 4.0});
    surface.BorderThickness({1.0, 1.0, 1.0, 1.0});
    surface.ClearValue(surface.template try_as<muxc::Grid>()
        ? muxc::Panel::BackgroundProperty() : muxc::Border::BackgroundProperty());
    surface.ClearValue(Surface::BorderBrushProperty());
    if (const auto application = mux::Application::Current())
    {
        // The XAML style retains ThemeResource expressions, so an element
        // theme/high-contrast change never leaves a captured light brush.
        const auto key = surface.template try_as<muxc::Grid>()
            ? L"SettingsGridCardSurface" : L"SettingsBorderCardSurface";
        surface.Style(application.Resources().Lookup(winrt::box_value(key)).template as<mux::Style>());
    }
}

// Related child rows share their parent's surface. Keep their existing
// content/handlers and only remove the nested card treatment.
template <typename Surface>
inline void StyleSettingsSubrow(const Surface& surface,
    bool first = false, double indent = 16.0)
{
    const auto cardKey = winrt::box_value(L"SnowDesktopSettingsCard");
    if (surface.Resources().HasKey(cardKey)) surface.Resources().Remove(cardKey);
    surface.Padding({16.0 + indent, 8.0, 16.0, 8.0});
    surface.MinHeight(52.0);
    surface.CornerRadius({0.0});
    surface.BorderThickness({0.0, first ? 0.0 : 1.0, 0.0, 0.0});
    surface.ClearValue(surface.template try_as<muxc::Grid>()
        ? muxc::Panel::BackgroundProperty() : muxc::Border::BackgroundProperty());
    surface.ClearValue(Surface::BorderBrushProperty());
    if (const auto application = mux::Application::Current())
    {
        const auto key = surface.template try_as<muxc::Grid>()
            ? L"SettingsGridSubrowSurface" : L"SettingsBorderSubrowSurface";
        surface.Style(application.Resources().Lookup(winrt::box_value(key)).template as<mux::Style>());
    }
}

inline void UpdateSettingsSubrows(const muxc::StackPanel& panel,
    double indent = 16.0)
{
    bool first = true;
    panel.Spacing(0.0);
    for (const auto& child : panel.Children())
    {
        const auto element = child.try_as<mux::FrameworkElement>();
        if (!element || element.Visibility() != mux::Visibility::Visible)
            continue;
        if (const auto border = child.try_as<muxc::Border>())
        {
            if (border.Resources().HasKey(winrt::box_value(L"SnowDesktopSettingsDisclosureRow")))
                border.BorderThickness({0, first ? 0.0 : 1.0, 0, 0});
            else StyleSettingsSubrow(border, first, indent);
        }
        else if (const auto grid = child.try_as<muxc::Grid>())
            StyleSettingsSubrow(grid, first, indent);
        else if (const auto group = child.try_as<muxc::StackPanel>(); group &&
            group.Resources().HasKey(winrt::box_value(L"SnowDesktopSettingsRowGroup")))
        {
            group.Resources().Insert(winrt::box_value(L"SnowDesktopSettingsFirstRow"), winrt::box_value(first));
            for (const auto& nested : group.Children())
            {
                const auto row = nested.try_as<muxc::Grid>();
                if (!row || row.Visibility() != mux::Visibility::Visible ||
                    !row.Resources().HasKey(winrt::box_value(L"SnowDesktopSettingRow"))) continue;
                row.BorderThickness({0, first ? 0.0 : 1.0, 0, 0});
                first = false;
            }
            continue;
        }
        else
            continue;
        first = false;
    }
}

// PowerToys SettingsGroup separates the section heading from the surface.
// Keep the stable outer element for visibility/focus ownership; only the body
// is a card. Rows inside it share one surface and one horizontal inset.
inline muxc::Border InitializeSettingsGroup(const muxc::Border& root,
    const mux::FrameworkElement& heading, const muxc::StackPanel& body,
    const mux::Style& style, bool paddedBody = false)
{
    muxc::StackPanel group;
    group.Spacing(8.0);
    if (heading)
    {
        heading.Margin({0.0, 0.0, 0.0, 0.0});
        muxa::AutomationProperties::SetHeadingLevel(heading,
            muxa::Peers::AutomationHeadingLevel::Level2);
        group.Children().Append(heading);
    }
    muxc::Border surface;
    if (style) surface.Style(style);
    if (!paddedBody) surface.Padding({0.0, 0.0, 0.0, 0.0});
    surface.Child(body);
    group.Children().Append(surface);
    root.Child(group);
    return surface;
}

// A disclosure embedded in an existing settings surface keeps native keyboard
// and automation behavior, without introducing another background or outline.
inline void StyleInlineSettingsExpander(const muxc::Expander& expander)
{
    muxm::SolidColorBrush transparent{winrt::Windows::UI::Color{0, 0, 0, 0}};
    expander.Background(transparent);
    expander.BorderBrush(transparent);
    expander.BorderThickness({0, 0, 0, 0});
    expander.CornerRadius({0});
    expander.Resources().Insert(winrt::box_value(L"ExpanderHeaderBorderThickness"),
        winrt::box_value(mux::Thickness{0, 0, 0, 0}));
    for (const auto key : {L"ExpanderHeaderBackground", L"ExpanderHeaderBorderBrush",
        L"ExpanderHeaderBorderPointerOverBrush", L"ExpanderHeaderBorderPressedBrush",
        L"ExpanderHeaderDisabledBorderBrush"})
        expander.Resources().Insert(winrt::box_value(key), transparent);
    expander.Loaded([](const auto& sender, const auto&) {
        const auto disclosure = sender.template as<muxc::Expander>();
        (void)disclosure.ApplyTemplate();
        const auto flatten = [](const auto& self, const mux::DependencyObject& node) -> void {
            if (const auto border = node.try_as<muxc::Border>(); border && border.Name() == L"ExpanderContent")
            { border.BorderThickness({0, 0, 0, 0}); border.Background(nullptr); return; }
            for (int index = 0; index < muxm::VisualTreeHelper::GetChildrenCount(node); ++index)
                self(self, muxm::VisualTreeHelper::GetChild(node, index));
        };
        flatten(flatten, disclosure);
    });
}

// A straight divider belongs to the parent card, outside the native Expander
// template. Mirroring visibility keeps conditional sections from leaving gaps.
inline muxc::Border SettingsDisclosureRow(const muxc::Expander& expander, bool first = false)
{
    StyleInlineSettingsExpander(expander);
    muxc::Border row;
    StyleSettingsSubrow(row, first, 0);
    row.Padding({0});
    row.MinHeight(0);
    row.Resources().Insert(winrt::box_value(L"SnowDesktopSettingsDisclosureRow"), winrt::box_value(true));
    row.Child(expander);
    row.Visibility(expander.Visibility());
    const auto weak = winrt::make_weak(row);
    (void)expander.RegisterPropertyChangedCallback(mux::UIElement::VisibilityProperty(),
        [weak](const mux::DependencyObject& sender, const mux::DependencyProperty&) {
            if (const auto surface = weak.get()) surface.Visibility(sender.as<muxc::Expander>().Visibility());
        });
    return row;
}

// Appearance editors contain transparent layout panels around their sections.
// Join only those sections; their field layouts and nested editors stay intact.
inline void JoinSettingsSections(const muxc::StackPanel& panel)
{
    panel.Spacing(0);
    for (uint32_t index = 0; index < panel.Children().Size(); ++index)
    {
        const auto child = panel.Children().GetAt(index);
        if (const auto expander = child.try_as<muxc::Expander>())
        {
            panel.Children().RemoveAt(index);
            panel.Children().InsertAt(index, SettingsDisclosureRow(expander));
        }
        else if (const auto nested = child.try_as<muxc::StackPanel>()) JoinSettingsSections(nested);
    }
}

inline bool IsNumberBoxTextEditing(const muxc::NumberBox& number)
{
    const auto root = number.XamlRoot();
    if (!root) return false;
    auto focused = muxi::FocusManager::GetFocusedElement(root)
        .try_as<mux::DependencyObject>();
    if (!focused || !focused.try_as<muxc::TextBox>()) return false;
    while (focused)
    {
        if (focused == number) return true;
        focused = muxm::VisualTreeHelper::GetParent(focused);
    }
    return false;
}

// Local slider/spin/reset changes may replace the text, but redundant writes
// must not format it or move the caret while the user is typing.
inline void SetNumberBoxValue(const muxc::NumberBox& number, double value)
{
    if (number_box_update_rules::ShouldWriteValue(number.Value(), value))
        number.Value(value);
}

// Host snapshots can arrive while the inner TextBox still contains an empty
// draft, a sign, or a partial decimal. Let WinUI evaluate it on Enter/blur.
inline void SyncNumberBoxValue(const muxc::NumberBox& number, double value)
{
    const double current = number.Value();
    if (!number_box_update_rules::ShouldWriteValue(current, value)) return;
    if (number_box_update_rules::ShouldWriteValue(
        current, value, IsNumberBoxTextEditing(number)))
    {
        number.Value(value);
    }
}

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
    icon.FontSize(16.0);
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
 * the right. Compact controls stay paired with their wrapping label. Only
 * wider editor combinations move below the label when their own width and a
 * readable label no longer fit; there is no global narrow-window layout.
 */
struct SettingRow
{
    muxc::Grid root{nullptr};
    muxc::StackPanel text{nullptr};
    muxc::TextBlock label{nullptr};
    muxc::TextBlock help{nullptr};
    muxc::ContentControl controlHost{nullptr};

    struct LayoutOptions
    {
        double controlWidth = kSettingControlWidth;
        bool compact = false;
        mux::HorizontalAlignment alignment = mux::HorizontalAlignment::Stretch;
    };
    std::shared_ptr<LayoutOptions> layout;

    static void Arrange(const muxc::Grid& grid,
        const muxc::ContentControl& host, const LayoutOptions& options,
        double width)
    {
        const bool automatic = options.compact || options.controlWidth <= 0.0;
        const double editorWidth = options.controlWidth > 0.0
            ? options.controlWidth : kSettingControlWidth;
        const bool stacked = !options.compact && width > 0.0 &&
            width < editorWidth + kSettingLabelMinWidth + kSettingColumnGap;
        grid.ColumnDefinitions().GetAt(1).Width(stacked
            ? mux::GridLengthHelper::FromValueAndType(0.0, mux::GridUnitType::Pixel)
            : automatic ? mux::GridLengthHelper::Auto()
            : mux::GridLengthHelper::FromValueAndType(editorWidth, mux::GridUnitType::Pixel));
        grid.ColumnSpacing(stacked ? 0.0 : kSettingColumnGap);
        grid.RowSpacing(stacked ? kSettingControlGap : 0.0);
        muxc::Grid::SetColumn(host, stacked ? 0 : 1);
        muxc::Grid::SetRow(host, stacked ? 1 : 0);
        host.HorizontalAlignment(stacked ? mux::HorizontalAlignment::Stretch : options.alignment);
        host.HorizontalContentAlignment(stacked ? mux::HorizontalAlignment::Stretch : options.alignment);
    }

    void Initialize(
        const mux::UIElement& control,
        double controlWidth = kSettingControlWidth)
    {
        root = muxc::Grid{};
        root.Resources().Insert(winrt::box_value(L"SnowDesktopSettingRow"), winrt::box_value(true));
        root.ColumnSpacing(kSettingColumnGap);
        root.MinHeight(36.0);
        root.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        layout = std::make_shared<LayoutOptions>();
        layout->controlWidth = controlWidth;
        layout->compact = static_cast<bool>(control.try_as<muxc::ToggleSwitch>()) ||
            static_cast<bool>(control.try_as<muxc::CheckBox>()) ||
            static_cast<bool>(control.try_as<muxc::Button>());
        if (layout->compact) layout->alignment = mux::HorizontalAlignment::Right;
        if (const auto toggle = control.try_as<muxc::ToggleSwitch>()) toggle.MinWidth(0.0);
        if (const auto check = control.try_as<muxc::CheckBox>()) check.MinWidth(0.0);

        muxc::ColumnDefinition labelColumn{};
        labelColumn.Width(mux::GridLengthHelper::FromValueAndType(
            1.0, mux::GridUnitType::Star));
        muxc::ColumnDefinition controlColumn{};
        const bool autoControlWidth = controlWidth <= 0.0 || layout->compact;
        controlColumn.Width(autoControlWidth
                ? mux::GridLengthHelper::Auto()
                : mux::GridLengthHelper::FromValueAndType(
                      controlWidth, mux::GridUnitType::Pixel));
        root.ColumnDefinitions().Append(labelColumn);
        root.ColumnDefinitions().Append(controlColumn);
        muxc::RowDefinition labelRow{};
        labelRow.Height(mux::GridLengthHelper::FromValueAndType(1.0, mux::GridUnitType::Star));
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
        help.FontSize(12.0);
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
        controlHost.VerticalAlignment(mux::VerticalAlignment::Center);
        controlHost.HorizontalAlignment(layout->alignment);
        controlHost.HorizontalContentAlignment(layout->alignment);

        muxc::Grid::SetColumn(text, 0);
        muxc::Grid::SetColumn(controlHost, 1);
        root.Children().Append(text);
        root.Children().Append(controlHost);

        // SettingRow is also used as a short-lived builder in several
        // presenters, while its XAML elements remain in the visual tree.
        // Capture weak projected objects rather than `this` so responsive
        // updates remain safe after the builder object goes out of scope.
        const auto weakControlHost = winrt::make_weak(controlHost);
        root.SizeChanged(
            [weakControlHost, options = layout](
                const auto& sender,
                const mux::SizeChangedEventArgs& args) {
                const auto grid = sender.template try_as<muxc::Grid>();
                const auto responsiveControlHost = weakControlHost.get();
                if (!grid || !responsiveControlHost)
                {
                    return;
                }

                Arrange(grid, responsiveControlHost, *options, args.NewSize().Width);
            });
    }

    void SetCompactEditor(bool compact = true)
    {
        layout->compact = compact;
        Arrange(root, controlHost, *layout, root.ActualWidth());
    }

    void SetControlAlignment(mux::HorizontalAlignment alignment)
    {
        // Choice editors share the standard column width across settings pages.
        // Compact buttons and toggles still use their natural width at its edge.
        if (alignment == mux::HorizontalAlignment::Right)
        {
            if (const auto combo = controlHost.Content().try_as<muxc::ComboBox>())
            {
                combo.ClearValue(mux::FrameworkElement::WidthProperty());
                combo.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
                alignment = mux::HorizontalAlignment::Stretch;
            }
        }
        // ContentControl can still occupy the full editor column even when
        // its ContentPresenter aligns a compact child.  Shrink and align the
        // host as well so ToggleSwitch and other compact templates actually
        // meet the right edge shared by full-width editors.
        layout->alignment = alignment;
        Arrange(root, controlHost, *layout, root.ActualWidth());
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
            if (const auto check = controlHost.Content().try_as<muxc::CheckBox>())
                check.MinWidth(0.0);
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

/** Add the same compact reset action to an already initialized setting row. */
inline muxc::Button AddRestoreDefaultAction(SettingRow& row,
    const std::wstring& text, std::function<void()> restore)
{
    const auto control = row.controlHost.Content();
    row.controlHost.Content(nullptr);
    muxc::Grid pair; pair.ColumnSpacing(8);
    muxc::ColumnDefinition value; value.Width(mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star));
    muxc::ColumnDefinition tail; tail.Width(mux::GridLengthHelper::Auto());
    pair.ColumnDefinitions().Append(value); pair.ColumnDefinitions().Append(tail);
    muxc::ContentControl editor; editor.Content(control); editor.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
    pair.Children().Append(editor);
    muxc::Button button; ConfigureRestoreDefaultButton(button, text);
    muxc::Grid::SetColumn(button, 1); pair.Children().Append(button);
    button.Click([restore = std::move(restore)](auto const&, auto const&) { restore(); });
    row.controlHost.Content(pair); return button;
}

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

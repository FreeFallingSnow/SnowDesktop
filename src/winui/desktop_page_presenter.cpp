#include "pch.h"

#include "desktop_page_presenter.h"
#include "edge_light_editor.h"
#include "appearance_sections.h"
#include "settings_presenter_controls.h"

#include "../constants.h"
#include "../font_weight_rules.h"
#include "../layout_spacing_rules.h"
#include "../icon_beautify.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Globalization.NumberFormatting.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxa = winrt::Microsoft::UI::Xaml::Automation;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace muxi = winrt::Microsoft::UI::Xaml::Input;
using presenter_controls::ColorFlyoutEditor;
using presenter_controls::CoalescedPreviewTimer;
using presenter_controls::QuantizeNumericValue;
using presenter_controls::SettingRow;

namespace
{

// The legacy ImGui editor displayed this scale as 100%, while persistence and
// layout use 1.0.  Keep both sides explicit so the reset cannot drift when the
// presentation unit changes.
constexpr double kDefaultIconSpacingScale = 1.0;

struct SettingsCard
{
    muxc::Border root{nullptr};
    muxc::StackPanel content{nullptr};
    muxc::TextBlock title{nullptr};
};

void InitializeCard(
    SettingsCard& card,
    const mux::Style& style,
    const muxc::StackPanel& page)
{
    card.root = muxc::Border{};
    card.root.Style(style);
    card.content = muxc::StackPanel{};
    card.content.Spacing(12.0);
    card.title = muxc::TextBlock{};
    card.title.FontWeight(
        winrt::Windows::UI::Text::FontWeights::SemiBold());
    card.title.TextWrapping(mux::TextWrapping::Wrap);
    card.content.Children().Append(card.title);
    card.root.Child(card.content);
    page.Children().Append(card.root);
}

void SetHeader(
    const muxc::TextBlock& label,
    const mux::FrameworkElement& control,
    std::wstring text)
{
    label.Text(std::move(text));
    label.TextWrapping(mux::TextWrapping::Wrap);
    muxa::AutomationProperties::SetName(control, label.Text());
}

void SetComboItems(
    const muxc::ComboBox& combo,
    const std::vector<std::wstring>& labels,
    int selection)
{
    combo.Items().Clear();
    for (const auto& label : labels)
        combo.Items().Append(winrt::box_value(label));
    combo.SelectedIndex(selection);
}

std::wstring Trim(std::wstring value)
{
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos)
        return {};
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::uint8_t ColorByte(float value) noexcept
{
    return static_cast<std::uint8_t>(std::lround(
        std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

float ColorFloat(std::uint8_t value) noexcept
{
    return static_cast<float>(value) / 255.0f;
}

winrt::Windows::UI::Color MakeColor(float red, float green, float blue)
{
    winrt::Windows::UI::Color color{};
    color.A = 255;
    color.R = ColorByte(red);
    color.G = ColorByte(green);
    color.B = ColorByte(blue);
    return color;
}

bool SameColor(
    const winrt::Windows::UI::Color& left,
    const winrt::Windows::UI::Color& right) noexcept
{
    return left.A == right.A && left.R == right.R &&
        left.G == right.G && left.B == right.B;
}

constexpr std::array<IconBeautifyPreset, 6> kBeautifyPresets = {
    IconBeautifyPreset::None,
    IconBeautifyPreset::DefaultBeautify,
    IconBeautifyPreset::FrostedGlass,
    IconBeautifyPreset::FrostedGlassDark,
    IconBeautifyPreset::FrostedGlassLight,
    IconBeautifyPreset::Custom,
};

constexpr std::array<IconBeautifyShape, 5> kBeautifyShapes = {
    IconBeautifyShape::LegacyRounded,
    IconBeautifyShape::ContinuousRounded,
    IconBeautifyShape::SoftRounded,
    IconBeautifyShape::Circle,
    IconBeautifyShape::Pebble,
};

template <typename T, std::size_t Size>
int IndexOf(const std::array<T, Size>& values, T value) noexcept
{
    const auto found = std::find(values.begin(), values.end(), value);
    return found == values.end()
        ? 0
        : static_cast<int>(std::distance(values.begin(), found));
}

struct NumericEditor
{
    using ChangedCallback =
        std::function<void(double value, SettingsUpdateMode mode)>;

    SettingRow settingRow;
    muxc::Grid root{nullptr};
    muxc::TextBlock label{nullptr};
    muxc::Grid editors{nullptr};
    muxc::Slider slider{nullptr};
    muxc::StackPanel numberHost{nullptr};
    muxc::NumberBox number{nullptr};
    muxc::TextBlock unit{nullptr};
    muxc::Button reset{nullptr};
    CoalescedPreviewTimer<double> preview;
    ChangedCallback changed;
    double defaultValue = std::numeric_limits<double>::quiet_NaN();
    double step = 1.0;
    bool updating = false;
    bool pendingCommit = false;
    bool interactionActive = false;
    bool closed = false;
    mux::DispatcherTimer idleCommitTimer{nullptr};

    winrt::event_token sliderValueToken{};
    winrt::event_token numberValueToken{};
    winrt::event_token sliderPointerToken{};
    winrt::event_token numberPointerToken{};
    winrt::event_token sliderLostFocusToken{};
    winrt::event_token numberLostFocusToken{};
    winrt::event_token sliderKeyToken{};
    winrt::event_token numberKeyToken{};
    winrt::event_token resetToken{};
    winrt::event_token idleCommitToken{};

    NumericEditor(
        double minimum,
        double maximum,
        double step,
        int fractionalDigits,
        ChangedCallback callback,
        double resetValue = std::numeric_limits<double>::quiet_NaN())
        : changed(std::move(callback)), defaultValue(resetValue), step(step)
    {
        editors = muxc::Grid{};
        editors.ColumnSpacing(8.0);
        muxc::ColumnDefinition sliderColumn{};
        sliderColumn.Width(mux::GridLengthHelper::FromValueAndType(
            1.0, mux::GridUnitType::Star));
        muxc::ColumnDefinition numberColumn{};
        numberColumn.Width(mux::GridLengthHelper::Auto());
        editors.ColumnDefinitions().Append(sliderColumn);
        editors.ColumnDefinitions().Append(numberColumn);
        if (std::isfinite(defaultValue))
        {
            muxc::ColumnDefinition resetColumn{};
            resetColumn.Width(mux::GridLengthHelper::Auto());
            editors.ColumnDefinitions().Append(resetColumn);
        }

        slider = muxc::Slider{};
        slider.Minimum(minimum);
        slider.Maximum(maximum);
        slider.StepFrequency(step);
        slider.SmallChange(step);
        slider.LargeChange(std::max(step, (maximum - minimum) / 10.0));
        slider.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        slider.VerticalAlignment(mux::VerticalAlignment::Center);

        number = muxc::NumberBox{};
        number.Minimum(minimum);
        number.Maximum(maximum);
        number.SmallChange(step);
        number.LargeChange(std::max(step, (maximum - minimum) / 10.0));
        number.SpinButtonPlacementMode(
            muxc::NumberBoxSpinButtonPlacementMode::Compact);
        number.ValidationMode(
            muxc::NumberBoxValidationMode::InvalidInputOverwritten);
        number.Width(92.0);
        if (fractionalDigits == 0)
            number.AcceptsExpression(false);

        numberHost = muxc::StackPanel{};
        numberHost.Orientation(muxc::Orientation::Horizontal);
        numberHost.Spacing(6.0);
        numberHost.VerticalAlignment(mux::VerticalAlignment::Center);
        unit = muxc::TextBlock{};
        unit.VerticalAlignment(mux::VerticalAlignment::Center);
        unit.Opacity(0.72);
        unit.Visibility(mux::Visibility::Collapsed);
        numberHost.Children().Append(number);
        numberHost.Children().Append(unit);

        editors.Children().Append(slider);
        muxc::Grid::SetColumn(numberHost, 1);
        editors.Children().Append(numberHost);
        if (std::isfinite(defaultValue))
        {
            reset = muxc::Button{};
            reset.VerticalAlignment(mux::VerticalAlignment::Center);
            reset.VerticalContentAlignment(mux::VerticalAlignment::Center);
            reset.HorizontalContentAlignment(
                mux::HorizontalAlignment::Center);
            reset.MinHeight(32.0);
            muxc::Grid::SetColumn(reset, 2);
            editors.Children().Append(reset);
        }
        settingRow.Initialize(editors);
        root = settingRow.root;
        label = settingRow.label;

        idleCommitTimer = mux::DispatcherTimer{};
        idleCommitTimer.Interval(std::chrono::milliseconds(650));
        idleCommitToken = idleCommitTimer.Tick(
            [this](const auto&, const auto&) {
                idleCommitTimer.Stop();
                CommitPending();
            });
        preview.Initialize([this](const double& value) {
            if (changed)
                changed(value, SettingsUpdateMode::Preview);
        });

        sliderValueToken = slider.ValueChanged(
            [this](const auto&, const auto&) {
                if (updating || closed) return;
                const double value = Normalize(slider.Value());
                updating = true;
                presenter_controls::SetNumberBoxValue(number, value);
                updating = false;
                PublishPreview(value);
            });
        numberValueToken = number.ValueChanged(
            [this](const auto&, const auto&) {
                if (updating || closed || std::isnan(number.Value())) return;
                const double value = Normalize(number.Value());
                updating = true;
                slider.Value(value);
                presenter_controls::SetNumberBoxValue(number, value);
                updating = false;
                PublishPreview(value);
            });

        const auto pointerReleased = [this](const auto&, const auto&) {
            CommitPending();
        };
        const auto lostFocus = [this](const auto&, const auto&) {
            CommitPending();
            interactionActive = false;
        };
        const auto keyDown = [this](const auto&, const muxi::KeyRoutedEventArgs& args) {
            if (args.Key() == winrt::Windows::System::VirtualKey::Enter)
                CommitPending();
        };
        sliderPointerToken = slider.PointerReleased(pointerReleased);
        numberPointerToken = number.PointerReleased(pointerReleased);
        sliderLostFocusToken = slider.LostFocus(lostFocus);
        numberLostFocusToken = number.LostFocus(lostFocus);
        sliderKeyToken = slider.KeyDown(keyDown);
        numberKeyToken = number.KeyDown(keyDown);
        if (reset)
        {
            resetToken = reset.Click([this](const auto&, const auto&) {
                idleCommitTimer.Stop();
                preview.Cancel();
                pendingCommit = false;
                if (changed)
                    changed(defaultValue,
                        SettingsUpdateMode::PreviewAndCommit);
            });
        }
    }

    void PublishPreview(double value)
    {
        interactionActive = true;
        pendingCommit = true;
        idleCommitTimer.Stop();
        idleCommitTimer.Start();
        preview.Queue(value);
    }

    void CommitPending()
    {
        if (idleCommitTimer)
            idleCommitTimer.Stop();
        if (!pendingCommit || closed) return;
        preview.Cancel();
        pendingCommit = false;
        if (changed)
            changed(Normalize(slider.Value()),
                SettingsUpdateMode::PreviewAndCommit);
    }

    void CancelPending() noexcept
    {
        if (idleCommitTimer)
            idleCommitTimer.Stop();
        preview.Cancel();
        pendingCommit = false;
        interactionActive = false;
    }

    void SetValue(double value)
    {
        if (closed) return;
        // A controller preview publishes an immutable snapshot back to this
        // presenter on the next DispatcherQueue turn. Reassigning Slider.Value
        // during the interaction makes WinUI release the thumb's pointer
        // capture after its first movement. The editor already owns the newest
        // local value, so leave it untouched until focus exits the edit.
        if (interactionActive) return;
        value = Normalize(value);
        const bool wasUpdating = updating;
        updating = true;
        slider.Value(value);
        presenter_controls::SyncNumberBoxValue(number, value);
        updating = wasUpdating;
    }

    [[nodiscard]] double Normalize(double value) const noexcept
    {
        return QuantizeNumericValue(
            value, slider.Minimum(), slider.Maximum(), step);
    }

    [[nodiscard]] bool IsEditing() const noexcept
    {
        return interactionActive;
    }

    void SetLabel(std::wstring text, std::wstring help = {})
    {
        settingRow.SetText(std::move(text), std::move(help));
        muxa::AutomationProperties::SetName(slider, label.Text());
        muxa::AutomationProperties::SetName(number, label.Text());
    }

    void SetResetText(std::wstring text)
    {
        if (!reset) return;
        presenter_controls::ConfigureRestoreDefaultButton(reset, text);
    }

    void SetUnit(std::wstring text)
    {
        unit.Text(std::move(text));
        unit.Visibility(unit.Text().empty()
                ? mux::Visibility::Collapsed
                : mux::Visibility::Visible);
    }

    void SetEnabled(bool enabled)
    {
        settingRow.SetEnabled(enabled);
    }

    void Close() noexcept
    {
        if (closed) return;
        closed = true;
        try
        {
            idleCommitTimer.Stop();
            idleCommitTimer.Tick(idleCommitToken);
            preview.Close();
            slider.ValueChanged(sliderValueToken);
            number.ValueChanged(numberValueToken);
            slider.PointerReleased(sliderPointerToken);
            number.PointerReleased(numberPointerToken);
            slider.LostFocus(sliderLostFocusToken);
            number.LostFocus(numberLostFocusToken);
            slider.KeyDown(sliderKeyToken);
            number.KeyDown(numberKeyToken);
            if (reset)
                reset.Click(resetToken);
        }
        catch (...)
        {
        }
        changed = {};
    }
};

struct ColorEditor
{
    using ChangedCallback = std::function<void(
        const winrt::Windows::UI::Color& color,
        SettingsUpdateMode mode)>;

    ColorFlyoutEditor editor;
    muxc::Grid root{nullptr};

    explicit ColorEditor(ChangedCallback callback)
    {
        editor.Initialize(std::move(callback));
        root = editor.row.root;
    }

    void CommitPending()
    {
    }

    void CancelPending() noexcept
    {
    }

    void SetColor(const winrt::Windows::UI::Color& value)
    {
        editor.SetColor(value);
    }

    [[nodiscard]] bool IsEditing() const noexcept
    {
        return editor.IsOpen();
    }

    void SetLabel(
        std::wstring text,
        std::wstring cancelText)
    {
        editor.SetText(
            std::move(text), {}, std::move(cancelText));
    }

    void SetEnabled(bool enabled)
    {
        editor.SetEnabled(enabled);
    }

    void Dismiss() noexcept
    {
        editor.Dismiss();
    }

    void Close() noexcept
    {
        editor.Close();
    }
};

struct RuleRow
{
    std::wstring id;
    std::wstring customLabel;
    muxc::Border root{nullptr};
    muxc::StackPanel content{nullptr};
    muxc::Grid nameActions{nullptr};
    SettingRow labelRow;
    SettingRow extensionsRow;
    muxc::TextBox label{nullptr};
    muxc::TextBox extensions{nullptr};
    muxc::Button remove{nullptr};
    muxc::ToggleSwitch enabled{nullptr};
    muxc::Button restoreName{nullptr};
    muxc::Button restoreExtensions{nullptr};
    winrt::event_token labelToken{};
    winrt::event_token extensionsToken{};
    winrt::event_token removeToken{};
    winrt::event_token enabledToken{};
    winrt::event_token restoreNameToken{};
    winrt::event_token restoreExtensionsToken{};
    bool closed = false;

    void Close() noexcept
    {
        if (closed) return;
        closed = true;
        try
        {
            label.TextChanged(labelToken);
            extensions.TextChanged(extensionsToken);
            remove.Click(removeToken);
            enabled.Toggled(enabledToken);
            restoreName.Click(restoreNameToken);
            restoreExtensions.Click(restoreExtensionsToken);
        }
        catch (...)
        {
        }
    }
};

} // namespace

struct DesktopPagePresenter::Impl
{
    explicit Impl(LocalizeCallback callback, const mux::Style& style)
        : localize(std::move(callback)), cardStyle(style)
    {
        BuildControls();
        HookEvents();
        RefreshLocalizedText();
    }

    LocalizeCallback localize;
    DesktopPageActions actions;
    mux::Style cardStyle{nullptr};
    muxc::StackPanel root{nullptr};
    muxc::StackPanel desktopIconsRoot{nullptr};
    muxc::StackPanel iconBeautificationRoot{nullptr};
    muxc::StackPanel categoryRoot{nullptr};

    SettingsCard displayCard;
    SettingsCard categoryLayoutCard;
    SettingsCard beautifyCard;
    SettingsCard categoryRulesCard;

    std::unique_ptr<NumericEditor> iconSpacing;
    std::unique_ptr<NumericEditor> iconSize;
    std::unique_ptr<NumericEditor> itemFontSize;
    std::unique_ptr<NumericEditor> listFontSize;
    std::unique_ptr<NumericEditor> itemFontWeight;
    muxc::TextBlock shortcutArrowLabel{nullptr};
    muxc::ComboBox shortcutArrow{nullptr};
    SettingRow shortcutArrowRow;
    std::array<muxc::ComboBox, 3> titleLineCombos;
    std::array<SettingRow, 3> titleLineRows;
    std::array<winrt::event_token, 3> titleLineTokens{};
    muxc::ComboBox titleOverflow{nullptr};
    SettingRow titleOverflowRow;
    winrt::event_token titleOverflowToken{};

    muxc::ToggleSwitch showCategoryTabCounts{nullptr};
    SettingRow showCategoryTabCountsRow;
    muxc::ToggleSwitch collectPrograms{nullptr};
    SettingRow collectProgramsRow;

    muxc::TextBlock beautifyPresetLabel{nullptr};
    muxc::ComboBox beautifyPreset{nullptr};
    SettingRow beautifyPresetRow;
    muxc::StackPanel beautifyAdvanced{nullptr};
    muxc::TextBlock beautifyModeLabel{nullptr};
    muxc::ComboBox beautifyMode{nullptr};
    SettingRow beautifyModeRow;
    std::unique_ptr<ColorEditor> backgroundStart;
    std::unique_ptr<NumericEditor> backgroundOpacity;
    std::shared_ptr<EdgeLightEditor> edgeLightEditor;
    AppearanceSections beautifySections;
    muxc::TextBlock geometryTitle, finishTitle;
    muxc::ToggleSwitch glassEnabled{nullptr}, edgeReflection{nullptr};
    SettingRow glassEnabledRow, edgeReflectionRow;
    winrt::event_token glassEnabledToken{}, edgeReflectionToken{};
    std::unique_ptr<NumericEditor> glassBlurRadius, reflectionWidth, reflectionStrength;
    muxc::ToggleSwitch gradientEnabled{nullptr};
    SettingRow gradientEnabledRow;
    std::unique_ptr<ColorEditor> backgroundEnd;
    muxc::TextBlock gradientDirectionLabel{nullptr};
    muxc::ComboBox gradientDirection{nullptr};
    SettingRow gradientDirectionRow;
    muxc::TextBlock shapeLabel{nullptr};
    muxc::ComboBox shape{nullptr};
    SettingRow shapeRow;
    std::unique_ptr<NumericEditor> contentScale;
    std::unique_ptr<NumericEditor> highlightStrength;
    std::unique_ptr<NumericEditor> highlightSize;
    std::unique_ptr<NumericEditor> highlightAngle;
    std::unique_ptr<NumericEditor> shadeStrength;
    std::unique_ptr<NumericEditor> edgeHighlight;
    muxc::ToggleSwitch filterEnabled{nullptr};
    SettingRow filterEnabledRow;
    muxc::StackPanel filterDetails{nullptr};
    std::unique_ptr<ColorEditor> filterTint;
    std::unique_ptr<NumericEditor> filterStrength;
    std::unique_ptr<NumericEditor> shadowStrength;
    muxc::ToggleSwitch outlineEnabled{nullptr};
    SettingRow outlineEnabledRow;
    muxc::StackPanel outlineDetails{nullptr};
    std::unique_ptr<NumericEditor> outlineWidth;
    std::unique_ptr<NumericEditor> outlineOpacity;
    std::unique_ptr<ColorEditor> outlineColor;

    muxc::TextBlock categoryHint{nullptr};
    muxc::TextBlock categoryTypesHeading{nullptr};
    muxc::TextBlock addCategoryHeading{nullptr};
    muxc::TextBlock saveCategoryHeading{nullptr};
    muxc::StackPanel categoryRulePanel{nullptr};
    std::vector<std::unique_ptr<RuleRow>> ruleRows;
    muxc::TextBox newCategoryLabel{nullptr};
    muxc::TextBox newCategoryExtensions{nullptr};
    muxc::Button addCategory{nullptr};
    muxc::Grid newCategoryNameActions{nullptr};
    SettingRow newCategoryLabelRow;
    SettingRow newCategoryExtensionsRow;
    muxc::Button applyCategory{nullptr};
    muxc::Button restoreCategory{nullptr};
    SettingRow categoryActionsRow;
    muxc::TextBlock categoryStatus{nullptr};
    SettingRow categoryStatusRow;
    mux::DispatcherTimer categoryStatusTimer{nullptr};

    std::uint64_t generation = 0;
    std::uint64_t desktopRevision = 0;
    std::uint64_t categoryRevision = 0;
    std::uint64_t personalizationRevision = 0;
    bool hasSnapshot = false;
    bool updatingControls = false;
    bool active = false;
    bool closed = false;
    bool categoryDirty = false;
    bool categoryDirtyKnown = false;

    winrt::event_token shortcutArrowToken{};
    winrt::event_token showCountsToken{};
    winrt::event_token collectProgramsToken{};
    winrt::event_token beautifyPresetToken{};
    winrt::event_token beautifyModeToken{};
    winrt::event_token gradientEnabledToken{};
    winrt::event_token gradientDirectionToken{};
    winrt::event_token shapeToken{};
    winrt::event_token filterEnabledToken{};
    winrt::event_token outlineEnabledToken{};
    winrt::event_token addCategoryToken{};
    winrt::event_token applyCategoryToken{};
    winrt::event_token restoreCategoryToken{};
    winrt::event_token categoryStatusTimerToken{};

    [[nodiscard]] std::wstring L(
        std::string_view key,
        std::wstring_view fallback = {}) const
    {
        std::wstring value = localize ? localize(key) : std::wstring{};
        if (value.empty() || value == std::wstring(key.begin(), key.end()))
            value.assign(fallback);
        return value;
    }

    std::unique_ptr<NumericEditor> MakeDesktopNumber(
        double minimum,
        double maximum,
        double step,
        int digits,
        std::function<void(DesktopDisplaySettings&, double)> edit,
        double defaultValue = std::numeric_limits<double>::quiet_NaN())
    {
        return std::make_unique<NumericEditor>(minimum, maximum, step, digits,
            [this, edit = std::move(edit)](
                double value, SettingsUpdateMode mode) {
                UpdateDesktop(mode,
                    [edit, value](DesktopDisplaySettings& settings) {
                        edit(settings, value);
                    });
            }, defaultValue);
    }

    std::unique_ptr<NumericEditor> MakeBeautifyNumber(
        double minimum,
        double maximum,
        double step,
        int digits,
        std::function<void(IconBeautifySettings&, double)> edit)
    {
        return std::make_unique<NumericEditor>(minimum, maximum, step, digits,
            [this, edit = std::move(edit)](
                double value, SettingsUpdateMode mode) {
                UpdateBeautify(mode,
                    [edit, value](IconBeautifySettings& settings) {
                        edit(settings, value);
                    });
            });
    }

    std::unique_ptr<ColorEditor> MakeBeautifyColor(
        std::function<void(
            IconBeautifySettings&,
            const winrt::Windows::UI::Color&)> edit)
    {
        return std::make_unique<ColorEditor>(
            [this, edit = std::move(edit)](
                const winrt::Windows::UI::Color& value,
                SettingsUpdateMode mode) {
                UpdateBeautify(mode,
                    [edit, value](IconBeautifySettings& settings) {
                        edit(settings, value);
                    });
            });
    }

    void AppendCombo(
        const SettingsCard& card,
        SettingRow& row,
        muxc::ComboBox& combo)
    {
        combo = muxc::ComboBox{};
        combo.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        combo.MaxWidth(560.0);
        row.Initialize(combo);
        card.content.Children().Append(row.root);
    }

    void AppendAdvancedCombo(
        SettingRow& row,
        muxc::ComboBox& combo)
    {
        combo = muxc::ComboBox{};
        combo.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        combo.MaxWidth(560.0);
        row.Initialize(combo);
        beautifyAdvanced.Children().Append(row.root);
    }

    void BuildControls()
    {
        desktopIconsRoot = muxc::StackPanel{};
        desktopIconsRoot.Spacing(8.0);
        iconBeautificationRoot = muxc::StackPanel{};
        iconBeautificationRoot.Spacing(8.0);
        categoryRoot = muxc::StackPanel{};
        categoryRoot.Spacing(8.0);
        root = categoryRoot;

        InitializeCard(displayCard, cardStyle, desktopIconsRoot);
        iconSpacing = MakeDesktopNumber(
            layout_spacing_rules::kMinimumScale * 100.0f,
            layout_spacing_rules::kMaximumScale * 100.0f, 1.0, 0,
            [](DesktopDisplaySettings& settings, double value) {
                settings.iconSpacingScale =
                    static_cast<float>(value / 100.0);
            }, kDefaultIconSpacingScale * 100.0);
        iconSize = MakeDesktopNumber(
            kMinimumItemIconSizeScale * 100.0,
            kMaximumItemIconSizeScale * 100.0, 1.0, 0,
            [](DesktopDisplaySettings& settings, double value) {
                settings.itemIconSizeScale =
                    static_cast<float>(value / 100.0);
            }, kDefaultItemIconSizeScale * 100.0);
        itemFontSize = MakeDesktopNumber(
            kMinimumItemFontSizeCu, kMaximumItemFontSizeCu, 0.5, 1,
            [](DesktopDisplaySettings& settings, double value) {
                settings.itemFontSizeCu = static_cast<float>(value);
            }, 16.0);
        listFontSize = MakeDesktopNumber(
            kMinimumItemFontSizeCu, kMaximumItemFontSizeCu, 0.5, 1,
            [](DesktopDisplaySettings& settings, double value) {
                settings.listItemFontSizeCu = static_cast<float>(value);
            }, 16.0);
        itemFontWeight = MakeDesktopNumber(
            font_weight_rules::ToPercent(font_weight_rules::kMinimumWeight),
            font_weight_rules::ToPercent(font_weight_rules::kMaximumWeight),
            font_weight_rules::ToPercent(1), 1,
            [](DesktopDisplaySettings& settings, double value) {
                settings.itemFontWeight = font_weight_rules::FromPercent(value);
            }, 100.0);
        winrt::Windows::Globalization::NumberFormatting::DecimalFormatter weightFormatter;
        weightFormatter.FractionDigits(1);
        itemFontWeight->number.NumberFormatter(weightFormatter);
        iconSpacing->SetUnit(L"%");
        iconSize->SetUnit(L"%");
        itemFontSize->SetUnit(L"cu");
        listFontSize->SetUnit(L"cu");
        itemFontWeight->SetUnit(L"%");
        for (const auto* editor : {iconSize.get(),
                 itemFontSize.get(), listFontSize.get(), itemFontWeight.get()})
        {
            displayCard.content.Children().Append(editor->root);
        }
        AppendCombo(displayCard, shortcutArrowRow, shortcutArrow);
        for (std::size_t i = 0; i < titleLineCombos.size(); ++i) AppendCombo(displayCard, titleLineRows[i], titleLineCombos[i]);
        AppendCombo(displayCard, titleOverflowRow, titleOverflow);

        InitializeCard(categoryLayoutCard, cardStyle, categoryRoot);
        collectPrograms = muxc::ToggleSwitch{};
        collectPrograms.HorizontalAlignment(mux::HorizontalAlignment::Right);
        collectProgramsRow.Initialize(collectPrograms);
        collectProgramsRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        categoryLayoutCard.content.Children().Append(collectProgramsRow.root);
        showCategoryTabCounts = muxc::ToggleSwitch{};
        showCategoryTabCounts.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        showCategoryTabCountsRow.Initialize(showCategoryTabCounts);
        showCategoryTabCountsRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        categoryLayoutCard.content.Children().Append(
            showCategoryTabCountsRow.root);

        InitializeCard(
            beautifyCard, cardStyle, iconBeautificationRoot);
        AppendCombo(beautifyCard, beautifyPresetRow, beautifyPreset);
        beautifyAdvanced = muxc::StackPanel{};
        beautifyAdvanced.Spacing(12.0);
        beautifyCard.content.Children().Append(beautifyAdvanced);

        AppendAdvancedCombo(beautifyModeRow, beautifyMode);
        backgroundStart = MakeBeautifyColor(
            [](IconBeautifySettings& settings,
               const winrt::Windows::UI::Color& color) {
                settings.backgroundStartR = ColorFloat(color.R);
                settings.backgroundStartG = ColorFloat(color.G);
                settings.backgroundStartB = ColorFloat(color.B);
            });
        backgroundOpacity = MakeBeautifyNumber(0.0, 100.0, 1.0, 0,
            [](IconBeautifySettings& settings, double value) {
                settings.backgroundOpacity =
                    static_cast<float>(value / 100.0);
            });
        backgroundOpacity->SetUnit(L"%");
        gradientEnabled = muxc::ToggleSwitch{};
        gradientEnabled.HorizontalAlignment(mux::HorizontalAlignment::Right);
        gradientEnabledRow.Initialize(gradientEnabled);
        gradientEnabledRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        backgroundEnd = MakeBeautifyColor(
            [](IconBeautifySettings& settings,
               const winrt::Windows::UI::Color& color) {
                settings.backgroundEndR = ColorFloat(color.R);
                settings.backgroundEndG = ColorFloat(color.G);
                settings.backgroundEndB = ColorFloat(color.B);
            });
        beautifyAdvanced.Children().Append(backgroundStart->root);
        beautifyAdvanced.Children().Append(backgroundOpacity->root);
        beautifyAdvanced.Children().Append(gradientEnabledRow.root);
        beautifyAdvanced.Children().Append(backgroundEnd->root);
        AppendAdvancedCombo(gradientDirectionRow, gradientDirection);
        AppendAdvancedCombo(shapeRow, shape);

        glassEnabled = muxc::ToggleSwitch{};
        glassEnabled.HorizontalAlignment(mux::HorizontalAlignment::Right);
        glassEnabledRow.Initialize(glassEnabled);
        glassEnabledRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        edgeReflection = muxc::ToggleSwitch{};
        edgeReflection.HorizontalAlignment(mux::HorizontalAlignment::Right);
        edgeReflectionRow.Initialize(edgeReflection);
        edgeReflectionRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        glassBlurRadius = MakeBeautifyNumber(4.0, 48.0, 1.0, 0,
            [](auto& settings, double v) { settings.glassBlurRadius = static_cast<float>(v); });
        reflectionWidth = MakeBeautifyNumber(0.5, 4.0, 0.05, 2,
            [](auto& settings, double v) { settings.edgeHighlightWidth = static_cast<float>(v); });
        reflectionStrength = MakeBeautifyNumber(0.0, 100.0, 1.0, 0,
            [](auto& settings, double v) { settings.edgeHighlightStrength = static_cast<float>(v / 100.0); });
        glassBlurRadius->SetUnit(L"px"); reflectionWidth->SetUnit(L"px"); reflectionStrength->SetUnit(L"%");
        beautifyAdvanced.Children().Append(glassEnabledRow.root);
        beautifyAdvanced.Children().Append(glassBlurRadius->root);
        beautifyAdvanced.Children().Append(edgeReflectionRow.root);
        beautifyAdvanced.Children().Append(reflectionWidth->root);
        beautifyAdvanced.Children().Append(reflectionStrength->root);
        edgeLightEditor = EdgeLightEditor::Create([this](auto key) { return L(key, L""); }, [this](auto const& light, bool commit) {
            UpdateBeautify(commit ? SettingsUpdateMode::PreviewAndCommit : SettingsUpdateMode::Preview, [light](auto& settings) { settings.edgeLight = light; });
        });
        beautifyAdvanced.Children().Append(edgeLightEditor->Content());
        contentScale = MakeBeautifyNumber(50.0, 90.0, 1.0, 0,
            [](IconBeautifySettings& settings, double value) {
                settings.contentScale = static_cast<float>(value / 100.0);
            });
        highlightStrength = MakeBeautifyNumber(0.0, 100.0, 1.0, 0,
            [](IconBeautifySettings& settings, double value) {
                settings.textureHighlightStrength =
                    static_cast<float>(value / 100.0);
            });
        highlightSize = MakeBeautifyNumber(10.0, 100.0, 1.0, 0,
            [](IconBeautifySettings& settings, double value) {
                settings.textureHighlightSize =
                    static_cast<float>(value / 100.0);
            });
        highlightAngle = MakeBeautifyNumber(-45.0, 45.0, 1.0, 0,
            [](IconBeautifySettings& settings, double value) {
                settings.textureHighlightAngle =
                    static_cast<float>(value / 45.0);
            });
        shadeStrength = MakeBeautifyNumber(0.0, 100.0, 1.0, 0,
            [](IconBeautifySettings& settings, double value) {
                settings.textureShadeStrength =
                    static_cast<float>(value / 100.0);
            });
        edgeHighlight = MakeBeautifyNumber(0.0, 100.0, 1.0, 0,
            [](IconBeautifySettings& settings, double value) {
                settings.textureEdgeHighlight =
                    static_cast<float>(value / 100.0);
            });
        contentScale->SetUnit(L"%");
        highlightStrength->SetUnit(L"%");
        highlightSize->SetUnit(L"%");
        highlightAngle->SetUnit(L"°");
        shadeStrength->SetUnit(L"%");
        edgeHighlight->SetUnit(L"%");
        for (const auto* editor : {contentScale.get(), highlightStrength.get(),
                 highlightSize.get(), highlightAngle.get(),
                 shadeStrength.get(), edgeHighlight.get()})
        {
            beautifyAdvanced.Children().Append(editor->root);
        }

        filterEnabled = muxc::ToggleSwitch{};
        filterEnabled.HorizontalAlignment(mux::HorizontalAlignment::Right);
        filterEnabledRow.Initialize(filterEnabled);
        filterEnabledRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        filterDetails = muxc::StackPanel{};
        filterDetails.Spacing(12.0);
        filterTint = MakeBeautifyColor(
            [](IconBeautifySettings& settings,
               const winrt::Windows::UI::Color& color) {
                settings.filterTintR = ColorFloat(color.R);
                settings.filterTintG = ColorFloat(color.G);
                settings.filterTintB = ColorFloat(color.B);
            });
        filterStrength = MakeBeautifyNumber(0.0, 100.0, 1.0, 0,
            [](IconBeautifySettings& settings, double value) {
                settings.filterStrength =
                    static_cast<float>(value / 100.0);
            });
        shadowStrength = MakeBeautifyNumber(0.0, 100.0, 1.0, 0,
            [](IconBeautifySettings& settings, double value) {
                settings.shadowStrength =
                    static_cast<float>(value / 100.0);
            });
        outlineEnabled = muxc::ToggleSwitch{};
        outlineEnabled.HorizontalAlignment(mux::HorizontalAlignment::Right);
        outlineEnabledRow.Initialize(outlineEnabled);
        outlineEnabledRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        outlineDetails = muxc::StackPanel{};
        outlineDetails.Spacing(12.0);
        outlineWidth = MakeBeautifyNumber(0.0, 4.0, 0.1, 1,
            [](IconBeautifySettings& settings, double value) {
                settings.outlineWidth = static_cast<float>(value);
            });
        outlineOpacity = MakeBeautifyNumber(0.0, 100.0, 1.0, 0,
            [](IconBeautifySettings& settings, double value) {
                settings.outlineOpacity =
                    static_cast<float>(value / 100.0);
            });
        filterStrength->SetUnit(L"%");
        shadowStrength->SetUnit(L"%");
        outlineWidth->SetUnit(L"px");
        outlineOpacity->SetUnit(L"%");
        outlineColor = MakeBeautifyColor(
            [](IconBeautifySettings& settings,
               const winrt::Windows::UI::Color& color) {
                settings.outlineR = ColorFloat(color.R);
                settings.outlineG = ColorFloat(color.G);
                settings.outlineB = ColorFloat(color.B);
            });
        beautifyAdvanced.Children().Append(filterEnabledRow.root);
        filterDetails.Children().Append(filterTint->root);
        filterDetails.Children().Append(filterStrength->root);
        beautifyAdvanced.Children().Append(filterDetails);
        beautifyAdvanced.Children().Append(shadowStrength->root);
        beautifyAdvanced.Children().Append(outlineEnabledRow.root);
        outlineDetails.Children().Append(outlineWidth->root);
        outlineDetails.Children().Append(outlineOpacity->root);
        outlineDetails.Children().Append(outlineColor->root);
        beautifyAdvanced.Children().Append(outlineDetails);

        beautifyAdvanced.Children().Clear();
        std::vector<muxc::Expander> geometryDisclosures;
        auto geometry = AppearanceSections::Section(beautifyAdvanced, geometryTitle, &geometryDisclosures);
        geometry.Children().Append(beautifyModeRow.root);
        geometry.Children().Append(shapeRow.root);
        geometry.Children().Append(contentScale->root);
        beautifySections.Initialize(beautifyAdvanced, true, false, false);
        beautifySections.disclosures.insert(beautifySections.disclosures.begin(), geometryDisclosures.begin(), geometryDisclosures.end());
        for (auto const& control : {backgroundStart->root, backgroundOpacity->root, gradientEnabledRow.root, backgroundEnd->root, gradientDirectionRow.root})
            beautifySections.colors.Children().Append(control);
        beautifySections.material.Children().Append(glassEnabledRow.root); beautifySections.material.Children().Append(glassBlurRadius->root);
        beautifySections.border.Children().Append(outlineEnabledRow.root); beautifySections.border.Children().Append(outlineDetails);
        beautifySections.border.Children().Append(edgeReflectionRow.root); beautifySections.border.Children().Append(reflectionWidth->root);
        beautifySections.border.Children().Append(reflectionStrength->root); beautifySections.border.Children().Append(edgeLightEditor->Content());
        auto finish = AppearanceSections::Section(beautifyAdvanced, finishTitle, &beautifySections.disclosures);
        for (auto const* editor : {highlightStrength.get(), highlightSize.get(), highlightAngle.get(), shadeStrength.get(), edgeHighlight.get(), shadowStrength.get()}) finish.Children().Append(editor->root);
        finish.Children().Append(filterEnabledRow.root); finish.Children().Append(filterDetails);

        InitializeCard(categoryRulesCard, cardStyle, categoryRoot);
        const auto makeSubsectionHeading = [] {
            muxc::TextBlock heading{};
            heading.FontWeight(
                winrt::Windows::UI::Text::FontWeights::SemiBold());
            heading.TextWrapping(mux::TextWrapping::Wrap);
            return heading;
        };
        categoryTypesHeading = makeSubsectionHeading();
        addCategoryHeading = makeSubsectionHeading();
        saveCategoryHeading = makeSubsectionHeading();
        categoryHint = muxc::TextBlock{};
        categoryHint.Opacity(0.72);
        categoryHint.TextWrapping(mux::TextWrapping::Wrap);
        categoryRulesCard.content.Children().Append(categoryTypesHeading);
        categoryRulesCard.content.Children().Append(categoryHint);
        categoryRulePanel = muxc::StackPanel{};
        categoryRulePanel.Spacing(10.0);
        categoryRulesCard.content.Children().Append(categoryRulePanel);
        categoryRulesCard.content.Children().Append(addCategoryHeading);
        newCategoryLabel = muxc::TextBox{};
        newCategoryExtensions = muxc::TextBox{};
        addCategory = muxc::Button{};
        newCategoryNameActions = muxc::Grid{};
        newCategoryNameActions.ColumnSpacing(8.0);
        muxc::ColumnDefinition newNameColumn{};
        newNameColumn.Width(mux::GridLengthHelper::FromValueAndType(
            1.0, mux::GridUnitType::Star));
        muxc::ColumnDefinition newAddColumn{};
        newAddColumn.Width(mux::GridLengthHelper::Auto());
        newCategoryNameActions.ColumnDefinitions().Append(newNameColumn);
        newCategoryNameActions.ColumnDefinitions().Append(newAddColumn);
        newCategoryNameActions.Children().Append(newCategoryLabel);
        muxc::Grid::SetColumn(addCategory, 1);
        newCategoryNameActions.Children().Append(addCategory);
        newCategoryLabelRow.Initialize(newCategoryNameActions);
        newCategoryExtensionsRow.Initialize(newCategoryExtensions);
        categoryRulesCard.content.Children().Append(newCategoryLabelRow.root);
        categoryRulesCard.content.Children().Append(
            newCategoryExtensionsRow.root);

        muxc::StackPanel categoryActions{};
        categoryActions.Orientation(muxc::Orientation::Horizontal);
        categoryActions.HorizontalAlignment(mux::HorizontalAlignment::Right);
        categoryActions.Spacing(8.0);
        applyCategory = muxc::Button{};
        restoreCategory = muxc::Button{};
        applyCategory.VerticalAlignment(mux::VerticalAlignment::Center);
        restoreCategory.VerticalAlignment(mux::VerticalAlignment::Center);
        applyCategory.VerticalContentAlignment(mux::VerticalAlignment::Center);
        restoreCategory.VerticalContentAlignment(
            mux::VerticalAlignment::Center);
        applyCategory.MinHeight(32.0);
        restoreCategory.MinHeight(32.0);
        applyCategory.Visibility(mux::Visibility::Collapsed);
        restoreCategory.Visibility(mux::Visibility::Collapsed);
        categoryActionsRow.Initialize(categoryActions);
        categoryActionsRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        saveCategoryHeading.Visibility(mux::Visibility::Collapsed);
        categoryRulesCard.content.Children().Append(categoryActionsRow.root);
        categoryStatus = muxc::TextBlock{};
        categoryStatus.Opacity(0.72);
        categoryStatusRow.Initialize(categoryStatus);
        categoryStatusRow.root.Visibility(mux::Visibility::Collapsed);
        categoryRulesCard.content.Children().Append(categoryStatusRow.root);
    }

    template <typename Edit>
    void UpdateDesktop(SettingsUpdateMode mode, Edit edit)
    {
        if (!closed && active && hasSnapshot && !updatingControls &&
            actions.updateDesktop)
        {
            actions.updateDesktop(generation, mode,
                DesktopPageActions::DesktopEdit(std::move(edit)));
        }
    }

    template <typename Edit>
    void UpdatePersonalization(SettingsUpdateMode mode, Edit edit)
    {
        if (!closed && active && hasSnapshot && !updatingControls &&
            actions.updatePersonalization)
        {
            actions.updatePersonalization(generation, mode,
                DesktopPageActions::PersonalizationEdit(std::move(edit)));
        }
    }

    template <typename Edit>
    void UpdateCategory(SettingsUpdateMode mode, Edit edit)
    {
        if (!closed && active && hasSnapshot && !updatingControls &&
            actions.updateCategory)
        {
            actions.updateCategory(generation, mode,
                DesktopPageActions::CategoryEdit(std::move(edit)));
        }
    }

    template <typename Edit>
    void UpdateBeautify(SettingsUpdateMode mode, Edit edit)
    {
        UpdateDesktop(mode,
            [edit = std::move(edit)](DesktopDisplaySettings& desktop) mutable {
                edit(desktop.iconBeautify);
                desktop.iconBeautify.enabled = true;
                desktop.iconBeautify.preset = IconBeautifyPreset::Custom;
            });
    }

    void HookEvents()
    {
        for (std::size_t i = 0; i < titleLineCombos.size(); ++i)
            titleLineTokens[i] = titleLineCombos[i].SelectionChanged([this, i](const auto&, const auto&) {
                const int lines = titleLineCombos[i].SelectedIndex() + 1;
                if (lines < 1 || lines > 2) return;
                UpdateDesktop(SettingsUpdateMode::PreviewAndCommit, [i, lines](DesktopDisplaySettings& s) {
                    const std::array<int DesktopDisplaySettings::*, 3> fields{&DesktopDisplaySettings::desktopTitleLines, &DesktopDisplaySettings::largeFolderTitleLines, &DesktopDisplaySettings::scrollingTitleLines};
                    s.*fields[i] = lines;
                });
            });
        titleOverflowToken = titleOverflow.SelectionChanged(
            [this](const auto&, const auto&) {
                const int selection = titleOverflow.SelectedIndex();
                if (selection < 0 || selection > 1) return;
                UpdateDesktop(SettingsUpdateMode::PreviewAndCommit,
                    [selection](DesktopDisplaySettings& settings) {
                        settings.titleEllipsis = selection == 0;
                    });
            });
        shortcutArrowToken = shortcutArrow.SelectionChanged(
            [this](const auto&, const auto&) {
                const int selection = shortcutArrow.SelectedIndex();
                if (selection < 0) return;
                UpdateDesktop(SettingsUpdateMode::PreviewAndCommit,
                    [selection](DesktopDisplaySettings& settings) {
                        settings.shortcutArrowMode = selection;
                    });
            });
        showCountsToken = showCategoryTabCounts.Toggled(
            [this](const auto&, const auto&) {
                const bool enabled = showCategoryTabCounts.IsOn();
                UpdatePersonalization(SettingsUpdateMode::PreviewAndCommit,
                    [enabled](PersonalizationSettings& settings) {
                        settings.showCategoryTabCounts = enabled;
                    });
            });
        collectProgramsToken = collectPrograms.Toggled(
            [this](const auto&, const auto&) {
                const bool enabled = collectPrograms.IsOn();
                UpdateCategory(SettingsUpdateMode::PreviewAndCommit,
                    [enabled](CategorySettings& settings) {
                        settings.collectProgramsEnabled = enabled;
                    });
                if (!closed && active && hasSnapshot && !updatingControls &&
                    actions.commitCategory)
                    actions.commitCategory(generation);
            });
        beautifyPresetToken = beautifyPreset.SelectionChanged(
            [this](const auto&, const auto&) {
                const int selection = beautifyPreset.SelectedIndex();
                if (selection < 0 ||
                    static_cast<std::size_t>(selection) >=
                        kBeautifyPresets.size())
                    return;
                if (!updatingControls && active && hasSnapshot) edgeLightEditor->Cancel();
                const IconBeautifyPreset preset =
                    kBeautifyPresets[static_cast<std::size_t>(selection)];
                if (!updatingControls && active && hasSnapshot && preset == IconBeautifyPreset::Custom)
                    beautifySections.CollapseAll();
                UpdateDesktop(SettingsUpdateMode::PreviewAndCommit,
                    [preset](DesktopDisplaySettings& desktop) {
                        if (preset == IconBeautifyPreset::Custom)
                        {
                            desktop.iconBeautify.preset = preset;
                            desktop.iconBeautify.enabled = true;
                        }
                        else
                        {
                            desktop.iconBeautify =
                                icon_beautify::MakePreset(preset);
                        }
                    });
                UpdateConditionalStates();
            });
        beautifyModeToken = beautifyMode.SelectionChanged(
            [this](const auto&, const auto&) {
                const int selection = beautifyMode.SelectedIndex();
                if (selection < 0) return;
                UpdateBeautify(SettingsUpdateMode::PreviewAndCommit,
                    [selection](IconBeautifySettings& settings) {
                        settings.mode = selection;
                    });
            });
        glassEnabledToken = glassEnabled.Toggled([this](auto const&, auto const&) {
            const bool v = glassEnabled.IsOn();
            UpdateBeautify(SettingsUpdateMode::PreviewAndCommit, [v](auto& settings) { settings.glassEnabled = v; });
        });
        edgeReflectionToken = edgeReflection.Toggled([this](auto const&, auto const&) {
            UpdateConditionalStates();
            const bool v = edgeReflection.IsOn();
            UpdateBeautify(SettingsUpdateMode::PreviewAndCommit, [v](auto& settings) { settings.edgeHighlightEnabled = v; });
        });
        gradientEnabledToken = gradientEnabled.Toggled(
            [this](const auto&, const auto&) {
                const bool enabled = gradientEnabled.IsOn();
                UpdateBeautify(SettingsUpdateMode::PreviewAndCommit,
                    [enabled](IconBeautifySettings& settings) {
                        settings.gradientEnabled = enabled;
                    });
                UpdateConditionalStates();
            });
        gradientDirectionToken = gradientDirection.SelectionChanged(
            [this](const auto&, const auto&) {
                const int selection = gradientDirection.SelectedIndex();
                if (selection < 0) return;
                UpdateBeautify(SettingsUpdateMode::PreviewAndCommit,
                    [selection](IconBeautifySettings& settings) {
                        settings.gradientDirection = selection;
                    });
            });
        shapeToken = shape.SelectionChanged(
            [this](const auto&, const auto&) {
                const int selection = shape.SelectedIndex();
                if (selection < 0 ||
                    static_cast<std::size_t>(selection) >=
                        kBeautifyShapes.size())
                    return;
                const auto value =
                    kBeautifyShapes[static_cast<std::size_t>(selection)];
                UpdateBeautify(SettingsUpdateMode::PreviewAndCommit,
                    [value](IconBeautifySettings& settings) {
                        settings.shape = value;
                    });
            });
        filterEnabledToken = filterEnabled.Toggled(
            [this](const auto&, const auto&) {
                const bool enabled = filterEnabled.IsOn();
                UpdateBeautify(SettingsUpdateMode::PreviewAndCommit,
                    [enabled](IconBeautifySettings& settings) {
                        settings.filterEnabled = enabled;
                    });
                UpdateConditionalStates();
            });
        outlineEnabledToken = outlineEnabled.Toggled(
            [this](const auto&, const auto&) {
                const bool enabled = outlineEnabled.IsOn();
                UpdateBeautify(SettingsUpdateMode::PreviewAndCommit,
                    [enabled](IconBeautifySettings& settings) {
                        settings.outlineEnabled = enabled;
                    });
                UpdateConditionalStates();
            });
        addCategoryToken = addCategory.Click(
            [this](const auto&, const auto&) { AddCategoryRule(); });
        applyCategoryToken = applyCategory.Click(
            [this](const auto&, const auto&) {
                if (!closed && active && hasSnapshot &&
                    actions.commitCategory)
                {
                    actions.commitCategory(generation);
                }
            });
        restoreCategoryToken = restoreCategory.Click(
            [this](const auto&, const auto&) {
                UpdateCategory(SettingsUpdateMode::Commit,
                    [](CategorySettings& settings) {
                        settings = CategorySettings::Defaults();
                    });
            });
        categoryStatusTimer = mux::DispatcherTimer{};
        categoryStatusTimer.Interval(std::chrono::milliseconds(2500));
        categoryStatusTimerToken = categoryStatusTimer.Tick(
            [this](const auto&, const auto&) {
                categoryStatusTimer.Stop();
                if (!categoryDirty)
                {
                    categoryStatus.Text(L"");
                    categoryStatusRow.root.Visibility(
                        mux::Visibility::Collapsed);
                }
            });
    }

    std::wstring RuleLabel(const CategoryRule& rule) const
    {
        if (rule.id == L"programs")
            return L("widget.categories.default_program", L"Programs");
        if (!rule.customLabel.empty())
            return rule.customLabel;
        if (rule.id == L"folders")
            return L("widget.categories.folder", L"Folders");
        if (rule.id == L"videos")
            return L("widget.categories.default_video", L"Videos");
        if (rule.id == L"images")
            return L("widget.categories.default_image", L"Images");
        if (rule.id == L"documents")
            return L("widget.categories.default_document", L"Documents");
        if (rule.id == L"archives")
            return L("widget.categories.default_archive", L"Archives");
        if (rule.id == L"audio")
            return L("widget.categories.default_audio", L"Audio");
        return L("widget.categories.unnamed", L"Unnamed");
    }

    void AddCategoryRule()
    {
        std::wstring label = Trim(newCategoryLabel.Text().c_str());
        if (label.empty())
            label = L("app.settings.new_category", L"New category");
        const std::wstring extensions = newCategoryExtensions.Text().c_str();
        static std::atomic<std::uint64_t> nextId{1};
        const std::wstring id = L"custom-winui-" +
            std::to_wstring(generation) + L"-" +
            std::to_wstring(nextId.fetch_add(1));
        UpdateCategory(SettingsUpdateMode::PreviewAndCommit,
            [id, label = std::move(label), extensions](
                CategorySettings& settings) mutable {
                CategoryRule rule;
                rule.id = std::move(id);
                rule.customLabel = std::move(label);
                rule.extensions = extensions;
                settings.rules.push_back(std::move(rule));
            });
        newCategoryLabel.Text(L"");
        newCategoryExtensions.Text(L"");
    }

    void RemoveCategoryRule(std::wstring id)
    {
        if (IsBuiltinCategoryRuleId(id)) return;
        UpdateCategory(SettingsUpdateMode::PreviewAndCommit,
            [id = std::move(id)](CategorySettings& settings) {
                settings.rules.erase(
                    std::remove_if(
                        settings.rules.begin(), settings.rules.end(),
                        [&id](const CategoryRule& rule) {
                            return rule.id == id;
                        }),
                    settings.rules.end());
            });
    }

    void EditCategoryLabel(const std::wstring& id, std::wstring label)
    {
        UpdateCategory(SettingsUpdateMode::PreviewAndCommit,
            [id, label = std::move(label)](CategorySettings& settings) {
                const auto found = std::find_if(
                    settings.rules.begin(), settings.rules.end(),
                    [&id](const CategoryRule& rule) {
                        return rule.id == id;
                    });
                if (found != settings.rules.end())
                    found->customLabel = label;
            });
    }

    void EditCategoryExtensions(
        const std::wstring& id,
        std::wstring extensions)
    {
        UpdateCategory(SettingsUpdateMode::PreviewAndCommit,
            [id, extensions = std::move(extensions)](
                CategorySettings& settings) {
                const auto found = std::find_if(
                    settings.rules.begin(), settings.rules.end(),
                    [&id](const CategoryRule& rule) {
                        return rule.id == id;
                    });
                if (found != settings.rules.end())
                    found->extensions = extensions;
            });
    }

    void RestoreCategoryField(const std::wstring& id, bool extensions)
    {
        UpdateCategory(SettingsUpdateMode::PreviewAndCommit, [id, extensions](CategorySettings& settings) {
            const auto defaults = CategorySettings::Defaults();
            const auto builtin = std::find_if(defaults.rules.begin(), defaults.rules.end(),
                [&](const CategoryRule& rule) { return rule.id == id; });
            if (builtin == defaults.rules.end()) return;
            for (auto& rule : settings.rules)
            {
                if (rule.id != id) continue;
                if (extensions) rule.extensions = builtin->extensions;
                else rule.customLabel.clear();
            }
        });
    }

    void CloseRuleRows() noexcept
    {
        for (auto& row : ruleRows)
            row->Close();
        ruleRows.clear();
    }

    void BuildRuleRows(const CategorySettings& settings)
    {
        CloseRuleRows();
        categoryRulePanel.Children().Clear();
        ruleRows.reserve(settings.rules.size());
        for (const CategoryRule& rule : settings.rules)
        {
            auto row = std::make_unique<RuleRow>();
            row->id = rule.id;
            row->customLabel = rule.customLabel;
            row->root = muxc::Border{};
            row->content = muxc::StackPanel{};
            row->content.Spacing(8.0);
            row->label = muxc::TextBox{};
            row->extensions = muxc::TextBox{};
            row->remove = muxc::Button{};
            row->enabled = muxc::ToggleSwitch{};
            row->restoreName = muxc::Button{};
            row->restoreExtensions = muxc::Button{};
            row->enabled.IsOn(rule.enabled);
            row->enabled.VerticalAlignment(mux::VerticalAlignment::Center);
            row->nameActions = muxc::Grid{};
            row->nameActions.ColumnSpacing(8.0);
            muxc::ColumnDefinition labelColumn{};
            labelColumn.Width(mux::GridLengthHelper::FromValueAndType(
                1.0, mux::GridUnitType::Star));
            muxc::ColumnDefinition deleteColumn{};
            deleteColumn.Width(mux::GridLengthHelper::Auto());
            row->nameActions.ColumnDefinitions().Append(labelColumn);
            row->nameActions.ColumnDefinitions().Append(deleteColumn);
            muxc::ColumnDefinition restoreColumn{};
            restoreColumn.Width(mux::GridLengthHelper::Auto());
            row->nameActions.ColumnDefinitions().Append(restoreColumn);
            row->label.Text(RuleLabel(rule));
            if (rule.id == L"programs")
            {
                row->label.IsReadOnly(true);
                row->remove.Visibility(mux::Visibility::Collapsed);
            }
            row->extensions.Text(rule.extensions);
            row->nameActions.Children().Append(row->label);
            muxc::Grid::SetColumn(row->remove, 1);
            row->nameActions.Children().Append(row->remove);
            if (IsBuiltinCategoryRuleId(rule.id))
            {
                row->remove.Visibility(mux::Visibility::Collapsed);
                muxc::Grid::SetColumn(row->enabled, 1);
                row->nameActions.Children().Append(row->enabled);
                muxc::Grid::SetColumn(row->restoreName, 2);
                row->nameActions.Children().Append(row->restoreName);
            }
            row->labelRow.Initialize(row->nameActions);
            muxc::Grid extensionActions{};
            extensionActions.ColumnSpacing(8.0);
            muxc::ColumnDefinition extensionColumn{};
            extensionColumn.Width(mux::GridLengthHelper::FromValueAndType(1.0, mux::GridUnitType::Star));
            muxc::ColumnDefinition restoreExtensionColumn{};
            restoreExtensionColumn.Width(mux::GridLengthHelper::Auto());
            extensionActions.ColumnDefinitions().Append(extensionColumn);
            extensionActions.ColumnDefinitions().Append(restoreExtensionColumn);
            extensionActions.Children().Append(row->extensions);
            if (IsBuiltinCategoryRuleId(rule.id))
            {
                muxc::Grid::SetColumn(row->restoreExtensions, 1);
                extensionActions.Children().Append(row->restoreExtensions);
            }
            row->extensionsRow.Initialize(extensionActions);
            row->content.Children().Append(row->labelRow.root);
            row->content.Children().Append(row->extensionsRow.root);
            row->root.Child(row->content);
            categoryRulePanel.Children().Append(row->root);

            RuleRow* const raw = row.get();
            row->restoreNameToken = row->restoreName.Click([this, raw](const auto&, const auto&) {
                if (!raw->closed) RestoreCategoryField(raw->id, false);
            });
            row->restoreExtensionsToken = row->restoreExtensions.Click([this, raw](const auto&, const auto&) {
                if (!raw->closed) RestoreCategoryField(raw->id, true);
            });
            row->enabledToken = row->enabled.Toggled([this, raw](const auto&, const auto&) {
                if (updatingControls || raw->closed) return;
                const bool enabled = raw->enabled.IsOn();
                const auto id = raw->id;
                UpdateCategory(SettingsUpdateMode::PreviewAndCommit, [id, enabled](CategorySettings& settings) {
                    for (auto& rule : settings.rules) if (rule.id == id) rule.enabled = enabled;
                });
            });
            row->labelToken = row->label.TextChanged(
                [this, raw](const auto&, const auto&) {
                    if (updatingControls || raw->closed) return;
                    raw->customLabel = raw->label.Text().c_str();
                    EditCategoryLabel(raw->id, raw->label.Text().c_str());
                });
            row->extensionsToken = row->extensions.TextChanged(
                [this, raw](const auto&, const auto&) {
                    if (updatingControls || raw->closed) return;
                    EditCategoryExtensions(
                        raw->id, raw->extensions.Text().c_str());
                });
            row->removeToken = row->remove.Click(
                [this, raw](const auto&, const auto&) {
                    if (!raw->closed)
                        RemoveCategoryRule(raw->id);
                });
            ruleRows.push_back(std::move(row));
        }
        LocalizeRuleRows();
    }

    bool RuleTopologyMatches(const CategorySettings& settings) const
    {
        if (ruleRows.size() != settings.rules.size())
            return false;
        for (std::size_t index = 0; index < ruleRows.size(); ++index)
            if (ruleRows[index]->id != settings.rules[index].id)
                return false;
        return true;
    }

    void PatchRuleRows(const CategorySettings& settings)
    {
        if (!RuleTopologyMatches(settings))
        {
            BuildRuleRows(settings);
            return;
        }
        for (std::size_t index = 0; index < ruleRows.size(); ++index)
        {
            const CategoryRule& rule = settings.rules[index];
            RuleRow& row = *ruleRows[index];
            row.customLabel = rule.customLabel;
            row.enabled.IsOn(rule.enabled);
            if (row.label.FocusState() == mux::FocusState::Unfocused)
                row.label.Text(RuleLabel(rule));
            if (row.extensions.FocusState() == mux::FocusState::Unfocused)
                row.extensions.Text(rule.extensions);
        }
        LocalizeRuleRows();
    }

    void LocalizeRuleRows()
    {
        for (const auto& row : ruleRows)
        {
            // Language changes do not change the category domain revision.
            // Refresh default names with the captions, without turning their
            // translated display text into a persisted custom name.
            if ((row->id == L"programs" ||
                    (row->customLabel.empty() && IsBuiltinCategoryRuleId(row->id))) &&
                (row->id == L"programs" || row->label.FocusState() == mux::FocusState::Unfocused))
                row->label.Text(RuleLabel(CategoryRule{row->id, row->customLabel, L""}));
            row->labelRow.SetText(
                L("app.settings.category_name", L"Category name"));
            row->extensionsRow.SetText(
                L("app.settings.category_extensions", L"Extensions"));
            row->remove.Content(winrt::box_value(
                L("app.settings.delete", L"Delete")));
            const auto restore = L("app.settings.restore_default", L"Restore default");
            presenter_controls::ConfigureRestoreDefaultButton(row->restoreName,
                std::wstring((restore + L" · " + L("app.settings.category_name", L"Category name") + L" · " + row->label.Text()).c_str()));
            presenter_controls::ConfigureRestoreDefaultButton(row->restoreExtensions,
                std::wstring((restore + L" · " + L("app.settings.category_extensions", L"Extensions") + L" · " + row->label.Text()).c_str()));
            muxa::AutomationProperties::SetName(row->enabled,
                L("app.settings.widgets_enable", L"Enable") + L" " + row->label.Text());
            muxa::AutomationProperties::SetName(row->remove,
                L("app.settings.delete", L"Delete") + L" " +
                    row->label.Text());
        }
    }

    void UpdateConditionalStates()
    {
        const bool custom = beautifyPreset.SelectedIndex() == IndexOf(kBeautifyPresets, IconBeautifyPreset::Custom);
        beautifyAdvanced.Visibility(
            custom ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        edgeLightEditor->Content().Visibility(edgeReflection.IsOn() ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        glassBlurRadius->root.Visibility(glassEnabled.IsOn() ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        reflectionWidth->root.Visibility(edgeReflection.IsOn() ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        reflectionStrength->root.Visibility(edgeReflection.IsOn() ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        backgroundEnd->root.Visibility(gradientEnabled.IsOn() ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        gradientDirectionRow.root.Visibility(gradientEnabled.IsOn() ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        backgroundEnd->SetEnabled(gradientEnabled.IsOn());
        gradientDirection.IsEnabled(gradientEnabled.IsOn());
        filterDetails.Visibility(filterEnabled.IsOn()
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
        outlineDetails.Visibility(outlineEnabled.IsOn()
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
    }

    void PatchDesktop(const DesktopDisplaySettings& settings)
    {
        const std::array<int, 3> lines{settings.desktopTitleLines, settings.largeFolderTitleLines, settings.scrollingTitleLines};
        for (std::size_t i = 0; i < lines.size(); ++i) titleLineCombos[i].SelectedIndex(std::clamp(lines[i], 1, 2) - 1);
        titleOverflow.SelectedIndex(settings.titleEllipsis ? 0 : 1);
        iconSpacing->SetValue(settings.iconSpacingScale * 100.0);
        iconSize->SetValue(settings.itemIconSizeScale * 100.0);
        itemFontSize->SetValue(settings.itemFontSizeCu);
        listFontSize->SetValue(settings.listItemFontSizeCu);
        itemFontWeight->SetValue(font_weight_rules::ToPercent(settings.itemFontWeight));
        shortcutArrow.SelectedIndex(
            std::clamp(settings.shortcutArrowMode, 0, 2));

        const IconBeautifySettings& value = settings.iconBeautify;
        if (value.preset == IconBeautifyPreset::Custom &&
            beautifyPreset.SelectedIndex() != IndexOf(kBeautifyPresets, IconBeautifyPreset::Custom))
            beautifySections.CollapseAll();
        beautifyPreset.SelectedIndex(IndexOf(kBeautifyPresets, value.preset));
        beautifyMode.SelectedIndex(std::clamp(value.mode, 0, 1));
        backgroundStart->SetColor(MakeColor(
            value.backgroundStartR,
            value.backgroundStartG,
            value.backgroundStartB));
        backgroundOpacity->SetValue(value.backgroundOpacity * 100.0);
        glassEnabled.IsOn(value.glassEnabled); edgeReflection.IsOn(value.edgeHighlightEnabled);
        glassBlurRadius->SetValue(value.glassBlurRadius);
        edgeLightEditor->SetValue(value.edgeLight);
        reflectionWidth->SetValue(value.edgeHighlightWidth);
        reflectionStrength->SetValue(value.edgeHighlightStrength * 100.0);
        gradientEnabled.IsOn(value.gradientEnabled);
        backgroundEnd->SetColor(MakeColor(
            value.backgroundEndR,
            value.backgroundEndG,
            value.backgroundEndB));
        gradientDirection.SelectedIndex(
            std::clamp(value.gradientDirection, 0, 3));
        shape.SelectedIndex(IndexOf(kBeautifyShapes, value.shape));
        contentScale->SetValue(value.contentScale * 100.0);
        highlightStrength->SetValue(
            value.textureHighlightStrength * 100.0);
        highlightSize->SetValue(value.textureHighlightSize * 100.0);
        highlightAngle->SetValue(value.textureHighlightAngle * 45.0);
        shadeStrength->SetValue(value.textureShadeStrength * 100.0);
        edgeHighlight->SetValue(value.textureEdgeHighlight * 100.0);
        filterEnabled.IsOn(value.filterEnabled);
        filterTint->SetColor(MakeColor(
            value.filterTintR, value.filterTintG, value.filterTintB));
        filterStrength->SetValue(value.filterStrength * 100.0);
        shadowStrength->SetValue(value.shadowStrength * 100.0);
        outlineEnabled.IsOn(value.outlineEnabled);
        outlineWidth->SetValue(value.outlineWidth);
        outlineOpacity->SetValue(value.outlineOpacity * 100.0);
        outlineColor->SetColor(MakeColor(
            value.outlineR, value.outlineG, value.outlineB));
    }

    [[nodiscard]] bool HasActiveDesktopEdit() const noexcept
    {
        for (const NumericEditor* editor : {
                 iconSpacing.get(), iconSize.get(), itemFontSize.get(),
                 listFontSize.get(), itemFontWeight.get(),
                 backgroundOpacity.get(), glassBlurRadius.get(), reflectionWidth.get(), reflectionStrength.get(), contentScale.get(),
                 highlightStrength.get(), highlightSize.get(),
                 highlightAngle.get(), shadeStrength.get(),
                 edgeHighlight.get(), filterStrength.get(),
                 shadowStrength.get(), outlineWidth.get(),
                 outlineOpacity.get()})
        {
            if (editor->IsEditing()) return true;
        }
        for (const ColorEditor* editor : {
                 backgroundStart.get(), backgroundEnd.get(),
                 filterTint.get(), outlineColor.get()})
        {
            if (editor->IsEditing()) return true;
        }
        return false;
    }

    void PatchCategory(const CategorySettings& settings)
    {
        collectPrograms.IsOn(settings.collectProgramsEnabled);
        PatchRuleRows(settings);
    }

    void PatchPersonalization(const PersonalizationSettings& settings)
    {
        showCategoryTabCounts.IsOn(settings.showCategoryTabCounts);
    }

    std::vector<NumericEditor*> ContinuousNumbers() const
    {
        return {
            iconSpacing.get(), iconSize.get(), itemFontSize.get(),
            listFontSize.get(), itemFontWeight.get(),
            backgroundOpacity.get(), glassBlurRadius.get(), reflectionWidth.get(), reflectionStrength.get(), contentScale.get(),
            highlightStrength.get(), highlightSize.get(),
            highlightAngle.get(), shadeStrength.get(), edgeHighlight.get(),
            filterStrength.get(), shadowStrength.get(), outlineWidth.get(),
            outlineOpacity.get(),
        };
    }

    std::vector<ColorEditor*> ContinuousColors() const
    {
        return {
            backgroundStart.get(), backgroundEnd.get(), filterTint.get(),
            outlineColor.get(),
        };
    }

    void CommitContinuousEdits()
    {
        if (edgeLightEditor) edgeLightEditor->Flush();
        for (NumericEditor* editor : ContinuousNumbers())
            editor->CommitPending();
        for (ColorEditor* editor : ContinuousColors())
            editor->CommitPending();
    }

    void CancelContinuousEdits() noexcept
    {
        if (edgeLightEditor) edgeLightEditor->Cancel();
        for (NumericEditor* editor : ContinuousNumbers())
            editor->CancelPending();
        for (ColorEditor* editor : ContinuousColors())
            editor->CancelPending();
    }

    void ApplySnapshot(const SettingsSnapshot& snapshot)
    {
        if (closed) return;
        const bool newGeneration =
            !hasSnapshot || snapshot.generation != generation;
        if (newGeneration)
        {
            CancelContinuousEdits();
            categoryStatusTimer.Stop();
            categoryDirty = false;
            categoryDirtyKnown = false;
        }
        generation = snapshot.generation;
        const bool desktopEditActive = HasActiveDesktopEdit();
        const bool wasUpdating = updatingControls;
        updatingControls = true;

        if (newGeneration ||
            snapshot.domainRevisions.desktop != desktopRevision)
        {
            // A local continuous edit already owns the authoritative display
            // value. Do not patch any sibling control in the same XAML tree
            // until release/focus/idle commit publishes the next revision.
            if (newGeneration || !desktopEditActive)
            {
                PatchDesktop(snapshot.values.desktop);
                desktopRevision = snapshot.domainRevisions.desktop;
            }
        }
        if (newGeneration ||
            snapshot.domainRevisions.category != categoryRevision)
        {
            PatchCategory(snapshot.values.category);
            categoryRevision = snapshot.domainRevisions.category;
        }
        if (newGeneration ||
            snapshot.domainRevisions.personalization !=
                personalizationRevision)
        {
            PatchPersonalization(snapshot.values.personalization);
            personalizationRevision =
                snapshot.domainRevisions.personalization;
        }
        const bool nextCategoryDirty = HasSettingsDomain(
            snapshot.dirtyDomains, SettingsDomain::Category);
        if (nextCategoryDirty)
        {
            categoryStatusTimer.Stop();
            categoryStatus.Text(
                L("app.settings.save_unsaved", L"Unsaved changes"));
            categoryStatusRow.root.Visibility(mux::Visibility::Visible);
        }
        else if (categoryDirtyKnown && categoryDirty)
        {
            categoryStatus.Text(L("app.settings.saved", L"Saved"));
            categoryStatusRow.root.Visibility(mux::Visibility::Visible);
            categoryStatusTimer.Stop();
            categoryStatusTimer.Start();
        }
        else if (!categoryStatusTimer.IsEnabled())
        {
            categoryStatus.Text(L"");
            categoryStatusRow.root.Visibility(mux::Visibility::Collapsed);
        }
        categoryDirty = nextCategoryDirty;
        categoryDirtyKnown = true;
        hasSnapshot = true;
        if (!desktopEditActive)
            UpdateConditionalStates();
        updatingControls = wasUpdating;
    }

    void SetCardText(
        SettingsCard& card,
        std::string_view key,
        std::wstring_view fallback)
    {
        card.title.Text(L(key, fallback));
        muxa::AutomationProperties::SetName(card.root, card.title.Text());
    }

    void RefreshLocalizedText()
    {
        if (closed) return;
        const bool wasUpdating = updatingControls;
        updatingControls = true;

        SetCardText(displayCard, "app.settings.desktop_icons",
            L"Desktop Icons");
        SetCardText(categoryLayoutCard, "settings.desktop.categoryLayout",
            L"Category tab layout");
        SetCardText(beautifyCard, "app.settings.icon_beautify",
            L"Icon beautification");
        SetCardText(categoryRulesCard, "app.settings.category_rules",
            L"Category rules");

        iconSpacing->SetLabel(L(
            "app.settings.layout_spacing", L"Layout spacing"), L(
            "app.settings.layout_spacing_hint"));
        iconSize->SetLabel(L("app.settings.icon_size", L"Icon size"), L(
            "app.settings.icon_size_hint"));
        itemFontSize->SetLabel(L(
            "app.settings.title_font_size", L"Title font size"));
        listFontSize->SetLabel(L(
            "app.settings.list_font_size", L"List font size"));
        const std::array<const char*, 3> lineKeys{"titleLines.desktop", "titleLines.largeFolder", "titleLines.scrolling"};
        for (std::size_t i = 0; i < titleLineCombos.size(); ++i)
        {
            const int selected = titleLineCombos[i].SelectedIndex();
            titleLineRows[i].SetText(L(lineKeys[i]));
            SetComboItems(titleLineCombos[i], {L("titleLines.one"), L("titleLines.two")}, selected);
            titleLineCombos[i].SelectedIndex(selected);
            muxa::AutomationProperties::SetName(titleLineCombos[i], titleLineRows[i].label.Text());
        }
        const int overflowSelected = titleOverflow.SelectedIndex();
        titleOverflowRow.SetText(L("titleOverflow.title"), L("titleOverflow.hint"));
        SetComboItems(titleOverflow, {L("titleOverflow.ellipsis"), L("titleOverflow.clip")}, overflowSelected);
        titleOverflow.SelectedIndex(overflowSelected);
        muxa::AutomationProperties::SetName(titleOverflow, titleOverflowRow.label.Text());
        itemFontWeight->SetLabel(L(
            "app.settings.title_font_weight", L"Title font weight"));
        for (NumericEditor* editor : {iconSpacing.get(), iconSize.get(),
                 itemFontSize.get(), listFontSize.get(),
                 itemFontWeight.get()})
        {
            editor->SetResetText(
                L("app.settings.restore_default", L"Restore Default"));
        }
        shortcutArrowRow.SetText(
            L("app.settings.shortcut_arrow", L"Shortcut arrow"));
        muxa::AutomationProperties::SetName(
            shortcutArrow, shortcutArrowRow.label.Text());
        SetComboItems(shortcutArrow, {
            L("app.settings.arrow_default", L"System default"),
            L("app.settings.arrow_hide_all", L"Hide all"),
            L("app.settings.arrow_show_all", L"Show all"),
        }, std::max(0, shortcutArrow.SelectedIndex()));

        showCategoryTabCountsRow.SetText(L(
            "app.settings.category_show_count", L"Show item counts"));
        muxa::AutomationProperties::SetName(showCategoryTabCounts,
            showCategoryTabCountsRow.label.Text());

        beautifyPresetRow.SetText(
            L("app.settings.icon_beautify", L"Icon beautification"));
        muxa::AutomationProperties::SetName(
            beautifyPreset, beautifyPresetRow.label.Text());
        SetComboItems(beautifyPreset, {
            L("app.settings.beautify_preset_none", L"None"),
            L("app.settings.beautify_preset_default", L"Default"),
            L("app.settings.transparent_glass", L"Transparent glass"),
            L("app.settings.dark_glass", L"Dark glass"),
            L("app.settings.light_glass", L"Light glass"),
            L("app.settings.custom", L"Custom"),
        }, std::max(0, beautifyPreset.SelectedIndex()));
        beautifyModeRow.SetText(
            L("app.settings.beautify_mode", L"Beautification mode"));
        muxa::AutomationProperties::SetName(
            beautifyMode, beautifyModeRow.label.Text());
        SetComboItems(beautifyMode, {
            L("app.settings.beautify_smart", L"Smart"),
            L("app.settings.beautify_shrink_bg", L"Shrink background"),
        }, std::max(0, beautifyMode.SelectedIndex()));
        backgroundStart->SetLabel(
            L("app.settings.default_bg", L"Background color"),
            L("app.settings.cancel", L"Cancel"));
        backgroundOpacity->SetLabel(
            L("app.settings.bg_opacity_val", L"Background opacity"));
        gradientEnabledRow.SetText(L(
            "app.settings.enable_gradient_bg", L"Enable gradient"));
        muxa::AutomationProperties::SetName(gradientEnabled,
            gradientEnabledRow.label.Text());
        backgroundEnd->SetLabel(L(
            "app.settings.gradient_end_color", L"Gradient end color"),
            L("app.settings.cancel", L"Cancel"));
        gradientDirectionRow.SetText(
            L("app.settings.beautify_gradient_dir", L"Gradient direction"));
        muxa::AutomationProperties::SetName(
            gradientDirection, gradientDirectionRow.label.Text());
        SetComboItems(gradientDirection, {
            L("app.settings.beautify_gradient_updown", L"Top to bottom"),
            L("app.settings.beautify_gradient_leftright", L"Left to right"),
            L("app.settings.beautify_gradient_topleft_bottomright",
                L"Top-left to bottom-right"),
            L("app.settings.beautify_gradient_bottomleft_topright",
                L"Bottom-left to top-right"),
        }, std::max(0, gradientDirection.SelectedIndex()));
        shapeRow.SetText(L("app.settings.beautify_shape", L"Shape"));
        muxa::AutomationProperties::SetName(shape, shapeRow.label.Text());
        SetComboItems(shape, {
            L("app.settings.beautify_shape_legacy", L"Rounded"),
            L("app.settings.beautify_shape_continuous_rounded",
                L"Continuous rounded"),
            L("app.settings.beautify_shape_soft_rounded", L"Soft rounded"),
            L("app.settings.beautify_shape_circle", L"Circle"),
            L("app.settings.beautify_shape_pebble", L"Pebble"),
        }, std::max(0, shape.SelectedIndex()));
        geometryTitle.Text(L("appearance.iconGeometry")); finishTitle.Text(L("appearance.iconFinish"));
        beautifySections.RefreshLocalizedText([this](auto key) { return L(key); });
        edgeLightEditor->RefreshLocalizedText();
        glassEnabledRow.SetText(L("app.settings.glass_enabled", L"Frosted glass background"));
        edgeReflectionRow.SetText(L("app.settings.edge_highlight", L"Edge highlight"));
        muxa::AutomationProperties::SetName(glassEnabled, glassEnabledRow.label.Text());
        muxa::AutomationProperties::SetName(edgeReflection, edgeReflectionRow.label.Text());
        glassBlurRadius->SetLabel(L("app.settings.blur_radius", L"Blur radius"));
        reflectionWidth->SetLabel(L("app.settings.edge_highlight_width", L"Edge highlight width"));
        reflectionStrength->SetLabel(L("app.settings.edge_highlight_strength", L"Edge highlight strength"));
        contentScale->SetLabel(L(
            "app.settings.beautify_content_scale", L"Content scale"));
        highlightStrength->SetLabel(L(
            "app.settings.beautify_texture_highlight",
            L"Texture highlight"));
        highlightSize->SetLabel(L(
            "app.settings.beautify_texture_highlight_size",
            L"Highlight size"));
        highlightAngle->SetLabel(L(
            "app.settings.beautify_texture_highlight_angle",
            L"Highlight angle"));
        shadeStrength->SetLabel(L(
            "app.settings.beautify_texture_shade", L"Texture shade"));
        edgeHighlight->SetLabel(L(
            "app.settings.beautify_texture_edge", L"Edge highlight"));
        filterEnabledRow.SetText(
            L("app.settings.beautify_filter", L"Enable color filter"));
        muxa::AutomationProperties::SetName(filterEnabled,
            filterEnabledRow.label.Text());
        filterTint->SetLabel(L(
            "app.settings.beautify_filter_color", L"Filter color"),
            L("app.settings.cancel", L"Cancel"));
        filterStrength->SetLabel(L(
            "app.settings.beautify_filter_strength", L"Filter strength"));
        shadowStrength->SetLabel(L(
            "app.settings.beautify_shadow_strength", L"Shadow strength"));
        outlineEnabledRow.SetText(
            L("app.settings.beautify_outline", L"Enable outline"));
        muxa::AutomationProperties::SetName(outlineEnabled,
            outlineEnabledRow.label.Text());
        outlineWidth->SetLabel(L(
            "app.settings.beautify_outline_width", L"Outline width"));
        outlineOpacity->SetLabel(L(
            "app.settings.beautify_outline_opacity", L"Outline opacity"));
        outlineColor->SetLabel(L(
            "app.settings.beautify_outline_color", L"Outline color"),
            L("app.settings.cancel", L"Cancel"));

        categoryHint.Text(L("app.settings.category_hint",
            L"Changes to category rules are applied explicitly."));
        collectProgramsRow.SetText(
            L("app.settings.collect_programs", L"Collect programs"),
            L("app.settings.collect_programs_hint", L"Allow file category widgets to collect programs and shortcuts. Off by default."));
        muxa::AutomationProperties::SetName(collectPrograms, collectProgramsRow.label.Text());
        categoryTypesHeading.Text(
            L("app.settings.category_type", L"Category type"));
        addCategoryHeading.Text(
            L("app.settings.add_category", L"Add category type"));
        saveCategoryHeading.Text(
            L("app.settings.save_settings", L"Save settings"));
        newCategoryLabelRow.SetText(
            L("app.settings.category_name", L"Category name"));
        newCategoryExtensionsRow.SetText(
            L("app.settings.category_extensions", L"Extensions"));
        addCategory.Content(winrt::box_value(
            L("app.settings.add", L"Add")));
        applyCategory.Content(winrt::box_value(
            L("app.settings.apply", L"Apply")));
        restoreCategory.Content(winrt::box_value(
            L("app.settings.restore_default", L"Restore defaults")));
        categoryActionsRow.SetText(
            L("app.settings.category_rules", L"Category rules"));
        categoryStatusRow.SetText(
            L("app.settings.save_status", L"Save status"));
        muxa::AutomationProperties::SetName(addCategory,
            L("app.settings.add_category", L"Add category"));
        muxa::AutomationProperties::SetName(applyCategory,
            L("app.settings.apply", L"Apply"));
        muxa::AutomationProperties::SetName(restoreCategory,
            L("app.settings.restore_default", L"Restore defaults"));
        if (categoryDirty)
            categoryStatus.Text(
                L("app.settings.save_unsaved", L"Unsaved changes"));
        else if (categoryStatusTimer.IsEnabled())
            categoryStatus.Text(L("app.settings.saved", L"Saved"));
        LocalizeRuleRows();
        UpdateConditionalStates();
        updatingControls = wasUpdating;
    }

    mux::FrameworkElement FocusTarget(std::string_view id) const noexcept
    {
        const auto appearanceTarget = [this](mux::FrameworkElement target) {
            if (beautifyPreset.SelectedIndex() != IndexOf(kBeautifyPresets, IconBeautifyPreset::Custom))
                return mux::FrameworkElement{beautifyPreset};
            beautifySections.Reveal(target);
            return target;
        };
        if (id == "desktop.spacing" || id == "desktop.iconSpacing")
            return iconSpacing->slider;
        if (id == "desktop.iconSize") return iconSize->slider;
        if (id == "desktop.itemFontSize") return itemFontSize->number;
        if (id == "desktop.listFontSize") return listFontSize->number;
        if (id == "desktop.titleLines") return titleLineCombos[0];
        if (id == "desktop.largeFolderTitleLines") return titleLineCombos[1];
        if (id == "desktop.scrollingTitleLines") return titleLineCombos[2];
        if (id == "desktop.titleOverflow") return titleOverflow;
        if (id == "desktop.fontWeight") return itemFontWeight->number;
        if (id == "desktop.shortcutArrow") return shortcutArrow;
        if (id == "desktop.categoryCounts") return showCategoryTabCounts;
        if (id == "desktop.collectPrograms") return collectPrograms;
        if (id == "desktop.iconBeautify" ||
            id == "desktop.iconBeautify.preset")
            return beautifyPreset;
        if (id == "desktop.iconBeautify.mode") return appearanceTarget(beautifyMode);
        if (id == "desktop.iconBeautify.backgroundColor")
            return appearanceTarget(backgroundStart->editor.button);
        if (id == "desktop.iconBeautify.glass") return appearanceTarget(glassEnabled);
        if (id == "desktop.iconBeautify.blurRadius") return appearanceTarget(glassBlurRadius->slider);
        if (id == "desktop.iconBeautify.edgeReflection") return appearanceTarget(edgeReflection);
        if (id == "desktop.iconBeautify.reflectionWidth") return appearanceTarget(reflectionWidth->slider);
        if (id == "desktop.iconBeautify.reflectionStrength") return appearanceTarget(reflectionStrength->slider);
        if (id == "desktop.iconBeautify.backgroundOpacity")
            return appearanceTarget(backgroundOpacity->slider);
        if (id == "desktop.iconBeautify.gradient")
            return appearanceTarget(gradientEnabled);
        if (id == "desktop.iconBeautify.gradientEndColor")
            return appearanceTarget(backgroundEnd->editor.button);
        if (id == "desktop.iconBeautify.gradientDirection")
            return appearanceTarget(gradientDirection);
        if (id == "desktop.iconBeautify.shape") return appearanceTarget(shape);
        if (id == "desktop.iconBeautify.contentScale")
            return appearanceTarget(contentScale->slider);
        if (id == "desktop.iconBeautify.highlightStrength")
            return appearanceTarget(highlightStrength->slider);
        if (id == "desktop.iconBeautify.highlightSize")
            return appearanceTarget(highlightSize->slider);
        if (id == "desktop.iconBeautify.highlightAngle")
            return appearanceTarget(highlightAngle->slider);
        if (id == "desktop.iconBeautify.shadeStrength")
            return appearanceTarget(shadeStrength->slider);
        if (id == "desktop.iconBeautify.edgeHighlight")
            return appearanceTarget(edgeHighlight->slider);
        if (id == "desktop.iconBeautify.filter") return appearanceTarget(filterEnabled);
        if (id == "desktop.iconBeautify.filterColor")
            return appearanceTarget(filterTint->editor.button);
        if (id == "desktop.iconBeautify.filterStrength")
            return appearanceTarget(filterStrength->slider);
        if (id == "desktop.iconBeautify.shadowStrength")
            return appearanceTarget(shadowStrength->slider);
        if (id == "desktop.iconBeautify.outline") return appearanceTarget(outlineEnabled);
        if (id == "desktop.iconBeautify.outlineWidth")
            return appearanceTarget(outlineWidth->slider);
        if (id == "desktop.iconBeautify.outlineOpacity")
            return appearanceTarget(outlineOpacity->slider);
        if (id == "desktop.iconBeautify.outlineColor")
            return appearanceTarget(outlineColor->editor.button);
        if (id == "desktop.categories" || id == "desktop.categoryRules")
            return applyCategory;
        if (id == "desktop.category.add") return newCategoryLabel;
        return nullptr;
    }

    void CloseEditors() noexcept
    {
        if (edgeLightEditor) edgeLightEditor->Close();
        iconSpacing->Close();
        iconSize->Close();
        itemFontSize->Close();
        listFontSize->Close();
        itemFontWeight->Close();
        for (std::size_t i = 0; i < titleLineCombos.size(); ++i) titleLineCombos[i].SelectionChanged(titleLineTokens[i]);
        titleOverflow.SelectionChanged(titleOverflowToken);
        backgroundStart->Close();
        backgroundOpacity->Close();
        glassBlurRadius->Close(); reflectionWidth->Close(); reflectionStrength->Close();
        backgroundEnd->Close();
        contentScale->Close();
        highlightStrength->Close();
        highlightSize->Close();
        highlightAngle->Close();
        shadeStrength->Close();
        edgeHighlight->Close();
        filterTint->Close();
        filterStrength->Close();
        shadowStrength->Close();
        outlineWidth->Close();
        outlineOpacity->Close();
        outlineColor->Close();
    }

    void CommitOpenColorEditors() noexcept
    {
        backgroundStart->Dismiss();
        backgroundEnd->Dismiss();
        filterTint->Dismiss();
        outlineColor->Dismiss();
    }

    void Close() noexcept
    {
        if (closed) return;
        CommitOpenColorEditors();
        if (active)
            CommitContinuousEdits();
        active = false;
        closed = true;
        CloseRuleRows();
        CloseEditors();
        try
        {
            if (categoryStatusTimer)
            {
                categoryStatusTimer.Stop();
                categoryStatusTimer.Tick(categoryStatusTimerToken);
            }
            shortcutArrow.SelectionChanged(shortcutArrowToken);
            showCategoryTabCounts.Toggled(showCountsToken);
            collectPrograms.Toggled(collectProgramsToken);
            beautifyPreset.SelectionChanged(beautifyPresetToken);
            beautifyMode.SelectionChanged(beautifyModeToken);
            gradientEnabled.Toggled(gradientEnabledToken);
            glassEnabled.Toggled(glassEnabledToken); edgeReflection.Toggled(edgeReflectionToken);
            gradientDirection.SelectionChanged(gradientDirectionToken);
            shape.SelectionChanged(shapeToken);
            filterEnabled.Toggled(filterEnabledToken);
            outlineEnabled.Toggled(outlineEnabledToken);
            addCategory.Click(addCategoryToken);
            applyCategory.Click(applyCategoryToken);
            restoreCategory.Click(restoreCategoryToken);
        }
        catch (...)
        {
        }
        actions = {};
        localize = {};
    }
};

DesktopPagePresenter::DesktopPagePresenter(
    LocalizeCallback localize,
    const mux::Style& cardStyle)
    : impl_(std::make_unique<Impl>(std::move(localize), cardStyle))
{
}

DesktopPagePresenter::~DesktopPagePresenter()
{
    Close();
}

void DesktopPagePresenter::SetActions(DesktopPageActions actions)
{
    if (impl_ && !impl_->closed)
        impl_->actions = std::move(actions);
}

mux::FrameworkElement DesktopPagePresenter::Content() const noexcept
{
    return impl_ ? impl_->categoryRoot : nullptr;
}

mux::FrameworkElement
DesktopPagePresenter::LayoutSpacingContent() const noexcept
{
    return impl_ ? impl_->iconSpacing->root : nullptr;
}

mux::FrameworkElement
DesktopPagePresenter::DesktopIconsContent() const noexcept
{
    return impl_ ? impl_->desktopIconsRoot : nullptr;
}

mux::FrameworkElement
DesktopPagePresenter::IconBeautificationContent() const noexcept
{
    return impl_ ? impl_->iconBeautificationRoot : nullptr;
}

mux::FrameworkElement
DesktopPagePresenter::CategoryContent() const noexcept
{
    return impl_ ? impl_->categoryRoot : nullptr;
}

void DesktopPagePresenter::ApplySnapshot(const SettingsSnapshot& snapshot)
{
    if (impl_) impl_->ApplySnapshot(snapshot);
}

void DesktopPagePresenter::RefreshLocalizedText()
{
    if (impl_) impl_->RefreshLocalizedText();
}

mux::FrameworkElement DesktopPagePresenter::FocusTarget(
    std::string_view focusId) const noexcept
{
    return impl_ ? impl_->FocusTarget(focusId) : nullptr;
}

void DesktopPagePresenter::Activate() noexcept
{
    if (impl_ && !impl_->closed)
        impl_->active = true;
}

void DesktopPagePresenter::Deactivate() noexcept
{
    if (!impl_ || impl_->closed || !impl_->active) return;
    impl_->CommitOpenColorEditors();
    impl_->CommitContinuousEdits();
    impl_->active = false;
}

void DesktopPagePresenter::Close() noexcept
{
    if (impl_) impl_->Close();
}

} // namespace snowdesktop::winui

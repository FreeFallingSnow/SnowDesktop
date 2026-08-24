#include "pch.h"

#include "desktop_page_presenter.h"
#include "settings_presenter_controls.h"

#include "../constants.h"
#include "../icon_beautify.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.System.h>

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

constexpr std::array<IconBeautifyPreset, 3> kBeautifyPresets = {
    IconBeautifyPreset::None,
    IconBeautifyPreset::DefaultBeautify,
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
    ChangedCallback changed;
    double defaultValue = std::numeric_limits<double>::quiet_NaN();
    bool updating = false;
    bool pendingCommit = false;
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
        : changed(std::move(callback)), defaultValue(resetValue)
    {
        editors = muxc::Grid{};
        editors.ColumnSpacing(12.0);
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
        number.Width(124.0);
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

        sliderValueToken = slider.ValueChanged(
            [this](const auto&, const auto&) {
                if (updating || closed) return;
                updating = true;
                number.Value(slider.Value());
                updating = false;
                PublishPreview(slider.Value());
            });
        numberValueToken = number.ValueChanged(
            [this](const auto&, const auto&) {
                if (updating || closed || std::isnan(number.Value())) return;
                updating = true;
                slider.Value(number.Value());
                updating = false;
                PublishPreview(number.Value());
            });

        const auto pointerReleased = [this](const auto&, const auto&) {
            CommitPending();
        };
        const auto lostFocus = [this](const auto&, const auto&) {
            CommitPending();
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
                pendingCommit = false;
                if (changed)
                    changed(defaultValue,
                        SettingsUpdateMode::PreviewAndCommit);
            });
        }
    }

    void PublishPreview(double value)
    {
        pendingCommit = true;
        idleCommitTimer.Stop();
        idleCommitTimer.Start();
        if (changed)
            changed(value, SettingsUpdateMode::Preview);
    }

    void CommitPending()
    {
        if (idleCommitTimer)
            idleCommitTimer.Stop();
        if (!pendingCommit || closed) return;
        pendingCommit = false;
        if (changed)
            changed(slider.Value(), SettingsUpdateMode::PreviewAndCommit);
    }

    void CancelPending() noexcept
    {
        if (idleCommitTimer)
            idleCommitTimer.Stop();
        pendingCommit = false;
    }

    void SetValue(double value)
    {
        if (closed) return;
        value = std::clamp(value, slider.Minimum(), slider.Maximum());
        const bool wasUpdating = updating;
        updating = true;
        slider.Value(value);
        number.Value(value);
        updating = wasUpdating;
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
        reset.Content(winrt::box_value(text));
        muxa::AutomationProperties::SetName(reset, text);
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

    void SetLabel(
        std::wstring text,
        std::wstring applyText,
        std::wstring cancelText)
    {
        editor.SetText(std::move(text), {},
            std::move(applyText), std::move(cancelText));
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
    muxc::Border root{nullptr};
    muxc::StackPanel content{nullptr};
    muxc::Grid nameActions{nullptr};
    SettingRow labelRow;
    SettingRow extensionsRow;
    muxc::TextBox label{nullptr};
    muxc::TextBox extensions{nullptr};
    muxc::Button remove{nullptr};
    winrt::event_token labelToken{};
    winrt::event_token extensionsToken{};
    winrt::event_token removeToken{};
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
    muxc::StackPanel appearanceRoot{nullptr};
    muxc::StackPanel categoryRoot{nullptr};
    muxc::StackPanel hiddenCompatibilityRoot{nullptr};

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

    std::unique_ptr<NumericEditor> categorizedTabHeight;
    std::unique_ptr<NumericEditor> tabFontSize;
    muxc::ToggleSwitch showCategoryTabCounts{nullptr};
    SettingRow showCategoryTabCountsRow;

    muxc::TextBlock beautifyPresetLabel{nullptr};
    muxc::ComboBox beautifyPreset{nullptr};
    SettingRow beautifyPresetRow;
    muxc::ToggleSwitch beautifyEnabled{nullptr};
    SettingRow beautifyEnabledRow;
    muxc::StackPanel beautifyAdvanced{nullptr};
    muxc::TextBlock beautifyModeLabel{nullptr};
    muxc::ComboBox beautifyMode{nullptr};
    SettingRow beautifyModeRow;
    std::unique_ptr<ColorEditor> backgroundStart;
    std::unique_ptr<NumericEditor> backgroundOpacity;
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
    winrt::event_token beautifyPresetToken{};
    winrt::event_token beautifyEnabledToken{};
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
        appearanceRoot = muxc::StackPanel{};
        appearanceRoot.Spacing(8.0);
        categoryRoot = muxc::StackPanel{};
        categoryRoot.Spacing(8.0);
        hiddenCompatibilityRoot = muxc::StackPanel{};
        hiddenCompatibilityRoot.Spacing(8.0);
        root = categoryRoot;

        InitializeCard(displayCard, cardStyle, appearanceRoot);
        iconSpacing = MakeDesktopNumber(50.0, 200.0, 1.0, 0,
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
        itemFontWeight = MakeDesktopNumber(100.0, 900.0, 50.0, 0,
            [](DesktopDisplaySettings& settings, double value) {
                settings.itemFontWeight = static_cast<int>(std::lround(value));
            }, 600.0);
        iconSpacing->SetUnit(L"%");
        iconSize->SetUnit(L"%");
        itemFontSize->SetUnit(L"cu");
        listFontSize->SetUnit(L"cu");
        for (const auto* editor : {iconSpacing.get(), iconSize.get(),
                 itemFontSize.get(), listFontSize.get(), itemFontWeight.get()})
        {
            displayCard.content.Children().Append(editor->root);
        }
        AppendCombo(displayCard, shortcutArrowRow, shortcutArrow);

        // These post-migration Category layout additions remain synchronized
        // for route compatibility, but are not part of the legacy surface.
        InitializeCard(categoryLayoutCard, cardStyle, hiddenCompatibilityRoot);
        categorizedTabHeight = std::make_unique<NumericEditor>(
            24.0, 48.0, 1.0, 0,
            [this](double value, SettingsUpdateMode mode) {
                UpdatePersonalization(mode,
                    [value](PersonalizationSettings& settings) {
                        settings.categorizedTabHeight =
                            static_cast<float>(value);
                    });
            });
        tabFontSize = std::make_unique<NumericEditor>(
            10.0, 22.0, 0.5, 1,
            [this](double value, SettingsUpdateMode) {
                UpdateCategory(SettingsUpdateMode::Draft,
                    [value](CategorySettings& settings) {
                        settings.tabFontSize = static_cast<float>(value);
                    });
            });
        categorizedTabHeight->SetUnit(L"cu");
        tabFontSize->SetUnit(L"cu");
        showCategoryTabCounts = muxc::ToggleSwitch{};
        showCategoryTabCounts.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        showCategoryTabCountsRow.Initialize(showCategoryTabCounts);
        showCategoryTabCountsRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        categoryLayoutCard.content.Children().Append(
            categorizedTabHeight->root);
        categoryLayoutCard.content.Children().Append(tabFontSize->root);
        categoryLayoutCard.content.Children().Append(
            showCategoryTabCountsRow.root);

        InitializeCard(beautifyCard, cardStyle, appearanceRoot);
        AppendCombo(beautifyCard, beautifyPresetRow, beautifyPreset);
        beautifyEnabled = muxc::ToggleSwitch{};
        beautifyEnabled.HorizontalAlignment(mux::HorizontalAlignment::Right);
        beautifyEnabledRow.Initialize(beautifyEnabled);
        beautifyEnabledRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        beautifyCard.content.Children().Append(beautifyEnabledRow.root);
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
        categoryActions.Children().Append(applyCategory);
        categoryActions.Children().Append(restoreCategory);
        categoryActionsRow.Initialize(categoryActions);
        categoryActionsRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        categoryRulesCard.content.Children().Append(saveCategoryHeading);
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
        beautifyPresetToken = beautifyPreset.SelectionChanged(
            [this](const auto&, const auto&) {
                const int selection = beautifyPreset.SelectedIndex();
                if (selection < 0 ||
                    static_cast<std::size_t>(selection) >=
                        kBeautifyPresets.size())
                    return;
                const IconBeautifyPreset preset =
                    kBeautifyPresets[static_cast<std::size_t>(selection)];
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
        beautifyEnabledToken = beautifyEnabled.Toggled(
            [this](const auto&, const auto&) {
                const bool enabled = beautifyEnabled.IsOn();
                UpdateDesktop(SettingsUpdateMode::PreviewAndCommit,
                    [enabled](DesktopDisplaySettings& desktop) {
                        desktop.iconBeautify.enabled = enabled;
                        desktop.iconBeautify.preset =
                            IconBeautifyPreset::Custom;
                    });
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
        if (!rule.customLabel.empty())
            return rule.customLabel;
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
        UpdateCategory(SettingsUpdateMode::Draft,
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
        UpdateCategory(SettingsUpdateMode::Draft,
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
        UpdateCategory(SettingsUpdateMode::Draft,
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
        UpdateCategory(SettingsUpdateMode::Draft,
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
            row->root = muxc::Border{};
            row->content = muxc::StackPanel{};
            row->content.Spacing(8.0);
            row->label = muxc::TextBox{};
            row->extensions = muxc::TextBox{};
            row->remove = muxc::Button{};
            row->nameActions = muxc::Grid{};
            row->nameActions.ColumnSpacing(8.0);
            muxc::ColumnDefinition labelColumn{};
            labelColumn.Width(mux::GridLengthHelper::FromValueAndType(
                1.0, mux::GridUnitType::Star));
            muxc::ColumnDefinition deleteColumn{};
            deleteColumn.Width(mux::GridLengthHelper::Auto());
            row->nameActions.ColumnDefinitions().Append(labelColumn);
            row->nameActions.ColumnDefinitions().Append(deleteColumn);
            row->label.Text(RuleLabel(rule));
            row->extensions.Text(rule.extensions);
            row->nameActions.Children().Append(row->label);
            muxc::Grid::SetColumn(row->remove, 1);
            row->nameActions.Children().Append(row->remove);
            row->labelRow.Initialize(row->nameActions);
            row->extensionsRow.Initialize(row->extensions);
            row->content.Children().Append(row->labelRow.root);
            row->content.Children().Append(row->extensionsRow.root);
            row->root.Child(row->content);
            categoryRulePanel.Children().Append(row->root);

            RuleRow* const raw = row.get();
            row->labelToken = row->label.TextChanged(
                [this, raw](const auto&, const auto&) {
                    if (updatingControls || raw->closed) return;
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
            row->labelRow.SetText(
                L("app.settings.category_name", L"Category name"));
            row->extensionsRow.SetText(
                L("app.settings.category_extensions", L"Extensions"));
            row->remove.Content(winrt::box_value(
                L("app.settings.delete", L"Delete")));
            muxa::AutomationProperties::SetName(row->remove,
                L("app.settings.delete", L"Delete") + L" " +
                    row->label.Text());
        }
    }

    void UpdateConditionalStates()
    {
        const bool custom = beautifyPreset.SelectedIndex() ==
            IndexOf(kBeautifyPresets, IconBeautifyPreset::Custom);
        beautifyAdvanced.Visibility(
            custom ? mux::Visibility::Visible : mux::Visibility::Collapsed);
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
        iconSpacing->SetValue(settings.iconSpacingScale * 100.0);
        iconSize->SetValue(settings.itemIconSizeScale * 100.0);
        itemFontSize->SetValue(settings.itemFontSizeCu);
        listFontSize->SetValue(settings.listItemFontSizeCu);
        itemFontWeight->SetValue(settings.itemFontWeight);
        shortcutArrow.SelectedIndex(
            std::clamp(settings.shortcutArrowMode, 0, 2));

        const IconBeautifySettings& value = settings.iconBeautify;
        beautifyPreset.SelectedIndex(IndexOf(kBeautifyPresets, value.preset));
        beautifyEnabled.IsOn(value.enabled);
        beautifyMode.SelectedIndex(std::clamp(value.mode, 0, 1));
        backgroundStart->SetColor(MakeColor(
            value.backgroundStartR,
            value.backgroundStartG,
            value.backgroundStartB));
        backgroundOpacity->SetValue(value.backgroundOpacity * 100.0);
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

    void PatchCategory(const CategorySettings& settings)
    {
        tabFontSize->SetValue(settings.tabFontSize);
        PatchRuleRows(settings);
    }

    void PatchPersonalization(const PersonalizationSettings& settings)
    {
        categorizedTabHeight->SetValue(settings.categorizedTabHeight);
        showCategoryTabCounts.IsOn(settings.showCategoryTabCounts);
    }

    std::vector<NumericEditor*> ContinuousNumbers() const
    {
        return {
            iconSpacing.get(), iconSize.get(), itemFontSize.get(),
            listFontSize.get(), itemFontWeight.get(),
            categorizedTabHeight.get(),
            backgroundOpacity.get(), contentScale.get(),
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
        for (NumericEditor* editor : ContinuousNumbers())
            editor->CommitPending();
        for (ColorEditor* editor : ContinuousColors())
            editor->CommitPending();
        // Category tabFontSize is explicitly applied with its draft.
        tabFontSize->CancelPending();
    }

    void CancelContinuousEdits() noexcept
    {
        for (NumericEditor* editor : ContinuousNumbers())
            editor->CancelPending();
        for (ColorEditor* editor : ContinuousColors())
            editor->CancelPending();
        tabFontSize->CancelPending();
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
        const bool wasUpdating = updatingControls;
        updatingControls = true;

        if (newGeneration ||
            snapshot.domainRevisions.desktop != desktopRevision)
        {
            PatchDesktop(snapshot.values.desktop);
            desktopRevision = snapshot.domainRevisions.desktop;
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
        SetCardText(categoryLayoutCard, "app.settings.category_settings",
            L"Category tab layout");
        SetCardText(beautifyCard, "app.settings.icon_beautify",
            L"Icon beautification");
        SetCardText(categoryRulesCard, "app.settings.category_settings",
            L"Category settings");

        iconSpacing->SetLabel(L(
            "app.settings.layout_spacing", L"Icon spacing"), L(
            "app.settings.layout_spacing_hint"));
        iconSize->SetLabel(L("app.settings.icon_size", L"Icon size"), L(
            "app.settings.icon_size_hint"));
        itemFontSize->SetLabel(L(
            "app.settings.title_font_size", L"Title font size"));
        listFontSize->SetLabel(L(
            "app.settings.list_font_size", L"List font size"));
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

        categorizedTabHeight->SetLabel(
            L("app.settings.tab_height", L"Category tab height"));
        tabFontSize->SetLabel(
            L("app.settings.category_font_size", L"Category tab font size"));
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
            L("app.settings.custom", L"Custom"),
        }, std::max(0, beautifyPreset.SelectedIndex()));
        beautifyEnabledRow.SetText(L(
            "app.settings.beautify_enabled", L"Enable beautification"));
        muxa::AutomationProperties::SetName(
            beautifyEnabled, beautifyEnabledRow.label.Text());
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
            L("app.settings.apply", L"Apply"),
            L("app.settings.cancel", L"Cancel"));
        backgroundOpacity->SetLabel(
            L("app.settings.bg_opacity_val", L"Background opacity"));
        gradientEnabledRow.SetText(L(
            "app.settings.enable_gradient_bg", L"Enable gradient"));
        muxa::AutomationProperties::SetName(gradientEnabled,
            gradientEnabledRow.label.Text());
        backgroundEnd->SetLabel(L(
            "app.settings.gradient_end_color", L"Gradient end color"),
            L("app.settings.apply", L"Apply"),
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
            L("app.settings.apply", L"Apply"),
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
            L("app.settings.apply", L"Apply"),
            L("app.settings.cancel", L"Cancel"));

        categoryHint.Text(L("app.settings.category_hint",
            L"Changes to category rules are applied explicitly."));
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
        if (id == "desktop.spacing" || id == "desktop.iconSpacing")
            return iconSpacing->slider;
        if (id == "desktop.iconSize") return iconSize->slider;
        if (id == "desktop.itemFontSize") return itemFontSize->number;
        if (id == "desktop.listFontSize") return listFontSize->number;
        if (id == "desktop.fontWeight") return itemFontWeight->number;
        if (id == "desktop.shortcutArrow") return shortcutArrow;
        if (id == "desktop.categoryLayout" || id == "desktop.tabHeight")
            return categorizedTabHeight->slider;
        if (id == "desktop.tabFontSize") return tabFontSize->number;
        if (id == "desktop.categoryCounts") return showCategoryTabCounts;
        if (id == "desktop.iconBeautify" ||
            id == "desktop.iconBeautify.preset")
            return beautifyPreset;
        if (id == "desktop.iconBeautify.mode") return beautifyMode;
        if (id == "desktop.iconBeautify.backgroundColor")
            return backgroundStart->editor.button;
        if (id == "desktop.iconBeautify.backgroundOpacity")
            return backgroundOpacity->slider;
        if (id == "desktop.iconBeautify.gradient")
            return gradientEnabled;
        if (id == "desktop.iconBeautify.gradientEndColor")
            return backgroundEnd->editor.button;
        if (id == "desktop.iconBeautify.gradientDirection")
            return gradientDirection;
        if (id == "desktop.iconBeautify.shape") return shape;
        if (id == "desktop.iconBeautify.contentScale")
            return contentScale->slider;
        if (id == "desktop.iconBeautify.highlightStrength")
            return highlightStrength->slider;
        if (id == "desktop.iconBeautify.highlightSize")
            return highlightSize->slider;
        if (id == "desktop.iconBeautify.highlightAngle")
            return highlightAngle->slider;
        if (id == "desktop.iconBeautify.shadeStrength")
            return shadeStrength->slider;
        if (id == "desktop.iconBeautify.edgeHighlight")
            return edgeHighlight->slider;
        if (id == "desktop.iconBeautify.filter") return filterEnabled;
        if (id == "desktop.iconBeautify.filterColor")
            return filterTint->editor.button;
        if (id == "desktop.iconBeautify.filterStrength")
            return filterStrength->slider;
        if (id == "desktop.iconBeautify.shadowStrength")
            return shadowStrength->slider;
        if (id == "desktop.iconBeautify.outline") return outlineEnabled;
        if (id == "desktop.iconBeautify.outlineWidth")
            return outlineWidth->slider;
        if (id == "desktop.iconBeautify.outlineOpacity")
            return outlineOpacity->slider;
        if (id == "desktop.iconBeautify.outlineColor")
            return outlineColor->editor.button;
        if (id == "desktop.categories" || id == "desktop.categoryRules")
            return applyCategory;
        if (id == "desktop.category.add") return newCategoryLabel;
        return nullptr;
    }

    void CloseEditors() noexcept
    {
        iconSpacing->Close();
        iconSize->Close();
        itemFontSize->Close();
        listFontSize->Close();
        itemFontWeight->Close();
        categorizedTabHeight->Close();
        tabFontSize->Close();
        backgroundStart->Close();
        backgroundOpacity->Close();
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

    void DismissColorEditors() noexcept
    {
        backgroundStart->Dismiss();
        backgroundEnd->Dismiss();
        filterTint->Dismiss();
        outlineColor->Dismiss();
    }

    void Close() noexcept
    {
        if (closed) return;
        DismissColorEditors();
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
            beautifyPreset.SelectionChanged(beautifyPresetToken);
            beautifyEnabled.Toggled(beautifyEnabledToken);
            beautifyMode.SelectionChanged(beautifyModeToken);
            gradientEnabled.Toggled(gradientEnabledToken);
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
DesktopPagePresenter::AppearanceContent() const noexcept
{
    return impl_ ? impl_->appearanceRoot : nullptr;
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
    impl_->DismissColorEditors();
    impl_->CommitContinuousEdits();
    impl_->active = false;
}

void DesktopPagePresenter::Close() noexcept
{
    if (impl_) impl_->Close();
}

} // namespace snowdesktop::winui

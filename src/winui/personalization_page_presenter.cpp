#include "pch.h"

#include "personalization_page_presenter.h"
#include "settings_presenter_controls.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

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

constexpr std::array<int, 7> kPresetIds = {
    kAppearancePresetDark,
    kAppearancePresetLight,
    kAppearancePresetGlassDark,
    kAppearancePresetGlassLight,
    kAppearancePresetAcrylicDark,
    kAppearancePresetAcrylicLight,
    kAppearancePresetCustom,
};

struct SettingsCard
{
    muxc::Border root{nullptr};
    muxc::StackPanel content{nullptr};
    muxc::TextBlock title{nullptr};
};

struct ContinuousControl
{
    SettingRow row;
    muxc::Grid editors{nullptr};
    muxc::Slider slider{nullptr};
    muxc::NumberBox number{nullptr};
    muxc::TextBlock unit{nullptr};
    muxc::Button reset{nullptr};
    float PersonalizationSettings::* member = nullptr;
    double scale = 1.0;
    double defaultValue = std::numeric_limits<double>::quiet_NaN();
    bool dirty = false;
    CoalescedPreviewTimer<double> preview;
    mux::DispatcherTimer idleCommitTimer{nullptr};

    winrt::event_token sliderChanged{};
    winrt::event_token numberChanged{};
    winrt::event_token sliderReleased{};
    winrt::event_token numberReleased{};
    winrt::event_token sliderLostFocus{};
    winrt::event_token numberLostFocus{};
    winrt::event_token sliderKeyDown{};
    winrt::event_token numberKeyDown{};
    winrt::event_token resetClicked{};
    winrt::event_token idleCommitToken{};
};

struct ColorControl
{
    ColorFlyoutEditor editor;
    float PersonalizationSettings::* red = nullptr;
    float PersonalizationSettings::* green = nullptr;
    float PersonalizationSettings::* blue = nullptr;

};

void InitializeCard(
    SettingsCard& card,
    const mux::Style& style,
    const muxc::StackPanel& page)
{
    card.root = muxc::Border{};
    if (style)
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

std::uint8_t ToByte(float value) noexcept
{
    return static_cast<std::uint8_t>(std::lround(
        std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

float FromByte(std::uint8_t value) noexcept
{
    return static_cast<float>(value) / 255.0f;
}

winrt::Windows::UI::Color ToColor(float red, float green, float blue) noexcept
{
    return winrt::Windows::UI::Color{
        255, ToByte(red), ToByte(green), ToByte(blue)};
}

bool IsEnter(const muxi::KeyRoutedEventArgs& args) noexcept
{
    return args.Key() == winrt::Windows::System::VirtualKey::Enter;
}

} // namespace

struct PersonalizationPagePresenter::Impl
{
    explicit Impl(LocalizeCallback callback, const mux::Style& style)
        : localize(std::move(callback)), cardStyle(style)
    {
        BuildControls();
        HookEvents();
        RefreshLocalizedText();
    }

    LocalizeCallback localize;
    PersonalizationPageActions actions;
    mux::Style cardStyle{nullptr};
    muxc::StackPanel themeRoot{nullptr};
    muxc::StackPanel widgetLayoutRoot{nullptr};

    SettingsCard themeCard;
    SettingsCard themeTargetsCard;
    SettingsCard widgetAppearanceCard;
    SettingsCard contextMenuCard;
    SettingsCard layoutCard;

    muxc::ComboBox presetCombo{nullptr};
    muxc::ComboBox quickNavigationThemeCombo{nullptr};
    muxc::ComboBox collectionPopupThemeCombo{nullptr};
    muxc::ToggleSwitch gradientToggle{nullptr};
    muxc::ToggleSwitch glassToggle{nullptr};
    muxc::ToggleSwitch acrylicToggle{nullptr};
    muxc::ToggleSwitch edgeHighlightToggle{nullptr};
    muxc::ComboBox contentThemeCombo{nullptr};
    ColorControl backgroundColor;
    ColorControl borderColor;
    ContinuousControl widgetAlpha;
    ContinuousControl borderAlpha;
    ContinuousControl borderWidth;
    ContinuousControl edgeHighlightWidth;
    ContinuousControl edgeHighlightStrength;
    ContinuousControl gradientEndAlpha;
    ContinuousControl blurRadius;
    muxc::ComboBox contextMenuCombo{nullptr};
    ContinuousControl cornerRadius;
    ContinuousControl barHeight;
    ContinuousControl categorizedTabHeight;
    ContinuousControl luaWidgetContentRowHeight;

    SettingRow presetRow;
    SettingRow quickNavigationThemeRow;
    SettingRow collectionPopupThemeRow;
    SettingRow gradientToggleRow;
    SettingRow edgeHighlightRow;
    SettingRow glassRow;
    SettingRow acrylicRow;
    SettingRow contentThemeRow;
    SettingRow contextMenuRow;

    std::array<ContinuousControl*, 11> continuousControls = {
        &widgetAlpha,
        &borderAlpha,
        &borderWidth,
        &edgeHighlightWidth,
        &edgeHighlightStrength,
        &gradientEndAlpha,
        &blurRadius,
        &cornerRadius,
        &barHeight,
        &categorizedTabHeight,
        &luaWidgetContentRowHeight,
    };
    std::array<ColorControl*, 2> colorControls = {
        &backgroundColor,
        &borderColor,
    };

    std::uint64_t generation = 0;
    std::uint64_t personalizationRevision = 0;
    std::uint64_t generalRevision = 0;
    int currentBackgroundPreset = kAppearancePresetDark;
    bool hasSnapshot = false;
    bool updatingControls = false;
    bool synchronizingPair = false;
    bool active = false;
    bool closed = false;

    winrt::event_token presetToken{};
    winrt::event_token quickNavigationThemeToken{};
    winrt::event_token collectionPopupThemeToken{};
    winrt::event_token gradientToken{};
    winrt::event_token edgeHighlightToken{};
    winrt::event_token glassToken{};
    winrt::event_token acrylicToken{};
    winrt::event_token contentThemeToken{};
    winrt::event_token contextMenuToken{};

    [[nodiscard]] std::wstring L(
        std::string_view key,
        std::wstring_view fallback) const
    {
        if (localize)
        {
            std::wstring translated = localize(key);
            if (!translated.empty())
                return translated;
        }
        return std::wstring(fallback);
    }

    [[nodiscard]] bool CanEmit() const noexcept
    {
        return !closed && active && hasSnapshot && !updatingControls &&
            !synchronizingPair && static_cast<bool>(actions.update);
    }

    template <typename Edit>
    void Emit(SettingsUpdateMode mode, Edit edit)
    {
        if (!CanEmit())
            return;
        actions.update(generation, mode,
            PersonalizationPageActions::Edit(std::move(edit)));
    }

    template <typename Edit>
    void EmitGeneral(SettingsUpdateMode mode, Edit edit)
    {
        if (closed || !active || !hasSnapshot || updatingControls ||
            synchronizingPair || !actions.updateGeneral)
        {
            return;
        }
        actions.updateGeneral(generation, mode,
            PersonalizationPageActions::GeneralEdit(std::move(edit)));
    }

    void BuildControls()
    {
        themeRoot = muxc::StackPanel{};
        themeRoot.Spacing(8.0);
        widgetLayoutRoot = muxc::StackPanel{};
        widgetLayoutRoot.Spacing(8.0);

        InitializeCard(themeCard, cardStyle, themeRoot);
        presetCombo = muxc::ComboBox{};
        presetCombo.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        presetCombo.MaxWidth(520.0);
        presetRow.Initialize(presetCombo);
        themeCard.content.Children().Append(presetRow.root);

        InitializeCard(themeTargetsCard, cardStyle, themeRoot);
        quickNavigationThemeCombo = muxc::ComboBox{};
        collectionPopupThemeCombo = muxc::ComboBox{};
        for (const auto& combo : {
                 quickNavigationThemeCombo, collectionPopupThemeCombo})
        {
            combo.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
            combo.MaxWidth(520.0);
        }
        quickNavigationThemeRow.Initialize(quickNavigationThemeCombo);
        collectionPopupThemeRow.Initialize(collectionPopupThemeCombo);
        themeTargetsCard.content.Children().Append(
            quickNavigationThemeRow.root);
        themeTargetsCard.content.Children().Append(
            collectionPopupThemeRow.root);

        InitializeCard(widgetAppearanceCard, cardStyle, themeRoot);
        InitializeColorControl(backgroundColor,
            &PersonalizationSettings::widgetBgR,
            &PersonalizationSettings::widgetBgG,
            &PersonalizationSettings::widgetBgB);
        InitializeColorControl(borderColor,
            &PersonalizationSettings::widgetBorderR,
            &PersonalizationSettings::widgetBorderG,
            &PersonalizationSettings::widgetBorderB);
        widgetAppearanceCard.content.Children().Append(
            backgroundColor.editor.row.root);

        InitializeContinuousControl(widgetAlpha,
            &PersonalizationSettings::widgetAlpha, 0.0, 100.0, 1.0, 0.01);
        InitializeContinuousControl(borderAlpha,
            &PersonalizationSettings::widgetBorderAlpha,
            0.0, 100.0, 1.0, 0.01);
        InitializeContinuousControl(borderWidth,
            &PersonalizationSettings::widgetBorderWidth,
            kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth,
            0.5, 1.0);
        InitializeContinuousControl(edgeHighlightWidth,
            &PersonalizationSettings::widgetEdgeHighlightWidth,
            kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth,
            0.5, 1.0);
        InitializeContinuousControl(edgeHighlightStrength,
            &PersonalizationSettings::widgetEdgeHighlightStrength,
            0.0, 100.0, 1.0, 0.01);
        InitializeContinuousControl(gradientEndAlpha,
            &PersonalizationSettings::gradientEndA,
            0.0, 100.0, 1.0, 0.01);
        InitializeContinuousControl(blurRadius,
            &PersonalizationSettings::glassBlurRadius,
            4.0, 48.0, 1.0, 1.0);
        SetUnit(widgetAlpha, L"%");
        SetUnit(borderAlpha, L"%");
        SetUnit(borderWidth, L"px");
        SetUnit(edgeHighlightWidth, L"px");
        SetUnit(edgeHighlightStrength, L"%");
        SetUnit(gradientEndAlpha, L"%");
        SetUnit(blurRadius, L"px");
        widgetAppearanceCard.content.Children().Append(widgetAlpha.row.root);
        widgetAppearanceCard.content.Children().Append(
            borderColor.editor.row.root);
        widgetAppearanceCard.content.Children().Append(borderAlpha.row.root);
        widgetAppearanceCard.content.Children().Append(borderWidth.row.root);

        edgeHighlightToggle = muxc::ToggleSwitch{};
        edgeHighlightToggle.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        edgeHighlightRow.Initialize(edgeHighlightToggle);
        edgeHighlightRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        widgetAppearanceCard.content.Children().Append(edgeHighlightRow.root);
        widgetAppearanceCard.content.Children().Append(
            edgeHighlightWidth.row.root);
        widgetAppearanceCard.content.Children().Append(
            edgeHighlightStrength.row.root);

        gradientToggle = muxc::ToggleSwitch{};
        gradientToggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
        gradientToggleRow.Initialize(gradientToggle);
        gradientToggleRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        widgetAppearanceCard.content.Children().Append(
            gradientToggleRow.root);
        widgetAppearanceCard.content.Children().Append(
            gradientEndAlpha.row.root);

        glassToggle = muxc::ToggleSwitch{};
        glassToggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
        acrylicToggle = muxc::ToggleSwitch{};
        acrylicToggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
        glassRow.Initialize(glassToggle);
        acrylicRow.Initialize(acrylicToggle);
        glassRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        acrylicRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        widgetAppearanceCard.content.Children().Append(glassRow.root);
        widgetAppearanceCard.content.Children().Append(blurRadius.row.root);
        widgetAppearanceCard.content.Children().Append(acrylicRow.root);

        contentThemeCombo = muxc::ComboBox{};
        contentThemeCombo.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        contentThemeCombo.MaxWidth(520.0);
        contentThemeRow.Initialize(contentThemeCombo);
        widgetAppearanceCard.content.Children().Append(contentThemeRow.root);

        InitializeCard(contextMenuCard, cardStyle, themeRoot);
        contextMenuCombo = muxc::ComboBox{};
        contextMenuCombo.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        contextMenuCombo.MaxWidth(520.0);
        contextMenuRow.Initialize(contextMenuCombo);
        contextMenuCard.content.Children().Append(contextMenuRow.root);

        InitializeCard(layoutCard, cardStyle, widgetLayoutRoot);
        InitializeContinuousControl(cornerRadius,
            &PersonalizationSettings::cornerRadius,
            4.0, 28.0, 1.0, 1.0, 12.0);
        InitializeContinuousControl(barHeight,
            &PersonalizationSettings::barHeight,
            16.0, 48.0, 1.0, 1.0, 24.0);
        InitializeContinuousControl(categorizedTabHeight,
            &PersonalizationSettings::categorizedTabHeight,
            24.0, 48.0, 1.0, 1.0, 34.0);
        SetUnit(cornerRadius, L"cu");
        SetUnit(barHeight, L"cu");
        SetUnit(categorizedTabHeight, L"cu");
        layoutCard.content.Children().Append(cornerRadius.row.root);
        layoutCard.content.Children().Append(barHeight.row.root);
        layoutCard.content.Children().Append(categorizedTabHeight.row.root);
        InitializeContinuousControl(luaWidgetContentRowHeight,
            &PersonalizationSettings::luaWidgetContentRowHeight,
            18.0, 48.0, 1.0, 1.0, 28.0);
        SetUnit(luaWidgetContentRowHeight, L"cu");
        layoutCard.content.Children().Append(
            luaWidgetContentRowHeight.row.root);
    }

    void InitializeColorControl(
        ColorControl& control,
        float PersonalizationSettings::* red,
        float PersonalizationSettings::* green,
        float PersonalizationSettings::* blue)
    {
        control.red = red;
        control.green = green;
        control.blue = blue;
        control.editor.Initialize(
            [this, &control](const winrt::Windows::UI::Color& color,
                SettingsUpdateMode mode) {
                const float redValue = FromByte(color.R);
                const float greenValue = FromByte(color.G);
                const float blueValue = FromByte(color.B);
                Emit(mode, [&control, redValue, greenValue, blueValue](
                    PersonalizationSettings& settings) {
                    settings.*control.red = redValue;
                    settings.*control.green = greenValue;
                    settings.*control.blue = blueValue;
                });
            });
    }

    void InitializeContinuousControl(
        ContinuousControl& control,
        float PersonalizationSettings::* member,
        double minimum,
        double maximum,
        double step,
        double scale,
        double defaultValue = std::numeric_limits<double>::quiet_NaN())
    {
        control.editors = muxc::Grid{};
        control.editors.ColumnSpacing(8.0);
        muxc::ColumnDefinition sliderColumn{};
        sliderColumn.Width(mux::GridLengthHelper::FromValueAndType(
            1.0, mux::GridUnitType::Star));
        muxc::ColumnDefinition numberColumn{};
        numberColumn.Width(mux::GridLengthHelper::Auto());
        muxc::ColumnDefinition unitColumn{};
        unitColumn.Width(mux::GridLengthHelper::Auto());
        control.editors.ColumnDefinitions().Append(sliderColumn);
        control.editors.ColumnDefinitions().Append(numberColumn);
        control.editors.ColumnDefinitions().Append(unitColumn);
        control.slider = muxc::Slider{};
        control.slider.Minimum(minimum);
        control.slider.Maximum(maximum);
        control.slider.StepFrequency(step);
        control.slider.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        control.slider.VerticalAlignment(mux::VerticalAlignment::Center);
        control.number = muxc::NumberBox{};
        control.number.Minimum(minimum);
        control.number.Maximum(maximum);
        control.number.SmallChange(step);
        control.number.LargeChange(step * 5.0);
        control.number.SpinButtonPlacementMode(
            muxc::NumberBoxSpinButtonPlacementMode::Compact);
        control.number.Width(92.0);
        control.unit = muxc::TextBlock{};
        control.unit.VerticalAlignment(mux::VerticalAlignment::Center);
        control.unit.Opacity(0.72);
        control.unit.Visibility(mux::Visibility::Collapsed);
        control.member = member;
        control.scale = scale;
        control.defaultValue = defaultValue;
        control.editors.Children().Append(control.slider);
        muxc::Grid::SetColumn(control.number, 1);
        control.editors.Children().Append(control.number);
        muxc::Grid::SetColumn(control.unit, 2);
        control.editors.Children().Append(control.unit);
        if (std::isfinite(defaultValue))
        {
            muxc::ColumnDefinition resetColumn{};
            resetColumn.Width(mux::GridLengthHelper::Auto());
            control.editors.ColumnDefinitions().Append(resetColumn);
            control.reset = muxc::Button{};
            control.reset.VerticalAlignment(mux::VerticalAlignment::Center);
            control.reset.VerticalContentAlignment(
                mux::VerticalAlignment::Center);
            control.reset.HorizontalContentAlignment(
                mux::HorizontalAlignment::Center);
            control.reset.MinHeight(32.0);
            muxc::Grid::SetColumn(control.reset, 3);
            control.editors.Children().Append(control.reset);
        }
        control.row.Initialize(control.editors);
    }

    static void SetUnit(ContinuousControl& control, std::wstring text)
    {
        control.unit.Text(std::move(text));
        control.unit.Visibility(control.unit.Text().empty()
                ? mux::Visibility::Collapsed
                : mux::Visibility::Visible);
    }

    void HookEvents()
    {
        presetToken = presetCombo.SelectionChanged(
            [this](const auto&, const auto&) {
                UpdateDependentStates();
                if (!CanEmit())
                    return;
                const int index = presetCombo.SelectedIndex();
                if (index < 0 ||
                    static_cast<std::size_t>(index) >= kPresetIds.size())
                    return;
                const int preset =
                    kPresetIds[static_cast<std::size_t>(index)];
                const int previousPreset = currentBackgroundPreset;
                currentBackgroundPreset = preset;
                if (preset == kAppearancePresetCustom)
                {
                    const int inheritedTheme =
                        FourThemeSelectionFromAppearancePreset(
                            NormalizeAppearancePresetId(previousPreset));
                    EmitGeneral(SettingsUpdateMode::PreviewAndCommit,
                        [inheritedTheme](GeneralSettings& settings) {
                            settings.quickNavTheme = inheritedTheme;
                            settings.collectionPopupTheme = inheritedTheme;
                        });
                }
                Emit(SettingsUpdateMode::PreviewAndCommit,
                    [preset](PersonalizationSettings& settings) {
                        if (preset == kAppearancePresetCustom)
                        {
                            settings.backgroundPreset =
                                kAppearancePresetCustom;
                            return;
                        }
                        const float corner = settings.cornerRadius;
                        const float bar = settings.barHeight;
                        const float tab = settings.categorizedTabHeight;
                        const float luaWidgetContentRowHeight =
                            settings.luaWidgetContentRowHeight;
                        const bool counts = settings.showCategoryTabCounts;
                        const int menu = settings.contextMenuStyle;
                        settings = MakeAppearancePreset(preset);
                        settings.cornerRadius = corner;
                        settings.barHeight = bar;
                        settings.categorizedTabHeight = tab;
                        settings.luaWidgetContentRowHeight =
                            luaWidgetContentRowHeight;
                        settings.showCategoryTabCounts = counts;
                        settings.contextMenuStyle = menu;
                    });
            });
        quickNavigationThemeToken =
            quickNavigationThemeCombo.SelectionChanged(
                [this](const auto&, const auto&) {
                    const int value = quickNavigationThemeCombo.SelectedIndex();
                    if (value < 0) return;
                    EmitGeneral(SettingsUpdateMode::PreviewAndCommit,
                        [value](GeneralSettings& settings) {
                            settings.quickNavTheme = std::clamp(value, 0, 3);
                        });
                });
        collectionPopupThemeToken =
            collectionPopupThemeCombo.SelectionChanged(
                [this](const auto&, const auto&) {
                    const int value = collectionPopupThemeCombo.SelectedIndex();
                    if (value < 0) return;
                    EmitGeneral(SettingsUpdateMode::PreviewAndCommit,
                        [value](GeneralSettings& settings) {
                            settings.collectionPopupTheme =
                                std::clamp(value, 0, 3);
                        });
                });
        gradientToken = gradientToggle.Toggled(
            [this](const auto&, const auto&) {
                UpdateDependentStates();
                const bool enabled = gradientToggle.IsOn();
                Emit(SettingsUpdateMode::PreviewAndCommit,
                    [enabled](PersonalizationSettings& settings) {
                        settings.gradientEndA = enabled
                            ? MakeAppearancePreset(
                                  settings.backgroundPreset).gradientEndA
                            : 0.0f;
                    });
            });
        edgeHighlightToken = edgeHighlightToggle.Toggled(
            [this](const auto&, const auto&) {
                UpdateDependentStates();
                const bool value = edgeHighlightToggle.IsOn();
                Emit(SettingsUpdateMode::PreviewAndCommit,
                    [value](PersonalizationSettings& settings) {
                        settings.widgetEdgeHighlightEnabled = value;
                    });
            });
        glassToken = glassToggle.Toggled(
            [this](const auto&, const auto&) {
                UpdateDependentStates();
                const bool value = glassToggle.IsOn();
                Emit(SettingsUpdateMode::PreviewAndCommit,
                    [value](PersonalizationSettings& settings) {
                        settings.glassEnabled = value;
                    });
            });
        acrylicToken = acrylicToggle.Toggled(
            [this](const auto&, const auto&) {
                const bool value = acrylicToggle.IsOn();
                Emit(SettingsUpdateMode::PreviewAndCommit,
                    [value](PersonalizationSettings& settings) {
                        settings.acrylicEnabled = value;
                    });
            });
        contentThemeToken = contentThemeCombo.SelectionChanged(
            [this](const auto&, const auto&) {
                const int value = contentThemeCombo.SelectedIndex();
                if (value < 0)
                    return;
                Emit(SettingsUpdateMode::PreviewAndCommit,
                    [value](PersonalizationSettings& settings) {
                        settings.contentTheme = std::clamp(value, 0, 1);
                    });
            });
        contextMenuToken = contextMenuCombo.SelectionChanged(
            [this](const auto&, const auto&) {
                const int value = contextMenuCombo.SelectedIndex();
                if (value < 0)
                    return;
                Emit(SettingsUpdateMode::PreviewAndCommit,
                    [value](PersonalizationSettings& settings) {
                        settings.contextMenuStyle = std::clamp(value, 0, 4);
                    });
            });
        for (ContinuousControl* control : continuousControls)
            HookContinuousControl(*control);
    }

    void HookContinuousControl(ContinuousControl& control)
    {
        control.idleCommitTimer = mux::DispatcherTimer{};
        control.idleCommitTimer.Interval(std::chrono::milliseconds(650));
        control.idleCommitToken = control.idleCommitTimer.Tick(
            [this, &control](const auto&, const auto&) {
                control.idleCommitTimer.Stop();
                Commit(control);
            });
        control.preview.Initialize([this, &control](const double& value) {
            PublishPreview(control, value);
        });
        control.sliderChanged = control.slider.ValueChanged(
            [this, &control](const auto&, const auto&) {
                if (updatingControls || synchronizingPair || closed)
                    return;
                const double value = control.slider.Value();
                synchronizingPair = true;
                control.number.Value(value);
                synchronizingPair = false;
                Preview(control, value);
            });
        control.numberChanged = control.number.ValueChanged(
            [this, &control](const auto&, const auto&) {
                if (updatingControls || synchronizingPair || closed)
                    return;
                double value = control.number.Value();
                if (std::isnan(value))
                    value = control.slider.Value();
                value = std::clamp(
                    value, control.slider.Minimum(), control.slider.Maximum());
                synchronizingPair = true;
                control.slider.Value(value);
                synchronizingPair = false;
                Preview(control, value);
            });
        control.sliderReleased = control.slider.PointerReleased(
            [this, &control](const auto&, const auto&) {
                Commit(control);
            });
        control.numberReleased = control.number.PointerReleased(
            [this, &control](const auto&, const auto&) {
                Commit(control);
            });
        control.sliderLostFocus = control.slider.LostFocus(
            [this, &control](const auto&, const auto&) {
                Commit(control);
            });
        control.numberLostFocus = control.number.LostFocus(
            [this, &control](const auto&, const auto&) {
                Commit(control);
            });
        control.sliderKeyDown = control.slider.KeyDown(
            [this, &control](const auto&, const muxi::KeyRoutedEventArgs& args) {
                if (IsEnter(args))
                    Commit(control);
            });
        control.numberKeyDown = control.number.KeyDown(
            [this, &control](const auto&, const muxi::KeyRoutedEventArgs& args) {
                if (IsEnter(args))
                    Commit(control);
            });
        if (control.reset)
        {
            control.resetClicked = control.reset.Click(
                [this, &control](const auto&, const auto&) {
                    if (!std::isfinite(control.defaultValue)) return;
                    control.idleCommitTimer.Stop();
                    control.preview.Cancel();
                    control.dirty = false;
                    const float value = static_cast<float>(
                        control.defaultValue * control.scale);
                    const auto member = control.member;
                    Emit(SettingsUpdateMode::PreviewAndCommit,
                        [member, value](PersonalizationSettings& settings) {
                            settings.*member = value;
                        });
                });
        }
    }

    void Preview(ContinuousControl& control, double uiValue)
    {
        if (!CanEmit())
            return;
        control.dirty = true;
        control.idleCommitTimer.Stop();
        control.idleCommitTimer.Start();
        control.preview.Queue(uiValue);
    }

    void PublishPreview(ContinuousControl& control, double uiValue)
    {
        if (!control.dirty || !CanEmit())
            return;
        const float value = static_cast<float>(uiValue * control.scale);
        const auto member = control.member;
        Emit(SettingsUpdateMode::Preview,
            [member, value](PersonalizationSettings& settings) {
                settings.*member = value;
            });
    }

    void Commit(ContinuousControl& control)
    {
        if (control.idleCommitTimer)
            control.idleCommitTimer.Stop();
        control.preview.Cancel();
        if (!control.dirty || !CanEmit())
            return;
        control.dirty = false;
        const float value = static_cast<float>(
            control.slider.Value() * control.scale);
        const auto member = control.member;
        Emit(SettingsUpdateMode::PreviewAndCommit,
            [member, value](PersonalizationSettings& settings) {
                settings.*member = value;
            });
    }

    void PatchContinuous(
        ContinuousControl& control,
        const PersonalizationSettings& settings)
    {
        const double value = static_cast<double>(settings.*control.member) /
            control.scale;
        const double clamped = QuantizeNumericValue(value,
            control.slider.Minimum(), control.slider.Maximum(),
            control.slider.StepFrequency());
        control.slider.Value(clamped);
        control.number.Value(clamped);
    }

    void PatchColor(
        ColorControl& control,
        const PersonalizationSettings& settings)
    {
        control.editor.SetColor(ToColor(
            settings.*control.red,
            settings.*control.green,
            settings.*control.blue));
    }

    void Patch(const PersonalizationSettings& settings)
    {
        const int normalized = NormalizeAppearancePresetId(
            settings.backgroundPreset);
        currentBackgroundPreset = normalized;
        auto preset = std::find(kPresetIds.begin(), kPresetIds.end(), normalized);
        presetCombo.SelectedIndex(preset == kPresetIds.end()
            ? static_cast<int>(kPresetIds.size() - 1)
            : static_cast<int>(std::distance(kPresetIds.begin(), preset)));
        PatchColor(backgroundColor, settings);
        PatchColor(borderColor, settings);
        for (ContinuousControl* control : continuousControls)
            PatchContinuous(*control, settings);
        gradientToggle.IsOn(settings.gradientEndA > 0.001f);
        glassToggle.IsOn(settings.glassEnabled);
        acrylicToggle.IsOn(settings.acrylicEnabled);
        edgeHighlightToggle.IsOn(settings.widgetEdgeHighlightEnabled);
        contentThemeCombo.SelectedIndex(
            std::clamp(settings.contentTheme, 0, 1));
        contextMenuCombo.SelectedIndex(
            std::clamp(settings.contextMenuStyle, 0, 4));
        UpdateDependentStates();
    }

    void PatchGeneral(const GeneralSettings& settings)
    {
        quickNavigationThemeCombo.SelectedIndex(
            std::clamp(settings.quickNavTheme, 0, 3));
        collectionPopupThemeCombo.SelectedIndex(
            std::clamp(settings.collectionPopupTheme, 0, 3));
    }

    void UpdateDependentStates()
    {
        if (closed)
            return;
        const bool custom = presetCombo.SelectedIndex() ==
            static_cast<int>(kPresetIds.size() - 1);
        themeTargetsCard.root.Visibility(custom
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
        widgetAppearanceCard.root.Visibility(custom
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
        backgroundColor.editor.SetEnabled(custom);
        borderColor.editor.SetEnabled(custom);
        widgetAlpha.row.SetEnabled(custom);
        borderAlpha.row.SetEnabled(custom);
        borderWidth.row.SetEnabled(custom);
        edgeHighlightRow.SetEnabled(custom);
        edgeHighlightWidth.row.SetEnabled(custom &&
            edgeHighlightToggle.IsOn());
        edgeHighlightStrength.row.SetEnabled(custom &&
            edgeHighlightToggle.IsOn());
        gradientToggleRow.SetEnabled(custom);
        gradientEndAlpha.row.SetEnabled(custom && gradientToggle.IsOn());
        glassRow.SetEnabled(custom);
        blurRadius.row.SetEnabled(custom && glassToggle.IsOn());
        acrylicRow.SetEnabled(custom && glassToggle.IsOn());
        contentThemeRow.SetEnabled(custom);
    }

    void SetCardText(
        SettingsCard& card,
        std::string_view key,
        std::wstring_view fallback)
    {
        card.title.Text(L(key, fallback));
        muxa::AutomationProperties::SetName(card.root, card.title.Text());
    }

    void SetContinuousText(
        ContinuousControl& control,
        std::string_view key,
        std::wstring_view fallback)
    {
        control.row.SetText(L(key, fallback));
        muxa::AutomationProperties::SetName(
            control.slider, control.row.label.Text());
        muxa::AutomationProperties::SetName(
            control.number, control.row.label.Text());
        if (control.reset)
        {
            presenter_controls::ConfigureRestoreDefaultButton(
                control.reset,
                L("app.settings.restore_default", L"Restore Default"));
        }
    }

    void SetColorText(
        ColorControl& control,
        std::string_view key,
        std::wstring_view fallback)
    {
        control.editor.SetText(
            L(key, fallback), {},
            L("app.settings.cancel", L"Cancel"));
    }

    void ReplaceComboItems(
        const muxc::ComboBox& combo,
        const std::initializer_list<std::pair<
            std::string_view, std::wstring_view>>& items)
    {
        const int selected = combo.SelectedIndex();
        combo.Items().Clear();
        for (const auto& [key, fallback] : items)
            combo.Items().Append(winrt::box_value(L(key, fallback)));
        if (!items.size())
            return;
        combo.SelectedIndex(std::clamp(
            selected, 0, static_cast<int>(items.size()) - 1));
    }

    void RefreshLocalizedText()
    {
        if (closed)
            return;
        const bool previousUpdating = updatingControls;
        updatingControls = true;

        SetCardText(themeCard,
            "app.settings.global_theme", L"Global Theme");
        SetCardText(themeTargetsCard,
            "settings.personalization.theme", L"Theme and Material");
        SetCardText(widgetAppearanceCard,
            "app.settings.component_bg", L"Widget Appearance");
        SetCardText(contextMenuCard,
            "app.settings.context_menu_appearance", L"Context Menu");
        SetCardText(layoutCard,
            "app.settings.widget_layout", L"Widget Layout");

        presetRow.SetText(L("app.settings.theme", L"Theme"));
        ReplaceComboItems(presetCombo, {
            {"app.settings.dark", L"Dark"},
            {"app.settings.light", L"Light"},
            {"app.settings.dark_glass", L"Dark Glass"},
            {"app.settings.light_glass", L"Light Glass"},
            {"app.settings.dark_acrylic", L"Dark Acrylic"},
            {"app.settings.light_acrylic", L"Light Acrylic"},
            {"app.settings.custom", L"Custom"},
        });
        muxa::AutomationProperties::SetName(
            presetCombo, L("app.settings.theme", L"Theme"));

        quickNavigationThemeRow.SetText(
            L("app.settings.quick_nav_theme", L"Quick Nav Theme"));
        collectionPopupThemeRow.SetText(L(
            "app.settings.collection_popup_theme",
            L"Widget & Dock Popup Theme"));
        const std::initializer_list<std::pair<
            std::string_view, std::wstring_view>> themeChoices = {
            {"app.settings.dark", L"Dark"},
            {"app.settings.light", L"Light"},
            {"app.settings.dark_acrylic", L"Dark Acrylic"},
            {"app.settings.light_acrylic", L"Light Acrylic"},
        };
        ReplaceComboItems(quickNavigationThemeCombo, themeChoices);
        ReplaceComboItems(collectionPopupThemeCombo, themeChoices);
        muxa::AutomationProperties::SetName(quickNavigationThemeCombo,
            quickNavigationThemeRow.label.Text());
        muxa::AutomationProperties::SetName(collectionPopupThemeCombo,
            collectionPopupThemeRow.label.Text());

        SetColorText(backgroundColor,
            "app.settings.component_bg", L"Widget Background");
        SetColorText(borderColor,
            "app.settings.component_border", L"Border Color");
        SetContinuousText(widgetAlpha,
            "app.settings.bg_opacity", L"Background Opacity");
        SetContinuousText(borderAlpha,
            "app.settings.border_opacity", L"Border Opacity");
        SetContinuousText(borderWidth,
            "app.settings.border_width", L"Border Width");
        edgeHighlightRow.SetText(
            L("app.settings.edge_highlight", L"Edge Highlight"));
        SetContinuousText(edgeHighlightWidth,
            "app.settings.edge_highlight_width", L"Edge Highlight Width");
        SetContinuousText(edgeHighlightStrength,
            "app.settings.edge_highlight_strength",
            L"Edge Highlight Strength");
        SetContinuousText(gradientEndAlpha,
            "app.settings.gradient_end_alpha", L"Gradient End Opacity");
        SetContinuousText(blurRadius,
            "app.settings.blur_radius", L"Blur Radius");

        gradientToggleRow.SetText(
            L("app.settings.enable_gradient", L"Enable Bottom Gradient"));
        glassRow.SetText(
            L("app.settings.glass_enabled", L"Frosted Glass Background"));
        acrylicRow.SetText(
            L("app.settings.acrylic_noise", L"Acrylic Noise"));
        contentThemeRow.SetText(
            L("app.settings.text_color", L"Text Color"));
        ReplaceComboItems(contentThemeCombo, {
            {"app.settings.light", L"Light"},
            {"app.settings.dark", L"Dark"},
        });

        contextMenuRow.SetText(
            L("app.settings.context_menu_style", L"Menu Style"));
        ReplaceComboItems(contextMenuCombo, {
            {"app.settings.context_menu_follow_system", L"Follow System"},
            {"app.settings.context_menu_system_light_blur", L"Light"},
            {"app.settings.context_menu_system_dark_blur", L"Dark"},
            {"app.settings.context_menu_opaque_light", L"Light (Opaque)"},
            {"app.settings.context_menu_opaque_dark", L"Dark (Opaque)"},
        });

        SetContinuousText(cornerRadius,
            "app.settings.corner_radius", L"Corner Radius");
        SetContinuousText(barHeight,
            "app.settings.bar_height", L"Bar Height");
        SetContinuousText(categorizedTabHeight,
            "app.settings.tab_height", L"Category Tab Height");
        SetContinuousText(luaWidgetContentRowHeight,
            "app.settings.lua_widget_row_height",
            L"Lua Widget Row Height");
        muxa::AutomationProperties::SetName(
            gradientToggle, gradientToggleRow.label.Text());
        muxa::AutomationProperties::SetName(
            edgeHighlightToggle, edgeHighlightRow.label.Text());
        muxa::AutomationProperties::SetName(
            glassToggle, glassRow.label.Text());
        muxa::AutomationProperties::SetName(
            acrylicToggle, acrylicRow.label.Text());
        muxa::AutomationProperties::SetName(
            contentThemeCombo, contentThemeRow.label.Text());
        muxa::AutomationProperties::SetName(
            contextMenuCombo, contextMenuRow.label.Text());

        updatingControls = previousUpdating;
        UpdateDependentStates();
    }

    void ApplySnapshot(const SettingsSnapshot& snapshot)
    {
        if (closed)
            return;
        const bool newGeneration =
            !hasSnapshot || snapshot.generation != generation;
        generation = snapshot.generation;
        const bool personalizationChanged = newGeneration ||
            snapshot.domainRevisions.personalization !=
                personalizationRevision;
        const bool generalChanged = newGeneration ||
            snapshot.domainRevisions.general != generalRevision;
        if (!personalizationChanged && !generalChanged)
        {
            hasSnapshot = true;
            return;
        }

        const bool previousUpdating = updatingControls;
        updatingControls = true;
        if (newGeneration)
        {
            for (ContinuousControl* control : continuousControls)
            {
                if (control->idleCommitTimer)
                    control->idleCommitTimer.Stop();
                control->dirty = false;
            }
        }
        if (personalizationChanged)
        {
            Patch(snapshot.values.personalization);
            personalizationRevision =
                snapshot.domainRevisions.personalization;
        }
        if (generalChanged)
        {
            PatchGeneral(snapshot.values.general);
            generalRevision = snapshot.domainRevisions.general;
        }
        hasSnapshot = true;
        updatingControls = previousUpdating;
    }

    mux::FrameworkElement FocusTarget(std::string_view id) const noexcept
    {
        if (id == "personalization.theme" ||
            id == "personalization.globalTheme")
            return presetCombo;
        if (id == "personalization.backgroundColor")
            return backgroundColor.editor.button;
        if (id == "personalization.borderColor")
            return borderColor.editor.button;
        if (id == "personalization.quickNavigationTheme" ||
            id == "personalization.quickNavTheme")
            return quickNavigationThemeCombo;
        if (id == "personalization.collectionPopupTheme")
            return collectionPopupThemeCombo;
        if (id == "personalization.widgetAlpha" ||
            id == "personalization.backgroundOpacity")
            return widgetAlpha.slider;
        if (id == "personalization.borderAlpha" ||
            id == "personalization.borderOpacity")
            return borderAlpha.slider;
        if (id == "personalization.borderWidth")
            return borderWidth.slider;
        if (id == "personalization.edgeHighlight")
            return edgeHighlightToggle;
        if (id == "personalization.edgeHighlightWidth")
            return edgeHighlightWidth.slider;
        if (id == "personalization.edgeHighlightStrength")
            return edgeHighlightStrength.slider;
        if (id == "personalization.gradientEndAlpha")
            return gradientEndAlpha.slider;
        if (id == "personalization.enableGradient")
            return gradientToggle;
        if (id == "personalization.glass")
            return glassToggle;
        if (id == "personalization.blurRadius")
            return blurRadius.slider;
        if (id == "personalization.acrylic")
            return acrylicToggle;
        if (id == "personalization.contentTheme")
            return contentThemeCombo;
        if (id == "personalization.contextMenu")
            return contextMenuCombo;
        if (id == "personalization.cornerRadius")
            return cornerRadius.slider;
        if (id == "personalization.barHeight")
            return barHeight.slider;
        if (id == "personalization.luaWidgetRowHeight")
            return luaWidgetContentRowHeight.slider;
        if (id == "desktop.categoryLayout" ||
            id == "desktop.tabHeight" ||
            id == "personalization.tabHeight")
        {
            return categorizedTabHeight.slider;
        }
        return nullptr;
    }

    void UnhookContinuousControl(ContinuousControl& control) noexcept
    {
        try
        {
            if (control.idleCommitTimer)
            {
                control.idleCommitTimer.Stop();
                control.idleCommitTimer.Tick(control.idleCommitToken);
            }
            control.preview.Close();
            control.slider.ValueChanged(control.sliderChanged);
            control.number.ValueChanged(control.numberChanged);
            control.slider.PointerReleased(control.sliderReleased);
            control.number.PointerReleased(control.numberReleased);
            control.slider.LostFocus(control.sliderLostFocus);
            control.number.LostFocus(control.numberLostFocus);
            control.slider.KeyDown(control.sliderKeyDown);
            control.number.KeyDown(control.numberKeyDown);
            if (control.reset)
                control.reset.Click(control.resetClicked);
        }
        catch (...)
        {
        }
    }

    void UnhookColorControl(ColorControl& control) noexcept
    {
        control.editor.Close();
    }

    void CommitOpenColorEditors() noexcept
    {
        for (ColorControl* control : colorControls)
            control->editor.Dismiss();
    }

    void CommitContinuousEdits() noexcept
    {
        try
        {
            for (ContinuousControl* control : continuousControls)
                Commit(*control);
        }
        catch (...)
        {
        }
    }

    void Close() noexcept
    {
        if (closed)
            return;
        CommitOpenColorEditors();
        if (active)
            CommitContinuousEdits();
        active = false;
        closed = true;
        try
        {
            presetCombo.SelectionChanged(presetToken);
            quickNavigationThemeCombo.SelectionChanged(
                quickNavigationThemeToken);
            collectionPopupThemeCombo.SelectionChanged(
                collectionPopupThemeToken);
            gradientToggle.Toggled(gradientToken);
            edgeHighlightToggle.Toggled(edgeHighlightToken);
            glassToggle.Toggled(glassToken);
            acrylicToggle.Toggled(acrylicToken);
            contentThemeCombo.SelectionChanged(contentThemeToken);
            contextMenuCombo.SelectionChanged(contextMenuToken);
        }
        catch (...)
        {
        }
        for (ContinuousControl* control : continuousControls)
            UnhookContinuousControl(*control);
        for (ColorControl* control : colorControls)
            UnhookColorControl(*control);
        actions = {};
        localize = {};
    }
};

PersonalizationPagePresenter::PersonalizationPagePresenter(
    LocalizeCallback localize,
    const mux::Style& cardStyle)
    : impl_(std::make_unique<Impl>(std::move(localize), cardStyle))
{
}

PersonalizationPagePresenter::~PersonalizationPagePresenter()
{
    Close();
}

void PersonalizationPagePresenter::SetActions(
    PersonalizationPageActions actions)
{
    if (impl_ && !impl_->closed)
        impl_->actions = std::move(actions);
}

mux::UIElement PersonalizationPagePresenter::ThemeContent() const noexcept
{
    return impl_ ? impl_->themeRoot : nullptr;
}

mux::UIElement PersonalizationPagePresenter::WidgetLayoutContent()
    const noexcept
{
    return impl_ ? impl_->widgetLayoutRoot : nullptr;
}

void PersonalizationPagePresenter::ApplySnapshot(
    const SettingsSnapshot& snapshot)
{
    if (impl_)
        impl_->ApplySnapshot(snapshot);
}

void PersonalizationPagePresenter::RefreshLocalizedText()
{
    if (impl_)
        impl_->RefreshLocalizedText();
}

void PersonalizationPagePresenter::Activate() noexcept
{
    if (impl_ && !impl_->closed)
        impl_->active = true;
}

void PersonalizationPagePresenter::Deactivate() noexcept
{
    if (!impl_ || impl_->closed) return;
    impl_->CommitOpenColorEditors();
    impl_->CommitContinuousEdits();
    impl_->active = false;
}

mux::FrameworkElement PersonalizationPagePresenter::FocusTarget(
    std::string_view focusId) const noexcept
{
    return impl_ ? impl_->FocusTarget(focusId) : nullptr;
}

void PersonalizationPagePresenter::Close() noexcept
{
    if (impl_)
        impl_->Close();
}

} // namespace snowdesktop::winui

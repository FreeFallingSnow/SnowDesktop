#include "pch.h"

#include "personalization_page_presenter.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxa = winrt::Microsoft::UI::Xaml::Automation;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace muxi = winrt::Microsoft::UI::Xaml::Input;

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
    muxc::StackPanel root{nullptr};
    muxc::TextBlock label{nullptr};
    muxc::StackPanel editors{nullptr};
    muxc::Slider slider{nullptr};
    muxc::NumberBox number{nullptr};
    float PersonalizationSettings::* member = nullptr;
    double scale = 1.0;

    winrt::event_token sliderChanged{};
    winrt::event_token numberChanged{};
    winrt::event_token sliderReleased{};
    winrt::event_token numberReleased{};
    winrt::event_token sliderLostFocus{};
    winrt::event_token numberLostFocus{};
    winrt::event_token sliderKeyDown{};
    winrt::event_token numberKeyDown{};
};

struct ColorControl
{
    muxc::StackPanel root{nullptr};
    muxc::TextBlock label{nullptr};
    muxc::ColorPicker picker{nullptr};
    float PersonalizationSettings::* red = nullptr;
    float PersonalizationSettings::* green = nullptr;
    float PersonalizationSettings::* blue = nullptr;

    winrt::event_token changed{};
    winrt::event_token released{};
    winrt::event_token lostFocus{};
    winrt::event_token keyDown{};
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
    muxc::StackPanel root{nullptr};

    SettingsCard themeCard;
    SettingsCard customCard;
    SettingsCard contextMenuCard;
    SettingsCard layoutCard;

    muxc::ComboBox presetCombo{nullptr};
    muxc::ToggleSwitch glassToggle{nullptr};
    muxc::ToggleSwitch acrylicToggle{nullptr};
    muxc::ComboBox contentThemeCombo{nullptr};
    ColorControl backgroundColor;
    ColorControl borderColor;
    ContinuousControl widgetAlpha;
    ContinuousControl borderAlpha;
    ContinuousControl gradientEndAlpha;
    ContinuousControl blurRadius;
    muxc::ComboBox contextMenuCombo{nullptr};
    ContinuousControl cornerRadius;
    ContinuousControl barHeight;
    ContinuousControl categorizedTabHeight;
    muxc::ToggleSwitch showCategoryTabCountsToggle{nullptr};

    std::array<ContinuousControl*, 7> continuousControls = {
        &widgetAlpha,
        &borderAlpha,
        &gradientEndAlpha,
        &blurRadius,
        &cornerRadius,
        &barHeight,
        &categorizedTabHeight,
    };
    std::array<ColorControl*, 2> colorControls = {
        &backgroundColor,
        &borderColor,
    };

    std::uint64_t generation = 0;
    std::uint64_t personalizationRevision = 0;
    bool hasSnapshot = false;
    bool updatingControls = false;
    bool synchronizingPair = false;
    bool active = false;
    bool closed = false;

    winrt::event_token presetToken{};
    winrt::event_token glassToken{};
    winrt::event_token acrylicToken{};
    winrt::event_token contentThemeToken{};
    winrt::event_token contextMenuToken{};
    winrt::event_token showCountsToken{};

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

    void BuildControls()
    {
        root = muxc::StackPanel{};
        root.Spacing(8.0);

        InitializeCard(themeCard, cardStyle, root);
        presetCombo = muxc::ComboBox{};
        presetCombo.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        presetCombo.MaxWidth(520.0);
        themeCard.content.Children().Append(presetCombo);

        InitializeCard(customCard, cardStyle, root);
        InitializeColorControl(backgroundColor,
            &PersonalizationSettings::widgetBgR,
            &PersonalizationSettings::widgetBgG,
            &PersonalizationSettings::widgetBgB);
        InitializeColorControl(borderColor,
            &PersonalizationSettings::widgetBorderR,
            &PersonalizationSettings::widgetBorderG,
            &PersonalizationSettings::widgetBorderB);
        customCard.content.Children().Append(backgroundColor.root);
        customCard.content.Children().Append(borderColor.root);

        InitializeContinuousControl(widgetAlpha,
            &PersonalizationSettings::widgetAlpha, 0.0, 100.0, 1.0, 0.01);
        InitializeContinuousControl(borderAlpha,
            &PersonalizationSettings::widgetBorderAlpha,
            0.0, 100.0, 1.0, 0.01);
        InitializeContinuousControl(gradientEndAlpha,
            &PersonalizationSettings::gradientEndA,
            0.0, 100.0, 1.0, 0.01);
        InitializeContinuousControl(blurRadius,
            &PersonalizationSettings::glassBlurRadius,
            4.0, 48.0, 1.0, 1.0);
        customCard.content.Children().Append(widgetAlpha.root);
        customCard.content.Children().Append(borderAlpha.root);
        customCard.content.Children().Append(gradientEndAlpha.root);

        glassToggle = muxc::ToggleSwitch{};
        glassToggle.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        acrylicToggle = muxc::ToggleSwitch{};
        acrylicToggle.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        customCard.content.Children().Append(glassToggle);
        customCard.content.Children().Append(blurRadius.root);
        customCard.content.Children().Append(acrylicToggle);

        contentThemeCombo = muxc::ComboBox{};
        contentThemeCombo.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        contentThemeCombo.MaxWidth(520.0);
        customCard.content.Children().Append(contentThemeCombo);

        InitializeCard(contextMenuCard, cardStyle, root);
        contextMenuCombo = muxc::ComboBox{};
        contextMenuCombo.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        contextMenuCombo.MaxWidth(520.0);
        contextMenuCard.content.Children().Append(contextMenuCombo);

        InitializeCard(layoutCard, cardStyle, root);
        InitializeContinuousControl(cornerRadius,
            &PersonalizationSettings::cornerRadius,
            4.0, 28.0, 1.0, 1.0);
        InitializeContinuousControl(barHeight,
            &PersonalizationSettings::barHeight,
            16.0, 48.0, 1.0, 1.0);
        InitializeContinuousControl(categorizedTabHeight,
            &PersonalizationSettings::categorizedTabHeight,
            24.0, 48.0, 1.0, 1.0);
        layoutCard.content.Children().Append(cornerRadius.root);
        layoutCard.content.Children().Append(barHeight.root);
        layoutCard.content.Children().Append(categorizedTabHeight.root);
        showCategoryTabCountsToggle = muxc::ToggleSwitch{};
        showCategoryTabCountsToggle.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        layoutCard.content.Children().Append(showCategoryTabCountsToggle);
    }

    void InitializeColorControl(
        ColorControl& control,
        float PersonalizationSettings::* red,
        float PersonalizationSettings::* green,
        float PersonalizationSettings::* blue)
    {
        control.root = muxc::StackPanel{};
        control.root.Spacing(6.0);
        control.label = muxc::TextBlock{};
        control.label.TextWrapping(mux::TextWrapping::Wrap);
        control.picker = muxc::ColorPicker{};
        control.picker.IsAlphaEnabled(false);
        control.picker.IsMoreButtonVisible(false);
        control.picker.HorizontalAlignment(mux::HorizontalAlignment::Left);
        control.red = red;
        control.green = green;
        control.blue = blue;
        control.root.Children().Append(control.label);
        control.root.Children().Append(control.picker);
    }

    void InitializeContinuousControl(
        ContinuousControl& control,
        float PersonalizationSettings::* member,
        double minimum,
        double maximum,
        double step,
        double scale)
    {
        control.root = muxc::StackPanel{};
        control.root.Spacing(6.0);
        control.label = muxc::TextBlock{};
        control.label.TextWrapping(mux::TextWrapping::Wrap);
        control.editors = muxc::StackPanel{};
        control.editors.Orientation(muxc::Orientation::Horizontal);
        control.editors.Spacing(12.0);
        control.slider = muxc::Slider{};
        control.slider.Minimum(minimum);
        control.slider.Maximum(maximum);
        control.slider.StepFrequency(step);
        control.slider.Width(360.0);
        control.slider.VerticalAlignment(mux::VerticalAlignment::Center);
        control.number = muxc::NumberBox{};
        control.number.Minimum(minimum);
        control.number.Maximum(maximum);
        control.number.SmallChange(step);
        control.number.LargeChange(step * 5.0);
        control.number.SpinButtonPlacementMode(
            muxc::NumberBoxSpinButtonPlacementMode::Compact);
        control.number.Width(128.0);
        control.member = member;
        control.scale = scale;
        control.editors.Children().Append(control.slider);
        control.editors.Children().Append(control.number);
        control.root.Children().Append(control.label);
        control.root.Children().Append(control.editors);
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
                        const bool counts = settings.showCategoryTabCounts;
                        const int menu = settings.contextMenuStyle;
                        settings = MakeAppearancePreset(preset);
                        settings.cornerRadius = corner;
                        settings.barHeight = bar;
                        settings.categorizedTabHeight = tab;
                        settings.showCategoryTabCounts = counts;
                        settings.contextMenuStyle = menu;
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
        showCountsToken = showCategoryTabCountsToggle.Toggled(
            [this](const auto&, const auto&) {
                const bool value = showCategoryTabCountsToggle.IsOn();
                Emit(SettingsUpdateMode::PreviewAndCommit,
                    [value](PersonalizationSettings& settings) {
                        settings.showCategoryTabCounts = value;
                    });
            });

        for (ContinuousControl* control : continuousControls)
            HookContinuousControl(*control);
        for (ColorControl* control : colorControls)
            HookColorControl(*control);
    }

    void HookContinuousControl(ContinuousControl& control)
    {
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
    }

    void HookColorControl(ColorControl& control)
    {
        control.changed = control.picker.ColorChanged(
            [this, &control](const auto&, const auto&) {
                Preview(control);
            });
        control.released = control.picker.PointerReleased(
            [this, &control](const auto&, const auto&) {
                Commit(control);
            });
        control.lostFocus = control.picker.LostFocus(
            [this, &control](const auto&, const auto&) {
                Commit(control);
            });
        control.keyDown = control.picker.KeyDown(
            [this, &control](const auto&, const muxi::KeyRoutedEventArgs& args) {
                if (IsEnter(args))
                    Commit(control);
            });
    }

    void Preview(ContinuousControl& control, double uiValue)
    {
        if (!CanEmit())
            return;
        const auto member = control.member;
        const float value = static_cast<float>(uiValue * control.scale);
        Emit(SettingsUpdateMode::Preview,
            [member, value](PersonalizationSettings& settings) {
                settings.*member = value;
            });
    }

    void Commit(ContinuousControl& control)
    {
        if (!CanEmit())
            return;
        const auto member = control.member;
        const float value = static_cast<float>(
            control.slider.Value() * control.scale);
        Emit(SettingsUpdateMode::PreviewAndCommit,
            [member, value](PersonalizationSettings& settings) {
                settings.*member = value;
            });
    }

    void Preview(ColorControl& control)
    {
        if (!CanEmit())
            return;
        const auto color = control.picker.Color();
        const auto red = control.red;
        const auto green = control.green;
        const auto blue = control.blue;
        const float redValue = FromByte(color.R);
        const float greenValue = FromByte(color.G);
        const float blueValue = FromByte(color.B);
        Emit(SettingsUpdateMode::Preview,
            [red, green, blue, redValue, greenValue, blueValue](
                PersonalizationSettings& settings) {
                settings.*red = redValue;
                settings.*green = greenValue;
                settings.*blue = blueValue;
            });
    }

    void Commit(ColorControl& control)
    {
        if (!CanEmit())
            return;
        const auto color = control.picker.Color();
        const auto red = control.red;
        const auto green = control.green;
        const auto blue = control.blue;
        const float redValue = FromByte(color.R);
        const float greenValue = FromByte(color.G);
        const float blueValue = FromByte(color.B);
        Emit(SettingsUpdateMode::PreviewAndCommit,
            [red, green, blue, redValue, greenValue, blueValue](
                PersonalizationSettings& settings) {
                settings.*red = redValue;
                settings.*green = greenValue;
                settings.*blue = blueValue;
            });
    }

    void PatchContinuous(
        ContinuousControl& control,
        const PersonalizationSettings& settings)
    {
        const double value = static_cast<double>(settings.*control.member) /
            control.scale;
        const double clamped = std::clamp(
            value, control.slider.Minimum(), control.slider.Maximum());
        control.slider.Value(clamped);
        control.number.Value(clamped);
    }

    void PatchColor(
        ColorControl& control,
        const PersonalizationSettings& settings)
    {
        control.picker.Color(ToColor(
            settings.*control.red,
            settings.*control.green,
            settings.*control.blue));
    }

    void Patch(const PersonalizationSettings& settings)
    {
        const int normalized = NormalizeAppearancePresetId(
            settings.backgroundPreset);
        auto preset = std::find(kPresetIds.begin(), kPresetIds.end(), normalized);
        presetCombo.SelectedIndex(preset == kPresetIds.end()
            ? static_cast<int>(kPresetIds.size() - 1)
            : static_cast<int>(std::distance(kPresetIds.begin(), preset)));
        PatchColor(backgroundColor, settings);
        PatchColor(borderColor, settings);
        for (ContinuousControl* control : continuousControls)
            PatchContinuous(*control, settings);
        glassToggle.IsOn(settings.glassEnabled);
        acrylicToggle.IsOn(settings.acrylicEnabled);
        contentThemeCombo.SelectedIndex(
            std::clamp(settings.contentTheme, 0, 1));
        contextMenuCombo.SelectedIndex(
            std::clamp(settings.contextMenuStyle, 0, 4));
        showCategoryTabCountsToggle.IsOn(settings.showCategoryTabCounts);
        UpdateDependentStates();
    }

    void UpdateDependentStates()
    {
        if (closed)
            return;
        const bool custom = presetCombo.SelectedIndex() ==
            static_cast<int>(kPresetIds.size() - 1);
        customCard.root.Opacity(custom ? 1.0 : 0.62);
        backgroundColor.picker.IsEnabled(custom);
        borderColor.picker.IsEnabled(custom);
        for (ContinuousControl* control : {
                 &widgetAlpha, &borderAlpha, &gradientEndAlpha})
        {
            control->slider.IsEnabled(custom);
            control->number.IsEnabled(custom);
        }
        glassToggle.IsEnabled(custom);
        blurRadius.slider.IsEnabled(custom && glassToggle.IsOn());
        blurRadius.number.IsEnabled(custom && glassToggle.IsOn());
        acrylicToggle.IsEnabled(custom && glassToggle.IsOn());
        contentThemeCombo.IsEnabled(custom);
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
        control.label.Text(L(key, fallback));
        muxa::AutomationProperties::SetName(
            control.slider, control.label.Text());
        muxa::AutomationProperties::SetName(
            control.number, control.label.Text());
    }

    void SetColorText(
        ColorControl& control,
        std::string_view key,
        std::wstring_view fallback)
    {
        control.label.Text(L(key, fallback));
        muxa::AutomationProperties::SetName(
            control.picker, control.label.Text());
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
        SetCardText(customCard,
            "app.settings.component_bg", L"Widget Appearance");
        SetCardText(contextMenuCard,
            "app.settings.context_menu_appearance", L"Context Menu");
        SetCardText(layoutCard,
            "app.settings.widget_layout", L"Widget Layout");

        presetCombo.Header(winrt::box_value(
            L("app.settings.theme", L"Theme")));
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

        SetColorText(backgroundColor,
            "app.settings.component_bg", L"Widget Background");
        SetColorText(borderColor,
            "app.settings.component_border", L"Widget Border");
        SetContinuousText(widgetAlpha,
            "app.settings.bg_opacity", L"Background Opacity");
        SetContinuousText(borderAlpha,
            "app.settings.border_opacity", L"Border Opacity");
        SetContinuousText(gradientEndAlpha,
            "app.settings.gradient_end_alpha", L"Gradient End Opacity");
        SetContinuousText(blurRadius,
            "app.settings.blur_radius", L"Blur Radius");

        glassToggle.Header(winrt::box_value(
            L("app.settings.glass_enabled", L"Frosted Glass Background")));
        acrylicToggle.Header(winrt::box_value(
            L("app.settings.acrylic_noise", L"Acrylic Noise")));
        contentThemeCombo.Header(winrt::box_value(
            L("app.settings.widget_content_theme", L"Widget Foreground Colors")));
        ReplaceComboItems(contentThemeCombo, {
            {"app.settings.light", L"Light"},
            {"app.settings.dark", L"Dark"},
        });

        contextMenuCombo.Header(winrt::box_value(
            L("app.settings.context_menu_style", L"Menu Style")));
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
            "app.settings.tab_height", L"Tab and Search Box Height");
        showCategoryTabCountsToggle.Header(winrt::box_value(
            L("app.settings.category_show_count",
                L"Show File Counts on Category Tabs")));

        for (const auto& toggle : {
                 glassToggle, acrylicToggle, showCategoryTabCountsToggle})
        {
            muxa::AutomationProperties::SetName(
                toggle, winrt::unbox_value_or<winrt::hstring>(
                    toggle.Header(), winrt::hstring{}));
        }
        muxa::AutomationProperties::SetName(
            contentThemeCombo,
            L("app.settings.widget_content_theme", L"Widget Foreground Colors"));
        muxa::AutomationProperties::SetName(
            contextMenuCombo,
            L("app.settings.context_menu_style", L"Menu Style"));

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
        if (!newGeneration && snapshot.domainRevisions.personalization ==
                personalizationRevision)
        {
            hasSnapshot = true;
            return;
        }

        const bool previousUpdating = updatingControls;
        updatingControls = true;
        Patch(snapshot.values.personalization);
        personalizationRevision =
            snapshot.domainRevisions.personalization;
        hasSnapshot = true;
        updatingControls = previousUpdating;
    }

    mux::FrameworkElement FocusTarget(std::string_view id) const noexcept
    {
        if (id == "personalization.theme" ||
            id == "personalization.globalTheme")
            return presetCombo;
        if (id == "personalization.backgroundColor")
            return backgroundColor.picker;
        if (id == "personalization.borderColor")
            return borderColor.picker;
        if (id == "personalization.widgetAlpha" ||
            id == "personalization.backgroundOpacity")
            return widgetAlpha.slider;
        if (id == "personalization.borderAlpha" ||
            id == "personalization.borderOpacity")
            return borderAlpha.slider;
        if (id == "personalization.gradientEndAlpha")
            return gradientEndAlpha.slider;
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
        if (id == "personalization.tabHeight")
            return categorizedTabHeight.slider;
        if (id == "personalization.showCategoryTabCounts" ||
            id == "personalization.showCounts")
            return showCategoryTabCountsToggle;
        return nullptr;
    }

    void UnhookContinuousControl(ContinuousControl& control) noexcept
    {
        try
        {
            control.slider.ValueChanged(control.sliderChanged);
            control.number.ValueChanged(control.numberChanged);
            control.slider.PointerReleased(control.sliderReleased);
            control.number.PointerReleased(control.numberReleased);
            control.slider.LostFocus(control.sliderLostFocus);
            control.number.LostFocus(control.numberLostFocus);
            control.slider.KeyDown(control.sliderKeyDown);
            control.number.KeyDown(control.numberKeyDown);
        }
        catch (...)
        {
        }
    }

    void UnhookColorControl(ColorControl& control) noexcept
    {
        try
        {
            control.picker.ColorChanged(control.changed);
            control.picker.PointerReleased(control.released);
            control.picker.LostFocus(control.lostFocus);
            control.picker.KeyDown(control.keyDown);
        }
        catch (...)
        {
        }
    }

    void Close() noexcept
    {
        if (closed)
            return;
        active = false;
        closed = true;
        try
        {
            presetCombo.SelectionChanged(presetToken);
            glassToggle.Toggled(glassToken);
            acrylicToggle.Toggled(acrylicToken);
            contentThemeCombo.SelectionChanged(contentThemeToken);
            contextMenuCombo.SelectionChanged(contextMenuToken);
            showCategoryTabCountsToggle.Toggled(showCountsToken);
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

mux::UIElement PersonalizationPagePresenter::Content() const noexcept
{
    return impl_ ? impl_->root : nullptr;
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
    if (impl_ && !impl_->closed)
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

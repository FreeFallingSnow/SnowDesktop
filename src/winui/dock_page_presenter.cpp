#include "pch.h"

#include "dock_page_presenter.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <utility>

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxa = winrt::Microsoft::UI::Xaml::Automation;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace muxi = winrt::Microsoft::UI::Xaml::Input;

namespace
{

struct SettingsCard
{
    muxc::Border root{nullptr};
    muxc::StackPanel content{nullptr};
    muxc::TextBlock title{nullptr};
};

enum class ContinuousField
{
    ThicknessScale,
    FrequentItemCount,
    TaskbarBackgroundAlpha,
    TaskbarBorderAlpha,
    TaskbarBlurRadius,
};

struct ContinuousControl
{
    muxc::StackPanel root{nullptr};
    muxc::TextBlock label{nullptr};
    muxc::StackPanel editors{nullptr};
    muxc::Slider slider{nullptr};
    muxc::NumberBox number{nullptr};
    ContinuousField field = ContinuousField::ThicknessScale;
    bool dirty = false;

    winrt::event_token sliderChanged{};
    winrt::event_token numberChanged{};
    winrt::event_token sliderReleased{};
    winrt::event_token numberReleased{};
    winrt::event_token sliderLostFocus{};
    winrt::event_token numberLostFocus{};
    winrt::event_token sliderKeyDown{};
    winrt::event_token numberKeyDown{};
};

enum class ColorField
{
    TaskbarBackground,
    TaskbarBorder,
};

struct ColorControl
{
    muxc::StackPanel root{nullptr};
    muxc::TextBlock label{nullptr};
    muxc::ColorPicker picker{nullptr};
    ColorField field = ColorField::TaskbarBackground;
    bool dirty = false;

    winrt::event_token changed{};
    winrt::event_token released{};
    winrt::event_token lostFocus{};
    winrt::event_token keyDown{};
};

struct DynamicRuleControl
{
    SettingsCard card;
    muxc::ToggleSwitch enabled{nullptr};
    muxc::ComboBox theme{nullptr};
    muxc::ComboBox contentTheme{nullptr};
    SystemTaskbarDynamicRule DockSettings::* member = nullptr;

    winrt::event_token enabledToken{};
    winrt::event_token themeToken{};
    winrt::event_token contentThemeToken{};
};

struct ConfirmationGate
{
    std::atomic_bool alive{true};
    std::atomic<std::uint64_t> generation{0};
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

bool IsEnter(const muxi::KeyRoutedEventArgs& args) noexcept
{
    return args.Key() == winrt::Windows::System::VirtualKey::Enter;
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

int MainTaskbarThemeMode(const DockSettings& settings) noexcept
{
    if (!settings.systemTaskbarBackdropEnabled)
        return static_cast<int>(SystemTaskbarThemeMode::Native);
    if (settings.systemTaskbarFollowPersonalization)
        return static_cast<int>(SystemTaskbarThemeMode::FollowGlobal);

    const int preset = NormalizeAppearancePresetId(
        settings.systemTaskbarAppearance.backgroundPreset);
    switch (preset)
    {
    case kAppearancePresetDark:
        return static_cast<int>(SystemTaskbarThemeMode::Dark);
    case kAppearancePresetLight:
        return static_cast<int>(SystemTaskbarThemeMode::Light);
    case kAppearancePresetGlassDark:
        return static_cast<int>(SystemTaskbarThemeMode::GlassDark);
    case kAppearancePresetGlassLight:
        return static_cast<int>(SystemTaskbarThemeMode::GlassLight);
    case kAppearancePresetAcrylicDark:
        return static_cast<int>(SystemTaskbarThemeMode::AcrylicDark);
    case kAppearancePresetAcrylicLight:
        return static_cast<int>(SystemTaskbarThemeMode::AcrylicLight);
    case kAppearancePresetTaskbarTransparent:
        return static_cast<int>(SystemTaskbarThemeMode::Transparent);
    case kAppearancePresetCustom:
    default:
        return static_cast<int>(SystemTaskbarThemeMode::Custom);
    }
}

int PresetForTaskbarMode(SystemTaskbarThemeMode mode) noexcept
{
    switch (mode)
    {
    case SystemTaskbarThemeMode::Dark:
        return kAppearancePresetDark;
    case SystemTaskbarThemeMode::Light:
        return kAppearancePresetLight;
    case SystemTaskbarThemeMode::GlassDark:
        return kAppearancePresetGlassDark;
    case SystemTaskbarThemeMode::GlassLight:
        return kAppearancePresetGlassLight;
    case SystemTaskbarThemeMode::AcrylicDark:
        return kAppearancePresetAcrylicDark;
    case SystemTaskbarThemeMode::AcrylicLight:
        return kAppearancePresetAcrylicLight;
    default:
        return -1;
    }
}

void ApplyMainTaskbarTheme(DockSettings& settings, int selectedIndex)
{
    const auto mode = static_cast<SystemTaskbarThemeMode>(std::clamp(
        selectedIndex,
        static_cast<int>(SystemTaskbarThemeMode::Native),
        static_cast<int>(SystemTaskbarThemeMode::Transparent)));
    switch (mode)
    {
    case SystemTaskbarThemeMode::Native:
        settings.systemTaskbarBackdropEnabled = false;
        break;
    case SystemTaskbarThemeMode::FollowGlobal:
        settings.systemTaskbarBackdropEnabled = true;
        settings.systemTaskbarFollowPersonalization = true;
        settings.systemTaskbarContentTheme = -1;
        break;
    case SystemTaskbarThemeMode::Dark:
    case SystemTaskbarThemeMode::Light:
    case SystemTaskbarThemeMode::GlassDark:
    case SystemTaskbarThemeMode::GlassLight:
    case SystemTaskbarThemeMode::AcrylicDark:
    case SystemTaskbarThemeMode::AcrylicLight:
        settings.systemTaskbarBackdropEnabled = true;
        settings.systemTaskbarFollowPersonalization = false;
        settings.systemTaskbarContentTheme = -1;
        settings.systemTaskbarAppearance = MakeAppearancePreset(
            PresetForTaskbarMode(mode));
        break;
    case SystemTaskbarThemeMode::Custom:
        settings.systemTaskbarBackdropEnabled = true;
        settings.systemTaskbarFollowPersonalization = false;
        settings.systemTaskbarAppearance.backgroundPreset =
            kAppearancePresetCustom;
        if (settings.systemTaskbarContentTheme < 0)
        {
            settings.systemTaskbarContentTheme = std::clamp(
                settings.systemTaskbarAppearance.contentTheme, 0, 1);
        }
        break;
    case SystemTaskbarThemeMode::Transparent:
        settings.systemTaskbarBackdropEnabled = true;
        settings.systemTaskbarFollowPersonalization = false;
        settings.systemTaskbarContentTheme = -1;
        settings.systemTaskbarAppearance =
            MakeTransparentTaskbarAppearance();
        break;
    }
}

void PrepareDynamicRuleTheme(
    SystemTaskbarDynamicRule& rule,
    SystemTaskbarThemeMode mode)
{
    const SystemTaskbarThemeMode previous = rule.themeMode;
    rule.themeMode = mode;
    if (mode != SystemTaskbarThemeMode::Custom)
        return;

    const int preset = PresetForTaskbarMode(previous);
    if (preset >= 0)
        rule.appearance = MakeAppearancePreset(preset);
    rule.appearance.backgroundPreset = kAppearancePresetCustom;
}

} // namespace

struct DockPagePresenter::Impl
{
    explicit Impl(LocalizeCallback callback, const mux::Style& style)
        : localize(std::move(callback)), cardStyle(style),
          confirmationGate(std::make_shared<ConfirmationGate>())
    {
        BuildControls();
        HookEvents();
        RefreshLocalizedText();
    }

    LocalizeCallback localize;
    DockPageActions actions;
    mux::Style cardStyle{nullptr};
    muxc::StackPanel root{nullptr};

    SettingsCard enableCard;
    SettingsCard layoutCard;
    SettingsCard behaviorCard;
    SettingsCard taskbarCard;
    SettingsCard taskbarAppearanceCard;
    SettingsCard taskbarCustomCard;

    muxc::ToggleSwitch dockEnabledToggle{nullptr};
    muxc::ComboBox positionCombo{nullptr};
    muxc::ComboBox layoutCombo{nullptr};
    muxc::ComboBox monitorScopeCombo{nullptr};
    muxc::ToggleSwitch floatingShortcutToggle{nullptr};
    muxc::TextBlock floatingShortcutHint{nullptr};
    muxc::ToggleSwitch floatingEdgeSwipeToggle{nullptr};
    muxc::TextBlock floatingEdgeSwipeHint{nullptr};
    muxc::ToggleSwitch showWindowsButtonToggle{nullptr};
    muxc::ToggleSwitch showFrequentItemsToggle{nullptr};
    muxc::ToggleSwitch keepWhenDesktopHiddenToggle{nullptr};
    ContinuousControl thicknessScale;
    ContinuousControl frequentItemCount;

    muxc::ToggleSwitch taskbarAutoHideToggle{nullptr};
    muxc::ComboBox taskbarAlignmentCombo{nullptr};
    muxc::Button restartExplorerButton{nullptr};
    muxc::TextBlock restartExplorerHint{nullptr};
    muxc::ComboBox taskbarThemeCombo{nullptr};
    muxc::ComboBox taskbarContentThemeCombo{nullptr};
    ColorControl taskbarBackgroundColor;
    ColorControl taskbarBorderColor;
    ContinuousControl taskbarBackgroundAlpha;
    ContinuousControl taskbarBorderAlpha;
    ContinuousControl taskbarBlurRadius;
    muxc::ToggleSwitch taskbarGlassToggle{nullptr};
    muxc::ToggleSwitch taskbarAcrylicToggle{nullptr};

    DynamicRuleControl visibleWindowRule;
    DynamicRuleControl maximizedWindowRule;
    DynamicRuleControl shellUiRule;
    std::array<DynamicRuleControl*, 3> dynamicRules = {
        &visibleWindowRule, &maximizedWindowRule, &shellUiRule};
    std::array<ContinuousControl*, 5> continuousControls = {
        &thicknessScale,
        &frequentItemCount,
        &taskbarBackgroundAlpha,
        &taskbarBorderAlpha,
        &taskbarBlurRadius,
    };
    std::array<ColorControl*, 2> colorControls = {
        &taskbarBackgroundColor, &taskbarBorderColor};

    std::shared_ptr<ConfirmationGate> confirmationGate;
    std::uint64_t generation = 0;
    std::uint64_t generalRevision = 0;
    std::uint64_t dockRevision = 0;
    bool hasSnapshot = false;
    bool updatingControls = false;
    bool synchronizingPair = false;
    bool active = false;
    bool closed = false;

    winrt::event_token dockEnabledToken{};
    winrt::event_token positionToken{};
    winrt::event_token layoutToken{};
    winrt::event_token monitorScopeToken{};
    winrt::event_token floatingShortcutToken{};
    winrt::event_token floatingEdgeSwipeToken{};
    winrt::event_token showWindowsButtonToken{};
    winrt::event_token showFrequentItemsToken{};
    winrt::event_token keepWhenDesktopHiddenToken{};
    winrt::event_token taskbarAutoHideToken{};
    winrt::event_token taskbarAlignmentToken{};
    winrt::event_token restartExplorerToken{};
    winrt::event_token taskbarThemeToken{};
    winrt::event_token taskbarContentThemeToken{};
    winrt::event_token taskbarGlassToken{};
    winrt::event_token taskbarAcrylicToken{};

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

    [[nodiscard]] bool CanEmitGeneral() const noexcept
    {
        return !closed && active && hasSnapshot && !updatingControls &&
            !synchronizingPair && static_cast<bool>(actions.updateGeneral);
    }

    [[nodiscard]] bool CanEmitDock() const noexcept
    {
        return !closed && active && hasSnapshot && !updatingControls &&
            !synchronizingPair && static_cast<bool>(actions.updateDock);
    }

    template <typename Edit>
    void EmitGeneral(SettingsUpdateMode mode, Edit edit)
    {
        if (!CanEmitGeneral())
            return;
        actions.updateGeneral(generation, mode,
            DockPageActions::GeneralEdit(std::move(edit)));
    }

    template <typename Edit>
    void EmitDock(SettingsUpdateMode mode, Edit edit)
    {
        if (!CanEmitDock())
            return;
        actions.updateDock(generation, mode,
            DockPageActions::DockEdit(std::move(edit)));
    }

    void BuildControls()
    {
        root = muxc::StackPanel{};
        root.Spacing(8.0);

        InitializeCard(enableCard, cardStyle, root);
        dockEnabledToggle = muxc::ToggleSwitch{};
        dockEnabledToggle.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        enableCard.content.Children().Append(dockEnabledToggle);

        InitializeCard(layoutCard, cardStyle, root);
        positionCombo = NewCombo();
        layoutCombo = NewCombo();
        monitorScopeCombo = NewCombo();
        InitializeContinuousControl(thicknessScale,
            ContinuousField::ThicknessScale, 50.0, 100.0, 1.0);
        layoutCard.content.Children().Append(positionCombo);
        layoutCard.content.Children().Append(layoutCombo);
        layoutCard.content.Children().Append(monitorScopeCombo);
        layoutCard.content.Children().Append(thicknessScale.root);

        InitializeCard(behaviorCard, cardStyle, root);
        floatingShortcutToggle = muxc::ToggleSwitch{};
        floatingEdgeSwipeToggle = muxc::ToggleSwitch{};
        showWindowsButtonToggle = muxc::ToggleSwitch{};
        showFrequentItemsToggle = muxc::ToggleSwitch{};
        keepWhenDesktopHiddenToggle = muxc::ToggleSwitch{};
        for (const auto& toggle : {
                 floatingShortcutToggle,
                 floatingEdgeSwipeToggle,
                 showWindowsButtonToggle,
                 showFrequentItemsToggle,
                 keepWhenDesktopHiddenToggle})
        {
            toggle.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        }
        floatingShortcutHint = NewHint();
        floatingEdgeSwipeHint = NewHint();
        InitializeContinuousControl(frequentItemCount,
            ContinuousField::FrequentItemCount, 1.0, 8.0, 1.0);
        behaviorCard.content.Children().Append(floatingShortcutToggle);
        behaviorCard.content.Children().Append(floatingShortcutHint);
        behaviorCard.content.Children().Append(floatingEdgeSwipeToggle);
        behaviorCard.content.Children().Append(floatingEdgeSwipeHint);
        behaviorCard.content.Children().Append(showWindowsButtonToggle);
        behaviorCard.content.Children().Append(showFrequentItemsToggle);
        behaviorCard.content.Children().Append(frequentItemCount.root);
        behaviorCard.content.Children().Append(keepWhenDesktopHiddenToggle);

        InitializeCard(taskbarCard, cardStyle, root);
        taskbarAutoHideToggle = muxc::ToggleSwitch{};
        taskbarAutoHideToggle.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        taskbarAlignmentCombo = NewCombo();
        restartExplorerHint = NewHint();
        restartExplorerButton = muxc::Button{};
        restartExplorerButton.HorizontalAlignment(
            mux::HorizontalAlignment::Left);
        taskbarCard.content.Children().Append(taskbarAutoHideToggle);
        taskbarCard.content.Children().Append(taskbarAlignmentCombo);
        taskbarCard.content.Children().Append(restartExplorerHint);
        taskbarCard.content.Children().Append(restartExplorerButton);

        InitializeCard(taskbarAppearanceCard, cardStyle, root);
        taskbarThemeCombo = NewCombo();
        taskbarContentThemeCombo = NewCombo();
        taskbarAppearanceCard.content.Children().Append(taskbarThemeCombo);
        taskbarAppearanceCard.content.Children().Append(
            taskbarContentThemeCombo);

        InitializeCard(taskbarCustomCard, cardStyle, root);
        InitializeColorControl(taskbarBackgroundColor,
            ColorField::TaskbarBackground);
        InitializeColorControl(taskbarBorderColor,
            ColorField::TaskbarBorder);
        InitializeContinuousControl(taskbarBackgroundAlpha,
            ContinuousField::TaskbarBackgroundAlpha, 0.0, 100.0, 1.0);
        InitializeContinuousControl(taskbarBorderAlpha,
            ContinuousField::TaskbarBorderAlpha, 0.0, 100.0, 1.0);
        InitializeContinuousControl(taskbarBlurRadius,
            ContinuousField::TaskbarBlurRadius, 4.0, 48.0, 1.0);
        taskbarGlassToggle = muxc::ToggleSwitch{};
        taskbarAcrylicToggle = muxc::ToggleSwitch{};
        taskbarCustomCard.content.Children().Append(taskbarBackgroundColor.root);
        taskbarCustomCard.content.Children().Append(taskbarBorderColor.root);
        taskbarCustomCard.content.Children().Append(taskbarBackgroundAlpha.root);
        taskbarCustomCard.content.Children().Append(taskbarBorderAlpha.root);
        taskbarCustomCard.content.Children().Append(taskbarGlassToggle);
        taskbarCustomCard.content.Children().Append(taskbarBlurRadius.root);
        taskbarCustomCard.content.Children().Append(taskbarAcrylicToggle);

        InitializeDynamicRule(visibleWindowRule,
            &DockSettings::systemTaskbarVisibleWindow);
        InitializeDynamicRule(maximizedWindowRule,
            &DockSettings::systemTaskbarMaximizedWindow);
        InitializeDynamicRule(shellUiRule,
            &DockSettings::systemTaskbarShellUi);
    }

    muxc::ComboBox NewCombo()
    {
        muxc::ComboBox combo;
        combo.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        combo.MaxWidth(520.0);
        return combo;
    }

    muxc::TextBlock NewHint()
    {
        muxc::TextBlock hint;
        hint.Opacity(0.72);
        hint.TextWrapping(mux::TextWrapping::Wrap);
        return hint;
    }

    void InitializeContinuousControl(
        ContinuousControl& control,
        ContinuousField field,
        double minimum,
        double maximum,
        double step)
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
        control.number = muxc::NumberBox{};
        control.number.Minimum(minimum);
        control.number.Maximum(maximum);
        control.number.SmallChange(step);
        control.number.LargeChange(step * 5.0);
        control.number.SpinButtonPlacementMode(
            muxc::NumberBoxSpinButtonPlacementMode::Compact);
        control.number.Width(128.0);
        control.field = field;
        control.editors.Children().Append(control.slider);
        control.editors.Children().Append(control.number);
        control.root.Children().Append(control.label);
        control.root.Children().Append(control.editors);
    }

    void InitializeColorControl(ColorControl& control, ColorField field)
    {
        control.root = muxc::StackPanel{};
        control.root.Spacing(6.0);
        control.label = muxc::TextBlock{};
        control.label.TextWrapping(mux::TextWrapping::Wrap);
        control.picker = muxc::ColorPicker{};
        control.picker.IsAlphaEnabled(false);
        control.picker.IsMoreButtonVisible(false);
        control.picker.HorizontalAlignment(mux::HorizontalAlignment::Left);
        control.field = field;
        control.root.Children().Append(control.label);
        control.root.Children().Append(control.picker);
    }

    void InitializeDynamicRule(
        DynamicRuleControl& control,
        SystemTaskbarDynamicRule DockSettings::* member)
    {
        InitializeCard(control.card, cardStyle, root);
        control.enabled = muxc::ToggleSwitch{};
        control.enabled.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        control.theme = NewCombo();
        control.contentTheme = NewCombo();
        control.member = member;
        control.card.content.Children().Append(control.enabled);
        control.card.content.Children().Append(control.theme);
        control.card.content.Children().Append(control.contentTheme);
    }

    void HookEvents()
    {
        dockEnabledToken = dockEnabledToggle.Toggled(
            [this](const auto&, const auto&) {
                UpdateDependentStates();
                const bool value = dockEnabledToggle.IsOn();
                EmitGeneral(SettingsUpdateMode::PreviewAndCommit,
                    [value](GeneralSettings& settings) {
                        settings.dockEnabled = value;
                    });
            });
        positionToken = positionCombo.SelectionChanged(
            [this](const auto&, const auto&) {
                const int value = positionCombo.SelectedIndex();
                if (value < 0) return;
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [value](DockSettings& settings) {
                        settings.position = static_cast<DockPosition>(
                            std::clamp(value, 0, 3));
                    });
            });
        layoutToken = layoutCombo.SelectionChanged(
            [this](const auto&, const auto&) {
                const int value = layoutCombo.SelectedIndex();
                if (value < 0) return;
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [value](DockSettings& settings) {
                        settings.edgeAttached = value == 1;
                    });
            });
        monitorScopeToken = monitorScopeCombo.SelectionChanged(
            [this](const auto&, const auto&) {
                const int value = monitorScopeCombo.SelectedIndex();
                if (value < 0) return;
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [value](DockSettings& settings) {
                        settings.monitorScope = static_cast<DockMonitorScope>(
                            std::clamp(value, 0, 2));
                    });
            });
        floatingShortcutToken = floatingShortcutToggle.Toggled(
            [this](const auto&, const auto&) {
                const bool value = floatingShortcutToggle.IsOn();
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [value](DockSettings& settings) {
                        settings.floatingShortcutMode = value;
                    });
            });
        floatingEdgeSwipeToken = floatingEdgeSwipeToggle.Toggled(
            [this](const auto&, const auto&) {
                const bool value = floatingEdgeSwipeToggle.IsOn();
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [value](DockSettings& settings) {
                        settings.floatingEdgeSwipeEnabled = value;
                    });
            });
        showWindowsButtonToken = showWindowsButtonToggle.Toggled(
            [this](const auto&, const auto&) {
                const bool value = showWindowsButtonToggle.IsOn();
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [value](DockSettings& settings) {
                        settings.showWindowsButton = value;
                    });
            });
        showFrequentItemsToken = showFrequentItemsToggle.Toggled(
            [this](const auto&, const auto&) {
                UpdateDependentStates();
                const bool value = showFrequentItemsToggle.IsOn();
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [value](DockSettings& settings) {
                        settings.showFrequentItems = value;
                    });
            });
        keepWhenDesktopHiddenToken = keepWhenDesktopHiddenToggle.Toggled(
            [this](const auto&, const auto&) {
                const bool value = keepWhenDesktopHiddenToggle.IsOn();
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [value](DockSettings& settings) {
                        settings.keepWhenDesktopHidden = value;
                    });
            });
        taskbarAutoHideToken = taskbarAutoHideToggle.Toggled(
            [this](const auto&, const auto&) {
                const bool value = taskbarAutoHideToggle.IsOn();
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [value](DockSettings& settings) {
                        settings.systemTaskbarAutoHide = value;
                    });
            });
        taskbarAlignmentToken = taskbarAlignmentCombo.SelectionChanged(
            [this](const auto&, const auto&) {
                const int value = taskbarAlignmentCombo.SelectedIndex();
                if (value < 0) return;
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [value](DockSettings& settings) {
                        settings.systemTaskbarAlignment =
                            std::clamp(value, 0, 1);
                    });
            });
        taskbarThemeToken = taskbarThemeCombo.SelectionChanged(
            [this](const auto&, const auto&) {
                UpdateDependentStates();
                const int value = taskbarThemeCombo.SelectedIndex();
                if (value < 0) return;
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [value](DockSettings& settings) {
                        ApplyMainTaskbarTheme(settings, value);
                    });
            });
        taskbarContentThemeToken = taskbarContentThemeCombo.SelectionChanged(
            [this](const auto&, const auto&) {
                const int value = taskbarContentThemeCombo.SelectedIndex();
                if (value < 0) return;
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [value](DockSettings& settings) {
                        settings.systemTaskbarContentTheme =
                            std::clamp(value - 1, -1, 1);
                    });
            });
        taskbarGlassToken = taskbarGlassToggle.Toggled(
            [this](const auto&, const auto&) {
                UpdateDependentStates();
                const bool value = taskbarGlassToggle.IsOn();
                EmitCustomAppearance(SettingsUpdateMode::PreviewAndCommit,
                    [value](PersonalizationSettings& appearance) {
                        appearance.glassEnabled = value;
                    });
            });
        taskbarAcrylicToken = taskbarAcrylicToggle.Toggled(
            [this](const auto&, const auto&) {
                const bool value = taskbarAcrylicToggle.IsOn();
                EmitCustomAppearance(SettingsUpdateMode::PreviewAndCommit,
                    [value](PersonalizationSettings& appearance) {
                        appearance.acrylicEnabled = value;
                    });
            });
        restartExplorerToken = restartExplorerButton.Click(
            [this](const auto&, const auto&) {
                ConfirmRestartExplorer();
            });

        for (ContinuousControl* control : continuousControls)
            HookContinuousControl(*control);
        for (ColorControl* control : colorControls)
            HookColorControl(*control);
        for (DynamicRuleControl* control : dynamicRules)
            HookDynamicRule(*control);
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
            [this, &control](const auto&, const auto&) { Commit(control); });
        control.numberReleased = control.number.PointerReleased(
            [this, &control](const auto&, const auto&) { Commit(control); });
        control.sliderLostFocus = control.slider.LostFocus(
            [this, &control](const auto&, const auto&) { Commit(control); });
        control.numberLostFocus = control.number.LostFocus(
            [this, &control](const auto&, const auto&) { Commit(control); });
        control.sliderKeyDown = control.slider.KeyDown(
            [this, &control](const auto&, const muxi::KeyRoutedEventArgs& args) {
                if (IsEnter(args)) Commit(control);
            });
        control.numberKeyDown = control.number.KeyDown(
            [this, &control](const auto&, const muxi::KeyRoutedEventArgs& args) {
                if (IsEnter(args)) Commit(control);
            });
    }

    void HookColorControl(ColorControl& control)
    {
        control.changed = control.picker.ColorChanged(
            [this, &control](const auto&, const auto&) { Preview(control); });
        control.released = control.picker.PointerReleased(
            [this, &control](const auto&, const auto&) { Commit(control); });
        control.lostFocus = control.picker.LostFocus(
            [this, &control](const auto&, const auto&) { Commit(control); });
        control.keyDown = control.picker.KeyDown(
            [this, &control](const auto&, const muxi::KeyRoutedEventArgs& args) {
                if (IsEnter(args)) Commit(control);
            });
    }

    void HookDynamicRule(DynamicRuleControl& control)
    {
        control.enabledToken = control.enabled.Toggled(
            [this, &control](const auto&, const auto&) {
                UpdateDependentStates();
                const bool value = control.enabled.IsOn();
                const auto member = control.member;
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [member, value](DockSettings& settings) {
                        (settings.*member).enabled = value;
                    });
            });
        control.themeToken = control.theme.SelectionChanged(
            [this, &control](const auto&, const auto&) {
                UpdateDependentStates();
                const int value = control.theme.SelectedIndex();
                if (value < 0) return;
                const auto member = control.member;
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [member, value](DockSettings& settings) {
                        PrepareDynamicRuleTheme(settings.*member,
                            static_cast<SystemTaskbarThemeMode>(std::clamp(
                                value, 0, 9)));
                    });
            });
        control.contentThemeToken = control.contentTheme.SelectionChanged(
            [this, &control](const auto&, const auto&) {
                const int value = control.contentTheme.SelectedIndex();
                if (value < 0) return;
                const auto member = control.member;
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [member, value](DockSettings& settings) {
                        (settings.*member).contentTheme =
                            std::clamp(value - 1, -1, 1);
                    });
            });
    }

    template <typename Edit>
    void EmitCustomAppearance(SettingsUpdateMode mode, Edit edit)
    {
        EmitDock(mode,
            [edit = std::move(edit)](DockSettings& settings) mutable {
                settings.systemTaskbarBackdropEnabled = true;
                settings.systemTaskbarFollowPersonalization = false;
                settings.systemTaskbarAppearance.backgroundPreset =
                    kAppearancePresetCustom;
                edit(settings.systemTaskbarAppearance);
            });
    }

    static double ReadContinuous(
        const ContinuousControl& control,
        const DockSettings& settings) noexcept
    {
        switch (control.field)
        {
        case ContinuousField::ThicknessScale:
            return ClampDockScale(settings.thicknessScale) * 100.0;
        case ContinuousField::FrequentItemCount:
            return std::clamp(settings.frequentItemCount, 1, 8);
        case ContinuousField::TaskbarBackgroundAlpha:
            return std::clamp(
                settings.systemTaskbarAppearance.widgetAlpha,
                0.0f, 1.0f) * 100.0;
        case ContinuousField::TaskbarBorderAlpha:
            return std::clamp(
                settings.systemTaskbarAppearance.widgetBorderAlpha,
                0.0f, 1.0f) * 100.0;
        case ContinuousField::TaskbarBlurRadius:
            return std::clamp(
                settings.systemTaskbarAppearance.glassBlurRadius,
                4.0f, 48.0f);
        }
        return 0.0;
    }

    static void WriteContinuous(
        DockSettings& settings,
        ContinuousField field,
        double uiValue) noexcept
    {
        switch (field)
        {
        case ContinuousField::ThicknessScale:
            settings.thicknessScale = ClampDockScale(
                static_cast<float>(uiValue / 100.0));
            break;
        case ContinuousField::FrequentItemCount:
            settings.frequentItemCount = std::clamp(
                static_cast<int>(std::lround(uiValue)), 1, 8);
            break;
        case ContinuousField::TaskbarBackgroundAlpha:
            settings.systemTaskbarBackdropEnabled = true;
            settings.systemTaskbarFollowPersonalization = false;
            settings.systemTaskbarAppearance.backgroundPreset =
                kAppearancePresetCustom;
            settings.systemTaskbarAppearance.widgetAlpha =
                static_cast<float>(std::clamp(uiValue, 0.0, 100.0) / 100.0);
            break;
        case ContinuousField::TaskbarBorderAlpha:
            settings.systemTaskbarBackdropEnabled = true;
            settings.systemTaskbarFollowPersonalization = false;
            settings.systemTaskbarAppearance.backgroundPreset =
                kAppearancePresetCustom;
            settings.systemTaskbarAppearance.widgetBorderAlpha =
                static_cast<float>(std::clamp(uiValue, 0.0, 100.0) / 100.0);
            break;
        case ContinuousField::TaskbarBlurRadius:
            settings.systemTaskbarBackdropEnabled = true;
            settings.systemTaskbarFollowPersonalization = false;
            settings.systemTaskbarAppearance.backgroundPreset =
                kAppearancePresetCustom;
            settings.systemTaskbarAppearance.glassBlurRadius =
                static_cast<float>(std::clamp(uiValue, 4.0, 48.0));
            break;
        }
    }

    void Preview(ContinuousControl& control, double value)
    {
        if (!CanEmitDock())
            return;
        control.dirty = true;
        const ContinuousField field = control.field;
        EmitDock(SettingsUpdateMode::Preview,
            [field, value](DockSettings& settings) {
                WriteContinuous(settings, field, value);
            });
    }

    void Commit(ContinuousControl& control)
    {
        if (!control.dirty || !CanEmitDock())
            return;
        control.dirty = false;
        const ContinuousField field = control.field;
        const double value = control.slider.Value();
        EmitDock(SettingsUpdateMode::PreviewAndCommit,
            [field, value](DockSettings& settings) {
                WriteContinuous(settings, field, value);
            });
    }

    static void WriteColor(
        DockSettings& settings,
        ColorField field,
        const winrt::Windows::UI::Color& color) noexcept
    {
        settings.systemTaskbarBackdropEnabled = true;
        settings.systemTaskbarFollowPersonalization = false;
        auto& appearance = settings.systemTaskbarAppearance;
        appearance.backgroundPreset = kAppearancePresetCustom;
        float* red = field == ColorField::TaskbarBackground
            ? &appearance.widgetBgR : &appearance.widgetBorderR;
        float* green = field == ColorField::TaskbarBackground
            ? &appearance.widgetBgG : &appearance.widgetBorderG;
        float* blue = field == ColorField::TaskbarBackground
            ? &appearance.widgetBgB : &appearance.widgetBorderB;
        *red = FromByte(color.R);
        *green = FromByte(color.G);
        *blue = FromByte(color.B);
    }

    void Preview(ColorControl& control)
    {
        if (!CanEmitDock())
            return;
        control.dirty = true;
        const ColorField field = control.field;
        const auto color = control.picker.Color();
        EmitDock(SettingsUpdateMode::Preview,
            [field, color](DockSettings& settings) {
                WriteColor(settings, field, color);
            });
    }

    void Commit(ColorControl& control)
    {
        if (!control.dirty || !CanEmitDock())
            return;
        control.dirty = false;
        const ColorField field = control.field;
        const auto color = control.picker.Color();
        EmitDock(SettingsUpdateMode::PreviewAndCommit,
            [field, color](DockSettings& settings) {
                WriteColor(settings, field, color);
            });
    }

    void ConfirmRestartExplorer()
    {
        if (closed || !active || !hasSnapshot || !actions.confirm ||
            !actions.invokeHost)
        {
            return;
        }
        const std::uint64_t expectedGeneration = generation;
        const std::weak_ptr<ConfirmationGate> weakGate = confirmationGate;
        auto invokeHost = actions.invokeHost;
        const std::wstring restartTitle = L(
            "app.settings.restart_explorer", L"Restart File Explorer");
        const std::wstring restartMessage = restartTitle + L"?\n\n" +
            L("app.settings.system_panel_hint2",
                L"File Explorer will restart. Continue?");
        actions.confirm(expectedGeneration,
            restartTitle,
            restartMessage,
            [weakGate, invokeHost = std::move(invokeHost),
                expectedGeneration](bool confirmed) mutable {
                if (!confirmed)
                    return;
                const auto gate = weakGate.lock();
                if (!gate || !gate->alive.load(std::memory_order_acquire) ||
                    gate->generation.load(std::memory_order_acquire) !=
                        expectedGeneration)
                {
                    return;
                }
                SettingsHostActions::Request request;
                request.action =
                    SettingsHostActions::Action::RestartExplorer;
                invokeHost(expectedGeneration, std::move(request));
            });
    }

    void PatchGeneral(const GeneralSettings& settings)
    {
        dockEnabledToggle.IsOn(settings.dockEnabled);
    }

    void PatchContinuous(
        ContinuousControl& control,
        const DockSettings& settings)
    {
        const double value = std::clamp(ReadContinuous(control, settings),
            control.slider.Minimum(), control.slider.Maximum());
        control.slider.Value(value);
        control.number.Value(value);
    }

    void PatchColor(
        ColorControl& control,
        const DockSettings& settings)
    {
        const auto& appearance = settings.systemTaskbarAppearance;
        if (control.field == ColorField::TaskbarBackground)
        {
            control.picker.Color(ToColor(
                appearance.widgetBgR,
                appearance.widgetBgG,
                appearance.widgetBgB));
        }
        else
        {
            control.picker.Color(ToColor(
                appearance.widgetBorderR,
                appearance.widgetBorderG,
                appearance.widgetBorderB));
        }
    }

    void PatchDynamicRule(
        DynamicRuleControl& control,
        const DockSettings& settings)
    {
        const auto& rule = settings.*control.member;
        control.enabled.IsOn(rule.enabled);
        control.theme.SelectedIndex(std::clamp(
            static_cast<int>(rule.themeMode), 0, 9));
        control.contentTheme.SelectedIndex(
            std::clamp(rule.contentTheme, -1, 1) + 1);
    }

    void PatchDock(const DockSettings& settings)
    {
        positionCombo.SelectedIndex(std::clamp(
            static_cast<int>(settings.position), 0, 3));
        layoutCombo.SelectedIndex(settings.edgeAttached ? 1 : 0);
        monitorScopeCombo.SelectedIndex(std::clamp(
            static_cast<int>(settings.monitorScope), 0, 2));
        floatingShortcutToggle.IsOn(settings.floatingShortcutMode);
        floatingEdgeSwipeToggle.IsOn(settings.floatingEdgeSwipeEnabled);
        showWindowsButtonToggle.IsOn(settings.showWindowsButton);
        showFrequentItemsToggle.IsOn(settings.showFrequentItems);
        keepWhenDesktopHiddenToggle.IsOn(settings.keepWhenDesktopHidden);
        taskbarAutoHideToggle.IsOn(settings.systemTaskbarAutoHide);
        taskbarAlignmentCombo.SelectedIndex(std::clamp(
            settings.systemTaskbarAlignment, 0, 1));
        taskbarThemeCombo.SelectedIndex(MainTaskbarThemeMode(settings));
        taskbarContentThemeCombo.SelectedIndex(std::clamp(
            settings.systemTaskbarContentTheme, -1, 1) + 1);
        taskbarGlassToggle.IsOn(
            settings.systemTaskbarAppearance.glassEnabled);
        taskbarAcrylicToggle.IsOn(
            settings.systemTaskbarAppearance.acrylicEnabled);

        for (ContinuousControl* control : continuousControls)
            PatchContinuous(*control, settings);
        for (ColorControl* control : colorControls)
            PatchColor(*control, settings);
        for (DynamicRuleControl* control : dynamicRules)
            PatchDynamicRule(*control, settings);
        UpdateDependentStates();
    }

    void UpdateDependentStates()
    {
        if (closed)
            return;
        const bool dockEnabled = dockEnabledToggle.IsOn();
        layoutCard.root.IsHitTestVisible(dockEnabled);
        behaviorCard.root.IsHitTestVisible(dockEnabled);
        layoutCard.root.Opacity(dockEnabled ? 1.0 : 0.62);
        behaviorCard.root.Opacity(dockEnabled ? 1.0 : 0.62);
        frequentItemCount.slider.IsEnabled(
            dockEnabled && showFrequentItemsToggle.IsOn());
        frequentItemCount.number.IsEnabled(
            dockEnabled && showFrequentItemsToggle.IsOn());

        const int taskbarMode = taskbarThemeCombo.SelectedIndex();
        const bool taskbarStyled = taskbarMode !=
            static_cast<int>(SystemTaskbarThemeMode::Native);
        const bool taskbarCustom = taskbarMode ==
            static_cast<int>(SystemTaskbarThemeMode::Custom);
        taskbarContentThemeCombo.IsEnabled(taskbarStyled);
        taskbarCustomCard.root.Opacity(taskbarCustom ? 1.0 : 0.62);
        taskbarCustomCard.root.IsHitTestVisible(taskbarCustom);
        taskbarBlurRadius.slider.IsEnabled(
            taskbarCustom && taskbarGlassToggle.IsOn());
        taskbarBlurRadius.number.IsEnabled(
            taskbarCustom && taskbarGlassToggle.IsOn());
        taskbarAcrylicToggle.IsEnabled(
            taskbarCustom && taskbarGlassToggle.IsOn());

        for (DynamicRuleControl* control : dynamicRules)
        {
            const bool enabled = control->enabled.IsOn();
            const bool native = control->theme.SelectedIndex() ==
                static_cast<int>(SystemTaskbarThemeMode::Native);
            control->theme.IsEnabled(enabled);
            control->contentTheme.IsEnabled(enabled && !native);
            control->card.root.Opacity(enabled ? 1.0 : 0.78);
        }
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
        muxa::AutomationProperties::SetName(control.slider,
            control.label.Text());
        muxa::AutomationProperties::SetName(control.number,
            control.label.Text());
    }

    void SetColorText(
        ColorControl& control,
        std::string_view key,
        std::wstring_view fallback)
    {
        control.label.Text(L(key, fallback));
        muxa::AutomationProperties::SetName(control.picker,
            control.label.Text());
    }

    void SetHeader(
        const muxc::ToggleSwitch& toggle,
        std::string_view key,
        std::wstring_view fallback)
    {
        const std::wstring text = L(key, fallback);
        toggle.Header(winrt::box_value(text));
        muxa::AutomationProperties::SetName(toggle, text);
    }

    void SetHeader(
        const muxc::ComboBox& combo,
        std::string_view key,
        std::wstring_view fallback)
    {
        const std::wstring text = L(key, fallback);
        combo.Header(winrt::box_value(text));
        muxa::AutomationProperties::SetName(combo, text);
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
        if (items.size())
        {
            combo.SelectedIndex(std::clamp(
                selected, 0, static_cast<int>(items.size()) - 1));
        }
    }

    void ReplaceTaskbarThemeItems(const muxc::ComboBox& combo)
    {
        ReplaceComboItems(combo, {
            {"app.settings.taskbar_windows_native", L"Windows Native"},
            {"app.settings.taskbar_follow_global", L"Follow Global Theme"},
            {"app.settings.dark", L"Dark"},
            {"app.settings.light", L"Light"},
            {"app.settings.dark_glass", L"Dark Glass"},
            {"app.settings.light_glass", L"Light Glass"},
            {"app.settings.dark_acrylic", L"Dark Acrylic"},
            {"app.settings.light_acrylic", L"Light Acrylic"},
            {"app.settings.custom", L"Custom"},
            {"app.settings.taskbar_transparent", L"Transparent"},
        });
    }

    void ReplaceContentThemeItems(const muxc::ComboBox& combo)
    {
        ReplaceComboItems(combo, {
            {"app.settings.taskbar_foreground_auto", L"Automatic"},
            {"app.settings.light", L"Light"},
            {"app.settings.dark", L"Dark"},
        });
    }

    void RefreshLocalizedText()
    {
        if (closed)
            return;
        const bool previousUpdating = updatingControls;
        updatingControls = true;

        SetCardText(enableCard,
            "app.settings.dock_bar", L"Dock and Taskbar");
        SetCardText(layoutCard,
            "app.dock.layout", L"Dock Layout");
        SetCardText(behaviorCard,
            "app.dock.detailed", L"Dock Behavior");
        SetCardText(taskbarCard,
            "app.settings.system_panel", L"Windows Taskbar");
        SetCardText(taskbarAppearanceCard,
            "app.settings.taskbar_theme", L"Taskbar Theme");
        SetCardText(taskbarCustomCard,
            "app.settings.custom", L"Custom Taskbar Appearance");

        SetHeader(dockEnabledToggle,
            "app.dock.enable", L"Enable Dock");
        SetHeader(positionCombo,
            "app.settings.dock_position", L"Position");
        ReplaceComboItems(positionCombo, {
            {"app.dock.bottom", L"Bottom"},
            {"app.dock.top", L"Top"},
            {"app.dock.left", L"Left"},
            {"app.dock.right", L"Right"},
        });
        SetHeader(layoutCombo,
            "app.dock.layout", L"Dock Style");
        ReplaceComboItems(layoutCombo, {
            {"app.dock.island", L"Island"},
            {"app.dock.edge", L"Edge"},
        });
        SetHeader(monitorScopeCombo,
            "app.settings.display_scope", L"Display Scope");
        ReplaceComboItems(monitorScopeCombo, {
            {"app.dock.first_screen", L"Primary Display"},
            {"app.dock.last_screen", L"Last Active Display"},
            {"app.dock.all_screens", L"All Displays"},
        });
        SetContinuousText(thicknessScale,
            "app.settings.dock_thickness", L"Dock Thickness (%)");

        SetHeader(floatingShortcutToggle,
            "app.dock.floating_shortcut_mode", L"Floating Dock Shortcut");
        floatingShortcutHint.Text(L("app.dock.floating_shortcut_hint",
            L"Show the floating Dock with its configured shortcut."));
        SetHeader(floatingEdgeSwipeToggle,
            "app.dock.floating_edge_swipe", L"Edge Swipe");
        floatingEdgeSwipeHint.Text(L("app.dock.floating_edge_swipe_hint",
            L"Reveal the floating Dock from a screen edge."));
        SetHeader(showWindowsButtonToggle,
            "app.dock.show_windows_button", L"Show Windows Button");
        SetHeader(showFrequentItemsToggle,
            "app.dock.show_frequent_items", L"Show Frequent Items");
        SetContinuousText(frequentItemCount,
            "app.settings.show_count", L"Frequent Item Count");
        SetHeader(keepWhenDesktopHiddenToggle,
            "app.dock.keep_when_hidden", L"Keep When Desktop Hidden");

        SetHeader(taskbarAutoHideToggle,
            "app.settings.auto_hide_taskbar", L"Auto-hide System Taskbar");
        SetHeader(taskbarAlignmentCombo,
            "app.settings.taskbar_alignment", L"Taskbar Alignment");
        ReplaceComboItems(taskbarAlignmentCombo, {
            {"app.settings.taskbar_left", L"Left"},
            {"app.settings.taskbar_center", L"Center"},
        });
        restartExplorerHint.Text(L("app.settings.system_panel_hint2",
            L"Restart File Explorer after changing Windows shell settings."));
        restartExplorerButton.Content(winrt::box_value(
            L("app.settings.restart_explorer", L"Restart File Explorer")));
        muxa::AutomationProperties::SetName(restartExplorerButton,
            L("app.settings.restart_explorer", L"Restart File Explorer"));

        SetHeader(taskbarThemeCombo,
            "app.settings.taskbar_theme", L"Taskbar Theme");
        ReplaceTaskbarThemeItems(taskbarThemeCombo);
        SetHeader(taskbarContentThemeCombo,
            "app.settings.taskbar_foreground_color",
            L"Taskbar Icon and Text Color");
        ReplaceContentThemeItems(taskbarContentThemeCombo);

        SetColorText(taskbarBackgroundColor,
            "app.settings.bg_color", L"Background Color");
        SetColorText(taskbarBorderColor,
            "app.settings.border_color", L"Border Color");
        SetContinuousText(taskbarBackgroundAlpha,
            "app.settings.bg_opacity", L"Background Opacity");
        SetContinuousText(taskbarBorderAlpha,
            "app.settings.border_opacity", L"Border Opacity");
        SetContinuousText(taskbarBlurRadius,
            "app.settings.blur_radius", L"Blur Radius");
        SetHeader(taskbarGlassToggle,
            "app.settings.glass_enabled", L"Frosted Glass Background");
        SetHeader(taskbarAcrylicToggle,
            "app.settings.acrylic_noise", L"Acrylic Noise");

        RefreshDynamicRuleText(visibleWindowRule,
            "app.settings.taskbar_dynamic_visible_window",
            L"When a window is visible");
        RefreshDynamicRuleText(maximizedWindowRule,
            "app.settings.taskbar_dynamic_maximized_window",
            L"When a window is maximized");
        RefreshDynamicRuleText(shellUiRule,
            "app.settings.taskbar_dynamic_shell_ui",
            L"When a taskbar panel is open");

        updatingControls = previousUpdating;
        UpdateDependentStates();
    }

    void RefreshDynamicRuleText(
        DynamicRuleControl& control,
        std::string_view titleKey,
        std::wstring_view fallback)
    {
        SetCardText(control.card, titleKey, fallback);
        SetHeader(control.enabled, titleKey, fallback);
        SetHeader(control.theme,
            "app.settings.taskbar_dynamic_theme", L"Appearance when active");
        ReplaceTaskbarThemeItems(control.theme);
        SetHeader(control.contentTheme,
            "app.settings.taskbar_foreground_color",
            L"Taskbar Icon and Text Color");
        ReplaceContentThemeItems(control.contentTheme);
    }

    void ApplySnapshot(const SettingsSnapshot& snapshot)
    {
        if (closed)
            return;
        const bool newGeneration =
            !hasSnapshot || snapshot.generation != generation;
        generation = snapshot.generation;
        confirmationGate->generation.store(generation,
            std::memory_order_release);
        const bool previousUpdating = updatingControls;
        updatingControls = true;

        if (newGeneration ||
            snapshot.domainRevisions.general != generalRevision)
        {
            PatchGeneral(snapshot.values.general);
            generalRevision = snapshot.domainRevisions.general;
        }
        if (newGeneration ||
            snapshot.domainRevisions.dock != dockRevision)
        {
            PatchDock(snapshot.values.dock);
            dockRevision = snapshot.domainRevisions.dock;
        }
        if (newGeneration)
        {
            for (ContinuousControl* control : continuousControls)
                control->dirty = false;
            for (ColorControl* control : colorControls)
                control->dirty = false;
        }
        hasSnapshot = true;
        UpdateDependentStates();
        updatingControls = previousUpdating;
    }

    mux::FrameworkElement FocusTarget(std::string_view id) const noexcept
    {
        if (id == "dock.enable") return dockEnabledToggle;
        if (id == "dock.position") return positionCombo;
        if (id == "dock.layout" || id == "dock.edgeAttached")
            return layoutCombo;
        if (id == "dock.monitor" || id == "dock.monitorScope")
            return monitorScopeCombo;
        if (id == "dock.thickness" || id == "dock.thicknessScale")
            return thicknessScale.slider;
        if (id == "dock.floatingShortcutMode")
            return floatingShortcutToggle;
        if (id == "dock.floatingEdgeSwipe" ||
            id == "dock.floatingEdgeSwipeEnabled")
            return floatingEdgeSwipeToggle;
        if (id == "dock.showWindowsButton")
            return showWindowsButtonToggle;
        if (id == "dock.showFrequentItems")
            return showFrequentItemsToggle;
        if (id == "dock.frequentItemCount")
            return frequentItemCount.slider;
        if (id == "dock.keepWhenDesktopHidden")
            return keepWhenDesktopHiddenToggle;
        if (id == "taskbar.autoHide") return taskbarAutoHideToggle;
        if (id == "taskbar.alignment") return taskbarAlignmentCombo;
        if (id == "taskbar.theme" || id == "taskbar.backdrop" ||
            id == "taskbar.followGlobal")
            return taskbarThemeCombo;
        if (id == "taskbar.contentTheme")
            return taskbarContentThemeCombo;
        if (id == "taskbar.backgroundColor")
            return taskbarBackgroundColor.picker;
        if (id == "taskbar.borderColor")
            return taskbarBorderColor.picker;
        if (id == "taskbar.backgroundOpacity")
            return taskbarBackgroundAlpha.slider;
        if (id == "taskbar.borderOpacity")
            return taskbarBorderAlpha.slider;
        if (id == "taskbar.glass") return taskbarGlassToggle;
        if (id == "taskbar.blurRadius") return taskbarBlurRadius.slider;
        if (id == "taskbar.acrylic") return taskbarAcrylicToggle;
        if (id == "taskbar.restartExplorer") return restartExplorerButton;
        if (id == "taskbar.dynamic.visibleWindow")
            return visibleWindowRule.enabled;
        if (id == "taskbar.dynamic.maximizedWindow")
            return maximizedWindowRule.enabled;
        if (id == "taskbar.dynamic.shellUi")
            return shellUiRule.enabled;
        return nullptr;
    }

    void CommitContinuousEdits() noexcept
    {
        try
        {
            for (ContinuousControl* control : continuousControls)
                Commit(*control);
            for (ColorControl* control : colorControls)
                Commit(*control);
        }
        catch (...)
        {
        }
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

    void UnhookDynamicRule(DynamicRuleControl& control) noexcept
    {
        try
        {
            control.enabled.Toggled(control.enabledToken);
            control.theme.SelectionChanged(control.themeToken);
            control.contentTheme.SelectionChanged(control.contentThemeToken);
        }
        catch (...)
        {
        }
    }

    void Close() noexcept
    {
        if (closed)
            return;
        CommitContinuousEdits();
        active = false;
        closed = true;
        confirmationGate->alive.store(false, std::memory_order_release);
        try
        {
            dockEnabledToggle.Toggled(dockEnabledToken);
            positionCombo.SelectionChanged(positionToken);
            layoutCombo.SelectionChanged(layoutToken);
            monitorScopeCombo.SelectionChanged(monitorScopeToken);
            floatingShortcutToggle.Toggled(floatingShortcutToken);
            floatingEdgeSwipeToggle.Toggled(floatingEdgeSwipeToken);
            showWindowsButtonToggle.Toggled(showWindowsButtonToken);
            showFrequentItemsToggle.Toggled(showFrequentItemsToken);
            keepWhenDesktopHiddenToggle.Toggled(keepWhenDesktopHiddenToken);
            taskbarAutoHideToggle.Toggled(taskbarAutoHideToken);
            taskbarAlignmentCombo.SelectionChanged(taskbarAlignmentToken);
            restartExplorerButton.Click(restartExplorerToken);
            taskbarThemeCombo.SelectionChanged(taskbarThemeToken);
            taskbarContentThemeCombo.SelectionChanged(
                taskbarContentThemeToken);
            taskbarGlassToggle.Toggled(taskbarGlassToken);
            taskbarAcrylicToggle.Toggled(taskbarAcrylicToken);
        }
        catch (...)
        {
        }
        for (ContinuousControl* control : continuousControls)
            UnhookContinuousControl(*control);
        for (ColorControl* control : colorControls)
            UnhookColorControl(*control);
        for (DynamicRuleControl* control : dynamicRules)
            UnhookDynamicRule(*control);
        actions = {};
        localize = {};
    }
};

DockPagePresenter::DockPagePresenter(
    LocalizeCallback localize,
    const mux::Style& cardStyle)
    : impl_(std::make_unique<Impl>(std::move(localize), cardStyle))
{
}

DockPagePresenter::~DockPagePresenter()
{
    Close();
}

void DockPagePresenter::SetActions(DockPageActions actions)
{
    if (impl_ && !impl_->closed)
        impl_->actions = std::move(actions);
}

mux::UIElement DockPagePresenter::Content() const noexcept
{
    return impl_ ? impl_->root : nullptr;
}

void DockPagePresenter::ApplySnapshot(const SettingsSnapshot& snapshot)
{
    if (impl_)
        impl_->ApplySnapshot(snapshot);
}

void DockPagePresenter::RefreshLocalizedText()
{
    if (impl_)
        impl_->RefreshLocalizedText();
}

void DockPagePresenter::Activate() noexcept
{
    if (impl_ && !impl_->closed)
        impl_->active = true;
}

void DockPagePresenter::Deactivate() noexcept
{
    if (!impl_ || impl_->closed)
        return;
    impl_->CommitContinuousEdits();
    impl_->active = false;
}

mux::FrameworkElement DockPagePresenter::FocusTarget(
    std::string_view focusId) const noexcept
{
    return impl_ ? impl_->FocusTarget(focusId) : nullptr;
}

void DockPagePresenter::Close() noexcept
{
    if (impl_)
        impl_->Close();
}

} // namespace snowdesktop::winui

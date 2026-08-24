#include "pch.h"

#include "dock_page_presenter.h"
#include "settings_presenter_controls.h"

#include "../dock_settings.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>

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

std::wstring ExtractNumericUnit(std::wstring text)
{
    if (const std::size_t marker = text.find(L"%d");
        marker != std::wstring::npos)
    {
        text.erase(marker, 2);
    }
    const std::size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos)
        return {};
    const std::size_t last = text.find_last_not_of(L" \t\r\n");
    return text.substr(first, last - first + 1);
}

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
    SettingRow row;
    muxc::Grid root{nullptr};
    muxc::TextBlock label{nullptr};
    muxc::StackPanel editors{nullptr};
    muxc::Slider slider{nullptr};
    muxc::NumberBox number{nullptr};
    muxc::TextBlock unit{nullptr};
    muxc::Button reset{nullptr};
    ContinuousField field = ContinuousField::ThicknessScale;
    SystemTaskbarDynamicRule DockSettings::* ruleMember = nullptr;
    double defaultValue = std::numeric_limits<double>::quiet_NaN();
    bool dirty = false;
    mux::DispatcherTimer idleCommitTimer{nullptr};

    winrt::event_token sliderChanged{};
    winrt::event_token numberChanged{};
    winrt::event_token sliderReleased{};
    winrt::event_token numberReleased{};
    winrt::event_token sliderLostFocus{};
    winrt::event_token numberLostFocus{};
    winrt::event_token sliderKeyDown{};
    winrt::event_token numberKeyDown{};
    winrt::event_token resetToken{};
    winrt::event_token idleCommitToken{};
};

enum class ColorField
{
    TaskbarBackground,
    TaskbarBorder,
};

struct ColorControl
{
    ColorFlyoutEditor editor;
    muxc::Grid root{nullptr};
    ColorField field = ColorField::TaskbarBackground;
    SystemTaskbarDynamicRule DockSettings::* ruleMember = nullptr;
};

struct DynamicRuleControl
{
    SettingsCard card;
    muxc::ToggleSwitch enabled{nullptr};
    muxc::ComboBox theme{nullptr};
    muxc::ComboBox contentTheme{nullptr};
    muxc::StackPanel details{nullptr};
    muxc::StackPanel customAppearance{nullptr};
    SettingRow enabledRow;
    SettingRow themeRow;
    SettingRow contentThemeRow;
    ColorControl backgroundColor;
    ColorControl borderColor;
    ContinuousControl backgroundAlpha;
    ContinuousControl borderAlpha;
    muxc::ToggleSwitch glass{nullptr};
    SettingRow glassRow;
    ContinuousControl blurRadius;
    muxc::ToggleSwitch acrylic{nullptr};
    SettingRow acrylicRow;
    SystemTaskbarDynamicRule DockSettings::* member = nullptr;

    winrt::event_token enabledToken{};
    winrt::event_token themeToken{};
    winrt::event_token contentThemeToken{};
    winrt::event_token glassToken{};
    winrt::event_token acrylicToken{};
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
    muxc::StackPanel dockEnableRoot{nullptr};
    muxc::StackPanel dockRoot{nullptr};
    muxc::StackPanel taskbarRoot{nullptr};

    SettingsCard enableCard;
    SettingsCard edgeSwipeCard;
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
    muxc::ComboBox windowsSystemThemeCombo{nullptr};
    muxc::TextBlock taskbarRuntimeStatus{nullptr};

    SettingRow dockEnabledRow;
    SettingRow positionRow;
    SettingRow layoutRow;
    SettingRow monitorScopeRow;
    SettingRow floatingEdgeSwipeRow;
    SettingRow showWindowsButtonRow;
    SettingRow showFrequentItemsRow;
    SettingRow taskbarAutoHideRow;
    SettingRow taskbarAlignmentRow;
    SettingRow windowsSystemThemeRow;
    SettingRow taskbarThemeRow;
    SettingRow taskbarContentThemeRow;
    SettingRow taskbarGlassRow;
    SettingRow taskbarAcrylicRow;

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
    bool taskbarContentThemeCustomItems = false;
    bool taskbarHookRequired = false;
    int taskbarContentThemeValue = -1;
    int taskbarAppearanceContentThemeValue = 0;
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
    winrt::event_token windowsSystemThemeToken{};

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
        dockEnableRoot = muxc::StackPanel{};
        dockEnableRoot.Spacing(8.0);
        dockRoot = muxc::StackPanel{};
        dockRoot.Spacing(8.0);
        taskbarRoot = muxc::StackPanel{};
        taskbarRoot.Spacing(8.0);
        root = dockRoot;

        InitializeCard(enableCard, cardStyle, dockEnableRoot);
        dockEnabledToggle = muxc::ToggleSwitch{};
        dockEnabledToggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
        dockEnabledRow.Initialize(dockEnabledToggle);
        dockEnabledRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        enableCard.content.Children().Append(dockEnabledRow.root);

        InitializeCard(edgeSwipeCard, cardStyle, dockRoot);
        InitializeCard(layoutCard, cardStyle, dockRoot);
        positionCombo = NewCombo();
        layoutCombo = NewCombo();
        monitorScopeCombo = NewCombo();
        positionRow.Initialize(positionCombo);
        layoutRow.Initialize(layoutCombo);
        monitorScopeRow.Initialize(monitorScopeCombo);
        InitializeContinuousControl(thicknessScale,
            ContinuousField::ThicknessScale, 50.0, 100.0, 1.0,
            nullptr, 100.0);
        layoutCard.content.Children().Append(positionRow.root);
        layoutCard.content.Children().Append(monitorScopeRow.root);
        layoutCard.content.Children().Append(layoutRow.root);
        layoutCard.content.Children().Append(thicknessScale.root);

        InitializeCard(behaviorCard, cardStyle, dockRoot);
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
            toggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
        }
        floatingShortcutHint = NewHint();
        floatingEdgeSwipeHint = NewHint();
        floatingEdgeSwipeRow.Initialize(floatingEdgeSwipeToggle);
        showWindowsButtonRow.Initialize(showWindowsButtonToggle);
        showFrequentItemsRow.Initialize(showFrequentItemsToggle);
        floatingEdgeSwipeRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        showWindowsButtonRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        showFrequentItemsRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        InitializeContinuousControl(frequentItemCount,
            ContinuousField::FrequentItemCount, 1.0, 8.0, 1.0);
        // Floating shortcut mode/hotkey is rendered once by General.
        edgeSwipeCard.content.Children().Append(floatingEdgeSwipeRow.root);
        behaviorCard.content.Children().Append(showWindowsButtonRow.root);
        behaviorCard.content.Children().Append(showFrequentItemsRow.root);
        behaviorCard.content.Children().Append(frequentItemCount.root);
        // keepWhenDesktopHidden is a post-migration field and intentionally
        // remains outside the 1:1 legacy surface.

        InitializeCard(taskbarCard, cardStyle, taskbarRoot);
        taskbarAutoHideToggle = muxc::ToggleSwitch{};
        taskbarAutoHideToggle.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        taskbarAlignmentCombo = NewCombo();
        windowsSystemThemeCombo = NewCombo();
        taskbarAutoHideRow.Initialize(taskbarAutoHideToggle);
        taskbarAutoHideRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        taskbarAlignmentRow.Initialize(taskbarAlignmentCombo);
        restartExplorerHint = NewHint();
        restartExplorerButton = muxc::Button{};
        restartExplorerButton.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        restartExplorerButton.VerticalAlignment(
            mux::VerticalAlignment::Center);
        muxc::StackPanel systemThemeActions{};
        systemThemeActions.Orientation(muxc::Orientation::Horizontal);
        systemThemeActions.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        systemThemeActions.Spacing(8.0);
        systemThemeActions.Children().Append(windowsSystemThemeCombo);
        systemThemeActions.Children().Append(restartExplorerButton);
        windowsSystemThemeRow.Initialize(systemThemeActions);
        windowsSystemThemeRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        taskbarCard.content.Children().Append(taskbarAutoHideRow.root);
        taskbarCard.content.Children().Append(taskbarAlignmentRow.root);
        taskbarCard.content.Children().Append(windowsSystemThemeRow.root);

        InitializeCard(taskbarAppearanceCard, cardStyle, taskbarRoot);
        taskbarThemeCombo = NewCombo();
        taskbarContentThemeCombo = NewCombo();
        taskbarThemeRow.Initialize(taskbarThemeCombo);
        taskbarContentThemeRow.Initialize(taskbarContentThemeCombo);
        taskbarRuntimeStatus = muxc::TextBlock{};
        taskbarRuntimeStatus.TextWrapping(mux::TextWrapping::Wrap);
        taskbarRuntimeStatus.Opacity(0.78);
        taskbarAppearanceCard.content.Children().Append(taskbarThemeRow.root);
        taskbarAppearanceCard.content.Children().Append(
            taskbarContentThemeRow.root);
        taskbarAppearanceCard.content.Children().Append(taskbarRuntimeStatus);

        InitializeCard(taskbarCustomCard, cardStyle, taskbarRoot);
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
        taskbarGlassToggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
        taskbarAcrylicToggle.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        taskbarGlassRow.Initialize(taskbarGlassToggle);
        taskbarAcrylicRow.Initialize(taskbarAcrylicToggle);
        taskbarGlassRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        taskbarAcrylicRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        taskbarCustomCard.content.Children().Append(taskbarBackgroundColor.root);
        taskbarCustomCard.content.Children().Append(taskbarBorderColor.root);
        taskbarCustomCard.content.Children().Append(taskbarBackgroundAlpha.root);
        taskbarCustomCard.content.Children().Append(taskbarBorderAlpha.root);
        taskbarCustomCard.content.Children().Append(taskbarGlassRow.root);
        taskbarCustomCard.content.Children().Append(taskbarBlurRadius.root);
        taskbarCustomCard.content.Children().Append(taskbarAcrylicRow.root);

        InitializeDynamicRule(visibleWindowRule,
            &DockSettings::systemTaskbarVisibleWindow);
        InitializeDynamicRule(maximizedWindowRule,
            &DockSettings::systemTaskbarMaximizedWindow);
        InitializeDynamicRule(shellUiRule,
            &DockSettings::systemTaskbarShellUi);

        // Match the legacy settings order: SnowDesktop appearance first,
        // followed by the three dynamic overrides, with Windows' own
        // taskbar controls at the end of the section.
        taskbarRoot.Children().RemoveAt(0);
        taskbarRoot.Children().Append(taskbarCard.root);
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
        double step,
        SystemTaskbarDynamicRule DockSettings::* ruleMember = nullptr,
        double defaultValue = std::numeric_limits<double>::quiet_NaN())
    {
        control.editors = muxc::StackPanel{};
        control.editors.Orientation(muxc::Orientation::Horizontal);
        control.editors.Spacing(12.0);
        control.slider = muxc::Slider{};
        control.slider.Minimum(minimum);
        control.slider.Maximum(maximum);
        control.slider.StepFrequency(step);
        control.slider.VerticalAlignment(mux::VerticalAlignment::Center);
        control.slider.Width(
            std::isfinite(defaultValue) ? 220.0
            : field == ContinuousField::FrequentItemCount ? 280.0
            : 320.0);
        control.number = muxc::NumberBox{};
        control.number.Minimum(minimum);
        control.number.Maximum(maximum);
        control.number.SmallChange(step);
        control.number.LargeChange(step * 5.0);
        control.number.SpinButtonPlacementMode(
            muxc::NumberBoxSpinButtonPlacementMode::Compact);
        control.number.Width(128.0);
        control.unit = muxc::TextBlock{};
        control.unit.VerticalAlignment(mux::VerticalAlignment::Center);
        control.unit.Opacity(0.72);
        control.unit.Visibility(mux::Visibility::Collapsed);
        control.field = field;
        control.ruleMember = ruleMember;
        control.defaultValue = defaultValue;
        control.editors.Children().Append(control.slider);
        control.editors.Children().Append(control.number);
        control.editors.Children().Append(control.unit);
        if (std::isfinite(defaultValue))
        {
            control.reset = muxc::Button{};
            control.reset.VerticalAlignment(mux::VerticalAlignment::Center);
            control.editors.Children().Append(control.reset);
        }
        control.row.Initialize(control.editors);
        control.root = control.row.root;
        control.label = control.row.label;
        switch (field)
        {
        case ContinuousField::ThicknessScale:
        case ContinuousField::TaskbarBackgroundAlpha:
        case ContinuousField::TaskbarBorderAlpha:
            SetUnit(control, L"%");
            break;
        case ContinuousField::TaskbarBlurRadius:
            SetUnit(control, L"px");
            break;
        case ContinuousField::FrequentItemCount:
            break;
        }
    }

    static void SetUnit(ContinuousControl& control, std::wstring text)
    {
        control.unit.Text(std::move(text));
        control.unit.Visibility(control.unit.Text().empty()
                ? mux::Visibility::Collapsed
                : mux::Visibility::Visible);
    }

    void InitializeColorControl(
        ColorControl& control,
        ColorField field,
        SystemTaskbarDynamicRule DockSettings::* ruleMember = nullptr)
    {
        control.field = field;
        control.ruleMember = ruleMember;
        control.editor.Initialize(
            [this, &control](const winrt::Windows::UI::Color& color,
                SettingsUpdateMode mode) {
                const auto field = control.field;
                const auto member = control.ruleMember;
                EmitDock(mode,
                    [field, member, color](DockSettings& settings) {
                        WriteColor(settings, field, member, color);
                    });
            });
        control.root = control.editor.row.root;
    }

    void InitializeDynamicRule(
        DynamicRuleControl& control,
        SystemTaskbarDynamicRule DockSettings::* member)
    {
        InitializeCard(control.card, cardStyle, taskbarRoot);
        control.enabled = muxc::ToggleSwitch{};
        control.enabled.HorizontalAlignment(mux::HorizontalAlignment::Right);
        control.theme = NewCombo();
        control.contentTheme = NewCombo();
        control.member = member;
        control.enabledRow.Initialize(control.enabled);
        control.enabledRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        control.details = muxc::StackPanel{};
        control.details.Spacing(12.0);
        control.themeRow.Initialize(control.theme);
        control.contentThemeRow.Initialize(control.contentTheme);
        control.details.Children().Append(control.themeRow.root);
        control.details.Children().Append(control.contentThemeRow.root);

        control.customAppearance = muxc::StackPanel{};
        control.customAppearance.Spacing(12.0);
        InitializeColorControl(control.backgroundColor,
            ColorField::TaskbarBackground, member);
        InitializeColorControl(control.borderColor,
            ColorField::TaskbarBorder, member);
        InitializeContinuousControl(control.backgroundAlpha,
            ContinuousField::TaskbarBackgroundAlpha,
            0.0, 100.0, 1.0, member);
        InitializeContinuousControl(control.borderAlpha,
            ContinuousField::TaskbarBorderAlpha,
            0.0, 100.0, 1.0, member);
        control.glass = muxc::ToggleSwitch{};
        control.glass.HorizontalAlignment(mux::HorizontalAlignment::Right);
        control.glassRow.Initialize(control.glass);
        control.glassRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        InitializeContinuousControl(control.blurRadius,
            ContinuousField::TaskbarBlurRadius,
            4.0, 48.0, 1.0, member);
        control.acrylic = muxc::ToggleSwitch{};
        control.acrylic.HorizontalAlignment(mux::HorizontalAlignment::Right);
        control.acrylicRow.Initialize(control.acrylic);
        control.acrylicRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        control.customAppearance.Children().Append(
            control.backgroundColor.root);
        control.customAppearance.Children().Append(control.borderColor.root);
        control.customAppearance.Children().Append(control.backgroundAlpha.root);
        control.customAppearance.Children().Append(control.borderAlpha.root);
        control.customAppearance.Children().Append(control.glassRow.root);
        control.customAppearance.Children().Append(control.blurRadius.root);
        control.customAppearance.Children().Append(control.acrylicRow.root);
        control.details.Children().Append(control.customAppearance);
        control.card.content.Children().Append(control.enabledRow.root);
        control.card.content.Children().Append(control.details);
    }

    std::vector<ContinuousControl*> AllContinuousControls()
    {
        std::vector<ContinuousControl*> result(
            continuousControls.begin(), continuousControls.end());
        for (DynamicRuleControl* rule : dynamicRules)
        {
            result.push_back(&rule->backgroundAlpha);
            result.push_back(&rule->borderAlpha);
            result.push_back(&rule->blurRadius);
        }
        return result;
    }

    std::vector<ColorControl*> AllColorControls()
    {
        std::vector<ColorControl*> result(
            colorControls.begin(), colorControls.end());
        for (DynamicRuleControl* rule : dynamicRules)
        {
            result.push_back(&rule->backgroundColor);
            result.push_back(&rule->borderColor);
        }
        return result;
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
                UpdateEdgeSwipeHintVisibility();
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
        windowsSystemThemeToken = windowsSystemThemeCombo.SelectionChanged(
            [this](const auto&, const auto&) {
                if (closed || updatingControls || !active || !hasSnapshot)
                    return;
                const int value = windowsSystemThemeCombo.SelectedIndex();
                if (value >= 0)
                    (void)RequestWindowsSystemLightThemeEnabled(value == 0);
            });
        taskbarThemeToken = taskbarThemeCombo.SelectionChanged(
            [this](const auto&, const auto&) {
                const int value = taskbarThemeCombo.SelectedIndex();
                if (value < 0) return;
                const bool custom = value ==
                    static_cast<int>(SystemTaskbarThemeMode::Custom);
                if (!updatingControls &&
                    custom != taskbarContentThemeCustomItems)
                {
                    const int logicalValue = custom &&
                            taskbarContentThemeValue < 0
                        ? taskbarAppearanceContentThemeValue
                        : taskbarContentThemeValue;
                    ReplaceMainContentThemeItems(custom, logicalValue);
                }
                UpdateDependentStates();
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [value](DockSettings& settings) {
                        ApplyMainTaskbarTheme(settings, value);
                    });
            });
        taskbarContentThemeToken = taskbarContentThemeCombo.SelectionChanged(
            [this](const auto&, const auto&) {
                const int value = taskbarContentThemeCombo.SelectedIndex();
                if (value < 0) return;
                const bool custom = taskbarThemeCombo.SelectedIndex() ==
                    static_cast<int>(SystemTaskbarThemeMode::Custom);
                const int logicalValue = custom
                    ? std::clamp(value, 0, 1)
                    : std::clamp(value - 1, -1, 1);
                taskbarContentThemeValue = logicalValue;
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [logicalValue](DockSettings& settings) {
                        settings.systemTaskbarContentTheme =
                            logicalValue;
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

        for (ContinuousControl* control : AllContinuousControls())
            HookContinuousControl(*control);
        for (DynamicRuleControl* control : dynamicRules)
            HookDynamicRule(*control);
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
        if (control.reset)
        {
            control.resetToken = control.reset.Click(
                [this, &control](const auto&, const auto&) {
                    if (!std::isfinite(control.defaultValue)) return;
                    control.idleCommitTimer.Stop();
                    control.dirty = false;
                    const auto field = control.field;
                    const auto member = control.ruleMember;
                    const double value = control.defaultValue;
                    EmitDock(SettingsUpdateMode::PreviewAndCommit,
                        [field, member, value](DockSettings& settings) {
                            WriteContinuous(
                                settings, field, member, value);
                        });
                });
        }
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
        control.glassToken = control.glass.Toggled(
            [this, &control](const auto&, const auto&) {
                UpdateDependentStates();
                const auto member = control.member;
                const bool value = control.glass.IsOn();
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [member, value](DockSettings& settings) {
                        auto& appearance = (settings.*member).appearance;
                        appearance.backgroundPreset = kAppearancePresetCustom;
                        appearance.glassEnabled = value;
                    });
            });
        control.acrylicToken = control.acrylic.Toggled(
            [this, &control](const auto&, const auto&) {
                const auto member = control.member;
                const bool value = control.acrylic.IsOn();
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [member, value](DockSettings& settings) {
                        auto& appearance = (settings.*member).appearance;
                        appearance.backgroundPreset = kAppearancePresetCustom;
                        appearance.acrylicEnabled = value;
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
        const PersonalizationSettings* appearance =
            &settings.systemTaskbarAppearance;
        if (control.ruleMember)
            appearance = &(settings.*control.ruleMember).appearance;
        switch (control.field)
        {
        case ContinuousField::ThicknessScale:
            return ClampDockScale(settings.thicknessScale) * 100.0;
        case ContinuousField::FrequentItemCount:
            return std::clamp(settings.frequentItemCount, 1, 8);
        case ContinuousField::TaskbarBackgroundAlpha:
            return std::clamp(
                appearance->widgetAlpha,
                0.0f, 1.0f) * 100.0;
        case ContinuousField::TaskbarBorderAlpha:
            return std::clamp(
                appearance->widgetBorderAlpha,
                0.0f, 1.0f) * 100.0;
        case ContinuousField::TaskbarBlurRadius:
            return std::clamp(
                appearance->glassBlurRadius,
                4.0f, 48.0f);
        }
        return 0.0;
    }

    static void WriteContinuous(
        DockSettings& settings,
        ContinuousField field,
        SystemTaskbarDynamicRule DockSettings::* ruleMember,
        double uiValue) noexcept
    {
        PersonalizationSettings* appearance =
            &settings.systemTaskbarAppearance;
        if (ruleMember)
        {
            appearance = &(settings.*ruleMember).appearance;
            appearance->backgroundPreset = kAppearancePresetCustom;
        }
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
            if (!ruleMember)
            {
                settings.systemTaskbarBackdropEnabled = true;
                settings.systemTaskbarFollowPersonalization = false;
            }
            appearance->backgroundPreset = kAppearancePresetCustom;
            appearance->widgetAlpha =
                static_cast<float>(std::clamp(uiValue, 0.0, 100.0) / 100.0);
            break;
        case ContinuousField::TaskbarBorderAlpha:
            if (!ruleMember)
            {
                settings.systemTaskbarBackdropEnabled = true;
                settings.systemTaskbarFollowPersonalization = false;
            }
            appearance->backgroundPreset = kAppearancePresetCustom;
            appearance->widgetBorderAlpha =
                static_cast<float>(std::clamp(uiValue, 0.0, 100.0) / 100.0);
            break;
        case ContinuousField::TaskbarBlurRadius:
            if (!ruleMember)
            {
                settings.systemTaskbarBackdropEnabled = true;
                settings.systemTaskbarFollowPersonalization = false;
            }
            appearance->backgroundPreset = kAppearancePresetCustom;
            appearance->glassBlurRadius =
                static_cast<float>(std::clamp(uiValue, 4.0, 48.0));
            break;
        }
    }

    void Preview(ContinuousControl& control, double value)
    {
        if (!CanEmitDock())
            return;
        control.dirty = true;
        control.idleCommitTimer.Stop();
        control.idleCommitTimer.Start();
        const ContinuousField field = control.field;
        const auto member = control.ruleMember;
        EmitDock(SettingsUpdateMode::Preview,
            [field, member, value](DockSettings& settings) {
                WriteContinuous(settings, field, member, value);
            });
    }

    void Commit(ContinuousControl& control)
    {
        if (control.idleCommitTimer)
            control.idleCommitTimer.Stop();
        if (!control.dirty || !CanEmitDock())
            return;
        control.dirty = false;
        const ContinuousField field = control.field;
        const auto member = control.ruleMember;
        const double value = control.slider.Value();
        EmitDock(SettingsUpdateMode::PreviewAndCommit,
            [field, member, value](DockSettings& settings) {
                WriteContinuous(settings, field, member, value);
            });
    }

    static void WriteColor(
        DockSettings& settings,
        ColorField field,
        SystemTaskbarDynamicRule DockSettings::* ruleMember,
        const winrt::Windows::UI::Color& color) noexcept
    {
        if (!ruleMember)
        {
            settings.systemTaskbarBackdropEnabled = true;
            settings.systemTaskbarFollowPersonalization = false;
        }
        auto& appearance = ruleMember
            ? (settings.*ruleMember).appearance
            : settings.systemTaskbarAppearance;
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
        const auto& appearance = control.ruleMember
            ? (settings.*control.ruleMember).appearance
            : settings.systemTaskbarAppearance;
        if (control.field == ColorField::TaskbarBackground)
        {
            control.editor.SetColor(ToColor(
                appearance.widgetBgR,
                appearance.widgetBgG,
                appearance.widgetBgB));
        }
        else
        {
            control.editor.SetColor(ToColor(
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
        PatchColor(control.backgroundColor, settings);
        PatchColor(control.borderColor, settings);
        PatchContinuous(control.backgroundAlpha, settings);
        PatchContinuous(control.borderAlpha, settings);
        control.glass.IsOn(rule.appearance.glassEnabled);
        PatchContinuous(control.blurRadius, settings);
        control.acrylic.IsOn(rule.appearance.acrylicEnabled);
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
        windowsSystemThemeCombo.SelectedIndex(
            IsWindowsSystemLightThemeEnabled() ? 0 : 1);
        const int taskbarMode = MainTaskbarThemeMode(settings);
        taskbarThemeCombo.SelectedIndex(taskbarMode);
        taskbarContentThemeValue = std::clamp(
            settings.systemTaskbarContentTheme, -1, 1);
        taskbarAppearanceContentThemeValue = std::clamp(
            settings.systemTaskbarAppearance.contentTheme, 0, 1);
        const bool custom = taskbarMode ==
            static_cast<int>(SystemTaskbarThemeMode::Custom);
        ReplaceMainContentThemeItems(custom,
            custom && taskbarContentThemeValue < 0
                ? taskbarAppearanceContentThemeValue
                : taskbarContentThemeValue);
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
        const auto ruleNeedsHook = [](const SystemTaskbarDynamicRule& rule) {
            return rule.enabled &&
                rule.themeMode != SystemTaskbarThemeMode::Native;
        };
        taskbarHookRequired = settings.systemTaskbarBackdropEnabled ||
            ruleNeedsHook(settings.systemTaskbarVisibleWindow) ||
            ruleNeedsHook(settings.systemTaskbarMaximizedWindow) ||
            ruleNeedsHook(settings.systemTaskbarShellUi);
        RefreshTaskbarRuntimeStatus();
        UpdateDependentStates();
    }

    void UpdateDependentStates()
    {
        if (closed)
            return;
        UpdateEdgeSwipeHintVisibility();
        const bool dockEnabled = dockEnabledToggle.IsOn();
        // Match the legacy BeginDisabled(!dockEnabled_) scope. Border and Grid
        // are FrameworkElements, not Controls, so disable each SettingRow's
        // ContentControl host to remove its descendants from keyboard/Tab
        // input. IsHitTestVisible on the card remains the pointer guard.
        floatingEdgeSwipeRow.SetEnabled(dockEnabled);
        positionRow.SetEnabled(dockEnabled);
        monitorScopeRow.SetEnabled(dockEnabled);
        layoutRow.SetEnabled(dockEnabled);
        thicknessScale.row.SetEnabled(dockEnabled);
        showWindowsButtonRow.SetEnabled(dockEnabled);
        showFrequentItemsRow.SetEnabled(dockEnabled);
        edgeSwipeCard.root.IsHitTestVisible(dockEnabled);
        layoutCard.root.IsHitTestVisible(dockEnabled);
        behaviorCard.root.IsHitTestVisible(dockEnabled);
        edgeSwipeCard.root.Opacity(dockEnabled ? 1.0 : 0.62);
        layoutCard.root.Opacity(dockEnabled ? 1.0 : 0.62);
        behaviorCard.root.Opacity(dockEnabled ? 1.0 : 0.62);
        frequentItemCount.slider.IsEnabled(
            dockEnabled && showFrequentItemsToggle.IsOn());
        frequentItemCount.number.IsEnabled(
            dockEnabled && showFrequentItemsToggle.IsOn());
        frequentItemCount.row.SetEnabled(
            dockEnabled && showFrequentItemsToggle.IsOn());

        const int taskbarMode = taskbarThemeCombo.SelectedIndex();
        const bool taskbarStyled = taskbarMode !=
            static_cast<int>(SystemTaskbarThemeMode::Native);
        const bool taskbarCustom = taskbarMode ==
            static_cast<int>(SystemTaskbarThemeMode::Custom);
        taskbarContentThemeRow.root.Visibility(taskbarStyled
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
        taskbarCustomCard.root.Visibility(taskbarCustom
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
        taskbarBlurRadius.row.SetEnabled(
            taskbarCustom && taskbarGlassToggle.IsOn());
        taskbarAcrylicRow.SetEnabled(
            taskbarCustom && taskbarGlassToggle.IsOn());

        for (DynamicRuleControl* control : dynamicRules)
        {
            const bool enabled = control->enabled.IsOn();
            const bool native = control->theme.SelectedIndex() ==
                static_cast<int>(SystemTaskbarThemeMode::Native);
            const bool custom = control->theme.SelectedIndex() ==
                static_cast<int>(SystemTaskbarThemeMode::Custom);
            control->details.Visibility(enabled
                    ? mux::Visibility::Visible
                    : mux::Visibility::Collapsed);
            control->contentThemeRow.root.Visibility(!native
                    ? mux::Visibility::Visible
                    : mux::Visibility::Collapsed);
            control->customAppearance.Visibility(custom
                    ? mux::Visibility::Visible
                    : mux::Visibility::Collapsed);
            control->blurRadius.row.SetEnabled(
                custom && control->glass.IsOn());
            control->acrylicRow.SetEnabled(
                custom && control->glass.IsOn());
        }
    }

    void UpdateEdgeSwipeHintVisibility()
    {
        floatingEdgeSwipeRow.help.Visibility(
            floatingEdgeSwipeToggle.IsOn()
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
    }

    void RefreshTaskbarRuntimeStatus()
    {
        if (!taskbarRuntimeStatus) return;
        std::wstring text;
        if (taskbarHookRequired)
        {
            switch (GetSystemTaskbarBackdropRuntimeState())
            {
            case SystemTaskbarBackdropRuntimeState::Loading:
                text = L("app.settings.taskbar_connecting",
                    L"Connecting to the Explorer taskbar...");
                break;
            case SystemTaskbarBackdropRuntimeState::Unsupported:
                text = L("app.settings.taskbar_unsupported",
                    L"This system does not support taskbar personalization.");
                break;
            case SystemTaskbarBackdropRuntimeState::Failed:
                text = L("app.settings.taskbar_connect_failed",
                    L"Failed to connect to taskbar personalization.");
                break;
            default:
                break;
            }
        }
        taskbarRuntimeStatus.Text(text);
        taskbarRuntimeStatus.Visibility(text.empty()
                ? mux::Visibility::Collapsed
                : mux::Visibility::Visible);
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
        std::wstring_view fallback,
        std::wstring help = {})
    {
        control.row.SetText(L(key, fallback), std::move(help));
        muxa::AutomationProperties::SetName(control.slider,
            control.label.Text());
        if (control.reset)
        {
            const std::wstring text = L(
                "app.settings.restore_default", L"Restore Default");
            control.reset.Content(winrt::box_value(text));
            muxa::AutomationProperties::SetName(control.reset, text);
        }
        muxa::AutomationProperties::SetName(control.number,
            control.label.Text());
    }

    void SetColorText(
        ColorControl& control,
        std::string_view key,
        std::wstring_view fallback)
    {
        control.editor.SetText(
            L(key, fallback), {},
            L("app.settings.apply", L"Apply"),
            L("app.settings.cancel", L"Cancel"));
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

    void ReplaceMainContentThemeItems(bool custom, int logicalValue)
    {
        const bool previousUpdating = updatingControls;
        updatingControls = true;
        taskbarContentThemeCombo.Items().Clear();
        if (custom)
        {
            taskbarContentThemeCombo.Items().Append(winrt::box_value(
                L("app.settings.light", L"Light")));
            taskbarContentThemeCombo.Items().Append(winrt::box_value(
                L("app.settings.dark", L"Dark")));
            taskbarContentThemeCombo.SelectedIndex(
                std::clamp(logicalValue, 0, 1));
        }
        else
        {
            taskbarContentThemeCombo.Items().Append(winrt::box_value(
                L("app.settings.taskbar_foreground_auto", L"Automatic")));
            taskbarContentThemeCombo.Items().Append(winrt::box_value(
                L("app.settings.light", L"Light")));
            taskbarContentThemeCombo.Items().Append(winrt::box_value(
                L("app.settings.dark", L"Dark")));
            taskbarContentThemeCombo.SelectedIndex(
                std::clamp(logicalValue, -1, 1) + 1);
        }
        taskbarContentThemeCustomItems = custom;
        updatingControls = previousUpdating;
    }

    void RefreshLocalizedText()
    {
        if (closed)
            return;
        const bool previousUpdating = updatingControls;
        updatingControls = true;

        SetCardText(enableCard,
            "app.settings.dock_bar", L"Dock and Taskbar");
        SetCardText(edgeSwipeCard,
            "app.dock.floating_edge_swipe", L"Floating Dock Edge Swipe");
        SetCardText(layoutCard,
            "app.dock.layout", L"Dock Layout");
        SetCardText(behaviorCard,
            "app.dock.detailed", L"Dock Behavior");
        SetCardText(taskbarCard,
            "app.settings.system_panel", L"Windows Taskbar");
        SetCardText(taskbarAppearanceCard,
            "app.settings.system_appearance", L"System Appearance");
        SetCardText(taskbarCustomCard,
            "app.settings.custom", L"Custom Taskbar Appearance");

        dockEnabledRow.SetText(L("app.dock.enable", L"Enable Dock"));
        muxa::AutomationProperties::SetName(
            dockEnabledToggle, dockEnabledRow.label.Text());
        positionRow.SetText(L("app.settings.dock_position", L"Position"));
        muxa::AutomationProperties::SetName(
            positionCombo, positionRow.label.Text());
        ReplaceComboItems(positionCombo, {
            {"app.dock.bottom", L"Bottom"},
            {"app.dock.top", L"Top"},
            {"app.dock.left", L"Left"},
            {"app.dock.right", L"Right"},
        });
        layoutRow.SetText(L("app.dock.layout", L"Dock Style"));
        muxa::AutomationProperties::SetName(
            layoutCombo, layoutRow.label.Text());
        ReplaceComboItems(layoutCombo, {
            {"app.dock.island", L"Island"},
            {"app.dock.edge", L"Edge"},
        });
        monitorScopeRow.SetText(
            L("app.settings.display_scope", L"Display Scope"));
        muxa::AutomationProperties::SetName(
            monitorScopeCombo, monitorScopeRow.label.Text());
        ReplaceComboItems(monitorScopeCombo, {
            {"app.dock.first_screen", L"Primary Display"},
            {"app.dock.last_screen", L"Last Active Display"},
            {"app.dock.all_screens", L"All Displays"},
        });
        SetContinuousText(thicknessScale,
            "app.settings.dock_thickness", L"Dock Thickness",
            L("app.settings.dock_thickness_hint",
                L"Adjust dock bar and icon size together."));

        floatingEdgeSwipeRow.SetText(
            L("app.dock.floating_edge_swipe", L"Edge Swipe"),
            L("app.dock.floating_edge_swipe_hint",
                L"Reveal the floating Dock from a screen edge."));
        showWindowsButtonRow.SetText(L(
            "app.dock.show_windows_button", L"Show Windows Button"));
        showFrequentItemsRow.SetText(L(
            "app.dock.show_frequent_items", L"Show Frequent Items"));
        muxa::AutomationProperties::SetName(
            floatingEdgeSwipeToggle, floatingEdgeSwipeRow.label.Text());
        muxa::AutomationProperties::SetName(
            showWindowsButtonToggle, showWindowsButtonRow.label.Text());
        muxa::AutomationProperties::SetName(
            showFrequentItemsToggle, showFrequentItemsRow.label.Text());
        SetContinuousText(frequentItemCount,
            "app.settings.show_count", L"Frequent Item Count");
        SetUnit(frequentItemCount, ExtractNumericUnit(
            L("app.settings.items_unit", L"%d items")));
        taskbarAutoHideRow.SetText(L(
            "app.settings.auto_hide_taskbar", L"Auto-hide System Taskbar"));
        taskbarAlignmentRow.SetText(
            L("app.settings.taskbar_alignment", L"Taskbar Alignment"),
            L("app.settings.taskbar_alignment_hint", L""));
        muxa::AutomationProperties::SetName(
            taskbarAutoHideToggle, taskbarAutoHideRow.label.Text());
        muxa::AutomationProperties::SetName(
            taskbarAlignmentCombo, taskbarAlignmentRow.label.Text());
        ReplaceComboItems(taskbarAlignmentCombo, {
            {"app.settings.taskbar_left", L"Left"},
            {"app.settings.taskbar_center", L"Center"},
        });
        windowsSystemThemeRow.SetText(
            L("app.settings.system_panel", L"System Panel Style"),
            L("app.settings.system_panel_hint", L"") + L" " +
                L("app.settings.system_panel_hint2", L""));
        ReplaceComboItems(windowsSystemThemeCombo, {
            {"app.settings.light", L"Light"},
            {"app.settings.dark", L"Dark"},
        });
        windowsSystemThemeCombo.SelectedIndex(
            IsWindowsSystemLightThemeEnabled() ? 0 : 1);
        muxa::AutomationProperties::SetName(
            windowsSystemThemeCombo, windowsSystemThemeRow.label.Text());
        restartExplorerButton.Content(winrt::box_value(
            L("app.settings.restart_explorer", L"Restart File Explorer")));
        muxa::AutomationProperties::SetName(restartExplorerButton,
            L("app.settings.restart_explorer", L"Restart File Explorer"));

        taskbarThemeRow.SetText(
            L("app.settings.taskbar_theme", L"Taskbar Theme"),
            L("app.settings.taskbar_theme_hint", L""));
        muxa::AutomationProperties::SetName(
            taskbarThemeCombo, taskbarThemeRow.label.Text());
        ReplaceTaskbarThemeItems(taskbarThemeCombo);
        taskbarContentThemeRow.SetText(L(
            "app.settings.taskbar_foreground_color",
            L"Taskbar Icon and Text Color"));
        muxa::AutomationProperties::SetName(
            taskbarContentThemeCombo, taskbarContentThemeRow.label.Text());
        ReplaceMainContentThemeItems(
            taskbarThemeCombo.SelectedIndex() ==
                static_cast<int>(SystemTaskbarThemeMode::Custom),
            taskbarContentThemeCustomItems &&
                    taskbarContentThemeValue < 0
                ? taskbarAppearanceContentThemeValue
                : taskbarContentThemeValue);

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
        taskbarGlassRow.SetText(L(
            "app.settings.glass_enabled", L"Frosted Glass Background"));
        taskbarAcrylicRow.SetText(
            L("app.settings.acrylic_noise", L"Acrylic Noise"));
        muxa::AutomationProperties::SetName(
            taskbarGlassToggle, taskbarGlassRow.label.Text());
        muxa::AutomationProperties::SetName(
            taskbarAcrylicToggle, taskbarAcrylicRow.label.Text());

        RefreshDynamicRuleText(visibleWindowRule,
            "app.settings.taskbar_dynamic_visible_window",
            L"When a window is visible");
        RefreshDynamicRuleText(maximizedWindowRule,
            "app.settings.taskbar_dynamic_maximized_window",
            L"When a window is maximized");
        RefreshDynamicRuleText(shellUiRule,
            "app.settings.taskbar_dynamic_shell_ui",
            L"When a taskbar panel is open");
        RefreshTaskbarRuntimeStatus();

        updatingControls = previousUpdating;
        UpdateDependentStates();
    }

    void RefreshDynamicRuleText(
        DynamicRuleControl& control,
        std::string_view titleKey,
        std::wstring_view fallback)
    {
        SetCardText(control.card, titleKey, fallback);
        control.enabledRow.SetText(L(titleKey, fallback));
        control.themeRow.SetText(L(
            "app.settings.taskbar_dynamic_theme", L"Appearance when active"));
        muxa::AutomationProperties::SetName(
            control.enabled, control.enabledRow.label.Text());
        muxa::AutomationProperties::SetName(
            control.theme, control.themeRow.label.Text());
        ReplaceTaskbarThemeItems(control.theme);
        control.contentThemeRow.SetText(L(
            "app.settings.taskbar_foreground_color",
            L"Taskbar Icon and Text Color"));
        muxa::AutomationProperties::SetName(
            control.contentTheme, control.contentThemeRow.label.Text());
        ReplaceContentThemeItems(control.contentTheme);
        SetColorText(control.backgroundColor,
            "app.settings.bg_color", L"Background Color");
        SetColorText(control.borderColor,
            "app.settings.border_color", L"Border Color");
        SetContinuousText(control.backgroundAlpha,
            "app.settings.bg_opacity", L"Background Opacity");
        SetContinuousText(control.borderAlpha,
            "app.settings.border_opacity", L"Border Opacity");
        control.glassRow.SetText(L(
            "app.settings.glass_enabled", L"Frosted Glass Background"));
        SetContinuousText(control.blurRadius,
            "app.settings.blur_radius", L"Blur Radius");
        control.acrylicRow.SetText(
            L("app.settings.acrylic_noise", L"Acrylic Noise"));
        muxa::AutomationProperties::SetName(
            control.glass, control.glassRow.label.Text());
        muxa::AutomationProperties::SetName(
            control.acrylic, control.acrylicRow.label.Text());
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
            for (ContinuousControl* control : AllContinuousControls())
            {
                if (control->idleCommitTimer)
                    control->idleCommitTimer.Stop();
                control->dirty = false;
            }
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
        if (id == "taskbar.systemTheme" || id == "taskbar.systemPanel")
            return windowsSystemThemeCombo;
        if (id == "taskbar.theme" || id == "taskbar.backdrop" ||
            id == "taskbar.followGlobal")
            return taskbarThemeCombo;
        if (id == "taskbar.contentTheme")
            return taskbarContentThemeCombo;
        if (id == "taskbar.backgroundColor")
            return taskbarBackgroundColor.editor.button;
        if (id == "taskbar.borderColor")
            return taskbarBorderColor.editor.button;
        if (id == "taskbar.backgroundOpacity")
            return taskbarBackgroundAlpha.slider;
        if (id == "taskbar.borderOpacity")
            return taskbarBorderAlpha.slider;
        if (id == "taskbar.glass") return taskbarGlassToggle;
        if (id == "taskbar.blurRadius") return taskbarBlurRadius.slider;
        if (id == "taskbar.acrylic") return taskbarAcrylicToggle;
        if (id == "taskbar.restartExplorer") return restartExplorerButton;
        if (id == "taskbar.dynamic.visibleWindow" ||
            id == "taskbar.visibleWindow")
            return visibleWindowRule.enabled;
        if (id == "taskbar.dynamic.maximizedWindow" ||
            id == "taskbar.maximizedWindow")
            return maximizedWindowRule.enabled;
        if (id == "taskbar.dynamic.shellUi" ||
            id == "taskbar.shellUi")
            return shellUiRule.enabled;
        return nullptr;
    }

    void CommitContinuousEdits() noexcept
    {
        try
        {
            for (ContinuousControl* control : continuousControls)
                Commit(*control);
            for (DynamicRuleControl* rule : dynamicRules)
            {
                Commit(rule->backgroundAlpha);
                Commit(rule->borderAlpha);
                Commit(rule->blurRadius);
            }
        }
        catch (...)
        {
        }
    }

    void DismissColorEditors() noexcept
    {
        for (ColorControl* control : colorControls)
            control->editor.Dismiss();
        for (DynamicRuleControl* rule : dynamicRules)
        {
            rule->backgroundColor.editor.Dismiss();
            rule->borderColor.editor.Dismiss();
        }
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
            control.slider.ValueChanged(control.sliderChanged);
            control.number.ValueChanged(control.numberChanged);
            control.slider.PointerReleased(control.sliderReleased);
            control.number.PointerReleased(control.numberReleased);
            control.slider.LostFocus(control.sliderLostFocus);
            control.number.LostFocus(control.numberLostFocus);
            control.slider.KeyDown(control.sliderKeyDown);
            control.number.KeyDown(control.numberKeyDown);
            if (control.reset)
                control.reset.Click(control.resetToken);
        }
        catch (...)
        {
        }
    }

    void UnhookColorControl(ColorControl& control) noexcept
    {
        control.editor.Close();
    }

    void UnhookDynamicRule(DynamicRuleControl& control) noexcept
    {
        try
        {
            control.enabled.Toggled(control.enabledToken);
            control.theme.SelectionChanged(control.themeToken);
            control.contentTheme.SelectionChanged(control.contentThemeToken);
            control.glass.Toggled(control.glassToken);
            control.acrylic.Toggled(control.acrylicToken);
        }
        catch (...)
        {
        }
    }

    void Close() noexcept
    {
        if (closed)
            return;
        DismissColorEditors();
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
            windowsSystemThemeCombo.SelectionChanged(
                windowsSystemThemeToken);
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
        {
            UnhookContinuousControl(control->backgroundAlpha);
            UnhookContinuousControl(control->borderAlpha);
            UnhookContinuousControl(control->blurRadius);
            UnhookColorControl(control->backgroundColor);
            UnhookColorControl(control->borderColor);
            UnhookDynamicRule(*control);
        }
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
    return impl_ ? impl_->dockRoot : nullptr;
}

mux::UIElement DockPagePresenter::DockEnableContent() const noexcept
{
    return impl_ ? impl_->dockEnableRoot : nullptr;
}

mux::UIElement DockPagePresenter::DockContent() const noexcept
{
    return impl_ ? impl_->dockRoot : nullptr;
}

mux::UIElement DockPagePresenter::TaskbarContent() const noexcept
{
    return impl_ ? impl_->taskbarRoot : nullptr;
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
    impl_->DismissColorEditors();
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

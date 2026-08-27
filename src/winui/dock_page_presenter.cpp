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
using presenter_controls::CoalescedPreviewTimer;
using presenter_controls::QuantizeNumericValue;
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
    muxc::Grid editors{nullptr};
    muxc::Slider slider{nullptr};
    muxc::NumberBox number{nullptr};
    muxc::TextBlock unit{nullptr};
    muxc::Button reset{nullptr};
    ContinuousField field = ContinuousField::ThicknessScale;
    SystemTaskbarDynamicRule DockSettings::* ruleMember = nullptr;
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
    muxc::StackPanel root{nullptr};
    muxc::Expander expander{nullptr};
    muxc::TextBlock detailTitle{nullptr};
    muxc::TextBlock summary{nullptr};
    muxc::ToggleSwitch enabled{nullptr};
    muxc::ComboBox theme{nullptr};
    muxc::ComboBox contentTheme{nullptr};
    muxc::StackPanel details{nullptr};
    muxc::StackPanel appearanceDetails{nullptr};
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

void StretchExpanderBody(
    const muxc::Expander& expander,
    const mux::FrameworkElement& body)
{
    const auto weakBody = winrt::make_weak(body);
    expander.SizeChanged(
        [weakBody](const auto&, const mux::SizeChangedEventArgs& args) {
            if (const auto currentBody = weakBody.get())
            {
                // The WinUI Expander template contributes 16 DIP of padding
                // on each side. Keep rule rows aligned with the card body.
                currentBody.Width(std::max(0.0,
                    static_cast<double>(args.NewSize().Width) - 32.0));
            }
        });
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
    SettingsCard taskbarRulesCard;
    SettingsCard taskbarSystemPanelCard;

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
    muxc::ToggleSwitch allowDesktopContentOverlapToggle{nullptr};
    muxc::ToggleSwitch showOnlyWhenSummonedToggle{nullptr};
    ContinuousControl thicknessScale;
    ContinuousControl frequentItemCount;

    muxc::ToggleSwitch taskbarAutoHideToggle{nullptr};
    muxc::RadioButtons taskbarAlignmentChoices{nullptr};
    muxc::Button restartExplorerButton{nullptr};
    muxc::ComboBox taskbarThemeCombo{nullptr};
    muxc::ComboBox taskbarContentThemeCombo{nullptr};
    muxc::StackPanel taskbarCustomAppearance{nullptr};
    muxc::TextBlock taskbarCustomTitle{nullptr};
    muxc::TextBlock taskbarRulesHint{nullptr};
    ColorControl taskbarBackgroundColor;
    ColorControl taskbarBorderColor;
    ContinuousControl taskbarBackgroundAlpha;
    ContinuousControl taskbarBorderAlpha;
    ContinuousControl taskbarBlurRadius;
    muxc::ToggleSwitch taskbarGlassToggle{nullptr};
    muxc::ToggleSwitch taskbarAcrylicToggle{nullptr};
    muxc::RadioButtons windowsSystemThemeChoices{nullptr};
    muxc::InfoBar taskbarRuntimeStatus{nullptr};

    SettingRow dockEnabledRow;
    SettingRow positionRow;
    SettingRow layoutRow;
    SettingRow monitorScopeRow;
    SettingRow floatingEdgeSwipeRow;
    SettingRow showWindowsButtonRow;
    SettingRow showFrequentItemsRow;
    SettingRow allowDesktopContentOverlapRow;
    SettingRow showOnlyWhenSummonedRow;
    SettingRow taskbarAutoHideRow;
    SettingRow taskbarAlignmentRow;
    SettingRow windowsSystemThemeRow;
    SettingRow restartExplorerRow;
    SettingRow taskbarThemeRow;
    SettingRow taskbarContentThemeRow;
    SettingRow taskbarGlassRow;
    SettingRow taskbarAcrylicRow;

    DynamicRuleControl visibleWindowRule;
    DynamicRuleControl maximizedWindowRule;
    DynamicRuleControl shellUiRule;
    std::array<DynamicRuleControl*, 3> dynamicRules = {
        &shellUiRule, &maximizedWindowRule, &visibleWindowRule};
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
    std::uint64_t systemTaskbarRevision = 0;
    bool hasSnapshot = false;
    bool updatingControls = false;
    bool synchronizingPair = false;
    bool taskbarContentThemeCustomItems = false;
    bool taskbarHookRequired = false;
    bool taskbarInputReady = false;
    bool taskbarAutoHideValue = false;
    int taskbarAlignmentValue = 1;
    int windowsSystemThemeValue = 0;
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
    winrt::event_token allowDesktopContentOverlapToken{};
    winrt::event_token showOnlyWhenSummonedToken{};
    winrt::event_token taskbarAutoHideToken{};
    winrt::event_token taskbarAlignmentToken{};
    winrt::event_token restartExplorerToken{};
    winrt::event_token taskbarThemeToken{};
    winrt::event_token taskbarContentThemeToken{};
    winrt::event_token taskbarGlassToken{};
    winrt::event_token taskbarAcrylicToken{};
    winrt::event_token windowsSystemThemeToken{};
    winrt::event_token taskbarRootLoadedToken{};

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
        allowDesktopContentOverlapToggle = muxc::ToggleSwitch{};
        showOnlyWhenSummonedToggle = muxc::ToggleSwitch{};
        for (const auto& toggle : {
                 floatingShortcutToggle,
                 floatingEdgeSwipeToggle,
                 showWindowsButtonToggle,
                 showFrequentItemsToggle,
                 keepWhenDesktopHiddenToggle,
                 allowDesktopContentOverlapToggle,
                 showOnlyWhenSummonedToggle})
        {
            toggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
        }
        floatingShortcutHint = NewHint();
        floatingEdgeSwipeHint = NewHint();
        floatingEdgeSwipeRow.Initialize(floatingEdgeSwipeToggle);
        showWindowsButtonRow.Initialize(showWindowsButtonToggle);
        showFrequentItemsRow.Initialize(showFrequentItemsToggle);
        allowDesktopContentOverlapRow.Initialize(
            allowDesktopContentOverlapToggle);
        showOnlyWhenSummonedRow.Initialize(showOnlyWhenSummonedToggle);
        floatingEdgeSwipeRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        showWindowsButtonRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        showFrequentItemsRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        allowDesktopContentOverlapRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        showOnlyWhenSummonedRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        InitializeContinuousControl(frequentItemCount,
            ContinuousField::FrequentItemCount, 1.0, 8.0, 1.0);
        // Floating shortcut mode/hotkey is rendered once by General.
        edgeSwipeCard.content.Children().Append(floatingEdgeSwipeRow.root);
        behaviorCard.content.Children().Append(
            allowDesktopContentOverlapRow.root);
        behaviorCard.content.Children().Append(showOnlyWhenSummonedRow.root);
        behaviorCard.content.Children().Append(showWindowsButtonRow.root);
        behaviorCard.content.Children().Append(showFrequentItemsRow.root);
        behaviorCard.content.Children().Append(frequentItemCount.root);
        // keepWhenDesktopHidden is a post-migration field and intentionally
        // remains outside the 1:1 legacy surface.

        InitializeCard(taskbarCard, cardStyle, taskbarRoot);
        taskbarAutoHideToggle = muxc::ToggleSwitch{};
        taskbarAutoHideToggle.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        taskbarAlignmentChoices = NewInlineChoices();
        taskbarAutoHideRow.Initialize(taskbarAutoHideToggle);
        taskbarAutoHideRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        taskbarAlignmentRow.Initialize(taskbarAlignmentChoices);
        taskbarCard.content.Children().Append(taskbarAutoHideRow.root);
        taskbarCard.content.Children().Append(taskbarAlignmentRow.root);

        InitializeCard(taskbarAppearanceCard, cardStyle, taskbarRoot);
        taskbarThemeCombo = NewCombo();
        taskbarContentThemeCombo = NewCombo();
        taskbarThemeRow.Initialize(taskbarThemeCombo);
        taskbarContentThemeRow.Initialize(taskbarContentThemeCombo);
        taskbarRuntimeStatus = muxc::InfoBar{};
        taskbarRuntimeStatus.IsClosable(false);
        taskbarRuntimeStatus.IsOpen(false);
        taskbarAppearanceCard.content.Children().Append(taskbarThemeRow.root);
        taskbarAppearanceCard.content.Children().Append(
            taskbarContentThemeRow.root);
        taskbarAppearanceCard.content.Children().Append(taskbarRuntimeStatus);

        taskbarCustomAppearance = muxc::StackPanel{};
        taskbarCustomAppearance.Spacing(12.0);
        taskbarCustomTitle = muxc::TextBlock{};
        taskbarCustomTitle.FontWeight(
            winrt::Windows::UI::Text::FontWeights::SemiBold());
        taskbarCustomTitle.TextWrapping(mux::TextWrapping::Wrap);
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
        taskbarCustomAppearance.Children().Append(taskbarCustomTitle);
        taskbarCustomAppearance.Children().Append(taskbarBackgroundColor.root);
        taskbarCustomAppearance.Children().Append(taskbarBorderColor.root);
        taskbarCustomAppearance.Children().Append(taskbarBackgroundAlpha.root);
        taskbarCustomAppearance.Children().Append(taskbarBorderAlpha.root);
        taskbarCustomAppearance.Children().Append(taskbarGlassRow.root);
        taskbarCustomAppearance.Children().Append(taskbarBlurRadius.root);
        taskbarCustomAppearance.Children().Append(taskbarAcrylicRow.root);
        taskbarAppearanceCard.content.Children().Append(
            taskbarCustomAppearance);

        InitializeCard(taskbarRulesCard, cardStyle, taskbarRoot);
        taskbarRulesHint = NewHint();
        taskbarRulesCard.content.Children().Append(taskbarRulesHint);
        InitializeDynamicRule(shellUiRule,
            &DockSettings::systemTaskbarShellUi);
        InitializeDynamicRule(maximizedWindowRule,
            &DockSettings::systemTaskbarMaximizedWindow);
        InitializeDynamicRule(visibleWindowRule,
            &DockSettings::systemTaskbarVisibleWindow);

        InitializeCard(taskbarSystemPanelCard, cardStyle, taskbarRoot);
        windowsSystemThemeChoices = NewInlineChoices();
        restartExplorerButton = muxc::Button{};
        restartExplorerButton.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        restartExplorerButton.VerticalAlignment(
            mux::VerticalAlignment::Center);
        windowsSystemThemeRow.Initialize(windowsSystemThemeChoices);
        restartExplorerRow.Initialize(restartExplorerButton);
        restartExplorerRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        taskbarSystemPanelCard.content.Children().Append(
            windowsSystemThemeRow.root);
        taskbarSystemPanelCard.content.Children().Append(
            restartExplorerRow.root);
    }

    muxc::ComboBox NewCombo()
    {
        muxc::ComboBox combo;
        combo.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        combo.MaxWidth(520.0);
        return combo;
    }

    muxc::RadioButtons NewInlineChoices()
    {
        muxc::RadioButtons choices{};
        choices.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        choices.MaxColumns(2);
        // Keep application-owned RadioButton instances stable. Rebuilding a
        // string Items collection while the control is unloaded can leave
        // RadioButtons.SelectedIndex set but no generated container checked.
        choices.Items().Append(muxc::RadioButton{});
        choices.Items().Append(muxc::RadioButton{});
        return choices;
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
        control.slider.VerticalAlignment(mux::VerticalAlignment::Center);
        control.slider.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
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
        control.field = field;
        control.ruleMember = ruleMember;
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
        control.root = muxc::StackPanel{};
        control.root.Spacing(6.0);
        control.enabled = muxc::ToggleSwitch{};
        control.enabled.HorizontalAlignment(mux::HorizontalAlignment::Right);
        control.theme = NewCombo();
        control.contentTheme = NewCombo();
        control.member = member;
        control.enabledRow.Initialize(control.enabled);
        control.enabledRow.SetControlAlignment(mux::HorizontalAlignment::Right);

        control.expander = muxc::Expander{};
        control.expander.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        control.expander.HorizontalContentAlignment(
            mux::HorizontalAlignment::Stretch);
        control.expander.IsExpanded(false);
        control.expander.UseSystemFocusVisuals(true);
        muxc::StackPanel header{};
        header.Spacing(3.0);
        control.detailTitle = muxc::TextBlock{};
        control.detailTitle.FontWeight(
            winrt::Windows::UI::Text::FontWeights::SemiBold());
        control.detailTitle.TextWrapping(mux::TextWrapping::Wrap);
        control.summary = NewHint();
        header.Children().Append(control.detailTitle);
        header.Children().Append(control.summary);
        control.expander.Header(header);

        control.details = muxc::StackPanel{};
        control.details.Spacing(12.0);
        control.details.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        control.themeRow.Initialize(control.theme);
        control.contentThemeRow.Initialize(control.contentTheme);
        // Keep the rule switch inside its named scenario.  The scenario is the
        // parent disclosure and all appearance editors are dependent children.
        control.details.Children().Append(control.enabledRow.root);
        control.appearanceDetails = muxc::StackPanel{};
        control.appearanceDetails.Spacing(12.0);
        control.appearanceDetails.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        control.appearanceDetails.Children().Append(control.themeRow.root);
        control.appearanceDetails.Children().Append(
            control.contentThemeRow.root);

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
        control.appearanceDetails.Children().Append(control.customAppearance);
        control.details.Children().Append(control.appearanceDetails);
        control.expander.Content(control.details);
        StretchExpanderBody(control.expander, control.details);
        control.root.Children().Append(control.expander);
        taskbarRulesCard.content.Children().Append(control.root);
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
        taskbarRootLoadedToken = taskbarRoot.Loaded(
            [this](const auto&, const auto&) {
                if (closed)
                    return;
                RefreshTaskbarEntryState();
                taskbarInputReady = true;
            });
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
                        snowdesktop::dock_settings_rules::
                            DisableSummonOnlyWhenPrerequisiteDisabled(
                                settings.floatingEdgeSwipeEnabled,
                                settings.showOnlyWhenSummoned);
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
        allowDesktopContentOverlapToken =
            allowDesktopContentOverlapToggle.Toggled(
                [this](const auto&, const auto&) {
                    const bool value =
                        allowDesktopContentOverlapToggle.IsOn();
                    EmitDock(SettingsUpdateMode::PreviewAndCommit,
                        [value](DockSettings& settings) {
                            settings.allowDesktopContentOverlap = value;
                            snowdesktop::dock_settings_rules::
                                DisableSummonOnlyWhenPrerequisiteDisabled(
                                    settings.allowDesktopContentOverlap,
                                    settings.showOnlyWhenSummoned);
                        });
                });
        showOnlyWhenSummonedToken = showOnlyWhenSummonedToggle.Toggled(
            [this](const auto&, const auto&) {
                const bool value = showOnlyWhenSummonedToggle.IsOn();
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [value](DockSettings& settings) {
                        settings.showOnlyWhenSummoned = value;
                        snowdesktop::dock_settings_rules::
                            NormalizeSummonOnlyDependencies(
                                settings.showOnlyWhenSummoned,
                                settings.allowDesktopContentOverlap,
                                settings.floatingEdgeSwipeEnabled);
                    });
            });
        taskbarAutoHideToken = taskbarAutoHideToggle.Toggled(
            [this](const auto&, const auto&) {
                if (!taskbarInputReady)
                    return;
                const bool value = taskbarAutoHideToggle.IsOn();
                if (!updatingControls)
                    taskbarAutoHideValue = value;
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [value](DockSettings& settings) {
                        settings.systemTaskbarAutoHide = value;
                    });
            });
        taskbarAlignmentToken = taskbarAlignmentChoices.SelectionChanged(
            [this](const auto&, const auto&) {
                if (!taskbarInputReady)
                    return;
                const int value = taskbarAlignmentChoices.SelectedIndex();
                if (value < 0) return;
                if (!updatingControls)
                    taskbarAlignmentValue = std::clamp(value, 0, 1);
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [value](DockSettings& settings) {
                        settings.systemTaskbarAlignment =
                            std::clamp(value, 0, 1);
                    });
            });
        windowsSystemThemeToken = windowsSystemThemeChoices.SelectionChanged(
            [this](const auto&, const auto&) {
                if (!taskbarInputReady || closed || updatingControls ||
                    !active || !hasSnapshot)
                    return;
                const int value = windowsSystemThemeChoices.SelectedIndex();
                if (value >= 0)
                {
                    windowsSystemThemeValue = std::clamp(value, 0, 1);
                    (void)RequestWindowsSystemLightThemeEnabled(value == 0);
                }
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
                    control.preview.Cancel();
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
        control.preview.Queue(value);
    }

    void PublishPreview(ContinuousControl& control, double value)
    {
        if (!control.dirty || !CanEmitDock())
            return;
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
        control.preview.Cancel();
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
        const double value = QuantizeNumericValue(
            ReadContinuous(control, settings),
            control.slider.Minimum(), control.slider.Maximum(),
            control.slider.StepFrequency());
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

    void PatchSystemTaskbarControls(const DockSettings& settings)
    {
        taskbarAutoHideValue = settings.systemTaskbarAutoHide;
        taskbarAlignmentValue = std::clamp(
            settings.systemTaskbarAlignment, 0, 1);
        taskbarAutoHideToggle.IsOn(taskbarAutoHideValue);
        SelectChoice(taskbarAlignmentChoices, taskbarAlignmentValue);
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
        allowDesktopContentOverlapToggle.IsOn(
            settings.allowDesktopContentOverlap);
        showOnlyWhenSummonedToggle.IsOn(settings.showOnlyWhenSummoned);
        PatchSystemTaskbarControls(settings);
        windowsSystemThemeValue =
            IsWindowsSystemLightThemeEnabled() ? 0 : 1;
        SelectChoice(windowsSystemThemeChoices, windowsSystemThemeValue);
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
        allowDesktopContentOverlapRow.SetEnabled(dockEnabled);
        showOnlyWhenSummonedRow.SetEnabled(dockEnabled);
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
        taskbarContentThemeRow.SetEnabled(taskbarStyled);
        taskbarCustomAppearance.Visibility(taskbarCustom
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
        taskbarBackgroundColor.editor.row.SetEnabled(taskbarCustom);
        taskbarBorderColor.editor.row.SetEnabled(taskbarCustom);
        taskbarBackgroundAlpha.row.SetEnabled(taskbarCustom);
        taskbarBorderAlpha.row.SetEnabled(taskbarCustom);
        taskbarGlassRow.SetEnabled(taskbarCustom);
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
            control->appearanceDetails.Visibility(enabled
                    ? mux::Visibility::Visible
                    : mux::Visibility::Collapsed);
            control->appearanceDetails.IsHitTestVisible(enabled);
            control->contentThemeRow.root.Visibility(!native
                    ? mux::Visibility::Visible
                    : mux::Visibility::Collapsed);
            control->customAppearance.Visibility(custom
                    ? mux::Visibility::Visible
                    : mux::Visibility::Collapsed);
            control->themeRow.SetEnabled(enabled);
            control->contentThemeRow.SetEnabled(enabled && !native);
            control->backgroundColor.editor.row.SetEnabled(
                enabled && custom);
            control->borderColor.editor.row.SetEnabled(enabled && custom);
            control->backgroundAlpha.row.SetEnabled(enabled && custom);
            control->borderAlpha.row.SetEnabled(enabled && custom);
            control->glassRow.SetEnabled(enabled && custom);
            control->blurRadius.row.SetEnabled(
                enabled && custom && control->glass.IsOn());
            control->acrylicRow.SetEnabled(
                enabled && custom && control->glass.IsOn());
            RefreshDynamicRuleSummary(*control);
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
        muxc::InfoBarSeverity severity =
            muxc::InfoBarSeverity::Informational;
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
                severity = muxc::InfoBarSeverity::Warning;
                break;
            case SystemTaskbarBackdropRuntimeState::Failed:
                text = L("app.settings.taskbar_connect_failed",
                    L"Failed to connect to taskbar personalization.");
                severity = muxc::InfoBarSeverity::Error;
                break;
            default:
                break;
            }
        }
        const std::wstring title = L(
            "settings.taskbar.defaultAppearance", L"Default appearance");
        taskbarRuntimeStatus.Title(title);
        taskbarRuntimeStatus.Message(text);
        taskbarRuntimeStatus.Severity(severity);
        taskbarRuntimeStatus.IsOpen(!text.empty());
        muxa::AutomationProperties::SetName(taskbarRuntimeStatus, title);
        muxa::AutomationProperties::SetHelpText(taskbarRuntimeStatus, text);
    }

    void RefreshTaskbarEntryState()
    {
        const bool previousUpdating = updatingControls;
        updatingControls = true;
        try
        {
            taskbarAutoHideToggle.IsOn(taskbarAutoHideValue);
            SelectChoice(taskbarAlignmentChoices, taskbarAlignmentValue);
            windowsSystemThemeValue =
                IsWindowsSystemLightThemeEnabled() ? 0 : 1;
            SelectChoice(
                windowsSystemThemeChoices, windowsSystemThemeValue);
            RefreshTaskbarRuntimeStatus();
        }
        catch (...)
        {
        }
        updatingControls = previousUpdating;
    }

    void RefreshTaskbarRuntimeState()
    {
        RefreshTaskbarEntryState();
    }

    void SetCardText(
        SettingsCard& card,
        std::string_view key,
        std::wstring_view fallback)
    {
        SetCardText(card, L(key, fallback));
    }

    void SetCardText(SettingsCard& card, std::wstring text)
    {
        card.title.Text(std::move(text));
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
            presenter_controls::ConfigureRestoreDefaultButton(
                control.reset, text);
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

    void SelectChoice(
        const muxc::RadioButtons& choices,
        int selectedIndex)
    {
        const int selected = std::clamp(selectedIndex, 0, 1);
        for (int index = 0; index < 2; ++index)
        {
            if (const auto option =
                    choices.Items().GetAt(index).try_as<muxc::RadioButton>())
            {
                option.IsChecked(index == selected);
            }
        }
        choices.SelectedIndex(selected);
    }

    void ReplaceChoiceItems(
        const muxc::RadioButtons& choices,
        const std::initializer_list<std::pair<
            std::string_view, std::wstring_view>>& items,
        int selectedIndex)
    {
        std::uint32_t index = 0;
        for (const auto& [key, fallback] : items)
        {
            const auto option = choices.Items().GetAt(index).as<
                muxc::RadioButton>();
            const std::wstring text = L(key, fallback);
            option.Content(winrt::box_value(text));
            muxa::AutomationProperties::SetName(option, text);
            ++index;
        }
        SelectChoice(choices, selectedIndex);
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

    [[nodiscard]] std::wstring TaskbarThemeText(int index) const
    {
        switch (index)
        {
        case 0:
            return L("app.settings.taskbar_windows_native",
                L"Windows Native");
        case 1:
            return L("app.settings.taskbar_follow_global",
                L"Follow Global Theme");
        case 2:
            return L("app.settings.dark", L"Dark");
        case 3:
            return L("app.settings.light", L"Light");
        case 4:
            return L("app.settings.dark_glass", L"Dark Glass");
        case 5:
            return L("app.settings.light_glass", L"Light Glass");
        case 6:
            return L("app.settings.dark_acrylic", L"Dark Acrylic");
        case 7:
            return L("app.settings.light_acrylic", L"Light Acrylic");
        case 8:
            return L("app.settings.custom", L"Custom");
        case 9:
            return L("app.settings.taskbar_transparent", L"Transparent");
        default:
            return L("app.settings.taskbar_dynamic_theme",
                L"Appearance when active");
        }
    }

    [[nodiscard]] static std::wstring ContextualText(
        std::wstring_view context,
        std::wstring_view text)
    {
        if (context.empty())
            return std::wstring(text);
        if (text.empty())
            return std::wstring(context);
        std::wstring result(context);
        result.append(L" — ");
        result.append(text);
        return result;
    }

    void RefreshDynamicRuleSummary(DynamicRuleControl& control)
    {
        const std::wstring summary = control.enabled.IsOn()
            ? TaskbarThemeText(control.theme.SelectedIndex())
            : L("app.settings.hotkey_status_disabled", L"Disabled");
        control.summary.Text(summary);

        const winrt::hstring contextText = control.detailTitle.Text();
        const std::wstring context{
            contextText.c_str(), contextText.size()};
        muxa::AutomationProperties::SetName(control.expander, context);
        muxa::AutomationProperties::SetHelpText(control.expander,
            ContextualText(context, summary));
    }

    void SetDynamicRuleAutomation(
        DynamicRuleControl& control,
        std::wstring_view context)
    {
        const std::wstring help = ContextualText(context,
            L("settings.taskbar.theme.description",
                L"Set taskbar theme, material, colors and dynamic rules."));
        const auto set = [&](const auto& element, std::wstring_view label) {
            muxa::AutomationProperties::SetName(
                element, ContextualText(context, label));
            muxa::AutomationProperties::SetHelpText(element, help);
        };

        muxa::AutomationProperties::SetName(control.enabled,
            ContextualText(context,
                L("app.settings.widgets_enabled", L"Enabled")));
        muxa::AutomationProperties::SetHelpText(control.enabled, help);
        set(control.theme, control.themeRow.label.Text());
        set(control.contentTheme, control.contentThemeRow.label.Text());
        set(control.backgroundColor.editor.button,
            control.backgroundColor.editor.row.label.Text());
        set(control.backgroundColor.editor.picker,
            control.backgroundColor.editor.row.label.Text());
        set(control.borderColor.editor.button,
            control.borderColor.editor.row.label.Text());
        set(control.borderColor.editor.picker,
            control.borderColor.editor.row.label.Text());
        set(control.backgroundColor.editor.cancel,
            L("app.settings.cancel", L"Cancel"));
        set(control.borderColor.editor.cancel,
            L("app.settings.cancel", L"Cancel"));
        for (ContinuousControl* numeric : {
                 &control.backgroundAlpha,
                 &control.borderAlpha,
                 &control.blurRadius})
        {
            set(numeric->slider, numeric->label.Text());
            set(numeric->number, numeric->label.Text());
            if (numeric->reset)
            {
                const std::wstring action = ContextualText(
                    L("app.settings.restore_default", L"Restore Default"),
                    numeric->label.Text());
                const std::wstring accessible =
                    ContextualText(context, action);
                muxc::ToolTipService::SetToolTip(
                    numeric->reset, winrt::box_value(action));
                muxa::AutomationProperties::SetName(
                    numeric->reset, accessible);
                muxa::AutomationProperties::SetHelpText(
                    numeric->reset, accessible);
            }
        }
        set(control.glass, control.glassRow.label.Text());
        set(control.acrylic, control.acrylicRow.label.Text());
        muxa::AutomationProperties::SetName(
            control.root, std::wstring(context));
        muxa::AutomationProperties::SetHelpText(control.root, help);
        muxa::AutomationProperties::SetName(control.appearanceDetails,
            ContextualText(context,
                L("app.settings.taskbar_dynamic_theme",
                    L"Appearance when active")));
        muxa::AutomationProperties::SetHelpText(
            control.appearanceDetails, help);
        RefreshDynamicRuleSummary(control);
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
            "settings.dock.edgeSwipe", L"Floating Dock edge gesture");
        SetCardText(layoutCard,
            "settings.dock.layoutAndPosition", L"Position and layout");
        SetCardText(behaviorCard,
            "settings.dock.itemsAndBehavior", L"Items and behavior");
        SetCardText(taskbarCard,
            "settings.taskbar.behavior", L"Behavior");
        SetCardText(taskbarAppearanceCard,
            "settings.taskbar.defaultAppearance", L"Default appearance");
        SetCardText(taskbarRulesCard,
            "settings.taskbar.scenarioOverrides", L"Scenario overrides");
        SetCardText(taskbarSystemPanelCard,
            "settings.taskbar.systemPanels", L"System panels");
        taskbarCustomTitle.Text(L(
            "settings.taskbar.customAppearance", L"Custom appearance"));
        muxa::AutomationProperties::SetName(
            taskbarCustomAppearance, taskbarCustomTitle.Text());
        const std::wstring rulesDescription =
            L("settings.taskbar.theme.description",
                L"Set taskbar theme, material, colors and dynamic rules.");
        taskbarRulesHint.Text(L(
            "settings.taskbar.scenarioOverrides.description",
            L"Rules are matched from top to bottom. Higher rules take "
              "priority; otherwise the default appearance is used."));
        muxa::AutomationProperties::SetHelpText(
            taskbarRulesCard.root, taskbarRulesHint.Text());
        muxa::AutomationProperties::SetHelpText(taskbarCard.root,
            L("settings.taskbar.autoHide.description",
                L"Control Windows taskbar auto-hide.") + L" " +
            L("settings.taskbar.alignment.description",
                L"Align taskbar items to the left or center."));
        muxa::AutomationProperties::SetHelpText(taskbarAppearanceCard.root,
            rulesDescription);
        const std::wstring systemPanelHelp =
            L("app.settings.system_panel_hint", L"") + L" " +
            L("app.settings.system_panel_hint2", L"");
        muxa::AutomationProperties::SetHelpText(
            taskbarSystemPanelCard.root, systemPanelHelp);

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
        allowDesktopContentOverlapRow.SetText(
            L("settings.dock.allowDesktopContentOverlap",
                L"Allow Dock to overlap desktop content"),
            L("settings.dock.allowDesktopContentOverlap.description",
                L"Stop reserving desktop layout space for the Dock, so it "
                  "may cover content near the screen edge."));
        showOnlyWhenSummonedRow.SetText(
            L("settings.dock.showOnlyWhenSummoned",
                L"Show Dock only when summoned"),
            L("settings.dock.showOnlyWhenSummoned.description",
                L"Keep the Dock hidden until summoned. Swipe along its "
                  "screen edge to show it as a floating Dock. It hides "
                  "again after you click outside to dismiss it, while "
                  "moving a dragged item to that edge reveals it "
                  "temporarily. Enabling this also allows desktop content "
                  "overlap and edge-swipe reveal."));
        showWindowsButtonRow.SetText(L(
            "app.dock.show_windows_button", L"Show Windows Button"));
        showFrequentItemsRow.SetText(L(
            "app.dock.show_frequent_items", L"Show Frequent Items"));
        muxa::AutomationProperties::SetName(
            floatingEdgeSwipeToggle, floatingEdgeSwipeRow.label.Text());
        muxa::AutomationProperties::SetName(
            allowDesktopContentOverlapToggle,
            allowDesktopContentOverlapRow.label.Text());
        muxa::AutomationProperties::SetName(
            showOnlyWhenSummonedToggle,
            showOnlyWhenSummonedRow.label.Text());
        muxa::AutomationProperties::SetName(
            showWindowsButtonToggle, showWindowsButtonRow.label.Text());
        muxa::AutomationProperties::SetName(
            showFrequentItemsToggle, showFrequentItemsRow.label.Text());
        SetContinuousText(frequentItemCount,
            "app.settings.show_count", L"Frequent Item Count");
        SetUnit(frequentItemCount, ExtractNumericUnit(
            L("app.settings.items_unit", L"%d items")));
        taskbarAutoHideRow.SetText(
            L("settings.taskbar.autoHide", L"Automatically hide taskbar"),
            L("settings.taskbar.autoHide.description",
                L"Control Windows taskbar auto-hide."));
        taskbarAlignmentRow.SetText(
            L("settings.taskbar.alignment", L"Taskbar alignment"),
            L("settings.taskbar.alignment.description",
                L"Align taskbar items to the left or center."));
        muxa::AutomationProperties::SetName(
            taskbarAutoHideToggle, taskbarAutoHideRow.label.Text());
        muxa::AutomationProperties::SetName(
            taskbarAlignmentChoices, taskbarAlignmentRow.label.Text());
        ReplaceChoiceItems(taskbarAlignmentChoices, {
            {"app.settings.taskbar_left", L"Left"},
            {"app.settings.taskbar_center", L"Center"},
        }, taskbarAlignmentValue);
        windowsSystemThemeRow.SetText(
            L("app.settings.appearance", L"Appearance"), systemPanelHelp);
        windowsSystemThemeValue =
            IsWindowsSystemLightThemeEnabled() ? 0 : 1;
        ReplaceChoiceItems(windowsSystemThemeChoices, {
            {"app.settings.light", L"Light"},
            {"app.settings.dark", L"Dark"},
        }, windowsSystemThemeValue);
        muxa::AutomationProperties::SetName(
            windowsSystemThemeChoices, windowsSystemThemeRow.label.Text());
        restartExplorerRow.SetText(
            L("settings.taskbar.restartExplorer",
                L"Restart File Explorer"),
            L("settings.taskbar.restartExplorer.description",
                L"Restart File Explorer to apply Windows shell changes."));
        restartExplorerButton.Content(winrt::box_value(
            L("settings.taskbar.restartExplorer",
                L"Restart File Explorer")));
        muxa::AutomationProperties::SetName(restartExplorerButton,
            L("settings.taskbar.restartExplorer",
                L"Restart File Explorer"));
        muxa::AutomationProperties::SetHelpText(restartExplorerButton,
            L("settings.taskbar.restartExplorer.description",
                L"Restart File Explorer to apply Windows shell changes."));

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

        RefreshDynamicRuleText(shellUiRule,
            "app.settings.taskbar_dynamic_shell_ui",
            L"When Start, Search, Task View, or a system panel is open");
        RefreshDynamicRuleText(maximizedWindowRule,
            "app.settings.taskbar_dynamic_maximized_window",
            L"When this display has a maximized app window");
        RefreshDynamicRuleText(visibleWindowRule,
            "app.settings.taskbar_dynamic_visible_window",
            L"When this display has an app window");
        RefreshTaskbarRuntimeStatus();

        updatingControls = previousUpdating;
        UpdateDependentStates();
    }

    void RefreshDynamicRuleText(
        DynamicRuleControl& control,
        std::string_view titleKey,
        std::wstring_view fallback)
    {
        const std::wstring context = L(titleKey, fallback);
        control.detailTitle.Text(context);
        control.enabledRow.SetText(
            L("app.settings.widgets_enabled", L"Enabled"));
        control.themeRow.SetText(L(
            "app.settings.taskbar_dynamic_theme", L"Appearance when active"));
        ReplaceTaskbarThemeItems(control.theme);
        control.contentThemeRow.SetText(L(
            "app.settings.taskbar_foreground_color",
            L"Taskbar Icon and Text Color"));
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
        SetDynamicRuleAutomation(control, context);
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
            systemTaskbarRevision =
                snapshot.domainRevisions.systemTaskbar;
        }
        else if (snapshot.domainRevisions.systemTaskbar !=
            systemTaskbarRevision)
        {
            PatchSystemTaskbarControls(snapshot.values.dock);
            systemTaskbarRevision =
                snapshot.domainRevisions.systemTaskbar;
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
        if (id == "dock.allowDesktopContentOverlap")
            return allowDesktopContentOverlapToggle;
        if (id == "dock.showOnlyWhenSummoned" || id == "dock.autoHide")
            return showOnlyWhenSummonedToggle;
        if (id == "taskbar.autoHide") return taskbarAutoHideToggle;
        if (id == "taskbar.alignment") return taskbarAlignmentChoices;
        if (id == "taskbar.systemTheme" || id == "taskbar.systemPanel")
            return windowsSystemThemeChoices;
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
            return visibleWindowRule.expander;
        if (id == "taskbar.dynamic.maximizedWindow" ||
            id == "taskbar.maximizedWindow")
            return maximizedWindowRule.expander;
        if (id == "taskbar.dynamic.shellUi" ||
            id == "taskbar.shellUi")
            return shellUiRule.expander;
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

    void CommitOpenColorEditors() noexcept
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
        CommitOpenColorEditors();
        CommitContinuousEdits();
        active = false;
        closed = true;
        confirmationGate->alive.store(false, std::memory_order_release);
        try
        {
            taskbarRoot.Loaded(taskbarRootLoadedToken);
            dockEnabledToggle.Toggled(dockEnabledToken);
            positionCombo.SelectionChanged(positionToken);
            layoutCombo.SelectionChanged(layoutToken);
            monitorScopeCombo.SelectionChanged(monitorScopeToken);
            floatingShortcutToggle.Toggled(floatingShortcutToken);
            floatingEdgeSwipeToggle.Toggled(floatingEdgeSwipeToken);
            showWindowsButtonToggle.Toggled(showWindowsButtonToken);
            showFrequentItemsToggle.Toggled(showFrequentItemsToken);
            keepWhenDesktopHiddenToggle.Toggled(keepWhenDesktopHiddenToken);
            allowDesktopContentOverlapToggle.Toggled(
                allowDesktopContentOverlapToken);
            showOnlyWhenSummonedToggle.Toggled(showOnlyWhenSummonedToken);
            taskbarAutoHideToggle.Toggled(taskbarAutoHideToken);
            taskbarAlignmentChoices.SelectionChanged(taskbarAlignmentToken);
            windowsSystemThemeChoices.SelectionChanged(
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

void DockPagePresenter::ActivateTaskbar() noexcept
{
    if (!impl_ || impl_->closed)
        return;
    impl_->taskbarInputReady = false;
    impl_->active = true;
    impl_->RefreshTaskbarRuntimeState();
}

void DockPagePresenter::Deactivate() noexcept
{
    if (!impl_ || impl_->closed)
        return;
    impl_->CommitOpenColorEditors();
    impl_->CommitContinuousEdits();
    impl_->taskbarInputReady = false;
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

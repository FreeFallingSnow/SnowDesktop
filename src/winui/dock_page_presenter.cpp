#include "pch.h"

#include "dock_page_presenter.h"
#include "settings_presenter_controls.h"
#include "appearance_sections.h"
#include "panel_gradient_editor.h"
#include "panel_appearance_editor.h"
#include "merged_bar_height_editor.h"
#include "dock_tray_settings_editor.h"
#include "icon_library_page_presenter.h"

#include "dock/dock_settings.h"
#include "theme/taskbar_appearance.h"
#include "theme_library_controls.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>
#include <filesystem>
#include <shobjidl.h>

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
    muxc::Border highlight{nullptr};
    muxc::StackPanel content{nullptr};
    muxc::TextBlock title{nullptr};
    muxc::Expander section{nullptr};
};

enum class ContinuousField
{
    ThicknessScale,
    EdgeDistanceScale,
    MergedIconScale,
    FrequentItemCount,
    EdgeHoverDelayMs,
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

struct TaskbarGradientControl
{
    AppearanceSections sections;
    std::shared_ptr<PanelGradientEditor> editor;
    bool enabled = false;
    std::shared_ptr<PanelAppearanceEditor> borderEditor;
};

struct DynamicRuleControl
{
    muxc::StackPanel root{nullptr};
    muxc::TextBlock detailTitle{nullptr};
    muxc::TextBlock summary{nullptr};
    muxc::ToggleSwitch enabled{nullptr};
    muxc::ComboBox theme{nullptr};
    muxc::ComboBox contentTheme{nullptr};
    muxc::StackPanel details{nullptr};
    muxc::StackPanel appearanceDetails{nullptr};
    muxc::StackPanel themeRows;
    muxc::StackPanel customAppearance{nullptr};
    SettingRow enabledRow;
    SettingRow themeRow;
    SettingRow contentThemeRow;
    TaskbarGradientControl gradient;
    ColorControl backgroundColor;
    ColorControl borderColor;
    ContinuousControl backgroundAlpha;
    ContinuousControl borderAlpha;
    muxc::ToggleSwitch glass{nullptr};
    SettingRow glassRow;
    ContinuousControl blurRadius;
    muxc::ToggleSwitch acrylic{nullptr};
    SettingRow acrylicRow;
    std::unique_ptr<ThemeLibraryControls> savedThemes;
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
    std::atomic_bool active{false};
    std::atomic_bool confirmationPending{false};
    std::atomic<std::uint64_t> generation{0};
};

void InitializeCard(
    SettingsCard& card,
    const mux::Style& style,
    const muxc::StackPanel& page,
    bool collapsible = false)
{
    card.root = muxc::Border{};
    card.content = muxc::StackPanel{};
    card.content.Spacing(2.0);
    card.title = muxc::TextBlock{};
    card.title.FontWeight(
        winrt::Windows::UI::Text::FontWeights::SemiBold());
    card.title.TextWrapping(mux::TextWrapping::Wrap);
    card.title.Margin({0, 0, 0, collapsible ? 0.0 : 8.0});
    if (collapsible)
    {
        card.section = muxc::Expander{};
        card.section.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        card.section.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        card.section.Padding({0, 0, 0, 0});
        card.section.IsExpanded(false);
        card.section.Header(card.title);
        card.section.Content(card.content);
        card.root.Child(card.section);
    }
    else
    {
        presenter_controls::InitializeSettingsGroup(card.root, card.title, card.content, style);
    }
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

void ApplyMainTaskbarTheme(DockSettings& settings, int selectedIndex, const PersonalizationSettings& global)
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
        SelectTaskbarCustomAppearance(settings, global);
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
        runtimeTimer = mux::DispatcherTimer{};
        runtimeTimer.Interval(std::chrono::seconds(1));
        runtimeTimerToken = runtimeTimer.Tick([this](const auto&, const auto&) {
            if (closed || !active) return;
            try { RefreshTaskbarRuntimeStatus(); } catch (...) {}
        });
        highlightTimer = mux::DispatcherTimer{};
        highlightTimer.Interval(std::chrono::milliseconds(1600));
        highlightTimerToken = highlightTimer.Tick([this](const auto&, const auto&) { StopCardHighlights(); });
    }

    mux::DispatcherTimer runtimeTimer{nullptr};
    winrt::event_token runtimeTimerToken{};
    mux::DispatcherTimer highlightTimer{nullptr};
    winrt::event_token highlightTimerToken{};
    mux::Media::Animation::Storyboard cardHighlightAnimation{nullptr};
    LocalizeCallback localize;
    DockPageActions actions;
    std::unique_ptr<MergedBarHeightEditor> mergedHeight;
    std::unique_ptr<DockTraySettingsEditor> trayEditor;
    BarSettingsAvailability barAvailability;
    muxc::ToggleSwitch homeSize;
    presenter_controls::SettingRow homeSizeRow;
    muxc::ComboBox taskbarMode;
    winrt::event_token homeSizeToken{}, taskbarModeToken{};
    mux::Style cardStyle{nullptr};
    muxc::StackPanel root{nullptr};
    muxc::StackPanel dockEnableRoot{nullptr};
    muxc::StackPanel dockAppearanceRoot{nullptr};
    muxc::ContentControl appearanceHost, animationHost, shortcutsHost;
    muxc::StackPanel dockRoot{nullptr};
    muxc::StackPanel taskbarRoot{nullptr};

    SettingsCard enableCard;
    SettingsCard edgeSwipeCard;
    SettingsCard fullscreenCard;
    SettingsCard layoutCard;
    SettingsCard mergedCard;
    SettingsCard behaviorCard;
    SettingsCard appearanceCard;
    SettingsCard animationCard;
    SettingsCard itemsCard;
    SettingsCard taskbarCard;
    SettingsCard taskbarAppearanceCard;
    bool centralizedAppearance = false;
    std::unique_ptr<ThemeLibraryControls> taskbarThemes;
    SettingsCard taskbarRulesCard;
    SettingsCard taskbarSystemPanelCard;

    muxc::ToggleSwitch dockEnabledToggle{nullptr};
    muxc::ComboBox positionCombo{nullptr};
    muxc::ComboBox layoutCombo{nullptr};
    muxc::ComboBox monitorScopeCombo{nullptr};
    muxc::ToggleSwitch floatingShortcutToggle{nullptr};
    muxc::ToggleSwitch floatingEdgeSwipeToggle{nullptr};
    muxc::ComboBox edgeRevealGesture{nullptr};
    muxc::ToggleSwitch windowPreviews{nullptr};
    muxc::ToggleSwitch singleClickLaunchItems{nullptr};
    muxc::ComboBox fullscreenPolicyCombo{nullptr};
    muxc::Button fullscreenExceptionAdd{nullptr};
    muxc::ComboBox fullscreenRunningApps{nullptr};
    std::vector<DockFullscreenApplication> fullscreenRunningApplications;
    winrt::event_token fullscreenRunningOpenToken{}, fullscreenRunningSelectToken{};
    muxc::StackPanel fullscreenExceptionList{nullptr};
    muxc::ContentControl fullscreenExceptionListHost{nullptr};
    muxc::Expander fullscreenExceptionsExpander{nullptr};
    SettingRow fullscreenExceptionsRow;
    muxc::TextBlock fullscreenExceptionsManagementTitle{nullptr};
    winrt::event_token fullscreenExceptionAddToken{};
    struct FullscreenExceptionRow
    {
        SettingRow row;
        muxc::ComboBox policy{nullptr};
        muxc::Button remove{nullptr};
        winrt::event_token policyToken{}, removeToken{};
    };
    std::vector<FullscreenExceptionRow> fullscreenExceptionRows;
    std::vector<DockFullscreenException> fullscreenExceptionValues;
    muxc::Button startIconChoose, startIconRestore, searchIconChoose, searchIconRestore, startIconBeautify, searchIconBeautify;
    SettingRow startIconRow, searchIconRow;
    muxc::InfoBar iconStatus;
    std::function<void()> cancelIconPicker;
    std::shared_ptr<bool> iconPickerAlive = std::make_shared<bool>(true);
    void ChooseControlIcon(std::wstring target)
    {
        if (!active || closed || !actions.iconLibrary) return;
        IconLibraryRequest begin; begin.action = "begin"; begin.target = target;
        auto snapshot = actions.iconLibrary(begin);
        if (!snapshot.succeeded) return;
        const auto action = actions.iconLibrary;
        cancelIconPicker = IconLibraryPagePresenter::Pick(startIconRow.root.XamlRoot(), localize,
            action, {}, [this, alive = std::weak_ptr<bool>(iconPickerAlive), action, target, session = snapshot.session](std::string id) {
                const auto guard = alive.lock();
                if (!guard || !*guard || !active || closed) id.clear();
                IconLibraryRequest request; request.action = id.empty() ? "cancel" : "apply";
                request.target = target; request.session = session;
                if (!id.empty()) request.ids.push_back(std::move(id));
                const auto result = action(std::move(request));
                if (!guard || !*guard) return;
                // An unlocalized error key must not open an empty banner.
                const auto message = L(result.error.c_str(), L"");
                iconStatus.Message(message);
                iconStatus.IsOpen(!message.empty());
            }, snapshot.targetName);
    }
    void RestoreControlIcon(const std::wstring& target)
    {
        if (!active || closed || !actions.iconLibrary) return;
        IconLibraryRequest request; request.action = "restore"; request.target = target;
        const auto result = actions.iconLibrary(std::move(request));
        const auto message = L(result.error.c_str(), L"");
        iconStatus.Message(message);
        iconStatus.IsOpen(!message.empty());
    }
    muxc::ToggleSwitch showWindowsButtonToggle{nullptr};
    muxc::ToggleSwitch startMenuKeepTaskbarHiddenToggle{nullptr};
    muxc::ToggleSwitch showQuickSearchButtonToggle{nullptr};
    muxc::ToggleSwitch suppressTaskbarToggle{nullptr};
    muxc::ToggleSwitch taskbarSuppressToggle{nullptr};
    muxc::ToggleSwitch reserveScreenSpaceToggle{nullptr};
    muxc::InfoBar suppressionStatus{nullptr};
    muxc::ToggleSwitch showFrequentItemsToggle{nullptr};
    muxc::ToggleSwitch keepWhenDesktopHiddenToggle{nullptr};
    muxc::ToggleSwitch allowDesktopContentOverlapToggle{nullptr};
    muxc::ToggleSwitch showOnlyWhenSummonedToggle{nullptr};
    muxc::ToggleSwitch summonOnAppMinimizeToggle{nullptr};
    ContinuousControl thicknessScale;
    ContinuousControl edgeDistanceScale;
    ContinuousControl mergedIconScale;
    ContinuousControl frequentItemCount;
    ContinuousControl edgeHoverDelayMs;

    muxc::Button taskbarSettingsLink{nullptr};
    muxc::Button restartExplorerButton{nullptr};
    muxc::ComboBox taskbarThemeCombo{nullptr};
    muxc::ComboBox taskbarContentThemeCombo{nullptr};
    muxc::StackPanel taskbarCustomAppearance{nullptr};
    muxc::StackPanel taskbarBaseRows;
    muxc::TextBlock taskbarCustomTitle{nullptr};
    muxc::TextBlock taskbarRulesHint{nullptr};
    TaskbarGradientControl taskbarGradient;
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
    SettingRow edgeRevealGestureRow, windowPreviewsRow;
    SettingRow singleClickLaunchItemsRow;
    SettingRow fullscreenPolicyRow;
    SettingRow showWindowsButtonRow;
    SettingRow startMenuKeepTaskbarHiddenRow;
    SettingRow showQuickSearchButtonRow;
    SettingRow suppressTaskbarRow;
    SettingRow taskbarSuppressRow;
    SettingRow reserveScreenSpaceRow;
    SettingRow showFrequentItemsRow;
    SettingRow allowDesktopContentOverlapRow;
    SettingRow showOnlyWhenSummonedRow;
    SettingRow summonOnAppMinimizeRow;
    SettingRow taskbarSettingsRow;
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
    std::array<ContinuousControl*, 8> continuousControls = {
        &thicknessScale,
        &edgeDistanceScale,
        &mergedIconScale,
        &frequentItemCount,
        &edgeHoverDelayMs,
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
    PersonalizationSettings globalAppearance;
    bool hasSnapshot = false;
    bool updatingControls = false;
    bool synchronizingPair = false;
    bool taskbarContentThemeCustomItems = false;
    bool taskbarHookRequired = false;
    bool taskbarInputReady = false;
    bool spaceReservedByStatusBar = false;
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
    winrt::event_token edgeRevealGestureToken{}, windowPreviewsToken{};
    winrt::event_token singleClickLaunchItemsToken{};
    winrt::event_token fullscreenPolicyToken{};
    winrt::event_token showWindowsButtonToken{};
    winrt::event_token startMenuKeepTaskbarHiddenToken{};
    winrt::event_token showQuickSearchButtonToken{};
    winrt::event_token suppressTaskbarToken{};
    winrt::event_token taskbarSuppressToken{};
    winrt::event_token reserveScreenSpaceToken{};
    winrt::event_token showFrequentItemsToken{};
    winrt::event_token keepWhenDesktopHiddenToken{};
    winrt::event_token allowDesktopContentOverlapToken{};
    winrt::event_token showOnlyWhenSummonedToken{};
    winrt::event_token summonOnAppMinimizeToken{};
    winrt::event_token taskbarSettingsToken{};
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
        dockEnableRoot.Spacing(24.0);
        dockAppearanceRoot = muxc::StackPanel{};
        dockAppearanceRoot.Spacing(24.0);
        InitializeCard(appearanceCard, nullptr, dockAppearanceRoot);
        InitializeCard(animationCard, cardStyle, dockAppearanceRoot);
        for (auto const& host : {appearanceHost, animationHost, shortcutsHost})
            host.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        appearanceCard.content.Children().Append(appearanceHost);
        animationCard.content.Children().Append(animationHost);
        dockRoot = muxc::StackPanel{};
        dockRoot.Spacing(24.0);
        taskbarRoot = muxc::StackPanel{};
        taskbarRoot.Spacing(24.0);
        root = dockRoot;

        InitializeCard(enableCard, cardStyle, dockEnableRoot);
        enableCard.title.Visibility(mux::Visibility::Collapsed);
        dockEnabledToggle = muxc::ToggleSwitch{};
        dockEnabledToggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
        dockEnabledRow.Initialize(dockEnabledToggle);
        dockEnabledRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        enableCard.content.Children().Append(dockEnabledRow.root);

        presenter_controls::UpdateSettingsSubrows(enableCard.content, 0);
        InitializeCard(edgeSwipeCard, cardStyle, dockRoot);
        InitializeCard(layoutCard, cardStyle, dockRoot);
        InitializeCard(mergedCard, cardStyle, dockRoot, true);
        InitializeCard(behaviorCard, cardStyle, dockRoot);
        InitializeCard(itemsCard, cardStyle, dockRoot);
        InitializeCard(fullscreenCard, cardStyle, dockRoot);
        positionCombo = NewCombo();
        layoutCombo = NewCombo();
        monitorScopeCombo = NewCombo();
        positionRow.Initialize(positionCombo);
        layoutRow.Initialize(layoutCombo);
        monitorScopeRow.Initialize(monitorScopeCombo);
        InitializeContinuousControl(thicknessScale,
            ContinuousField::ThicknessScale, 50.0, kDockMaximumScale * 100.0, 1.0,
            nullptr, 100.0);
        layoutCard.content.Children().Append(positionRow.root);
        layoutCard.content.Children().Append(monitorScopeRow.root);
        layoutCard.content.Children().Append(layoutRow.root);
        InitializeContinuousControl(edgeDistanceScale,
            ContinuousField::EdgeDistanceScale,
            kDockMinimumEdgeDistanceScale * 100.0f,
            kDockMaximumEdgeDistanceScale * 100.0, 1.0, nullptr, 100.0);
        layoutCard.content.Children().Append(edgeDistanceScale.root);
        layoutCard.content.Children().Append(thicknessScale.root);
        mergedHeight = std::make_unique<MergedBarHeightEditor>(localize);
        mergedCard.content.Children().Append(mergedHeight->Content());
        InitializeContinuousControl(mergedIconScale,
            ContinuousField::MergedIconScale, 50.0, 100.0, 1.0,
            nullptr, 100.0);
        mergedCard.content.Children().Append(mergedIconScale.root);
        homeSizeRow.Initialize(homeSize);
        homeSizeRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        layoutCard.content.Children().Append(homeSizeRow.root);
        homeSizeToken = homeSize.Toggled([this](const auto&, const auto&) {
            const bool enabled = homeSize.IsOn();
            EmitDock(SettingsUpdateMode::PreviewAndCommit, [enabled](DockSettings& value) { value.lastMonitorUseHomeSize = enabled; });
        });

        floatingShortcutToggle = muxc::ToggleSwitch{};
        floatingEdgeSwipeToggle = muxc::ToggleSwitch{};
        edgeRevealGesture = NewCombo();
        windowPreviews = muxc::ToggleSwitch{};
        singleClickLaunchItems = muxc::ToggleSwitch{};
        fullscreenPolicyCombo = NewCombo();
        showWindowsButtonToggle = muxc::ToggleSwitch{};
        startMenuKeepTaskbarHiddenToggle = muxc::ToggleSwitch{};
        showQuickSearchButtonToggle = muxc::ToggleSwitch{};
        suppressTaskbarToggle = muxc::ToggleSwitch{};
        taskbarSuppressToggle = muxc::ToggleSwitch{};
        reserveScreenSpaceToggle = muxc::ToggleSwitch{};
        showFrequentItemsToggle = muxc::ToggleSwitch{};
        keepWhenDesktopHiddenToggle = muxc::ToggleSwitch{};
        allowDesktopContentOverlapToggle = muxc::ToggleSwitch{};
        showOnlyWhenSummonedToggle = muxc::ToggleSwitch{};
        summonOnAppMinimizeToggle = muxc::ToggleSwitch{};
        for (const auto& toggle : {
                 floatingShortcutToggle,
                 floatingEdgeSwipeToggle,
                 showWindowsButtonToggle,
                 showQuickSearchButtonToggle,
                 suppressTaskbarToggle,
                 showFrequentItemsToggle,
                 keepWhenDesktopHiddenToggle,
                 allowDesktopContentOverlapToggle,
                 showOnlyWhenSummonedToggle,
                 summonOnAppMinimizeToggle})
        {
            toggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
        }
        floatingEdgeSwipeRow.Initialize(floatingEdgeSwipeToggle);
        edgeRevealGestureRow.Initialize(edgeRevealGesture);
        windowPreviewsRow.Initialize(windowPreviews);
        windowPreviewsRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        singleClickLaunchItemsRow.Initialize(singleClickLaunchItems);
        singleClickLaunchItemsRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        fullscreenPolicyRow.Initialize(fullscreenPolicyCombo);
        fullscreenPolicyRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        muxc::Grid startActions, searchActions;
        for (const auto& panel : {startActions, searchActions})
        {
            panel.ColumnSpacing(8);
            panel.ColumnDefinitions().Append(muxc::ColumnDefinition{});
            panel.ColumnDefinitions().Append(muxc::ColumnDefinition{});
            muxc::ColumnDefinition iconColumn; iconColumn.Width(mux::GridLengthHelper::Auto()); panel.ColumnDefinitions().Append(iconColumn);
        }
        for (const auto& button : {startIconRestore, searchIconRestore}) muxc::Grid::SetColumn(button, 1);
        for (const auto& button : {startIconBeautify, searchIconBeautify})
        {
            muxc::Grid::SetColumn(button, 2);
            button.Width(32);
            button.Height(32);
            button.MinWidth(32);
            button.MinHeight(32);
            button.Padding({0, 0, 0, 0});
            button.HorizontalContentAlignment(mux::HorizontalAlignment::Center);
            button.VerticalContentAlignment(mux::VerticalAlignment::Center);
            button.VerticalAlignment(mux::VerticalAlignment::Center);
        }
        startActions.Children().Append(startIconChoose); startActions.Children().Append(startIconRestore);
        searchActions.Children().Append(searchIconChoose); searchActions.Children().Append(searchIconRestore);
        startActions.Children().Append(startIconBeautify); searchActions.Children().Append(searchIconBeautify);
        startIconRow.Initialize(startActions); searchIconRow.Initialize(searchActions);
        startIconBeautify.Click([this](const auto&, const auto&) { if (active && !closed && actions.editIconAppearance) actions.editIconAppearance(L"control:start"); });
        searchIconBeautify.Click([this](const auto&, const auto&) { if (active && !closed && actions.editIconAppearance) actions.editIconAppearance(L"control:search"); });
        startIconChoose.Click([this](const auto&, const auto&) { ChooseControlIcon(L"control:start"); });
        searchIconChoose.Click([this](const auto&, const auto&) { ChooseControlIcon(L"control:search"); });
        startIconRestore.Click([this](const auto&, const auto&) { RestoreControlIcon(L"control:start"); });
        searchIconRestore.Click([this](const auto&, const auto&) { RestoreControlIcon(L"control:search"); });
        iconStatus.IsOpen(false); iconStatus.Severity(muxc::InfoBarSeverity::Error);
        presenter_controls::BindInfoBarVisibility(iconStatus);
        showWindowsButtonRow.Initialize(showWindowsButtonToggle);
        startMenuKeepTaskbarHiddenRow.Initialize(startMenuKeepTaskbarHiddenToggle);
        startMenuKeepTaskbarHiddenRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        showQuickSearchButtonRow.Initialize(showQuickSearchButtonToggle);
        showQuickSearchButtonRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        suppressTaskbarRow.Initialize(suppressTaskbarToggle);
        suppressTaskbarRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        showFrequentItemsRow.Initialize(showFrequentItemsToggle);
        allowDesktopContentOverlapRow.Initialize(
            allowDesktopContentOverlapToggle);
        showOnlyWhenSummonedRow.Initialize(showOnlyWhenSummonedToggle);
        summonOnAppMinimizeRow.Initialize(summonOnAppMinimizeToggle);
        summonOnAppMinimizeRow.SetControlAlignment(mux::HorizontalAlignment::Right);
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
        fullscreenCard.content.Children().Append(fullscreenPolicyRow.root);
        fullscreenExceptionAdd = muxc::Button{};
        fullscreenExceptionAdd.HorizontalAlignment(mux::HorizontalAlignment::Right);
        fullscreenRunningApps = NewCombo();
        muxc::Grid exceptionActions{};
        muxc::ColumnDefinition runningColumn{}, browseColumn{};
        runningColumn.Width({1, mux::GridUnitType::Star});
        browseColumn.Width({1, mux::GridUnitType::Auto});
        exceptionActions.ColumnDefinitions().Append(runningColumn);
        exceptionActions.ColumnDefinitions().Append(browseColumn);
        exceptionActions.ColumnSpacing(8);
        exceptionActions.Children().Append(fullscreenRunningApps);
        muxc::Grid::SetColumn(fullscreenExceptionAdd, 1);
        exceptionActions.Children().Append(fullscreenExceptionAdd);
        exceptionActions.MaxWidth(520);
        exceptionActions.HorizontalAlignment(mux::HorizontalAlignment::Right);
        exceptionActions.VerticalAlignment(mux::VerticalAlignment::Center);
        fullscreenRunningApps.ClearValue(mux::FrameworkElement::WidthProperty());
        fullscreenRunningApps.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        fullscreenExceptionsRow.Initialize(exceptionActions, 420);
        presenter_controls::StyleSettingsSubrow(fullscreenExceptionsRow.root, true, 0);
        fullscreenExceptionsExpander = mux::Markup::XamlReader::Load(
            LR"(<Expander xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource SubtleFillColorTransparentBrush}" HorizontalAlignment="Stretch" HorizontalContentAlignment="Stretch" IsExpanded="False" />)")
            .as<muxc::Expander>();
        fullscreenExceptionsManagementTitle = muxc::TextBlock{};
        fullscreenExceptionsManagementTitle.TextWrapping(mux::TextWrapping::Wrap);
        fullscreenExceptionsExpander.Header(fullscreenExceptionsManagementTitle);
        fullscreenExceptionList = muxc::StackPanel{};
        fullscreenExceptionList.Spacing(0);
        fullscreenExceptionListHost = muxc::ContentControl{};
        fullscreenExceptionListHost.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        fullscreenExceptionListHost.Content(fullscreenExceptionList);
        fullscreenExceptionListHost.Visibility(mux::Visibility::Collapsed);
        muxc::StackPanel exceptionManagement;
        exceptionManagement.Spacing(0);
        exceptionManagement.Children().Append(fullscreenExceptionsRow.root);
        exceptionManagement.Children().Append(fullscreenExceptionListHost);
        fullscreenExceptionsExpander.Content(exceptionManagement);
        fullscreenExceptionsExpander.Padding({0});
        fullscreenCard.content.Children().Append(
            presenter_controls::SettingsDisclosureRow(fullscreenExceptionsExpander));
        edgeSwipeCard.content.Children().Append(showOnlyWhenSummonedRow.root);
        edgeSwipeCard.content.Children().Append(summonOnAppMinimizeRow.root);
        edgeSwipeCard.content.Children().Append(shortcutsHost);
        edgeSwipeCard.content.Children().Append(floatingEdgeSwipeRow.root);
        edgeSwipeCard.content.Children().Append(edgeRevealGestureRow.root);
        InitializeContinuousControl(edgeHoverDelayMs,
            ContinuousField::EdgeHoverDelayMs,
            snowdesktop::dock_settings_rules::kMinimumEdgeHoverDelayMs,
            snowdesktop::dock_settings_rules::kMaximumEdgeHoverDelayMs,
            50.0, nullptr, snowdesktop::dock_settings_rules::kDefaultEdgeHoverDelayMs);
        edgeSwipeCard.content.Children().Append(edgeHoverDelayMs.root);
        reserveScreenSpaceRow.Initialize(reserveScreenSpaceToggle);
        reserveScreenSpaceRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        behaviorCard.content.Children().Append(allowDesktopContentOverlapRow.root);
        behaviorCard.content.Children().Append(reserveScreenSpaceRow.root);

        suppressionStatus = muxc::InfoBar{};
        suppressionStatus.IsClosable(false);
        suppressionStatus.IsOpen(false);
        presenter_controls::BindInfoBarVisibility(suppressionStatus);
        behaviorCard.content.Children().Append(suppressionStatus);
        itemsCard.content.Children().Append(showWindowsButtonRow.root);
        itemsCard.content.Children().Append(startMenuKeepTaskbarHiddenRow.root);
        itemsCard.content.Children().Append(startIconRow.root);
        itemsCard.content.Children().Append(showQuickSearchButtonRow.root);
        itemsCard.content.Children().Append(searchIconRow.root);
        itemsCard.content.Children().Append(iconStatus);
        itemsCard.content.Children().Append(showFrequentItemsRow.root);
        itemsCard.content.Children().Append(frequentItemCount.root);
        itemsCard.content.Children().Append(windowPreviewsRow.root);
        itemsCard.content.Children().Append(singleClickLaunchItemsRow.root);
        trayEditor = std::make_unique<DockTraySettingsEditor>(
            [this](std::string_view key, std::wstring_view fallback) { return L(key, fallback); },
            [this](auto edit) { EmitDock(SettingsUpdateMode::PreviewAndCommit, std::move(edit)); },
            [this] {
                if (!actions.invokeHost || closed || !active) return std::vector<DockFullscreenApplication>{};
                SettingsHostActions::Request request;
                request.action = SettingsHostActions::Action::GetDockTrayApplications;
                const auto result = actions.invokeHost(generation, request);
                return result.Succeeded() ? result.dockApplications : std::vector<DockFullscreenApplication>{};
            });
        itemsCard.content.Children().Append(trayEditor->Content());
        for (auto* card : {&layoutCard, &mergedCard, &itemsCard, &behaviorCard, &edgeSwipeCard, &fullscreenCard})
        {
            auto overlay = mux::Markup::XamlReader::Load(
                LR"(<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" BorderBrush="{ThemeResource AccentTextFillColorPrimaryBrush}" BorderThickness="2" CornerRadius="8" IsHitTestVisible="False" Opacity="0" />)")
                .as<muxc::Border>();
            const auto padding = card->root.Padding();
            overlay.Margin({-padding.Left, -padding.Top, -padding.Right, -padding.Bottom});
            muxc::Grid layer;
            const auto content = card->root.Child();
            card->root.Child(nullptr);
            layer.Children().Append(content);
            layer.Children().Append(overlay);
            card->root.Child(layer);
            card->highlight = overlay;
        }
        // keepWhenDesktopHidden is a post-migration field and intentionally
        // remains outside the 1:1 legacy surface.

        InitializeCard(taskbarCard, cardStyle, taskbarRoot);
        taskbarSuppressRow.Initialize(taskbarMode);
        taskbarModeToken = taskbarMode.SelectionChanged([this](const auto&, const auto&) {
            if (updatingControls) return;
            const int mode = taskbarMode.SelectedIndex();
            if (mode < 0 || (mode == 2 && !dockEnabledToggle.IsOn())) return;
            EmitDock(SettingsUpdateMode::PreviewAndCommit, [mode](DockSettings& value) { SetTaskbarDisplayMode(value, mode); });
        });
        taskbarSuppressRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        taskbarCard.content.Children().Append(taskbarSuppressRow.root);
        taskbarSettingsLink = muxc::Button{};
        taskbarSettingsLink.UseSystemFocusVisuals(true);
        taskbarSettingsRow.Initialize(taskbarSettingsLink);
        taskbarSettingsRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        taskbarCard.content.Children().Append(taskbarSettingsRow.root);

        InitializeCard(taskbarAppearanceCard, cardStyle, taskbarRoot);
        taskbarAppearanceCard.content.Spacing(0);
        taskbarThemeCombo = NewCombo();
        taskbarThemes = std::make_unique<ThemeLibraryControls>(localize, "taskbar", false, taskbarThemeCombo, 10, 8);
        taskbarContentThemeCombo = NewCombo();
        taskbarThemeRow.Initialize(taskbarThemes->SelectionContent());
        taskbarContentThemeRow.Initialize(taskbarContentThemeCombo);
        taskbarBaseRows.Children().Append(taskbarThemeRow.root);
        taskbarBaseRows.Children().Append(taskbarContentThemeRow.root);
        taskbarRuntimeStatus = muxc::InfoBar{};
        taskbarRuntimeStatus.IsClosable(false);
        taskbarRuntimeStatus.IsOpen(false);
        presenter_controls::BindInfoBarVisibility(taskbarRuntimeStatus);
        taskbarAppearanceCard.content.Children().Append(taskbarBaseRows);
        taskbarAppearanceCard.content.Children().Append(taskbarThemes->Content());
        taskbarAppearanceCard.content.Children().Append(taskbarRuntimeStatus);

        taskbarCustomAppearance = muxc::StackPanel{};
        taskbarCustomAppearance.Spacing(0);
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
        InitializeTaskbarGradient(taskbarGradient, taskbarCustomAppearance);
        taskbarGradient.sections.colors.Children().Append(taskbarBackgroundColor.root);
        taskbarGradient.sections.colors.Children().Append(taskbarBackgroundAlpha.root);
        taskbarGradient.sections.material.Children().Append(taskbarGlassRow.root);
        taskbarGradient.sections.material.Children().Append(taskbarBlurRadius.root);
        taskbarGradient.sections.material.Children().Append(taskbarAcrylicRow.root);
        taskbarCustomAppearance.Children().Append(taskbarThemes->SaveContent());
        presenter_controls::JoinSettingsSections(taskbarCustomAppearance);
        taskbarThemes->SetCustomContent({taskbarCustomAppearance});
        taskbarAppearanceCard.content.Children().Append(
            taskbarCustomAppearance);

        InitializeCard(taskbarRulesCard, nullptr, taskbarAppearanceCard.content, true);
        taskbarRulesCard.root.Child(nullptr);
        taskbarRulesCard.root.Child(presenter_controls::SettingsDisclosureRow(taskbarRulesCard.section));
        taskbarRulesCard.content.Spacing(0.0);
        taskbarRulesHint = NewHint();
        InitializeDynamicRule(shellUiRule,
            &DockSettings::systemTaskbarShellUi);
        shellUiRule.enabledRow.text.Children().Append(taskbarRulesHint);
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
        windowsSystemThemeRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        restartExplorerRow.Initialize(restartExplorerButton);
        restartExplorerRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        taskbarSystemPanelCard.content.Children().Append(
            windowsSystemThemeRow.root);
        taskbarSystemPanelCard.content.Children().Append(
            restartExplorerRow.root);
        for (const auto* card : {&layoutCard, &mergedCard, &itemsCard,
                 &behaviorCard, &edgeSwipeCard, &fullscreenCard,
                 &taskbarCard, &taskbarSystemPanelCard})
            presenter_controls::UpdateSettingsSubrows(card->content, 0);
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
        choices.HorizontalAlignment(mux::HorizontalAlignment::Right);
        const int count = IsClassicSystemTaskbar() ? 3 : 2;
        choices.MaxColumns(count);
        // Keep application-owned RadioButton instances stable. Rebuilding a
        // string Items collection while the control is unloaded can leave
        // RadioButtons.SelectedIndex set but no generated container checked.
        for (int index = 0; index < count; ++index)
        {
            muxc::RadioButton choice{};
            choice.MinWidth(0.0);
            choices.Items().Append(choice);
        }
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
        control.number.Width(presenter_controls::kSettingNumberWidth);
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
        case ContinuousField::EdgeDistanceScale:
        case ContinuousField::MergedIconScale:
        case ContinuousField::TaskbarBackgroundAlpha:
        case ContinuousField::TaskbarBorderAlpha:
            SetUnit(control, L"%");
            break;
        case ContinuousField::TaskbarBlurRadius:
            SetUnit(control, L"px");
            break;
        case ContinuousField::EdgeHoverDelayMs:
            SetUnit(control, L"ms");
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

    void InitializeTaskbarGradient(TaskbarGradientControl& control,
        const muxc::StackPanel& parent,
        SystemTaskbarDynamicRule DockSettings::* member = nullptr)
    {
        control.sections.Initialize(parent, true, false, false);
        presenter_controls::JoinSettingsSections(parent);
        control.borderEditor = PanelAppearanceEditor::Create(
            [this](auto key) { return L(key, L""); },
            [this, member](const PersonalizationSettings& value, bool commit) {
                EmitDock(commit ? SettingsUpdateMode::PreviewAndCommit : SettingsUpdateMode::Preview,
                    [member, value](DockSettings& settings) {
                        auto& appearance = member ? (settings.*member).appearance : settings.systemTaskbarAppearance;
                        appearance.widgetBorderR = value.widgetBorderR;
                        appearance.widgetBorderG = value.widgetBorderG;
                        appearance.widgetBorderB = value.widgetBorderB;
                        appearance.widgetBorderAlpha = value.widgetBorderAlpha;
                        appearance.widgetBorderWidth = value.widgetBorderWidth;
                        appearance.widgetEdgeHighlightEnabled = value.widgetEdgeHighlightEnabled;
                        appearance.widgetEdgeHighlightWidth = value.widgetEdgeHighlightWidth;
                        appearance.widgetEdgeHighlightStrength = value.widgetEdgeHighlightStrength;
                        appearance.edgeLight = value.edgeLight;
                    });
            }, true);
        control.sections.border.Children().Append(control.borderEditor->Content());
        control.editor = PanelGradientEditor::Create(
            [this](auto key) { return L(key, L""); },
            [this, &control, member](const PanelGradient& gradient, bool commit) {
                if (!CanEmitDock()) return;
                control.enabled = gradient.enabled;
                EmitDock(commit ? SettingsUpdateMode::PreviewAndCommit : SettingsUpdateMode::Preview,
                    [member, gradient](DockSettings& settings) {
                        auto& appearance = member ? (settings.*member).appearance : settings.systemTaskbarAppearance;
                        appearance.backgroundPreset = kAppearancePresetCustom;
                        appearance.panelGradient = gradient;
                    });
                UpdateDependentStates();
            }, false, {}, true);
        control.sections.colors.Children().Append(control.editor->Content());
    }

    static void PatchGradient(TaskbarGradientControl& control, const PanelGradient& gradient, bool force = false)
    {
        control.editor->SetValue(gradient, force);
        control.enabled = control.editor->Value().enabled;
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

        control.detailTitle = muxc::TextBlock{};
        control.detailTitle.FontWeight(
            winrt::Windows::UI::Text::FontWeights::SemiBold());
        control.detailTitle.TextWrapping(mux::TextWrapping::Wrap);
        control.summary = NewHint();

        control.details = muxc::StackPanel{};
        control.details.Spacing(0.0);
        control.details.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        control.contentThemeRow.Initialize(control.contentTheme);
        // The always-visible scenario switch discloses its appearance editors.
        control.details.Children().Append(control.enabledRow.root);
        presenter_controls::StyleSettingsSubrow(control.enabledRow.root,
            member == &DockSettings::systemTaskbarShellUi, 0.0);
        control.appearanceDetails = muxc::StackPanel{};
        control.appearanceDetails.Spacing(0.0);
        control.appearanceDetails.Margin({16, 0, 0, 0});
        control.appearanceDetails.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        const std::string target = member == &DockSettings::systemTaskbarShellUi ? "taskbar/shellUi" :
            member == &DockSettings::systemTaskbarMaximizedWindow ? "taskbar/maximizedWindow" : "taskbar/visibleWindow";
        control.savedThemes = std::make_unique<ThemeLibraryControls>(localize, target, false, control.theme, 10, 8);
        control.savedThemes->SetInlineSurface();
        control.themeRow.Initialize(control.savedThemes->SelectionContent());
        control.themeRows.Children().Append(control.themeRow.root);
        control.themeRows.Children().Append(control.contentThemeRow.root);
        presenter_controls::UpdateSettingsSubrows(control.themeRows, 0.0);
        control.appearanceDetails.Children().Append(control.themeRows);
        control.appearanceDetails.Children().Append(control.savedThemes->Content());

        control.customAppearance = muxc::StackPanel{};
        control.customAppearance.Spacing(0.0);
        InitializeTaskbarGradient(control.gradient, control.customAppearance, member);
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
        control.gradient.sections.colors.Children().Append(control.backgroundColor.root);
        control.gradient.sections.colors.Children().Append(control.backgroundAlpha.root);
        control.gradient.sections.material.Children().Append(control.glassRow.root);
        control.gradient.sections.material.Children().Append(control.blurRadius.root);
        control.gradient.sections.material.Children().Append(control.acrylicRow.root);
        control.customAppearance.Children().Append(control.savedThemes->SaveContent());
        presenter_controls::JoinSettingsSections(control.customAppearance);
        control.savedThemes->SetCustomContent({control.customAppearance});
        control.appearanceDetails.Children().Append(control.customAppearance);
        control.details.Children().Append(control.appearanceDetails);
        control.root.Children().Append(control.details);
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

    void StopCardHighlights() noexcept
    {
        try
        {
            if (highlightTimer) highlightTimer.Stop();
            if (cardHighlightAnimation) cardHighlightAnimation.Stop();
            cardHighlightAnimation = nullptr;
            for (auto* card : {&layoutCard, &mergedCard, &itemsCard, &behaviorCard, &edgeSwipeCard, &fullscreenCard})
                if (card->highlight) card->highlight.Opacity(0);
        }
        catch (...) {}
    }

    void HighlightEnabledDockCards()
    {
        StopCardHighlights();
        if (closed || !active || !dockRoot.IsLoaded()) return;
        namespace animation = mux::Media::Animation;
        const bool animate = winrt::Windows::UI::ViewManagement::UISettings{}.AnimationsEnabled();
        cardHighlightAnimation = animation::Storyboard{};
        for (auto* card : {&layoutCard, &mergedCard, &itemsCard, &behaviorCard, &edgeSwipeCard, &fullscreenCard})
        {
            if (card->root.Visibility() != mux::Visibility::Visible) continue;
            if (!animate) { card->highlight.Opacity(1); continue; }
            animation::DoubleAnimation pulse;
            pulse.From(0.0);
            pulse.To(1.0);
            pulse.Duration(mux::DurationHelper::FromTimeSpan(std::chrono::milliseconds(350)));
            pulse.AutoReverse(true);
            pulse.RepeatBehavior(animation::RepeatBehaviorHelper::FromCount(2));
            pulse.FillBehavior(animation::FillBehavior::Stop);
            animation::Storyboard::SetTarget(pulse, card->highlight);
            animation::Storyboard::SetTargetProperty(pulse, L"Opacity");
            cardHighlightAnimation.Children().Append(pulse);
        }
        if (animate) cardHighlightAnimation.Begin();
        highlightTimer.Start();
    }

    void HookEvents()
    {
        // The shared appearance surface can also be mounted on the themes
        // page, where taskbarRoot itself is not in the visual tree.
        taskbarRootLoadedToken = taskbarAppearanceCard.root.Loaded(
            [this](const auto&, const auto&) {
                if (closed)
                    return;
                RefreshTaskbarEntryState();
                taskbarInputReady = true;
            });
        dockEnabledToken = dockEnabledToggle.Toggled(
            [this](const auto&, const auto&) {
                if (!CanEmitGeneral()) return;
                const bool value = dockEnabledToggle.IsOn();
                if (!value)
                {
                    updatingControls = true;
                    dockEnabledToggle.IsOn(true);
                    updatingControls = false;
                    UpdateDependentStates();
                    ConfirmDisableDock();
                    return;
                }
                EmitGeneral(SettingsUpdateMode::PreviewAndCommit,
                    [value](GeneralSettings& settings) {
                        settings.dockEnabled = value;
                    });
                UpdateDependentStates();
                HighlightEnabledDockCards();
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
                fullscreenPolicyRow.SetEnabled(
                    dockEnabledToggle.IsOn());
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
        edgeRevealGestureToken = edgeRevealGesture.SelectionChanged([this](const auto&, const auto&) {
            UpdateDependentStates();
            const int value = edgeRevealGesture.SelectedIndex();
            if (value >= 0) EmitDock(SettingsUpdateMode::PreviewAndCommit,
                [value](DockSettings& settings) { settings.edgeRevealGesture = value; });
        });
        windowPreviewsToken = windowPreviews.Toggled([this](const auto&, const auto&) {
            const bool value = windowPreviews.IsOn();
            EmitDock(SettingsUpdateMode::PreviewAndCommit,
                [value](DockSettings& settings) { settings.showWindowPreviews = value; });
        });
        singleClickLaunchItemsToken = singleClickLaunchItems.Toggled([this](const auto&, const auto&) {
            const bool value = singleClickLaunchItems.IsOn();
            EmitDock(SettingsUpdateMode::PreviewAndCommit,
                [value](DockSettings& settings) { settings.singleClickLaunchItems = value; });
        });
        fullscreenPolicyToken = fullscreenPolicyCombo.SelectionChanged(
            [this](const auto&, const auto&) {
                const int value = fullscreenPolicyCombo.SelectedIndex();
                if (value < 0) return;
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [value](DockSettings& settings) {
                        settings.fullscreenPolicy = snowdesktop::dock_fullscreen::Normalize(value);
                    });
            });
        fullscreenExceptionAddToken = fullscreenExceptionAdd.Click([this](const auto&, const auto&) {
            PickFullscreenException();
        });
        fullscreenRunningOpenToken = fullscreenRunningApps.DropDownOpened([this](const auto&, const auto&) {
            if (!actions.invokeHost || closed || !active) return;
            SettingsHostActions::Request request;
            request.action = SettingsHostActions::Action::GetDockRunningApplications;
            const auto result = actions.invokeHost(generation, request);
            if (!result.Succeeded()) return;
            fullscreenRunningApplications = result.dockApplications;
            const bool previous = updatingControls;
            updatingControls = true;
            fullscreenRunningApps.Items().Clear();
            fullscreenRunningApps.Items().Append(winrt::box_value(L("settings.dock.fullscreenFromRunning", L"Choose from running apps")));
            for (const auto& application : fullscreenRunningApplications)
                fullscreenRunningApps.Items().Append(winrt::box_value(application.name + L" — " + application.executable));
            fullscreenRunningApps.SelectedIndex(0);
            updatingControls = previous;
        });
        fullscreenRunningSelectToken = fullscreenRunningApps.SelectionChanged([this](const auto&, const auto&) {
            if (updatingControls) return;
            const int selected = fullscreenRunningApps.SelectedIndex();
            if (selected <= 0 || static_cast<size_t>(selected) > fullscreenRunningApplications.size()) return;
            const auto path = fullscreenRunningApplications[static_cast<size_t>(selected - 1)].executable;
            fullscreenRunningApps.SelectedIndex(0);
            AddFullscreenException(path);
        });
        suppressTaskbarToken = suppressTaskbarToggle.Toggled(
            [this](const auto&, const auto&) {
                if (updatingControls) return;
                const bool value = suppressTaskbarToggle.IsOn();
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [value](DockSettings& settings) { settings.suppressSystemTaskbar = value; });
            });
        taskbarSuppressToken = taskbarSuppressToggle.Toggled(
            [this](const auto&, const auto&) {
                if (updatingControls) return;
                const bool value = taskbarSuppressToggle.IsOn();
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [value](DockSettings& settings) { settings.suppressSystemTaskbar = value; });
            });
        reserveScreenSpaceToken = reserveScreenSpaceToggle.Toggled(
            [this](const auto&, const auto&) {
                const bool value = reserveScreenSpaceToggle.IsOn();
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [value](DockSettings& settings) { settings.reserveScreenSpace = value; });
            });
        showWindowsButtonToken = showWindowsButtonToggle.Toggled(
            [this](const auto&, const auto&) {
                UpdateDependentStates();
                const bool value = showWindowsButtonToggle.IsOn();
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [value](DockSettings& settings) {
                        settings.showWindowsButton = value;
                    });
            });
        startMenuKeepTaskbarHiddenToken = startMenuKeepTaskbarHiddenToggle.Toggled(
            [this](const auto&, const auto&) {
                const bool value = startMenuKeepTaskbarHiddenToggle.IsOn();
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [value](DockSettings& settings) { settings.startMenuKeepTaskbarHidden = value; });
            });
        showQuickSearchButtonToken = showQuickSearchButtonToggle.Toggled(
            [this](const auto&, const auto&) {
                const bool value = showQuickSearchButtonToggle.IsOn();
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [value](DockSettings& settings) {
                        settings.showQuickSearchButton = value;
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
                            settings.allowDesktopContentOverlap = !value;
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
                    });
            });
        summonOnAppMinimizeToken = summonOnAppMinimizeToggle.Toggled(
            [this](const auto&, const auto&) {
                const bool value = summonOnAppMinimizeToggle.IsOn();
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [value](DockSettings& settings) { settings.summonOnAppMinimize = value; });
            });
        windowsSystemThemeToken = windowsSystemThemeChoices.SelectionChanged(
            [this](const auto&, const auto&) {
                if (!taskbarInputReady || closed || updatingControls ||
                    !active || !hasSnapshot)
                    return;
                const int value = windowsSystemThemeChoices.SelectedIndex();
                if (value >= 0)
                {
                    if (IsClassicSystemTaskbar())
                    {
                        SetClassicSystemTheme(value);
                        return;
                    }
                    windowsSystemThemeValue = std::clamp(value, 0, 1);
                    (void)RequestWindowsSystemLightThemeEnabled(value == 0);
                    RefreshTaskbarEntryState();
                }
            });
        taskbarThemeToken = taskbarThemeCombo.SelectionChanged(
            [this](const auto&, const auto&) {
                if (updatingControls) return;
                if (taskbarThemes->ApplySelection()) return;
                const int value = taskbarThemeCombo.SelectedIndex();
                if (value < 0) return;
                CommitContinuousEdits();
                CommitOpenColorEditors();
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
                    [value, global = globalAppearance](DockSettings& settings) {
                        ApplyMainTaskbarTheme(settings, value, global);
                    });
            });
        taskbarContentThemeToken = taskbarContentThemeCombo.SelectionChanged(
            [this](const auto&, const auto&) {
                const int value = taskbarContentThemeCombo.SelectedIndex();
                if (value < 0) return;
                if (IsClassicSystemTaskbar())
                {
                    if (!taskbarInputReady || closed || updatingControls ||
                        !active || !hasSnapshot) return;
                    SetClassicSystemTheme(value);
                    return;
                }
                const bool custom = taskbarThemes->NativeSelection() ==
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
        taskbarSettingsToken = taskbarSettingsLink.Click(
            [this](const auto&, const auto&) {
                if (!closed && active && hasSnapshot && actions.openTaskbarSettings)
                    actions.openTaskbarSettings(generation);
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
                presenter_controls::SetNumberBoxValue(control.number, value);
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
                if (updatingControls) return;
                if (control.savedThemes->ApplySelection()) return;
                const int value = control.theme.SelectedIndex();
                if (value < 0) return;
                CommitContinuousEdits();
                CommitOpenColorEditors();
                const auto member = control.member;
                EmitDock(SettingsUpdateMode::PreviewAndCommit,
                    [member, value, global = globalAppearance](DockSettings& settings) {
                        SelectTaskbarRuleTheme(settings.*member,
                            static_cast<SystemTaskbarThemeMode>(std::clamp(
                                value, 0, 9)), global);
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
        case ContinuousField::EdgeDistanceScale:
            return ClampDockEdgeDistanceScale(settings.edgeDistanceScale) * 100.0;
        case ContinuousField::MergedIconScale:
            return ClampMergedDockIconScale(settings.mergedIconScale) * 100.0;
        case ContinuousField::FrequentItemCount:
            return std::clamp(settings.frequentItemCount, 1, 8);
        case ContinuousField::EdgeHoverDelayMs:
            return settings.edgeHoverDelayMs;
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
        case ContinuousField::EdgeDistanceScale:
            settings.edgeDistanceScale = ClampDockEdgeDistanceScale(
                static_cast<float>(uiValue / 100.0));
            break;
        case ContinuousField::MergedIconScale:
            settings.mergedIconScale = ClampMergedDockIconScale(
                static_cast<float>(uiValue / 100.0));
            break;
        case ContinuousField::FrequentItemCount:
            settings.frequentItemCount = std::clamp(
                static_cast<int>(std::lround(uiValue)), 1, 8);
            break;
        case ContinuousField::EdgeHoverDelayMs:
            settings.edgeHoverDelayMs = snowdesktop::dock_settings_rules::NormalizeEdgeHoverDelayMs(uiValue);
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

    void ConfirmDisableDock()
    {
        if (!CanEmitGeneral() || !actions.confirm ||
            confirmationGate->confirmationPending.exchange(true)) return;
        const auto expectedGeneration = generation;
        const std::weak_ptr<ConfirmationGate> weak = confirmationGate;
        const auto update = actions.updateGeneral;
        actions.confirm(expectedGeneration,
            L("settings.dock.disableConfirm.title", L"Turn off Dock?"),
            L("settings.dock.disableConfirm.description", L"Dock items will move back to the desktop."),
            [weak, update, expectedGeneration](bool confirmed) {
                const auto gate = weak.lock();
                if (!gate) return;
                if (gate->generation != expectedGeneration) return;
                gate->confirmationPending = false;
                if (!confirmed || !gate->alive || !gate->active ||
                    gate->generation != expectedGeneration) return;
                update(expectedGeneration, SettingsUpdateMode::PreviewAndCommit,
                    [](GeneralSettings& settings) { settings.dockEnabled = false; });
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
        const double value = QuantizeNumericValue(
            ReadContinuous(control, settings),
            control.slider.Minimum(), control.slider.Maximum(),
            control.slider.StepFrequency());
        control.slider.Value(value);
        presenter_controls::SyncNumberBoxValue(control.number, value);
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
        PatchGradient(control.gradient, rule.appearance.panelGradient);
        control.gradient.borderEditor->SetValue(rule.appearance);
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
        PatchGradient(taskbarGradient, settings.systemTaskbarAppearance.panelGradient);
        taskbarGradient.borderEditor->SetValue(settings.systemTaskbarAppearance);
        positionCombo.SelectedIndex(std::clamp(
            static_cast<int>(settings.position), 0, 3));
        layoutCombo.SelectedIndex(settings.edgeAttached ? 1 : 0);
        monitorScopeCombo.SelectedIndex(std::clamp(
            static_cast<int>(settings.monitorScope), 0, 2));
        edgeRevealGesture.SelectedIndex(settings.edgeRevealGesture);
        windowPreviews.IsOn(settings.showWindowPreviews);
        singleClickLaunchItems.IsOn(settings.singleClickLaunchItems);
        floatingShortcutToggle.IsOn(settings.floatingShortcutMode);
        fullscreenPolicyCombo.SelectedIndex(static_cast<int>(settings.fullscreenPolicy));
        trayEditor->Update(settings);
        if (fullscreenExceptionValues != settings.fullscreenExceptions)
        {
            fullscreenExceptionValues = settings.fullscreenExceptions;
            RebuildFullscreenExceptions();
        }
        floatingEdgeSwipeToggle.IsOn(
            snowdesktop::dock_settings_rules::
                IsFloatingEdgeSwipeEnabled(
                    settings.showOnlyWhenSummoned,
                    settings.floatingEdgeSwipeEnabled));
        suppressTaskbarToggle.IsOn(settings.suppressSystemTaskbar);
        taskbarSuppressToggle.IsOn(settings.suppressSystemTaskbar);
        taskbarMode.SelectedIndex(TaskbarDisplayMode(settings));
        homeSize.IsOn(settings.lastMonitorUseHomeSize);
        reserveScreenSpaceToggle.IsOn(settings.reserveScreenSpace);
        showWindowsButtonToggle.IsOn(ShowDockWindowsButton(settings));
        startMenuKeepTaskbarHiddenToggle.IsOn(settings.startMenuKeepTaskbarHidden);
        showQuickSearchButtonToggle.IsOn(settings.showQuickSearchButton);
        showFrequentItemsToggle.IsOn(settings.showFrequentItems);
        keepWhenDesktopHiddenToggle.IsOn(settings.keepWhenDesktopHidden);
        allowDesktopContentOverlapToggle.IsOn(
            snowdesktop::dock_settings_rules::
                ShouldReserveDesktopWorkArea(
                    settings.showOnlyWhenSummoned,
                    settings.allowDesktopContentOverlap));
        showOnlyWhenSummonedToggle.IsOn(settings.showOnlyWhenSummoned);
        summonOnAppMinimizeToggle.IsOn(settings.summonOnAppMinimize);
        windowsSystemThemeValue = IsClassicSystemTaskbar()
            ? std::clamp(settings.classicTaskbarSystemTheme, -1, 1) + 1
            : (IsWindowsSystemLightThemeEnabled() ? 0 : 1);
        SelectChoice(windowsSystemThemeChoices, windowsSystemThemeValue);
        const int selectedTheme = MainTaskbarThemeMode(settings);
        taskbarThemeCombo.SelectedIndex(selectedTheme);
        taskbarContentThemeValue = std::clamp(
            settings.systemTaskbarContentTheme, -1, 1);
        taskbarAppearanceContentThemeValue = std::clamp(
            settings.systemTaskbarAppearance.contentTheme, 0, 1);
        const bool custom = selectedTheme ==
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
        const bool dockEnabled = dockEnabledToggle.IsOn();
        // Match the legacy BeginDisabled(!dockEnabled_) scope. Border and Grid
        // are FrameworkElements, not Controls, so disable each SettingRow's
        // ContentControl host to remove its descendants from keyboard/Tab
        // input. IsHitTestVisible on the card remains the pointer guard.
        floatingEdgeSwipeRow.SetEnabled(dockEnabled);
        fullscreenPolicyRow.SetEnabled(dockEnabled);
        fullscreenExceptionsRow.SetEnabled(dockEnabled);
        fullscreenExceptionsExpander.IsEnabled(dockEnabled);
        positionRow.SetEnabled(dockEnabled);
        monitorScopeRow.SetEnabled(dockEnabled);
        layoutRow.SetEnabled(dockEnabled);
        thicknessScale.row.SetEnabled(dockEnabled);
        edgeDistanceScale.root.Visibility(layoutCombo.SelectedIndex() == 0
            ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        edgeDistanceScale.row.SetEnabled(dockEnabled && layoutCombo.SelectedIndex() == 0);
        mergedIconScale.row.SetEnabled(dockEnabled && barAvailability.anyMerged);
        suppressTaskbarRow.SetEnabled(dockEnabled);
        taskbarSuppressRow.SetEnabled(true);
        if (taskbarMode.Items().Size() == 3)
            taskbarMode.Items().GetAt(2).as<muxc::ComboBoxItem>().IsEnabled(dockEnabled || taskbarSuppressToggle.IsOn());
        homeSizeRow.SetEnabled(dockEnabled && barAvailability.dockOnLastMonitor && !barAvailability.allDockMerged);
        reserveScreenSpaceRow.SetEnabled(dockEnabled &&
            !showOnlyWhenSummonedToggle.IsOn() && !spaceReservedByStatusBar);
        showWindowsButtonRow.SetEnabled(dockEnabled);
        startMenuKeepTaskbarHiddenRow.SetEnabled(dockEnabled && showWindowsButtonToggle.IsOn() &&
            taskbarMode.SelectedIndex() > 0);
        showQuickSearchButtonRow.SetEnabled(dockEnabled);
        showFrequentItemsRow.SetEnabled(dockEnabled);
        allowDesktopContentOverlapRow.SetEnabled(dockEnabled && !spaceReservedByStatusBar && !showOnlyWhenSummonedToggle.IsOn());
        floatingEdgeSwipeRow.SetEnabled(dockEnabled && !showOnlyWhenSummonedToggle.IsOn());
        edgeRevealGestureRow.SetEnabled(dockEnabled && floatingEdgeSwipeToggle.IsOn());
        edgeHoverDelayMs.root.Visibility(edgeRevealGesture.SelectedIndex() == 1
            ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        edgeHoverDelayMs.row.SetEnabled(dockEnabled && floatingEdgeSwipeToggle.IsOn());
        windowPreviewsRow.SetEnabled(dockEnabled);
        singleClickLaunchItemsRow.SetEnabled(dockEnabled);
        frequentItemCount.root.Visibility(showFrequentItemsToggle.IsOn() ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        showOnlyWhenSummonedRow.SetEnabled(dockEnabled);
        summonOnAppMinimizeRow.SetEnabled(dockEnabled);
        edgeSwipeCard.root.IsHitTestVisible(dockEnabled);
        fullscreenCard.root.IsHitTestVisible(dockEnabled);
        layoutCard.root.IsHitTestVisible(dockEnabled);
        mergedCard.root.IsHitTestVisible(dockEnabled);
        behaviorCard.root.IsHitTestVisible(dockEnabled);
        itemsCard.root.IsHitTestVisible(dockEnabled);
        edgeSwipeCard.root.Opacity(dockEnabled ? 1.0 : 0.62);
        fullscreenCard.root.Opacity(dockEnabled ? 1.0 : 0.62);
        layoutCard.root.Opacity(dockEnabled ? 1.0 : 0.62);
        mergedCard.root.Opacity(dockEnabled ? 1.0 : 0.62);
        behaviorCard.root.Opacity(dockEnabled ? 1.0 : 0.62);
        itemsCard.root.Opacity(dockEnabled ? 1.0 : 0.62);
        frequentItemCount.slider.IsEnabled(
            dockEnabled && showFrequentItemsToggle.IsOn());
        frequentItemCount.number.IsEnabled(
            dockEnabled && showFrequentItemsToggle.IsOn());
        frequentItemCount.row.SetEnabled(
            dockEnabled && showFrequentItemsToggle.IsOn());

        const int selectedTheme = taskbarThemes->NativeSelection();
        const bool taskbarStyled = selectedTheme !=
            static_cast<int>(SystemTaskbarThemeMode::Native);
        const bool taskbarCustom = taskbarThemes->CustomSelected() && selectedTheme ==
            static_cast<int>(SystemTaskbarThemeMode::Custom);
        taskbarContentThemeRow.root.Visibility(!taskbarThemes->HasSavedSelection() && (taskbarStyled || IsClassicSystemTaskbar())
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
        taskbarContentThemeRow.SetEnabled(taskbarStyled || IsClassicSystemTaskbar());
        presenter_controls::UpdateSettingsSubrows(taskbarBaseRows, 0);
        taskbarCustomAppearance.Visibility(taskbarCustom
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
        taskbarBackgroundColor.root.Visibility(taskbarGradient.enabled ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        taskbarBackgroundAlpha.root.Visibility(taskbarGradient.enabled ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        taskbarGradient.editor->Content().IsHitTestVisible(taskbarCustom);
        taskbarBlurRadius.root.Visibility(!IsClassicSystemTaskbar() && taskbarGlassToggle.IsOn() && !taskbarAcrylicToggle.IsOn() ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        taskbarAcrylicRow.root.Visibility(taskbarGlassToggle.IsOn() ? mux::Visibility::Visible : mux::Visibility::Collapsed);
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
            const bool native = control->savedThemes->NativeSelection() ==
                static_cast<int>(SystemTaskbarThemeMode::Native);
            const bool custom = control->savedThemes->CustomSelected() && control->savedThemes->NativeSelection() ==
                static_cast<int>(SystemTaskbarThemeMode::Custom);
            control->appearanceDetails.Visibility(enabled
                    ? mux::Visibility::Visible
                    : mux::Visibility::Collapsed);
            control->appearanceDetails.IsHitTestVisible(enabled);
            control->contentThemeRow.root.Visibility(!control->savedThemes->HasSavedSelection() && !native && !IsClassicSystemTaskbar()
                    ? mux::Visibility::Visible
                    : mux::Visibility::Collapsed);
            presenter_controls::UpdateSettingsSubrows(control->themeRows, 0);
            control->customAppearance.Visibility(custom
                    ? mux::Visibility::Visible
                    : mux::Visibility::Collapsed);
            control->themeRow.SetEnabled(enabled);
            control->contentThemeRow.SetEnabled(enabled && !native);
            control->backgroundColor.root.Visibility(control->gradient.enabled ? mux::Visibility::Collapsed : mux::Visibility::Visible);
            control->backgroundAlpha.root.Visibility(control->gradient.enabled ? mux::Visibility::Collapsed : mux::Visibility::Visible);
            control->gradient.editor->Content().IsHitTestVisible(enabled && custom);
            control->blurRadius.root.Visibility(!IsClassicSystemTaskbar() && control->glass.IsOn() && !control->acrylic.IsOn() ? mux::Visibility::Visible : mux::Visibility::Collapsed);
            control->acrylicRow.root.Visibility(control->glass.IsOn() ? mux::Visibility::Visible : mux::Visibility::Collapsed);
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
            control->gradient.sections.UpdateRows();
            RefreshDynamicRuleSummary(*control);
        }
        taskbarGradient.sections.UpdateRows();
        presenter_controls::UpdateSettingsSubrows(itemsCard.content, 0);
        presenter_controls::UpdateSettingsSubrows(edgeSwipeCard.content, 0);
        presenter_controls::StyleSettingsSubrow(edgeHoverDelayMs.root, false, 16.0);
    }

    void RefreshTaskbarRuntimeStatus()
    {
        if (suppressionStatus)
        {
            const auto state = GetSystemTaskbarSuppressionRuntimeState();
            const bool failed = state == SystemTaskbarBackdropRuntimeState::Failed;
            const bool pending = state == SystemTaskbarBackdropRuntimeState::Loading ||
                state == SystemTaskbarBackdropRuntimeState::Disabled;
            suppressionStatus.Message(L(failed ? "settings.dock.suppressTaskbar.failed" : "app.settings.taskbar_connecting",
                failed ? L"The taskbar could not be hidden. Turn this option off to restore it, then try again." : L"Connecting to the Explorer taskbar..."));
            suppressionStatus.Severity(failed ? muxc::InfoBarSeverity::Error : muxc::InfoBarSeverity::Informational);
            suppressionStatus.IsOpen(dockEnabledToggle.IsOn() && suppressTaskbarToggle.IsOn() && (failed || pending));
        }
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

    void SetClassicSystemTheme(int selectedIndex)
    {
        windowsSystemThemeValue = std::clamp(selectedIndex, 0, 2);
        const int preference = windowsSystemThemeValue - 1;
        RefreshTaskbarEntryState();
        EmitDock(SettingsUpdateMode::PreviewAndCommit,
            [preference](DockSettings& settings) {
                settings.classicTaskbarSystemTheme = preference;
            });
    }

    void RefreshTaskbarEntryState()
    {
        const bool previousUpdating = updatingControls;
        updatingControls = true;
        try
        {
            if (!IsClassicSystemTaskbar())
                windowsSystemThemeValue = IsWindowsSystemLightThemeEnabled() ? 0 : 1;
            SelectChoice(
                windowsSystemThemeChoices, windowsSystemThemeValue);
            if (IsClassicSystemTaskbar())
                taskbarContentThemeCombo.SelectedIndex(windowsSystemThemeValue);
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
        if (card.section)
            muxa::AutomationProperties::SetName(card.section, card.title.Text());
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

    void SetFullscreenChoices(const muxc::ComboBox& combo)
    {
        ReplaceComboItems(combo, {
            {"settings.dock.fullscreenAllow", L"Allow"},
            {"settings.dock.fullscreenBlockGestures", L"Block gestures only"},
            {"settings.dock.fullscreenBlockAll", L"Full protection"}});
    }

    void ClearFullscreenExceptionRows()
    {
        for (auto& item : fullscreenExceptionRows)
        {
            item.policy.SelectionChanged(item.policyToken);
            item.remove.Click(item.removeToken);
        }
        fullscreenExceptionRows.clear();
        fullscreenExceptionList.Children().Clear();
        fullscreenExceptionListHost.Visibility(mux::Visibility::Collapsed);
    }

    void RebuildFullscreenExceptions()
    {
        ClearFullscreenExceptionRows();
        for (const auto& entry : fullscreenExceptionValues)
        {
            FullscreenExceptionRow item;
            item.policy = NewCombo();
            SetFullscreenChoices(item.policy);
            item.policy.SelectedIndex(static_cast<int>(entry.policy));
            item.remove = muxc::Button{};
            item.remove.Content(winrt::box_value(L("app.settings.delete", L"Delete")));
            muxc::Grid editors{};
            muxc::ColumnDefinition policyColumn{}, removeColumn{};
            policyColumn.Width({1, mux::GridUnitType::Star});
            removeColumn.Width({1, mux::GridUnitType::Auto});
            editors.ColumnDefinitions().Append(policyColumn);
            editors.ColumnDefinitions().Append(removeColumn);
            editors.ColumnSpacing(8);
            editors.Children().Append(item.policy);
            muxc::Grid::SetColumn(item.remove, 1);
            editors.Children().Append(item.remove);
            item.policy.ClearValue(mux::FrameworkElement::WidthProperty());
            item.policy.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
            item.remove.VerticalAlignment(mux::VerticalAlignment::Center);
            item.row.Initialize(editors, 380);
            presenter_controls::StyleSettingsSubrow(item.row.root, false, 0);
            const auto name = std::filesystem::path(entry.executable).filename().wstring();
            item.row.SetText(name, entry.executable);
            item.row.help.FontSize(12);
            item.row.help.TextWrapping(mux::TextWrapping::NoWrap);
            item.row.help.TextTrimming(mux::TextTrimming::CharacterEllipsis);
            muxc::ToolTipService::SetToolTip(
                item.row.text, winrt::box_value(entry.executable));
            muxa::AutomationProperties::SetName(item.policy, name + L" — " + L("settings.dock.fullscreenPolicy", L"Fullscreen protection"));
            muxa::AutomationProperties::SetName(item.remove, L("app.settings.delete", L"Delete") + L" " + name);
            const std::wstring path = entry.executable;
            const auto policyControl = item.policy;
            item.policyToken = item.policy.SelectionChanged([this, path, policyControl](const auto&, const auto&) {
                const int selected = policyControl.SelectedIndex();
                if (selected < 0) return;
                EmitDock(SettingsUpdateMode::PreviewAndCommit, [path, selected](DockSettings& settings) {
                    for (auto& exception : settings.fullscreenExceptions)
                        if (snowdesktop::dock_fullscreen::SameExecutable(exception.executable, path))
                            exception.policy = snowdesktop::dock_fullscreen::Normalize(selected);
                });
            });
            item.removeToken = item.remove.Click([this, path](const auto&, const auto&) {
                EmitDock(SettingsUpdateMode::PreviewAndCommit, [path](DockSettings& settings) {
                    std::erase_if(settings.fullscreenExceptions, [&](const auto& exception) {
                        return snowdesktop::dock_fullscreen::SameExecutable(exception.executable, path);
                    });
                });
            });
            fullscreenExceptionList.Children().Append(item.row.root);
            fullscreenExceptionRows.push_back(std::move(item));
        }
        fullscreenExceptionListHost.Visibility(fullscreenExceptionRows.empty()
            ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        fullscreenExceptionsManagementTitle.Text(
            L("settings.dock.fullscreenExceptionManage", L"Manage application exceptions") +
            L" (" + std::to_wstring(fullscreenExceptionRows.size()) + L")");
    }

    void PickFullscreenException()
    {
        const auto gate = confirmationGate;
        const auto expectedGeneration = generation;
        winrt::com_ptr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(dialog.put())))) return;
        DWORD options = 0;
        if (FAILED(dialog->GetOptions(&options)) ||
            FAILED(dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST))) return;
        const std::wstring title = L("settings.dock.fullscreenExceptionAdd", L"Add application");
        dialog->SetTitle(title.c_str());
        const COMDLG_FILTERSPEC filter{L"*.exe", L"*.exe"};
        if (FAILED(dialog->SetFileTypes(1, &filter)) || FAILED(dialog->Show(GetActiveWindow()))) return;
        if (!gate->alive.load(std::memory_order_acquire) ||
            gate->generation.load(std::memory_order_acquire) != expectedGeneration || closed || !active) return;
        winrt::com_ptr<IShellItem> selected;
        PWSTR raw = nullptr;
        if (FAILED(dialog->GetResult(selected.put())) || FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH, &raw))) return;
        const auto path = snowdesktop::dock_fullscreen::NormalizeExecutable(raw);
        CoTaskMemFree(raw);
        if (path.empty()) return;
        AddFullscreenException(path);
    }

    void AddFullscreenException(const std::wstring& path)
    {
        EmitDock(SettingsUpdateMode::PreviewAndCommit, [path](DockSettings& settings) {
            for (const auto& exception : settings.fullscreenExceptions)
                if (snowdesktop::dock_fullscreen::SameExecutable(exception.executable, path)) return;
            if (settings.fullscreenExceptions.size() < 128)
                settings.fullscreenExceptions.push_back({path, DockFullscreenPolicy::Allow});
        });
    }

    void SelectChoice(
        const muxc::RadioButtons& choices,
        int selectedIndex)
    {
        const int count = static_cast<int>(choices.Items().Size());
        if (count == 0) return;
        const int selected = std::clamp(selectedIndex, 0, count - 1);
        for (int index = 0; index < count; ++index)
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
            ? (control.theme.SelectedItem() ? std::wstring(winrt::unbox_value<winrt::hstring>(control.theme.SelectedItem()).c_str()) : L"")
            : L("app.settings.hotkey_status_disabled", L"Disabled");
        control.summary.Text(summary);

        const winrt::hstring contextText = control.detailTitle.Text();
        const std::wstring context{
            contextText.c_str(), contextText.size()};
        muxa::AutomationProperties::SetName(control.enabled, context);
        muxa::AutomationProperties::SetHelpText(control.enabled,
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
        if (IsClassicSystemTaskbar())
        {
            custom = false;
            logicalValue = windowsSystemThemeValue - 1;
        }
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
        taskbarThemes->LocalizeText();
        for (auto* rule : dynamicRules) rule->savedThemes->LocalizeText();
        const auto refresh = [this](TaskbarGradientControl& gradient) {
            gradient.borderEditor->RefreshLocalizedText();
            gradient.sections.RefreshLocalizedText([this](auto key) { return L(key, L""); });
            gradient.editor->RefreshLocalizedText();
        };
        refresh(taskbarGradient);
        for (auto* control : dynamicRules) refresh(control->gradient);
        if (closed)
            return;
        const bool previousUpdating = updatingControls;
        updatingControls = true;

        SetCardText(appearanceCard,
            centralizedAppearance ? "settings.nav.dock" : "settings.dock.appearance", L"Appearance");
        SetCardText(animationCard,
            "settings.animation.dock", L"Dock animations");
        SetCardText(edgeSwipeCard,
            "settings.dock.visibilityAndSummon", L"Visibility and summon");
        SetCardText(fullscreenCard,
            "settings.dock.fullscreenPolicy", L"Fullscreen protection");
        SetCardText(layoutCard,
            "settings.dock.layoutAndPosition", L"Position and layout");
        SetCardText(mergedCard,
            "settings.bars.mergedSettings", L"Merged bar settings");
        SetCardText(behaviorCard,
            "settings.dock.spaceAvoidance", L"Space reservation");
        SetCardText(itemsCard,
            "settings.dock.contentAndLaunch", L"Items and launch");
        SetCardText(taskbarCard,
            "settings.taskbar.behavior", L"Behavior");
        SetCardText(taskbarAppearanceCard,
            centralizedAppearance ? "settings.nav.taskbar" : "settings.taskbar.defaultAppearance", L"Default appearance");
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
            L"Match rules from top to bottom; otherwise use the default appearance."));
        muxa::AutomationProperties::SetHelpText(
            taskbarRulesCard.root, taskbarRulesHint.Text());
        muxa::AutomationProperties::SetHelpText(taskbarCard.root,
            L("settings.taskbar.systemSettings.description",
                L"Manage taskbar behavior in Windows Settings."));
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
            "app.settings.dock_thickness", L"Dock Scale",
            L("app.settings.dock_thickness_hint",
                L"Adjust dock bar and icon size together."));
        SetContinuousText(mergedIconScale,
            "settings.bars.iconSize", L"Icon size", L"");
        SetContinuousText(edgeDistanceScale,
            "settings.dock.edgeDistance", L"Distance from screen edge",
            L("settings.dock.edgeDistance.description",
                L"Relative to the desktop component margin."));

        floatingEdgeSwipeRow.SetText(
            L("app.dock.floating_edge_swipe", L"Edge Swipe"));
        edgeRevealGestureRow.SetText(L("settings.dock.edgeRevealGesture", L"Reveal gesture"),
            L("settings.dock.edgeRevealGesture.description", L"Dragging an item to the edge reveals the bar immediately."));
        ReplaceComboItems(edgeRevealGesture, {
            {"settings.dock.gestureSwipe", L"Swipe"}, {"settings.dock.gestureHover", L"Hover"}});
        SetContinuousText(edgeHoverDelayMs,
            "settings.dock.edgeHoverDelay", L"Hover delay");
        windowPreviewsRow.SetText(L("settings.dock.windowPreviews", L"Task thumbnails"),
            L("settings.dock.windowPreviews.description", L"Show open windows when hovering over an app."));
        muxa::AutomationProperties::SetName(edgeRevealGesture, edgeRevealGestureRow.label.Text());
        muxa::AutomationProperties::SetName(windowPreviews, windowPreviewsRow.label.Text());
        fullscreenPolicyRow.SetText(
            L("settings.dock.fullscreenDefaultPolicy", L"Default protection"),
            L("settings.dock.fullscreenPolicy.description",
                L"Full protection blocks Dock invocation and keyboard focus takeover. Automatic focus restoration never interrupts a foreground fullscreen app."));
        SetFullscreenChoices(fullscreenPolicyCombo);
        fullscreenExceptionsRow.SetText(
            L("settings.dock.fullscreenExceptions", L"Application exceptions"),
            L("settings.dock.fullscreenExceptions.description", L"Applies to every fullscreen window of the selected executable, including browser games."));
        trayEditor->RefreshText();
        fullscreenExceptionsManagementTitle.Text(L("settings.dock.fullscreenExceptionManage", L"Manage application exceptions") +
            L" (" + std::to_wstring(fullscreenExceptionValues.size()) + L")");
        muxa::AutomationProperties::SetName(fullscreenExceptionsExpander, fullscreenExceptionsManagementTitle.Text());
        fullscreenExceptionAdd.Content(winrt::box_value(L("settings.dock.fullscreenExceptionAdd", L"Add application")));
        const bool previousFullscreenUpdate = updatingControls;
        updatingControls = true;
        fullscreenRunningApps.Items().Clear();
        fullscreenRunningApplications.clear();
        fullscreenRunningApps.Items().Append(winrt::box_value(L("settings.dock.fullscreenFromRunning", L"Choose from running apps")));
        fullscreenRunningApps.SelectedIndex(0);
        muxa::AutomationProperties::SetName(fullscreenRunningApps, L("settings.dock.fullscreenFromRunning", L"Choose from running apps"));
        updatingControls = previousFullscreenUpdate;
        RebuildFullscreenExceptions();
        muxa::AutomationProperties::SetName(
            fullscreenPolicyCombo, fullscreenPolicyRow.label.Text());
        allowDesktopContentOverlapRow.SetText(
            L("settings.dock.allowDesktopContentOverlap",
                L"Keep desktop content clear of Dock"),
            L("settings.dock.allowDesktopContentOverlap.description",
                L"Keep desktop icons and widgets clear of the Dock. Maximized windows are controlled separately."));
        showOnlyWhenSummonedRow.SetText(
            L("settings.dock.showOnlyWhenSummoned",
                L"Show Dock only when summoned"),
            L("settings.dock.showOnlyWhenSummoned.description",
                L"Show the Dock by swiping along its screen edge or "
                  "dragging an item to that edge."));
        summonOnAppMinimizeRow.SetText(
            L("settings.dock.summonOnAppMinimize", L"Summon Dock when an app is minimized"),
            L("settings.dock.summonOnAppMinimize.description",
                L"When off, custom animations reveal the Dock only until they finish; system animations do not reveal it."));
        muxa::AutomationProperties::SetName(summonOnAppMinimizeToggle, summonOnAppMinimizeRow.label.Text());
        suppressTaskbarRow.SetText(L("settings.dock.suppressTaskbar", L"Always hide the system taskbar"),
            L("settings.dock.suppressTaskbar.description", L"Hide the taskbar on Dock displays; show it for system panels."));
        muxa::AutomationProperties::SetName(suppressTaskbarToggle, suppressTaskbarRow.label.Text());
        taskbarSuppressRow.SetText(L("settings.bars.taskbarMode", L"Taskbar visibility"));
        const int displayMode = taskbarMode.SelectedIndex();
        taskbarMode.Items().Clear();
        for (const auto key : {"settings.bars.taskbarVisible", "settings.taskbar.autoHide", "settings.dock.suppressTaskbar"})
        {
            muxc::ComboBoxItem item; item.Content(winrt::box_value(L(key, L"")));
            taskbarMode.Items().Append(item);
        }
        taskbarMode.SelectedIndex(displayMode);
        homeSizeRow.SetText(L("settings.bars.homeSize", L"Keep the last display Dock at its first-page size"), L("settings.bars.homeSize.description", L""));
        mergedHeight->RefreshLocalizedText();
        muxa::AutomationProperties::SetName(taskbarSuppressToggle, suppressTaskbarRow.label.Text());
        reserveScreenSpaceRow.SetText(L("settings.dock.reserveScreenSpace", L"Reserve screen space"),
            L("settings.dock.reserveScreenSpace.description", L"Keep maximized windows clear of the standalone Dock. A summon-only Dock reserves no space."));
        muxa::AutomationProperties::SetName(reserveScreenSpaceToggle, reserveScreenSpaceRow.label.Text());
        for (const auto& button : {startIconBeautify, searchIconBeautify})
        {
            muxc::FontIcon icon;
            icon.Glyph(L"\xE70F");
            icon.FontSize(16);
            button.Content(icon);
            const auto label = L("iconAppearance.edit", L"Adjust this icon’s appearance");
            muxa::AutomationProperties::SetName(button, label);
            muxc::ToolTipService::SetToolTip(button, winrt::box_value(label));
        }
        startIconRow.SetText(L("iconLibrary.startIcon", L"Start button icon"));
        searchIconRow.SetText(L("iconLibrary.searchIcon", L"Search button icon"));
        for (const auto& button : {startIconChoose, searchIconChoose, startIconRestore, searchIconRestore})
        {
            const bool restore = button == startIconRestore || button == searchIconRestore;
            muxc::TextBlock text; text.Text(L(restore ? "iconLibrary.restore" : "iconLibrary.change", restore ? L"Restore default icon" : L"Change icon"));
            text.TextWrapping(mux::TextWrapping::Wrap); button.Content(text); button.MinWidth(0);
            button.HorizontalAlignment(mux::HorizontalAlignment::Stretch); button.VerticalAlignment(mux::VerticalAlignment::Center);
        }
        showWindowsButtonRow.SetText(L(
            "app.dock.show_windows_button", L"Show Windows Button"));
        startMenuKeepTaskbarHiddenRow.SetText(
            L("settings.dock.startMenuKeepTaskbarHidden", L"Keep the taskbar hidden when opening Start from the Dock"),
            L("settings.dock.startMenuKeepTaskbarHidden.description",
                L"For hidden or auto-hidden taskbars. Start opens centered beside the Dock without covering it. Unsupported layouts use Windows defaults."));
        muxa::AutomationProperties::SetName(startMenuKeepTaskbarHiddenToggle,
            startMenuKeepTaskbarHiddenRow.label.Text());
        showQuickSearchButtonRow.SetText(L(
            "app.dock.show_quick_search_button", L"Show quick search button"));
        singleClickLaunchItemsRow.SetText(
            L("settings.dock.singleClickLaunchItems", L"Launch pinned and frequent icons with a single click"),
            L("settings.dock.singleClickLaunchItems.description",
                L"When off, double-click to launch. Folders and collections keep their current opening behavior."));
        muxa::AutomationProperties::SetName(
            singleClickLaunchItems, singleClickLaunchItemsRow.label.Text());
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
            showQuickSearchButtonToggle, showQuickSearchButtonRow.label.Text());
        muxa::AutomationProperties::SetName(
            showFrequentItemsToggle, showFrequentItemsRow.label.Text());
        SetContinuousText(frequentItemCount,
            "app.settings.show_count", L"Frequent Item Count");
        SetUnit(frequentItemCount, ExtractNumericUnit(
            L("app.settings.items_unit", L"%d items")));
        taskbarSettingsRow.SetText(
            L("settings.taskbar.systemSettings", L"Windows taskbar settings"));
        taskbarSettingsLink.Content(winrt::box_value(L(
            "settings.taskbar.systemSettings.open", L"Open settings")));
        muxa::AutomationProperties::SetName(
            taskbarSettingsLink, taskbarSettingsRow.label.Text());
        muxa::AutomationProperties::SetHelpText(taskbarSettingsLink,
            L("settings.taskbar.systemSettings.description",
                L"Manage taskbar behavior in Windows Settings."));
        windowsSystemThemeRow.SetText(
            L("app.settings.appearance", L"Appearance"), systemPanelHelp);
        if (IsClassicSystemTaskbar())
            ReplaceChoiceItems(windowsSystemThemeChoices, {
                {"settings.taskbar.classic.automatic", L"Automatic"},
                {"app.settings.light", L"Light"},
                {"app.settings.dark", L"Dark"},
            }, windowsSystemThemeValue);
        else
        {
            windowsSystemThemeValue = IsWindowsSystemLightThemeEnabled() ? 0 : 1;
            ReplaceChoiceItems(windowsSystemThemeChoices, {
                {"app.settings.light", L"Light"},
                {"app.settings.dark", L"Dark"},
            }, windowsSystemThemeValue);
        }
        muxa::AutomationProperties::SetName(
            windowsSystemThemeChoices, windowsSystemThemeRow.label.Text());
        restartExplorerRow.SetText(
            L("settings.taskbar.restartExplorer",
                L"Restart File Explorer"),
            L("settings.taskbar.restartExplorer.description",
                L"The desktop and taskbar will briefly disappear."));
        restartExplorerButton.Content(winrt::box_value(
            L("settings.taskbar.restartExplorer",
                L"Restart File Explorer")));
        muxa::AutomationProperties::SetName(restartExplorerButton,
            L("settings.taskbar.restartExplorer",
                L"Restart File Explorer"));
        muxa::AutomationProperties::SetHelpText(restartExplorerButton,
            L("settings.taskbar.restartExplorer.description",
                L"The desktop and taskbar will briefly disappear."));

        taskbarThemeRow.SetText(
            L("app.settings.taskbar_theme", L"Taskbar Theme"));
        muxa::AutomationProperties::SetName(
            taskbarThemeCombo, taskbarThemeRow.label.Text());
        ReplaceTaskbarThemeItems(taskbarThemeCombo);
        taskbarContentThemeRow.SetText(IsClassicSystemTaskbar()
                ? L("settings.taskbar.classic.theme", L"Taskbar theme and colors")
                : L("app.settings.taskbar_foreground_color", L"Taskbar Icon and Text Color"), IsClassicSystemTaskbar()
                ? L("settings.taskbar.classic.description",
                    L"Shared with system panels. Automatic matches the primary taskbar appearance. Blur strength is controlled by Windows.")
                : L"");
        muxa::AutomationProperties::SetName(
            taskbarContentThemeCombo, taskbarContentThemeRow.label.Text());
        ReplaceMainContentThemeItems(
            taskbarThemes->NativeSelection() ==
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
        control.enabledRow.SetText(context);
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
        taskbarThemes->SetGeneration(generation);
        for (auto* rule : dynamicRules) rule->savedThemes->SetGeneration(generation);
        confirmationGate->generation.store(generation,
            std::memory_order_release);
        const bool previousUpdating = updatingControls;
        updatingControls = true;
        globalAppearance = snapshot.values.personalization;
        const auto& dock = snapshot.values.dock;
        const auto& bar = snapshot.values.general.statusBar;
        // Disable only when every Dock copy is merged. With an all-monitor
        // Dock and a single-monitor bar, this still controls the other copies.
        barAvailability = ResolveBarSettingsAvailability(snapshot.values.general.dockEnabled, dock, bar, GetSystemMetrics(SM_CMONITORS));
        spaceReservedByStatusBar = barAvailability.allDockMerged;
        mergedHeight->Update(snapshot);
        mergedCard.root.Visibility(barAvailability.anyMerged ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        if (newGeneration)
        {
            confirmationGate->confirmationPending = false;
            PatchGradient(taskbarGradient, snapshot.values.dock.systemTaskbarAppearance.panelGradient, true);
            taskbarGradient.borderEditor->SetValue(snapshot.values.dock.systemTaskbarAppearance, true);
            for (auto* control : dynamicRules)
            {
                PatchGradient(control->gradient, (snapshot.values.dock.*control->member).appearance.panelGradient, true);
                control->gradient.borderEditor->SetValue((snapshot.values.dock.*control->member).appearance, true);
            }
        }

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
        taskbarThemes->SyncSelection(MainTaskbarThemeMode(dock), snapshot.values);
        for (auto* control : dynamicRules)
            control->savedThemes->SyncSelection(static_cast<int>((dock.*control->member).themeMode), snapshot.values);
        UpdateDependentStates();
        updatingControls = previousUpdating;
    }

    mux::FrameworkElement FocusTarget(std::string_view id, SettingsFocusPrerequisite* prerequisite) const noexcept
    {
        if (prerequisite) *prerequisite = {};
        const auto prerequisiteTarget = [prerequisite](mux::FrameworkElement target,
            std::string_view message, std::wstring_view name) {
            if (prerequisite)
            {
                prerequisite->messageKey = "settings.search.prerequisite." + std::string(message);
                prerequisite->controlName.assign(name);
            }
            return target;
        };
        const auto dockTarget = [&](mux::FrameworkElement target) {
            // Apply only to the rows disabled by UpdateDependentStates. The
            // taskbar and independently editable appearance routes stay usable.
            return dockEnabledToggle.IsOn() ? target : prerequisiteTarget(dockEnabledToggle,
                "enable", dockEnabledRow.label.Text().c_str());
        };
        const auto edgeRevealTarget = [&] {
            return dockTarget(showOnlyWhenSummonedToggle.IsOn()
                ? prerequisiteTarget(showOnlyWhenSummonedToggle, "disable", showOnlyWhenSummonedRow.label.Text().c_str())
                : floatingEdgeSwipeToggle.as<mux::FrameworkElement>());
        };
        if (id == "dock.enable") return dockEnabledToggle;
        if (id == "dock.position") return dockTarget(positionCombo);
        if (id == "dock.layout" || id == "dock.edgeAttached")
            return dockTarget(layoutCombo);
        if (id == "dock.edgeDistanceScale")
            return dockTarget(layoutCombo.SelectedIndex() == 0
                ? edgeDistanceScale.slider.as<mux::FrameworkElement>()
                : prerequisiteTarget(layoutCombo, "choice", layoutRow.label.Text().c_str()));
        if (id == "dock.monitor" || id == "dock.monitorScope")
            return dockTarget(monitorScopeCombo);
        if (id == "dock.mergedBarHeight")
            return dockTarget(barAvailability.anyMerged ? mergedHeight->FocusTarget().as<mux::FrameworkElement>()
                : prerequisiteTarget(positionCombo, "dependency", positionRow.label.Text().c_str()));
        if (id == "dock.mergedIconScale")
            return dockTarget(barAvailability.anyMerged ? mergedIconScale.slider.as<mux::FrameworkElement>()
                : prerequisiteTarget(positionCombo, "dependency", positionRow.label.Text().c_str()));
        if (id == "dock.lastMonitorUseHomeSize")
        {
            if (!barAvailability.dockOnLastMonitor)
                return dockTarget(prerequisiteTarget(monitorScopeCombo, "choice", monitorScopeRow.label.Text().c_str()));
            if (barAvailability.allDockMerged)
                return dockTarget(prerequisiteTarget(layoutCombo, "dependency", layoutRow.label.Text().c_str()));
            return dockTarget(homeSize);
        }
        if (id == "taskbar.displayMode" || id == "taskbar.suppressSystemTaskbar" || id == "taskbar.autoHide") return taskbarMode;
        if (id == "dock.thickness" || id == "dock.thicknessScale")
            return dockTarget(thicknessScale.slider);
        if (id == "dock.floatingShortcutMode" || id == "dock.floatingShortcutMode.hotkey")
        {
            if (!dockEnabledToggle.IsOn()) return dockTarget(dockEnabledToggle);
            return nullptr; // General owns the mounted shortcut editor.
        }
        if (id == "dock.edgeRevealGesture" || id == "dock.edgeHoverDelayMs")
        {
            if (!floatingEdgeSwipeToggle.IsOn())
            {
                auto target = prerequisiteTarget(floatingEdgeSwipeToggle, "enable", floatingEdgeSwipeRow.label.Text().c_str());
                return showOnlyWhenSummonedToggle.IsOn() ? edgeRevealTarget() : dockTarget(target);
            }
            if (id == "dock.edgeHoverDelayMs")
            {
                if (edgeRevealGesture.SelectedIndex() != 1)
                    return dockTarget(prerequisiteTarget(edgeRevealGesture,
                        "choice", edgeRevealGestureRow.label.Text().c_str()));
                return dockTarget(edgeHoverDelayMs.slider);
            }
            return dockTarget(edgeRevealGesture);
        }
        if (id == "dock.showWindowPreviews") return dockTarget(windowPreviews);
        if (id == "dock.singleClickLaunchItems") return dockTarget(singleClickLaunchItems);
        if (id == "dock.trayApplications") return dockTarget(trayEditor->FocusTarget());
        if (id == "dock.fullscreenPolicy" || id == "dock.floatingEdgeSwipeBlockFullscreen")
            return dockTarget(fullscreenPolicyCombo);
        if (id == "dock.fullscreenExceptions")
        {
            return dockTarget(fullscreenExceptionAdd);
        }
        if (id == "dock.floatingEdgeSwipe" ||
            id == "dock.floatingEdgeSwipeEnabled")
            return edgeRevealTarget();
        if (id == "dock.suppressSystemTaskbar") return taskbarMode;
        if (id == "dock.reserveScreenSpace" || id == "dock.allowDesktopContentOverlap")
        {
            if (showOnlyWhenSummonedToggle.IsOn())
                return dockTarget(prerequisiteTarget(showOnlyWhenSummonedToggle, "disable", showOnlyWhenSummonedRow.label.Text().c_str()));
            if (spaceReservedByStatusBar)
                return dockTarget(prerequisiteTarget(layoutCombo, "dependency", layoutRow.label.Text().c_str()));
            return dockTarget(id == "dock.reserveScreenSpace" ? reserveScreenSpaceToggle : allowDesktopContentOverlapToggle);
        }
        if (id == "dock.startIcon") return dockTarget(startIconChoose);
        if (id == "dock.searchIcon") return dockTarget(searchIconChoose);
        if (id == "dock.showWindowsButton")
            return dockTarget(showWindowsButtonToggle);
        if (id == "dock.startMenuKeepTaskbarHidden")
            return dockTarget(startMenuKeepTaskbarHiddenToggle);
        if (id == "dock.showQuickSearchButton")
            return dockTarget(showQuickSearchButtonToggle);
        if (id == "dock.showFrequentItems")
            return dockTarget(showFrequentItemsToggle);
        if (id == "dock.frequentItemCount")
            return dockTarget(showFrequentItemsToggle.IsOn() ? frequentItemCount.slider.as<mux::FrameworkElement>()
                : prerequisiteTarget(showFrequentItemsToggle, "enable", showFrequentItemsRow.label.Text().c_str()));
        if (id == "dock.keepWhenDesktopHidden")
            return dockEnabledToggle; // Retained data field has no page control.
        if (id == "dock.showOnlyWhenSummoned" || id == "dock.autoHide")
            return dockTarget(showOnlyWhenSummonedToggle);
        if (id == "dock.summonOnAppMinimize")
            return dockTarget(summonOnAppMinimizeToggle);
        if (id == "taskbar.systemSettings" || id == "taskbar.autoHide" ||
            id == "taskbar.alignment") return taskbarSettingsLink;
        if (id == "taskbar.systemTheme" || id == "taskbar.systemPanel")
            return windowsSystemThemeChoices;
        if (id == "taskbar.theme" || id == "taskbar.backdrop" ||
            id == "taskbar.followGlobal")
            return taskbarThemeCombo;
        if (id == "taskbar.contentTheme")
            return taskbarContentThemeRow.root.Visibility() == mux::Visibility::Visible
                ? taskbarContentThemeCombo : prerequisiteTarget(taskbarThemeCombo, "choice", taskbarThemeRow.label.Text().c_str());
        // Classic taskbars cannot expose blur radius under any theme. A theme
        // change or enabling glass would therefore be misleading guidance.
        if (id == "taskbar.blurRadius" && IsClassicSystemTaskbar())
            return prerequisiteTarget(taskbarThemeCombo, "unavailable", taskbarBlurRadius.row.label.Text().c_str());
        // A search may target a value whose prerequisite is not selected.
        // Locate that visible prerequisite without changing the user's values.
        if ((id == "taskbar.backgroundColor" || id == "taskbar.borderColor" ||
                id == "taskbar.backgroundOpacity" || id == "taskbar.borderOpacity" ||
                id == "taskbar.glass" || id == "taskbar.blurRadius" || id == "taskbar.acrylic") &&
            taskbarCustomAppearance.Visibility() != mux::Visibility::Visible)
            return prerequisiteTarget(taskbarThemeCombo, "customTheme", taskbarThemeRow.label.Text().c_str());
        if (id == "taskbar.backgroundColor")
            return taskbarGradient.enabled ? taskbarGradient.editor->Content().as<mux::FrameworkElement>()
                : taskbarBackgroundColor.editor.button;
        if (id == "taskbar.borderColor")
            return taskbarGradient.borderEditor->FocusTarget("app.settings.border_color");
        if (id == "taskbar.backgroundOpacity")
            return taskbarGradient.enabled ? taskbarGradient.editor->Content().as<mux::FrameworkElement>()
                : taskbarBackgroundAlpha.slider;
        if (id == "taskbar.borderOpacity")
            return taskbarGradient.borderEditor->FocusTarget("largeIcon.borderOpacity");
        if (id == "taskbar.glass") return taskbarGlassToggle;
        if (id == "taskbar.blurRadius")
        {
            if (!taskbarGlassToggle.IsOn())
                return prerequisiteTarget(taskbarGlassToggle, "enable", taskbarGlassRow.label.Text().c_str());
            if (taskbarAcrylicToggle.IsOn())
                return prerequisiteTarget(taskbarAcrylicToggle, "disable", taskbarAcrylicRow.label.Text().c_str());
            return taskbarBlurRadius.slider;
        }
        if (id == "taskbar.acrylic")
            return taskbarGlassToggle.IsOn() ? taskbarAcrylicToggle
                : prerequisiteTarget(taskbarGlassToggle, "enable", taskbarGlassRow.label.Text().c_str());
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
            taskbarGradient.borderEditor->Flush();
            for (auto* rule : dynamicRules) rule->gradient.borderEditor->Flush();
            taskbarGradient.editor->Flush();
            for (auto* rule : dynamicRules) rule->gradient.editor->Flush();
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
        StopCardHighlights();
        taskbarThemes->Close();
        for (auto* rule : dynamicRules) rule->savedThemes->Close();
        taskbarGradient.borderEditor->Close();
        for (auto* rule : dynamicRules) rule->gradient.borderEditor->Close();
        taskbarGradient.editor->Close();
        for (auto* rule : dynamicRules) rule->gradient.editor->Close();
        confirmationGate->alive.store(false, std::memory_order_release);
        try
        {
            runtimeTimer.Stop();
            runtimeTimer.Tick(runtimeTimerToken);
            highlightTimer.Tick(highlightTimerToken);
            taskbarAppearanceCard.root.Loaded(taskbarRootLoadedToken);
            dockEnabledToggle.Toggled(dockEnabledToken);
            positionCombo.SelectionChanged(positionToken);
            layoutCombo.SelectionChanged(layoutToken);
            monitorScopeCombo.SelectionChanged(monitorScopeToken);
            floatingShortcutToggle.Toggled(floatingShortcutToken);
            floatingEdgeSwipeToggle.Toggled(floatingEdgeSwipeToken);
            edgeRevealGesture.SelectionChanged(edgeRevealGestureToken);
            windowPreviews.Toggled(windowPreviewsToken);
            singleClickLaunchItems.Toggled(singleClickLaunchItemsToken);
            fullscreenPolicyCombo.SelectionChanged(fullscreenPolicyToken);
            fullscreenExceptionAdd.Click(fullscreenExceptionAddToken);
            fullscreenRunningApps.DropDownOpened(fullscreenRunningOpenToken);
            fullscreenRunningApps.SelectionChanged(fullscreenRunningSelectToken);
            ClearFullscreenExceptionRows();
            trayEditor.reset();
            showWindowsButtonToggle.Toggled(showWindowsButtonToken);
            startMenuKeepTaskbarHiddenToggle.Toggled(startMenuKeepTaskbarHiddenToken);
            showQuickSearchButtonToggle.Toggled(showQuickSearchButtonToken);
            mergedHeight->Close();
            homeSize.Toggled(homeSizeToken);
            taskbarMode.SelectionChanged(taskbarModeToken);
            suppressTaskbarToggle.Toggled(suppressTaskbarToken);
            taskbarSuppressToggle.Toggled(taskbarSuppressToken);
            reserveScreenSpaceToggle.Toggled(reserveScreenSpaceToken);
            showFrequentItemsToggle.Toggled(showFrequentItemsToken);
            keepWhenDesktopHiddenToggle.Toggled(keepWhenDesktopHiddenToken);
            allowDesktopContentOverlapToggle.Toggled(
                allowDesktopContentOverlapToken);
            showOnlyWhenSummonedToggle.Toggled(showOnlyWhenSummonedToken);
            summonOnAppMinimizeToggle.Toggled(summonOnAppMinimizeToken);
            taskbarSettingsLink.Click(taskbarSettingsToken);
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
        *iconPickerAlive = false;
        if (cancelIconPicker) { cancelIconPicker(); cancelIconPicker = {}; }
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
    {
        impl_->mergedHeight->SetActions(actions);
        impl_->actions = std::move(actions);
        const auto configure = [state = impl_.get()](ThemeLibraryControls& themes) {
            themes.SetChanged([state] {
                state->taskbarThemes->Refresh();
                for (auto* rule : state->dynamicRules) rule->savedThemes->Refresh();
            });
            themes.SetActions(state->actions.themeLibrary, state->actions.themeAsync, [state] {
                if (!state->active || state->closed || !state->hasSnapshot) return false;
                state->CommitOpenColorEditors(); state->CommitContinuousEdits();
                state->taskbarGradient.borderEditor->Flush(); state->taskbarGradient.editor->Flush();
                for (auto* rule : state->dynamicRules) { rule->gradient.borderEditor->Flush(); rule->gradient.editor->Flush(); }
                return true;
            });
        };
        configure(*impl_->taskbarThemes);
        for (auto* rule : impl_->dynamicRules) configure(*rule->savedThemes);
    }
}

mux::UIElement DockPagePresenter::Content() const noexcept
{
    return impl_ ? impl_->dockRoot : nullptr;
}

mux::UIElement DockPagePresenter::DockEnableContent() const noexcept
{
    return impl_ ? impl_->dockEnableRoot : nullptr;
}

mux::UIElement DockPagePresenter::DockAppearanceContent() const
{
    if (impl_)
    {
        impl_->centralizedAppearance = false;
        impl_->SetCardText(impl_->appearanceCard, "settings.dock.appearance", L"Appearance");
        uint32_t index = 0;
        if (!impl_->dockAppearanceRoot.Children().IndexOf(impl_->appearanceCard.root, index))
            impl_->dockAppearanceRoot.Children().InsertAt(0, impl_->appearanceCard.root);
    }
    return impl_ ? impl_->dockAppearanceRoot : nullptr;
}

mux::UIElement DockPagePresenter::DockThemeContent() const
{
    if (!impl_) return nullptr;
    impl_->centralizedAppearance = true;
    impl_->SetCardText(impl_->appearanceCard, "settings.nav.dock", L"Dock");
    uint32_t index = 0;
    if (impl_->dockAppearanceRoot.Children().IndexOf(impl_->appearanceCard.root, index))
        impl_->dockAppearanceRoot.Children().RemoveAt(index);
    return impl_->appearanceCard.root;
}

mux::UIElement DockPagePresenter::TaskbarAppearanceContent() const
{
    if (!impl_) return nullptr;
    impl_->centralizedAppearance = true;
    impl_->SetCardText(impl_->taskbarAppearanceCard, "settings.nav.taskbar", L"Windows taskbar");
    uint32_t index = 0;
    if (impl_->taskbarRoot.Children().IndexOf(impl_->taskbarAppearanceCard.root, index))
        impl_->taskbarRoot.Children().RemoveAt(index);
    return impl_->taskbarAppearanceCard.root;
}

void DockPagePresenter::SetSupplementalContent(const mux::UIElement& appearance,
    const mux::UIElement& animation, const mux::UIElement& shortcuts)
{
    if (!impl_) return;
    if (impl_->appearanceHost.Content().try_as<mux::UIElement>() != appearance)
        impl_->appearanceHost.Content(appearance);
    if (impl_->animationHost.Content().try_as<mux::UIElement>() != animation)
        impl_->animationHost.Content(animation);
    if (impl_->shortcutsHost.Content().try_as<mux::UIElement>() != shortcuts)
        impl_->shortcutsHost.Content(shortcuts);
}

mux::UIElement DockPagePresenter::DockContent() const noexcept
{
    return impl_ ? impl_->dockRoot : nullptr;
}

mux::UIElement DockPagePresenter::TaskbarContent() const
{
    if (impl_)
    {
        impl_->centralizedAppearance = false;
        impl_->SetCardText(impl_->taskbarAppearanceCard, "settings.taskbar.defaultAppearance", L"Default appearance");
        uint32_t index = 0;
        if (!impl_->taskbarRoot.Children().IndexOf(impl_->taskbarAppearanceCard.root, index))
            impl_->taskbarRoot.Children().InsertAt(1, impl_->taskbarAppearanceCard.root);
    }
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
    {
        impl_->active = true;
        impl_->confirmationGate->active = true;
        try { impl_->runtimeTimer.Start(); impl_->RefreshTaskbarRuntimeState(); } catch (...) {}
    }
}

void DockPagePresenter::ActivateTaskbar() noexcept
{
    if (!impl_ || impl_->closed)
        return;
    impl_->taskbarInputReady = false;
    impl_->active = true;
    try {
        impl_->taskbarThemes->Refresh();
        for (auto* rule : impl_->dynamicRules) rule->savedThemes->Refresh();
    } catch (...) {}
    impl_->confirmationGate->active = false;
    try { impl_->runtimeTimer.Start(); impl_->RefreshTaskbarRuntimeState(); } catch (...) {}
}

void DockPagePresenter::Deactivate() noexcept
{
    if (impl_ && impl_->cancelIconPicker)
    { auto cancel = std::exchange(impl_->cancelIconPicker, {}); cancel(); }
    if (!impl_ || impl_->closed)
        return;
    impl_->mergedHeight->Flush();
    impl_->CommitOpenColorEditors();
    impl_->CommitContinuousEdits();
    impl_->taskbarInputReady = false;
    impl_->active = false;
    impl_->confirmationGate->active = false;
    impl_->StopCardHighlights();
    try { impl_->runtimeTimer.Stop(); } catch (...) {}
}

mux::FrameworkElement DockPagePresenter::FocusTarget(
    std::string_view focusId, SettingsFocusPrerequisite* prerequisite) const noexcept
{
    if (prerequisite) *prerequisite = {};
    return impl_ ? impl_->FocusTarget(focusId, prerequisite) : nullptr;
}

void DockPagePresenter::Close() noexcept
{
    if (impl_)
        impl_->Close();
}

} // namespace snowdesktop::winui

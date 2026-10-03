#include "pch.h"

#include "personalization_page_presenter.h"
#include "settings_presenter_controls.h"
#include "panel_gradient_editor.h"
#include "appearance_sections.h"
#include "edge_light_editor.h"
#include "panel_appearance_editor.h"
#include "font_picker_search.h"
#include "quick_navigation_options.h"
#include "../theme_library_settings.h"

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

constexpr std::array<int, 8> kPresetIds = {
    kAppearancePresetDark,
    kAppearancePresetLight,
    kAppearancePresetGlassDark,
    kAppearancePresetGlassLight,
    kAppearancePresetGlassTransparent,
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
    explicit Impl(LocalizeCallback callback, const mux::Style& style, const mux::Style& navigation)
        : localize(std::move(callback)), cardStyle(style), navigationStyle(navigation)
    {
        BuildControls();
        HookEvents();
        RefreshLocalizedText();
    }

    LocalizeCallback localize;
    PersonalizationPageActions actions;
    mux::Style cardStyle{nullptr};
    mux::Style navigationStyle{nullptr};
    muxc::StackPanel themeRoot{nullptr}, dockThemeRoot{nullptr};
    muxc::ContentControl dockAppearanceHost;
    muxc::StackPanel menuRoot;
    muxc::StackPanel widgetLayoutRoot{nullptr};
    muxc::StackPanel widgetBehaviorRoot;

    SettingsCard themeCard;
    SettingsCard libraryCard;
    muxc::ComboBox libraryTarget, libraryChoice, libraryQuick, libraryPopup, libraryReplacement;
    muxc::TextBox libraryName;
    muxc::StackPanel libraryScopes;
    std::array<muxc::CheckBox, 4> libraryScopeChecks;
    muxc::InfoBar libraryFeedback;
    muxc::CommandBar libraryCommands;
    std::array<muxc::AppBarButton, 5> libraryButtons;
    std::array<winrt::event_token, 5> libraryButtonTokens{};
    winrt::event_token libraryTargetToken{}, libraryChoiceToken{};
    themes::Library savedThemes;
    std::vector<themes::Theme> libraryChoices, libraryQuickChoices, libraryPopupChoices;
    bool updatingLibrary = false;
    static constexpr std::array<const char*, 6> libraryTargets = {"global", "dock", "statusBar", "taskbar", "quickPanel", "popup"};
    SettingsCard fontCard;
    SettingRow fontRow;
    muxc::DropDownButton fontPicker;
    muxc::TextBlock fontPickerLabel;
    muxc::Flyout fontFlyout;
    muxc::StackPanel fontPopup;
    muxc::TextBox fontSearch;
    muxc::ListView fontResults;
    muxc::TextBlock fontEmpty;
    muxc::Button fontImportFile, fontImportFolder;
    muxc::Button fontRestartButton;
    muxc::InfoBar fontRestart;
    muxc::TextBlock fontError;
    winrt::event_token fontOpenToken{}, fontSearchToken{}, fontSearchKeyToken{}, fontResultToken{}, fontResultKeyToken{};
    winrt::event_token fontImportFileToken{}, fontImportFolderToken{}, fontRestartToken{};
    std::vector<app_fonts::Choice> fonts;
    std::vector<std::size_t> visibleFonts;
    app_fonts::Selection selectedFont;
    SettingsCard themeTargetsCard;
    SettingsCard popupThemeCard, dockThemeCard;
    std::shared_ptr<PanelAppearanceEditor> quickAppearanceEditor, popupAppearanceEditor, dockAppearanceEditor;
    std::unique_ptr<QuickNavigationOptions> quickAppearanceOptions;
    muxc::StackPanel quickAppearanceContent;
    std::uint64_t navigationRevision = 0;
    muxc::ComboBox dockAppearanceCombo{nullptr};
    muxc::Button taskbarLink{nullptr}, statusBarLink{nullptr};
    muxc::TextBlock taskbarLinkTitle, taskbarLinkDescription, statusBarLinkTitle, statusBarLinkDescription;
    SettingRow dockAppearanceRow;
    winrt::event_token dockAppearanceToken{}, statusBarLinkToken{}, taskbarLinkToken{};
    PersonalizationSettings currentGlobalAppearance;
    std::uint64_t dockRevision = 0;
    SettingsCard widgetAppearanceCard;
    SettingsCard contextMenuCard;
    SettingsCard layoutCard;
    SettingsCard behaviorCard;
    AppearanceSections appearanceSections;
    std::shared_ptr<PanelGradientEditor> panelGradientEditor;
    std::shared_ptr<EdgeLightEditor> edgeLightEditor;
    std::vector<std::pair<SettingRow*, muxc::Button>> appearanceResets;
    bool panelGradientEnabled = false;

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
    muxc::ToggleSwitch topTitleBarToggle{nullptr};
    SettingRow topTitleBarRow;
    winrt::event_token topTitleBarToken{};
    muxc::ToggleSwitch widgetTransformCursors{nullptr};
    SettingRow widgetTransformCursorsRow;
    winrt::event_token widgetTransformCursorsToken{};
    ContinuousControl luaWidgetContentRowHeight;
    muxc::ToggleSwitch showGroupTabCounts{nullptr};
    SettingRow showGroupTabCountsRow;
    muxc::ToggleSwitch popupHoverOpen{nullptr};
    SettingRow popupHoverOpenRow;
    winrt::event_token popupHoverOpenToken{};
    ContinuousControl popupHoverDelayMs;

    SettingRow presetRow;
    SettingRow quickNavigationThemeRow;
    SettingRow collectionPopupThemeRow;
    SettingRow gradientToggleRow;
    SettingRow edgeHighlightRow;
    SettingRow glassRow;
    SettingRow acrylicRow;
    SettingRow contentThemeRow;
    SettingRow contextMenuRow;

    std::array<ContinuousControl*, 12> continuousControls = {
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
        &popupHoverDelayMs,
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
    winrt::event_token showGroupTabCountsToken{};

    [[nodiscard]] std::wstring L(
        std::string_view key,
        std::wstring_view fallback = {}) const
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
        actions.update(generation, mode, [edit = std::move(edit)](auto& settings) {
            const auto previous = settings; edit(settings);
            bool changed = previous.edgeLight != settings.edgeLight || previous.panelGradient != settings.panelGradient || previous.contentTheme != settings.contentTheme || previous.gradientEndA != settings.gradientEndA;
            VisitPanelAppearanceFields([&](auto, auto field, double, double) { changed = changed || (field != &PersonalizationSettings::cornerRadius && previous.*field != settings.*field); });
            VisitPanelAppearanceFlags([&](auto, auto field) { changed = changed || previous.*field != settings.*field; });
            if (settings.backgroundPreset == previous.backgroundPreset && changed) settings.backgroundPreset = kAppearancePresetCustom;
        });
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
        InitializeCard(libraryCard, cardStyle, themeRoot);
        libraryTarget.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        libraryChoice.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        libraryCard.content.Children().Append(libraryTarget);
        libraryCard.content.Children().Append(libraryChoice);
        libraryName.MaxLength(128);
        libraryCard.content.Children().Append(libraryName);
        libraryScopes.Orientation(muxc::Orientation::Horizontal);
        libraryScopes.Spacing(8);
        for (auto& scope : libraryScopeChecks) { scope.IsChecked(true); libraryScopes.Children().Append(scope); }
        libraryCard.content.Children().Append(libraryScopes);
        libraryCard.content.Children().Append(libraryQuick);
        libraryCard.content.Children().Append(libraryPopup);
        libraryCard.content.Children().Append(libraryReplacement);
        libraryCommands.DefaultLabelPosition(muxc::CommandBarDefaultLabelPosition::Right);
        for (auto& button : libraryButtons) libraryCommands.PrimaryCommands().Append(button);
        libraryCard.content.Children().Append(libraryCommands);
        libraryFeedback.IsOpen(false);
        libraryCard.content.Children().Append(libraryFeedback);

        InitializeCard(widgetAppearanceCard, cardStyle, themeRoot);
        appearanceSections.Initialize(widgetAppearanceCard.content, true, true);
        InitializeColorControl(backgroundColor,
            &PersonalizationSettings::widgetBgR,
            &PersonalizationSettings::widgetBgG,
            &PersonalizationSettings::widgetBgB);
        InitializeColorControl(borderColor,
            &PersonalizationSettings::widgetBorderR,
            &PersonalizationSettings::widgetBorderG,
            &PersonalizationSettings::widgetBorderB);
        appearanceSections.colors.Children().Append(backgroundColor.editor.row.root);

        InitializeContinuousControl(widgetAlpha,
            &PersonalizationSettings::widgetAlpha, 0.0, 100.0, 1.0, 0.01);
        InitializeContinuousControl(borderAlpha,
            &PersonalizationSettings::widgetBorderAlpha,
            0.0, 100.0, 1.0, 0.01);
        InitializeContinuousControl(borderWidth,
            &PersonalizationSettings::widgetBorderWidth,
            kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth,
            0.05, 1.0);
        InitializeContinuousControl(edgeHighlightWidth,
            &PersonalizationSettings::widgetEdgeHighlightWidth,
            kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth,
            0.05, 1.0);
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
        appearanceSections.colors.Children().Append(widgetAlpha.row.root);
        appearanceSections.border.Children().Append(borderColor.editor.row.root);
        appearanceSections.border.Children().Append(borderAlpha.row.root);
        appearanceSections.border.Children().Append(borderWidth.row.root);

        edgeHighlightToggle = muxc::ToggleSwitch{};
        edgeHighlightToggle.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        edgeHighlightRow.Initialize(edgeHighlightToggle);
        edgeHighlightRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        appearanceSections.border.Children().Append(edgeHighlightRow.root);
        appearanceSections.border.Children().Append(edgeHighlightWidth.row.root);
        appearanceSections.border.Children().Append(edgeHighlightStrength.row.root);
        edgeLightEditor = EdgeLightEditor::Create([this](auto key) { return L(key, L""); }, [this](auto const& light, bool commit) {
            Emit(commit ? SettingsUpdateMode::PreviewAndCommit : SettingsUpdateMode::Preview, [light](auto& settings) { settings.edgeLight = light; settings.backgroundPreset = kAppearancePresetCustom; });
        });
        appearanceSections.border.Children().Append(edgeLightEditor->Content());

        panelGradientEditor = PanelGradientEditor::Create(
            [this](std::string_view key) { return L(key, L""); },
            [this](const PanelGradient& gradient, bool commit) {
                Emit(commit ? SettingsUpdateMode::PreviewAndCommit : SettingsUpdateMode::Preview,
                    [gradient](PersonalizationSettings& settings) { settings.panelGradient = gradient; });
                panelGradientEnabled = gradient.enabled; UpdateDependentStates();
            }, false, {}, true);
        appearanceSections.colors.Children().InsertAt(0, panelGradientEditor->Content());
        gradientToggle = muxc::ToggleSwitch{};
        gradientToggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
        gradientToggleRow.Initialize(gradientToggle);
        gradientToggleRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        appearanceSections.bottomBar.Children().Append(gradientToggleRow.root);
        appearanceSections.bottomBar.Children().Append(gradientEndAlpha.row.root);

        glassToggle = muxc::ToggleSwitch{};
        glassToggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
        acrylicToggle = muxc::ToggleSwitch{};
        acrylicToggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
        glassRow.Initialize(glassToggle);
        acrylicRow.Initialize(acrylicToggle);
        glassRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        acrylicRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        appearanceSections.material.Children().Append(glassRow.root);
        appearanceSections.material.Children().Append(blurRadius.row.root);
        appearanceSections.material.Children().Append(acrylicRow.root);

        contentThemeCombo = muxc::ComboBox{};
        contentThemeCombo.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        contentThemeCombo.MaxWidth(520.0);
        contentThemeRow.Initialize(contentThemeCombo);
        appearanceSections.text.Children().Append(contentThemeRow.root);

        const auto reset = [this](SettingRow& row, auto action) {
            appearanceResets.emplace_back(&row, presenter_controls::AddRestoreDefaultAction(row,
                L("app.settings.restore_default", L"Restore default"), [this, action] {
                    Emit(SettingsUpdateMode::PreviewAndCommit, action);
                }));
        };
        for (auto* color : colorControls)
            reset(color->editor.row, [color](auto& value) {
                const PersonalizationSettings defaults;
                value.*color->red = defaults.*color->red; value.*color->green = defaults.*color->green; value.*color->blue = defaults.*color->blue;
            });
        reset(glassRow, [](auto& value) { value.glassEnabled = PersonalizationSettings{}.glassEnabled; });
        reset(acrylicRow, [](auto& value) { value.acrylicEnabled = PersonalizationSettings{}.acrylicEnabled; });
        reset(edgeHighlightRow, [](auto& value) { value.widgetEdgeHighlightEnabled = PersonalizationSettings{}.widgetEdgeHighlightEnabled; });
        reset(contentThemeRow, [](auto& value) { value.contentTheme = PersonalizationSettings{}.contentTheme; });
        reset(gradientToggleRow, [](auto& value) { value.gradientEndA = PersonalizationSettings{}.gradientEndA; });
        InitializeCard(contextMenuCard, cardStyle, menuRoot);
        contextMenuCombo = muxc::ComboBox{};
        contextMenuCombo.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        contextMenuCombo.MaxWidth(520.0);
        contextMenuRow.Initialize(contextMenuCombo);
        contextMenuCard.content.Children().Append(contextMenuRow.root);

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


        quickAppearanceEditor = PanelAppearanceEditor::Create(localize,
            [this](const auto& value, bool commit) {
                EmitGeneral(commit ? SettingsUpdateMode::PreviewAndCommit : SettingsUpdateMode::Preview,
                    [value](auto& settings) { settings.quickNavigationAppearance.appearance = value; settings.quickNavigationAppearance.customized = true; settings.quickNavigationAppearance.mode = 4; });
            });
        quickAppearanceContent.Spacing(8);
        quickAppearanceContent.Children().Append(quickAppearanceEditor->Content());
        quickAppearanceOptions = std::make_unique<QuickNavigationOptions>(localize,
            [this](QuickNavigationOptions::Edit edit) {
                if (CanEmit() && actions.updateNavigation)
                    actions.updateNavigation(generation, SettingsUpdateMode::PreviewAndCommit, std::move(edit));
            }, cardStyle, true);
        quickAppearanceContent.Children().Append(quickAppearanceOptions->Content());
        themeTargetsCard.content.Children().Append(quickAppearanceContent);
        InitializeCard(popupThemeCard, cardStyle, themeRoot);
        popupThemeCard.content.Children().Append(collectionPopupThemeRow.root);
        popupAppearanceEditor = PanelAppearanceEditor::Create(localize,
            [this](const auto& value, bool commit) {
                EmitGeneral(commit ? SettingsUpdateMode::PreviewAndCommit : SettingsUpdateMode::Preview,
                    [value](auto& settings) { settings.collectionPopupAppearance.appearance = value; settings.collectionPopupAppearance.customized = true; settings.collectionPopupAppearance.mode = 4; });
            });
        popupThemeCard.content.Children().Append(popupAppearanceEditor->Content());
        dockThemeRoot = muxc::StackPanel{};
        muxc::StackPanel dockBody;
        InitializeCard(dockThemeCard, cardStyle, dockBody);
        dockAppearanceHost.Content(dockBody);
        dockAppearanceHost.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        dockThemeRoot.Children().Append(dockAppearanceHost);
        dockAppearanceCombo = muxc::ComboBox{};
        dockAppearanceCombo.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        dockAppearanceCombo.MaxWidth(520.0);
        dockAppearanceRow.Initialize(dockAppearanceCombo);
        dockThemeCard.content.Children().Append(dockAppearanceRow.root);
        dockAppearanceEditor = PanelAppearanceEditor::Create(localize,
            [this](const auto& value, bool commit) {
                EmitDockAppearance(commit ? SettingsUpdateMode::PreviewAndCommit : SettingsUpdateMode::Preview,
                    [value](auto& settings) { settings.customAppearance = value; settings.appearancePreset = kAppearancePresetCustom; });
            });
        dockThemeCard.content.Children().Append(dockAppearanceEditor->Content());
        InitializeNavigationCard(statusBarLink, statusBarLinkTitle, statusBarLinkDescription);
        InitializeNavigationCard(taskbarLink, taskbarLinkTitle, taskbarLinkDescription);
        InitializeCard(fontCard, cardStyle, themeRoot);
        fontPicker.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        fontPicker.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        fontPicker.MaxWidth(520.0);
        fontPickerLabel.TextTrimming(mux::TextTrimming::CharacterEllipsis);
        fontPicker.Content(fontPickerLabel);
        fontPopup.Width(presenter_controls::kSettingControlWidth);
        fontPopup.Spacing(8.0);
        fontSearch.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        fontResults.SelectionMode(muxc::ListViewSelectionMode::Single);
        fontResults.IsItemClickEnabled(true);
        fontResults.MaxHeight(300.0);
        fontEmpty.TextWrapping(mux::TextWrapping::Wrap);
        fontEmpty.Margin({12.0, 8.0, 12.0, 8.0});
        fontEmpty.Visibility(mux::Visibility::Collapsed);
        fontPopup.Children().Append(fontSearch);
        fontPopup.Children().Append(fontResults);
        fontPopup.Children().Append(fontEmpty);
        for (const auto& button : {fontImportFile, fontImportFolder})
        {
            button.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
            button.HorizontalContentAlignment(mux::HorizontalAlignment::Left);
            fontPopup.Children().Append(button);
        }
        fontFlyout.Content(fontPopup);
        fontFlyout.Placement(muxc::Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft);
        fontPicker.Flyout(fontFlyout);
        fontRow.Initialize(fontPicker);
        fontCard.content.Children().Append(fontRow.root);
        fontRestart.IsClosable(false);
        fontRestart.IsOpen(false);
        fontRestart.Severity(muxc::InfoBarSeverity::Informational);
        fontRestartButton.HorizontalAlignment(mux::HorizontalAlignment::Right);
        fontRestart.ActionButton(fontRestartButton);
        fontCard.content.Children().Append(fontRestart);
        fontError.TextWrapping(mux::TextWrapping::Wrap);
        fontError.Visibility(mux::Visibility::Collapsed);
        fontCard.content.Children().Append(fontError);

        InitializeCard(layoutCard, cardStyle, widgetLayoutRoot);
        InitializeCard(behaviorCard, cardStyle, widgetBehaviorRoot);
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
        topTitleBarToggle = muxc::ToggleSwitch{};
        topTitleBarToggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
        topTitleBarRow.Initialize(topTitleBarToggle);
        topTitleBarRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        behaviorCard.content.Children().Append(topTitleBarRow.root);
        layoutCard.content.Children().Append(categorizedTabHeight.row.root);
        InitializeContinuousControl(luaWidgetContentRowHeight,
            &PersonalizationSettings::luaWidgetContentRowHeight,
            18.0, 48.0, 1.0, 1.0, 28.0);
        SetUnit(luaWidgetContentRowHeight, L"cu");
        layoutCard.content.Children().Append(
            luaWidgetContentRowHeight.row.root);
        widgetTransformCursors = muxc::ToggleSwitch{};
        widgetTransformCursors.HorizontalAlignment(mux::HorizontalAlignment::Right);
        widgetTransformCursorsRow.Initialize(widgetTransformCursors);
        widgetTransformCursorsRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        layoutCard.content.Children().Append(widgetTransformCursorsRow.root);
        showGroupTabCounts = muxc::ToggleSwitch{};
        showGroupTabCounts.HorizontalAlignment(mux::HorizontalAlignment::Right);
        showGroupTabCountsRow.Initialize(showGroupTabCounts);
        showGroupTabCountsRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        behaviorCard.content.Children().Append(showGroupTabCountsRow.root);
        popupHoverOpen = muxc::ToggleSwitch{};
        popupHoverOpen.HorizontalAlignment(mux::HorizontalAlignment::Right);
        popupHoverOpenRow.Initialize(popupHoverOpen);
        popupHoverOpenRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        behaviorCard.content.Children().Append(popupHoverOpenRow.root);
        InitializeContinuousControl(popupHoverDelayMs,
            &PersonalizationSettings::popupHoverDelayMs,
            kMinimumPopupHoverDelayMs, kMaximumPopupHoverDelayMs,
            100.0, 1.0, kDefaultPopupHoverDelayMs);
        SetUnit(popupHoverDelayMs, L"ms");
        behaviorCard.content.Children().Append(popupHoverDelayMs.row.root);
    }

    void InitializeNavigationCard(muxc::Button& button, muxc::TextBlock& title, muxc::TextBlock& description)
    {
        button = muxc::Button{};
        if (navigationStyle) button.Style(navigationStyle);
        muxc::Grid content;
        content.ColumnSpacing(20);
        muxc::ColumnDefinition textColumn, actionColumn;
        textColumn.Width(mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star));
        actionColumn.Width(mux::GridLengthHelper::Auto());
        content.ColumnDefinitions().Append(textColumn);
        content.ColumnDefinitions().Append(actionColumn);
        muxc::StackPanel text;
        text.Spacing(4);
        title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        title.TextWrapping(mux::TextWrapping::Wrap);
        description.TextWrapping(mux::TextWrapping::Wrap);
        description.Opacity(0.68);
        text.Children().Append(title);
        text.Children().Append(description);
        content.Children().Append(text);
        muxc::FontIcon chevron;
        chevron.Glyph(L"\xE76C");
        chevron.FontSize(14);
        muxc::Grid::SetColumn(chevron, 1);
        content.Children().Append(chevron);
        button.Content(content);
        themeRoot.Children().Append(button);
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
        if (!std::isfinite(defaultValue)) defaultValue = PersonalizationSettings{}.*member / scale;
        control.defaultValue = defaultValue;
        control.editors.Children().Append(control.slider);
        muxc::Grid::SetColumn(control.number, 1);
        control.editors.Children().Append(control.number);
        muxc::Grid::SetColumn(control.unit, 2);
        control.editors.Children().Append(control.unit);
        if (std::isfinite(control.defaultValue))
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

    std::string LibraryTarget() const
    {
        const int index = libraryTarget.SelectedIndex();
        return libraryTargets[index >= 0 && index < 6 ? index : 0];
    }

    std::wstring ThemeLabel(const themes::Theme& theme) const
    {
        if (!themes::Builtin(theme.id)) return winrt::to_hstring(theme.name).c_str();
        const std::map<std::string, std::string> keys = {{"dark", "app.settings.dark"}, {"light", "app.settings.light"},
            {"glass-dark", "app.settings.dark_glass"}, {"glass-light", "app.settings.light_glass"},
            {"glass-transparent", "app.settings.transparent_glass"}, {"acrylic-dark", "app.settings.dark_acrylic"},
            {"acrylic-light", "app.settings.light_acrylic"}};
        const auto found = keys.find(theme.name);
        return found == keys.end() ? winrt::to_hstring(theme.name).c_str() : L(found->second, winrt::to_hstring(theme.name).c_str());
    }

    void RefreshLibraryChoices(std::string preferred = {})
    {
        updatingLibrary = true;
        const auto target = LibraryTarget();
        libraryChoices = themes::Choices(savedThemes, themes::TargetKind(target), themes::TargetScope(target));
        libraryQuickChoices = themes::Choices(savedThemes, themes::Kind::QuickPanel);
        libraryPopupChoices = themes::Choices(savedThemes, themes::Kind::Popup);
        libraryChoice.Items().Clear(); libraryReplacement.Items().Clear();
        libraryReplacement.Items().Append(winrt::box_value(L("themeLibrary.preserve", L"Keep current appearance as custom")));
        int selected = -1;
        if (preferred.empty())
            if (const auto found = savedThemes.references.find(target); found != savedThemes.references.end()) preferred = found->second.id;
        for (std::size_t i = 0; i < libraryChoices.size(); ++i)
        {
            libraryChoice.Items().Append(winrt::box_value(ThemeLabel(libraryChoices[i])));
            libraryReplacement.Items().Append(winrt::box_value(ThemeLabel(libraryChoices[i])));
            if (libraryChoices[i].id == preferred) selected = static_cast<int>(i);
        }
        libraryChoice.SelectedIndex(selected);
        libraryReplacement.SelectedIndex(0);
        libraryQuick.Items().Clear(); libraryPopup.Items().Clear();
        libraryQuick.Items().Append(winrt::box_value(L("themeLibrary.currentQuick", L"Save current quick panel appearance")));
        libraryPopup.Items().Append(winrt::box_value(L("themeLibrary.currentPopup", L"Save current popup appearance")));
        for (const auto& theme : libraryQuickChoices) libraryQuick.Items().Append(winrt::box_value(ThemeLabel(theme)));
        for (const auto& theme : libraryPopupChoices) libraryPopup.Items().Append(winrt::box_value(ThemeLabel(theme)));
        libraryQuick.SelectedIndex(0); libraryPopup.SelectedIndex(0);
        const auto visibility = themes::TargetKind(target) == themes::Kind::Global ? mux::Visibility::Visible : mux::Visibility::Collapsed;
        libraryScopes.Visibility(visibility); libraryQuick.Visibility(visibility); libraryPopup.Visibility(visibility);
        updatingLibrary = false;
        PatchLibraryChoice();
    }

    void PatchLibraryChoice()
    {
        if (updatingLibrary) return;
        const int index = libraryChoice.SelectedIndex();
        const themes::Theme* selected = index >= 0 && static_cast<std::size_t>(index) < libraryChoices.size() ? &libraryChoices[index] : nullptr;
        libraryName.Text(selected ? ThemeLabel(*selected) : L"");
        for (std::size_t i = 0; i < libraryScopeChecks.size(); ++i)
            libraryScopeChecks[i].IsChecked(!selected || (selected->scopes & (1u << i)) != 0);
        const auto selectBinding = [](auto& combo, const auto& choices, const std::string& id) {
            int chosen = 0;
            for (std::size_t i = 0; i < choices.size(); ++i) if (choices[i].id == id) chosen = static_cast<int>(i + 1);
            combo.SelectedIndex(chosen);
        };
        if (selected && selected->kind == themes::Kind::Global)
        { selectBinding(libraryQuick, libraryQuickChoices, selected->quickPanel); selectBinding(libraryPopup, libraryPopupChoices, selected->popup); }
        libraryButtons[1].IsEnabled(selected && !themes::Builtin(selected->id));
        libraryButtons[2].IsEnabled(selected != nullptr);
        libraryButtons[3].IsEnabled(selected && !themes::Builtin(selected->id));
        libraryButtons[4].IsEnabled(selected != nullptr);
    }

    void LibraryAction(ThemeLibraryCommand command)
    {
        if (!CanEmit() || !actions.themeLibrary) return;
        CommitContinuousEdits(); CommitOpenColorEditors();
        quickAppearanceEditor->Flush(); popupAppearanceEditor->Flush();
        ThemeLibraryRequest request; request.command = command; request.target = LibraryTarget();
        const int index = libraryChoice.SelectedIndex();
        if (index >= 0 && static_cast<std::size_t>(index) < libraryChoices.size()) request.id = libraryChoices[index].id;
        request.name = winrt::to_string(libraryName.Text()); request.scopes = 0;
        for (std::size_t i = 0; i < libraryScopeChecks.size(); ++i)
        {
            const auto checked = libraryScopeChecks[i].IsChecked();
            if (checked && checked.Value()) request.scopes |= 1u << i;
        }
        const auto binding = [](auto& combo, const auto& choices) -> std::string {
            const int selected = combo.SelectedIndex();
            return selected > 0 && static_cast<std::size_t>(selected) <= choices.size() ? choices[selected - 1].id : std::string{};
        };
        request.quickPanel = binding(libraryQuick, libraryQuickChoices); request.popup = binding(libraryPopup, libraryPopupChoices);
        request.replacement = binding(libraryReplacement, libraryChoices);
        const auto result = actions.themeLibrary(generation, request);
        if (result.succeeded)
        {
            savedThemes = result.library;
            if (!result.savedId.empty()) request.id = result.savedId;
            RefreshLibraryChoices(command == ThemeLibraryCommand::Remove ? std::string{} : request.id);
        }
        libraryFeedback.Severity(result.succeeded ? muxc::InfoBarSeverity::Success : muxc::InfoBarSeverity::Error);
        libraryFeedback.Message(result.message); libraryFeedback.IsOpen(!result.message.empty());
    }

    void HookEvents()
    {
        libraryTargetToken = libraryTarget.SelectionChanged([this](const auto&, const auto&) { if (!updatingLibrary) RefreshLibraryChoices(); });
        libraryChoiceToken = libraryChoice.SelectionChanged([this](const auto&, const auto&) { PatchLibraryChoice(); });
        constexpr ThemeLibraryCommand commands[] = {ThemeLibraryCommand::SaveAs, ThemeLibraryCommand::Update,
            ThemeLibraryCommand::Apply, ThemeLibraryCommand::Remove, ThemeLibraryCommand::Export};
        for (std::size_t i = 0; i < libraryButtons.size(); ++i)
            libraryButtonTokens[i] = libraryButtons[i].Click([this, command = commands[i]](const auto&, const auto&) { LibraryAction(command); });
        fontOpenToken = fontFlyout.Opened([this](const auto&, const auto&) {
            if (!CanEmit()) return;
            // Rescan newly installed fonts, but never commit a search query or
            // replace a saved font just because the dropdown is opened.
            updatingControls = true;
            fontSearch.Text(L"");
            updatingControls = false;
            fontPopup.Width(std::clamp(fontPicker.ActualWidth(), 240.0, 520.0));
            RefreshFonts();
            fontSearch.Focus(mux::FocusState::Programmatic);
        });
        fontSearchToken = fontSearch.TextChanged([this](const auto&, const auto&) {
            if (CanEmit()) FilterFonts();
        });
        fontSearchKeyToken = fontSearch.KeyDown([this](const auto&, const muxi::KeyRoutedEventArgs& args) {
            if (!CanEmit() || visibleFonts.empty()) return;
            if (args.Key() == winrt::Windows::System::VirtualKey::Down)
            {
                if (fontResults.SelectedIndex() < 0) fontResults.SelectedIndex(0);
                fontResults.Focus(mux::FocusState::Keyboard);
                fontResults.ScrollIntoView(fontResults.SelectedItem());
                args.Handled(true);
            }
            else if (IsEnter(args))
            {
                ChooseFont(fontResults.SelectedIndex() < 0 ? 0 : fontResults.SelectedIndex());
                args.Handled(true);
            }
        });
        fontResultToken = fontResults.ItemClick([this](const auto&, const muxc::ItemClickEventArgs& args) {
            uint32_t index = 0;
            if (fontResults.Items().IndexOf(args.ClickedItem(), index)) ChooseFont(static_cast<int>(index));
        });
        fontResultKeyToken = fontResults.KeyDown([this](const auto&, const muxi::KeyRoutedEventArgs& args) {
            if (IsEnter(args))
            {
                ChooseFont(fontResults.SelectedIndex());
                args.Handled(true);
            }
        });
        fontImportFileToken = fontImportFile.Click([this](const auto&, const auto&) {
            fontFlyout.Hide();
            ImportFonts(false);
        });
        fontImportFolderToken = fontImportFolder.Click([this](const auto&, const auto&) {
            fontFlyout.Hide();
            ImportFonts(true);
        });
        fontRestartToken = fontRestartButton.Click([this](const auto&, const auto&) {
            if (CanEmit() && fontRestart.IsOpen() && actions.restartApplication)
                actions.restartApplication(generation);
        });
        presetToken = presetCombo.SelectionChanged(
            [this](const auto&, const auto&) {
                UpdateDependentStates();
                if (!CanEmit())
                    return;
                edgeLightEditor->Cancel();
                const int index = presetCombo.SelectedIndex();
                if (index < 0 ||
                    static_cast<std::size_t>(index) >= kPresetIds.size())
                    return;
                const int preset =
                    kPresetIds[static_cast<std::size_t>(index)];
                currentBackgroundPreset = preset;
                Emit(SettingsUpdateMode::PreviewAndCommit,
                    [preset](PersonalizationSettings& settings) {
                        if (preset == kAppearancePresetCustom)
                        {
                            settings.backgroundPreset =
                                kAppearancePresetCustom;
                            return;
                        }
                        ApplyAppearancePreset(settings, preset);
                    });
            });
        quickNavigationThemeToken = quickNavigationThemeCombo.SelectionChanged(
            [this](auto const&, auto const&) { SelectSurfaceTheme(true, quickNavigationThemeCombo.SelectedIndex()); });
        collectionPopupThemeToken = collectionPopupThemeCombo.SelectionChanged(
            [this](auto const&, auto const&) { SelectSurfaceTheme(false, collectionPopupThemeCombo.SelectedIndex()); });
        dockAppearanceToken = dockAppearanceCombo.SelectionChanged([this](auto const&, auto const&) {
            if (!CanEmit()) return;
            const int index = dockAppearanceCombo.SelectedIndex();
            if (index < 0 || index > static_cast<int>(kPresetIds.size())) return;
            dockAppearanceEditor->Flush();
            const auto global = currentGlobalAppearance;
            EmitDockAppearance(SettingsUpdateMode::PreviewAndCommit, [index, global](auto& settings) {
                SelectDockAppearance(settings, index == 0,
                    index > 0 ? kPresetIds[index - 1] : settings.appearancePreset, global);
            });
        });
        statusBarLinkToken = statusBarLink.Click([this](const auto&, const auto&) {
            if (!closed && active && actions.navigate)
                actions.navigate(SettingsRoute::ForPage(SettingsPage::StatusBar, "statusBar.theme"));
        });
        taskbarLinkToken = taskbarLink.Click([this](auto const&, auto const&) {
            if (!closed && active && actions.navigate) actions.navigate(SettingsRoute::ForPage(SettingsPage::Taskbar, "taskbar.theme"));
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
                        settings.contextMenuStyle = std::clamp(value, 0, 6);
                    });
            });
        showGroupTabCountsToken = showGroupTabCounts.Toggled(
            [this](const auto&, const auto&) {
                const bool enabled = showGroupTabCounts.IsOn();
                Emit(SettingsUpdateMode::PreviewAndCommit,
                    [enabled](PersonalizationSettings& settings) {
                        settings.showGroupTabCounts = enabled;
                    });
            });
        topTitleBarToken = topTitleBarToggle.Toggled(
            [this](const auto&, const auto&) {
                const bool enabled = topTitleBarToggle.IsOn();
                Emit(SettingsUpdateMode::PreviewAndCommit,
                    [enabled](PersonalizationSettings& settings) {
                        settings.scrollableTitleBarOnTop = enabled;
                    });
            });
        popupHoverOpenToken = popupHoverOpen.Toggled(
            [this](const auto&, const auto&) {
                UpdateDependentStates();
                const bool enabled = popupHoverOpen.IsOn();
                Emit(SettingsUpdateMode::PreviewAndCommit,
                    [enabled](PersonalizationSettings& settings) {
                        settings.popupHoverOpen = enabled;
                    });
            });
        widgetTransformCursorsToken = widgetTransformCursors.Toggled(
            [this](const auto&, const auto&) {
                const bool enabled = widgetTransformCursors.IsOn();
                Emit(SettingsUpdateMode::PreviewAndCommit,
                    [enabled](PersonalizationSettings& settings) {
                        settings.widgetTransformCursors = enabled;
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
        presenter_controls::SyncNumberBoxValue(control.number, clamped);
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
        if (currentBackgroundPreset != normalized && normalized == kAppearancePresetCustom)
            appearanceSections.CollapseAll();
        currentBackgroundPreset = normalized;
        auto preset = std::find(kPresetIds.begin(), kPresetIds.end(), normalized);
        presetCombo.SelectedIndex(preset == kPresetIds.end()
            ? static_cast<int>(kPresetIds.size() - 1)
            : static_cast<int>(std::distance(kPresetIds.begin(), preset)));
        PatchColor(backgroundColor, settings);
        PatchColor(borderColor, settings);
        for (ContinuousControl* control : continuousControls)
            PatchContinuous(*control, settings);
        panelGradientEnabled = settings.panelGradient.enabled;
        panelGradientEditor->SetValue(settings.panelGradient);
        gradientToggle.IsOn(settings.gradientEndA > 0.001f);
        glassToggle.IsOn(settings.glassEnabled);
        acrylicToggle.IsOn(settings.acrylicEnabled);
        edgeHighlightToggle.IsOn(settings.widgetEdgeHighlightEnabled);
        edgeLightEditor->SetValue(settings.edgeLight);
        showGroupTabCounts.IsOn(settings.showGroupTabCounts);
        popupHoverOpen.IsOn(settings.popupHoverOpen);
        topTitleBarToggle.IsOn(settings.scrollableTitleBarOnTop);
        widgetTransformCursors.IsOn(settings.widgetTransformCursors);
        contentThemeCombo.SelectedIndex(
            std::clamp(settings.contentTheme, 0, 1));
        contextMenuCombo.SelectedIndex(
            std::clamp(settings.contextMenuStyle, 0, 6));
        UpdateDependentStates();
    }

    void UpdateFontRestartNotice()
    {
        const auto applied = app_fonts::current.load();
        const auto hostFont = actions.appliedFont ? actions.appliedFont()
            : (applied ? applied->selection : app_fonts::Selection{});
        fontRestart.IsOpen(selectedFont != hostFont);
    }

    std::wstring FontChoiceName(const app_fonts::Choice& choice) const
    {
        auto name = choice.selection.package == "system" ? L("font.system", L"System default") : choice.name;
        if (choice.selection.package == "installed") name += L" · " + L("font.installed", L"Installed");
        return name;
    }

    void FilterFonts()
    {
        visibleFonts = font_picker::Filter(fonts, fontSearch.Text().c_str(), L("font.system", L"System default"));
        fontResults.Items().Clear();
        int selected = -1;
        for (std::size_t i = 0; i < visibleFonts.size(); ++i)
        {
            const auto& choice = fonts[visibleFonts[i]];
            fontResults.Items().Append(winrt::box_value(FontChoiceName(choice)));
            if (choice.selection == selectedFont) selected = static_cast<int>(i);
        }
        fontResults.SelectedIndex(selected);
        fontResults.Visibility(visibleFonts.empty() ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        fontEmpty.Visibility(visibleFonts.empty() ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    }

    void ChooseFont(int index)
    {
        if (!CanEmit() || index < 0 || static_cast<std::size_t>(index) >= visibleFonts.size()) return;
        const auto& choice = fonts[visibleFonts[static_cast<std::size_t>(index)]];
        selectedFont = choice.selection;
        fontPickerLabel.Text(FontChoiceName(choice));
        fontFlyout.Hide();
        EmitGeneral(SettingsUpdateMode::PreviewAndCommit, [selection = selectedFont](auto& settings) { settings.font = selection; });
        UpdateFontRestartNotice();
    }

    void RefreshFonts()
    {
        if (!actions.listFonts) return;
        const bool previous = updatingControls;
        updatingControls = true;
        fonts = actions.listFonts();
        const auto selected = std::find_if(fonts.begin(), fonts.end(), [this](const auto& choice) { return choice.selection == selectedFont; });
        // Missing packages retain the saved selection. Merely opening settings
        // must never replace an unavailable user font with the system default.
        if (selected == fonts.end())
        {
            fonts.push_back({selectedFont, L("font.unavailable", L"Saved font unavailable"), {}});
            fontPickerLabel.Text(fonts.back().name);
        }
        else fontPickerLabel.Text(FontChoiceName(*selected));
        FilterFonts();
        updatingControls = previous;
        UpdateFontRestartNotice();
    }

    void ImportFonts(bool folder)
    {
        if (!CanEmit() || !actions.importFonts) return;
        std::string error;
        const auto imported = actions.importFonts(folder, error);
        fontError.Text(error.empty() ? L"" : L(error));
        fontError.Visibility(error.empty() ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        if (imported.empty()) return;
        selectedFont = imported.front().selection;
        RefreshFonts();
        EmitGeneral(SettingsUpdateMode::PreviewAndCommit, [selection = selectedFont](auto& settings) { settings.font = selection; });
    }

    void PatchGeneral(const GeneralSettings& settings)
    {
        if (selectedFont != settings.font || fonts.empty())
        {
            selectedFont = settings.font;
            RefreshFonts();
        }
        const auto index = [this](const SurfaceTheme& theme, int legacy) {
            if (theme.mode != -2) return theme.mode + 1;
            return currentGlobalAppearance.backgroundPreset == kAppearancePresetCustom ? NormalizeFourThemeSelection(legacy) + 1 : 0;
        };
        quickNavigationThemeCombo.SelectedIndex(index(settings.quickNavigationAppearance, settings.quickNavTheme));
        collectionPopupThemeCombo.SelectedIndex(index(settings.collectionPopupAppearance, settings.collectionPopupTheme));
        quickAppearanceContent.Visibility(IsCustomSurfaceTheme(settings.quickNavigationAppearance, currentGlobalAppearance) ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        popupAppearanceEditor->Content().Visibility(IsCustomSurfaceTheme(settings.collectionPopupAppearance, currentGlobalAppearance) ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    }

    void SelectSurfaceTheme(bool quick, int index)
    {
        if (!CanEmit() || index < 0 || index > 5) return;
        (quick ? quickAppearanceEditor : popupAppearanceEditor)->Flush();
        const auto global = currentGlobalAppearance;
        EmitGeneral(SettingsUpdateMode::PreviewAndCommit, [quick, index, global](auto& settings) {
            auto& theme = quick ? settings.quickNavigationAppearance : settings.collectionPopupAppearance;
            SelectSurfaceThemeMode(theme, index - 1,
                ResolveSurfaceTheme(theme, global, quick ? settings.quickNavTheme : settings.collectionPopupTheme, quick,
                    quick ? &settings.globalQuickNavigationAppearance : &settings.globalCollectionPopupAppearance));
        });
    }

    template<class Edit> void EmitDockAppearance(SettingsUpdateMode mode, Edit edit)
    {
        if (CanEmit() && actions.updateDock) actions.updateDock(generation, mode, std::move(edit));
    }

    void UpdateDependentStates()
    {
        if (closed)
            return;
        const bool custom = currentBackgroundPreset == kAppearancePresetCustom;
        themeTargetsCard.root.Visibility(mux::Visibility::Visible);
        widgetAppearanceCard.root.Visibility(custom
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
        backgroundColor.editor.row.root.Visibility(panelGradientEnabled ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        appearanceSections.PlaceOpacity(widgetAlpha.row.root, panelGradientEnabled);
        widgetAlpha.row.root.Visibility(panelGradientEnabled && !gradientToggle.IsOn() ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        widgetAlpha.row.SetText(L(panelGradientEnabled ? "panelGradient.barStartOpacity" : "app.settings.bg_opacity", L"Background opacity"));
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
        const auto visible = [](bool value) { return value ? mux::Visibility::Visible : mux::Visibility::Collapsed; };
        edgeHighlightWidth.row.root.Visibility(visible(edgeHighlightToggle.IsOn()));
        edgeHighlightStrength.row.root.Visibility(visible(edgeHighlightToggle.IsOn()));
        edgeLightEditor->Content().Visibility(visible(edgeHighlightToggle.IsOn()));
        gradientEndAlpha.row.root.Visibility(visible(gradientToggle.IsOn()));
        blurRadius.row.root.Visibility(visible(glassToggle.IsOn()));
        acrylicRow.root.Visibility(visible(glassToggle.IsOn()));
        popupHoverDelayMs.row.root.Visibility(visible(popupHoverOpen.IsOn()));
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
        const bool previousLibrary = updatingLibrary;
        updatingLibrary = true;
        const int target = libraryTarget.SelectedIndex();
        libraryCard.title.Text(L("themeLibrary.title", L"Saved themes"));
        libraryTarget.Items().Clear();
        for (const auto name : libraryTargets) libraryTarget.Items().Append(winrt::box_value(L("themeLibrary." + std::string(name))));
        libraryTarget.SelectedIndex(target < 0 ? 0 : target);
        libraryName.Header(winrt::box_value(L("themeLibrary.name")));
        libraryChoice.Header(winrt::box_value(L("themeLibrary.choose")));
        libraryQuick.Header(winrt::box_value(L("themeLibrary.quickPanel")));
        libraryPopup.Header(winrt::box_value(L("themeLibrary.popup")));
        libraryReplacement.Header(winrt::box_value(L("themeLibrary.replacement")));
        constexpr const char* labels[] = {"saveAs", "update", "apply", "remove", "export"};
        for (std::size_t i = 0; i < libraryButtons.size(); ++i) libraryButtons[i].Label(L("themeLibrary." + std::string(labels[i])));
        constexpr const char* scopes[] = {"components", "dock", "statusBar", "taskbar"};
        for (std::size_t i = 0; i < libraryScopeChecks.size(); ++i) libraryScopeChecks[i].Content(winrt::box_value(L("themeLibrary." + std::string(scopes[i]))));
        updatingLibrary = previousLibrary;
        RefreshLibraryChoices();
        edgeLightEditor->RefreshLocalizedText();
        appearanceSections.RefreshLocalizedText([this](auto key) { return L(key, L""); });
        if (panelGradientEditor) panelGradientEditor->RefreshLocalizedText();
        if (closed)
            return;
        const bool previousUpdating = updatingControls;
        updatingControls = true;

        SetCardText(fontCard, "font.title", L"Interface font");
        fontRow.SetText(L("font.family", L"Font"), L("font.hint"));
        muxa::AutomationProperties::SetName(fontPicker, fontRow.label.Text());
        fontSearch.PlaceholderText(L("font.search", L"Search fonts"));
        muxa::AutomationProperties::SetName(fontSearch, fontSearch.PlaceholderText());
        muxa::AutomationProperties::SetName(fontResults, fontRow.label.Text());
        fontEmpty.Text(L("font.noResults", L"No matching fonts"));
        fontImportFile.Content(winrt::box_value(L("font.importFile")));
        fontImportFolder.Content(winrt::box_value(L("font.importFolder")));
        fontRestartButton.Content(winrt::box_value(L("font.restartNow", L"Restart now")));
        fontRestart.Message(L("font.restartRequired", L"Restart SnowDesktop to apply this font."));
        RefreshFonts();
        SetCardText(themeCard,
            "app.settings.global_theme", L"Global Theme");
        SetCardText(themeTargetsCard, "appearance.quickPanelCard", L"Quick panel theme");
        SetCardText(popupThemeCard, "appearance.popupCard", L"Popup theme");
        SetCardText(dockThemeCard, "settings.dock.dock", L"Dock");
        statusBarLinkTitle.Text(L("settings.nav.statusBar", L"Status bar"));
        statusBarLinkDescription.Text(L("appearance.openStatusBar", L"Open status bar settings"));
        taskbarLinkTitle.Text(L("settings.dock.taskbar", L"Taskbar"));
        taskbarLinkDescription.Text(L("appearance.openTaskbar", L"Open taskbar settings"));
        muxa::AutomationProperties::SetName(statusBarLink, statusBarLinkDescription.Text());
        muxa::AutomationProperties::SetName(taskbarLink, taskbarLinkDescription.Text());
        dockAppearanceRow.SetText(L("app.settings.theme", L"Theme"));
        ReplaceComboItems(dockAppearanceCombo, {{"app.settings.taskbar_follow_global", L"Follow global theme"},
            {"app.settings.dark", L"Dark"}, {"app.settings.light", L"Light"},
            {"app.settings.dark_glass", L"Dark glass"}, {"app.settings.light_glass", L"Light glass"},
            {"app.settings.transparent_glass", L"Transparent glass"},
            {"app.settings.dark_acrylic", L"Dark acrylic"}, {"app.settings.light_acrylic", L"Light acrylic"},
            {"app.settings.custom", L"Custom"}});
        quickAppearanceEditor->RefreshLocalizedText(); popupAppearanceEditor->RefreshLocalizedText(); dockAppearanceEditor->RefreshLocalizedText();
        quickAppearanceOptions->RefreshText();
        SetCardText(widgetAppearanceCard,
            "app.settings.component_bg", L"Widget Appearance");
        SetCardText(contextMenuCard,
            "app.settings.context_menu_appearance", L"Context Menu");
        SetCardText(layoutCard,
            "app.settings.widget_layout", L"Widget Layout");
        SetCardText(behaviorCard,
            "settings.widgetBehavior.title", L"Widget behavior");

        presetRow.SetText(L("app.settings.theme", L"Theme"));
        ReplaceComboItems(presetCombo, {
            {"app.settings.dark", L"Dark"},
            {"app.settings.light", L"Light"},
            {"app.settings.dark_glass", L"Dark Glass"},
            {"app.settings.light_glass", L"Light Glass"},
            {"app.settings.transparent_glass", L"Transparent glass"},
            {"app.settings.dark_acrylic", L"Dark Acrylic"},
            {"app.settings.light_acrylic", L"Light Acrylic"},
            {"app.settings.custom", L"Custom"},
        });
        muxa::AutomationProperties::SetName(
            presetCombo, L("app.settings.theme", L"Theme"));

        quickNavigationThemeRow.SetText(L("app.settings.theme", L"Theme"));
        collectionPopupThemeRow.SetText(L("app.settings.theme", L"Theme"));
        const std::initializer_list<std::pair<
            std::string_view, std::wstring_view>> themeChoices = {
            {"appearance.followGlobalPreset", L"Global theme preset (default)"},
            {"app.settings.dark", L"Dark"},
            {"app.settings.light", L"Light"},
            {"app.settings.dark_acrylic", L"Dark Acrylic"},
            {"app.settings.light_acrylic", L"Light Acrylic"},
            {"app.settings.custom", L"Custom"},
        };
        ReplaceComboItems(quickNavigationThemeCombo, themeChoices);
        ReplaceComboItems(collectionPopupThemeCombo, themeChoices);
        muxa::AutomationProperties::SetName(quickNavigationThemeCombo,
            quickNavigationThemeRow.label.Text());
        muxa::AutomationProperties::SetName(collectionPopupThemeCombo,
            collectionPopupThemeRow.label.Text());

        presetRow.help.Text(L("appearance.presetHelp", L"")); presetRow.help.Visibility(mux::Visibility::Visible);
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
            {"appearance.lightText", L"Light text"},
            {"appearance.darkText", L"Dark text"},
        });

        contextMenuRow.SetText(
            L("app.settings.context_menu_style", L"Menu Style"));
        ReplaceComboItems(contextMenuCombo, {
            {"app.settings.context_menu_follow_system", L"Follow System"},
            {"app.settings.context_menu_system_light_blur", L"Light"},
            {"app.settings.context_menu_system_dark_blur", L"Dark"},
            {"app.settings.context_menu_opaque_light", L"Light (Opaque)"},
            {"app.settings.context_menu_opaque_dark", L"Dark (Opaque)"},
            {"app.settings.context_menu_win10_light", L"Win10 Light"},
            {"app.settings.context_menu_win10_dark", L"Win10 Dark"},
        });

        SetContinuousText(cornerRadius,
            "app.settings.corner_radius", L"Corner Radius");
        SetContinuousText(barHeight,
            "app.settings.bar_height", L"Bar Height");
        SetContinuousText(categorizedTabHeight,
            "app.settings.tab_height", L"Top Bar, Tab and Search Box Height");
        topTitleBarRow.SetText(
            L("app.settings.scrollable_title_bar_position",
                L"Use top title bars for storage widgets"),
            L("app.settings.scrollable_title_bar_position_hint",
                L"Place titles and actions at the top, except in large-folder mode."));
        muxa::AutomationProperties::SetName(
            topTitleBarToggle, topTitleBarRow.label.Text());
        SetContinuousText(luaWidgetContentRowHeight,
            "app.settings.lua_widget_row_height",
            L"Lua Widget Row Height");
        widgetTransformCursorsRow.SetText(
            L("app.settings.widget_transform_cursors",
                L"Show component move and resize cursors"),
            L("app.settings.widget_transform_cursors_hint",
                L"Change the pointer when hovering over or dragging component move and resize handles. Turn off to use the standard pointer."));
        muxa::AutomationProperties::SetName(
            widgetTransformCursors, widgetTransformCursorsRow.label.Text());
        showGroupTabCountsRow.SetText(
            L("app.settings.group_show_count", L"Show file counts on group tabs"),
            L("app.settings.group_show_count_hint",
                L"Applies to collection group and file group tabs."));
        muxa::AutomationProperties::SetName(
            showGroupTabCounts, showGroupTabCountsRow.label.Text());
        popupHoverOpenRow.SetText(
            L("app.settings.popup_hover_open", L"Open popups on hover"),
            L("app.settings.popup_hover_open_hint",
                L"Hover over a Dock folder or collection, or a collection's expand button, to open its popup after the configured delay."));
        muxa::AutomationProperties::SetName(
            popupHoverOpen, popupHoverOpenRow.label.Text());
        SetContinuousText(popupHoverDelayMs,
            "app.settings.popup_hover_delay", L"Hover delay");
        muxa::AutomationProperties::SetName(
            gradientToggle, gradientToggleRow.label.Text());
        const auto explain = [](auto& row, const std::wstring& text) { row.help.Text(text); row.help.Visibility(mux::Visibility::Visible); };
        explain(widgetAlpha.row, L("appearance.opacityHelp", L""));
        explain(glassRow, L("appearance.glassHelp", L""));
        explain(edgeHighlightRow, L("appearance.borderHelp", L""));
        explain(edgeHighlightWidth.row, L("appearance.rimWidthHelp", L""));
        for (auto const& [row, button] : appearanceResets) presenter_controls::ConfigureRestoreDefaultButton(button, L("app.settings.restore_default", L"Restore default") + L" · " + std::wstring(row->label.Text()));
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
        const bool dockChanged = newGeneration || snapshot.domainRevisions.dock != dockRevision;
        const bool navigationChanged = newGeneration || snapshot.domainRevisions.navigation != navigationRevision;
        if (!personalizationChanged && !generalChanged && !dockChanged && !navigationChanged)
        {
            hasSnapshot = true;
            return;
        }

        const bool previousUpdating = updatingControls;
        updatingControls = true;
        if (newGeneration)
        {
            edgeLightEditor->Cancel();
            for (ContinuousControl* control : continuousControls)
            {
                if (control->idleCommitTimer)
                    control->idleCommitTimer.Stop();
                control->dirty = false;
            }
        }
        dockAppearanceHost.IsEnabled(snapshot.values.general.dockEnabled);
        currentGlobalAppearance = snapshot.values.personalization;
        if (personalizationChanged)
        {
            Patch(snapshot.values.personalization);
            personalizationRevision =
                snapshot.domainRevisions.personalization;
        }
        if (generalChanged || personalizationChanged)
        {
            quickAppearanceEditor->SetValue(ResolveSurfaceTheme(snapshot.values.general.quickNavigationAppearance, currentGlobalAppearance, snapshot.values.general.quickNavTheme, true, &snapshot.values.general.globalQuickNavigationAppearance), newGeneration);
            popupAppearanceEditor->SetValue(ResolveSurfaceTheme(snapshot.values.general.collectionPopupAppearance, currentGlobalAppearance, snapshot.values.general.collectionPopupTheme, false, &snapshot.values.general.globalCollectionPopupAppearance), newGeneration);
            PatchGeneral(snapshot.values.general);
            generalRevision = snapshot.domainRevisions.general;
        }
        if (dockChanged)
        {
            const auto& dock = snapshot.values.dock;
            const auto found = std::find(kPresetIds.begin(), kPresetIds.end(), dock.appearancePreset);
            dockAppearanceCombo.SelectedIndex(dock.followComponentAppearance ? 0 :
                found == kPresetIds.end() ? static_cast<int>(kPresetIds.size()) : static_cast<int>(found - kPresetIds.begin()) + 1);
            dockAppearanceEditor->SetValue(ResolveDockAppearance(dock, currentGlobalAppearance), newGeneration);
            dockAppearanceEditor->Content().Visibility(!dock.followComponentAppearance && dock.appearancePreset == kAppearancePresetCustom ? mux::Visibility::Visible : mux::Visibility::Collapsed);
            dockRevision = snapshot.domainRevisions.dock;
        }
        if (navigationChanged || generalChanged || personalizationChanged)
        {
            const auto appearance = ResolveSurfaceTheme(snapshot.values.general.quickNavigationAppearance,
                currentGlobalAppearance, snapshot.values.general.quickNavTheme, true, &snapshot.values.general.globalQuickNavigationAppearance);
            quickAppearanceOptions->Apply(snapshot.values.navigation, appearance.contentTheme == 1);
            navigationRevision = snapshot.domainRevisions.navigation;
        }
        hasSnapshot = true;
        updatingControls = previousUpdating;
        if (newGeneration && actions.themeLibrary)
        {
            const auto result = actions.themeLibrary(generation, {});
            if (result.succeeded) { savedThemes = result.library; RefreshLibraryChoices(); }
            else { libraryFeedback.Severity(muxc::InfoBarSeverity::Error); libraryFeedback.Message(result.message); libraryFeedback.IsOpen(true); }
        }
    }

    mux::FrameworkElement FocusTarget(std::string_view id) const noexcept
    {
        if (id == "personalization.savedThemes") return libraryChoice;
        if (id == "quickNav.layout" || id.starts_with("quickNav.layout.") ||
            id.starts_with("quickNav.color.") || id.starts_with("quickNav.colors."))
        {
            if (quickAppearanceContent.Visibility() != mux::Visibility::Visible) return quickNavigationThemeCombo;
            return quickAppearanceOptions->FocusTarget(id);
        }
        const auto appearanceTarget = [this](mux::FrameworkElement target) {
            if (currentBackgroundPreset != kAppearancePresetCustom) return mux::FrameworkElement{presetCombo};
            appearanceSections.Reveal(target);
            return target;
        };
        if (id == "personalization.theme" ||
            id == "personalization.globalTheme")
            return presetCombo;
        if (id == "personalization.font") return fontPicker;
        if (id == "personalization.dockAppearance") return dockAppearanceCombo;
        if (id == "personalization.statusBarTheme") return statusBarLink;
        if (id == "personalization.taskbar") return taskbarLink;
        if (id == "personalization.backgroundColor")
            return appearanceTarget(backgroundColor.editor.button);
        if (id == "personalization.borderColor")
            return appearanceTarget(borderColor.editor.button);
        if (id == "personalization.quickNavigationTheme" ||
            id == "personalization.quickNavTheme")
            return quickNavigationThemeCombo;
        if (id == "personalization.collectionPopupTheme")
            return collectionPopupThemeCombo;
        if (id == "personalization.widgetAlpha" ||
            id == "personalization.backgroundOpacity")
            return appearanceTarget(widgetAlpha.slider);
        if (id == "personalization.borderAlpha" ||
            id == "personalization.borderOpacity")
            return appearanceTarget(borderAlpha.slider);
        if (id == "personalization.borderWidth")
            return appearanceTarget(borderWidth.slider);
        if (id == "personalization.edgeHighlight")
            return appearanceTarget(edgeHighlightToggle);
        if (id == "personalization.edgeHighlightWidth")
            return appearanceTarget(edgeHighlightWidth.slider);
        if (id == "personalization.edgeHighlightStrength")
            return appearanceTarget(edgeHighlightStrength.slider);
        if (id == "personalization.gradientEndAlpha")
            return appearanceTarget(gradientEndAlpha.slider);
        if (id == "personalization.enableGradient")
            return appearanceTarget(gradientToggle);
        if (id == "personalization.glass")
            return appearanceTarget(glassToggle);
        if (id == "personalization.blurRadius")
            return appearanceTarget(blurRadius.slider);
        if (id == "personalization.acrylic")
            return appearanceTarget(acrylicToggle);
        if (id == "personalization.contentTheme")
            return appearanceTarget(contentThemeCombo);
        if (id == "personalization.contextMenu")
            return contextMenuCombo;
        if (id == "personalization.cornerRadius")
            return cornerRadius.slider;
        if (id == "personalization.showGroupTabCounts")
            return showGroupTabCounts;
        if (id == "personalization.popupHoverOpen")
            return popupHoverOpen;
        if (id == "personalization.popupHoverDelayMs")
        {
            if (!popupHoverOpen.IsOn()) return popupHoverOpen;
            return popupHoverDelayMs.slider;
        }
        if (id == "personalization.barHeight")
            return barHeight.slider;
        if (id == "personalization.scrollableTitleBarOnTop")
            return topTitleBarToggle;
        if (id == "personalization.luaWidgetRowHeight")
            return luaWidgetContentRowHeight.slider;
        if (id == "personalization.widgetTransformCursors")
            return widgetTransformCursors;
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
        quickAppearanceOptions->Dismiss();
        for (ColorControl* control : colorControls)
            control->editor.Dismiss();
    }

    void CommitContinuousEdits() noexcept
    {
        try
        {
            quickAppearanceEditor->Flush(); popupAppearanceEditor->Flush(); dockAppearanceEditor->Flush();
            if (panelGradientEditor) panelGradientEditor->Flush();
            if (edgeLightEditor) edgeLightEditor->Flush();
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
        libraryTarget.SelectionChanged(libraryTargetToken);
        libraryChoice.SelectionChanged(libraryChoiceToken);
        for (std::size_t i = 0; i < libraryButtons.size(); ++i) libraryButtons[i].Click(libraryButtonTokens[i]);
        CommitOpenColorEditors();
        if (active)
            CommitContinuousEdits();
        active = false;
        closed = true;
        quickAppearanceEditor->Close(); popupAppearanceEditor->Close(); dockAppearanceEditor->Close();
        quickAppearanceOptions->Close();
        dockAppearanceCombo.SelectionChanged(dockAppearanceToken); statusBarLink.Click(statusBarLinkToken); taskbarLink.Click(taskbarLinkToken);
        try
        {
            fontFlyout.Hide();
            fontFlyout.Opened(fontOpenToken);
            fontSearch.TextChanged(fontSearchToken);
            fontSearch.KeyDown(fontSearchKeyToken);
            fontResults.ItemClick(fontResultToken);
            fontResults.KeyDown(fontResultKeyToken);
            fontImportFile.Click(fontImportFileToken);
            fontImportFolder.Click(fontImportFolderToken);
            fontRestartButton.Click(fontRestartToken);
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
            showGroupTabCounts.Toggled(showGroupTabCountsToken);
            popupHoverOpen.Toggled(popupHoverOpenToken);
            topTitleBarToggle.Toggled(topTitleBarToken);
            widgetTransformCursors.Toggled(widgetTransformCursorsToken);
        }
        catch (...)
        {
        }
        for (ContinuousControl* control : continuousControls)
            UnhookContinuousControl(*control);
        for (ColorControl* control : colorControls)
            UnhookColorControl(*control);
            if (panelGradientEditor) panelGradientEditor->Close();
            if (edgeLightEditor) edgeLightEditor->Close();
        actions = {};
        localize = {};
    }
};

PersonalizationPagePresenter::PersonalizationPagePresenter(
    LocalizeCallback localize,
    const mux::Style& cardStyle,
    const mux::Style& navigationStyle)
    : impl_(std::make_unique<Impl>(std::move(localize), cardStyle, navigationStyle))
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
    {
        impl_->actions = std::move(actions);
        impl_->RefreshFonts();
    }
}

void PersonalizationPagePresenter::SetLayoutSpacingContent(
    const mux::UIElement& content)
{
    if (!impl_ || !content) return;
    uint32_t index = 0;
    const auto children = impl_->layoutCard.content.Children();
    // The card title remains first; spacing is the first setting below it.
    if (!children.IndexOf(content, index)) children.InsertAt(1, content);
}

mux::UIElement PersonalizationPagePresenter::MenuContent() const noexcept
{ return impl_ ? impl_->menuRoot : nullptr; }

mux::UIElement PersonalizationPagePresenter::DockAppearanceContent() const noexcept { return impl_ ? impl_->dockThemeRoot : nullptr; }

mux::UIElement PersonalizationPagePresenter::ThemeContent() const noexcept
{
    return impl_ ? impl_->themeRoot : nullptr;
}

mux::UIElement PersonalizationPagePresenter::WidgetLayoutContent()
    const noexcept
{
    return impl_ ? impl_->widgetLayoutRoot : nullptr;
}

mux::UIElement PersonalizationPagePresenter::WidgetBehaviorContent() const noexcept
{
    return impl_ ? impl_->widgetBehaviorRoot : nullptr;
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
    {
        impl_->active = true;
        try
        {
            if (impl_->hasSnapshot && impl_->actions.themeLibrary)
            {
                const auto result = impl_->actions.themeLibrary(impl_->generation, {});
                if (result.succeeded) { impl_->savedThemes = result.library; impl_->RefreshLibraryChoices(); }
                else { impl_->libraryFeedback.Message(result.message); impl_->libraryFeedback.Severity(muxc::InfoBarSeverity::Error); impl_->libraryFeedback.IsOpen(true); }
            }
        }
        catch (...) { }
    }
}

void PersonalizationPagePresenter::Deactivate() noexcept
{
    if (!impl_ || impl_->closed) return;
    impl_->CommitOpenColorEditors();
    impl_->CommitContinuousEdits();
    impl_->active = false;
    try { impl_->fontFlyout.Hide(); } catch (...) {}
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

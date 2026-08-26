#include "pch.h"

#include "settings_presenter_controls.h"
#include "widgets_page_presenter.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <cwctype>
#include <utility>

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxa = winrt::Microsoft::UI::Xaml::Automation;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace muxcp = winrt::Microsoft::UI::Xaml::Controls::Primitives;
namespace muxm = winrt::Microsoft::UI::Xaml::Media;
namespace wad = winrt::Windows::ApplicationModel::DataTransfer;

namespace
{

enum class PackageFilter : std::uint8_t
{
    All,
    Installed,
    Included,
    Development,
};

struct SettingsCard
{
    muxc::Border root{nullptr};
    muxc::StackPanel content{nullptr};
    muxc::TextBlock title{nullptr};
    muxc::TextBlock description{nullptr};
};

void InitializeCard(
    SettingsCard& card,
    const mux::Style& style,
    const muxc::StackPanel& page)
{
    card.root = muxc::Border{};
    card.root.Style(style);
    card.content = muxc::StackPanel{};
    card.content.Spacing(10.0);
    card.content.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
    card.title = muxc::TextBlock{};
    card.title.FontWeight(
        winrt::Windows::UI::Text::FontWeights::SemiBold());
    card.title.TextWrapping(mux::TextWrapping::Wrap);
    card.description = muxc::TextBlock{};
    card.description.Opacity(0.72);
    card.description.TextWrapping(mux::TextWrapping::Wrap);
    card.content.Children().Append(card.title);
    card.content.Children().Append(card.description);
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
                // on each side. Pinning the body to the remaining width keeps
                // action rows aligned with the full card instead of letting a
                // desired-width StackPanel appear centered.
                currentBody.Width(std::max(0.0,
                    static_cast<double>(args.NewSize().Width) - 32.0));
            }
        });
}

void SetAutomation(
    const mux::DependencyObject& element,
    std::wstring_view name,
    std::wstring_view help = {})
{
    muxa::AutomationProperties::SetName(element, std::wstring(name));
    muxa::AutomationProperties::SetHelpText(element, std::wstring(help));
}

void ApplyAccentButtonStyle(const muxc::Button& button) noexcept
{
    try
    {
        if (const auto application = mux::Application::Current())
        {
            if (const auto resource = application.Resources().TryLookup(
                    winrt::box_value(L"AccentButtonStyle")))
            {
                if (const auto style = resource.try_as<mux::Style>())
                    button.Style(style);
            }
        }
    }
    catch (...)
    {
    }
}

std::wstring Lower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t value) {
            return static_cast<wchar_t>(std::towlower(value));
        });
    return value;
}

bool ContainsInsensitive(
    std::wstring_view haystack,
    std::wstring_view needle)
{
    if (needle.empty())
        return true;
    return Lower(std::wstring(haystack)).find(
               Lower(std::wstring(needle))) != std::wstring::npos;
}

std::wstring JoinMetadata(
    std::initializer_list<std::wstring_view> values)
{
    std::wstring result;
    for (const std::wstring_view value : values)
    {
        if (value.empty())
            continue;
        if (!result.empty())
            result.append(L" · ");
        result.append(value);
    }
    return result;
}

muxc::InfoBarSeverity ToInfoBarSeverity(
    WidgetsPageFeedbackSeverity severity)
{
    switch (severity)
    {
    case WidgetsPageFeedbackSeverity::Success:
        return muxc::InfoBarSeverity::Success;
    case WidgetsPageFeedbackSeverity::Warning:
        return muxc::InfoBarSeverity::Warning;
    case WidgetsPageFeedbackSeverity::Error:
        return muxc::InfoBarSeverity::Error;
    case WidgetsPageFeedbackSeverity::None:
    case WidgetsPageFeedbackSeverity::Informational:
    default:
        return muxc::InfoBarSeverity::Informational;
    }
}

struct CallbackGenerationGate
{
    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::uint64_t> activation{0};
    std::atomic_bool active{false};
    std::atomic_bool closed{false};
};

} // namespace

struct WidgetsPagePresenter::Impl
{
    struct PackageRowBinding
    {
        std::wstring packageId;
        muxc::Border card{nullptr};
        muxc::TextBlock name{nullptr};
        muxc::TextBlock metadata{nullptr};
        muxc::StackPanel tags{nullptr};
        muxc::TextBlock description{nullptr};
        muxc::TextBlock version{nullptr};
        muxc::TextBlock author{nullptr};
        muxc::TextBlock sourceId{nullptr};
        muxc::Button enabledAction{nullptr};
        muxc::Button addAction{nullptr};
        muxc::Button developmentAction{nullptr};
        std::vector<muxc::Button> advancedActions;
        std::vector<std::function<void()>> revokers;
    };

    explicit Impl(LocalizeCallback callback, const mux::Style& style,
        const mux::DataTemplate& fluentTemplate,
        const mux::DataTemplate& fontAwesomeTemplate)
        : localize(std::move(callback)), cardStyle(style),
          fluentGlyphTemplate(fluentTemplate),
          fontAwesomeGlyphTemplate(fontAwesomeTemplate),
          callbackGate(std::make_shared<CallbackGenerationGate>())
    {
        BuildControls();
        HookStaticEvents();
        RefreshLocalizedText();
        RenderTask();
        RenderFeedback();
    }

    LocalizeCallback localize;
    WidgetsPageActions actions;
    mux::Style cardStyle{nullptr};
    mux::DataTemplate fluentGlyphTemplate{nullptr};
    mux::DataTemplate fontAwesomeGlyphTemplate{nullptr};
    muxc::StackPanel root{nullptr};
    muxc::InfoBar feedbackBar{nullptr};
    muxc::StackPanel developerRoot{nullptr};
    muxc::InfoBar developerFeedbackBar{nullptr};
    muxc::StackPanel debugRoot{nullptr};
    muxc::InfoBar debugFeedbackBar{nullptr};

    SettingsCard searchCard;
    SettingsCard filterCard;
    SettingsCard developerOverridesCard;
    SettingsCard developerWorkspaceCard;
    SettingsCard developerCliCard;
    SettingsCard developerPublishCard;
    SettingsCard developerReferenceCard;
    SettingsCard developerToolsCard;
    SettingsCard debugRuntimeCard;

    muxc::AutoSuggestBox searchBox{nullptr};
    muxc::StackPanel filterActions{nullptr};
    muxcp::ToggleButton allFilterButton{nullptr};
    muxcp::ToggleButton installedFilterButton{nullptr};
    muxcp::ToggleButton includedFilterButton{nullptr};
    muxcp::ToggleButton developmentFilterButton{nullptr};
    presenter_controls::SettingRow managementRow;
    muxc::StackPanel managementActions{nullptr};
    muxc::Button installFileButton{nullptr};
    muxc::Button workshopButton{nullptr};
    muxcp::ToggleButton selfDevelopButton{nullptr};
    muxc::StackPanel taskPanel{nullptr};
    muxc::ProgressRing taskRing{nullptr};
    muxc::ProgressBar taskProgress{nullptr};
    muxc::TextBlock taskStatus{nullptr};
    muxc::Button cancelTaskButton{nullptr};
    muxc::StackPanel installedRows{nullptr};
    muxc::StackPanel developerOverrideRows{nullptr};
    presenter_controls::SettingRow agentSkillActionsRow;
    muxc::StackPanel agentSkillActions{nullptr};
    muxc::Button agentSkillApplyButton{nullptr};
    muxc::Button agentSkillRefreshButton{nullptr};
    muxc::StackPanel agentSkillRows{nullptr};
    presenter_controls::SettingRow developmentWorkspaceRow;
    muxc::Button openDevelopmentFolderButton{nullptr};
    muxc::TextBlock developmentWorkspacePath{nullptr};
    muxc::StackPanel cliRows{nullptr};
    muxc::Button publishWorkspaceButton{nullptr};
    muxc::TextBlock iconReferenceTitle{nullptr};
    muxc::Expander fluentIconsExpander{nullptr};
    muxc::Expander fontAwesomeIconsExpander{nullptr};
    muxc::GridView fluentIconsGrid{nullptr};
    muxc::GridView fontAwesomeIconsGrid{nullptr};
    muxc::TextBlock fluentIconsCount{nullptr};
    muxc::TextBlock fontAwesomeIconsCount{nullptr};
    presenter_controls::SettingRow developerErrorActionsRow;
    muxc::StackPanel developerErrorActions{nullptr};
    muxc::Button copyAllErrorsButton{nullptr};
    muxc::Button clearAllErrorsButton{nullptr};
    muxc::ScrollViewer developerErrorScroller{nullptr};
    muxc::StackPanel developerErrorRows{nullptr};
    presenter_controls::SettingRow developerDiagnosticActionsRow;
    muxc::Button copyDiagnosticsButton{nullptr};
    muxc::Button developerRefreshButton{nullptr};
    muxc::StackPanel developerDiagnosticRows{nullptr};
    muxc::Button debugRefreshButton{nullptr};
    muxc::StackPanel debugDiagnosticRows{nullptr};

    mux::FrameworkElement firstPackageTarget{nullptr};
    mux::FrameworkElement firstIncludedPackageTarget{nullptr};
    mux::FrameworkElement firstPermissionTarget{nullptr};
    mux::FrameworkElement firstDeveloperOverrideTarget{nullptr};
    mux::FrameworkElement firstDeveloperDiagnosticTarget{nullptr};
    mux::FrameworkElement firstDebugDiagnosticTarget{nullptr};

    std::vector<InstalledWidgetPackageSnapshot> packages;
    std::vector<PackageRowBinding> packageRowBindings;
    std::vector<WidgetAgentSkillTargetSnapshot> agentSkills;
    std::vector<WidgetRuntimeErrorSnapshot> errors;
    std::vector<WidgetRuntimeDiagnosticSnapshot> diagnostics;
    std::vector<WidgetSourceGroupSnapshot> sources;
    WidgetsPageTaskSnapshot task;
    WidgetsPageFeedbackSnapshot feedback;
    std::wstring query;
    std::wstring developmentWorkspace;
    std::wstring componentCliPath;
    std::wstring developerActionStatus;
    std::wstring agentSkillStatusError;
    int agentSkillTargetMask = 0;
    bool developerPublisherAvailable = false;
    bool developerOverridesVisible = false;
    PackageFilter filter = PackageFilter::All;

    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    std::uint64_t searchRevision = 0;
    std::uint64_t requestedSearchRevision = 0;
    bool hasRevision = false;
    bool updatingControls = false;
    bool active = false;
    bool closed = false;

    std::shared_ptr<CallbackGenerationGate> callbackGate;
    std::vector<std::function<void()>> developerRevokers;
    std::vector<std::function<void()>> debugRevokers;

    winrt::event_token searchChangedToken{};
    winrt::event_token allFilterToken{};
    winrt::event_token installedFilterToken{};
    winrt::event_token includedFilterToken{};
    winrt::event_token developmentFilterToken{};
    winrt::event_token installFileToken{};
    winrt::event_token workshopToken{};
    winrt::event_token selfDevelopToken{};
    winrt::event_token cancelTaskToken{};
    winrt::event_token developerRefreshToken{};
    winrt::event_token agentSkillApplyToken{};
    winrt::event_token agentSkillRefreshToken{};
    winrt::event_token openDevelopmentFolderToken{};
    winrt::event_token publishWorkspaceToken{};
    winrt::event_token copyAllErrorsToken{};
    winrt::event_token clearAllErrorsToken{};
    winrt::event_token copyDiagnosticsToken{};
    winrt::event_token fluentIconClickToken{};
    winrt::event_token fontAwesomeIconClickToken{};
    winrt::event_token fluentIconContainerToken{};
    winrt::event_token fontAwesomeIconContainerToken{};
    winrt::event_token debugRefreshToken{};

    [[nodiscard]] std::wstring L(
        std::string_view key,
        std::wstring_view fallback = {}) const
    {
        if (localize)
        {
            std::wstring value = localize(key);
            if (!value.empty())
                return value;
        }
        return std::wstring(fallback);
    }

    [[nodiscard]] std::wstring PermissionStateText(
        WidgetPackagePermissionState state) const
    {
        switch (state)
        {
        case WidgetPackagePermissionState::LegacyImplicit:
            return L("app.settings.widgets_permission_state_legacy",
                L"Not reviewed");
        case WidgetPackagePermissionState::Pending:
            return L("app.settings.widgets_permission_state_pending",
                L"Waiting for approval");
        case WidgetPackagePermissionState::Granted:
            return L("app.settings.widgets_permission_state_granted",
                L"Allowed");
        case WidgetPackagePermissionState::Denied:
            return L("app.settings.widgets_permission_state_denied",
                L"Not allowed");
        case WidgetPackagePermissionState::MissingRequired:
        default:
            return L("app.settings.widgets_permission_state_missing_required",
                L"Required access missing");
        }
    }

    [[nodiscard]] std::wstring TaskText(WidgetsPageTaskKind kind) const
    {
        switch (kind)
        {
        case WidgetsPageTaskKind::Searching:
            return L("app.settings.widgets_search", L"Searching");
        case WidgetsPageTaskKind::Installing:
            return L("app.settings.widgets_install", L"Installing");
        case WidgetsPageTaskKind::Uninstalling:
            return L("app.settings.widgets_uninstall", L"Uninstalling");
        case WidgetsPageTaskKind::SynchronizingWorkshop:
            return L("app.settings.widgets_source_steam",
                L"Synchronizing Workshop");
        case WidgetsPageTaskKind::ApplyingPermissions:
            return L("app.settings.widgets_manage_permissions",
                L"Applying permissions");
        case WidgetsPageTaskKind::ApplyingDevelopmentOverride:
            return L("app.settings.widgets_development",
                L"Applying development override");
        case WidgetsPageTaskKind::AddingToDesktop:
            return L("app.widget_preview.add_to_desktop",
                L"Adding to desktop");
        case WidgetsPageTaskKind::None:
        default:
            return {};
        }
    }

    [[nodiscard]] std::wstring PackageStateText(
        const InstalledWidgetPackageSnapshot& package) const
    {
        if (!package.valid)
        {
            return !package.workshopInstallFailures.empty()
                ? L("app.settings.widgets_workshop_install_failed",
                    L"Workshop install failed")
                : L("app.settings.invalid", L"Invalid");
        }
        if (package.canUseDevelopmentOverride)
        {
            if (package.developmentOverrideActive)
            {
                return L("app.settings.widgets_development_active",
                    L"Development version active");
            }
            if (package.canEnable)
            {
                return L("app.settings.widgets_development_using_installed",
                    L"Currently using the installed version");
            }
            return L("app.settings.widgets_development_inactive",
                L"Development version inactive");
        }
        if (package.canEnable)
        {
            return L(package.enabled
                    ? "app.settings.widgets_active"
                    : "app.settings.widgets_disabled",
                package.enabled ? L"In use" : L"Disabled");
        }
        return L("app.settings.valid", L"Valid");
    }

    static std::wstring CodepointText(std::uint32_t codepoint)
    {
        if (codepoint <= 0xFFFF)
            return std::wstring(1, static_cast<wchar_t>(codepoint));
        codepoint -= 0x10000;
        return {static_cast<wchar_t>(0xD800 + (codepoint >> 10)),
            static_cast<wchar_t>(0xDC00 + (codepoint & 0x3FF))};
    }

    static bool CopyText(std::wstring_view text) noexcept
    {
        try
        {
            wad::DataPackage package;
            package.SetText(winrt::hstring(text));
            wad::Clipboard::SetContent(package);
            wad::Clipboard::Flush();
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    static std::uint32_t CodepointFromText(std::wstring_view text) noexcept
    {
        if (text.empty()) return 0;
        const std::uint32_t first = text.front();
        if (first >= 0xD800 && first <= 0xDBFF && text.size() > 1)
        {
            const std::uint32_t second = text[1];
            if (second >= 0xDC00 && second <= 0xDFFF)
            {
                return 0x10000 + ((first - 0xD800) << 10) +
                    (second - 0xDC00);
            }
        }
        return first;
    }

    [[nodiscard]] std::wstring GlyphTooltip(
        std::string_view key,
        std::wstring_view fallback,
        std::wstring_view glyph) const
    {
        wchar_t codepoint[16]{};
        swprintf_s(codepoint, L"%04X", CodepointFromText(glyph));
        std::wstring text = L(key, fallback);
        const std::size_t marker = text.find(L"%04X");
        if (marker != std::wstring::npos)
            text.replace(marker, 4, codepoint);
        return text;
    }

    winrt::Windows::Foundation::Collections::
        IObservableVector<winrt::Windows::Foundation::IInspectable>
    FluentGlyphItems() const
    {
        auto values = winrt::single_threaded_observable_vector<
            winrt::Windows::Foundation::IInspectable>();
        for (std::uint32_t codepoint = 0xE000; codepoint <= 0xF8FF;
             ++codepoint)
        {
            values.Append(winrt::box_value(CodepointText(codepoint)));
        }
        for (std::uint32_t codepoint = 0xF0000; codepoint <= 0xF0CCE;
             ++codepoint)
        {
            values.Append(winrt::box_value(CodepointText(codepoint)));
        }
        return values;
    }

    winrt::Windows::Foundation::Collections::
        IObservableVector<winrt::Windows::Foundation::IInspectable>
    FontAwesomeGlyphItems() const
    {
        auto values = winrt::single_threaded_observable_vector<
            winrt::Windows::Foundation::IInspectable>();
        const HFONT font = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE,
            FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH,
            L"Font Awesome 6 Free Solid");
        const HDC dc = CreateCompatibleDC(nullptr);
        if (!font || !dc)
        {
            if (dc) DeleteDC(dc);
            if (font) DeleteObject(font);
            return values;
        }
        const HGDIOBJ previous = SelectObject(dc, font);
        for (std::uint32_t codepoint = 0xE000; codepoint <= 0xF8FF;
             ++codepoint)
        {
            const wchar_t character = static_cast<wchar_t>(codepoint);
            WORD glyph = 0xFFFF;
            if (GetGlyphIndicesW(dc, &character, 1, &glyph,
                    GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR &&
                glyph != 0xFFFF)
            {
                values.Append(winrt::box_value(
                    std::wstring(1, character)));
            }
        }
        SelectObject(dc, previous);
        DeleteDC(dc);
        DeleteObject(font);
        return values;
    }

    void InitializeIconReference(
        muxc::Expander& expander,
        muxc::TextBlock& count,
        muxc::GridView& grid,
        std::string_view titleKey,
        std::wstring_view titleFallback,
        std::string_view hintKey,
        std::wstring_view hintFallback,
        const mux::DataTemplate& itemTemplate,
        const winrt::Windows::Foundation::Collections::
            IObservableVector<winrt::Windows::Foundation::IInspectable>&
                items)
    {
        expander = muxc::Expander{};
        expander.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        expander.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        muxc::StackPanel header;
        header.Spacing(3.0);
        muxc::TextBlock title;
        title.Text(L(titleKey, titleFallback));
        title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        title.TextWrapping(mux::TextWrapping::Wrap);
        header.Children().Append(title);
        header.Children().Append(MakeSecondaryText(
            L(hintKey, hintFallback)));
        expander.Header(header);

        muxc::StackPanel body;
        body.Spacing(8.0);
        count = muxc::TextBlock{};
        count.Opacity(0.72);
        body.Children().Append(count);
        grid = muxc::GridView{};
        grid.Height(220.0);
        grid.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        grid.SelectionMode(muxc::ListViewSelectionMode::None);
        grid.IsItemClickEnabled(true);
        grid.ItemsSource(items);
        grid.ItemTemplate(itemTemplate);
        body.Children().Append(grid);
        expander.Content(body);
        StretchExpanderBody(expander, body);
    }

    void BuildControls()
    {
        root = muxc::StackPanel{};
        root.Spacing(8.0);
        root.HorizontalAlignment(mux::HorizontalAlignment::Stretch);

        feedbackBar = muxc::InfoBar{};
        feedbackBar.IsClosable(true);
        feedbackBar.IsOpen(false);
        root.Children().Append(feedbackBar);

        InitializeCard(searchCard, cardStyle, root);
        managementActions = muxc::StackPanel{};
        managementActions.Orientation(muxc::Orientation::Horizontal);
        managementActions.Spacing(8.0);
        managementActions.HorizontalAlignment(
            mux::HorizontalAlignment::Right);

        installFileButton = muxc::Button{};
        installFileButton.HorizontalAlignment(mux::HorizontalAlignment::Right);
        installFileButton.UseSystemFocusVisuals(true);
        managementActions.Children().Append(installFileButton);

        workshopButton = muxc::Button{};
        workshopButton.HorizontalAlignment(mux::HorizontalAlignment::Right);
        workshopButton.UseSystemFocusVisuals(true);
        managementActions.Children().Append(workshopButton);

        selfDevelopButton = muxcp::ToggleButton{};
        selfDevelopButton.HorizontalAlignment(mux::HorizontalAlignment::Right);
        selfDevelopButton.UseSystemFocusVisuals(true);
        managementActions.Children().Append(selfDevelopButton);
        managementRow.Initialize(managementActions, 0.0);
        managementRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        searchCard.content.Children().Append(managementRow.root);

        InitializeCard(filterCard, cardStyle, root);

        searchBox = muxc::AutoSuggestBox{};
        searchBox.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        searchBox.IsSuggestionListOpen(false);
        searchBox.UseSystemFocusVisuals(true);
        filterCard.content.Children().Append(searchBox);

        filterActions = muxc::StackPanel{};
        filterActions.Orientation(muxc::Orientation::Horizontal);
        filterActions.Spacing(8.0);
        filterActions.HorizontalAlignment(mux::HorizontalAlignment::Left);
        allFilterButton = muxcp::ToggleButton{};
        installedFilterButton = muxcp::ToggleButton{};
        includedFilterButton = muxcp::ToggleButton{};
        developmentFilterButton = muxcp::ToggleButton{};
        for (const muxcp::ToggleButton& button : {allFilterButton,
                 installedFilterButton, includedFilterButton,
                 developmentFilterButton})
        {
            button.UseSystemFocusVisuals(true);
            button.HorizontalAlignment(mux::HorizontalAlignment::Left);
            filterActions.Children().Append(button);
        }
        filterCard.content.Children().Append(filterActions);

        taskPanel = muxc::StackPanel{};
        taskPanel.Spacing(8.0);
        taskPanel.Visibility(mux::Visibility::Collapsed);
        taskRing = muxc::ProgressRing{};
        taskRing.Width(28.0);
        taskRing.Height(28.0);
        taskRing.IsActive(false);
        taskProgress = muxc::ProgressBar{};
        taskProgress.Minimum(0.0);
        taskProgress.Maximum(100.0);
        taskProgress.Visibility(mux::Visibility::Collapsed);
        taskStatus = muxc::TextBlock{};
        taskStatus.TextWrapping(mux::TextWrapping::Wrap);
        cancelTaskButton = muxc::Button{};
        cancelTaskButton.HorizontalAlignment(mux::HorizontalAlignment::Right);
        cancelTaskButton.UseSystemFocusVisuals(true);
        taskPanel.Children().Append(taskRing);
        taskPanel.Children().Append(taskProgress);
        taskPanel.Children().Append(taskStatus);
        taskPanel.Children().Append(cancelTaskButton);
        filterCard.content.Children().Append(taskPanel);

        installedRows = muxc::StackPanel{};
        installedRows.Spacing(8.0);
        installedRows.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        root.Children().Append(installedRows);

        developerRoot = muxc::StackPanel{};
        developerRoot.Spacing(8.0);
        developerRoot.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        developerFeedbackBar = muxc::InfoBar{};
        developerFeedbackBar.IsClosable(true);
        developerFeedbackBar.IsOpen(false);
        developerRoot.Children().Append(developerFeedbackBar);
        InitializeCard(developerOverridesCard, cardStyle, developerRoot);
        developerOverridesCard.root.IsTabStop(true);
        developerOverridesCard.root.UseSystemFocusVisuals(true);
        agentSkillActions = muxc::StackPanel{};
        agentSkillActions.Orientation(muxc::Orientation::Horizontal);
        agentSkillActions.Spacing(8.0);
        agentSkillActions.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        agentSkillApplyButton = muxc::Button{};
        agentSkillApplyButton.UseSystemFocusVisuals(true);
        ApplyAccentButtonStyle(agentSkillApplyButton);
        agentSkillRefreshButton = muxc::Button{};
        agentSkillRefreshButton.UseSystemFocusVisuals(true);
        agentSkillActions.Children().Append(agentSkillApplyButton);
        agentSkillActions.Children().Append(agentSkillRefreshButton);
        agentSkillActionsRow.Initialize(agentSkillActions);
        agentSkillActionsRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        developerOverridesCard.content.Children().Append(
            agentSkillActionsRow.root);
        agentSkillRows = muxc::StackPanel{};
        agentSkillRows.Spacing(8.0);
        agentSkillRows.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        developerOverridesCard.content.Children().Append(agentSkillRows);
        developerOverrideRows = muxc::StackPanel{};
        developerOverrideRows.Spacing(8.0);
        developerOverrideRows.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        // Retained as an implementation cache for package rows used on the
        // main Widgets page; DrawWidgetDeveloperTools did not surface them.

        InitializeCard(developerWorkspaceCard, cardStyle, developerRoot);
        openDevelopmentFolderButton = muxc::Button{};
        openDevelopmentFolderButton.UseSystemFocusVisuals(true);
        openDevelopmentFolderButton.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        developmentWorkspaceRow.Initialize(openDevelopmentFolderButton);
        developmentWorkspaceRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        developerWorkspaceCard.content.Children().Append(
            developmentWorkspaceRow.root);
        developmentWorkspacePath = MakeSecondaryText(L"");
        developerWorkspaceCard.content.Children().Append(
            developmentWorkspacePath);

        InitializeCard(developerCliCard, cardStyle, developerRoot);
        cliRows = muxc::StackPanel{};
        cliRows.Spacing(8.0);
        cliRows.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        developerCliCard.content.Children().Append(cliRows);

        InitializeCard(developerPublishCard, cardStyle, developerRoot);
        publishWorkspaceButton = muxc::Button{};
        publishWorkspaceButton.UseSystemFocusVisuals(true);
        ApplyAccentButtonStyle(publishWorkspaceButton);
        publishWorkspaceButton.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        developerPublishCard.content.Children().Append(
            publishWorkspaceButton);

        InitializeCard(developerReferenceCard, cardStyle, developerRoot);
        iconReferenceTitle = muxc::TextBlock{};
        iconReferenceTitle.FontWeight(
            winrt::Windows::UI::Text::FontWeights::SemiBold());
        iconReferenceTitle.TextWrapping(mux::TextWrapping::Wrap);
        developerReferenceCard.content.Children().Append(
            iconReferenceTitle);
        InitializeIconReference(fluentIconsExpander, fluentIconsCount,
            fluentIconsGrid, "app.settings.fluent_icons",
            L"Fluent System Icons Regular Characters",
            "app.settings.fluent_icon_hint",
            L"Click an icon to copy it. Set iconFont = \"fluent\" on the "
                L"Lua menu item that uses it.",
            fluentGlyphTemplate, FluentGlyphItems());
        developerReferenceCard.content.Children().Append(
            fluentIconsExpander);
        InitializeIconReference(fontAwesomeIconsExpander,
            fontAwesomeIconsCount, fontAwesomeIconsGrid,
            "app.settings.fa_icons", L"Font Awesome Icon Characters",
            "app.settings.fa_icon_hint",
            L"Click an icon to copy its character, then paste it into a Lua "
                L"menu item's icon field.",
            fontAwesomeGlyphTemplate, FontAwesomeGlyphItems());
        developerReferenceCard.content.Children().Append(
            fontAwesomeIconsExpander);

        InitializeCard(developerToolsCard, cardStyle, developerRoot);
        developerToolsCard.root.IsTabStop(true);
        developerToolsCard.root.UseSystemFocusVisuals(true);
        developerErrorActions = muxc::StackPanel{};
        developerErrorActions.Orientation(muxc::Orientation::Horizontal);
        developerErrorActions.Spacing(8.0);
        developerErrorActions.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        copyAllErrorsButton = muxc::Button{};
        copyAllErrorsButton.UseSystemFocusVisuals(true);
        clearAllErrorsButton = muxc::Button{};
        clearAllErrorsButton.UseSystemFocusVisuals(true);
        developerErrorActions.Children().Append(copyAllErrorsButton);
        developerErrorActions.Children().Append(clearAllErrorsButton);
        developerErrorActionsRow.Initialize(developerErrorActions);
        developerErrorActionsRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        developerToolsCard.content.Children().Append(
            developerErrorActionsRow.root);
        developerErrorRows = muxc::StackPanel{};
        developerErrorRows.Spacing(8.0);
        developerErrorRows.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        developerErrorScroller = muxc::ScrollViewer{};
        developerErrorScroller.MaxHeight(160.0);
        developerErrorScroller.HorizontalScrollBarVisibility(
            muxc::ScrollBarVisibility::Auto);
        developerErrorScroller.VerticalScrollBarVisibility(
            muxc::ScrollBarVisibility::Auto);
        developerErrorScroller.Content(developerErrorRows);
        developerToolsCard.content.Children().Append(
            developerErrorScroller);
        copyDiagnosticsButton = muxc::Button{};
        copyDiagnosticsButton.UseSystemFocusVisuals(true);
        copyDiagnosticsButton.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        developerDiagnosticActionsRow.Initialize(copyDiagnosticsButton);
        developerDiagnosticActionsRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        developerToolsCard.content.Children().Append(
            developerDiagnosticActionsRow.root);
        developerRefreshButton = muxc::Button{};
        developerRefreshButton.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        developerRefreshButton.UseSystemFocusVisuals(true);
        developerDiagnosticRows = muxc::StackPanel{};
        developerDiagnosticRows.Spacing(8.0);
        developerDiagnosticRows.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        developerToolsCard.content.Children().Append(
            developerDiagnosticRows);

        debugRoot = muxc::StackPanel{};
        debugRoot.Spacing(8.0);
        debugRoot.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        debugFeedbackBar = muxc::InfoBar{};
        debugFeedbackBar.IsClosable(true);
        debugFeedbackBar.IsOpen(false);
        debugRoot.Children().Append(debugFeedbackBar);
        InitializeCard(debugRuntimeCard, cardStyle, debugRoot);
        debugRuntimeCard.root.IsTabStop(true);
        debugRuntimeCard.root.UseSystemFocusVisuals(true);
        debugRefreshButton = muxc::Button{};
        debugRefreshButton.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        debugRefreshButton.UseSystemFocusVisuals(true);
        debugRuntimeCard.content.Children().Append(debugRefreshButton);
        debugDiagnosticRows = muxc::StackPanel{};
        debugDiagnosticRows.Spacing(8.0);
        debugDiagnosticRows.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        debugRuntimeCard.content.Children().Append(debugDiagnosticRows);
    }

    void HookStaticEvents()
    {
        searchChangedToken = searchBox.TextChanged(
            [this](const muxc::AutoSuggestBox& sender,
                const muxc::AutoSuggestBoxTextChangedEventArgs& args) {
                if (updatingControls || closed || !active ||
                    args.Reason() !=
                        muxc::AutoSuggestionBoxTextChangeReason::UserInput)
                {
                    return;
                }
                query = sender.Text().c_str();
                RenderInstalledRows();
                WidgetsPageRequest request;
                request.command = WidgetsPageCommand::SearchSources;
                request.query = query;
                request.searchRevision = ++requestedSearchRevision;
                Emit(std::move(request));
            });

        const auto selectFilter = [this](PackageFilter selected) {
            if (updatingControls || closed || !active)
                return;
            filter = selected;
            RefreshFilterItems();
            RenderInstalledRows();
        };
        allFilterToken = allFilterButton.Click(
            [selectFilter](
                const winrt::Windows::Foundation::IInspectable&,
                const mux::RoutedEventArgs&) {
                selectFilter(PackageFilter::All);
            });
        installedFilterToken = installedFilterButton.Click(
            [selectFilter](
                const winrt::Windows::Foundation::IInspectable&,
                const mux::RoutedEventArgs&) {
                selectFilter(PackageFilter::Installed);
            });
        includedFilterToken = includedFilterButton.Click(
            [selectFilter](
                const winrt::Windows::Foundation::IInspectable&,
                const mux::RoutedEventArgs&) {
                selectFilter(PackageFilter::Included);
            });
        developmentFilterToken = developmentFilterButton.Click(
            [selectFilter](
                const winrt::Windows::Foundation::IInspectable&,
                const mux::RoutedEventArgs&) {
                selectFilter(PackageFilter::Development);
            });

        installFileToken = installFileButton.Click(
            [this](const winrt::Windows::Foundation::IInspectable&,
                const mux::RoutedEventArgs&) {
                WidgetsPageRequest request;
                request.command = WidgetsPageCommand::BrowseInstallPackage;
                Emit(std::move(request));
            });

        workshopToken = workshopButton.Click(
            [this](const winrt::Windows::Foundation::IInspectable&,
                const mux::RoutedEventArgs&) {
                WidgetsPageRequest request;
                request.command = WidgetsPageCommand::OpenWorkshop;
                Emit(std::move(request));
            });

        selfDevelopToken = selfDevelopButton.Click(
            [this](const winrt::Windows::Foundation::IInspectable&,
                const mux::RoutedEventArgs&) {
                if (closed || !active ||
                    !actions.setDeveloperToolsEnabled)
                {
                    return;
                }
                const bool enable = !developerOverridesVisible;
                const bool applied =
                    actions.setDeveloperToolsEnabled(generation, enable);
                if (applied && enable && actions.navigate)
                {
                    SettingsRoute route;
                    route.page = SettingsPage::DeveloperTools;
                    actions.navigate(generation, std::move(route));
                }
                else if (!applied)
                {
                    selfDevelopButton.IsChecked(developerOverridesVisible);
                }
            });

        cancelTaskToken = cancelTaskButton.Click(
            [this](const winrt::Windows::Foundation::IInspectable&,
                const mux::RoutedEventArgs&) {
                WidgetsPageRequest request;
                request.command = WidgetsPageCommand::CancelTask;
                request.taskId = task.taskId;
                Emit(std::move(request));
            });

        developerRefreshToken = developerRefreshButton.Click(
            [this](const winrt::Windows::Foundation::IInspectable&,
                const mux::RoutedEventArgs&) {
                WidgetsPageRequest request;
                request.command = WidgetsPageCommand::Refresh;
                Emit(std::move(request));
            });

        agentSkillApplyToken = agentSkillApplyButton.Click(
            [this](const winrt::Windows::Foundation::IInspectable&,
                const mux::RoutedEventArgs&) {
                WidgetsPageRequest request;
                request.command =
                    WidgetsPageCommand::ApplyAgentSkillSelection;
                Emit(std::move(request));
            });
        agentSkillRefreshToken = agentSkillRefreshButton.Click(
            [this](const winrt::Windows::Foundation::IInspectable&,
                const mux::RoutedEventArgs&) {
                WidgetsPageRequest request;
                request.command = WidgetsPageCommand::RefreshAgentSkills;
                Emit(std::move(request));
            });
        openDevelopmentFolderToken = openDevelopmentFolderButton.Click(
            [this](const winrt::Windows::Foundation::IInspectable&,
                const mux::RoutedEventArgs&) {
                WidgetsPageRequest request;
                request.command =
                    WidgetsPageCommand::OpenDevelopmentFolder;
                Emit(std::move(request));
            });
        publishWorkspaceToken = publishWorkspaceButton.Click(
            [this](const winrt::Windows::Foundation::IInspectable&,
                const mux::RoutedEventArgs&) {
                WidgetsPageRequest request;
                request.command =
                    WidgetsPageCommand::PublishDevelopmentWorkspace;
                Emit(std::move(request));
            });
        copyAllErrorsToken = copyAllErrorsButton.Click(
            [this](const winrt::Windows::Foundation::IInspectable&,
                const mux::RoutedEventArgs&) {
                (void)CopyText(BuildErrorCopyText());
            });
        clearAllErrorsToken = clearAllErrorsButton.Click(
            [this](const winrt::Windows::Foundation::IInspectable&,
                const mux::RoutedEventArgs&) {
                WidgetsPageRequest request;
                request.command = WidgetsPageCommand::ClearWidgetErrors;
                Emit(std::move(request));
            });
        copyDiagnosticsToken = copyDiagnosticsButton.Click(
            [this](const winrt::Windows::Foundation::IInspectable&,
                const mux::RoutedEventArgs&) {
                (void)CopyText(BuildDiagnosticsCopyText());
            });
        fluentIconClickToken = fluentIconsGrid.ItemClick(
            [](const winrt::Windows::Foundation::IInspectable&,
                const muxc::ItemClickEventArgs& args) {
                try
                {
                    (void)CopyText(winrt::unbox_value<winrt::hstring>(
                        args.ClickedItem()).c_str());
                }
                catch (...)
                {
                }
            });
        fontAwesomeIconClickToken = fontAwesomeIconsGrid.ItemClick(
            [](const winrt::Windows::Foundation::IInspectable&,
                const muxc::ItemClickEventArgs& args) {
                try
                {
                    (void)CopyText(winrt::unbox_value<winrt::hstring>(
                        args.ClickedItem()).c_str());
                }
                catch (...)
                {
                }
            });
        fluentIconContainerToken = fluentIconsGrid.ContainerContentChanging(
            [this](const muxc::ListViewBase&,
                const muxc::ContainerContentChangingEventArgs& args) {
                if (args.InRecycleQueue() || !args.ItemContainer()) return;
                try
                {
                    const winrt::hstring glyph =
                        winrt::unbox_value<winrt::hstring>(args.Item());
                    const std::wstring tooltip = GlyphTooltip(
                        "app.settings.fluent_copy_tooltip",
                        L"U+%04X\nClick to copy", glyph.c_str());
                    muxc::ToolTipService::SetToolTip(
                        args.ItemContainer(), winrt::box_value(tooltip));
                    SetAutomation(args.ItemContainer(), glyph.c_str(),
                        tooltip);
                }
                catch (...)
                {
                }
            });
        fontAwesomeIconContainerToken =
            fontAwesomeIconsGrid.ContainerContentChanging(
                [this](const muxc::ListViewBase&,
                    const muxc::ContainerContentChangingEventArgs& args) {
                    if (args.InRecycleQueue() || !args.ItemContainer())
                        return;
                    try
                    {
                        const winrt::hstring glyph =
                            winrt::unbox_value<winrt::hstring>(args.Item());
                        const std::wstring tooltip = GlyphTooltip(
                            "app.settings.fa_copy_tooltip",
                            L"U+%04X\nClick to copy", glyph.c_str());
                        muxc::ToolTipService::SetToolTip(
                            args.ItemContainer(), winrt::box_value(tooltip));
                        SetAutomation(args.ItemContainer(), glyph.c_str(),
                            tooltip);
                    }
                    catch (...)
                    {
                    }
                });

        debugRefreshToken = debugRefreshButton.Click(
            [this](const winrt::Windows::Foundation::IInspectable&,
                const mux::RoutedEventArgs&) {
                WidgetsPageRequest request;
                request.command = WidgetsPageCommand::Refresh;
                Emit(std::move(request));
            });
    }

    void Emit(WidgetsPageRequest request) const
    {
        if (!closed && active && actions.invoke)
            actions.invoke(generation, std::move(request));
    }

    template <typename Control, typename Handler>
    void HookClick(
        const Control& control,
        Handler&& handler,
        std::vector<std::function<void()>>& revokers)
    {
        const winrt::event_token token = control.Click(
            std::forward<Handler>(handler));
        revokers.emplace_back([control, token]() {
            control.Click(token);
        });
    }

    template <typename Handler>
    void HookToggled(
        const muxc::ToggleSwitch& control,
        Handler&& handler,
        std::vector<std::function<void()>>& revokers)
    {
        const winrt::event_token token = control.Toggled(
            std::forward<Handler>(handler));
        revokers.emplace_back([control, token]() {
            control.Toggled(token);
        });
    }

    static void Revoke(std::vector<std::function<void()>>& revokers) noexcept
    {
        for (auto& revoke : revokers)
        {
            try
            {
                revoke();
            }
            catch (...)
            {
            }
        }
        revokers.clear();
    }

    void RevokePackageRowEvents() noexcept
    {
        for (PackageRowBinding& binding : packageRowBindings)
            Revoke(binding.revokers);
    }

    [[nodiscard]] bool MatchesQuery(
        const InstalledWidgetPackageSnapshot& package) const
    {
        if (ContainsInsensitive(package.name, query) ||
            ContainsInsensitive(package.description, query) ||
            ContainsInsensitive(package.packageId, query) ||
            ContainsInsensitive(package.author, query) ||
            ContainsInsensitive(package.sourceName, query) ||
            ContainsInsensitive(package.version, query))
            return true;
        for (const auto& source : package.invalidSources)
        {
            if (ContainsInsensitive(source.sourceName, query) ||
                ContainsInsensitive(source.version, query) ||
                ContainsInsensitive(source.rootName, query))
                return true;
            for (const auto& issue : source.issues)
            {
                if (ContainsInsensitive(issue.code, query) ||
                    ContainsInsensitive(issue.message, query))
                    return true;
            }
        }
        return std::any_of(package.workshopInstallFailures.begin(),
            package.workshopInstallFailures.end(), [&](const auto& failure) {
                return ContainsInsensitive(failure.sourceId, query) ||
                    ContainsInsensitive(failure.externalItemId, query) ||
                    ContainsInsensitive(failure.version, query) ||
                    ContainsInsensitive(failure.error, query);
            });
    }

    [[nodiscard]] bool MatchesFilter(
        const InstalledWidgetPackageSnapshot& package) const
    {
        switch (filter)
        {
        case PackageFilter::Installed:
            if (!IsInstalledPackage(package))
                return false;
            break;
        case PackageFilter::Included:
            if (!IsIncludedPackage(package))
                return false;
            break;
        case PackageFilter::Development:
            if (!IsDevelopmentPackage(package))
                return false;
            break;
        case PackageFilter::All:
        default:
            break;
        }
        return MatchesQuery(package);
    }

    [[nodiscard]] static bool IsIncludedPackage(
        const InstalledWidgetPackageSnapshot& package) noexcept
    {
        return package.builtIn;
    }

    [[nodiscard]] static bool IsInstalledPackage(
        const InstalledWidgetPackageSnapshot& package) noexcept
    {
        return package.canEnable || package.canUninstall ||
            !package.workshopInstallFailures.empty();
    }

    [[nodiscard]] static bool IsDevelopmentPackage(
        const InstalledWidgetPackageSnapshot& package) noexcept
    {
        return package.canUseDevelopmentOverride || package.development ||
            std::any_of(package.invalidSources.begin(),
                package.invalidSources.end(), [](const auto& source) {
                    return source.development;
                });
    }

    [[nodiscard]] std::size_t PackageCount() const noexcept
    {
        return packages.size();
    }

    [[nodiscard]] std::size_t InstalledPackageCount() const noexcept
    {
        return static_cast<std::size_t>(std::count_if(
            packages.begin(), packages.end(), IsInstalledPackage));
    }

    [[nodiscard]] std::size_t DevelopmentPackageCount() const noexcept
    {
        return static_cast<std::size_t>(std::count_if(
            packages.begin(), packages.end(), IsDevelopmentPackage));
    }

    [[nodiscard]] std::size_t IncludedPackageCount() const noexcept
    {
        return static_cast<std::size_t>(std::count_if(
            packages.begin(), packages.end(), IsIncludedPackage));
    }

    [[nodiscard]] muxc::Border MakePackageTag(
        std::wstring text) const
    {
        muxc::Border tag;
        tag.Padding({8.0, 3.0, 8.0, 3.0});
        tag.BorderThickness({1.0, 1.0, 1.0, 1.0});
        tag.CornerRadius({5.0, 5.0, 5.0, 5.0});
        tag.HorizontalAlignment(mux::HorizontalAlignment::Left);
        tag.IsHitTestVisible(false);

        muxc::TextBlock label;
        label.Text(std::move(text));
        label.FontSize(12.0);
        label.TextWrapping(mux::TextWrapping::NoWrap);
        tag.Child(label);

        try
        {
            if (const auto application = mux::Application::Current())
            {
                const auto resources = application.Resources();
                if (const auto background = resources.TryLookup(
                        winrt::box_value(
                            L"SolidBackgroundFillColorTertiaryBrush"))
                        .try_as<muxm::Brush>())
                {
                    tag.Background(background);
                }
                if (const auto border = resources.TryLookup(winrt::box_value(
                        L"CardStrokeColorDefaultBrush"))
                        .try_as<muxm::Brush>())
                {
                    tag.BorderBrush(border);
                }
            }
        }
        catch (...)
        {
            // Tags remain readable with the default transparent brushes when
            // an older WinUI resource dictionary omits these optional keys.
        }
        SetAutomation(tag, label.Text());
        return tag;
    }

    void PopulatePackageTags(
        const muxc::StackPanel& tags,
        const InstalledWidgetPackageSnapshot& package) const
    {
        tags.Children().Clear();
        if (IsInstalledPackage(package))
        {
            tags.Children().Append(MakePackageTag(L(
                "app.settings.widgets_filter_installed", L"Installed")));
        }
        if (IsIncludedPackage(package))
        {
            tags.Children().Append(MakePackageTag(L(
                "app.settings.widgets_filter_builtin", L"Included")));
        }
        if (IsDevelopmentPackage(package))
        {
            tags.Children().Append(MakePackageTag(L(
                "app.settings.widgets_filter_development",
                L"Local development")));
        }
        tags.Visibility(tags.Children().Size() == 0
                ? mux::Visibility::Collapsed
                : mux::Visibility::Visible);
    }

    [[nodiscard]] std::wstring FilterText(
        std::string_view key,
        std::wstring_view fallback,
        std::size_t count) const
    {
        return L(key, fallback) + L" " + std::to_wstring(count);
    }

    void RefreshFilterItems()
    {
        const std::size_t installedCount = InstalledPackageCount();
        const std::size_t includedCount = IncludedPackageCount();
        const std::size_t developmentCount = DevelopmentPackageCount();
        allFilterButton.Content(winrt::box_value(FilterText(
            "app.settings.widgets_filter_all", L"All", PackageCount())));
        installedFilterButton.Content(winrt::box_value(FilterText(
            "app.settings.widgets_filter_installed", L"Installed",
            installedCount)));
        includedFilterButton.Content(winrt::box_value(FilterText(
            "app.settings.widgets_filter_builtin", L"Included",
            includedCount)));
        developmentFilterButton.Content(winrt::box_value(FilterText(
            "app.settings.widgets_filter_development", L"Local development",
            developmentCount)));
        allFilterButton.IsChecked(filter == PackageFilter::All);
        installedFilterButton.IsChecked(filter == PackageFilter::Installed);
        includedFilterButton.IsChecked(filter == PackageFilter::Included);
        developmentFilterButton.IsChecked(
            filter == PackageFilter::Development);
        SetAutomation(allFilterButton,
            FilterText("app.settings.widgets_filter_all", L"All",
                PackageCount()));
        SetAutomation(installedFilterButton,
            FilterText("app.settings.widgets_filter_installed", L"Installed",
                installedCount));
        SetAutomation(includedFilterButton,
            FilterText("app.settings.widgets_filter_builtin", L"Included",
                includedCount));
        SetAutomation(developmentFilterButton,
            FilterText("app.settings.widgets_filter_development",
                L"Local development", developmentCount));
    }

    [[nodiscard]] const InstalledWidgetPackageSnapshot* FindPackage(
        std::wstring_view packageId) const noexcept
    {
        const auto found = std::find_if(packages.begin(), packages.end(),
            [packageId](const InstalledWidgetPackageSnapshot& package) {
                return package.packageId == packageId;
            });
        return found == packages.end() ? nullptr : &*found;
    }

    void OpenPermissionEditor(
        const InstalledWidgetPackageSnapshot& package)
    {
        if (closed || !active || !actions.editPermissions ||
            !actions.invoke || package.permissions.empty())
            return;

        WidgetPermissionEditorRequest editor;
        editor.packageId = package.packageId;
        editor.packageName = package.name;
        editor.version = package.version;
        editor.sourceId = package.sourceId;
        editor.sourceExternalItemId = package.sourceExternalItemId;
        editor.scopeFingerprint = package.permissionScopeFingerprint;
        editor.permissionState = package.permissionState;
        editor.canRevoke = package.canRevokePermissions;
        editor.permissions = package.permissions;
        editor.declaredNetworkDomains = package.declaredNetworkDomains;
        if (package.permissionState == WidgetPackagePermissionState::Pending ||
            package.permissionState == WidgetPackagePermissionState::Denied)
        {
            for (auto& permission : editor.permissions)
                permission.granted = permission.required ||
                    !permission.requiresConsent;
        }

        const std::uint64_t requestGeneration = generation;
        const std::uint64_t requestActivation =
            callbackGate->activation.load(std::memory_order_acquire);
        const std::shared_ptr<CallbackGenerationGate> gate = callbackGate;
        const auto invoke = actions.invoke;
        WidgetsPageRequest command;
        command.command = WidgetsPageCommand::SetPermissionDecision;
        command.packageId = package.packageId;
        command.version = package.version;
        command.sourceId = package.sourceId;
        command.externalItemId = package.sourceExternalItemId;
        command.scopeFingerprint = package.permissionScopeFingerprint;
        actions.editPermissions(requestGeneration, std::move(editor),
            [gate, invoke, requestGeneration, requestActivation,
                command = std::move(command)](
                    WidgetPermissionEditorResult result) mutable {
                if (!invoke ||
                    result.action == WidgetPermissionEditorAction::Cancel ||
                    gate->closed.load(std::memory_order_acquire) ||
                    !gate->active.load(std::memory_order_acquire) ||
                    gate->generation.load(std::memory_order_acquire) !=
                        requestGeneration ||
                    gate->activation.load(std::memory_order_acquire) !=
                        requestActivation)
                    return;
                command.permissionState =
                    result.action == WidgetPermissionEditorAction::Revoke
                    ? WidgetPackagePermissionState::Denied
                    : WidgetPackagePermissionState::Granted;
                command.grantedPermissions =
                    std::move(result.grantedPermissions);
                command.grantedNetworkDomains =
                    std::move(result.grantedNetworkDomains);
                invoke(requestGeneration, std::move(command));
            });
    }

    muxc::TextBlock MakeSecondaryText(std::wstring_view text) const
    {
        muxc::TextBlock block;
        block.Text(std::wstring(text));
        block.Opacity(0.72);
        block.TextWrapping(mux::TextWrapping::Wrap);
        return block;
    }

    muxc::Button MakeActionButton(
        std::wstring text,
        std::wstring help = {}) const
    {
        muxc::Button button;
        button.Content(winrt::box_value(text));
        button.HorizontalAlignment(mux::HorizontalAlignment::Right);
        button.VerticalAlignment(mux::VerticalAlignment::Center);
        button.HorizontalContentAlignment(mux::HorizontalAlignment::Center);
        button.UseSystemFocusVisuals(true);
        SetAutomation(button, text, help);
        return button;
    }

    static std::wstring FormatValues(
        std::wstring format,
        std::initializer_list<std::wstring_view> values)
    {
        for (const std::wstring_view value : values)
        {
            const std::size_t placeholder = format.find(L"%s");
            if (placeholder == std::wstring::npos)
            {
                if (!format.empty()) format += L" ";
                format.append(value);
                continue;
            }
            format.replace(placeholder, 2, value);
        }
        return format;
    }

    static std::wstring FormatCount(
        std::wstring format, std::size_t count)
    {
        const std::size_t marker = format.find(L"%d");
        if (marker != std::wstring::npos)
            format.replace(marker, 2, std::to_wstring(count));
        return format;
    }

    [[nodiscard]] std::wstring AgentSkillTargetLabel(
        WidgetAgentSkillTargetKind kind) const
    {
        switch (kind)
        {
        case WidgetAgentSkillTargetKind::Shared:
            return L("app.settings.widgets_skill_shared",
                L"Shared Agent Skills (recommended)");
        case WidgetAgentSkillTargetKind::Codex:
            return L"Codex";
        case WidgetAgentSkillTargetKind::ClaudeCode:
            return L"Claude Code";
        case WidgetAgentSkillTargetKind::Cursor:
            return L"Cursor";
        case WidgetAgentSkillTargetKind::GitHubCopilot:
            return L"GitHub Copilot";
        case WidgetAgentSkillTargetKind::GeminiCli:
            return L"Gemini CLI";
        default:
            return L"Agent Skills";
        }
    }

    [[nodiscard]] std::wstring AgentSkillStateLabel(
        WidgetAgentSkillInstallState state) const
    {
        switch (state)
        {
        case WidgetAgentSkillInstallState::NotInstalled:
            return L("app.settings.widgets_skill_not_installed",
                L"Not installed");
        case WidgetAgentSkillInstallState::UpdateAvailable:
            return L("app.settings.widgets_skill_update_available",
                L"Update available");
        case WidgetAgentSkillInstallState::Current:
            return L("app.settings.widgets_skill_current", L"Up to date");
        case WidgetAgentSkillInstallState::Unavailable:
        default:
            return L("app.settings.widgets_skill_unavailable",
                L"Unavailable");
        }
    }

    [[nodiscard]] static int AgentSkillTargetBit(
        WidgetAgentSkillTargetKind kind) noexcept
    {
        return 1 << static_cast<int>(kind);
    }

    [[nodiscard]] std::wstring BuildErrorCopyText() const
    {
        std::wstring text;
        for (const auto& error : errors)
        {
            text += L"[" + error.key + L"]\n";
            text += error.message + L"\n\n";
        }
        return text;
    }

    [[nodiscard]] std::wstring BuildDiagnosticsCopyText() const
    {
        std::wstring text;
        const auto appendViewNodes = [&text](std::wstring_view surface,
                                             const auto& nodes) {
            for (const auto& node : nodes)
            {
                text += L"view[" + std::wstring(surface) +
                    L"] depth=" + std::to_wstring(node.depth) +
                    L", type=" + node.type + L", key=" + node.key;
                if (!node.debugName.empty())
                    text += L", debugName=" + node.debugName;
                if (!node.testId.empty())
                    text += L", testId=" + node.testId;
                text += L", frame=" + std::to_wstring(node.x) + L"," +
                    std::to_wstring(node.y) + L"," +
                    std::to_wstring(node.width) + L"," +
                    std::to_wstring(node.height) + L"\n";
            }
        };
        for (const auto& diagnostic : diagnostics)
        {
            text += L"[" + diagnostic.instanceId + L"] " +
                diagnostic.displayName + L"\n";
            text += std::wstring(L"valid=") +
                (diagnostic.valid ? L"true" : L"false") +
                L", manifest=" +
                (diagnostic.hasManifest ? L"true" : L"false") + L"\n";
            text += L"permissions=";
            for (std::size_t index = 0;
                 index < diagnostic.permissions.size(); ++index)
            {
                if (index != 0) text += L",";
                text += diagnostic.permissions[index];
            }
            text += L"\n";
            if (!diagnostic.lastError.empty())
                text += L"lastError=" + diagnostic.lastError + L"\n";
            for (const auto& log : diagnostic.recentLogs)
                text += log.level + L": " + log.message + L"\n";
            appendViewNodes(L"desktop", diagnostic.desktopViewNodes);
            appendViewNodes(diagnostic.auxiliarySurface.empty()
                    ? std::wstring_view(L"auxiliary")
                    : std::wstring_view(diagnostic.auxiliarySurface),
                diagnostic.auxiliaryViewNodes);
            text += L"\n";
        }
        return text;
    }

    void AddDevelopmentOverrideRow(
        const InstalledWidgetPackageSnapshot& package)
    {
        muxc::Expander row;
        row.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        row.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        row.IsExpanded(false);
        muxc::StackPanel header;
        header.Spacing(3.0);
        muxc::TextBlock name;
        name.Text(package.name.empty() ? package.packageId : package.name);
        name.FontWeight(
            winrt::Windows::UI::Text::FontWeights::SemiBold());
        name.TextWrapping(mux::TextWrapping::Wrap);
        header.Children().Append(name);
        const std::wstring metadata = JoinMetadata(
            {package.version, package.packageId, package.sourceName});
        if (!metadata.empty())
            header.Children().Append(MakeSecondaryText(metadata));
        row.Header(header);

        muxc::StackPanel body;
        body.Spacing(8.0);
        body.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        if (!package.description.empty())
            body.Children().Append(MakeSecondaryText(package.description));

        if (developerOverridesVisible &&
            package.canUseDevelopmentOverride)
        {
            muxc::ToggleSwitch development;
            development.IsOn(package.developmentOverrideActive);
            development.HorizontalAlignment(
                mux::HorizontalAlignment::Right);
            development.UseSystemFocusVisuals(true);
            SetAutomation(development,
                L("app.settings.widgets_development",
                    L"Development version"),
                L("settings.developer.overrides.description",
                    L"Use the local development source."));
            HookToggled(development,
                [this, development, packageId = package.packageId](
                    const winrt::Windows::Foundation::IInspectable&,
                    const mux::RoutedEventArgs&) {
                    if (updatingControls || closed || !active)
                        return;
                    WidgetsPageRequest request;
                    request.command =
                        WidgetsPageCommand::SetDevelopmentOverride;
                    request.packageId = packageId;
                    request.enabled = development.IsOn();
                    Emit(std::move(request));
                },
                developerRevokers);
            presenter_controls::SettingRow developmentRow;
            developmentRow.Initialize(development, 180.0);
            developmentRow.SetControlAlignment(
                mux::HorizontalAlignment::Right);
            developmentRow.SetText(L(
                    "app.settings.widgets_development",
                    L"Development version"),
                L("settings.developer.overrides.description",
                    L"Use the local development source."));
            body.Children().Append(developmentRow.root);
            if (!firstDeveloperOverrideTarget)
                firstDeveloperOverrideTarget = development;
        }
        else
        {
            body.Children().Append(MakeSecondaryText(L(
                package.developmentOverrideActive
                    ? "app.settings.widgets_development_active"
                    : "app.settings.widgets_development_inactive",
                package.developmentOverrideActive
                    ? L"Development version active"
                    : L"Development version inactive")));
        }

        row.Content(body);
        StretchExpanderBody(row, body);
        SetAutomation(row, name.Text(), package.description);
        if (!firstDeveloperOverrideTarget)
            firstDeveloperOverrideTarget = row;
        developerOverrideRows.Children().Append(row);
    }

    void AddDiagnosticRow(
        const WidgetRuntimeDiagnosticSnapshot& diagnostic,
        const muxc::StackPanel& rows,
        bool allowReload,
        std::vector<std::function<void()>>& revokers,
        mux::FrameworkElement& firstTarget)
    {
        muxc::StackPanel row;
        row.Spacing(6.0);
        row.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        muxcp::ToggleButton disclosure;
        disclosure.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        disclosure.HorizontalContentAlignment(
            mux::HorizontalAlignment::Stretch);
        disclosure.UseSystemFocusVisuals(true);
        disclosure.IsChecked(false);
        muxc::Grid headerGrid;
        headerGrid.ColumnSpacing(12.0);
        muxc::ColumnDefinition headerColumn;
        headerColumn.Width(mux::GridLengthHelper::FromValueAndType(
            1.0, mux::GridUnitType::Star));
        headerGrid.ColumnDefinitions().Append(headerColumn);
        muxc::ColumnDefinition chevronColumn;
        chevronColumn.Width(mux::GridLengthHelper::Auto());
        headerGrid.ColumnDefinitions().Append(chevronColumn);
        muxc::StackPanel header;
        header.Spacing(3.0);
        muxc::TextBlock name;
        name.Text(diagnostic.displayName.empty()
                ? diagnostic.instanceId
                : diagnostic.displayName);
        name.FontWeight(
            winrt::Windows::UI::Text::FontWeights::SemiBold());
        name.TextWrapping(mux::TextWrapping::Wrap);
        header.Children().Append(name);
        header.Children().Append(MakeSecondaryText(JoinMetadata(
            {diagnostic.instanceId, diagnostic.packageId})));
        headerGrid.Children().Append(header);
        muxc::TextBlock chevron;
        chevron.Text(L"⌄");
        chevron.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::Grid::SetColumn(chevron, 1);
        headerGrid.Children().Append(chevron);
        disclosure.Content(headerGrid);
        row.Children().Append(disclosure);

        muxc::StackPanel body;
        body.Spacing(7.0);
        body.Margin(mux::ThicknessHelper::FromLengths(12.0, 2.0, 12.0, 8.0));
        body.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        body.Visibility(mux::Visibility::Collapsed);
        const std::wstring validity = L(diagnostic.valid
                ? "app.settings.valid" : "app.settings.invalid",
            diagnostic.valid ? L"Valid" : L"Invalid");
        const std::wstring manifest = L(diagnostic.hasManifest
                ? "app.settings.yes" : "app.settings.no",
            diagnostic.hasManifest ? L"Yes" : L"No");
        body.Children().Append(MakeSecondaryText(FormatValues(
            L("app.settings.debug_status_manifest",
                L"Status: %s | Manifest: %s"),
            {validity, manifest})));
        if (!diagnostic.scriptPath.empty())
        {
            body.Children().Append(MakeSecondaryText(FormatValues(
                L("app.settings.debug_script", L"Script: %s"),
                {diagnostic.scriptPath})));
        }
        std::wstring permissions;
        for (const std::wstring& permission : diagnostic.permissions)
        {
            if (!permissions.empty()) permissions += L", ";
            permissions += permission;
        }
        body.Children().Append(MakeSecondaryText(FormatValues(
            L("app.settings.debug_permissions", L"Permissions: %s"),
            {permissions.empty()
                    ? L("app.settings.none", L"(None)")
                    : permissions})));
        if (!diagnostic.lastError.empty())
        {
            body.Children().Append(MakeSecondaryText(FormatValues(
                L("app.settings.debug_last_error",
                    L"Most Recent Error: %s"),
                {diagnostic.lastError})));
        }
        if (!diagnostic.recentLogs.empty())
        {
            muxc::TextBlock logsTitle;
            logsTitle.Text(L("app.settings.recent_logs", L"Recent Logs"));
            logsTitle.FontWeight(
                winrt::Windows::UI::Text::FontWeights::SemiBold());
            body.Children().Append(logsTitle);
            for (const auto& log : diagnostic.recentLogs)
            {
                std::wstring text;
                if (!log.level.empty())
                    text = L"[" + log.level + L"] ";
                text += log.message;
                body.Children().Append(MakeSecondaryText(text));
            }
        }

        if (allowReload)
        {
            muxc::Button reload = MakeActionButton(
                L("app.settings.reload", L"Reload"),
                diagnostic.instanceId);
            reload.HorizontalAlignment(mux::HorizontalAlignment::Right);
            reload.Width(96.0);
            reload.IsEnabled(static_cast<bool>(
                actions.reloadWidgetInstance));
            HookClick(reload,
                [this, instanceId = diagnostic.instanceId](
                    const winrt::Windows::Foundation::IInspectable&,
                    const mux::RoutedEventArgs&) {
                    if (!closed && active &&
                        actions.reloadWidgetInstance)
                    {
                        actions.reloadWidgetInstance(
                            generation, instanceId);
                    }
                },
                revokers);
            body.Children().Append(reload);
        }

        row.Children().Append(body);
        const auto weakBody = winrt::make_weak(body);
        const auto weakChevron = winrt::make_weak(chevron);
        HookClick(disclosure,
            [disclosure, weakBody, weakChevron](const auto&, const auto&) {
                const auto checked = disclosure.IsChecked();
                const bool expanded = checked && checked.Value();
                if (const auto currentBody = weakBody.get())
                {
                    currentBody.Visibility(expanded
                            ? mux::Visibility::Visible
                            : mux::Visibility::Collapsed);
                }
                if (const auto currentChevron = weakChevron.get())
                    currentChevron.Text(expanded ? L"⌃" : L"⌄");
            },
            revokers);
        SetAutomation(disclosure, name.Text(), diagnostic.lastError);
        if (!firstTarget)
            firstTarget = disclosure;
        rows.Children().Append(row);
    }

    void RenderDeveloperRows()
    {
        Revoke(developerRevokers);
        agentSkillRows.Children().Clear();
        cliRows.Children().Clear();
        developerErrorRows.Children().Clear();
        developerOverrideRows.Children().Clear();
        developerDiagnosticRows.Children().Clear();
        firstDeveloperOverrideTarget = nullptr;
        firstDeveloperDiagnosticTarget = nullptr;

        int pendingTargets = 0;
        bool selectedTargetUnavailable = false;
        for (const auto& skill : agentSkills)
        {
            const bool needsInstall =
                skill.state == WidgetAgentSkillInstallState::NotInstalled ||
                skill.state ==
                    WidgetAgentSkillInstallState::UpdateAvailable;
            if ((skill.selected && needsInstall) ||
                (!skill.selected && skill.installed))
            {
                ++pendingTargets;
            }
            if (skill.selected && skill.state ==
                    WidgetAgentSkillInstallState::Unavailable)
            {
                selectedTargetUnavailable = true;
            }
        }
        const bool selectionApplied = !agentSkills.empty() &&
            pendingTargets == 0 && !selectedTargetUnavailable;
        const std::wstring applyLabel = pendingTargets > 0
            ? L("app.settings.widgets_skill_sync_all", L"Apply Selection")
            : selectionApplied
                ? L("app.settings.widgets_skill_all_current",
                    L"Selection Applied")
                : L("app.settings.widgets_skill_unavailable",
                    L"Unavailable");
        agentSkillApplyButton.Content(winrt::box_value(applyLabel));
        agentSkillApplyButton.IsEnabled(pendingTargets != 0 &&
            static_cast<bool>(actions.invoke));
        agentSkillRefreshButton.IsEnabled(static_cast<bool>(actions.invoke));
        agentSkillActionsRow.SetText(L(
            "app.settings.widgets_skill_all_agents",
            L"All compatible assistants"));
        SetAutomation(agentSkillApplyButton, applyLabel,
            developerOverridesCard.description.Text());

        for (const auto& skill : agentSkills)
        {
            muxc::CheckBox selected;
            selected.IsChecked(skill.selected);
            selected.IsEnabled(static_cast<bool>(actions.invoke));
            selected.HorizontalAlignment(mux::HorizontalAlignment::Right);
            selected.UseSystemFocusVisuals(true);
            const std::wstring label = AgentSkillTargetLabel(skill.kind);
            const std::wstring stateAndPath = JoinMetadata(
                {AgentSkillStateLabel(skill.state), skill.targetPath});
            SetAutomation(selected, label, stateAndPath);
            HookClick(selected,
                [this, selected, bit = AgentSkillTargetBit(skill.kind)](
                    const winrt::Windows::Foundation::IInspectable&,
                    const mux::RoutedEventArgs&) {
                    if (updatingControls || closed || !active)
                        return;
                    int mask = agentSkillTargetMask;
                    const auto checked = selected.IsChecked();
                    if (checked && checked.Value())
                        mask |= bit;
                    else
                        mask &= ~bit;
                    WidgetsPageRequest request;
                    request.command =
                        WidgetsPageCommand::SetAgentSkillTargetSelection;
                    request.agentSkillTargetMask = mask;
                    Emit(std::move(request));
                },
                developerRevokers);
            presenter_controls::SettingRow row;
            row.Initialize(selected, 180.0);
            row.SetControlAlignment(mux::HorizontalAlignment::Right);
            row.SetText(label, stateAndPath);
            agentSkillRows.Children().Append(row.root);
            if (!firstDeveloperOverrideTarget)
                firstDeveloperOverrideTarget = selected;
        }
        if (!agentSkillStatusError.empty())
            agentSkillRows.Children().Append(
                MakeSecondaryText(agentSkillStatusError));
        if (!developerActionStatus.empty())
            agentSkillRows.Children().Append(
                MakeSecondaryText(developerActionStatus));

        developmentWorkspacePath.Text(developmentWorkspace);
        openDevelopmentFolderButton.IsEnabled(
            static_cast<bool>(actions.invoke));
        developmentWorkspaceRow.SetText(L(
            "app.settings.widgets_authoring_folder",
            L"Development components folder"));

        const auto quote = [](std::wstring value) {
            return L"\"" + value + L"\"";
        };
        const std::wstring project = quote(developmentWorkspace.empty()
                ? L"my-widget"
                : developmentWorkspace + L"\\my-widget");
        const std::wstring output = quote(developmentWorkspace.empty()
                ? L"my-widget.snowwidget"
                : developmentWorkspace + L"\\my-widget.snowwidget");
        const std::wstring cli = quote(componentCliPath);
        const auto addCommandRow = [this](std::string_view labelKey,
                                           std::wstring_view fallback,
                                           std::wstring command) {
            muxc::TextBox value;
            value.Text(command);
            value.IsReadOnly(true);
            value.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
            value.MinWidth(80.0);
            value.UseSystemFocusVisuals(true);
            muxc::Button copy = MakeActionButton(L(
                "app.settings.widgets_copy_command", L"Copy"));
            copy.HorizontalAlignment(mux::HorizontalAlignment::Right);
            muxc::Grid controls;
            controls.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
            controls.ColumnSpacing(8.0);
            muxc::ColumnDefinition valueColumn;
            valueColumn.Width(mux::GridLengthHelper::FromValueAndType(
                1.0, mux::GridUnitType::Star));
            muxc::ColumnDefinition copyColumn;
            copyColumn.Width(mux::GridLengthHelper::Auto());
            controls.ColumnDefinitions().Append(valueColumn);
            controls.ColumnDefinitions().Append(copyColumn);
            controls.Children().Append(value);
            muxc::Grid::SetColumn(copy, 1);
            controls.Children().Append(copy);
            HookClick(copy,
                [this, command = std::move(command)](
                    const winrt::Windows::Foundation::IInspectable&,
                    const mux::RoutedEventArgs&) {
                    if (CopyText(command))
                    {
                        developerFeedbackBar.Severity(
                            muxc::InfoBarSeverity::Success);
                        developerFeedbackBar.Message(L(
                            "app.settings.widgets_command_copied",
                            L"Command copied."));
                        developerFeedbackBar.IsOpen(true);
                    }
                },
                developerRevokers);
            presenter_controls::SettingRow row;
            row.Initialize(controls);
            row.SetText(L(labelKey, fallback));
            cliRows.Children().Append(row.root);
        };
        addCommandRow("app.settings.widgets_cli_capabilities",
            L"Capabilities", cli + L" capabilities");
        addCommandRow("app.settings.widgets_cli_validate",
            L"Validate component", cli + L" validate " + project);
        addCommandRow("app.settings.widgets_cli_pack",
            L"Package component",
            cli + L" pack " + project + L" " + output);
        cliRows.Children().Append(MakeSecondaryText(componentCliPath));

        developerPublishCard.root.Visibility(developerPublisherAvailable
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
        publishWorkspaceButton.IsEnabled(developerPublisherAvailable &&
            static_cast<bool>(actions.invoke));

        const std::size_t fluentCount = fluentIconsGrid.Items().Size();
        const std::size_t fontAwesomeCount =
            fontAwesomeIconsGrid.Items().Size();
        fluentIconsCount.Text(fluentCount == 0
                ? L("app.settings.fluent_not_found",
                    L"No usable Fluent Regular glyphs found.")
                : FormatCount(L("app.settings.fluent_valid_chars",
                    L"Available characters: %d"), fluentCount));
        fontAwesomeIconsCount.Text(fontAwesomeCount == 0
                ? L("app.settings.fa_not_found",
                    L"No usable Font Awesome glyphs found.")
                : FormatCount(L("app.settings.fa_valid_chars",
                    L"Available characters: %d"), fontAwesomeCount));

        const std::wstring errorLabel = FormatCount(L(
            "app.settings.error_count", L"Error Log: %d"), errors.size());
        developerErrorActionsRow.SetText(errorLabel);
        copyAllErrorsButton.IsEnabled(!errors.empty());
        clearAllErrorsButton.IsEnabled(!errors.empty() &&
            static_cast<bool>(actions.invoke));
        if (errors.empty())
        {
            developerErrorRows.Children().Append(MakeSecondaryText(L(
                "app.settings.no_widget_errors",
                L"No widget error records.")));
        }
        else
        {
            for (const auto& error : errors)
            {
                const std::wstring text =
                    L"[" + error.key + L"]\n" + error.message;
                muxc::Button item;
                muxc::TextBlock itemText;
                itemText.Text(text);
                itemText.TextWrapping(mux::TextWrapping::Wrap);
                item.Content(itemText);
                item.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
                item.HorizontalContentAlignment(
                    mux::HorizontalAlignment::Left);
                item.UseSystemFocusVisuals(true);
                SetAutomation(item, text,
                    L("app.settings.copy_error",
                        L"Click to copy this error"));
                HookClick(item,
                    [text](const winrt::Windows::Foundation::IInspectable&,
                        const mux::RoutedEventArgs&) {
                        (void)CopyText(text);
                    },
                    developerRevokers);
                developerErrorRows.Children().Append(item);
            }
        }

        const std::wstring diagnosticLabel = FormatCount(L(
            "app.settings.widgets_diagnostic_count",
            L"Component diagnostics: %d"), diagnostics.size());
        developerDiagnosticActionsRow.SetText(diagnosticLabel);
        copyDiagnosticsButton.IsEnabled(!diagnostics.empty());
        if (diagnostics.empty())
        {
            developerDiagnosticRows.Children().Append(MakeSecondaryText(
                L("app.settings.no_widgets_loaded",
                    L"No Lua widgets are currently loaded.")));
        }
        else
        {
            for (const auto& diagnostic : diagnostics)
            {
                AddDiagnosticRow(diagnostic, developerDiagnosticRows, true,
                    developerRevokers, firstDeveloperDiagnosticTarget);
            }
        }
    }

    void RenderDebugRows()
    {
        Revoke(debugRevokers);
        debugDiagnosticRows.Children().Clear();
        firstDebugDiagnosticTarget = nullptr;
        if (diagnostics.empty())
        {
            debugDiagnosticRows.Children().Append(MakeSecondaryText(
                L("app.settings.no_widgets_loaded",
                    L"No Lua widgets are currently loaded.")));
            return;
        }
        for (const auto& diagnostic : diagnostics)
        {
            AddDiagnosticRow(diagnostic, debugDiagnosticRows, false,
                debugRevokers, firstDebugDiagnosticTarget);
        }
    }

    void AddPermissionControls(
        const InstalledWidgetPackageSnapshot& package,
        const muxc::StackPanel& body,
        std::vector<std::function<void()>>& revokers)
    {
        muxc::Expander permissionsExpander;
        permissionsExpander.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        permissionsExpander.HorizontalContentAlignment(
            mux::HorizontalAlignment::Stretch);
        muxc::TextBlock permissionHeader;
        const std::wstring permissionState =
            PermissionStateText(package.permissionState);
        permissionHeader.Text(L("app.settings.widgets_permissions",
            L"Permissions") + L" · " + permissionState);
        permissionHeader.FontWeight(
            winrt::Windows::UI::Text::FontWeights::SemiBold());
        permissionHeader.TextWrapping(mux::TextWrapping::Wrap);
        permissionsExpander.Header(permissionHeader);

        muxc::StackPanel permissionBody;
        permissionBody.Spacing(7.0);
        permissionBody.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        const std::wstring stateText =
            L("app.settings.widgets_permission_state",
                L"Permission status") + L": " +
            permissionState;
        permissionBody.Children().Append(MakeSecondaryText(stateText));

        if (package.permissions.empty())
        {
            permissionBody.Children().Append(MakeSecondaryText(
                L("app.settings.widgets_no_permissions",
                    L"No additional access required")));
        }
        else
        {
            for (const WidgetPermissionSnapshot& permission :
                 package.permissions)
            {
                std::wstring label = permission.label;
                if (!permission.labelKey.empty())
                    label = L(permission.labelKey, label);
                if (label.empty())
                    label = permission.id;

                permissionBody.Children().Append(MakeSecondaryText(
                    std::wstring(permission.granted ? L"✓ " : L"○ ") +
                    label));
            }

            if (!package.declaredNetworkDomains.empty())
            {
                std::wstring domains = L(
                    "app.settings.widgets_network_domains",
                    L"Allowed network domains");
                domains += L": ";
                for (std::size_t index = 0;
                    index < package.declaredNetworkDomains.size(); ++index)
                {
                    if (index != 0)
                        domains += L", ";
                    domains += package.declaredNetworkDomains[index];
                }
                permissionBody.Children().Append(
                    MakeSecondaryText(domains));
            }

            muxc::Button manage = MakeActionButton(
                L("app.settings.widgets_manage_permissions",
                    L"Manage access"),
                L("app.settings.widgets_permission_required_notice",
                    L"Clearing required access pauses this component."));
            manage.IsEnabled(static_cast<bool>(actions.editPermissions) &&
                static_cast<bool>(actions.invoke));
            HookClick(manage,
                [this, packageId = package.packageId](
                    const winrt::Windows::Foundation::IInspectable&,
                    const mux::RoutedEventArgs&) {
                    const auto* current = FindPackage(packageId);
                    if (current)
                        OpenPermissionEditor(*current);
                },
                revokers);
            if (!firstPermissionTarget)
                firstPermissionTarget = manage;
            permissionBody.Children().Append(manage);
        }

        permissionsExpander.Content(permissionBody);
        StretchExpanderBody(permissionsExpander, permissionBody);
        SetAutomation(permissionsExpander,
            permissionHeader.Text(), stateText);
        body.Children().Append(permissionsExpander);
    }

    void AddInstanceControls(
        const InstalledWidgetPackageSnapshot& package,
        const muxc::StackPanel& body,
        std::vector<std::function<void()>>& revokers)
    {
        if (package.instances.empty())
            return;

        muxc::Expander instancesExpander;
        instancesExpander.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        instancesExpander.HorizontalContentAlignment(
            mux::HorizontalAlignment::Stretch);
        muxc::TextBlock header;
        header.Text(L("settings.widget.fields", L"Widget settings"));
        header.FontWeight(
            winrt::Windows::UI::Text::FontWeights::SemiBold());
        instancesExpander.Header(header);

        muxc::StackPanel rows;
        rows.Spacing(7.0);
        rows.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        for (const WidgetInstanceSnapshot& instance : package.instances)
        {
            muxc::StackPanel row;
            row.Spacing(4.0);
            row.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
            muxc::TextBlock name;
            name.Text(instance.displayName.empty()
                    ? instance.instanceId
                    : instance.displayName);
            name.TextWrapping(mux::TextWrapping::Wrap);
            row.Children().Append(name);
            if (instance.settingsAvailable)
            {
                muxc::Button settings = MakeActionButton(
                    L("settings.widget.fields", L"Widget settings"),
                    L("settings.page.widget.description",
                        L"Open this instance's declarative settings."));
                HookClick(settings,
                    [this, instanceId = instance.instanceId](
                        const winrt::Windows::Foundation::IInspectable&,
                        const mux::RoutedEventArgs&) {
                        if (!closed && active && actions.navigate)
                        {
                            actions.navigate(generation,
                                SettingsRoute::ForWidget(instanceId));
                        }
                    },
                    revokers);
                row.Children().Append(settings);
            }
            rows.Children().Append(row);
        }
        instancesExpander.Content(rows);
        StretchExpanderBody(instancesExpander, rows);
        SetAutomation(instancesExpander, header.Text());
        body.Children().Append(instancesExpander);
    }

    void AddLegacyPackageActions(
        const InstalledWidgetPackageSnapshot& package,
        const muxc::StackPanel& body,
        std::vector<std::function<void()>>& revokers,
        muxc::TextBlock& sourceIdText,
        std::vector<muxc::Button>& advancedActions)
    {
        const bool hasWorkshopItem =
            !package.workshopExternalItemId.empty();
        const bool hasAdvancedAction = actions.invoke &&
            (package.canCreateDevelopmentProject ||
            package.canInstallDevelopmentSnapshot ||
            package.canPublishDevelopmentPackage ||
            !package.restorableVersions.empty() || hasWorkshopItem);
        muxc::Expander advanced;
        advanced.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        advanced.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        advanced.Header(winrt::box_value(L(
            "app.settings.widgets_advanced", L"Advanced Options")));
        muxc::StackPanel actionsPanel;
        actionsPanel.Spacing(7.0);
        actionsPanel.HorizontalAlignment(mux::HorizontalAlignment::Stretch);

        const auto appendTechnicalValue = [&](std::string_view labelKey,
                                               std::wstring_view fallback,
                                               std::wstring_view value) {
            if (!value.empty())
            {
                actionsPanel.Children().Append(MakeSecondaryText(
                    L(labelKey, fallback) + L": " + std::wstring(value)));
            }
        };
        appendTechnicalValue("app.settings.widgets_package_id",
            L"Component ID", package.packageId);
        sourceIdText = MakeSecondaryText(
            L("app.settings.widgets_provider_id", L"Source ID") + L": " +
            package.sourceId);
        sourceIdText.Visibility(package.sourceId.empty()
                ? mux::Visibility::Collapsed
                : mux::Visibility::Visible);
        actionsPanel.Children().Append(sourceIdText);
        appendTechnicalValue("app.settings.widgets_permission_state",
            L"Permission status", PermissionStateText(
                package.permissionState));

        const auto makeCommand = [this, &package, &revokers,
                                  &advancedActions](
            WidgetsPageCommand command,
            std::string_view labelKey,
            std::wstring_view fallback,
            std::wstring version = {}) {
            muxc::Button button = MakeActionButton(
                L(labelKey, fallback), package.description);
            HookClick(button,
                [this, command, packageId = package.packageId,
                    version = std::move(version)](
                    const winrt::Windows::Foundation::IInspectable&,
                    const mux::RoutedEventArgs&) {
                    const auto* current = FindPackage(packageId);
                    if (!current)
                        return;
                    WidgetsPageRequest request;
                    request.command = command;
                    request.packageId = packageId;
                    request.sourceId = command ==
                            WidgetsPageCommand::OpenWorkshopItem
                        ? L"steam-workshop" : current->sourceId;
                    request.externalItemId = command ==
                            WidgetsPageCommand::OpenWorkshopItem
                        ? current->workshopExternalItemId
                        : current->sourceExternalItemId;
                    request.version = version;
                    Emit(std::move(request));
                },
                revokers);
            advancedActions.push_back(button);
            return button;
        };

        if (hasAdvancedAction && package.canCreateDevelopmentProject)
        {
            actionsPanel.Children().Append(makeCommand(
                WidgetsPageCommand::CreateDevelopmentProject,
                "app.settings.widgets_create_development",
                L"Create Development Version"));
        }
        if (hasAdvancedAction && package.canInstallDevelopmentSnapshot)
        {
            actionsPanel.Children().Append(makeCommand(
                WidgetsPageCommand::InstallDevelopmentSnapshot,
                "app.settings.widgets_install_managed",
                L"Install to Installed"));
        }
        if (hasAdvancedAction && !package.restorableVersions.empty())
        {
            muxc::Expander versions;
            versions.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
            versions.HorizontalContentAlignment(
                mux::HorizontalAlignment::Stretch);
            versions.Header(winrt::box_value(L(
                "app.settings.widgets_old_versions",
                L"Restorable Older Versions")));
            muxc::StackPanel versionRows;
            versionRows.Spacing(6.0);
            versionRows.HorizontalAlignment(
                mux::HorizontalAlignment::Stretch);
            for (const auto& restorable : package.restorableVersions)
            {
                muxc::Button restore = makeCommand(
                    WidgetsPageCommand::RollbackPackage,
                    "app.settings.widgets_rollback_action",
                    L"Restore This Version", restorable.version);
                restore.Content(winrt::box_value(
                    L("app.settings.widgets_rollback_action",
                        L"Restore This Version") +
                    L" — " + restorable.version));
                versionRows.Children().Append(restore);
            }
            versions.Content(versionRows);
            StretchExpanderBody(versions, versionRows);
            actionsPanel.Children().Append(versions);
        }
        if (hasAdvancedAction && package.canPublishDevelopmentPackage)
        {
            actionsPanel.Children().Append(makeCommand(
                WidgetsPageCommand::PublishDevelopmentPackage,
                "app.settings.widgets_upload_steam", L"Upload to Steam"));
        }
        if (hasAdvancedAction && hasWorkshopItem)
        {
            actionsPanel.Children().Append(makeCommand(
                WidgetsPageCommand::OpenWorkshopItem,
                "app.settings.widgets_open_workshop_item",
                L"Open Workshop Page"));
        }

        advanced.Content(actionsPanel);
        StretchExpanderBody(advanced, actionsPanel);
        SetAutomation(advanced,
            L("app.settings.widgets_advanced", L"Advanced Options"),
            package.name);
        body.Children().Append(advanced);
    }

    [[nodiscard]] std::wstring PackageDisplayName(
        const InstalledWidgetPackageSnapshot& package) const
    {
        std::wstring displayName = package.name.empty()
            ? package.packageId : package.name;
        if (package.name.empty() &&
            !package.workshopInstallFailures.empty() &&
            !package.workshopInstallFailures.front().externalItemId.empty())
        {
            displayName = FormatValues(L(
                "app.settings.widgets_workshop_detected_item",
                L"Detected Workshop item %s"),
                {package.workshopInstallFailures.front().externalItemId});
        }
        return displayName;
    }

    void AddPackageRow(
        const InstalledWidgetPackageSnapshot& package,
        const muxc::StackPanel& targetRows)
    {
        muxc::Border row;
        std::vector<std::function<void()>> revokers;
        std::vector<muxc::Button> advancedActions;
        row.Style(cardStyle);
        row.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        row.IsTabStop(true);
        row.UseSystemFocusVisuals(true);

        muxc::StackPanel primaryActions;
        primaryActions.Orientation(muxc::Orientation::Horizontal);
        primaryActions.HorizontalAlignment(mux::HorizontalAlignment::Right);
        primaryActions.Spacing(8.0);

        presenter_controls::SettingRow summaryRow;
        summaryRow.Initialize(primaryActions, 0.0);
        summaryRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        summaryRow.SetText(PackageDisplayName(package), JoinMetadata(
            {package.sourceName, PackageStateText(package)}));
        muxc::TextBlock name = summaryRow.label;
        name.FontWeight(
            winrt::Windows::UI::Text::FontWeights::SemiBold());
        name.TextWrapping(mux::TextWrapping::Wrap);
        muxc::TextBlock metadata = summaryRow.help;
        const std::wstring packageState = PackageStateText(package);
        muxa::AutomationProperties::SetLiveSetting(metadata,
            muxa::Peers::AutomationLiveSetting::Polite);
        muxc::StackPanel tags;
        tags.Orientation(muxc::Orientation::Horizontal);
        tags.Spacing(6.0);
        tags.HorizontalAlignment(mux::HorizontalAlignment::Left);
        PopulatePackageTags(tags, package);

        muxc::StackPanel body;
        body.Spacing(8.0);
        body.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        body.Children().Append(summaryRow.root);
        body.Children().Append(tags);
        muxc::TextBlock description = MakeSecondaryText(package.description);
        description.MaxLines(2);
        description.TextTrimming(mux::TextTrimming::CharacterEllipsis);
        description.Visibility(package.description.empty()
                ? mux::Visibility::Collapsed
                : mux::Visibility::Visible);
        body.Children().Append(description);

        muxc::StackPanel detailsBody;
        detailsBody.Spacing(8.0);
        detailsBody.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        muxc::TextBlock version = MakeSecondaryText(
            L("app.settings.version", L"Version") + L": " +
            package.version);
        version.Visibility(package.version.empty()
                ? mux::Visibility::Collapsed
                : mux::Visibility::Visible);
        detailsBody.Children().Append(version);
        muxc::TextBlock author = MakeSecondaryText(
            L("app.settings.author", L"Author") + L": " +
            package.author);
        author.Visibility(package.author.empty()
                ? mux::Visibility::Collapsed
                : mux::Visibility::Visible);
        detailsBody.Children().Append(author);

        for (const auto& invalid : package.invalidSources)
        {
            detailsBody.Children().Append(MakeSecondaryText(JoinMetadata({
                invalid.sourceName, invalid.version,
                L("app.settings.invalid", L"Invalid")})));
            for (const auto& issue : invalid.issues)
            {
                std::wstring text;
                if (!issue.code.empty())
                    text = L"[" + issue.code + L"] ";
                text += issue.message;
                if (!text.empty())
                    detailsBody.Children().Append(MakeSecondaryText(text));
            }
        }
        for (const auto& failure : package.workshopInstallFailures)
        {
            detailsBody.Children().Append(MakeSecondaryText(JoinMetadata({
                L("app.settings.widgets_source_steam", L"Steam Workshop"),
                failure.version,
                L("app.settings.widgets_workshop_install_failed",
                    L"Workshop install failed")})));
            if (!failure.error.empty())
                detailsBody.Children().Append(MakeSecondaryText(failure.error));
            muxc::StackPanel failureActions;
            failureActions.Orientation(muxc::Orientation::Horizontal);
            failureActions.HorizontalAlignment(
                mux::HorizontalAlignment::Right);
            failureActions.Spacing(8.0);
            muxc::Button retry = MakeActionButton(L(
                "app.settings.widgets_retry_install", L"Retry install"));
            HookClick(retry,
                [this, packageId = package.packageId,
                    sourceId = failure.sourceId,
                    externalItemId = failure.externalItemId,
                    version = failure.version](const auto&, const auto&) {
                    WidgetsPageRequest request;
                    request.command =
                        WidgetsPageCommand::RetryWorkshopInstall;
                    request.packageId = packageId;
                    request.sourceId = sourceId;
                    request.externalItemId = externalItemId;
                    request.version = version;
                    Emit(std::move(request));
                }, revokers);
            failureActions.Children().Append(retry);
            if (!failure.externalItemId.empty())
            {
                muxc::Button open = MakeActionButton(L(
                    "app.settings.widgets_open_workshop_item",
                    L"Open Workshop page"));
                HookClick(open,
                    [this, packageId = package.packageId,
                        sourceId = failure.sourceId,
                        externalItemId = failure.externalItemId,
                        version = failure.version](
                        const auto&, const auto&) {
                        WidgetsPageRequest request;
                        request.command =
                            WidgetsPageCommand::OpenWorkshopItem;
                        request.packageId = packageId;
                        request.sourceId = sourceId;
                        request.externalItemId = externalItemId;
                        request.version = version;
                        Emit(std::move(request));
                    }, revokers);
                failureActions.Children().Append(open);
            }
            detailsBody.Children().Append(failureActions);
        }

        muxc::Button enabledAction{nullptr};
        muxc::Button addAction{nullptr};
        if (package.canEnable || package.showAddToDesktop)
        {
            if (package.canEnable)
            {
                enabledAction = MakeActionButton(L(
                    package.enabled
                        ? "app.settings.widgets_disable"
                        : "app.settings.widgets_enable",
                    package.enabled ? L"Disable" : L"Enable"));
                HookClick(enabledAction,
                    [this, packageId = package.packageId](
                        const winrt::Windows::Foundation::IInspectable&,
                        const mux::RoutedEventArgs&) {
                        if (updatingControls || closed || !active)
                            return;
                        const auto* current = FindPackage(packageId);
                        if (!current || !current->canEnable)
                            return;
                        WidgetsPageRequest request;
                        request.command =
                            WidgetsPageCommand::SetPackageEnabled;
                        request.packageId = packageId;
                        request.enabled = !current->enabled;
                        Emit(std::move(request));
                    },
                    revokers);
                primaryActions.Children().Append(enabledAction);
            }
            if (package.showAddToDesktop)
            {
                addAction = MakeActionButton(
                    L("app.widget_preview.add_to_desktop",
                        L"Add to Desktop"));
                addAction.IsEnabled(package.canAddToDesktop);
                HookClick(addAction,
                    [this, packageId = package.packageId](
                        const winrt::Windows::Foundation::IInspectable&,
                        const mux::RoutedEventArgs&) {
                        WidgetsPageRequest request;
                        request.command =
                            WidgetsPageCommand::AddPackageToDesktop;
                        request.packageId = packageId;
                        Emit(std::move(request));
                    },
                    revokers);
                primaryActions.Children().Append(addAction);
            }
        }

        muxc::Button developmentAction{nullptr};
        if (developerOverridesVisible &&
            package.canUseDevelopmentOverride)
        {
            developmentAction = MakeActionButton(L(
                package.developmentOverrideActive
                    ? "app.settings.widgets_development_deactivate"
                    : "app.settings.widgets_development_activate",
                package.developmentOverrideActive
                    ? L"Deactivate Development Version"
                    : L"Activate Development Version"));
            HookClick(developmentAction,
                [this, packageId = package.packageId](
                    const winrt::Windows::Foundation::IInspectable&,
                    const mux::RoutedEventArgs&) {
                    if (updatingControls || closed || !active)
                        return;
                    const auto* current = FindPackage(packageId);
                    if (!current || !current->canUseDevelopmentOverride)
                        return;
                    WidgetsPageRequest request;
                    request.command =
                        WidgetsPageCommand::SetDevelopmentOverride;
                    request.packageId = packageId;
                    request.enabled =
                        !current->developmentOverrideActive;
                    Emit(std::move(request));
                },
                revokers);
            presenter_controls::SettingRow developmentRow;
            developmentRow.Initialize(developmentAction, 300.0);
            developmentRow.SetControlAlignment(
                mux::HorizontalAlignment::Right);
            developmentRow.SetText(L(
                    "app.settings.widgets_development",
                    L"Development version"),
                L("settings.developer.overrides.description",
                    L"Use the local development source."));
            detailsBody.Children().Append(developmentRow.root);
        }

        muxc::TextBlock sourceId{nullptr};
        if (package.valid)
        {
            AddPermissionControls(package, detailsBody, revokers);
            AddInstanceControls(package, detailsBody, revokers);
            muxc::TextBlock sourceIdText{nullptr};
            AddLegacyPackageActions(package, detailsBody, revokers,
                sourceIdText, advancedActions);
            sourceId = sourceIdText;
        }

        if (package.canUninstall && !package.builtIn)
        {
            muxc::Button uninstall = MakeActionButton(
                L("app.settings.widgets_uninstall", L"Uninstall"),
                L("app.settings.widgets_uninstall_confirm",
                    L"Remove this component from SnowDesktop."));
            uninstall.HorizontalAlignment(mux::HorizontalAlignment::Right);
            HookClick(uninstall,
                [this, packageId = package.packageId](
                    const winrt::Windows::Foundation::IInspectable&,
                    const mux::RoutedEventArgs&) {
                    const auto* current = FindPackage(packageId);
                    if (!current)
                        return;
                    RequestUninstall(packageId, PackageDisplayName(*current),
                        !current->workshopExternalItemId.empty());
                },
                revokers);
            detailsBody.Children().Append(uninstall);
        }

        if (detailsBody.Children().Size() != 0)
        {
            muxc::Expander details;
            details.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
            details.HorizontalContentAlignment(
                mux::HorizontalAlignment::Stretch);
            details.IsExpanded(false);
            const std::wstring detailsText = L(
                "app.settings.widgets_technical_details",
                L"Technical Details");
            details.Header(winrt::box_value(detailsText));
            details.Content(detailsBody);
            StretchExpanderBody(details, detailsBody);
            SetAutomation(details, detailsText);
            body.Children().Append(details);
        }

        row.Child(body);
        SetAutomation(row, name.Text(), package.description);
        muxa::AutomationProperties::SetItemStatus(row, packageState);
        if (!firstPackageTarget)
            firstPackageTarget = row;
        if (IsIncludedPackage(package) && !firstIncludedPackageTarget)
            firstIncludedPackageTarget = row;
        targetRows.Children().Append(row);
        packageRowBindings.push_back(PackageRowBinding{
            package.packageId, row, name, metadata, tags, description,
            version, author, sourceId, enabledAction, addAction,
            developmentAction,
            std::move(advancedActions), std::move(revokers)});
    }

    void RequestUninstall(
        const std::wstring& packageId,
        const std::wstring& displayName,
        bool workshop)
    {
        if (closed || !active || !actions.confirm || !actions.invoke)
            return;

        const std::uint64_t requestGeneration = generation;
        const std::uint64_t requestActivation =
            callbackGate->activation.load(std::memory_order_acquire);
        const std::wstring title =
            L("app.settings.widgets_uninstall", L"Uninstall") +
            (displayName.empty() ? std::wstring{} : L" — " + displayName);
        const std::wstring message = L(workshop
                ? "app.settings.widgets_uninstall_workshop_confirm"
                : "app.settings.widgets_uninstall_confirm",
            workshop
                ? L"Unsubscribe and uninstall this component?"
                : L"Uninstall this component? Its desktop instances will stop.");
        const std::wstring primaryButtonText = L(workshop
                ? "app.settings.widgets_unsubscribe_and_uninstall"
                : "app.settings.widgets_uninstall",
            workshop ? L"Unsubscribe and Uninstall" : L"Uninstall");
        const std::shared_ptr<CallbackGenerationGate> gate = callbackGate;
        const auto invoke = actions.invoke;

        actions.confirm(requestGeneration, title, message,
            primaryButtonText,
            [gate, invoke, requestGeneration, requestActivation,
                packageId](bool confirmed) {
                if (!confirmed || !invoke ||
                    gate->closed.load(std::memory_order_acquire) ||
                    !gate->active.load(std::memory_order_acquire) ||
                    gate->generation.load(std::memory_order_acquire) !=
                        requestGeneration ||
                    gate->activation.load(std::memory_order_acquire) !=
                        requestActivation)
                {
                    return;
                }
                WidgetsPageRequest request;
                request.command = WidgetsPageCommand::UninstallPackage;
                request.packageId = packageId;
                invoke(requestGeneration, std::move(request));
            });
    }

    [[nodiscard]] static bool HasOnlyInlinePackageStateChanges(
        const InstalledWidgetPackageSnapshot& previous,
        const InstalledWidgetPackageSnapshot& current)
    {
        InstalledWidgetPackageSnapshot previousStructure = previous;
        InstalledWidgetPackageSnapshot currentStructure = current;
        previousStructure.enabled = currentStructure.enabled = false;
        previousStructure.active = currentStructure.active = false;
        previousStructure.canAddToDesktop =
            currentStructure.canAddToDesktop = false;
        previousStructure.developmentOverrideActive =
            currentStructure.developmentOverrideActive = false;
        previousStructure.name = currentStructure.name = {};
        previousStructure.description = currentStructure.description = {};
        previousStructure.version = currentStructure.version = {};
        previousStructure.author = currentStructure.author = {};
        previousStructure.sourceId = currentStructure.sourceId = {};
        previousStructure.sourceName = currentStructure.sourceName = {};
        previousStructure.sourceExternalItemId =
            currentStructure.sourceExternalItemId = {};
        previousStructure.development = currentStructure.development = false;
        return previousStructure == currentStructure;
    }

    [[nodiscard]] std::vector<std::wstring> VisiblePackageIds(
        const std::vector<InstalledWidgetPackageSnapshot>& values) const
    {
        std::vector<std::wstring> result;
        result.reserve(values.size());
        for (const auto& package : values)
        {
            if (MatchesFilter(package))
                result.push_back(package.packageId);
        }
        return result;
    }

    void PatchPackageRowState(
        const PackageRowBinding& binding,
        const InstalledWidgetPackageSnapshot& package)
    {
        const std::wstring packageState = PackageStateText(package);
        const std::wstring displayName = PackageDisplayName(package);
        binding.name.Text(displayName);
        binding.metadata.Text(JoinMetadata(
            {package.sourceName, packageState}));
        PopulatePackageTags(binding.tags, package);
        binding.description.Text(package.description);
        binding.description.Visibility(package.description.empty()
                ? mux::Visibility::Collapsed
                : mux::Visibility::Visible);
        binding.version.Text(
            L("app.settings.version", L"Version") + L": " +
            package.version);
        binding.version.Visibility(package.version.empty()
                ? mux::Visibility::Collapsed
                : mux::Visibility::Visible);
        binding.author.Text(
            L("app.settings.author", L"Author") + L": " +
            package.author);
        binding.author.Visibility(package.author.empty()
                ? mux::Visibility::Collapsed
                : mux::Visibility::Visible);
        if (binding.sourceId)
        {
            binding.sourceId.Text(
                L("app.settings.widgets_provider_id", L"Source ID") +
                L": " + package.sourceId);
            binding.sourceId.Visibility(package.sourceId.empty()
                    ? mux::Visibility::Collapsed
                    : mux::Visibility::Visible);
        }
        SetAutomation(binding.card, displayName, package.description);
        muxa::AutomationProperties::SetItemStatus(
            binding.card, packageState);
        for (const muxc::Button& action : binding.advancedActions)
            muxa::AutomationProperties::SetHelpText(
                action, package.description);

        if (binding.enabledAction)
        {
            const std::wstring label = L(package.enabled
                    ? "app.settings.widgets_disable"
                    : "app.settings.widgets_enable",
                package.enabled ? L"Disable" : L"Enable");
            binding.enabledAction.Content(winrt::box_value(label));
            SetAutomation(binding.enabledAction, label);
        }
        if (binding.addAction)
            binding.addAction.IsEnabled(package.canAddToDesktop);
        if (binding.developmentAction)
        {
            const std::wstring label = L(
                package.developmentOverrideActive
                    ? "app.settings.widgets_development_deactivate"
                    : "app.settings.widgets_development_activate",
                package.developmentOverrideActive
                    ? L"Deactivate Development Version"
                    : L"Activate Development Version");
            binding.developmentAction.Content(winrt::box_value(label));
            SetAutomation(binding.developmentAction, label);
        }
    }

    [[nodiscard]] bool TryPatchInstalledRows(
        const std::vector<InstalledWidgetPackageSnapshot>& previousPackages,
        bool developerVisibilityChanged)
    {
        if (developerVisibilityChanged)
            return false;

        const std::vector<std::wstring> previousIds =
            VisiblePackageIds(previousPackages);
        const std::vector<std::wstring> currentIds =
            VisiblePackageIds(packages);
        if (previousIds != currentIds ||
            currentIds.size() != packageRowBindings.size())
        {
            return false;
        }
        std::vector<std::size_t> changedRows;
        for (std::size_t index = 0; index < currentIds.size(); ++index)
        {
            if (packageRowBindings[index].packageId != currentIds[index])
                return false;
            const auto previous = std::find_if(previousPackages.begin(),
                previousPackages.end(), [&](const auto& package) {
                    return package.packageId == currentIds[index];
                });
            const auto current = std::find_if(packages.begin(), packages.end(),
                [&](const auto& package) {
                    return package.packageId == currentIds[index];
                });
            if (previous == previousPackages.end() ||
                current == packages.end() ||
                (*previous != *current &&
                    !HasOnlyInlinePackageStateChanges(*previous, *current)))
            {
                return false;
            }
            if (*previous != *current)
                changedRows.push_back(index);
        }

        for (const std::size_t index : changedRows)
        {
            const auto current = std::find_if(packages.begin(), packages.end(),
                [&](const auto& package) {
                    return package.packageId == currentIds[index];
                });
            if (current != packages.end())
                PatchPackageRowState(packageRowBindings[index], *current);
        }
        return true;
    }

    void RenderInstalledRows()
    {
        RevokePackageRowEvents();
        installedRows.Children().Clear();
        packageRowBindings.clear();
        firstPackageTarget = nullptr;
        firstIncludedPackageTarget = nullptr;
        firstPermissionTarget = nullptr;

        bool anyPackage = false;
        for (const InstalledWidgetPackageSnapshot& package : packages)
        {
            if (!MatchesFilter(package))
                continue;
            anyPackage = true;
            AddPackageRow(package, installedRows);
        }
        if (!anyPackage)
        {
            installedRows.Children().Append(MakeSecondaryText(
                L("app.settings.widgets_filter_empty", L"No components")));
        }
    }

    void RenderManagementControls()
    {
        const bool workshopAvailable = std::any_of(
            sources.begin(), sources.end(), [](const auto& source) {
                return source.workshop && source.available;
            }) || std::any_of(packages.begin(), packages.end(),
                [](const auto& package) {
                    return !package.workshopExternalItemId.empty();
                });
        installFileButton.IsEnabled(static_cast<bool>(actions.invoke));
        workshopButton.Visibility(workshopAvailable
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
        workshopButton.IsEnabled(workshopAvailable &&
            static_cast<bool>(actions.invoke));
        selfDevelopButton.Visibility(actions.setDeveloperToolsEnabled
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
        selfDevelopButton.IsEnabled(
            static_cast<bool>(actions.setDeveloperToolsEnabled));
        selfDevelopButton.IsChecked(developerOverridesVisible);
    }

    void RenderTask()
    {
        const bool running = task.kind != WidgetsPageTaskKind::None;
        taskPanel.Visibility(running
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
        taskRing.IsActive(running);
        taskRing.Visibility(running
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);

        const bool determinate = running && task.progress.has_value();
        taskProgress.Visibility(determinate
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
        if (determinate)
        {
            taskProgress.Value(std::clamp(*task.progress, 0.0, 1.0) * 100.0);
        }
        const std::wstring status = task.status.empty()
            ? TaskText(task.kind)
            : task.status;
        taskStatus.Text(status);
        cancelTaskButton.Visibility(running && task.cancellable
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
        cancelTaskButton.IsEnabled(running && task.cancellable);
        SetAutomation(taskRing, status);
        SetAutomation(cancelTaskButton,
            L("settings.progress.cancel", L"Cancel"), status);
    }

    void RenderFeedback()
    {
        const bool visible = feedback.severity !=
            WidgetsPageFeedbackSeverity::None;
        std::wstring title = feedback.title;
        if (!feedback.titleKey.empty())
            title = L(feedback.titleKey, title);
        for (const muxc::InfoBar& bar : {
                 feedbackBar, developerFeedbackBar, debugFeedbackBar})
        {
            bar.Severity(ToInfoBarSeverity(feedback.severity));
            bar.Title(title);
            bar.Message(feedback.message);
            bar.IsOpen(visible);
            SetAutomation(bar, title, feedback.message);
        }
    }

    bool ApplySnapshot(const WidgetsPageSnapshot& snapshot)
    {
        if (closed || !active || snapshot.generation != generation)
            return false;
        if (hasRevision && snapshot.revision <= revision)
            return false;

        const std::vector<InstalledWidgetPackageSnapshot> previousPackages =
            packages;
        const bool previousDeveloperOverridesVisible =
            developerOverridesVisible;
        const std::wstring previousQuery = query;
        const PackageFilter previousFilter = filter;
        const bool firstPublication = !hasRevision;
        const bool installedPresentationChanged = firstPublication ||
            packages != snapshot.installed || developerOverridesVisible !=
                snapshot.developerOverridesVisible;
        const bool developerPresentationChanged =
            installedPresentationChanged ||
            agentSkills != snapshot.agentSkills ||
            agentSkillTargetMask != snapshot.agentSkillTargetMask ||
            agentSkillStatusError != snapshot.agentSkillStatusError ||
            developerActionStatus != snapshot.developerActionStatus ||
            developmentWorkspace != snapshot.developmentWorkspace ||
            componentCliPath != snapshot.componentCliPath ||
            developerPublisherAvailable !=
                snapshot.developerPublisherAvailable ||
            errors != snapshot.errors || diagnostics != snapshot.diagnostics;
        const bool debugPresentationChanged =
            firstPublication || diagnostics != snapshot.diagnostics;
        updatingControls = true;
        revision = snapshot.revision;
        hasRevision = true;
        packages = snapshot.installed;
        agentSkills = snapshot.agentSkills;
        agentSkillTargetMask = snapshot.agentSkillTargetMask;
        agentSkillStatusError = snapshot.agentSkillStatusError;
        developerActionStatus = snapshot.developerActionStatus;
        developmentWorkspace = snapshot.developmentWorkspace;
        componentCliPath = snapshot.componentCliPath;
        developerPublisherAvailable =
            snapshot.developerPublisherAvailable;
        errors = snapshot.errors;
        diagnostics = snapshot.diagnostics;
        task = snapshot.task;
        feedback = snapshot.feedback;
        developerOverridesVisible = snapshot.developerOverridesVisible;
        const bool acceptSearch =
            snapshot.searchRevision >= searchRevision &&
            snapshot.searchRevision >= requestedSearchRevision;
        const bool sourcePresentationChanged = acceptSearch &&
            (firstPublication || sources != snapshot.sources ||
                query != snapshot.searchQuery);
        if (acceptSearch)
        {
            searchRevision = snapshot.searchRevision;
            requestedSearchRevision = std::max(
                requestedSearchRevision, snapshot.searchRevision);
            sources = snapshot.sources;
            query = snapshot.searchQuery;
            searchBox.Text(query);
        }
        const bool installedQueryChanged = previousQuery != query;
        if (installedPresentationChanged || installedQueryChanged)
        {
            if (installedPresentationChanged)
                RefreshFilterItems();
            const bool patched = !firstPublication &&
                !installedQueryChanged && previousFilter == filter &&
                TryPatchInstalledRows(previousPackages,
                    previousDeveloperOverridesVisible !=
                        developerOverridesVisible);
            if (!patched)
                RenderInstalledRows();
        }
        if (developerPresentationChanged)
            RenderDeveloperRows();
        if (debugPresentationChanged)
            RenderDebugRows();
        if (installedPresentationChanged || sourcePresentationChanged)
            RenderManagementControls();
        RenderTask();
        RenderFeedback();
        updatingControls = false;
        return true;
    }

    void RefreshLocalizedText()
    {
        const bool previousUpdating = updatingControls;
        updatingControls = true;

        searchCard.title.Text(L("app.settings.widgets_management",
            L"Add Components"));
        searchCard.title.Visibility(mux::Visibility::Collapsed);
        searchCard.description.Text(L"");
        searchCard.description.Visibility(mux::Visibility::Collapsed);
        managementRow.SetText(
            std::wstring(searchCard.title.Text().c_str()));
        filterCard.title.Text(L("app.settings.widgets_my_components",
            L"My Components"));
        filterCard.description.Text(L"");
        filterCard.description.Visibility(mux::Visibility::Collapsed);
        developerOverridesCard.title.Text(L(
            "app.settings.widgets_agent_skill", L"Agent Skill"));
        developerOverridesCard.description.Text(L(
            "app.settings.widgets_agent_skill_description",
            L"Choose which assistants should receive the Skill."));
        developerWorkspaceCard.title.Text(L(
            "app.settings.widgets_authoring_workspace",
            L"Development Workspace"));
        developerWorkspaceCard.description.Text(L"");
        developerCliCard.title.Text(L(
            "app.settings.widgets_component_cli", L"Component CLI"));
        developerCliCard.description.Text(L"");
        developerPublishCard.title.Text(L(
            "app.settings.widgets_authoring_publish", L"Publish"));
        developerPublishCard.description.Text(L(
            "app.settings.widgets_authoring_publish_description",
            L"Open the Steam-only manager to publish a component."));
        developerReferenceCard.title.Text(L(
            "app.settings.widgets_authoring_reference",
            L"Development Reference"));
        developerReferenceCard.description.Text(L(
            "app.settings.widgets_authoring_reference_description",
            L"Icon resources and runtime diagnostics for component "
                L"development."));
        iconReferenceTitle.Text(L(
            "app.settings.widgets_icon_reference", L"Icon Resources"));
        developerToolsCard.title.Text(L(
            "app.settings.widgets_runtime_diagnostics",
            L"Runtime Diagnostics"));
        developerToolsCard.description.Text(L"");
        debugRuntimeCard.title.Text(L(
            "settings.debug.runtime", L"Runtime diagnostics"));
        debugRuntimeCard.description.Text(L(
            "settings.debug.runtime.description",
            L"Inspect diagnostic state and traces."));

        searchBox.PlaceholderText(L("app.settings.widgets_search_hint",
            L"Search components"));
        RefreshFilterItems();
        installFileButton.Content(winrt::box_value(L(
            "app.settings.widgets_install_package",
            L"Install from File...")));
        workshopButton.Content(winrt::box_value(L(
            "app.settings.widgets_open_steam_workshop",
            L"Open Workshop")));
        selfDevelopButton.Content(winrt::box_value(L(
            "app.settings.widgets_self_develop", L"Develop Your Own")));
        cancelTaskButton.Content(winrt::box_value(L(
            "settings.progress.cancel", L"Cancel")));
        developerRefreshButton.Content(winrt::box_value(L(
            "app.menu.refresh", L"Refresh")));
        agentSkillApplyButton.Content(winrt::box_value(L(
            "app.settings.widgets_skill_sync_all", L"Apply Selection")));
        agentSkillRefreshButton.Content(winrt::box_value(L(
            "app.settings.widgets_skill_check_updates",
            L"Check for Updates")));
        openDevelopmentFolderButton.Content(winrt::box_value(L(
            "app.settings.widgets_open_development_folder",
            L"Open Development Components Folder")));
        publishWorkspaceButton.Content(winrt::box_value(L(
            "app.settings.widgets_publish_steam", L"Publish to Steam")));
        copyAllErrorsButton.Content(winrt::box_value(L(
            "app.settings.copy_all", L"Copy All")));
        clearAllErrorsButton.Content(winrt::box_value(L(
            "app.settings.clear_all", L"Clear All")));
        copyDiagnosticsButton.Content(winrt::box_value(L(
            "app.settings.copy_diag", L"Copy Diagnostics")));
        debugRefreshButton.Content(winrt::box_value(L(
            "app.menu.refresh", L"Refresh")));

        const auto updateIconHeader = [this](const muxc::Expander& expander,
                                               std::string_view titleKey,
                                               std::wstring_view titleFallback,
                                               std::string_view hintKey,
                                               std::wstring_view hintFallback) {
            const auto header = expander.Header().try_as<muxc::StackPanel>();
            if (!header || header.Children().Size() < 2) return;
            if (const auto title = header.Children().GetAt(0).
                    try_as<muxc::TextBlock>())
                title.Text(L(titleKey, titleFallback));
            if (const auto hint = header.Children().GetAt(1).
                    try_as<muxc::TextBlock>())
                hint.Text(L(hintKey, hintFallback));
        };
        updateIconHeader(fluentIconsExpander,
            "app.settings.fluent_icons",
            L"Fluent System Icons Regular Characters",
            "app.settings.fluent_icon_hint",
            L"Click an icon to copy it. Set iconFont = \"fluent\" on the "
                L"Lua menu item that uses it.");
        updateIconHeader(fontAwesomeIconsExpander,
            "app.settings.fa_icons", L"Font Awesome Icon Characters",
            "app.settings.fa_icon_hint",
            L"Click an icon to copy its character, then paste it into a Lua "
                L"menu item's icon field.");

        SetAutomation(searchCard.root, managementRow.label.Text());
        SetAutomation(filterCard.root, filterCard.title.Text());
        SetAutomation(searchBox,
            L("app.settings.widgets_search", L"Search"),
            searchBox.PlaceholderText());
        SetAutomation(filterActions,
            L("app.settings.widgets_my_components", L"My Components"));
        SetAutomation(installFileButton,
            L("app.settings.widgets_install_package",
                L"Install from File..."));
        SetAutomation(workshopButton,
            L("app.settings.widgets_open_steam_workshop",
                L"Open Workshop"),
            L("app.settings.widgets_workshop_auto_sync_hint",
                 L"Manage Workshop subscriptions."));
        SetAutomation(selfDevelopButton,
            L("app.settings.widgets_self_develop", L"Develop Your Own"),
            L("app.settings.widgets_developer_tools",
                L"Component Developer Tools"));
        SetAutomation(developerOverridesCard.root,
            developerOverridesCard.title.Text(),
            developerOverridesCard.description.Text());
        SetAutomation(developerToolsCard.root,
            developerToolsCard.title.Text(),
            developerToolsCard.description.Text());
        SetAutomation(developerWorkspaceCard.root,
            developerWorkspaceCard.title.Text(),
            developerWorkspaceCard.description.Text());
        SetAutomation(developerCliCard.root,
            developerCliCard.title.Text(),
            developerCliCard.description.Text());
        SetAutomation(developerPublishCard.root,
            developerPublishCard.title.Text(),
            developerPublishCard.description.Text());
        SetAutomation(developerReferenceCard.root,
            developerReferenceCard.title.Text(),
            developerReferenceCard.description.Text());
        SetAutomation(agentSkillRefreshButton,
            L("app.settings.widgets_skill_check_updates",
                L"Check for Updates"),
            developerOverridesCard.description.Text());
        SetAutomation(openDevelopmentFolderButton,
            L("app.settings.widgets_open_development_folder",
                L"Open Development Components Folder"),
            developmentWorkspace);
        SetAutomation(publishWorkspaceButton,
            L("app.settings.widgets_publish_steam", L"Publish to Steam"),
            developerPublishCard.description.Text());
        SetAutomation(copyAllErrorsButton,
            L("app.settings.copy_all", L"Copy All"));
        SetAutomation(clearAllErrorsButton,
            L("app.settings.clear_all", L"Clear All"));
        SetAutomation(copyDiagnosticsButton,
            L("app.settings.copy_diag", L"Copy Diagnostics"));
        SetAutomation(debugRuntimeCard.root,
            debugRuntimeCard.title.Text(),
            debugRuntimeCard.description.Text());
        SetAutomation(developerRefreshButton,
            L("app.menu.refresh", L"Refresh"),
            developerToolsCard.description.Text());
        SetAutomation(debugRefreshButton,
            L("app.menu.refresh", L"Refresh"),
            debugRuntimeCard.description.Text());

        RenderInstalledRows();
        RenderDeveloperRows();
        RenderDebugRows();
        RenderTask();
        RenderFeedback();
        RenderManagementControls();
        updatingControls = previousUpdating;
    }

    mux::FrameworkElement FocusTarget(std::string_view focusId) const noexcept
    {
        if (focusId == "widgets.search")
            return searchBox;
        if (focusId == "widgets.install")
            return installFileButton;
        if (focusId == "widgets.workshop")
            return workshopButton;
        if (focusId == "widgets.developer")
            return selfDevelopButton;
        if (focusId == "widgets.sources")
            return workshopButton;
        if (focusId == "widgets.permissions")
            return firstPermissionTarget
                ? firstPermissionTarget
                : (firstPackageTarget ? firstPackageTarget : installedRows);
        if (focusId == "widgets.installed")
            return firstPackageTarget ? firstPackageTarget : installedRows;
        if (focusId == "widgets.included")
            return firstIncludedPackageTarget
                ? firstIncludedPackageTarget : installedRows;
        return nullptr;
    }

    mux::FrameworkElement DeveloperToolsFocusTarget(
        std::string_view focusId) const noexcept
    {
        if (focusId == "developer.overrides")
        {
            return firstDeveloperOverrideTarget
                ? firstDeveloperOverrideTarget
                : developerOverridesCard.root;
        }
        if (focusId == "developer.agentSkill")
            return firstDeveloperOverrideTarget
                ? firstDeveloperOverrideTarget
                : agentSkillApplyButton;
        if (focusId == "developer.workspace")
            return openDevelopmentFolderButton;
        if (focusId == "developer.cli")
            return developerCliCard.root;
        if (focusId == "developer.publish")
            return publishWorkspaceButton;
        if (focusId == "developer.reference")
            return fluentIconsExpander;
        if (focusId == "developer.runtime")
            return firstDeveloperDiagnosticTarget
                ? firstDeveloperDiagnosticTarget
                : copyDiagnosticsButton;
        if (focusId == "developer.tools")
        {
            return firstDeveloperDiagnosticTarget
                ? firstDeveloperDiagnosticTarget
                : copyDiagnosticsButton;
        }
        return nullptr;
    }

    mux::FrameworkElement DebugFocusTarget(
        std::string_view focusId) const noexcept
    {
        if (focusId == "debug.runtime")
        {
            return firstDebugDiagnosticTarget
                ? firstDebugDiagnosticTarget
                : debugRefreshButton;
        }
        return nullptr;
    }

    void Activate(std::uint64_t nextGeneration) noexcept
    {
        if (closed)
            return;
        active = true;
        if (generation != nextGeneration)
        {
            hasRevision = false;
            revision = 0;
            searchRevision = 0;
            requestedSearchRevision = 0;
        }
        generation = nextGeneration;
        callbackGate->generation.store(
            nextGeneration, std::memory_order_release);
        callbackGate->activation.fetch_add(1, std::memory_order_acq_rel);
        callbackGate->active.store(true, std::memory_order_release);
    }

    void Deactivate() noexcept
    {
        active = false;
        callbackGate->active.store(false, std::memory_order_release);
        callbackGate->activation.fetch_add(1, std::memory_order_acq_rel);
        diagnostics.clear();
        errors.clear();
        try
        {
            RenderDeveloperRows();
            RenderDebugRows();
        }
        catch (...)
        {
        }
    }

    void Close() noexcept
    {
        if (closed)
            return;
        closed = true;
        active = false;
        callbackGate->active.store(false, std::memory_order_release);
        callbackGate->closed.store(true, std::memory_order_release);

        RevokePackageRowEvents();
        Revoke(developerRevokers);
        Revoke(debugRevokers);
        try
        {
            searchBox.TextChanged(searchChangedToken);
            allFilterButton.Click(allFilterToken);
            installedFilterButton.Click(installedFilterToken);
            includedFilterButton.Click(includedFilterToken);
            developmentFilterButton.Click(developmentFilterToken);
            installFileButton.Click(installFileToken);
            workshopButton.Click(workshopToken);
            selfDevelopButton.Click(selfDevelopToken);
            cancelTaskButton.Click(cancelTaskToken);
            developerRefreshButton.Click(developerRefreshToken);
            agentSkillApplyButton.Click(agentSkillApplyToken);
            agentSkillRefreshButton.Click(agentSkillRefreshToken);
            openDevelopmentFolderButton.Click(openDevelopmentFolderToken);
            publishWorkspaceButton.Click(publishWorkspaceToken);
            copyAllErrorsButton.Click(copyAllErrorsToken);
            clearAllErrorsButton.Click(clearAllErrorsToken);
            copyDiagnosticsButton.Click(copyDiagnosticsToken);
            fluentIconsGrid.ItemClick(fluentIconClickToken);
            fontAwesomeIconsGrid.ItemClick(fontAwesomeIconClickToken);
            fluentIconsGrid.ContainerContentChanging(
                fluentIconContainerToken);
            fontAwesomeIconsGrid.ContainerContentChanging(
                fontAwesomeIconContainerToken);
            debugRefreshButton.Click(debugRefreshToken);
        }
        catch (...)
        {
        }
        actions = {};
        packages.clear();
        agentSkills.clear();
        errors.clear();
        diagnostics.clear();
        sources.clear();
    }
};

WidgetsPagePresenter::WidgetsPagePresenter(
    LocalizeCallback localize,
    const mux::Style& cardStyle,
    const mux::DataTemplate& fluentGlyphTemplate,
    const mux::DataTemplate& fontAwesomeGlyphTemplate)
    : impl_(std::make_unique<Impl>(std::move(localize), cardStyle,
          fluentGlyphTemplate, fontAwesomeGlyphTemplate))
{
}

WidgetsPagePresenter::~WidgetsPagePresenter()
{
    Close();
}

void WidgetsPagePresenter::SetActions(WidgetsPageActions actions)
{
    if (impl_ && !impl_->closed)
    {
        impl_->actions = std::move(actions);
        try
        {
            impl_->RenderManagementControls();
            impl_->RenderInstalledRows();
            impl_->RenderDeveloperRows();
        }
        catch (...)
        {
        }
    }
}

mux::UIElement WidgetsPagePresenter::Content() const noexcept
{
    return impl_ ? impl_->root : nullptr;
}

mux::UIElement WidgetsPagePresenter::DeveloperToolsContent() const noexcept
{
    return impl_ ? impl_->developerRoot : nullptr;
}

mux::UIElement WidgetsPagePresenter::DebugContent() const noexcept
{
    return impl_ ? impl_->debugRoot : nullptr;
}

void WidgetsPagePresenter::Activate(std::uint64_t generation) noexcept
{
    if (impl_)
        impl_->Activate(generation);
}

void WidgetsPagePresenter::Deactivate() noexcept
{
    if (impl_)
        impl_->Deactivate();
}

bool WidgetsPagePresenter::ApplySnapshot(
    const WidgetsPageSnapshot& snapshot)
{
    return impl_ && impl_->ApplySnapshot(snapshot);
}

void WidgetsPagePresenter::RefreshLocalizedText()
{
    if (impl_ && !impl_->closed)
        impl_->RefreshLocalizedText();
}

mux::FrameworkElement WidgetsPagePresenter::FocusTarget(
    std::string_view focusId) const noexcept
{
    return impl_ ? impl_->FocusTarget(focusId) : nullptr;
}

mux::FrameworkElement WidgetsPagePresenter::DeveloperToolsFocusTarget(
    std::string_view focusId) const noexcept
{
    return impl_ ? impl_->DeveloperToolsFocusTarget(focusId) : nullptr;
}

mux::FrameworkElement WidgetsPagePresenter::DebugFocusTarget(
    std::string_view focusId) const noexcept
{
    return impl_ ? impl_->DebugFocusTarget(focusId) : nullptr;
}

void WidgetsPagePresenter::Close() noexcept
{
    if (impl_)
        impl_->Close();
}

} // namespace snowdesktop::winui

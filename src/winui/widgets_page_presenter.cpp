#include "pch.h"

#include "settings_presenter_controls.h"
#include "widgets_page_presenter.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
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
namespace wad = winrt::Windows::ApplicationModel::DataTransfer;

namespace
{

enum class PackageFilter : std::uint8_t
{
    All,
    Installed,
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
    explicit Impl(LocalizeCallback callback, const mux::Style& style)
        : localize(std::move(callback)), cardStyle(style),
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
    muxc::StackPanel root{nullptr};
    muxc::InfoBar feedbackBar{nullptr};
    muxc::StackPanel developerRoot{nullptr};
    muxc::InfoBar developerFeedbackBar{nullptr};
    muxc::StackPanel debugRoot{nullptr};
    muxc::InfoBar debugFeedbackBar{nullptr};

    SettingsCard searchCard;
    SettingsCard installedCard;
    SettingsCard sourcesCard;
    SettingsCard developerOverridesCard;
    SettingsCard developerWorkspaceCard;
    SettingsCard developerCliCard;
    SettingsCard developerPublishCard;
    SettingsCard developerReferenceCard;
    SettingsCard developerToolsCard;
    SettingsCard debugRuntimeCard;

    muxc::AutoSuggestBox searchBox{nullptr};
    presenter_controls::SettingRow managementRow;
    muxc::StackPanel filterActions{nullptr};
    muxcp::ToggleButton allFilterButton{nullptr};
    muxcp::ToggleButton installedFilterButton{nullptr};
    muxcp::ToggleButton developmentFilterButton{nullptr};
    muxc::StackPanel managementActions{nullptr};
    muxc::Button installFileButton{nullptr};
    muxc::Button workshopButton{nullptr};
    muxcp::ToggleButton selfDevelopButton{nullptr};
    muxc::TextBlock workshopHint{nullptr};
    muxc::StackPanel taskPanel{nullptr};
    muxc::ProgressRing taskRing{nullptr};
    muxc::ProgressBar taskProgress{nullptr};
    muxc::TextBlock taskStatus{nullptr};
    muxc::Button cancelTaskButton{nullptr};
    muxc::StackPanel installedRows{nullptr};
    muxc::Expander includedExpander{nullptr};
    muxc::TextBlock includedTitle{nullptr};
    muxc::TextBlock includedDescription{nullptr};
    muxc::StackPanel includedRows{nullptr};
    muxc::StackPanel sourceRows{nullptr};
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
    muxc::ScrollViewer developerDiagnosticScroller{nullptr};
    muxc::StackPanel developerDiagnosticRows{nullptr};
    muxc::Button debugRefreshButton{nullptr};
    muxc::StackPanel debugDiagnosticRows{nullptr};

    mux::FrameworkElement firstPackageTarget{nullptr};
    mux::FrameworkElement firstPermissionTarget{nullptr};
    mux::FrameworkElement firstSourceTarget{nullptr};
    mux::FrameworkElement firstDeveloperOverrideTarget{nullptr};
    mux::FrameworkElement firstDeveloperDiagnosticTarget{nullptr};
    mux::FrameworkElement firstDebugDiagnosticTarget{nullptr};

    std::vector<InstalledWidgetPackageSnapshot> packages;
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
    std::vector<std::function<void()>> installedRevokers;
    std::vector<std::function<void()>> sourceRevokers;
    std::vector<std::function<void()>> developerRevokers;
    std::vector<std::function<void()>> debugRevokers;

    winrt::event_token searchChangedToken{};
    winrt::event_token allFilterToken{};
    winrt::event_token installedFilterToken{};
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

    mux::DataTemplate GlyphTemplate(std::wstring_view family) const
    {
        const std::wstring markup =
            LR"(<DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"><Border Width="38" Height="38"><TextBlock Text="{Binding}" HorizontalAlignment="Center" VerticalAlignment="Center" FontSize="18" FontFamily=")" +
            std::wstring(family) + LR"(" /></Border></DataTemplate>)";
        return winrt::Microsoft::UI::Xaml::Markup::XamlReader::Load(
            winrt::hstring(markup)).as<mux::DataTemplate>();
    }

    void InitializeIconReference(
        muxc::Expander& expander,
        muxc::TextBlock& count,
        muxc::GridView& grid,
        std::string_view titleKey,
        std::wstring_view titleFallback,
        std::string_view hintKey,
        std::wstring_view hintFallback,
        std::wstring_view family,
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
        try
        {
            grid.ItemTemplate(GlyphTemplate(family));
        }
        catch (...)
        {
        }
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
        managementRow.Initialize(managementActions);
        managementRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        searchCard.content.Children().Append(managementRow.root);

        workshopHint = MakeSecondaryText(L"");
        workshopHint.Visibility(mux::Visibility::Collapsed);
        searchCard.content.Children().Append(workshopHint);

        searchBox = muxc::AutoSuggestBox{};
        searchBox.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        searchBox.MaxWidth(720.0);
        searchBox.IsSuggestionListOpen(false);
        searchBox.UseSystemFocusVisuals(true);
        searchCard.content.Children().Append(searchBox);

        filterActions = muxc::StackPanel{};
        filterActions.Orientation(muxc::Orientation::Horizontal);
        filterActions.Spacing(8.0);
        filterActions.HorizontalAlignment(mux::HorizontalAlignment::Right);
        allFilterButton = muxcp::ToggleButton{};
        installedFilterButton = muxcp::ToggleButton{};
        developmentFilterButton = muxcp::ToggleButton{};
        for (const muxcp::ToggleButton& button : {allFilterButton,
                 installedFilterButton, developmentFilterButton})
        {
            button.UseSystemFocusVisuals(true);
            button.HorizontalAlignment(mux::HorizontalAlignment::Right);
            filterActions.Children().Append(button);
        }
        searchCard.content.Children().Append(filterActions);

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
        searchCard.content.Children().Append(taskPanel);

        InitializeCard(installedCard, cardStyle, root);
        installedRows = muxc::StackPanel{};
        installedRows.Spacing(8.0);
        installedRows.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        installedCard.content.Children().Append(installedRows);

        includedExpander = muxc::Expander{};
        includedExpander.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        includedExpander.HorizontalContentAlignment(
            mux::HorizontalAlignment::Stretch);
        includedExpander.IsExpanded(false);
        muxc::StackPanel includedHeader;
        includedHeader.Spacing(3.0);
        includedTitle = muxc::TextBlock{};
        includedTitle.FontWeight(
            winrt::Windows::UI::Text::FontWeights::SemiBold());
        includedTitle.TextWrapping(mux::TextWrapping::Wrap);
        includedDescription = muxc::TextBlock{};
        includedDescription.Opacity(0.72);
        includedDescription.TextWrapping(mux::TextWrapping::Wrap);
        includedHeader.Children().Append(includedTitle);
        includedHeader.Children().Append(includedDescription);
        includedExpander.Header(includedHeader);
        includedRows = muxc::StackPanel{};
        includedRows.Spacing(8.0);
        includedRows.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        includedExpander.Content(includedRows);
        StretchExpanderBody(includedExpander, includedRows);
        root.Children().Append(includedExpander);

        InitializeCard(sourcesCard, cardStyle, root);
        sourceRows = muxc::StackPanel{};
        sourceRows.Spacing(8.0);
        sourceRows.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        sourcesCard.content.Children().Append(sourceRows);

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
            L"FluentSystemIcons-Regular", FluentGlyphItems());
        developerReferenceCard.content.Children().Append(
            fluentIconsExpander);
        InitializeIconReference(fontAwesomeIconsExpander,
            fontAwesomeIconsCount, fontAwesomeIconsGrid,
            "app.settings.fa_icons", L"Font Awesome Icon Characters",
            "app.settings.fa_icon_hint",
            L"Click an icon to copy its character, then paste it into a Lua "
                L"menu item's icon field.",
            L"Font Awesome 6 Free Solid", FontAwesomeGlyphItems());
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
        developerDiagnosticScroller = muxc::ScrollViewer{};
        developerDiagnosticScroller.MaxHeight(180.0);
        developerDiagnosticScroller.HorizontalScrollBarVisibility(
            muxc::ScrollBarVisibility::Auto);
        developerDiagnosticScroller.VerticalScrollBarVisibility(
            muxc::ScrollBarVisibility::Auto);
        developerDiagnosticScroller.Content(developerDiagnosticRows);
        developerToolsCard.content.Children().Append(
            developerDiagnosticScroller);

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
                RenderSourceRows();
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
            if (!package.canEnable && !package.canUninstall &&
                package.workshopInstallFailures.empty())
                return false;
            break;
        case PackageFilter::Development:
            if (!package.canUseDevelopmentOverride && !package.development &&
                std::none_of(package.invalidSources.begin(),
                    package.invalidSources.end(), [](const auto& source) {
                        return source.development;
                    }))
                return false;
            break;
        case PackageFilter::All:
        default:
            break;
        }
        return MatchesQuery(package);
    }

    [[nodiscard]] static bool IsIncludedOnlyPackage(
        const InstalledWidgetPackageSnapshot& package) noexcept
    {
        return package.builtIn && !package.canEnable &&
            !package.canUseDevelopmentOverride;
    }

    [[nodiscard]] std::size_t UserPackageCount() const noexcept
    {
        return static_cast<std::size_t>(std::count_if(
            packages.begin(), packages.end(), [](const auto& package) {
                return !IsIncludedOnlyPackage(package);
            }));
    }

    [[nodiscard]] std::size_t InstalledPackageCount() const noexcept
    {
        return static_cast<std::size_t>(std::count_if(
            packages.begin(), packages.end(), [](const auto& package) {
                return !IsIncludedOnlyPackage(package) &&
                    (package.canEnable || package.canUninstall ||
                        !package.workshopInstallFailures.empty());
            }));
    }

    [[nodiscard]] std::size_t DevelopmentPackageCount() const noexcept
    {
        return static_cast<std::size_t>(std::count_if(
            packages.begin(), packages.end(), [](const auto& package) {
                return !IsIncludedOnlyPackage(package) &&
                    (package.canUseDevelopmentOverride ||
                        package.development ||
                        std::any_of(package.invalidSources.begin(),
                            package.invalidSources.end(),
                            [](const auto& source) {
                                return source.development;
                            }));
            }));
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
        const std::size_t developmentCount = DevelopmentPackageCount();
        if ((filter == PackageFilter::Installed && installedCount == 0) ||
            (filter == PackageFilter::Development &&
                developmentCount == 0))
        {
            filter = PackageFilter::All;
        }
        allFilterButton.Content(winrt::box_value(FilterText(
            "app.settings.widgets_filter_all", L"All", UserPackageCount())));
        installedFilterButton.Content(winrt::box_value(FilterText(
            "app.settings.widgets_filter_installed", L"Installed",
            installedCount)));
        developmentFilterButton.Content(winrt::box_value(FilterText(
            "app.settings.widgets_filter_development", L"Local development",
            developmentCount)));
        installedFilterButton.Visibility(installedCount != 0
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
        developmentFilterButton.Visibility(developmentCount != 0
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
        allFilterButton.IsChecked(filter == PackageFilter::All);
        installedFilterButton.IsChecked(filter == PackageFilter::Installed);
        developmentFilterButton.IsChecked(
            filter == PackageFilter::Development);
        SetAutomation(allFilterButton,
            FilterText("app.settings.widgets_filter_all", L"All",
                UserPackageCount()));
        SetAutomation(installedFilterButton,
            FilterText("app.settings.widgets_filter_installed", L"Installed",
                installedCount));
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
        row.IsExpanded(true);
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
        muxc::Expander row;
        row.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        row.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
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
        row.Header(header);

        muxc::StackPanel body;
        body.Spacing(7.0);
        body.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
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
            if (!firstTarget)
                firstTarget = reload;
        }

        row.Content(body);
        StretchExpanderBody(row, body);
        SetAutomation(row, name.Text(), diagnostic.lastError);
        if (!firstTarget)
            firstTarget = row;
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
        const muxc::StackPanel& body)
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
                [this, package](
                    const winrt::Windows::Foundation::IInspectable&,
                    const mux::RoutedEventArgs&) {
                    OpenPermissionEditor(package);
                },
                installedRevokers);
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
        const muxc::StackPanel& body)
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
                    installedRevokers);
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
        const muxc::StackPanel& body)
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
        appendTechnicalValue("app.settings.widgets_provider_id",
            L"Source ID", package.sourceId);
        appendTechnicalValue("app.settings.widgets_permission_state",
            L"Permission status", PermissionStateText(
                package.permissionState));

        const auto makeCommand = [this, &package](
            WidgetsPageCommand command,
            std::string_view labelKey,
            std::wstring_view fallback,
            std::wstring version = {}) {
            muxc::Button button = MakeActionButton(
                L(labelKey, fallback), package.description);
            HookClick(button,
                [this, command, packageId = package.packageId,
                    sourceId = package.sourceId,
                    externalItemId = package.workshopExternalItemId,
                    version = std::move(version)](
                    const winrt::Windows::Foundation::IInspectable&,
                    const mux::RoutedEventArgs&) {
                    WidgetsPageRequest request;
                    request.command = command;
                    request.packageId = packageId;
                    request.sourceId = sourceId;
                    request.externalItemId = externalItemId;
                    request.version = version;
                    Emit(std::move(request));
                },
                installedRevokers);
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

    void AddPackageRow(
        const InstalledWidgetPackageSnapshot& package,
        const muxc::StackPanel& targetRows)
    {
        muxc::Expander row;
        row.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        row.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        row.IsExpanded(true);

        muxc::StackPanel header;
        header.Spacing(3.0);
        header.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        muxc::TextBlock name;
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
        name.Text(displayName);
        name.FontWeight(
            winrt::Windows::UI::Text::FontWeights::SemiBold());
        name.TextWrapping(mux::TextWrapping::Wrap);
        muxc::TextBlock metadata;
        const std::wstring packageState = PackageStateText(package);
        metadata.Text(JoinMetadata(
            {package.sourceName, packageState}));
        metadata.Opacity(0.72);
        metadata.TextWrapping(mux::TextWrapping::Wrap);
        header.Children().Append(name);
        header.Children().Append(metadata);
        row.Header(header);

        muxc::StackPanel body;
        body.Spacing(9.0);
        body.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        if (!package.description.empty())
            body.Children().Append(MakeSecondaryText(package.description));
        if (!package.version.empty())
        {
            body.Children().Append(MakeSecondaryText(
                L("app.settings.version", L"Version") + L": " +
                package.version));
        }
        if (!package.author.empty())
        {
            body.Children().Append(MakeSecondaryText(
                L("app.settings.author", L"Author") + L": " +
                package.author));
        }

        for (const auto& invalid : package.invalidSources)
        {
            body.Children().Append(MakeSecondaryText(JoinMetadata({
                invalid.sourceName, invalid.version,
                L("app.settings.invalid", L"Invalid")})));
            for (const auto& issue : invalid.issues)
            {
                std::wstring text;
                if (!issue.code.empty())
                    text = L"[" + issue.code + L"] ";
                text += issue.message;
                if (!text.empty())
                    body.Children().Append(MakeSecondaryText(text));
            }
        }
        for (const auto& failure : package.workshopInstallFailures)
        {
            body.Children().Append(MakeSecondaryText(JoinMetadata({
                L("app.settings.widgets_source_steam", L"Steam Workshop"),
                failure.version,
                L("app.settings.widgets_workshop_install_failed",
                    L"Workshop install failed")})));
            if (!failure.error.empty())
                body.Children().Append(MakeSecondaryText(failure.error));
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
                }, installedRevokers);
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
                    }, installedRevokers);
                failureActions.Children().Append(open);
            }
            body.Children().Append(failureActions);
        }

        if (package.canEnable || package.showAddToDesktop)
        {
            muxc::StackPanel primaryActions;
            primaryActions.Orientation(muxc::Orientation::Horizontal);
            primaryActions.HorizontalAlignment(
                mux::HorizontalAlignment::Right);
            primaryActions.Spacing(8.0);
            if (package.canEnable)
            {
                muxc::Button enabledAction = MakeActionButton(L(
                    package.enabled
                        ? "app.settings.widgets_disable"
                        : "app.settings.widgets_enable",
                    package.enabled ? L"Disable" : L"Enable"));
                HookClick(enabledAction,
                    [this, packageId = package.packageId,
                        enable = !package.enabled](
                        const winrt::Windows::Foundation::IInspectable&,
                        const mux::RoutedEventArgs&) {
                        if (updatingControls || closed || !active)
                            return;
                        WidgetsPageRequest request;
                        request.command =
                            WidgetsPageCommand::SetPackageEnabled;
                        request.packageId = packageId;
                        request.enabled = enable;
                        Emit(std::move(request));
                    },
                    installedRevokers);
                primaryActions.Children().Append(enabledAction);
            }
            if (package.showAddToDesktop)
            {
                muxc::Button add = MakeActionButton(
                    L("app.widget_preview.add_to_desktop",
                        L"Add to Desktop"));
                add.IsEnabled(package.canAddToDesktop);
                HookClick(add,
                    [this, packageId = package.packageId](
                        const winrt::Windows::Foundation::IInspectable&,
                        const mux::RoutedEventArgs&) {
                        WidgetsPageRequest request;
                        request.command =
                            WidgetsPageCommand::AddPackageToDesktop;
                        request.packageId = packageId;
                        Emit(std::move(request));
                    },
                    installedRevokers);
                primaryActions.Children().Append(add);
            }
            presenter_controls::SettingRow primaryActionsRow;
            primaryActionsRow.Initialize(primaryActions);
            primaryActionsRow.SetControlAlignment(
                mux::HorizontalAlignment::Right);
            primaryActionsRow.SetText(
                L("app.settings.widgets_enabled", L"Enabled"),
                PackageStateText(package));
            body.Children().Append(primaryActionsRow.root);
        }

        if (developerOverridesVisible &&
            package.canUseDevelopmentOverride)
        {
            muxc::Button developmentAction = MakeActionButton(L(
                package.developmentOverrideActive
                    ? "app.settings.widgets_development_deactivate"
                    : "app.settings.widgets_development_activate",
                package.developmentOverrideActive
                    ? L"Deactivate Development Version"
                    : L"Activate Development Version"));
            HookClick(developmentAction,
                [this, packageId = package.packageId,
                    enable = !package.developmentOverrideActive](
                    const winrt::Windows::Foundation::IInspectable&,
                    const mux::RoutedEventArgs&) {
                    if (updatingControls || closed || !active)
                        return;
                    WidgetsPageRequest request;
                    request.command =
                        WidgetsPageCommand::SetDevelopmentOverride;
                    request.packageId = packageId;
                    request.enabled = enable;
                    Emit(std::move(request));
                },
                installedRevokers);
            presenter_controls::SettingRow developmentRow;
            developmentRow.Initialize(developmentAction, 300.0);
            developmentRow.SetControlAlignment(
                mux::HorizontalAlignment::Right);
            developmentRow.SetText(L(
                    "app.settings.widgets_development",
                    L"Development version"),
                L("settings.developer.overrides.description",
                    L"Use the local development source."));
            body.Children().Append(developmentRow.root);
        }

        if (package.valid)
        {
            AddPermissionControls(package, body);
            AddInstanceControls(package, body);
            AddLegacyPackageActions(package, body);
        }

        if (package.canUninstall && !package.builtIn)
        {
            muxc::Button uninstall = MakeActionButton(
                L("app.settings.widgets_uninstall", L"Uninstall"),
                L("app.settings.widgets_uninstall_confirm",
                    L"Remove this component from SnowDesktop."));
            uninstall.HorizontalAlignment(mux::HorizontalAlignment::Right);
            HookClick(uninstall,
                [this, packageId = package.packageId,
                    displayName = std::wstring{name.Text().c_str()},
                    workshop = !package.workshopExternalItemId.empty()](
                    const winrt::Windows::Foundation::IInspectable&,
                    const mux::RoutedEventArgs&) {
                    RequestUninstall(packageId, displayName, workshop);
                },
                installedRevokers);
            body.Children().Append(uninstall);
        }

        row.Content(body);
        StretchExpanderBody(row, body);
        SetAutomation(row, name.Text(), package.description);
        if (!firstPackageTarget)
            firstPackageTarget = row;
        targetRows.Children().Append(row);
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

    void RenderInstalledRows()
    {
        Revoke(installedRevokers);
        installedRows.Children().Clear();
        includedRows.Children().Clear();
        firstPackageTarget = nullptr;
        firstPermissionTarget = nullptr;

        bool anyUserPackage = false;
        bool anyIncludedPackage = false;
        for (const InstalledWidgetPackageSnapshot& package : packages)
        {
            if (IsIncludedOnlyPackage(package))
            {
                continue;
            }
            if (!MatchesFilter(package))
                continue;
            anyUserPackage = true;
            AddPackageRow(package, installedRows);
        }
        if (!anyUserPackage)
        {
            installedRows.Children().Append(MakeSecondaryText(
                L("app.settings.widgets_filter_empty", L"No components")));
        }

        for (const InstalledWidgetPackageSnapshot& package : packages)
        {
            if (!IsIncludedOnlyPackage(package))
                continue;
            if (!MatchesQuery(package))
                continue;
            anyIncludedPackage = true;
            AddPackageRow(package, includedRows);
        }
        includedExpander.Visibility(anyIncludedPackage
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
        includedTitle.Text(L(
            "app.settings.widgets_filter_builtin", L"Included") + L" " +
            std::to_wstring(includedRows.Children().Size()));
        if (!query.empty() && anyIncludedPackage)
            includedExpander.IsExpanded(true);
    }

    void AddCatalogResult(
        const WidgetCatalogItemSnapshot& item,
        const muxc::StackPanel& rows)
    {
        muxc::StackPanel row;
        row.Spacing(4.0);
        row.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        muxc::TextBlock name;
        name.Text(item.name.empty() ? item.packageId : item.name);
        name.FontWeight(
            winrt::Windows::UI::Text::FontWeights::SemiBold());
        name.TextWrapping(mux::TextWrapping::Wrap);
        row.Children().Append(name);
        if (!item.description.empty())
            row.Children().Append(MakeSecondaryText(item.description));
        row.Children().Append(MakeSecondaryText(JoinMetadata(
            {item.version, item.author})));

        std::wstring actionText;
        if (item.updateAvailable)
            actionText = L("app.settings.widgets_update", L"Update");
        else if (item.installed)
            actionText = L("app.settings.widgets_installed", L"Installed");
        else
            actionText = L("app.settings.widgets_install", L"Install");

        muxc::Button install = MakeActionButton(actionText, item.description);
        install.IsEnabled(item.installAllowed &&
            (!item.installed || item.updateAvailable));
        HookClick(install,
            [this, item](
                const winrt::Windows::Foundation::IInspectable&,
                const mux::RoutedEventArgs&) {
                WidgetsPageRequest request;
                request.command = WidgetsPageCommand::InstallCatalogItem;
                request.packageId = item.packageId;
                request.sourceId = item.sourceId;
                request.externalItemId = item.externalItemId;
                request.version = item.version;
                Emit(std::move(request));
            },
            sourceRevokers);
        row.Children().Append(install);
        rows.Children().Append(row);
    }

    void AddSourceGroup(const WidgetSourceGroupSnapshot& source)
    {
        muxc::Expander expander;
        expander.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        expander.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        muxc::StackPanel header;
        header.Spacing(3.0);
        muxc::TextBlock name;
        std::wstring sourceName = source.name;
        if (!source.nameKey.empty())
            sourceName = L(source.nameKey, sourceName);
        if (sourceName.empty())
            sourceName = source.sourceId;
        name.Text(sourceName);
        name.FontWeight(
            winrt::Windows::UI::Text::FontWeights::SemiBold());
        name.TextWrapping(mux::TextWrapping::Wrap);
        muxc::TextBlock status = MakeSecondaryText(source.status);
        header.Children().Append(name);
        if (!source.status.empty())
            header.Children().Append(status);
        expander.Header(header);

        muxc::StackPanel body;
        body.Spacing(8.0);
        body.HorizontalAlignment(mux::HorizontalAlignment::Stretch);

        if (source.workshop)
        {
            muxc::Button open = MakeActionButton(
                L("app.settings.widgets_open_steam_workshop",
                    L"Open Workshop"));
            HookClick(open,
                [this, sourceId = source.sourceId](
                    const winrt::Windows::Foundation::IInspectable&,
                    const mux::RoutedEventArgs&) {
                    WidgetsPageRequest request;
                    request.command = WidgetsPageCommand::OpenWorkshop;
                    request.sourceId = sourceId;
                    Emit(std::move(request));
                },
                sourceRevokers);
            body.Children().Append(open);
        }

        if (source.supportsSynchronization)
        {
            muxc::Button synchronize = MakeActionButton(
                L("app.menu.refresh", L"Synchronize"), source.status);
            synchronize.IsEnabled(source.available);
            HookClick(synchronize,
                [this, sourceId = source.sourceId](
                    const winrt::Windows::Foundation::IInspectable&,
                    const mux::RoutedEventArgs&) {
                    WidgetsPageRequest request;
                    request.command = WidgetsPageCommand::SynchronizeSource;
                    request.sourceId = sourceId;
                    Emit(std::move(request));
                },
                sourceRevokers);
            body.Children().Append(synchronize);
        }

        bool anyResult = false;
        for (const WidgetCatalogItemSnapshot& result : source.results)
        {
            if (!ContainsInsensitive(result.name, query) &&
                !ContainsInsensitive(result.description, query) &&
                !ContainsInsensitive(result.packageId, query) &&
                !ContainsInsensitive(result.author, query))
            {
                continue;
            }
            anyResult = true;
            AddCatalogResult(result, body);
        }

        if (!anyResult)
        {
            body.Children().Append(MakeSecondaryText(
                L("app.settings.widgets_catalog_empty",
                    L"No matching components were found.")));
        }

        expander.Content(body);
        StretchExpanderBody(expander, body);
        SetAutomation(expander, sourceName, source.status);
        if (!firstSourceTarget)
            firstSourceTarget = expander;
        sourceRows.Children().Append(expander);
    }

    void RenderSourceRows()
    {
        Revoke(sourceRevokers);
        sourceRows.Children().Clear();
        firstSourceTarget = nullptr;
        if (sources.empty())
        {
            sourceRows.Children().Append(MakeSecondaryText(
                L("app.settings.widgets_catalog_empty",
                    L"No component sources are available.")));
            return;
        }
        for (const WidgetSourceGroupSnapshot& source : sources)
            AddSourceGroup(source);
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
        workshopHint.Visibility(workshopAvailable
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
        selfDevelopButton.Visibility(actions.setDeveloperToolsEnabled
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
        selfDevelopButton.IsEnabled(
            static_cast<bool>(actions.setDeveloperToolsEnabled));
        selfDevelopButton.IsChecked(developerOverridesVisible);
        sourcesCard.root.Visibility(sources.empty()
                ? mux::Visibility::Collapsed
                : mux::Visibility::Visible);
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
        if (acceptSearch)
        {
            searchRevision = snapshot.searchRevision;
            requestedSearchRevision = std::max(
                requestedSearchRevision, snapshot.searchRevision);
            sources = snapshot.sources;
            query = snapshot.searchQuery;
            searchBox.Text(query);
        }
        RefreshFilterItems();
        RenderInstalledRows();
        RenderDeveloperRows();
        RenderDebugRows();
        if (acceptSearch)
            RenderSourceRows();
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

        searchCard.title.Text(L("app.settings.widgets_my_components",
            L"My Components"));
        searchCard.description.Text(L("app.settings.widgets_subtitle",
            L"Browse, install, and manage desktop components."));
        installedCard.title.Text(L("settings.widgets.installed",
            L"Installed widgets"));
        installedCard.description.Text(L(
            "settings.widgets.installed.description",
            L"Search, enable, disable or uninstall widgets."));
        includedTitle.Text(L(
            "app.settings.widgets_filter_builtin", L"Included") + L" " +
            std::to_wstring(includedRows
                    ? includedRows.Children().Size()
                    : 0));
        includedDescription.Text(L(
            "app.settings.widgets_builtin",
            L"Included with SnowDesktop"));
        sourcesCard.title.Text(L("settings.widgets.sources",
            L"Sources and Workshop"));
        sourcesCard.description.Text(L(
            "settings.widgets.sources.description",
            L"Install widgets and synchronize sources."));
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
        managementRow.SetText(L(
                "app.settings.widgets_management", L"Add Components"),
            L("app.settings.widgets_subtitle",
                L"Browse, install, and manage desktop components."));
        RefreshFilterItems();
        installFileButton.Content(winrt::box_value(L(
            "app.settings.widgets_install_package",
            L"Install from File...")));
        workshopButton.Content(winrt::box_value(L(
            "app.settings.widgets_open_steam_workshop",
            L"Open Workshop")));
        selfDevelopButton.Content(winrt::box_value(L(
            "app.settings.widgets_self_develop", L"Develop Your Own")));
        workshopHint.Text(L(
            "app.settings.widgets_workshop_auto_sync_hint",
            L"Workshop subscriptions synchronize automatically."));
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

        SetAutomation(searchCard.root, searchCard.title.Text(),
            searchCard.description.Text());
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
        SetAutomation(installedCard.root, installedCard.title.Text(),
            installedCard.description.Text());
        SetAutomation(includedExpander, includedTitle.Text(),
            includedDescription.Text());
        SetAutomation(sourcesCard.root, sourcesCard.title.Text(),
            sourcesCard.description.Text());
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
        RenderSourceRows();
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
            return firstSourceTarget ? firstSourceTarget : sourceRows;
        if (focusId == "widgets.permissions")
            return firstPermissionTarget
                ? firstPermissionTarget
                : (firstPackageTarget ? firstPackageTarget : installedRows);
        if (focusId == "widgets.installed")
            return firstPackageTarget ? firstPackageTarget : installedRows;
        if (focusId == "widgets.included")
            return includedRows;
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

        Revoke(installedRevokers);
        Revoke(sourceRevokers);
        Revoke(developerRevokers);
        Revoke(debugRevokers);
        try
        {
            searchBox.TextChanged(searchChangedToken);
            allFilterButton.Click(allFilterToken);
            installedFilterButton.Click(installedFilterToken);
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
    const mux::Style& cardStyle)
    : impl_(std::make_unique<Impl>(std::move(localize), cardStyle))
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

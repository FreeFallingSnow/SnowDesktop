#include "pch.h"

#include "widgets_page_presenter.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <utility>

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxa = winrt::Microsoft::UI::Xaml::Automation;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;

namespace
{

enum class PackageFilter : std::uint8_t
{
    All,
    BuiltIn,
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

void SetAutomation(
    const mux::DependencyObject& element,
    std::wstring_view name,
    std::wstring_view help = {})
{
    muxa::AutomationProperties::SetName(element, std::wstring(name));
    muxa::AutomationProperties::SetHelpText(element, std::wstring(help));
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

    SettingsCard searchCard;
    SettingsCard installedCard;
    SettingsCard sourcesCard;

    muxc::AutoSuggestBox searchBox{nullptr};
    muxc::ComboBox filterCombo{nullptr};
    muxc::Button installFileButton{nullptr};
    muxc::Button workshopButton{nullptr};
    muxc::StackPanel taskPanel{nullptr};
    muxc::ProgressRing taskRing{nullptr};
    muxc::ProgressBar taskProgress{nullptr};
    muxc::TextBlock taskStatus{nullptr};
    muxc::Button cancelTaskButton{nullptr};
    muxc::StackPanel installedRows{nullptr};
    muxc::StackPanel sourceRows{nullptr};

    mux::FrameworkElement firstPackageTarget{nullptr};
    mux::FrameworkElement firstPermissionTarget{nullptr};
    mux::FrameworkElement firstSourceTarget{nullptr};

    std::vector<InstalledWidgetPackageSnapshot> packages;
    std::vector<WidgetSourceGroupSnapshot> sources;
    WidgetsPageTaskSnapshot task;
    WidgetsPageFeedbackSnapshot feedback;
    std::wstring query;
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

    winrt::event_token searchChangedToken{};
    winrt::event_token filterChangedToken{};
    winrt::event_token installFileToken{};
    winrt::event_token workshopToken{};
    winrt::event_token cancelTaskToken{};

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
        searchBox = muxc::AutoSuggestBox{};
        searchBox.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        searchBox.MaxWidth(720.0);
        searchBox.IsSuggestionListOpen(false);
        searchBox.UseSystemFocusVisuals(true);
        searchCard.content.Children().Append(searchBox);

        filterCombo = muxc::ComboBox{};
        filterCombo.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        filterCombo.MaxWidth(420.0);
        filterCombo.UseSystemFocusVisuals(true);
        searchCard.content.Children().Append(filterCombo);

        installFileButton = muxc::Button{};
        installFileButton.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        installFileButton.HorizontalContentAlignment(
            mux::HorizontalAlignment::Center);
        installFileButton.UseSystemFocusVisuals(true);
        searchCard.content.Children().Append(installFileButton);

        workshopButton = muxc::Button{};
        workshopButton.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        workshopButton.HorizontalContentAlignment(
            mux::HorizontalAlignment::Center);
        workshopButton.UseSystemFocusVisuals(true);
        searchCard.content.Children().Append(workshopButton);

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
        cancelTaskButton.HorizontalAlignment(mux::HorizontalAlignment::Left);
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

        InitializeCard(sourcesCard, cardStyle, root);
        sourceRows = muxc::StackPanel{};
        sourceRows.Spacing(8.0);
        sourceRows.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        sourcesCard.content.Children().Append(sourceRows);
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

        filterChangedToken = filterCombo.SelectionChanged(
            [this](const winrt::Windows::Foundation::IInspectable&,
                const muxc::SelectionChangedEventArgs&) {
                if (updatingControls || closed)
                    return;
                const int selected = filterCombo.SelectedIndex();
                if (selected >= 0 && selected <= 3)
                    filter = static_cast<PackageFilter>(selected);
                RenderInstalledRows();
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

        cancelTaskToken = cancelTaskButton.Click(
            [this](const winrt::Windows::Foundation::IInspectable&,
                const mux::RoutedEventArgs&) {
                WidgetsPageRequest request;
                request.command = WidgetsPageCommand::CancelTask;
                request.taskId = task.taskId;
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

    [[nodiscard]] bool MatchesFilter(
        const InstalledWidgetPackageSnapshot& package) const
    {
        switch (filter)
        {
        case PackageFilter::BuiltIn:
            if (!package.builtIn)
                return false;
            break;
        case PackageFilter::Installed:
            if (package.builtIn || package.development)
                return false;
            break;
        case PackageFilter::Development:
            if (!package.development &&
                !package.developmentOverrideActive)
            {
                return false;
            }
            break;
        case PackageFilter::All:
        default:
            break;
        }

        return ContainsInsensitive(package.name, query) ||
            ContainsInsensitive(package.description, query) ||
            ContainsInsensitive(package.packageId, query) ||
            ContainsInsensitive(package.author, query) ||
            ContainsInsensitive(package.sourceName, query);
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

    void EmitPermissionDecision(
        std::wstring packageId,
        std::wstring permissionId,
        bool granted)
    {
        const InstalledWidgetPackageSnapshot* package =
            FindPackage(packageId);
        if (!package)
            return;

        WidgetsPageRequest request;
        request.command = WidgetsPageCommand::SetPermissionDecision;
        request.packageId = std::move(packageId);
        request.permissionState = WidgetPackagePermissionState::Granted;
        request.grantedNetworkDomains = package->grantedNetworkDomains;
        const bool enablingNetwork = granted &&
            (permissionId == L"network.http" ||
                permissionId == L"network.internet");
        if (enablingNetwork && request.grantedNetworkDomains.empty())
        {
            // These domains are rendered in the permission expander below;
            // only an explicit network-permission enable carries them back.
            request.grantedNetworkDomains =
                package->declaredNetworkDomains;
        }
        for (const WidgetPermissionSnapshot& permission :
             package->permissions)
        {
            const bool include = permission.id == permissionId
                ? granted
                : permission.granted;
            if (include)
                request.grantedPermissions.push_back(permission.id);
        }
        Emit(std::move(request));
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
        button.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        button.HorizontalContentAlignment(mux::HorizontalAlignment::Center);
        button.UseSystemFocusVisuals(true);
        SetAutomation(button, text, help);
        return button;
    }

    void AddPermissionControls(
        const InstalledWidgetPackageSnapshot& package,
        const muxc::StackPanel& body)
    {
        muxc::Expander permissionsExpander;
        permissionsExpander.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        muxc::TextBlock permissionHeader;
        permissionHeader.Text(L("app.settings.widgets_permissions",
            L"Permissions"));
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
            PermissionStateText(package.permissionState);
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

                muxc::CheckBox check;
                check.Content(winrt::box_value(label));
                check.IsChecked(permission.granted);
                check.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
                check.UseSystemFocusVisuals(true);
                std::wstring help = permission.description;
                if (permission.required)
                {
                    const std::wstring warning = L(
                        "app.settings.widgets_permission_required_notice",
                        L"Clearing required access pauses this component.");
                    if (!help.empty())
                        help.append(L" ");
                    help.append(warning);
                }
                SetAutomation(check, label, help);
                if (!firstPermissionTarget)
                    firstPermissionTarget = check;

                HookClick(check,
                    [this, check, packageId = package.packageId,
                        permissionId = permission.id](
                        const winrt::Windows::Foundation::IInspectable&,
                        const mux::RoutedEventArgs&) {
                        if (updatingControls || closed || !active)
                            return;
                        const auto checked = check.IsChecked();
                        EmitPermissionDecision(packageId, permissionId,
                            checked && checked.Value());
                    },
                    installedRevokers);
                permissionBody.Children().Append(check);

                if (!permission.description.empty())
                    permissionBody.Children().Append(
                        MakeSecondaryText(permission.description));
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

            muxc::Button revoke = MakeActionButton(
                L("app.settings.widgets_revoke_permissions",
                    L"Revoke access"),
                L("app.settings.widgets_permission_required_notice",
                    L"Required access will be removed."));
            HookClick(revoke,
                [this, packageId = package.packageId](
                    const winrt::Windows::Foundation::IInspectable&,
                    const mux::RoutedEventArgs&) {
                    WidgetsPageRequest request;
                    request.command =
                        WidgetsPageCommand::SetPermissionDecision;
                    request.packageId = packageId;
                    request.permissionState =
                        WidgetPackagePermissionState::Denied;
                    Emit(std::move(request));
                },
                installedRevokers);
            permissionBody.Children().Append(revoke);
        }

        permissionsExpander.Content(permissionBody);
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
        muxc::TextBlock header;
        header.Text(L("settings.widget.fields", L"Widget settings"));
        header.FontWeight(
            winrt::Windows::UI::Text::FontWeights::SemiBold());
        instancesExpander.Header(header);

        muxc::StackPanel rows;
        rows.Spacing(7.0);
        for (const WidgetInstanceSnapshot& instance : package.instances)
        {
            muxc::StackPanel row;
            row.Spacing(4.0);
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
        SetAutomation(instancesExpander, header.Text());
        body.Children().Append(instancesExpander);
    }

    void AddPackageRow(const InstalledWidgetPackageSnapshot& package)
    {
        muxc::Expander row;
        row.HorizontalAlignment(mux::HorizontalAlignment::Stretch);

        muxc::StackPanel header;
        header.Spacing(3.0);
        header.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        muxc::TextBlock name;
        name.Text(package.name.empty() ? package.packageId : package.name);
        name.FontWeight(
            winrt::Windows::UI::Text::FontWeights::SemiBold());
        name.TextWrapping(mux::TextWrapping::Wrap);
        muxc::TextBlock metadata;
        metadata.Text(JoinMetadata(
            {package.version, package.author, package.sourceName}));
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

        muxc::ToggleSwitch enabled;
        enabled.Header(winrt::box_value(L(
            "app.settings.widgets_enabled", L"Enabled")));
        enabled.IsOn(package.enabled);
        enabled.IsEnabled(package.canEnable);
        enabled.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        enabled.UseSystemFocusVisuals(true);
        SetAutomation(enabled,
            L("app.settings.widgets_enabled", L"Enabled"), name.Text());
        HookToggled(enabled,
            [this, enabled, packageId = package.packageId](
                const winrt::Windows::Foundation::IInspectable&,
                const mux::RoutedEventArgs&) {
                if (updatingControls || closed || !active)
                    return;
                WidgetsPageRequest request;
                request.command = WidgetsPageCommand::SetPackageEnabled;
                request.packageId = packageId;
                request.enabled = enabled.IsOn();
                Emit(std::move(request));
            },
            installedRevokers);
        body.Children().Append(enabled);

        if (package.canAddToDesktop)
        {
            muxc::Button add = MakeActionButton(
                L("app.widget_preview.add_to_desktop",
                    L"Add to Desktop"));
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
            body.Children().Append(add);
        }

        if (developerOverridesVisible &&
            package.canUseDevelopmentOverride)
        {
            muxc::ToggleSwitch development;
            development.Header(winrt::box_value(L(
                "app.settings.widgets_development",
                L"Development version")));
            development.IsOn(package.developmentOverrideActive);
            development.HorizontalAlignment(
                mux::HorizontalAlignment::Stretch);
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
                installedRevokers);
            body.Children().Append(development);
        }

        AddPermissionControls(package, body);
        AddInstanceControls(package, body);

        if (package.canUninstall && !package.builtIn)
        {
            muxc::Button uninstall = MakeActionButton(
                L("app.settings.widgets_uninstall", L"Uninstall"),
                L("app.settings.widgets_uninstall_confirm",
                    L"Remove this component from SnowDesktop."));
            HookClick(uninstall,
                [this, packageId = package.packageId,
                    displayName = std::wstring{name.Text().c_str()}](
                    const winrt::Windows::Foundation::IInspectable&,
                    const mux::RoutedEventArgs&) {
                    RequestUninstall(packageId, displayName);
                },
                installedRevokers);
            body.Children().Append(uninstall);
        }

        row.Content(body);
        SetAutomation(row, name.Text(), package.description);
        if (!firstPackageTarget)
            firstPackageTarget = row;
        installedRows.Children().Append(row);
    }

    void RequestUninstall(
        const std::wstring& packageId,
        const std::wstring& displayName)
    {
        if (closed || !active || !actions.confirm || !actions.invoke)
            return;

        const std::uint64_t requestGeneration = generation;
        const std::wstring title =
            L("app.settings.widgets_uninstall", L"Uninstall") +
            (displayName.empty() ? std::wstring{} : L" — " + displayName);
        const std::wstring message = L(
            "app.settings.widgets_uninstall_confirm",
            L"Uninstall this component? Its desktop instances will stop.");
        const std::shared_ptr<CallbackGenerationGate> gate = callbackGate;
        const auto invoke = actions.invoke;

        actions.confirm(requestGeneration, title, message,
            [gate, invoke, requestGeneration, packageId](bool confirmed) {
                if (!confirmed || !invoke ||
                    gate->closed.load(std::memory_order_acquire) ||
                    !gate->active.load(std::memory_order_acquire) ||
                    gate->generation.load(std::memory_order_acquire) !=
                        requestGeneration)
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
        firstPackageTarget = nullptr;
        firstPermissionTarget = nullptr;

        bool any = false;
        for (const InstalledWidgetPackageSnapshot& package : packages)
        {
            if (!MatchesFilter(package))
                continue;
            any = true;
            AddPackageRow(package);
        }
        if (!any)
        {
            installedRows.Children().Append(MakeSecondaryText(
                L("app.settings.widgets_filter_empty", L"No components")));
        }
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
        feedbackBar.Severity(ToInfoBarSeverity(feedback.severity));
        feedbackBar.Title(title);
        feedbackBar.Message(feedback.message);
        feedbackBar.IsOpen(visible);
        SetAutomation(feedbackBar, title, feedback.message);
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
        RenderInstalledRows();
        if (acceptSearch)
            RenderSourceRows();
        RenderTask();
        RenderFeedback();
        updatingControls = false;
        return true;
    }

    void RefreshLocalizedText()
    {
        const bool previousUpdating = updatingControls;
        updatingControls = true;

        searchCard.title.Text(L("app.settings.widgets_title",
            L"Components"));
        searchCard.description.Text(L("app.settings.widgets_subtitle",
            L"Browse, install, and manage desktop components."));
        installedCard.title.Text(L("settings.widgets.installed",
            L"Installed widgets"));
        installedCard.description.Text(L(
            "settings.widgets.installed.description",
            L"Search, enable, disable or uninstall widgets."));
        sourcesCard.title.Text(L("settings.widgets.sources",
            L"Sources and Workshop"));
        sourcesCard.description.Text(L(
            "settings.widgets.sources.description",
            L"Install widgets and synchronize sources."));

        searchBox.PlaceholderText(L("app.settings.widgets_search_hint",
            L"Search components"));
        filterCombo.Items().Clear();
        filterCombo.Items().Append(winrt::box_value(
            L("app.settings.widgets_filter_all", L"All")));
        filterCombo.Items().Append(winrt::box_value(
            L("app.settings.widgets_filter_builtin", L"Included")));
        filterCombo.Items().Append(winrt::box_value(
            L("app.settings.widgets_filter_installed", L"Installed")));
        filterCombo.Items().Append(winrt::box_value(
            L("app.settings.widgets_filter_development",
                L"Local development")));
        filterCombo.SelectedIndex(static_cast<int>(filter));
        installFileButton.Content(winrt::box_value(L(
            "app.settings.widgets_install_package",
            L"Install from File...")));
        workshopButton.Content(winrt::box_value(L(
            "app.settings.widgets_open_steam_workshop",
            L"Open Workshop")));
        cancelTaskButton.Content(winrt::box_value(L(
            "settings.progress.cancel", L"Cancel")));

        SetAutomation(searchCard.root, searchCard.title.Text(),
            searchCard.description.Text());
        SetAutomation(searchBox,
            L("app.settings.widgets_search", L"Search"),
            searchBox.PlaceholderText());
        SetAutomation(filterCombo,
            L("app.settings.widgets_my_components", L"My Components"));
        SetAutomation(installFileButton,
            L("app.settings.widgets_install_package",
                L"Install from File..."));
        SetAutomation(workshopButton,
            L("app.settings.widgets_open_steam_workshop",
                L"Open Workshop"),
            L("app.settings.widgets_workshop_auto_sync_hint",
                L"Manage Workshop subscriptions."));
        SetAutomation(installedCard.root, installedCard.title.Text(),
            installedCard.description.Text());
        SetAutomation(sourcesCard.root, sourcesCard.title.Text(),
            sourcesCard.description.Text());

        RenderInstalledRows();
        RenderSourceRows();
        RenderTask();
        RenderFeedback();
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
        if (focusId == "widgets.sources")
            return firstSourceTarget ? firstSourceTarget : sourceRows;
        if (focusId == "widgets.permissions")
            return firstPermissionTarget
                ? firstPermissionTarget
                : (firstPackageTarget ? firstPackageTarget : installedRows);
        if (focusId == "widgets.installed")
            return firstPackageTarget ? firstPackageTarget : installedRows;
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
        callbackGate->active.store(true, std::memory_order_release);
    }

    void Deactivate() noexcept
    {
        active = false;
        callbackGate->active.store(false, std::memory_order_release);
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
        try
        {
            searchBox.TextChanged(searchChangedToken);
            filterCombo.SelectionChanged(filterChangedToken);
            installFileButton.Click(installFileToken);
            workshopButton.Click(workshopToken);
            cancelTaskButton.Click(cancelTaskToken);
        }
        catch (...)
        {
        }
        actions = {};
        packages.clear();
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
        impl_->actions = std::move(actions);
}

mux::UIElement WidgetsPagePresenter::Content() const noexcept
{
    return impl_ ? impl_->root : nullptr;
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

void WidgetsPagePresenter::Close() noexcept
{
    if (impl_)
        impl_->Close();
}

} // namespace snowdesktop::winui

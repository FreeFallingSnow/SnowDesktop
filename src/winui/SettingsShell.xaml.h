#pragma once

#include "SettingsShell.g.h"

#include "../settings_controller.h"
#include "../settings_search_index.h"
#include "backup_data_page_presenter.h"
#include "desktop_page_presenter.h"
#include "dock_page_presenter.h"
#include "general_page_presenter.h"
#include "home_about_page_presenter.h"
#include "personalization_page_presenter.h"
#include "settings_shell_navigation.h"
#include "widget_settings_presenter.h"
#include "widgets_page_presenter.h"

#include <winrt/Microsoft.UI.Xaml.Media.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace winrt::SnowDesktop::implementation
{

enum class SettingsShellInfoSeverity : std::uint8_t
{
    Informational,
    Success,
    Warning,
    Error,
};

struct SettingsShellProgress
{
    std::uint64_t generation = 0;
    std::wstring title;
    std::wstring message;
    bool indeterminate = true;
    double value = 0.0;
    bool cancellable = false;
};

struct SettingsShellDialogRequest
{
    std::uint64_t generation = 0;
    std::wstring title;
    std::wstring message;
    std::wstring primaryButtonText;
    std::wstring closeButtonText;
    bool destructive = false;
};

/**
 * WinUI 3 settings-center view hosted by DesktopWindowXamlSource.
 *
 * This type deliberately owns presentation state only.  SettingsController,
 * search indexing, persistence and widget operations remain native services
 * injected through snapshots and callbacks.
 */
struct SettingsShell : SettingsShellT<SettingsShell>
{
    using LocalizeCallback =
        std::function<std::wstring(std::string_view key)>;
    using RouteRequestedCallback =
        std::function<void(const snowdesktop::SettingsRoute& route)>;
    using SearchRequestedCallback = std::function<void(
        std::wstring query,
        std::uint64_t generation,
        std::uint64_t requestId)>;
    using CancelOperationCallback =
        std::function<void(std::uint64_t generation)>;
    using DialogCompletedCallback = std::function<void(bool confirmed)>;

    SettingsShell();
    ~SettingsShell();

    SettingsShell(const SettingsShell&) = delete;
    SettingsShell& operator=(const SettingsShell&) = delete;

    void Close() noexcept;

    void SetLocalizer(LocalizeCallback localizer);
    void RefreshLocalizedText();

    /** Keep the root transparent only while the Island backdrop is active. */
    void SetSystemBackdropActive(bool active) noexcept;

    /**
     * Reserve the system AppWindow title-bar metrics inside the XAML Island.
     * AppWindow reports physical pixels; rasterizationScale converts them to
     * the device-independent units consumed by XAML.
     */
    void SetIntegratedTitleBarLayout(
        bool active,
        int heightPixels,
        int leftInsetPixels,
        int rightInsetPixels,
        double rasterizationScale) noexcept;

    void SetRouteRequestedCallback(RouteRequestedCallback callback);
    void SetSearchRequestedCallback(SearchRequestedCallback callback);
    void SetCancelOperationCallback(CancelOperationCallback callback);
    void SetGeneralPageActions(
        snowdesktop::winui::GeneralPageActions actions);
    void SetPersonalizationPageActions(
        snowdesktop::winui::PersonalizationPageActions actions);
    void SetDesktopPageActions(
        snowdesktop::winui::DesktopPageActions actions);
    void SetDockPageActions(
        snowdesktop::winui::DockPageActions actions);
    void SetHomeAboutPageActions(
        snowdesktop::winui::HomeAboutPageActions actions);
    [[nodiscard]] bool ApplyHomeAboutStatusPatch(
        const snowdesktop::winui::HomeAboutStatusPatch& patch);
    void SetWidgetSettingsService(
        snowdesktop::widget_runtime::WidgetSettingsService* service) noexcept;
    [[nodiscard]] bool ApplyWidgetSettingsSnapshot(
        const snowdesktop::widget_runtime::WidgetSettingsSnapshot& snapshot);
    void SetWidgetsPageActions(
        snowdesktop::winui::WidgetsPageActions actions);
    [[nodiscard]] bool ApplyWidgetsPageSnapshot(
        const snowdesktop::winui::WidgetsPageSnapshot& snapshot);
    void SetBackupDataPageActions(
        snowdesktop::winui::BackupDataPageActions actions);
    [[nodiscard]] bool ApplyBackupDataPageSnapshot(
        const snowdesktop::winui::BackupDataPageSnapshot& snapshot);

    [[nodiscard]] bool IsHotkeyCaptureActive() const noexcept;
    void CaptureRegisteredHotkey(UINT modifiers, UINT virtualKey);

    /** Flush the active declarative component editor before route/close. */
    [[nodiscard]] snowdesktop::widget_runtime::WidgetSettingMutationResult
        FlushPendingWidgetSettings();

    /** Pause active page input while the reusable top-level HWND is hidden. */
    void SuspendInteraction() noexcept;
    /** Rebind and reactivate the current route after the HWND is shown again. */
    void ResumeInteraction() noexcept;

    /**
     * Applies an immutable controller publication.  Returns false for an
     * invalid route or a stale generation/revision pair.
     */
    [[nodiscard]] bool ApplySnapshot(
        const snowdesktop::SettingsSnapshot& snapshot) noexcept;

    /**
     * Navigates presentation state.  notifyHost is reserved for user-like
     * entry points (search, breadcrumb, compatibility wrappers).
     */
    [[nodiscard]] bool Navigate(
        const snowdesktop::SettingsRoute& route,
        bool notifyHost = false) noexcept;

    [[nodiscard]] snowdesktop::SettingsRoute CurrentRoute() const;
    [[nodiscard]] std::uint64_t CurrentGeneration() const noexcept;
    [[nodiscard]] std::uint64_t CurrentRevision() const noexcept;

    /** Controls whether Developer Tools and Debug exist in navigation. */
    void SetConditionalPagesVisible(
        bool developerToolsVisible,
        bool debugVisible);

    /**
     * Replaces AutoSuggestBox results for the current query.  A result from a
     * closed/reloaded generation or superseded request is ignored.
     */
    [[nodiscard]] bool SetSearchResults(
        std::vector<snowdesktop::SettingsSearchResult> results,
        std::uint64_t generation,
        std::uint64_t requestId) noexcept;
    void ClearSearch();
    [[nodiscard]] std::uint64_t CurrentSearchRequestId() const noexcept;

    void RegisterFocusTarget(
        std::string focusId,
        const winrt::Microsoft::UI::Xaml::FrameworkElement& element);
    void UnregisterFocusTarget(std::string_view focusId);

    [[nodiscard]] bool ShowInfoForGeneration(
        std::uint64_t generation,
        SettingsShellInfoSeverity severity,
        std::wstring title,
        std::wstring message,
        bool closable = true) noexcept;
    void ClearInfo() noexcept;

    [[nodiscard]] bool ShowProgress(
        const SettingsShellProgress& progress) noexcept;
    void HideProgress(std::uint64_t generation) noexcept;

    void ShowConfirmation(
        SettingsShellDialogRequest request,
        DialogCompletedCallback completed);
    void ShowWidgetInstallConfirmation(
        std::uint64_t generation,
        snowdesktop::winui::WidgetInstallConfirmationRequest request,
        DialogCompletedCallback completed);
    void ShowWidgetPermissionEditor(
        std::uint64_t generation,
        snowdesktop::winui::WidgetPermissionEditorRequest request,
        snowdesktop::winui::WidgetsPageActions::PermissionEditorCompletion
            completed);

private:
    void HookEvents();
    void UnhookEvents() noexcept;
    void RenderRoute(
        bool forcePageCards = false,
        bool scheduleFocus = true);
    void RenderNavigationSelection();
    void RenderBreadcrumb();
    void RenderPageCards(bool forcePageCards = false);
    void RenderConditionalPages();
    void RenderControllerStatus(
        const snowdesktop::SettingsSnapshot& snapshot);
    void ScheduleFocus();
    void FocusPendingTarget();

    void RequestRoute(const snowdesktop::SettingsRoute& route);
    [[nodiscard]] std::wstring Localize(std::string_view key) const;
    [[nodiscard]] std::wstring PageTitleText(
        snowdesktop::SettingsPage page) const;
    [[nodiscard]] std::wstring PageDescriptionText(
        snowdesktop::SettingsPage page) const;

    [[nodiscard]] winrt::Microsoft::UI::Xaml::Controls::NavigationViewItem
        NavigationItemForPage(snowdesktop::SettingsPage page);
    [[nodiscard]] bool TrySelectSearchResult(
        const winrt::Windows::Foundation::IInspectable& selectedItem);

    winrt::Microsoft::UI::Xaml::Controls::Border CreatePlaceholderCard(
        std::string focusId,
        std::string_view titleKey,
        std::string_view descriptionKey);

    winrt::fire_and_forget ShowConfirmationAsync(
        SettingsShellDialogRequest request,
        DialogCompletedCallback completed);
    winrt::fire_and_forget ShowWidgetInstallConfirmationAsync(
        std::uint64_t generation,
        snowdesktop::SettingsRoute route,
        snowdesktop::winui::WidgetInstallConfirmationRequest request,
        DialogCompletedCallback completed);
    winrt::fire_and_forget ShowWidgetPermissionEditorAsync(
        std::uint64_t generation,
        snowdesktop::SettingsRoute route,
        snowdesktop::winui::WidgetPermissionEditorRequest request,
        snowdesktop::winui::WidgetsPageActions::PermissionEditorCompletion
            completed);

    LocalizeCallback localizer_;
    RouteRequestedCallback routeRequested_;
    SearchRequestedCallback searchRequested_;
    CancelOperationCallback cancelOperation_;

    std::unique_ptr<snowdesktop::winui::GeneralPagePresenter> generalPage_;
    std::unique_ptr<snowdesktop::winui::PersonalizationPagePresenter>
        personalizationPage_;
    std::unique_ptr<snowdesktop::winui::DesktopPagePresenter> desktopPage_;
    std::unique_ptr<snowdesktop::winui::DockPagePresenter> dockPage_;
    std::unique_ptr<snowdesktop::winui::HomeAboutPagePresenter>
        homeAboutPage_;
    snowdesktop::widget_runtime::WidgetSettingsService*
        widgetSettingsService_ = nullptr;
    std::unique_ptr<snowdesktop::winui::WidgetSettingsPresenter>
        widgetSettingsPage_;
    std::unique_ptr<snowdesktop::winui::WidgetsPagePresenter>
        widgetsPage_;
    std::unique_ptr<snowdesktop::winui::BackupDataPagePresenter>
        backupDataPage_;

    snowdesktop::winui::SettingsShellNavigationState navigation_;
    std::vector<snowdesktop::SettingsSearchResult> searchResults_;
    std::vector<snowdesktop::SettingsRoute> breadcrumbRoutes_;
    winrt::Windows::Foundation::Collections::IObservableVector<
        winrt::Windows::Foundation::IInspectable> searchItems_{nullptr};
    std::unordered_map<std::string,
        winrt::weak_ref<winrt::Microsoft::UI::Xaml::FrameworkElement>>
        focusTargets_;

    winrt::Microsoft::UI::Xaml::Controls::ContentDialog activeDialog_{nullptr};
    winrt::Microsoft::UI::Xaml::Media::Brush
        solidFallbackBackground_{nullptr};
    std::uint64_t searchRequestId_ = 0;
    std::uint64_t progressGeneration_ = 0;
    std::optional<snowdesktop::SettingsRoute> renderedPageRoute_;
    std::uint32_t ownerThreadId_ = 0;
    bool updatingNavigation_ = false;
    bool updatingSearch_ = false;
    bool integratedTitleBarActive_ = false;
    bool closed_ = false;

    winrt::event_token selectionChangedToken_{};
    winrt::event_token backRequestedToken_{};
    winrt::event_token breadcrumbClickedToken_{};
    winrt::event_token searchTextChangedToken_{};
    winrt::event_token searchQuerySubmittedToken_{};
    winrt::event_token searchSuggestionChosenToken_{};
    winrt::event_token clearSearchToken_{};
    winrt::event_token cancelOperationToken_{};
};

} // namespace winrt::SnowDesktop::implementation

namespace winrt::SnowDesktop::factory_implementation
{
struct SettingsShell :
    SettingsShellT<SettingsShell, implementation::SettingsShell>
{
};
}

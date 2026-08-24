#include "pch.h"

#include "settings_window_host.h"

#include "SettingsShell.xaml.h"
#include "winui_runtime.h"
#include "../widget_settings_service.h"

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace snowdesktop::winui
{
namespace mud = winrt::Microsoft::UI::Dispatching;
namespace mux = winrt::Microsoft::UI::Xaml;
namespace shell_impl = winrt::SnowDesktop::implementation;

namespace
{
constexpr wchar_t kSettingsWindowClassName[] =
    L"SnowDesktop.WinUI3.SettingsWindow";
constexpr int kDefaultClientWidth = 1100;
constexpr int kDefaultClientHeight = 760;
constexpr int kMinimumClientWidth = 720;
constexpr int kMinimumClientHeight = 520;

struct StaticSearchDefinition
{
    SettingsPage page;
    const char* focusId;
    const char* labelKey;
    const char* descriptionKey;
};

constexpr StaticSearchDefinition kStaticSearchDefinitions[] = {
    {SettingsPage::Home, "home.theme", "settings.home.theme",
        "settings.home.theme.description"},
    {SettingsPage::Home, "home.dock", "settings.home.dock",
        "settings.home.dock.description"},
    {SettingsPage::Home, "home.widgets", "settings.home.widgets",
        "settings.home.widgets.description"},
    {SettingsPage::Home, "home.backup", "settings.home.backup",
        "settings.home.backup.description"},
    {SettingsPage::General, "general.softwareDesktop",
        "settings.general.softwareDesktop",
        "settings.general.softwareDesktop.description"},
    {SettingsPage::General, "general.language",
        "settings.general.language",
        "settings.general.language.description"},
    {SettingsPage::General, "general.doubleClickHide",
        "settings.general.doubleClickHide",
        "settings.general.doubleClickHide.description"},
    {SettingsPage::General, "general.quickNavigation",
        "settings.general.quickNavigation",
        "settings.general.quickNavigation.description"},
    {SettingsPage::General, "general.pageNavigation",
        "settings.general.pageNavigation",
        "settings.general.pageNavigation.description"},
    {SettingsPage::General, "general.desktopPassthrough",
        "settings.general.desktopPassthrough",
        "settings.general.desktopPassthrough.description"},
    {SettingsPage::General, "general.floatingDock",
        "settings.general.floatingDock",
        "settings.general.floatingDock.description"},
    {SettingsPage::Personalization, "personalization.theme",
        "settings.personalization.theme",
        "settings.personalization.theme.description"},
    {SettingsPage::Personalization, "personalization.backgroundColor",
        "settings.personalization.colors",
        "settings.personalization.colors.description"},
    {SettingsPage::Personalization, "personalization.glass",
        "settings.personalization.glass",
        "settings.personalization.glass.description"},
    {SettingsPage::Personalization, "personalization.acrylic",
        "settings.personalization.acrylic",
        "settings.personalization.acrylic.description"},
    {SettingsPage::Personalization, "personalization.contextMenu",
        "settings.personalization.contextMenu",
        "settings.personalization.contextMenu.description"},
    {SettingsPage::Personalization, "personalization.cornerRadius",
        "settings.personalization.widgets",
        "settings.personalization.widgets.description"},
    {SettingsPage::Desktop, "desktop.spacing", "settings.desktop.spacing",
        "settings.desktop.spacing.description"},
    {SettingsPage::Desktop, "desktop.iconSize", "settings.desktop.iconSize",
        "settings.desktop.iconSize.description"},
    {SettingsPage::Desktop, "desktop.itemFontSize",
        "settings.desktop.typography",
        "settings.desktop.typography.description"},
    {SettingsPage::Desktop, "desktop.shortcutArrow",
        "settings.desktop.shortcutArrow",
        "settings.desktop.shortcutArrow.description"},
    {SettingsPage::Desktop, "desktop.iconBeautify",
        "settings.desktop.iconBeautify",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::Desktop, "desktop.categoryLayout",
        "settings.desktop.categoryLayout",
        "settings.desktop.categoryLayout.description"},
    {SettingsPage::Desktop, "desktop.categories",
        "settings.desktop.categories",
        "settings.desktop.categories.description"},
    {SettingsPage::DockAndTaskbar, "dock.enable", "settings.dock.enable",
        "settings.dock.enable.description"},
    {SettingsPage::DockAndTaskbar, "dock.position",
        "settings.dock.position", "settings.dock.position.description"},
    {SettingsPage::DockAndTaskbar, "dock.layout", "settings.dock.layout",
        "settings.dock.layout.description"},
    {SettingsPage::DockAndTaskbar, "dock.monitor", "settings.dock.monitor",
        "settings.dock.monitor.description"},
    {SettingsPage::DockAndTaskbar, "dock.thickness",
        "settings.dock.thickness", "settings.dock.thickness.description"},
    {SettingsPage::DockAndTaskbar, "dock.showFrequentItems",
        "settings.dock.frequentItems",
        "settings.dock.frequentItems.description"},
    {SettingsPage::DockAndTaskbar, "taskbar.autoHide",
        "settings.taskbar.autoHide",
        "settings.taskbar.autoHide.description"},
    {SettingsPage::DockAndTaskbar, "taskbar.alignment",
        "settings.taskbar.alignment",
        "settings.taskbar.alignment.description"},
    {SettingsPage::DockAndTaskbar, "taskbar.theme",
        "settings.taskbar.theme", "settings.taskbar.theme.description"},
    {SettingsPage::DockAndTaskbar, "taskbar.restartExplorer",
        "settings.taskbar.restartExplorer",
        "settings.taskbar.restartExplorer.description"},
    {SettingsPage::Widgets, "widgets.installed",
        "settings.widgets.installed",
        "settings.widgets.installed.description"},
    {SettingsPage::Widgets, "widgets.sources", "settings.widgets.sources",
        "settings.widgets.sources.description"},
    {SettingsPage::Widgets, "widgets.permissions",
        "settings.widgets.permissions",
        "settings.widgets.permissions.description"},
    {SettingsPage::BackupAndData, "backup.layout", "settings.backup.layout",
        "settings.backup.layout.description"},
    {SettingsPage::BackupAndData, "backup.full", "settings.backup.full",
        "settings.backup.full.description"},
    {SettingsPage::BackupAndData, "backup.directory",
        "settings.backup.directory",
        "settings.backup.directory.description"},
    {SettingsPage::About, "about.version", "settings.about.version",
        "settings.about.version.description"},
    {SettingsPage::About, "about.project", "settings.about.project",
        "settings.about.project.description"},
    {SettingsPage::About, "about.thirdparty", "settings.about.thirdparty",
        "settings.about.thirdparty.description"},
    {SettingsPage::DeveloperTools, "developer.overrides",
        "settings.developer.overrides",
        "settings.developer.overrides.description"},
    {SettingsPage::DeveloperTools, "developer.tools",
        "settings.developer.tools",
        "settings.developer.tools.description"},
    {SettingsPage::Debug, "debug.runtime", "settings.debug.runtime",
        "settings.debug.runtime.description"},
};

std::wstring FormatWin32Error(const wchar_t* operation, DWORD error)
{
    std::wstring result = operation ? operation : L"Win32 operation";
    result += L" (";
    result += std::to_wstring(error);
    result += L")";
    return result;
}

bool IsUsableControllerSnapshot(
    const SettingsController::SnapshotPtr& snapshot) noexcept
{
    return snapshot && snapshot->initialized;
}
} // namespace

struct SettingsWindowHost::Impl
{
    struct CallbackState
    {
        std::atomic<bool> alive{true};
        std::atomic<bool> snapshotQueued{false};
        std::atomic<bool> flushQueued{false};
        std::mutex snapshotMutex;
        SettingsController::SnapshotPtr latestSnapshot;
        mud::DispatcherQueue dispatcher{nullptr};
        Impl* owner = nullptr;
    };

    DWORD ownerThreadId = 0;
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    SettingsController* controller = nullptr;
    widget_runtime::WidgetSettingsService* widgetSettingsService = nullptr;
    SettingsWindowHostOptions options;
    WinUiRuntime runtime;
    winrt::com_ptr<shell_impl::SettingsShell> shell;
    SettingsSearchIndex searchIndex;
    std::shared_ptr<CallbackState> callbacks;
    std::uint64_t viewEpoch = 0;
    bool initialized = false;
    bool shuttingDown = false;
    bool interactionSuspended = true;
    std::wstring lastError;

    [[nodiscard]] bool OnOwnerThread() const noexcept
    {
        return ownerThreadId != 0 &&
            ownerThreadId == GetCurrentThreadId();
    }

    [[nodiscard]] bool Visible() const noexcept
    {
        return window && IsWindow(window) &&
            IsWindowVisible(window) != FALSE;
    }

    std::wstring L(std::string_view key) const
    {
        if (options.localize)
        {
            std::wstring value = options.localize(key);
            if (!value.empty())
                return value;
        }
        return {};
    }

    void SetError(std::wstring message)
    {
        lastError = std::move(message);
    }

    void ShowActionError(const SettingsActionResult& result)
    {
        if (!shell || !controller || result.Succeeded())
            return;
        const auto snapshot = controller->Snapshot();
        if (!snapshot)
            return;
        std::wstring title = L("settings.status.error");
        if (title.empty())
            title = L"Settings";
        std::wstring message = result.message;
        if (message.empty())
            message = L("settings.status.saveFailed");
        (void)shell->ShowInfoForGeneration(snapshot->generation,
            shell_impl::SettingsShellInfoSeverity::Error,
            std::move(title), std::move(message));
    }

    SettingsSearchIndexInput BuildSearchInput() const
    {
        SettingsSearchIndexInput input;
        if (options.searchInput)
            input = options.searchInput();

        input.developerToolsVisible = options.developerToolsVisible &&
            options.developerToolsVisible();
        input.debugVisible = options.debugVisible &&
            options.debugVisible();
        if (input.languageTag.empty())
            input.languageTag = "runtime";

        if (input.staticSettings.empty())
        {
            input.staticSettings.reserve(std::size(kStaticSearchDefinitions));
            for (const auto& definition : kStaticSearchDefinitions)
            {
                StaticSettingSearchDescriptor descriptor;
                descriptor.page = definition.page;
                descriptor.focusId = definition.focusId;
                descriptor.label = L(definition.labelKey);
                descriptor.description = L(definition.descriptionKey);
                descriptor.visible =
                    definition.page != SettingsPage::DeveloperTools &&
                        definition.page != SettingsPage::Debug
                    ? true
                    : (definition.page == SettingsPage::DeveloperTools
                        ? input.developerToolsVisible
                        : input.debugVisible);
                if (!descriptor.label.empty())
                    input.staticSettings.push_back(std::move(descriptor));
            }
        }
        return input;
    }

    void RebuildSearchIndex()
    {
        try
        {
            SettingsSearchIndexInput input = BuildSearchInput();
            searchIndex.Rebuild(input);
            if (shell)
            {
                shell->SetConditionalPagesVisible(
                    input.developerToolsVisible, input.debugVisible);
            }
        }
        catch (...)
        {
            SetError(L"Rebuild settings search index failed");
        }
    }

    void QueueSnapshot(SettingsController::SnapshotPtr snapshot)
    {
        if (!callbacks || !snapshot)
            return;
        {
            std::lock_guard lock(callbacks->snapshotMutex);
            callbacks->latestSnapshot = std::move(snapshot);
        }
        if (callbacks->snapshotQueued.exchange(true))
            return;

        const std::uint64_t expectedEpoch = viewEpoch;
        const std::weak_ptr<CallbackState> weak = callbacks;
        try
        {
            if (!callbacks->dispatcher.TryEnqueue(
                    [weak, expectedEpoch]() {
                        const auto state = weak.lock();
                        if (!state)
                            return;
                        state->snapshotQueued.store(false);
                        if (!state->alive.load() || !state->owner ||
                            state->owner->viewEpoch != expectedEpoch)
                        {
                            return;
                        }
                        SettingsController::SnapshotPtr latest;
                        {
                            std::lock_guard lock(state->snapshotMutex);
                            latest = std::move(state->latestSnapshot);
                        }
                        state->owner->ApplySnapshotNow(std::move(latest));
                    }))
            {
                callbacks->snapshotQueued.store(false);
            }
        }
        catch (...)
        {
            callbacks->snapshotQueued.store(false);
        }
    }

    void ApplySnapshotNow(SettingsController::SnapshotPtr snapshot)
    {
        if (!shell || !snapshot || shuttingDown)
            return;
        if (!Visible() && !snapshot->sessionActive)
            return;
        (void)shell->ApplySnapshot(*snapshot);
        if (options.homeAboutStatus)
        {
            HomeAboutStatusPatch patch = options.homeAboutStatus(
                snapshot->generation, snapshot->revision);
            patch.generation = snapshot->generation;
            patch.revision = snapshot->revision;
            (void)shell->ApplyHomeAboutStatusPatch(patch);
        }
    }

    void QueuePendingFlush()
    {
        if (!callbacks || callbacks->flushQueued.exchange(true))
            return;
        const std::uint64_t expectedEpoch = viewEpoch;
        const std::weak_ptr<CallbackState> weak = callbacks;
        try
        {
            if (!callbacks->dispatcher.TryEnqueue(
                    [weak, expectedEpoch]() {
                        const auto state = weak.lock();
                        if (!state)
                            return;
                        state->flushQueued.store(false);
                        if (!state->alive.load() || !state->owner ||
                            state->owner->viewEpoch != expectedEpoch)
                        {
                            return;
                        }
                        state->owner->FlushPendingNow();
                    }))
            {
                callbacks->flushQueued.store(false);
            }
        }
        catch (...)
        {
            callbacks->flushQueued.store(false);
        }
    }

    void FlushPendingNow()
    {
        if (!controller || shuttingDown)
            return;
        const SettingsActionResult result = controller->FlushPending();
        if (!result.Succeeded())
            ShowActionError(result);
        else
            RefreshLocalizedPresentation();
    }

    void ConfigurePageActions()
    {
        if (!shell || !callbacks)
            return;
        const std::weak_ptr<CallbackState> weak = callbacks;

        GeneralPageActions general;
        general.commitGeneral = [weak](std::uint64_t generation,
                                      GeneralPageActions::GeneralEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditGeneral(
                    generation, SettingsUpdateMode::Commit, std::move(edit));
            }
        };
        general.commitNavigation = [weak](
            std::uint64_t generation,
            GeneralPageActions::NavigationEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditNavigation(
                    generation, SettingsUpdateMode::Commit, std::move(edit));
            }
        };
        general.commitDock = [weak](std::uint64_t generation,
                                   GeneralPageActions::DockEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditDock(
                    generation, SettingsUpdateMode::Commit, std::move(edit));
            }
        };
        general.probeHotkey = [weak](
            SettingsHostActions::HotkeyTarget target,
            HotkeyChord chord,
            std::uint64_t generation,
            std::uint64_t,
            HotkeyRecorder::AvailabilityCompletion completion) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                completion(false, L"");
                return;
            }
            SettingsHostActions::Request request;
            request.action = SettingsHostActions::Action::
                ProbeHotkeyAvailability;
            request.hotkeyTarget = target;
            request.modifiers = chord.modifiers;
            request.virtualKey = chord.virtualKey;
            const SettingsActionResult result =
                state->owner->controller->InvokeHostAction(request);
            completion(result.Succeeded(), result.message);
        };
        general.languageCatalog = [weak]() {
            std::vector<SettingsLanguageOption> result;
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->options.languageCatalog)
            {
                return result;
            }
            for (auto&& [code, label] :
                state->owner->options.languageCatalog())
            {
                result.push_back(
                    {std::move(code), std::move(label)});
            }
            return result;
        };
        shell->SetGeneralPageActions(std::move(general));

        PersonalizationPageActions personalization;
        personalization.update = [weak](
            std::uint64_t generation,
            SettingsUpdateMode mode,
            PersonalizationPageActions::Edit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditPersonalization(
                    generation, mode, std::move(edit));
            }
        };
        shell->SetPersonalizationPageActions(std::move(personalization));

        DesktopPageActions desktop;
        desktop.updateDesktop = [weak](
            std::uint64_t generation,
            SettingsUpdateMode mode,
            DesktopPageActions::DesktopEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditDesktop(
                    generation, mode, std::move(edit));
            }
        };
        desktop.updateCategory = [weak](
            std::uint64_t generation,
            SettingsUpdateMode mode,
            DesktopPageActions::CategoryEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditCategory(
                    generation, mode, std::move(edit));
            }
        };
        desktop.updatePersonalization = [weak](
            std::uint64_t generation,
            SettingsUpdateMode mode,
            DesktopPageActions::PersonalizationEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditPersonalization(
                    generation, mode, std::move(edit));
            }
        };
        desktop.commitCategory = [weak](std::uint64_t generation) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                return;
            }
            state->owner->controller->RequestCommit(SettingsDomain::Category);
        };
        shell->SetDesktopPageActions(std::move(desktop));

        DockPageActions dock;
        dock.updateGeneral = [weak](
            std::uint64_t generation,
            SettingsUpdateMode mode,
            DockPageActions::GeneralEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditGeneral(
                    generation, mode, std::move(edit));
            }
        };
        dock.updateDock = [weak](
            std::uint64_t generation,
            SettingsUpdateMode mode,
            DockPageActions::DockEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditDock(
                    generation, mode, std::move(edit));
            }
        };
        dock.invokeHost = [weak](
            std::uint64_t generation,
            SettingsHostActions::Request request) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                return;
            }
            const SettingsActionResult result =
                state->owner->controller->InvokeHostAction(request);
            state->owner->ShowActionError(result);
        };
        dock.confirm = [weak](
            std::uint64_t generation,
            std::wstring title,
            std::wstring message,
            DockPageActions::ConfirmationCompletion completion) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->shell ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                completion(false);
                return;
            }
            shell_impl::SettingsShellDialogRequest request;
            request.generation = generation;
            request.title = std::move(title);
            request.message = std::move(message);
            request.primaryButtonText =
                state->owner->L("settings.dialog.confirm");
            request.closeButtonText =
                state->owner->L("settings.dialog.cancel");
            request.destructive = true;
            state->owner->shell->ShowConfirmation(
                std::move(request), std::move(completion));
        };
        shell->SetDockPageActions(std::move(dock));

        HomeAboutPageActions homeAbout;
        homeAbout.navigate = [weak](const SettingsRoute& route) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->RequestRoute(route);
            }
        };
        homeAbout.invoke = [weak](
            std::uint64_t generation,
            HomeAboutCommand command) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                return;
            }

            SettingsHostActions::Request request;
            switch (command)
            {
            case HomeAboutCommand::CheckForUpdates:
                request.action = SettingsHostActions::Action::CheckForUpdates;
                break;
            case HomeAboutCommand::OpenProject:
                request.action = SettingsHostActions::Action::OpenProject;
                break;
            case HomeAboutCommand::OpenLicense:
                request.action = SettingsHostActions::Action::OpenLicense;
                break;
            case HomeAboutCommand::OpenThirdPartyNotices:
                request.action =
                    SettingsHostActions::Action::OpenThirdPartyNotices;
                break;
            }
            const SettingsActionResult result =
                state->owner->controller->InvokeHostAction(request);
            state->owner->ShowActionError(result);
        };
        shell->SetHomeAboutPageActions(std::move(homeAbout));
    }

    void EditGeneral(std::uint64_t generation, SettingsUpdateMode mode,
        GeneralPageActions::GeneralEdit edit)
    {
        if (!controller || !controller->IsGenerationCurrent(generation))
            return;
        const auto snapshot = controller->Snapshot();
        if (!snapshot || snapshot->generation != generation)
            return;
        GeneralSettings value = snapshot->values.general;
        edit(value);
        controller->UpdateGeneral(std::move(value), mode);
    }

    void EditNavigation(std::uint64_t generation, SettingsUpdateMode mode,
        GeneralPageActions::NavigationEdit edit)
    {
        if (!controller || !controller->IsGenerationCurrent(generation))
            return;
        const auto snapshot = controller->Snapshot();
        if (!snapshot || snapshot->generation != generation)
            return;
        NavigationSettings value = snapshot->values.navigation;
        edit(value);
        controller->UpdateNavigation(std::move(value), mode);
    }

    void EditDock(std::uint64_t generation, SettingsUpdateMode mode,
        DockPageActions::DockEdit edit)
    {
        if (!controller || !controller->IsGenerationCurrent(generation))
            return;
        const auto snapshot = controller->Snapshot();
        if (!snapshot || snapshot->generation != generation)
            return;
        DockSettings value = snapshot->values.dock;
        edit(value);
        controller->UpdateDock(std::move(value), mode);
    }

    void EditPersonalization(std::uint64_t generation,
        SettingsUpdateMode mode, PersonalizationPageActions::Edit edit)
    {
        if (!controller || !controller->IsGenerationCurrent(generation))
            return;
        const auto snapshot = controller->Snapshot();
        if (!snapshot || snapshot->generation != generation)
            return;
        PersonalizationSettings value = snapshot->values.personalization;
        edit(value);
        controller->UpdatePersonalization(std::move(value), mode);
    }

    void EditDesktop(std::uint64_t generation, SettingsUpdateMode mode,
        DesktopPageActions::DesktopEdit edit)
    {
        if (!controller || !controller->IsGenerationCurrent(generation))
            return;
        const auto snapshot = controller->Snapshot();
        if (!snapshot || snapshot->generation != generation)
            return;
        DesktopDisplaySettings value = snapshot->values.desktop;
        edit(value);
        controller->UpdateDesktop(std::move(value), mode);
    }

    void EditCategory(std::uint64_t generation, SettingsUpdateMode mode,
        DesktopPageActions::CategoryEdit edit)
    {
        if (!controller || !controller->IsGenerationCurrent(generation))
            return;
        const auto snapshot = controller->Snapshot();
        if (!snapshot || snapshot->generation != generation)
            return;
        CategorySettings value = snapshot->values.category;
        edit(value);
        controller->UpdateCategory(std::move(value), mode);
    }

    void RequestRoute(const SettingsRoute& route)
    {
        if (!controller || !route.IsValid() || shuttingDown)
            return;
        const SettingsActionResult result = controller->Open(route);
        if (!result.Succeeded())
            ShowActionError(result);
    }

    void RequestSearch(std::wstring query, std::uint64_t generation,
        std::uint64_t requestId)
    {
        if (!callbacks)
            return;
        const std::uint64_t expectedEpoch = viewEpoch;
        const std::weak_ptr<CallbackState> weak = callbacks;
        try
        {
            (void)callbacks->dispatcher.TryEnqueue(
                [weak, query = std::move(query), generation, requestId,
                    expectedEpoch]() mutable {
                    const auto state = weak.lock();
                    if (!state || !state->alive.load() || !state->owner ||
                        state->owner->viewEpoch != expectedEpoch ||
                        !state->owner->shell ||
                        !state->owner->controller ||
                        !state->owner->controller->IsGenerationCurrent(
                            generation))
                    {
                        return;
                    }
                    auto results = state->owner->searchIndex.Search(query);
                    (void)state->owner->shell->SetSearchResults(
                        std::move(results), generation, requestId);
                });
        }
        catch (...)
        {
        }
    }

    void RefreshLocalizedPresentation()
    {
        if (!shell)
            return;
        shell->RefreshLocalizedText();
        RebuildSearchIndex();
        std::wstring title = L("settings.shell.title");
        if (title.empty())
            title = options.windowTitle;
        if (window && IsWindow(window))
            SetWindowTextW(window, title.c_str());
    }

    void ResumeInteraction()
    {
        if (!shell || !interactionSuspended)
            return;
        shell->ResumeInteraction();
        interactionSuspended = false;
    }

    void SuspendInteraction()
    {
        if (!shell || interactionSuspended)
            return;
        shell->SuspendInteraction();
        interactionSuspended = true;
    }

    static LRESULT CALLBACK WindowProcedure(
        HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        Impl* self = reinterpret_cast<Impl*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(
                hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self)
                self->window = hwnd;
        }

        if (!self)
            return DefWindowProcW(hwnd, message, wParam, lParam);

        switch (message)
        {
        case WM_CLOSE:
            (void)self->HideWindow();
            return 0;
        case WM_GETMINMAXINFO:
        {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            const UINT dpi = GetDpiForWindow(hwnd);
            info->ptMinTrackSize.x = MulDiv(
                kMinimumClientWidth, static_cast<int>(dpi), 96);
            info->ptMinTrackSize.y = MulDiv(
                kMinimumClientHeight, static_cast<int>(dpi), 96);
            return 0;
        }
        case WM_DPICHANGED:
        {
            const auto* suggested = reinterpret_cast<RECT*>(lParam);
            if (suggested)
            {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                    suggested->right - suggested->left,
                    suggested->bottom - suggested->top,
                    SWP_NOACTIVATE | SWP_NOZORDER);
            }
            break;
        }
        case WM_NCDESTROY:
            self->runtime.HandleWindowMessage(message, wParam, lParam);
            self->window = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return DefWindowProcW(hwnd, message, wParam, lParam);
        default:
            break;
        }

        self->runtime.HandleWindowMessage(message, wParam, lParam);
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    [[nodiscard]] bool RegisterWindowClass()
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = WindowProcedure;
        windowClass.hInstance = instance;
        windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
        if (!windowClass.hIcon)
            windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        windowClass.hIconSm = windowClass.hIcon;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground =
            reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClass.lpszClassName = kSettingsWindowClassName;
        if (RegisterClassExW(&windowClass) != 0)
            return true;
        const DWORD error = GetLastError();
        if (error == ERROR_CLASS_ALREADY_EXISTS)
            return true;
        SetError(FormatWin32Error(L"Register settings window class", error));
        return false;
    }

    [[nodiscard]] bool CreateHostWindow()
    {
        const UINT dpi = GetDpiForSystem();
        RECT bounds{0, 0,
            MulDiv(kDefaultClientWidth, static_cast<int>(dpi), 96),
            MulDiv(kDefaultClientHeight, static_cast<int>(dpi), 96)};
        AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPEDWINDOW, FALSE,
            WS_EX_APPWINDOW, dpi);
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        const int x = std::max(0, (GetSystemMetrics(SM_CXSCREEN) - width) / 2);
        const int y = std::max(0, (GetSystemMetrics(SM_CYSCREEN) - height) / 2);

        window = CreateWindowExW(WS_EX_APPWINDOW,
            kSettingsWindowClassName, options.windowTitle.c_str(),
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            x, y, width, height, nullptr, nullptr, instance, this);
        if (!window)
        {
            SetError(FormatWin32Error(
                L"Create settings window", GetLastError()));
            return false;
        }
        return true;
    }

    [[nodiscard]] bool HideWindow()
    {
        if (!controller || !window || shuttingDown)
            return false;
        ++viewEpoch;
        SuspendInteraction();
        const SettingsActionResult result = controller->CloseSession();
        if (!result.Succeeded())
        {
            --viewEpoch;
            ResumeInteraction();
            ShowActionError(result);
            return false;
        }
        if (widgetSettingsService)
            widgetSettingsService->CloseAll();
        ShowWindow(window, SW_HIDE);
        return true;
    }
};

SettingsWindowHost::SettingsWindowHost()
    : impl_(std::make_unique<Impl>())
{
}

SettingsWindowHost::~SettingsWindowHost()
{
    Shutdown();
}

bool SettingsWindowHost::Initialize(
    HINSTANCE instance,
    SettingsController& controller,
    widget_runtime::WidgetSettingsService* widgetSettingsService,
    SettingsWindowHostOptions options)
{
    if (impl_->initialized)
        return impl_->OnOwnerThread();
    if (!instance)
    {
        impl_->SetError(L"Settings window initialization requires HINSTANCE");
        return false;
    }

    impl_->ownerThreadId = GetCurrentThreadId();
    impl_->instance = instance;
    impl_->controller = &controller;
    impl_->widgetSettingsService = widgetSettingsService;
    impl_->options = std::move(options);
    impl_->lastError.clear();

    if (!impl_->runtime.Initialize() || !impl_->RegisterWindowClass() ||
        !impl_->CreateHostWindow())
    {
        if (impl_->lastError.empty())
            impl_->lastError = impl_->runtime.LastError();
        Shutdown();
        return false;
    }

    try
    {
        impl_->shell = winrt::make_self<shell_impl::SettingsShell>();
        impl_->callbacks =
            std::make_shared<Impl::CallbackState>();
        impl_->callbacks->owner = impl_.get();
        impl_->callbacks->dispatcher =
            mud::DispatcherQueue::GetForCurrentThread();
        if (!impl_->callbacks->dispatcher)
            winrt::throw_hresult(E_UNEXPECTED);

        const std::weak_ptr<Impl::CallbackState> weak = impl_->callbacks;
        impl_->shell->SetLocalizer([weak](std::string_view key) {
            const auto state = weak.lock();
            return state && state->alive.load() && state->owner
                ? state->owner->L(key)
                : std::wstring{};
        });
        impl_->shell->SetRouteRequestedCallback(
            [weak](const SettingsRoute& route) {
                if (const auto state = weak.lock();
                    state && state->alive.load() && state->owner)
                {
                    state->owner->RequestRoute(route);
                }
            });
        impl_->shell->SetSearchRequestedCallback(
            [weak](std::wstring query, std::uint64_t generation,
                   std::uint64_t requestId) {
                if (const auto state = weak.lock();
                    state && state->alive.load() && state->owner)
                {
                    state->owner->RequestSearch(
                        std::move(query), generation, requestId);
                }
            });
        impl_->shell->SetCancelOperationCallback([](std::uint64_t) {});
        impl_->shell->SetWidgetSettingsService(widgetSettingsService);
        impl_->ConfigurePageActions();
        impl_->RebuildSearchIndex();

        if (!impl_->runtime.Attach(impl_->window,
                impl_->shell.as<mux::UIElement>()))
        {
            impl_->SetError(impl_->runtime.LastError());
            Shutdown();
            return false;
        }

        controller.SetSnapshotChangedCallback(
            [weak](SettingsController::SnapshotPtr snapshot) {
                if (const auto state = weak.lock();
                    state && state->alive.load() && state->owner)
                {
                    state->owner->QueueSnapshot(std::move(snapshot));
                }
            });
        controller.SetPendingWorkCallback([weak]() {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->QueuePendingFlush();
            }
        });

        impl_->initialized = true;
        impl_->ApplySnapshotNow(controller.Snapshot());
        impl_->RefreshLocalizedPresentation();
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(
            L"Initialize WinUI settings shell (" +
            std::to_wstring(static_cast<unsigned int>(error.code().value)) +
            L")");
    }
    catch (...)
    {
        impl_->SetError(L"Initialize WinUI settings shell failed");
    }

    Shutdown();
    return false;
}

void SettingsWindowHost::Shutdown() noexcept
{
    if (!impl_ || impl_->shuttingDown)
        return;
    impl_->shuttingDown = true;

    if (impl_->shell)
        impl_->shell->SetWidgetSettingsService(nullptr);
    if (impl_->controller)
    {
        impl_->controller->SetSnapshotChangedCallback({});
        impl_->controller->SetPendingWorkCallback({});
        (void)impl_->controller->CloseSession();
    }
    if (impl_->widgetSettingsService)
        impl_->widgetSettingsService->CloseAll();

    if (impl_->callbacks)
    {
        impl_->callbacks->alive.store(false);
        impl_->callbacks->owner = nullptr;
        {
            std::lock_guard lock(impl_->callbacks->snapshotMutex);
            impl_->callbacks->latestSnapshot.reset();
        }
    }

    if (impl_->shell)
    {
        impl_->shell->Close();
        impl_->shell = nullptr;
    }
    impl_->runtime.Detach();
    if (impl_->window && IsWindow(impl_->window))
        DestroyWindow(impl_->window);
    impl_->window = nullptr;
    impl_->runtime.Shutdown();

    impl_->callbacks.reset();
    impl_->widgetSettingsService = nullptr;
    impl_->controller = nullptr;
    impl_->instance = nullptr;
    impl_->initialized = false;
    impl_->interactionSuspended = true;
    impl_->ownerThreadId = 0;
    impl_->shuttingDown = false;
}

bool SettingsWindowHost::Open(const SettingsRoute& route)
{
    if (!impl_->initialized || !impl_->OnOwnerThread() || !route.IsValid())
        return false;

    ++impl_->viewEpoch;
    const bool reopening = !impl_->Visible();
    SettingsActionResult reloadResult = SettingsActionResult::Success();
    if (reopening)
    {
        reloadResult = impl_->controller->Reload(
            SettingsReloadPolicy::PreservePendingChanges);
        if (impl_->options.refreshExternalState)
            impl_->options.refreshExternalState();
        impl_->RefreshLocalizedPresentation();
    }

    std::optional<widget_runtime::WidgetSettingsLoadResult> widgetLoad;
    if (route.page == SettingsPage::WidgetSettings &&
        impl_->widgetSettingsService)
    {
        widgetLoad = impl_->widgetSettingsService->Load(
            route.widgetInstanceId);
    }

    // This is the single authoritative route-open call for each host Open.
    const SettingsActionResult openResult = impl_->controller->Open(route);
    const auto snapshot = impl_->controller->Snapshot();
    if (!IsUsableControllerSnapshot(snapshot) ||
        !snapshot->sessionActive || snapshot->route != route)
    {
        impl_->ShowActionError(openResult);
        return false;
    }

    impl_->ApplySnapshotNow(snapshot);
    if (widgetLoad && widgetLoad->snapshot && impl_->shell)
        (void)impl_->shell->ApplyWidgetSettingsSnapshot(
            *widgetLoad->snapshot);
    impl_->ResumeInteraction();
    if (IsIconic(impl_->window))
        ShowWindow(impl_->window, SW_RESTORE);
    else
        ShowWindow(impl_->window, SW_SHOWNORMAL);
    SetForegroundWindow(impl_->window);
    SetActiveWindow(impl_->window);

    if (!reloadResult.Succeeded())
        impl_->ShowActionError(reloadResult);
    if (!openResult.Succeeded())
        impl_->ShowActionError(openResult);
    if (widgetLoad && !widgetLoad->Succeeded() && impl_->shell)
    {
        std::wstring message(
            widgetLoad->message.begin(), widgetLoad->message.end());
        if (message.empty())
            message = impl_->L("settings.widget.loadFailed");
        (void)impl_->shell->ShowInfoForGeneration(snapshot->generation,
            shell_impl::SettingsShellInfoSeverity::Error,
            impl_->L("settings.status.error"), std::move(message));
    }
    return true;
}

bool SettingsWindowHost::Hide()
{
    return impl_->initialized && impl_->OnOwnerThread() &&
        impl_->HideWindow();
}

void SettingsWindowHost::SetWidgetSettingsService(
    widget_runtime::WidgetSettingsService* service) noexcept
{
    if (impl_->shell)
        impl_->shell->SetWidgetSettingsService(service);
    impl_->widgetSettingsService = service;
}

void SettingsWindowHost::ApplyLanguageChange()
{
    if (impl_->initialized && impl_->OnOwnerThread())
        impl_->RefreshLocalizedPresentation();
}

bool SettingsWindowHost::PublishHomeAboutStatus(
    HomeAboutStatusPatch patch)
{
    return impl_->initialized && impl_->OnOwnerThread() && impl_->shell &&
        impl_->controller &&
        impl_->controller->IsGenerationCurrent(patch.generation) &&
        impl_->shell->ApplyHomeAboutStatusPatch(patch);
}

bool SettingsWindowHost::PreTranslateMessage(MSG* message) noexcept
{
    return impl_->initialized && impl_->OnOwnerThread() &&
        impl_->runtime.PreTranslateMessage(message);
}

bool SettingsWindowHost::ProcessTabNavigation(MSG* message) noexcept
{
    return impl_->initialized && impl_->OnOwnerThread() &&
        impl_->runtime.ProcessTabNavigation(message);
}

bool SettingsWindowHost::IsHotkeyCaptureActive() const noexcept
{
    return impl_->initialized && impl_->shell &&
        impl_->shell->IsHotkeyCaptureActive();
}

void SettingsWindowHost::CaptureRegisteredHotkey(
    UINT modifiers, UINT virtualKey)
{
    if (impl_->initialized && impl_->shell)
        impl_->shell->CaptureRegisteredHotkey(modifiers, virtualKey);
}

void SettingsWindowHost::ShowExitConfirmation(
    std::function<void(bool)> completed)
{
    if (!impl_->initialized || !impl_->shell || !impl_->controller)
    {
        if (completed)
            completed(false);
        return;
    }
    const auto snapshot = impl_->controller->Snapshot();
    if (!snapshot || !snapshot->sessionActive)
    {
        if (completed)
            completed(false);
        return;
    }
    shell_impl::SettingsShellDialogRequest request;
    request.generation = snapshot->generation;
    request.title = impl_->L("app.exit_confirm_title");
    request.message = impl_->L("app.exit_confirm_message");
    request.primaryButtonText = impl_->L("app.exit");
    request.closeButtonText = impl_->L("app.cancel");
    request.destructive = true;
    impl_->shell->ShowConfirmation(std::move(request), std::move(completed));
}

bool SettingsWindowHost::IsInitialized() const noexcept
{
    return impl_->initialized;
}

bool SettingsWindowHost::IsVisible() const noexcept
{
    return impl_->Visible();
}

HWND SettingsWindowHost::Window() const noexcept
{
    return impl_->window;
}

const std::wstring& SettingsWindowHost::LastError() const noexcept
{
    return impl_->lastError;
}

} // namespace snowdesktop::winui

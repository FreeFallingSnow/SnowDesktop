#include "system/status_bar/status_bar_settings.h"
#include "navigation/navigation_settings.h"
#include "settings/shell_extension_settings.h"
#include "diagnostics/diagnostic_log_modules.h"
#include "general_settings.h"
#include "category_settings.h"
#include "settings_controller.h"
#include "settings_snapshot.h"
#include "settings_store.h"
#include "dock/dock_settings.h"
#include "desktop/desktop_style_presets.h"

#include <array>
#include <stdexcept>
#include <utility>

namespace snowdesktop
{
namespace
{

constexpr std::array<SettingsDomain, 5> kPersistedDomains = {
    SettingsDomain::Personalization,
    SettingsDomain::Dock,
    SettingsDomain::Navigation,
    SettingsDomain::General,
    SettingsDomain::Category,
};

bool HasUpdateFlag(
    SettingsUpdateMode mode,
    SettingsUpdateMode flag) noexcept
{
    return (static_cast<std::uint8_t>(mode) &
            static_cast<std::uint8_t>(flag)) != 0;
}

void AppendMessage(std::wstring& destination, const std::wstring& message)
{
    if (message.empty()) return;
    if (!destination.empty()) destination += L"\n";
    destination += message;
}

void MergeResult(
    SettingsActionResult& destination,
    const SettingsActionResult& source)
{
    destination.completedDomains |= source.completedDomains;
    destination.failedDomains |= source.failedDomains;
    AppendMessage(destination.message, source.message);

    if (source.status == SettingsActionStatus::Failed)
        destination.status = SettingsActionStatus::Failed;
    else if (source.status == SettingsActionStatus::Busy &&
        destination.status == SettingsActionStatus::Succeeded)
        destination.status = SettingsActionStatus::Busy;
}

SettingsActionResult ExternalReplacementPendingResult()
{
    return SettingsActionResult::Busy(
        L"An external data replacement is pending application restart.");
}

bool IsTerminalHostAction(SettingsHostActions::Action action) noexcept
{
    return action == SettingsHostActions::Action::RestartApplication ||
        action == SettingsHostActions::Action::ExitApplication;
}

} // namespace

struct SettingsController::State
{
    State(std::shared_ptr<SettingsStore> store, SettingsHostActions* hostActions)
        : store_(std::move(store)), hostActions_(hostActions)
    {
    }

    std::shared_ptr<SettingsStore> store_;
    SettingsHostActions* hostActions_ = nullptr;
    SettingsValues values_;
    SettingsRoute route_;
    SettingsDomain dirtyDomains_ = SettingsDomain::None;
    SettingsDomain pendingPreviewDomains_ = SettingsDomain::None;
    SettingsDomain pendingCommitDomains_ = SettingsDomain::None;
    std::array<std::uint64_t, 6> domainRevisions_{};
    std::uint64_t systemTaskbarRevision_ = 0;
    std::uint64_t revision_ = 0;
    std::uint64_t generation_ = 0;
    bool initialized_ = false;
    bool sessionActive_ = false;
    bool flushing_ = false;
    bool retryRequired_ = false;
    bool externalReplacementPending_ = false;
    bool systemTaskbarAutoHideEdited_ = false;
    bool systemTaskbarAlignmentEdited_ = false;
    std::wstring lastActionMessage_;
    SnapshotPtr snapshot_;
    SnapshotChangedCallback snapshotChangedCallback_;
    PendingWorkCallback pendingWorkCallback_;
};

SettingsController::SettingsController(
    std::shared_ptr<SettingsStore> store,
    SettingsHostActions* hostActions)
    : state_(std::make_unique<State>(std::move(store), hostActions))
{
    if (!state_->store_)
        throw std::invalid_argument("SettingsController requires a store");
    PublishSnapshot();
}

SettingsController::~SettingsController() = default;

SettingsActionResult SettingsController::Initialize()
{
    if (state_->externalReplacementPending_)
        return ExternalReplacementPendingResult();
    if (state_->initialized_)
        return SettingsActionResult::Success();

    SettingsValues loaded;
    const SettingsActionResult loadResult = state_->store_->Load(loaded);
    if (!LoadProducedUsableSnapshot(loadResult))
        return loadResult;

    state_->values_ = std::move(loaded);
    diagnostics::SetLogModules(state_->values_.general.diagnosticLogModules);
    NormalizeDockSettings(state_->values_.dock);
    state_->dirtyDomains_ = SettingsDomain::None;
    state_->pendingPreviewDomains_ = SettingsDomain::None;
    state_->pendingCommitDomains_ = SettingsDomain::None;
    state_->retryRequired_ = false;
    state_->lastActionMessage_.clear();
    state_->initialized_ = true;
    ++state_->generation_;
    ++state_->revision_;
    state_->domainRevisions_.fill(state_->revision_);
    state_->systemTaskbarRevision_ = state_->revision_;
    state_->systemTaskbarAutoHideEdited_ = false;
    state_->systemTaskbarAlignmentEdited_ = false;
    PublishSnapshot();
    return loadResult;
}

SettingsActionResult SettingsController::Reload(SettingsReloadPolicy policy)
{
    if (state_->externalReplacementPending_)
        return ExternalReplacementPendingResult();
    if (!state_->initialized_)
        return Initialize();
    if (state_->dirtyDomains_ != SettingsDomain::None &&
        policy == SettingsReloadPolicy::PreservePendingChanges)
    {
        return SettingsActionResult::Busy(
            L"Settings have pending changes and were not reloaded.");
    }

    SettingsValues loaded;
    const SettingsActionResult loadResult = state_->store_->Load(loaded);
    if (!LoadProducedUsableSnapshot(loadResult))
        return loadResult;

    state_->values_ = std::move(loaded);
    diagnostics::SetLogModules(state_->values_.general.diagnosticLogModules);
    NormalizeDockSettings(state_->values_.dock);
    state_->dirtyDomains_ = SettingsDomain::None;
    state_->pendingPreviewDomains_ = SettingsDomain::None;
    state_->pendingCommitDomains_ = SettingsDomain::None;
    state_->retryRequired_ = false;
    state_->lastActionMessage_.clear();
    ++state_->generation_;
    ++state_->revision_;
    state_->domainRevisions_.fill(state_->revision_);
    state_->systemTaskbarRevision_ = state_->revision_;
    state_->systemTaskbarAutoHideEdited_ = false;
    state_->systemTaskbarAlignmentEdited_ = false;
    PublishSnapshot();
    return loadResult;
}

SettingsActionResult SettingsController::Open(SettingsRoute route)
{
    if (!route.IsValid())
    {
        return SettingsActionResult::Failure(
            L"The requested settings route is invalid.");
    }
    if (state_->externalReplacementPending_)
        return ExternalReplacementPendingResult();

    SettingsActionResult result = SettingsActionResult::Success();
    if (!state_->initialized_)
    {
        result = Initialize();
        if (!state_->initialized_)
            return result;
    }

    if ((route.page == SettingsPage::RuntimeModules || route.page == SettingsPage::AgentOperations) && !state_->values_.general.showAdvancedSettings)
        route = SettingsRoute::ForPage(SettingsPage::About, "about.showAdvancedSettings");
    const bool openingSession = !state_->sessionActive_;
    state_->sessionActive_ = true;
    if (openingSession)
        ++state_->generation_;
    state_->route_ = std::move(route);
    ++state_->revision_;
    PublishSnapshot();

    if (state_->hostActions_)
    {
        MergeResult(
            result,
            state_->hostActions_->OnSettingsRouteChanged(state_->route_));
    }
    return result;
}

SettingsActionResult SettingsController::CloseSession()
{
    if (state_->externalReplacementPending_)
    {
        if (state_->sessionActive_)
        {
            state_->sessionActive_ = false;
            ++state_->generation_;
            ++state_->revision_;
            PublishSnapshot();
        }
        return SettingsActionResult::Success();
    }

    SettingsActionResult result = FlushAll();
    if (!result.Succeeded() && HasPendingWork())
        return result;

    if (state_->sessionActive_)
    {
        state_->sessionActive_ = false;
        ++state_->generation_;
        ++state_->revision_;
        PublishSnapshot();
    }
    return result;
}

void SettingsController::SetHostActions(
    SettingsHostActions* hostActions) noexcept
{
    state_->hostActions_ = hostActions;
}

void SettingsController::SetSnapshotChangedCallback(
    SnapshotChangedCallback callback)
{
    state_->snapshotChangedCallback_ = std::move(callback);
    if (state_->snapshotChangedCallback_)
        state_->snapshotChangedCallback_(state_->snapshot_);
}

void SettingsController::SetPendingWorkCallback(PendingWorkCallback callback)
{
    state_->pendingWorkCallback_ = std::move(callback);
    if (state_->pendingWorkCallback_ && HasPendingWork() && !state_->flushing_)
        state_->pendingWorkCallback_();
}

SettingsController::SnapshotPtr SettingsController::Snapshot() const noexcept
{
    return state_->snapshot_;
}

std::uint64_t SettingsController::Generation() const noexcept
{
    return state_->generation_;
}

bool SettingsController::IsGenerationCurrent(
    std::uint64_t generation) const noexcept
{
    return state_->sessionActive_ && !state_->externalReplacementPending_ &&
        generation == state_->generation_;
}

void SettingsController::UpdatePersonalization(
    PersonalizationSettings settings,
    SettingsUpdateMode mode)
{
    if (state_->externalReplacementPending_) return;
    state_->values_.personalization = std::move(settings);
    MarkChanged(SettingsDomain::Personalization, mode);
}

void SettingsController::UpdateDock(
    DockSettings settings,
    SettingsUpdateMode mode)
{
    if (state_->externalReplacementPending_) return;
    NormalizeDockSettings(settings);
    state_->systemTaskbarAutoHideEdited_ = state_->systemTaskbarAutoHideEdited_ ||
        settings.systemTaskbarAutoHide !=
            state_->values_.dock.systemTaskbarAutoHide;
    state_->systemTaskbarAlignmentEdited_ = state_->systemTaskbarAlignmentEdited_ ||
        settings.systemTaskbarAlignment !=
            state_->values_.dock.systemTaskbarAlignment;
    state_->values_.dock = std::move(settings);
    MarkChanged(SettingsDomain::Dock, mode);
}

void SettingsController::UpdateNavigation(
    NavigationSettings settings,
    SettingsUpdateMode mode)
{
    if (state_->externalReplacementPending_) return;
    NormalizeNavigationSettings(settings);
    state_->values_.navigation = std::move(settings);
    MarkChanged(SettingsDomain::Navigation, mode);
}

void SettingsController::UpdateGeneral(
    GeneralSettings settings,
    SettingsUpdateMode mode)
{
    if (state_->externalReplacementPending_) return;
    NormalizeGeneralAnimationSettings(settings);
    settings.desktopIdleHideDelaySeconds = desktop_idle_hide::NormalizeDelay(settings.desktopIdleHideDelaySeconds);
    NormalizeStatusBarSettings(settings.statusBar);
    calendar::Normalize(settings.calendarDisplay);
    shell_extensions::Normalize(settings.shellExtensions);
    settings.diagnosticLogModules = diagnostics::NormalizeLogModules(settings.diagnosticLogModules);
    diagnostics::SetLogModules(settings.diagnosticLogModules);
    state_->values_.general = std::move(settings);
    if (!state_->values_.general.showAdvancedSettings && (state_->route_.page == SettingsPage::RuntimeModules || state_->route_.page == SettingsPage::AgentOperations))
        state_->route_ = SettingsRoute::ForPage(SettingsPage::About, "about.showAdvancedSettings");
    MarkChanged(SettingsDomain::General, mode);
}

void SettingsController::UpdateCategory(
    CategorySettings settings,
    SettingsUpdateMode mode)
{
    if (state_->externalReplacementPending_) return;
    // Category text boxes are intentionally draft buffers.  Normalizing on
    // every keystroke rewrites the text under the caret; the legacy settings
    // page normalized only when Apply (or window close) committed the draft.
    if (HasUpdateFlag(mode, SettingsUpdateMode::Commit))
        NormalizeCategorySettings(settings);
    state_->values_.category = std::move(settings);
    MarkChanged(SettingsDomain::Category, mode);
}

void SettingsController::UpdateDesktop(
    DesktopDisplaySettings settings,
    SettingsUpdateMode mode)
{
    if (state_->externalReplacementPending_) return;
    state_->values_.desktop = std::move(settings);
    MarkChanged(SettingsDomain::Desktop, mode);
}

void SettingsController::RequestCommit(SettingsDomain domains)
{
    if (state_->externalReplacementPending_) return;
    const SettingsDomain requested = domains & state_->dirtyDomains_ &
        SettingsDomain::All;
    if (requested == SettingsDomain::None) return;

    const bool previouslyPending = HasPendingWork();
    if (HasSettingsDomain(requested, SettingsDomain::Category))
        NormalizeCategorySettings(state_->values_.category);
    state_->pendingCommitDomains_ |= requested;
    ++state_->revision_;
    if (HasSettingsDomain(requested, SettingsDomain::Category))
    {
        state_->domainRevisions_[DomainIndex(SettingsDomain::Category)] = state_->revision_;
    }
    PublishSnapshot();
    SchedulePendingWorkIfNeeded(previouslyPending);
}

SettingsActionResult SettingsController::FlushPending()
{
    if (state_->externalReplacementPending_)
        return SettingsActionResult::Success();
    if (state_->flushing_)
    {
        return SettingsActionResult::Busy(
            L"Settings work is already being flushed.");
    }

    SettingsActionResult result = SettingsActionResult::Success();
    bool scheduleAgain = false;
    {
        struct FlushingGuard
        {
            bool& flushing;
            explicit FlushingGuard(bool& value) : flushing(value)
            {
                flushing = true;
            }
            ~FlushingGuard() { flushing = false; }
        } guard(state_->flushing_);

        SettingsDomain previewBlocked = SettingsDomain::None;
        const SettingsDomain previewDomains = state_->pendingPreviewDomains_;
        if (previewDomains != SettingsDomain::None)
        {
            state_->pendingPreviewDomains_ &= ~previewDomains;
            ++state_->revision_;
            PublishSnapshot();
            if (state_->hostActions_)
            {
                SettingsActionResult previewResult =
                    state_->hostActions_->OnSettingsPreview(
                        *state_->snapshot_, previewDomains);
                SettingsDomain completed =
                    previewResult.completedDomains & previewDomains;
                if (previewResult.Succeeded() &&
                    completed == SettingsDomain::None)
                {
                    completed = previewDomains;
                }
                state_->pendingPreviewDomains_ |= previewDomains & ~completed;
                previewBlocked = previewDomains & ~completed;
                MergeResult(result, previewResult);
            }
        }

        const SettingsDomain commitDomains = state_->pendingCommitDomains_;
        const SettingsDomain hostRequested =
            commitDomains & ~previewBlocked;
        const SettingsValues committedValues = state_->values_;
        const auto committedDomainRevisions = state_->domainRevisions_;
        const std::uint64_t committedSystemTaskbarRevision =
            state_->systemTaskbarRevision_;
        SettingsDomain hostCompleted = hostRequested;
        if (hostRequested != SettingsDomain::None && state_->hostActions_)
        {
            SettingsActionResult hostResult =
                state_->hostActions_->OnSettingsCommitted(
                    *state_->snapshot_, hostRequested);
            hostCompleted =
                hostResult.completedDomains & hostRequested;
            if (hostResult.Succeeded() &&
                hostCompleted == SettingsDomain::None)
            {
                hostCompleted = hostRequested;
            }
            MergeResult(result, hostResult);
        }
        else if (HasSettingsDomain(hostRequested, SettingsDomain::Desktop))
        {
            hostCompleted &= ~SettingsDomain::Desktop;
            MergeResult(
                result,
                SettingsActionResult::Failure(
                    L"Desktop layout changes require host actions.",
                    SettingsDomain::Desktop));
        }

        bool stateChanged = false;
        for (SettingsDomain domain : kPersistedDomains)
        {
            if (state_->externalReplacementPending_) break;
            if (!HasSettingsDomain(commitDomains, domain) ||
                !HasSettingsDomain(hostCompleted, domain))
            {
                continue;
            }

            SettingsActionResult saveResult =
                SaveDomain(domain, committedValues);
            const bool revisionStillCurrent =
                state_->domainRevisions_[DomainIndex(domain)] ==
                    committedDomainRevisions[DomainIndex(domain)] &&
                (domain != SettingsDomain::Dock ||
                    state_->systemTaskbarRevision_ ==
                        committedSystemTaskbarRevision);
            if (saveResult.Succeeded() && revisionStillCurrent)
            {
                state_->pendingCommitDomains_ &= ~domain;
                state_->dirtyDomains_ &= ~domain;
                if (domain == SettingsDomain::Dock)
                {
                    state_->systemTaskbarAutoHideEdited_ = false;
                    state_->systemTaskbarAlignmentEdited_ = false;
                }
            }
            stateChanged = true;
            MergeResult(result, saveResult);
        }

        if (!state_->externalReplacementPending_ &&
            HasSettingsDomain(commitDomains, SettingsDomain::Desktop) &&
            HasSettingsDomain(hostCompleted, SettingsDomain::Desktop) &&
            state_->domainRevisions_[DomainIndex(SettingsDomain::Desktop)] ==
                committedDomainRevisions[
                    DomainIndex(SettingsDomain::Desktop)])
        {
            state_->pendingCommitDomains_ &= ~SettingsDomain::Desktop;
            state_->dirtyDomains_ &= ~SettingsDomain::Desktop;
            stateChanged = true;
        }

        const bool previousRetryRequired = state_->retryRequired_;
        const std::wstring previousMessage = state_->lastActionMessage_;
        state_->retryRequired_ = !result.Succeeded() && HasPendingWork();
        state_->lastActionMessage_ = result.Succeeded()
            ? std::wstring{}
            : result.message;
        stateChanged = stateChanged ||
            previousRetryRequired != state_->retryRequired_ ||
            previousMessage != state_->lastActionMessage_;
        if (stateChanged)
        {
            ++state_->revision_;
            PublishSnapshot();
        }
        scheduleAgain = result.Succeeded() && HasPendingWork();
    }

    if (scheduleAgain && state_->pendingWorkCallback_)
        state_->pendingWorkCallback_();
    return result;
}

SettingsActionResult SettingsController::FlushAll()
{
    if (state_->externalReplacementPending_)
        return SettingsActionResult::Success();
    RequestCommit(state_->dirtyDomains_);
    return FlushPending();
}

bool SettingsController::RetryPending()
{
    if (state_->externalReplacementPending_) return false;
    if (!HasPendingWork()) return false;
    state_->retryRequired_ = false;
    state_->lastActionMessage_.clear();
    ++state_->revision_;
    PublishSnapshot();
    if (state_->pendingWorkCallback_ && !state_->flushing_)
        state_->pendingWorkCallback_();
    return true;
}

void SettingsController::PrepareForExternalDataReplacement()
{
    if (state_->externalReplacementPending_) return;

    state_->dirtyDomains_ = SettingsDomain::None;
    state_->pendingPreviewDomains_ = SettingsDomain::None;
    state_->pendingCommitDomains_ = SettingsDomain::None;
    state_->retryRequired_ = false;
    state_->externalReplacementPending_ = true;
    state_->lastActionMessage_.clear();
    ++state_->generation_;
    ++state_->revision_;
    state_->domainRevisions_.fill(state_->revision_);
    state_->systemTaskbarRevision_ = state_->revision_;
    state_->systemTaskbarAutoHideEdited_ = false;
    state_->systemTaskbarAlignmentEdited_ = false;
    PublishSnapshot();
}

SettingsActionResult SettingsController::InvokeHostAction(
    const SettingsHostActions::Request& request)
{
    if (state_->externalReplacementPending_ &&
        !IsTerminalHostAction(request.action))
    {
        return ExternalReplacementPendingResult();
    }
    if (request.action == SettingsHostActions::Action::ApplyDesktopStylePreset ||
        request.action == SettingsHostActions::Action::ApplyDesktopStyleAnimations)
    {
        auto general = state_->values_.general;
        auto dock = state_->values_.dock;
        const bool layout = request.action == SettingsHostActions::Action::ApplyDesktopStylePreset;
        const bool valid = layout ? ApplyDesktopStylePreset(request.value, general, dock,
            request.desktopStyleDockPosition, request.desktopStyleDockAttached)
            : ApplyDesktopStyleAnimations(request.value, dock);
        if (!valid) return SettingsActionResult::Failure(L"Unknown desktop style preset.");
        if (layout && state_->values_.general.dockEnabled && !general.dockEnabled && !request.boolValue)
            return SettingsActionResult::Busy(L"Confirm before disabling Dock.");
        state_->systemTaskbarAutoHideEdited_ = state_->systemTaskbarAutoHideEdited_ ||
            dock.systemTaskbarAutoHide != state_->values_.dock.systemTaskbarAutoHide;
        state_->values_.general = std::move(general);
        state_->values_.dock = std::move(dock);
        const auto domains = layout ? SettingsDomain::General | SettingsDomain::Dock : SettingsDomain::Dock;
        // One publication and one commit, without an intermediate mixed layout.
        state_->pendingPreviewDomains_ &= ~domains;
        MarkChanged(domains, SettingsUpdateMode::Commit);
        return SettingsActionResult::Success(domains);
    }
    if (!state_->hostActions_)
    {
        return SettingsActionResult::Failure(
            L"The settings host action is unavailable.");
    }
    return state_->hostActions_->Invoke(request);
}

bool SettingsController::SynchronizePersonalization(
    PersonalizationSettings settings)
{
    return SynchronizeDomain(
        SettingsDomain::Personalization,
        [this, settings = std::move(settings)]() mutable {
            state_->values_.personalization = std::move(settings);
        });
}

bool SettingsController::SynchronizeDock(DockSettings settings)
{
    NormalizeDockSettings(settings);
    return SynchronizeDomain(
        SettingsDomain::Dock,
        [this, settings = std::move(settings)]() mutable {
            state_->values_.dock = std::move(settings);
        });
}

bool SettingsController::SynchronizeSystemTaskbarState(
    bool autoHide,
    bool alignmentCentered)
{
    if (state_->externalReplacementPending_)
        return false;

    const int alignment = alignmentCentered ? 1 : 0;
    bool changed = false;
    if (!state_->systemTaskbarAutoHideEdited_ &&
        state_->values_.dock.systemTaskbarAutoHide != autoHide)
    {
        state_->values_.dock.systemTaskbarAutoHide = autoHide;
        changed = true;
    }
    if (!state_->systemTaskbarAlignmentEdited_ &&
        state_->values_.dock.systemTaskbarAlignment != alignment)
    {
        state_->values_.dock.systemTaskbarAlignment = alignment;
        changed = true;
    }
    if (!changed)
        return true;

    ++state_->revision_;
    state_->systemTaskbarRevision_ = state_->revision_;
    PublishSnapshot();
    return true;
}

bool SettingsController::SynchronizeNavigation(NavigationSettings settings)
{
    return SynchronizeDomain(
        SettingsDomain::Navigation,
        [this, settings = std::move(settings)]() mutable {
            state_->values_.navigation = std::move(settings);
        });
}

bool SettingsController::SynchronizeGeneral(GeneralSettings settings)
{
    return SynchronizeDomain(
        SettingsDomain::General,
        [this, settings = std::move(settings)]() mutable {
            settings.diagnosticLogModules = diagnostics::NormalizeLogModules(settings.diagnosticLogModules);
            diagnostics::SetLogModules(settings.diagnosticLogModules);
            state_->values_.general = std::move(settings);
        });
}

bool SettingsController::SynchronizeCategory(CategorySettings settings)
{
    NormalizeCategorySettings(settings);
    return SynchronizeDomain(
        SettingsDomain::Category,
        [this, settings = std::move(settings)]() mutable {
            state_->values_.category = std::move(settings);
        });
}

bool SettingsController::SynchronizeDesktop(
    DesktopDisplaySettings settings)
{
    return SynchronizeDomain(
        SettingsDomain::Desktop,
        [this, settings = std::move(settings)]() mutable {
            state_->values_.desktop = std::move(settings);
        });
}

bool SettingsController::LoadProducedUsableSnapshot(
    const SettingsActionResult& result) noexcept
{
    return result.Succeeded() ||
        (result.completedDomains & SettingsDomain::NativeStored) ==
            SettingsDomain::NativeStored;
}

void SettingsController::MarkChanged(
    SettingsDomain domain,
    SettingsUpdateMode mode)
{
    const bool previouslyPending = HasPendingWork();
    state_->dirtyDomains_ |= domain;
    if (HasUpdateFlag(mode, SettingsUpdateMode::Preview))
        state_->pendingPreviewDomains_ |= domain;
    if (HasUpdateFlag(mode, SettingsUpdateMode::Commit))
        state_->pendingCommitDomains_ |= domain;
    ++state_->revision_;
    for (auto part : {SettingsDomain::Personalization, SettingsDomain::Dock,
        SettingsDomain::Navigation, SettingsDomain::General, SettingsDomain::Category, SettingsDomain::Desktop})
        if (HasSettingsDomain(domain, part)) state_->domainRevisions_[DomainIndex(part)] = state_->revision_;
    if (HasSettingsDomain(domain, SettingsDomain::Dock))
        state_->systemTaskbarRevision_ = state_->revision_;
    PublishSnapshot();
    SchedulePendingWorkIfNeeded(previouslyPending);
}

void SettingsController::PublishSnapshot()
{
    auto snapshot = std::make_shared<SettingsSnapshot>();
    snapshot->revision = state_->revision_;
    snapshot->generation = state_->generation_;
    snapshot->initialized = state_->initialized_;
    snapshot->sessionActive = state_->sessionActive_;
    snapshot->route = state_->route_;
    snapshot->values = state_->values_;
    snapshot->domainRevisions.personalization =
        state_->domainRevisions_[DomainIndex(SettingsDomain::Personalization)];
    snapshot->domainRevisions.dock =
        state_->domainRevisions_[DomainIndex(SettingsDomain::Dock)];
    snapshot->domainRevisions.systemTaskbar = state_->systemTaskbarRevision_;
    snapshot->domainRevisions.navigation =
        state_->domainRevisions_[DomainIndex(SettingsDomain::Navigation)];
    snapshot->domainRevisions.general =
        state_->domainRevisions_[DomainIndex(SettingsDomain::General)];
    snapshot->domainRevisions.category =
        state_->domainRevisions_[DomainIndex(SettingsDomain::Category)];
    snapshot->domainRevisions.desktop =
        state_->domainRevisions_[DomainIndex(SettingsDomain::Desktop)];
    snapshot->dirtyDomains = state_->dirtyDomains_;
    snapshot->pendingPreviewDomains = state_->pendingPreviewDomains_;
    snapshot->pendingCommitDomains = state_->pendingCommitDomains_;
    snapshot->retryRequired = state_->retryRequired_;
    snapshot->externalReplacementPending = state_->externalReplacementPending_;
    snapshot->lastActionMessage = state_->lastActionMessage_;
    state_->snapshot_ = std::move(snapshot);

    if (state_->snapshotChangedCallback_)
        state_->snapshotChangedCallback_(state_->snapshot_);
}

void SettingsController::SchedulePendingWorkIfNeeded(bool previouslyPending)
{
    if (!previouslyPending && HasPendingWork() &&
        state_->pendingWorkCallback_ && !state_->flushing_)
    {
        state_->pendingWorkCallback_();
    }
}

bool SettingsController::HasPendingWork() const noexcept
{
    return state_->pendingPreviewDomains_ != SettingsDomain::None ||
        state_->pendingCommitDomains_ != SettingsDomain::None;
}

SettingsActionResult SettingsController::SaveDomain(
    SettingsDomain domain,
    const SettingsValues& values)
{
    switch (domain)
    {
    case SettingsDomain::Personalization:
        return state_->store_->SavePersonalization(values.personalization);
    case SettingsDomain::Dock:
        return state_->store_->SaveDock(values.dock);
    case SettingsDomain::Navigation:
        return state_->store_->SaveNavigation(values.navigation);
    case SettingsDomain::General:
        return state_->store_->SaveGeneral(values.general);
    case SettingsDomain::Category:
        return state_->store_->SaveCategory(values.category);
    default:
        return SettingsActionResult::Failure(
            L"The requested settings domain cannot be persisted.", domain);
    }
}

std::size_t SettingsController::DomainIndex(
    SettingsDomain domain) noexcept
{
    switch (domain)
    {
    case SettingsDomain::Personalization: return 0;
    case SettingsDomain::Dock: return 1;
    case SettingsDomain::Navigation: return 2;
    case SettingsDomain::General: return 3;
    case SettingsDomain::Category: return 4;
    case SettingsDomain::Desktop: return 5;
    default: return 0;
    }
}

bool SettingsController::SynchronizeDomain(
    SettingsDomain domain,
    const std::function<void()>& assign)
{
    if (state_->externalReplacementPending_) return false;
    if (HasSettingsDomain(state_->dirtyDomains_, domain)) return false;
    assign();
    ++state_->revision_;
    state_->domainRevisions_[DomainIndex(domain)] = state_->revision_;
    if (domain == SettingsDomain::Dock)
        state_->systemTaskbarRevision_ = state_->revision_;
    PublishSnapshot();
    return true;
}

} // namespace snowdesktop

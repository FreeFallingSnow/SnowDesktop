#include "settings_controller.h"

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

} // namespace

SettingsActionResult SettingsActionResult::Success(SettingsDomain completed)
{
    SettingsActionResult result;
    result.completedDomains = completed;
    return result;
}

SettingsActionResult SettingsActionResult::Busy(std::wstring message)
{
    SettingsActionResult result;
    result.status = SettingsActionStatus::Busy;
    result.message = std::move(message);
    return result;
}

SettingsActionResult SettingsActionResult::Failure(
    std::wstring message,
    SettingsDomain failed)
{
    SettingsActionResult result;
    result.status = SettingsActionStatus::Failed;
    result.failedDomains = failed;
    result.message = std::move(message);
    return result;
}

SettingsController::SettingsController(
    std::shared_ptr<SettingsStore> store,
    SettingsHostActions* hostActions)
    : store_(std::move(store)),
      hostActions_(hostActions)
{
    if (!store_)
        throw std::invalid_argument("SettingsController requires a store");
    PublishSnapshot();
}

SettingsActionResult SettingsController::Initialize()
{
    if (initialized_)
        return SettingsActionResult::Success();

    SettingsValues loaded;
    const SettingsActionResult loadResult = store_->Load(loaded);
    if (!LoadProducedUsableSnapshot(loadResult))
        return loadResult;

    values_ = std::move(loaded);
    NormalizeDockSettings(values_.dock);
    dirtyDomains_ = SettingsDomain::None;
    pendingPreviewDomains_ = SettingsDomain::None;
    pendingCommitDomains_ = SettingsDomain::None;
    retryRequired_ = false;
    lastActionMessage_.clear();
    initialized_ = true;
    ++generation_;
    ++revision_;
    domainRevisions_.fill(revision_);
    PublishSnapshot();
    return loadResult;
}

SettingsActionResult SettingsController::Reload(SettingsReloadPolicy policy)
{
    if (!initialized_)
        return Initialize();
    if (dirtyDomains_ != SettingsDomain::None &&
        policy == SettingsReloadPolicy::PreservePendingChanges)
    {
        return SettingsActionResult::Busy(
            L"Settings have pending changes and were not reloaded.");
    }

    SettingsValues loaded;
    const SettingsActionResult loadResult = store_->Load(loaded);
    if (!LoadProducedUsableSnapshot(loadResult))
        return loadResult;

    values_ = std::move(loaded);
    NormalizeDockSettings(values_.dock);
    dirtyDomains_ = SettingsDomain::None;
    pendingPreviewDomains_ = SettingsDomain::None;
    pendingCommitDomains_ = SettingsDomain::None;
    retryRequired_ = false;
    lastActionMessage_.clear();
    ++generation_;
    ++revision_;
    domainRevisions_.fill(revision_);
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

    SettingsActionResult result = SettingsActionResult::Success();
    if (!initialized_)
    {
        result = Initialize();
        if (!initialized_)
            return result;
    }

    const bool openingSession = !sessionActive_;
    sessionActive_ = true;
    if (openingSession)
        ++generation_;
    route_ = std::move(route);
    ++revision_;
    PublishSnapshot();

    if (hostActions_)
    {
        MergeResult(
            result,
            hostActions_->OnSettingsRouteChanged(route_));
    }
    return result;
}

SettingsActionResult SettingsController::CloseSession()
{
    SettingsActionResult result = FlushAll();
    if (!result.Succeeded() && HasPendingWork())
        return result;

    if (sessionActive_)
    {
        sessionActive_ = false;
        ++generation_;
        ++revision_;
        PublishSnapshot();
    }
    return result;
}

void SettingsController::SetHostActions(
    SettingsHostActions* hostActions) noexcept
{
    hostActions_ = hostActions;
}

void SettingsController::SetSnapshotChangedCallback(
    SnapshotChangedCallback callback)
{
    snapshotChangedCallback_ = std::move(callback);
    if (snapshotChangedCallback_)
        snapshotChangedCallback_(snapshot_);
}

void SettingsController::SetPendingWorkCallback(PendingWorkCallback callback)
{
    pendingWorkCallback_ = std::move(callback);
    if (pendingWorkCallback_ && HasPendingWork() && !flushing_)
        pendingWorkCallback_();
}

SettingsController::SnapshotPtr SettingsController::Snapshot() const noexcept
{
    return snapshot_;
}

std::uint64_t SettingsController::Generation() const noexcept
{
    return generation_;
}

bool SettingsController::IsGenerationCurrent(
    std::uint64_t generation) const noexcept
{
    return sessionActive_ && generation == generation_;
}

void SettingsController::UpdatePersonalization(
    PersonalizationSettings settings,
    SettingsUpdateMode mode)
{
    values_.personalization = std::move(settings);
    MarkChanged(SettingsDomain::Personalization, mode);
}

void SettingsController::UpdateDock(
    DockSettings settings,
    SettingsUpdateMode mode)
{
    NormalizeDockSettings(settings);
    values_.dock = std::move(settings);
    MarkChanged(SettingsDomain::Dock, mode);
}

void SettingsController::UpdateNavigation(
    NavigationSettings settings,
    SettingsUpdateMode mode)
{
    values_.navigation = std::move(settings);
    MarkChanged(SettingsDomain::Navigation, mode);
}

void SettingsController::UpdateGeneral(
    GeneralSettings settings,
    SettingsUpdateMode mode)
{
    values_.general = std::move(settings);
    MarkChanged(SettingsDomain::General, mode);
}

void SettingsController::UpdateCategory(
    CategorySettings settings,
    SettingsUpdateMode mode)
{
    NormalizeCategorySettings(settings);
    values_.category = std::move(settings);
    MarkChanged(SettingsDomain::Category, mode);
}

void SettingsController::UpdateDesktop(
    DesktopDisplaySettings settings,
    SettingsUpdateMode mode)
{
    values_.desktop = std::move(settings);
    MarkChanged(SettingsDomain::Desktop, mode);
}

void SettingsController::RequestCommit(SettingsDomain domains)
{
    const SettingsDomain requested = domains & dirtyDomains_ &
        SettingsDomain::All;
    if (requested == SettingsDomain::None) return;

    const bool previouslyPending = HasPendingWork();
    pendingCommitDomains_ |= requested;
    ++revision_;
    PublishSnapshot();
    SchedulePendingWorkIfNeeded(previouslyPending);
}

SettingsActionResult SettingsController::FlushPending()
{
    if (flushing_)
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
        } guard(flushing_);

        SettingsDomain previewBlocked = SettingsDomain::None;
        const SettingsDomain previewDomains = pendingPreviewDomains_;
        if (previewDomains != SettingsDomain::None)
        {
            pendingPreviewDomains_ &= ~previewDomains;
            ++revision_;
            PublishSnapshot();
            if (hostActions_)
            {
                SettingsActionResult previewResult =
                    hostActions_->OnSettingsPreview(
                        *snapshot_, previewDomains);
                SettingsDomain completed =
                    previewResult.completedDomains & previewDomains;
                if (previewResult.Succeeded() &&
                    completed == SettingsDomain::None)
                {
                    completed = previewDomains;
                }
                pendingPreviewDomains_ |= previewDomains & ~completed;
                previewBlocked = previewDomains & ~completed;
                MergeResult(result, previewResult);
            }
        }

        const SettingsDomain commitDomains = pendingCommitDomains_;
        const SettingsDomain hostRequested =
            commitDomains & ~previewBlocked;
        const SettingsValues committedValues = values_;
        const auto committedDomainRevisions = domainRevisions_;
        SettingsDomain hostCompleted = hostRequested;
        if (hostRequested != SettingsDomain::None && hostActions_)
        {
            SettingsActionResult hostResult =
                hostActions_->OnSettingsCommitted(
                    *snapshot_, hostRequested);
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
            if (!HasSettingsDomain(commitDomains, domain) ||
                !HasSettingsDomain(hostCompleted, domain))
            {
                continue;
            }

            SettingsActionResult saveResult =
                SaveDomain(domain, committedValues);
            if (saveResult.Succeeded() &&
                domainRevisions_[DomainIndex(domain)] ==
                    committedDomainRevisions[DomainIndex(domain)])
            {
                pendingCommitDomains_ &= ~domain;
                dirtyDomains_ &= ~domain;
            }
            stateChanged = true;
            MergeResult(result, saveResult);
        }

        if (HasSettingsDomain(commitDomains, SettingsDomain::Desktop) &&
            HasSettingsDomain(hostCompleted, SettingsDomain::Desktop) &&
            domainRevisions_[DomainIndex(SettingsDomain::Desktop)] ==
                committedDomainRevisions[
                    DomainIndex(SettingsDomain::Desktop)])
        {
            pendingCommitDomains_ &= ~SettingsDomain::Desktop;
            dirtyDomains_ &= ~SettingsDomain::Desktop;
            stateChanged = true;
        }

        const bool previousRetryRequired = retryRequired_;
        const std::wstring previousMessage = lastActionMessage_;
        retryRequired_ = !result.Succeeded() && HasPendingWork();
        lastActionMessage_ = result.Succeeded()
            ? std::wstring{}
            : result.message;
        stateChanged = stateChanged ||
            previousRetryRequired != retryRequired_ ||
            previousMessage != lastActionMessage_;
        if (stateChanged)
        {
            ++revision_;
            PublishSnapshot();
        }
        scheduleAgain = result.Succeeded() && HasPendingWork();
    }

    if (scheduleAgain && pendingWorkCallback_)
        pendingWorkCallback_();
    return result;
}

SettingsActionResult SettingsController::FlushAll()
{
    RequestCommit(dirtyDomains_);
    return FlushPending();
}

bool SettingsController::RetryPending()
{
    if (!HasPendingWork()) return false;
    retryRequired_ = false;
    lastActionMessage_.clear();
    ++revision_;
    PublishSnapshot();
    if (pendingWorkCallback_ && !flushing_)
        pendingWorkCallback_();
    return true;
}

void SettingsController::PrepareForExternalDataReplacement()
{
    dirtyDomains_ = SettingsDomain::None;
    pendingPreviewDomains_ = SettingsDomain::None;
    pendingCommitDomains_ = SettingsDomain::None;
    retryRequired_ = false;
    lastActionMessage_.clear();
    ++generation_;
    ++revision_;
    domainRevisions_.fill(revision_);
    PublishSnapshot();
}

SettingsActionResult SettingsController::InvokeHostAction(
    const SettingsHostActions::Request& request)
{
    if (!hostActions_)
    {
        return SettingsActionResult::Failure(
            L"The settings host action is unavailable.");
    }
    return hostActions_->Invoke(request);
}

bool SettingsController::SynchronizePersonalization(
    PersonalizationSettings settings)
{
    return SynchronizeDomain(
        SettingsDomain::Personalization,
        [this, settings = std::move(settings)]() mutable {
            values_.personalization = std::move(settings);
        });
}

bool SettingsController::SynchronizeDock(DockSettings settings)
{
    NormalizeDockSettings(settings);
    return SynchronizeDomain(
        SettingsDomain::Dock,
        [this, settings = std::move(settings)]() mutable {
            values_.dock = std::move(settings);
        });
}

bool SettingsController::SynchronizeNavigation(NavigationSettings settings)
{
    return SynchronizeDomain(
        SettingsDomain::Navigation,
        [this, settings = std::move(settings)]() mutable {
            values_.navigation = std::move(settings);
        });
}

bool SettingsController::SynchronizeGeneral(GeneralSettings settings)
{
    return SynchronizeDomain(
        SettingsDomain::General,
        [this, settings = std::move(settings)]() mutable {
            values_.general = std::move(settings);
        });
}

bool SettingsController::SynchronizeCategory(CategorySettings settings)
{
    NormalizeCategorySettings(settings);
    return SynchronizeDomain(
        SettingsDomain::Category,
        [this, settings = std::move(settings)]() mutable {
            values_.category = std::move(settings);
        });
}

bool SettingsController::SynchronizeDesktop(
    DesktopDisplaySettings settings)
{
    return SynchronizeDomain(
        SettingsDomain::Desktop,
        [this, settings = std::move(settings)]() mutable {
            values_.desktop = std::move(settings);
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
    dirtyDomains_ |= domain;
    if (HasUpdateFlag(mode, SettingsUpdateMode::Preview))
        pendingPreviewDomains_ |= domain;
    if (HasUpdateFlag(mode, SettingsUpdateMode::Commit))
        pendingCommitDomains_ |= domain;
    ++revision_;
    domainRevisions_[DomainIndex(domain)] = revision_;
    PublishSnapshot();
    SchedulePendingWorkIfNeeded(previouslyPending);
}

void SettingsController::PublishSnapshot()
{
    auto snapshot = std::make_shared<SettingsSnapshot>();
    snapshot->revision = revision_;
    snapshot->generation = generation_;
    snapshot->initialized = initialized_;
    snapshot->sessionActive = sessionActive_;
    snapshot->route = route_;
    snapshot->values = values_;
    snapshot->domainRevisions.personalization =
        domainRevisions_[DomainIndex(SettingsDomain::Personalization)];
    snapshot->domainRevisions.dock =
        domainRevisions_[DomainIndex(SettingsDomain::Dock)];
    snapshot->domainRevisions.navigation =
        domainRevisions_[DomainIndex(SettingsDomain::Navigation)];
    snapshot->domainRevisions.general =
        domainRevisions_[DomainIndex(SettingsDomain::General)];
    snapshot->domainRevisions.category =
        domainRevisions_[DomainIndex(SettingsDomain::Category)];
    snapshot->domainRevisions.desktop =
        domainRevisions_[DomainIndex(SettingsDomain::Desktop)];
    snapshot->dirtyDomains = dirtyDomains_;
    snapshot->pendingPreviewDomains = pendingPreviewDomains_;
    snapshot->pendingCommitDomains = pendingCommitDomains_;
    snapshot->retryRequired = retryRequired_;
    snapshot->lastActionMessage = lastActionMessage_;
    snapshot_ = std::move(snapshot);

    if (snapshotChangedCallback_)
        snapshotChangedCallback_(snapshot_);
}

void SettingsController::SchedulePendingWorkIfNeeded(bool previouslyPending)
{
    if (!previouslyPending && HasPendingWork() &&
        pendingWorkCallback_ && !flushing_)
    {
        pendingWorkCallback_();
    }
}

bool SettingsController::HasPendingWork() const noexcept
{
    return pendingPreviewDomains_ != SettingsDomain::None ||
        pendingCommitDomains_ != SettingsDomain::None;
}

SettingsActionResult SettingsController::SaveDomain(
    SettingsDomain domain,
    const SettingsValues& values)
{
    switch (domain)
    {
    case SettingsDomain::Personalization:
        return store_->SavePersonalization(values.personalization);
    case SettingsDomain::Dock:
        return store_->SaveDock(values.dock);
    case SettingsDomain::Navigation:
        return store_->SaveNavigation(values.navigation);
    case SettingsDomain::General:
        return store_->SaveGeneral(values.general);
    case SettingsDomain::Category:
        return store_->SaveCategory(values.category);
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
    if (HasSettingsDomain(dirtyDomains_, domain)) return false;
    assign();
    ++revision_;
    domainRevisions_[DomainIndex(domain)] = revision_;
    PublishSnapshot();
    return true;
}

} // namespace snowdesktop

#pragma once

#include "global_drag_ole_driver.inc"
#include "global_drag_mixed_source.inc"

namespace behavior { struct V4VirtualCopyObservation; struct V4VirtualBusinessEvidence; }

namespace drag_global::ole_driver
{
// The receipt is produced only by the versioned executor and contains actual
// return values and provider records, not a format-selection prediction.
class V4MixedOleRunReceipt final
{
    friend class V4MixedGraphExecutor;
public:
    V4MixedOleRunReceipt(V4MixedOleRunReceipt&&) = default;
    V4MixedOleRunReceipt& operator=(V4MixedOleRunReceipt&&) = delete;
    V4MixedOleRunReceipt(const V4MixedOleRunReceipt&) = delete;
    V4MixedOleRunReceipt& operator=(const V4MixedOleRunReceipt&) = delete;
    const Result& Execution() const noexcept { return execution_; }
    const V4JoinedOleExecutionPlan& Plan() const noexcept { return plan_; }
    const std::optional<V4IndependentFileInputObservation>& FileInitial() const noexcept { return fileInitial_; }
    const std::optional<V4IndependentFileInputObservation>& FileAtExecution() const noexcept { return fileAtExecution_; }
    const std::vector<V2CapturedOleReturn>& CapturedReturns() const noexcept { return captured_; }
    const std::vector<mixed_fixture::Call>& MixedCalls() const noexcept { return mixedCalls_; }
    const std::vector<ole_file_source_fixture::FileDropRead>& FileReads() const noexcept { return fileReads_; }
    const std::vector<ole_content_source_fixture::Call>& ContentCalls() const noexcept { return contentCalls_; }
    const std::vector<MessageObservation>& CompletionTrace() const noexcept { return completionTrace_; }
    bool ProviderBaselineCaptured() const noexcept { return providerBaselineCaptured_; }
    const std::vector<mixed_fixture::Call>& BaselineMixedCalls() const noexcept { return baselineMixedCalls_; }
    const std::vector<ole_file_source_fixture::FileDropRead>& BaselineFileReads() const noexcept { return baselineFileReads_; }
    const std::vector<ole_content_source_fixture::Call>& BaselineContentCalls() const noexcept { return baselineContentCalls_; }
    bool ProviderRecordsCaptured() const noexcept { return providerRecordsCaptured_; }
    bool CompletionTraceCaptured() const noexcept { return completionTraceCaptured_; }
    bool SameBoundHost(const mixed_fixture::MixedBinding& binding,
        const std::shared_ptr<mixed_fixture::MixedOwner>& owner, OleDragDropAdapter* adapter,
        const std::shared_ptr<const V3HostEpoch>& epoch, const MessageTrace& trace, HWND control) const noexcept
    {
        auto* source = binding.SourceObject();
        return execution_.productionStarted && plan_.graphIdentityValid && source && owner && epoch &&
            owner_.lock() == owner && epoch_.lock() == epoch && source == source_ && adapter == adapter_ &&
            binding.candidateId == plan_.candidateId && epoch->Matches(source, adapter, &trace, control);
    }
    bool SameBoundFiles(const V4IndependentShellSource& binding,
        const std::shared_ptr<V4IndependentShellOwner>& owner) const noexcept
    {
        auto* source = binding.SourceObject();
        if (!execution_.productionStarted || !source || !owner || fileOwner_.lock() != owner ||
            fileSource_ != source || binding.candidateId != plan_.candidateId) return false;
        const auto current = source->LifetimeIdentity();
        return !fileLifetime_.expired() && !current.expired() &&
            !fileLifetime_.owner_before(current) && !current.owner_before(fileLifetime_);
    }
private:
    V4MixedOleRunReceipt() = default;
    Result execution_;
    V4JoinedOleExecutionPlan plan_;
    std::optional<V4IndependentFileInputObservation> fileInitial_, fileAtExecution_;
    std::vector<V2CapturedOleReturn> captured_;
    std::vector<mixed_fixture::Call> baselineMixedCalls_;
    std::vector<ole_file_source_fixture::FileDropRead> baselineFileReads_;
    std::vector<ole_content_source_fixture::Call> baselineContentCalls_;
    std::vector<mixed_fixture::Call> mixedCalls_;
    std::vector<ole_file_source_fixture::FileDropRead> fileReads_;
    std::vector<ole_content_source_fixture::Call> contentCalls_;
    std::vector<MessageObservation> completionTrace_;
    bool providerBaselineCaptured_ = false, providerRecordsCaptured_ = false, completionTraceCaptured_ = false;
    std::weak_ptr<mixed_fixture::MixedOwner> owner_;
    std::weak_ptr<V4IndependentShellOwner> fileOwner_;
    std::weak_ptr<const ole_file_source_fixture::Source::LifetimeToken> fileLifetime_;
    ole_file_source_fixture::Source* fileSource_ = nullptr;
    std::weak_ptr<const V3HostEpoch> epoch_;
    mixed_fixture::Source* source_ = nullptr;
    OleDragDropAdapter* adapter_ = nullptr;
};

class V4MixedGraphExecutor final
{
public:
    static V4MixedOleRunReceipt RunBound(const V4FamilyValidatedContext& verified,
        const event_graph::V4FamilyIdentityContext& identity, const std::vector<bool>& present,
        const event_graph::V4JoinedGraphVariant& selected, const std::vector<Input>& inputs,
        const std::shared_ptr<V4IndependentFilePayload>& payload, const V4IndependentShellSource& fileBinding,
        const std::shared_ptr<V4IndependentShellOwner>& fileOwner, const mixed_fixture::MixedBinding& binding,
        const std::shared_ptr<mixed_fixture::MixedOwner>& owner, OleDragDropAdapter* adapter,
        const std::shared_ptr<const V3HostEpoch>& epoch, const HostBoundaries& host,
        const std::function<void(const Observation&)>& assertIndependentOleResult,
        const std::function<V2HostCoordinateSnapshot(const Observation&)>& sampler = {})
    {
        V4MixedOleRunReceipt receipt;
        const auto pinnedPayload = payload; const auto pinnedFileOwner = fileOwner;
        const auto pinnedOwner = owner; const auto pinnedEpoch = epoch;
        bool dispatchEntered = false;
        try
        {
            receipt.plan_ = BuildV4JoinedOleExecutionPlan(verified, identity, present, selected, inputs);
            receipt.execution_.obligations = receipt.plan_.preconditions;
            receipt.execution_.postconditionsPending = receipt.plan_.postDispatchObligations;
            receipt.execution_.unclassifiedObligations = receipt.plan_.unclassified;
            auto* source = binding.SourceObject(); auto* fileSource = fileBinding.SourceObject();
            if (!pinnedPayload || !pinnedOwner || !pinnedFileOwner || !source || !fileSource ||
                !binding.inputPrepared || binding.resourceUnavailable || !fileBinding.inputPrepared ||
                fileBinding.resourceUnavailable || binding.candidateId != receipt.plan_.candidateId ||
                fileBinding.candidateId != receipt.plan_.candidateId || binding.owner_.lock() != pinnedOwner ||
                !pinnedOwner->Owns(pinnedFileOwner, pinnedPayload, fileSource))
                receipt.execution_.obligations.emplace_back("v4_mixed_source_owner_or_candidate_unavailable");
            if (pinnedPayload && fileSource)
            {
                receipt.fileInitial_ = InspectV4IndependentFileInputs(verified, identity, *pinnedPayload, *fileSource, true);
                if (!receipt.fileInitial_->captured || receipt.fileInitial_->resourceUnavailable ||
                    receipt.fileInitial_->candidateId != receipt.plan_.candidateId)
                    receipt.execution_.obligations.emplace_back("v4_mixed_file_snapshot_unavailable");
            }
            if (Value(verified.Axes(), verified.Coordinate(), "V4.source.kind") != "file_and_content" ||
                Value(verified.Axes(), verified.Coordinate(), "T01") != "external_ingress")
                receipt.execution_.obligations.emplace_back("v4_mixed_external_source_unimplemented");
            if (!source || !adapter || !pinnedEpoch || !pinnedEpoch->Matches(source, adapter))
                receipt.execution_.obligations.emplace_back("v4_mixed_host_epoch_unavailable");
            if (!host.beforeOleCall || !host.observeOleReturn || !host.awaitProductionCompletionOrRejection ||
                !host.assertIndependentBusinessResults || !assertIndependentOleResult)
                receipt.execution_.obligations.emplace_back("v4_mixed_host_observer_or_oracle_missing");
            // Binding/entries are input evidence only. They do not establish the
            // complete offered formats, actual selected GetData, worker identity,
            // independent oracle capability or non-atomic payload stability.
            receipt.execution_.obligations.emplace_back("v4_mixed_complete_source_and_all_axes_unimplemented");
            receipt.execution_.obligations.emplace_back("v4_mixed_selected_format_and_graph_presence_unimplemented");
            receipt.execution_.obligations.emplace_back("v4_mixed_same_task_completion_and_business_oracle_unimplemented");
            if (!receipt.plan_.graphIdentityValid || !receipt.plan_.structurallyPlanned || !receipt.execution_.obligations.empty())
                return receipt;
            if (!pinnedOwner->Begin(pinnedFileOwner, false))
            { receipt.execution_.additionalObservationUnavailable = true; receipt.execution_.obligations.emplace_back("v4_mixed_source_owner_busy"); return receipt; }
            const auto endSource = [](mixed_fixture::MixedOwner* active) noexcept { active->End(); };
            const std::unique_ptr<mixed_fixture::MixedOwner, decltype(endSource)> sourceGuard(pinnedOwner.get(), endSource);
            if (!pinnedEpoch->TryBeginExecution(source, adapter))
            { receipt.execution_.additionalObservationUnavailable = true; receipt.execution_.obligations.emplace_back("v4_mixed_host_epoch_busy"); return receipt; }
            const auto endHost = [](const V3HostEpoch* active) noexcept { active->EndExecution(); };
            const std::unique_ptr<const V3HostEpoch, decltype(endHost)> hostGuard(pinnedEpoch.get(), endHost);
            receipt.fileAtExecution_ = InspectV4IndependentFileInputs(verified, identity, *pinnedPayload, *fileSource, true);
            // This snapshot is deliberately not promoted to a lease or proof of
            // unchanged source contents; its comparison is not implemented.
            if (!receipt.fileAtExecution_->captured || receipt.fileAtExecution_->resourceUnavailable)
            { receipt.execution_.additionalObservationUnavailable = true; receipt.execution_.obligations.emplace_back("v4_mixed_execution_snapshot_unavailable"); return receipt; }
            receipt.owner_ = pinnedOwner; receipt.fileOwner_ = pinnedFileOwner; receipt.fileSource_ = fileSource;
            receipt.fileLifetime_ = fileSource->LifetimeIdentity(); receipt.epoch_ = pinnedEpoch;
            receipt.source_ = source; receipt.adapter_ = adapter;
            const auto requireLive = [&] {
                if (binding.SourceObject() != source || fileBinding.SourceObject() != fileSource ||
                    !pinnedOwner->Owns(pinnedFileOwner, pinnedPayload, fileSource) || !pinnedEpoch->Matches(source, adapter))
                    throw ObservationUnavailable("v4_mixed_execution_source_or_host_changed");
            };
            const bool baselineCalls = source->CopyCalls(receipt.baselineMixedCalls_);
            const bool baselineProviders = source->CopyProviderEvidence(receipt.baselineFileReads_, receipt.baselineContentCalls_);
            receipt.providerBaselineCaptured_ = baselineCalls && baselineProviders;
            requireLive();
            if (!receipt.providerBaselineCaptured_ || !receipt.baselineMixedCalls_.empty() ||
                !receipt.baselineFileReads_.empty() || !receipt.baselineContentCalls_.empty())
            { receipt.execution_.additionalObservationUnavailable = true;
              receipt.execution_.obligations.emplace_back("v4_mixed_provider_baseline_unavailable_or_nonempty"); return receipt; }
            const HostBoundaries guarded{
                [&](const Input& input) { requireLive(); host.beforeOleCall(input); requireLive(); },
                [&](const Observation& event) { requireLive(); host.observeOleReturn(event); requireLive(); },
                [&](const Observation& terminal) {
                    requireLive(); host.awaitProductionCompletionOrRejection(terminal); requireLive();
                    const bool callsCaptured = source->CopyCalls(receipt.mixedCalls_);
                    const bool providersCaptured = source->CopyProviderEvidence(receipt.fileReads_, receipt.contentCalls_);
                    receipt.providerRecordsCaptured_ = callsCaptured && providersCaptured;
                    if (pinnedEpoch->trace_ && !pinnedEpoch->trace_->LostOrCrossThread() &&
                        pinnedEpoch->trace_->OwnerThread() == GetCurrentThreadId())
                    { receipt.completionTrace_ = pinnedEpoch->trace_->Observations(); receipt.completionTraceCaptured_ = true; }
                    requireLive();
                    if (!receipt.providerRecordsCaptured_ || !receipt.completionTraceCaptured_)
                        throw ObservationUnavailable("v4_mixed_provider_or_completion_trace_unavailable");
                },
                [&] { requireLive(); host.assertIndependentBusinessResults(); requireLive(); }};
            const std::function<void(const Observation&)> oracle = [&](const Observation& event) {
                requireLive(); assertIndependentOleResult(event); requireLive(); };
            const std::function<V2HostCoordinateSnapshot(const Observation&)> guardedSampler = sampler
                ? std::function<V2HostCoordinateSnapshot(const Observation&)>{[&](const Observation& event) {
                    requireLive(); auto value = sampler(event); requireLive(); return value; }}
                : std::function<V2HostCoordinateSnapshot(const Observation&)>{};
            dispatchEntered = true;
            receipt.execution_ = V2GraphExecutor::DispatchApprovedPlan(receipt.plan_.steps, inputs.size(), adapter,
                static_cast<IDataObject*>(source), guarded, oracle, &guardedSampler, &receipt.captured_);
            requireLive();
            const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
            if (!replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
                replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
                replay.variant.unresolved != selected.unresolved)
            { receipt.plan_.graphIdentityValid = false; receipt.execution_.additionalObservationUnavailable = true; }
        }
        catch (const std::exception& error)
        {
            if (dispatchEntered && !receipt.execution_.productionStarted) receipt.execution_.productionEntryUnknown = true;
            receipt.execution_.additionalObservationUnavailable = true;
            if (receipt.execution_.status != Status::event_failed && receipt.execution_.status != Status::event_oracle_failed &&
                receipt.execution_.status != Status::business_failed) receipt.execution_.status = Status::observation_unavailable;
            try { if (receipt.execution_.failure.empty()) receipt.execution_.failure = error.what(); } catch (...) {}
        }
        catch (...)
        {
            if (dispatchEntered && !receipt.execution_.productionStarted) receipt.execution_.productionEntryUnknown = true;
            receipt.execution_.additionalObservationUnavailable = true;
            if (receipt.execution_.status != Status::event_failed && receipt.execution_.status != Status::event_oracle_failed &&
                receipt.execution_.status != Status::business_failed) receipt.execution_.status = Status::observation_unavailable;
            try { if (receipt.execution_.failure.empty()) receipt.execution_.failure = "unknown_v4_mixed_execution_exception"; } catch (...) {}
        }
        return receipt;
    }
};

// Same combined source task, underlying file lease and actual control-window returns.
inline V4FileTaskWindowObservation InspectV4MixedTaskWindows(const V4MixedOleRunReceipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    const V4IndependentShellSource& fileBinding, const std::shared_ptr<V4IndependentShellOwner>& fileOwner,
    const mixed_fixture::MixedBinding& binding, const std::shared_ptr<mixed_fixture::MixedOwner>& owner,
    OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
    const MessageTrace& trace, HWND control, UINT completionMessage)
{
    namespace diagnostic = snowdesktop::external_transfer_observation;
    V4FileTaskWindowObservation result;
    auto* mixedSource = binding.SourceObject();
    auto* fileSource = fileBinding.SourceObject();
    if (!mixedSource || !fileSource) { result.observationUnavailable = true; return result; }
    auto& source = *fileSource;
    try
    {
        if (!receipt.SameBoundHost(binding, owner, adapter, epoch, trace, control) ||
            !receipt.SameBoundFiles(fileBinding, fileOwner) || !receipt.CompletionTraceCaptured() ||
            !receipt.FileAtExecution() || !V4SourceSnapshotReady(*receipt.FileAtExecution()) ||
            trace.LostOrCrossThread() || trace.OwnerThread() != GetCurrentThreadId() ||
            !control || !IsWindow(control) || GetWindowThreadProcessId(control, nullptr) != trace.OwnerThread())
        { result.observationUnavailable = true; result.unresolved.emplace_back("v4_file_task_receipt_trace_or_host_unavailable"); return result; }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || raw.id != receipt.Plan().candidateId ||
            raw.id != receipt.FileAtExecution()->candidateId || selected.id != receipt.Plan().graphId ||
            !replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
            replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
            replay.variant.unresolved != selected.unresolved)
        { result.observationUnavailable = true; result.unresolved.emplace_back("v4_file_task_candidate_or_graph_unavailable"); return result; }
        result.messages = receipt.CompletionTrace();
        for (const auto& call : receipt.CapturedReturns())
        {
            if (call.observation.kind != Kind::drop) continue;
            if (result.drop) { result.observationUnavailable = true; result.unresolved.emplace_back("v4_file_task_multiple_drop_calls"); return result; }
            result.drop = call;
        }
        if (!result.drop || !diagnostic::Complete())
        { result.observationUnavailable = true; result.unresolved.emplace_back("v4_file_task_drop_or_stopped_log_unavailable"); return result; }
        diagnostic::ReadSession session;
        if (!session.Open()) { result.observationUnavailable = true; result.unresolved.emplace_back("v4_file_task_session_unavailable"); return result; }
        const auto count = session.Count();
        result.events.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            V4FileTaskWindowObservation::RawEvent record; record.index = i;
            if (!session.ReadEvent(i, record.event))
            { result.observationUnavailable = true; result.unresolved.emplace_back("v4_file_task_log_record_unavailable"); return result; }
            if (record.event.hasBlob)
            {
                if (record.event.blobSize > diagnostic::BlobLimit()) { result.observationUnavailable = true; return result; }
                record.blob.resize(record.event.blobSize); std::size_t size = 0;
                if (!session.CopyBlob(i, record.blob.data(), record.blob.size(), size) || size != record.blob.size())
                { result.observationUnavailable = true; result.unresolved.emplace_back("v4_file_task_blob_unavailable"); return result; }
            }
            result.events.push_back(std::move(record));
        }
        result.passive = InspectPassiveTransfer(static_cast<IDataObject*>(mixedSource));
        result.identityValid = true;
        const auto& drop = *result.drop; const auto& origin = result.passive->adapterScope;
        const auto sourceAddress = reinterpret_cast<std::uintptr_t>(static_cast<IDataObject*>(mixedSource));
        const auto transferCount = std::count_if(result.events.begin(), result.events.end(), [&](const auto& record) {
            return record.event.phase == diagnostic::Phase::transfer_enter && record.event.object == sourceAddress;
        });
        result.localAdapterScopeMatched = transferCount == 1 && result.passive->task && origin.observed &&
            drop.entered.kind == snowdesktop::ole_adapter_observation::Kind::drop &&
            drop.returned.kind == snowdesktop::ole_adapter_observation::Kind::drop &&
            drop.entered.phase == snowdesktop::ole_adapter_observation::Phase::entered &&
            drop.returned.phase == snowdesktop::ole_adapter_observation::Phase::returned &&
            drop.entered.session && drop.entered.call && !drop.entered.parent && !drop.entered.depth &&
            drop.entered.session == drop.returned.session && drop.entered.call == drop.returned.call &&
            drop.entered.thread == trace.OwnerThread() && drop.returned.thread == drop.entered.thread &&
            drop.entered.adapter == reinterpret_cast<std::uintptr_t>(adapter) && drop.returned.adapter == drop.entered.adapter &&
            drop.returned.result == drop.observation.result && origin.session == drop.entered.session &&
            origin.call == drop.entered.call && origin.parent == drop.entered.parent && origin.depth == drop.entered.depth &&
            origin.thread == drop.entered.thread && origin.adapter == drop.entered.adapter &&
            origin.kind == static_cast<unsigned>(snowdesktop::ole_adapter_observation::Kind::drop);
        if (result.localAdapterScopeMatched)
        {
            result.messageCandidates = FindPassiveTaskWindowCandidates(*result.passive, result.messages,
                control, MessageRoute::control_window_proc, completionMessage, trace.OwnerThread());
            result.localChain = FindV4FileLocalChain(*result.passive, result.events,
                result.messageCandidates, trace.OwnerThread(), source);
            if (!result.localChain) result.unresolved.emplace_back("v4_file_local_task_work_chain_unavailable");
            const V4FileTaskWindowObservation::RawEvent* origin = nullptr;
            for (const auto& record : result.events)
                if (record.event.phase == diagnostic::Phase::desktop_file_transfer_origin &&
                    record.event.task == result.passive->task)
                { if (origin) { origin = nullptr; break; } origin = &record; }
            if (origin && origin->event.extra)
            {
                const V4FileTaskWindowObservation::RawEvent* enter = nullptr;
                const V4FileTaskWindowObservation::RawEvent* returned = nullptr;
                bool duplicate = false;
                for (const auto& record : result.events)
                {
                    if (record.event.extra != origin->event.extra) continue;
                    if (record.event.phase == diagnostic::Phase::desktop_file_callsite_enter)
                    { if (enter) duplicate = true; else enter = &record; }
                    if (record.event.phase == diagnostic::Phase::desktop_file_callsite_return)
                    { if (returned) duplicate = true; else returned = &record; }
                }
                const auto matchesScope = [&](const diagnostic::AdapterScopeIdentity& scope) {
                    return scope.observed && scope.session == drop.entered.session && scope.call == drop.entered.call &&
                        scope.parent == drop.entered.parent && scope.adapter == drop.entered.adapter &&
                        scope.thread == drop.entered.thread && scope.depth == drop.entered.depth &&
                        scope.kind == static_cast<unsigned>(snowdesktop::ole_adapter_observation::Kind::drop);
                };
                const auto sourceAddress = reinterpret_cast<std::uintptr_t>(static_cast<IDataObject*>(mixedSource));
                if (!duplicate && enter && returned && enter->index < origin->index && origin->index < returned->index &&
                    enter->event.task == 0 && returned->event.task == 0 && origin->event.task == result.passive->task &&
                    enter->event.object == sourceAddress && origin->event.object == sourceAddress && returned->event.object == sourceAddress &&
                    enter->event.thread == trace.OwnerThread() && origin->event.thread == enter->event.thread &&
                    returned->event.thread == enter->event.thread && origin->event.localTask == result.passive->task &&
                    !origin->event.localWork && matchesScope(enter->event.adapterScope) &&
                    matchesScope(origin->event.adapterScope) && matchesScope(returned->event.adapterScope))
                {
                    result.directCallsiteMatched = true; result.directCallsite = origin->event.extra;
                    result.callsiteEnterIndex = enter->index; result.transferOriginIndex = origin->index;
                    result.callsiteReturnIndex = returned->index;
                }
            }
            if (!result.directCallsiteMatched) result.unresolved.emplace_back("v4_file_direct_drop_callsite_unavailable");
        }
        else result.unresolved.emplace_back("v4_file_task_drop_scope_not_matched");
        result.unresolved.insert(result.unresolved.end(), result.passive->unresolved.begin(), result.passive->unresolved.end());
        result.unresolved.emplace_back("v4_file_task_window_candidates_not_full_causal_attribution");
        result.unresolved.emplace_back("v4_file_task_unobserved_reentry_late_work_and_business_unresolved");
        result.observationUnavailable = true;
    }
    catch (const std::exception&) { result.observationUnavailable = true; }
    catch (...) { result.observationUnavailable = true; }
    return result;
}

struct V4MixedDiagnosticRecord
{
    std::size_t index = 0;
    snowdesktop::external_transfer_observation::Event event{};
    bool blobCopied = false;
    std::vector<std::byte> blob;
};
struct V4MixedGraphEvidence
{
    bool identityValid = false, hypothesisObservationRecorded = false, hypothesisNotMet = false;
    bool observationUnavailable = true, providerRecordsComplete = false, diagnosticRecordsComplete = false;
    bool completionTraceCaptured = false, unicodeEnvelopeObserved = false;
    std::array<std::size_t, 6> unicodeEnvelopeIndices{};
    std::vector<V4FileGraphEventEvidence> events;
    std::vector<V4FileGraphOrderEvidence> orders;
    std::vector<V2CapturedOleReturn> oleEvidence;
    // Both partial and complete snapshots are actual values from this receipt.
    // Query/Get/Enum are never converted into a selected-format decision.
    std::vector<mixed_fixture::Call> mixedCalls;
    std::vector<ole_file_source_fixture::FileDropRead> fileReads;
    std::vector<ole_content_source_fixture::Call> contentCalls;
    std::vector<V4MixedDiagnosticRecord> diagnosticRecords;
    std::vector<MessageObservation> completionTrace;
    std::optional<V4FileTaskWindowObservation> taskWindow;
    std::vector<std::string> unresolved;
};

inline V4MixedGraphEvidence InspectV4MixedGraph(const V4MixedOleRunReceipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    const mixed_fixture::MixedBinding& binding, const std::shared_ptr<mixed_fixture::MixedOwner>& owner,
    OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
    const MessageTrace& trace, HWND control)
{
    namespace diagnostic = snowdesktop::external_transfer_observation;
    V4MixedGraphEvidence out;
    try
    {
        const auto mapping = event_graph::MapV4PresenceCandidate(identity, verified);
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
        if (!mapping.graphAvailable || raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() ||
            raw.id != receipt.Plan().candidateId || selected.id != receipt.Plan().graphId ||
            !replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
            replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
            replay.variant.unresolved != selected.unresolved || !receipt.SameBoundHost(binding, owner, adapter, epoch, trace, control))
        { out.unresolved.emplace_back("v4_mixed_graph_identity_or_host_unavailable"); return out; }
        auto requirements = mapping.spec.requirements;
        std::sort(requirements.begin(), requirements.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        if (requirements.size() != present.size())
        { out.unresolved.emplace_back("v4_mixed_graph_presence_shape_unavailable"); return out; }
        std::map<std::string, std::size_t> byId, classes;
        std::size_t active = 0;
        for (std::size_t i = 0; i < requirements.size(); ++i)
        {
            if (!byId.emplace(requirements[i].id, out.events.size()).second ||
                (present[i] && active >= selected.classes.size()))
            { out.unresolved.emplace_back("v4_mixed_graph_partition_unavailable"); return out; }
            out.events.push_back({requirements[i].id, present[i], V4FileGraphObservationState::unavailable, {}, {}});
            if (present[i]) classes.emplace(requirements[i].id, selected.classes[active++]);
        }
        if (active != selected.classes.size())
        { out.unresolved.emplace_back("v4_mixed_graph_partition_unavailable"); return out; }
        std::map<std::size_t, std::size_t> ranks;
        for (std::size_t i = 0; i < selected.order.size(); ++i)
        {
            const auto group = classes.find(selected.order[i]);
            if (group == classes.end() || !ranks.emplace(group->second, i).second)
            { out.unresolved.emplace_back("v4_mixed_graph_order_unavailable"); return out; }
        }
        out.identityValid = true;
        out.oleEvidence = receipt.CapturedReturns();
        const auto mark = [&](const std::string& id, V4FileGraphObservationState state,
                std::optional<std::size_t> index = {}) {
            const auto it = byId.find(id);
            if (it == byId.end()) return false;
            auto& item = out.events[it->second]; item.state = state; item.logIndex = index; return true;
        };
        const auto before = [&](const std::string& first, const std::string& last, const char* basis) {
            const auto a = classes.find(first), b = classes.find(last);
            if (a == classes.end() || b == classes.end() || !ranks.contains(a->second) || !ranks.contains(b->second)) return;
            const bool agrees = ranks.at(a->second) < ranks.at(b->second);
            out.orders.push_back({first, last, basis, agrees}); out.hypothesisObservationRecorded = true;
            if (!agrees) out.hypothesisNotMet = true;
        };
        std::vector<std::pair<std::string, std::string>> calls;
        for (const auto& item : out.oleEvidence)
        {
            const auto& entered = item.entered; const auto& returned = item.returned;
            const auto expectedKind = item.observation.kind == Kind::enter ? snowdesktop::ole_adapter_observation::Kind::enter :
                item.observation.kind == Kind::over ? snowdesktop::ole_adapter_observation::Kind::over :
                item.observation.kind == Kind::drop ? snowdesktop::ole_adapter_observation::Kind::drop :
                snowdesktop::ole_adapter_observation::Kind::leave;
            if (!entered.session || !entered.call || entered.parent || entered.depth || entered.thread != GetCurrentThreadId() ||
                entered.kind != expectedKind || entered.adapter != reinterpret_cast<std::uintptr_t>(adapter) ||
                entered.phase != snowdesktop::ole_adapter_observation::Phase::entered || returned.session != entered.session ||
                returned.call != entered.call || returned.thread != entered.thread || returned.adapter != entered.adapter ||
                returned.parent != entered.parent || returned.depth != entered.depth || returned.kind != entered.kind ||
                returned.phase != snowdesktop::ole_adapter_observation::Phase::returned || returned.result != item.observation.result)
            { out.unresolved.emplace_back("v4_mixed_graph_ole_scope_unavailable"); continue; }
            const auto base = "program_" + std::to_string(item.observation.programIndex);
            const auto first = base + "_call_start", last = base + "_call_return";
            if (!mark(first, V4FileGraphObservationState::observed_local) || !mark(last, V4FileGraphObservationState::observed_local))
            { out.unresolved.emplace_back("v4_mixed_graph_ole_requirement_unavailable"); continue; }
            out.hypothesisObservationRecorded = true;
            if (!out.events[byId.at(first)].selectedPresent || !out.events[byId.at(last)].selectedPresent)
                out.hypothesisNotMet = true;
            calls.emplace_back(first, last);
        }
        for (std::size_t i = 0; i < calls.size(); ++i)
        {
            before(calls[i].first, calls[i].second, "captured_same_ole_call");
            if (i) before(calls[i - 1].second, calls[i].first, "sequential_ole_dispatch");
        }
        out.mixedCalls = receipt.MixedCalls(); out.fileReads = receipt.FileReads(); out.contentCalls = receipt.ContentCalls();
        out.providerRecordsComplete = receipt.ProviderBaselineCaptured() && receipt.BaselineMixedCalls().empty() &&
            receipt.BaselineFileReads().empty() && receipt.BaselineContentCalls().empty() && receipt.ProviderRecordsCaptured();
        out.completionTrace = receipt.CompletionTrace(); out.completionTraceCaptured = receipt.CompletionTraceCaptured();
        // Stop and writer drain establish only a complete diagnostic snapshot,
        // never the identity or quiescence of the production task.
        if (diagnostic::Complete())
        {
            diagnostic::ReadSession session;
            if (session.Open() && session.Count() <= diagnostic::EventLimit())
            {
                const auto count = session.Count();
                out.diagnosticRecords.reserve(count);
                bool complete = true;
                for (std::size_t i = 0; i < count; ++i)
                {
                    V4MixedDiagnosticRecord item; item.index = i;
                    if (!session.ReadEvent(i, item.event)) { complete = false; break; }
                    if (item.event.hasBlob)
                    {
                        if (item.event.blobSize > diagnostic::BlobLimit()) { complete = false; break; }
                        item.blob.resize(item.event.blobSize); std::size_t size = 0;
                        if (!session.CopyBlob(i, item.blob.data(), item.blob.size(), size) || size != item.blob.size()) { complete = false; break; }
                        item.blobCopied = true;
                    }
                    out.diagnosticRecords.push_back(std::move(item));
                }
                out.diagnosticRecordsComplete = complete;
            }
        }
        // A first top-level combined-source call is only a candidate for a
        // first provider occurrence: the separate provider logs lack a shared
        // call id and a complete host/worker interval. Preserve the actual
        // request and result but do not compare presence or cross-log order.
        if (out.providerRecordsComplete)
        {
            for (const auto format : {CF_HDROP, CF_UNICODETEXT})
            {
                const std::string base = format == CF_HDROP ? "v4_first_cfhdrop_getdata" : "v4_first_unicode_getdata";
                for (std::size_t i = 0; i < out.mixedCalls.size(); ++i)
                {
                    const auto& enter = out.mixedCalls[i];
                    if (enter.phase != mixed_fixture::CallPhase::entered || enter.query || enter.enumeration ||
                        !enter.formatPresent || enter.format != format) continue;
                    if (!enter.call || enter.parent || enter.depth || enter.thread != GetCurrentThreadId())
                    { out.unresolved.emplace_back("v4_mixed_nested_provider_occurrence_unavailable"); break; }
                    mark(base + "_enter", V4FileGraphObservationState::unattributed_source_candidate, i);
                    for (std::size_t j = i + 1; j < out.mixedCalls.size(); ++j)
                    {
                        const auto& end = out.mixedCalls[j];
                        if (end.call != enter.call || end.thread != enter.thread || end.parent != enter.parent || end.depth != enter.depth) continue;
                        if (end.phase == mixed_fixture::CallPhase::returned)
                            mark(base + "_return", V4FileGraphObservationState::unattributed_source_candidate, j);
                        break;
                    }
                    break;
                }
            }
        }

        // Only the same-thread synchronous Unicode route can currently be
        // correlated across the actual caller, combined IDataObject and the
        // underlying content provider. All three scopes and both ABI returns
        // must be unique and enclosed by the same actual Drop invocation.
        // An empty or ambiguous log never proves the invocation was absent.
        if (out.providerRecordsComplete && out.diagnosticRecordsComplete)
        {
            const V2CapturedOleReturn* drop = nullptr; bool multipleDrop = false;
            for (const auto& item : out.oleEvidence)
                if (item.observation.kind == Kind::drop)
                { if (drop) multipleDrop = true; else drop = &item; }
            std::array<const V4MixedDiagnosticRecord*, 6> envelope{};
            std::array<std::size_t, 6> counts{};
            auto* source = binding.SourceObject();
            const auto combinedAddress = reinterpret_cast<std::uintptr_t>(static_cast<IDataObject*>(source));
            const auto contentAddress = source ? source->ContentProviderIdentity() : 0;
            for (const auto& record : out.diagnosticRecords)
            {
                const auto phase = record.event.phase;
                int slot = -1;
                if (phase == diagnostic::Phase::unicode_getdata_caller_enter) slot = 0;
                else if (phase == diagnostic::Phase::mixed_provider_getdata_enter) slot = 1;
                else if (phase == diagnostic::Phase::content_provider_getdata_enter) slot = 2;
                else if (phase == diagnostic::Phase::content_provider_getdata_return) slot = 3;
                else if (phase == diagnostic::Phase::mixed_provider_getdata_return) slot = 4;
                else if (phase == diagnostic::Phase::unicode_getdata_caller_return) slot = 5;
                else if (phase == diagnostic::Phase::mixed_provider_getdata_unwound) { multipleDrop = true; continue; }
                if (slot >= 0) { ++counts[static_cast<std::size_t>(slot)]; envelope[static_cast<std::size_t>(slot)] = &record; }
            }
            bool valid = drop && !multipleDrop && source && contentAddress &&
                std::all_of(counts.begin(), counts.end(), [](std::size_t n) { return n == 1; });
            if (valid)
            {
                const auto& entered = drop->entered; const auto& returned = drop->returned;
                valid = entered.kind == snowdesktop::ole_adapter_observation::Kind::drop &&
                    entered.phase == snowdesktop::ole_adapter_observation::Phase::entered && entered.session && entered.call &&
                    !entered.parent && !entered.depth && entered.thread == GetCurrentThreadId() &&
                    entered.adapter == reinterpret_cast<std::uintptr_t>(adapter) &&
                    returned.phase == snowdesktop::ole_adapter_observation::Phase::returned && returned.kind == entered.kind &&
                    returned.session == entered.session && returned.call == entered.call && returned.thread == entered.thread &&
                    returned.adapter == entered.adapter && returned.result == drop->observation.result;
                for (std::size_t i = 0; valid && i < envelope.size(); ++i)
                {
                    const auto& e = envelope[i]->event; const auto& scope = e.adapterScope;
                    const auto expectedObject = (i == 2 || i == 3) ? contentAddress : combinedAddress;
                    valid = e.object == expectedObject && e.thread == entered.thread && scope.observed &&
                        scope.session == entered.session && scope.call == entered.call && scope.parent == entered.parent &&
                        scope.depth == entered.depth && scope.thread == entered.thread && scope.adapter == entered.adapter &&
                        scope.kind == static_cast<unsigned>(snowdesktop::ole_adapter_observation::Kind::drop) &&
                        (!i || envelope[i - 1]->index < envelope[i]->index);
                }
            }
            if (valid)
            {
                const auto& callerEnter = envelope[0]->event; const auto& mixedEnter = envelope[1]->event;
                const auto& contentEnter = envelope[2]->event; const auto& contentReturn = envelope[3]->event;
                const auto& mixedReturn = envelope[4]->event; const auto& callerReturn = envelope[5]->event;
                valid = callerEnter.extra && callerEnter.extra == callerReturn.extra && mixedEnter.extra &&
                    mixedEnter.extra == mixedReturn.extra && contentEnter.extra && contentEnter.extra == contentReturn.extra &&
                    mixedEnter.value == CF_UNICODETEXT && contentReturn.value == mixedReturn.value &&
                    mixedReturn.value == callerReturn.value;
                const mixed_fixture::Call* localEnter = nullptr; const mixed_fixture::Call* localReturn = nullptr;
                std::size_t enterIndex = 0, returnIndex = 0;
                for (std::size_t i = 0; i < out.mixedCalls.size(); ++i)
                {
                    const auto& call = out.mixedCalls[i]; if (call.call != mixedEnter.extra) continue;
                    if (call.phase == mixed_fixture::CallPhase::entered)
                    { if (localEnter) valid = false; else { localEnter = &call; enterIndex = i; } }
                    else if (call.phase == mixed_fixture::CallPhase::returned)
                    { if (localReturn) valid = false; else { localReturn = &call; returnIndex = i; } }
                    else valid = false;
                }
                valid = valid && localEnter && localReturn;
                if (valid)
                    valid = returnIndex == enterIndex + 1 && !localEnter->query && !localEnter->enumeration &&
                        localEnter->formatPresent && localEnter->format == CF_UNICODETEXT && localEnter->aspect == DVASPECT_CONTENT &&
                        localEnter->index == -1 && localEnter->tymed == TYMED_HGLOBAL && !localEnter->targetDevicePresent &&
                        !localEnter->parent && !localEnter->depth && localEnter->thread == GetCurrentThreadId() &&
                        localReturn->thread == localEnter->thread && localReturn->parent == localEnter->parent &&
                        localReturn->depth == localEnter->depth && static_cast<std::int64_t>(localReturn->result) == mixedReturn.value;
                const ole_content_source_fixture::Call* content = nullptr;
                for (const auto& call : out.contentCalls)
                    if (!call.query && call.format == CF_UNICODETEXT)
                    { if (content) { valid = false; break; } content = &call; }
                valid = valid && content && content->thread == GetCurrentThreadId() && content->aspect == DVASPECT_CONTENT &&
                    content->index == -1 && content->requestedTymed == TYMED_HGLOBAL && !content->targetDevicePresent &&
                    static_cast<std::int64_t>(content->result) == contentReturn.value;
            }
            if (valid)
            {
                for (std::size_t i = 0; i < envelope.size(); ++i) out.unicodeEnvelopeIndices[i] = envelope[i]->index;
                out.unicodeEnvelopeObserved = true;
                const std::string first = "v4_first_unicode_getdata_enter", last = "v4_first_unicode_getdata_return";
                if (mark(first, V4FileGraphObservationState::observed_local, envelope[1]->index) &&
                    mark(last, V4FileGraphObservationState::observed_local, envelope[4]->index))
                {
                    out.hypothesisObservationRecorded = true;
                    if (!out.events[byId.at(first)].selectedPresent || !out.events[byId.at(last)].selectedPresent)
                        out.hypothesisNotMet = true;
                    before(first, last, "same_caller_combined_and_content_provider_interval");
                }
            }
            else out.unresolved.emplace_back("v4_mixed_unicode_caller_provider_envelope_unavailable");
        }
        if (!out.providerRecordsComplete) out.unresolved.emplace_back("v4_mixed_provider_records_incomplete");
        if (!out.diagnosticRecordsComplete) out.unresolved.emplace_back("v4_mixed_task_diagnostic_snapshot_unavailable");
        if (!out.completionTraceCaptured) out.unresolved.emplace_back("v4_mixed_completion_trace_unavailable");
        out.unresolved.emplace_back("v4_mixed_provider_call_to_selected_occurrence_and_task_unattributed");
        out.unresolved.emplace_back("v4_mixed_missing_presence_cross_thread_completion_and_business_unavailable");
    }
    catch (...) { out.observationUnavailable = true; }
    return out;
}

// Task markers are only compared after the same combined-source Drop is
// bound to one real transfer, read/file works and control-window returns.
inline V4MixedGraphEvidence InspectV4MixedGraphWithTask(const V4MixedOleRunReceipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    const V4IndependentShellSource& fileBinding, const std::shared_ptr<V4IndependentShellOwner>& fileOwner,
    const mixed_fixture::MixedBinding& binding, const std::shared_ptr<mixed_fixture::MixedOwner>& owner,
    OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
    const MessageTrace& trace, HWND control, UINT completionMessage)
{
    auto out = InspectV4MixedGraph(receipt, verified, identity, present, selected, binding, owner, adapter, epoch, trace, control);
    try
    {
        if (!out.identityValid) return out;
        out.taskWindow = InspectV4MixedTaskWindows(receipt, verified, identity, present, selected,
            fileBinding, fileOwner, binding, owner, adapter, epoch, trace, control, completionMessage);
        if (!out.taskWindow->identityValid || !out.taskWindow->localAdapterScopeMatched ||
            !out.taskWindow->directCallsiteMatched || !out.taskWindow->localChain)
        { out.unresolved.emplace_back("v4_mixed_graph_task_chain_unavailable"); return out; }
        std::map<std::string, std::size_t> evidenceById, classes;
        const auto mapping = event_graph::MapV4PresenceCandidate(identity, verified);
        auto requirements = mapping.spec.requirements;
        std::sort(requirements.begin(), requirements.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        if (!mapping.graphAvailable || requirements.size() != present.size())
        { out.unresolved.emplace_back("v4_mixed_graph_task_schema_unavailable"); return out; }
        std::size_t active = 0;
        for (std::size_t i = 0; i < requirements.size(); ++i)
        {
            if (i >= out.events.size() || out.events[i].id != requirements[i].id ||
                !evidenceById.emplace(requirements[i].id, i).second)
            { out.unresolved.emplace_back("v4_mixed_graph_task_nodes_unavailable"); return out; }
            if (present[i])
            { if (active >= selected.classes.size()) return out; classes.emplace(requirements[i].id, selected.classes[active++]); }
        }
        if (active != selected.classes.size()) return out;
        std::map<std::size_t, std::size_t> rank;
        for (std::size_t i = 0; i < selected.order.size(); ++i)
        {
            const auto found = classes.find(selected.order[i]);
            if (found == classes.end() || !rank.emplace(found->second, i).second) return out;
        }
        const auto mark = [&](const std::string& id, V4FileGraphObservationState state, std::size_t index) {
            const auto found = evidenceById.find(id); if (found == evidenceById.end()) return false;
            auto& event = out.events[found->second]; event.state = state; event.logIndex = index; return true;
        };
        const auto before = [&](const std::string& first, const std::string& last, const char* basis) {
            const auto a = classes.find(first), b = classes.find(last);
            if (a == classes.end() || b == classes.end() || !rank.contains(a->second) || !rank.contains(b->second)) return;
            const bool agrees = rank.at(a->second) < rank.at(b->second);
            out.orders.push_back({first, last, basis, agrees}); out.hypothesisObservationRecorded = true;
            if (!agrees) out.hypothesisNotMet = true;
        };
        if (out.taskWindow->localChain)
        {
            const auto& chain = *out.taskWindow->localChain;
            namespace diagnostic = snowdesktop::external_transfer_observation;
            for (const auto& record : out.taskWindow->events)
            {
                const auto& event = record.event;
                if (event.task != chain.task) continue;
                const char* id = nullptr;
                switch (event.phase)
                {
                case diagnostic::Phase::read_queue_enter: id = "v4_read_queue_enter"; break;
                case diagnostic::Phase::read_queued: id = "v4_read_queued"; break;
                case diagnostic::Phase::read_worker_enter: id = "v4_read_worker_enter"; break;
                case diagnostic::Phase::read_content_paths: id = "v4_read_content_paths"; break;
                case diagnostic::Phase::read_worker_return: id = "v4_read_worker_return"; break;
                case diagnostic::Phase::read_completion_callback: id = "v4_read_completion_callback"; break;
                case diagnostic::Phase::commit_content_paths: id = "v4_commit_content_paths"; break;
                case diagnostic::Phase::file_queue_enter: id = "v4_file_queue_enter"; break;
                case diagnostic::Phase::file_queued: id = "v4_file_queued"; break;
                case diagnostic::Phase::file_completion_callback: id = "v4_file_completion_callback"; break;
                case diagnostic::Phase::completion_posted:
                    id = event.extra == chain.readWork ? "v4_read_completion_posted" :
                        event.extra == chain.fileWork ? "v4_file_completion_posted" : nullptr; break;
                case diagnostic::Phase::completion_enter:
                    id = event.extra == chain.readWork ? "v4_read_completion_enter" :
                        event.extra == chain.fileWork ? "v4_file_completion_enter" : nullptr; break;
                case diagnostic::Phase::completion_return:
                    id = event.extra == chain.readWork ? "v4_read_completion_return" :
                        event.extra == chain.fileWork ? "v4_file_completion_return" : nullptr; break;
                case diagnostic::Phase::worker_execute_enter:
                    if (event.extra == chain.fileWork) id = "v4_file_worker_execute_enter"; break;
                case diagnostic::Phase::worker_execute_return:
                    if (event.extra == chain.fileWork) id = "v4_file_worker_execute_return"; break;
                default: break;
                }
                if (id)
                {
                    const bool readBound = out.taskWindow->directCallsiteMatched &&
                        (event.phase == diagnostic::Phase::read_queue_enter || event.phase == diagnostic::Phase::read_queued ||
                         event.phase == diagnostic::Phase::read_worker_enter || event.phase == diagnostic::Phase::read_content_paths ||
                         event.phase == diagnostic::Phase::read_worker_return || event.phase == diagnostic::Phase::read_completion_callback ||
                         ((event.phase == diagnostic::Phase::completion_posted || event.phase == diagnostic::Phase::completion_enter ||
                           event.phase == diagnostic::Phase::completion_return) && event.extra == chain.readWork));
                    const bool fileBound = out.taskWindow->directCallsiteMatched && chain.operationResult &&
                        (event.phase == diagnostic::Phase::commit_content_paths || event.phase == diagnostic::Phase::file_queue_enter ||
                         event.phase == diagnostic::Phase::file_queued || event.phase == diagnostic::Phase::file_completion_callback ||
                         ((event.phase == diagnostic::Phase::worker_execute_enter || event.phase == diagnostic::Phase::worker_execute_return ||
                           event.phase == diagnostic::Phase::completion_posted || event.phase == diagnostic::Phase::completion_enter ||
                           event.phase == diagnostic::Phase::completion_return) && event.extra == chain.fileWork));
                    const bool bound = readBound || fileBound;
                    const auto state = bound ? V4FileGraphObservationState::observed_local :
                        V4FileGraphObservationState::unattributed_task_candidate;
                    if (mark(id, state, record.index) && bound)
                    {
                        out.hypothesisObservationRecorded = true;
                        if (!out.events[evidenceById.at(id)].selectedPresent) out.hypothesisNotMet = true;
                    }
                }
            }
            const auto sameThreadOrder = [&](const char* first, const char* last, const char* basis) {
                const auto a = evidenceById.find(first), b = evidenceById.find(last);
                if (a == evidenceById.end() || b == evidenceById.end()) return;
                const auto& left = out.events[a->second]; const auto& right = out.events[b->second];
                if (left.state != V4FileGraphObservationState::observed_local ||
                    right.state != V4FileGraphObservationState::observed_local || !left.logIndex || !right.logIndex ||
                    *left.logIndex >= *right.logIndex) return;
                const auto findRecord = [&](std::size_t index) {
                    return std::find_if(out.taskWindow->events.begin(), out.taskWindow->events.end(),
                        [&](const auto& record) { return record.index == index; });
                };
                const auto leftRecord = findRecord(*left.logIndex), rightRecord = findRecord(*right.logIndex);
                if (leftRecord != out.taskWindow->events.end() && rightRecord != out.taskWindow->events.end() &&
                    leftRecord->event.thread && leftRecord->event.thread == rightRecord->event.thread)
                    before(first, last, basis);
            };
            sameThreadOrder("v4_read_queue_enter", "v4_read_queued", "same_read_queue_call");
            sameThreadOrder("v4_read_worker_enter", "v4_read_content_paths", "same_read_worker_call");
            sameThreadOrder("v4_read_content_paths", "v4_read_worker_return", "same_read_worker_call");
            sameThreadOrder("v4_read_completion_callback", "v4_read_completion_posted", "same_read_callback");
            sameThreadOrder("v4_read_completion_enter", "v4_read_completion_return", "same_read_completion_call");
            sameThreadOrder("v4_file_queue_enter", "v4_file_queued", "same_file_queue_call");
            sameThreadOrder("v4_file_worker_execute_enter", "v4_file_worker_execute_return", "same_file_worker_call");
            sameThreadOrder("v4_file_completion_callback", "v4_file_completion_posted", "same_file_callback");
            sameThreadOrder("v4_file_completion_enter", "v4_file_completion_return", "same_file_completion_call");
        }
        out.unresolved.emplace_back("v4_mixed_graph_task_reentrancy_late_work_and_business_unresolved");
    }
    catch (...) { out.observationUnavailable = true; }
    return out;
}


struct V4ConfiguredStreamSnapshot
{
    LONG index = -1;
    std::uintptr_t identity = 0;
    std::uint64_t providerCallId = 0;
    bool observationUnavailable = false;
    std::vector<ole_content_source_fixture::StreamRead> reads;
};
struct V4ConfiguredContentSnapshot
{
    bool captured = false, resourceUnavailable = false;
    std::vector<ole_content_source_fixture::Entry> entries;
    std::vector<ole_content_source_fixture::Call> calls;
    std::vector<V4ConfiguredStreamSnapshot> streams;
    std::vector<std::string> unresolved;
};


// A distinct control block for one trusted observer instance. Its constructor
// is private to that observer and it carries no pointers or COM ownership.
class V4VirtualObserverIdentity final
{
    friend struct ::behavior::V4VirtualCopyObservation;
    V4VirtualObserverIdentity() = default;
public:
    V4VirtualObserverIdentity(const V4VirtualObserverIdentity&) = delete;
    V4VirtualObserverIdentity& operator=(const V4VirtualObserverIdentity&) = delete;
    ~V4VirtualObserverIdentity() = default;
};

// A unique per-RunBound capability, created only after the real owner and host
// leases are held. Only the trusted fixture observer may bind its live
// baseline or publish its own immutable raw business snapshot. A caller may
// carry the key, but cannot construct, mutate or seal it.
class V4VirtualExecutionKey final
{
    friend class V4ConfiguredContentGraphExecutor;
    friend class V4ConfiguredContentOleRunReceipt;
    friend struct ::behavior::V4VirtualCopyObservation;
public:
    V4VirtualExecutionKey(const V4VirtualExecutionKey&) = delete;
    V4VirtualExecutionKey& operator=(const V4VirtualExecutionKey&) = delete;
private:
    enum class Phase { created, before_drop, dispatching, business, capturing, closed };
    V4VirtualExecutionKey(const std::shared_ptr<mixed_fixture::ConfiguredContentOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, IDataObject* source,
        OleDragDropAdapter* adapter, std::string candidate, std::string graph)
        : owner_(owner), epoch_(epoch), source_(source), adapter_(adapter), candidate_(std::move(candidate)),
          graph_(std::move(graph)), thread_(GetCurrentThreadId()) {}
    bool SameObjects(const std::shared_ptr<mixed_fixture::ConfiguredContentOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app,
        IDataObject* source, std::string_view candidate) const noexcept
    {
        const auto actualOwner = owner_.lock(); const auto actualEpoch = epoch_.lock();
        return GetCurrentThreadId() == thread_ && actualOwner && actualEpoch && owner && epoch &&
            !actualOwner.owner_before(owner) && !owner.owner_before(actualOwner) &&
            !actualEpoch.owner_before(epoch) && !epoch.owner_before(actualEpoch) &&
            actualEpoch->app_ == reinterpret_cast<std::uintptr_t>(app) && source == source_ &&
            candidate == candidate_ && !graph_.empty() && actualEpoch->Matches(source_, adapter_);
    }
    bool BindBaseline(const std::shared_ptr<mixed_fixture::ConfiguredContentOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app,
        IDataObject* source, std::string_view candidate,
        const std::shared_ptr<const V4VirtualObserverIdentity>& observer) noexcept
    {
        if (phase_ != Phase::before_drop || bound_ || !observer || !SameObjects(owner, epoch, app, source, candidate)) return false;
        observer_ = observer; bound_ = true; return true;
    }
    bool CanCapture(const std::shared_ptr<mixed_fixture::ConfiguredContentOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4VirtualObserverIdentity>& observer) const noexcept
    {
        const auto boundObserver = observer_.lock();
        return phase_ == Phase::business && bound_ && !published_ && observer && boundObserver &&
            !boundObserver.owner_before(observer) && !observer.owner_before(boundObserver) &&
            SameObjects(owner, epoch, app, source, candidate);
    }
    bool BeginCapture(const std::shared_ptr<mixed_fixture::ConfiguredContentOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4VirtualObserverIdentity>& observer) noexcept
    {
        if (!CanCapture(owner, epoch, app, source, candidate, observer)) return false;
        phase_ = Phase::capturing; return true;
    }
    bool Publish(const std::shared_ptr<mixed_fixture::ConfiguredContentOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4VirtualObserverIdentity>& observer,
        std::shared_ptr<const ::behavior::V4VirtualBusinessEvidence> value) noexcept
    {
        const auto boundObserver = observer_.lock();
        if (!value || phase_ != Phase::capturing || !bound_ || published_ || !observer || !boundObserver ||
            boundObserver.owner_before(observer) || observer.owner_before(boundObserver) ||
            !SameObjects(owner, epoch, app, source, candidate)) return false;
        snapshot_ = std::move(value); published_ = true; return true;
    }
    std::weak_ptr<const V4VirtualObserverIdentity> observer_;
    std::weak_ptr<mixed_fixture::ConfiguredContentOwner> owner_;
    std::weak_ptr<const V3HostEpoch> epoch_;
    IDataObject* source_ = nullptr;
    OleDragDropAdapter* adapter_ = nullptr;
    std::string candidate_, graph_;
    DWORD thread_ = 0;
    Phase phase_ = Phase::created;
    bool bound_ = false, published_ = false, committed_ = false;
    std::shared_ptr<const ::behavior::V4VirtualBusinessEvidence> snapshot_;
};

// The callbacks do not themselves establish capability or a passing result;
// only the trusted observer's private bind/publish operations can seal data.
struct V4VirtualObserverBoundaries
{
    std::function<void(const std::shared_ptr<V4VirtualExecutionKey>&)> bindBeforeDrop;
    std::function<void(const std::shared_ptr<V4VirtualExecutionKey>&)> observeBusiness;
};

class V4ConfiguredContentOleRunReceipt final
{
    friend class V4ConfiguredContentGraphExecutor;
public:
    V4ConfiguredContentOleRunReceipt(V4ConfiguredContentOleRunReceipt&&) = default;
    V4ConfiguredContentOleRunReceipt& operator=(V4ConfiguredContentOleRunReceipt&&) = delete;
    V4ConfiguredContentOleRunReceipt(const V4ConfiguredContentOleRunReceipt&) = delete;
    V4ConfiguredContentOleRunReceipt& operator=(const V4ConfiguredContentOleRunReceipt&) = delete;
    const Result& Execution() const noexcept { return execution_; }
    const V4JoinedOleExecutionPlan& Plan() const noexcept { return plan_; }
    // Returned only after the same real execution and trusted callback sealed
    // a genuinely immutable value; no caller-supplied result is accepted.
    std::shared_ptr<const ::behavior::V4VirtualBusinessEvidence> SealedBusiness() const noexcept
    {
        return execution_.productionStarted && plan_.graphIdentityValid && executionKey_ &&
            executionKey_->phase_ == V4VirtualExecutionKey::Phase::closed && executionKey_->published_ && executionKey_->committed_
            ? executionKey_->snapshot_ : nullptr;
    }
    const std::optional<V4ConfiguredContentSnapshot>& Initial() const noexcept { return initial_; }
    const std::optional<V4ConfiguredContentSnapshot>& AtExecution() const noexcept { return atExecution_; }
    // This is a callback return, not proof that the host is quiescent.
    bool AwaitReturned() const noexcept { return awaitReturned_; }
    const std::optional<V4ConfiguredContentSnapshot>& AfterAwait() const noexcept { return afterAwait_; }
    // Captured after dispatch returns even when Await or an oracle failed;
    // this is an instantaneous partial snapshot, never producer quiescence.
    const std::optional<V4ConfiguredContentSnapshot>& AfterDispatch() const noexcept { return afterDispatch_; }
    const std::vector<V2CapturedOleReturn>& CapturedReturns() const noexcept { return captured_; }
    const std::vector<MessageObservation>& CompletionTrace() const noexcept { return completionTrace_; }
    bool CompletionTraceCaptured() const noexcept { return completionTraceCaptured_; }
    bool SameBoundHost(const mixed_fixture::ConfiguredContentBinding& binding,
        const std::shared_ptr<mixed_fixture::ConfiguredContentOwner>& owner, OleDragDropAdapter* adapter,
        const std::shared_ptr<const V3HostEpoch>& epoch, const MessageTrace& trace, HWND control) const noexcept
    {
        auto* source = binding.SourceObject();
        return execution_.productionStarted && plan_.graphIdentityValid && source && owner && epoch &&
            owner_.lock() == owner && epoch_.lock() == epoch && source == source_ && adapter == adapter_ &&
            binding.candidateId == plan_.candidateId && epoch->Matches(source, adapter, &trace, control);
    }
    std::uintptr_t HostIdentity() const noexcept
    { const auto live = epoch_.lock(); return live ? live->app_ : 0; }
private:
    V4ConfiguredContentOleRunReceipt() = default;
    Result execution_;
    V4JoinedOleExecutionPlan plan_;
    std::shared_ptr<V4VirtualExecutionKey> executionKey_;
    std::optional<V4ConfiguredContentSnapshot> initial_, atExecution_, afterAwait_, afterDispatch_;
    std::vector<V2CapturedOleReturn> captured_;
    std::vector<MessageObservation> completionTrace_;
    bool awaitReturned_ = false, completionTraceCaptured_ = false;
    std::weak_ptr<mixed_fixture::ConfiguredContentOwner> owner_;
    std::weak_ptr<const V3HostEpoch> epoch_;
    ole_content_source_fixture::Source* source_ = nullptr;
    OleDragDropAdapter* adapter_ = nullptr;
};

class V4ConfiguredContentGraphExecutor final
{
    static V4ConfiguredContentSnapshot Capture(ole_content_source_fixture::Source& source,
        const mixed_fixture::ConfiguredContentBinding& binding, bool requireEmpty)
    {
        V4ConfiguredContentSnapshot out;
        try
        {
            if (source.ownerThread != GetCurrentThreadId() || source.wrongApartment.load() || source.observationFailed.load())
            { out.unresolved.emplace_back("v4_content_source_thread_or_log_unavailable"); return out; }
            out.entries = source.entries;
            std::vector<std::shared_ptr<ole_content_source_fixture::StreamObservation>> streams;
            {
                std::lock_guard lock(source.observationMutex);
                out.calls = source.calls; streams = source.streamObservations;
            }
            for (const auto& stream : streams)
            {
                V4ConfiguredStreamSnapshot item;
                if (!stream) { item.observationUnavailable = true; out.streams.push_back(std::move(item)); continue; }
                item.index = stream->index; item.identity = reinterpret_cast<std::uintptr_t>(stream.get());
                item.providerCallId = stream->providerCallId;
                { std::lock_guard lock(stream->mutex); item.reads = stream->reads; }
                item.observationUnavailable = stream->observationFailed.load();
                out.streams.push_back(std::move(item));
            }
            const auto& expected = binding.ConfiguredEntries();
            bool equal = out.entries.size() == expected.size();
            if (equal) for (std::size_t i = 0; i < expected.size(); ++i)
            {
                const auto& a = out.entries[i]; const auto& b = expected[i];
                if (a.format != b.format || a.index != b.index || a.tymed != b.tymed || a.bytes != b.bytes ||
                    a.readFailure != b.readFailure || a.getDataFailure != b.getDataFailure)
                { equal = false; break; }
            }
            if (!equal || (requireEmpty && (!out.calls.empty() || !out.streams.empty())) ||
                std::any_of(out.streams.begin(), out.streams.end(), [](const auto& stream) { return stream.observationUnavailable; }) ||
                source.wrongApartment.load() || source.observationFailed.load())
            { out.unresolved.emplace_back("v4_content_entries_or_log_changed"); return out; }
            out.captured = true;
            out.unresolved.emplace_back("v4_content_snapshot_non_atomic_and_not_production_selection");
            out.unresolved.emplace_back("v4_content_stream_read_call_identity_and_copyto_unobserved");
        }
        catch (const std::bad_alloc&) { out.resourceUnavailable = true; }
        catch (const std::length_error&) { out.resourceUnavailable = true; }
        catch (...) { out.unresolved.emplace_back("v4_content_snapshot_unavailable"); }
        return out;
    }
public:
    static V4ConfiguredContentOleRunReceipt RunBound(const V4FamilyValidatedContext& verified,
        const event_graph::V4FamilyIdentityContext& identity, const std::vector<bool>& present,
        const event_graph::V4JoinedGraphVariant& selected, const std::vector<Input>& inputs,
        const mixed_fixture::ConfiguredContentBinding& binding,
        const std::shared_ptr<mixed_fixture::ConfiguredContentOwner>& owner, OleDragDropAdapter* adapter,
        const std::shared_ptr<const V3HostEpoch>& epoch, const HostBoundaries& host,
        const std::function<void(const Observation&)>& assertIndependentOleResult,
        const std::function<V2HostCoordinateSnapshot(const Observation&)>& sampler = {},
        const V4VirtualObserverBoundaries& virtualObserver = {})
    {
        V4ConfiguredContentOleRunReceipt receipt;
        const auto pinnedOwner = owner; const auto pinnedEpoch = epoch;
        bool dispatchEntered = false;
        try
        {
            receipt.plan_ = BuildV4JoinedOleExecutionPlan(verified, identity, present, selected, inputs);
            receipt.execution_.obligations = receipt.plan_.preconditions;
            receipt.execution_.postconditionsPending = receipt.plan_.postDispatchObligations;
            receipt.execution_.unclassifiedObligations = receipt.plan_.unclassified;
            auto* source = binding.SourceObject();
            if (!pinnedOwner || binding.owner_.lock() != pinnedOwner || !binding.inputPrepared || binding.resourceUnavailable ||
                !source || pinnedOwner->source_.Get() != source || binding.candidateId != receipt.plan_.candidateId)
                receipt.execution_.obligations.emplace_back("v4_content_owner_source_or_candidate_unavailable");
            if (source)
            {
                receipt.initial_ = Capture(*source, binding, true);
                if (!receipt.initial_->captured || receipt.initial_->resourceUnavailable)
                    receipt.execution_.obligations.emplace_back("v4_content_initial_snapshot_unavailable");
            }
            if (Value(verified.Axes(), verified.Coordinate(), "V4.source.kind") != "content_only" ||
                Value(verified.Axes(), verified.Coordinate(), "T01") != "external_ingress")
                receipt.execution_.obligations.emplace_back("v4_content_external_source_unimplemented");
            if (!source || !adapter || !pinnedEpoch || !pinnedEpoch->Matches(source, adapter))
                receipt.execution_.obligations.emplace_back("v4_content_host_epoch_unavailable");
            if (!host.beforeOleCall || !host.observeOleReturn || !host.awaitProductionCompletionOrRejection ||
                !host.assertIndependentBusinessResults || !assertIndependentOleResult)
                receipt.execution_.obligations.emplace_back("v4_content_host_observer_or_oracle_missing");
            if (Value(verified.Axes(), verified.Coordinate(), "T02.virtual_files") == "yes" &&
                (!virtualObserver.bindBeforeDrop || !virtualObserver.observeBusiness))
                receipt.execution_.obligations.emplace_back("v4_virtual_trusted_business_capture_unavailable");
            receipt.execution_.obligations.emplace_back("v4_content_complete_input_axes_and_source_mutation_unimplemented");
            receipt.execution_.obligations.emplace_back("v4_content_selected_graph_presence_and_format_unimplemented");
            receipt.execution_.obligations.emplace_back("v4_content_same_execution_completion_and_business_oracle_unimplemented");
            if (!receipt.plan_.graphIdentityValid || !receipt.plan_.structurallyPlanned || !receipt.execution_.obligations.empty())
                return receipt;
            if (!pinnedOwner->Begin(false))
            { receipt.execution_.additionalObservationUnavailable = true; receipt.execution_.obligations.emplace_back("v4_content_owner_busy"); return receipt; }
            const auto endOwner = [](const mixed_fixture::ConfiguredContentOwner* active) noexcept { active->End(); };
            const std::unique_ptr<const mixed_fixture::ConfiguredContentOwner, decltype(endOwner)> sourceGuard(pinnedOwner.get(), endOwner);
            if (!pinnedEpoch->TryBeginExecution(source, adapter))
            { receipt.execution_.additionalObservationUnavailable = true; receipt.execution_.obligations.emplace_back("v4_content_host_epoch_busy"); return receipt; }
            const auto endHost = [](const V3HostEpoch* active) noexcept { active->EndExecution(); };
            const std::unique_ptr<const V3HostEpoch, decltype(endHost)> hostGuard(pinnedEpoch.get(), endHost);
            receipt.atExecution_ = Capture(*source, binding, true);
            if (!receipt.atExecution_->captured || receipt.atExecution_->resourceUnavailable)
            { receipt.execution_.additionalObservationUnavailable = true; receipt.execution_.obligations.emplace_back("v4_content_execution_snapshot_unavailable"); return receipt; }
            receipt.owner_ = pinnedOwner; receipt.epoch_ = pinnedEpoch; receipt.source_ = source; receipt.adapter_ = adapter;
            if (virtualObserver.bindBeforeDrop && virtualObserver.observeBusiness)
                receipt.executionKey_ = std::shared_ptr<V4VirtualExecutionKey>(new V4VirtualExecutionKey(
                    pinnedOwner, pinnedEpoch, static_cast<IDataObject*>(source), adapter,
                    receipt.plan_.candidateId, receipt.plan_.graphId));
            const auto requireLive = [&] {
                if (binding.SourceObject() != source || !pinnedEpoch->Matches(source, adapter))
                    throw ObservationUnavailable("v4_content_execution_source_or_host_changed");
            };
            const HostBoundaries guarded{
                [&](const Input& input) {
                    requireLive(); host.beforeOleCall(input); requireLive();
                    if (input.kind == Kind::drop && receipt.executionKey_)
                    {
                        auto& key = *receipt.executionKey_;
                        if (key.phase_ != V4VirtualExecutionKey::Phase::created)
                            throw ObservationUnavailable("v4_virtual_execution_duplicate_drop_unavailable");
                        key.phase_ = V4VirtualExecutionKey::Phase::before_drop;
                        virtualObserver.bindBeforeDrop(receipt.executionKey_);
                        if (!key.bound_) throw ObservationUnavailable("v4_virtual_execution_baseline_not_bound");
                        requireLive(); key.phase_ = V4VirtualExecutionKey::Phase::dispatching;
                    }
                },
                [&](const Observation& event) { requireLive(); host.observeOleReturn(event); requireLive(); },
                [&](const Observation& terminal) {
                    requireLive(); host.awaitProductionCompletionOrRejection(terminal); receipt.awaitReturned_ = true; requireLive();
                    receipt.afterAwait_ = Capture(*source, binding, false);
                    if (pinnedEpoch->trace_ && !pinnedEpoch->trace_->LostOrCrossThread() &&
                        pinnedEpoch->trace_->OwnerThread() == GetCurrentThreadId())
                    { receipt.completionTrace_ = pinnedEpoch->trace_->Observations(); receipt.completionTraceCaptured_ = true; }
                    requireLive();
                    if (!receipt.afterAwait_->captured || receipt.afterAwait_->resourceUnavailable || !receipt.completionTraceCaptured_)
                        throw ObservationUnavailable("v4_content_provider_or_completion_trace_unavailable");
                },
                [&] {
                    requireLive();
                    if (!receipt.executionKey_) { host.assertIndependentBusinessResults(); requireLive(); return; }
                    auto& key = *receipt.executionKey_;
                    if (key.phase_ != V4VirtualExecutionKey::Phase::dispatching || !key.bound_ ||
                        !receipt.awaitReturned_ || !receipt.completionTraceCaptured_)
                        throw ObservationUnavailable("v4_virtual_business_execution_scope_unavailable");
                    key.phase_ = V4VirtualExecutionKey::Phase::business;
                    struct CloseScope final { V4VirtualExecutionKey& key; ~CloseScope() { key.phase_ = V4VirtualExecutionKey::Phase::closed; } } close{key};
                    virtualObserver.observeBusiness(receipt.executionKey_);
                    requireLive();
                    if (!key.published_ || !key.snapshot_)
                        throw ObservationUnavailable("v4_virtual_business_snapshot_not_sealed");
                }};
            const std::function<void(const Observation&)> oracle = [&](const Observation& event) {
                requireLive(); assertIndependentOleResult(event); requireLive(); };
            const std::function<V2HostCoordinateSnapshot(const Observation&)> guardedSampler = sampler
                ? std::function<V2HostCoordinateSnapshot(const Observation&)>{[&](const Observation& event) {
                    requireLive(); auto value = sampler(event); requireLive(); return value; }}
                : std::function<V2HostCoordinateSnapshot(const Observation&)>{};
            dispatchEntered = true;
            receipt.execution_ = V2GraphExecutor::DispatchApprovedPlan(receipt.plan_.steps, inputs.size(), adapter,
                static_cast<IDataObject*>(source), guarded, oracle, &guardedSampler, &receipt.captured_);
            requireLive();
            if (receipt.execution_.productionStarted)
            {
                try
                {
                    receipt.afterDispatch_ = Capture(*source, binding, false);
                    if (!receipt.afterDispatch_->captured || receipt.afterDispatch_->resourceUnavailable)
                        receipt.execution_.additionalObservationUnavailable = true;
                }
                catch (...) { receipt.execution_.additionalObservationUnavailable = true; }
            }
            const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
            if (!replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
                replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
                replay.variant.unresolved != selected.unresolved)
            { receipt.plan_.graphIdentityValid = false; receipt.execution_.additionalObservationUnavailable = true; }
            else if (receipt.executionKey_ && receipt.execution_.productionStarted &&
                receipt.executionKey_->phase_ == V4VirtualExecutionKey::Phase::closed && receipt.executionKey_->published_)
                receipt.executionKey_->committed_ = true;
        }
        catch (const std::exception& error)
        {
            if (dispatchEntered && !receipt.execution_.productionStarted) receipt.execution_.productionEntryUnknown = true;
            receipt.execution_.additionalObservationUnavailable = true;
            if (receipt.execution_.status != Status::event_failed && receipt.execution_.status != Status::event_oracle_failed &&
                receipt.execution_.status != Status::business_failed) receipt.execution_.status = Status::observation_unavailable;
            try { if (receipt.execution_.failure.empty()) receipt.execution_.failure = error.what(); } catch (...) {}
        }
        catch (...)
        {
            if (dispatchEntered && !receipt.execution_.productionStarted) receipt.execution_.productionEntryUnknown = true;
            receipt.execution_.additionalObservationUnavailable = true;
            if (receipt.execution_.status != Status::event_failed && receipt.execution_.status != Status::event_oracle_failed &&
                receipt.execution_.status != Status::business_failed) receipt.execution_.status = Status::observation_unavailable;
            try { if (receipt.execution_.failure.empty()) receipt.execution_.failure = "unknown_v4_content_execution_exception"; } catch (...) {}
        }
        return receipt;
    }
};


struct V4ConfiguredProviderCall
{
    std::size_t callIndex = 0, enterIndex = 0, returnIndex = 0;
    std::uint64_t callId = 0;
};
struct V4ConfiguredContentObservation
{
    bool identityValid = false, awaitReturned = false, providerIntervalObserved = false;
    bool observationUnavailable = true;
    std::vector<V2CapturedOleReturn> oleEvidence;
    std::optional<V4ConfiguredContentSnapshot> afterAwait;
    std::vector<V4MixedDiagnosticRecord> records;
    std::vector<V4ConfiguredProviderCall> providerCalls;
    std::vector<MessageObservation> completionTrace;
    std::vector<std::string> unresolved;
};

// Correlates direct provider GetData markers to the actual same-thread Drop
// scope and its value-only calls. Provider return is an internal marker, not
// caller ABI return, stream EOF, completed materialization or business result.
inline V4ConfiguredContentObservation InspectV4ConfiguredContent(const V4ConfiguredContentOleRunReceipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    const mixed_fixture::ConfiguredContentBinding& binding,
    const std::shared_ptr<mixed_fixture::ConfiguredContentOwner>& owner, OleDragDropAdapter* adapter,
    const std::shared_ptr<const V3HostEpoch>& epoch, const MessageTrace& trace, HWND control)
{
    namespace diagnostic = snowdesktop::external_transfer_observation;
    V4ConfiguredContentObservation out;
    try
    {
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || raw.id != receipt.Plan().candidateId ||
            receipt.Plan().graphId != selected.id || !replay.identityValid || !replay.errors.empty() ||
            replay.variant.id != selected.id || replay.variant.classes != selected.classes ||
            replay.variant.order != selected.order || replay.variant.unresolved != selected.unresolved ||
            !receipt.SameBoundHost(binding, owner, adapter, epoch, trace, control))
        { out.unresolved.emplace_back("v4_content_postflight_identity_or_host_unavailable"); return out; }
        out.identityValid = true; out.awaitReturned = receipt.AwaitReturned();
        out.oleEvidence = receipt.CapturedReturns(); out.afterAwait = receipt.AfterAwait();
        out.completionTrace = receipt.CompletionTrace();
        if (!out.awaitReturned || !out.afterAwait || !out.afterAwait->captured ||
            out.afterAwait->resourceUnavailable || !receipt.CompletionTraceCaptured() || !diagnostic::Complete())
        { out.unresolved.emplace_back("v4_content_postflight_snapshot_or_diagnostic_unavailable"); return out; }
        const V2CapturedOleReturn* drop = nullptr;
        for (const auto& item : out.oleEvidence)
            if (item.observation.kind == Kind::drop)
            { if (drop) { out.unresolved.emplace_back("v4_content_postflight_multiple_drop_unavailable"); return out; } drop = &item; }
        if (!drop || drop->entered.kind != snowdesktop::ole_adapter_observation::Kind::drop ||
            drop->entered.phase != snowdesktop::ole_adapter_observation::Phase::entered ||
            drop->returned.phase != snowdesktop::ole_adapter_observation::Phase::returned ||
            drop->returned.kind != drop->entered.kind || !drop->entered.session || !drop->entered.call ||
            drop->entered.parent || drop->entered.depth || drop->entered.thread != GetCurrentThreadId() ||
            drop->entered.adapter != reinterpret_cast<std::uintptr_t>(adapter) ||
            drop->returned.session != drop->entered.session || drop->returned.call != drop->entered.call ||
            drop->returned.thread != drop->entered.thread || drop->returned.adapter != drop->entered.adapter ||
            drop->returned.result != drop->observation.result)
        { out.unresolved.emplace_back("v4_content_postflight_drop_scope_unavailable"); return out; }
        auto* source = binding.SourceObject();
        if (!source) { out.unresolved.emplace_back("v4_content_postflight_source_unavailable"); return out; }
        const auto address = reinterpret_cast<std::uintptr_t>(static_cast<IDataObject*>(source));
        diagnostic::ReadSession session;
        if (!session.Open()) { out.unresolved.emplace_back("v4_content_postflight_session_unavailable"); return out; }
        const auto count = session.Count();
        if (count > diagnostic::EventLimit()) { out.unresolved.emplace_back("v4_content_postflight_log_overflow"); return out; }
        out.records.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            V4MixedDiagnosticRecord item; item.index = i;
            if (!session.ReadEvent(i, item.event)) { out.unresolved.emplace_back("v4_content_postflight_log_lost"); return out; }
            if (item.event.hasBlob)
            {
                if (item.event.blobSize > diagnostic::BlobLimit()) { out.unresolved.emplace_back("v4_content_postflight_blob_unavailable"); return out; }
                item.blob.resize(item.event.blobSize); std::size_t length = 0;
                if (!session.CopyBlob(i, item.blob.data(), item.blob.size(), length) || length != item.blob.size())
                { out.unresolved.emplace_back("v4_content_postflight_blob_unavailable"); return out; }
                item.blobCopied = true;
            }
            out.records.push_back(std::move(item));
        }
        std::vector<std::size_t> gets;
        for (std::size_t i = 0; i < out.afterAwait->calls.size(); ++i)
            if (!out.afterAwait->calls[i].query) gets.push_back(i);
        std::vector<const V4MixedDiagnosticRecord*> markers;
        for (const auto& record : out.records)
            if ((record.event.phase == diagnostic::Phase::content_provider_getdata_enter ||
                record.event.phase == diagnostic::Phase::content_provider_getdata_return) && record.event.object == address)
                markers.push_back(&record);
        if (gets.empty() || markers.size() != gets.size() * 2)
        { out.unresolved.emplace_back("v4_content_postflight_getdata_count_unavailable"); return out; }
        for (std::size_t i = 0; i < gets.size(); ++i)
        {
            const auto& first = *markers[2 * i]; const auto& last = *markers[2 * i + 1];
            const auto& call = out.afterAwait->calls[gets[i]];
            const auto sameScope = [&](const diagnostic::Event& event) {
                const auto& scope = event.adapterScope;
                return event.object == address && event.thread == drop->entered.thread && scope.observed &&
                    scope.session == drop->entered.session && scope.call == drop->entered.call &&
                    scope.parent == drop->entered.parent && scope.depth == drop->entered.depth &&
                    scope.thread == drop->entered.thread && scope.adapter == drop->entered.adapter &&
                    scope.kind == static_cast<unsigned>(snowdesktop::ole_adapter_observation::Kind::drop);
            };
            const auto& a = first.event.adapterScope; const auto& b = last.event.adapterScope;
            if (first.event.phase != diagnostic::Phase::content_provider_getdata_enter ||
                last.event.phase != diagnostic::Phase::content_provider_getdata_return ||
                !first.event.extra || first.event.extra != last.event.extra || first.index >= last.index ||
                (i && markers[2 * i - 1]->index >= first.index) ||
                first.event.thread != drop->entered.thread || last.event.thread != first.event.thread ||
                a.observed != b.observed || a.session != b.session || a.call != b.call || a.parent != b.parent ||
                a.adapter != b.adapter || a.thread != b.thread || a.depth != b.depth || a.kind != b.kind ||
                call.thread != first.event.thread || static_cast<std::int64_t>(call.result) != last.event.value)
            { out.providerCalls.clear(); out.unresolved.emplace_back("v4_content_postflight_getdata_pair_unavailable"); return out; }
            // Calls during Enter/Over must remain in the pairing sequence, but
            // do not belong to the selected Drop occurrence.
            if (sameScope(first.event) && sameScope(last.event))
                out.providerCalls.push_back({gets[i], first.index, last.index, first.event.extra});
        }
        out.providerIntervalObserved = !out.providerCalls.empty();
        if (!out.providerIntervalObserved) out.unresolved.emplace_back("v4_content_drop_provider_not_observed");
        out.unresolved.emplace_back("v4_content_provider_caller_abi_and_uninstrumented_reentry_unverified");
        out.unresolved.emplace_back("v4_content_stream_identity_completion_refresh_and_business_unverified");
    }
    catch (...) { out.observationUnavailable = true; }
    return out;
}


struct V4VirtualFlowMember
{
    std::size_t descriptorIndex = 0, outputIndex = 0;
    std::uint64_t memberId = 0, providerCallId = 0;
    std::vector<std::uint64_t> readIds;
    std::vector<std::byte> stagedPath, pipelinePath, actualDestination;
    std::optional<FILE_ID_INFO> stagedFileId;
    std::optional<DWORD> stagedAttributes;
    std::optional<bool> referencesSource;
    bool observedReadValuesMatch = false, terminalReadObserved = false;
};
struct V4VirtualFlowEvidence
{
    bool identityValid = false, batchObserved = false, pipelineReturned = false, callbackReturned = false;
    bool observationUnavailable = true;
    std::uint64_t batch = 0;
    std::uintptr_t operationResult = 0;
    bool outputMappingObserved = false;
    std::vector<V4VirtualFlowMember> members;
    std::optional<V4ConfiguredContentObservation> content;
    std::vector<std::string> unresolved;
};

// Raw, bounded observations made through handles retained from materialization.
// A matching named object at inspection time is not proof of the object opened
// by Shell in between, and neither snapshot is an atomic file snapshot.
struct V4VirtualStagingHandleEvidence
{
    std::uint64_t batch = 0, member = 0;
    std::uintptr_t source = 0;
    DWORD thread = 0;
    FILE_ID_INFO handleIdentity{}, namedIdentity{};
    std::uint64_t actualSize = 0;
    std::vector<std::byte> actualBytes;
    bool handleIdentityObserved = false, namedIdentityObserved = false;
    bool bytesComplete = false, nameMatchesHandle = false;
};
struct V4VirtualStagingHandlesEvidence
{
    bool logComplete = false, observationUnavailable = false;
    std::vector<V4VirtualStagingHandleEvidence> records;
};

inline V4VirtualStagingHandlesEvidence InspectV4VirtualStagingHandles(
    const V4VirtualFlowEvidence& flow, std::size_t byteBudget)
{
    namespace diagnostic = snowdesktop::external_transfer_observation;
    V4VirtualStagingHandlesEvidence result;
    result.logComplete = diagnostic::Complete();
    try
    {
        diagnostic::ReadSession session;
        if (!session.Open(true)) { result.observationUnavailable = true; return result; }
        for (std::size_t i = 0; i < session.StagingCount(); ++i)
        {
            diagnostic::StagingHandleObservation raw{};
            if (!session.DuplicateStagingHandle(i, raw)) { result.observationUnavailable = true; continue; }
            struct HandleGuard final { HANDLE value = INVALID_HANDLE_VALUE; ~HandleGuard() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } } duplicate{raw.handle};
            // DuplicateHandle shares the underlying file pointer. Reopen by
            // handle for an independent pointer and verify the actual ID.
            HandleGuard independent{ReOpenFile(duplicate.value, GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_FLAG_OPEN_REPARSE_POINT)};
            FILE_ID_INFO duplicateId{}, independentId{};
            if (independent.value == INVALID_HANDLE_VALUE ||
                !GetFileInformationByHandleEx(duplicate.value, FileIdInfo, &duplicateId, sizeof(duplicateId)) ||
                !GetFileInformationByHandleEx(independent.value, FileIdInfo, &independentId, sizeof(independentId)) ||
                duplicateId.VolumeSerialNumber != independentId.VolumeSerialNumber ||
                std::memcmp(duplicateId.FileId.Identifier, independentId.FileId.Identifier, sizeof(duplicateId.FileId.Identifier)) != 0)
            { result.observationUnavailable = true; continue; }
            raw.handle = independent.value;
            V4VirtualStagingHandleEvidence evidence;
            evidence.batch = raw.batch; evidence.member = raw.member; evidence.source = raw.source; evidence.thread = raw.thread;
            FILE_STANDARD_INFO before{}, after{}; FILE_BASIC_INFO basicBefore{}, basicAfter{};
            FILE_ATTRIBUTE_TAG_INFO attributes{};
            evidence.handleIdentityObserved = GetFileInformationByHandleEx(raw.handle, FileIdInfo,
                &evidence.handleIdentity, sizeof(evidence.handleIdentity)) != FALSE;
            const bool metadata = GetFileInformationByHandleEx(raw.handle, FileStandardInfo, &before, sizeof(before)) &&
                GetFileInformationByHandleEx(raw.handle, FileBasicInfo, &basicBefore, sizeof(basicBefore)) &&
                GetFileInformationByHandleEx(raw.handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) &&
                !(attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) && before.EndOfFile.QuadPart >= 0;
            if (metadata)
            {
                evidence.actualSize = static_cast<std::uint64_t>(before.EndOfFile.QuadPart);
                if (evidence.actualSize <= byteBudget && evidence.actualSize <= (std::numeric_limits<std::size_t>::max)())
                {
                    evidence.actualBytes.resize(static_cast<std::size_t>(evidence.actualSize));
                    LARGE_INTEGER zero{}; std::size_t offset = 0;
                    bool readOk = SetFilePointerEx(raw.handle, zero, nullptr, FILE_BEGIN) != FALSE;
                    while (readOk && offset < evidence.actualBytes.size())
                    {
                        const auto count = (std::min)(evidence.actualBytes.size() - offset, static_cast<std::size_t>(MAXDWORD));
                        DWORD read = 0;
                        readOk = ReadFile(raw.handle, evidence.actualBytes.data() + offset, static_cast<DWORD>(count), &read, nullptr) && read != 0;
                        offset += read;
                    }
                    evidence.bytesComplete = readOk && offset == evidence.actualBytes.size() &&
                        GetFileInformationByHandleEx(raw.handle, FileStandardInfo, &after, sizeof(after)) &&
                        GetFileInformationByHandleEx(raw.handle, FileBasicInfo, &basicAfter, sizeof(basicAfter)) &&
                        before.EndOfFile.QuadPart == after.EndOfFile.QuadPart &&
                        basicBefore.LastWriteTime.QuadPart == basicAfter.LastWriteTime.QuadPart &&
                        basicBefore.ChangeTime.QuadPart == basicAfter.ChangeTime.QuadPart;
                    evidence.actualBytes.resize(offset); // never expose unread, zero-filled tail as actual data
                    byteBudget -= evidence.actualBytes.size();
                }
            }
            const V4VirtualFlowMember* member = nullptr;
            for (const auto& candidate : flow.members)
                if (flow.batch == raw.batch && candidate.memberId == raw.member)
                { if (member) { member = nullptr; break; } member = &candidate; }
            if (member && !member->stagedPath.empty() && member->stagedPath.size() % sizeof(wchar_t) == 0)
            {
                std::wstring path(member->stagedPath.size() / sizeof(wchar_t), L'\0');
                std::memcpy(path.data(), member->stagedPath.data(), member->stagedPath.size());
                if (path.find(L'\0') == std::wstring::npos)
                {
                    HANDLE named = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                    if (named != INVALID_HANDLE_VALUE)
                    {
                        FILE_ATTRIBUTE_TAG_INFO namedAttributes{};
                        evidence.namedIdentityObserved = GetFileInformationByHandleEx(named, FileIdInfo,
                            &evidence.namedIdentity, sizeof(evidence.namedIdentity)) &&
                            GetFileInformationByHandleEx(named, FileAttributeTagInfo, &namedAttributes, sizeof(namedAttributes)) &&
                            !(namedAttributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
                        CloseHandle(named);
                    }
                }
            }
            evidence.nameMatchesHandle = evidence.handleIdentityObserved && evidence.namedIdentityObserved &&
                std::any_of(std::begin(evidence.handleIdentity.FileId.Identifier), std::end(evidence.handleIdentity.FileId.Identifier),
                    [](BYTE value) { return value != 0; }) &&
                evidence.handleIdentity.VolumeSerialNumber == evidence.namedIdentity.VolumeSerialNumber &&
                std::memcmp(evidence.handleIdentity.FileId.Identifier, evidence.namedIdentity.FileId.Identifier,
                    sizeof(evidence.handleIdentity.FileId.Identifier)) == 0;
            if (!evidence.handleIdentityObserved || !evidence.namedIdentityObserved || !evidence.bytesComplete)
                result.observationUnavailable = true;
            result.records.push_back(std::move(evidence));
        }
        if (!result.logComplete) result.observationUnavailable = true;
    }
    catch (...) { result.observationUnavailable = true; }
    return result;
}

// A positive local correlation for the restricted synchronous virtual-file
// path. It does not establish isolated staging, durable files, final model or
// absence of late work. Caller must independently verify those outcomes.
inline V4VirtualFlowEvidence InspectV4VirtualFlow(const V4ConfiguredContentOleRunReceipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    const mixed_fixture::ConfiguredContentBinding& binding,
    const std::shared_ptr<mixed_fixture::ConfiguredContentOwner>& owner, OleDragDropAdapter* adapter,
    const std::shared_ptr<const V3HostEpoch>& epoch, const MessageTrace& trace, HWND control,
    const mixed_fixture::VirtualExpectedManifest& supplied, std::size_t byteBudget, std::size_t virtualBudget)
{
    namespace diagnostic = snowdesktop::external_transfer_observation;
    V4VirtualFlowEvidence out;
    try
    {
        const auto manifest = mixed_fixture::BuildVirtualExpectedManifest(verified, identity, byteBudget, virtualBudget);
        if (!manifest.ready || manifest.resourceUnavailable || !supplied.ready || supplied.resourceUnavailable ||
            manifest.candidateId != supplied.candidateId || manifest.candidateId != receipt.Plan().candidateId ||
            manifest.members.size() != supplied.members.size())
        { out.unresolved.emplace_back("v4_virtual_flow_manifest_unavailable"); return out; }
        for (std::size_t i = 0; i < manifest.members.size(); ++i)
            if (manifest.members[i].descriptorIndex != supplied.members[i].descriptorIndex ||
                manifest.members[i].name != supplied.members[i].name || manifest.members[i].bytes != supplied.members[i].bytes)
            { out.unresolved.emplace_back("v4_virtual_flow_manifest_mismatch"); return out; }
        out.content = InspectV4ConfiguredContent(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control);
        if (!out.content->identityValid || !out.content->awaitReturned || !out.content->providerIntervalObserved ||
            !out.content->afterAwait || !out.content->afterAwait->captured || !receipt.CompletionTraceCaptured())
        { out.unresolved.emplace_back("v4_virtual_flow_content_or_completion_unavailable"); return out; }
        auto* source = binding.SourceObject();
        if (!source) { out.unresolved.emplace_back("v4_virtual_flow_source_unavailable"); return out; }
        const auto sourceAddress = reinterpret_cast<std::uintptr_t>(static_cast<IDataObject*>(source));
        const V2CapturedOleReturn* drop = nullptr;
        for (const auto& item : out.content->oleEvidence)
            if (item.observation.kind == Kind::drop)
            { if (drop) { out.unresolved.emplace_back("v4_virtual_flow_drop_ambiguous"); return out; } drop = &item; }
        if (!drop) { out.unresolved.emplace_back("v4_virtual_flow_drop_unavailable"); return out; }
        const auto sameDrop = [&](const diagnostic::Event& event) {
            const auto& scope = event.adapterScope;
            return event.object == sourceAddress && event.thread == drop->entered.thread && scope.observed &&
                scope.session == drop->entered.session && scope.call == drop->entered.call && scope.parent == drop->entered.parent &&
                scope.depth == drop->entered.depth && scope.thread == drop->entered.thread && scope.adapter == drop->entered.adapter &&
                scope.kind == static_cast<unsigned>(snowdesktop::ole_adapter_observation::Kind::drop);
        };
        const auto& records = out.content->records;
        const V4MixedDiagnosticRecord* batchEnter = nullptr; const V4MixedDiagnosticRecord* batchReturn = nullptr;
        for (const auto& r : records)
            if (r.event.phase == diagnostic::Phase::virtual_batch_enter)
            { if (batchEnter) { out.unresolved.emplace_back("v4_virtual_flow_multiple_batches"); return out; } batchEnter = &r; }
        if (!batchEnter || !batchEnter->event.task || !sameDrop(batchEnter->event) ||
            batchEnter->event.value != diagnostic::ObservedSize(manifest.members.size()))
        { out.unresolved.emplace_back("v4_virtual_flow_batch_unavailable"); return out; }
        out.batch = batchEnter->event.task;
        const V4MixedDiagnosticRecord* pipelineEnter = nullptr; const V4MixedDiagnosticRecord* pipelineReturn = nullptr;
        const V4MixedDiagnosticRecord* completionEnter = nullptr; const V4MixedDiagnosticRecord* completionReturn = nullptr;
        const V4MixedDiagnosticRecord* resultOutputs = nullptr;
        std::vector<const V4MixedDiagnosticRecord*> starts, ends, staged, pipelinePaths, fileIds, identityStatus;
        std::vector<const V4MixedDiagnosticRecord*> resultSources, resultDestinations, resultFlags;
        bool conflict = false;
        for (const auto& r : records)
        {
            if (r.event.task != out.batch) continue;
            // Per-item Shell callbacks have sink objects, not the IDataObject
            // address used by the virtual pipeline markers. They are checked
            // independently by the tracked-copy reader below.
            if (r.event.phase >= diagnostic::Phase::tracked_copy_item_source &&
                r.event.phase <= diagnostic::Phase::tracked_copy_post_return) continue;
            // Refresh links carry the host app address and are independently
            // attributed by InspectV4VirtualRefresh, not by this source loop.
            if (r.event.phase == diagnostic::Phase::virtual_refresh_link_enter ||
                r.event.phase == diagnostic::Phase::virtual_refresh_link_return) continue;
            const bool outputRecord = r.event.phase == diagnostic::Phase::virtual_result_outputs ||
                r.event.phase == diagnostic::Phase::virtual_result_source ||
                r.event.phase == diagnostic::Phase::virtual_result_destination ||
                r.event.phase == diagnostic::Phase::virtual_result_output_flags;
            if ((!outputRecord && r.event.object != sourceAddress) || r.event.thread != drop->entered.thread)
            { conflict = true; break; }
            const auto once = [&](const V4MixedDiagnosticRecord*& target) { if (target) conflict = true; else target = &r; };
            switch (r.event.phase)
            {
            case diagnostic::Phase::virtual_batch_return: once(batchReturn); break;
            case diagnostic::Phase::virtual_batch_unwound: case diagnostic::Phase::virtual_member_unwound: conflict = true; break;
            case diagnostic::Phase::virtual_member_enter: starts.push_back(&r); break;
            case diagnostic::Phase::virtual_member_return: ends.push_back(&r); break;
            case diagnostic::Phase::virtual_member_path: staged.push_back(&r); break;
            case diagnostic::Phase::virtual_member_file_id: fileIds.push_back(&r); break;
            case diagnostic::Phase::virtual_member_identity_status: identityStatus.push_back(&r); break;
            case diagnostic::Phase::virtual_pipeline_path: pipelinePaths.push_back(&r); break;
            case diagnostic::Phase::virtual_result_outputs: once(resultOutputs); break;
            case diagnostic::Phase::virtual_result_source: resultSources.push_back(&r); break;
            case diagnostic::Phase::virtual_result_destination: resultDestinations.push_back(&r); break;
            case diagnostic::Phase::virtual_result_output_flags: resultFlags.push_back(&r); break;
            case diagnostic::Phase::virtual_pipeline_enter: once(pipelineEnter); break;
            case diagnostic::Phase::virtual_pipeline_return: once(pipelineReturn); break;
            case diagnostic::Phase::virtual_completion_enter: once(completionEnter); break;
            case diagnostic::Phase::virtual_completion_return: once(completionReturn); break;
            default: break;
            }
        }
        const auto size = manifest.members.size();
        if (conflict || !batchReturn || !pipelineEnter || !pipelineReturn || !completionEnter || !completionReturn ||
            starts.size() != size || ends.size() != size || staged.size() != size || pipelinePaths.size() != size ||
            fileIds.size() != size || identityStatus.size() != size || !resultOutputs || !resultOutputs->event.object ||
            resultSources.size() != size || resultDestinations.size() != size || resultFlags.size() != size ||
            resultOutputs->event.value != diagnostic::ObservedSize(size) ||
            batchReturn->event.value != 1 || pipelineEnter->event.value != diagnostic::ObservedSize(size) ||
            pipelineReturn->event.value != 1 || completionEnter->event.value != 1 || completionReturn->event.value != 1 ||
            !sameDrop(batchReturn->event) || !sameDrop(pipelineEnter->event) || !sameDrop(pipelineReturn->event) ||
            !sameDrop(completionEnter->event) || !sameDrop(completionReturn->event) ||
            batchEnter->index >= batchReturn->index || batchReturn->index >= pipelineEnter->index ||
            pipelineEnter->index >= pipelineReturn->index || pipelineEnter->index >= resultOutputs->index ||
            resultOutputs->index >= completionEnter->index || pipelineEnter->index >= completionEnter->index ||
            completionEnter->index >= completionReturn->index)
        { out.unresolved.emplace_back("v4_virtual_flow_batch_pipeline_or_completion_unavailable"); return out; }
        out.operationResult = resultOutputs->event.object;
        const auto descriptor = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW"));
        const auto contents = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents"));
        if (!descriptor || !contents) { out.unresolved.emplace_back("v4_virtual_flow_formats_unavailable"); return out; }
        std::size_t descriptorReads = 0;
        for (const auto& provider : out.content->providerCalls)
        {
            const auto& call = out.content->afterAwait->calls[provider.callIndex];
            if (call.format == descriptor && call.index == -1 && call.requestedTymed == TYMED_HGLOBAL &&
                SUCCEEDED(call.result) && provider.returnIndex < batchEnter->index) ++descriptorReads;
        }
        if (descriptorReads != 1) { out.unresolved.emplace_back("v4_virtual_flow_descriptor_occurrence_unavailable"); return out; }
        std::set<std::uint64_t> usedMembers, usedProviders, usedReads;
        std::set<std::size_t> usedOutputIndices;
        for (std::size_t i = 0; i < size; ++i)
        {
            const auto& start = *starts[i]; const auto& end = *ends[i]; const auto& path = *staged[i]; const auto& submitted = *pipelinePaths[i];
            const auto& fileId = *fileIds[i]; const auto& attributes = *identityStatus[i];
            std::optional<std::size_t> outputIndex;
            for (std::size_t j = 0; j < resultSources.size(); ++j)
                if (resultSources[j]->blobCopied && resultSources[j]->blob == path.blob)
                { if (outputIndex) { outputIndex.reset(); break; } outputIndex = j; }
            if (!outputIndex || !usedOutputIndices.insert(*outputIndex).second)
            { out.unresolved.emplace_back("v4_virtual_flow_result_source_mapping_unavailable"); return out; }
            const auto& outputSource = *resultSources[*outputIndex];
            const auto& outputDestination = *resultDestinations[*outputIndex]; const auto& flags = *resultFlags[*outputIndex];
            const auto memberId = start.event.extra;
            if (!memberId || !usedMembers.insert(memberId).second || end.event.extra != memberId || path.event.extra != memberId ||
                start.event.value != static_cast<std::int64_t>(manifest.members[i].descriptorIndex) ||
                end.event.value != diagnostic::ObservedSize(manifest.members[i].bytes.size()) ||
                !sameDrop(start.event) || !sameDrop(end.event) || start.index <= batchEnter->index ||
                end.index <= start.index || path.index <= end.index || path.index >= batchReturn->index ||
                submitted.event.extra != i || submitted.index >= pipelineEnter->index || !path.blobCopied || !submitted.blobCopied ||
                path.blob.empty() || path.blob != submitted.blob || fileId.event.extra != memberId || attributes.event.extra != memberId ||
                !fileId.blobCopied || fileId.blob.size() != sizeof(FILE_ID_INFO) || attributes.event.value < 0 ||
                (static_cast<DWORD>(attributes.event.value) & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                fileId.index <= path.index || attributes.index <= fileId.index || attributes.index >= batchReturn->index ||
                outputSource.event.object != out.operationResult || outputDestination.event.object != out.operationResult ||
                flags.event.object != out.operationResult || outputSource.event.extra != *outputIndex || outputDestination.event.extra != *outputIndex ||
                flags.event.extra != *outputIndex || !outputSource.blobCopied || !outputDestination.blobCopied || outputDestination.blob.empty() ||
                outputSource.blob != path.blob || outputSource.index <= resultOutputs->index ||
                outputDestination.index <= resultOutputs->index || flags.index <= resultOutputs->index ||
                outputSource.index >= completionEnter->index || outputDestination.index >= completionEnter->index ||
                flags.index >= completionEnter->index || (flags.event.value != 0 && flags.event.value != 1))
            { out.unresolved.emplace_back("v4_virtual_flow_member_or_pipeline_path_unavailable"); return out; }
            const V4ConfiguredProviderCall* provider = nullptr;
            for (const auto& item : out.content->providerCalls)
            {
                const auto& call = out.content->afterAwait->calls[item.callIndex];
                if (call.format == contents && call.index == static_cast<LONG>(manifest.members[i].descriptorIndex) &&
                    item.enterIndex > start.index && item.returnIndex < end.index)
                { if (provider) { provider = nullptr; break; } provider = &item; }
            }
            if (!provider || !provider->callId || !usedProviders.insert(provider->callId).second)
            { out.unresolved.emplace_back("v4_virtual_flow_stream_provider_unavailable"); return out; }
            const auto& providerCall = out.content->afterAwait->calls[provider->callIndex];
            if (providerCall.query || providerCall.requestedTymed != TYMED_ISTREAM || FAILED(providerCall.result) ||
                providerCall.returnedTymed != TYMED_ISTREAM || !providerCall.mediumObserved)
            { out.unresolved.emplace_back("v4_virtual_flow_stream_medium_unavailable"); return out; }
            const V4ConfiguredStreamSnapshot* stream = nullptr;
            for (const auto& item : out.content->afterAwait->streams)
                if (item.providerCallId == provider->callId)
                { if (stream) { stream = nullptr; break; } stream = &item; }
            if (!stream || stream->observationUnavailable || !stream->identity || stream->index != providerCall.index || stream->reads.empty())
            { out.unresolved.emplace_back("v4_virtual_flow_stream_snapshot_unavailable"); return out; }
            V4VirtualFlowMember member; member.descriptorIndex = manifest.members[i].descriptorIndex; member.outputIndex = *outputIndex;
            member.memberId = memberId; member.providerCallId = provider->callId; member.stagedPath = path.blob; member.pipelinePath = submitted.blob;
            FILE_ID_INFO actualId{}; std::memcpy(&actualId, fileId.blob.data(), sizeof(actualId));
            if (!std::any_of(std::begin(actualId.FileId.Identifier), std::end(actualId.FileId.Identifier), [](BYTE b) { return b != 0; }))
            { out.unresolved.emplace_back("v4_virtual_flow_staging_file_identity_unavailable"); return out; }
            member.stagedFileId = actualId; member.stagedAttributes = static_cast<DWORD>(attributes.event.value);
            member.actualDestination = outputDestination.blob; member.referencesSource = flags.event.value != 0;
            std::size_t offset = 0, previous = provider->returnIndex;
            bool bytesMatch = true, terminal = false;
            for (const auto& read : stream->reads)
            {
                if (!read.readId || !usedReads.insert(read.readId).second || read.thread != drop->entered.thread ||
                    FAILED(read.result) || read.returned > read.requested || read.returned != read.bytes.size() || terminal)
                { out.unresolved.emplace_back("v4_virtual_flow_stream_read_unavailable"); return out; }
                const V4MixedDiagnosticRecord* readEnter = nullptr; const V4MixedDiagnosticRecord* readReturn = nullptr;
                for (const auto& r : records)
                {
                    if (r.event.task != provider->callId || r.event.extra != read.readId) continue;
                    if (r.event.phase == diagnostic::Phase::provider_stream_read_enter)
                    { if (readEnter) { readEnter = nullptr; break; } readEnter = &r; }
                    if (r.event.phase == diagnostic::Phase::provider_stream_read_return)
                    { if (readReturn) { readReturn = nullptr; break; } readReturn = &r; }
                }
                const auto streamScopeMatches = [&](const diagnostic::Event& event) {
                    const auto& scope = event.adapterScope;
                    return scope.observed && scope.session == drop->entered.session && scope.call == drop->entered.call &&
                        scope.parent == drop->entered.parent && scope.depth == drop->entered.depth &&
                        scope.thread == drop->entered.thread && scope.adapter == drop->entered.adapter &&
                        scope.kind == static_cast<unsigned>(snowdesktop::ole_adapter_observation::Kind::drop);
                };
                if (!readEnter || !readReturn || readEnter->event.object != stream->identity || readReturn->event.object != stream->identity ||
                    readEnter->event.thread != drop->entered.thread || readReturn->event.thread != drop->entered.thread ||
                    !streamScopeMatches(readEnter->event) || !streamScopeMatches(readReturn->event) ||
                    readEnter->event.value != read.requested || readReturn->event.value != static_cast<std::int64_t>(read.result) ||
                    readEnter->index <= previous || readReturn->index <= readEnter->index || readReturn->index >= end.index)
                { out.unresolved.emplace_back("v4_virtual_flow_stream_read_interval_unavailable"); return out; }
                previous = readReturn->index; member.readIds.push_back(read.readId);
                if (offset > manifest.members[i].bytes.size() || read.bytes.size() > manifest.members[i].bytes.size() - offset ||
                    !std::equal(read.bytes.begin(), read.bytes.end(), manifest.members[i].bytes.begin() + offset)) bytesMatch = false;
                if (read.bytes.size() > (std::numeric_limits<std::size_t>::max)() - offset)
                { out.unresolved.emplace_back("v4_virtual_flow_stream_size_overflow"); return out; }
                offset += read.bytes.size(); terminal = read.returned == 0 || read.result == S_FALSE;
            }
            member.terminalReadObserved = terminal; member.observedReadValuesMatch = bytesMatch && offset == manifest.members[i].bytes.size();
            out.members.push_back(std::move(member));
        }
        out.identityValid = true; out.batchObserved = true; out.pipelineReturned = true; out.callbackReturned = true;
        out.outputMappingObserved = true;
        out.unresolved.emplace_back("v4_virtual_flow_staging_process_isolation_and_object_identity_unverified");
        out.unresolved.emplace_back("v4_virtual_flow_stream_caller_abi_late_work_and_uninstrumented_reentry_unverified");
        out.unresolved.emplace_back("v4_virtual_flow_output_refresh_layout_and_business_unverified");
    }
    catch (...) { out.observationUnavailable = true; }
    return out;
}


// The per-item values below are bound to one synchronous virtual Drop and
// its actual staging/pipeline result. They do not expose Shell's opened file
// handle or prove filesystem identity, durable output, or producer quiescence.
struct V4VirtualTrackedShellEvidence
{
    bool localItemObserved = false, observationUnavailable = true, resourceUnavailable = false;
    std::uint64_t batch = 0, work = 0, call = 0;
    std::vector<V4MixedDiagnosticRecord> records;
    std::vector<std::string> unresolved;
};
inline V4VirtualTrackedShellEvidence InspectV4VirtualTrackedShell(
    const V4ConfiguredContentOleRunReceipt& receipt, const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const std::vector<bool>& present,
    const event_graph::V4JoinedGraphVariant& selected, const mixed_fixture::ConfiguredContentBinding& binding,
    const std::shared_ptr<mixed_fixture::ConfiguredContentOwner>& owner, OleDragDropAdapter* adapter,
    const std::shared_ptr<const V3HostEpoch>& epoch, const MessageTrace& trace, HWND control,
    const mixed_fixture::VirtualExpectedManifest& manifest, std::size_t byteBudget, std::size_t virtualBudget)
{
    namespace diagnostic = snowdesktop::external_transfer_observation;
    V4VirtualTrackedShellEvidence out;
    try
    {
        const auto flow = InspectV4VirtualFlow(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control, manifest, byteBudget, virtualBudget);
        if (!flow.content) { out.unresolved.emplace_back("v4_virtual_shell_content_unavailable"); return out; }
        for (const auto& record : flow.content->records)
            if ((record.event.phase >= diagnostic::Phase::tracked_copy_item_source &&
                 record.event.phase <= diagnostic::Phase::tracked_copy_post_return) ||
                record.event.phase == diagnostic::Phase::virtual_sync_file_scope_enter ||
                record.event.phase == diagnostic::Phase::virtual_sync_file_scope_return ||
                record.event.phase == diagnostic::Phase::virtual_pipeline_enter ||
                record.event.phase == diagnostic::Phase::virtual_pipeline_return ||
                record.event.phase == diagnostic::Phase::virtual_result_outputs)
                out.records.push_back(record);
        if (!flow.identityValid || !flow.batchObserved || !flow.outputMappingObserved || !flow.batch ||
            !flow.operationResult || flow.members.size() != 1 || !binding.SourceObject())
        { out.unresolved.emplace_back("v4_virtual_shell_flow_unavailable"); return out; }
        const auto source = reinterpret_cast<std::uintptr_t>(static_cast<IDataObject*>(binding.SourceObject()));
        const V4MixedDiagnosticRecord *enter = nullptr, *returned = nullptr, *outputs = nullptr;
        const V4MixedDiagnosticRecord *pipelineEnter = nullptr, *pipelineReturn = nullptr;
        constexpr auto first = static_cast<unsigned>(diagnostic::Phase::tracked_copy_item_source);
        constexpr auto last = static_cast<unsigned>(diagnostic::Phase::tracked_copy_post_return);
        std::array<const V4MixedDiagnosticRecord*, last - first + 1> events{};
        for (const auto& record : out.records)
        {
            const auto& event = record.event; if (event.task != flow.batch) continue;
            if (event.phase == diagnostic::Phase::virtual_sync_file_scope_enter)
            { if (enter) { out.unresolved.emplace_back("v4_virtual_shell_scope_duplicate"); return out; } enter = &record; }
            else if (event.phase == diagnostic::Phase::virtual_sync_file_scope_return)
            { if (returned) { out.unresolved.emplace_back("v4_virtual_shell_scope_duplicate"); return out; } returned = &record; }
            else if (event.phase == diagnostic::Phase::virtual_result_outputs)
            { if (outputs) { out.unresolved.emplace_back("v4_virtual_shell_result_duplicate"); return out; } outputs = &record; }
            else if (event.phase == diagnostic::Phase::virtual_pipeline_enter)
            { if (pipelineEnter) { out.unresolved.emplace_back("v4_virtual_shell_pipeline_duplicate"); return out; } pipelineEnter = &record; }
            else if (event.phase == diagnostic::Phase::virtual_pipeline_return)
            { if (pipelineReturn) { out.unresolved.emplace_back("v4_virtual_shell_pipeline_duplicate"); return out; } pipelineReturn = &record; }
            else
            {
                auto& slot = events[static_cast<unsigned>(event.phase) - first];
                if (slot) { out.unresolved.emplace_back("v4_virtual_shell_item_duplicate"); return out; }
                slot = &record;
            }
        }
        if (!enter || !returned || !outputs || !pipelineEnter || !pipelineReturn ||
            pipelineEnter->event.object != source || pipelineReturn->event.object != source ||
            !enter->event.extra || enter->event.object != source ||
            returned->event.object != source || returned->event.extra != enter->event.extra || returned->event.value != 1 ||
            outputs->event.object != flow.operationResult || !enter->event.thread ||
            std::any_of(events.begin(), events.end(), [](const auto* item) { return !item; }))
        { out.unresolved.emplace_back("v4_virtual_shell_scope_or_item_unavailable"); return out; }
        const auto work = enter->event.extra; const auto thread = enter->event.thread;
        if (thread != GetCurrentThreadId() || pipelineEnter->event.thread != thread || pipelineReturn->event.thread != thread ||
            enter->event.localTask != flow.batch || enter->event.localWork != work ||
            returned->event.thread != thread || returned->event.localTask != flow.batch || returned->event.localWork != work ||
            outputs->event.thread != thread || outputs->event.localTask != flow.batch || outputs->event.localWork != work)
        { out.unresolved.emplace_back("v4_virtual_shell_scope_identity_unavailable"); return out; }
        const auto sink = events[0]->event.object, call = events[0]->event.extra;
        for (std::size_t i = 0; i < events.size(); ++i)
            if (!sink || !call || events[i]->event.object != sink || events[i]->event.extra != call ||
                events[i]->event.thread != thread || events[i]->event.localTask != flow.batch ||
                events[i]->event.localWork != work || (i && events[i - 1]->index >= events[i]->index))
            { out.unresolved.emplace_back("v4_virtual_shell_item_identity_unavailable"); return out; }
        const auto pointerValue = [](std::int64_t value) { return static_cast<std::uintptr_t>(static_cast<std::uint64_t>(value)); };
        if (!(pipelineEnter->index < enter->index && returned->index < pipelineReturn->index) ||
            enter->index >= events.front()->index || events.back()->index >= outputs->index || outputs->index >= returned->index ||
            !pointerValue(events[0]->event.value) || pointerValue(events[1]->event.value) != flow.operationResult ||
            !events[2]->blobCopied || events[2]->blob != flow.members.front().pipelinePath ||
            FAILED(static_cast<HRESULT>(events[3]->event.value)) || FAILED(static_cast<HRESULT>(events[4]->event.value)) ||
            !pointerValue(events[5]->event.value) || !pointerValue(events[6]->event.value) ||
            FAILED(static_cast<HRESULT>(events[7]->event.value)) || events[8]->event.value != 0 ||
            FAILED(static_cast<HRESULT>(events[9]->event.value)))
        { out.unresolved.emplace_back("v4_virtual_shell_actual_values_unavailable_or_differ"); return out; }
        out.batch = flow.batch; out.work = work; out.call = call; out.localItemObserved = true;
        out.unresolved.emplace_back("v4_virtual_shell_internal_open_file_id_unobserved");
        out.unresolved.emplace_back("v4_virtual_shell_reentry_late_work_and_durable_business_unverified");
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; }
    catch (const std::length_error&) { out.resourceUnavailable = true; }
    catch (...) { out.observationUnavailable = true; }
    return out;
}

struct V4VirtualRefreshEvidence
{
    struct WorkerRecord { std::size_t index = 0; snowdesktop::background_work_observation::Event event{}; };
    bool localRefreshBound = false, observationUnavailable = true;
    std::uint64_t batch = 0, revision = 0, serial = 0, windowScope = 0;
    std::uintptr_t host = 0, context = 0, workerState = 0;
    std::optional<std::size_t> windowTraceIndex;
    std::vector<V4MixedDiagnosticRecord> appRecords;
    std::vector<WorkerRecord> workerRecords;
    std::optional<MessageObservation> windowMessage;
    bool modelSnapshotConsumed = false;
    std::optional<std::int64_t> modelOutcome;
    bool modelLayoutSaveObserved = false;
    std::optional<std::uint64_t> modelLayoutSaveRevision;
    std::optional<std::size_t> modelWindowTraceIndex;
    std::optional<MessageObservation> modelWindowMessage;
    std::vector<std::string> unresolved;
};

// Follows the diagnostic batch captured by the actual file-backed operation
// closure into its direct RequestShellRefresh call, one revision/snapshot and
// BackgroundWork serial, and the matching actual WndProc scope and return.
// Model, disk, staging isolation and uninstrumented reentry remain independent.
inline V4VirtualRefreshEvidence InspectV4VirtualRefresh(const V4ConfiguredContentOleRunReceipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    const mixed_fixture::ConfiguredContentBinding& binding,
    const std::shared_ptr<mixed_fixture::ConfiguredContentOwner>& owner, OleDragDropAdapter* adapter,
    const std::shared_ptr<const V3HostEpoch>& epoch, const MessageTrace& trace, HWND control,
    const mixed_fixture::VirtualExpectedManifest& manifest, std::size_t byteBudget, std::size_t virtualBudget)
{
    namespace app = snowdesktop::external_transfer_observation;
    namespace work = snowdesktop::background_work_observation;
    V4VirtualRefreshEvidence out;
    try
    {
        const auto flow = InspectV4VirtualFlow(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control, manifest, byteBudget, virtualBudget);
        if (!flow.identityValid || !flow.batchObserved || !flow.pipelineReturned || !flow.callbackReturned ||
            !flow.batch || !flow.content || !app::Complete() || !work::Complete() || !receipt.HostIdentity() ||
            !receipt.CompletionTraceCaptured() || !receipt.SameBoundHost(binding, owner, adapter, epoch, trace, control))
        { out.unresolved.emplace_back("v4_virtual_refresh_flow_or_logs_unavailable"); return out; }
        out.batch = flow.batch; out.host = receipt.HostIdentity();
        const auto& events = flow.content->records;
        const V4MixedDiagnosticRecord *linkEnter = nullptr, *linkReturn = nullptr, *requestEnter = nullptr, *requestReturn = nullptr;
        for (const auto& record : events)
        {
            if (record.event.task != out.batch) continue;
            if (record.event.phase == app::Phase::virtual_refresh_link_enter)
            { if (linkEnter) { out.unresolved.emplace_back("v4_virtual_refresh_multiple_links"); return out; } linkEnter = &record; }
            if (record.event.phase == app::Phase::virtual_refresh_link_return)
            { if (linkReturn) { out.unresolved.emplace_back("v4_virtual_refresh_multiple_links"); return out; } linkReturn = &record; }
        }
        if (!linkEnter || !linkReturn || linkEnter->index >= linkReturn->index ||
            linkEnter->event.object != out.host || linkReturn->event.object != out.host ||
            linkEnter->event.thread != trace.OwnerThread() || linkReturn->event.thread != trace.OwnerThread())
        { out.unresolved.emplace_back("v4_virtual_refresh_link_unavailable"); return out; }
        for (const auto& record : events)
        {
            if (record.index <= linkEnter->index || record.index >= linkReturn->index) continue;
            if (record.event.phase == app::Phase::shell_refresh_request_enter)
            { if (requestEnter) { out.unresolved.emplace_back("v4_virtual_refresh_request_ambiguous"); return out; } requestEnter = &record; }
            if (record.event.phase == app::Phase::shell_refresh_request_return)
            { if (requestReturn) { out.unresolved.emplace_back("v4_virtual_refresh_request_ambiguous"); return out; } requestReturn = &record; }
        }
        if (!requestEnter || !requestReturn || requestEnter->index >= requestReturn->index ||
            requestEnter->event.object != out.host || requestReturn->event.object != out.host ||
            requestEnter->event.thread != trace.OwnerThread() || requestReturn->event.thread != trace.OwnerThread() ||
            (requestEnter->event.value & 15) || !(requestReturn->event.value & 2) ||
            requestEnter->event.extra == (std::numeric_limits<std::uint64_t>::max)() ||
            requestReturn->event.extra != requestEnter->event.extra + 1)
        { out.unresolved.emplace_back("v4_virtual_refresh_request_revision_unavailable"); return out; }
        out.revision = requestReturn->event.extra;
        out.appRecords = {*linkEnter, *requestEnter, *requestReturn, *linkReturn};
        const std::array<app::Phase, 6> appPhases{app::Phase::shell_refresh_submit_enter,
            app::Phase::shell_refresh_submit_return, app::Phase::shell_refresh_read_enter,
            app::Phase::shell_refresh_read_return, app::Phase::shell_refresh_apply_enter,
            app::Phase::shell_refresh_apply_return};
        std::array<const V4MixedDiagnosticRecord*, 6> stages{};
        for (const auto& record : events)
        {
            if (record.event.extra != out.revision) continue;
            for (std::size_t i = 0; i < appPhases.size(); ++i)
                if (record.event.phase == appPhases[i])
                { if (stages[i]) { out.unresolved.emplace_back("v4_virtual_refresh_revision_ambiguous"); return out; } stages[i] = &record; }
        }
        if (std::any_of(stages.begin(), stages.end(), [](const auto* stage) { return !stage; }))
        { out.unresolved.emplace_back("v4_virtual_refresh_stages_missing"); return out; }
        for (const auto* stage : stages) out.appRecords.push_back(*stage);
        out.context = stages[0]->event.object;
        if (!out.context || stages[0]->index <= linkReturn->index || stages[0]->index >= stages[1]->index ||
            stages[2]->index >= stages[3]->index || stages[4]->index >= stages[5]->index ||
            stages[1]->event.value != 1 || stages[3]->event.value != 1 || stages[4]->event.value != 1 || stages[5]->event.value != 1)
        { out.unresolved.emplace_back("v4_virtual_refresh_stage_order_or_result_unavailable"); return out; }
        for (const auto* stage : stages)
            if (stage->event.object != out.context)
            { out.unresolved.emplace_back("v4_virtual_refresh_snapshot_identity_unavailable"); return out; }
        if (stages[0]->event.thread != trace.OwnerThread() || stages[1]->event.thread != trace.OwnerThread() ||
            stages[2]->event.thread != stages[3]->event.thread || stages[4]->event.thread != trace.OwnerThread() ||
            stages[5]->event.thread != trace.OwnerThread() || !stages[4]->event.windowScope ||
            stages[4]->event.windowScope != stages[5]->event.windowScope)
        { out.unresolved.emplace_back("v4_virtual_refresh_thread_or_window_scope_unavailable"); return out; }
        out.windowScope = stages[4]->event.windowScope;
        const auto& messages = receipt.CompletionTrace();
        for (std::size_t i = 0; i < messages.size(); ++i)
        {
            const auto& message = messages[i];
            if (message.windowScope != out.windowScope) continue;
            if (out.windowMessage) { out.unresolved.emplace_back("v4_virtual_refresh_window_scope_ambiguous"); return out; }
            out.windowMessage = message; out.windowTraceIndex = i;
        }
        if (!out.windowMessage || out.windowMessage->window != control ||
            out.windowMessage->route != MessageRoute::control_window_proc ||
            out.windowMessage->message != kBackgroundShellReadyMessage || !out.windowMessage->returned ||
            out.windowMessage->enteredThread != trace.OwnerThread() ||
            out.windowMessage->returnedThread != trace.OwnerThread() ||
            out.windowMessage->returnedSequence <= out.windowMessage->enteredSequence)
        { out.unresolved.emplace_back("v4_virtual_refresh_wndproc_return_unavailable"); return out; }
        const std::array<work::Phase, 7> workPhases{work::Phase::queued, work::Phase::worker_enter,
            work::Phase::worker_return, work::Phase::ready, work::Phase::post_result,
            work::Phase::apply_enter, work::Phase::apply_return};
        std::array<std::optional<V4VirtualRefreshEvidence::WorkerRecord>, 7> workStages{};
        if (work::Count() > work::ObserveBudget().eventCapacity) { out.unresolved.emplace_back("v4_virtual_refresh_worker_log_overflow"); return out; }
        for (std::size_t i = 0; i < work::Count(); ++i)
        {
            work::Event event;
            if (!work::Read(i, event)) { out.unresolved.emplace_back("v4_virtual_refresh_worker_log_lost"); return out; }
            if (event.context != out.context) continue;
            out.workerRecords.push_back({i, event});
            if (event.phase == work::Phase::coalesced || event.phase == work::Phase::queue_full || event.phase == work::Phase::discarded)
            { out.unresolved.emplace_back("v4_virtual_refresh_worker_coalesced_or_discarded"); return out; }
            for (std::size_t j = 0; j < workPhases.size(); ++j)
                if (event.phase == workPhases[j])
                { if (workStages[j]) { out.unresolved.emplace_back("v4_virtual_refresh_worker_duplicate"); return out; } workStages[j] = V4VirtualRefreshEvidence::WorkerRecord{i, event}; }
        }
        if (std::any_of(workStages.begin(), workStages.end(), [](const auto& stage) { return !stage; }))
        { out.unresolved.emplace_back("v4_virtual_refresh_worker_incomplete"); return out; }
        out.workerState = workStages[0]->event.state; out.serial = workStages[0]->event.serial;
        if (!out.workerState || !out.serial)
        { out.unresolved.emplace_back("v4_virtual_refresh_worker_identity_unavailable"); return out; }
        for (const auto& stage : workStages)
            if (stage->event.state != out.workerState || stage->event.serial != out.serial)
            { out.unresolved.emplace_back("v4_virtual_refresh_worker_identity_mismatch"); return out; }
        if (workStages[1]->index >= workStages[2]->index || workStages[2]->index >= workStages[3]->index ||
            workStages[3]->index >= workStages[4]->index || workStages[5]->index >= workStages[6]->index ||
            workStages[0]->event.thread != stages[0]->event.thread ||
            workStages[1]->event.thread != stages[2]->event.thread || workStages[2]->event.thread != stages[3]->event.thread ||
            workStages[3]->event.thread != workStages[2]->event.thread || workStages[4]->event.thread != workStages[2]->event.thread ||
            workStages[5]->event.thread != stages[4]->event.thread || workStages[6]->event.thread != stages[5]->event.thread ||
            !workStages[4]->event.result || workStages[4]->event.window != reinterpret_cast<std::uintptr_t>(control) ||
            workStages[4]->event.message != kBackgroundShellReadyMessage)
        { out.unresolved.emplace_back("v4_virtual_refresh_worker_order_or_post_unavailable"); return out; }
        out.localRefreshBound = true;
        // The worker's apply only stores readyShellRefresh_. A later real
        // desktop timer consumes that same snapshot and applies the model.
        // Match its scope to the actual returned WndProc; no empty log proves
        // absence, and a discarded/reused/ambiguous snapshot stays unresolved.
        const V4MixedDiagnosticRecord *modelEnter = nullptr, *modelReturn = nullptr;
        std::size_t submitsForContext = 0;
        bool modelAmbiguous = false;
        for (const auto& record : events)
        {
            if (record.event.object != out.context) continue;
            if (record.event.phase == app::Phase::shell_refresh_submit_enter) ++submitsForContext;
            if (record.event.phase == app::Phase::shell_refresh_model_enter)
            { if (modelEnter) modelAmbiguous = true; else modelEnter = &record; }
            if (record.event.phase == app::Phase::shell_refresh_model_return)
            { if (modelReturn) modelAmbiguous = true; else modelReturn = &record; }
        }
        if (modelEnter) out.appRecords.push_back(*modelEnter);
        if (modelReturn) out.appRecords.push_back(*modelReturn);
        if (!modelAmbiguous && submitsForContext == 1 && modelEnter && modelReturn &&
            modelEnter->index > stages[5]->index && modelReturn->index > modelEnter->index &&
            modelEnter->event.thread == trace.OwnerThread() && modelReturn->event.thread == trace.OwnerThread() &&
            modelEnter->event.value == 1 && (modelReturn->event.value == 3 || modelReturn->event.value == 4) &&
            modelEnter->event.windowScope && modelEnter->event.windowScope == modelReturn->event.windowScope)
        {
            for (std::size_t i = 0; i < messages.size(); ++i)
            {
                const auto& message = messages[i];
                if (message.windowScope != modelEnter->event.windowScope) continue;
                if (out.modelWindowMessage) { modelAmbiguous = true; break; }
                out.modelWindowMessage = message; out.modelWindowTraceIndex = i;
            }
            if (!modelAmbiguous && out.modelWindowMessage &&
                out.modelWindowMessage->route == MessageRoute::desktop_window_proc &&
                epoch && epoch->IsDesktopWindow(out.modelWindowMessage->window) &&
                out.modelWindowMessage->message == WM_TIMER && out.modelWindowMessage->wParam == kShellChangeTimerId &&
                out.modelWindowMessage->returned && out.modelWindowMessage->enteredThread == trace.OwnerThread() &&
                out.modelWindowMessage->returnedThread == trace.OwnerThread() &&
                out.modelWindowMessage->returnedSequence > out.modelWindowMessage->enteredSequence)
            { out.modelSnapshotConsumed = true; out.modelOutcome = modelReturn->event.value; }
        }
        if (!out.modelSnapshotConsumed)
            out.unresolved.emplace_back("v4_virtual_refresh_model_snapshot_or_timer_return_unavailable");
        if (out.modelSnapshotConsumed && out.modelOutcome == std::optional<std::int64_t>{4})
        {
            const std::array<app::Phase, 4> savePhases{app::Phase::layout_write_enter,
                app::Phase::layout_write_return, app::Phase::layout_finish_enter, app::Phase::layout_finish_return};
            std::array<const V4MixedDiagnosticRecord*, 4> save{};
            bool ambiguousSave = false;
            for (const auto& record : events)
            {
                if (record.event.object != out.host || record.event.windowScope != modelEnter->event.windowScope ||
                    record.index <= modelEnter->index || record.index >= modelReturn->index) continue;
                for (std::size_t i = 0; i < savePhases.size(); ++i)
                    if (record.event.phase == savePhases[i])
                    { if (save[i]) ambiguousSave = true; else save[i] = &record; }
            }
            for (const auto* record : save) if (record) out.appRecords.push_back(*record);
            if (!ambiguousSave && std::all_of(save.begin(), save.end(), [](const auto* item) { return item != nullptr; }))
            {
                const auto revision = save[0]->event.extra;
                const bool sameRevisionAndThread = std::all_of(save.begin(), save.end(), [&](const auto* item) {
                    return item->event.extra == revision && item->event.thread == trace.OwnerThread(); });
                if (revision && sameRevisionAndThread && save[0]->index < save[1]->index &&
                    save[1]->index < save[2]->index && save[2]->index < save[3]->index &&
                    save[1]->event.value == 1 && (save[3]->event.value & 15) == 11 &&
                    save[3]->event.currentRevision == revision)
                { out.modelLayoutSaveObserved = true; out.modelLayoutSaveRevision = revision; }
            }
        }
        if (!out.modelLayoutSaveObserved)
            out.unresolved.emplace_back("v4_virtual_refresh_synchronous_model_save_unavailable");
        out.unresolved.emplace_back("v4_virtual_refresh_uninstrumented_reentry_and_late_work_unverified");
        out.unresolved.emplace_back("v4_virtual_refresh_applied_model_disk_and_staging_isolation_unverified");
    }
    catch (...) { out.observationUnavailable = true; }
    return out;
}


// Join only positive local evidence rebuilt from the same receipt. The
// refresh's later worker/timer records remain separate from the synchronous
// Shell callback and are never ordered by comparing unrelated log clocks.
struct V4VirtualProductionChainEvidence
{
    bool localTrackedToRefresh = false, observationUnavailable = true, resourceUnavailable = false;
    std::optional<bool> streamBytesMatch, terminalReadObserved;
    bool modelSnapshotConsumed = false, modelLayoutSaveObserved = false;
    std::optional<std::int64_t> modelOutcome;
    std::optional<std::uint64_t> modelLayoutSaveRevision;
    std::optional<V4VirtualTrackedShellEvidence> tracked;
    std::optional<V4VirtualRefreshEvidence> refresh;
    std::vector<std::string> unresolved;
};
inline V4VirtualProductionChainEvidence InspectV4VirtualProductionChain(
    const V4ConfiguredContentOleRunReceipt& receipt, const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const std::vector<bool>& present,
    const event_graph::V4JoinedGraphVariant& selected, const mixed_fixture::ConfiguredContentBinding& binding,
    const std::shared_ptr<mixed_fixture::ConfiguredContentOwner>& owner, OleDragDropAdapter* adapter,
    const std::shared_ptr<const V3HostEpoch>& epoch, const MessageTrace& trace, HWND control,
    const mixed_fixture::VirtualExpectedManifest& manifest, std::size_t byteBudget, std::size_t virtualBudget)
{
    namespace diagnostic = snowdesktop::external_transfer_observation;
    V4VirtualProductionChainEvidence out;
    try
    {
        const auto flow = InspectV4VirtualFlow(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control, manifest, byteBudget, virtualBudget);
        out.tracked = InspectV4VirtualTrackedShell(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control, manifest, byteBudget, virtualBudget);
        out.refresh = InspectV4VirtualRefresh(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control, manifest, byteBudget, virtualBudget);
        if (flow.identityValid && flow.members.size() == 1)
        {
            out.streamBytesMatch = flow.members.front().observedReadValuesMatch;
            out.terminalReadObserved = flow.members.front().terminalReadObserved;
        }
        if (!out.tracked->localItemObserved || !out.refresh->localRefreshBound ||
            !flow.identityValid || flow.batch != out.tracked->batch || flow.batch != out.refresh->batch)
        { out.resourceUnavailable = out.tracked->resourceUnavailable;
          out.unresolved.emplace_back("v4_virtual_chain_tracked_shell_or_refresh_unavailable"); return out; }
        const V4MixedDiagnosticRecord *post = nullptr, *outputs = nullptr, *scopeEnd = nullptr;
        for (const auto& record : out.tracked->records)
        {
            if (record.event.task != flow.batch) continue;
            const V4MixedDiagnosticRecord** slot = record.event.phase == diagnostic::Phase::tracked_copy_post_return ? &post :
                record.event.phase == diagnostic::Phase::virtual_result_outputs ? &outputs :
                record.event.phase == diagnostic::Phase::virtual_sync_file_scope_return ? &scopeEnd : nullptr;
            if (slot) { if (*slot) { out.unresolved.emplace_back("v4_virtual_chain_shell_record_ambiguous"); return out; } *slot = &record; }
        }
        const V4MixedDiagnosticRecord *refreshEnter = nullptr, *refreshReturn = nullptr;
        for (const auto& record : out.refresh->appRecords)
        {
            if (record.event.phase == diagnostic::Phase::virtual_refresh_link_enter)
            { if (refreshEnter) { out.unresolved.emplace_back("v4_virtual_chain_refresh_ambiguous"); return out; } refreshEnter = &record; }
            if (record.event.phase == diagnostic::Phase::virtual_refresh_link_return)
            { if (refreshReturn) { out.unresolved.emplace_back("v4_virtual_chain_refresh_ambiguous"); return out; } refreshReturn = &record; }
        }
        const auto valid = [&](const V4MixedDiagnosticRecord* record) {
            return record && record->event.task == flow.batch && record->event.localTask == flow.batch &&
                record->event.localWork == out.tracked->work && record->event.thread == trace.OwnerThread();
        };
        if (!valid(post) || !valid(outputs) || !valid(scopeEnd) || !valid(refreshEnter) || !valid(refreshReturn) ||
            !(post->index < outputs->index && outputs->index < refreshEnter->index &&
              refreshEnter->index < refreshReturn->index && refreshReturn->index < scopeEnd->index))
        { out.unresolved.emplace_back("v4_virtual_chain_same_scope_or_order_unavailable"); return out; }
        out.localTrackedToRefresh = true;
        out.modelSnapshotConsumed = out.refresh->modelSnapshotConsumed;
        out.modelOutcome = out.refresh->modelOutcome;
        out.modelLayoutSaveObserved = out.refresh->modelLayoutSaveObserved;
        out.modelLayoutSaveRevision = out.refresh->modelLayoutSaveRevision;
        out.unresolved.emplace_back("v4_virtual_chain_shell_open_identity_and_late_work_unverified");
        out.unresolved.emplace_back("v4_virtual_chain_non_atomic_staging_and_business_unverified");
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; }
    catch (const std::length_error&) { out.resourceUnavailable = true; }
    catch (...) { out.observationUnavailable = true; }
    return out;
}

enum class V4VirtualFailureKind
{
    provider_getdata_failed,
    member_no_output,
    member_unwound,
    batch_incomplete,
    batch_unwound,
    pipeline_declined,
    completion_reported_failure
};
struct V4VirtualFailureFact
{
    V4VirtualFailureKind kind = V4VirtualFailureKind::provider_getdata_failed;
    std::size_t firstIndex = 0, lastIndex = 0;
    std::uint64_t callOrMember = 0;
    std::optional<std::uint32_t> descriptorIndex;
    std::optional<CLIPFORMAT> format;
    std::int64_t actualValue = 0;
    bool returned = false;
};
struct V4VirtualFailureObservation
{
    bool identityValid = false, diagnosticComplete = false, batchBound = false;
    bool observationUnavailable = true;
    std::uint64_t batch = 0;
    Status executionStatus = Status::unimplemented;
    bool productionStarted = false, productionEntryUnknown = false;
    bool businessFailureObserved = false, eventOracleFailureObserved = false;
    std::string executionFailure;
    std::vector<Observation> oleObservations;
    std::vector<V2CapturedOleReturn> capturedReturns;
    std::optional<V4ConfiguredContentSnapshot> afterDispatch;
    std::vector<V4MixedDiagnosticRecord> records;
    std::vector<V4VirtualFailureFact> failures;
    std::vector<std::string> unresolved;
};

// Captures actual negative/partial values independently of the success path.
// A failed provider result, rejected operation or failed callback is observed
// data; only the independent candidate-specific contract can judge it. Missing
// logs, a missing completion or an unmatched stage never imply failure.
inline V4VirtualFailureObservation InspectV4VirtualFailures(const V4ConfiguredContentOleRunReceipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    const mixed_fixture::ConfiguredContentBinding& binding,
    const std::shared_ptr<mixed_fixture::ConfiguredContentOwner>& owner, OleDragDropAdapter* adapter,
    const std::shared_ptr<const V3HostEpoch>& epoch, const MessageTrace& trace, HWND control)
{
    namespace diagnostic = snowdesktop::external_transfer_observation;
    V4VirtualFailureObservation out;
    try
    {
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || raw.id != receipt.Plan().candidateId ||
            receipt.Plan().graphId != selected.id || !replay.identityValid || !replay.errors.empty() ||
            replay.variant.id != selected.id || replay.variant.classes != selected.classes ||
            replay.variant.order != selected.order || replay.variant.unresolved != selected.unresolved ||
            !receipt.SameBoundHost(binding, owner, adapter, epoch, trace, control))
        { out.unresolved.emplace_back("v4_virtual_failure_identity_or_host_unavailable"); return out; }
        const auto& execution = receipt.Execution();
        out.identityValid = true; out.executionStatus = execution.status;
        out.productionStarted = execution.productionStarted; out.productionEntryUnknown = execution.productionEntryUnknown;
        out.businessFailureObserved = execution.businessFailureObserved; out.eventOracleFailureObserved = execution.eventOracleFailureObserved;
        out.executionFailure = execution.failure; out.oleObservations = execution.observations;
        out.capturedReturns = receipt.CapturedReturns(); out.afterDispatch = receipt.AfterDispatch();
        if (!diagnostic::Complete() || diagnostic::Count() > diagnostic::EventLimit())
        { out.unresolved.emplace_back("v4_virtual_failure_diagnostic_incomplete"); return out; }
        diagnostic::ReadSession session;
        if (!session.Open()) { out.unresolved.emplace_back("v4_virtual_failure_session_unavailable"); return out; }
        for (std::size_t i = 0; i < session.Count(); ++i)
        {
            V4MixedDiagnosticRecord item; item.index = i;
            if (!session.ReadEvent(i, item.event)) { out.unresolved.emplace_back("v4_virtual_failure_record_unavailable"); return out; }
            if (item.event.hasBlob)
            {
                if (item.event.blobSize > diagnostic::BlobLimit()) { out.unresolved.emplace_back("v4_virtual_failure_blob_unavailable"); return out; }
                item.blob.resize(item.event.blobSize); std::size_t size = 0;
                if (!session.CopyBlob(i, item.blob.data(), item.blob.size(), size) || size != item.blob.size())
                { out.unresolved.emplace_back("v4_virtual_failure_blob_unavailable"); return out; }
                item.blobCopied = true;
            }
            out.records.push_back(std::move(item));
        }
        out.diagnosticComplete = true;
        const V2CapturedOleReturn* drop = nullptr;
        for (const auto& captured : out.capturedReturns)
            if (captured.observation.kind == Kind::drop)
            { if (drop) { out.unresolved.emplace_back("v4_virtual_failure_multiple_drop_unavailable"); return out; } drop = &captured; }
        if (!drop || drop->entered.kind != snowdesktop::ole_adapter_observation::Kind::drop ||
            drop->entered.phase != snowdesktop::ole_adapter_observation::Phase::entered ||
            drop->returned.phase != snowdesktop::ole_adapter_observation::Phase::returned ||
            drop->returned.kind != drop->entered.kind || !drop->entered.session || !drop->entered.call ||
            drop->entered.parent || drop->entered.depth || drop->entered.thread != GetCurrentThreadId() ||
            drop->entered.adapter != reinterpret_cast<std::uintptr_t>(adapter) ||
            drop->returned.session != drop->entered.session || drop->returned.call != drop->entered.call ||
            drop->returned.thread != drop->entered.thread || drop->returned.adapter != drop->entered.adapter ||
            drop->returned.result != drop->observation.result)
        { out.unresolved.emplace_back("v4_virtual_failure_drop_scope_unavailable"); return out; }
        auto* source = binding.SourceObject();
        if (!source) { out.unresolved.emplace_back("v4_virtual_failure_source_unavailable"); return out; }
        const auto sourceAddress = reinterpret_cast<std::uintptr_t>(static_cast<IDataObject*>(source));
        const auto sameDrop = [&](const diagnostic::Event& e) {
            const auto& scope = e.adapterScope;
            return e.object == sourceAddress && e.thread == drop->entered.thread && scope.observed &&
                scope.session == drop->entered.session && scope.call == drop->entered.call &&
                scope.parent == drop->entered.parent && scope.depth == drop->entered.depth &&
                scope.thread == drop->entered.thread && scope.adapter == drop->entered.adapter &&
                scope.kind == static_cast<unsigned>(snowdesktop::ole_adapter_observation::Kind::drop);
        };
        // Match actual provider return values with their own complete ordered
        // call stream; Enter/Over calls remain in the sequence but only Drop
        // scope is attributed to this Drop. No missing call is inferred.
        if (out.afterDispatch && out.afterDispatch->captured && !out.afterDispatch->resourceUnavailable)
        {
            std::vector<std::size_t> gets;
            for (std::size_t i = 0; i < out.afterDispatch->calls.size(); ++i)
                if (!out.afterDispatch->calls[i].query) gets.push_back(i);
            std::vector<const V4MixedDiagnosticRecord*> markers;
            for (const auto& record : out.records)
                if (record.event.object == sourceAddress &&
                    (record.event.phase == diagnostic::Phase::content_provider_getdata_enter ||
                     record.event.phase == diagnostic::Phase::content_provider_getdata_return)) markers.push_back(&record);
            bool valid = gets.size() <= (std::numeric_limits<std::size_t>::max)() / 2 && markers.size() == gets.size() * 2;
            std::vector<V4VirtualFailureFact> providerFailures;
            for (std::size_t i = 0; valid && i < gets.size(); ++i)
            {
                const auto& first = *markers[2 * i]; const auto& last = *markers[2 * i + 1];
                const auto& call = out.afterDispatch->calls[gets[i]];
                const auto& a = first.event.adapterScope; const auto& b = last.event.adapterScope;
                valid = first.event.phase == diagnostic::Phase::content_provider_getdata_enter &&
                    last.event.phase == diagnostic::Phase::content_provider_getdata_return && first.event.extra &&
                    first.event.extra == last.event.extra && first.index < last.index &&
                    (!i || markers[2 * i - 1]->index < first.index) && first.event.thread == last.event.thread &&
                    call.thread == first.event.thread && a.observed == b.observed && a.session == b.session && a.call == b.call &&
                    a.parent == b.parent && a.adapter == b.adapter && a.thread == b.thread && a.depth == b.depth && a.kind == b.kind &&
                    static_cast<std::int64_t>(call.result) == last.event.value;
                if (valid && sameDrop(first.event) && sameDrop(last.event) && FAILED(call.result))
                    providerFailures.push_back({V4VirtualFailureKind::provider_getdata_failed, first.index, last.index,
                        first.event.extra, call.index >= 0 ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(call.index)) : std::nullopt,
                        call.format, static_cast<std::int64_t>(call.result), true});
            }
            if (valid) out.failures.insert(out.failures.end(), providerFailures.begin(), providerFailures.end());
            else out.unresolved.emplace_back("v4_virtual_failure_provider_calls_unavailable");
        }
        else out.unresolved.emplace_back("v4_virtual_failure_source_snapshot_unavailable");
        const V4MixedDiagnosticRecord* batchEnter = nullptr;
        for (const auto& record : out.records)
            if (record.event.phase == diagnostic::Phase::virtual_batch_enter && sameDrop(record.event))
            { if (batchEnter) { out.unresolved.emplace_back("v4_virtual_failure_batch_ambiguous"); return out; } batchEnter = &record; }
        if (!batchEnter || !batchEnter->event.task)
        { out.unresolved.emplace_back("v4_virtual_failure_batch_not_observed"); return out; }
        out.batch = batchEnter->event.task; out.batchBound = true;
        std::map<std::uint64_t, const V4MixedDiagnosticRecord*> memberStarts;
        std::map<std::uint64_t, const V4MixedDiagnosticRecord*> memberEnds;
        std::vector<const V4MixedDiagnosticRecord*> batchEnds, pipelines, completionStarts, completionEnds;
        bool ambiguous = false;
        for (const auto& record : out.records)
        {
            const auto& event = record.event;
            if (event.task != out.batch || event.object != sourceAddress) continue;
            switch (event.phase)
            {
            case diagnostic::Phase::virtual_member_enter:
                if (!event.extra || !memberStarts.emplace(event.extra, &record).second) ambiguous = true; break;
            case diagnostic::Phase::virtual_member_return: case diagnostic::Phase::virtual_member_unwound:
                if (!event.extra || !memberEnds.emplace(event.extra, &record).second) ambiguous = true; break;
            case diagnostic::Phase::virtual_batch_return: case diagnostic::Phase::virtual_batch_unwound: batchEnds.push_back(&record); break;
            case diagnostic::Phase::virtual_pipeline_return: pipelines.push_back(&record); break;
            case diagnostic::Phase::virtual_completion_enter: completionStarts.push_back(&record); break;
            case diagnostic::Phase::virtual_completion_return: completionEnds.push_back(&record); break;
            default: break;
            }
        }
        if (ambiguous || batchEnds.size() > 1 || pipelines.size() > 1 || completionStarts.size() > 1 || completionEnds.size() > 1)
        { out.unresolved.emplace_back("v4_virtual_failure_stage_ambiguous"); return out; }
        for (const auto& [member, end] : memberEnds)
        {
            const auto first = memberStarts.find(member);
            if (first == memberStarts.end() || first->second->index >= end->index ||
                !sameDrop(first->second->event) || !sameDrop(end->event) || first->second->event.value < 0 ||
                static_cast<std::uint64_t>(first->second->event.value) > (std::numeric_limits<std::uint32_t>::max)())
            { out.unresolved.emplace_back("v4_virtual_failure_member_binding_unavailable"); continue; }
            if (end->event.phase == diagnostic::Phase::virtual_member_unwound || end->event.value == -1)
                out.failures.push_back({end->event.phase == diagnostic::Phase::virtual_member_unwound ?
                    V4VirtualFailureKind::member_unwound : V4VirtualFailureKind::member_no_output,
                    first->second->index, end->index, member, static_cast<std::uint32_t>(first->second->event.value), {},
                    end->event.value, end->event.phase == diagnostic::Phase::virtual_member_return});
        }
        if (!batchEnds.empty() && batchEnter->index < batchEnds[0]->index && sameDrop(batchEnds[0]->event) &&
            (batchEnds[0]->event.phase == diagnostic::Phase::virtual_batch_unwound || batchEnds[0]->event.value == 0))
            out.failures.push_back({batchEnds[0]->event.phase == diagnostic::Phase::virtual_batch_unwound ?
                V4VirtualFailureKind::batch_unwound : V4VirtualFailureKind::batch_incomplete,
                batchEnter->index, batchEnds[0]->index, out.batch, {}, {}, batchEnds[0]->event.value,
                batchEnds[0]->event.phase == diagnostic::Phase::virtual_batch_return});
        if (!pipelines.empty() && sameDrop(pipelines[0]->event) && pipelines[0]->event.value == 0)
            out.failures.push_back({V4VirtualFailureKind::pipeline_declined, pipelines[0]->index, pipelines[0]->index,
                out.batch, {}, {}, 0, true});
        if (!completionStarts.empty() && completionStarts[0]->event.value == 0)
        {
            const bool returned = !completionEnds.empty() && completionStarts[0]->index < completionEnds[0]->index &&
                completionEnds[0]->event.value == 0 && completionEnds[0]->event.thread == completionStarts[0]->event.thread;
            out.failures.push_back({V4VirtualFailureKind::completion_reported_failure, completionStarts[0]->index,
                returned ? completionEnds[0]->index : completionStarts[0]->index, out.batch, {}, {}, 0, returned});
        }
        out.unresolved.emplace_back("v4_virtual_failure_contract_causality_late_work_and_business_unverified");
    }
    catch (...) { out.observationUnavailable = true; }
    return out;
}

}

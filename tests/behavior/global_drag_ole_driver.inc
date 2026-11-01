#pragma once

#include "global_drag_model.h"
#include "global_drag_event_graph.inc"
#include "app/drag_drop/ole_drag_drop_adapter.h"
#include "common/background_work_observation.h"
#include "common/constants.h"
#include "common/types.h"
#include "shell/shell_file_operation_completion.h"
#include "drag_drop/url_drop_download_worker.h"
#include "ui/input/private_pointer.h"
#include "ole_content_source.h"
#include "ole_file_source.h"
#include "global_drag_payload_fixture.inc"
#include "global_drag_directory_fixture.inc"
#include "global_drag_directory_lease.inc"

#include <objbase.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <functional>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <wrl/client.h>

struct NativeDragIntegrationAccess;
namespace behavior { struct V4BatchCopyObservation; struct V4BatchCopyBusinessEvidence; struct V4CollectionCopyObservation; struct V4CollectionCopyEvidence; struct V4GroupCopyObservation; struct V4GroupCopyEvidence; struct V4DirectoryCopyObservation; struct V4DirectoryCopyEvidence; struct V4DesktopFilesCopyObservation; struct V4DesktopFilesCopyEvidence; struct V4FolderMappingCopyObservation; struct V4FolderMappingCopyEvidence; struct V4FileLeaveObservation; struct V4FileLeaveEvidence; struct V4FileRejectObservation; struct V4FileRejectEvidence; struct V4FileMoveObservation; struct V4FileMoveEvidence; struct V4NativeDesktopMoveObservation; struct V4NativeDesktopMoveEvidence; }
namespace behavior { struct GeneratedNativeMemberObservation; struct NativeMemberBusinessEvidence; }
namespace behavior { struct V4NativeCategoryObservation; struct V4NativeCategoryEvidence; }
namespace behavior { struct V4NativeMultiSelectionObservation; struct V4NativeMultiSelectionEvidence; }
namespace behavior { struct V4NativeMultiDragObservation; struct V4NativeMultiDragEvidence; }
namespace behavior { struct V4NativeCollectionReorderObservation; struct V4NativeCollectionReorderEvidence; }

namespace drag_global
{
// Defined by the global-model command beside its complete symbolic audit.
EventProgram Program(const std::vector<Axis>& axes, const Candidate& candidate);
}

namespace drag_global::ole_driver
{
// The observation mechanism cannot establish the requested evidence. This is
// distinct from a failed product assertion and never implies a passing case.
class ObservationUnavailable : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};
// Thrown only by an independent business oracle after it observes a definite
// result violating its contract. Missing data or I/O errors use unavailable.
class BusinessAssertionFailure : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};
// An independently checked OLE event value violated its expected contract.
// This is separate from a file/layout business assertion and missing evidence.
class EventAssertionFailure : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

struct PassiveWorkerEvidence
{
    std::uint64_t work = 0;
    std::uintptr_t worker = 0;
    std::int64_t kind = 0; // 1 = ShellReadRequest, 2 = ShellFileOperationRequest
    DWORD workerThread = 0;
    unsigned dequeued = 0, entered = 0, returned = 0, finished = 0, cancelled = 0;
    std::uintptr_t completion = 0;
    DWORD messageThread = 0;
    unsigned queueEntries = 0, queueAccepted = 0, queueFailed = 0, callbacks = 0;
    unsigned posts = 0, postFailed = 0, messageEntered = 0, messageReturned = 0;
    std::optional<std::int64_t> executeResult, callbackResult;
};
struct PassiveFilePathsGetDataEvidence
{
    std::uint64_t call = 0, work = 0;
    std::uintptr_t sourceInterface = 0; // May be a marshaled COM proxy.
    DWORD thread = 0;
    std::size_t enterIndex = (std::numeric_limits<std::size_t>::max)();
    std::size_t returnIndex = (std::numeric_limits<std::size_t>::max)();
    unsigned entered = 0, returned = 0;
    std::optional<std::int64_t> result;
};
struct PassiveTransferEvidence
{
    std::uint64_t task = 0;
    snowdesktop::external_transfer_observation::AdapterScopeIdentity adapterScope;
    std::size_t readEnqueued = 0, readWorkerEntered = 0, readWorkerReturned = 0;
    std::size_t readCallbacks = 0, fileEnqueued = 0, fileCallbacks = 0;
    std::size_t sourceCompletions = 0, fileCompletions = 0, sourceEnds = 0;
    unsigned endEntered = 0;
    std::optional<std::int64_t> endRequestedStatus, endReturnedStatus;
    std::uint64_t endRequestedEffect = 0, endLocalTask = 0, endLocalWork = 0;
    DWORD endThread = 0;
    std::uintptr_t finishedClosure = 0;
    std::uint64_t finishedWork = 0;
    DWORD finishedThread = 0;
    unsigned finishedEntered = 0, finishedReturned = 0, finishedDuplicates = 0, finishedContextMismatch = 0;
    std::optional<std::int64_t> finishedInput, finishedOutcome;
    std::uintptr_t contentObject = 0;
    std::uint64_t contentReadWork = 0;
    DWORD contentReadThread = 0, contentCommitThread = 0;
    unsigned contentReadRecords = 0, contentCommitRecords = 0;
    std::optional<std::int64_t> contentReadPathCount, contentCommitPathCount;
    std::vector<PassiveWorkerEvidence> workers;
    std::vector<PassiveFilePathsGetDataEvidence> filePathsGetData;
    std::vector<std::string> unresolved;
};
// Run only after the host has independently reached its chosen quiescent
// boundary, stopped observation, and retained the same source. The address is
// a value sampled during Transfer, never dereferenced; task generation prevents
// cross-task aggregation. This remains partial observation, not task proof.
inline PassiveTransferEvidence InspectPassiveTransfer(IDataObject* expectedSource)
{
    using namespace snowdesktop::external_transfer_observation;
    PassiveTransferEvidence result;
    if (!expectedSource || !Complete())
    { result.unresolved.emplace_back("passive_transfer_log_unavailable_or_active"); return result; }
    ReadSession session;
    if (!session.Open()) { result.unresolved.emplace_back("passive_transfer_session_unavailable"); return result; }
    const auto sourceAddress = reinterpret_cast<std::uintptr_t>(expectedSource);
    std::map<std::uint64_t, PassiveWorkerEvidence> workers;
    std::map<std::uint64_t, PassiveFilePathsGetDataEvidence> filePathsCalls;
    for (std::size_t i = 0; i < session.Count(); ++i)
    {
        Event event;
        if (!session.ReadEvent(i, event)) { result.unresolved.emplace_back("passive_transfer_log_record_unavailable"); return result; }
        if (event.phase == Phase::transfer_enter && event.object == sourceAddress)
        {
            if (result.task) { result.unresolved.emplace_back("passive_transfer_source_task_ambiguous"); return result; }
            result.task = event.task;
            result.adapterScope = event.adapterScope;
            if (!event.adapterScope.observed || !event.adapterScope.session || !event.adapterScope.call ||
                !event.adapterScope.adapter || !event.adapterScope.thread || event.adapterScope.thread != event.thread)
                result.unresolved.emplace_back("passive_transfer_adapter_scope_unavailable");
        }
    }
    if (!result.task) { result.unresolved.emplace_back("passive_transfer_source_task_unobserved"); return result; }
    for (std::size_t i = 0; i < session.Count(); ++i)
    {
        Event event;
        if (!session.ReadEvent(i, event)) { result.unresolved.emplace_back("passive_transfer_log_record_unavailable"); return result; }
        if (event.task != result.task) continue;
        const bool completionEvent = event.phase == Phase::read_queue_enter || event.phase == Phase::read_queued ||
            event.phase == Phase::read_queue_failed || event.phase == Phase::file_queue_enter || event.phase == Phase::file_queued ||
            event.phase == Phase::file_queue_failed || event.phase == Phase::read_completion_callback ||
            event.phase == Phase::file_completion_callback || event.phase == Phase::completion_posted ||
            event.phase == Phase::completion_post_failed || event.phase == Phase::completion_enter ||
            event.phase == Phase::completion_return;
        if (completionEvent)
        {
            if (!event.extra || !event.object)
                result.unresolved.emplace_back("passive_completion_work_identity_unavailable");
            else
            {
                auto& item = workers[event.extra];
                item.work = event.extra;
                if (item.completion && item.completion != event.object)
                    result.unresolved.emplace_back("passive_work_completion_identity_ambiguous");
                else item.completion = event.object;
                const std::int64_t kind = event.phase == Phase::read_queue_enter || event.phase == Phase::read_queued ||
                    event.phase == Phase::read_queue_failed || event.phase == Phase::read_completion_callback ? 1 :
                    event.phase == Phase::file_queue_enter || event.phase == Phase::file_queued ||
                    event.phase == Phase::file_queue_failed || event.phase == Phase::file_completion_callback ? 2 :
                    event.phase == Phase::completion_enter ? (event.value == 0 ? 1 : event.value == 1 ? 2 : 0) : 0;
                if (kind && item.kind && item.kind != kind) result.unresolved.emplace_back("passive_work_completion_kind_ambiguous");
                else if (kind) item.kind = kind;
                if (event.phase == Phase::read_queue_enter || event.phase == Phase::file_queue_enter) ++item.queueEntries;
                else if (event.phase == Phase::read_queued || event.phase == Phase::file_queued) ++item.queueAccepted;
                else if (event.phase == Phase::read_queue_failed || event.phase == Phase::file_queue_failed) ++item.queueFailed;
                else if (event.phase == Phase::read_completion_callback || event.phase == Phase::file_completion_callback)
                { ++item.callbacks; item.callbackResult = event.value; }
                else if (event.phase == Phase::completion_posted) ++item.posts;
                else if (event.phase == Phase::completion_post_failed) ++item.postFailed;
                else if (event.phase == Phase::completion_enter) ++item.messageEntered;
                else if (event.phase == Phase::completion_return) ++item.messageReturned;
                if (event.phase == Phase::completion_enter || event.phase == Phase::completion_return)
                {
                    if (!event.thread || (item.messageThread && item.messageThread != event.thread))
                        result.unresolved.emplace_back("passive_completion_window_thread_ambiguous");
                    else item.messageThread = event.thread;
                }
            }
        }
        switch (event.phase)
        {
        case Phase::file_paths_getdata_enter: case Phase::file_paths_getdata_return:
        {
            if (!event.extra || !event.object || !event.thread || event.localTask != result.task)
            { result.unresolved.emplace_back("passive_filedrop_caller_context_unavailable"); break; }
            auto& call = filePathsCalls[event.extra];
            call.call = event.extra;
            if ((call.sourceInterface && call.sourceInterface != event.object) ||
                (call.thread && call.thread != event.thread) ||
                ((call.entered || call.returned) && call.work != event.localWork))
                result.unresolved.emplace_back("passive_filedrop_caller_identity_ambiguous");
            else { call.sourceInterface = event.object; call.thread = event.thread; call.work = event.localWork; }
            if (event.phase == Phase::file_paths_getdata_enter)
            { ++call.entered; call.enterIndex = i; }
            else { ++call.returned; call.returnIndex = i; call.result = event.value; }
            break;
        }
        case Phase::read_queued: ++result.readEnqueued; break;
        case Phase::read_worker_enter: ++result.readWorkerEntered; break;
        case Phase::read_worker_return: ++result.readWorkerReturned; break;
        case Phase::read_completion_callback: ++result.readCallbacks; break;
        case Phase::file_queued: ++result.fileEnqueued; break;
        case Phase::file_completion_callback: ++result.fileCallbacks; break;
        case Phase::completion_enter:
            if (event.value == 0) ++result.sourceCompletions;
            else if (event.value == 1) ++result.fileCompletions;
            else result.unresolved.emplace_back("passive_completion_kind_invalid");
            break;
        case Phase::end_operation_enter:
            ++result.endEntered;
            result.endRequestedStatus = event.value;
            result.endRequestedEffect = event.extra;
            result.endLocalTask = event.localTask;
            result.endLocalWork = event.localWork;
            result.endThread = event.thread;
            break;
        case Phase::end_operation:
            ++result.sourceEnds;
            result.endReturnedStatus = event.value;
            if (result.endThread != event.thread || result.endLocalTask != event.localTask ||
                result.endLocalWork != event.localWork)
                result.unresolved.emplace_back("passive_end_operation_scope_ambiguous");
            break;
        case Phase::read_content_paths: case Phase::commit_content_paths:
            if (!event.object || !event.extra || !event.thread || event.value < 0 ||
                (result.contentObject && result.contentObject != event.object) ||
                (result.contentReadWork && result.contentReadWork != event.extra))
                result.unresolved.emplace_back("passive_content_paths_identity_or_count_unavailable");
            else
            {
                result.contentObject = event.object;
                result.contentReadWork = event.extra;
                if (event.phase == Phase::read_content_paths)
                { ++result.contentReadRecords; result.contentReadPathCount = event.value; result.contentReadThread = event.thread; }
                else
                { ++result.contentCommitRecords; result.contentCommitPathCount = event.value; result.contentCommitThread = event.thread; }
            }
            break;
        case Phase::transfer_finished_enter: case Phase::transfer_finished_return:
        case Phase::transfer_finished_duplicate: case Phase::transfer_finished_context_mismatch:
            if (!event.object || (result.finishedClosure && result.finishedClosure != event.object) ||
                !event.thread || (result.finishedThread && result.finishedThread != event.thread) ||
                (result.finishedWork && result.finishedWork != event.extra))
                result.unresolved.emplace_back("passive_finished_closure_or_context_ambiguous");
            else
            {
                result.finishedClosure = event.object;
                result.finishedThread = event.thread;
                result.finishedWork = event.extra;
            }
            if (event.phase == Phase::transfer_finished_enter)
            { ++result.finishedEntered; result.finishedInput = event.value; }
            else if (event.phase == Phase::transfer_finished_return)
            { ++result.finishedReturned; result.finishedOutcome = event.value; }
            else if (event.phase == Phase::transfer_finished_duplicate) ++result.finishedDuplicates;
            else ++result.finishedContextMismatch;
            break;
        case Phase::ambiguous_nested_scope: result.unresolved.emplace_back("passive_nested_scope_unresolved"); break;
        case Phase::read_queue_failed: case Phase::file_queue_failed: case Phase::completion_post_failed:
            result.unresolved.emplace_back("passive_queue_or_post_failure_unresolved"); break;
        case Phase::worker_dequeued: case Phase::worker_execute_enter: case Phase::worker_execute_return:
        case Phase::worker_task_finished: case Phase::worker_task_cancelled:
        {
            if (!event.extra || !event.object || !event.thread)
            { result.unresolved.emplace_back("passive_worker_identity_unavailable"); break; }
            auto& item = workers[event.extra];
            item.work = event.extra;
            if (item.worker && item.worker != event.object)
                result.unresolved.emplace_back("passive_worker_instance_ambiguous");
            else item.worker = event.object;
            if (event.phase != Phase::worker_task_cancelled)
            {
                if (item.workerThread && item.workerThread != event.thread)
                    result.unresolved.emplace_back("passive_worker_thread_ambiguous");
                else item.workerThread = event.thread;
            }
            if (event.phase == Phase::worker_dequeued || event.phase == Phase::worker_execute_enter ||
                event.phase == Phase::worker_task_finished || event.phase == Phase::worker_task_cancelled)
            {
                if ((event.value != 1 && event.value != 2) || (item.kind && item.kind != event.value))
                    result.unresolved.emplace_back("passive_worker_kind_ambiguous");
                else item.kind = event.value;
            }
            if (event.phase == Phase::worker_dequeued) ++item.dequeued;
            else if (event.phase == Phase::worker_execute_enter) ++item.entered;
            else if (event.phase == Phase::worker_execute_return) { ++item.returned; item.executeResult = event.value; }
            else if (event.phase == Phase::worker_task_finished) ++item.finished;
            else ++item.cancelled;
            break;
        }
        default: break;
        }
    }
    for (auto& [id, call] : filePathsCalls)
    {
        (void)id;
        if (call.entered != 1 || call.returned != 1 || !call.result || call.enterIndex >= call.returnIndex)
            result.unresolved.emplace_back("passive_filedrop_caller_return_incomplete_or_ambiguous");
        if (call.work && !workers.contains(call.work))
            result.unresolved.emplace_back("passive_filedrop_caller_work_unavailable");
        result.filePathsGetData.push_back(std::move(call));
    }
    if (!result.filePathsGetData.empty())
        result.unresolved.emplace_back("passive_filedrop_proxy_source_and_reentrancy_unverified");
    for (auto& [work, item] : workers)
    {
        (void)work;
        if (item.dequeued > 1 || item.entered > 1 || item.returned > 1 || item.finished > 1 || item.cancelled > 1 ||
            (item.cancelled && (item.dequeued || item.entered || item.returned || item.finished)) ||
            (item.returned && (!item.entered || !item.dequeued)) || (item.finished && !item.dequeued) ||
            item.queueEntries > 1 || item.queueAccepted > 1 || item.queueFailed > 1 || item.callbacks > 1 ||
            item.posts > 1 || item.postFailed > 1 || item.messageEntered > 1 || item.messageReturned > 1 ||
            (item.queueAccepted && item.queueFailed) || (item.posts && item.postFailed))
            result.unresolved.emplace_back("passive_worker_lifecycle_ambiguous");
        if (item.cancelled) result.unresolved.emplace_back("passive_worker_stop_cancelled_not_executed");
        else if (item.dequeued != 1 || item.entered != 1 || item.returned != 1 || item.finished != 1)
            result.unresolved.emplace_back("passive_worker_execution_incomplete_or_com_unavailable");
        result.workers.push_back(std::move(item));
    }
    if (result.workers.empty()) result.unresolved.emplace_back("passive_worker_identity_unobserved");
    if (result.endEntered != 1 || result.sourceEnds != 1 || !result.endRequestedStatus ||
        !result.endReturnedStatus || !result.endThread || result.endLocalTask != result.task || !result.endLocalWork)
        result.unresolved.emplace_back("passive_end_operation_context_unavailable");
    const auto contentWorker = std::find_if(result.workers.begin(), result.workers.end(),
        [&](const auto& worker) { return worker.work == result.contentReadWork; });
    if (result.contentReadRecords != 1 || result.contentCommitRecords != 1 || !result.contentReadPathCount ||
        !result.contentCommitPathCount || *result.contentReadPathCount != *result.contentCommitPathCount ||
        contentWorker == result.workers.end() || contentWorker->kind != 1 || contentWorker->entered != 1 ||
        contentWorker->returned != 1 || contentWorker->messageEntered != 1 || contentWorker->messageReturned != 1 ||
        !result.contentReadThread || contentWorker->workerThread != result.contentReadThread ||
        !result.contentCommitThread || contentWorker->messageThread != result.contentCommitThread)
        result.unresolved.emplace_back("passive_content_read_to_commit_context_unavailable");
    result.unresolved.emplace_back("passive_content_count_not_bound_to_same_ole_host_epoch_or_business_result");
    if (result.finishedEntered != 1 || result.finishedReturned != 1 || result.finishedDuplicates ||
        result.finishedContextMismatch || !result.finishedWork || !result.finishedInput || !result.finishedOutcome ||
        *result.finishedInput < 0 || *result.finishedInput > 1 || *result.finishedOutcome != *result.finishedInput)
        result.unresolved.emplace_back("passive_transfer_finished_lifecycle_unavailable");
    const auto finishedWorker = std::find_if(result.workers.begin(), result.workers.end(),
        [&](const auto& worker) { return worker.work == result.finishedWork; });
    if (finishedWorker == result.workers.end() || finishedWorker->kind != 2 ||
        finishedWorker->messageEntered != 1 || finishedWorker->messageReturned != 1 ||
        !result.finishedThread || finishedWorker->messageThread != result.finishedThread)
        result.unresolved.emplace_back("passive_finished_file_completion_context_unavailable");
    result.unresolved.emplace_back("passive_finished_current_context_reentrancy_not_excluded");
    result.unresolved.emplace_back("passive_message_identity_return_and_reentrant_commit_binding_unimplemented");
    result.unresolved.emplace_back("passive_worker_stop_or_failure_and_business_result_unimplemented");
    return result;
}

// This executor understands only the legacy all-required graph identity.
// A future presence-aware replay must explicitly bind each hypothesis to an
// observed production interval before this guard can be replaced.
inline bool RequireLegacyGraphPresence(const event_graph::Spec& spec,
    std::vector<std::string>& obligations, std::string_view prefix)
{
    bool supported = true;
    for (const auto& requirement : spec.requirements)
    {
        if (requirement.presence != event_graph::PresencePolicy::required || !requirement.presenceCondition.empty())
        {
            obligations.push_back(std::string(prefix) + "conditional_presence_unimplemented:" + requirement.id);
            supported = false;
        }
    }
    if (!spec.presenceConstraints.empty())
    {
        obligations.push_back(std::string(prefix) + "presence_constraints_unimplemented");
        supported = false;
    }
    return supported;
}
inline bool RequireLegacyGraphVersion(const event_graph::Variant& selected,
    std::vector<std::string>& obligations, std::string_view prefix)
{
    std::string_view remaining(selected.id), version;
    if (!event_graph::ReadGraphToken(remaining, version) || version != "global_drag_event_graph_v1")
    {
        obligations.push_back(std::string(prefix) + "graph_version_unimplemented_or_invalid");
        return false;
    }
    return true;
}

enum class FileSourceEvidenceState { partial_observation, pending, unresolved, mismatch };
struct FileSourceAxisEvidence
{
    std::string id, candidateValue;
    FileSourceEvidenceState state = FileSourceEvidenceState::pending;
    std::string detail;
};
struct FileSourceEvidence
{
    std::vector<FileSourceAxisEvidence> axes;
    std::vector<std::string> obligations;
    // Counts are source callbacks observed since the baseline, not evidence
    // of the host's task identity, completion message or business outcome.
    std::optional<int> sourceStarts, sourceEnds;
    std::optional<HRESULT> sourceOutcome;
    std::optional<DWORD> sourceEndEffect;
};

// Call only after ValidatePayloadCandidate has rebuilt the canonical schema
// and index. This identifies the full candidate, including outcome hypotheses;
// it does not treat those hypotheses as fixture inputs or observations.
inline std::string FileSourceCandidateBinding(const std::vector<Axis>& canonical,
    const Candidate& safe, SelectionSchemaVersion version, const V3FamilyValidatedContext* v3 = nullptr)
{
    std::string result;
    event_graph::AppendToken(result, "single_file_source_candidate_v1");
    event_graph::AppendToken(result, v3 ? "source_members_v3_family" :
        version == SelectionSchemaVersion::source_members_v2 ? "source_members_v2" : "legacy_v1");
    if (v3)
    {
        event_graph::AppendToken(result, v3->FamilyBinding());
        event_graph::AppendToken(result, std::to_string(canonical.size()));
        for (const auto& axis : canonical)
        {
            event_graph::AppendToken(result, axis.id); event_graph::AppendToken(result, RoleName(axis.role));
            event_graph::AppendToken(result, axis.label); event_graph::AppendToken(result, std::to_string(axis.values.size()));
            for (const auto& value : axis.values) event_graph::AppendToken(result, value);
        }
    }
    event_graph::AppendToken(result, Id(canonical, safe));
    return result;
}

inline std::string ValidateFileSourceCandidate(const std::vector<Axis>& axes, const Candidate& input,
    std::vector<Axis>& schema, Candidate& safe, SelectionSchemaVersion version,
    const V3FamilyValidatedContext* v3)
{
    if (!v3) return ValidatePayloadCandidate(axes, input, schema, safe, version);
    if (version != SelectionSchemaVersion::source_members_v2 || v3->Status() != V3CoordinateStatus::coordinate_ready ||
        &axes != &v3->Axes() || &input != &v3->Coordinate() || v3->TaskBActive())
        return "v3_file_source_authoritative_coordinate_unavailable";
    const auto selection = v3->AuditSelectionA();
    if (selection.state != V3SelectionAuditState::unresolved ||
        selection.reason != "v3_selection_remaining_structure_and_execution_unresolved")
        return std::string("v3_file_source_selection_unavailable:") + std::string(selection.reason);
    const auto transfer = v3->AuditTransfer();
    if (!transfer.invalid.empty() || !transfer.localExclusion.empty())
        return "v3_file_source_transfer_structure_unavailable";
    schema = axes;
    safe = input;
    return {};
}

// Read-only input evidence for one already-constructed Shell source. The
// caller must retain the SAME fixture, source and pre-operation snapshot on
// the owning STA through completion, and supply pre-operation log offsets.
// No QueryGetData/GetData is issued here. Observed format offers and returned
// CF_HDROP data are partial input evidence, never proof of host acceptance,
// production-resolved count, completion or an independent business result.
inline FileSourceEvidence InspectSingleFileSourceEvidence(const std::vector<Axis>& axes,
    const Candidate& input, const PayloadFixtureResult& fixture, const PayloadInputSnapshot& baseline,
    const ole_file_source_fixture::Source& source, std::size_t fileReadStart, std::size_t formatProbeStart,
    SelectionSchemaVersion version = SelectionSchemaVersion::legacy_v1,
    const V3FamilyValidatedContext* v3 = nullptr)
{
    FileSourceEvidence result;
    std::vector<Axis> schema;
    Candidate candidate;
    if (const auto reason = ValidateFileSourceCandidate(axes, input, schema, candidate, version, v3); !reason.empty())
    { result.obligations.push_back("file_source_candidate_unavailable:" + reason); return result; }
    if (!v3 && version == SelectionSchemaVersion::source_members_v2 &&
        !SelectionV2HasExplicitFileSource(schema, candidate))
    { result.obligations.emplace_back("file_source_v2_source_vector_semantics_unimplemented"); return result; }
    if (version == SelectionSchemaVersion::source_members_v2 &&
        (Value(schema, candidate, "P02.source_actual_count") != "one" ||
         Value(schema, candidate, "P01.primary") != "file" || Value(schema, candidate, "P01.second") != "na"))
    { result.obligations.emplace_back("file_source_single_file_candidate_unavailable"); return result; }
    if (!fixture.payload_prepared || fixture.axes.size() != schema.size() || !baseline.captured ||
        fixture.members.size() != 1 || baseline.memberEntryIndices.size() != 1 ||
        baseline.memberEntryIndices.front() >= baseline.entries.size() || baseline.root != fixture.directory)
    { result.obligations.emplace_back("file_source_fixture_or_baseline_unavailable"); return result; }
    for (std::size_t i = 0; i < schema.size(); ++i)
        if (fixture.axes[i].id != schema[i].id || fixture.axes[i].value != candidate.values[i])
        { result.obligations.push_back("file_source_candidate_mismatch:" + schema[i].id); return result; }
    if (!source.HasConstructionShell() || !source.multiParentPaths.empty() || source.owner != GetCurrentThreadId() ||
        source.wrongApartment || source.fileDropObservationFailed ||
        source.formatProbeObservationFailed || source.fileReads < 0 ||
        source.fileDropReads.size() != static_cast<std::size_t>(source.fileReads) ||
        fileReadStart > source.fileDropReads.size() || formatProbeStart > source.formatProbes.size())
    { result.obligations.emplace_back("file_source_log_unavailable_or_incomplete"); return result; }
    const auto value = [&](std::string_view id) { return Value(schema, candidate, id); };
    const auto append = [&](std::string_view id, FileSourceEvidenceState state, std::string detail) {
        const auto actual = value(id);
        const auto axis = candidate.lookup->find(std::string(id));
        const bool derived = axis != candidate.lookup->end() && schema[axis->second].role == Role::Derived;
        if ((!derived && actual.empty()) || actual.find("unknown") != std::string_view::npos ||
            actual.find("unresolved") != std::string_view::npos)
            state = FileSourceEvidenceState::unresolved;
        result.axes.push_back({std::string(id), std::string(actual), state, std::move(detail)});
    };
    const auto& member = fixture.members.front();
    const auto& expected = baseline.entries[baseline.memberEntryIndices.front()];
    const auto current = CapturePayloadInputSnapshot(fixture);
    if (!current.captured || current.memberEntryIndices.size() != 1 ||
        current.memberEntryIndices.front() >= current.entries.size())
    { result.obligations.emplace_back("file_source_current_snapshot_unavailable"); return result; }
    const auto& actual = current.entries[current.memberEntryIndices.front()];
    const bool snapshotSame = member.kind == "file" && expected.kind == "file" && actual.kind == "file" &&
        member.path.lexically_relative(baseline.root) == expected.relative && actual.relative == expected.relative &&
        member.bytes == expected.bytes && actual.bytes == expected.bytes && actual.volumeSerial == expected.volumeSerial &&
        actual.fileIndexHigh == expected.fileIndexHigh && actual.fileIndexLow == expected.fileIndexLow &&
        (expected.fileIndexHigh != 0 || expected.fileIndexLow != 0);
    if (!snapshotSame) result.obligations.emplace_back("file_source_snapshot_identity_or_content_unavailable");
    bool sawFilePaths = false, sawImage = false, sawText = false, sawShell = false, sawUrl = false, sawDescriptor = false;
    for (std::size_t i = formatProbeStart; i < source.formatProbes.size(); ++i)
    {
        const auto& probe = source.formatProbes[i];
        if (probe.threadId != source.owner)
        { result.obligations.emplace_back("file_source_probe_thread_unavailable"); return result; }
        if (!probe.hasFormat || probe.targetDevicePresent || probe.aspect != DVASPECT_CONTENT ||
            probe.index != -1 || probe.result != S_OK) continue;
        const bool global = (probe.tymed & TYMED_HGLOBAL) != 0;
        sawFilePaths |= probe.format == CF_HDROP && global;
        sawImage |= ((probe.format == CF_DIB || probe.format == CF_DIBV5) && global) ||
                    (probe.format == CF_BITMAP && (probe.tymed & TYMED_GDI));
        sawText |= probe.format == CF_UNICODETEXT && global;
        if (probe.format >= 0xc000)
        {
            wchar_t name[256]{};
            const DWORD lastError = GetLastError();
            const int length = GetClipboardFormatNameW(probe.format, name, static_cast<int>(std::size(name)));
            SetLastError(lastError);
            if (length > 0 && length < static_cast<int>(std::size(name)) - 1)
            {
                const std::wstring_view format(name, static_cast<std::size_t>(length));
                sawShell |= global && format == L"Shell IDList Array";
                sawDescriptor |= global && (format == L"FileGroupDescriptorW" || format == L"FileGroupDescriptor");
                sawUrl |= global && (format == L"UniformResourceLocatorW" || format == L"UniformResourceLocator");
                sawImage |= (probe.tymed & (TYMED_HGLOBAL | TYMED_ISTREAM)) &&
                    (format == L"PNG" || format == L"image/png");
            }
        }
    }
    bool matchedRead = false, divergentRead = false;
    for (std::size_t i = fileReadStart; i < source.fileDropReads.size(); ++i)
    {
        const auto& read = source.fileDropReads[i];
        if (read.threadId != source.owner)
        { result.obligations.emplace_back("file_source_read_thread_unavailable"); return result; }
        if (FAILED(read.result)) continue;
        if (read.capture != ole_file_source_fixture::FileDropCapture::captured)
        { result.obligations.emplace_back("file_source_read_capture_unavailable"); return result; }
        sawFilePaths = true;
        bool same = false;
        if (read.paths.size() == 1 && snapshotSame && ObserveJointSameObject(read.paths.front(), member.path, same))
        { if (same) matchedRead = true; else divergentRead = true; }
        else if (read.paths.size() != 1) divergentRead = true;
        else result.obligations.emplace_back("file_source_read_identity_unavailable");
    }
    const auto offer = [&](std::string_view id, bool observed) {
        const auto candidateValue = value(id);
        append(id, !observed ? FileSourceEvidenceState::pending : candidateValue == "yes" ?
            FileSourceEvidenceState::partial_observation : FileSourceEvidenceState::mismatch,
            observed ? "production_query_or_read_offered_format; other_formats_unobserved" : "production_did_not_confirm_offer_or_absence");
    };
    offer("T02.file_paths", sawFilePaths);
    offer("T02.image", sawImage);
    offer("T02.text", sawText);
    offer("T02.shell_object", sawShell);
    offer("T02.url", sawUrl);
    append("T02.virtual_files", FileSourceEvidenceState::pending, sawDescriptor ?
        "descriptor_offered_stream_set_and_count_unobserved" : "virtual_descriptor_and_streams_unobserved");
    for (const auto id : {"T02.virtual_completeness", "T02.url_kind", "T02.invalid_format", "T02",
                          "T04.capability", "T04.availability", "P02.source_actual_count", "P02.host_summary_count"})
        append(id, FileSourceEvidenceState::pending, "source_or_host_axis_not_established_by_these_records");
    if (divergentRead) result.obligations.emplace_back("file_source_observed_paths_differ_from_single_fixture_file");
    append("P01.primary", matchedRead && !divergentRead && value("P01.primary") == "file" ?
        FileSourceEvidenceState::partial_observation : FileSourceEvidenceState::pending,
        "actual_captured_hdrop_and_live_file_identity_only");
    append("P02.resolved_count", FileSourceEvidenceState::pending,
        "one_path_in_source_medium_not_host_resolved_count");
    if (version == SelectionSchemaVersion::source_members_v2)
        append("P02.many_count", FileSourceEvidenceState::pending,
            "output_hypothesis_requires_independent_host_observation");
    result.obligations.emplace_back("file_source_non_hdrop_getdata_payloads_and_capability_calls_unobserved");
    result.obligations.emplace_back("file_source_observations_do_not_authorize_execution_or_establish_business_results");
    return result;
}

struct FileSourceInitialValidation
{
    bool matches = false;
    std::vector<std::string> obligations;
};

// Capture before the source is submitted to the host. Identity is a weak,
// non-COM token: every capture, validation, postflight and local-count audit
// must receive a Source strongly owned by the caller throughout the call on
// its STA. Comparing that Source's token rejects a recycled address. A host
// epoch separately covers fixture lifetime; neither proves business success.
class FileSourceInitialObservation
{
public:
    FileSourceInitialObservation() = default;
    FileSourceInitialObservation(const FileSourceInitialObservation&) = delete;
    FileSourceInitialObservation& operator=(const FileSourceInitialObservation&) = delete;
    FileSourceInitialObservation(FileSourceInitialObservation&&) = default;
    FileSourceInitialObservation& operator=(FileSourceInitialObservation&&) = delete;
    bool Captured() const noexcept { return captured && ownerThread == GetCurrentThreadId(); }
    const std::vector<std::string>& Obligations() const noexcept { return obligations; }
private:
    friend FileSourceInitialObservation CaptureSingleFileSourceInitial(const std::vector<Axis>&,
        const Candidate&, const PayloadFixtureResult&, const PayloadInputSnapshot&, ole_file_source_fixture::Source&,
        SelectionSchemaVersion, const V3FamilyValidatedContext*);
    friend FileSourceEvidence InspectSingleFileSourceEvidence(const std::vector<Axis>&,
        const Candidate&, const PayloadFixtureResult&, const FileSourceInitialObservation&,
        const ole_file_source_fixture::Source&, SelectionSchemaVersion, const V3FamilyValidatedContext*);
    friend FileSourceInitialValidation ValidateSingleFileSourceInitial(const std::vector<Axis>&,
        const Candidate&, const PayloadFixtureResult&, const PayloadInputSnapshot&,
        ole_file_source_fixture::Source&, const FileSourceInitialObservation&, SelectionSchemaVersion, const V3FamilyValidatedContext*);
    friend class V3LocalPresenceObservation;
    friend class V3SessionPresenceObservation;
    bool MatchesSource(const ole_file_source_fixture::Source& source) const noexcept
    {
        if (!Captured() || source.owner != ownerThread || sourceAddress != reinterpret_cast<std::uintptr_t>(&source))
            return false;
        const auto actual = source.LifetimeIdentity();
        return !sourceIdentity.expired() && !actual.expired() &&
            !sourceIdentity.owner_before(actual) && !actual.owner_before(sourceIdentity);
    }
    std::weak_ptr<const ole_file_source_fixture::Source::LifetimeToken> sourceIdentity;
    std::uintptr_t sourceAddress = 0;
    DWORD ownerThread = 0;
    std::string candidateId;
    SelectionSchemaVersion version = SelectionSchemaVersion::legacy_v1;
    bool v3Family = false;
    PayloadInputSnapshot input;
    std::size_t fileReadStart = 0, formatProbeStart = 0;
    int starts = 0, ends = 0;
    bool initialAsyncMode = false;
    std::vector<std::string> obligations;
    bool captured = false;
};
inline FileSourceInitialObservation CaptureSingleFileSourceInitial(const std::vector<Axis>& axes,
    const Candidate& input, const PayloadFixtureResult& fixture, const PayloadInputSnapshot& expected,
    ole_file_source_fixture::Source& source, SelectionSchemaVersion version = SelectionSchemaVersion::legacy_v1,
    const V3FamilyValidatedContext* v3 = nullptr)
{
    FileSourceInitialObservation result;
    std::vector<Axis> schema;
    Candidate candidate;
    if (const auto reason = ValidateFileSourceCandidate(axes, input, schema, candidate, version, v3); !reason.empty())
    { result.obligations.push_back("file_source_initial_candidate_unavailable:" + reason); return result; }
    if (!v3 && version == SelectionSchemaVersion::source_members_v2 &&
        !SelectionV2HasExplicitFileSource(schema, candidate))
    { result.obligations.emplace_back("file_source_v2_source_vector_semantics_unimplemented"); return result; }
    if (version == SelectionSchemaVersion::source_members_v2 &&
        (Value(schema, candidate, "P02.source_actual_count") != "one" ||
         Value(schema, candidate, "P01.primary") != "file" || Value(schema, candidate, "P01.second") != "na"))
    { result.obligations.emplace_back("file_source_single_file_candidate_unavailable"); return result; }
    if (!fixture.payload_prepared || fixture.axes.size() != schema.size() || fixture.members.size() != 1 ||
        !expected.captured || expected.root != fixture.directory || !source.HasConstructionShell() || !source.multiParentPaths.empty() ||
        source.owner != GetCurrentThreadId() || source.wrongApartment || source.fileDropObservationFailed ||
        source.formatProbeObservationFailed || source.fileReads < 0 ||
        source.fileDropReads.size() != static_cast<std::size_t>(source.fileReads) || source.starts != 0 || source.ends != 0)
    { result.obligations.emplace_back("file_source_initial_fixture_or_source_unavailable"); return result; }
    for (std::size_t i = 0; i < schema.size(); ++i)
        if (fixture.axes[i].id != schema[i].id || fixture.axes[i].value != candidate.values[i])
        { result.obligations.push_back("file_source_initial_candidate_mismatch:" + schema[i].id); return result; }
    const auto current = CapturePayloadInputSnapshot(fixture);
    if (!current.captured || current.root != expected.root || current.targetRelative != expected.targetRelative ||
        current.desktopRelative != expected.desktopRelative || current.sourceMappingRelative != expected.sourceMappingRelative ||
        current.targetMappingRelative != expected.targetMappingRelative || current.desktopEntryIndex != expected.desktopEntryIndex ||
        current.sourceMappingEntryIndex != expected.sourceMappingEntryIndex || current.targetMappingEntryIndex != expected.targetMappingEntryIndex ||
        current.scope != expected.scope || current.memberEntryIndices != expected.memberEntryIndices ||
        current.entries.size() != expected.entries.size())
    { result.obligations.emplace_back("file_source_initial_snapshot_changed_or_unavailable"); return result; }
    for (std::size_t i = 0; i < current.entries.size(); ++i)
    {
        const auto& actual = current.entries[i]; const auto& prior = expected.entries[i];
        if (actual.relative != prior.relative || actual.kind != prior.kind || actual.bytes != prior.bytes ||
            actual.volumeSerial != prior.volumeSerial || actual.fileIndexHigh != prior.fileIndexHigh ||
            actual.fileIndexLow != prior.fileIndexLow)
        { result.obligations.emplace_back("file_source_initial_snapshot_changed"); return result; }
    }
    result.ownerThread = GetCurrentThreadId();
    result.sourceIdentity = source.LifetimeIdentity();
    result.sourceAddress = reinterpret_cast<std::uintptr_t>(&source);
    result.candidateId = FileSourceCandidateBinding(schema, candidate, version, v3);
    result.version = version;
    result.v3Family = v3 != nullptr;
    result.input = current;
    result.fileReadStart = source.fileDropReads.size(); result.formatProbeStart = source.formatProbes.size();
    result.starts = source.starts; result.ends = source.ends; result.initialAsyncMode = source.asynchronous;
    result.captured = true;
    result.obligations.emplace_back("file_source_initial_config_is_not_host_capability_negotiation");
    return result;
}

// Compare with a newly captured baseline on the owning STA, before any
// production dispatch. This is a non-atomic snapshot and a source identity
// check; it neither queries the source nor proves host binding or execution.
inline FileSourceInitialValidation ValidateSingleFileSourceInitial(const std::vector<Axis>& axes,
    const Candidate& candidate, const PayloadFixtureResult& fixture, const PayloadInputSnapshot& expected,
    ole_file_source_fixture::Source& source, const FileSourceInitialObservation& initial,
    SelectionSchemaVersion version = SelectionSchemaVersion::legacy_v1,
    const V3FamilyValidatedContext* v3 = nullptr)
{
    FileSourceInitialValidation result;
    const auto fresh = CaptureSingleFileSourceInitial(axes, candidate, fixture, expected, source, version, v3);
    result.obligations = fresh.obligations;
    if (!fresh.MatchesSource(source) || !initial.MatchesSource(source) ||
        initial.version != version || initial.v3Family != (v3 != nullptr) || initial.candidateId != fresh.candidateId ||
        initial.fileReadStart != fresh.fileReadStart || initial.formatProbeStart != fresh.formatProbeStart ||
        initial.starts != fresh.starts || initial.ends != fresh.ends || initial.initialAsyncMode != fresh.initialAsyncMode)
    { result.obligations.emplace_back("file_source_initial_identity_or_log_baseline_changed"); return result; }
    const auto& a = initial.input;
    const auto& b = fresh.input;
    if (!a.captured || !b.captured || a.root != b.root || a.targetRelative != b.targetRelative ||
        a.desktopRelative != b.desktopRelative || a.sourceMappingRelative != b.sourceMappingRelative ||
        a.targetMappingRelative != b.targetMappingRelative || a.desktopEntryIndex != b.desktopEntryIndex ||
        a.sourceMappingEntryIndex != b.sourceMappingEntryIndex || a.targetMappingEntryIndex != b.targetMappingEntryIndex ||
        a.memberEntryIndices != b.memberEntryIndices || a.entries.size() != b.entries.size() || a.scope != b.scope)
    { result.obligations.emplace_back("file_source_initial_snapshot_binding_changed"); return result; }
    for (std::size_t i = 0; i < a.entries.size(); ++i)
    {
        const auto& left = a.entries[i]; const auto& right = b.entries[i];
        if (left.relative != right.relative || left.kind != right.kind || left.bytes != right.bytes ||
            left.volumeSerial != right.volumeSerial || left.fileIndexHigh != right.fileIndexHigh ||
            left.fileIndexLow != right.fileIndexLow)
        { result.obligations.emplace_back("file_source_initial_snapshot_entry_changed"); return result; }
    }
    result.matches = true;
    result.obligations.insert(result.obligations.end(), initial.obligations.begin(), initial.obligations.end());
    result.obligations.emplace_back("file_source_snapshot_is_non_atomic_and_host_binding_unobserved");
    return result;
}

inline FileSourceEvidence InspectSingleFileSourceEvidence(const std::vector<Axis>& axes,
    const Candidate& candidate, const PayloadFixtureResult& fixture, const FileSourceInitialObservation& initial,
    const ole_file_source_fixture::Source& source, SelectionSchemaVersion version = SelectionSchemaVersion::legacy_v1,
    const V3FamilyValidatedContext* v3 = nullptr)
{
    FileSourceEvidence result;
    std::vector<Axis> schema;
    Candidate safe;
    if (const auto reason = ValidateFileSourceCandidate(axes, candidate, schema, safe, version, v3); !reason.empty())
    { result.obligations.push_back("file_source_candidate_unavailable:" + reason); return result; }
    if (!initial.MatchesSource(source) || initial.version != version ||
        initial.v3Family != (v3 != nullptr) || FileSourceCandidateBinding(schema, safe, version, v3) != initial.candidateId ||
        source.owner != GetCurrentThreadId() || source.starts < initial.starts || source.ends < initial.ends)
    { result.obligations.emplace_back("file_source_initial_identity_or_lease_unavailable"); return result; }
    result = InspectSingleFileSourceEvidence(v3 ? axes : schema, v3 ? candidate : safe, fixture, initial.input, source,
        initial.fileReadStart, initial.formatProbeStart, version, v3);
    if (result.axes.empty()) return result;
    for (auto& axis : result.axes)
        if (axis.id == "T04.capability")
        {
            const bool matches = (axis.candidateValue == "enabled" && initial.initialAsyncMode) ||
                (axis.candidateValue == "present_disabled" && !initial.initialAsyncMode);
            axis.state = matches ? FileSourceEvidenceState::partial_observation : FileSourceEvidenceState::mismatch;
            axis.detail = "source_fixture_implements_async_interface_and_initial_mode_only; host_query_unobserved";
        }
    result.sourceStarts = source.starts - initial.starts;
    result.sourceEnds = source.ends - initial.ends;
    if (*result.sourceEnds > 0) { result.sourceOutcome = source.outcome; result.sourceEndEffect = source.endEffect; }
    result.obligations.insert(result.obligations.end(), initial.obligations.begin(), initial.obligations.end());
    result.obligations.emplace_back("file_source_capability_query_transitions_and_host_completion_unobserved");
    return result;
}

// Explicit V3 wrappers prevent a V3 fixture from being validated under the
// legacy V2 schema. The context owns the exact family/schema/candidate view.
inline FileSourceInitialObservation CaptureV3SingleFileSourceInitial(const V3FamilyValidatedContext& context,
    const PayloadFixtureResult& fixture, const PayloadInputSnapshot& expected, ole_file_source_fixture::Source& source)
{
    return CaptureSingleFileSourceInitial(context.Axes(), context.Coordinate(), fixture, expected, source,
        SelectionSchemaVersion::source_members_v2, &context);
}
inline FileSourceInitialValidation ValidateV3SingleFileSourceInitial(const V3FamilyValidatedContext& context,
    const PayloadFixtureResult& fixture, const PayloadInputSnapshot& expected, ole_file_source_fixture::Source& source,
    const FileSourceInitialObservation& initial)
{
    return ValidateSingleFileSourceInitial(context.Axes(), context.Coordinate(), fixture, expected, source, initial,
        SelectionSchemaVersion::source_members_v2, &context);
}
inline FileSourceEvidence InspectV3SingleFileSourceEvidence(const V3FamilyValidatedContext& context,
    const PayloadFixtureResult& fixture, const FileSourceInitialObservation& initial,
    const ole_file_source_fixture::Source& source)
{
    return InspectSingleFileSourceEvidence(context.Axes(), context.Coordinate(), fixture, initial, source,
        SelectionSchemaVersion::source_members_v2, &context);
}

// Partial, pre-dispatch V4 file-input evidence. The source and held root are
// borrowed: callers must keep both alive on the source STA. A path snapshot
// does not by itself prove what the Shell IDataObject returns to production.
struct V4SingleFileInputObservation
{
    bool captured = false, resourceUnavailable = false;
    std::string candidateId;
    std::filesystem::path root, file;
    FILE_ID_INFO rootId{}, fileId{};
    std::vector<std::byte> bytes;
    DWORD ownerThread = 0;
    std::uintptr_t sourceAddress = 0;
    std::weak_ptr<const ole_file_source_fixture::Source::LifetimeToken> sourceLifetime;
    std::vector<std::string> unresolved;
};

inline V4SingleFileInputObservation InspectV4SingleFileInputs(const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const V4SingleFileFixture& fixture,
    HANDLE trustedRoot, const FILE_ID_INFO& trustedRootId, ole_file_source_fixture::Source& source)
{
    V4SingleFileInputObservation result;
    try
    {
        if (verified.Status() != V4CoordinateStatus::coordinate_ready ||
            identity.Status() != V4FamilyStatus::schema_only || verified.TaskBActive() ||
            verified.FamilyBinding() != identity.FamilyBinding() || verified.Axes().size() != identity.Axes().size())
        { result.unresolved.emplace_back("v4_file_input_context_unavailable"); return result; }
        const auto& axes = verified.Axes(); const auto& candidate = verified.Coordinate();
        for (std::size_t i = 0; i < axes.size(); ++i)
        {
            const auto& a = axes[i]; const auto& b = identity.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.values != b.values || a.label != b.label)
            { result.unresolved.emplace_back("v4_file_input_schema_mismatch"); return result; }
        }
        const auto raw = identity.RawCandidateIdentity(candidate);
        if (raw.resourceUnavailable) result.resourceUnavailable = true;
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || raw.id != fixture.candidateId)
        { result.unresolved.emplace_back("v4_file_input_candidate_unavailable"); return result; }
        const auto value = [&](std::string_view id) { return Value(axes, candidate, id); };
        if (!fixture.inputPrepared || !fixture.created || !fixture.fileIdentityObserved ||
            (value("V4.source.kind") != "file_only" && value("V4.source.kind") != "file_and_content") ||
            value("V4.source.file_count") != "one" || value("V4.file.member0_kind") != "file" ||
            value("V4.file.member0_identity") != "unique" || fixture.file.parent_path() != fixture.root)
        { result.unresolved.emplace_back("v4_file_input_shape_unavailable"); return result; }
        ole_file_source_fixture::FileDropCallState callState;
        auto sourcePath = fixture.file; sourcePath.make_preferred();
        APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
        if (!source.HasConstructionShell() || source.ConstructionPaths().size() != 1 ||
            source.ConstructionPaths().front() != sourcePath || source.owner != GetCurrentThreadId() || source.wrongApartment ||
            FAILED(CoGetApartmentType(&apartment, &qualifier)) ||
            (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA) ||
            source.fileDropObservationFailed || source.formatProbeObservationFailed ||
            source.fileReads || source.dataReads || source.identityReads || source.starts || source.ends || !source.fileDropReads.empty() ||
            !source.formatProbes.empty() || !source.multiParentPaths.empty() ||
            !source.ReadFileDropCallState(callState) || callState.failed || callState.count || callState.depth)
        { result.unresolved.emplace_back("v4_file_source_initial_state_unavailable"); return result; }
        const auto sameId = [](const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
            return a.VolumeSerialNumber == b.VolumeSerialNumber &&
                std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) == 0 &&
                std::any_of(std::begin(a.FileId.Identifier), std::end(a.FileId.Identifier), [](BYTE x) { return x != 0; });
        };
        struct Handle { HANDLE value = INVALID_HANDLE_VALUE; ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
        FILE_ID_INFO heldRoot{}, namedRoot{}, actualFile{}, afterFile{};
        FILE_ATTRIBUTE_TAG_INFO rootTag{}, fileTag{};
        if (!trustedRoot || trustedRoot == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(trustedRoot, FileIdInfo, &heldRoot, sizeof(heldRoot)) ||
            !sameId(heldRoot, trustedRootId) || !sameId(heldRoot, fixture.rootId))
        { result.unresolved.emplace_back("v4_file_input_held_root_unavailable"); return result; }
        Handle root{CreateFileW(fixture.root.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        Handle file{CreateFileW(fixture.file.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        LARGE_INTEGER size{};
        if (root.value == INVALID_HANDLE_VALUE || file.value == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(root.value, FileIdInfo, &namedRoot, sizeof(namedRoot)) || !sameId(namedRoot, heldRoot) ||
            !GetFileInformationByHandleEx(root.value, FileAttributeTagInfo, &rootTag, sizeof(rootTag)) ||
            !(rootTag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (rootTag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !GetFileInformationByHandleEx(file.value, FileIdInfo, &actualFile, sizeof(actualFile)) || !sameId(actualFile, fixture.fileId) ||
            !GetFileInformationByHandleEx(file.value, FileAttributeTagInfo, &fileTag, sizeof(fileTag)) ||
            (fileTag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !GetFileSizeEx(file.value, &size) || size.QuadPart < 0 ||
            static_cast<std::uint64_t>(size.QuadPart) != fixture.bytes.size())
        { result.unresolved.emplace_back("v4_file_input_identity_or_size_unavailable"); return result; }
        std::string actual;
        if (!ReadPayloadFixtureFileChecked(fixture.file, actual) || actual.size() != fixture.bytes.size() ||
            (!actual.empty() && std::memcmp(actual.data(), fixture.bytes.data(), actual.size()) != 0) ||
            !GetFileInformationByHandleEx(file.value, FileIdInfo, &afterFile, sizeof(afterFile)) || !sameId(afterFile, actualFile) ||
            !GetFileInformationByHandleEx(trustedRoot, FileIdInfo, &heldRoot, sizeof(heldRoot)) || !sameId(heldRoot, trustedRootId))
        { result.unresolved.emplace_back("v4_file_input_bytes_or_identity_changed"); return result; }
        Handle rootAfter{CreateFileW(fixture.root.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        FILE_ID_INFO namedRootAfter{};
        if (rootAfter.value == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(rootAfter.value, FileIdInfo, &namedRootAfter, sizeof(namedRootAfter)) ||
            !sameId(namedRootAfter, heldRoot))
        { result.unresolved.emplace_back("v4_file_input_root_name_changed"); return result; }
        constexpr char hex[] = "0123456789abcdef";
        const auto appendByte = [&](std::string& out, unsigned byte) { out.push_back(hex[byte >> 4]); out.push_back(hex[byte & 15]); };
        std::string path = "utf16le_hex_";
        for (const auto ch : fixture.file.wstring())
        { const auto unit = static_cast<unsigned short>(ch); appendByte(path, unit & 255); appendByte(path, unit >> 8); }
        std::string bytes = "hex_";
        for (const unsigned char byte : actual) appendByte(bytes, byte);
        if (value("V4.file_path.index_0.utf16le") != path || value("V4.file_content.index_0.bytes") != bytes)
        { result.unresolved.emplace_back("v4_file_input_candidate_path_or_bytes_mismatch"); return result; }
        result.candidateId = raw.id; result.root = fixture.root; result.file = fixture.file;
        result.rootId = namedRoot; result.fileId = actualFile;
        result.bytes.resize(actual.size());
        if (!actual.empty()) std::memcpy(result.bytes.data(), actual.data(), actual.size());
        result.ownerThread = source.owner; result.sourceAddress = reinterpret_cast<std::uintptr_t>(&source);
        result.sourceLifetime = source.LifetimeIdentity(); result.captured = true;
        result.unresolved.emplace_back("v4_file_shell_formats_and_production_read_unobserved");
        result.unresolved.emplace_back("v4_file_input_snapshot_non_atomic");
        result.unresolved.emplace_back("v4_file_other_axes_host_completion_and_business_unobserved");
    }
    catch (const std::bad_alloc&) { result.captured = false; result.resourceUnavailable = true; }
    catch (const std::length_error&) { result.captured = false; result.resourceUnavailable = true; }
    catch (...) { result.captured = false; }
    return result;
}

struct V4IndependentFileInputObservation
{
    struct Entry { std::filesystem::path path; FILE_ID_INFO id{}; std::vector<std::byte> bytes; };
    bool captured = false, resourceUnavailable = false;
    std::string candidateId;
    std::filesystem::path root;
    FILE_ID_INFO rootId{};
    DWORD ownerThread = 0;
    std::uintptr_t sourceAddress = 0;
    std::weak_ptr<const ole_file_source_fixture::Source::LifetimeToken> sourceLifetime;
    std::vector<Entry> files;
    // Only axes compared against this actual prepared source and its held
    // files. This list does not prove rendered formats or unrelated axes.
    std::vector<std::pair<std::string, std::string>> exactSourceAxes;
    std::vector<std::string> unresolved;
};

// Observe existing shared leases; do not create, mutate or render any source
// representation. The source's construction order is not a production read.
inline V4IndependentFileInputObservation InspectV4IndependentFileInputs(const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const V4IndependentFilePayload& payload,
    ole_file_source_fixture::Source& source, bool allowMixed = false)
{
    V4IndependentFileInputObservation result;
    try
    {
        if (verified.Status() != V4CoordinateStatus::coordinate_ready || identity.Status() != V4FamilyStatus::schema_only ||
            verified.TaskBActive() || verified.FamilyBinding() != identity.FamilyBinding() ||
            verified.Axes().size() != identity.Axes().size() || payload.status != V4IndependentFileStatus::input_prepared ||
            payload.files.empty())
        { result.unresolved.emplace_back("v4_multi_input_context_or_payload_unavailable"); return result; }
        const auto& axes = verified.Axes(); const auto& candidate = verified.Coordinate();
        for (std::size_t i = 0; i < axes.size(); ++i)
        {
            const auto& a = axes[i]; const auto& b = identity.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.values != b.values || a.label != b.label)
            { result.unresolved.emplace_back("v4_multi_input_schema_mismatch"); return result; }
        }
        const auto raw = identity.RawCandidateIdentity(candidate);
        if (raw.resourceUnavailable) result.resourceUnavailable = true;
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || raw.id != payload.candidateId)
        { result.unresolved.emplace_back("v4_multi_input_candidate_unavailable"); return result; }
        const auto get = [&](std::string_view id) { return Value(axes, candidate, id); };
        const auto count = payload.files.size() == 1 ? std::string("one") : payload.files.size() == 2 ? std::string("two") :
            std::string("count_") + std::to_string(payload.files.size());
        if ((get("V4.source.kind") != "file_only" && !(allowMixed && get("V4.source.kind") == "file_and_content")) ||
            get("V4.source.file_count") != count ||
            get("T04.capability") != "enabled" || !source.asynchronous)
        { result.unresolved.emplace_back("v4_multi_input_count_or_kind_unavailable"); return result; }
        APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
        ole_file_source_fixture::FileDropCallState callState;
        if (!source.HasConstructionShell() || source.owner != GetCurrentThreadId() || source.wrongApartment ||
            FAILED(CoGetApartmentType(&apartment, &qualifier)) || (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA) ||
            source.fileDropObservationFailed || source.formatProbeObservationFailed || source.fileReads || source.dataReads ||
            source.identityReads || source.starts || source.ends || !source.fileDropReads.empty() ||
            (!source.formatProbes.empty() && (!source.RestrictedPreflightSucceeded() || source.formatProbes.size() != 1)) ||
            source.restrictedGetObservationFailed || !source.restrictedGets.empty() ||
            !source.multiParentPaths.empty() || source.ConstructionPaths().size() != payload.files.size() ||
            !source.ReadFileDropCallState(callState) || callState.failed || callState.count || callState.depth)
        { result.unresolved.emplace_back("v4_multi_input_source_initial_state_unavailable"); return result; }
        const auto sameId = [](const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
            return a.VolumeSerialNumber == b.VolumeSerialNumber &&
                std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) == 0 &&
                std::any_of(std::begin(a.FileId.Identifier), std::end(a.FileId.Identifier), [](BYTE x) { return x != 0; });
        };
        struct Handle { HANDLE value = INVALID_HANDLE_VALUE; ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
        FILE_ID_INFO rootId{}; FILE_ATTRIBUTE_TAG_INFO tag{};
        if (!payload.rootHandle || payload.rootHandle == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(payload.rootHandle, FileIdInfo, &rootId, sizeof(rootId)) || !sameId(rootId, payload.rootId))
        { result.unresolved.emplace_back("v4_multi_input_root_lease_unavailable"); return result; }
        const auto rootNamed = [&] {
            Handle root{CreateFileW(payload.root.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
            FILE_ID_INFO id{}; FILE_ATTRIBUTE_TAG_INFO attributes{};
            return root.value != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(root.value, FileIdInfo, &id, sizeof(id)) &&
                sameId(id, rootId) && GetFileInformationByHandleEx(root.value, FileAttributeTagInfo, &attributes, sizeof(attributes)) &&
                (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
        };
        if (!rootNamed()) { result.unresolved.emplace_back("v4_multi_input_root_name_unavailable"); return result; }
        constexpr char digits[] = "0123456789abcdef";
        const auto appendByte = [&](std::string& out, unsigned byte) { out.push_back(digits[byte >> 4]); out.push_back(digits[byte & 15]); };
        std::set<std::string> representedAxes;
        result.files.reserve(payload.files.size());
        for (std::size_t i = 0; i < payload.files.size(); ++i)
        {
            const auto& file = payload.files[i]; const auto index = std::to_string(i);
            const auto pathAxis = "V4.file_path.index_" + index + ".utf16le";
            const auto bytesAxis = "V4.file_content.index_" + index + ".bytes";
            representedAxes.insert(pathAxis); representedAxes.insert(bytesAxis);
            auto sourcePath = file.path; sourcePath.make_preferred();
            if (!file.created || !file.identityObserved || !file.bytesObserved || !file.copyObservationHandle ||
                !file.handle || file.handle == INVALID_HANDLE_VALUE ||
                file.path.parent_path() != payload.root || source.ConstructionPaths()[i] != sourcePath ||
                get("V4.file.member" + index + "_kind") != "file" || get("V4.file.member" + index + "_identity") != "unique")
            { result.unresolved.emplace_back("v4_multi_input_file_or_source_order_unavailable"); return result; }
            FILE_ID_INFO id{}, afterId{}, namedId{}; FILE_BASIC_INFO before{}, after{}; LARGE_INTEGER size{}, afterSize{}, zero{};
            if (!GetFileInformationByHandleEx(file.handle, FileIdInfo, &id, sizeof(id)) || !sameId(id, file.identity) ||
                !GetFileInformationByHandleEx(file.handle, FileAttributeTagInfo, &tag, sizeof(tag)) ||
                (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                !GetFileInformationByHandleEx(file.handle, FileBasicInfo, &before, sizeof(before)) ||
                !GetFileSizeEx(file.handle, &size) || size.QuadPart < 0 ||
                static_cast<std::uint64_t>(size.QuadPart) != file.bytes.size() || !SetFilePointerEx(file.handle, zero, nullptr, FILE_BEGIN))
            { result.unresolved.emplace_back("v4_multi_input_file_identity_or_size_unavailable"); return result; }
            V4IndependentFileInputObservation::Entry observed; observed.path = file.path; observed.id = id;
            observed.bytes.resize(file.bytes.size());
            std::size_t offset = 0;
            while (offset < observed.bytes.size())
            {
                const auto amount = static_cast<DWORD>((std::min)(observed.bytes.size() - offset, std::size_t{1024 * 1024}));
                DWORD read = 0;
                if (!ReadFile(file.handle, observed.bytes.data() + offset, amount, &read, nullptr) || !read || read > amount)
                { result.unresolved.emplace_back("v4_multi_input_file_read_unavailable"); return result; }
                offset += read;
            }
            Handle named{CreateFileW(file.path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
            if (named.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(named.value, FileIdInfo, &namedId, sizeof(namedId)) ||
                !sameId(namedId, id) || !GetFileInformationByHandleEx(named.value, FileAttributeTagInfo, &tag, sizeof(tag)) ||
                (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                !GetFileInformationByHandleEx(file.handle, FileIdInfo, &afterId, sizeof(afterId)) || !sameId(afterId, id) ||
                !GetFileInformationByHandleEx(file.handle, FileBasicInfo, &after, sizeof(after)) ||
                before.LastWriteTime.QuadPart != after.LastWriteTime.QuadPart ||
                !GetFileSizeEx(file.handle, &afterSize) || afterSize.QuadPart != size.QuadPart)
            { result.unresolved.emplace_back("v4_multi_input_file_changed_or_name_unavailable"); return result; }
            std::string encodedPath = "utf16le_hex_", encodedBytes = "hex_";
            for (const auto ch : file.path.wstring())
            { const auto unit = static_cast<unsigned short>(ch); appendByte(encodedPath, unit & 255); appendByte(encodedPath, unit >> 8); }
            for (const auto byte : observed.bytes) appendByte(encodedBytes, std::to_integer<unsigned>(byte));
            if (get(pathAxis) != encodedPath || get(bytesAxis) != encodedBytes || observed.bytes != file.bytes)
            { result.unresolved.emplace_back("v4_multi_input_candidate_path_or_bytes_mismatch"); return result; }
            for (const auto& previous : result.files)
                if (sameId(previous.id, id)) { result.unresolved.emplace_back("v4_multi_input_file_alias_unavailable"); return result; }
            result.files.push_back(std::move(observed));
        }
        for (const auto& axis : axes)
            if ((axis.id.starts_with("V4.file_path.index_") || axis.id.starts_with("V4.file_content.index_")) &&
                !representedAxes.contains(axis.id))
            { result.unresolved.emplace_back("v4_multi_input_extra_file_axis_unavailable"); return result; }
        if (source.RestrictedFileDrop())
        {
            if (!source.RestrictedPreflightSucceeded() || payload.files.size() != 1 || allowMixed ||
                get("T02.file_paths") != "yes" || get("T02.shell_object") != "no")
            { result.unresolved.emplace_back("v4_restricted_file_source_or_probe_unavailable"); return result; }
            for (const auto& axis : axes)
            {
                if (!axis.id.starts_with("V4.format.")) continue;
                std::string_view expected;
                if (axis.id.ends_with(".offer")) expected = axis.id == "V4.format.cf_hdrop.offer" ? "yes" : "no";
                else if (axis.id.find(".medium.") != std::string::npos)
                    expected = axis.id == "V4.format.cf_hdrop.medium.hglobal" ? "yes" : "no";
                else if (axis.id.ends_with(".unlisted_payloads")) expected = "reject_unlisted";
                else { result.unresolved.emplace_back("v4_restricted_file_format_axis_unavailable"); return result; }
                if (get(axis.id) != expected)
                { result.unresolved.emplace_back("v4_restricted_file_format_axis_mismatch:" + axis.id); return result; }
            }
        }
        FILE_ID_INFO finalRoot{};
        if (!rootNamed() || !GetFileInformationByHandleEx(payload.rootHandle, FileIdInfo, &finalRoot, sizeof(finalRoot)) ||
            !sameId(finalRoot, rootId)) { result.unresolved.emplace_back("v4_multi_input_root_changed"); return result; }
        for (const auto* axis : {"V4.source.kind", "V4.source.file_count", "T04.capability"})
            result.exactSourceAxes.emplace_back(axis, get(axis));
        if (source.RestrictedFileDrop())
        {
            result.exactSourceAxes.emplace_back("T02.file_paths", get("T02.file_paths"));
            result.exactSourceAxes.emplace_back("T02.shell_object", get("T02.shell_object"));
            for (const auto& axis : axes) if (axis.id.starts_with("V4.format."))
                result.exactSourceAxes.emplace_back(axis.id, get(axis.id));
        }
        for (std::size_t i = 0; i < payload.files.size(); ++i)
        {
            const auto index = std::to_string(i);
            for (const auto& axis : {"V4.file.member" + index + "_kind", "V4.file.member" + index + "_identity",
                    "V4.file_path.index_" + index + ".utf16le", "V4.file_content.index_" + index + ".bytes"})
                result.exactSourceAxes.emplace_back(axis, get(axis));
        }
        result.root = payload.root; result.rootId = rootId; result.candidateId = raw.id;
        result.ownerThread = source.owner; result.sourceAddress = reinterpret_cast<std::uintptr_t>(&source);
        result.sourceLifetime = source.LifetimeIdentity(); result.captured = true;
        result.unresolved.emplace_back("v4_multi_input_snapshot_non_atomic_and_mutable_payload_unresolved");
        result.unresolved.emplace_back("v4_multi_source_actual_render_order_host_completion_and_business_unobserved");
        if (get("V4.source.kind") == "file_and_content")
            result.unresolved.emplace_back("v4_mixed_combined_provider_content_and_product_selection_unobserved");
    }
    catch (const std::bad_alloc&) { result.captured = false; result.resourceUnavailable = true; }
    catch (const std::length_error&) { result.captured = false; result.resourceUnavailable = true; }
    catch (...) { result.captured = false; }
    return result;
}

// Read-only partial records for the same validated v2 source and candidate.
// Passive task/work tokens provide observations, but reentrant attribution,
// host epoch, completion and independent business outcomes are not established.
// This is not an execution receipt and cannot mark any hypothesis as matched.
struct V2SourceAndPassiveEvidence
{
    FileSourceEvidence source;
    std::optional<PassiveTransferEvidence> passive;
    std::vector<std::string> obligations;
};
inline V2SourceAndPassiveEvidence InspectV2SourceAndPassive(const std::vector<Axis>& axes,
    const Candidate& candidate, const PayloadFixtureResult& fixture, const FileSourceInitialObservation& initial,
    ole_file_source_fixture::Source& source)
{
    V2SourceAndPassiveEvidence result;
    result.source = InspectSingleFileSourceEvidence(axes, candidate, fixture, initial, source,
        SelectionSchemaVersion::source_members_v2);
    for (const auto& reason : result.source.obligations)
        result.obligations.push_back("v2_source_partial:" + reason);
    if (result.source.axes.empty())
    { result.obligations.emplace_back("v2_passive_source_identity_or_snapshot_unavailable"); return result; }
    result.passive = InspectPassiveTransfer(static_cast<IDataObject*>(&source));
    result.obligations.insert(result.obligations.end(), result.passive->unresolved.begin(), result.passive->unresolved.end());
    result.obligations.emplace_back("v2_passive_records_not_bound_to_host_epoch_or_ole_execution");
    result.obligations.emplace_back("v2_completion_and_independent_business_result_unavailable");
    return result;
}

struct V3SourceAndPassiveEvidence
{
    FileSourceEvidence source;
    std::optional<PassiveTransferEvidence> passive;
    std::vector<std::string> obligations;
};
inline V3SourceAndPassiveEvidence InspectV3SourceAndPassive(const V3FamilyValidatedContext& context,
    const PayloadFixtureResult& fixture, const FileSourceInitialObservation& initial, ole_file_source_fixture::Source& source)
{
    V3SourceAndPassiveEvidence result;
    result.source = InspectV3SingleFileSourceEvidence(context, fixture, initial, source);
    for (const auto& reason : result.source.obligations)
        result.obligations.push_back("v3_source_partial:" + reason);
    if (result.source.axes.empty())
    { result.obligations.emplace_back("v3_passive_source_identity_or_snapshot_unavailable"); return result; }
    result.passive = InspectPassiveTransfer(static_cast<IDataObject*>(&source));
    result.obligations.insert(result.obligations.end(), result.passive->unresolved.begin(), result.passive->unresolved.end());
    result.obligations.emplace_back("v3_passive_records_not_bound_to_host_epoch_or_ole_execution");
    result.obligations.emplace_back("v3_completion_and_independent_business_result_unavailable");
    return result;
}

enum class V3LocalCountComparison { unavailable, no_exact_hypothesis, matches_local_count, hypothesis_differs_from_local_count };
// This immutable report is constructed only from the real provider and the
// stopped passive log. A local count comparison is not a same-run host
// attribution, completion proof, independent business assertion or PASS.
class V3LocalResolvedCountObservation
{
public:
    static V3LocalResolvedCountObservation Inspect(const V3FamilyValidatedContext& context,
        const PayloadFixtureResult& fixture, const FileSourceInitialObservation& initial,
        ole_file_source_fixture::Source& source)
    {
        V3LocalResolvedCountObservation result;
        try
        {
            const auto evidence = InspectV3SourceAndPassive(context, fixture, initial, source);
            result.unresolved_ = evidence.obligations;
            if (!evidence.passive) { result.unresolved_.emplace_back("v3_local_count_passive_log_unavailable"); return result; }
            const auto sourceMember = std::find_if(evidence.source.axes.begin(), evidence.source.axes.end(),
                [](const auto& axis) { return axis.id == "P01.primary"; });
            if (sourceMember == evidence.source.axes.end() || sourceMember->state != FileSourceEvidenceState::partial_observation)
            { result.unresolved_.emplace_back("v3_local_count_actual_source_member_unavailable"); return result; }
            for (const auto& reason : evidence.source.obligations)
                if (reason == "file_source_read_identity_unavailable" ||
                    reason == "file_source_observed_paths_differ_from_single_fixture_file" ||
                    reason == "file_source_snapshot_identity_or_content_unavailable")
                { result.unresolved_.emplace_back("v3_local_count_source_identity_ambiguous"); return result; }
            const auto& passive = *evidence.passive;
            for (const auto& reason : passive.unresolved)
                if (reason == "passive_content_paths_identity_or_count_unavailable" ||
                    reason == "passive_completion_work_identity_unavailable" ||
                    reason == "passive_work_completion_identity_ambiguous" ||
                    reason == "passive_work_completion_kind_ambiguous" ||
                    reason == "passive_worker_identity_unavailable" ||
                    reason == "passive_worker_instance_ambiguous" || reason == "passive_worker_thread_ambiguous" ||
                    reason == "passive_worker_kind_ambiguous" || reason == "passive_completion_window_thread_ambiguous" ||
                    reason == "passive_worker_lifecycle_ambiguous" || reason == "passive_worker_execution_incomplete_or_com_unavailable" ||
                    reason == "passive_worker_stop_cancelled_not_executed" || reason == "passive_queue_or_post_failure_unresolved" ||
                    reason == "passive_completion_kind_invalid" || reason == "passive_nested_scope_unresolved" ||
                    reason == "passive_finished_closure_or_context_ambiguous" ||
                    reason == "passive_content_read_to_commit_context_unavailable")
                { result.unresolved_.emplace_back("v3_local_count_correlation_log_ambiguous"); return result; }
            const auto worker = std::find_if(passive.workers.begin(), passive.workers.end(),
                [&](const auto& item) { return item.work == passive.contentReadWork; });
            if (!passive.task || !passive.contentObject || !passive.contentReadWork ||
                passive.finishedDuplicates || passive.finishedContextMismatch ||
                passive.contentReadRecords != 1 || passive.contentCommitRecords != 1 ||
                !passive.contentReadPathCount || !passive.contentCommitPathCount ||
                *passive.contentReadPathCount < 0 || *passive.contentReadPathCount != *passive.contentCommitPathCount ||
                worker == passive.workers.end() || worker->kind != 1 || worker->queueEntries != 1 ||
                worker->queueAccepted != 1 || worker->queueFailed || worker->dequeued != 1 || worker->entered != 1 ||
                worker->returned != 1 || worker->finished != 1 || worker->cancelled || !worker->executeResult ||
                *worker->executeResult != 1 || worker->callbacks != 1 || !worker->callbackResult ||
                *worker->callbackResult != 1 || worker->posts != 1 || worker->postFailed ||
                worker->messageEntered != 1 || worker->messageReturned != 1 ||
                !passive.contentReadThread || worker->workerThread != passive.contentReadThread ||
                !passive.contentCommitThread || worker->messageThread != passive.contentCommitThread)
            { result.unresolved_.emplace_back("v3_local_count_read_commit_work_correlation_unavailable"); return result; }
            result.observedCount_ = *passive.contentReadPathCount;
            result.task_ = passive.task;
            result.work_ = passive.contentReadWork;
            const auto& axes = context.Axes();
            const auto& candidate = context.Coordinate();
            const auto hypothesis = Value(axes, candidate, "P02.resolved_count");
            std::string_view expected;
            if (hypothesis == "zero") expected = "0";
            else if (hypothesis == "one") expected = "1";
            else if (hypothesis == "two") expected = "2";
            else if (hypothesis == "many")
            {
                const auto count = Value(axes, candidate, "P02.many_count");
                if (count.starts_with("count_")) expected = count.substr(6);
            }
            if (expected.empty()) result.comparison_ = V3LocalCountComparison::no_exact_hypothesis;
            else result.comparison_ = expected == std::to_string(*result.observedCount_)
                ? V3LocalCountComparison::matches_local_count : V3LocalCountComparison::hypothesis_differs_from_local_count;
            result.unresolved_.emplace_back("v3_local_count_host_epoch_and_reentrant_attribution_unavailable");
            result.unresolved_.emplace_back("v3_local_count_is_not_production_completion_or_business_result");
        }
        catch (const std::bad_alloc&) { result.comparison_ = V3LocalCountComparison::unavailable; }
        catch (const std::exception& error)
        { result.comparison_ = V3LocalCountComparison::unavailable; result.unresolved_.push_back(std::string("v3_local_count_observation_unavailable:") + error.what()); }
        catch (...) { result.comparison_ = V3LocalCountComparison::unavailable; result.unresolved_.emplace_back("v3_local_count_observation_unavailable"); }
        return result;
    }
    std::optional<std::int64_t> ObservedCount() const noexcept { return observedCount_; }
    std::uint64_t Task() const noexcept { return task_; }
    std::uint64_t Work() const noexcept { return work_; }
    V3LocalCountComparison Comparison() const noexcept { return comparison_; }
    const std::vector<std::string>& Unresolved() const noexcept { return unresolved_; }
private:
    V3LocalResolvedCountObservation() = default;
    std::optional<std::int64_t> observedCount_;
    std::uint64_t task_ = 0, work_ = 0;
    V3LocalCountComparison comparison_ = V3LocalCountComparison::unavailable;
    std::vector<std::string> unresolved_;
};

struct ContentSourceBinding
{
    Microsoft::WRL::ComPtr<ole_content_source_fixture::Source> source;
    std::vector<std::string> obligations;
};

// Bind the offered formats to the exact candidate before the provider is
// constructed. This synchronous provider does not implement Shell-object /
// CF_HDROP combinations, async capability or URL destination behavior.
// A constructed source is fixture input, never proof of production selection,
// stream consumption, persistence or business success.
class V4UnicodeSourceBinding;
// Created only by the owned fixture on its live STA. The fixture retains this
// owner and invalidates it before teardown/CoUninitialize; borrowed bindings
// never extend COM ownership beyond that boundary.
class V4UnicodeSourceOwner final
{
    friend struct ::NativeDragIntegrationAccess;
    friend class V4UnicodeSourceBinding;
    friend class V4GraphExecutor;
    friend V4UnicodeSourceBinding PrepareV4UnicodeSource(const V4FamilyValidatedContext&,
        const event_graph::V4FamilyIdentityContext&, const std::shared_ptr<V4UnicodeSourceOwner>&);
public:
    V4UnicodeSourceOwner(const V4UnicodeSourceOwner&) = delete;
    V4UnicodeSourceOwner& operator=(const V4UnicodeSourceOwner&) = delete;
    ~V4UnicodeSourceOwner() { (void)source_.Detach(); } // missed invalidation retains, never wrong-thread Release
private:
    V4UnicodeSourceOwner() noexcept : ownerThread_(GetCurrentThreadId()) {}
    bool Live() const noexcept
    {
        if (!active_.load(std::memory_order_acquire) || GetCurrentThreadId() != ownerThread_) return false;
        APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
        return SUCCEEDED(CoGetApartmentType(&apartment, &qualifier)) &&
            (apartment == APTTYPE_STA || apartment == APTTYPE_MAINSTA);
    }
    bool Attach(Microsoft::WRL::ComPtr<ole_content_source_fixture::Source>&& source) noexcept
    {
        if (!Live() || source_ || !source || source->ownerThread != ownerThread_) return false;
        source_ = std::move(source); return true;
    }
    bool CanInvalidate() const noexcept
    { return GetCurrentThreadId() == ownerThread_ && !executing_.load(std::memory_order_acquire); }
    bool TryBeginExecution() const noexcept
    {
        if (!Live() || !source_) return false;
        bool expected = false;
        return executing_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }
    void EndExecution() const noexcept { executing_.store(false, std::memory_order_release); }
    void Invalidate() noexcept
    {
        const DWORD saved = GetLastError();
        active_.store(false, std::memory_order_release);
        if (GetCurrentThreadId() == ownerThread_) source_.Reset();
        SetLastError(saved);
    }
    DWORD ownerThread_ = 0;
    std::atomic_bool active_{true};
    mutable std::atomic_bool executing_{false};
    Microsoft::WRL::ComPtr<ole_content_source_fixture::Source> source_;
};

// Restricted V4 fixture input only. The binding does not prove host capability
// queries, selected format, completion, or business outcome.
class V4UnicodeSourceBinding
{
    friend class V4GraphExecutor;
public:
    V4UnicodeSourceBinding(V4UnicodeSourceBinding&&) = default;
    V4UnicodeSourceBinding& operator=(V4UnicodeSourceBinding&&) = delete;
    V4UnicodeSourceBinding(const V4UnicodeSourceBinding&) = delete;
    V4UnicodeSourceBinding& operator=(const V4UnicodeSourceBinding&) = delete;
    ole_content_source_fixture::Source* SourceObject() const noexcept
    {
        const auto locked = owner.lock();
        return locked && locked->Live() && locked->source_.Get() == sourceIdentity ? sourceIdentity : nullptr;
    }
    const std::string& CandidateId() const noexcept { return rawCandidateId; }
    const std::vector<std::byte>& ExactBytes() const noexcept { return exactBytes; }
    HRESULT ConfiguredGetFailure() const noexcept { return configuredGetFailure; }
    bool EntryBound() const noexcept { return entryBound; }
    bool ResourceUnavailable() const noexcept { return resourceUnavailable; }
    const std::vector<std::string>& Unresolved() const noexcept { return unresolved; }
private:
    friend V4UnicodeSourceBinding PrepareV4UnicodeSource(const V4FamilyValidatedContext&,
        const event_graph::V4FamilyIdentityContext&, const std::shared_ptr<V4UnicodeSourceOwner>&);
    V4UnicodeSourceBinding() = default;
    std::weak_ptr<V4UnicodeSourceOwner> owner;
    ole_content_source_fixture::Source* sourceIdentity = nullptr;
    std::string rawCandidateId;
    std::vector<std::byte> exactBytes;
    HRESULT configuredGetFailure = S_OK;
    bool entryBound = false;
    bool resourceUnavailable = false;
    std::vector<std::string> unresolved;
};

inline V4UnicodeSourceBinding PrepareV4UnicodeSource(const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const std::shared_ptr<V4UnicodeSourceOwner>& owner)
{
    V4UnicodeSourceBinding result;
    try
    {
        if (!owner || !owner->Live() || owner->source_ || verified.Status() != V4CoordinateStatus::coordinate_ready ||
            identity.Status() != V4FamilyStatus::schema_only || verified.TaskBActive() ||
            verified.FamilyBinding() != identity.FamilyBinding() || verified.Axes().size() != identity.Axes().size())
        { result.unresolved.emplace_back("v4_unicode_authoritative_context_unavailable"); return result; }
        const auto& axes = verified.Axes(); const auto& candidate = verified.Coordinate();
        for (std::size_t i = 0; i < axes.size(); ++i)
        {
            const auto& a = axes[i]; const auto& b = identity.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.values != b.values || a.label != b.label)
            { result.unresolved.emplace_back("v4_unicode_schema_mismatch"); return result; }
        }
        const auto raw = identity.RawCandidateIdentity(candidate);
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty())
        { result.unresolved.emplace_back("v4_unicode_candidate_identity_unavailable"); return result; }
        result.rawCandidateId = raw.id;
        const auto value = [&](std::string_view id) { return Value(axes, candidate, id); };
        if (value("X03.task_count") != "one" || value("V4.source.kind") != "content_only" ||
            value("V4.source.file_count") != "na" ||
            (value("V4.source.virtual_count") != "na" && value("V4.source.virtual_count") != "zero") ||
            value("T01") != "external_ingress" || value("T02.text") != "yes" ||
            value("T04.capability") != "absent" || value("T04.availability") != "immediate")
        { result.unresolved.emplace_back("v4_unicode_source_kind_or_protocol_unimplemented"); return result; }
        for (const auto id : {"T02.file_paths", "T02.shell_object", "T02.virtual_files", "T02.image", "T02.url", "T02.invalid_format"})
            if (value(id) != "no") { result.unresolved.push_back(std::string("v4_unicode_other_format_unimplemented:") + id); return result; }
        constexpr std::string_view payloadId = "V4.payload.text_unicode.hglobal.default.bytes";
        const auto payload = value(payloadId);
        if (!payload.starts_with("hex_") || payload.size() < 8 || (payload.size() - 4) % 4 != 0 ||
            payload.substr(payload.size() - 4) != "0000")
        { result.unresolved.emplace_back("v4_unicode_utf16le_bytes_unimplemented"); return result; }
        const auto hexDigit = [](char c) -> int {
            return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        };
        const auto hex = payload.substr(4);
        std::vector<std::byte> expected; expected.reserve(hex.size() / 2);
        for (std::size_t i = 0; i < hex.size(); i += 2)
        {
            const int high = hexDigit(hex[i]), low = hexDigit(hex[i + 1]);
            if (high < 0 || low < 0) { result.unresolved.emplace_back("v4_unicode_hex_invalid"); return result; }
            expected.push_back(static_cast<std::byte>((high << 4) | low));
        }
        HRESULT getFailure = S_OK;
        bool payloadSeen = false;
        for (std::size_t i = 0; i < axes.size(); ++i)
        {
            const std::string_view id = axes[i].id;
            if (id.starts_with("task_b.")) continue;
            const auto& selected = candidate.values[i];
            if (id.starts_with("V4.format.") && id.ends_with(".offer"))
            {
                const bool text = id == "V4.format.text_unicode.offer";
                if (selected != (text ? "yes" : "no"))
                { result.unresolved.push_back("v4_unicode_offer_unimplemented:" + std::string(id)); return result; }
            }
            else if (id.starts_with("V4.format.") && id.find(".medium.") != std::string_view::npos)
            {
                const bool textGlobal = id == "V4.format.text_unicode.medium.hglobal";
                if (selected != (textGlobal ? "yes" : "no"))
                { result.unresolved.push_back("v4_unicode_medium_unimplemented:" + std::string(id)); return result; }
            }
            else if (id.starts_with("V4.payload.") && id.ends_with(".bytes"))
            {
                if (id == payloadId) payloadSeen = true;
                else if (selected != "na")
                { result.unresolved.push_back("v4_unicode_additional_payload_unimplemented:" + std::string(id)); return result; }
            }
            else if ((id.starts_with("V4.file_path.") || id.starts_with("V4.virtual_name.")) &&
                     id.ends_with(".utf16le") && selected != "na")
            { result.unresolved.push_back("v4_unicode_file_or_virtual_input_unimplemented:" + std::string(id)); return result; }
            else if (id.starts_with("V4.request.") && id.ends_with(".hresult"))
            {
                if (selected == "na") continue;
                const bool query = id == "V4.request.text_unicode.hglobal.default.query.hresult";
                const bool get = id == "V4.request.text_unicode.hglobal.default.get.hresult";
                if ((!query && !get) || !selected.starts_with("hresult_") || selected.size() != 16)
                { result.unresolved.push_back("v4_unicode_request_unimplemented:" + std::string(id)); return result; }
                std::uint32_t bits = 0;
                for (const char c : std::string_view(selected).substr(8))
                {
                    const int digit = hexDigit(c);
                    if (digit < 0) { result.unresolved.emplace_back("v4_unicode_request_hresult_invalid"); return result; }
                    bits = (bits << 4) | static_cast<std::uint32_t>(digit);
                }
                const HRESULT requested = static_cast<HRESULT>(bits);
                if ((query && requested != S_OK) || (get && requested != S_OK && SUCCEEDED(requested)))
                { result.unresolved.push_back("v4_unicode_request_result_unimplemented:" + std::string(id)); return result; }
                if (get) getFailure = requested;
            }
        }
        if (!payloadSeen) { result.unresolved.emplace_back("v4_unicode_payload_missing"); return result; }
        APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
        if (FAILED(CoGetApartmentType(&apartment, &qualifier)) ||
            (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA))
        { result.unresolved.emplace_back("v4_unicode_live_sta_unavailable"); return result; }
        static_assert(sizeof(wchar_t) == 2, "Windows UTF-16 code units are required");
        std::wstring text; text.reserve(expected.size() / 2 - 1);
        for (std::size_t i = 0; i + 2 < expected.size(); i += 2)
        {
            const auto unit = std::to_integer<unsigned>(expected[i]) | (std::to_integer<unsigned>(expected[i + 1]) << 8);
            text.push_back(static_cast<wchar_t>(unit));
        }
        ole_content_source_fixture::Inputs inputs;
        inputs.text = std::move(text);
        if (FAILED(getFailure)) inputs.getDataFaults.push_back({CF_UNICODETEXT, -1, getFailure});
        auto source = ole_content_source_fixture::Make(inputs);
        if (!source || source->ownerThread != GetCurrentThreadId() || source->wrongApartment.load() ||
            source->observationFailed.load() || !source->calls.empty() || source->entries.size() != 1)
        { result.unresolved.emplace_back("v4_unicode_source_initial_state_unavailable"); return result; }
        const auto& entry = source->entries.front();
        if (entry.format != CF_UNICODETEXT || entry.index != -1 || entry.tymed != TYMED_HGLOBAL ||
            entry.readFailure || entry.getDataFailure != getFailure || entry.bytes != expected)
        { result.unresolved.emplace_back("v4_unicode_entry_mismatch"); return result; }
        result.unresolved.emplace_back("v4_unicode_unlisted_representations_and_requests_unimplemented");
        result.unresolved.emplace_back("v4_unicode_host_reads_format_selection_completion_and_business_unobserved");
        result.unresolved.emplace_back("v4_unicode_live_sta_and_source_lifetime_required");
        result.exactBytes = std::move(expected); result.configuredGetFailure = getFailure;
        auto* sourceIdentity = source.Get();
        if (!owner->Attach(std::move(source)))
        { result.unresolved.emplace_back("v4_unicode_fixture_owner_unavailable"); return result; }
        result.owner = owner; result.sourceIdentity = sourceIdentity; result.entryBound = true;
    }
    catch (const std::bad_alloc&) { result.resourceUnavailable = true; }
    catch (const std::length_error&) { result.resourceUnavailable = true; }
    catch (const std::exception&) { result.unresolved.emplace_back("v4_unicode_fixture_unavailable"); }
    catch (...) { result.unresolved.emplace_back("v4_unicode_fixture_unavailable"); }
    return result;
}

// A point-in-time fixture observation, not evidence of a host request or a
// product result. In particular, matching one axis does not close the other
// source axes or make the selected graph executable.
struct V4UnicodeFixtureAxisObservation
{
    std::string axis;
    std::string selected;
    std::string observed;
    bool matches = false;
};
struct V4UnicodeFixtureInputObservation
{
    bool snapshotComplete = false;
    bool resourceUnavailable = false;
    DWORD ownerThread = 0;
    std::uintptr_t sourceIdentity = 0;
    CLIPFORMAT format = 0;
    LONG index = 0;
    DWORD tymed = 0;
    HRESULT configuredGetFailure = S_OK;
    bool configuredReadFailure = false;
    std::vector<std::byte> bytes;
    std::vector<V4UnicodeFixtureAxisObservation> axes;
    std::vector<std::string> unresolved;
};

inline V4UnicodeFixtureInputObservation InspectV4UnicodeFixtureInputs(
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const V4UnicodeSourceBinding& binding)
{
    V4UnicodeFixtureInputObservation result;
    try
    {
        if (verified.Status() != V4CoordinateStatus::coordinate_ready ||
            identity.Status() != V4FamilyStatus::schema_only || verified.TaskBActive() ||
            verified.FamilyBinding() != identity.FamilyBinding() ||
            verified.Axes().size() != identity.Axes().size() || !binding.EntryBound())
        { result.unresolved.emplace_back("v4_unicode_fixture_context_unavailable"); return result; }
        const auto& axes = verified.Axes();
        for (std::size_t i = 0; i < axes.size(); ++i)
        {
            const auto& a = axes[i]; const auto& b = identity.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.values != b.values || a.label != b.label)
            { result.unresolved.emplace_back("v4_unicode_fixture_schema_mismatch"); return result; }
        }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable) result.resourceUnavailable = true;
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || raw.id != binding.CandidateId())
        { result.unresolved.emplace_back("v4_unicode_fixture_candidate_unavailable"); return result; }
        auto* source = binding.SourceObject();
        if (!source || source->ownerThread != GetCurrentThreadId() || source->wrongApartment.load() ||
            source->observationFailed.load())
        { result.unresolved.emplace_back("v4_unicode_fixture_live_source_unavailable"); return result; }
        {
            std::lock_guard lock(source->observationMutex);
            if (!source->calls.empty())
            { result.unresolved.emplace_back("v4_unicode_fixture_initial_call_baseline_unavailable"); return result; }
        }
        if (source->entries.size() != 1)
        { result.unresolved.emplace_back("v4_unicode_fixture_entry_snapshot_unavailable"); return result; }
        const auto& entry = source->entries.front();
        result.ownerThread = source->ownerThread;
        result.sourceIdentity = reinterpret_cast<std::uintptr_t>(source);
        result.format = entry.format; result.index = entry.index; result.tymed = entry.tymed;
        result.configuredGetFailure = entry.getDataFailure; result.configuredReadFailure = entry.readFailure;
        result.bytes = entry.bytes;
        const auto add = [&](std::string_view id, std::string actual) {
            const std::string selected(Value(axes, verified.Coordinate(), id));
            result.axes.push_back({std::string(id), selected, std::move(actual), false});
            auto& observation = result.axes.back();
            observation.matches = observation.selected == observation.observed;
        };
        const bool unicode = entry.format == CF_UNICODETEXT && entry.index == -1;
        add("V4.format.text_unicode.offer", unicode ? "yes" : "no");
        for (const auto& [name, mask] : std::initializer_list<std::pair<std::string_view, DWORD>>{
                 {"hglobal", TYMED_HGLOBAL}, {"file", TYMED_FILE}, {"istream", TYMED_ISTREAM},
                 {"istorage", TYMED_ISTORAGE}, {"gdi", TYMED_GDI}, {"mfpict", TYMED_MFPICT},
                 {"enhmf", TYMED_ENHMF}})
            add(std::string("V4.format.text_unicode.medium.") + std::string(name),
                unicode && (entry.tymed & mask) ? "yes" : "no");
        constexpr char digits[] = "0123456789abcdef";
        std::string encoded = "hex_";
        for (const auto byte : result.bytes)
        {
            const auto value = std::to_integer<unsigned>(byte);
            encoded.push_back(digits[value >> 4]); encoded.push_back(digits[value & 15]);
        }
        if (unicode && entry.tymed == TYMED_HGLOBAL)
            add("V4.payload.text_unicode.hglobal.default.bytes", std::move(encoded));
        else result.unresolved.emplace_back("v4_unicode_fixture_payload_entry_unavailable");
        if (entry.readFailure || entry.getDataFailure != binding.ConfiguredGetFailure() ||
            result.bytes != binding.ExactBytes())
            result.unresolved.emplace_back("v4_unicode_fixture_initial_entry_changed");
        result.unresolved.emplace_back("v4_unicode_fixture_snapshot_non_atomic");
        result.unresolved.emplace_back("v4_unicode_other_axes_host_and_business_unobserved");
        result.snapshotComplete = true;
    }
    catch (const std::bad_alloc&) { result.resourceUnavailable = true; result.snapshotComplete = false; }
    catch (const std::length_error&) { result.resourceUnavailable = true; result.snapshotComplete = false; }
    catch (...) { result.snapshotComplete = false; }
    return result;
}

inline ContentSourceBinding PrepareContentSource(const event_graph::Context& context,
    const Candidate& input, const ole_content_source_fixture::Inputs& inputs)
{
    ContentSourceBinding result;
    result.obligations = event_graph::RevalidateContext(context);
    if (context.version != SelectionSchemaVersion::legacy_v1)
        result.obligations.emplace_back("content_v2_source_semantics_unimplemented");
    if (!result.obligations.empty()) return result;
    if (input.values.size() != context.axes.size())
    { result.obligations.emplace_back("content_candidate_size_invalid"); return result; }
    Candidate candidate = input;
    candidate.lookup = context.lookup;
    for (std::size_t i = 0; i < context.axes.size(); ++i)
    {
        const auto& axis = context.axes[i];
        if (axis.role == Role::Derived)
        {
            if (!candidate.values[i].empty()) result.obligations.push_back("content_derived_input_invalid:" + axis.id);
        }
        else if (std::find(axis.values.begin(), axis.values.end(), candidate.values[i]) == axis.values.end() ||
            !PrefixAllows(context.axes, candidate, axis.id, candidate.values[i]))
            result.obligations.push_back("content_candidate_value_invalid:" + axis.id);
    }
    if (!result.obligations.empty()) return result;
    if (const auto excluded = StructuralExclusion(context.axes, candidate); !excluded.empty())
    { result.obligations.push_back("content_structural_exclusion:" + std::string(excluded)); return result; }
    const auto v = [&](std::string_view id) { return Value(context.axes, candidate, id); };
    if (v("T01") != "external_ingress") result.obligations.emplace_back("content_nonexternal_source_unimplemented");
    if (v("T02.file_paths") != "no" || v("T02.shell_object") != "no")
        result.obligations.emplace_back("content_shell_object_or_file_path_combination_unimplemented");
    const auto offered = [&](std::string_view id, bool present) {
        const auto value = v(id);
        if ((value == "yes") != present || (value != "yes" && value != "no"))
            result.obligations.push_back("content_offer_binding_unimplemented:" + std::string(id));
    };
    offered("T02.image", inputs.dib.has_value());
    offered("T02.text", inputs.text.has_value());
    offered("T02.url", inputs.url.has_value());
    offered("T02.virtual_files", inputs.virtualFiles.has_value());
    if (v("T04.capability") != "absent" || v("T04.availability") != "immediate")
        result.obligations.emplace_back("content_async_or_delayed_provider_unimplemented");
    if (inputs.virtualFiles)
    {
        bool missing = false, unreadable = false;
        for (const auto& file : *inputs.virtualFiles)
        {
            missing |= file.content == ole_content_source_fixture::VirtualContent::missing;
            unreadable |= file.content == ole_content_source_fixture::VirtualContent::read_failure;
        }
        const auto completeness = v("T02.virtual_completeness");
        const bool bound = (completeness == "complete" && !missing && !unreadable) ||
            (completeness == "missing_entry" && missing && !unreadable) ||
            (completeness == "unreadable_stream" && unreadable && !missing);
        if (!bound) result.obligations.emplace_back("content_virtual_batch_completeness_unimplemented");
    }
    else if (v("T02.virtual_completeness") != "na")
        result.obligations.emplace_back("content_virtual_absence_binding_unimplemented");
    if (!result.obligations.empty()) return result;
    try { result.source = ole_content_source_fixture::Make(inputs); }
    catch (const std::exception& error)
    { result.obligations.push_back(std::string("content_fixture_construction_failed:") + error.what()); return result; }
    catch (...) { result.obligations.emplace_back("content_fixture_construction_failed:unknown_exception"); return result; }
    if (inputs.url) result.obligations.emplace_back("content_url_kind_destination_and_network_unimplemented");
    if (!inputs.getDataFaults.empty()) result.obligations.emplace_back("content_getdata_fault_candidate_mapping_unimplemented");
    if (v("T02.invalid_format") != "no") result.obligations.emplace_back("content_malformed_representation_binding_unimplemented");
    if (inputs.virtualFiles) result.obligations.emplace_back("content_stream_read_or_copyto_observation_completeness_unimplemented");
    result.obligations.emplace_back("content_input_semantics_and_actual_production_reads_unimplemented");
    result.obligations.emplace_back("content_independent_output_and_completion_oracle_unimplemented");
    return result;
}

// Controlled mixed-format fixture, not a prediction of which format production
// will choose. The malformed case uses a one-byte, truncated DIB header; the
// valid case is one 32-bit pixel. The caller independently supplies any output
// expectation after selecting a concrete business scenario.
struct DibTextFixtureInput
{
    bool malformedDib = false;
    std::uint8_t blue = 0, green = 0, red = 0, alpha = 255;
    std::wstring text;
};
struct DibTextSourceBinding
{
    ContentSourceBinding content;
    std::vector<std::string> candidateValues;
    std::vector<std::byte> offeredDib;
    std::vector<std::byte> offeredUnicodeText;
};
inline DibTextSourceBinding PrepareDibTextSource(const event_graph::Context& context,
    const Candidate& input, const DibTextFixtureInput& fixture)
{
    DibTextSourceBinding result;
    if (!event_graph::RevalidateContext(context).empty() || context.version != SelectionSchemaVersion::legacy_v1 ||
        input.values.size() != context.axes.size())
    { result.content.obligations.emplace_back("dib_text_candidate_schema_unavailable"); return result; }
    Candidate candidate = input;
    candidate.lookup = context.lookup;
    const auto value = [&](std::string_view id) { return Value(context.axes, candidate, id); };
    if (value("T02.image") != "yes" || value("T02.text") != "yes" ||
        value("T02.invalid_format") != (fixture.malformedDib ? "yes" : "no"))
    { result.content.obligations.emplace_back("dib_text_candidate_format_binding_unimplemented"); return result; }
    if (fixture.text.empty() || fixture.text.find(L'\0') != std::wstring::npos)
    { result.content.obligations.emplace_back("dib_text_fixture_text_unimplemented"); return result; }
    ole_content_source_fixture::Inputs offered;
    offered.dib = fixture.malformedDib ? std::vector<std::byte>{std::byte{0}} :
        ole_content_source_fixture::DibPixel(fixture.blue, fixture.green, fixture.red, fixture.alpha);
    offered.text = fixture.text;
    result.offeredDib = *offered.dib;
    result.offeredUnicodeText = ole_content_source_fixture::WideBytes(*offered.text);
    result.content = PrepareContentSource(context, candidate, offered);
    if (result.content.source) result.candidateValues = candidate.values;
    // Member identities, full graph, independent output and completed host
    // scheduling remain obligations even when these input bytes are bound.
    result.content.obligations.emplace_back("dib_text_member_identity_and_host_completion_unimplemented");
    return result;
}

struct DibTextReadEvidence
{
    std::size_t dibGets = 0, textGets = 0;
    std::vector<std::string> unresolved;
};
// Inspect the same IDataObject's actual GetData snapshots, without requesting
// any format itself. This proves returned fixture bytes only, not that the
// consumer decoded them, chose a winner, or committed any business result.
inline DibTextReadEvidence InspectDibTextReads(const event_graph::Context& context,
    const Candidate& candidate, DibTextSourceBinding& binding)
{
    DibTextReadEvidence result;
    auto* source = binding.content.source.Get();
    if (!source || binding.candidateValues.empty() || !event_graph::RevalidateContext(context).empty() ||
        context.version != SelectionSchemaVersion::legacy_v1 ||
        candidate.values != binding.candidateValues || candidate.values.size() != context.axes.size())
    { result.unresolved.emplace_back("dib_text_source_unavailable"); return result; }
    std::lock_guard lock(source->observationMutex);
    if (source->wrongApartment.load() || source->observationFailed.load())
    { result.unresolved.emplace_back("dib_text_source_observation_unavailable"); return result; }
    for (const auto& call : source->calls)
    {
        if (call.query || (call.format != CF_DIB && call.format != CF_UNICODETEXT)) continue;
        if (FAILED(call.result)) continue;
        if (call.thread != source->ownerThread || call.aspect != DVASPECT_CONTENT || call.index != -1 ||
            call.targetDevicePresent || !(call.requestedTymed & TYMED_HGLOBAL) ||
            call.returnedTymed != TYMED_HGLOBAL || !call.mediumObserved)
        { result.unresolved.emplace_back("dib_text_getdata_context_unavailable"); continue; }
        const auto& expected = call.format == CF_DIB ? binding.offeredDib : binding.offeredUnicodeText;
        if (call.hglobalBytes != expected)
        { result.unresolved.emplace_back("dib_text_getdata_bytes_mismatch"); continue; }
        if (call.format == CF_DIB) ++result.dibGets; else ++result.textGets;
    }
    result.unresolved.emplace_back("dib_text_read_stage_attribution_unestablished");
    result.unresolved.emplace_back("dib_text_decoding_selection_and_business_result_unobserved");
    return result;
}

enum class Kind { enter, over, drop, leave };

// Each input is bound to an exact step of the complete generated program.
// Coordinates are screen coordinates, not a fabricated source window. OLE
// DragLeave has no key state, effect or coordinate arguments.
struct Input
{
    std::size_t programIndex = 0;
    Kind kind = Kind::enter;
    std::string modelParameter;
    std::optional<DWORD> keyState;
    std::optional<DWORD> allowedEffect;
    std::optional<POINTL> screenPoint;
};

struct Observation
{
    std::size_t programIndex = 0;
    Kind kind = Kind::enter;
    HRESULT result = E_PENDING;
    std::optional<DWORD> keyState;
    std::optional<DWORD> offeredEffect;
    std::optional<DWORD> returnedEffect;
    std::optional<POINTL> screenPoint;
};

enum class MessageRoute { desktop_window_proc, control_window_proc };
struct CompletionData
{
    bool fileOperation = false;
    bool succeeded = false;
    // Numeric values copied before production owns/deletes the completion.
    // They are correlation observations, not retained pointers or success.
    std::uintptr_t identity = 0;
    std::uint64_t task = 0, work = 0;
};
struct MessageObservation
{
    std::uint64_t enteredSequence = 0;
    std::uint64_t returnedSequence = 0;
    std::uint64_t windowScope = 0;
    MessageRoute route = MessageRoute::desktop_window_proc;
    HWND window = nullptr;
    UINT message = 0;
    WPARAM wParam = 0;
    // Only value-only native input messages store LPARAM; completion messages
    // carry an owned pointer that becomes invalid after production dispatch.
    std::optional<LPARAM> inputLParam;
    // Captured at entry. Return must not sample a possibly ended or nested
    // device session again, and a rejected permit is still an entry.
    snowdesktop::private_pointer::DeliveryIdentity delivery;
    DWORD enteredThread = 0;
    DWORD returnedThread = 0;
    bool returned = false;
    bool malformedCompletion = false;
    std::optional<CompletionData> completion;
    LRESULT windowResult = 0;
};

// A transparent host-window observation hook. The owning fixture invokes
// Enter immediately on arrival, forwards the SAME message to the real
// DesktopApp WndProc/ControlWndProc, then invokes Return with its LRESULT.
// This object never calls, retains, takes ownership of, or synthesizes the
// Shell completion callback. A message record alone does not identify a task,
// prove business success, or authorize execution of an unresolved program.
class MessageTrace final
{
public:
    explicit MessageTrace(UINT completionMessage) : completionMessage_(completionMessage), ownerThread_(GetCurrentThreadId()) {}
    static constexpr std::size_t InvalidToken = (std::numeric_limits<std::size_t>::max)();
private:
    friend struct ::NativeDragIntegrationAccess;
    std::size_t Enter(MessageRoute route, HWND window, UINT message, WPARAM wp, LPARAM lp) noexcept
    {
        const DWORD savedError = GetLastError();
        std::size_t token = InvalidToken;
        if (GetCurrentThreadId() != ownerThread_)
        {
            lost_.store(true);
            SetLastError(savedError);
            return token;
        }
        try
        {
            MessageObservation item;
            item.enteredSequence = nextSequence_++;
            item.route = route; item.window = window; item.message = message; item.wParam = wp;
            item.enteredThread = GetCurrentThreadId();
            item.delivery = snowdesktop::private_pointer::CurrentDeliveryIdentity();
            switch (message)
            {
            case WM_MOUSEMOVE: case WM_MOUSELEAVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
            case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
            case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
            case WM_CHAR: case WM_SYSCHAR: case WM_SETFOCUS: case WM_KILLFOCUS:
            case WM_CAPTURECHANGED: case WM_CANCELMODE:
                item.inputLParam = lp;
                break;
            default: break;
            }
            if (message == completionMessage_)
            {
                const auto* completion = reinterpret_cast<const snowdesktop::ShellFileOperationUiCompletion*>(lp);
                if (!completion) item.malformedCompletion = true;
                else item.completion = CompletionData{completion->fileOperation, completion->succeeded,
                    reinterpret_cast<std::uintptr_t>(completion), completion->observationTask, completion->observationWork};
            }
            token = observations_.size();
            observations_.push_back(std::move(item));
        }
        catch (...) { lost_.store(true); token = InvalidToken; }
        SetLastError(savedError);
        return token;
    }

    // Called only by the owning fixture immediately before forwarding this
    // same message to the production WndProc. The scope value is captured at
    // entry and retained at return; nested messages receive distinct values.
    bool BindWindowScope(std::size_t token, std::uint64_t scope) noexcept
    {
        const DWORD savedError = GetLastError();
        const bool valid = GetCurrentThreadId() == ownerThread_ && scope && token < observations_.size() &&
            !observations_[token].returned && !observations_[token].windowScope;
        if (valid) observations_[token].windowScope = scope;
        else lost_.store(true);
        SetLastError(savedError);
        return valid;
    }

    void Return(std::size_t token, LRESULT value) noexcept
    {
        const DWORD savedError = GetLastError();
        if (GetCurrentThreadId() != ownerThread_)
        {
            lost_.store(true);
            SetLastError(savedError);
            return;
        }
        if (token >= observations_.size() || observations_[token].returned) lost_.store(true);
        else
        {
            auto& item = observations_[token];
            item.returnedThread = GetCurrentThreadId();
            item.returnedSequence = nextSequence_++;
            item.windowResult = value;
            item.returned = true;
        }
        SetLastError(savedError);
    }

public:
    // Read observations only on the constructing UI thread after dispatch.
    const std::vector<MessageObservation>& Observations() const { return observations_; }
    bool LostOrCrossThread() const { return lost_.load(); }
    DWORD OwnerThread() const noexcept { return ownerThread_; }

private:
    UINT completionMessage_;
    DWORD ownerThread_;
    std::uint64_t nextSequence_ = 1;
    std::atomic_bool lost_{false};
    std::vector<MessageObservation> observations_;
};

// The native input host has no OLE adapter or source. Only the fixture's
// window owner creates and invalidates this value-bound, STA-affine epoch.
class NativeHostEpoch final
{
    friend struct ::NativeDragIntegrationAccess;
    friend class V4NativeSequence;
    friend class V4NativeRunReceipt;
    friend class V4NativeDesktopExecutionKey;
    friend class V4NativeMemberExecutionKey;
    friend class V4NativeCategoryExecutionKey;
    friend class V4NativeReorderExecutionKey;
    friend class V4NativeSelectionExecutionKey;
    friend class V4NativeMultiDragExecutionKey;
    friend class V4NativeSelectionPreparationReceipt;
    friend class V4NativeSelectionPreparationExecutor;
public:
    NativeHostEpoch(const NativeHostEpoch&) = delete;
    NativeHostEpoch& operator=(const NativeHostEpoch&) = delete;
private:
    NativeHostEpoch(MessageTrace& trace, HWND desktop, HWND control, const void* app) noexcept
        : trace_(&trace), desktop_(desktop), control_(control), app_(reinterpret_cast<std::uintptr_t>(app)),
          ownerThread_(GetCurrentThreadId())
    {
        const DWORD saved = GetLastError();
        desktopProc_ = GetWindowLongPtrW(desktop_, GWLP_WNDPROC);
        controlProc_ = GetWindowLongPtrW(control_, GWLP_WNDPROC);
        SetLastError(saved);
    }
    bool CanInvalidate() const noexcept
    { return GetCurrentThreadId() == ownerThread_ && !executing_.load(std::memory_order_acquire); }
    void Invalidate() noexcept { active_.store(false, std::memory_order_release); }
    bool Matches(const MessageTrace* trace, HWND desktop, HWND control) const noexcept
    {
        const DWORD saved = GetLastError();
        DWORD desktopProcess = 0, controlProcess = 0;
        const bool ok = active_.load(std::memory_order_acquire) && ownerThread_ == GetCurrentThreadId() &&
            trace_ && trace == trace_ && trace_->OwnerThread() == ownerThread_ && !trace_->LostOrCrossThread() &&
            desktop == desktop_ && control == control_ && desktop_ && control_ && desktop_ != control_ &&
            IsWindow(desktop_) && IsWindow(control_) &&
            GetWindowThreadProcessId(desktop_, &desktopProcess) == ownerThread_ &&
            GetWindowThreadProcessId(control_, &controlProcess) == ownerThread_ &&
            desktopProcess == GetCurrentProcessId() && controlProcess == GetCurrentProcessId() && app_ &&
            static_cast<std::uintptr_t>(GetWindowLongPtrW(desktop_, GWLP_USERDATA)) == app_ &&
            static_cast<std::uintptr_t>(GetWindowLongPtrW(control_, GWLP_USERDATA)) == app_ &&
            desktopProc_ && controlProc_ && GetWindowLongPtrW(desktop_, GWLP_WNDPROC) == desktopProc_ &&
            GetWindowLongPtrW(control_, GWLP_WNDPROC) == controlProc_;
        SetLastError(saved);
        return ok;
    }
    bool TryBeginExecution(const MessageTrace* trace, HWND desktop, HWND control) const noexcept
    {
        if (!Matches(trace, desktop, control)) return false;
        bool expected = false;
        return executing_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }
    void EndExecution() const noexcept { executing_.store(false, std::memory_order_release); }
    MessageTrace* trace_ = nullptr;
    HWND desktop_ = nullptr, control_ = nullptr;
    std::uintptr_t app_ = 0;
    DWORD ownerThread_ = 0;
    LONG_PTR desktopProc_ = 0, controlProc_ = 0;
    std::atomic_bool active_{true};
    mutable std::atomic_bool executing_{false};
};

// Only the fixture's real window dispatcher may create/invalidate this token.
// The token proves an owning-thread window/adapter/source binding while it is
// active; it does not prove lack of non-adapter reentry or product success.
class V3HostEpoch final
{
    friend struct ::NativeDragIntegrationAccess;
    friend class V3GraphExecutor;
    friend class V3DropTaskWindowObservation;
    friend class V3OleRunReceipt;
    friend class V4GraphExecutor;
    friend class V4FileGraphExecutor;
    friend class V4FileOleRunReceipt;
    friend class V4MultiFileGraphExecutor;
    friend class V4MultiFileOleRunReceipt;
    friend class V4DirectoryGraphExecutor;
    friend class V4MixedGraphExecutor;
    friend class V4MixedOleRunReceipt;
    friend class V4ConfiguredContentGraphExecutor;
    friend class V4ConfiguredContentOleRunReceipt;
    friend class V4VirtualExecutionKey;
    friend class V4BatchExecutionKey;
    friend class V4CollectionExecutionKey;
    friend class V4GroupExecutionKey;
    friend class V4DirectoryExecutionKey;
    friend class V4DesktopFilesExecutionKey;
    friend class V4FolderMappingExecutionKey;
    friend class V4LeaveExecutionKey;
    friend class V4RejectExecutionKey;
    friend class V4MoveExecutionKey;
    friend class V4LeaveHostObservation;
    friend class V4DirectoryOleRunReceipt;
    friend class V4UnicodeReadObservation;
    friend class V4OleRunReceipt;
    friend class V4RefreshRequestObservation;
    friend class V4GroupHitActivationReader;
public:
    V3HostEpoch(const V3HostEpoch&) = delete;
    V3HostEpoch& operator=(const V3HostEpoch&) = delete;
    // A value-only comparison; the caller still validates the complete epoch
    // and receipt before attributing any window message to this host.
    bool IsDesktopWindow(HWND window) const noexcept
    { return active_.load(std::memory_order_acquire) && GetCurrentThreadId() == ownerThread_ && window && window == desktop_; }
    ~V3HostEpoch()
    {
        // Do not invoke apartment-bound Release from an arbitrary thread. The
        // normal owner-thread invalidation releases both references. If that
        // protocol was bypassed, retain them rather than release incorrectly.
        if (GetCurrentThreadId() != ownerThread_)
        {
            (void)adapter_.Detach();
            (void)source_.Detach();
        }
    }
private:
    V3HostEpoch(MessageTrace& trace, HWND desktop, HWND control, const void* app,
        OleDragDropAdapter* adapter, IDataObject* source) noexcept
        : trace_(&trace), desktop_(desktop), control_(control), app_(reinterpret_cast<std::uintptr_t>(app)),
          ownerThread_(GetCurrentThreadId())
    {
        const DWORD saved = GetLastError();
        desktopProc_ = GetWindowLongPtrW(desktop_, GWLP_WNDPROC);
        controlProc_ = GetWindowLongPtrW(control_, GWLP_WNDPROC);
        adapter_ = adapter; source_ = source;
        SetLastError(saved);
    }
    void Invalidate() noexcept
    {
        const DWORD saved = GetLastError();
        active_.store(false, std::memory_order_release);
        if (GetCurrentThreadId() == ownerThread_)
        {
            adapter_.Reset();
            source_.Reset();
        }
        SetLastError(saved);
    }
    bool CanInvalidate() const noexcept
    { return GetCurrentThreadId() == ownerThread_ && !executing_.load(std::memory_order_acquire); }
    bool TryBeginExecution(IDataObject* source, OleDragDropAdapter* adapter) const noexcept
    {
        if (!Matches(source, adapter)) return false;
        bool expected = false;
        return executing_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }
    void EndExecution() const noexcept { executing_.store(false, std::memory_order_release); }
    bool Matches(IDataObject* source, OleDragDropAdapter* adapter,
        const MessageTrace* trace = nullptr, HWND control = nullptr) const noexcept
    {
        const DWORD saved = GetLastError();
        DWORD desktopProcess = 0, controlProcess = 0;
        const bool ok = active_.load(std::memory_order_acquire) && ownerThread_ == GetCurrentThreadId() &&
            trace_ && trace_->OwnerThread() == ownerThread_ && !trace_->LostOrCrossThread() &&
            (!trace || trace == trace_) && (!control || control == control_) &&
            source && source_.Get() == source && adapter && adapter_.Get() == adapter &&
            desktop_ && control_ && desktop_ != control_ && IsWindow(desktop_) && IsWindow(control_) &&
            GetWindowThreadProcessId(desktop_, &desktopProcess) == ownerThread_ &&
            GetWindowThreadProcessId(control_, &controlProcess) == ownerThread_ &&
            desktopProcess == GetCurrentProcessId() && controlProcess == GetCurrentProcessId() &&
            app_ && static_cast<std::uintptr_t>(GetWindowLongPtrW(desktop_, GWLP_USERDATA)) == app_ &&
            static_cast<std::uintptr_t>(GetWindowLongPtrW(control_, GWLP_USERDATA)) == app_ &&
            desktopProc_ && controlProc_ && GetWindowLongPtrW(desktop_, GWLP_WNDPROC) == desktopProc_ &&
            GetWindowLongPtrW(control_, GWLP_WNDPROC) == controlProc_;
        SetLastError(saved);
        return ok;
    }
    MessageTrace* trace_ = nullptr;
    HWND desktop_ = nullptr, control_ = nullptr;
    std::uintptr_t app_ = 0;
    DWORD ownerThread_ = 0;
    LONG_PTR desktopProc_ = 0, controlProc_ = 0;
    std::atomic_bool active_{true};
    mutable std::atomic_bool executing_{false};
    Microsoft::WRL::ComPtr<OleDragDropAdapter> adapter_;
    Microsoft::WRL::ComPtr<IDataObject> source_;
};

struct NativeDeliveryEvidence
{
    // A local WndProc entry/return and its admitted permit were uniquely
    // matched. This is not proof of graph role or business behavior.
    bool matchedMessageAndPermit = false;
    std::size_t traceIndex = MessageTrace::InvalidToken;
    std::vector<std::string> unresolved;
};

// Read-only correlation for one input delivery. The device log is a bounded
// value log, not an authorization source: a rejected record only describes
// the current context, never the rejected message's origin. A forwarded
// record lacks a complete parent/scope tree, and an End/cancel can share the
// last event sequence; both require additional independent evidence.
inline NativeDeliveryEvidence InspectNativeDelivery(const MessageTrace& trace, std::size_t traceStart,
    const snowdesktop::private_pointer::DeliveryObservationLog* log,
    const snowdesktop::private_pointer::DeliveryIdentity& expected,
    HWND window, MessageRoute route, UINT message, WPARAM wp, LPARAM lp)
{
    using namespace snowdesktop::private_pointer;
    NativeDeliveryEvidence result;
    const auto unresolved = [&](std::string reason) { result.unresolved.push_back(std::move(reason)); };
    if (!log || log->failed || trace.LostOrCrossThread() || traceStart > trace.Observations().size() ||
        log->ownerThread != GetCurrentThreadId() || !log->session || expected.session != log->session ||
        expected.ownerThread != log->ownerThread || !expected.delivery || !window || !IsWindow(window) ||
        (expected.forwarded != (expected.forward != 0)) || (expected.queuedEvent != (expected.event != 0)))
    { unresolved("native_delivery_log_or_identity_unavailable"); return result; }
    const auto same = [](const DeliveryIdentity& a, const DeliveryIdentity& b) {
        return a.session == b.session && a.event == b.event && a.delivery == b.delivery &&
            a.forwarded == b.forwarded && a.ownerThread == b.ownerThread && a.forward == b.forward &&
            a.queuedEvent == b.queuedEvent;
    };
    auto outer = expected;
    outer.forwarded = false;
    outer.forward = 0;
    std::size_t begin = MessageTrace::InvalidToken, permit = MessageTrace::InvalidToken, returned = MessageTrace::InvalidToken;
    std::size_t traceIndex = MessageTrace::InvalidToken;
    for (std::size_t i = 0; i < log->entries.size(); ++i)
    {
        const auto& item = log->entries[i];
        if (same(item.identity, outer) && item.kind == DeliveryObservationKind::begin)
        {
            if (begin != MessageTrace::InvalidToken) { unresolved("native_delivery_duplicate_begin"); return result; }
            begin = i;
        }
        if (same(item.identity, outer) && item.kind == DeliveryObservationKind::returned)
        {
            if (returned != MessageTrace::InvalidToken) { unresolved("native_delivery_duplicate_return"); return result; }
            returned = i;
        }
        if (same(item.identity, expected) && item.kind == DeliveryObservationKind::permit_rejected)
        { unresolved("native_delivery_rejected_context_ambiguous"); return result; }
        if (same(item.identity, expected) && item.kind == DeliveryObservationKind::permit_consumed)
        {
            if (permit != MessageTrace::InvalidToken) { unresolved("native_delivery_duplicate_permit"); return result; }
            permit = i;
        }
    }
    for (std::size_t i = traceStart; i < trace.Observations().size(); ++i)
        if (same(trace.Observations()[i].delivery, expected))
        {
            if (traceIndex != MessageTrace::InvalidToken) { unresolved("native_delivery_reentrant_trace_ambiguous"); return result; }
            traceIndex = i;
        }
    if (begin == MessageTrace::InvalidToken || permit == MessageTrace::InvalidToken || returned == MessageTrace::InvalidToken ||
        traceIndex == MessageTrace::InvalidToken || !(begin < permit && permit < returned))
    { unresolved("native_delivery_begin_permit_return_incomplete"); return result; }
    const auto& first = log->entries[begin];
    const auto& allowed = log->entries[permit];
    const auto& last = log->entries[returned];
    const auto& observed = trace.Observations()[traceIndex];
    if (first.window != last.window || first.message != last.message || first.wParam != last.wParam || first.lParam != last.lParam ||
        allowed.window != window || allowed.message != message || allowed.wParam != wp || allowed.lParam != lp ||
        observed.window != window || observed.route != route || observed.message != message || observed.wParam != wp ||
        !observed.inputLParam || *observed.inputLParam != lp || !observed.returned ||
        observed.enteredThread != log->ownerThread || observed.returnedThread != observed.enteredThread ||
        observed.returnedSequence <= observed.enteredSequence)
    { unresolved("native_delivery_message_or_return_mismatch"); return result; }
    if (!expected.forwarded && (first.window != window || first.message != message || first.wParam != wp ||
        first.lParam != lp || last.result != observed.windowResult))
    { unresolved("native_delivery_outer_return_mismatch"); return result; }
    result.matchedMessageAndPermit = true;
    result.traceIndex = traceIndex;
    if (expected.forwarded) unresolved("native_forward_parent_and_scope_tree_unobserved");
    unresolved(expected.queuedEvent ? "native_queued_event_program_binding_unestablished" :
        "native_nonqueued_cleanup_or_secondary_role_unestablished");
    unresolved("native_delivery_business_outcome_unestablished");
    return result;
}

// This is only a transport observation for one isolated asynchronous source.
// The host reads these fields from its actual source object and production
// idle state. They do not prove a file, membership, or layout result.
struct CompletionHostState
{
    // For DragLeave this must include the production ExternalLeaveIdle test,
    // including expiration of the leave grace period, as well as file idle.
    bool productionIdle = false;
    std::uint64_t sourceStarts = 0;
    std::uint64_t sourceEnds = 0;
};
enum class CompletionSignal { pending, completed_messages, terminal_without_lease, failed_message, invalid_trace, timed_out, interrupted };
struct CompletionEvidence
{
    CompletionSignal signal = CompletionSignal::pending;
    std::uint64_t sourceEntered = 0;
    std::uint64_t commitEntered = 0;
    std::string detail;
};

// The caller must keep the SAME profile, source, adapter and trace alive and
// isolate the operation: Shell completion messages carry no task identity.
// This routine observes only the asynchronous CF_HDROP source-read -> file
// operation route. Other transports and concurrent messages are unsupported.
inline CompletionEvidence InspectCompletion(const MessageTrace& trace, std::size_t start,
    HWND window, MessageRoute route, const Observation& terminal,
    const CompletionHostState& before, const CompletionHostState& now, UINT completionMessage)
{
    CompletionEvidence evidence;
    const auto invalid = [&](std::string reason) {
        evidence.signal = CompletionSignal::invalid_trace;
        evidence.detail = std::move(reason);
        return evidence;
    };
    if (trace.LostOrCrossThread() || start > trace.Observations().size() || !window || !IsWindow(window))
        return invalid("trace_or_window_invalid");
    if ((terminal.kind != Kind::drop && terminal.kind != Kind::leave) || FAILED(terminal.result))
        return invalid("terminal_ole_call_not_observed_successfully");
    if (now.sourceStarts < before.sourceStarts || now.sourceEnds < before.sourceEnds ||
        now.sourceStarts - before.sourceStarts > 1 || now.sourceEnds - before.sourceEnds > 1)
        return invalid("source_lease_not_single_operation");
    unsigned sourceCount = 0, commitCount = 0;
    bool failed = false;
    for (std::size_t i = start; i < trace.Observations().size(); ++i)
    {
        const auto& event = trace.Observations()[i];
        if (event.message != completionMessage) continue;
        if (event.window != window || event.route != route || event.malformedCompletion || !event.completion ||
            event.enteredThread != GetCurrentThreadId()) return invalid("completion_route_or_payload_invalid");
        if (!event.returned) return evidence;
        if (event.returnedThread != event.enteredThread || event.returnedSequence <= event.enteredSequence)
            return invalid("completion_dispatch_return_invalid");
        failed |= !event.completion->succeeded;
        if (event.completion->fileOperation) { ++commitCount; evidence.commitEntered = event.enteredSequence; }
        else { ++sourceCount; evidence.sourceEntered = event.enteredSequence; }
    }
    if (sourceCount > 1 || commitCount > 1) return invalid("completion_attribution_ambiguous");
    if (failed) { evidence.signal = CompletionSignal::failed_message; return evidence; }
    if (sourceCount && commitCount && evidence.sourceEntered >= evidence.commitEntered)
        return invalid("completion_order_invalid");
    if (!now.productionIdle) return evidence;
    const auto starts = now.sourceStarts - before.sourceStarts;
    const auto ends = now.sourceEnds - before.sourceEnds;
    if (sourceCount == 1 && commitCount == 1 && starts == 1 && ends == 1)
        evidence.signal = CompletionSignal::completed_messages;
    else if (!sourceCount && !commitCount && !starts && !ends &&
        (terminal.kind == Kind::leave || (terminal.returnedEffect && *terminal.returnedEffect == DROPEFFECT_NONE)))
        evidence.signal = CompletionSignal::terminal_without_lease;
    return evidence;
}

// Pump the actual owning UI thread until observed completion or an explicit
// bound. `readState` only reads the real host/source, and `throwAsync` surfaces
// exceptions captured by the fixture WndProc. Neither callback performs any
// product operation. A terminal signal is not a business assertion or PASS.
inline CompletionEvidence WaitForCompletion(const MessageTrace& trace, std::size_t start,
    HWND window, MessageRoute route, const Observation& terminal,
    const CompletionHostState& before, UINT completionMessage, DWORD timeoutMs,
    const std::function<CompletionHostState()>& readState,
    const std::function<void()>& throwAsync)
{
    if (!readState || !throwAsync) return {CompletionSignal::invalid_trace, 0, 0, "host_observer_missing"};
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;)
    {
        throwAsync();
        auto evidence = InspectCompletion(trace, start, window, route, terminal, before, readState(), completionMessage);
        throwAsync();
        if (evidence.signal != CompletionSignal::pending) return evidence;
        if (GetTickCount64() >= deadline) { evidence.signal = CompletionSignal::timed_out; return evidence; }
        if (MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_FAILED)
            return {CompletionSignal::invalid_trace, 0, 0, "message_wait_failed"};
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                PostQuitMessage(static_cast<int>(message.wParam));
                evidence.signal = CompletionSignal::interrupted;
                return evidence;
            }
            DispatchMessageW(&message);
            throwAsync();
            if (GetTickCount64() >= deadline) break;
        }
    }
}

enum class UrlCompletionSignal { pending, signals_observed_unattributed, unresolved, invalid_trace, timed_out, interrupted };
struct UrlCompletionEvidence
{
    UrlCompletionSignal signal = UrlCompletionSignal::pending;
    std::size_t urlTraceIndex = MessageTrace::InvalidToken;
    std::size_t commitTraceIndex = MessageTrace::InvalidToken;
    std::string detail;
};

// A value snapshot captured by the actual host before the URL completion
// owner consumes its LPARAM. It never retains that owned pointer.
struct UrlWorkerObservation
{
    std::size_t traceIndex = MessageTrace::InvalidToken;
    snowdesktop::UrlDropDownloadOutcome outcome = snowdesktop::UrlDropDownloadOutcome::Failed;
    std::wstring originalUrl, finalUrl, localPath, contentType, shortcutFallbackUrl;
    std::size_t remainingUrls = 0, replacementShortcuts = 0;
    bool retryableCandidateFailure = false, returned = false, malformed = false;
};
struct UrlWorkerBinding
{
    bool observedSingleShortcutResult = false;
    std::vector<std::string> unresolved;
};

// This validates copied event data against the real trace and exact input, not
// an assumed worker decision. It does not associate an unlabelled subsequent
// Shell completion with that data or inspect the resulting shortcut.
inline UrlWorkerBinding InspectUrlWorkerResult(const MessageTrace& trace,
    const UrlCompletionEvidence& signal, const std::vector<UrlWorkerObservation>& observations,
    bool observationFailed, HWND window, MessageRoute route, UINT urlMessage, std::wstring_view expectedUrl)
{
    UrlWorkerBinding result;
    if (observationFailed || trace.LostOrCrossThread() || signal.urlTraceIndex == MessageTrace::InvalidToken ||
        signal.urlTraceIndex >= trace.Observations().size())
    { result.unresolved.emplace_back("url_worker_snapshot_or_trace_incomplete"); return result; }
    const auto& event = trace.Observations()[signal.urlTraceIndex];
    if (!window || !IsWindow(window) || !urlMessage || event.window != window || event.route != route || event.message != urlMessage ||
        !event.returned || event.enteredThread != GetCurrentThreadId() || event.returnedThread != event.enteredThread ||
        event.returnedSequence <= event.enteredSequence)
    { result.unresolved.emplace_back("url_worker_dispatch_return_unobserved"); return result; }
    const UrlWorkerObservation* selected = nullptr;
    for (const auto& observation : observations)
    {
        if (observation.traceIndex != signal.urlTraceIndex) continue;
        if (selected) { result.unresolved.emplace_back("url_worker_snapshot_identity_ambiguous"); return result; }
        selected = &observation;
    }
    if (!selected || !selected->returned || selected->malformed)
    { result.unresolved.emplace_back("url_worker_snapshot_missing_or_malformed"); return result; }
    if (expectedUrl.empty() || selected->originalUrl != expectedUrl || selected->shortcutFallbackUrl != expectedUrl)
        result.unresolved.emplace_back("url_worker_input_or_fallback_binding_unimplemented");
    if (selected->remainingUrls || selected->replacementShortcuts)
        result.unresolved.emplace_back("url_worker_retry_or_replacement_unimplemented");
    if (selected->outcome == snowdesktop::UrlDropDownloadOutcome::Failed)
        result.unresolved.emplace_back("url_worker_failed_retry_or_fallback_unimplemented");
    else if (selected->outcome == snowdesktop::UrlDropDownloadOutcome::Downloaded)
        result.unresolved.emplace_back("url_worker_downloaded_copy_or_fallback_unimplemented");
    else if (selected->outcome != snowdesktop::UrlDropDownloadOutcome::Shortcut)
        result.unresolved.emplace_back("url_worker_outcome_unimplemented");
    if (selected->retryableCandidateFailure)
        result.unresolved.emplace_back("url_worker_retryable_state_unimplemented");
    result.observedSingleShortcutResult = result.unresolved.empty();
    // A semantic URL result still cannot identify the next Shell task.
    result.unresolved.emplace_back("url_worker_to_file_commit_identity_unresolved");
    result.unresolved.emplace_back("url_final_output_and_layout_oracle_unimplemented");
    return result;
}

// One isolated URL task only: URL worker -> actual host WndProc -> one actual
// asynchronous file-operation completion. Neither message contains a task ID;
// retries, replacements, other sources and concurrent tasks remain outside
// this observer. The host separately inspects the URL result copied before its
// owned completion is consumed, then asserts real output, model and storage.
inline UrlCompletionEvidence InspectUrlCompletion(const MessageTrace& trace, std::size_t start,
    HWND window, MessageRoute route, const Observation& terminal, UINT urlMessage,
    UINT fileMessage, bool hostIdle)
{
    UrlCompletionEvidence result;
    const auto invalid = [&](std::string reason) {
        result.signal = UrlCompletionSignal::invalid_trace;
        result.detail = std::move(reason);
        return result;
    };
    if (trace.LostOrCrossThread() || start > trace.Observations().size() || !window || !IsWindow(window) ||
        !urlMessage || !fileMessage || urlMessage == fileMessage)
        return invalid("url_trace_window_or_message_invalid");
    if (terminal.kind != Kind::drop || FAILED(terminal.result) || !terminal.returnedEffect ||
        ((*terminal.returnedEffect & (DROPEFFECT_COPY | DROPEFFECT_LINK)) == 0))
        return invalid("url_terminal_drop_not_accepted");
    std::size_t urlCount = 0, fileCount = 0;
    for (std::size_t i = start; i < trace.Observations().size(); ++i)
    {
        const auto& event = trace.Observations()[i];
        if (event.message != urlMessage && event.message != fileMessage) continue;
        if (event.window != window || event.route != route || event.enteredThread != GetCurrentThreadId())
            return invalid("url_completion_route_or_thread_invalid");
        if (!event.returned) return result;
        if (event.returnedThread != event.enteredThread || event.returnedSequence <= event.enteredSequence)
            return invalid("url_completion_return_invalid");
        if (event.message == urlMessage)
        { ++urlCount; result.urlTraceIndex = i; }
        else
        {
            if (event.malformedCompletion || !event.completion)
                return invalid("url_file_completion_payload_invalid");
            if (!event.completion->fileOperation)
            { result.signal = UrlCompletionSignal::unresolved; result.detail = "url_other_source_completion_unattributed"; return result; }
            ++fileCount; result.commitTraceIndex = i;
        }
    }
    if (urlCount > 1 || fileCount > 1)
    {
        result.signal = UrlCompletionSignal::unresolved;
        result.detail = urlCount > 1 ? "url_retry_or_multiple_tasks_unimplemented" : "url_multiple_commits_or_fallback_unimplemented";
        return result;
    }
    if (!urlCount || !fileCount) return result;
    const auto& url = trace.Observations()[result.urlTraceIndex];
    const auto& commit = trace.Observations()[result.commitTraceIndex];
    // A nested loop may dispatch the file completion before the URL WndProc
    // returns, but the file completion cannot precede entry to that WndProc.
    if (url.enteredSequence >= commit.enteredSequence)
    { result.signal = UrlCompletionSignal::unresolved; result.detail = "url_commit_task_identity_unresolved"; return result; }
    if (!commit.completion->succeeded)
    {
        // A failed downloaded-file copy can schedule a shortcut fallback.
        result.signal = UrlCompletionSignal::unresolved;
        result.detail = "url_file_failure_and_possible_fallback_unimplemented";
        return result;
    }
    if (hostIdle)
    {
        // The completion has no operation/source identity; order does not
        // establish which submission it belongs to, even on an idle host.
        result.signal = UrlCompletionSignal::signals_observed_unattributed;
        result.detail = "url_to_file_commit_identity_unresolved";
    }
    return result;
}

// readHostIdle only reads production pending/idle state for the same fixture;
// throwAsync rethrows exceptions observed by its actual WndProc. Returned
// messages are transport evidence only, never an output or business oracle.
inline UrlCompletionEvidence WaitForUrlCompletion(const MessageTrace& trace, std::size_t start,
    HWND window, MessageRoute route, const Observation& terminal, UINT urlMessage, UINT fileMessage,
    DWORD timeoutMs, const std::function<bool()>& readHostIdle, const std::function<void()>& throwAsync)
{
    if (!readHostIdle || !throwAsync)
        return {UrlCompletionSignal::invalid_trace, MessageTrace::InvalidToken, MessageTrace::InvalidToken, "url_host_observer_missing"};
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;)
    {
        throwAsync();
        auto evidence = InspectUrlCompletion(trace, start, window, route, terminal, urlMessage, fileMessage, readHostIdle());
        throwAsync();
        if (evidence.signal != UrlCompletionSignal::pending) return evidence;
        if (GetTickCount64() >= deadline) { evidence.signal = UrlCompletionSignal::timed_out; return evidence; }
        if (MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_FAILED)
            return {UrlCompletionSignal::invalid_trace, MessageTrace::InvalidToken, MessageTrace::InvalidToken, "url_message_wait_failed"};
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                PostQuitMessage(static_cast<int>(message.wParam));
                evidence.signal = UrlCompletionSignal::interrupted;
                return evidence;
            }
            DispatchMessageW(&message);
            throwAsync();
            if (GetTickCount64() >= deadline) break;
        }
    }
}

struct Inspection
{
    std::vector<std::string> obligations;
    // Structurally absent task-B outcomes are not observation obligations.
    std::vector<std::string> notApplicable;
    bool CanExecute() const { return obligations.empty(); }
};

enum class Status { unimplemented, attempted, event_failed, event_oracle_failed, host_observation_failed, fixture_failed,
                    observation_unavailable, business_failed };
struct Result
{
    Status status = Status::unimplemented;
    bool productionStarted = false;
    // True when a native dispatch may have entered production but the host
    // trace could not be inspected; it must not be interpreted as not started.
    bool productionEntryUnknown = false;
    // Independent facts can coexist; an observation failure must not erase a
    // previously established business assertion failure.
    bool businessFailureObserved = false;
    bool eventOracleFailureObserved = false;
    // Oracle callbacks may report differences before the diagnostic attempt
    // has proved candidate/graph attribution. Preserve their raw flags without
    // promoting them to authenticated product failures.
    std::optional<bool> unattributedBusinessFailureObserved, unattributedEventOracleFailureObserved;
    bool additionalObservationUnavailable = false;
    std::string failure;
    std::vector<std::string> obligations;
    // Expected outcomes and event presence require the same actual execution;
    // they are not pre-dispatch facts or caller-controlled device inputs.
    std::vector<std::string> postconditionsPending;
    // Reasons whose stage has not yet been established. They remain a
    // capability blocker and may not be silently promoted to observations.
    std::vector<std::string> unclassifiedObligations;
    std::vector<Observation> observations;
    // Last real TLS adapter-log state, including loss/overflow even when a
    // Dispatch cannot be certified. No completion is inferred from this value.
    std::optional<snowdesktop::ole_adapter_observation::State> oleAdapterLogLast;
    // These indicate a host callback returned, not that a result passed.
    std::vector<std::string> hostStagesReturned;
};

inline std::optional<Kind> ProgramKind(std::string_view kind)
{
    if (kind == "external_enter") return Kind::enter;
    if (kind == "hover" || kind == "hover_later") return Kind::over;
    if (kind == "release") return Kind::drop;
    if (kind == "external_leave") return Kind::leave;
    return std::nullopt;
}

inline bool SameProgramPostconditions(const EventProgram& left, const EventProgram& right)
{
    if (left.postconditions.size() != right.postconditions.size()) return false;
    for (std::size_t i = 0; i < left.postconditions.size(); ++i)
        if (left.postconditions[i].axis != right.postconditions[i].axis ||
            left.postconditions[i].value != right.postconditions[i].value) return false;
    return true;
}

struct KeyEncoding
{
    DWORD value = 0;
    bool valid = false;
    bool altX1Ambiguous = false;
};

// This only encodes supplied input bits. It never chooses a drop action.
// Canonical token order is the order used by the model's generated domains.
inline KeyEncoding EncodeKeyState(std::string_view buttons, std::string_view modifiers)
{
    KeyEncoding result;
    const auto parse = [](std::string_view text, const char* const* names, const DWORD* bits,
                          std::size_t count, DWORD& output) {
        if (text == "none") return true;
        if (text.empty() || text == "unknown" || text == "na") return false;
        std::size_t start = 0, nextAllowed = 0;
        while (start < text.size())
        {
            const auto end = text.find('_', start);
            const auto token = text.substr(start, end == std::string_view::npos ? end : end - start);
            std::size_t index = nextAllowed;
            while (index < count && token != names[index]) ++index;
            if (index == count) return false;
            output |= bits[index];
            nextAllowed = index + 1;
            if (end == std::string_view::npos) return true;
            start = end + 1;
            if (start == text.size()) return false;
        }
        return false;
    };
    const char* const buttonNames[]{"left", "right", "middle", "x1", "x2"};
    const DWORD buttonBits[]{MK_LBUTTON, MK_RBUTTON, MK_MBUTTON, MK_XBUTTON1, MK_XBUTTON2};
    const char* const modifierNames[]{"ctrl", "shift", "alt"};
    const DWORD modifierBits[]{MK_CONTROL, MK_SHIFT, MK_ALT};
    DWORD buttonMask = 0, modifierMask = 0;
    if (!parse(buttons, buttonNames, buttonBits, 5, buttonMask) ||
        !parse(modifiers, modifierNames, modifierBits, 3, modifierMask)) return result;
    result.value = buttonMask | modifierMask;
    // WinUser's MK_XBUTTON1 and oleidl's MK_ALT both equal 0x20. A raw
    // keyState with this bit cannot independently demonstrate those inputs.
    result.altX1Ambiguous = MK_XBUTTON1 == MK_ALT && (result.value & MK_ALT) != 0;
    result.valid = true;
    return result;
}

inline std::optional<std::pair<std::string_view, std::string_view>> ParseKeyParameter(std::string_view parameter)
{
    constexpr std::string_view prefix = "buttons=";
    constexpr std::string_view separator = ";modifiers=";
    if (parameter.substr(0, prefix.size()) != prefix) return std::nullopt;
    const auto split = parameter.find(separator, prefix.size());
    if (split == std::string_view::npos) return std::nullopt;
    return std::pair{parameter.substr(prefix.size(), split - prefix.size()),
        parameter.substr(split + separator.size())};
}

inline std::pair<std::string_view, std::string_view> PhaseAxes(std::string_view kind)
{
    if (kind == "external_enter") return {"G02.enter_buttons", "G02.enter"};
    if (kind == "hover") return {"G02.hover_first_buttons", "G02.hover_first"};
    if (kind == "hover_later") return {"G02.hover_later_buttons", "G02.hover_later"};
    if (kind == "release") return {"G02.release_buttons", "G02.release"};
    return {};
}

inline std::optional<DWORD> EncodeAllowedEffect(std::string_view effects, std::string_view extra)
{
    DWORD mask = 0;
    if (effects != "none")
    {
        const char* const names[]{"copy", "move", "link"};
        const DWORD bits[]{DROPEFFECT_COPY, DROPEFFECT_MOVE, DROPEFFECT_LINK};
        if (effects.empty() || effects == "na" || effects == "unknown") return std::nullopt;
        std::size_t start = 0, nextAllowed = 0;
        while (start < effects.size())
        {
            const auto end = effects.find('_', start);
            const auto token = effects.substr(start, end == std::string_view::npos ? end : end - start);
            std::size_t index = nextAllowed;
            while (index < 3 && token != names[index]) ++index;
            if (index == 3) return std::nullopt;
            mask |= bits[index];
            nextAllowed = index + 1;
            if (end == std::string_view::npos) break;
            start = end + 1;
            if (start == effects.size()) return std::nullopt;
        }
    }
    if (extra == "scroll") mask |= DROPEFFECT_SCROLL;
    else if (extra != "none") return std::nullopt;
    return mask;
}

struct V3OleInputAudit
{
    bool candidateIdentityValid = false;
    std::string rawCandidateId;
    // These are only candidate/program-bound raw input encodings. They do not
    // prove a live source, target, coordinate mapping or a production call.
    std::vector<Input> phaseEncodingBindings;
    // Expectations only; actual comparison must use the same completed run.
    std::vector<OutcomeHypothesis> postconditions;
    std::vector<std::string> preDispatchBlockers;
    std::vector<std::string> postDispatchObligations;
    // Upstream mapping/program reasons still mix stages. Never guess their
    // category from string contents; their presence blocks dispatch until the
    // source supplies typed evidence.
    std::vector<std::string> unclassifiedObligations;
};

inline V3OleInputAudit InspectV3OleInputs(const V3FamilyValidatedContext& verified,
    const event_graph::V3FamilyIdentityContext& identity, const std::vector<Input>& inputs)
{
    V3OleInputAudit result;
    try
    {
        if (verified.Status() != V3CoordinateStatus::coordinate_ready ||
            identity.Status() != V3FamilyStatus::schema_only ||
            verified.FamilyBinding() != identity.FamilyBinding())
        { result.preDispatchBlockers.emplace_back("v3_ole_family_context_unavailable"); return result; }
        const auto& axes = verified.Axes();
        const auto& candidate = verified.Coordinate();
        const auto& graphAxes = identity.Axes();
        if (axes.size() != graphAxes.size())
        { result.preDispatchBlockers.emplace_back("v3_ole_schema_mismatch"); return result; }
        for (std::size_t i = 0; i < axes.size(); ++i)
            if (axes[i].id != graphAxes[i].id || axes[i].role != graphAxes[i].role ||
                axes[i].values != graphAxes[i].values || axes[i].label != graphAxes[i].label)
            { result.preDispatchBlockers.push_back("v3_ole_schema_mismatch:" + std::to_string(i)); return result; }
        const auto raw = identity.RawCandidateIdentity(candidate);
        result.preDispatchBlockers.insert(result.preDispatchBlockers.end(), raw.errors.begin(), raw.errors.end());
        result.unclassifiedObligations.insert(result.unclassifiedObligations.end(), raw.unresolved.begin(), raw.unresolved.end());
        if (raw.id.empty() || !raw.errors.empty())
        { result.preDispatchBlockers.emplace_back("v3_ole_candidate_identity_unavailable"); return result; }
        result.candidateIdentityValid = true;
        result.rawCandidateId = raw.id;
        const auto program = verified.TransferProgram();
        result.postconditions = program.postconditions;
        for (const auto& condition : program.postconditions)
            result.postDispatchObligations.push_back("v3_outcome_observation_pending:" + condition.axis);
        const auto mapping = event_graph::MapV3FamilyCandidate(identity, verified, program);
        result.unclassifiedObligations.insert(result.unclassifiedObligations.end(), mapping.reasons.begin(), mapping.reasons.end());
        if (!mapping.structuralExclusion.empty())
            result.preDispatchBlockers.push_back("v3_ole_local_structure_excluded:" + mapping.structuralExclusion);
        if (!mapping.graphAvailable) result.preDispatchBlockers.emplace_back("v3_ole_event_graph_unavailable");
        for (const auto& reason : program.unresolved) result.preDispatchBlockers.push_back("v3_program:" + reason);
        if (verified.TaskBActive()) result.preDispatchBlockers.emplace_back("v3_ole_multiple_sessions_unimplemented");
        if (Value(axes, candidate, "T01") != "external_ingress")
            result.preDispatchBlockers.emplace_back("v3_ole_external_ingress_required_for_this_adapter");
        const auto allowed = EncodeAllowedEffect(Value(axes, candidate, "T03"),
            Value(axes, candidate, "T03.extra_protocol_bits"));
        if (!allowed) result.preDispatchBlockers.emplace_back("v3_ole_allowed_effect_unavailable");
        std::vector<const Input*> byStep(program.steps.size(), nullptr);
        for (const auto& input : inputs)
        {
            if (input.programIndex >= program.steps.size() || byStep[input.programIndex] ||
                !ProgramKind(program.steps[input.programIndex].kind))
            { result.preDispatchBlockers.emplace_back("v3_ole_input_index_invalid_or_duplicate"); continue; }
            byStep[input.programIndex] = &input;
        }
        bool entered = false, terminated = false;
        for (std::size_t i = 0; i < program.steps.size(); ++i)
        {
            const auto& step = program.steps[i];
            const auto kind = ProgramKind(step.kind);
            if (!kind)
            {
                if (step.kind != "observe_completion_or_rejection" && step.kind != "assert_business_results")
                    result.preDispatchBlockers.push_back("v3_ole_non_call_stage_requires_real_executor:" + step.kind);
                continue;
            }
            if (terminated || (*kind == Kind::enter && entered) || (*kind != Kind::enter && !entered))
                result.preDispatchBlockers.push_back("v3_ole_session_sequence_unimplemented:" + std::to_string(i));
            if (*kind == Kind::enter) entered = true;
            if (*kind == Kind::drop || *kind == Kind::leave) terminated = true;
            const auto* input = byStep[i];
            if (!input || input->kind != *kind || input->modelParameter != step.parameter)
            { result.preDispatchBlockers.push_back("v3_ole_step_binding_unavailable:" + std::to_string(i)); continue; }
            if (*kind == Kind::leave)
            {
                if (input->keyState || input->allowedEffect || input->screenPoint)
                { result.preDispatchBlockers.push_back("v3_ole_leave_arguments_invalid:" + std::to_string(i)); continue; }
            }
            else
            {
                const auto [buttonAxis, modifierAxis] = PhaseAxes(step.kind);
                const auto buttons = Value(axes, candidate, buttonAxis), modifiers = Value(axes, candidate, modifierAxis);
                const auto parsed = ParseKeyParameter(step.parameter);
                const auto encoded = EncodeKeyState(buttons, modifiers);
                if (!parsed || parsed->first != buttons || parsed->second != modifiers || !encoded.valid ||
                    encoded.altX1Ambiguous || !input->keyState || *input->keyState != encoded.value ||
                    !allowed || !input->allowedEffect || *input->allowedEffect != *allowed || !input->screenPoint)
                { result.preDispatchBlockers.push_back("v3_ole_phase_input_unavailable:" + std::to_string(i)); continue; }
            }
            result.phaseEncodingBindings.push_back(*input);
        }
        if (!entered || !terminated) result.preDispatchBlockers.emplace_back("v3_ole_terminal_sequence_unavailable");
        result.preDispatchBlockers.emplace_back("v3_ole_input_geometry_live_source_and_host_binding_unimplemented");
        result.preDispatchBlockers.emplace_back("v3_ole_event_graph_and_real_dispatch_unimplemented");
        result.preDispatchBlockers.emplace_back("v3_ole_completion_and_independent_business_oracle_unimplemented");
    }
    catch (const std::bad_alloc&)
    { result.phaseEncodingBindings.clear(); result.preDispatchBlockers.emplace_back("v3_ole_input_audit_resource_unavailable"); }
    catch (const std::length_error&)
    { result.phaseEncodingBindings.clear(); result.preDispatchBlockers.emplace_back("v3_ole_input_audit_resource_unavailable"); }
    return result;
}

// Inspect the WHOLE program before any adapter call. No caller-supplied flag
// can make an unknown step or missing production wait/oracle executable.
// The caller may inspect the result, but Run repeats the inspection itself.
inline Inspection Inspect(const std::vector<Axis>& axes, const Candidate& candidate,
    const EventProgram& program, const std::vector<Input>& inputs,
    SelectionSchemaVersion version = SelectionSchemaVersion::legacy_v1)
{
    Inspection result;
    if (candidate.values.size() != axes.size())
    {
        result.obligations.emplace_back("candidate_axis_count_mismatch");
        return result;
    }
    const auto expectedSchema = Model(version);
    if (axes.size() != expectedSchema.size())
    {
        result.obligations.emplace_back("model_schema_axis_count_mismatch");
        return result;
    }
    for (std::size_t i = 0; i < axes.size(); ++i)
    {
        if (axes[i].id != expectedSchema[i].id || axes[i].role != expectedSchema[i].role ||
            axes[i].values != expectedSchema[i].values || axes[i].label != expectedSchema[i].label)
            result.obligations.push_back("model_schema_mismatch:" + std::to_string(i));
        const auto& value = candidate.values[i];
        if (axes[i].role == Role::Derived && !value.empty())
            result.obligations.push_back("candidate_derived_value_must_be_empty:" + axes[i].id);
        else if (axes[i].role != Role::Derived &&
            std::find(axes[i].values.begin(), axes[i].values.end(), value) == axes[i].values.end())
            result.obligations.push_back("candidate_value_outside_domain:" + axes[i].id);
    }
    // Do not evaluate the trusted program or its predicates using malformed
    // schema/values: Value assumes a coherent candidate and index.
    if (!result.obligations.empty()) return result;
    // Key/effect encoders establish only raw input equivalence; they do not
    // prove that a fixture constructed the candidate's environment, payload,
    // target or any derived observation. Keep an explicit obligation for each
    // axis until its own real constructor/observer is integrated here.
    Candidate canonical = candidate;
    canonical.lookup = BuildIndex(axes);
    for (std::size_t i = 0; i < axes.size(); ++i)
    {
        if (axes[i].id.substr(0, 7) == "task_b." && Value(axes, canonical, "X03.task_count") == "one" &&
            (axes[i].role == Role::Derived ? candidate.values[i].empty() : candidate.values[i] == "na"))
        { result.notApplicable.push_back(axes[i].id); continue; }
        result.obligations.push_back(axes[i].role == Role::Derived
            ? "derived_observation_unimplemented:" + axes[i].id
            : axes[i].role == Role::Hypothesis
                ? "outcome_hypothesis_observation_unimplemented:" + axes[i].id
                : "input_construction_unimplemented:" + axes[i].id + "=" + candidate.values[i]);
    }
    if (canonical.lookup->size() != axes.size()) result.obligations.emplace_back("duplicate_model_axis");
    for (std::size_t i = 0; i < axes.size(); ++i)
        if (axes[i].role != Role::Derived && !(version == SelectionSchemaVersion::legacy_v1
            ? PrefixAllows(axes, canonical, axes[i].id, canonical.values[i])
            : PrefixAllowsV2(axes, canonical, axes[i].id, canonical.values[i])))
            result.obligations.push_back("candidate_prefix_structure_invalid:" + axes[i].id);
    const auto reason = version == SelectionSchemaVersion::legacy_v1
        ? StructuralExclusion(axes, canonical) : StructuralExclusionV2(axes, canonical);
    if (!reason.empty())
        result.obligations.push_back("candidate_structure_excluded:" + std::string(reason));
    const auto expectedProgram = drag_global::ProgramVersion(axes, canonical, version);
    bool programMatches = program.steps.size() == expectedProgram.steps.size() &&
        program.unresolved == expectedProgram.unresolved && SameProgramPostconditions(program, expectedProgram);
    if (programMatches)
        for (std::size_t i = 0; i < program.steps.size(); ++i)
            if (program.steps[i].kind != expectedProgram.steps[i].kind ||
                program.steps[i].parameter != expectedProgram.steps[i].parameter)
            { programMatches = false; break; }
    if (!programMatches) result.obligations.emplace_back("program_candidate_mismatch");
    if (Value(axes, canonical, "T01") != "external_ingress")
        result.obligations.emplace_back("ole_session_model_context_unimplemented");
    const auto allowed = EncodeAllowedEffect(Value(axes, canonical, "T03"),
        Value(axes, canonical, "T03.extra_protocol_bits"));
    if (!allowed) result.obligations.emplace_back("ole_allowed_effect_model_unimplemented");
    for (const auto& reason : program.unresolved)
        result.obligations.push_back("program_unresolved:" + reason);
    if (inputs.size() != program.steps.size()) result.obligations.emplace_back("program_step_input_count_mismatch");
    bool entered = false, terminated = false;
    for (std::size_t i = 0; i < program.steps.size(); ++i)
    {
        const auto& step = program.steps[i];
        const auto expectedKind = ProgramKind(step.kind);
        if (!expectedKind)
        {
            result.obligations.push_back("program_step_executor_unimplemented:" + step.kind);
            continue;
        }
        if (i >= inputs.size()) continue;
        const auto& input = inputs[i];
        if (input.programIndex != i || input.kind != *expectedKind || input.modelParameter != step.parameter)
            result.obligations.push_back("ole_step_binding_mismatch:" + std::to_string(i));
        if (terminated || (*expectedKind == Kind::enter && entered) ||
            (*expectedKind != Kind::enter && !entered))
            result.obligations.push_back("ole_session_sequence_invalid:" + std::to_string(i));
        if (*expectedKind == Kind::enter) entered = true;
        if (*expectedKind == Kind::drop || *expectedKind == Kind::leave) terminated = true;
        if (*expectedKind == Kind::leave)
        {
            if (input.keyState || input.allowedEffect || input.screenPoint)
                result.obligations.push_back("ole_leave_has_unexpected_arguments:" + std::to_string(i));
        }
        else if (!input.keyState || !input.allowedEffect || !input.screenPoint)
            result.obligations.push_back("ole_step_arguments_missing:" + std::to_string(i));
        if (*expectedKind != Kind::leave)
        {
            const auto parsed = ParseKeyParameter(step.parameter);
            const auto [buttonAxis, modifierAxis] = PhaseAxes(step.kind);
            const auto buttons = Value(axes, canonical, buttonAxis);
            const auto modifiers = Value(axes, canonical, modifierAxis);
            if (!parsed || parsed->first != buttons || parsed->second != modifiers)
                result.obligations.push_back("ole_model_phase_binding_mismatch:" + std::to_string(i));
            const auto encoded = EncodeKeyState(buttons, modifiers);
            if (!encoded.valid)
                result.obligations.push_back("ole_key_state_unimplemented:" + std::to_string(i));
            else
            {
                if (encoded.altX1Ambiguous)
                    result.obligations.push_back("ole_alt_x1_bit_ambiguity_unimplemented:" + std::to_string(i));
                if (input.keyState && *input.keyState != encoded.value)
                    result.obligations.push_back("ole_caller_key_state_mismatch:" + std::to_string(i));
            }
            if (input.allowedEffect && allowed && *input.allowedEffect != *allowed)
                result.obligations.push_back("ole_caller_allowed_effect_mismatch:" + std::to_string(i));
        }
    }
    if (!entered || !terminated) result.obligations.emplace_back("ole_session_not_fully_described");

    // Screen coordinates and other scene inputs still lack a candidate-bound
    // constructor. Completion waits and business oracles are also missing.
    result.obligations.emplace_back("model_coordinate_and_scene_binding_unimplemented");
    result.obligations.emplace_back("production_completion_and_business_oracle_unimplemented");
    return result;
}

class Sequence final
{
public:
    // No local event prefix can run while the rest of the generated program
    // lacks an executor. `attempted` never means business success or coverage.
    static Result Run(const std::vector<Axis>& axes, const Candidate& candidate,
        const EventProgram& program, const std::vector<Input>& inputs,
        OleDragDropAdapter* adapter, IDataObject* source)
    {
        Result result;
        auto inspection = Inspect(axes, candidate, program, inputs);
        result.obligations = std::move(inspection.obligations);
        if (!adapter) result.obligations.emplace_back("production_ole_adapter_missing");
        if (!source) result.obligations.emplace_back("ole_source_missing");
        if (!result.obligations.empty()) return result;

        Microsoft::WRL::ComPtr<OleDragDropAdapter> heldAdapter(adapter);
        Microsoft::WRL::ComPtr<IDataObject> heldSource(source);
        result.status = Status::attempted;
        for (const auto& input : inputs)
        {
            auto observed = Dispatch(*heldAdapter.Get(), heldSource.Get(), input);
            const bool failed = FAILED(observed.result);
            result.observations.push_back(std::move(observed));
            if (failed) { result.status = Status::event_failed; break; }
        }
        return result;
    }

private:
    friend class GraphExecutor;
    friend class V2GraphExecutor;
    static Observation Dispatch(OleDragDropAdapter& adapter, IDataObject* source, const Input& input)
    {
        Observation result;
        result.programIndex = input.programIndex;
        result.kind = input.kind;
        result.keyState = input.keyState;
        result.offeredEffect = input.allowedEffect;
        result.screenPoint = input.screenPoint;
        if (input.kind == Kind::leave)
        {
            result.result = adapter.DragLeave();
            return result;
        }
        DWORD effect = *input.allowedEffect;
        switch (input.kind)
        {
        case Kind::enter:
            result.result = adapter.DragEnter(source, *input.keyState, *input.screenPoint, &effect);
            break;
        case Kind::over:
            result.result = adapter.DragOver(*input.keyState, *input.screenPoint, &effect);
            break;
        case Kind::drop:
            result.result = adapter.Drop(source, *input.keyState, *input.screenPoint, &effect);
            break;
        case Kind::leave: break;
        }
        result.returnedEffect = effect;
        return result;
    }
};

// These boundaries must be implemented by the same isolated host that owns the
// adapter and source. They may wait for actual production signals or inspect
// actual business state; they must not perform a product decision, file
// operation, ownership mutation, placement or a fabricated completion.
struct HostBoundaries
{
    // Read-only observation of the existing production session immediately
    // before this call; it must not resolve or replace the target.
    std::function<void(const Input&)> beforeOleCall;
    // Called immediately after each real OLE call returns, before the next
    // generated event, including calls that return a failing HRESULT.
    std::function<void(const Observation&)> observeOleReturn;
    std::function<void(const Observation&)> awaitProductionCompletionOrRejection;
    std::function<void()> assertIndependentBusinessResults;
};

// A v2 active graph is a presence hypothesis, not an executable v1 Variant.
// This result records identity and input checks only. It has no execution
// status and cannot authorize calling the production adapter.
struct V2OlePreflight
{
    bool identityValid = false;
    std::vector<std::string> obligations;
    std::vector<std::string> notApplicable;
};

inline V2OlePreflight InspectV2OleGraph(const event_graph::Context& context,
    const Candidate& candidate, const EventProgram& program, const std::vector<bool>& present,
    const event_graph::PresenceActiveGraphVariant& selected, const std::vector<Input>& oleInputs,
    OleDragDropAdapter* adapter, IDataObject* source, const HostBoundaries& host)
{
    V2OlePreflight result;
    if (context.version != SelectionSchemaVersion::source_members_v2)
    { result.obligations.emplace_back("v2_ole_context_version_required"); return result; }
    const auto mapping = event_graph::MapSimpleExternal(context, candidate, program);
    if (!mapping.structuralExclusion.empty())
        result.obligations.push_back("candidate_structure_excluded:" + mapping.structuralExclusion);
    result.obligations.insert(result.obligations.end(), mapping.reasons.begin(), mapping.reasons.end());
    if (!mapping.graphAvailable)
    { result.obligations.emplace_back("v2_ole_candidate_graph_unavailable"); return result; }

    // MapSimpleExternal reconstructs the authoritative Spec from this exact
    // Context, candidate and Program; no caller-supplied Spec is trusted.
    const auto replay = event_graph::PresenceActiveGraph::Replay(context, candidate, mapping.spec, present, selected.id);
    result.obligations.insert(result.obligations.end(), replay.errors.begin(), replay.errors.end());
    if (!replay.structuralExclusion.empty())
        result.obligations.push_back("candidate_structure_excluded:" + replay.structuralExclusion);
    if (replay.identityValid && replay.errors.empty() && replay.structuralExclusion.empty() &&
        selected.id == replay.variant.id && selected.classes == replay.variant.classes &&
        selected.order == replay.variant.order && selected.unresolved == replay.variant.unresolved)
        result.identityValid = true;
    else result.obligations.emplace_back("v2_ole_selected_graph_identity_unavailable");

    // Presence positions use the normalized schema, not the original Spec's
    // vector order. Neither a selected absence nor a positive hypothesis is
    // an observation; retain each conditional proposition independently.
    const auto schema = event_graph::PreparePresenceGraph(context, candidate, mapping.spec);
    if (schema.errors.empty() && schema.structuralExclusion.empty() && present.size() == schema.requirements.size())
        for (std::size_t i = 0; i < present.size(); ++i)
            if (schema.requirements[i].presence == event_graph::PresencePolicy::conditional_unresolved)
                result.obligations.push_back(std::string(present[i] ? "v2_presence_unobserved_present:" :
                    "v2_presence_unobserved_absent:") + schema.requirements[i].id);
    for (const auto& reason : replay.variant.unresolved)
        result.obligations.push_back("selected_graph_unresolved:" + reason);

    std::vector<Input> expanded(program.steps.size());
    std::vector<bool> supplied(program.steps.size(), false);
    for (const auto& input : oleInputs)
    {
        if (input.programIndex >= program.steps.size() || supplied[input.programIndex] ||
            !ProgramKind(program.steps[input.programIndex].kind))
        { result.obligations.emplace_back("v2_ole_input_index_invalid_or_duplicate"); continue; }
        expanded[input.programIndex] = input;
        supplied[input.programIndex] = true;
    }
    for (std::size_t i = 0; i < program.steps.size(); ++i)
        if (ProgramKind(program.steps[i].kind) && !supplied[i])
            result.obligations.push_back("v2_ole_input_missing:" + std::to_string(i));
    auto inspection = Inspect(context.axes, candidate, program, expanded, context.version);
    result.obligations.insert(result.obligations.end(), inspection.obligations.begin(), inspection.obligations.end());
    result.notApplicable = std::move(inspection.notApplicable);
    if (!adapter) result.obligations.emplace_back("production_ole_adapter_missing");
    if (!source) result.obligations.emplace_back("ole_source_missing");
    if (!host.beforeOleCall) result.obligations.emplace_back("pre_ole_observation_missing");
    if (!host.observeOleReturn) result.obligations.emplace_back("immediate_ole_observation_missing");
    if (!host.awaitProductionCompletionOrRejection) result.obligations.emplace_back("production_completion_wait_missing");
    if (!host.assertIndependentBusinessResults) result.obligations.emplace_back("independent_business_oracle_missing");
    // Callback availability cannot prove any of these facts. In particular,
    // actual selected graph order, object identity, absence intervals and
    // async task attribution must be bound to the same production session.
    result.obligations.emplace_back("v2_ole_graph_order_and_session_dispatch_unimplemented");
    result.obligations.emplace_back("v2_ole_presence_and_absence_observation_unimplemented");
    result.obligations.emplace_back("v2_ole_source_fixture_and_host_identity_binding_unimplemented");
    result.obligations.emplace_back("v2_ole_production_completion_and_business_oracle_binding_unimplemented");
    return result;
}

enum class OlePlanKind { ole_call, completion_observation, business_observation };
enum class V2PreEvidenceState { proven, unimplemented, invalid, not_applicable };
enum class V2PostEvidenceState { pending, observed_match, hypothesis_not_met, unavailable, failed, not_applicable };
struct V2PreEvidence
{
    std::string id;
    V2PreEvidenceState state = V2PreEvidenceState::unimplemented;
    std::string evidence;
};
struct V2PostEvidence
{
    std::string id;
    V2PostEvidenceState state = V2PostEvidenceState::pending;
    std::string evidence;
};
struct V2StageEvidence
{
    // Model/graph binding is not a fixture, session, epoch or execution ID.
    std::string modelGraphBinding;
    std::vector<V2PreEvidence> pre;
    std::vector<V2PostEvidence> post;
};
struct OlePlanStep
{
    OlePlanKind kind;
    std::size_t programIndex;
    std::optional<Input> input;
    // Requirement IDs name actual call boundaries or an observation stage;
    // they are graph identity evidence, not completed production events.
    std::vector<std::string> requirements;
    std::vector<std::size_t> classes;
};
struct V2OleExecutionPlan
{
    V2OlePreflight preflight;
    V2StageEvidence evidence;
    bool structurallyPlanned = false;
    std::vector<OlePlanStep> steps;
    std::vector<std::string> obligations;
};

// This is a restricted, non-executing plan. Every graph class must name one
// distinct requirement and the selected order must exactly match synchronous
// call/return and host-stage boundaries. Coalescing, nested events, absence
// and other legal topologies remain explicit obligations, never filtered from
// the model or silently translated into a legacy graph.
inline V2OleExecutionPlan BuildV2OleExecutionPlan(const event_graph::Context& context,
    const Candidate& candidate, const EventProgram& program, const std::vector<bool>& present,
    const event_graph::PresenceActiveGraphVariant& selected, const std::vector<Input>& oleInputs,
    OleDragDropAdapter* adapter, IDataObject* source, const HostBoundaries& host)
{
    V2OleExecutionPlan result;
    result.preflight = InspectV2OleGraph(context, candidate, program, present, selected, oleInputs, adapter, source, host);
    result.obligations = result.preflight.obligations;
    if (!result.preflight.identityValid)
    {
        result.evidence.pre.push_back({"candidate_graph_identity", V2PreEvidenceState::unimplemented, "replay_unavailable"});
        result.obligations.emplace_back("v2_plan_graph_identity_required"); return result;
    }
    // The preflight has validated schema, full candidate, program and replay.
    // Do not use this symbolic binding to identify source, host or run.
    auto& binding = result.evidence.modelGraphBinding;
    event_graph::AppendToken(binding, "v2_ole_model_graph_evidence_v1");
    event_graph::AppendToken(binding, context.schemaBinding);
    event_graph::AppendToken(binding, FileSourceCandidateBinding(context.axes, candidate, context.version));
    event_graph::AppendToken(binding, selected.id);
    event_graph::AppendToken(binding, std::to_string(present.size()));
    for (const bool value : present) event_graph::AppendToken(binding, value ? "present" : "absent");
    result.evidence.pre.push_back({"candidate_graph_identity", V2PreEvidenceState::proven, "canonical_mapping_and_replay_only"});
    result.evidence.pre.push_back({"source_host_session_epoch", V2PreEvidenceState::unimplemented, "same_live_instance_binding_unobserved"});
    result.evidence.pre.push_back({"input_coordinates_and_scene", V2PreEvidenceState::unimplemented, "candidate_bound_real_coordinates_unavailable"});
    result.evidence.pre.push_back({"event_presence_observation_capability", V2PreEvidenceState::unimplemented, "bounded_complete_log_unavailable"});
    result.evidence.pre.push_back({"completion_and_independent_oracles", V2PreEvidenceState::unimplemented, "callback_presence_is_not_capability"});
    Candidate evidenceCandidate = candidate;
    evidenceCandidate.lookup = context.lookup;
    for (std::size_t i = 0; i < context.axes.size(); ++i)
    {
        const auto& axis = context.axes[i];
        const bool absentB = axis.id.substr(0, 7) == "task_b." &&
            Value(context.axes, evidenceCandidate, "X03.task_count") == "one" &&
            (axis.role == Role::Derived ? candidate.values[i].empty() : candidate.values[i] == "na");
        if (axis.role == Role::Hypothesis || axis.role == Role::Derived)
        {
            result.evidence.post.push_back({axis.id, absentB ? V2PostEvidenceState::not_applicable : V2PostEvidenceState::pending,
                absentB ? "task_b_structurally_absent" : "requires_same_execution_observation"});
        }
        else result.evidence.pre.push_back({axis.id, absentB ? V2PreEvidenceState::not_applicable : V2PreEvidenceState::unimplemented,
            absentB ? "task_b_structurally_absent" : "candidate_input_construction_unproven"});
    }
    result.evidence.post.push_back({"ole_return_and_independent_expected_result", V2PostEvidenceState::pending, "actual_hresult_and_effect_not_observed"});
    result.evidence.post.push_back({"production_completion", V2PostEvidenceState::pending, "same_task_completion_not_observed"});
    result.evidence.post.push_back({"business_result", V2PostEvidenceState::pending, "independent_oracle_not_run"});
    result.obligations.emplace_back("v2_stage_pre_dispatch_evidence_unimplemented");
    const auto mapping = event_graph::MapSimpleExternal(context, candidate, program);
    if (!mapping.graphAvailable)
    { result.obligations.emplace_back("v2_plan_authoritative_mapping_unavailable"); return result; }
    const auto schema = event_graph::PreparePresenceGraph(context, candidate, mapping.spec);
    if (!schema.errors.empty() || !schema.structuralExclusion.empty() || present.size() != schema.requirements.size())
    { result.obligations.emplace_back("v2_plan_presence_schema_unavailable"); return result; }
    bool absentConditional = false;
    for (std::size_t i = 0; i < present.size(); ++i)
    {
        if (schema.requirements[i].presence == event_graph::PresencePolicy::conditional_unresolved)
            result.evidence.post.push_back({"presence:" + schema.requirements[i].id, V2PostEvidenceState::pending,
                present[i] ? "expected_present_not_observed" : "expected_absent_not_observed"});
        if (!present[i] && schema.requirements[i].presence == event_graph::PresencePolicy::conditional_unresolved)
        { result.obligations.push_back("v2_plan_conditional_absence_unobserved:" + schema.requirements[i].id); absentConditional = true; }
    }
    if (absentConditional) return result;
    const auto projection = event_graph::ProjectPresenceHypothesis(context, candidate, mapping.spec, present);
    if (!projection.structurallyValid || !projection.errors.empty() || !projection.structuralExclusion.empty() ||
        selected.classes.size() != projection.requirements.size())
    { result.obligations.emplace_back("v2_plan_projection_unavailable"); return result; }

    std::map<std::string, std::size_t> indices;
    std::map<std::size_t, std::string> classOwners;
    for (std::size_t i = 0; i < projection.requirements.size(); ++i)
    {
        const auto& requirement = projection.requirements[i];
        if (!indices.emplace(requirement.id, i).second ||
            !classOwners.emplace(selected.classes[i], requirement.id).second)
        { result.obligations.emplace_back("v2_plan_merged_or_duplicate_graph_requirement_unimplemented"); return result; }
    }
    std::vector<std::string> expectedOrder;
    std::vector<OlePlanStep> steps;
    bool entered = false, terminated = false, completion = false, business = false;
    const auto bind = [&](OlePlanStep& plan, const std::string& id) {
        const auto found = indices.find(id);
        if (found == indices.end()) return false;
        plan.requirements.push_back(id);
        plan.classes.push_back(selected.classes[found->second]);
        expectedOrder.push_back(id);
        return true;
    };
    const auto lookup = BuildIndex(context.axes);
    Candidate canonical = candidate;
    canonical.lookup = lookup;
    const auto allowed = EncodeAllowedEffect(Value(context.axes, canonical, "T03"),
        Value(context.axes, canonical, "T03.extra_protocol_bits"));
    for (std::size_t i = 0; i < program.steps.size(); ++i)
    {
        const auto& step = program.steps[i];
        const auto kind = ProgramKind(step.kind);
        OlePlanStep plan{OlePlanKind::ole_call, i, std::nullopt, {}, {}};
        if (kind)
        {
            if (completion || business || terminated || (*kind == Kind::enter && entered) ||
                (*kind != Kind::enter && !entered))
            { result.obligations.push_back("v2_plan_ole_session_order_unimplemented:" + std::to_string(i)); return result; }
            if (*kind == Kind::enter) entered = true;
            if (*kind == Kind::drop || *kind == Kind::leave) terminated = true;
            const Input* input = nullptr;
            for (const auto& proposed : oleInputs)
                if (proposed.programIndex == i)
                { if (input) { result.obligations.emplace_back("v2_plan_duplicate_input"); return result; } input = &proposed; }
            if (!input || input->kind != *kind || input->modelParameter != step.parameter)
            { result.obligations.push_back("v2_plan_input_binding_unavailable:" + std::to_string(i)); return result; }
            if (*kind == Kind::leave)
            {
                if (input->keyState || input->allowedEffect || input->screenPoint)
                { result.obligations.emplace_back("v2_plan_leave_arguments_invalid"); return result; }
            }
            else
            {
                const auto parsed = ParseKeyParameter(step.parameter);
                const auto [buttonAxis, modifierAxis] = PhaseAxes(step.kind);
                const auto buttons = Value(context.axes, canonical, buttonAxis);
                const auto modifiers = Value(context.axes, canonical, modifierAxis);
                const auto encoded = EncodeKeyState(buttons, modifiers);
                if (!parsed || parsed->first != buttons || parsed->second != modifiers || !encoded.valid ||
                    encoded.altX1Ambiguous || !input->keyState || *input->keyState != encoded.value ||
                    !allowed || !input->allowedEffect || *input->allowedEffect != *allowed || !input->screenPoint)
                { result.obligations.push_back("v2_plan_input_values_unavailable:" + std::to_string(i)); return result; }
            }
            plan.input = *input;
            if (!bind(plan, "program_" + std::to_string(i) + "_call_start") ||
                !bind(plan, "program_" + std::to_string(i) + "_call_return"))
            { result.obligations.push_back("v2_plan_call_boundary_absent:" + std::to_string(i)); return result; }
        }
        else if (step.kind == "observe_completion_or_rejection" && terminated && !completion && !business)
        {
            completion = true; plan.kind = OlePlanKind::completion_observation;
            if (!bind(plan, "program_" + std::to_string(i) + "_observe"))
            { result.obligations.emplace_back("v2_plan_completion_observation_absent"); return result; }
        }
        else if (step.kind == "assert_business_results" && completion && !business)
        {
            business = true; plan.kind = OlePlanKind::business_observation;
            if (!bind(plan, "program_" + std::to_string(i) + "_observe"))
            { result.obligations.emplace_back("v2_plan_business_observation_absent"); return result; }
        }
        else { result.obligations.push_back("v2_plan_step_unimplemented:" + step.kind); return result; }
        steps.push_back(std::move(plan));
    }
    if (!entered || !terminated || !completion || !business || expectedOrder.size() != indices.size() ||
        selected.order != expectedOrder || oleInputs.size() != static_cast<std::size_t>(std::count_if(steps.begin(), steps.end(),
            [](const auto& step) { return step.kind == OlePlanKind::ole_call; })))
    { result.obligations.emplace_back("v2_plan_graph_order_extra_input_or_stage_unimplemented"); return result; }
    result.steps = std::move(steps);
    result.structurallyPlanned = true;
    result.obligations.emplace_back("v2_plan_is_not_production_execution_or_completion_evidence");
    return result;
}

// The sampler returns raw values only. A callback-provided snapshot is not a
// proof of host identity, causality, a matched hypothesis or a business result.
struct V2HostCoordinateSnapshot
{
    HWND window = nullptr;
    POINT sessionPoint{};
    std::uintptr_t target = 0;
    int region = 0;
    DWORD thread = 0;
    std::vector<std::string> unresolved;
};
struct V2CapturedOleReturn
{
    Observation observation;
    snowdesktop::ole_adapter_observation::Event entered, returned;
    std::optional<V2HostCoordinateSnapshot> hostSnapshot;
    // Only the synchronous Dispatch interval; gaps, asynchronous reads and
    // non-adapter reentrancy are not attributed by this observation.
    std::optional<ole_file_source_fixture::FileDropCallState> fileDropBefore, fileDropAfter;
    std::vector<ole_file_source_fixture::FileDropCallEvent> fileDropEvents;
    std::optional<snowdesktop::ole_adapter_observation::State> adapterLog;
};

// Rebuild the plan inside the executor. A caller-supplied plan or presence
// hypothesis cannot authorize execution. Today the unconditional unresolved
// obligations above make this path return before every production call.
class V2OleRunReceipt
{
public:
    V2OleRunReceipt(V2OleRunReceipt&&) = default;
    V2OleRunReceipt& operator=(V2OleRunReceipt&&) = default;
    V2OleRunReceipt(const V2OleRunReceipt&) = delete;
    V2OleRunReceipt& operator=(const V2OleRunReceipt&) = delete;
    const Result& Execution() const noexcept { return execution_; }
    const std::string& ModelGraphBinding() const noexcept { return modelGraphBinding_; }
    const std::vector<std::string>& Obligations() const noexcept { return obligations_; }
    const std::vector<V2CapturedOleReturn>& CapturedReturns() const noexcept { return capturedReturns_; }
    bool SameSource(const ole_file_source_fixture::Source& source) const noexcept
    { return source_.Get() == static_cast<const IDataObject*>(&source); }
    bool SameAdapter(const OleDragDropAdapter* adapter) const noexcept { return adapter_.Get() == adapter; }
private:
    friend class V2GraphExecutor;
    V2OleRunReceipt() = default;
    Result execution_;
    std::string modelGraphBinding_;
    std::vector<std::string> obligations_;
    std::vector<V2CapturedOleReturn> capturedReturns_;
    Microsoft::WRL::ComPtr<IDataObject> source_;
    Microsoft::WRL::ComPtr<OleDragDropAdapter> adapter_;
};

class V2GraphExecutor final
{
    friend class V3GraphExecutor;
    friend class V4GraphExecutor;
    friend class V4FileGraphExecutor;
    friend class V4MultiFileGraphExecutor;
    friend class V4DirectoryGraphExecutor;
    friend class V4MixedGraphExecutor;
    friend class V4ConfiguredContentGraphExecutor;
private:
    static Result RunInternal(const event_graph::Context& context, const Candidate& candidate,
        const EventProgram& program, const std::vector<bool>& present,
        const event_graph::PresenceActiveGraphVariant& selected, const std::vector<Input>& oleInputs,
        OleDragDropAdapter* adapter, IDataObject* source, const HostBoundaries& host,
        const std::function<void(const Observation&)>& assertIndependentOleResult,
        const std::function<V2HostCoordinateSnapshot(const Observation&)>* sampler,
        std::vector<V2CapturedOleReturn>* captured)
    {
        Result result;
        try
        {
            auto plan = BuildV2OleExecutionPlan(context, candidate, program, present, selected,
                oleInputs, adapter, source, host);
            result.obligations = std::move(plan.obligations);
            if (!plan.structurallyPlanned || !result.obligations.empty()) return result;
            return DispatchApprovedPlan(plan.steps, oleInputs.size(), adapter, source, host,
                assertIndependentOleResult, sampler, captured);
        }
        catch (const std::bad_alloc&)
        { result.status = Status::observation_unavailable; }
        catch (const std::exception& error)
        { result.status = Status::fixture_failed; result.failure = error.what(); }
        catch (...)
        { result.status = Status::fixture_failed; result.failure = "unknown_ole_plan_exception"; }
        return result;
    }

    // Private shared dispatch implementation. Callers must first build and
    // validate their own version-specific candidate and graph plan; accepting
    // these steps here never converts a V3 graph to a V2 graph identity.
    static Result DispatchApprovedPlan(const std::vector<OlePlanStep>& steps, std::size_t inputCount,
        OleDragDropAdapter* adapter, IDataObject* source, const HostBoundaries& host,
        const std::function<void(const Observation&)>& assertIndependentOleResult,
        const std::function<V2HostCoordinateSnapshot(const Observation&)>* sampler,
        std::vector<V2CapturedOleReturn>* captured,
        const ole_file_source_fixture::Source* fileSourceLog = nullptr)
    {
        Result result;
        try
        {
            if (!adapter || !source || !host.beforeOleCall || !host.observeOleReturn ||
                !host.awaitProductionCompletionOrRejection || !host.assertIndependentBusinessResults || !assertIndependentOleResult)
            { result.obligations.emplace_back("v2_executor_host_or_source_unavailable"); return result; }

            Microsoft::WRL::ComPtr<OleDragDropAdapter> heldAdapter(adapter);
            Microsoft::WRL::ComPtr<IDataObject> heldSource(source);
            result.observations.reserve(inputCount);
            result.hostStagesReturned.reserve(steps.size());
            result.failure.reserve(512);
            result.status = Status::attempted;
            for (const auto& step : steps)
            {
                if (step.kind == OlePlanKind::ole_call)
                {
                    if (!step.input) { result.status = Status::unimplemented; result.obligations.emplace_back("v2_executor_input_missing"); return result; }
                    try { host.beforeOleCall(*step.input); }
                    catch (const ObservationUnavailable& error)
                    { result.status = Status::observation_unavailable; result.failure = error.what(); return result; }
                    catch (const EventAssertionFailure& error)
                    { result.status = Status::event_oracle_failed; result.eventOracleFailureObserved = true; result.failure = error.what(); return result; }
                    catch (const std::bad_alloc& error)
                    { result.status = Status::observation_unavailable; result.failure = error.what(); return result; }
                    catch (const std::exception& error)
                    { result.status = Status::host_observation_failed; result.failure = error.what(); return result; }
                    catch (...)
                    { result.status = Status::host_observation_failed; result.failure = "unknown_pre_ole_observation_exception"; return result; }
                    namespace diagnostic = snowdesktop::ole_adapter_observation;
                    struct AdapterLogGuard
                    {
                        bool active = false;
                        ~AdapterLogGuard() { if (active) (void)diagnostic::End(); }
                        bool Finish() { const bool ok = diagnostic::End(); active = false; return ok; }
                    } guard;
                    if (!diagnostic::Begin())
                    { result.status = Status::observation_unavailable; result.failure = "v2_adapter_observation_begin_unavailable"; return result; }
                    guard.active = true;
                    const auto started = diagnostic::ReadState();
                    result.oleAdapterLogLast = started;
                    if (!started.active || started.failed || !started.session || started.ownerThread != GetCurrentThreadId() ||
                        started.depth != 0 || started.count != 0)
                    { result.status = Status::observation_unavailable; result.failure = "v2_adapter_observation_initial_state_invalid"; return result; }
                    ole_file_source_fixture::FileDropCallState fileDropBefore, fileDropAfter;
                    if (fileSourceLog && (!captured || static_cast<const IDataObject*>(fileSourceLog) != source ||
                        !fileSourceLog->ReadFileDropCallState(fileDropBefore) || fileDropBefore.failed ||
                        fileDropBefore.owner != GetCurrentThreadId() || fileDropBefore.depth != 0))
                    { result.status = Status::observation_unavailable; result.failure = "v3_filedrop_interval_start_unavailable"; return result; }
                    result.productionStarted = true;
                    Observation observed;
                    try { observed = Sequence::Dispatch(*heldAdapter.Get(), heldSource.Get(), *step.input); }
                    catch (const std::bad_alloc& error)
                    { result.oleAdapterLogLast = diagnostic::ReadState(); result.status = Status::observation_unavailable; result.failure = error.what(); return result; }
                    catch (const std::exception& error)
                    { result.oleAdapterLogLast = diagnostic::ReadState(); result.status = Status::event_failed; result.failure = error.what(); return result; }
                    catch (...)
                    { result.oleAdapterLogLast = diagnostic::ReadState(); result.status = Status::event_failed; result.failure = "unknown_ole_dispatch_exception"; return result; }
                    const bool failed = FAILED(observed.result);
                    result.observations.push_back(std::move(observed));
                    if (fileSourceLog && (!fileSourceLog->ReadFileDropCallState(fileDropAfter) || fileDropAfter.failed ||
                        fileDropAfter.owner != fileDropBefore.owner || fileDropAfter.depth != 0 ||
                        fileDropAfter.count < fileDropBefore.count))
                    { result.status = Status::observation_unavailable; result.failure = "v3_filedrop_interval_end_unavailable"; return result; }
                    const auto actual = diagnostic::ReadState();
                    result.oleAdapterLogLast = actual;
                    diagnostic::Event entered, returned;
                    const auto expectedKind = step.input->kind == Kind::enter ? diagnostic::Kind::enter :
                        step.input->kind == Kind::over ? diagnostic::Kind::over :
                        step.input->kind == Kind::drop ? diagnostic::Kind::drop : diagnostic::Kind::leave;
                    if (!actual.active || actual.failed || actual.session != started.session ||
                        actual.ownerThread != started.ownerThread || actual.depth != 0 || actual.count != 2 ||
                        !diagnostic::ReadEvent(0, entered) || !diagnostic::ReadEvent(1, returned) ||
                        entered.session != started.session || returned.session != started.session ||
                        entered.thread != started.ownerThread || returned.thread != started.ownerThread ||
                        entered.adapter != reinterpret_cast<std::uintptr_t>(adapter) || returned.adapter != entered.adapter ||
                        !entered.call || returned.call != entered.call || entered.parent || returned.parent ||
                        entered.depth || returned.depth || entered.kind != expectedKind || returned.kind != expectedKind ||
                        entered.phase != diagnostic::Phase::entered || returned.phase != diagnostic::Phase::returned ||
                        returned.result != result.observations.back().result)
                    { result.status = Status::observation_unavailable; result.failure = "v2_adapter_observation_nested_or_invalid"; return result; }
                    std::optional<V2HostCoordinateSnapshot> snapshot;
                    if (sampler && *sampler && step.input->kind == Kind::enter)
                    {
                        try { snapshot = (*sampler)(result.observations.back()); }
                        catch (const std::exception& error)
                        { result.status = Status::observation_unavailable; result.failure = error.what(); return result; }
                        catch (...)
                        { result.status = Status::observation_unavailable; result.failure = "v2_host_sample_unavailable"; return result; }
                        const auto afterSample = diagnostic::ReadState();
                        result.oleAdapterLogLast = afterSample;
                        if (!afterSample.active || afterSample.failed || afterSample.session != started.session ||
                            afterSample.ownerThread != started.ownerThread || afterSample.depth != 0 || afterSample.count != 2)
                        { result.status = Status::observation_unavailable; result.failure = "v2_adapter_reentry_during_host_sample"; return result; }
                        snapshot->unresolved.emplace_back("host_snapshot_callback_provenance_and_non_adapter_reentrancy_unverified");
                    }
                    const bool logFinished = guard.Finish();
                    result.oleAdapterLogLast = diagnostic::ReadState();
                    if (!logFinished)
                    { result.status = Status::observation_unavailable; result.failure = "v2_adapter_observation_end_unavailable"; return result; }
                    if (captured)
                    {
                        V2CapturedOleReturn record{result.observations.back(), entered, returned, std::move(snapshot), {}, {}, {}};
                        record.adapterLog = result.oleAdapterLogLast;
                        if (fileSourceLog)
                        {
                            record.fileDropBefore = fileDropBefore;
                            record.fileDropAfter = fileDropAfter;
                            for (std::size_t index = fileDropBefore.count; index < fileDropAfter.count; ++index)
                            {
                                ole_file_source_fixture::FileDropCallEvent event;
                                if (!fileSourceLog->ReadFileDropCallEvent(index, event))
                                { result.status = Status::observation_unavailable; result.failure = "v3_filedrop_interval_record_unavailable"; return result; }
                                record.fileDropEvents.push_back(event);
                            }
                        }
                        captured->push_back(std::move(record));
                    }
                    try { host.observeOleReturn(result.observations.back()); }
                    catch (const ObservationUnavailable& error)
                    {
                        result.status = Status::observation_unavailable;
                        result.failure = error.what();
                        return result;
                    }
                    catch (const EventAssertionFailure& error)
                    { result.status = Status::event_oracle_failed; result.eventOracleFailureObserved = true; result.failure = error.what(); return result; }
                    catch (const std::bad_alloc& error)
                    { result.status = Status::observation_unavailable; result.failure = error.what(); return result; }
                    catch (const std::exception& error)
                    { result.status = Status::host_observation_failed; result.failure = error.what(); return result; }
                    catch (...)
                    { result.status = Status::host_observation_failed; result.failure = "unknown_ole_observation_exception"; return result; }
                    // HRESULT is observed data, including expected refusal or
                    // injected failure. Only an independent candidate-bound
                    // oracle may judge it; an HRESULT alone is not a verdict.
                    try { assertIndependentOleResult(result.observations.back()); }
                    catch (const ObservationUnavailable& error)
                    { result.status = Status::observation_unavailable; result.failure = error.what(); return result; }
                    catch (const EventAssertionFailure& error)
                    { result.status = Status::event_oracle_failed; result.eventOracleFailureObserved = true; result.failure = error.what(); return result; }
                    catch (const BusinessAssertionFailure& error)
                    { result.status = Status::event_oracle_failed; result.eventOracleFailureObserved = true; result.failure = error.what(); return result; }
                    catch (const std::bad_alloc& error)
                    { result.status = Status::observation_unavailable; result.failure = error.what(); return result; }
                    catch (const std::exception& error)
                    { result.status = Status::host_observation_failed; result.failure = error.what(); return result; }
                    catch (...)
                    { result.status = Status::host_observation_failed; result.failure = "unknown_ole_result_oracle_exception"; return result; }
                    if (failed && step.input->kind != Kind::drop && step.input->kind != Kind::leave)
                    {
                        result.status = Status::unimplemented;
                        result.obligations.emplace_back("v2_executor_after_failed_nonterminal_call_unimplemented");
                        return result;
                    }
                }
                else
                {
                    if (step.kind == OlePlanKind::completion_observation &&
                        (result.observations.empty() ||
                         (result.observations.back().kind != Kind::drop && result.observations.back().kind != Kind::leave)))
                    { result.status = Status::unimplemented; result.obligations.emplace_back("v2_executor_terminal_observation_missing"); return result; }
                    if (step.kind != OlePlanKind::completion_observation && step.kind != OlePlanKind::business_observation)
                    { result.status = Status::unimplemented; result.obligations.emplace_back("v2_executor_stage_unavailable"); return result; }
                    try
                    {
                        if (step.kind == OlePlanKind::completion_observation)
                            host.awaitProductionCompletionOrRejection(result.observations.back());
                        else host.assertIndependentBusinessResults();
                    }
                    catch (const ObservationUnavailable& error)
                    { result.status = Status::observation_unavailable; result.failure = error.what(); return result; }
                    catch (const EventAssertionFailure& error)
                    {
                        result.status = step.kind == OlePlanKind::completion_observation
                            ? Status::event_oracle_failed : Status::observation_unavailable;
                        if (step.kind == OlePlanKind::completion_observation) result.eventOracleFailureObserved = true;
                        result.failure = error.what(); return result;
                    }
                    catch (const BusinessAssertionFailure& error)
                    {
                        result.status = step.kind == OlePlanKind::business_observation
                            ? Status::business_failed : Status::host_observation_failed;
                        if (step.kind == OlePlanKind::business_observation) result.businessFailureObserved = true;
                        result.failure = error.what(); return result;
                    }
                    catch (const std::bad_alloc& error)
                    { result.status = Status::observation_unavailable; result.failure = error.what(); return result; }
                    catch (const std::exception& error)
                    { result.status = Status::host_observation_failed; result.failure = error.what(); return result; }
                    catch (...)
                    { result.status = Status::host_observation_failed; result.failure = "unknown_host_stage_exception"; return result; }
                    result.hostStagesReturned.emplace_back(step.kind == OlePlanKind::completion_observation
                        ? "observe_completion_or_rejection" : "assert_business_results");
                }
            }
        }
        catch (const std::bad_alloc&)
        {
            result.additionalObservationUnavailable = true;
            if (!result.businessFailureObserved && !result.eventOracleFailureObserved && result.status != Status::event_failed)
                result.status = Status::observation_unavailable;
        }
        catch (const std::exception& error)
        {
            result.status = result.productionStarted ? Status::observation_unavailable : Status::fixture_failed;
            try { result.failure = std::string("v2_ole_executor_exception:") + error.what(); }
            catch (...) { result.failure = "v2_ole_executor_exception_detail_unavailable"; }
        }
        catch (...)
        { result.status = result.productionStarted ? Status::observation_unavailable : Status::fixture_failed; result.failure = "unknown_v2_ole_executor_exception"; }
        return result;
    }

public:
    static Result Run(const event_graph::Context& context, const Candidate& candidate,
        const EventProgram& program, const std::vector<bool>& present,
        const event_graph::PresenceActiveGraphVariant& selected, const std::vector<Input>& oleInputs,
        OleDragDropAdapter* adapter, IDataObject* source, const HostBoundaries& host,
        const std::function<void(const Observation&)>& assertIndependentOleResult)
    {
        return RunInternal(context, candidate, program, present, selected, oleInputs,
            adapter, source, host, assertIndependentOleResult, nullptr, nullptr);
    }

    // The private receipt can only contain observations produced by this
    // executor invocation, never an externally supplied Result. The source,
    // adapter and initial observation must remain on their owning STA; the
    // receipt retains COM references and must also be destroyed there.
    static V2OleRunReceipt RunBound(const event_graph::Context& context, const Candidate& candidate,
        const EventProgram& program, const std::vector<bool>& present,
        const event_graph::PresenceActiveGraphVariant& selected, const std::vector<Input>& oleInputs,
        const PayloadFixtureResult& fixture, const PayloadInputSnapshot& expected,
        const FileSourceInitialObservation& initial, ole_file_source_fixture::Source& source,
        OleDragDropAdapter* adapter, const HostBoundaries& host,
        const std::function<void(const Observation&)>& assertIndependentOleResult,
        const std::function<V2HostCoordinateSnapshot(const Observation&)>& sampler = {})
    {
        V2OleRunReceipt receipt;
        try
        {
            const auto initialCheck = ValidateSingleFileSourceInitial(context.axes, candidate, fixture, expected,
                source, initial, SelectionSchemaVersion::source_members_v2);
            receipt.obligations_ = initialCheck.obligations;
            if (!initialCheck.matches || !adapter)
            { receipt.obligations_.emplace_back("v2_receipt_initial_source_or_adapter_unavailable"); return receipt; }
            const auto before = BuildV2OleExecutionPlan(context, candidate, program, present, selected, oleInputs,
                adapter, static_cast<IDataObject*>(&source), host);
            if (!before.preflight.identityValid || before.evidence.modelGraphBinding.empty())
            { receipt.obligations_.emplace_back("v2_receipt_model_graph_unavailable"); return receipt; }
            receipt.modelGraphBinding_ = before.evidence.modelGraphBinding;
            receipt.source_ = static_cast<IDataObject*>(&source);
            receipt.adapter_ = adapter;
            receipt.execution_ = RunInternal(context, candidate, program, present, selected, oleInputs,
                adapter, static_cast<IDataObject*>(&source), host, assertIndependentOleResult,
                &sampler, &receipt.capturedReturns_);
            const auto after = BuildV2OleExecutionPlan(context, candidate, program, present, selected, oleInputs,
                adapter, static_cast<IDataObject*>(&source), host);
            if (!after.preflight.identityValid || after.evidence.modelGraphBinding != receipt.modelGraphBinding_)
            {
                receipt.modelGraphBinding_.clear();
                receipt.obligations_.emplace_back("v2_receipt_model_graph_changed_or_unavailable");
            }
            if (!receipt.execution_.productionStarted)
                receipt.obligations_.emplace_back("v2_receipt_no_production_dispatch_observed");
            receipt.obligations_.emplace_back("v2_receipt_host_epoch_and_reentrant_worker_attribution_unavailable");
            receipt.obligations_.emplace_back("v2_receipt_completion_and_independent_business_evidence_unavailable");
        }
        catch (const std::exception& error)
        {
            if (receipt.execution_.status == Status::attempted)
                receipt.execution_.status = Status::observation_unavailable;
            else if (!receipt.execution_.productionStarted && receipt.execution_.status == Status::unimplemented)
                receipt.execution_.status = Status::fixture_failed;
            receipt.obligations_.push_back(std::string("v2_receipt_construction_or_revalidation_unavailable:") + error.what());
        }
        catch (...)
        {
            if (receipt.execution_.status == Status::attempted)
                receipt.execution_.status = Status::observation_unavailable;
            else if (!receipt.execution_.productionStarted && receipt.execution_.status == Status::unimplemented)
                receipt.execution_.status = Status::fixture_failed;
            receipt.obligations_.emplace_back("v2_receipt_construction_or_revalidation_unavailable");
        }
        return receipt;
    }

};

struct V3OleExecutionPlan
{
    bool graphIdentityValid = false, structurallyPlanned = false;
    std::string rawCandidateId, graphId;
    std::vector<OlePlanStep> steps;
    std::vector<OutcomeHypothesis> postconditions;
    // Only missing execution capabilities and unimplemented input/graph
    // bindings belong here; postconditions remain expectations after a run.
    std::vector<std::string> preconditions;
    std::vector<std::string> postDispatchObligations, unclassifiedObligations;
};
struct V3PresenceExpectation
{
    std::string requirement;
    bool present = false; // A post-dispatch proposition, never an input.
};
struct V3JoinedOleExecutionPlan
{
    bool graphIdentityValid = false, structurallyPlanned = false;
    std::string rawCandidateId, graphId;
    std::vector<OlePlanStep> steps;
    std::vector<OutcomeHypothesis> postconditions;
    std::vector<V3PresenceExpectation> presence;
    // Graph facts include post-dispatch presence propositions; they are not
    // inputs and must not be required to have occurred before dispatch.
    std::vector<std::string> graphUnresolved;
    std::vector<std::string> preconditions;
    std::vector<std::string> postDispatchObligations, unclassifiedObligations;
};

inline V3OleExecutionPlan BuildV3OleExecutionPlan(const V3FamilyValidatedContext& verified,
    const event_graph::V3FamilyIdentityContext& identity, const event_graph::V3GraphVariant& selected,
    const std::vector<Input>& inputs)
{
    V3OleExecutionPlan result;
    try
    {
        const auto audit = InspectV3OleInputs(verified, identity, inputs);
        result.rawCandidateId = audit.rawCandidateId;
        result.postconditions = audit.postconditions;
        result.preconditions = audit.preDispatchBlockers;
        result.postDispatchObligations = audit.postDispatchObligations;
        result.unclassifiedObligations = audit.unclassifiedObligations;
        if (!result.unclassifiedObligations.empty())
            result.preconditions.emplace_back("v3_audit_obligation_stages_unclassified");
        // This graph omits conditional events and cannot authorize production.
        result.preconditions.emplace_back("v3_joined_presence_graph_required_for_production");
        if (!audit.candidateIdentityValid)
        { result.preconditions.emplace_back("v3_plan_candidate_identity_unavailable"); return result; }
        const auto program = verified.TransferProgram();
        const auto mapping = event_graph::MapV3FamilyCandidate(identity, verified, program);
        if (!mapping.graphAvailable)
        { result.preconditions.emplace_back("v3_plan_authoritative_graph_unavailable"); return result; }
        const auto replay = event_graph::V3Graph::Replay(identity, verified, program, selected.id);
        result.preconditions.insert(result.preconditions.end(), replay.errors.begin(), replay.errors.end());
        if (!replay.identityValid || !replay.errors.empty() || selected.id != replay.variant.id ||
            selected.classes != replay.variant.classes || selected.order != replay.variant.order ||
            selected.unresolved != replay.variant.unresolved)
        { result.preconditions.emplace_back("v3_plan_graph_identity_unavailable"); return result; }
        result.graphIdentityValid = true;
        result.graphId = selected.id;
        for (const auto& reason : replay.variant.unresolved)
            result.preconditions.push_back("v3_selected_graph_unresolved:" + reason);
        if (!mapping.spec.causalCoverageComplete)
            result.preconditions.emplace_back("v3_graph_causal_coverage_unavailable");
        if (!mapping.spec.presenceConstraints.empty())
        { result.preconditions.emplace_back("v3_graph_presence_constraints_unimplemented"); return result; }
        auto requirements = mapping.spec.requirements;
        std::sort(requirements.begin(), requirements.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        if (requirements.size() != selected.classes.size())
        { result.preconditions.emplace_back("v3_graph_requirement_count_mismatch"); return result; }
        std::map<std::string, std::size_t> indices;
        std::map<std::size_t, std::string> owners;
        for (std::size_t i = 0; i < requirements.size(); ++i)
        {
            if (requirements[i].presence != event_graph::PresencePolicy::required ||
                !requirements[i].presenceCondition.empty())
            { result.preconditions.push_back("v3_conditional_presence_unimplemented:" + requirements[i].id); return result; }
            if (!indices.emplace(requirements[i].id, i).second ||
                !owners.emplace(selected.classes[i], requirements[i].id).second)
            { result.preconditions.emplace_back("v3_merged_or_duplicate_graph_requirement_unimplemented"); return result; }
        }
        std::vector<std::string> order;
        std::vector<OlePlanStep> steps;
        const auto bind = [&](OlePlanStep& plan, const std::string& id) {
            const auto found = indices.find(id);
            if (found == indices.end()) return false;
            plan.requirements.push_back(id); plan.classes.push_back(selected.classes[found->second]);
            order.push_back(id); return true;
        };
        bool entered = false, terminated = false, completed = false, asserted = false;
        for (std::size_t i = 0; i < program.steps.size(); ++i)
        {
            const auto& step = program.steps[i];
            const auto kind = ProgramKind(step.kind);
            OlePlanStep plan{OlePlanKind::ole_call, i, std::nullopt, {}, {}};
            if (kind)
            {
                if (terminated || completed || asserted || (*kind == Kind::enter && entered) ||
                    (*kind != Kind::enter && !entered))
                { result.preconditions.push_back("v3_plan_session_order_unavailable:" + std::to_string(i)); return result; }
                if (*kind == Kind::enter) entered = true;
                if (*kind == Kind::drop || *kind == Kind::leave) terminated = true;
                const auto input = std::find_if(audit.phaseEncodingBindings.begin(), audit.phaseEncodingBindings.end(),
                    [&](const auto& item) { return item.programIndex == i; });
                if (input == audit.phaseEncodingBindings.end())
                { result.preconditions.push_back("v3_plan_input_unavailable:" + std::to_string(i)); return result; }
                plan.input = *input;
                if (!bind(plan, "program_" + std::to_string(i) + "_call_start") ||
                    !bind(plan, "program_" + std::to_string(i) + "_call_return"))
                { result.preconditions.push_back("v3_plan_call_boundary_unavailable:" + std::to_string(i)); return result; }
            }
            else if (step.kind == "observe_completion_or_rejection" && terminated && !completed && !asserted)
            {
                completed = true; plan.kind = OlePlanKind::completion_observation;
                if (!bind(plan, "program_" + std::to_string(i) + "_observe"))
                { result.preconditions.emplace_back("v3_plan_completion_boundary_unavailable"); return result; }
            }
            else if (step.kind == "assert_business_results" && completed && !asserted)
            {
                asserted = true; plan.kind = OlePlanKind::business_observation;
                if (!bind(plan, "program_" + std::to_string(i) + "_observe"))
                { result.preconditions.emplace_back("v3_plan_business_boundary_unavailable"); return result; }
            }
            else { result.preconditions.push_back("v3_plan_stage_unimplemented:" + step.kind); return result; }
            steps.push_back(std::move(plan));
        }
        if (!entered || !terminated || !completed || !asserted || order.size() != indices.size() ||
            selected.order != order || inputs.size() != audit.phaseEncodingBindings.size())
        { result.preconditions.emplace_back("v3_plan_graph_order_or_input_unavailable"); return result; }
        result.steps = std::move(steps);
        result.structurallyPlanned = true;
        result.preconditions.emplace_back("v3_plan_conditional_presence_and_background_causality_unimplemented");
        result.preconditions.emplace_back("v3_plan_same_host_source_completion_and_oracle_capabilities_unimplemented");
    }
    catch (const std::bad_alloc&) { result.preconditions.emplace_back("v3_plan_resource_unavailable"); }
    catch (const std::length_error&) { result.preconditions.emplace_back("v3_plan_resource_unavailable"); }
    return result;
}

inline V3JoinedOleExecutionPlan BuildV3JoinedOleExecutionPlan(const V3FamilyValidatedContext& verified,
    const event_graph::V3FamilyIdentityContext& identity, const std::vector<bool>& present,
    const event_graph::V3JoinedGraphVariant& selected, const std::vector<Input>& inputs)
{
    V3JoinedOleExecutionPlan result;
    try
    {
        const auto audit = InspectV3OleInputs(verified, identity, inputs);
        result.rawCandidateId = audit.rawCandidateId;
        result.postconditions = audit.postconditions;
        result.preconditions = audit.preDispatchBlockers;
        result.postDispatchObligations = audit.postDispatchObligations;
        result.unclassifiedObligations = audit.unclassifiedObligations;
        if (!result.unclassifiedObligations.empty())
            result.preconditions.emplace_back("v3_audit_obligation_stages_unclassified");
        if (!audit.candidateIdentityValid)
        { result.preconditions.emplace_back("v3_joined_plan_candidate_unavailable"); return result; }
        const auto program = verified.TransferProgram();
        const auto mapping = event_graph::MapV3PresenceCandidate(identity, verified, program);
        if (!mapping.graphAvailable)
        { result.preconditions.emplace_back("v3_joined_plan_graph_unavailable"); return result; }
        const auto replay = event_graph::V3JoinedGraph::Replay(identity, verified, program, present, selected.id);
        result.preconditions.insert(result.preconditions.end(), replay.errors.begin(), replay.errors.end());
        if (!replay.identityValid || !replay.errors.empty() || selected.id != replay.variant.id ||
            selected.classes != replay.variant.classes || selected.order != replay.variant.order ||
            selected.unresolved != replay.variant.unresolved)
        { result.preconditions.emplace_back("v3_joined_plan_identity_unavailable"); return result; }
        result.graphIdentityValid = true;
        result.graphId = selected.id;
        result.graphUnresolved = replay.variant.unresolved;
        if (!result.graphUnresolved.empty())
            result.preconditions.emplace_back("v3_joined_graph_observation_capabilities_unimplemented");
        if (!mapping.spec.causalCoverageComplete)
            result.preconditions.emplace_back("v3_joined_causal_coverage_unavailable");
        auto requirements = mapping.spec.requirements;
        std::sort(requirements.begin(), requirements.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        if (present.size() != requirements.size())
        { result.preconditions.emplace_back("v3_joined_presence_shape_invalid"); return result; }
        std::vector<event_graph::Requirement> active;
        for (std::size_t i = 0; i < requirements.size(); ++i)
        {
            const auto& requirement = requirements[i];
            if (requirement.presence == event_graph::PresencePolicy::conditional_unresolved)
            {
                result.presence.push_back({requirement.id, present[i]});
                result.postDispatchObligations.push_back("v3_presence_observation_pending:" + requirement.id);
            }
            if (present[i]) active.push_back(requirement);
        }
        if (active.size() != selected.classes.size())
        { result.preconditions.emplace_back("v3_joined_active_requirement_count_mismatch"); return result; }
        std::map<std::string, std::size_t> indices;
        std::map<std::size_t, std::string> owners;
        for (std::size_t i = 0; i < active.size(); ++i)
            if (!indices.emplace(active[i].id, i).second || !owners.emplace(selected.classes[i], active[i].id).second)
            { result.preconditions.emplace_back("v3_joined_merged_or_duplicate_requirement_unimplemented"); return result; }
        std::vector<std::string> callOrder;
        for (const auto& id : selected.order)
        {
            const auto found = indices.find(id);
            if (found == indices.end())
            { result.preconditions.emplace_back("v3_joined_order_requirement_unavailable"); return result; }
            if (active[found->second].presence == event_graph::PresencePolicy::required) callOrder.push_back(id);
        }
        std::vector<std::string> expectedOrder;
        std::vector<OlePlanStep> steps;
        const auto bind = [&](OlePlanStep& plan, const std::string& id) {
            const auto found = indices.find(id);
            if (found == indices.end() || active[found->second].presence != event_graph::PresencePolicy::required) return false;
            plan.requirements.push_back(id); plan.classes.push_back(selected.classes[found->second]);
            expectedOrder.push_back(id); return true;
        };
        bool entered = false, terminated = false, completed = false, asserted = false;
        for (std::size_t i = 0; i < program.steps.size(); ++i)
        {
            const auto& step = program.steps[i]; const auto kind = ProgramKind(step.kind);
            OlePlanStep plan{OlePlanKind::ole_call, i, std::nullopt, {}, {}};
            if (kind)
            {
                if (terminated || completed || asserted || (*kind == Kind::enter && entered) ||
                    (*kind != Kind::enter && !entered))
                { result.preconditions.push_back("v3_joined_session_order_unavailable:" + std::to_string(i)); return result; }
                if (*kind == Kind::enter) entered = true;
                if (*kind == Kind::drop || *kind == Kind::leave) terminated = true;
                const auto input = std::find_if(audit.phaseEncodingBindings.begin(), audit.phaseEncodingBindings.end(),
                    [&](const auto& item) { return item.programIndex == i; });
                if (input == audit.phaseEncodingBindings.end())
                { result.preconditions.push_back("v3_joined_input_unavailable:" + std::to_string(i)); return result; }
                plan.input = *input;
                if (!bind(plan, "program_" + std::to_string(i) + "_call_start") ||
                    !bind(plan, "program_" + std::to_string(i) + "_call_return"))
                { result.preconditions.push_back("v3_joined_call_boundary_unavailable:" + std::to_string(i)); return result; }
            }
            else if (step.kind == "observe_completion_or_rejection" && terminated && !completed && !asserted)
            {
                completed = true; plan.kind = OlePlanKind::completion_observation;
                if (!bind(plan, "program_" + std::to_string(i) + "_observe"))
                { result.preconditions.emplace_back("v3_joined_completion_boundary_unavailable"); return result; }
            }
            else if (step.kind == "assert_business_results" && completed && !asserted)
            {
                asserted = true; plan.kind = OlePlanKind::business_observation;
                if (!bind(plan, "program_" + std::to_string(i) + "_observe"))
                { result.preconditions.emplace_back("v3_joined_business_boundary_unavailable"); return result; }
            }
            else { result.preconditions.push_back("v3_joined_stage_unimplemented:" + step.kind); return result; }
            steps.push_back(std::move(plan));
        }
        if (!entered || !terminated || !completed || !asserted || callOrder != expectedOrder ||
            inputs.size() != audit.phaseEncodingBindings.size())
        { result.preconditions.emplace_back("v3_joined_call_order_or_input_unavailable"); return result; }
        result.steps = std::move(steps);
        result.structurallyPlanned = true;
        // Conditional nodes are observed, never dispatched by the fixture;
        // exact interval/order and absence capability are not yet bound.
        result.preconditions.emplace_back("v3_joined_presence_interval_and_order_observer_unimplemented");
        result.preconditions.emplace_back("v3_joined_background_events_and_causality_unimplemented");
        result.preconditions.emplace_back("v3_joined_same_host_source_completion_and_oracle_capabilities_unimplemented");
    }
    catch (const std::bad_alloc&) { result.preconditions.emplace_back("v3_joined_plan_resource_unavailable"); }
    catch (const std::length_error&) { result.preconditions.emplace_back("v3_joined_plan_resource_unavailable"); }
    return result;
}

// The source-log interval is sealed by the executor across its own dispatch
// and host wait. A returned wait callback is not by itself proof of task
// completion, producer quiescence, provenance, or global absence.
struct V3SourceSessionInterval
{
    ole_file_source_fixture::FileDropCallState before{}, after{};
    std::vector<ole_file_source_fixture::FileDropCallEvent> events;
};

class V3OleRunReceipt
{
public:
    V3OleRunReceipt(V3OleRunReceipt&&) = default;
    V3OleRunReceipt& operator=(V3OleRunReceipt&&) = default;
    V3OleRunReceipt(const V3OleRunReceipt&) = delete;
    V3OleRunReceipt& operator=(const V3OleRunReceipt&) = delete;
    const Result& Execution() const noexcept { return execution_; }
    const std::vector<V2CapturedOleReturn>& CapturedReturns() const noexcept { return captured_; }
    const std::vector<OutcomeHypothesis>& Postconditions() const noexcept { return postconditions_; }
    const std::vector<V3PresenceExpectation>& PresenceExpectations() const noexcept { return presence_; }
    const std::optional<V3SourceSessionInterval>& SourceSessionInterval() const noexcept { return sourceSessionInterval_; }
    const std::vector<std::string>& GraphUnresolved() const noexcept { return graphUnresolved_; }
    const std::vector<std::string>& Obligations() const noexcept { return obligations_; }
    const std::vector<std::string>& PostDispatchObligations() const noexcept { return postDispatchObligations_; }
    const std::vector<std::string>& UnclassifiedObligations() const noexcept { return unclassifiedObligations_; }
    const std::string& GraphId() const noexcept { return graphId_; }
    bool JoinedIdentity() const noexcept { return joinedIdentity_; }
    bool SameSource(const ole_file_source_fixture::Source& source) const noexcept
    { return hostEpoch_ && source_ == static_cast<const IDataObject*>(&source) &&
        hostEpoch_->Matches(source_, adapter_); }
    bool SameAdapterIdentity(std::uintptr_t adapter) const noexcept
    { return hostEpoch_ && hostEpoch_->Matches(source_, adapter_) && adapter &&
        reinterpret_cast<std::uintptr_t>(adapter_) == adapter; }
    bool SameCandidate(const V3FamilyValidatedContext& context) const
    {
        return context.Status() == V3CoordinateStatus::coordinate_ready && !candidateBinding_.empty() &&
            candidateBinding_ == FileSourceCandidateBinding(context.Axes(), context.Coordinate(),
                SelectionSchemaVersion::source_members_v2, &context);
    }
    bool SameHostEpoch(const MessageTrace& trace, HWND control, const ole_file_source_fixture::Source& source) const noexcept
    {
        return hostEpoch_ && source_ == static_cast<const IDataObject*>(&source) &&
            hostEpoch_->Matches(source_, adapter_, &trace, control);
    }
private:
    friend class V3GraphExecutor;
    V3OleRunReceipt() = default;
    Result execution_;
    std::string graphId_, candidateBinding_;
    std::vector<V2CapturedOleReturn> captured_;
    std::vector<OutcomeHypothesis> postconditions_;
    std::vector<V3PresenceExpectation> presence_;
    std::optional<V3SourceSessionInterval> sourceSessionInterval_;
    std::vector<std::string> graphUnresolved_;
    std::vector<std::string> obligations_;
    std::vector<std::string> postDispatchObligations_, unclassifiedObligations_;
    bool joinedIdentity_ = false;
    // Numeric identities are only usable while the epoch owns the actual
    // COM references on the fixture STA. Never dereference these borrowed
    // pointers after invalidation or retain apartment objects in the receipt.
    IDataObject* source_ = nullptr;
    OleDragDropAdapter* adapter_ = nullptr;
    std::shared_ptr<const V3HostEpoch> hostEpoch_;
};

class V3GraphExecutor final
{
public:
    // The receipt, source and adapter remain on their owning STA. No caller
    // supplies a plan or observed outcome; this method reconstructs both the
    // graph plan and the source baseline before considering a production call.
    static V3OleRunReceipt RunBound(const V3FamilyValidatedContext& verified,
        const event_graph::V3FamilyIdentityContext& identity, const event_graph::V3GraphVariant& selected,
        const std::vector<Input>& inputs, const PayloadFixtureResult& fixture, const PayloadInputSnapshot& expected,
        const FileSourceInitialObservation& initial, ole_file_source_fixture::Source& source,
        OleDragDropAdapter* adapter, const HostBoundaries& host,
        const std::function<void(const Observation&)>& assertIndependentOleResult,
        const std::function<V2HostCoordinateSnapshot(const Observation&)>& sampler = {})
    {
        V3OleRunReceipt receipt;
        try
        {
            auto plan = BuildV3OleExecutionPlan(verified, identity, selected, inputs);
            receipt.graphId_ = plan.graphId;
            if (plan.graphIdentityValid)
                receipt.candidateBinding_ = FileSourceCandidateBinding(verified.Axes(), verified.Coordinate(),
                    SelectionSchemaVersion::source_members_v2, &verified);
            receipt.postconditions_ = plan.postconditions;
            receipt.obligations_ = plan.preconditions;
            receipt.postDispatchObligations_ = plan.postDispatchObligations;
            receipt.unclassifiedObligations_ = plan.unclassifiedObligations;
            const auto initialCheck = ValidateV3SingleFileSourceInitial(verified, fixture, expected, source, initial);
            receipt.obligations_.insert(receipt.obligations_.end(), initialCheck.obligations.begin(), initialCheck.obligations.end());
            if (!initialCheck.matches) receipt.obligations_.emplace_back("v3_receipt_source_baseline_unavailable");
            if (!adapter) receipt.obligations_.emplace_back("v3_receipt_adapter_unavailable");
            if (!plan.graphIdentityValid || !plan.structurallyPlanned || !receipt.obligations_.empty()) return receipt;
            receipt.source_ = static_cast<IDataObject*>(&source);
            receipt.adapter_ = adapter;
            receipt.execution_ = V2GraphExecutor::DispatchApprovedPlan(plan.steps, inputs.size(), adapter,
                static_cast<IDataObject*>(&source), host, assertIndependentOleResult, &sampler, &receipt.captured_);
            // A future postflight must revalidate this graph and bind a real
            // host epoch/completion before comparing outcome hypotheses.
            const auto replay = event_graph::V3Graph::Replay(identity, verified, verified.TransferProgram(), selected.id);
            if (!replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
                replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
                replay.variant.unresolved != selected.unresolved)
            {
                receipt.execution_.additionalObservationUnavailable = true;
                receipt.obligations_.emplace_back("v3_receipt_graph_revalidation_unavailable");
            }
            receipt.obligations_.emplace_back("v3_receipt_host_epoch_completion_and_business_evidence_unavailable");
        }
        catch (const std::bad_alloc&)
        {
            receipt.execution_.additionalObservationUnavailable = true;
            if (receipt.execution_.status == Status::attempted || receipt.execution_.status == Status::unimplemented)
                receipt.execution_.status = Status::observation_unavailable;
        }
        catch (const std::exception& error)
        {
            receipt.execution_.additionalObservationUnavailable = true;
            if (receipt.execution_.status == Status::attempted) receipt.execution_.status = Status::observation_unavailable;
            else if (!receipt.execution_.productionStarted && receipt.execution_.status == Status::unimplemented)
                receipt.execution_.status = Status::fixture_failed;
            receipt.obligations_.push_back(std::string("v3_receipt_exception:") + error.what());
        }
        catch (...)
        {
            receipt.execution_.additionalObservationUnavailable = true;
            if (receipt.execution_.status == Status::attempted) receipt.execution_.status = Status::observation_unavailable;
            else if (!receipt.execution_.productionStarted && receipt.execution_.status == Status::unimplemented)
                receipt.execution_.status = Status::fixture_failed;
            receipt.obligations_.emplace_back("unknown_v3_receipt_exception");
        }
        return receipt;
    }

    static V3OleRunReceipt RunBound(const V3FamilyValidatedContext& verified,
        const event_graph::V3FamilyIdentityContext& identity, const std::vector<bool>& present,
        const event_graph::V3JoinedGraphVariant& selected, const std::vector<Input>& inputs,
        const PayloadFixtureResult& fixture, const PayloadInputSnapshot& expected,
        const FileSourceInitialObservation& initial, ole_file_source_fixture::Source& source,
        OleDragDropAdapter* adapter, const HostBoundaries& host,
        const std::function<void(const Observation&)>& assertIndependentOleResult,
        const std::function<V2HostCoordinateSnapshot(const Observation&)>& sampler = {},
        std::shared_ptr<const V3HostEpoch> hostEpoch = {})
    {
        V3OleRunReceipt receipt;
        try
        {
            auto plan = BuildV3JoinedOleExecutionPlan(verified, identity, present, selected, inputs);
            receipt.graphId_ = plan.graphId;
            receipt.joinedIdentity_ = plan.graphIdentityValid;
            if (plan.graphIdentityValid)
                receipt.candidateBinding_ = FileSourceCandidateBinding(verified.Axes(), verified.Coordinate(),
                    SelectionSchemaVersion::source_members_v2, &verified);
            receipt.postconditions_ = plan.postconditions;
            receipt.presence_ = plan.presence;
            receipt.graphUnresolved_ = plan.graphUnresolved;
            receipt.obligations_ = plan.preconditions;
            receipt.postDispatchObligations_ = plan.postDispatchObligations;
            receipt.unclassifiedObligations_ = plan.unclassifiedObligations;
            const auto initialCheck = ValidateV3SingleFileSourceInitial(verified, fixture, expected, source, initial);
            receipt.obligations_.insert(receipt.obligations_.end(), initialCheck.obligations.begin(), initialCheck.obligations.end());
            if (!initialCheck.matches) receipt.obligations_.emplace_back("v3_receipt_source_baseline_unavailable");
            if (!adapter) receipt.obligations_.emplace_back("v3_receipt_adapter_unavailable");
            if (!hostEpoch || !hostEpoch->Matches(static_cast<IDataObject*>(&source), adapter))
                receipt.obligations_.emplace_back("v3_receipt_host_epoch_unavailable");
            if (!host.beforeOleCall || !host.observeOleReturn || !host.awaitProductionCompletionOrRejection ||
                !host.assertIndependentBusinessResults || !assertIndependentOleResult)
                receipt.obligations_.emplace_back("v3_receipt_host_observer_or_oracle_missing");
            if (!plan.graphIdentityValid || !plan.structurallyPlanned || !receipt.obligations_.empty()) return receipt;
            if (!hostEpoch->TryBeginExecution(static_cast<IDataObject*>(&source), adapter))
            {
                receipt.execution_.additionalObservationUnavailable = true;
                receipt.obligations_.emplace_back("v3_receipt_host_epoch_busy_or_unavailable");
                return receipt;
            }
            const auto endExecution = [](const V3HostEpoch* epoch) noexcept { epoch->EndExecution(); };
            const std::unique_ptr<const V3HostEpoch, decltype(endExecution)> executionGuard(hostEpoch.get(), endExecution);
            receipt.source_ = static_cast<IDataObject*>(&source);
            receipt.adapter_ = adapter;
            receipt.hostEpoch_ = hostEpoch;
            const auto requireEpoch = [&] {
                if (!hostEpoch->Matches(static_cast<IDataObject*>(&source), adapter))
                    throw ObservationUnavailable("v3_host_epoch_changed_or_unavailable");
            };
            ole_file_source_fixture::FileDropCallState sessionStart;
            requireEpoch();
            if (!source.ReadFileDropCallState(sessionStart) || sessionStart.failed || sessionStart.depth ||
                sessionStart.owner != GetCurrentThreadId())
                throw ObservationUnavailable("v3_source_session_start_unavailable");
            const HostBoundaries epochHost{
                [&](const Input& input) { requireEpoch(); host.beforeOleCall(input); requireEpoch(); },
                [&](const Observation& event) { requireEpoch(); host.observeOleReturn(event); requireEpoch(); },
                [&](const Observation& terminal) {
                    requireEpoch();
                    host.awaitProductionCompletionOrRejection(terminal);
                    requireEpoch();
                    V3SourceSessionInterval interval;
                    interval.before = sessionStart;
                    if (!source.ReadFileDropCallState(interval.after) || interval.after.failed || interval.after.depth ||
                        interval.after.owner != sessionStart.owner || interval.after.count < sessionStart.count)
                        throw ObservationUnavailable("v3_source_session_end_unavailable");
                    interval.events.reserve(interval.after.count - interval.before.count);
                    for (std::size_t index = interval.before.count; index < interval.after.count; ++index)
                    {
                        ole_file_source_fixture::FileDropCallEvent event;
                        if (!source.ReadFileDropCallEvent(index, event))
                            throw ObservationUnavailable("v3_source_session_record_unavailable");
                        interval.events.push_back(event);
                    }
                    ole_file_source_fixture::FileDropCallState confirmed;
                    if (!source.ReadFileDropCallState(confirmed) || confirmed.failed || confirmed.depth ||
                        confirmed.owner != interval.after.owner || confirmed.count != interval.after.count)
                        throw ObservationUnavailable("v3_source_session_changed_during_capture");
                    requireEpoch();
                    receipt.sourceSessionInterval_ = std::move(interval);
                },
                [&] { requireEpoch(); host.assertIndependentBusinessResults(); requireEpoch(); }};
            const std::function<V2HostCoordinateSnapshot(const Observation&)> epochSampler = sampler
                ? std::function<V2HostCoordinateSnapshot(const Observation&)>{[&](const Observation& event) {
                    requireEpoch(); auto sample = sampler(event); requireEpoch(); return sample;
                }} : std::function<V2HostCoordinateSnapshot(const Observation&)>{};
            const std::function<void(const Observation&)> epochOracle = [&](const Observation& event) {
                requireEpoch(); assertIndependentOleResult(event); requireEpoch();
            };
            receipt.execution_ = V2GraphExecutor::DispatchApprovedPlan(plan.steps, inputs.size(), adapter,
                static_cast<IDataObject*>(&source), epochHost, epochOracle, &epochSampler, &receipt.captured_, &source);
            if (!hostEpoch->Matches(static_cast<IDataObject*>(&source), adapter))
            {
                receipt.execution_.additionalObservationUnavailable = true;
                receipt.obligations_.emplace_back("v3_receipt_host_epoch_changed_or_unavailable");
            }
            const auto replay = event_graph::V3JoinedGraph::Replay(identity, verified, verified.TransferProgram(), present, selected.id);
            if (!replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
                replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
                replay.variant.unresolved != selected.unresolved)
            {
                receipt.joinedIdentity_ = false;
                receipt.execution_.additionalObservationUnavailable = true;
                receipt.obligations_.emplace_back("v3_receipt_graph_revalidation_unavailable");
            }
            receipt.obligations_.emplace_back("v3_receipt_presence_host_epoch_completion_and_business_evidence_unavailable");
        }
        catch (const std::bad_alloc&)
        {
            receipt.joinedIdentity_ = false;
            receipt.execution_.additionalObservationUnavailable = true;
            if (receipt.execution_.status == Status::attempted || receipt.execution_.status == Status::unimplemented)
                receipt.execution_.status = Status::observation_unavailable;
        }
        catch (const std::exception& error)
        {
            receipt.joinedIdentity_ = false;
            receipt.execution_.additionalObservationUnavailable = true;
            if (receipt.execution_.status == Status::attempted) receipt.execution_.status = Status::observation_unavailable;
            else if (!receipt.execution_.productionStarted && receipt.execution_.status == Status::unimplemented)
                receipt.execution_.status = Status::fixture_failed;
            receipt.obligations_.push_back(std::string("v3_joined_receipt_exception:") + error.what());
        }
        catch (...)
        {
            receipt.joinedIdentity_ = false;
            receipt.execution_.additionalObservationUnavailable = true;
            if (receipt.execution_.status == Status::attempted) receipt.execution_.status = Status::observation_unavailable;
            else if (!receipt.execution_.productionStarted && receipt.execution_.status == Status::unimplemented)
                receipt.execution_.status = Status::fixture_failed;
            receipt.obligations_.emplace_back("unknown_v3_joined_receipt_exception");
        }
        return receipt;
    }
};

struct V4PresenceExpectation { std::string requirement; bool present = false; };
struct V4JoinedOleExecutionPlan
{
    bool graphIdentityValid = false, structurallyPlanned = false;
    std::string candidateId, graphId;
    std::vector<OlePlanStep> steps;
    std::vector<OutcomeHypothesis> postconditions;
    std::vector<V4PresenceExpectation> presence;
    std::vector<std::string> preconditions, unclassified, graphUnresolved, postDispatchObligations;
};

// Pure planning against the actual V4 joined identity. Presence is an
// observation hypothesis, never an input or proof that an event occurred.
inline V4JoinedOleExecutionPlan BuildV4JoinedOleExecutionPlan(const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const std::vector<bool>& present,
    const event_graph::V4JoinedGraphVariant& selected, const std::vector<Input>& inputs)
{
    V4JoinedOleExecutionPlan result;
    try
    {
        const auto mapping = event_graph::MapV4PresenceCandidate(identity, verified);
        result.unclassified = mapping.reasons;
        if (!mapping.graphAvailable)
        { result.preconditions.emplace_back("v4_joined_mapping_unavailable"); return result; }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty())
        { result.preconditions.emplace_back("v4_joined_candidate_unavailable"); return result; }
        result.candidateId = raw.id;
        const auto program = verified.TransferProgram();
        result.postconditions = program.postconditions;
        for (const auto& condition : program.postconditions)
            result.postDispatchObligations.push_back("v4_outcome_observation_pending:" + condition.axis);
        // Program reasons are untyped: retain them without assuming that a
        // future observation is itself a pre-dispatch input requirement.
        result.unclassified.insert(result.unclassified.end(), program.unresolved.begin(), program.unresolved.end());
        const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
        if (!replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
            replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
            replay.variant.unresolved != selected.unresolved)
        { result.preconditions.emplace_back("v4_joined_graph_identity_unavailable"); return result; }
        result.graphIdentityValid = true; result.graphId = selected.id; result.graphUnresolved = selected.unresolved;
        if (!result.unclassified.empty()) result.preconditions.emplace_back("v4_mapping_obligation_stages_unclassified");
        if (!mapping.spec.causalCoverageComplete) result.preconditions.emplace_back("v4_complete_event_causality_unavailable");
        auto requirements = mapping.spec.requirements;
        std::sort(requirements.begin(), requirements.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        if (present.size() != requirements.size())
        { result.preconditions.emplace_back("v4_joined_presence_shape_invalid"); return result; }
        std::vector<event_graph::Requirement> active;
        for (std::size_t i = 0; i < requirements.size(); ++i)
        {
            if (requirements[i].presence == event_graph::PresencePolicy::conditional_unresolved)
            {
                result.presence.push_back({requirements[i].id, present[i]});
                result.postDispatchObligations.push_back("v4_presence_observation_pending:" + requirements[i].id);
            }
            if (present[i]) active.push_back(requirements[i]);
        }
        if (active.size() != selected.classes.size())
        { result.preconditions.emplace_back("v4_joined_requirement_count_unavailable"); return result; }
        std::map<std::string, std::size_t> indices;
        std::map<std::size_t, std::string> owners;
        for (std::size_t i = 0; i < active.size(); ++i)
            if (!indices.emplace(active[i].id, i).second || !owners.emplace(selected.classes[i], active[i].id).second)
            { result.preconditions.emplace_back("v4_joined_merged_or_duplicate_events_unimplemented"); return result; }
        std::vector<std::string> actualRequired;
        for (const auto& id : selected.order)
        {
            const auto found = indices.find(id);
            if (found == indices.end()) { result.preconditions.emplace_back("v4_joined_order_invalid"); return result; }
            if (active[found->second].presence == event_graph::PresencePolicy::required) actualRequired.push_back(id);
        }
        const auto& axes = verified.Axes(); const auto& candidate = verified.Coordinate();
        const auto allowed = EncodeAllowedEffect(Value(axes, candidate, "T03"), Value(axes, candidate, "T03.extra_protocol_bits"));
        std::vector<const Input*> byStep(program.steps.size(), nullptr);
        for (const auto& input : inputs)
        {
            if (input.programIndex >= program.steps.size() || byStep[input.programIndex] ||
                !ProgramKind(program.steps[input.programIndex].kind))
            { result.preconditions.emplace_back("v4_joined_input_index_invalid"); return result; }
            byStep[input.programIndex] = &input;
        }
        std::vector<std::string> expectedRequired;
        const auto bind = [&](OlePlanStep& step, const std::string& id) {
            const auto found = indices.find(id);
            if (found == indices.end() || active[found->second].presence != event_graph::PresencePolicy::required) return false;
            step.requirements.push_back(id); step.classes.push_back(selected.classes[found->second]);
            expectedRequired.push_back(id); return true;
        };
        bool entered = false, terminated = false, completed = false, asserted = false;
        std::size_t calls = 0;
        for (std::size_t i = 0; i < program.steps.size(); ++i)
        {
            const auto& step = program.steps[i];
            if (step.kind == "configure_v4_source_input") continue; // input construction, not a dispatched event
            const auto kind = ProgramKind(step.kind);
            OlePlanStep plan{OlePlanKind::ole_call, i, std::nullopt, {}, {}};
            if (kind)
            {
                if (terminated || completed || asserted || (*kind == Kind::enter && entered) || (*kind != Kind::enter && !entered))
                { result.preconditions.emplace_back("v4_joined_session_order_unavailable"); return result; }
                const auto* input = byStep[i];
                if (!input || input->kind != *kind || input->modelParameter != step.parameter)
                { result.preconditions.emplace_back("v4_joined_input_binding_unavailable"); return result; }
                if (*kind == Kind::leave)
                {
                    if (input->keyState || input->allowedEffect || input->screenPoint)
                    { result.preconditions.emplace_back("v4_joined_leave_arguments_invalid"); return result; }
                }
                else
                {
                    const auto [buttonAxis, modifierAxis] = PhaseAxes(step.kind);
                    const auto buttons = Value(axes, candidate, buttonAxis), modifiers = Value(axes, candidate, modifierAxis);
                    const auto parsed = ParseKeyParameter(step.parameter);
                    const auto keys = EncodeKeyState(buttons, modifiers);
                    if (!parsed || parsed->first != buttons || parsed->second != modifiers || !keys.valid ||
                        keys.altX1Ambiguous || !input->keyState || *input->keyState != keys.value ||
                        !allowed || !input->allowedEffect || *input->allowedEffect != *allowed || !input->screenPoint)
                    { result.preconditions.emplace_back("v4_joined_key_effect_or_point_unavailable"); return result; }
                }
                entered |= *kind == Kind::enter; terminated |= *kind == Kind::drop || *kind == Kind::leave;
                plan.input = *input; ++calls;
                if (!bind(plan, "program_" + std::to_string(i) + "_call_start") ||
                    !bind(plan, "program_" + std::to_string(i) + "_call_return"))
                { result.preconditions.emplace_back("v4_joined_call_boundary_unavailable"); return result; }
            }
            else if (step.kind == "observe_completion_or_rejection" && terminated && !completed && !asserted)
            {
                completed = true; plan.kind = OlePlanKind::completion_observation;
                if (!bind(plan, "program_" + std::to_string(i) + "_observe"))
                { result.preconditions.emplace_back("v4_joined_completion_boundary_unavailable"); return result; }
            }
            else if (step.kind == "assert_business_results" && completed && !asserted)
            {
                asserted = true; plan.kind = OlePlanKind::business_observation;
                if (!bind(plan, "program_" + std::to_string(i) + "_observe"))
                { result.preconditions.emplace_back("v4_joined_oracle_boundary_unavailable"); return result; }
            }
            else { result.preconditions.push_back("v4_joined_stage_unimplemented:" + step.kind); return result; }
            result.steps.push_back(std::move(plan));
        }
        if (!entered || !terminated || !completed || !asserted || calls != inputs.size() || actualRequired != expectedRequired)
        { result.preconditions.emplace_back("v4_joined_program_order_or_input_unavailable"); return result; }
        result.structurallyPlanned = true;
        result.preconditions.emplace_back("v4_source_all_axes_and_coordinate_binding_unimplemented");
        result.preconditions.emplace_back("v4_condition_and_background_event_observation_capability_unimplemented");
        result.preconditions.emplace_back("v4_refresh_attribution_completion_and_independent_oracle_capability_unimplemented");
    }
    catch (const std::bad_alloc&) { result.preconditions.emplace_back("v4_plan_resource_unavailable"); }
    catch (const std::length_error&) { result.preconditions.emplace_back("v4_plan_resource_unavailable"); }
    return result;
}

// A bounded diagnostic attempt follows the authoritative Program's real
// call order. Selected graph presence/order remain hypotheses for later
// observation; this does not establish input completeness or a verdict.
inline bool BuildRestrictedDesktopCopySteps(const V4FamilyValidatedContext& verified,
    const std::vector<Input>& inputs, std::vector<OlePlanStep>& output)
{
    const auto& axes = verified.Axes(); const auto& candidate = verified.Coordinate();
    const auto value = [&](std::string_view id) { return Value(axes, candidate, id); };
    if (verified.TaskBActive() || value("V4.source.kind") != "file_only" || value("V4.source.file_count") != "one" ||
        value("T01") != "external_ingress" || value("T02.file_paths") != "yes" || value("T02.shell_object") != "no" ||
        value("T02.virtual_files") != "no" || value("T02.virtual_completeness") != "na" ||
        value("T02.image") != "no" || value("T02.text") != "no" || value("T02.url") != "no" ||
        value("T02.url_kind") != "na" || value("T02.invalid_format") != "no" ||
        value("T03") != "copy" || value("T03.extra_protocol_bits") != "none" || value("T04.capability") != "enabled" ||
        value("G04.termination") != "drop" || value("X03.task_count") != "one" ||
        value("target.E01") != "free_desktop" || value("target.E02") != "none" || value("H01.region") != "empty_slot" ||
        inputs.size() != 2 || inputs[0].kind != Kind::enter || inputs[1].kind != Kind::drop) return false;
    const auto allowed = EncodeAllowedEffect(value("T03"), value("T03.extra_protocol_bits"));
    if (!allowed || *allowed != DROPEFFECT_COPY) return false;
    const auto program = verified.TransferProgram();
    std::vector<OlePlanStep> steps;
    bool entered = false, dropped = false, completed = false, asserted = false;
    std::size_t nextInput = 0;
    for (std::size_t i = 0; i < program.steps.size(); ++i)
    {
        const auto& step = program.steps[i];
        if (step.kind == "configure_v4_source_input") { if (entered || dropped || completed || asserted) return false; continue; }
        const auto kind = ProgramKind(step.kind);
        OlePlanStep planned{OlePlanKind::ole_call, i, std::nullopt, {}, {}};
        if (kind)
        {
            if (nextInput >= inputs.size() || inputs[nextInput].programIndex != i || inputs[nextInput].kind != *kind ||
                inputs[nextInput].modelParameter != step.parameter || (*kind != Kind::enter && *kind != Kind::drop) ||
                (*kind == Kind::enter && entered) || (*kind == Kind::drop && (!entered || dropped)) || completed || asserted)
                return false;
            const auto& input = inputs[nextInput++];
            const auto [buttonAxis, modifierAxis] = PhaseAxes(step.kind);
            const auto buttons = value(buttonAxis), modifiers = value(modifierAxis);
            const auto parsed = ParseKeyParameter(step.parameter); const auto keys = EncodeKeyState(buttons, modifiers);
            if (!parsed || parsed->first != buttons || parsed->second != modifiers || !keys.valid || keys.altX1Ambiguous ||
                !input.keyState || *input.keyState != keys.value || !input.allowedEffect || *input.allowedEffect != *allowed ||
                !input.screenPoint) return false;
            entered |= *kind == Kind::enter; dropped |= *kind == Kind::drop; planned.input = input;
        }
        else if (step.kind == "observe_completion_or_rejection" && dropped && !completed && !asserted)
        { completed = true; planned.kind = OlePlanKind::completion_observation; }
        else if (step.kind == "assert_business_results" && completed && !asserted)
        { asserted = true; planned.kind = OlePlanKind::business_observation; }
        else return false;
        steps.push_back(std::move(planned));
    }
    if (!entered || !dropped || !completed || !asserted || nextInput != inputs.size()) return false;
    output = std::move(steps); return true;
}

class V4OleRunReceipt final
{
public:
    V4OleRunReceipt(V4OleRunReceipt&&) = default;
    V4OleRunReceipt& operator=(V4OleRunReceipt&&) = delete;
    V4OleRunReceipt(const V4OleRunReceipt&) = delete;
    V4OleRunReceipt& operator=(const V4OleRunReceipt&) = delete;
    const Result& Execution() const noexcept { return execution_; }
    const std::vector<V2CapturedOleReturn>& CapturedReturns() const noexcept { return captured_; }
    const V4JoinedOleExecutionPlan& Plan() const noexcept { return plan_; }
    // The first value is a preparation snapshot; the second is captured only
    // after both the source and host execution leases have been acquired.
    // Neither value establishes that the host consumed the source.
    const std::optional<V4UnicodeFixtureInputObservation>& SourcePreparation() const noexcept
    { return sourcePreparation_; }
    const std::optional<V4UnicodeFixtureInputObservation>& SourceAtExecution() const noexcept
    { return sourceAtExecution_; }
    bool CompletionTraceCaptured() const noexcept { return completionTraceCaptured_; }
    const std::vector<MessageObservation>& CompletionTrace() const noexcept { return completionTrace_; }
    bool SameBoundHost(const V4UnicodeSourceBinding& binding, const std::shared_ptr<V4UnicodeSourceOwner>& owner,
        OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
        const MessageTrace& trace, HWND control) const noexcept
    {
        auto* source = binding.SourceObject();
        return execution_.productionStarted && plan_.graphIdentityValid && source && owner && epoch &&
            owner_.lock() == owner && hostEpoch_.lock() == epoch && sourceIdentity_ == source &&
            adapterIdentity_ == adapter && binding.CandidateId() == plan_.candidateId &&
            epoch->Matches(source, adapter, &trace, control);
    }
private:
    friend class V4GraphExecutor;
    friend class V4UnicodeReadObservation;
    V4OleRunReceipt() = default;
    Result execution_;
    V4JoinedOleExecutionPlan plan_;
    std::optional<V4UnicodeFixtureInputObservation> sourcePreparation_, sourceAtExecution_;
    std::vector<V2CapturedOleReturn> captured_;
    std::vector<MessageObservation> completionTrace_;
    bool completionTraceCaptured_ = false;
    std::weak_ptr<V4UnicodeSourceOwner> owner_;
    std::weak_ptr<const V3HostEpoch> hostEpoch_;
    ole_content_source_fixture::Source* sourceIdentity_ = nullptr;
    OleDragDropAdapter* adapterIdentity_ = nullptr;
};

class V4GraphExecutor final
{
public:
    // The owner and host epoch originate in the same live isolated fixture;
    // callbacks observe actual dispatch, terminal completion and an
    // independent business oracle. This method never reports a PASS.
    static V4OleRunReceipt RunBound(const V4FamilyValidatedContext& verified,
        const event_graph::V4FamilyIdentityContext& identity, const std::vector<bool>& present,
        const event_graph::V4JoinedGraphVariant& selected, const std::vector<Input>& inputs,
        const V4UnicodeSourceBinding& binding, const std::shared_ptr<V4UnicodeSourceOwner>& owner,
        OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& hostEpoch,
        const HostBoundaries& host, const std::function<void(const Observation&)>& assertIndependentOleResult,
        const std::function<V2HostCoordinateSnapshot(const Observation&)>& sampler = {})
    {
        V4OleRunReceipt receipt;
        bool dispatchEntered = false;
        try
        {
            receipt.plan_ = BuildV4JoinedOleExecutionPlan(verified, identity, present, selected, inputs);
            receipt.execution_.obligations = receipt.plan_.preconditions;
            auto* source = binding.SourceObject();
            const auto sourceInputsMatch = [&](const V4UnicodeFixtureInputObservation& observation) {
                return observation.snapshotComplete && !observation.resourceUnavailable && source &&
                    observation.sourceIdentity == reinterpret_cast<std::uintptr_t>(source) &&
                    observation.ownerThread == GetCurrentThreadId() && observation.format == CF_UNICODETEXT &&
                    observation.index == -1 && observation.tymed == TYMED_HGLOBAL &&
                    !observation.configuredReadFailure &&
                    observation.configuredGetFailure == binding.ConfiguredGetFailure() &&
                    observation.bytes == binding.ExactBytes() && observation.axes.size() == 9 &&
                    std::all_of(observation.axes.begin(), observation.axes.end(),
                        [](const V4UnicodeFixtureAxisObservation& axis) { return axis.matches; });
            };
            receipt.sourcePreparation_ = InspectV4UnicodeFixtureInputs(verified, identity, binding);
            if (!sourceInputsMatch(*receipt.sourcePreparation_))
                receipt.execution_.obligations.emplace_back("v4_receipt_source_axis_snapshot_unavailable_or_mismatch");
            if (!owner || binding.owner.lock() != owner || !binding.EntryBound() || binding.ResourceUnavailable() ||
                !source || !owner->Live() || binding.CandidateId() != receipt.plan_.candidateId ||
                source->ownerThread != GetCurrentThreadId() || source->wrongApartment.load() || source->observationFailed.load() ||
                source->entries.size() != 1 || source->entries.front().format != CF_UNICODETEXT ||
                source->entries.front().index != -1 || source->entries.front().tymed != TYMED_HGLOBAL ||
                source->entries.front().readFailure || source->entries.front().getDataFailure != binding.ConfiguredGetFailure() ||
                source->entries.front().bytes != binding.ExactBytes())
                receipt.execution_.obligations.emplace_back("v4_receipt_same_source_baseline_unavailable");
            if (source)
            {
                std::lock_guard lock(source->observationMutex);
                if (!source->calls.empty()) receipt.execution_.obligations.emplace_back("v4_receipt_source_already_observed");
            }
            if (!adapter || !hostEpoch || !hostEpoch->Matches(source, adapter))
                receipt.execution_.obligations.emplace_back("v4_receipt_host_adapter_epoch_unavailable");
            if (!host.beforeOleCall || !host.observeOleReturn || !host.awaitProductionCompletionOrRejection ||
                !host.assertIndependentBusinessResults || !assertIndependentOleResult)
                receipt.execution_.obligations.emplace_back("v4_receipt_host_observer_or_oracle_missing");
            if (!receipt.plan_.graphIdentityValid || !receipt.plan_.structurallyPlanned || !receipt.execution_.obligations.empty())
                return receipt;
            if (!owner->TryBeginExecution())
            { receipt.execution_.additionalObservationUnavailable = true; receipt.execution_.obligations.emplace_back("v4_source_owner_busy_or_unavailable"); return receipt; }
            const auto endOwner = [](const V4UnicodeSourceOwner* active) noexcept { active->EndExecution(); };
            const std::unique_ptr<const V4UnicodeSourceOwner, decltype(endOwner)> ownerGuard(owner.get(), endOwner);
            if (!hostEpoch->TryBeginExecution(source, adapter))
            { receipt.execution_.additionalObservationUnavailable = true; receipt.execution_.obligations.emplace_back("v4_host_epoch_busy_or_unavailable"); return receipt; }
            const auto endHost = [](const V3HostEpoch* active) noexcept { active->EndExecution(); };
            const std::unique_ptr<const V3HostEpoch, decltype(endHost)> hostGuard(hostEpoch.get(), endHost);
            receipt.sourceAtExecution_ = InspectV4UnicodeFixtureInputs(verified, identity, binding);
            if (!sourceInputsMatch(*receipt.sourceAtExecution_))
            {
                receipt.execution_.additionalObservationUnavailable = true;
                receipt.execution_.obligations.emplace_back("v4_receipt_source_axis_at_execution_unavailable_or_mismatch");
                return receipt;
            }
            receipt.owner_ = owner; receipt.hostEpoch_ = hostEpoch;
            receipt.sourceIdentity_ = source; receipt.adapterIdentity_ = adapter;
            const auto requireLive = [&] {
                if (!owner->Live() || binding.SourceObject() != source || !hostEpoch->Matches(source, adapter))
                    throw ObservationUnavailable("v4_execution_owner_or_host_epoch_changed");
            };
            const HostBoundaries guarded{
                [&](const Input& input) { requireLive(); host.beforeOleCall(input); requireLive(); },
                [&](const Observation& event) { requireLive(); host.observeOleReturn(event); requireLive(); },
                [&](const Observation& terminal) {
                    requireLive(); host.awaitProductionCompletionOrRejection(terminal); requireLive();
                    if (!hostEpoch->trace_ || hostEpoch->trace_->LostOrCrossThread() ||
                        hostEpoch->trace_->OwnerThread() != GetCurrentThreadId())
                        throw ObservationUnavailable("v4_completion_trace_unavailable");
                    receipt.completionTrace_ = hostEpoch->trace_->Observations();
                    receipt.completionTraceCaptured_ = true;
                },
                [&] { requireLive(); host.assertIndependentBusinessResults(); requireLive(); }};
            const std::function<void(const Observation&)> guardedOle = [&](const Observation& event) {
                requireLive(); assertIndependentOleResult(event); requireLive();
            };
            const std::function<V2HostCoordinateSnapshot(const Observation&)> guardedSampler = sampler
                ? std::function<V2HostCoordinateSnapshot(const Observation&)>{[&](const Observation& event) {
                    requireLive(); auto sample = sampler(event); requireLive(); return sample;
                }} : std::function<V2HostCoordinateSnapshot(const Observation&)>{};
            dispatchEntered = true;
            receipt.execution_ = V2GraphExecutor::DispatchApprovedPlan(receipt.plan_.steps, inputs.size(), adapter,
                static_cast<IDataObject*>(source), guarded, guardedOle, &guardedSampler, &receipt.captured_);
            requireLive();
            const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
            if (!replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
                replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
                replay.variant.unresolved != selected.unresolved)
            { receipt.plan_.graphIdentityValid = false; receipt.execution_.additionalObservationUnavailable = true; }
        }
        catch (const std::bad_alloc&)
        {
            if (dispatchEntered && !receipt.execution_.productionStarted) receipt.execution_.productionEntryUnknown = true;
            receipt.plan_.graphIdentityValid = false; receipt.execution_.additionalObservationUnavailable = true;
            if (receipt.execution_.status != Status::event_failed && receipt.execution_.status != Status::event_oracle_failed && receipt.execution_.status != Status::business_failed)
                receipt.execution_.status = Status::observation_unavailable;
        }
        catch (const std::exception& error)
        {
            if (dispatchEntered && !receipt.execution_.productionStarted) receipt.execution_.productionEntryUnknown = true;
            receipt.plan_.graphIdentityValid = false; receipt.execution_.additionalObservationUnavailable = true;
            if (receipt.execution_.status != Status::event_failed && receipt.execution_.status != Status::event_oracle_failed && receipt.execution_.status != Status::business_failed)
                receipt.execution_.status = Status::observation_unavailable;
            try { if (receipt.execution_.failure.empty()) receipt.execution_.failure = error.what(); }
            catch (...) { /* Preserve the actual status and unavailable flag. */ }
        }
        catch (...)
        {
            if (dispatchEntered && !receipt.execution_.productionStarted) receipt.execution_.productionEntryUnknown = true;
            receipt.plan_.graphIdentityValid = false; receipt.execution_.additionalObservationUnavailable = true;
            if (receipt.execution_.status != Status::event_failed && receipt.execution_.status != Status::event_oracle_failed && receipt.execution_.status != Status::business_failed)
                receipt.execution_.status = Status::observation_unavailable;
            try { if (receipt.execution_.failure.empty()) receipt.execution_.failure = "unknown_v4_execution_observation_exception"; }
            catch (...) { /* Preserve the actual status and unavailable flag. */ }
        }
        return receipt;
    }
};

// This receipt is specific to the V4 file graph and the actual Shell-backed
// source. It owns no COM object: the fixture and host epoch must remain live
// on the same STA while the receipt is inspected.
class V4FileOleRunReceipt final
{
public:
    V4FileOleRunReceipt(V4FileOleRunReceipt&&) = default;
    V4FileOleRunReceipt& operator=(V4FileOleRunReceipt&&) = delete;
    V4FileOleRunReceipt(const V4FileOleRunReceipt&) = delete;
    V4FileOleRunReceipt& operator=(const V4FileOleRunReceipt&) = delete;
    const Result& Execution() const noexcept { return execution_; }
    const V4JoinedOleExecutionPlan& Plan() const noexcept { return plan_; }
    const std::optional<V4SingleFileInputObservation>& SourcePreparation() const noexcept { return sourcePreparation_; }
    const std::optional<V4SingleFileInputObservation>& SourceAtExecution() const noexcept { return sourceAtExecution_; }
    const std::vector<V2CapturedOleReturn>& CapturedReturns() const noexcept { return captured_; }
    const std::optional<V3SourceSessionInterval>& SourceSessionInterval() const noexcept { return sourceSessionInterval_; }
    bool CompletionTraceCaptured() const noexcept { return completionTraceCaptured_; }
    const std::vector<MessageObservation>& CompletionTrace() const noexcept { return completionTrace_; }
    bool SameBoundHost(const ole_file_source_fixture::Source& source, OleDragDropAdapter* adapter,
        const std::shared_ptr<const V3HostEpoch>& epoch, const MessageTrace& trace, HWND control) const noexcept
    {
        const auto current = source.LifetimeIdentity();
        return execution_.productionStarted && plan_.graphIdentityValid && sourceIdentity_ == &source &&
            adapterIdentity_ == adapter && epoch && hostEpoch_.lock() == epoch && !sourceLifetime_.expired() &&
            !current.expired() && !sourceLifetime_.owner_before(current) && !current.owner_before(sourceLifetime_) &&
            epoch->Matches(const_cast<ole_file_source_fixture::Source*>(&source), adapter, &trace, control);
    }
private:
    friend class V4FileGraphExecutor;
    V4FileOleRunReceipt() = default;
    Result execution_;
    V4JoinedOleExecutionPlan plan_;
    std::optional<V4SingleFileInputObservation> sourcePreparation_, sourceAtExecution_;
    std::vector<V2CapturedOleReturn> captured_;
    std::optional<V3SourceSessionInterval> sourceSessionInterval_;
    std::vector<MessageObservation> completionTrace_;
    bool completionTraceCaptured_ = false;
    std::weak_ptr<const V3HostEpoch> hostEpoch_;
    std::weak_ptr<const ole_file_source_fixture::Source::LifetimeToken> sourceLifetime_;
    const ole_file_source_fixture::Source* sourceIdentity_ = nullptr;
    OleDragDropAdapter* adapterIdentity_ = nullptr;
};

class V4FileGraphExecutor final
{
public:
    static V4FileOleRunReceipt RunBound(const V4FamilyValidatedContext& verified,
        const event_graph::V4FamilyIdentityContext& identity, const std::vector<bool>& present,
        const event_graph::V4JoinedGraphVariant& selected, const std::vector<Input>& inputs,
        const V4SingleFileFixture& fixture, HANDLE trustedRoot, const FILE_ID_INFO& trustedRootId,
        ole_file_source_fixture::Source& source, OleDragDropAdapter* adapter,
        const std::shared_ptr<const V3HostEpoch>& hostEpoch, const HostBoundaries& host,
        const std::function<void(const Observation&)>& assertIndependentOleResult,
        const std::function<V2HostCoordinateSnapshot(const Observation&)>& sampler = {})
    {
        V4FileOleRunReceipt receipt;
        const auto pinnedHostEpoch = hostEpoch;
        bool dispatchEntered = false;
        try
        {
            receipt.plan_ = BuildV4JoinedOleExecutionPlan(verified, identity, present, selected, inputs);
            receipt.execution_.obligations = receipt.plan_.preconditions;
            receipt.sourcePreparation_ = InspectV4SingleFileInputs(verified, identity, fixture, trustedRoot, trustedRootId, source);
            const auto& initial = *receipt.sourcePreparation_;
            if (!initial.captured || initial.resourceUnavailable || initial.candidateId != receipt.plan_.candidateId)
                receipt.execution_.obligations.emplace_back("v4_file_source_initial_binding_unavailable");
            const auto& axes = verified.Axes(); const auto& candidate = verified.Coordinate();
            if (Value(axes, candidate, "V4.source.kind") != "file_only" ||
                Value(axes, candidate, "T01") != "external_ingress" || Value(axes, candidate, "T02.file_paths") != "yes" ||
                Value(axes, candidate, "T04.capability") != "enabled" || !source.asynchronous)
                receipt.execution_.obligations.emplace_back("v4_file_single_external_async_source_unimplemented");
            if (!adapter || !pinnedHostEpoch || !pinnedHostEpoch->Matches(static_cast<IDataObject*>(&source), adapter))
                receipt.execution_.obligations.emplace_back("v4_file_host_epoch_unavailable");
            if (!host.beforeOleCall || !host.observeOleReturn || !host.awaitProductionCompletionOrRejection ||
                !host.assertIndependentBusinessResults || !assertIndependentOleResult)
                receipt.execution_.obligations.emplace_back("v4_file_host_observer_or_oracle_missing");
            // These are capability gaps, not statements that an outcome or a
            // conditional graph node must already have occurred.
            receipt.execution_.obligations.emplace_back("v4_file_all_input_axes_and_live_geometry_unimplemented");
            receipt.execution_.obligations.emplace_back("v4_file_presence_and_background_event_coverage_unimplemented");
            receipt.execution_.obligations.emplace_back("v4_file_same_task_completion_and_independent_business_oracle_unimplemented");
            if (!receipt.plan_.graphIdentityValid || !receipt.plan_.structurallyPlanned || !receipt.execution_.obligations.empty())
                return receipt;
            if (!pinnedHostEpoch->TryBeginExecution(static_cast<IDataObject*>(&source), adapter))
            { receipt.execution_.additionalObservationUnavailable = true; receipt.execution_.obligations.emplace_back("v4_file_host_epoch_busy"); return receipt; }
            const auto endExecution = [](const V3HostEpoch* epoch) noexcept { epoch->EndExecution(); };
            const std::unique_ptr<const V3HostEpoch, decltype(endExecution)> guard(pinnedHostEpoch.get(), endExecution);
            receipt.sourceAtExecution_ = InspectV4SingleFileInputs(verified, identity, fixture, trustedRoot, trustedRootId, source);
            const auto& atExecution = *receipt.sourceAtExecution_;
            const auto sameId = [](const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
                return a.VolumeSerialNumber == b.VolumeSerialNumber &&
                    std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) == 0;
            };
            if (!atExecution.captured || atExecution.resourceUnavailable || atExecution.candidateId != initial.candidateId ||
                atExecution.sourceAddress != initial.sourceAddress || atExecution.ownerThread != initial.ownerThread ||
                atExecution.sourceLifetime.owner_before(initial.sourceLifetime) || initial.sourceLifetime.owner_before(atExecution.sourceLifetime) ||
                !sameId(atExecution.rootId, initial.rootId) || !sameId(atExecution.fileId, initial.fileId) ||
                atExecution.root != initial.root || atExecution.file != initial.file || atExecution.bytes != initial.bytes)
            { receipt.execution_.additionalObservationUnavailable = true; receipt.execution_.obligations.emplace_back("v4_file_source_changed_before_execution"); return receipt; }
            receipt.sourceIdentity_ = &source; receipt.sourceLifetime_ = source.LifetimeIdentity();
            receipt.adapterIdentity_ = adapter; receipt.hostEpoch_ = pinnedHostEpoch;
            const auto requireLive = [&] {
                if (!pinnedHostEpoch->Matches(static_cast<IDataObject*>(&source), adapter) || source.owner != GetCurrentThreadId())
                    throw ObservationUnavailable("v4_file_execution_host_or_source_changed");
            };
            ole_file_source_fixture::FileDropCallState before;
            requireLive();
            if (!source.ReadFileDropCallState(before) || before.failed || before.depth || before.owner != GetCurrentThreadId())
                throw ObservationUnavailable("v4_file_source_interval_start_unavailable");
            const HostBoundaries guarded{
                [&](const Input& input) { requireLive(); host.beforeOleCall(input); requireLive(); },
                [&](const Observation& event) { requireLive(); host.observeOleReturn(event); requireLive(); },
                [&](const Observation& terminal) {
                    requireLive(); host.awaitProductionCompletionOrRejection(terminal); requireLive();
                    V3SourceSessionInterval interval; interval.before = before;
                    if (!source.ReadFileDropCallState(interval.after) || interval.after.failed || interval.after.depth ||
                        interval.after.owner != before.owner || interval.after.count < before.count)
                        throw ObservationUnavailable("v4_file_source_interval_end_unavailable");
                    interval.events.reserve(interval.after.count - before.count);
                    for (std::size_t i = before.count; i < interval.after.count; ++i)
                    {
                        ole_file_source_fixture::FileDropCallEvent event;
                        if (!source.ReadFileDropCallEvent(i, event)) throw ObservationUnavailable("v4_file_source_interval_record_unavailable");
                        interval.events.push_back(event);
                    }
                    ole_file_source_fixture::FileDropCallState confirmed;
                    if (!source.ReadFileDropCallState(confirmed) || confirmed.failed || confirmed.depth ||
                        confirmed.owner != interval.after.owner || confirmed.count != interval.after.count ||
                        !pinnedHostEpoch->trace_ || pinnedHostEpoch->trace_->LostOrCrossThread() ||
                        pinnedHostEpoch->trace_->OwnerThread() != GetCurrentThreadId())
                        throw ObservationUnavailable("v4_file_completion_trace_or_interval_unavailable");
                    receipt.sourceSessionInterval_ = std::move(interval);
                    receipt.completionTrace_ = pinnedHostEpoch->trace_->Observations();
                    receipt.completionTraceCaptured_ = true; requireLive();
                },
                [&] { requireLive(); host.assertIndependentBusinessResults(); requireLive(); }};
            const std::function<void(const Observation&)> guardedOracle = [&](const Observation& event) {
                requireLive(); assertIndependentOleResult(event); requireLive();
            };
            const std::function<V2HostCoordinateSnapshot(const Observation&)> guardedSampler = sampler
                ? std::function<V2HostCoordinateSnapshot(const Observation&)>{[&](const Observation& event) {
                    requireLive(); auto sample = sampler(event); requireLive(); return sample;
                }} : std::function<V2HostCoordinateSnapshot(const Observation&)>{};
            dispatchEntered = true;
            receipt.execution_ = V2GraphExecutor::DispatchApprovedPlan(receipt.plan_.steps, inputs.size(), adapter,
                static_cast<IDataObject*>(&source), guarded, guardedOracle, &guardedSampler, &receipt.captured_, &source);
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
            receipt.plan_.graphIdentityValid = false; receipt.execution_.additionalObservationUnavailable = true;
            if (receipt.execution_.status != Status::event_failed && receipt.execution_.status != Status::event_oracle_failed && receipt.execution_.status != Status::business_failed)
                receipt.execution_.status = Status::observation_unavailable;
            try { if (receipt.execution_.failure.empty()) receipt.execution_.failure = error.what(); } catch (...) {}
        }
        catch (...)
        {
            if (dispatchEntered && !receipt.execution_.productionStarted) receipt.execution_.productionEntryUnknown = true;
            receipt.plan_.graphIdentityValid = false; receipt.execution_.additionalObservationUnavailable = true;
            if (receipt.execution_.status != Status::event_failed && receipt.execution_.status != Status::event_oracle_failed && receipt.execution_.status != Status::business_failed)
                receipt.execution_.status = Status::observation_unavailable;
            try { if (receipt.execution_.failure.empty()) receipt.execution_.failure = "unknown_v4_file_execution_exception"; } catch (...) {}
        }
        return receipt;
    }
};

class V4BatchObserverIdentity final
{
    friend struct ::behavior::V4BatchCopyObservation;
    V4BatchObserverIdentity() = default;
public:
    V4BatchObserverIdentity(const V4BatchObserverIdentity&) = delete;
    V4BatchObserverIdentity& operator=(const V4BatchObserverIdentity&) = delete;
    ~V4BatchObserverIdentity() = default;
};

class V4BatchExecutionKey final
{
    friend class V4MultiFileGraphExecutor;
    friend class V4MultiFileOleRunReceipt;
    friend struct ::behavior::V4BatchCopyObservation;
public:
    V4BatchExecutionKey(const V4BatchExecutionKey&) = delete;
    V4BatchExecutionKey& operator=(const V4BatchExecutionKey&) = delete;
private:
    enum class Phase { created, before_drop, dispatching, business, capturing, closed };
    V4BatchExecutionKey(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, IDataObject* source,
        OleDragDropAdapter* adapter, std::string candidate, std::string graph)
        : owner_(owner), epoch_(epoch), source_(source), adapter_(adapter), candidate_(std::move(candidate)),
          graph_(std::move(graph)), thread_(GetCurrentThreadId()) {}
    bool SameObjects(const std::shared_ptr<V4IndependentShellOwner>& owner,
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
    bool BindBaseline(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4BatchObserverIdentity>& observer) noexcept
    {
        if (phase_ != Phase::before_drop || bound_ || !observer || !SameObjects(owner, epoch, app, source, candidate)) return false;
        observer_ = observer; bound_ = true; return true;
    }
    bool BeginCapture(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4BatchObserverIdentity>& observer) noexcept
    {
        const auto boundObserver = observer_.lock();
        if (phase_ != Phase::business || !bound_ || published_ || !observer || !boundObserver ||
            boundObserver.owner_before(observer) || observer.owner_before(boundObserver) ||
            !SameObjects(owner, epoch, app, source, candidate)) return false;
        phase_ = Phase::capturing; return true;
    }
    bool Publish(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4BatchObserverIdentity>& observer,
        std::shared_ptr<const ::behavior::V4BatchCopyBusinessEvidence> value) noexcept
    {
        const auto boundObserver = observer_.lock();
        if (!value || phase_ != Phase::capturing || !bound_ || published_ || !observer || !boundObserver ||
            boundObserver.owner_before(observer) || observer.owner_before(boundObserver) ||
            !SameObjects(owner, epoch, app, source, candidate)) return false;
        snapshot_ = std::move(value); published_ = true; return true;
    }
    std::weak_ptr<const V4BatchObserverIdentity> observer_;
    std::weak_ptr<V4IndependentShellOwner> owner_;
    std::weak_ptr<const V3HostEpoch> epoch_;
    IDataObject* source_ = nullptr;
    OleDragDropAdapter* adapter_ = nullptr;
    std::string candidate_, graph_;
    DWORD thread_ = 0;
    Phase phase_ = Phase::created;
    bool bound_ = false, published_ = false, committed_ = false;
    std::shared_ptr<const ::behavior::V4BatchCopyBusinessEvidence> snapshot_;
};

class V4CollectionObserverIdentity final
{
    friend struct ::behavior::V4CollectionCopyObservation;
    V4CollectionObserverIdentity() = default;
public:
    V4CollectionObserverIdentity(const V4CollectionObserverIdentity&) = delete;
    V4CollectionObserverIdentity& operator=(const V4CollectionObserverIdentity&) = delete;
    ~V4CollectionObserverIdentity() = default;
};

class V4CollectionExecutionKey final
{
    friend class V4MultiFileGraphExecutor;
    friend class V4MultiFileOleRunReceipt;
    friend struct ::behavior::V4CollectionCopyObservation;
public:
    V4CollectionExecutionKey(const V4CollectionExecutionKey&) = delete;
    V4CollectionExecutionKey& operator=(const V4CollectionExecutionKey&) = delete;
private:
    enum class Phase { created, before_drop, dispatching, business, capturing, closed };
    V4CollectionExecutionKey(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, IDataObject* source,
        OleDragDropAdapter* adapter, std::string candidate, std::string graph)
        : owner_(owner), epoch_(epoch), source_(source), adapter_(adapter), candidate_(std::move(candidate)),
          graph_(std::move(graph)), thread_(GetCurrentThreadId()) {}
    bool SameObjects(const std::shared_ptr<V4IndependentShellOwner>& owner,
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
    bool BindBaseline(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4CollectionObserverIdentity>& observer) noexcept
    {
        if (phase_ != Phase::before_drop || bound_ || !observer || !SameObjects(owner, epoch, app, source, candidate)) return false;
        observer_ = observer; bound_ = true; return true;
    }
    bool BeginCapture(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4CollectionObserverIdentity>& observer) noexcept
    {
        const auto boundObserver = observer_.lock();
        if (phase_ != Phase::business || !bound_ || published_ || !observer || !boundObserver ||
            boundObserver.owner_before(observer) || observer.owner_before(boundObserver) ||
            !SameObjects(owner, epoch, app, source, candidate)) return false;
        phase_ = Phase::capturing; return true;
    }
    bool Publish(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4CollectionObserverIdentity>& observer,
        std::shared_ptr<const ::behavior::V4CollectionCopyEvidence> value) noexcept
    {
        const auto boundObserver = observer_.lock();
        if (!value || phase_ != Phase::capturing || !bound_ || published_ || !observer || !boundObserver ||
            boundObserver.owner_before(observer) || observer.owner_before(boundObserver) ||
            !SameObjects(owner, epoch, app, source, candidate)) return false;
        snapshot_ = std::move(value); published_ = true; return true;
    }
    std::weak_ptr<const V4CollectionObserverIdentity> observer_;
    std::weak_ptr<V4IndependentShellOwner> owner_;
    std::weak_ptr<const V3HostEpoch> epoch_;
    IDataObject* source_ = nullptr;
    OleDragDropAdapter* adapter_ = nullptr;
    std::string candidate_, graph_;
    DWORD thread_ = 0;
    Phase phase_ = Phase::created;
    bool bound_ = false, published_ = false, committed_ = false;
    std::shared_ptr<const ::behavior::V4CollectionCopyEvidence> snapshot_;
};

class V4GroupObserverIdentity final
{
    friend struct ::behavior::V4GroupCopyObservation;
    V4GroupObserverIdentity() = default;
public:
    V4GroupObserverIdentity(const V4GroupObserverIdentity&) = delete;
    V4GroupObserverIdentity& operator=(const V4GroupObserverIdentity&) = delete;
    ~V4GroupObserverIdentity() = default;
};

class V4GroupExecutionKey final
{
    friend class V4MultiFileGraphExecutor;
    friend class V4MultiFileOleRunReceipt;
    friend struct ::behavior::V4GroupCopyObservation;
public:
    V4GroupExecutionKey(const V4GroupExecutionKey&) = delete;
    V4GroupExecutionKey& operator=(const V4GroupExecutionKey&) = delete;
private:
    enum class Phase { created, before_drop, dispatching, business, capturing, closed };
    V4GroupExecutionKey(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, IDataObject* source,
        OleDragDropAdapter* adapter, std::string candidate, std::string graph)
        : owner_(owner), epoch_(epoch), source_(source), adapter_(adapter), candidate_(std::move(candidate)),
          graph_(std::move(graph)), thread_(GetCurrentThreadId()) {}
    bool SameObjects(const std::shared_ptr<V4IndependentShellOwner>& owner,
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
    bool BindBaseline(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4GroupObserverIdentity>& observer) noexcept
    {
        if (phase_ != Phase::before_drop || bound_ || !observer || !SameObjects(owner, epoch, app, source, candidate)) return false;
        observer_ = observer; bound_ = true; return true;
    }
    bool BeginCapture(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4GroupObserverIdentity>& observer) noexcept
    {
        const auto boundObserver = observer_.lock();
        if (phase_ != Phase::business || !bound_ || published_ || !observer || !boundObserver ||
            boundObserver.owner_before(observer) || observer.owner_before(boundObserver) ||
            !SameObjects(owner, epoch, app, source, candidate)) return false;
        phase_ = Phase::capturing; return true;
    }
    bool Publish(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4GroupObserverIdentity>& observer,
        std::shared_ptr<const ::behavior::V4GroupCopyEvidence> value) noexcept
    {
        const auto boundObserver = observer_.lock();
        if (!value || phase_ != Phase::capturing || !bound_ || published_ || !observer || !boundObserver ||
            boundObserver.owner_before(observer) || observer.owner_before(boundObserver) ||
            !SameObjects(owner, epoch, app, source, candidate)) return false;
        snapshot_ = std::move(value); published_ = true; return true;
    }
    std::weak_ptr<const V4GroupObserverIdentity> observer_;
    std::weak_ptr<V4IndependentShellOwner> owner_;
    std::weak_ptr<const V3HostEpoch> epoch_;
    IDataObject* source_ = nullptr;
    OleDragDropAdapter* adapter_ = nullptr;
    std::string candidate_, graph_;
    DWORD thread_ = 0;
    Phase phase_ = Phase::created;
    bool bound_ = false, published_ = false, committed_ = false;
    std::shared_ptr<const ::behavior::V4GroupCopyEvidence> snapshot_;
};

class V4LeaveObserverIdentity final
{
    friend struct ::behavior::V4FileLeaveObservation;
    V4LeaveObserverIdentity() = default;
public:
    V4LeaveObserverIdentity(const V4LeaveObserverIdentity&) = delete;
    V4LeaveObserverIdentity& operator=(const V4LeaveObserverIdentity&) = delete;
    ~V4LeaveObserverIdentity() = default;
};

class V4LeaveExecutionKey final
{
    friend class V4MultiFileGraphExecutor;
    friend class V4MultiFileOleRunReceipt;
    friend struct ::behavior::V4FileLeaveObservation;
public:
    V4LeaveExecutionKey(const V4LeaveExecutionKey&) = delete;
    V4LeaveExecutionKey& operator=(const V4LeaveExecutionKey&) = delete;
private:
    enum class Phase { created, before_leave, dispatching, business, capturing, closed };
    V4LeaveExecutionKey(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V4PinnedCopyLease>& lease,
        const std::shared_ptr<const V3HostEpoch>& epoch, IDataObject* source,
        OleDragDropAdapter* adapter, std::string candidate, std::string graph)
        : owner_(owner), lease_(lease), epoch_(epoch), source_(source), adapter_(adapter), candidate_(std::move(candidate)),
          graph_(std::move(graph)), thread_(GetCurrentThreadId()) {}
    bool SameObjects(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V4PinnedCopyLease>& lease,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app,
        IDataObject* source, std::string_view candidate) const noexcept
    {
        const auto actualOwner = owner_.lock(); const auto actualLease = lease_.lock(); const auto actualEpoch = epoch_.lock();
        return GetCurrentThreadId() == thread_ && actualOwner && actualLease && actualEpoch && owner && lease && epoch &&
            actualOwner.get() == owner.get() && actualLease.get() == lease.get() && actualEpoch.get() == epoch.get() &&
            !actualLease.owner_before(lease) && !lease.owner_before(actualLease) &&
            !actualOwner.owner_before(owner) && !owner.owner_before(actualOwner) &&
            !actualEpoch.owner_before(epoch) && !epoch.owner_before(actualEpoch) &&
            actualEpoch->app_ == reinterpret_cast<std::uintptr_t>(app) && source == source_ &&
            candidate == candidate_ && !graph_.empty() && actualEpoch->Matches(source_, adapter_);
    }
    bool BindBaseline(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V4PinnedCopyLease>& lease,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4LeaveObserverIdentity>& observer) noexcept
    {
        if (phase_ != Phase::before_leave || bound_ || !observer || !SameObjects(owner, lease, epoch, app, source, candidate)) return false;
        observer_ = observer; bound_ = true; return true;
    }
    bool BeginCapture(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V4PinnedCopyLease>& lease,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4LeaveObserverIdentity>& observer) noexcept
    {
        const auto boundObserver = observer_.lock();
        if (phase_ != Phase::business || !bound_ || published_ || !observer || !boundObserver ||
            boundObserver.get() != observer.get() || boundObserver.owner_before(observer) || observer.owner_before(boundObserver) ||
            !SameObjects(owner, lease, epoch, app, source, candidate)) return false;
        phase_ = Phase::capturing; return true;
    }
    bool Publish(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V4PinnedCopyLease>& lease,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4LeaveObserverIdentity>& observer,
        std::shared_ptr<const ::behavior::V4FileLeaveEvidence> value) noexcept
    {
        const auto boundObserver = observer_.lock();
        if (!value || phase_ != Phase::capturing || !bound_ || published_ || !observer || !boundObserver ||
            boundObserver.get() != observer.get() || boundObserver.owner_before(observer) || observer.owner_before(boundObserver) ||
            !SameObjects(owner, lease, epoch, app, source, candidate)) return false;
        snapshot_ = std::move(value); published_ = true; return true;
    }
    std::weak_ptr<const V4LeaveObserverIdentity> observer_;
    std::weak_ptr<V4IndependentShellOwner> owner_;
    std::weak_ptr<const V4PinnedCopyLease> lease_;
    std::weak_ptr<const V3HostEpoch> epoch_;
    IDataObject* source_ = nullptr;
    OleDragDropAdapter* adapter_ = nullptr;
    std::string candidate_, graph_;
    DWORD thread_ = 0;
    Phase phase_ = Phase::created;
    bool bound_ = false, published_ = false, committed_ = false;
    std::shared_ptr<const ::behavior::V4FileLeaveEvidence> snapshot_;
};

class V4RejectObserverIdentity final
{
    friend struct ::behavior::V4FileRejectObservation;
    V4RejectObserverIdentity() = default;
public:
    V4RejectObserverIdentity(const V4RejectObserverIdentity&) = delete;
    V4RejectObserverIdentity& operator=(const V4RejectObserverIdentity&) = delete;
    ~V4RejectObserverIdentity() = default;
};

class V4RejectExecutionKey final
{
    friend class V4MultiFileGraphExecutor;
    friend class V4MultiFileOleRunReceipt;
    friend struct ::behavior::V4FileRejectObservation;
public:
    V4RejectExecutionKey(const V4RejectExecutionKey&) = delete;
    V4RejectExecutionKey& operator=(const V4RejectExecutionKey&) = delete;
private:
    enum class Phase { created, before_drop, dispatching, business, capturing, closed };
    V4RejectExecutionKey(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V4PinnedCopyLease>& lease,
        const std::shared_ptr<const V3HostEpoch>& epoch, IDataObject* source,
        OleDragDropAdapter* adapter, std::string candidate, std::string graph)
        : owner_(owner), lease_(lease), epoch_(epoch), source_(source), adapter_(adapter), candidate_(std::move(candidate)),
          graph_(std::move(graph)), thread_(GetCurrentThreadId()) {}
    bool SameObjects(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V4PinnedCopyLease>& lease,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app,
        IDataObject* source, std::string_view candidate) const noexcept
    {
        const auto actualOwner = owner_.lock(); const auto actualLease = lease_.lock(); const auto actualEpoch = epoch_.lock();
        return GetCurrentThreadId() == thread_ && actualOwner && actualLease && actualEpoch && owner && lease && epoch &&
            actualOwner.get() == owner.get() && actualLease.get() == lease.get() && actualEpoch.get() == epoch.get() &&
            !actualLease.owner_before(lease) && !lease.owner_before(actualLease) &&
            !actualOwner.owner_before(owner) && !owner.owner_before(actualOwner) &&
            !actualEpoch.owner_before(epoch) && !epoch.owner_before(actualEpoch) &&
            actualEpoch->app_ == reinterpret_cast<std::uintptr_t>(app) && source == source_ &&
            candidate == candidate_ && !graph_.empty() && actualEpoch->Matches(source_, adapter_);
    }
    bool BindBaseline(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V4PinnedCopyLease>& lease,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4RejectObserverIdentity>& observer) noexcept
    {
        if (phase_ != Phase::before_drop || bound_ || !observer || !SameObjects(owner, lease, epoch, app, source, candidate)) return false;
        observer_ = observer; bound_ = true; return true;
    }
    bool BeginCapture(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V4PinnedCopyLease>& lease,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4RejectObserverIdentity>& observer) noexcept
    {
        const auto boundObserver = observer_.lock();
        if (phase_ != Phase::business || !bound_ || published_ || !observer || !boundObserver ||
            boundObserver.get() != observer.get() || boundObserver.owner_before(observer) || observer.owner_before(boundObserver) ||
            !SameObjects(owner, lease, epoch, app, source, candidate)) return false;
        phase_ = Phase::capturing; return true;
    }
    bool Publish(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V4PinnedCopyLease>& lease,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4RejectObserverIdentity>& observer,
        std::shared_ptr<const ::behavior::V4FileRejectEvidence> value) noexcept
    {
        const auto boundObserver = observer_.lock();
        if (!value || phase_ != Phase::capturing || !bound_ || published_ || !observer || !boundObserver ||
            boundObserver.get() != observer.get() || boundObserver.owner_before(observer) || observer.owner_before(boundObserver) ||
            !SameObjects(owner, lease, epoch, app, source, candidate)) return false;
        snapshot_ = std::move(value); published_ = true; return true;
    }
    std::weak_ptr<const V4RejectObserverIdentity> observer_;
    std::weak_ptr<V4IndependentShellOwner> owner_;
    std::weak_ptr<const V4PinnedCopyLease> lease_;
    std::weak_ptr<const V3HostEpoch> epoch_;
    IDataObject* source_ = nullptr;
    OleDragDropAdapter* adapter_ = nullptr;
    std::string candidate_, graph_;
    DWORD thread_ = 0;
    Phase phase_ = Phase::created;
    bool bound_ = false, published_ = false, committed_ = false;
    std::shared_ptr<const ::behavior::V4FileRejectEvidence> snapshot_;
};


class V4MoveObserverIdentity final
{
    friend struct ::behavior::V4FileMoveObservation;
    V4MoveObserverIdentity() = default;
public:
    V4MoveObserverIdentity(const V4MoveObserverIdentity&) = delete;
    V4MoveObserverIdentity& operator=(const V4MoveObserverIdentity&) = delete;
    ~V4MoveObserverIdentity() = default;
};

class V4MoveExecutionKey final
{
    friend class V4MultiFileGraphExecutor;
    friend class V4MultiFileOleRunReceipt;
    friend struct ::behavior::V4FileMoveObservation;
public:
    V4MoveExecutionKey(const V4MoveExecutionKey&) = delete;
    V4MoveExecutionKey& operator=(const V4MoveExecutionKey&) = delete;
private:
    enum class Phase { created, before_drop, dispatching, business, capturing, closed };
    V4MoveExecutionKey(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V4SharedMoveLease>& lease,
        const std::shared_ptr<const V3HostEpoch>& epoch, IDataObject* source,
        OleDragDropAdapter* adapter, std::string candidate, std::string graph)
        : owner_(owner), lease_(lease), epoch_(epoch), source_(source), adapter_(adapter), candidate_(std::move(candidate)),
          graph_(std::move(graph)), thread_(GetCurrentThreadId()) {}
    bool SameObjects(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V4SharedMoveLease>& lease,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app,
        IDataObject* source, std::string_view candidate) const noexcept
    {
        const auto actualOwner = owner_.lock(); const auto actualLease = lease_.lock(); const auto actualEpoch = epoch_.lock();
        return GetCurrentThreadId() == thread_ && actualOwner && actualLease && actualEpoch && owner && lease && epoch &&
            actualOwner.get() == owner.get() && actualLease.get() == lease.get() && actualEpoch.get() == epoch.get() &&
            !actualLease.owner_before(lease) && !lease.owner_before(actualLease) &&
            !actualOwner.owner_before(owner) && !owner.owner_before(actualOwner) &&
            !actualEpoch.owner_before(epoch) && !epoch.owner_before(actualEpoch) &&
            actualEpoch->app_ == reinterpret_cast<std::uintptr_t>(app) && source == source_ &&
            candidate == candidate_ && !graph_.empty() && actualEpoch->Matches(source_, adapter_);
    }
    bool BindBaseline(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V4SharedMoveLease>& lease,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4MoveObserverIdentity>& observer) noexcept
    {
        if (phase_ != Phase::before_drop || bound_ || !observer || !SameObjects(owner, lease, epoch, app, source, candidate)) return false;
        observer_ = observer; bound_ = true; return true;
    }
    bool BeginCapture(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V4SharedMoveLease>& lease,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4MoveObserverIdentity>& observer) noexcept
    {
        const auto boundObserver = observer_.lock();
        if (phase_ != Phase::business || !bound_ || published_ || !observer || !boundObserver ||
            boundObserver.get() != observer.get() || boundObserver.owner_before(observer) || observer.owner_before(boundObserver) ||
            !SameObjects(owner, lease, epoch, app, source, candidate)) return false;
        phase_ = Phase::capturing; return true;
    }
    bool Publish(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V4SharedMoveLease>& lease,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4MoveObserverIdentity>& observer,
        std::shared_ptr<const ::behavior::V4FileMoveEvidence> value) noexcept
    {
        const auto boundObserver = observer_.lock();
        if (!value || phase_ != Phase::capturing || !bound_ || published_ || !observer || !boundObserver ||
            boundObserver.get() != observer.get() || boundObserver.owner_before(observer) || observer.owner_before(boundObserver) ||
            !SameObjects(owner, lease, epoch, app, source, candidate)) return false;
        snapshot_ = std::move(value); published_ = true; return true;
    }
    std::weak_ptr<const V4MoveObserverIdentity> observer_;
    std::weak_ptr<V4IndependentShellOwner> owner_;
    std::weak_ptr<const V4SharedMoveLease> lease_;
    std::weak_ptr<const V3HostEpoch> epoch_;
    IDataObject* source_ = nullptr;
    OleDragDropAdapter* adapter_ = nullptr;
    std::string candidate_, graph_;
    DWORD thread_ = 0;
    Phase phase_ = Phase::created;
    bool bound_ = false, published_ = false, committed_ = false;
    std::shared_ptr<const ::behavior::V4FileMoveEvidence> snapshot_;
};

class V4DesktopFilesObserverIdentity final
{
    friend struct ::behavior::V4DesktopFilesCopyObservation;
    V4DesktopFilesObserverIdentity() = default;
public:
    V4DesktopFilesObserverIdentity(const V4DesktopFilesObserverIdentity&) = delete;
    V4DesktopFilesObserverIdentity& operator=(const V4DesktopFilesObserverIdentity&) = delete;
    ~V4DesktopFilesObserverIdentity() = default;
};

class V4DesktopFilesExecutionKey final
{
    friend class V4MultiFileGraphExecutor;
    friend class V4MultiFileOleRunReceipt;
    friend struct ::behavior::V4DesktopFilesCopyObservation;
public:
    V4DesktopFilesExecutionKey(const V4DesktopFilesExecutionKey&) = delete;
    V4DesktopFilesExecutionKey& operator=(const V4DesktopFilesExecutionKey&) = delete;
private:
    enum class Phase { created, before_drop, dispatching, business, capturing, closed };
    V4DesktopFilesExecutionKey(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, IDataObject* source,
        OleDragDropAdapter* adapter, std::string candidate, std::string graph)
        : owner_(owner), epoch_(epoch), source_(source), adapter_(adapter), candidate_(std::move(candidate)),
          graph_(std::move(graph)), thread_(GetCurrentThreadId()) {}
    bool SameObjects(const std::shared_ptr<V4IndependentShellOwner>& owner,
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
    bool BindBaseline(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4DesktopFilesObserverIdentity>& observer) noexcept
    {
        if (phase_ != Phase::before_drop || bound_ || !observer || !SameObjects(owner, epoch, app, source, candidate)) return false;
        observer_ = observer; bound_ = true; return true;
    }
    bool BeginCapture(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4DesktopFilesObserverIdentity>& observer) noexcept
    {
        const auto boundObserver = observer_.lock();
        if (phase_ != Phase::business || !bound_ || published_ || !observer || !boundObserver ||
            boundObserver.owner_before(observer) || observer.owner_before(boundObserver) ||
            !SameObjects(owner, epoch, app, source, candidate)) return false;
        phase_ = Phase::capturing; return true;
    }
    bool Publish(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4DesktopFilesObserverIdentity>& observer,
        std::shared_ptr<const ::behavior::V4DesktopFilesCopyEvidence> value) noexcept
    {
        const auto boundObserver = observer_.lock();
        if (!value || phase_ != Phase::capturing || !bound_ || published_ || !observer || !boundObserver ||
            boundObserver.owner_before(observer) || observer.owner_before(boundObserver) ||
            !SameObjects(owner, epoch, app, source, candidate)) return false;
        snapshot_ = std::move(value); published_ = true; return true;
    }
    std::weak_ptr<const V4DesktopFilesObserverIdentity> observer_;
    std::weak_ptr<V4IndependentShellOwner> owner_;
    std::weak_ptr<const V3HostEpoch> epoch_;
    IDataObject* source_ = nullptr;
    OleDragDropAdapter* adapter_ = nullptr;
    std::string candidate_, graph_;
    DWORD thread_ = 0;
    Phase phase_ = Phase::created;
    bool bound_ = false, published_ = false, committed_ = false;
    std::shared_ptr<const ::behavior::V4DesktopFilesCopyEvidence> snapshot_;
};

class V4FolderMappingObserverIdentity final
{
    friend struct ::behavior::V4FolderMappingCopyObservation;
    V4FolderMappingObserverIdentity() = default;
public:
    V4FolderMappingObserverIdentity(const V4FolderMappingObserverIdentity&) = delete;
    V4FolderMappingObserverIdentity& operator=(const V4FolderMappingObserverIdentity&) = delete;
    ~V4FolderMappingObserverIdentity() = default;
};

class V4FolderMappingExecutionKey final
{
    friend class V4MultiFileGraphExecutor;
    friend class V4MultiFileOleRunReceipt;
    friend struct ::behavior::V4FolderMappingCopyObservation;
public:
    V4FolderMappingExecutionKey(const V4FolderMappingExecutionKey&) = delete;
    V4FolderMappingExecutionKey& operator=(const V4FolderMappingExecutionKey&) = delete;
private:
    enum class Phase { created, before_drop, dispatching, business, capturing, closed };
    V4FolderMappingExecutionKey(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, IDataObject* source,
        OleDragDropAdapter* adapter, std::string candidate, std::string graph)
        : owner_(owner), epoch_(epoch), source_(source), adapter_(adapter), candidate_(std::move(candidate)),
          graph_(std::move(graph)), thread_(GetCurrentThreadId()) {}
    bool SameObjects(const std::shared_ptr<V4IndependentShellOwner>& owner,
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
    bool BindBaseline(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4FolderMappingObserverIdentity>& observer) noexcept
    {
        if (phase_ != Phase::before_drop || bound_ || !observer || !SameObjects(owner, epoch, app, source, candidate)) return false;
        observer_ = observer; bound_ = true; return true;
    }
    bool BeginCapture(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4FolderMappingObserverIdentity>& observer) noexcept
    {
        const auto boundObserver = observer_.lock();
        if (phase_ != Phase::business || !bound_ || published_ || !observer || !boundObserver ||
            boundObserver.owner_before(observer) || observer.owner_before(boundObserver) ||
            !SameObjects(owner, epoch, app, source, candidate)) return false;
        phase_ = Phase::capturing; return true;
    }
    bool Publish(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4FolderMappingObserverIdentity>& observer,
        std::shared_ptr<const ::behavior::V4FolderMappingCopyEvidence> value) noexcept
    {
        const auto boundObserver = observer_.lock();
        if (!value || phase_ != Phase::capturing || !bound_ || published_ || !observer || !boundObserver ||
            boundObserver.owner_before(observer) || observer.owner_before(boundObserver) ||
            !SameObjects(owner, epoch, app, source, candidate)) return false;
        snapshot_ = std::move(value); published_ = true; return true;
    }
    std::weak_ptr<const V4FolderMappingObserverIdentity> observer_;
    std::weak_ptr<V4IndependentShellOwner> owner_;
    std::weak_ptr<const V3HostEpoch> epoch_;
    IDataObject* source_ = nullptr;
    OleDragDropAdapter* adapter_ = nullptr;
    std::string candidate_, graph_;
    DWORD thread_ = 0;
    Phase phase_ = Phase::created;
    bool bound_ = false, published_ = false, committed_ = false;
    std::shared_ptr<const ::behavior::V4FolderMappingCopyEvidence> snapshot_;
};

class V4DirectoryObserverIdentity final
{
    friend struct ::behavior::V4DirectoryCopyObservation;
    V4DirectoryObserverIdentity() = default;
public:
    V4DirectoryObserverIdentity(const V4DirectoryObserverIdentity&) = delete;
    V4DirectoryObserverIdentity& operator=(const V4DirectoryObserverIdentity&) = delete;
    ~V4DirectoryObserverIdentity() = default;
};

class V4DirectoryExecutionKey final
{
    friend class V4DirectoryGraphExecutor;
    friend class V4DirectoryOleRunReceipt;
    friend struct ::behavior::V4DirectoryCopyObservation;
public:
    V4DirectoryExecutionKey(const V4DirectoryExecutionKey&) = delete;
    V4DirectoryExecutionKey& operator=(const V4DirectoryExecutionKey&) = delete;
private:
    enum class Phase { created, before_drop, dispatching, business, capturing, closed };
    V4DirectoryExecutionKey(const std::shared_ptr<directory_fixture::DirectoryShellOwner>& owner,
        const std::shared_ptr<directory_fixture::DirectoryCopyLease>& lease,
        const std::shared_ptr<const V3HostEpoch>& epoch, IDataObject* source,
        OleDragDropAdapter* adapter, std::string candidate, std::string graph)
        : owner_(owner), lease_(lease), epoch_(epoch), source_(source), adapter_(adapter), candidate_(std::move(candidate)),
          graph_(std::move(graph)), thread_(GetCurrentThreadId()) {}
    bool SameObjects(const std::shared_ptr<directory_fixture::DirectoryShellOwner>& owner,
        const std::shared_ptr<directory_fixture::DirectoryCopyLease>& lease,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app,
        IDataObject* source, std::string_view candidate) const noexcept
    {
        const auto actualOwner = owner_.lock(); const auto actualLease = lease_.lock(); const auto actualEpoch = epoch_.lock();
        return GetCurrentThreadId() == thread_ && actualOwner && actualLease && actualEpoch && owner && lease && epoch &&
            actualOwner.get() == owner.get() && actualLease.get() == lease.get() && actualEpoch.get() == epoch.get() &&
            !actualLease.owner_before(lease) && !lease.owner_before(actualLease) &&
            lease->OwnerThread() == thread_ && lease->CandidateId() == candidate_ &&
            !actualOwner.owner_before(owner) && !owner.owner_before(actualOwner) &&
            !actualEpoch.owner_before(epoch) && !epoch.owner_before(actualEpoch) &&
            actualEpoch->app_ == reinterpret_cast<std::uintptr_t>(app) && source == source_ &&
            candidate == candidate_ && !graph_.empty() && actualEpoch->Matches(source_, adapter_);
    }
    bool BindBaseline(const std::shared_ptr<directory_fixture::DirectoryShellOwner>& owner,
        const std::shared_ptr<directory_fixture::DirectoryCopyLease>& lease,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4DirectoryObserverIdentity>& observer) noexcept
    {
        if (phase_ != Phase::before_drop || bound_ || !observer || !SameObjects(owner, lease, epoch, app, source, candidate)) return false;
        observer_ = observer; bound_ = true; return true;
    }
    bool BeginCapture(const std::shared_ptr<directory_fixture::DirectoryShellOwner>& owner,
        const std::shared_ptr<directory_fixture::DirectoryCopyLease>& lease,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4DirectoryObserverIdentity>& observer) noexcept
    {
        const auto boundObserver = observer_.lock();
        if (phase_ != Phase::business || !bound_ || published_ || !observer || !boundObserver ||
            boundObserver.get() != observer.get() ||
            boundObserver.owner_before(observer) || observer.owner_before(boundObserver) ||
            !SameObjects(owner, lease, epoch, app, source, candidate)) return false;
        phase_ = Phase::capturing; return true;
    }
    bool Publish(const std::shared_ptr<directory_fixture::DirectoryShellOwner>& owner,
        const std::shared_ptr<directory_fixture::DirectoryCopyLease>& lease,
        const std::shared_ptr<const V3HostEpoch>& epoch, const void* app, IDataObject* source,
        std::string_view candidate, const std::shared_ptr<const V4DirectoryObserverIdentity>& observer,
        std::shared_ptr<const ::behavior::V4DirectoryCopyEvidence> value) noexcept
    {
        const auto boundObserver = observer_.lock();
        if (!value || phase_ != Phase::capturing || !bound_ || published_ || !observer || !boundObserver ||
            boundObserver.get() != observer.get() ||
            boundObserver.owner_before(observer) || observer.owner_before(boundObserver) ||
            !SameObjects(owner, lease, epoch, app, source, candidate)) return false;
        snapshot_ = std::move(value); published_ = true; return true;
    }
    std::weak_ptr<const V4DirectoryObserverIdentity> observer_;
    std::weak_ptr<directory_fixture::DirectoryShellOwner> owner_;
    std::weak_ptr<directory_fixture::DirectoryCopyLease> lease_;
    std::weak_ptr<const V3HostEpoch> epoch_;
    IDataObject* source_ = nullptr;
    OleDragDropAdapter* adapter_ = nullptr;
    std::string candidate_, graph_;
    DWORD thread_ = 0;
    Phase phase_ = Phase::created;
    bool bound_ = false, published_ = false, committed_ = false;
    std::shared_ptr<const ::behavior::V4DirectoryCopyEvidence> snapshot_;
};

struct V4BatchObserverBoundaries
{
    std::function<void(const std::shared_ptr<V4BatchExecutionKey>&)> bindBeforeDrop;
    std::function<void(const std::shared_ptr<V4BatchExecutionKey>&)> observeBusiness;
};
struct V4CollectionObserverBoundaries
{
    std::function<void(const std::shared_ptr<V4CollectionExecutionKey>&)> bindBeforeDrop;
    std::function<void(const std::shared_ptr<V4CollectionExecutionKey>&)> observeBusiness;
};
struct V4GroupObserverBoundaries
{
    std::function<void(const std::shared_ptr<V4GroupExecutionKey>&)> bindBeforeDrop;
    std::function<void(const std::shared_ptr<V4GroupExecutionKey>&)> observeBusiness;
};
struct V4DesktopFilesObserverBoundaries
{
    std::function<void(const std::shared_ptr<V4DesktopFilesExecutionKey>&)> bindBeforeDrop;
    std::function<void(const std::shared_ptr<V4DesktopFilesExecutionKey>&)> observeBusiness;
};
struct V4FolderMappingObserverBoundaries
{
    std::function<void(const std::shared_ptr<V4FolderMappingExecutionKey>&)> bindBeforeDrop;
    std::function<void(const std::shared_ptr<V4FolderMappingExecutionKey>&)> observeBusiness;
};
struct V4LeaveObserverBoundaries
{
    std::function<void(const std::shared_ptr<V4LeaveExecutionKey>&)> bindBeforeLeave;
    std::function<void(const std::shared_ptr<V4LeaveExecutionKey>&)> observeBusiness;
};
struct V4RejectObserverBoundaries
{
    std::function<void(const std::shared_ptr<V4RejectExecutionKey>&)> bindBeforeDrop;
    std::function<void(const std::shared_ptr<V4RejectExecutionKey>&)> observeBusiness;
};
struct V4MoveObserverBoundaries
{
    std::function<void(const std::shared_ptr<V4MoveExecutionKey>&)> bindBeforeDrop;
    std::function<void(const std::shared_ptr<V4MoveExecutionKey>&)> observeBusiness;
};
struct V4DirectoryObserverBoundaries
{
    std::function<void(const std::shared_ptr<V4DirectoryExecutionKey>&)> bindBeforeDrop;
    std::function<void(const std::shared_ptr<V4DirectoryExecutionKey>&)> observeBusiness;
};

class V4MultiFileOleRunReceipt final
{
public:
    V4MultiFileOleRunReceipt(V4MultiFileOleRunReceipt&&) = default;
    V4MultiFileOleRunReceipt& operator=(V4MultiFileOleRunReceipt&&) = delete;
    V4MultiFileOleRunReceipt(const V4MultiFileOleRunReceipt&) = delete;
    V4MultiFileOleRunReceipt& operator=(const V4MultiFileOleRunReceipt&) = delete;
    const Result& Execution() const noexcept { return execution_; }
    const V4JoinedOleExecutionPlan& Plan() const noexcept { return plan_; }
    std::shared_ptr<const ::behavior::V4BatchCopyBusinessEvidence> SealedBusiness() const noexcept
    {
        return execution_.productionStarted && plan_.graphIdentityValid && executionKey_ &&
            executionKey_->phase_ == V4BatchExecutionKey::Phase::closed && executionKey_->published_ && executionKey_->committed_
            ? executionKey_->snapshot_ : nullptr;
    }
    std::shared_ptr<const ::behavior::V4CollectionCopyEvidence> SealedCollectionBusiness() const noexcept
    {
        return execution_.productionStarted && plan_.graphIdentityValid && collectionKey_ &&
            collectionKey_->phase_ == V4CollectionExecutionKey::Phase::closed &&
            collectionKey_->published_ && collectionKey_->committed_ ? collectionKey_->snapshot_ : nullptr;
    }
    std::shared_ptr<const ::behavior::V4GroupCopyEvidence> SealedGroupBusiness() const noexcept
    {
        return execution_.productionStarted && plan_.graphIdentityValid && groupKey_ &&
            groupKey_->phase_ == V4GroupExecutionKey::Phase::closed &&
            groupKey_->published_ && groupKey_->committed_ ? groupKey_->snapshot_ : nullptr;
    }
    std::shared_ptr<const ::behavior::V4DesktopFilesCopyEvidence> SealedDesktopFilesBusiness() const noexcept
    {
        return execution_.productionStarted && plan_.graphIdentityValid && desktopFilesKey_ &&
            desktopFilesKey_->phase_ == V4DesktopFilesExecutionKey::Phase::closed &&
            desktopFilesKey_->published_ && desktopFilesKey_->committed_ ? desktopFilesKey_->snapshot_ : nullptr;
    }
    std::shared_ptr<const ::behavior::V4FolderMappingCopyEvidence> SealedFolderMappingBusiness() const noexcept
    {
        return execution_.productionStarted && plan_.graphIdentityValid && folderMappingKey_ &&
            folderMappingKey_->phase_ == V4FolderMappingExecutionKey::Phase::closed &&
            folderMappingKey_->published_ && folderMappingKey_->committed_ ? folderMappingKey_->snapshot_ : nullptr;
    }
    std::shared_ptr<const ::behavior::V4FileLeaveEvidence> SealedLeaveBusiness() const noexcept
    {
        return execution_.productionStarted && plan_.graphIdentityValid && leaveKey_ &&
            leaveKey_->phase_ == V4LeaveExecutionKey::Phase::closed && leaveKey_->published_ && leaveKey_->committed_
            ? leaveKey_->snapshot_ : nullptr;
    }
    std::shared_ptr<const ::behavior::V4FileRejectEvidence> SealedRejectBusiness() const noexcept
    {
        return execution_.productionStarted && plan_.graphIdentityValid && rejectKey_ &&
            rejectKey_->phase_ == V4RejectExecutionKey::Phase::closed && rejectKey_->published_ && rejectKey_->committed_
            ? rejectKey_->snapshot_ : nullptr;
    }
    std::shared_ptr<const ::behavior::V4FileMoveEvidence> SealedMoveBusiness() const noexcept
    {
        return execution_.productionStarted && plan_.graphIdentityValid && moveKey_ &&
            moveKey_->phase_ == V4MoveExecutionKey::Phase::closed && moveKey_->published_ && moveKey_->committed_
            ? moveKey_->snapshot_ : nullptr;
    }
    const std::optional<V4IndependentFileInputObservation>& SourcePreparation() const noexcept { return sourcePreparation_; }
    const std::optional<V4IndependentFileInputObservation>& SourceAtExecution() const noexcept { return sourceAtExecution_; }
    // Program indices whose exact axis/value was checked by the prepared
    // source snapshot. This preflight is not a dispatch or a full-axis proof.
    const std::vector<std::size_t>& SourceInputStepsObserved() const noexcept { return sourceInputStepsObserved_; }
    const std::vector<V2CapturedOleReturn>& CapturedReturns() const noexcept { return captured_; }
    const std::optional<V3SourceSessionInterval>& SourceSessionInterval() const noexcept { return sourceSessionInterval_; }
    bool CompletionTraceCaptured() const noexcept { return completionTraceCaptured_; }
    const std::vector<MessageObservation>& CompletionTrace() const noexcept { return completionTrace_; }
    bool PinnedCopyPreflightObserved() const noexcept { return pinnedCopyPreflight_; }
    bool PinnedCopyBeforeDropObserved() const noexcept { return pinnedCopyBeforeDrop_; }
    bool PinnedCopyBeforeLeaveObserved() const noexcept { return pinnedCopyBeforeLeave_; }
    bool PinnedCopyAfterAwaitObserved() const noexcept { return pinnedCopyAfterAwait_; }
    bool SharedMoveBeforeDropObserved() const noexcept { return sharedMoveBeforeDrop_; }
    const std::optional<V4SharedMoveSourceAfter>& SharedMoveAfterAwait() const noexcept { return sharedMoveAfterAwait_; }
    bool SameSharedMoveLease(const std::shared_ptr<V4IndependentFilePayload>& payload,
        const std::shared_ptr<V4IndependentShellOwner>& owner, const std::shared_ptr<const V4SharedMoveLease>& lease) const noexcept
    {
        const auto captured = sharedMoveLease_.lock(); const auto capturedOwner = owner_.lock();
        return captured && lease && !captured.owner_before(lease) && !lease.owner_before(captured) &&
            capturedOwner && owner && !capturedOwner.owner_before(owner) && !owner.owner_before(capturedOwner) &&
            owner->OwnsSharedMoveLease(payload, lease) && lease->CandidateId() == plan_.candidateId;
    }
    bool SamePinnedCopyLease(const V4FamilyValidatedContext& verified,
        const event_graph::V4FamilyIdentityContext& identity, const std::shared_ptr<V4IndependentFilePayload>& payload,
        const std::shared_ptr<V4IndependentShellOwner>& owner, const std::shared_ptr<const V4PinnedCopyLease>& lease) const
    {
        const auto captured = pinnedCopyLease_.lock();
        const auto capturedOwner = owner_.lock();
        return captured && lease && !captured.owner_before(lease) && !lease.owner_before(captured) &&
            capturedOwner && owner && !capturedOwner.owner_before(owner) && !owner.owner_before(capturedOwner) &&
            owner->MatchesPinnedCopyLease(verified, identity, payload, lease);
    }
    bool SameBoundHost(const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& owner,
        OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
        const MessageTrace& trace, HWND control) const noexcept
    {
        auto* source = binding.SourceObject();
        if (!execution_.productionStarted || !plan_.graphIdentityValid || !source || !owner || !epoch ||
            owner_.lock() != owner || hostEpoch_.lock() != epoch || sourceIdentity_ != source || adapterIdentity_ != adapter ||
            binding.candidateId != plan_.candidateId) return false;
        const auto current = source->LifetimeIdentity();
        return !sourceLifetime_.expired() && !current.expired() && !sourceLifetime_.owner_before(current) &&
            !current.owner_before(sourceLifetime_) && epoch->Matches(source, adapter, &trace, control);
    }
private:
    friend class V4MultiFileGraphExecutor;
    V4MultiFileOleRunReceipt() = default;
    Result execution_;
    V4JoinedOleExecutionPlan plan_;
    std::optional<V4IndependentFileInputObservation> sourcePreparation_, sourceAtExecution_;
    std::vector<std::size_t> sourceInputStepsObserved_;
    std::vector<V2CapturedOleReturn> captured_;
    std::optional<V3SourceSessionInterval> sourceSessionInterval_;
    std::vector<MessageObservation> completionTrace_;
    bool completionTraceCaptured_ = false;
    std::weak_ptr<V4IndependentShellOwner> owner_;
    std::weak_ptr<const V3HostEpoch> hostEpoch_;
    std::weak_ptr<const ole_file_source_fixture::Source::LifetimeToken> sourceLifetime_;
    ole_file_source_fixture::Source* sourceIdentity_ = nullptr;
    OleDragDropAdapter* adapterIdentity_ = nullptr;
    std::shared_ptr<V4BatchExecutionKey> executionKey_;
    std::shared_ptr<V4CollectionExecutionKey> collectionKey_;
    std::shared_ptr<V4GroupExecutionKey> groupKey_;
    std::shared_ptr<V4DesktopFilesExecutionKey> desktopFilesKey_;
    std::shared_ptr<V4FolderMappingExecutionKey> folderMappingKey_;
    std::shared_ptr<V4LeaveExecutionKey> leaveKey_;
    std::shared_ptr<V4RejectExecutionKey> rejectKey_;
    std::shared_ptr<V4MoveExecutionKey> moveKey_;
    std::weak_ptr<const V4PinnedCopyLease> pinnedCopyLease_;
    std::weak_ptr<const V4SharedMoveLease> sharedMoveLease_;
    std::optional<V4SharedMoveSourceAfter> sharedMoveAfterAwait_;
    bool sharedMoveBeforeDrop_ = false;
    bool pinnedCopyPreflight_ = false, pinnedCopyBeforeDrop_ = false, pinnedCopyBeforeLeave_ = false, pinnedCopyAfterAwait_ = false;
};

class V4MultiFileGraphExecutor final
{
public:
    static V4MultiFileOleRunReceipt RunBound(const V4FamilyValidatedContext& verified,
        const event_graph::V4FamilyIdentityContext& identity, const std::vector<bool>& present,
        const event_graph::V4JoinedGraphVariant& selected, const std::vector<Input>& inputs,
        const std::shared_ptr<V4IndependentFilePayload>& payload, const V4IndependentShellSource& binding,
        const std::shared_ptr<V4IndependentShellOwner>& owner, OleDragDropAdapter* adapter,
        const std::shared_ptr<const V3HostEpoch>& hostEpoch, const HostBoundaries& host,
        const std::function<void(const Observation&)>& assertIndependentOleResult,
        const std::function<V2HostCoordinateSnapshot(const Observation&)>& sampler = {},
        const V4BatchObserverBoundaries& batchObserver = {},
        const std::shared_ptr<const V4PinnedCopyLease>& pinnedCopyLease = {},
        const V4CollectionObserverBoundaries& collectionObserver = {},
        const V4GroupObserverBoundaries& groupObserver = {},
        const V4DesktopFilesObserverBoundaries& desktopFilesObserver = {},
        const V4FolderMappingObserverBoundaries& folderMappingObserver = {},
        const V4LeaveObserverBoundaries& leaveObserver = {},
        const V4RejectObserverBoundaries& rejectObserver = {},
        const std::shared_ptr<const V4SharedMoveLease>& sharedMoveLease = {},
        const V4MoveObserverBoundaries& moveObserver = {})
    {
        V4MultiFileOleRunReceipt receipt;
        const auto pinnedPayload = payload;
        const auto pinnedOwner = owner;
        const auto pinnedHostEpoch = hostEpoch;
        const auto pinnedLease = pinnedCopyLease;
        const auto moveLease = sharedMoveLease;
        bool dispatchEntered = false;
        bool moveAfterUnavailable = false;
        bool restrictedDiagnosticAttempt = false;
        try
        {
            receipt.plan_ = BuildV4JoinedOleExecutionPlan(verified, identity, present, selected, inputs);
            receipt.execution_.obligations = receipt.plan_.preconditions;
            auto* source = binding.SourceObject();
            if (!pinnedPayload || !pinnedOwner || !pinnedOwner->Live() || pinnedOwner->payload_ != pinnedPayload || !source ||
                pinnedOwner->source_.Get() != source || !binding.inputPrepared || binding.resourceUnavailable ||
                binding.candidateId != receipt.plan_.candidateId)
                receipt.execution_.obligations.emplace_back("v4_multi_source_owner_binding_unavailable");
            if (pinnedPayload && source)
            {
                receipt.sourcePreparation_ = InspectV4IndependentFileInputs(verified, identity, *pinnedPayload, *source);
                if (!receipt.sourcePreparation_->captured || receipt.sourcePreparation_->resourceUnavailable ||
                    receipt.sourcePreparation_->candidateId != receipt.plan_.candidateId)
                    receipt.execution_.obligations.emplace_back("v4_multi_source_initial_snapshot_unavailable");
                else if (pinnedPayload->files.size() == 1 && pinnedLease)
                {
                    const auto program = verified.TransferProgram();
                    for (std::size_t i = 0; i < program.steps.size(); ++i)
                    {
                        const auto& step = program.steps[i];
                        if (step.kind != "configure_v4_source_input") continue;
                        for (const auto& [axis, value] : receipt.sourcePreparation_->exactSourceAxes)
                            if (step.parameter == axis + "=" + value)
                            { receipt.sourceInputStepsObserved_.push_back(i); break; }
                    }
                }
            }
            if (pinnedLease)
            {
                if (pinnedPayload && pinnedOwner && pinnedPayload->files.size() == 1 &&
                    pinnedOwner->MatchesPinnedCopyLease(verified, identity, pinnedPayload, pinnedLease))
                { receipt.pinnedCopyLease_ = pinnedLease; receipt.owner_ = pinnedOwner; receipt.pinnedCopyPreflight_ = true; }
                else receipt.execution_.obligations.emplace_back("v4_single_pinned_copy_lease_unavailable");
            }
            if (moveLease)
            {
                if (pinnedLease || !pinnedPayload || !pinnedOwner || pinnedPayload->files.size() != 1 ||
                    !pinnedOwner->MatchesSharedMoveLeaseBefore(verified, identity, pinnedPayload, moveLease))
                    receipt.execution_.obligations.emplace_back("v4_shared_move_lease_unavailable");
                else { receipt.sharedMoveLease_ = moveLease; receipt.owner_ = pinnedOwner; }
            }
            if (receipt.plan_.graphIdentityValid && pinnedPayload && pinnedPayload->files.size() == 1 && pinnedOwner &&
                pinnedOwner->Live() && pinnedOwner->payload_ == pinnedPayload && source && pinnedOwner->source_.Get() == source &&
                binding.inputPrepared && !binding.resourceUnavailable && binding.candidateId == receipt.plan_.candidateId &&
                pinnedLease && receipt.pinnedCopyPreflight_ && receipt.sourcePreparation_ &&
                receipt.sourcePreparation_->captured && !receipt.sourcePreparation_->resourceUnavailable &&
                receipt.sourcePreparation_->candidateId == receipt.plan_.candidateId && source->RestrictedPreflightSucceeded() &&
                batchObserver.bindBeforeDrop && batchObserver.observeBusiness && !moveLease &&
                !collectionObserver.bindBeforeDrop && !groupObserver.bindBeforeDrop && !desktopFilesObserver.bindBeforeDrop &&
                !folderMappingObserver.bindBeforeDrop && !leaveObserver.bindBeforeLeave && !rejectObserver.bindBeforeDrop &&
                !moveObserver.bindBeforeDrop)
            {
                std::vector<OlePlanStep> actualSteps;
                if (BuildRestrictedDesktopCopySteps(verified, inputs, actualSteps))
                {
                    // These preconditions describe missing candidate/graph
                    // proof, not actions to synthesize. Preserve them as
                    // pending evidence while allowing an isolated attempt.
                    for (const auto& reason : receipt.plan_.preconditions)
                        receipt.plan_.postDispatchObligations.push_back("v4_restricted_attempt_unverified:" + reason);
                    receipt.plan_.steps = std::move(actualSteps);
                    receipt.plan_.structurallyPlanned = true;
                    receipt.execution_.obligations.clear();
                    restrictedDiagnosticAttempt = true;
                }
            }
            const auto& axes = verified.Axes(); const auto& candidate = verified.Coordinate();
            if (Value(axes, candidate, "V4.source.kind") != "file_only" ||
                Value(axes, candidate, "T01") != "external_ingress" || Value(axes, candidate, "T02.file_paths") != "yes" ||
                Value(axes, candidate, "T04.capability") != "enabled" || !source || !source->asynchronous)
                receipt.execution_.obligations.emplace_back("v4_multi_external_async_source_unimplemented");
            if (!adapter || !source || !pinnedHostEpoch || !pinnedHostEpoch->Matches(source, adapter))
                receipt.execution_.obligations.emplace_back("v4_multi_host_epoch_unavailable");
            if (!host.beforeOleCall || !host.observeOleReturn || !host.awaitProductionCompletionOrRejection ||
                !host.assertIndependentBusinessResults || !assertIndependentOleResult)
                receipt.execution_.obligations.emplace_back("v4_multi_host_observer_or_oracle_missing");
            if (static_cast<bool>(batchObserver.bindBeforeDrop) != static_cast<bool>(batchObserver.observeBusiness))
                receipt.execution_.obligations.emplace_back("v4_multi_business_observer_incomplete");
            if (static_cast<bool>(collectionObserver.bindBeforeDrop) != static_cast<bool>(collectionObserver.observeBusiness) ||
                (batchObserver.bindBeforeDrop && collectionObserver.bindBeforeDrop))
                receipt.execution_.obligations.emplace_back("v4_collection_business_observer_incomplete_or_conflicting");
            if (static_cast<bool>(groupObserver.bindBeforeDrop) != static_cast<bool>(groupObserver.observeBusiness) ||
                (groupObserver.bindBeforeDrop && (batchObserver.bindBeforeDrop || collectionObserver.bindBeforeDrop)))
                receipt.execution_.obligations.emplace_back("v4_group_business_observer_incomplete_or_conflicting");
            if (static_cast<bool>(desktopFilesObserver.bindBeforeDrop) != static_cast<bool>(desktopFilesObserver.observeBusiness) ||
                (desktopFilesObserver.bindBeforeDrop && (batchObserver.bindBeforeDrop || collectionObserver.bindBeforeDrop || groupObserver.bindBeforeDrop)))
                receipt.execution_.obligations.emplace_back("v4_desktop_files_business_observer_incomplete_or_conflicting");
            if (static_cast<bool>(folderMappingObserver.bindBeforeDrop) != static_cast<bool>(folderMappingObserver.observeBusiness) ||
                (folderMappingObserver.bindBeforeDrop && (batchObserver.bindBeforeDrop || collectionObserver.bindBeforeDrop ||
                    groupObserver.bindBeforeDrop || desktopFilesObserver.bindBeforeDrop)))
                receipt.execution_.obligations.emplace_back("v4_folder_mapping_business_observer_incomplete_or_conflicting");
            if (static_cast<bool>(leaveObserver.bindBeforeLeave) != static_cast<bool>(leaveObserver.observeBusiness) ||
                (leaveObserver.bindBeforeLeave && (!pinnedLease || batchObserver.bindBeforeDrop || collectionObserver.bindBeforeDrop ||
                    groupObserver.bindBeforeDrop || desktopFilesObserver.bindBeforeDrop || folderMappingObserver.bindBeforeDrop)))
                receipt.execution_.obligations.emplace_back("v4_leave_business_observer_incomplete_or_conflicting");
            if (leaveObserver.bindBeforeLeave &&
                (Value(axes, candidate, "G04.termination") != "leave" ||
                 std::count_if(inputs.begin(), inputs.end(), [](const Input& input) { return input.kind == Kind::leave; }) != 1 ||
                 std::any_of(inputs.begin(), inputs.end(), [](const Input& input) { return input.kind == Kind::drop; })))
                receipt.execution_.obligations.emplace_back("v4_leave_terminal_input_unavailable");
            if (static_cast<bool>(rejectObserver.bindBeforeDrop) != static_cast<bool>(rejectObserver.observeBusiness) ||
                (rejectObserver.bindBeforeDrop && (!pinnedLease || batchObserver.bindBeforeDrop || collectionObserver.bindBeforeDrop ||
                    groupObserver.bindBeforeDrop || desktopFilesObserver.bindBeforeDrop || folderMappingObserver.bindBeforeDrop ||
                    leaveObserver.bindBeforeLeave)))
                receipt.execution_.obligations.emplace_back("v4_reject_business_observer_incomplete_or_conflicting");
            if (rejectObserver.bindBeforeDrop &&
                (Value(axes, candidate, "T03") != "none" || Value(axes, candidate, "T03.extra_protocol_bits") != "none" ||
                 inputs.size() != 2 || inputs[0].kind != Kind::enter || inputs[1].kind != Kind::drop ||
                 !inputs[0].allowedEffect || *inputs[0].allowedEffect != DROPEFFECT_NONE ||
                 !inputs[1].allowedEffect || *inputs[1].allowedEffect != DROPEFFECT_NONE))
                receipt.execution_.obligations.emplace_back("v4_reject_terminal_input_unavailable");
            if (static_cast<bool>(moveObserver.bindBeforeDrop) != static_cast<bool>(moveObserver.observeBusiness) ||
                (moveObserver.bindBeforeDrop && (!moveLease || pinnedLease || batchObserver.bindBeforeDrop ||
                    collectionObserver.bindBeforeDrop || groupObserver.bindBeforeDrop || desktopFilesObserver.bindBeforeDrop ||
                    folderMappingObserver.bindBeforeDrop || leaveObserver.bindBeforeLeave || rejectObserver.bindBeforeDrop)))
                receipt.execution_.obligations.emplace_back("v4_move_business_observer_incomplete_or_conflicting");
            if (moveLease && !moveObserver.bindBeforeDrop)
                receipt.execution_.obligations.emplace_back("v4_move_business_observer_required");
            if (moveObserver.bindBeforeDrop &&
                (Value(axes, candidate, "T03") != "copy_move" || Value(axes, candidate, "T03.extra_protocol_bits") != "none" ||
                 Value(axes, candidate, "G04.termination") != "drop" || Value(axes, candidate, "G02.release") != "none" ||
                 std::count_if(inputs.begin(), inputs.end(), [](const Input& input) { return input.kind == Kind::drop; }) != 1 ||
                 std::any_of(inputs.begin(), inputs.end(), [](const Input& input) { return input.kind == Kind::leave; })))
                receipt.execution_.obligations.emplace_back("v4_move_terminal_input_unavailable");
            // The shared payload remains mutable and shared-write/delete; its
            // snapshot is not an execution-time source lock or all-axis proof.
            if (!restrictedDiagnosticAttempt)
            {
                receipt.execution_.obligations.emplace_back("v4_multi_payload_lifetime_and_all_axes_unimplemented");
                receipt.execution_.obligations.emplace_back("v4_multi_ordered_source_read_and_graph_presence_unimplemented");
                receipt.execution_.obligations.emplace_back("v4_multi_same_task_completion_and_business_oracle_unimplemented");
            }
            if (!receipt.plan_.graphIdentityValid || !receipt.plan_.structurallyPlanned || !receipt.execution_.obligations.empty())
                return receipt;
            if (!pinnedOwner->TryBeginExecution())
            { receipt.execution_.additionalObservationUnavailable = true; receipt.execution_.obligations.emplace_back("v4_multi_source_owner_busy"); return receipt; }
            const auto endSource = [](const V4IndependentShellOwner* value) noexcept { value->EndExecution(); };
            const std::unique_ptr<const V4IndependentShellOwner, decltype(endSource)> sourceGuard(pinnedOwner.get(), endSource);
            if (!pinnedHostEpoch->TryBeginExecution(source, adapter))
            { receipt.execution_.additionalObservationUnavailable = true; receipt.execution_.obligations.emplace_back("v4_multi_host_epoch_busy"); return receipt; }
            const auto endHost = [](const V3HostEpoch* value) noexcept { value->EndExecution(); };
            const std::unique_ptr<const V3HostEpoch, decltype(endHost)> hostGuard(pinnedHostEpoch.get(), endHost);
            receipt.sourceAtExecution_ = InspectV4IndependentFileInputs(verified, identity, *pinnedPayload, *source);
            const auto& initial = *receipt.sourcePreparation_; const auto& actual = *receipt.sourceAtExecution_;
            const auto sameId = [](const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
                return a.VolumeSerialNumber == b.VolumeSerialNumber &&
                    std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) == 0;
            };
            bool equal = actual.captured && !actual.resourceUnavailable && actual.candidateId == initial.candidateId &&
                actual.ownerThread == initial.ownerThread && actual.sourceAddress == initial.sourceAddress &&
                !actual.sourceLifetime.owner_before(initial.sourceLifetime) && !initial.sourceLifetime.owner_before(actual.sourceLifetime) &&
                actual.root == initial.root && sameId(actual.rootId, initial.rootId) && actual.files.size() == initial.files.size() &&
                actual.exactSourceAxes == initial.exactSourceAxes;
            if (equal) for (std::size_t i = 0; i < actual.files.size(); ++i)
                if (actual.files[i].path != initial.files[i].path || !sameId(actual.files[i].id, initial.files[i].id) ||
                    actual.files[i].bytes != initial.files[i].bytes) { equal = false; break; }
            if (!equal)
            { receipt.execution_.additionalObservationUnavailable = true; receipt.execution_.obligations.emplace_back("v4_multi_source_changed_before_execution"); return receipt; }
            receipt.owner_ = pinnedOwner; receipt.hostEpoch_ = pinnedHostEpoch; receipt.sourceIdentity_ = source;
            receipt.sourceLifetime_ = source->LifetimeIdentity(); receipt.adapterIdentity_ = adapter;
            if (batchObserver.bindBeforeDrop && batchObserver.observeBusiness)
                receipt.executionKey_ = std::shared_ptr<V4BatchExecutionKey>(new V4BatchExecutionKey(
                    pinnedOwner, pinnedHostEpoch, static_cast<IDataObject*>(source), adapter,
                    receipt.plan_.candidateId, receipt.plan_.graphId));
            if (collectionObserver.bindBeforeDrop && collectionObserver.observeBusiness)
                receipt.collectionKey_ = std::shared_ptr<V4CollectionExecutionKey>(new V4CollectionExecutionKey(
                    pinnedOwner, pinnedHostEpoch, static_cast<IDataObject*>(source), adapter,
                    receipt.plan_.candidateId, receipt.plan_.graphId));
            if (groupObserver.bindBeforeDrop && groupObserver.observeBusiness)
                receipt.groupKey_ = std::shared_ptr<V4GroupExecutionKey>(new V4GroupExecutionKey(
                    pinnedOwner, pinnedHostEpoch, static_cast<IDataObject*>(source), adapter,
                    receipt.plan_.candidateId, receipt.plan_.graphId));
            if (desktopFilesObserver.bindBeforeDrop && desktopFilesObserver.observeBusiness)
                receipt.desktopFilesKey_ = std::shared_ptr<V4DesktopFilesExecutionKey>(new V4DesktopFilesExecutionKey(
                    pinnedOwner, pinnedHostEpoch, static_cast<IDataObject*>(source), adapter,
                    receipt.plan_.candidateId, receipt.plan_.graphId));
            if (folderMappingObserver.bindBeforeDrop && folderMappingObserver.observeBusiness)
                receipt.folderMappingKey_ = std::shared_ptr<V4FolderMappingExecutionKey>(new V4FolderMappingExecutionKey(
                    pinnedOwner, pinnedHostEpoch, static_cast<IDataObject*>(source), adapter,
                    receipt.plan_.candidateId, receipt.plan_.graphId));
            if (leaveObserver.bindBeforeLeave && leaveObserver.observeBusiness)
                receipt.leaveKey_ = std::shared_ptr<V4LeaveExecutionKey>(new V4LeaveExecutionKey(
                    pinnedOwner, pinnedLease, pinnedHostEpoch, static_cast<IDataObject*>(source), adapter,
                    receipt.plan_.candidateId, receipt.plan_.graphId));
            if (rejectObserver.bindBeforeDrop && rejectObserver.observeBusiness)
                receipt.rejectKey_ = std::shared_ptr<V4RejectExecutionKey>(new V4RejectExecutionKey(
                    pinnedOwner, pinnedLease, pinnedHostEpoch, static_cast<IDataObject*>(source), adapter,
                    receipt.plan_.candidateId, receipt.plan_.graphId));
            if (moveObserver.bindBeforeDrop && moveObserver.observeBusiness)
                receipt.moveKey_ = std::shared_ptr<V4MoveExecutionKey>(new V4MoveExecutionKey(
                    pinnedOwner, moveLease, pinnedHostEpoch, static_cast<IDataObject*>(source), adapter,
                    receipt.plan_.candidateId, receipt.plan_.graphId));
            const auto requireLive = [&] {
                if (!pinnedOwner->Live() || pinnedOwner->payload_ != pinnedPayload || pinnedOwner->source_.Get() != source ||
                    !pinnedHostEpoch->Matches(source, adapter)) throw ObservationUnavailable("v4_multi_execution_owner_or_host_changed");
            };
            const auto requirePinnedCopy = [&] {
                if (pinnedLease && !pinnedOwner->MatchesPinnedCopyLease(verified, identity, pinnedPayload, pinnedLease))
                    throw ObservationUnavailable("v4_single_pinned_copy_lease_changed");
            };
            const auto requireMoveBefore = [&] {
                if (moveLease && !pinnedOwner->MatchesSharedMoveLeaseBefore(verified, identity, pinnedPayload, moveLease))
                    throw ObservationUnavailable("v4_shared_move_source_changed_before_drop");
            };
            const auto requireMoveOwned = [&] {
                if (moveLease && !pinnedOwner->OwnsSharedMoveLease(pinnedPayload, moveLease))
                    throw ObservationUnavailable("v4_shared_move_owner_or_lease_changed");
            };
            requireLive(); requirePinnedCopy(); requireMoveBefore();
            ole_file_source_fixture::FileDropCallState before;
            requireLive();
            if (!source->ReadFileDropCallState(before) || before.failed || before.depth || before.owner != GetCurrentThreadId())
                throw ObservationUnavailable("v4_multi_source_interval_start_unavailable");
            const HostBoundaries guarded{
                [&](const Input& input) {
                    requireLive(); host.beforeOleCall(input); requireLive();
                    if (input.kind == Kind::drop && receipt.executionKey_)
                    {
                        auto& key = *receipt.executionKey_;
                        if (key.phase_ != V4BatchExecutionKey::Phase::created)
                            throw ObservationUnavailable("v4_multi_execution_duplicate_drop_unavailable");
                        key.phase_ = V4BatchExecutionKey::Phase::before_drop;
                        batchObserver.bindBeforeDrop(receipt.executionKey_);
                        if (!key.bound_) throw ObservationUnavailable("v4_multi_execution_baseline_not_bound");
                        requireLive(); key.phase_ = V4BatchExecutionKey::Phase::dispatching;
                    }
                    if (input.kind == Kind::drop && receipt.rejectKey_)
                    {
                        auto& key = *receipt.rejectKey_;
                        if (key.phase_ != V4RejectExecutionKey::Phase::created)
                            throw ObservationUnavailable("v4_reject_execution_duplicate_drop_unavailable");
                        requirePinnedCopy(); key.phase_ = V4RejectExecutionKey::Phase::before_drop;
                        rejectObserver.bindBeforeDrop(receipt.rejectKey_);
                        if (!key.bound_) throw ObservationUnavailable("v4_reject_execution_baseline_not_bound");
                        requireLive(); requirePinnedCopy(); key.phase_ = V4RejectExecutionKey::Phase::dispatching;
                    }
                    if (input.kind == Kind::drop && receipt.moveKey_)
                    {
                        auto& key = *receipt.moveKey_;
                        if (key.phase_ != V4MoveExecutionKey::Phase::created)
                            throw ObservationUnavailable("v4_move_execution_duplicate_drop_unavailable");
                        requireMoveBefore(); key.phase_ = V4MoveExecutionKey::Phase::before_drop;
                        moveObserver.bindBeforeDrop(receipt.moveKey_);
                        if (!key.bound_) throw ObservationUnavailable("v4_move_execution_baseline_not_bound");
                        requireLive(); requireMoveBefore(); receipt.sharedMoveBeforeDrop_ = true;
                        key.phase_ = V4MoveExecutionKey::Phase::dispatching;
                    }
                    if (input.kind == Kind::drop && receipt.collectionKey_)
                    {
                        auto& key = *receipt.collectionKey_;
                        if (key.phase_ != V4CollectionExecutionKey::Phase::created)
                            throw ObservationUnavailable("v4_collection_execution_duplicate_drop_unavailable");
                        key.phase_ = V4CollectionExecutionKey::Phase::before_drop;
                        collectionObserver.bindBeforeDrop(receipt.collectionKey_);
                        if (!key.bound_) throw ObservationUnavailable("v4_collection_execution_baseline_not_bound");
                        requireLive(); key.phase_ = V4CollectionExecutionKey::Phase::dispatching;
                    }
                    if (input.kind == Kind::drop && receipt.groupKey_)
                    {
                        auto& key = *receipt.groupKey_;
                        if (key.phase_ != V4GroupExecutionKey::Phase::created)
                            throw ObservationUnavailable("v4_group_execution_duplicate_drop_unavailable");
                        key.phase_ = V4GroupExecutionKey::Phase::before_drop;
                        groupObserver.bindBeforeDrop(receipt.groupKey_);
                        if (!key.bound_) throw ObservationUnavailable("v4_group_execution_baseline_not_bound");
                        requireLive(); key.phase_ = V4GroupExecutionKey::Phase::dispatching;
                    }
                    if (input.kind == Kind::drop && receipt.desktopFilesKey_)
                    {
                        auto& key = *receipt.desktopFilesKey_;
                        if (key.phase_ != V4DesktopFilesExecutionKey::Phase::created)
                            throw ObservationUnavailable("v4_desktop_files_execution_duplicate_drop_unavailable");
                        key.phase_ = V4DesktopFilesExecutionKey::Phase::before_drop;
                        desktopFilesObserver.bindBeforeDrop(receipt.desktopFilesKey_);
                        if (!key.bound_) throw ObservationUnavailable("v4_desktop_files_execution_baseline_not_bound");
                        requireLive(); key.phase_ = V4DesktopFilesExecutionKey::Phase::dispatching;
                    }
                    if (input.kind == Kind::drop && receipt.folderMappingKey_)
                    {
                        auto& key = *receipt.folderMappingKey_;
                        if (key.phase_ != V4FolderMappingExecutionKey::Phase::created)
                            throw ObservationUnavailable("v4_folder_mapping_execution_duplicate_drop_unavailable");
                        key.phase_ = V4FolderMappingExecutionKey::Phase::before_drop;
                        folderMappingObserver.bindBeforeDrop(receipt.folderMappingKey_);
                        if (!key.bound_) throw ObservationUnavailable("v4_folder_mapping_execution_baseline_not_bound");
                        requireLive(); key.phase_ = V4FolderMappingExecutionKey::Phase::dispatching;
                    }
                    if (input.kind == Kind::drop && pinnedLease)
                    { requireLive(); requirePinnedCopy(); receipt.pinnedCopyBeforeDrop_ = true; }
                    if (input.kind == Kind::leave && receipt.leaveKey_)
                    {
                        auto& key = *receipt.leaveKey_;
                        if (key.phase_ != V4LeaveExecutionKey::Phase::created)
                            throw ObservationUnavailable("v4_leave_execution_duplicate_leave_unavailable");
                        requirePinnedCopy(); key.phase_ = V4LeaveExecutionKey::Phase::before_leave;
                        leaveObserver.bindBeforeLeave(receipt.leaveKey_);
                        if (!key.bound_) throw ObservationUnavailable("v4_leave_execution_baseline_not_bound");
                        requireLive(); requirePinnedCopy(); receipt.pinnedCopyBeforeLeave_ = true;
                        key.phase_ = V4LeaveExecutionKey::Phase::dispatching;
                    }
                },
                [&](const Observation& event) { requireLive(); host.observeOleReturn(event); requireLive(); },
                [&](const Observation& terminal) {
                    requireLive(); host.awaitProductionCompletionOrRejection(terminal); requireLive();
                    if (pinnedLease) { requirePinnedCopy(); receipt.pinnedCopyAfterAwait_ = true; }
                    if (moveLease)
                    {
                        requireMoveOwned(); receipt.sharedMoveAfterAwait_ = moveLease->ObserveSourceAfter();
                        if (!receipt.sharedMoveAfterAwait_->observed || receipt.sharedMoveAfterAwait_->resourceUnavailable ||
                            !receipt.sharedMoveAfterAwait_->unresolved.empty())
                            moveAfterUnavailable = true;
                    }
                    V3SourceSessionInterval interval; interval.before = before;
                    if (!source->ReadFileDropCallState(interval.after) || interval.after.failed || interval.after.depth ||
                        interval.after.owner != before.owner || interval.after.count < before.count)
                        throw ObservationUnavailable("v4_multi_source_interval_end_unavailable");
                    interval.events.reserve(interval.after.count - before.count);
                    for (std::size_t i = before.count; i < interval.after.count; ++i)
                    {
                        ole_file_source_fixture::FileDropCallEvent event;
                        if (!source->ReadFileDropCallEvent(i, event)) throw ObservationUnavailable("v4_multi_source_interval_record_unavailable");
                        interval.events.push_back(event);
                    }
                    ole_file_source_fixture::FileDropCallState confirmed;
                    if (!source->ReadFileDropCallState(confirmed) || confirmed.failed || confirmed.depth ||
                        confirmed.owner != interval.after.owner || confirmed.count != interval.after.count ||
                        !pinnedHostEpoch->trace_ || pinnedHostEpoch->trace_->LostOrCrossThread() ||
                        pinnedHostEpoch->trace_->OwnerThread() != GetCurrentThreadId())
                        throw ObservationUnavailable("v4_multi_completion_trace_or_interval_unavailable");
                    receipt.sourceSessionInterval_ = std::move(interval);
                    receipt.completionTrace_ = pinnedHostEpoch->trace_->Observations(); receipt.completionTraceCaptured_ = true; requireLive();
                },
                [&] {
                    requireLive();
                    if (receipt.moveKey_)
                    {
                        auto& key = *receipt.moveKey_;
                        if (key.phase_ != V4MoveExecutionKey::Phase::dispatching || !key.bound_ ||
                            !receipt.sourceSessionInterval_ || !receipt.completionTraceCaptured_ || !receipt.sharedMoveAfterAwait_)
                            throw ObservationUnavailable("v4_move_business_execution_scope_unavailable");
                        requireMoveOwned(); key.phase_ = V4MoveExecutionKey::Phase::business;
                        struct CloseMoveScope final { V4MoveExecutionKey& key; ~CloseMoveScope() { key.phase_ = V4MoveExecutionKey::Phase::closed; } } close{key};
                        moveObserver.observeBusiness(receipt.moveKey_); requireLive(); requireMoveOwned();
                        if (!key.published_ || !key.snapshot_)
                            throw ObservationUnavailable("v4_move_business_snapshot_not_sealed");
                        return;
                    }
                    if (receipt.rejectKey_)
                    {
                        auto& key = *receipt.rejectKey_;
                        if (key.phase_ != V4RejectExecutionKey::Phase::dispatching || !key.bound_ ||
                            !receipt.sourceSessionInterval_ || !receipt.completionTraceCaptured_)
                            throw ObservationUnavailable("v4_reject_business_execution_scope_unavailable");
                        key.phase_ = V4RejectExecutionKey::Phase::business;
                        struct CloseRejectScope final { V4RejectExecutionKey& key; ~CloseRejectScope() { key.phase_ = V4RejectExecutionKey::Phase::closed; } } close{key};
                        rejectObserver.observeBusiness(receipt.rejectKey_); requireLive();
                        if (!key.published_ || !key.snapshot_)
                            throw ObservationUnavailable("v4_reject_business_snapshot_not_sealed");
                        return;
                    }
                    if (receipt.leaveKey_)
                    {
                        auto& key = *receipt.leaveKey_;
                        if (key.phase_ != V4LeaveExecutionKey::Phase::dispatching || !key.bound_ ||
                            !receipt.sourceSessionInterval_ || !receipt.completionTraceCaptured_)
                            throw ObservationUnavailable("v4_leave_business_execution_scope_unavailable");
                        key.phase_ = V4LeaveExecutionKey::Phase::business;
                        struct CloseLeaveScope final { V4LeaveExecutionKey& key; ~CloseLeaveScope() { key.phase_ = V4LeaveExecutionKey::Phase::closed; } } close{key};
                        leaveObserver.observeBusiness(receipt.leaveKey_); requireLive();
                        if (!key.published_ || !key.snapshot_)
                            throw ObservationUnavailable("v4_leave_business_snapshot_not_sealed");
                        return;
                    }
                    if (receipt.folderMappingKey_)
                    {
                        auto& key = *receipt.folderMappingKey_;
                        if (key.phase_ != V4FolderMappingExecutionKey::Phase::dispatching || !key.bound_ ||
                            !receipt.sourceSessionInterval_ || !receipt.completionTraceCaptured_)
                            throw ObservationUnavailable("v4_folder_mapping_business_execution_scope_unavailable");
                        key.phase_ = V4FolderMappingExecutionKey::Phase::business;
                        struct CloseFolderMappingScope final { V4FolderMappingExecutionKey& key; ~CloseFolderMappingScope() { key.phase_ = V4FolderMappingExecutionKey::Phase::closed; } } close{key};
                        folderMappingObserver.observeBusiness(receipt.folderMappingKey_); requireLive();
                        if (!key.published_ || !key.snapshot_)
                            throw ObservationUnavailable("v4_folder_mapping_business_snapshot_not_sealed");
                        return;
                    }
                    if (receipt.desktopFilesKey_)
                    {
                        auto& key = *receipt.desktopFilesKey_;
                        if (key.phase_ != V4DesktopFilesExecutionKey::Phase::dispatching || !key.bound_ ||
                            !receipt.sourceSessionInterval_ || !receipt.completionTraceCaptured_)
                            throw ObservationUnavailable("v4_desktop_files_business_execution_scope_unavailable");
                        key.phase_ = V4DesktopFilesExecutionKey::Phase::business;
                        struct CloseDesktopFilesScope final { V4DesktopFilesExecutionKey& key; ~CloseDesktopFilesScope() { key.phase_ = V4DesktopFilesExecutionKey::Phase::closed; } } close{key};
                        desktopFilesObserver.observeBusiness(receipt.desktopFilesKey_); requireLive();
                        if (!key.published_ || !key.snapshot_)
                            throw ObservationUnavailable("v4_desktop_files_business_snapshot_not_sealed");
                        return;
                    }
                    if (receipt.groupKey_)
                    {
                        auto& key = *receipt.groupKey_;
                        if (key.phase_ != V4GroupExecutionKey::Phase::dispatching || !key.bound_ ||
                            !receipt.sourceSessionInterval_ || !receipt.completionTraceCaptured_)
                            throw ObservationUnavailable("v4_group_business_execution_scope_unavailable");
                        key.phase_ = V4GroupExecutionKey::Phase::business;
                        struct CloseGroupScope final { V4GroupExecutionKey& key; ~CloseGroupScope() { key.phase_ = V4GroupExecutionKey::Phase::closed; } } close{key};
                        groupObserver.observeBusiness(receipt.groupKey_); requireLive();
                        if (!key.published_ || !key.snapshot_)
                            throw ObservationUnavailable("v4_group_business_snapshot_not_sealed");
                        return;
                    }
                    if (receipt.collectionKey_)
                    {
                        auto& key = *receipt.collectionKey_;
                        if (key.phase_ != V4CollectionExecutionKey::Phase::dispatching || !key.bound_ ||
                            !receipt.sourceSessionInterval_ || !receipt.completionTraceCaptured_)
                            throw ObservationUnavailable("v4_collection_business_execution_scope_unavailable");
                        key.phase_ = V4CollectionExecutionKey::Phase::business;
                        struct CloseCollectionScope final { V4CollectionExecutionKey& key; ~CloseCollectionScope() { key.phase_ = V4CollectionExecutionKey::Phase::closed; } } close{key};
                        collectionObserver.observeBusiness(receipt.collectionKey_); requireLive();
                        if (!key.published_ || !key.snapshot_)
                            throw ObservationUnavailable("v4_collection_business_snapshot_not_sealed");
                        return;
                    }
                    if (!receipt.executionKey_) { host.assertIndependentBusinessResults(); requireLive(); return; }
                    auto& key = *receipt.executionKey_;
                    if (key.phase_ != V4BatchExecutionKey::Phase::dispatching || !key.bound_ ||
                        !receipt.sourceSessionInterval_ || !receipt.completionTraceCaptured_)
                        throw ObservationUnavailable("v4_multi_business_execution_scope_unavailable");
                    key.phase_ = V4BatchExecutionKey::Phase::business;
                    struct CloseScope final { V4BatchExecutionKey& key; ~CloseScope() { key.phase_ = V4BatchExecutionKey::Phase::closed; } } close{key};
                    batchObserver.observeBusiness(receipt.executionKey_); requireLive();
                    if (!key.published_ || !key.snapshot_)
                        throw ObservationUnavailable("v4_multi_business_snapshot_not_sealed");
                }};
            const std::function<void(const Observation&)> guardedOracle = [&](const Observation& event) {
                requireLive(); assertIndependentOleResult(event); requireLive();
            };
            const std::function<V2HostCoordinateSnapshot(const Observation&)> guardedSampler = sampler
                ? std::function<V2HostCoordinateSnapshot(const Observation&)>{[&](const Observation& event) {
                    requireLive(); auto sample = sampler(event); requireLive(); return sample;
                }} : std::function<V2HostCoordinateSnapshot(const Observation&)>{};
            dispatchEntered = true;
            receipt.execution_ = V2GraphExecutor::DispatchApprovedPlan(receipt.plan_.steps, inputs.size(), adapter,
                static_cast<IDataObject*>(source), guarded, guardedOracle, &guardedSampler, &receipt.captured_, source);
            if (restrictedDiagnosticAttempt)
            {
                receipt.execution_.additionalObservationUnavailable = true;
                receipt.execution_.unattributedBusinessFailureObserved = receipt.execution_.businessFailureObserved;
                receipt.execution_.unattributedEventOracleFailureObserved = receipt.execution_.eventOracleFailureObserved;
                receipt.execution_.businessFailureObserved = false;
                receipt.execution_.eventOracleFailureObserved = false;
                receipt.execution_.status = Status::observation_unavailable;
                receipt.execution_.unclassifiedObligations = receipt.plan_.unclassified;
                receipt.execution_.postconditionsPending = receipt.plan_.postDispatchObligations;
                receipt.execution_.obligations.emplace_back("v4_restricted_attempt_input_graph_and_completion_not_fully_bound");
            }
            if (moveAfterUnavailable) receipt.execution_.additionalObservationUnavailable = true;
            requireLive();
            const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
            if (!replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
                replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
                replay.variant.unresolved != selected.unresolved)
            { receipt.plan_.graphIdentityValid = false; receipt.execution_.additionalObservationUnavailable = true; }
            else if (receipt.executionKey_ && receipt.execution_.productionStarted &&
                receipt.executionKey_->phase_ == V4BatchExecutionKey::Phase::closed && receipt.executionKey_->published_)
                receipt.executionKey_->committed_ = true;
            if (receipt.plan_.graphIdentityValid && receipt.collectionKey_ && receipt.execution_.productionStarted &&
                receipt.collectionKey_->phase_ == V4CollectionExecutionKey::Phase::closed && receipt.collectionKey_->published_)
                receipt.collectionKey_->committed_ = true;
            if (receipt.plan_.graphIdentityValid && receipt.groupKey_ && receipt.execution_.productionStarted &&
                receipt.groupKey_->phase_ == V4GroupExecutionKey::Phase::closed && receipt.groupKey_->published_)
                receipt.groupKey_->committed_ = true;
            if (receipt.plan_.graphIdentityValid && receipt.desktopFilesKey_ && receipt.execution_.productionStarted &&
                receipt.desktopFilesKey_->phase_ == V4DesktopFilesExecutionKey::Phase::closed && receipt.desktopFilesKey_->published_)
                receipt.desktopFilesKey_->committed_ = true;
            if (receipt.plan_.graphIdentityValid && receipt.folderMappingKey_ && receipt.execution_.productionStarted &&
                receipt.folderMappingKey_->phase_ == V4FolderMappingExecutionKey::Phase::closed && receipt.folderMappingKey_->published_)
                receipt.folderMappingKey_->committed_ = true;
            if (receipt.plan_.graphIdentityValid && receipt.leaveKey_ && receipt.execution_.productionStarted &&
                receipt.leaveKey_->phase_ == V4LeaveExecutionKey::Phase::closed && receipt.leaveKey_->published_)
                receipt.leaveKey_->committed_ = true;
            if (receipt.plan_.graphIdentityValid && receipt.rejectKey_ && receipt.execution_.productionStarted &&
                receipt.rejectKey_->phase_ == V4RejectExecutionKey::Phase::closed && receipt.rejectKey_->published_)
                receipt.rejectKey_->committed_ = true;
            if (receipt.plan_.graphIdentityValid && receipt.moveKey_ && receipt.execution_.productionStarted &&
                receipt.moveKey_->phase_ == V4MoveExecutionKey::Phase::closed && receipt.moveKey_->published_)
                receipt.moveKey_->committed_ = true;
        }
        catch (const std::exception& error)
        {
            if (dispatchEntered && !receipt.execution_.productionStarted) receipt.execution_.productionEntryUnknown = true;
            receipt.plan_.graphIdentityValid = false; receipt.execution_.additionalObservationUnavailable = true;
            if (receipt.execution_.status != Status::event_failed && receipt.execution_.status != Status::event_oracle_failed && receipt.execution_.status != Status::business_failed)
                receipt.execution_.status = Status::observation_unavailable;
            try { if (receipt.execution_.failure.empty()) receipt.execution_.failure = error.what(); } catch (...) {}
        }
        catch (...)
        {
            if (dispatchEntered && !receipt.execution_.productionStarted) receipt.execution_.productionEntryUnknown = true;
            receipt.plan_.graphIdentityValid = false; receipt.execution_.additionalObservationUnavailable = true;
            if (receipt.execution_.status != Status::event_failed && receipt.execution_.status != Status::event_oracle_failed && receipt.execution_.status != Status::business_failed)
                receipt.execution_.status = Status::observation_unavailable;
            try { if (receipt.execution_.failure.empty()) receipt.execution_.failure = "unknown_v4_multi_execution_exception"; } catch (...) {}
        }
        return receipt;
    }
};

// Records only the host's actual leave lifecycle. A matching timer is a local
// observation, not proof of global quiescence, output absence or business success.
class V4LeaveHostObservation final
{
public:
    struct Record { std::size_t index = 0; snowdesktop::external_transfer_observation::Event event; };
    bool immediateFinalizeObserved = false, graceTimerFinalizeObserved = false;
    bool observationUnavailable = true, resourceUnavailable = false;
    std::vector<Record> records;
    std::optional<MessageObservation> timerMessage;
    std::vector<std::string> unresolved;

    static V4LeaveHostObservation Inspect(const V4MultiFileOleRunReceipt& receipt,
        const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
        const std::shared_ptr<V4IndependentFilePayload>& payload, const V4IndependentShellSource& binding,
        const std::shared_ptr<V4IndependentShellOwner>& owner, const std::shared_ptr<const V4PinnedCopyLease>& lease,
        OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
        const MessageTrace& trace, HWND control)
    {
        namespace diagnostic = snowdesktop::external_transfer_observation;
        V4LeaveHostObservation out;
        try
        {
            if (!epoch || !receipt.SameBoundHost(binding, owner, adapter, epoch, trace, control) ||
                !receipt.SamePinnedCopyLease(verified, identity, payload, owner, lease) ||
                !receipt.PinnedCopyBeforeLeaveObserved() || !receipt.SourceSessionInterval() ||
                !receipt.CompletionTraceCaptured() || !diagnostic::Complete())
            { out.unresolved.emplace_back("v4_leave_host_receipt_or_log_unavailable"); return out; }
            const V2CapturedOleReturn* leave = nullptr;
            for (const auto& call : receipt.CapturedReturns())
                if (call.observation.kind == Kind::leave)
                { if (leave) { out.unresolved.emplace_back("v4_leave_host_multiple_calls"); return out; } leave = &call; }
            using AdapterKind = snowdesktop::ole_adapter_observation::Kind;
            using AdapterPhase = snowdesktop::ole_adapter_observation::Phase;
            if (!leave || leave->entered.kind != AdapterKind::leave || leave->returned.kind != AdapterKind::leave ||
                leave->entered.phase != AdapterPhase::entered || leave->returned.phase != AdapterPhase::returned ||
                !leave->entered.session || !leave->entered.call || leave->entered.parent || leave->entered.depth ||
                leave->entered.adapter != reinterpret_cast<std::uintptr_t>(adapter) ||
                leave->entered.thread != trace.OwnerThread() || leave->returned.session != leave->entered.session ||
                leave->returned.call != leave->entered.call || leave->returned.adapter != leave->entered.adapter ||
                leave->returned.thread != leave->entered.thread || leave->returned.result != leave->observation.result)
            { out.unresolved.emplace_back("v4_leave_host_adapter_scope_unavailable"); return out; }
            diagnostic::ReadSession session;
            if (!session.Open()) { out.unresolved.emplace_back("v4_leave_host_session_unavailable"); return out; }
            for (std::size_t i = 0; i < session.Count(); ++i)
            {
                diagnostic::Event event;
                if (!session.ReadEvent(i, event)) { out.unresolved.emplace_back("v4_leave_host_log_unavailable"); return out; }
                if (event.phase >= diagnostic::Phase::ole_leave_begin && event.phase <= diagnostic::Phase::ole_leave_finalize_return)
                    out.records.push_back({i, event});
            }
            if (out.records.size() != 5)
            { out.unresolved.emplace_back("v4_leave_host_lifecycle_ambiguous"); return out; }
            const auto& begin = out.records[0].event; const auto& arm = out.records[1].event;
            const auto& finalEnter = out.records[2].event; const auto& cancel = out.records[3].event;
            const auto& finalReturn = out.records[4].event;
            if (begin.phase != diagnostic::Phase::ole_leave_begin || arm.phase != diagnostic::Phase::ole_leave_armed ||
                finalEnter.phase != diagnostic::Phase::ole_leave_finalize_enter || cancel.phase != diagnostic::Phase::ole_leave_cancel ||
                finalReturn.phase != diagnostic::Phase::ole_leave_finalize_return || !begin.task ||
                arm.task != begin.task || finalEnter.task != begin.task || cancel.task != begin.task || finalReturn.task != begin.task)
            { out.unresolved.emplace_back("v4_leave_host_phase_or_identity_ambiguous"); return out; }
            const auto sameHost = [&](const diagnostic::Event& event) {
                return event.object == epoch->app_ && event.thread == trace.OwnerThread();
            };
            const auto sameLeaveScope = [&](const diagnostic::Event& event) {
                const auto& scope = event.adapterScope;
                return sameHost(event) && scope.observed && scope.session == leave->entered.session &&
                    scope.call == leave->entered.call && scope.parent == leave->entered.parent &&
                    scope.adapter == leave->entered.adapter && scope.thread == leave->entered.thread &&
                    scope.depth == leave->entered.depth && scope.kind == static_cast<unsigned>(AdapterKind::leave);
            };
            if (!sameLeaveScope(begin) || !sameLeaveScope(arm) || !sameHost(finalEnter) || !sameHost(cancel) ||
                !sameHost(finalReturn) || finalEnter.value != 1 || cancel.value != 1 || finalEnter.extra != arm.extra ||
                cancel.extra != arm.extra || (arm.value != 0 && arm.value != 1))
            { out.unresolved.emplace_back("v4_leave_host_scope_or_deadline_unavailable"); return out; }
            if (arm.value == 0 && arm.extra == 0 && sameLeaveScope(finalEnter) && sameLeaveScope(cancel) && sameLeaveScope(finalReturn))
                out.immediateFinalizeObserved = true;
            else if (arm.value == 1 && arm.extra && !finalEnter.adapterScope.observed &&
                !cancel.adapterScope.observed && !finalReturn.adapterScope.observed && finalEnter.windowScope &&
                finalEnter.windowScope == cancel.windowScope && cancel.windowScope == finalReturn.windowScope)
            {
                for (const auto& message : receipt.CompletionTrace())
                    if (message.windowScope == finalEnter.windowScope)
                    {
                        if (out.timerMessage) { out.unresolved.emplace_back("v4_leave_host_timer_scope_ambiguous"); return out; }
                        out.timerMessage = message;
                    }
                if (out.timerMessage && out.timerMessage->route == MessageRoute::desktop_window_proc &&
                    epoch->IsDesktopWindow(out.timerMessage->window) && out.timerMessage->message == WM_TIMER &&
                    out.timerMessage->wParam == kExternalOleDragLeaveGraceTimerId && out.timerMessage->returned &&
                    out.timerMessage->enteredSequence && out.timerMessage->returnedSequence > out.timerMessage->enteredSequence &&
                    out.timerMessage->enteredThread == trace.OwnerThread() && out.timerMessage->returnedThread == trace.OwnerThread())
                    out.graceTimerFinalizeObserved = true;
            }
            if (!out.immediateFinalizeObserved && !out.graceTimerFinalizeObserved)
                out.unresolved.emplace_back("v4_leave_host_completion_boundary_unavailable");
            out.unresolved.emplace_back("v4_leave_host_uninstrumented_reentry_late_work_and_business_unverified");
        }
        catch (const std::bad_alloc&) { out.resourceUnavailable = true; }
        catch (...) { out.observationUnavailable = true; }
        return out;
    }
};

class V4DirectoryOleRunReceipt final
{
public:
    V4DirectoryOleRunReceipt(V4DirectoryOleRunReceipt&&) = default;
    V4DirectoryOleRunReceipt& operator=(V4DirectoryOleRunReceipt&&) = delete;
    V4DirectoryOleRunReceipt(const V4DirectoryOleRunReceipt&) = delete;
    V4DirectoryOleRunReceipt& operator=(const V4DirectoryOleRunReceipt&) = delete;
    const Result& Execution() const noexcept { return execution_; }
    const V4JoinedOleExecutionPlan& Plan() const noexcept { return plan_; }
    std::shared_ptr<const ::behavior::V4DirectoryCopyEvidence> SealedDirectoryBusiness() const noexcept
    {
        return execution_.productionStarted && plan_.graphIdentityValid && executionKey_ &&
            executionKey_->phase_ == V4DirectoryExecutionKey::Phase::closed &&
            executionKey_->published_ && executionKey_->committed_ ? executionKey_->snapshot_ : nullptr;
    }
    const std::optional<directory_fixture::DirectoryCopyLeaseObservation>& SourcePreparation() const noexcept { return sourcePreparation_; }
    const std::optional<directory_fixture::DirectoryCopyLeaseObservation>& SourceAtExecution() const noexcept { return sourceAtExecution_; }
    const std::vector<V2CapturedOleReturn>& CapturedReturns() const noexcept { return captured_; }
    const std::optional<V3SourceSessionInterval>& SourceSessionInterval() const noexcept { return sourceSessionInterval_; }
    bool CompletionTraceCaptured() const noexcept { return completionTraceCaptured_; }
    const std::vector<MessageObservation>& CompletionTrace() const noexcept { return completionTrace_; }
    bool SameBoundHost(const std::shared_ptr<directory_fixture::DirectoryCopyLease>& lease,
        const directory_fixture::DirectoryShellBinding& binding,
        const std::shared_ptr<directory_fixture::DirectoryShellOwner>& owner, OleDragDropAdapter* adapter,
        const std::shared_ptr<const V3HostEpoch>& epoch, const MessageTrace& trace, HWND control) const noexcept
    {
        auto* source = binding.SourceObject();
        if (!execution_.productionStarted || !plan_.graphIdentityValid || !lease || lease_.lock() != lease ||
            !source || sourceIdentity_ != source || !owner || owner_.lock() != owner || binding.candidateId != plan_.candidateId ||
            adapterIdentity_ != adapter || !epoch || hostEpoch_.lock() != epoch ||
            lease->CandidateId() != plan_.candidateId || lease->OwnerThread() != GetCurrentThreadId()) return false;
        const auto current = source->LifetimeIdentity();
        return !sourceLifetime_.expired() && !current.expired() && !sourceLifetime_.owner_before(current) &&
            !current.owner_before(sourceLifetime_) &&
            epoch->Matches(source, adapter, &trace, control);
    }
private:
    friend class V4DirectoryGraphExecutor;
    V4DirectoryOleRunReceipt() = default;
    Result execution_;
    V4JoinedOleExecutionPlan plan_;
    std::optional<directory_fixture::DirectoryCopyLeaseObservation> sourcePreparation_, sourceAtExecution_;
    std::vector<V2CapturedOleReturn> captured_;
    std::optional<V3SourceSessionInterval> sourceSessionInterval_;
    std::vector<MessageObservation> completionTrace_;
    bool completionTraceCaptured_ = false;
    std::weak_ptr<directory_fixture::DirectoryCopyLease> lease_;
    std::weak_ptr<directory_fixture::DirectoryShellOwner> owner_;
    std::weak_ptr<const V3HostEpoch> hostEpoch_;
    std::weak_ptr<const ole_file_source_fixture::Source::LifetimeToken> sourceLifetime_;
    const ole_file_source_fixture::Source* sourceIdentity_ = nullptr;
    OleDragDropAdapter* adapterIdentity_ = nullptr;
    std::shared_ptr<V4DirectoryExecutionKey> executionKey_;
};

class V4DirectoryGraphExecutor final
{
public:
    static V4DirectoryOleRunReceipt RunBound(const V4FamilyDescriptor& family,
        const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
        const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
        const std::vector<Input>& inputs, const std::shared_ptr<directory_fixture::DirectoryCopyLease>& lease,
        const directory_fixture::DirectoryShellBinding& binding,
        const std::shared_ptr<directory_fixture::DirectoryShellOwner>& owner, OleDragDropAdapter* adapter,
        const std::shared_ptr<const V3HostEpoch>& hostEpoch, const HostBoundaries& host,
        const std::function<void(const Observation&)>& assertIndependentOleResult,
        const std::function<V2HostCoordinateSnapshot(const Observation&)>& sampler = {},
        const V4DirectoryObserverBoundaries& directoryObserver = {})
    {
        V4DirectoryOleRunReceipt receipt;
        // Keep the exact lease alive even if a host callback resets the
        // caller's shared_ptr during synchronous or nested dispatch.
        const auto pinnedLease = lease;
        const auto pinnedOwner = owner;
        const auto pinnedHostEpoch = hostEpoch;
        bool dispatchEntered = false;
        try
        {
            receipt.plan_ = BuildV4JoinedOleExecutionPlan(verified, identity, present, selected, inputs);
            receipt.execution_.obligations = receipt.plan_.preconditions;
            auto* const sourcePointer = binding.SourceObject();
            if (!pinnedOwner || !pinnedOwner->Live() || !pinnedOwner->retainedLease_ ||
                *pinnedOwner->retainedLease_ != pinnedLease || pinnedOwner->source_.Get() != sourcePointer ||
                !sourcePointer || !binding.inputPrepared || binding.resourceUnavailable ||
                binding.candidateId != receipt.plan_.candidateId)
            { receipt.execution_.obligations.emplace_back("v4_directory_source_owner_binding_unavailable"); return receipt; }
            auto& source = *sourcePointer;
            receipt.sourcePreparation_ = directory_fixture::RevalidateDirectoryCopyLease(family, verified, identity, pinnedLease);
            if (!receipt.sourcePreparation_->inputObserved || receipt.sourcePreparation_->resourceUnavailable ||
                !receipt.sourcePreparation_->fixtureLeaseOverlapObserved || receipt.sourcePreparation_->candidateId != receipt.plan_.candidateId)
                receipt.execution_.obligations.emplace_back("v4_directory_source_initial_lease_unavailable");
            auto expectedPath = pinnedLease ? pinnedLease->Source() : std::filesystem::path{}; expectedPath.make_preferred();
            const auto& axes = verified.Axes(); const auto& candidate = verified.Coordinate();
            ole_file_source_fixture::FileDropCallState before;
            APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
            if (!pinnedLease || pinnedLease->OwnerThread() != GetCurrentThreadId() || !source.HasConstructionShell() ||
                source.ConstructionPaths().size() != 1 || source.ConstructionPaths().front() != expectedPath ||
                source.owner != GetCurrentThreadId() || source.wrongApartment || !source.asynchronous ||
                FAILED(CoGetApartmentType(&apartment, &qualifier)) ||
                (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA) ||
                source.fileDropObservationFailed || source.formatProbeObservationFailed || source.fileReads ||
                source.dataReads || source.identityReads || source.starts || source.ends || !source.fileDropReads.empty() ||
                !source.formatProbes.empty() || !source.multiParentPaths.empty() ||
                !source.ReadFileDropCallState(before) || before.failed || before.depth || before.count ||
                Value(axes, candidate, "V4.source.kind") != "file_only" ||
                Value(axes, candidate, "V4.file.member0_kind") != "directory" ||
                Value(axes, candidate, "T01") != "external_ingress" || Value(axes, candidate, "T02.file_paths") != "yes" ||
                Value(axes, candidate, "T04.capability") != "enabled")
                receipt.execution_.obligations.emplace_back("v4_directory_shell_source_binding_unavailable");
            if (!adapter || !pinnedHostEpoch || !pinnedHostEpoch->Matches(static_cast<IDataObject*>(&source), adapter))
                receipt.execution_.obligations.emplace_back("v4_directory_host_epoch_unavailable");
            if (!host.beforeOleCall || !host.observeOleReturn || !host.awaitProductionCompletionOrRejection ||
                !host.assertIndependentBusinessResults || !assertIndependentOleResult)
                receipt.execution_.obligations.emplace_back("v4_directory_host_observer_or_oracle_missing");
            if (static_cast<bool>(directoryObserver.bindBeforeDrop) != static_cast<bool>(directoryObserver.observeBusiness))
                receipt.execution_.obligations.emplace_back("v4_directory_business_observer_incomplete");
            receipt.execution_.obligations.emplace_back("v4_directory_all_axes_and_live_geometry_unimplemented");
            receipt.execution_.obligations.emplace_back("v4_directory_source_presence_and_background_causality_unimplemented");
            receipt.execution_.obligations.emplace_back("v4_directory_same_task_completion_and_tree_business_oracle_unimplemented");
            if (!receipt.plan_.graphIdentityValid || !receipt.plan_.structurallyPlanned || !receipt.execution_.obligations.empty())
                return receipt;
            if (!pinnedOwner->TryBeginExecution())
            { receipt.execution_.additionalObservationUnavailable = true; receipt.execution_.obligations.emplace_back("v4_directory_source_owner_busy"); return receipt; }
            const auto endOwner = [](const directory_fixture::DirectoryShellOwner* value) noexcept { value->EndExecution(); };
            const std::unique_ptr<const directory_fixture::DirectoryShellOwner, decltype(endOwner)> ownerGuard(pinnedOwner.get(), endOwner);
            if (!pinnedHostEpoch->TryBeginExecution(static_cast<IDataObject*>(&source), adapter))
            { receipt.execution_.additionalObservationUnavailable = true; receipt.execution_.obligations.emplace_back("v4_directory_host_epoch_busy"); return receipt; }
            const auto endHost = [](const V3HostEpoch* value) noexcept { value->EndExecution(); };
            const std::unique_ptr<const V3HostEpoch, decltype(endHost)> hostGuard(pinnedHostEpoch.get(), endHost);
            receipt.sourceAtExecution_ = directory_fixture::RevalidateDirectoryCopyLease(family, verified, identity, pinnedLease);
            if (!receipt.sourceAtExecution_->inputObserved || receipt.sourceAtExecution_->resourceUnavailable ||
                !receipt.sourceAtExecution_->fixtureLeaseOverlapObserved ||
                receipt.sourceAtExecution_->candidateId != receipt.sourcePreparation_->candidateId)
            { receipt.execution_.additionalObservationUnavailable = true; receipt.execution_.obligations.emplace_back("v4_directory_source_changed_before_execution"); return receipt; }
            if (!source.HasConstructionShell() || source.ConstructionPaths().size() != 1 ||
                source.ConstructionPaths().front() != expectedPath || source.wrongApartment || source.fileDropObservationFailed ||
                source.formatProbeObservationFailed || source.fileReads || source.dataReads || source.identityReads ||
                source.starts || source.ends || !source.fileDropReads.empty() || !source.formatProbes.empty() ||
                !source.multiParentPaths.empty() || !source.ReadFileDropCallState(before) || before.failed || before.depth || before.count)
            { receipt.execution_.additionalObservationUnavailable = true; receipt.execution_.obligations.emplace_back("v4_directory_shell_changed_before_execution"); return receipt; }
            receipt.lease_ = pinnedLease; receipt.owner_ = pinnedOwner; receipt.hostEpoch_ = pinnedHostEpoch; receipt.sourceIdentity_ = &source;
            receipt.sourceLifetime_ = source.LifetimeIdentity(); receipt.adapterIdentity_ = adapter;
            if (directoryObserver.bindBeforeDrop && directoryObserver.observeBusiness)
                receipt.executionKey_ = std::shared_ptr<V4DirectoryExecutionKey>(new V4DirectoryExecutionKey(
                    pinnedOwner, pinnedLease, pinnedHostEpoch, static_cast<IDataObject*>(&source), adapter,
                    receipt.plan_.candidateId, receipt.plan_.graphId));
            const auto requireLive = [&] {
                APTTYPE currentApartment{}; APTTYPEQUALIFIER currentQualifier{};
                if (!pinnedLease || pinnedLease->OwnerThread() != GetCurrentThreadId() || pinnedLease->CandidateId() != receipt.plan_.candidateId ||
                    !pinnedOwner->Live() || !pinnedOwner->retainedLease_ || *pinnedOwner->retainedLease_ != pinnedLease ||
                    pinnedOwner->source_.Get() != &source ||
                    FAILED(CoGetApartmentType(&currentApartment, &currentQualifier)) ||
                    (currentApartment != APTTYPE_STA && currentApartment != APTTYPE_MAINSTA) ||
                    !pinnedHostEpoch->Matches(static_cast<IDataObject*>(&source), adapter))
                    throw ObservationUnavailable("v4_directory_execution_lease_or_host_changed");
            };
            requireLive();
            if (!source.ReadFileDropCallState(before) || before.failed || before.depth || before.owner != GetCurrentThreadId())
                throw ObservationUnavailable("v4_directory_source_interval_start_unavailable");
            const HostBoundaries guarded{
                [&](const Input& input) {
                    requireLive(); host.beforeOleCall(input); requireLive();
                    if (input.kind == Kind::drop && receipt.executionKey_)
                    {
                        auto& key = *receipt.executionKey_;
                        if (key.phase_ != V4DirectoryExecutionKey::Phase::created)
                            throw ObservationUnavailable("v4_directory_execution_duplicate_drop_unavailable");
                        key.phase_ = V4DirectoryExecutionKey::Phase::before_drop;
                        directoryObserver.bindBeforeDrop(receipt.executionKey_);
                        if (!key.bound_) throw ObservationUnavailable("v4_directory_execution_baseline_not_bound");
                        requireLive(); key.phase_ = V4DirectoryExecutionKey::Phase::dispatching;
                    }
                },
                [&](const Observation& event) { requireLive(); host.observeOleReturn(event); requireLive(); },
                [&](const Observation& terminal) {
                    requireLive(); host.awaitProductionCompletionOrRejection(terminal); requireLive();
                    V3SourceSessionInterval interval; interval.before = before;
                    if (!source.ReadFileDropCallState(interval.after) || interval.after.failed || interval.after.depth ||
                        interval.after.owner != before.owner || interval.after.count < before.count)
                        throw ObservationUnavailable("v4_directory_source_interval_end_unavailable");
                    interval.events.reserve(interval.after.count - before.count);
                    for (std::size_t i = before.count; i < interval.after.count; ++i)
                    {
                        ole_file_source_fixture::FileDropCallEvent event;
                        if (!source.ReadFileDropCallEvent(i, event)) throw ObservationUnavailable("v4_directory_source_interval_record_unavailable");
                        interval.events.push_back(event);
                    }
                    ole_file_source_fixture::FileDropCallState confirmed;
                    if (!source.ReadFileDropCallState(confirmed) || confirmed.failed || confirmed.depth ||
                        confirmed.owner != interval.after.owner || confirmed.count != interval.after.count ||
                        !pinnedHostEpoch->trace_ || pinnedHostEpoch->trace_->LostOrCrossThread() ||
                        pinnedHostEpoch->trace_->OwnerThread() != GetCurrentThreadId())
                        throw ObservationUnavailable("v4_directory_completion_trace_or_interval_unavailable");
                    receipt.sourceSessionInterval_ = std::move(interval);
                    receipt.completionTrace_ = pinnedHostEpoch->trace_->Observations(); receipt.completionTraceCaptured_ = true; requireLive();
                },
                [&] {
                    requireLive();
                    if (!receipt.executionKey_) { host.assertIndependentBusinessResults(); requireLive(); return; }
                    auto& key = *receipt.executionKey_;
                    if (key.phase_ != V4DirectoryExecutionKey::Phase::dispatching || !key.bound_ ||
                        !receipt.sourceSessionInterval_ || !receipt.completionTraceCaptured_)
                        throw ObservationUnavailable("v4_directory_business_execution_scope_unavailable");
                    key.phase_ = V4DirectoryExecutionKey::Phase::business;
                    struct CloseDirectoryScope final { V4DirectoryExecutionKey& key; ~CloseDirectoryScope() { key.phase_ = V4DirectoryExecutionKey::Phase::closed; } } close{key};
                    directoryObserver.observeBusiness(receipt.executionKey_); requireLive();
                    if (!key.published_ || !key.snapshot_)
                        throw ObservationUnavailable("v4_directory_business_snapshot_not_sealed");
                }};
            const std::function<void(const Observation&)> guardedOracle = [&](const Observation& event) {
                requireLive(); assertIndependentOleResult(event); requireLive();
            };
            const std::function<V2HostCoordinateSnapshot(const Observation&)> guardedSampler = sampler
                ? std::function<V2HostCoordinateSnapshot(const Observation&)>{[&](const Observation& event) {
                    requireLive(); auto sample = sampler(event); requireLive(); return sample;
                }} : std::function<V2HostCoordinateSnapshot(const Observation&)>{};
            dispatchEntered = true;
            receipt.execution_ = V2GraphExecutor::DispatchApprovedPlan(receipt.plan_.steps, inputs.size(), adapter,
                static_cast<IDataObject*>(&source), guarded, guardedOracle, &guardedSampler, &receipt.captured_, &source);
            requireLive();
            const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
            if (!replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
                replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
                replay.variant.unresolved != selected.unresolved)
            { receipt.plan_.graphIdentityValid = false; receipt.execution_.additionalObservationUnavailable = true; }
            else if (receipt.executionKey_ && receipt.execution_.productionStarted &&
                receipt.executionKey_->phase_ == V4DirectoryExecutionKey::Phase::closed && receipt.executionKey_->published_)
                receipt.executionKey_->committed_ = true;
        }
        catch (const std::exception& error)
        {
            if (dispatchEntered && !receipt.execution_.productionStarted) receipt.execution_.productionEntryUnknown = true;
            receipt.plan_.graphIdentityValid = false; receipt.execution_.additionalObservationUnavailable = true;
            if (receipt.execution_.status != Status::event_failed && receipt.execution_.status != Status::event_oracle_failed && receipt.execution_.status != Status::business_failed)
                receipt.execution_.status = Status::observation_unavailable;
            try { if (receipt.execution_.failure.empty()) receipt.execution_.failure = error.what(); } catch (...) {}
        }
        catch (...)
        {
            if (dispatchEntered && !receipt.execution_.productionStarted) receipt.execution_.productionEntryUnknown = true;
            receipt.plan_.graphIdentityValid = false; receipt.execution_.additionalObservationUnavailable = true;
            if (receipt.execution_.status != Status::event_failed && receipt.execution_.status != Status::event_oracle_failed && receipt.execution_.status != Status::business_failed)
                receipt.execution_.status = Status::observation_unavailable;
            try { if (receipt.execution_.failure.empty()) receipt.execution_.failure = "unknown_v4_directory_execution_exception"; } catch (...) {}
        }
        return receipt;
    }
};

// A successful provider return can be compared to the exact source path,
// but this is not a host parse, task attribution, selected-presence verdict,
// completion signal, or a business result. Preserve raw records even when a
// later observation is unavailable.
struct V4FilePayloadObservation
{
    bool identityValid = false, observationUnavailable = false;
    bool successfulPayloadObserved = false, candidatePathMismatchObserved = false;
    int sourceStarts = 0, sourceEnds = 0, fileReads = 0;
    HRESULT endOutcome = E_PENDING;
    DWORD endEffect = 0xffffffff;
    std::filesystem::path expectedPath;
    std::vector<ole_file_source_fixture::FileDropRead> reads;
    std::vector<ole_file_source_fixture::FormatProbe> probes;
    // When present, only this prefix is known to be fixture preflight;
    // later entries are not automatically attributed to production.
    std::optional<std::size_t> setupProbePrefixCount;
    std::vector<std::string> unresolved;
};

inline V4FilePayloadObservation InspectV4FilePayload(const V4FileOleRunReceipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    const ole_file_source_fixture::Source& source, OleDragDropAdapter* adapter,
    const std::shared_ptr<const V3HostEpoch>& epoch, const MessageTrace& trace, HWND control)
{
    V4FilePayloadObservation result;
    try
    {
        if (!receipt.SameBoundHost(source, adapter, epoch, trace, control) || !receipt.SourceAtExecution() ||
            !receipt.SourceAtExecution()->captured || source.owner != GetCurrentThreadId())
        { result.observationUnavailable = true; result.unresolved.emplace_back("v4_file_payload_receipt_source_or_epoch_unavailable"); return result; }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || raw.id != receipt.Plan().candidateId ||
            raw.id != receipt.SourceAtExecution()->candidateId || selected.id != receipt.Plan().graphId ||
            !replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
            replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
            replay.variant.unresolved != selected.unresolved)
        { result.observationUnavailable = true; result.unresolved.emplace_back("v4_file_payload_candidate_or_graph_unavailable"); return result; }
        result.expectedPath = receipt.SourceAtExecution()->file;
        result.sourceStarts = source.starts; result.sourceEnds = source.ends; result.fileReads = source.fileReads;
        result.endOutcome = source.outcome; result.endEffect = source.endEffect;
        result.reads = source.fileDropReads; result.probes = source.formatProbes;
        result.identityValid = true;
        if (source.wrongApartment || source.fileDropObservationFailed || source.formatProbeObservationFailed ||
            source.fileReads < 0 || result.reads.size() != static_cast<std::size_t>(source.fileReads))
        { result.observationUnavailable = true; result.unresolved.emplace_back("v4_file_payload_source_log_incomplete"); }
        for (const auto& read : result.reads)
        {
            if (read.threadId != source.owner || read.startsAtEntry <= read.endsAtEntry)
            { result.observationUnavailable = true; result.unresolved.emplace_back("v4_file_payload_source_thread_or_lease_unavailable"); continue; }
            if (FAILED(read.result)) continue;
            if (read.capture != ole_file_source_fixture::FileDropCapture::captured)
            { result.observationUnavailable = true; result.unresolved.emplace_back("v4_file_payload_medium_unavailable"); continue; }
            result.successfulPayloadObserved = true;
            if (read.paths.size() != 1) { result.candidatePathMismatchObserved = true; continue; }
            const int compared = CompareStringOrdinal(read.paths.front().c_str(), -1, result.expectedPath.c_str(), -1, FALSE);
            if (!compared)
            { result.observationUnavailable = true; result.unresolved.emplace_back("v4_file_payload_path_comparison_unavailable"); }
            else if (compared != CSTR_EQUAL)
            { result.observationUnavailable = true; result.unresolved.emplace_back("v4_file_payload_path_spelling_or_alias_unavailable"); }
        }
        if (!receipt.SourceSessionInterval())
        { result.observationUnavailable = true; result.unresolved.emplace_back("v4_file_payload_session_interval_unavailable"); }
        result.unresolved.emplace_back("v4_file_payload_caller_abi_and_selected_occurrence_unresolved");
        result.unresolved.emplace_back("v4_file_payload_host_task_completion_and_business_unresolved");
        // Same-thread wrapper observations are not a closed producer interval;
        // unrelated reentry and asynchronous later reads remain possible.
        result.observationUnavailable = true;
    }
    catch (const std::exception&)
    { result.observationUnavailable = true; }
    catch (...)
    { result.observationUnavailable = true; }
    return result;
}

struct PassiveTaskWindowCandidate
{
    std::uint64_t work = 0;
    std::size_t traceIndex = MessageTrace::InvalidToken;
    bool fileOperation = false;
};

// Version-independent value matching only. A matching task/work/completion
// tuple is a local candidate, not proof against reentrant attribution or an
// assertion that the worker and all follow-on work have finished.
inline std::vector<PassiveTaskWindowCandidate> FindPassiveTaskWindowCandidates(
    const PassiveTransferEvidence& passive, const std::vector<MessageObservation>& messages,
    HWND window, MessageRoute route, UINT completionMessage, DWORD ownerThread)
{
    std::vector<PassiveTaskWindowCandidate> candidates;
    if (!passive.task || !window || route != MessageRoute::control_window_proc || !ownerThread) return candidates;
    for (const auto& worker : passive.workers)
    {
        if (!worker.work || !worker.completion || (worker.kind != 1 && worker.kind != 2) ||
            worker.messageThread != ownerThread || worker.messageEntered != 1 || worker.messageReturned != 1 ||
            worker.callbacks != 1 || !worker.callbackResult) continue;
        std::size_t found = MessageTrace::InvalidToken;
        bool ambiguous = false;
        for (std::size_t i = 0; i < messages.size(); ++i)
        {
            const auto& message = messages[i];
            if (message.message != completionMessage || !message.completion ||
                message.completion->task != passive.task || message.completion->work != worker.work) continue;
            if (found != MessageTrace::InvalidToken || message.window != window || message.route != route ||
                message.malformedCompletion || message.completion->identity != worker.completion ||
                message.completion->fileOperation != (worker.kind == 2) ||
                message.completion->succeeded != (*worker.callbackResult != 0) ||
                message.enteredThread != ownerThread || message.returnedThread != ownerThread || !message.returned ||
                message.returnedSequence <= message.enteredSequence)
            { ambiguous = true; break; }
            found = i;
        }
        if (!ambiguous && found != MessageTrace::InvalidToken)
            candidates.push_back({worker.work, found, worker.kind == 2});
    }
    return candidates;
}

struct V4FileTaskWindowObservation
{
    struct RawEvent
    {
        std::size_t index = 0;
        snowdesktop::external_transfer_observation::Event event;
        std::vector<std::byte> blob;
    };
    struct LocalChain
    {
        std::uint64_t task = 0, readWork = 0, fileWork = 0;
        std::uintptr_t readCompletion = 0, fileCompletion = 0, content = 0, operationResult = 0;
        std::size_t readMessage = MessageTrace::InvalidToken, fileMessage = MessageTrace::InvalidToken;
        std::size_t resultReadIndex = MessageTrace::InvalidToken, resultQueueIndex = MessageTrace::InvalidToken;
        std::int64_t pathCount = 0;
    };
    bool identityValid = false, localAdapterScopeMatched = false, directCallsiteMatched = false, observationUnavailable = false;
    std::uint64_t directCallsite = 0;
    std::optional<std::size_t> callsiteEnterIndex, transferOriginIndex, callsiteReturnIndex;
    std::optional<V2CapturedOleReturn> drop;
    std::optional<PassiveTransferEvidence> passive;
    std::vector<RawEvent> events;
    std::vector<MessageObservation> messages;
    std::vector<PassiveTaskWindowCandidate> messageCandidates;
    std::optional<LocalChain> localChain;
    std::vector<std::string> unresolved;
};

// Strictly corroborate one successful read/file work pair using the actual
// diagnostic values and returned window messages. These equalities establish
// a local observation only: TLS inheritance during uninstrumented reentry,
// later work and business results are outside this check.
inline std::optional<V4FileTaskWindowObservation::LocalChain> FindV4FileLocalChain(
    const PassiveTransferEvidence& passive, const std::vector<V4FileTaskWindowObservation::RawEvent>& events,
    const std::vector<PassiveTaskWindowCandidate>& messages, DWORD ownerThread,
    const ole_file_source_fixture::Source& source)
{
    namespace diagnostic = snowdesktop::external_transfer_observation;
    if (!passive.task || passive.workers.size() != 2 || messages.size() != 2 || source.wrongApartment ||
        source.starts != 1 || source.ends != 1 || source.outcome != S_OK || source.endEffect != DROPEFFECT_COPY)
        return std::nullopt;
    const PassiveWorkerEvidence* read = nullptr; const PassiveWorkerEvidence* file = nullptr;
    for (const auto& worker : passive.workers)
    {
        if (worker.kind == 1 && !read) read = &worker;
        else if (worker.kind == 2 && !file) file = &worker;
        else return std::nullopt;
    }
    const auto completeWorker = [&](const PassiveWorkerEvidence* work) {
        return work && work->work && work->completion && work->worker && work->workerThread &&
            work->messageThread == ownerThread && work->queueEntries == 1 && work->queueAccepted == 1 &&
            !work->queueFailed && work->dequeued == 1 && work->entered == 1 && work->returned == 1 &&
            work->finished == 1 && !work->cancelled && work->executeResult && *work->executeResult == 1 &&
            work->callbacks == 1 && work->callbackResult && *work->callbackResult == 1 &&
            work->posts == 1 && !work->postFailed && work->messageEntered == 1 && work->messageReturned == 1;
    };
    if (!completeWorker(read) || !completeWorker(file) || read->work == file->work ||
        passive.readEnqueued != 1 || passive.readWorkerEntered != 1 || passive.readWorkerReturned != 1 ||
        passive.readCallbacks != 1 || passive.fileEnqueued != 1 || passive.fileCallbacks != 1 ||
        passive.sourceCompletions != 1 || passive.fileCompletions != 1 ||
        passive.contentReadWork != read->work || !passive.contentObject || passive.contentReadRecords != 1 ||
        passive.contentCommitRecords != 1 || !passive.contentReadPathCount || !passive.contentCommitPathCount ||
        *passive.contentReadPathCount <= 0 || *passive.contentReadPathCount != *passive.contentCommitPathCount ||
        passive.contentReadThread != read->workerThread || passive.contentCommitThread != ownerThread ||
        passive.finishedEntered != 1 || passive.finishedReturned != 1 || passive.finishedDuplicates ||
        passive.finishedContextMismatch || !passive.finishedClosure || passive.finishedWork != file->work ||
        passive.finishedThread != ownerThread || !passive.finishedInput || !passive.finishedOutcome ||
        *passive.finishedInput != 1 || *passive.finishedOutcome != 1 || passive.endEntered != 1 ||
        passive.sourceEnds != 1 || passive.endLocalTask != passive.task || passive.endLocalWork != file->work ||
        passive.endThread != ownerThread || !passive.endRequestedStatus || !passive.endReturnedStatus ||
        *passive.endRequestedStatus != static_cast<std::int64_t>(source.outcome) ||
        *passive.endReturnedStatus != static_cast<std::int64_t>(S_OK) || passive.endRequestedEffect != source.endEffect)
        return std::nullopt;
    const auto workFor = [&](std::uint64_t id) -> const PassiveWorkerEvidence* {
        return id == read->work ? read : id == file->work ? file : nullptr;
    };
    const V4FileTaskWindowObservation::RawEvent* resultRead = nullptr;
    const V4FileTaskWindowObservation::RawEvent* resultQueue = nullptr;
    const V4FileTaskWindowObservation::RawEvent* readQueue = nullptr;
    const V4FileTaskWindowObservation::RawEvent* fileQueue = nullptr;
    for (const auto& record : events)
    {
        const auto& event = record.event;
        if (event.task != passive.task) continue;
        switch (event.phase)
        {
        case diagnostic::Phase::ambiguous_nested_scope:
        case diagnostic::Phase::transfer_finished_duplicate:
        case diagnostic::Phase::transfer_finished_context_mismatch:
        case diagnostic::Phase::read_queue_failed:
        case diagnostic::Phase::file_queue_failed:
        case diagnostic::Phase::completion_post_failed:
        case diagnostic::Phase::worker_task_cancelled:
            return std::nullopt;
        case diagnostic::Phase::desktop_file_result_read_binding:
            if (resultRead || !event.object || event.extra != read->work || event.thread != ownerThread ||
                event.localTask != passive.task || event.localWork) return std::nullopt;
            resultRead = &record; break;
        case diagnostic::Phase::desktop_file_result_queue_binding:
            if (resultQueue || !event.object || event.extra != file->work || event.value < 0 ||
                static_cast<std::uint64_t>(event.value) != read->work || event.thread != ownerThread ||
                event.localTask != passive.task || event.localWork != read->work) return std::nullopt;
            resultQueue = &record; break;
        case diagnostic::Phase::read_queue_enter: case diagnostic::Phase::read_queued:
        case diagnostic::Phase::read_completion_callback:
            if (event.extra != read->work || event.object != read->completion) return std::nullopt;
            if (event.phase == diagnostic::Phase::read_queue_enter) readQueue = &record;
            break;
        case diagnostic::Phase::file_queue_enter: case diagnostic::Phase::file_queued:
        case diagnostic::Phase::file_completion_callback:
            if (event.extra != file->work || event.object != file->completion) return std::nullopt;
            if (event.phase == diagnostic::Phase::file_queue_enter) fileQueue = &record;
            break;
        case diagnostic::Phase::completion_posted: case diagnostic::Phase::completion_enter:
        case diagnostic::Phase::completion_return:
        {
            const auto* work = workFor(event.extra);
            if (!work || event.object != work->completion) return std::nullopt;
            break;
        }
        case diagnostic::Phase::worker_dequeued: case diagnostic::Phase::worker_execute_enter:
        case diagnostic::Phase::worker_execute_return: case diagnostic::Phase::worker_task_finished:
        {
            const auto* work = workFor(event.extra);
            if (!work || event.object != work->worker || event.thread != work->workerThread) return std::nullopt;
            break;
        }
        case diagnostic::Phase::read_content_paths: case diagnostic::Phase::commit_content_paths:
            if (event.object != passive.contentObject || event.extra != read->work ||
                event.value != *passive.contentReadPathCount) return std::nullopt;
            break;
        case diagnostic::Phase::read_worker_enter: case diagnostic::Phase::read_worker_return:
            if (event.extra != read->work || event.thread != read->workerThread) return std::nullopt;
            break;
        case diagnostic::Phase::transfer_finished_enter: case diagnostic::Phase::transfer_finished_return:
            if (event.object != passive.finishedClosure || event.extra != file->work || event.thread != ownerThread)
                return std::nullopt;
            break;
        case diagnostic::Phase::end_operation_enter:
            if (event.thread != ownerThread || event.localTask != passive.task || event.localWork != file->work ||
                event.value != static_cast<std::int64_t>(source.outcome) || event.extra != source.endEffect)
                return std::nullopt;
            break;
        case diagnostic::Phase::end_operation:
            if (event.thread != ownerThread || event.localTask != passive.task || event.localWork != file->work ||
                event.value != static_cast<std::int64_t>(S_OK)) return std::nullopt;
            break;
        default: break;
        }
    }
    const PassiveTaskWindowCandidate* readMessage = nullptr; const PassiveTaskWindowCandidate* fileMessage = nullptr;
    for (const auto& message : messages)
    {
        if (message.work == read->work && !message.fileOperation && !readMessage) readMessage = &message;
        else if (message.work == file->work && message.fileOperation && !fileMessage) fileMessage = &message;
        else return std::nullopt;
    }
    if (!readMessage || !fileMessage || readMessage->traceIndex == fileMessage->traceIndex ||
        !resultRead || !resultQueue || !readQueue || !fileQueue || resultRead->event.object != resultQueue->event.object ||
        resultRead->index >= readQueue->index || resultQueue->index >= fileQueue->index ||
        readQueue->event.thread != ownerThread || fileQueue->event.thread != ownerThread)
        return std::nullopt;
    V4FileTaskWindowObservation::LocalChain chain;
    chain.task = passive.task; chain.readWork = read->work; chain.fileWork = file->work;
    chain.readCompletion = read->completion; chain.fileCompletion = file->completion;
    chain.content = passive.contentObject; chain.operationResult = resultRead->event.object;
    chain.readMessage = readMessage->traceIndex; chain.fileMessage = fileMessage->traceIndex;
    chain.resultReadIndex = resultRead->index; chain.resultQueueIndex = resultQueue->index;
    chain.pathCount = *passive.contentReadPathCount;
    return chain;
}

inline bool V4SourceSnapshotReady(const V4SingleFileInputObservation& value) noexcept { return value.captured; }
inline bool V4SourceSnapshotReady(const V4IndependentFileInputObservation& value) noexcept { return value.captured; }
inline bool V4SourceSnapshotReady(const directory_fixture::DirectoryCopyLeaseObservation& value) noexcept
{ return value.inputObserved && value.fixtureLeaseOverlapObserved; }

class V4FileTaskWindowReader final
{
    template<class Receipt>
    static V4FileTaskWindowObservation InspectBound(const Receipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    ole_file_source_fixture::Source& source, OleDragDropAdapter* adapter,
    const MessageTrace& trace, HWND control, UINT completionMessage, bool sameHost)
{
    namespace diagnostic = snowdesktop::external_transfer_observation;
    V4FileTaskWindowObservation result;
    try
    {
        if (!sameHost || !receipt.CompletionTraceCaptured() ||
            !receipt.SourceAtExecution() || !V4SourceSnapshotReady(*receipt.SourceAtExecution()) ||
            trace.LostOrCrossThread() || trace.OwnerThread() != GetCurrentThreadId() ||
            !control || !IsWindow(control) || GetWindowThreadProcessId(control, nullptr) != trace.OwnerThread())
        { result.observationUnavailable = true; result.unresolved.emplace_back("v4_file_task_receipt_trace_or_host_unavailable"); return result; }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || raw.id != receipt.Plan().candidateId ||
            raw.id != receipt.SourceAtExecution()->candidateId || selected.id != receipt.Plan().graphId ||
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
        result.passive = InspectPassiveTransfer(static_cast<IDataObject*>(&source));
        result.identityValid = true;
        const auto& drop = *result.drop; const auto& origin = result.passive->adapterScope;
        const auto sourceAddress = reinterpret_cast<std::uintptr_t>(static_cast<IDataObject*>(&source));
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
                const auto sourceAddress = reinterpret_cast<std::uintptr_t>(static_cast<IDataObject*>(&source));
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
public:
    static V4FileTaskWindowObservation Single(const V4FileOleRunReceipt& receipt,
        const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
        const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
        ole_file_source_fixture::Source& source, OleDragDropAdapter* adapter,
        const std::shared_ptr<const V3HostEpoch>& epoch, const MessageTrace& trace, HWND control, UINT completionMessage)
    {
        return InspectBound(receipt, verified, identity, present, selected, source, adapter, trace, control,
            completionMessage, receipt.SameBoundHost(source, adapter, epoch, trace, control));
    }
    static V4FileTaskWindowObservation Multi(const V4MultiFileOleRunReceipt& receipt,
        const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
        const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
        const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& owner,
        OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
        const MessageTrace& trace, HWND control, UINT completionMessage)
    {
        auto* source = binding.SourceObject();
        if (!source) { V4FileTaskWindowObservation result; result.observationUnavailable = true; return result; }
        return InspectBound(receipt, verified, identity, present, selected, *source, adapter, trace, control,
            completionMessage, receipt.SameBoundHost(binding, owner, adapter, epoch, trace, control));
    }
    static V4FileTaskWindowObservation Directory(const V4DirectoryOleRunReceipt& receipt,
        const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
        const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
        const std::shared_ptr<directory_fixture::DirectoryCopyLease>& lease,
        const directory_fixture::DirectoryShellBinding& binding,
        const std::shared_ptr<directory_fixture::DirectoryShellOwner>& owner,
        OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
        const MessageTrace& trace, HWND control, UINT completionMessage)
    {
        auto* source = binding.SourceObject();
        if (!source) { V4FileTaskWindowObservation result; result.observationUnavailable = true; return result; }
        return InspectBound(receipt, verified, identity, present, selected, *source, adapter, trace, control,
            completionMessage, receipt.SameBoundHost(lease, binding, owner, adapter, epoch, trace, control));
    }
};

struct V4GroupHitActivationEvidence
{
    bool identityValid = false, hitAdapterBound = false, hitWindowScopeObserved = false;
    bool completionWindowBound = false, activationValuesObserved = false;
    bool observationUnavailable = true, resourceUnavailable = false;
    std::uint64_t callsite = 0, task = 0, work = 0;
    std::uintptr_t groupObject = 0, containerObject = 0, widgetObject = 0;
    std::optional<std::int64_t> groupKind, hitDecision;
    std::optional<bool> hitEqualsDestination, completionSucceeded, ownerIdUnchanged, resolvedTypeMatchesHit, activeAfterEqualsDestination;
    std::optional<std::size_t> hitTraceIndex, completionTraceIndex;
    std::vector<V4FileTaskWindowObservation::RawEvent> records;
    std::vector<std::string> unresolved;
};

// The hit is an already-returned production value, not a test-side hit test.
// The later callback is correlated only through the actual transfer/work and
// returned completion WndProc; no absent event or final business state follows.
class V4GroupHitActivationReader final
{
public:
    static V4GroupHitActivationEvidence Inspect(const V4MultiFileOleRunReceipt& receipt,
        const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
        const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
        const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& owner,
        OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
        const MessageTrace& trace, HWND control, UINT completionMessage)
    {
        namespace diagnostic = snowdesktop::external_transfer_observation;
        V4GroupHitActivationEvidence out;
        try
        {
            const bool complete = diagnostic::Complete(); diagnostic::ReadSession session;
            if (!session.Open(true)) { out.unresolved.emplace_back("v4_group_hit_session_unavailable"); return out; }
            for (std::size_t i = 0; i < session.Count(); ++i)
            {
                diagnostic::Event event;
                if (!session.ReadEvent(i, event)) { out.unresolved.emplace_back("v4_group_hit_record_unavailable"); return out; }
                const bool group = event.phase >= diagnostic::Phase::group_slot_callsite_enter && event.phase <= diagnostic::Phase::group_completion_return;
                if (!group && event.phase != diagnostic::Phase::desktop_file_transfer_origin &&
                    event.phase != diagnostic::Phase::completion_enter && event.phase != diagnostic::Phase::completion_return) continue;
                V4FileTaskWindowObservation::RawEvent record; record.index = i; record.event = event;
                if (event.hasBlob)
                {
                    if (event.blobSize > diagnostic::BlobLimit()) { out.unresolved.emplace_back("v4_group_hit_blob_unavailable"); return out; }
                    record.blob.resize(event.blobSize); std::size_t copied = 0;
                    if (!session.CopyBlob(i, record.blob.data(), record.blob.size(), copied) || copied != record.blob.size())
                    { out.unresolved.emplace_back("v4_group_hit_blob_unavailable"); return out; }
                }
                out.records.push_back(std::move(record));
            }
            auto* source = binding.SourceObject();
            if (!complete || !diagnostic::Complete() || !source || !epoch || !epoch->app_ ||
                !receipt.SameBoundHost(binding, owner, adapter, epoch, trace, control))
            { out.unresolved.emplace_back("v4_group_hit_receipt_or_log_unavailable"); return out; }
            const auto taskWindow = V4FileTaskWindowReader::Multi(receipt, verified, identity, present, selected,
                binding, owner, adapter, epoch, trace, control, completionMessage);
            if (!taskWindow.identityValid || !taskWindow.drop)
            { out.unresolved.emplace_back("v4_group_hit_graph_or_drop_unavailable"); return out; }
            out.identityValid = true;
            const auto& drop = *taskWindow.drop; const auto sourceAddress = reinterpret_cast<std::uintptr_t>(static_cast<IDataObject*>(source));
            if (drop.entered.kind != snowdesktop::ole_adapter_observation::Kind::drop ||
                drop.entered.phase != snowdesktop::ole_adapter_observation::Phase::entered ||
                drop.returned.kind != drop.entered.kind || drop.returned.phase != snowdesktop::ole_adapter_observation::Phase::returned ||
                !drop.entered.session || !drop.entered.call || drop.entered.parent || drop.entered.depth ||
                drop.entered.adapter != reinterpret_cast<std::uintptr_t>(adapter) || drop.entered.thread != trace.OwnerThread() ||
                drop.returned.session != drop.entered.session || drop.returned.call != drop.entered.call ||
                drop.returned.parent != drop.entered.parent || drop.returned.depth != drop.entered.depth ||
                drop.returned.adapter != drop.entered.adapter || drop.returned.thread != drop.entered.thread ||
                drop.returned.result != drop.observation.result)
            { out.unresolved.emplace_back("v4_group_hit_drop_scope_unavailable"); return out; }
            const auto matchesAdapter = [&](const diagnostic::Event& e) {
                const auto& s = e.adapterScope;
                return s.observed && s.session == drop.entered.session && s.call == drop.entered.call &&
                    s.parent == drop.entered.parent && s.adapter == drop.entered.adapter && s.thread == drop.entered.thread &&
                    s.depth == drop.entered.depth && s.kind == static_cast<unsigned>(snowdesktop::ole_adapter_observation::Kind::drop) &&
                    e.thread == drop.entered.thread;
            };
            using Raw = V4FileTaskWindowObservation::RawEvent;
            const Raw *callEnter = nullptr, *callReturn = nullptr, *hit = nullptr, *container = nullptr, *widget = nullptr,
                *hitValue = nullptr, *destination = nullptr, *ownerId = nullptr, *active = nullptr, *decision = nullptr;
            for (const auto& r : out.records)
                if (r.event.phase == diagnostic::Phase::group_slot_callsite_enter && !r.event.task && r.event.object == sourceAddress &&
                    r.event.extra && matchesAdapter(r.event))
                { if (callEnter) { out.unresolved.emplace_back("v4_group_hit_callsite_ambiguous"); return out; } callEnter = &r; }
            if (!callEnter) { out.unresolved.emplace_back("v4_group_hit_callsite_unavailable"); return out; }
            bool ambiguous = false;
            for (const auto& r : out.records)
            {
                if (r.event.extra != callEnter->event.extra || r.event.task != 0) continue;
                const auto phase = r.event.phase;
                if (phase < diagnostic::Phase::group_slot_callsite_enter || phase > diagnostic::Phase::group_hit_decision) continue;
                if (!matchesAdapter(r.event) || r.event.windowScope != callEnter->event.windowScope) { ambiguous = true; continue; }
                const Raw** slot = nullptr;
                switch (phase)
                {
                case diagnostic::Phase::group_slot_callsite_enter: if (&r != callEnter) ambiguous = true; break;
                case diagnostic::Phase::group_slot_callsite_return: slot = &callReturn; break;
                case diagnostic::Phase::group_hit_observed: slot = &hit; break;
                case diagnostic::Phase::group_hit_container: slot = &container; break;
                case diagnostic::Phase::group_hit_widget: slot = &widget; break;
                case diagnostic::Phase::group_hit_value: slot = &hitValue; break;
                case diagnostic::Phase::group_hit_destination: slot = &destination; break;
                case diagnostic::Phase::group_hit_owner_id: slot = &ownerId; break;
                case diagnostic::Phase::group_hit_active: slot = &active; break;
                case diagnostic::Phase::group_hit_decision: slot = &decision; break;
                default: break;
                }
                if (slot) { if (*slot) ambiguous = true; else *slot = &r; }
            }
            const auto blobOk = [](const Raw* r) {
                return r && r->event.hasBlob && r->blob.size() == r->event.blobSize && r->blob.size() % sizeof(wchar_t) == 0;
            };
            if (ambiguous || !callReturn || callReturn->event.object != sourceAddress || !hit || !container || !widget ||
                !decision || !hit->event.object || !container->event.object || (hit->event.value != 1 && hit->event.value != 2) ||
                decision->event.object != hit->event.object || decision->event.value < -1 || decision->event.value > 1 ||
                !blobOk(hitValue) || !blobOk(destination) || hitValue->event.object != hit->event.object ||
                destination->event.object != hit->event.object ||
                !(callEnter->index < hit->index && hit->index < decision->index && decision->index < callReturn->index) ||
                container->index <= hit->index || container->index >= decision->index || widget->index <= hit->index ||
                widget->index >= decision->index || hitValue->index <= hit->index || hitValue->index >= decision->index ||
                destination->index <= hit->index || destination->index >= decision->index ||
                (widget->event.value == 1 && !widget->event.object) ||
                (widget->event.value == 0 && (widget->event.object || ownerId)) ||
                (ownerId && (!blobOk(ownerId) || ownerId->event.object != widget->event.object)) ||
                (decision->event.value == 1 && !ownerId) ||
                (ownerId && (ownerId->index <= hit->index || ownerId->index >= decision->index)) ||
                (active && (active->index <= hit->index || active->index >= decision->index)) ||
                (widget->event.value != 0 && widget->event.value != 1) ||
                (decision->event.value >= 0 && (!blobOk(active) || active->event.object != hit->event.object)))
            { out.unresolved.emplace_back("v4_group_hit_interval_or_values_unavailable"); return out; }
            out.callsite = callEnter->event.extra; out.groupObject = hit->event.object; out.containerObject = container->event.object;
            out.widgetObject = widget->event.object; out.groupKind = hit->event.value; out.hitDecision = decision->event.value;
            out.hitEqualsDestination = hitValue->blob == destination->blob; out.hitAdapterBound = true;
            if (callEnter->event.windowScope)
            {
                std::optional<std::size_t> found;
                const auto& messages = receipt.CompletionTrace();
                for (std::size_t i = 0; i < messages.size(); ++i)
                    if (messages[i].windowScope == callEnter->event.windowScope)
                    { if (found) { found.reset(); break; } found = i; }
                if (found && *found < trace.Observations().size())
                {
                    const auto& a = messages[*found]; const auto& b = trace.Observations()[*found];
                    const bool surface = (a.window == control && a.route == MessageRoute::control_window_proc) ||
                        (epoch->IsDesktopWindow(a.window) && a.route == MessageRoute::desktop_window_proc);
                    if (surface && a.returned && b.returned && a.enteredThread == drop.entered.thread && a.returnedThread == drop.entered.thread &&
                        b.enteredThread == a.enteredThread && b.returnedThread == a.returnedThread && a.enteredSequence < a.returnedSequence &&
                        a.windowScope == b.windowScope && a.window == b.window && a.message == b.message && a.route == b.route &&
                        a.enteredSequence == b.enteredSequence && a.returnedSequence == b.returnedSequence &&
                        a.wParam == b.wParam && a.inputLParam == b.inputLParam && a.windowResult == b.windowResult)
                    { out.hitWindowScopeObserved = true; out.hitTraceIndex = *found; }
                }
            }
            if (!out.hitWindowScopeObserved) out.unresolved.emplace_back("v4_group_hit_window_scope_unavailable");
            if (!taskWindow.localAdapterScopeMatched || !taskWindow.passive || !taskWindow.localChain)
            { out.unresolved.emplace_back("v4_group_completion_task_chain_unavailable"); return out; }
            const auto& chain = *taskWindow.localChain; const Raw* transfer = nullptr;
            for (const auto& r : out.records)
                if (r.event.phase == diagnostic::Phase::desktop_file_transfer_origin && r.event.task == chain.task &&
                    r.event.extra == out.callsite)
                { if (transfer) { out.unresolved.emplace_back("v4_group_transfer_origin_ambiguous"); return out; } transfer = &r; }
            if (!transfer || transfer->event.object != sourceAddress || !matchesAdapter(transfer->event) ||
                transfer->event.localTask != chain.task || transfer->event.localWork ||
                !(decision->index < transfer->index && transfer->index < callReturn->index) ||
                chain.fileMessage >= taskWindow.messages.size() || chain.fileMessage >= trace.Observations().size())
            { out.unresolved.emplace_back("v4_group_transfer_origin_unavailable"); return out; }
            const auto& message = taskWindow.messages[chain.fileMessage];
            const auto& liveMessage = trace.Observations()[chain.fileMessage];
            if (!message.returned || !message.windowScope || message.enteredThread != trace.OwnerThread() ||
                message.returnedThread != trace.OwnerThread() || !liveMessage.returned ||
                message.windowScope != liveMessage.windowScope || message.window != liveMessage.window ||
                message.route != liveMessage.route || message.message != liveMessage.message || message.wParam != liveMessage.wParam ||
                message.inputLParam != liveMessage.inputLParam || message.windowResult != liveMessage.windowResult ||
                message.enteredThread != liveMessage.enteredThread || message.returnedThread != liveMessage.returnedThread ||
                message.enteredSequence != liveMessage.enteredSequence || message.returnedSequence != liveMessage.returnedSequence)
            { out.unresolved.emplace_back("v4_group_completion_window_unavailable"); return out; }
            const Raw *windowEnter = nullptr, *windowReturn = nullptr, *completed = nullptr, *completedReturn = nullptr,
                *resolved = nullptr, *resolvedOwner = nullptr, *before = nullptr, *after = nullptr;
            for (const auto& r : out.records)
            {
                const auto& e = r.event;
                if (e.task != chain.task || e.extra != chain.fileWork) continue;
                const auto phase = e.phase;
                if (phase != diagnostic::Phase::completion_enter && phase != diagnostic::Phase::completion_return &&
                    (phase < diagnostic::Phase::group_completion_enter || phase > diagnostic::Phase::group_completion_return)) continue;
                if (e.thread != trace.OwnerThread() || e.localTask != chain.task || e.localWork != chain.fileWork ||
                    e.windowScope != message.windowScope) { ambiguous = true; continue; }
                const Raw** slot = nullptr;
                switch (phase)
                {
                case diagnostic::Phase::completion_enter: slot = &windowEnter; break;
                case diagnostic::Phase::completion_return: slot = &windowReturn; break;
                case diagnostic::Phase::group_completion_enter: slot = &completed; break;
                case diagnostic::Phase::group_completion_return: slot = &completedReturn; break;
                case diagnostic::Phase::group_completion_resolved: slot = &resolved; break;
                case diagnostic::Phase::group_completion_owner_id: slot = &resolvedOwner; break;
                case diagnostic::Phase::group_completion_active_before: slot = &before; break;
                case diagnostic::Phase::group_completion_active_after: slot = &after; break;
                default: break;
                }
                if (slot) { if (*slot) ambiguous = true; else *slot = &r; }
            }
            if (ambiguous || !windowEnter || !windowReturn || !completed || !completedReturn ||
                windowEnter->event.object != chain.fileCompletion || windowReturn->event.object != chain.fileCompletion ||
                completed->event.object != epoch->app_ || completedReturn->event.object != epoch->app_ ||
                (completed->event.value != 0 && completed->event.value != 1) || completedReturn->event.value != completed->event.value ||
                !(windowEnter->index < completed->index && completed->index < completedReturn->index && completedReturn->index < windowReturn->index))
            { out.unresolved.emplace_back("v4_group_completion_scope_unavailable"); return out; }
            out.task = chain.task; out.work = chain.fileWork; out.completionTraceIndex = chain.fileMessage;
            out.completionSucceeded = completed->event.value == 1; out.completionWindowBound = true;
            if (resolved || resolvedOwner || before || after)
            {
                if (!resolved || !blobOk(resolvedOwner) || !blobOk(before) || !blobOk(after) ||
                    !out.completionSucceeded || !*out.completionSucceeded || !out.hitDecision || *out.hitDecision != 1 ||
                    !out.hitEqualsDestination || !*out.hitEqualsDestination ||
                    !resolved->event.object || resolvedOwner->event.object != resolved->event.object ||
                    before->event.object != resolved->event.object || after->event.object != resolved->event.object ||
                    !(completed->index < resolved->index && resolved->index < resolvedOwner->index &&
                      resolvedOwner->index < before->index && before->index < after->index && after->index < completedReturn->index))
                { out.unresolved.emplace_back("v4_group_activation_values_unavailable"); return out; }
                out.activationValuesObserved = true;
                if (ownerId && blobOk(ownerId)) out.ownerIdUnchanged = ownerId->blob == resolvedOwner->blob;
                out.resolvedTypeMatchesHit = (hit->event.value == 1 && resolved->event.value == static_cast<std::int64_t>(DesktopWidgetType::CollectionGroup)) ||
                    (hit->event.value == 2 && resolved->event.value == static_cast<std::int64_t>(DesktopWidgetType::FileGroup));
                out.activeAfterEqualsDestination = after->blob == destination->blob;
            }
            else out.unresolved.emplace_back("v4_group_activation_not_observed");
            out.unresolved.emplace_back("v4_group_object_identity_candidate_mapping_and_unobserved_reentry_unverified");
            out.unresolved.emplace_back("v4_group_final_business_late_work_and_durability_unverified");
        }
        catch (const std::bad_alloc&) { out.resourceUnavailable = true; }
        catch (const std::length_error&) { out.resourceUnavailable = true; }
        catch (...) { out.observationUnavailable = true; }
        return out;
    }
};

inline V4FileTaskWindowObservation InspectV4FileTaskWindows(const V4FileOleRunReceipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    ole_file_source_fixture::Source& source, OleDragDropAdapter* adapter,
    const std::shared_ptr<const V3HostEpoch>& epoch, const MessageTrace& trace, HWND control, UINT completionMessage)
{
    return V4FileTaskWindowReader::Single(receipt, verified, identity, present, selected, source, adapter, epoch, trace,
        control, completionMessage);
}

struct V4FileAbiEnvelope
{
    struct Record { std::size_t index = 0; snowdesktop::external_transfer_observation::Event event; };
    bool uniqueEnvelopeObserved = false, observationUnavailable = false;
    std::vector<Record> records;
    std::vector<std::string> unresolved;
};

// Correlate one actual marshaled caller with one source-provider invocation.
// This is a local envelope only; it cannot exclude uninstrumented reentry or
// prove that the selected graph occurrence is this call.
class V4FileAbiReader final
{
    template<class Receipt>
    static V4FileAbiEnvelope InspectBound(const Receipt& receipt, const V4FileTaskWindowObservation& task,
        ole_file_source_fixture::Source& source)
{
    namespace diagnostic = snowdesktop::external_transfer_observation;
    V4FileAbiEnvelope out;
    try
    {
        if (!task.identityValid || !task.localAdapterScopeMatched || !task.localChain ||
            !receipt.SourceSessionInterval() || !receipt.SourceAtExecution() || !V4SourceSnapshotReady(*receipt.SourceAtExecution()))
        { out.observationUnavailable = true; out.unresolved.emplace_back("v4_file_abi_task_or_interval_unavailable"); return out; }
        const auto& chain = *task.localChain;
        const std::array<diagnostic::Phase, 8> phases{diagnostic::Phase::source_marshal_enter,
            diagnostic::Phase::source_marshal_return, diagnostic::Phase::source_unmarshal_enter,
            diagnostic::Phase::source_unmarshal_return, diagnostic::Phase::file_paths_getdata_enter,
            diagnostic::Phase::fixture_filedrop_getdata_enter, diagnostic::Phase::fixture_filedrop_getdata_return,
            diagnostic::Phase::file_paths_getdata_return};
        std::array<std::optional<V4FileAbiEnvelope::Record>, 8> marks;
        for (const auto& record : task.events)
        {
            if (record.event.phase == diagnostic::Phase::fixture_filedrop_getdata_unwound ||
                (record.event.phase == diagnostic::Phase::ambiguous_nested_scope && record.event.task == chain.task))
            { out.observationUnavailable = true; out.unresolved.emplace_back("v4_file_abi_unwind_or_nested_unavailable"); return out; }
            const auto found = std::find(phases.begin(), phases.end(), record.event.phase);
            if (found == phases.end()) continue;
            auto& slot = marks[static_cast<std::size_t>(found - phases.begin())];
            if (slot) { out.observationUnavailable = true; out.unresolved.emplace_back("v4_file_abi_multiple_calls_unavailable"); return out; }
            slot = V4FileAbiEnvelope::Record{record.index, record.event};
        }
        for (std::size_t i = 0; i < marks.size(); ++i)
        {
            if (!marks[i] || (i && marks[i - 1]->index >= marks[i]->index))
            { out.observationUnavailable = true; out.unresolved.emplace_back("v4_file_abi_call_envelope_unavailable"); return out; }
            out.records.push_back(*marks[i]);
        }
        const auto& marshal = marks[0]->event; const auto& marshaled = marks[1]->event;
        const auto& unmarshal = marks[2]->event; const auto& unmarshaled = marks[3]->event;
        const auto& caller = marks[4]->event; const auto& provider = marks[5]->event;
        const auto& provided = marks[6]->event; const auto& returned = marks[7]->event;
        const auto original = reinterpret_cast<std::uintptr_t>(static_cast<IDataObject*>(&source));
        const auto concrete = reinterpret_cast<std::uintptr_t>(&source);
        if (!marshal.object || marshal.object != marshaled.object || marshal.object != unmarshal.object ||
            marshal.object != unmarshaled.object || marshal.task != chain.task || marshaled.task != chain.task ||
            unmarshal.task != chain.task || unmarshaled.task != chain.task || marshal.extra != original ||
            marshal.thread != source.owner || marshaled.thread != marshal.thread || marshal.localTask != chain.task ||
            marshaled.localTask != chain.task || !marshaled.extra || marshaled.extra != unmarshal.extra || !unmarshaled.extra ||
            FAILED(static_cast<HRESULT>(marshaled.value)) || FAILED(static_cast<HRESULT>(unmarshaled.value)) ||
            unmarshal.localTask != chain.task || unmarshaled.localTask != chain.task ||
            unmarshal.localWork != chain.readWork || unmarshaled.localWork != chain.readWork || !unmarshal.thread ||
            unmarshal.thread != unmarshaled.thread || caller.task != chain.task || returned.task != chain.task ||
            caller.localTask != chain.task || returned.localTask != chain.task ||
            caller.localWork != chain.readWork || returned.localWork != chain.readWork || caller.thread != unmarshal.thread ||
            returned.thread != caller.thread || !caller.extra || caller.extra != returned.extra ||
            caller.object != unmarshaled.extra || returned.object != caller.object || provider.task || provided.task ||
            provider.object != concrete || provided.object != concrete || !provider.extra || provider.extra != provided.extra ||
            provider.thread != source.owner || provided.thread != provider.thread || provider.localTask || provider.localWork ||
            provided.localTask || provided.localWork || provided.value != returned.value)
        { out.observationUnavailable = true; out.unresolved.emplace_back("v4_file_abi_identity_or_result_unavailable"); return out; }
        const auto& interval = *receipt.SourceSessionInterval();
        ole_file_source_fixture::FileDropCallState current;
        if (interval.before.count != 0 || interval.before.depth || interval.before.failed ||
            interval.after.failed || interval.after.depth || interval.events.size() != 2 ||
            interval.events[0].phase != ole_file_source_fixture::FileDropCallPhase::entered ||
            interval.events[1].phase != ole_file_source_fixture::FileDropCallPhase::returned ||
            interval.events[0].call != provider.extra || interval.events[1].call != provided.extra ||
            interval.events[0].parent || interval.events[1].parent || interval.events[0].depth || interval.events[1].depth ||
            interval.events[0].thread != source.owner || interval.events[1].thread != source.owner ||
            interval.events[1].result != static_cast<HRESULT>(provided.value) ||
            !source.ReadFileDropCallState(current) || current.failed || current.depth ||
            current.owner != interval.after.owner || current.count != interval.after.count)
        { out.observationUnavailable = true; out.unresolved.emplace_back("v4_file_abi_source_interval_unavailable"); return out; }
        out.uniqueEnvelopeObserved = true;
        out.unresolved.emplace_back("v4_file_abi_uninstrumented_reentry_and_selected_occurrence_unverified");
        out.unresolved.emplace_back("v4_file_abi_producer_quiescence_and_business_unverified");
        out.observationUnavailable = true;
    }
    catch (...) { out.observationUnavailable = true; }
    return out;
}
public:
    static V4FileAbiEnvelope Single(const V4FileOleRunReceipt& receipt,
        const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
        const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
        ole_file_source_fixture::Source& source, OleDragDropAdapter* adapter,
        const std::shared_ptr<const V3HostEpoch>& epoch, const MessageTrace& trace, HWND control, UINT completionMessage)
    {
        const auto task = V4FileTaskWindowReader::Single(receipt, verified, identity, present, selected,
            source, adapter, epoch, trace, control, completionMessage);
        return InspectBound(receipt, task, source);
    }
    static V4FileAbiEnvelope Multi(const V4MultiFileOleRunReceipt& receipt,
        const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
        const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
        const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& owner,
        OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
        const MessageTrace& trace, HWND control, UINT completionMessage)
    {
        auto* source = binding.SourceObject();
        if (!source) { V4FileAbiEnvelope result; result.observationUnavailable = true; return result; }
        const auto task = V4FileTaskWindowReader::Multi(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control, completionMessage);
        return InspectBound(receipt, task, *source);
    }
    static V4FileAbiEnvelope Directory(const V4DirectoryOleRunReceipt& receipt,
        const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
        const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
        const std::shared_ptr<directory_fixture::DirectoryCopyLease>& lease,
        const directory_fixture::DirectoryShellBinding& binding,
        const std::shared_ptr<directory_fixture::DirectoryShellOwner>& owner,
        OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
        const MessageTrace& trace, HWND control, UINT completionMessage)
    {
        auto* source = binding.SourceObject();
        if (!source) { V4FileAbiEnvelope result; result.observationUnavailable = true; return result; }
        const auto task = V4FileTaskWindowReader::Directory(receipt, verified, identity, present, selected,
            lease, binding, owner, adapter, epoch, trace, control, completionMessage);
        return InspectBound(receipt, task, *source);
    }
};

inline V4FileAbiEnvelope InspectV4FileAbiEnvelope(const V4FileOleRunReceipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    ole_file_source_fixture::Source& source, OleDragDropAdapter* adapter,
    const std::shared_ptr<const V3HostEpoch>& epoch, const MessageTrace& trace, HWND control, UINT completionMessage)
{
    return V4FileAbiReader::Single(receipt, verified, identity, present, selected, source, adapter, epoch, trace,
        control, completionMessage);
}

struct V4FilePathFlowObservation
{
    struct Value
    {
        std::size_t index = 0, readLogIndex = 0, commitLogIndex = 0;
        std::optional<std::size_t> modelIndex;
        std::vector<std::byte> providerUtf16, readUtf16, commitUtf16;
    };
    bool identityValid = false, valuesCaptured = false, exactValuesMatch = false;
    bool observationUnavailable = false;
    std::uint64_t task = 0, readWork = 0, fileWork = 0;
    std::uintptr_t content = 0, operationResult = 0;
    std::optional<V4FileDropOrderObservation> sourceOrder;
    std::vector<Value> values;
    std::vector<std::string> unresolved;
};

// The result is an observed value correspondence for the one-file supported
// slice. It is neither a business assertion nor proof of atomicity or of the
// eventual output. Non-identical spellings remain unavailable, not failures.
class V4SinglePathFlowReader final
{
    static V4FilePathFlowObservation InspectBound(const V4FileTaskWindowObservation& task,
        const V4FileAbiEnvelope& abi, ole_file_source_fixture::Source& source,
        const std::filesystem::path& expectedPath, DWORD ownerThread)
{
    namespace diagnostic = snowdesktop::external_transfer_observation;
    V4FilePathFlowObservation out;
    try
    {
        if (!task.identityValid || !task.directCallsiteMatched || !task.localChain || !task.passive ||
            !abi.uniqueEnvelopeObserved || abi.records.size() != 8 || source.wrongApartment || source.fileDropObservationFailed ||
            source.fileReads != 1 || source.fileDropReads.size() != 1)
        { out.observationUnavailable = true; out.unresolved.emplace_back("v4_file_path_flow_source_or_task_unavailable"); return out; }
        const auto& chain = *task.localChain; const auto& sourceRead = source.fileDropReads.front();
        const auto worker = std::find_if(task.passive->workers.begin(), task.passive->workers.end(),
            [&](const auto& item) { return item.work == chain.readWork && item.kind == 1; });
        if (chain.pathCount != 1 || !chain.content || !chain.operationResult || worker == task.passive->workers.end() ||
            FAILED(sourceRead.result) || sourceRead.result != static_cast<HRESULT>(abi.records[6].event.value) ||
            sourceRead.capture != ole_file_source_fixture::FileDropCapture::captured || sourceRead.threadId != source.owner ||
            sourceRead.startsAtEntry <= sourceRead.endsAtEntry || sourceRead.paths.size() != 1)
        { out.observationUnavailable = true; out.unresolved.emplace_back("v4_file_path_flow_provider_or_read_work_unavailable"); return out; }
        const V4FileTaskWindowObservation::RawEvent* read = nullptr;
        const V4FileTaskWindowObservation::RawEvent* commit = nullptr;
        const V4FileTaskWindowObservation::RawEvent* readSummary = nullptr;
        const V4FileTaskWindowObservation::RawEvent* readReturn = nullptr;
        const V4FileTaskWindowObservation::RawEvent* commitSummary = nullptr;
        const V4FileTaskWindowObservation::RawEvent* completionReturn = nullptr;
        for (const auto& record : task.events)
        {
            const auto& event = record.event;
            if (event.task != chain.task) continue;
            const auto set = [&](const V4FileTaskWindowObservation::RawEvent*& target) {
                if (target) return false;
                target = &record; return true;
            };
            bool unique = true;
            if (event.phase == diagnostic::Phase::read_content_path_value) unique = set(read);
            else if (event.phase == diagnostic::Phase::commit_content_path_value) unique = set(commit);
            else if (event.phase == diagnostic::Phase::read_content_paths) unique = set(readSummary);
            else if (event.phase == diagnostic::Phase::read_worker_return) unique = set(readReturn);
            else if (event.phase == diagnostic::Phase::commit_content_paths) unique = set(commitSummary);
            else if (event.phase == diagnostic::Phase::completion_return && event.extra == chain.readWork) unique = set(completionReturn);
            if (!unique) { out.observationUnavailable = true; out.unresolved.emplace_back("v4_file_path_flow_duplicate_marker"); return out; }
        }
        const auto validValue = [&](const V4FileTaskWindowObservation::RawEvent* record, DWORD thread) {
            return record && record->event.hasBlob && record->event.object == chain.content && !record->event.extra &&
                record->event.localTask == chain.task && record->event.localWork == chain.readWork &&
                record->event.thread == thread && record->blob.size() == record->event.blobSize &&
                record->blob.size() % sizeof(wchar_t) == 0;
        };
        if (!validValue(read, worker->workerThread) || !validValue(commit, ownerThread) ||
            !readSummary || !readReturn || !commitSummary || !completionReturn ||
            readSummary->event.thread != worker->workerThread || readReturn->event.thread != worker->workerThread ||
            commitSummary->event.thread != ownerThread || completionReturn->event.thread != ownerThread ||
            !(readSummary->index < read->index && read->index < readReturn->index) ||
            !(commitSummary->index < commit->index && commit->index < completionReturn->index))
        { out.observationUnavailable = true; out.unresolved.emplace_back("v4_file_path_flow_value_or_same_thread_interval_unavailable"); return out; }
        out.task = chain.task; out.readWork = chain.readWork; out.fileWork = chain.fileWork;
        out.content = chain.content; out.operationResult = chain.operationResult;
        V4FilePathFlowObservation::Value value; value.index = 0;
        value.readLogIndex = read->index; value.commitLogIndex = commit->index;
        const auto& path = sourceRead.paths.front();
        if (path.native().size() > (std::numeric_limits<std::size_t>::max)() / sizeof(wchar_t))
        { out.observationUnavailable = true; out.unresolved.emplace_back("v4_file_path_flow_source_size_unavailable"); return out; }
        value.providerUtf16.resize(path.native().size() * sizeof(wchar_t));
        if (!value.providerUtf16.empty()) std::memcpy(value.providerUtf16.data(), path.native().data(), value.providerUtf16.size());
        value.readUtf16 = read->blob; value.commitUtf16 = commit->blob;
        out.values.push_back(std::move(value)); out.identityValid = true; out.valuesCaptured = true;
        const auto& captured = out.values.front();
        const int pathMatch = CompareStringOrdinal(path.c_str(), -1, expectedPath.c_str(), -1, FALSE);
        out.exactValuesMatch = pathMatch == CSTR_EQUAL && captured.providerUtf16 == captured.readUtf16 &&
            captured.readUtf16 == captured.commitUtf16;
        if (!out.exactValuesMatch)
            out.unresolved.emplace_back("v4_file_path_flow_spelling_alias_or_value_difference_unavailable");
        out.unresolved.emplace_back("v4_file_path_flow_reentry_late_work_and_business_unverified");
        out.observationUnavailable = true;
    }
    catch (...) { out.observationUnavailable = true; }
    return out;
}
public:
    static V4FilePathFlowObservation Single(const V4FileOleRunReceipt& receipt,
        const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
        const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
        ole_file_source_fixture::Source& source, OleDragDropAdapter* adapter,
        const std::shared_ptr<const V3HostEpoch>& epoch, const MessageTrace& trace, HWND control, UINT completionMessage)
    {
        try
        {
            if (!receipt.SourceAtExecution() || !receipt.SourceAtExecution()->captured)
            { V4FilePathFlowObservation out; out.observationUnavailable = true; return out; }
            const auto task = V4FileTaskWindowReader::Single(receipt, verified, identity, present, selected,
                source, adapter, epoch, trace, control, completionMessage);
            const auto abi = V4FileAbiReader::Single(receipt, verified, identity, present, selected,
                source, adapter, epoch, trace, control, completionMessage);
            return InspectBound(task, abi, source, receipt.SourceAtExecution()->file, trace.OwnerThread());
        }
        catch (...) { V4FilePathFlowObservation out; out.observationUnavailable = true; return out; }
    }
    static V4FilePathFlowObservation Directory(const V4DirectoryOleRunReceipt& receipt,
        const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
        const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
        const std::shared_ptr<directory_fixture::DirectoryCopyLease>& lease,
        const directory_fixture::DirectoryShellBinding& binding,
        const std::shared_ptr<directory_fixture::DirectoryShellOwner>& owner,
        OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
        const MessageTrace& trace, HWND control, UINT completionMessage)
    {
        try
        {
            auto* source = binding.SourceObject();
            if (!source || !lease || !receipt.SourceAtExecution() || !receipt.SourceAtExecution()->inputObserved ||
                !receipt.SourceAtExecution()->fixtureLeaseOverlapObserved ||
                !receipt.SameBoundHost(lease, binding, owner, adapter, epoch, trace, control))
            { V4FilePathFlowObservation out; out.observationUnavailable = true; return out; }
            const auto task = V4FileTaskWindowReader::Directory(receipt, verified, identity, present, selected,
                lease, binding, owner, adapter, epoch, trace, control, completionMessage);
            const auto abi = V4FileAbiReader::Directory(receipt, verified, identity, present, selected,
                lease, binding, owner, adapter, epoch, trace, control, completionMessage);
            return InspectBound(task, abi, *source, lease->Source(), trace.OwnerThread());
        }
        catch (...) { V4FilePathFlowObservation out; out.observationUnavailable = true; return out; }
    }
};

inline V4FilePathFlowObservation InspectV4FilePathFlow(const V4FileOleRunReceipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    ole_file_source_fixture::Source& source, OleDragDropAdapter* adapter,
    const std::shared_ptr<const V3HostEpoch>& epoch, const MessageTrace& trace, HWND control, UINT completionMessage)
{
    return V4SinglePathFlowReader::Single(receipt, verified, identity, present, selected, source, adapter, epoch, trace,
        control, completionMessage);
}

inline V4FileTaskWindowObservation InspectV4DirectoryTaskWindows(const V4DirectoryOleRunReceipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    const std::shared_ptr<directory_fixture::DirectoryCopyLease>& lease,
    const directory_fixture::DirectoryShellBinding& binding,
    const std::shared_ptr<directory_fixture::DirectoryShellOwner>& owner,
    OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
    const MessageTrace& trace, HWND control, UINT completionMessage)
{
    return V4FileTaskWindowReader::Directory(receipt, verified, identity, present, selected, lease, binding, owner,
        adapter, epoch, trace, control, completionMessage);
}

inline V4FileAbiEnvelope InspectV4DirectoryAbiEnvelope(const V4DirectoryOleRunReceipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    const std::shared_ptr<directory_fixture::DirectoryCopyLease>& lease,
    const directory_fixture::DirectoryShellBinding& binding,
    const std::shared_ptr<directory_fixture::DirectoryShellOwner>& owner,
    OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
    const MessageTrace& trace, HWND control, UINT completionMessage)
{
    return V4FileAbiReader::Directory(receipt, verified, identity, present, selected, lease, binding, owner,
        adapter, epoch, trace, control, completionMessage);
}

inline V4FilePathFlowObservation InspectV4DirectoryPathFlow(const V4DirectoryOleRunReceipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    const std::shared_ptr<directory_fixture::DirectoryCopyLease>& lease,
    const directory_fixture::DirectoryShellBinding& binding,
    const std::shared_ptr<directory_fixture::DirectoryShellOwner>& owner,
    OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
    const MessageTrace& trace, HWND control, UINT completionMessage)
{
    return V4SinglePathFlowReader::Directory(receipt, verified, identity, present, selected, lease, binding, owner,
        adapter, epoch, trace, control, completionMessage);
}

// Actual directory CF_HDROP values only. This never opens the returned path,
// traverses a product destination, or asserts the copied directory tree.
inline V4FilePayloadObservation InspectV4DirectorySource(const V4DirectoryOleRunReceipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    const std::shared_ptr<directory_fixture::DirectoryCopyLease>& lease,
    const directory_fixture::DirectoryShellBinding& binding,
    const std::shared_ptr<directory_fixture::DirectoryShellOwner>& owner,
    OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
    const MessageTrace& trace, HWND control)
{
    V4FilePayloadObservation out;
    try
    {
        auto* source = binding.SourceObject();
        if (!source || !lease || !receipt.SourceAtExecution() || !receipt.SourceAtExecution()->inputObserved ||
            !receipt.SourceAtExecution()->fixtureLeaseOverlapObserved ||
            !receipt.SameBoundHost(lease, binding, owner, adapter, epoch, trace, control))
        { out.observationUnavailable = true; out.unresolved.emplace_back("v4_directory_source_receipt_or_host_unavailable"); return out; }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || raw.id != receipt.Plan().candidateId ||
            raw.id != receipt.SourceAtExecution()->candidateId || selected.id != receipt.Plan().graphId ||
            !replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
            replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
            replay.variant.unresolved != selected.unresolved)
        { out.observationUnavailable = true; out.unresolved.emplace_back("v4_directory_source_candidate_or_graph_unavailable"); return out; }
        out.expectedPath = lease->Source(); out.sourceStarts = source->starts; out.sourceEnds = source->ends;
        out.fileReads = source->fileReads; out.endOutcome = source->outcome; out.endEffect = source->endEffect;
        out.reads = source->fileDropReads; out.probes = source->formatProbes; out.identityValid = true;
        if (source->wrongApartment || source->fileDropObservationFailed || source->formatProbeObservationFailed ||
            source->fileReads < 0 || out.reads.size() != static_cast<std::size_t>(source->fileReads))
        { out.observationUnavailable = true; out.unresolved.emplace_back("v4_directory_source_log_incomplete"); }
        for (const auto& read : out.reads)
        {
            if (read.threadId != source->owner || read.startsAtEntry <= read.endsAtEntry)
            { out.observationUnavailable = true; out.unresolved.emplace_back("v4_directory_source_thread_or_lease_unavailable"); continue; }
            if (FAILED(read.result)) continue;
            if (read.capture != ole_file_source_fixture::FileDropCapture::captured)
            { out.observationUnavailable = true; out.unresolved.emplace_back("v4_directory_source_medium_unavailable"); continue; }
            out.successfulPayloadObserved = true;
            if (read.paths.size() != 1) { out.candidatePathMismatchObserved = true; continue; }
            const int compared = CompareStringOrdinal(read.paths.front().c_str(), -1, out.expectedPath.c_str(), -1, FALSE);
            if (compared != CSTR_EQUAL)
            { out.observationUnavailable = true; out.unresolved.emplace_back("v4_directory_source_spelling_or_alias_unavailable"); }
        }
        out.unresolved.emplace_back("v4_directory_source_selected_occurrence_and_task_unverified");
        out.unresolved.emplace_back("v4_directory_source_completion_tree_and_business_unverified");
        out.observationUnavailable = true;
    }
    catch (...) { out.observationUnavailable = true; }
    return out;
}

// A permutation is actual input order, not an error. This verifies only the
// same received/read/committed value sequence; landing and file results need
// their own production observations after completion.
inline V4FilePathFlowObservation InspectV4MultiFilePathFlow(const V4MultiFileOleRunReceipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    const std::shared_ptr<V4IndependentFilePayload>& payload, const V4IndependentShellSource& binding,
    const std::shared_ptr<V4IndependentShellOwner>& owner, OleDragDropAdapter* adapter,
    const std::shared_ptr<const V3HostEpoch>& epoch, const MessageTrace& trace, HWND control, UINT completionMessage)
{
    namespace diagnostic = snowdesktop::external_transfer_observation;
    V4FilePathFlowObservation out;
    try
    {
        auto* source = binding.SourceObject();
        if (!source || !payload || !receipt.SameBoundHost(binding, owner, adapter, epoch, trace, control) ||
            !receipt.SourceAtExecution() || !receipt.SourceAtExecution()->captured)
        { out.observationUnavailable = true; out.unresolved.emplace_back("v4_multi_path_flow_receipt_or_source_unavailable"); return out; }
        const auto task = V4FileTaskWindowReader::Multi(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control, completionMessage);
        const auto abi = V4FileAbiReader::Multi(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control, completionMessage);
        const auto order = ObserveV4FileDropOrder(verified, identity, payload, binding, owner, 0);
        out.sourceOrder = order;
        if (!task.identityValid || !task.directCallsiteMatched || !task.localChain || !task.passive ||
            !abi.uniqueEnvelopeObserved || abi.records.size() != 8 || source->wrongApartment ||
            source->fileDropObservationFailed || source->fileReads != 1 || source->fileDropReads.size() != 1 ||
            (order.status != V4FileDropOrderStatus::observed_model_order &&
             order.status != V4FileDropOrderStatus::observed_permutation))
        { out.observationUnavailable = true; out.unresolved.emplace_back("v4_multi_path_flow_task_abi_or_order_unavailable"); return out; }
        const auto& chain = *task.localChain; const auto& sourceRead = source->fileDropReads.front();
        const auto worker = std::find_if(task.passive->workers.begin(), task.passive->workers.end(),
            [&](const auto& item) { return item.work == chain.readWork && item.kind == 1; });
        const auto count = sourceRead.paths.size();
        if (!count || count != receipt.SourceAtExecution()->files.size() || count != order.modelIndexByActualOrder.size() ||
            chain.pathCount <= 0 || static_cast<std::uint64_t>(chain.pathCount) != count || !chain.content || !chain.operationResult ||
            worker == task.passive->workers.end() || FAILED(sourceRead.result) ||
            sourceRead.result != static_cast<HRESULT>(abi.records[6].event.value) ||
            sourceRead.capture != ole_file_source_fixture::FileDropCapture::captured || sourceRead.threadId != source->owner ||
            sourceRead.startsAtEntry <= sourceRead.endsAtEntry)
        { out.observationUnavailable = true; out.unresolved.emplace_back("v4_multi_path_flow_provider_or_count_unavailable"); return out; }
        std::vector<const V4FileTaskWindowObservation::RawEvent*> reads(count, nullptr), commits(count, nullptr);
        const V4FileTaskWindowObservation::RawEvent *readSummary = nullptr, *readReturn = nullptr,
            *commitSummary = nullptr, *completionReturn = nullptr;
        for (const auto& record : task.events)
        {
            const auto& event = record.event;
            if (event.task != chain.task) continue;
            const auto set = [&](const V4FileTaskWindowObservation::RawEvent*& target) {
                if (target) return false; target = &record; return true;
            };
            bool unique = true;
            if (event.phase == diagnostic::Phase::read_content_path_value || event.phase == diagnostic::Phase::commit_content_path_value)
            {
                if (event.extra >= count) unique = false;
                else unique = set(event.phase == diagnostic::Phase::read_content_path_value
                    ? reads[static_cast<std::size_t>(event.extra)] : commits[static_cast<std::size_t>(event.extra)]);
            }
            else if (event.phase == diagnostic::Phase::read_content_paths) unique = set(readSummary);
            else if (event.phase == diagnostic::Phase::read_worker_return) unique = set(readReturn);
            else if (event.phase == diagnostic::Phase::commit_content_paths) unique = set(commitSummary);
            else if (event.phase == diagnostic::Phase::completion_return && event.extra == chain.readWork) unique = set(completionReturn);
            if (!unique) { out.observationUnavailable = true; out.unresolved.emplace_back("v4_multi_path_flow_duplicate_or_index_unavailable"); return out; }
        }
        if (!readSummary || !readReturn || !commitSummary || !completionReturn ||
            readSummary->event.thread != worker->workerThread || readReturn->event.thread != worker->workerThread ||
            commitSummary->event.thread != trace.OwnerThread() || completionReturn->event.thread != trace.OwnerThread())
        { out.observationUnavailable = true; out.unresolved.emplace_back("v4_multi_path_flow_interval_unavailable"); return out; }
        out.values.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto valid = [&](const V4FileTaskWindowObservation::RawEvent* record, DWORD thread) {
                return record && record->event.hasBlob && record->event.object == chain.content && record->event.extra == i &&
                    record->event.localTask == chain.task && record->event.localWork == chain.readWork &&
                    record->event.thread == thread && record->blob.size() == record->event.blobSize &&
                    record->blob.size() % sizeof(wchar_t) == 0;
            };
            if (!valid(reads[i], worker->workerThread) || !valid(commits[i], trace.OwnerThread()) ||
                !(readSummary->index < reads[i]->index && reads[i]->index < readReturn->index) ||
                !(commitSummary->index < commits[i]->index && commits[i]->index < completionReturn->index))
            { out.observationUnavailable = true; out.unresolved.emplace_back("v4_multi_path_flow_value_interval_unavailable"); return out; }
            const auto& path = sourceRead.paths[i];
            if (path.native().size() > (std::numeric_limits<std::size_t>::max)() / sizeof(wchar_t))
            { out.observationUnavailable = true; out.unresolved.emplace_back("v4_multi_path_flow_source_size_unavailable"); return out; }
            V4FilePathFlowObservation::Value value; value.index = i; value.modelIndex = order.modelIndexByActualOrder[i];
            value.readLogIndex = reads[i]->index; value.commitLogIndex = commits[i]->index;
            value.providerUtf16.resize(path.native().size() * sizeof(wchar_t));
            if (!value.providerUtf16.empty()) std::memcpy(value.providerUtf16.data(), path.native().data(), value.providerUtf16.size());
            value.readUtf16 = reads[i]->blob; value.commitUtf16 = commits[i]->blob;
            out.values.push_back(std::move(value));
        }
        out.task = chain.task; out.readWork = chain.readWork; out.fileWork = chain.fileWork;
        out.content = chain.content; out.operationResult = chain.operationResult;
        out.identityValid = true; out.valuesCaptured = true;
        out.exactValuesMatch = std::all_of(out.values.begin(), out.values.end(), [](const auto& value) {
            return value.providerUtf16 == value.readUtf16 && value.readUtf16 == value.commitUtf16;
        });
        if (!out.exactValuesMatch) out.unresolved.emplace_back("v4_multi_path_flow_spelling_alias_or_value_difference_unavailable");
        out.unresolved.emplace_back("v4_multi_path_flow_reentry_late_work_and_business_unverified");
        out.observationUnavailable = true;
    }
    catch (...) { out.observationUnavailable = true; }
    return out;
}

// A local link between the already-recorded transfer path and the actual
// IFileOperation per-item callback. Shell does not expose its internal source
// handle here, so this never proves which filesystem object it opened.
struct V4TrackedShellCopyObservation
{
    bool identityValid = false, localItemObserved = false, observationUnavailable = true, resourceUnavailable = false;
    std::uint64_t task = 0, fileWork = 0, call = 0;
    std::vector<V4FileTaskWindowObservation::RawEvent> records;
    std::vector<std::string> unresolved;
};
inline V4TrackedShellCopyObservation InspectV4MultiTrackedShellCopy(
    const V4MultiFileOleRunReceipt& receipt, const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const std::vector<bool>& present,
    const event_graph::V4JoinedGraphVariant& selected, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& owner,
    OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
    const MessageTrace& trace, HWND control, UINT completionMessage)
{
    namespace diagnostic = snowdesktop::external_transfer_observation;
    V4TrackedShellCopyObservation out;
    try
    {
        const auto task = V4FileTaskWindowReader::Multi(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control, completionMessage);
        const auto flow = InspectV4MultiFilePathFlow(receipt, verified, identity, present, selected,
            payload, binding, owner, adapter, epoch, trace, control, completionMessage);
        const auto tracked = [](diagnostic::Phase phase) {
            return phase >= diagnostic::Phase::tracked_copy_item_source && phase <= diagnostic::Phase::tracked_copy_post_return;
        };
        for (const auto& record : task.events) if (tracked(record.event.phase)) out.records.push_back(record);
        if (!task.identityValid || !task.directCallsiteMatched || !task.localChain || !task.passive ||
            !flow.identityValid || !flow.valuesCaptured || !flow.exactValuesMatch || flow.values.size() != 1 ||
            !payload || payload->files.size() != 1)
        { out.unresolved.emplace_back("v4_tracked_copy_receipt_path_or_task_unavailable"); return out; }
        const auto& chain = *task.localChain;
        if (flow.task != chain.task || flow.fileWork != chain.fileWork || flow.operationResult != chain.operationResult)
        { out.unresolved.emplace_back("v4_tracked_copy_result_lineage_unavailable"); return out; }
        const auto worker = std::find_if(task.passive->workers.begin(), task.passive->workers.end(),
            [&](const auto& value) { return value.kind == 2 && value.work == chain.fileWork; });
        if (worker == task.passive->workers.end() || !worker->workerThread)
        { out.unresolved.emplace_back("v4_tracked_copy_worker_unavailable"); return out; }
        const V4FileTaskWindowObservation::RawEvent* workerEnter = nullptr;
        const V4FileTaskWindowObservation::RawEvent* workerReturn = nullptr;
        for (const auto& record : task.events)
        {
            const auto& event = record.event;
            if (event.task != chain.task || event.extra != chain.fileWork || event.object != worker->worker ||
                event.thread != worker->workerThread) continue;
            auto** slot = event.phase == diagnostic::Phase::worker_execute_enter ? &workerEnter :
                event.phase == diagnostic::Phase::worker_execute_return ? &workerReturn : nullptr;
            if (slot) { if (*slot) { out.unresolved.emplace_back("v4_tracked_copy_worker_interval_ambiguous"); return out; } *slot = &record; }
        }
        constexpr auto first = static_cast<unsigned>(diagnostic::Phase::tracked_copy_item_source);
        constexpr auto last = static_cast<unsigned>(diagnostic::Phase::tracked_copy_post_return);
        std::array<const V4FileTaskWindowObservation::RawEvent*, last - first + 1> events{};
        for (const auto& record : out.records)
        {
            const auto& event = record.event;
            if (event.task != chain.task) continue;
            auto& slot = events[static_cast<unsigned>(event.phase) - first];
            if (slot || event.localTask != chain.task || event.localWork != chain.fileWork ||
                event.thread != worker->workerThread || !event.object || !event.extra)
            { out.unresolved.emplace_back("v4_tracked_copy_duplicate_or_scope_unavailable"); return out; }
            slot = &record;
        }
        if (std::any_of(events.begin(), events.end(), [](const auto* value) { return !value; }))
        { out.unresolved.emplace_back("v4_tracked_copy_callback_or_result_unavailable"); return out; }
        const auto sink = events[0]->event.object; const auto call = events[0]->event.extra;
        for (std::size_t i = 0; i < events.size(); ++i)
            if (events[i]->event.object != sink || events[i]->event.extra != call ||
                (i && events[i - 1]->index >= events[i]->index))
            { out.unresolved.emplace_back("v4_tracked_copy_occurrence_or_order_unavailable"); return out; }
        if (!workerEnter || !workerReturn || workerEnter->index >= events.front()->index ||
            events.back()->index >= workerReturn->index)
        { out.unresolved.emplace_back("v4_tracked_copy_worker_interval_unavailable"); return out; }
        const auto pointerValue = [](std::int64_t value) { return static_cast<std::uintptr_t>(static_cast<std::uint64_t>(value)); };
        if (!pointerValue(events[0]->event.value) || pointerValue(events[1]->event.value) != chain.operationResult ||
            !events[2]->event.hasBlob || events[2]->blob != flow.values[0].commitUtf16 ||
            FAILED(static_cast<HRESULT>(events[3]->event.value)) || FAILED(static_cast<HRESULT>(events[4]->event.value)) ||
            !pointerValue(events[5]->event.value) || !pointerValue(events[6]->event.value) ||
            FAILED(static_cast<HRESULT>(events[7]->event.value)) || events[8]->event.value != 0 ||
            FAILED(static_cast<HRESULT>(events[9]->event.value)))
        { out.unresolved.emplace_back("v4_tracked_copy_actual_values_unavailable_or_differ"); return out; }
        out.task = chain.task; out.fileWork = chain.fileWork; out.call = call;
        out.identityValid = true; out.localItemObserved = true;
        out.unresolved.emplace_back("v4_tracked_copy_shell_internal_open_file_id_unobserved");
        out.unresolved.emplace_back("v4_tracked_copy_reentry_late_work_and_business_unverified");
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; }
    catch (const std::length_error&) { out.resourceUnavailable = true; }
    catch (...) { out.observationUnavailable = true; }
    return out;
}

enum class V4RestrictedCopyBusinessVerdict { unknown, passed, failed };
// Classifies only the observed single-file copy business, not the remaining
// candidate axes or selected event graph. A missing/ambiguous observation is
// unknown; a negative value is actionable only after the same receipt's
// source, task, actual completion windows and Shell callback are bound.
template<class Business>
inline V4RestrictedCopyBusinessVerdict ClassifyV4RestrictedCopyBusiness(
    const V4MultiFileOleRunReceipt& receipt, const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const std::vector<bool>& present,
    const event_graph::V4JoinedGraphVariant& selected, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& owner,
    const std::shared_ptr<const V4PinnedCopyLease>& lease,
    OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
    const MessageTrace& trace, HWND control, UINT completionMessage,
    const std::shared_ptr<const Business>& business)
{
    try
    {
        const auto sealed = receipt.SealedBusiness();
        const auto* source = binding.SourceObject();
        if (!business || !sealed || sealed.get() != business.get() || sealed.owner_before(business) || business.owner_before(sealed) ||
            !payload || payload->files.size() != 1 || !binding.SameOwner(owner) || !binding.SamePayload(payload) ||
            !source || !source->RestrictedFileDrop() || !source->RestrictedPreflightSucceeded() || source->restrictedGetObservationFailed ||
            Value(verified.Axes(), verified.Coordinate(), "T03") != "copy" ||
            !receipt.PinnedCopyBeforeDropObserved() || !receipt.PinnedCopyAfterAwaitObserved() ||
            !receipt.SamePinnedCopyLease(verified, identity, payload, owner, lease) ||
            !receipt.SameBoundHost(binding, owner, adapter, epoch, trace, control) || !receipt.SourceAtExecution() ||
            !receipt.SourceAtExecution()->captured || receipt.SourceAtExecution()->candidateId != payload->candidateId ||
            receipt.SourceAtExecution()->files.size() != 1 || business->members.size() != 1 ||
            !business->baselineRevalidated || !business->sourcePayloadObserved || business->modelIndexByReadOrder.size() != 1 ||
            business->modelIndexByReadOrder[0] != 0 || !business->completion ||
            business->completion->signal != CompletionSignal::completed_messages) return V4RestrictedCopyBusinessVerdict::unknown;
        const V2CapturedOleReturn* drop = nullptr;
        for (const auto& call : receipt.CapturedReturns()) if (call.observation.kind == Kind::drop)
        { if (drop) return V4RestrictedCopyBusinessVerdict::unknown; drop = &call; }
        if (!drop || FAILED(drop->observation.result) || !drop->observation.returnedEffect ||
            *drop->observation.returnedEffect != DROPEFFECT_COPY) return V4RestrictedCopyBusinessVerdict::unknown;
        const auto flow = InspectV4MultiFilePathFlow(receipt, verified, identity, present, selected,
            payload, binding, owner, adapter, epoch, trace, control, completionMessage);
        const auto task = V4FileTaskWindowReader::Multi(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control, completionMessage);
        const auto tracked = InspectV4MultiTrackedShellCopy(receipt, verified, identity, present, selected,
            payload, binding, owner, adapter, epoch, trace, control, completionMessage);
        if (!flow.identityValid || !flow.valuesCaptured || !flow.exactValuesMatch || flow.values.size() != 1 ||
            !flow.values[0].modelIndex || *flow.values[0].modelIndex != 0 || !task.identityValid || !task.directCallsiteMatched ||
            !task.localChain || task.localChain->readMessage >= task.messages.size() ||
            task.localChain->fileMessage >= task.messages.size() || task.localChain->readMessage == task.localChain->fileMessage ||
            !tracked.identityValid || !tracked.localItemObserved || tracked.task != task.localChain->task ||
            tracked.fileWork != task.localChain->fileWork || !receipt.CompletionTraceCaptured()) return V4RestrictedCopyBusinessVerdict::unknown;
        const auto& read = task.messages[task.localChain->readMessage]; const auto& file = task.messages[task.localChain->fileMessage];
        if (!read.returned || !file.returned || !read.enteredSequence || !file.enteredSequence ||
            business->completion->sourceEntered != read.enteredSequence || business->completion->commitEntered != file.enteredSequence)
            return V4RestrictedCopyBusinessVerdict::unknown;
        const auto& initial = receipt.SourceAtExecution()->files.front(); const auto& member = business->members.front();
        if (!member.sourceId || member.source != initial.path || member.expectedBytes != initial.bytes ||
            member.sourceId->VolumeSerialNumber != initial.id.VolumeSerialNumber ||
            std::memcmp(member.sourceId->FileId.Identifier, initial.id.FileId.Identifier, sizeof(initial.id.FileId.Identifier)) != 0)
            return V4RestrictedCopyBusinessVerdict::unknown;
        if (member.outputId && member.outputBytes && member.outputBytes->observed && !member.outputBytes->matches)
            return V4RestrictedCopyBusinessVerdict::failed;
        if (business->firstReceivedLandingMatched && !*business->firstReceivedLandingMatched)
            return V4RestrictedCopyBusinessVerdict::failed;
        if (member.outputCell && member.reloadedCell && member.reloadedBytes && member.reloadedBytes->observed &&
            (!member.reloadedBytes->matches || member.outputCell->pageId != member.reloadedCell->pageId ||
             member.outputCell->column != member.reloadedCell->column || member.outputCell->row != member.reloadedCell->row))
            return V4RestrictedCopyBusinessVerdict::failed;
        return business->independentAssertionsCompleted && business->firstReceivedLandingMatched.value_or(false)
            ? V4RestrictedCopyBusinessVerdict::passed : V4RestrictedCopyBusinessVerdict::unknown;
    }
    catch (...) { return V4RestrictedCopyBusinessVerdict::unknown; }
}

// Read the actual same-task default choice and Shell MOVE callback without
// reopening the now potentially moved source. This does not identify the
// object Shell opened or establish a successful business outcome.
struct V4TrackedShellMoveObservation
{
    bool volumeDecisionObserved = false, shellCallbackObserved = false;
    bool observationUnavailable = true, resourceUnavailable = false;
    std::uint64_t task = 0, readWork = 0, fileWork = 0, call = 0;
    std::vector<V4FileTaskWindowObservation::RawEvent> records;
    std::vector<MessageObservation> messages;
    std::optional<V2CapturedOleReturn> drop;
    std::optional<V4FileTaskWindowObservation::LocalChain> localChain;
    std::optional<V4FileAbiEnvelope> abi;
    std::vector<std::string> unresolved;
};
inline V4TrackedShellMoveObservation InspectV4MultiTrackedShellMove(
    const V4MultiFileOleRunReceipt& receipt, const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const std::vector<bool>& present,
    const event_graph::V4JoinedGraphVariant& selected, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& owner,
    OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
    const MessageTrace& trace, HWND control, UINT completionMessage)
{
    namespace diagnostic = snowdesktop::external_transfer_observation;
    V4TrackedShellMoveObservation out;
    try
    {
        const auto task = V4FileTaskWindowReader::Multi(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control, completionMessage);
        const auto abi = V4FileAbiReader::Multi(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control, completionMessage);
        out.records = task.events;
        out.messages = task.messages;
        out.drop = task.drop;
        out.localChain = task.localChain;
        out.abi = abi;
        auto* source = binding.SourceObject();
        if (!task.identityValid || !task.directCallsiteMatched || !task.localChain || !task.passive ||
            !abi.uniqueEnvelopeObserved || abi.records.size() != 8 || !receipt.SourceAtExecution() || receipt.SourceAtExecution()->files.size() != 1 ||
            !payload || payload->files.size() != 1 || !source || source->wrongApartment || source->fileDropObservationFailed ||
            source->fileReads != 1 || source->fileDropReads.size() != 1)
        { out.unresolved.emplace_back("v4_move_receipt_task_or_source_unavailable"); return out; }
        const auto& chain = *task.localChain; const auto& read = source->fileDropReads.front();
        out.task = chain.task; out.readWork = chain.readWork; out.fileWork = chain.fileWork;
        if (read.capture != ole_file_source_fixture::FileDropCapture::captured || FAILED(read.result) ||
            read.result != static_cast<HRESULT>(abi.records[6].event.value) || read.threadId != source->owner ||
            read.startsAtEntry <= read.endsAtEntry ||
            read.paths.size() != 1 || read.paths[0] != receipt.SourceAtExecution()->files[0].path || chain.pathCount != 1)
        { out.unresolved.emplace_back("v4_move_actual_provider_path_unavailable"); return out; }
        const V4FileTaskWindowObservation::RawEvent *target = nullptr, *sourceVolume = nullptr, *chosen = nullptr,
            *destination = nullptr, *readPath = nullptr, *commitPath = nullptr;
        bool duplicate = false;
        for (const auto& record : out.records)
        {
            const auto& event = record.event;
            if (event.task != chain.task) continue;
            const V4FileTaskWindowObservation::RawEvent** slot = nullptr;
            switch (event.phase)
            {
            case diagnostic::Phase::move_default_target_volume: slot = &target; break;
            case diagnostic::Phase::move_default_source_volume: slot = &sourceVolume; break;
            case diagnostic::Phase::move_default_selected: slot = &chosen; break;
            case diagnostic::Phase::move_default_destination_path: slot = &destination; break;
            case diagnostic::Phase::read_content_path_value: slot = &readPath; break;
            case diagnostic::Phase::commit_content_path_value: slot = &commitPath; break;
            default: break;
            }
            if (slot) { if (*slot) duplicate = true; else *slot = &record; }
        }
        const auto& path = read.paths[0].native();
        if (path.size() > (std::numeric_limits<std::size_t>::max)() / sizeof(wchar_t))
        { out.resourceUnavailable = true; return out; }
        const auto pathBytes = path.size() * sizeof(wchar_t);
        const auto pathMatches = [&](const V4FileTaskWindowObservation::RawEvent* record) {
            return record && record->event.hasBlob && record->event.object == chain.content && record->event.extra == 0 &&
                record->event.localTask == chain.task && record->event.localWork == chain.readWork &&
                record->blob.size() == pathBytes && (!pathBytes || std::memcmp(record->blob.data(), path.data(), pathBytes) == 0);
        };
        const auto decisionMatches = [&](const V4FileTaskWindowObservation::RawEvent* record) {
            return record && record->event.object == chain.operationResult && record->event.localTask == chain.task &&
                record->event.localWork == chain.readWork && record->event.thread == trace.OwnerThread();
        };
        if (duplicate || !pathMatches(readPath) || !pathMatches(commitPath) || !decisionMatches(destination) ||
            !decisionMatches(target) || !decisionMatches(sourceVolume) || !decisionMatches(chosen) ||
            !destination->event.hasBlob || destination->blob.empty() || destination->blob.size() % sizeof(wchar_t) ||
            !(commitPath->index < destination->index && destination->index < target->index &&
              target->index < sourceVolume->index && sourceVolume->index < chosen->index) ||
            chosen->event.value != DROPEFFECT_MOVE || chosen->event.extra != 1 ||
            sourceVolume->event.extra != receipt.SourceAtExecution()->files[0].id.VolumeSerialNumber ||
            sourceVolume->event.extra != target->event.extra)
        { out.unresolved.emplace_back("v4_move_actual_volume_decision_unavailable"); return out; }
        const auto worker = std::find_if(task.passive->workers.begin(), task.passive->workers.end(),
            [&](const auto& value) { return value.kind == 2 && value.work == chain.fileWork; });
        const auto reader = std::find_if(task.passive->workers.begin(), task.passive->workers.end(),
            [&](const auto& value) { return value.kind == 1 && value.work == chain.readWork; });
        if (worker == task.passive->workers.end() || !worker->workerThread ||
            reader == task.passive->workers.end() || !reader->workerThread ||
            readPath->event.thread != reader->workerThread || commitPath->event.thread != trace.OwnerThread())
        { out.unresolved.emplace_back("v4_move_worker_unavailable"); return out; }
        out.volumeDecisionObserved = true;
        const V4FileTaskWindowObservation::RawEvent *workerEnter = nullptr, *workerReturn = nullptr;
        for (const auto& record : task.events)
        {
            const auto& event = record.event;
            if (event.task != chain.task || event.extra != chain.fileWork || event.object != worker->worker ||
                event.thread != worker->workerThread) continue;
            auto** slot = event.phase == diagnostic::Phase::worker_execute_enter ? &workerEnter :
                event.phase == diagnostic::Phase::worker_execute_return ? &workerReturn : nullptr;
            if (slot) { if (*slot) { out.unresolved.emplace_back("v4_move_worker_interval_ambiguous"); return out; } *slot = &record; }
        }
        constexpr auto first = static_cast<unsigned>(diagnostic::Phase::tracked_move_item_source);
        constexpr auto last = static_cast<unsigned>(diagnostic::Phase::tracked_move_post_return);
        std::array<const V4FileTaskWindowObservation::RawEvent*, last - first + 1> events{};
        for (const auto& record : out.records)
        {
            const auto n = static_cast<unsigned>(record.event.phase);
            if (record.event.task != chain.task || n < first || n > last) continue;
            auto& slot = events[n - first];
            if (slot || record.event.localTask != chain.task || record.event.localWork != chain.fileWork ||
                record.event.thread != worker->workerThread || !record.event.object || !record.event.extra)
            { out.unresolved.emplace_back("v4_move_callback_scope_ambiguous"); return out; }
            slot = &record;
        }
        if (std::any_of(events.begin(), events.end(), [](const auto* value) { return !value; }))
        { out.unresolved.emplace_back("v4_move_callback_incomplete"); return out; }
        const auto sink = events[0]->event.object; out.call = events[0]->event.extra;
        for (std::size_t i = 0; i < events.size(); ++i)
            if (events[i]->event.object != sink || events[i]->event.extra != out.call ||
                (i && events[i - 1]->index >= events[i]->index))
            { out.unresolved.emplace_back("v4_move_callback_identity_or_order_unavailable"); return out; }
        if (!workerEnter || !workerReturn || workerEnter->index >= events.front()->index ||
            events.back()->index >= workerReturn->index || !events[0]->event.value ||
            static_cast<std::uintptr_t>(events[1]->event.value) != chain.operationResult ||
            !events[2]->event.hasBlob || events[2]->blob != commitPath->blob ||
            FAILED(static_cast<HRESULT>(events[3]->event.value)) || FAILED(static_cast<HRESULT>(events[4]->event.value)) ||
            !events[5]->event.value || !events[6]->event.value || FAILED(static_cast<HRESULT>(events[7]->event.value)) ||
            events[8]->event.value != 0 || FAILED(static_cast<HRESULT>(events[9]->event.value)))
        { out.unresolved.emplace_back("v4_move_callback_item_or_result_unavailable"); return out; }
        out.shellCallbackObserved = true;
        out.unresolved.emplace_back("v4_move_shell_internal_open_file_id_unobserved");
        out.unresolved.emplace_back("v4_move_target_identity_late_work_and_business_unverified");
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; }
    catch (const std::length_error&) { out.resourceUnavailable = true; }
    catch (...) { out.observationUnavailable = true; }
    return out;
}

enum class V4FileGraphObservationState { unavailable, observed_local, unattributed_source_candidate,
    unattributed_task_candidate };
struct V4FileGraphEventEvidence
{
    std::string id;
    bool selectedPresent = false;
    V4FileGraphObservationState state = V4FileGraphObservationState::unavailable;
    std::optional<std::size_t> logIndex, traceIndex;
};
struct V4FileGraphOrderEvidence
{
    std::string before, after, basis;
    bool selectedOrderAgrees = false;
};
struct V4FileGraphEvidence
{
    bool identityValid = false, hypothesisObservationRecorded = false, hypothesisNotMet = false;
    bool observationUnavailable = false;
    std::vector<V4FileGraphEventEvidence> events;
    std::vector<V4FileGraphOrderEvidence> orders;
    std::vector<V2CapturedOleReturn> oleEvidence;
    std::optional<V4FilePayloadObservation> payload;
    std::optional<V4FileTaskWindowObservation> taskWindow;
    std::optional<V4FileAbiEnvelope> abiEnvelope;
    std::vector<std::string> unresolved;
};

// Compare only receipt-bound OLE call boundaries and their actual sequential
// dispatch. Provider/work markers remain unattributed graph candidates until
// their selected occurrence and complete producer interval are proved.
class V4FileGraphReader final
{
    template<class Receipt, class PayloadObserver, class TaskObserver, class AbiObserver>
    static V4FileGraphEvidence InspectBound(const Receipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    OleDragDropAdapter* adapter, bool sameHost, PayloadObserver&& observePayload,
    TaskObserver&& observeTask, AbiObserver&& observeAbi)
{
    V4FileGraphEvidence out;
    try
    {
        const auto mapping = event_graph::MapV4PresenceCandidate(identity, verified);
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
        if (!mapping.graphAvailable || raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() ||
            raw.id != receipt.Plan().candidateId || selected.id != receipt.Plan().graphId ||
            !replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
            replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
            replay.variant.unresolved != selected.unresolved || !sameHost)
        { out.observationUnavailable = true; out.unresolved.emplace_back("v4_file_graph_identity_or_host_unavailable"); return out; }
        auto requirements = mapping.spec.requirements;
        std::sort(requirements.begin(), requirements.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        if (requirements.size() != present.size())
        { out.observationUnavailable = true; out.unresolved.emplace_back("v4_file_graph_presence_shape_unavailable"); return out; }
        std::map<std::string, std::size_t> evidenceById, classById;
        std::size_t active = 0;
        for (std::size_t i = 0; i < requirements.size(); ++i)
        {
            if (!evidenceById.emplace(requirements[i].id, out.events.size()).second)
            { out.observationUnavailable = true; out.unresolved.emplace_back("v4_file_graph_duplicate_requirement"); return out; }
            out.events.push_back({requirements[i].id, present[i], V4FileGraphObservationState::unavailable, {}, {}});
            if (present[i])
            {
                if (active >= selected.classes.size())
                { out.observationUnavailable = true; out.unresolved.emplace_back("v4_file_graph_partition_unavailable"); return out; }
                classById.emplace(requirements[i].id, selected.classes[active++]);
            }
        }
        if (active != selected.classes.size())
        { out.observationUnavailable = true; out.unresolved.emplace_back("v4_file_graph_partition_unavailable"); return out; }
        std::map<std::size_t, std::size_t> rank;
        for (std::size_t i = 0; i < selected.order.size(); ++i)
        {
            const auto item = classById.find(selected.order[i]);
            if (item == classById.end() || !rank.emplace(item->second, i).second)
            { out.observationUnavailable = true; out.unresolved.emplace_back("v4_file_graph_order_unavailable"); return out; }
        }
        out.identityValid = true;
        const auto mark = [&](const std::string& id, V4FileGraphObservationState state,
            std::optional<std::size_t> log = {}, std::optional<std::size_t> window = {}) {
            const auto found = evidenceById.find(id);
            if (found == evidenceById.end()) return false;
            auto& item = out.events[found->second]; item.state = state; item.logIndex = log; item.traceIndex = window;
            return true;
        };
        const auto before = [&](const std::string& first, const std::string& last, const char* basis) {
            const auto a = classById.find(first), b = classById.find(last);
            if (a == classById.end() || b == classById.end() || !rank.contains(a->second) || !rank.contains(b->second)) return;
            const bool agrees = rank.at(a->second) < rank.at(b->second);
            out.orders.push_back({first, last, basis, agrees}); out.hypothesisObservationRecorded = true;
            if (!agrees) out.hypothesisNotMet = true;
        };
        std::vector<std::pair<std::string, std::string>> calls;
        for (const auto& item : receipt.CapturedReturns())
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
            { out.observationUnavailable = true; out.unresolved.emplace_back("v4_file_graph_ole_scope_unavailable"); return out; }
            out.oleEvidence.push_back(item);
            const auto base = "program_" + std::to_string(item.observation.programIndex);
            const auto first = base + "_call_start", last = base + "_call_return";
            if (!mark(first, V4FileGraphObservationState::observed_local) || !mark(last, V4FileGraphObservationState::observed_local))
            { out.observationUnavailable = true; out.unresolved.emplace_back("v4_file_graph_ole_requirement_unavailable"); return out; }
            out.hypothesisObservationRecorded = true;
            if (!out.events[evidenceById.at(first)].selectedPresent || !out.events[evidenceById.at(last)].selectedPresent)
                out.hypothesisNotMet = true;
            calls.emplace_back(first, last);
        }
        for (std::size_t i = 0; i < calls.size(); ++i)
        {
            before(calls[i].first, calls[i].second, "captured_same_ole_call");
            if (i) before(calls[i - 1].second, calls[i].first, "sequential_ole_dispatch");
        }
        out.payload = observePayload();
        out.taskWindow = observeTask();
        out.abiEnvelope = observeAbi();
        if (const auto& interval = receipt.SourceSessionInterval(); interval && !interval->events.empty())
        {
            const auto& first = interval->events.front();
            if (first.phase == ole_file_source_fixture::FileDropCallPhase::entered && first.call && !first.parent && !first.depth &&
                first.thread == GetCurrentThreadId())
            {
                mark("v4_first_cfhdrop_getdata_enter", V4FileGraphObservationState::unattributed_source_candidate,
                    interval->before.count);
                if (interval->events.size() > 1)
                {
                    const auto& second = interval->events[1];
                    if (second.phase == ole_file_source_fixture::FileDropCallPhase::returned && second.call == first.call &&
                        second.parent == first.parent && second.depth == first.depth && second.thread == first.thread)
                        mark("v4_first_cfhdrop_getdata_return", V4FileGraphObservationState::unattributed_source_candidate,
                            interval->before.count + 1);
                }
            }
        }
        // An explicit callsite token binds this particular task to the real
        // delayed-desktop Drop. A unique marshal/proxy/caller/provider
        // envelope then identifies the first observed source invocation. It
        // still says nothing about later calls, global quiescence or business.
        if (out.taskWindow->directCallsiteMatched && out.taskWindow->localChain &&
            out.abiEnvelope->uniqueEnvelopeObserved && out.abiEnvelope->records.size() == 8)
        {
            const std::string first = "v4_first_cfhdrop_getdata_enter", last = "v4_first_cfhdrop_getdata_return";
            if (mark(first, V4FileGraphObservationState::observed_local, out.abiEnvelope->records[5].index) &&
                mark(last, V4FileGraphObservationState::observed_local, out.abiEnvelope->records[6].index))
            {
                out.hypothesisObservationRecorded = true;
                if (!out.events[evidenceById.at(first)].selectedPresent || !out.events[evidenceById.at(last)].selectedPresent)
                    out.hypothesisNotMet = true;
                before(first, last, "same_unique_source_call_and_caller_abi_return");
            }
        }
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
        out.unresolved.emplace_back("v4_file_graph_absence_selected_occurrence_and_cross_thread_order_unverified");
        out.unresolved.emplace_back("v4_file_graph_task_reentry_completion_and_business_unverified");
        out.observationUnavailable = true;
    }
    catch (...) { out.observationUnavailable = true; }
    return out;
}
public:
    // Owned-file execution has a distinct receipt and source lifetime. Reuse
    // the graph comparator only after its own host and candidate checks;
    // the raw provider snapshot below is not a complete payload assertion.
    static V4FileGraphEvidence Multi(const V4MultiFileOleRunReceipt& receipt,
        const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
        const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
        const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& owner,
        OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
        const MessageTrace& trace, HWND control, UINT completionMessage)
    {
        auto* source = binding.SourceObject();
        const bool sameHost = source && receipt.SameBoundHost(binding, owner, adapter, epoch, trace, control);
        return InspectBound(receipt, verified, identity, present, selected, adapter, sameHost,
            [&] {
                V4FilePayloadObservation payload;
                payload.observationUnavailable = true;
                if (!source || source->owner != GetCurrentThreadId())
                { payload.unresolved.emplace_back("v4_multi_graph_source_unavailable"); return payload; }
                payload.sourceStarts = source->starts; payload.sourceEnds = source->ends;
                payload.fileReads = source->fileReads; payload.endOutcome = source->outcome;
                payload.endEffect = source->endEffect; payload.reads = source->fileDropReads;
                payload.probes = source->formatProbes;
                if (source->RestrictedFileDrop() && source->RestrictedPreflightSucceeded())
                    payload.setupProbePrefixCount = 1;
                if (receipt.SourceAtExecution() && receipt.SourceAtExecution()->files.size() == 1)
                    payload.expectedPath = receipt.SourceAtExecution()->files.front().path;
                payload.unresolved.emplace_back("v4_multi_graph_raw_source_snapshot_not_payload_proof");
                return payload;
            },
            [&] { return V4FileTaskWindowReader::Multi(receipt, verified, identity, present, selected,
                binding, owner, adapter, epoch, trace, control, completionMessage); },
            [&] { return V4FileAbiReader::Multi(receipt, verified, identity, present, selected,
                binding, owner, adapter, epoch, trace, control, completionMessage); });
    }
    static V4FileGraphEvidence Single(const V4FileOleRunReceipt& receipt,
        const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
        const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
        ole_file_source_fixture::Source& source, OleDragDropAdapter* adapter,
        const std::shared_ptr<const V3HostEpoch>& epoch, const MessageTrace& trace, HWND control, UINT completionMessage)
    {
        return InspectBound(receipt, verified, identity, present, selected, adapter,
            receipt.SameBoundHost(source, adapter, epoch, trace, control),
            [&] { return InspectV4FilePayload(receipt, verified, identity, present, selected, source, adapter, epoch, trace, control); },
            [&] { return InspectV4FileTaskWindows(receipt, verified, identity, present, selected, source, adapter, epoch, trace, control, completionMessage); },
            [&] { return InspectV4FileAbiEnvelope(receipt, verified, identity, present, selected, source, adapter, epoch, trace, control, completionMessage); });
    }
    static V4FileGraphEvidence Directory(const V4DirectoryOleRunReceipt& receipt,
        const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
        const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
        const std::shared_ptr<directory_fixture::DirectoryCopyLease>& lease,
        const directory_fixture::DirectoryShellBinding& binding,
        const std::shared_ptr<directory_fixture::DirectoryShellOwner>& owner, OleDragDropAdapter* adapter,
        const std::shared_ptr<const V3HostEpoch>& epoch, const MessageTrace& trace, HWND control, UINT completionMessage)
    {
        return InspectBound(receipt, verified, identity, present, selected, adapter,
            receipt.SameBoundHost(lease, binding, owner, adapter, epoch, trace, control),
            [&] { return InspectV4DirectorySource(receipt, verified, identity, present, selected, lease, binding, owner, adapter, epoch, trace, control); },
            [&] { return InspectV4DirectoryTaskWindows(receipt, verified, identity, present, selected, lease, binding, owner, adapter, epoch, trace, control, completionMessage); },
            [&] { return InspectV4DirectoryAbiEnvelope(receipt, verified, identity, present, selected, lease, binding, owner, adapter, epoch, trace, control, completionMessage); });
    }
};

inline V4FileGraphEvidence InspectV4MultiFileGraph(const V4MultiFileOleRunReceipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& owner,
    OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
    const MessageTrace& trace, HWND control, UINT completionMessage)
{
    return V4FileGraphReader::Multi(receipt, verified, identity, present, selected, binding, owner,
        adapter, epoch, trace, control, completionMessage);
}

inline V4FileGraphEvidence InspectV4FileGraph(const V4FileOleRunReceipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    ole_file_source_fixture::Source& source, OleDragDropAdapter* adapter,
    const std::shared_ptr<const V3HostEpoch>& epoch, const MessageTrace& trace, HWND control, UINT completionMessage)
{
    return V4FileGraphReader::Single(receipt, verified, identity, present, selected, source, adapter, epoch, trace,
        control, completionMessage);
}

inline V4FileGraphEvidence InspectV4DirectoryGraph(const V4DirectoryOleRunReceipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    const std::shared_ptr<directory_fixture::DirectoryCopyLease>& lease,
    const directory_fixture::DirectoryShellBinding& binding,
    const std::shared_ptr<directory_fixture::DirectoryShellOwner>& owner, OleDragDropAdapter* adapter,
    const std::shared_ptr<const V3HostEpoch>& epoch, const MessageTrace& trace, HWND control, UINT completionMessage)
{
    return V4FileGraphReader::Directory(receipt, verified, identity, present, selected, lease, binding, owner, adapter,
        epoch, trace, control, completionMessage);
}

// Read-only partial source evidence from one executor-owned receipt. Calls are
// observed provider events, not evidence of product selection, ABI return,
// graph-presence absence, host refresh, or an independent business result.
class V4UnicodeReadObservation final
{
public:
    static V4UnicodeReadObservation Inspect(const V4OleRunReceipt& receipt,
        const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
        const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
        const V4UnicodeSourceBinding& binding, const std::shared_ptr<V4UnicodeSourceOwner>& owner,
        OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
        const MessageTrace& trace, HWND control)
    {
        V4UnicodeReadObservation result;
        try
        {
            auto* source = binding.SourceObject();
            if (!receipt.execution_.productionStarted || !receipt.plan_.graphIdentityValid ||
                !owner || !epoch || receipt.owner_.lock() != owner || receipt.hostEpoch_.lock() != epoch ||
                !source || receipt.sourceIdentity_ != source || receipt.adapterIdentity_ != adapter ||
                !epoch->Matches(source, adapter, &trace, control) || trace.LostOrCrossThread() ||
                trace.OwnerThread() != GetCurrentThreadId() || source->ownerThread != GetCurrentThreadId())
            { result.unresolved_.emplace_back("v4_unicode_read_receipt_source_or_epoch_unavailable"); return result; }
            const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
            const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
            if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() ||
                raw.id != receipt.plan_.candidateId || raw.id != binding.CandidateId() ||
                !replay.identityValid || !replay.errors.empty() || selected.id != receipt.plan_.graphId ||
                replay.variant.id != selected.id || replay.variant.classes != selected.classes ||
                replay.variant.order != selected.order || replay.variant.unresolved != selected.unresolved)
            { result.unresolved_.emplace_back("v4_unicode_read_candidate_or_graph_unavailable"); return result; }
            if (source->wrongApartment.load() || source->observationFailed.load() || source->entries.size() != 1)
            { result.unresolved_.emplace_back("v4_unicode_read_provider_log_unavailable"); return result; }
            const auto& entry = source->entries.front();
            if (entry.format != CF_UNICODETEXT || entry.index != -1 || entry.tymed != TYMED_HGLOBAL ||
                entry.readFailure || entry.getDataFailure != binding.ConfiguredGetFailure() || entry.bytes != binding.ExactBytes())
            { result.unresolved_.emplace_back("v4_unicode_read_source_changed"); return result; }
            {
                std::lock_guard lock(source->observationMutex);
                if (source->wrongApartment.load() || source->observationFailed.load())
                { result.unresolved_.emplace_back("v4_unicode_read_provider_log_unavailable"); return result; }
                for (const auto& call : source->calls)
                {
                    if (call.thread != source->ownerThread)
                    { result.unresolved_.emplace_back("v4_unicode_read_cross_thread_unavailable"); return result; }
                    result.calls_.push_back(call);
                }
            }
            if (!epoch->Matches(source, adapter, &trace, control))
            { result.calls_.clear(); result.unresolved_.emplace_back("v4_unicode_read_epoch_changed"); return result; }
            result.unresolved_.emplace_back("v4_unicode_read_dispatch_stage_and_non_adapter_reentrancy_unverified");
            result.unresolved_.emplace_back("v4_unicode_read_abi_return_and_presence_interval_unverified");
            result.unresolved_.emplace_back("v4_unicode_read_host_refresh_and_independent_business_unverified");
            result.localLogCaptured_ = true;
        }
        catch (...) { result.calls_.clear(); result.localLogCaptured_ = false; result.resourceOrObservationUnavailable_ = true; }
        return result;
    }
    bool LocalLogCaptured() const noexcept { return localLogCaptured_; }
    bool ResourceOrObservationUnavailable() const noexcept { return resourceOrObservationUnavailable_; }
    const std::vector<ole_content_source_fixture::Call>& Calls() const noexcept { return calls_; }
    const std::vector<std::string>& Unresolved() const noexcept { return unresolved_; }
private:
    bool localLogCaptured_ = false, resourceOrObservationUnavailable_ = false;
    std::vector<ole_content_source_fixture::Call> calls_;
    std::vector<std::string> unresolved_;
};

struct V4UnicodeCallerRecord
{
    std::size_t index = 0;
    snowdesktop::external_transfer_observation::Event event;
};
struct V4UnicodeCallerEvidence
{
    bool actualCallerReturned = false, uniqueProviderCallObserved = false, observationUnavailable = false;
    std::vector<V4UnicodeCallerRecord> records;
    std::optional<ole_content_source_fixture::Call> providerCall;
    std::vector<std::string> unresolved;
};

// Correlate a real same-source caller ABI return with the captured Drop scope.
// Provider logs have no common nested invocation ID: unique matching records
// remain local observations, not proof of consumed text or product completion.
inline V4UnicodeCallerEvidence InspectV4UnicodeCaller(const V4OleRunReceipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    const V4UnicodeSourceBinding& binding, const std::shared_ptr<V4UnicodeSourceOwner>& owner,
    OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
    const MessageTrace& trace, HWND control)
{
    namespace diagnostic = snowdesktop::external_transfer_observation;
    V4UnicodeCallerEvidence out;
    try
    {
        const auto provider = V4UnicodeReadObservation::Inspect(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control);
        if (!provider.LocalLogCaptured() || provider.ResourceOrObservationUnavailable() || !diagnostic::Complete() ||
            !receipt.SameBoundHost(binding, owner, adapter, epoch, trace, control))
        { out.unresolved.emplace_back("v4_unicode_caller_provider_or_log_unavailable"); return out; }
        auto* source = binding.SourceObject();
        if (!source) { out.unresolved.emplace_back("v4_unicode_caller_source_unavailable"); return out; }
        const V2CapturedOleReturn* drop = nullptr;
        for (const auto& item : receipt.CapturedReturns())
            if (item.observation.kind == Kind::drop)
            { if (drop) { out.unresolved.emplace_back("v4_unicode_caller_drop_ambiguous"); return out; } drop = &item; }
        if (!drop || drop->entered.kind != snowdesktop::ole_adapter_observation::Kind::drop ||
            drop->entered.phase != snowdesktop::ole_adapter_observation::Phase::entered ||
            drop->returned.phase != snowdesktop::ole_adapter_observation::Phase::returned ||
            drop->returned.kind != drop->entered.kind ||
            !drop->entered.session || !drop->entered.call || drop->entered.parent || drop->entered.depth ||
            drop->entered.thread != GetCurrentThreadId() || drop->entered.adapter != reinterpret_cast<std::uintptr_t>(adapter) ||
            drop->returned.session != drop->entered.session || drop->returned.call != drop->entered.call ||
            drop->returned.thread != drop->entered.thread || drop->returned.adapter != drop->entered.adapter ||
            drop->returned.result != drop->observation.result)
        { out.unresolved.emplace_back("v4_unicode_caller_drop_scope_unavailable"); return out; }
        const auto sourceAddress = reinterpret_cast<std::uintptr_t>(static_cast<IDataObject*>(source));
        diagnostic::ReadSession session;
        if (!session.Open()) { out.unresolved.emplace_back("v4_unicode_caller_session_unavailable"); return out; }
        for (std::size_t i = 0; i < session.Count(); ++i)
        {
            diagnostic::Event event;
            if (!session.ReadEvent(i, event)) { out.unresolved.emplace_back("v4_unicode_caller_log_lost"); return out; }
            if (event.phase == diagnostic::Phase::unicode_getdata_caller_enter ||
                event.phase == diagnostic::Phase::unicode_getdata_caller_return)
                out.records.push_back({i, event});
        }
        if (out.records.size() != 2 || out.records[0].event.phase != diagnostic::Phase::unicode_getdata_caller_enter ||
            out.records[1].event.phase != diagnostic::Phase::unicode_getdata_caller_return ||
            out.records[0].index >= out.records[1].index)
        { out.unresolved.emplace_back("v4_unicode_caller_count_or_order_unavailable"); return out; }
        const auto& first = out.records[0].event; const auto& last = out.records[1].event;
        const auto scopeMatches = [&](const diagnostic::Event& event) {
            const auto& scope = event.adapterScope;
            return event.object == sourceAddress && event.thread == drop->entered.thread && scope.observed &&
                scope.session == drop->entered.session && scope.call == drop->entered.call &&
                scope.parent == drop->entered.parent && scope.adapter == drop->entered.adapter &&
                scope.thread == drop->entered.thread && scope.depth == drop->entered.depth &&
                scope.kind == static_cast<unsigned>(snowdesktop::ole_adapter_observation::Kind::drop);
        };
        if (!first.extra || first.extra != last.extra || !scopeMatches(first) || !scopeMatches(last))
        { out.unresolved.emplace_back("v4_unicode_caller_source_or_scope_unavailable"); return out; }
        out.actualCallerReturned = true;
        for (const auto& call : provider.Calls())
            if (!call.query && call.format == CF_UNICODETEXT)
            {
                if (out.providerCall) { out.unresolved.emplace_back("v4_unicode_caller_provider_multiple_unavailable"); return out; }
                out.providerCall = call;
            }
        if (!out.providerCall || out.providerCall->thread != drop->entered.thread ||
            out.providerCall->aspect != DVASPECT_CONTENT || out.providerCall->index != -1 ||
            out.providerCall->requestedTymed != TYMED_HGLOBAL || out.providerCall->targetDevicePresent ||
            static_cast<std::int64_t>(out.providerCall->result) != last.value ||
            (SUCCEEDED(out.providerCall->result) && (last.currentRevision != TYMED_HGLOBAL ||
                out.providerCall->returnedTymed != TYMED_HGLOBAL || !out.providerCall->mediumObserved ||
                out.providerCall->hglobalBytes != binding.ExactBytes())))
        { out.unresolved.emplace_back("v4_unicode_caller_provider_values_unavailable"); return out; }
        out.uniqueProviderCallObserved = true;
        out.unresolved.emplace_back("v4_unicode_caller_provider_nested_interval_and_non_adapter_reentrancy_unverified");
        out.unresolved.emplace_back("v4_unicode_consumed_contents_presence_completion_and_business_unverified");
    }
    catch (...) { out.observationUnavailable = true; }
    return out;
}

struct V4UnicodeProviderIntervalEvidence
{
    bool localProviderIntervalObserved = false, observationUnavailable = false;
    std::vector<V4UnicodeCallerRecord> providerRecords;
    std::vector<std::string> unresolved;
};

// Both logs use one process-wide sequence; only same-thread records inside
// one actually captured caller invocation are compared. Provider return is a
// marker before return; the outer caller record establishes its ABI return.
inline V4UnicodeProviderIntervalEvidence InspectV4UnicodeProviderInterval(const V4OleRunReceipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    const V4UnicodeSourceBinding& binding, const std::shared_ptr<V4UnicodeSourceOwner>& owner,
    OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
    const MessageTrace& trace, HWND control)
{
    namespace diagnostic = snowdesktop::external_transfer_observation;
    V4UnicodeProviderIntervalEvidence out;
    try
    {
        const auto caller = InspectV4UnicodeCaller(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control);
        if (!caller.actualCallerReturned || !caller.uniqueProviderCallObserved || caller.observationUnavailable ||
            caller.records.size() != 2 || !diagnostic::Complete())
        { out.unresolved.emplace_back("v4_unicode_provider_caller_unavailable"); return out; }
        diagnostic::ReadSession session;
        if (!session.Open()) { out.unresolved.emplace_back("v4_unicode_provider_session_unavailable"); return out; }
        const auto& before = caller.records[0]; const auto& after = caller.records[1];
        for (std::size_t i = before.index + 1; i < after.index; ++i)
        {
            diagnostic::Event event;
            if (!session.ReadEvent(i, event)) { out.unresolved.emplace_back("v4_unicode_provider_log_lost"); return out; }
            if (event.phase == diagnostic::Phase::content_provider_getdata_enter ||
                event.phase == diagnostic::Phase::content_provider_getdata_return)
                out.providerRecords.push_back({i, event});
        }
        if (out.providerRecords.size() != 2 ||
            out.providerRecords[0].event.phase != diagnostic::Phase::content_provider_getdata_enter ||
            out.providerRecords[1].event.phase != diagnostic::Phase::content_provider_getdata_return ||
            out.providerRecords[0].index >= out.providerRecords[1].index)
        { out.unresolved.emplace_back("v4_unicode_provider_interval_ambiguous"); return out; }
        const auto& start = out.providerRecords[0].event; const auto& finish = out.providerRecords[1].event;
        const auto& scope = before.event.adapterScope;
        const auto sameScope = [&](const diagnostic::Event& item) {
            const auto& actual = item.adapterScope;
            return item.object == before.event.object && item.thread == before.event.thread && actual.observed &&
                actual.session == scope.session && actual.call == scope.call && actual.parent == scope.parent &&
                actual.adapter == scope.adapter && actual.thread == scope.thread && actual.depth == scope.depth && actual.kind == scope.kind;
        };
        if (!start.extra || start.extra != finish.extra || !sameScope(start) || !sameScope(finish) ||
            finish.value != after.event.value || !caller.providerCall ||
            finish.value != static_cast<std::int64_t>(caller.providerCall->result))
        { out.unresolved.emplace_back("v4_unicode_provider_identity_or_result_unavailable"); return out; }
        out.localProviderIntervalObserved = true;
        out.unresolved.emplace_back("v4_unicode_provider_other_reentrancy_and_presence_absence_unverified");
        out.unresolved.emplace_back("v4_unicode_provider_consumption_completion_and_business_unverified");
    }
    catch (...) { out.observationUnavailable = true; }
    return out;
}

struct V4UnicodeValueRecord
{
    std::size_t index = 0;
    snowdesktop::external_transfer_observation::Event event;
    std::vector<std::byte> actualBytes;
};
struct V4UnicodeConsumptionEvidence
{
    bool localValuesCaptured = false, observationUnavailable = false;
    std::vector<V4UnicodeValueRecord> records;
    std::vector<std::string> unresolved;
};

// Records actual producer values only. Scope and order provide a restricted
// local observation; no claim is made that a staged path became the final file.
inline V4UnicodeConsumptionEvidence InspectV4UnicodeConsumption(const V4OleRunReceipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    const V4UnicodeSourceBinding& binding, const std::shared_ptr<V4UnicodeSourceOwner>& owner,
    OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
    const MessageTrace& trace, HWND control)
{
    namespace diagnostic = snowdesktop::external_transfer_observation;
    V4UnicodeConsumptionEvidence out;
    try
    {
        const auto caller = InspectV4UnicodeCaller(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control);
        if (!caller.actualCallerReturned || !caller.uniqueProviderCallObserved || caller.observationUnavailable ||
            caller.records.size() != 2 || !diagnostic::Complete())
        { out.unresolved.emplace_back("v4_unicode_consumption_caller_unavailable"); return out; }
        diagnostic::ReadSession session;
        if (!session.Open()) { out.unresolved.emplace_back("v4_unicode_consumption_session_unavailable"); return out; }
        const std::array<diagnostic::Phase, 4> phases = {diagnostic::Phase::unicode_extracted_value,
            diagnostic::Phase::text_classified_value, diagnostic::Phase::text_encoded_value, diagnostic::Phase::text_staged_path};
        for (std::size_t i = 0; i < session.Count(); ++i)
        {
            diagnostic::Event event;
            if (!session.ReadEvent(i, event)) { out.unresolved.emplace_back("v4_unicode_consumption_log_lost"); return out; }
            if (std::find(phases.begin(), phases.end(), event.phase) == phases.end()) continue;
            V4UnicodeValueRecord record; record.index = i; record.event = event;
            if (event.blobSize > diagnostic::BlobLimit()) { out.unresolved.emplace_back("v4_unicode_consumption_blob_unavailable"); return out; }
            record.actualBytes.resize(event.blobSize); std::size_t size = 0;
            if (!session.CopyBlob(i, record.actualBytes.data(), record.actualBytes.size(), size) || size != record.actualBytes.size())
            { out.unresolved.emplace_back("v4_unicode_consumption_blob_unavailable"); return out; }
            out.records.push_back(std::move(record));
        }
        if (out.records.size() != phases.size())
        { out.unresolved.emplace_back("v4_unicode_consumption_count_unavailable"); return out; }
        const auto& callerReturn = caller.records[1];
        const auto& scope = callerReturn.event.adapterScope;
        for (std::size_t i = 0; i < phases.size(); ++i)
        {
            const auto& record = out.records[i]; const auto& current = record.event.adapterScope;
            if (record.event.phase != phases[i] || (i && out.records[i - 1].index >= record.index) ||
                record.index <= callerReturn.index || !current.observed || current.session != scope.session ||
                current.call != scope.call || current.parent != scope.parent || current.adapter != scope.adapter ||
                current.thread != scope.thread || current.depth != scope.depth || current.kind != scope.kind ||
                record.event.thread != callerReturn.event.thread)
            { out.unresolved.emplace_back("v4_unicode_consumption_scope_or_order_unavailable"); return out; }
        }
        if (out.records[0].event.object != callerReturn.event.object || out.records[0].event.extra != callerReturn.event.extra ||
            !out.records[1].event.object || out.records[1].event.object != out.records[2].event.object ||
            out.records[1].event.object != out.records[3].event.object ||
            out.records[0].actualBytes.size() % sizeof(wchar_t) || out.records[1].actualBytes.size() % sizeof(wchar_t) ||
            out.records[3].actualBytes.empty() || out.records[3].actualBytes.size() % sizeof(wchar_t))
        { out.unresolved.emplace_back("v4_unicode_consumption_value_identity_unavailable"); return out; }
        out.localValuesCaptured = true;
        out.unresolved.emplace_back("v4_unicode_consumption_nested_source_to_snapshot_identity_unverified");
        out.unresolved.emplace_back("v4_unicode_staged_to_final_file_and_completion_unverified");
    }
    catch (...) { out.localValuesCaptured = false; out.observationUnavailable = true; }
    return out;
}

// Only a unique actual request inside the captured Drop adapter scope is
// reported. Request markers precede function/ABI return and cannot prove the
// background worker, a WndProc return, quiescence or a business result.
class V4RefreshRequestObservation final
{
public:
    struct Record { std::size_t index; snowdesktop::external_transfer_observation::Event event; };
    static V4RefreshRequestObservation Inspect(const V4OleRunReceipt& receipt,
        const V4UnicodeSourceBinding& binding, const std::shared_ptr<V4UnicodeSourceOwner>& owner,
        OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
        const MessageTrace& trace, HWND control)
    {
        using namespace snowdesktop::external_transfer_observation;
        V4RefreshRequestObservation result;
        try
        {
            if (!receipt.SameBoundHost(binding, owner, adapter, epoch, trace, control) || !Complete())
            { result.unresolved_.emplace_back("v4_refresh_request_receipt_or_log_unavailable"); return result; }
            ReadSession diagnosticSession;
            if (!diagnosticSession.Open()) { result.unresolved_.emplace_back("v4_refresh_request_session_unavailable"); return result; }
            const V2CapturedOleReturn* drop = nullptr;
            for (const auto& call : receipt.CapturedReturns())
                if (call.observation.kind == Kind::drop)
                {
                    if (drop) { result.unresolved_.emplace_back("v4_refresh_multiple_drops_unavailable"); return result; }
                    drop = &call;
                }
            if (!drop || drop->entered.kind != snowdesktop::ole_adapter_observation::Kind::drop ||
                drop->returned.kind != drop->entered.kind || drop->entered.phase != snowdesktop::ole_adapter_observation::Phase::entered ||
                drop->returned.phase != snowdesktop::ole_adapter_observation::Phase::returned ||
                !drop->entered.session || !drop->entered.call || drop->entered.parent || drop->entered.depth ||
                drop->entered.session != drop->returned.session || drop->entered.call != drop->returned.call ||
                drop->entered.adapter != reinterpret_cast<std::uintptr_t>(adapter) ||
                drop->returned.adapter != drop->entered.adapter || drop->entered.thread != GetCurrentThreadId() ||
                drop->returned.thread != drop->entered.thread || drop->returned.result != drop->observation.result)
            { result.unresolved_.emplace_back("v4_refresh_drop_scope_unavailable"); return result; }
            Event begin, end; bool sawBegin = false, sawEnd = false; std::size_t beginIndex = 0, endIndex = 0;
            for (std::size_t i = 0; i < diagnosticSession.Count(); ++i)
            {
                Event item;
                if (!diagnosticSession.ReadEvent(i, item)) { result.unresolved_.emplace_back("v4_refresh_request_log_unavailable"); return result; }
                if (item.phase == Phase::shell_refresh_request_enter)
                { if (sawBegin) { result.unresolved_.emplace_back("v4_refresh_request_multiple_unavailable"); return result; } begin = item; beginIndex = i; sawBegin = true; result.enter_ = Record{i, item}; }
                else if (item.phase == Phase::shell_refresh_request_return)
                { if (sawEnd) { result.unresolved_.emplace_back("v4_refresh_request_multiple_unavailable"); return result; } end = item; endIndex = i; sawEnd = true; result.return_ = Record{i, item}; }
            }
            const auto sameScope = [&](const Event& item) {
                const auto& scope = item.adapterScope;
                return item.object == epoch->app_ && item.thread == drop->entered.thread && scope.observed &&
                    scope.session == drop->entered.session && scope.call == drop->entered.call &&
                    scope.parent == drop->entered.parent && scope.adapter == drop->entered.adapter &&
                    scope.thread == drop->entered.thread && scope.depth == drop->entered.depth &&
                    scope.kind == static_cast<unsigned>(snowdesktop::ole_adapter_observation::Kind::drop);
            };
            if (!sawBegin || !sawEnd || beginIndex >= endIndex || !sameScope(begin) || !sameScope(end) ||
                (begin.value & 1) || begin.extra == (std::numeric_limits<std::uint64_t>::max)() ||
                end.extra != begin.extra + 1 || !(end.value & 2))
            { result.unresolved_.emplace_back("v4_refresh_request_scope_or_revision_unavailable"); return result; }
            result.unresolved_.emplace_back("v4_refresh_request_non_adapter_reentrancy_and_abi_return_unverified");
            result.unresolved_.emplace_back("v4_refresh_request_worker_window_and_business_unverified");
            result.beforeRevision_ = begin.extra; result.afterRevision_ = end.extra;
            result.beforeFlags_ = begin.value; result.afterFlags_ = end.value;
            result.requestMarkerCaptured_ = true;
        }
        catch (...) { result.requestMarkerCaptured_ = false; result.resourceOrObservationUnavailable_ = true; }
        return result;
    }
    bool RequestMarkerCaptured() const noexcept { return requestMarkerCaptured_; }
    std::uint64_t BeforeRevision() const noexcept { return beforeRevision_; }
    std::uint64_t AfterRevision() const noexcept { return afterRevision_; }
    std::int64_t BeforeFlags() const noexcept { return beforeFlags_; }
    std::int64_t AfterFlags() const noexcept { return afterFlags_; }
    const std::optional<Record>& EnterRecord() const noexcept { return enter_; }
    const std::optional<Record>& ReturnRecord() const noexcept { return return_; }
    bool ResourceOrObservationUnavailable() const noexcept { return resourceOrObservationUnavailable_; }
    const std::vector<std::string>& Unresolved() const noexcept { return unresolved_; }
private:
    bool requestMarkerCaptured_ = false, resourceOrObservationUnavailable_ = false;
    std::uint64_t beforeRevision_ = 0, afterRevision_ = 0;
    std::int64_t beforeFlags_ = 0, afterFlags_ = 0;
    std::optional<Record> enter_, return_;
    std::vector<std::string> unresolved_;
};

struct V4UnicodeRefreshLinkEvidence
{
    bool sameDropScopeAndOrderObserved = false, observationUnavailable = false;
    std::optional<std::uint64_t> requestedRevision;
    std::vector<std::string> unresolved;
};

// The two independent observations are reconstructed from the same actual
// receipt/log. Order within this log does not establish causality in the
// presence of reentrancy or connect the staged file to the final output.
inline V4UnicodeRefreshLinkEvidence InspectV4UnicodeRefreshLink(const V4OleRunReceipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    const V4UnicodeSourceBinding& binding, const std::shared_ptr<V4UnicodeSourceOwner>& owner,
    OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
    const MessageTrace& trace, HWND control)
{
    V4UnicodeRefreshLinkEvidence out;
    try
    {
        const auto content = InspectV4UnicodeConsumption(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control);
        const auto refresh = V4RefreshRequestObservation::Inspect(receipt, binding, owner, adapter, epoch, trace, control);
        if (!content.localValuesCaptured || content.observationUnavailable || content.records.size() != 4 ||
            !refresh.RequestMarkerCaptured() || refresh.ResourceOrObservationUnavailable() ||
            !refresh.EnterRecord() || !refresh.ReturnRecord())
        { out.unresolved.emplace_back("v4_unicode_refresh_link_observation_unavailable"); return out; }
        const auto& stage = content.records.back(); const auto& request = *refresh.EnterRecord();
        const auto& a = stage.event.adapterScope; const auto& b = request.event.adapterScope;
        if (stage.index >= request.index || !a.observed || !b.observed ||
            a.session != b.session || a.call != b.call || a.parent != b.parent || a.adapter != b.adapter ||
            a.thread != b.thread || a.depth != b.depth || a.kind != b.kind || stage.event.thread != request.event.thread)
        { out.unresolved.emplace_back("v4_unicode_refresh_link_scope_or_order_unavailable"); return out; }
        out.sameDropScopeAndOrderObserved = true;
        out.requestedRevision = refresh.AfterRevision();
        out.unresolved.emplace_back("v4_unicode_refresh_link_reentrancy_and_worker_attribution_unverified");
        out.unresolved.emplace_back("v4_unicode_refresh_link_staged_to_final_and_business_unverified");
    }
    catch (...) { out.observationUnavailable = true; }
    return out;
}

// Correlates only raw records from one revision/snapshot and one BackgroundWork
// serial. It is a local observation, not a certificate of quiescence, WndProc
// return, absence of reentrancy or business success.
class V4RefreshWorkerObservation final
{
public:
    struct AppRecord { std::size_t index; snowdesktop::external_transfer_observation::Event event; };
    struct WorkerRecord { std::size_t index; snowdesktop::background_work_observation::Event event; };
    static V4RefreshWorkerObservation Inspect(const V4OleRunReceipt& receipt,
        const V4UnicodeSourceBinding& binding, const std::shared_ptr<V4UnicodeSourceOwner>& owner,
        OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
        const MessageTrace& trace, HWND control, const V4RefreshRequestObservation& request)
    {
        namespace app = snowdesktop::external_transfer_observation;
        namespace work = snowdesktop::background_work_observation;
        V4RefreshWorkerObservation out;
        try
        {
            if (!receipt.SameBoundHost(binding, owner, adapter, epoch, trace, control) ||
                !request.RequestMarkerCaptured() || !app::Complete() || !work::Complete())
            { out.unresolved_.emplace_back("v4_refresh_worker_identity_or_logs_unavailable"); return out; }
            app::ReadSession appSession;
            if (!appSession.Open()) { out.unresolved_.emplace_back("v4_refresh_worker_app_session_unavailable"); return out; }
            std::array<app::Event, 6> appEvents{};
            std::array<std::size_t, 6> indices{};
            std::array<bool, 6> found{};
            const std::array<app::Phase, 6> phases = {app::Phase::shell_refresh_submit_enter,
                app::Phase::shell_refresh_submit_return, app::Phase::shell_refresh_read_enter,
                app::Phase::shell_refresh_read_return, app::Phase::shell_refresh_apply_enter,
                app::Phase::shell_refresh_apply_return};
            for (std::size_t i = 0; i < appSession.Count(); ++i)
            {
                app::Event event;
                if (!appSession.ReadEvent(i, event)) { out.unresolved_.emplace_back("v4_refresh_worker_app_log_lost"); return out; }
                if (event.extra != request.AfterRevision()) continue;
                for (std::size_t p = 0; p < phases.size(); ++p)
                    if (event.phase == phases[p])
                    {
                        out.appRecords_.push_back({i, event});
                        if (found[p]) { out.unresolved_.emplace_back("v4_refresh_worker_app_records_ambiguous"); return out; }
                        found[p] = true; indices[p] = i; appEvents[p] = event;
                    }
            }
            if (std::find(found.begin(), found.end(), false) != found.end() || !appEvents[0].object ||
                indices[0] >= indices[1] || indices[2] >= indices[3] || indices[4] >= indices[5] ||
                appEvents[1].value != 1 || appEvents[3].value != 1 || appEvents[4].value != 1 || appEvents[5].value != 1)
            { out.unresolved_.emplace_back("v4_refresh_worker_app_phase_or_result_unavailable"); return out; }
            for (const auto& event : appEvents)
                if (event.object != appEvents[0].object)
                { out.unresolved_.emplace_back("v4_refresh_worker_snapshot_ambiguous"); return out; }
            if (appEvents[0].thread != GetCurrentThreadId() || appEvents[1].thread != appEvents[0].thread ||
                appEvents[4].thread != appEvents[0].thread || appEvents[5].thread != appEvents[0].thread ||
                appEvents[2].thread != appEvents[3].thread)
            { out.unresolved_.emplace_back("v4_refresh_worker_threads_unavailable"); return out; }
            const std::array<work::Phase, 7> workPhases = {work::Phase::queued, work::Phase::worker_enter,
                work::Phase::worker_return, work::Phase::ready, work::Phase::post_result,
                work::Phase::apply_enter, work::Phase::apply_return};
            std::array<work::Event, 7> workEvents{};
            std::array<std::size_t, 7> workIndices{};
            std::array<bool, 7> workFound{};
            for (std::size_t i = 0; i < work::Count(); ++i)
            {
                work::Event event;
                if (!work::Read(i, event)) { out.unresolved_.emplace_back("v4_refresh_worker_queue_log_lost"); return out; }
                if (event.context != appEvents[0].object) continue;
                out.workerRecords_.push_back({i, event});
                if (event.phase == work::Phase::coalesced || event.phase == work::Phase::queue_full || event.phase == work::Phase::discarded)
                { out.unresolved_.emplace_back("v4_refresh_worker_coalesced_failed_or_discarded"); return out; }
                for (std::size_t p = 0; p < workPhases.size(); ++p)
                    if (event.phase == workPhases[p])
                    {
                        if (workFound[p]) { out.unresolved_.emplace_back("v4_refresh_worker_queue_records_ambiguous"); return out; }
                        workFound[p] = true; workIndices[p] = i; workEvents[p] = event;
                    }
            }
            out.logScanComplete_ = true;
            if (std::find(workFound.begin(), workFound.end(), false) != workFound.end() ||
                !workEvents[0].state || !workEvents[0].serial)
            { out.unresolved_.emplace_back("v4_refresh_worker_queue_records_incomplete"); return out; }
            for (std::size_t i = 0; i < workEvents.size(); ++i)
                if (workEvents[i].state != workEvents[0].state || workEvents[i].serial != workEvents[0].serial ||
                    (i && workIndices[i - 1] >= workIndices[i]))
                { out.unresolved_.emplace_back("v4_refresh_worker_queue_identity_or_order_unavailable"); return out; }
            if (!workEvents[4].result || workEvents[4].window != reinterpret_cast<std::uintptr_t>(control) ||
                workEvents[4].message != kBackgroundShellReadyMessage ||
                workEvents[1].thread != appEvents[2].thread || workEvents[2].thread != appEvents[3].thread ||
                workEvents[5].thread != appEvents[4].thread || workEvents[6].thread != appEvents[5].thread)
            { out.unresolved_.emplace_back("v4_refresh_worker_post_or_thread_unavailable"); return out; }
            out.unresolved_.emplace_back("v4_refresh_worker_wndproc_return_and_producer_quiescence_unverified");
            out.unresolved_.emplace_back("v4_refresh_worker_non_adapter_reentrancy_and_business_unverified");
            out.context_ = appEvents[0].object; out.state_ = workEvents[0].state;
            out.serial_ = workEvents[0].serial; out.revision_ = request.AfterRevision(); out.localRecordsCaptured_ = true;
        }
        catch (...) { out.localRecordsCaptured_ = false; out.observationUnavailable_ = true; }
        return out;
    }
    bool LocalRecordsCaptured() const noexcept { return localRecordsCaptured_; }
    bool ObservationUnavailable() const noexcept { return observationUnavailable_; }
    std::uintptr_t Context() const noexcept { return context_; }
    std::uintptr_t State() const noexcept { return state_; }
    std::uint64_t Serial() const noexcept { return serial_; }
    std::uint64_t Revision() const noexcept { return revision_; }
    // Each index is local to its log; the two logs are not one clock. On an
    // early failure the returned records are an explicitly incomplete scan.
    bool LogScanComplete() const noexcept { return logScanComplete_; }
    const std::vector<AppRecord>& AppRecords() const noexcept { return appRecords_; }
    const std::vector<WorkerRecord>& WorkerRecords() const noexcept { return workerRecords_; }
    const std::vector<std::string>& Unresolved() const noexcept { return unresolved_; }
private:
    bool localRecordsCaptured_ = false, observationUnavailable_ = false, logScanComplete_ = false;
    std::uintptr_t context_ = 0, state_ = 0;
    std::uint64_t serial_ = 0, revision_ = 0;
    std::vector<AppRecord> appRecords_;
    std::vector<WorkerRecord> workerRecords_;
    std::vector<std::string> unresolved_;
};

enum class V4TextGraphObservationState { unavailable, observed_local, unattributed_window_candidate,
    unattributed_request_candidate };
struct V4TextGraphEventEvidence
{
    std::string id;
    bool selectedPresent = false;
    V4TextGraphObservationState state = V4TextGraphObservationState::unavailable;
    std::optional<std::size_t> logIndex, traceIndex;
};
struct V4TextGraphOrderEvidence
{
    std::string before, after, basis;
    bool selectedOrderAgrees = false;
};
struct V4TextGraphEvidence
{
    bool identityValid = false, hypothesisObservationRecorded = false, hypothesisNotMet = false, observationUnavailable = false;
    std::vector<V4TextGraphEventEvidence> events;
    std::vector<V4TextGraphOrderEvidence> orders;
    std::vector<V2CapturedOleReturn> oleEvidence;
    std::vector<V4UnicodeCallerRecord> callerEvidence, providerEvidence;
    std::optional<ole_content_source_fixture::Call> providerCall;
    std::vector<std::string> unresolved;
};

// Only actual, receipt-bound local occurrences and proved same-thread order
// are compared. An empty log cannot prove absence; unrelated refresh-window
// messages are kept as candidates until their worker identity is established.
inline V4TextGraphEvidence InspectV4TextGraph(const V4OleRunReceipt& receipt,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    const V4UnicodeSourceBinding& binding, const std::shared_ptr<V4UnicodeSourceOwner>& owner,
    OleDragDropAdapter* adapter, const std::shared_ptr<const V3HostEpoch>& epoch,
    const MessageTrace& trace, HWND control)
{
    V4TextGraphEvidence out;
    try
    {
        const auto mapping = event_graph::MapV4PresenceCandidate(identity, verified);
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
        if (!mapping.graphAvailable || raw.resourceUnavailable || !raw.errors.empty() || raw.id != receipt.Plan().candidateId ||
            !replay.identityValid || !replay.errors.empty() || selected.id != receipt.Plan().graphId ||
            replay.variant.id != selected.id || replay.variant.classes != selected.classes ||
            replay.variant.order != selected.order || replay.variant.unresolved != selected.unresolved ||
            !receipt.SameBoundHost(binding, owner, adapter, epoch, trace, control))
        { out.unresolved.emplace_back("v4_text_graph_identity_or_host_unavailable"); return out; }
        auto requirements = mapping.spec.requirements;
        std::sort(requirements.begin(), requirements.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        if (requirements.size() != present.size()) { out.unresolved.emplace_back("v4_text_graph_presence_shape_unavailable"); return out; }
        std::map<std::string, std::size_t> evidenceById, classById;
        std::size_t active = 0;
        for (std::size_t i = 0; i < requirements.size(); ++i)
        {
            evidenceById.emplace(requirements[i].id, out.events.size());
            out.events.push_back({requirements[i].id, present[i], V4TextGraphObservationState::unavailable, {}, {}});
            if (present[i])
            {
                if (active >= selected.classes.size()) { out.unresolved.emplace_back("v4_text_graph_partition_unavailable"); return out; }
                classById.emplace(requirements[i].id, selected.classes[active++]);
            }
        }
        if (active != selected.classes.size()) { out.unresolved.emplace_back("v4_text_graph_partition_unavailable"); return out; }
        std::map<std::size_t, std::size_t> rank;
        for (std::size_t i = 0; i < selected.order.size(); ++i)
        {
            const auto found = classById.find(selected.order[i]);
            if (found == classById.end() || !rank.emplace(found->second, i).second)
            { out.unresolved.emplace_back("v4_text_graph_order_unavailable"); return out; }
        }
        out.identityValid = true;
        const auto mark = [&](const std::string& id, V4TextGraphObservationState state,
            std::optional<std::size_t> log = {}, std::optional<std::size_t> window = {}) {
            const auto found = evidenceById.find(id);
            if (found == evidenceById.end()) return false;
            auto& item = out.events[found->second]; item.state = state; item.logIndex = log; item.traceIndex = window; return true;
        };
        const auto before = [&](const std::string& a, const std::string& b, const char* basis) {
            const auto left = classById.find(a), right = classById.find(b);
            if (left == classById.end() || right == classById.end() || !rank.contains(left->second) || !rank.contains(right->second)) return;
            const bool agrees = rank.at(left->second) < rank.at(right->second);
            out.orders.push_back({a, b, basis, agrees});
            out.hypothesisObservationRecorded = true;
            if (!agrees) out.hypothesisNotMet = true;
        };
        const V2CapturedOleReturn* drop = nullptr;
        std::vector<std::pair<std::string, std::string>> observedCalls;
        for (const auto& item : receipt.CapturedReturns())
        {
            const auto& entry = item.entered; const auto& returned = item.returned;
            const auto expectedKind = item.observation.kind == Kind::enter ? snowdesktop::ole_adapter_observation::Kind::enter :
                item.observation.kind == Kind::over ? snowdesktop::ole_adapter_observation::Kind::over :
                item.observation.kind == Kind::drop ? snowdesktop::ole_adapter_observation::Kind::drop :
                snowdesktop::ole_adapter_observation::Kind::leave;
            if (!entry.session || !entry.call || entry.parent || entry.depth || entry.thread != GetCurrentThreadId() ||
                entry.kind != expectedKind || entry.adapter != reinterpret_cast<std::uintptr_t>(adapter) || returned.session != entry.session ||
                returned.call != entry.call || returned.thread != entry.thread || returned.adapter != entry.adapter ||
                returned.parent != entry.parent || returned.depth != entry.depth || returned.kind != entry.kind ||
                entry.phase != snowdesktop::ole_adapter_observation::Phase::entered ||
                returned.phase != snowdesktop::ole_adapter_observation::Phase::returned || returned.result != item.observation.result)
            { out.unresolved.emplace_back("v4_text_graph_ole_scope_unavailable"); return out; }
            out.oleEvidence.push_back(item);
            const auto base = "program_" + std::to_string(item.observation.programIndex);
            if (!mark(base + "_call_start", V4TextGraphObservationState::observed_local) ||
                !mark(base + "_call_return", V4TextGraphObservationState::observed_local))
            { out.unresolved.emplace_back("v4_text_graph_ole_requirement_unavailable"); return out; }
            observedCalls.emplace_back(base + "_call_start", base + "_call_return");
            if (item.observation.kind == Kind::drop)
            { if (drop) { out.unresolved.emplace_back("v4_text_graph_multiple_drops_unavailable"); return out; } drop = &item; }
        }
        for (std::size_t i = 0; i < observedCalls.size(); ++i)
        {
            before(observedCalls[i].first, observedCalls[i].second, "captured_same_ole_call");
            if (i) before(observedCalls[i - 1].second, observedCalls[i].first, "sequential_ole_dispatch");
        }
        const auto provider = InspectV4UnicodeProviderInterval(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control);
        const auto refresh = V4RefreshRequestObservation::Inspect(receipt, binding, owner, adapter, epoch, trace, control);
        if (provider.localProviderIntervalObserved && provider.providerRecords.size() == 2)
        {
            const auto caller = InspectV4UnicodeCaller(receipt, verified, identity, present, selected,
                binding, owner, adapter, epoch, trace, control);
            if (!caller.actualCallerReturned || !caller.uniqueProviderCallObserved || caller.observationUnavailable ||
                caller.records.size() != 2 || !caller.providerCall)
            { out.unresolved.emplace_back("v4_text_graph_provider_evidence_unavailable"); return out; }
            out.callerEvidence = caller.records;
            out.providerEvidence = provider.providerRecords;
            out.providerCall = caller.providerCall;
            const std::string first = "v4_first_unicode_getdata_enter", last = "v4_first_unicode_getdata_return";
            mark(first, V4TextGraphObservationState::observed_local, provider.providerRecords[0].index);
            mark(last, V4TextGraphObservationState::observed_local, provider.providerRecords[1].index);
            out.hypothesisObservationRecorded = true;
            // Positive observed occurrence disproves an absent first-GetData
            // hypothesis; the inverse cannot be inferred from empty logs.
            if (!out.events.at(evidenceById.at(first)).selectedPresent || !out.events.at(evidenceById.at(last)).selectedPresent)
                out.hypothesisNotMet = true;
            before(first, last, "same_thread_provider_and_caller_return");
            if (drop)
            {
                const auto base = "program_" + std::to_string(drop->observation.programIndex);
                before(base + "_call_start", first, "provider_inside_captured_drop_scope");
                before(last, base + "_call_return", "caller_return_inside_captured_drop_scope");
            }
        }
        if (refresh.RequestMarkerCaptured() && refresh.EnterRecord())
        {
            const std::string id = "v4_refresh_request_enter";
            // The observed same-scope request could originate from unrelated
            // non-adapter reentrancy, so it is not yet this graph occurrence.
            mark(id, V4TextGraphObservationState::unattributed_request_candidate, refresh.EnterRecord()->index);
        }
        if (receipt.CompletionTraceCaptured())
        {
            std::optional<std::size_t> window;
            bool ambiguous = false;
            for (std::size_t i = 0; i < receipt.CompletionTrace().size(); ++i)
            {
                const auto& event = receipt.CompletionTrace()[i];
                if (event.route != MessageRoute::control_window_proc || event.window != control ||
                    event.message != kBackgroundShellReadyMessage || !event.returned ||
                    event.enteredThread != GetCurrentThreadId() || event.returnedThread != event.enteredThread ||
                    event.returnedSequence <= event.enteredSequence) continue;
                if (window) { ambiguous = true; break; } window = i;
            }
            if (window && !ambiguous)
            {
                mark("v4_refresh_window_enter", V4TextGraphObservationState::unattributed_window_candidate, {}, window);
                mark("v4_refresh_window_return", V4TextGraphObservationState::unattributed_window_candidate, {}, window);
            }
        }
        out.unresolved.emplace_back("v4_text_graph_unobserved_absence_and_selected_refresh_identity_unverified");
        out.unresolved.emplace_back("v4_text_graph_cross_thread_order_reentrancy_completion_and_business_unverified");
        if (!provider.localProviderIntervalObserved) out.unresolved.emplace_back("v4_text_graph_provider_interval_unavailable");
        if (!refresh.RequestMarkerCaptured()) out.unresolved.emplace_back("v4_text_graph_refresh_request_unavailable");
    }
    catch (...) { out.observationUnavailable = true; }
    return out;
}

// A continuous source-log slice from immediately before the first planned
// dispatch through the host wait callback. This is deliberately not a
// production completion certificate: callback return, an idle snapshot and
// an empty log do not exclude later producers or non-adapter reentrancy.
class V3SessionPresenceObservation final
{
public:
    static V3SessionPresenceObservation Inspect(const V3OleRunReceipt& receipt,
        const V3FamilyValidatedContext& context, const FileSourceInitialObservation& initial,
        const ole_file_source_fixture::Source& source, const MessageTrace& trace, HWND control)
    {
        V3SessionPresenceObservation result;
        try
        {
            if (!receipt.Execution().productionStarted || !receipt.JoinedIdentity() ||
                !receipt.SameCandidate(context) || !initial.MatchesSource(source) ||
                !receipt.SameSource(source) || !receipt.SameHostEpoch(trace, control, source) ||
                !receipt.SourceSessionInterval() ||
                std::find(receipt.Execution().hostStagesReturned.begin(), receipt.Execution().hostStagesReturned.end(),
                    "observe_completion_or_rejection") == receipt.Execution().hostStagesReturned.end())
            { result.unresolved_.emplace_back("v3_session_presence_identity_or_wait_unavailable"); return result; }
            const auto& interval = *receipt.SourceSessionInterval();
            ole_file_source_fixture::FileDropCallState now;
            if (interval.before.failed || interval.after.failed || interval.before.depth || interval.after.depth ||
                interval.before.owner != GetCurrentThreadId() || interval.after.owner != interval.before.owner ||
                interval.after.count < interval.before.count ||
                interval.events.size() != interval.after.count - interval.before.count ||
                !source.ReadFileDropCallState(now) || now.failed || now.depth || now.owner != interval.after.owner ||
                now.count != interval.after.count)
            { result.unresolved_.emplace_back("v3_session_presence_log_or_tail_unavailable"); return result; }
            for (const auto& call : receipt.CapturedReturns())
                if (!call.fileDropBefore || !call.fileDropAfter ||
                    call.fileDropBefore->count < interval.before.count || call.fileDropAfter->count > interval.after.count)
                { result.unresolved_.emplace_back("v3_session_presence_dispatch_interval_unavailable"); return result; }
            std::size_t pairs = 0;
            for (std::size_t i = 0; i < interval.events.size(); ++i)
            {
                using Phase = ole_file_source_fixture::FileDropCallPhase;
                ole_file_source_fixture::FileDropCallEvent actual;
                const auto& saved = interval.events[i];
                if (!source.ReadFileDropCallEvent(interval.before.count + i, actual) ||
                    actual.call != saved.call || actual.parent != saved.parent || actual.depth != saved.depth ||
                    actual.thread != saved.thread || actual.phase != saved.phase || actual.result != saved.result ||
                    !actual.call || actual.parent || actual.depth || actual.thread != now.owner ||
                    actual.phase != Phase::entered || i + 1 >= interval.events.size())
                { result.unresolved_.emplace_back("v3_session_presence_enter_or_reentrancy_unavailable"); return result; }
                const auto& savedReturn = interval.events[++i];
                ole_file_source_fixture::FileDropCallEvent returned;
                if (!source.ReadFileDropCallEvent(interval.before.count + i, returned) ||
                    returned.call != actual.call || returned.parent || returned.depth || returned.thread != actual.thread ||
                    returned.phase != Phase::returned || returned.call != savedReturn.call ||
                    returned.parent != savedReturn.parent || returned.depth != savedReturn.depth ||
                    returned.thread != savedReturn.thread || returned.phase != savedReturn.phase ||
                    returned.result != savedReturn.result)
                { result.unresolved_.emplace_back("v3_session_presence_return_or_reentrancy_unavailable"); return result; }
                ++pairs;
            }
            result.expectations_ = receipt.PresenceExpectations();
            result.unresolved_.emplace_back("v3_session_presence_window_provenance_and_non_adapter_reentrancy_unverified");
            result.unresolved_.emplace_back("v3_session_presence_producer_quiescence_and_tail_unverified");
            result.unresolved_.emplace_back("v3_session_presence_source_abi_return_and_graph_order_unverified");
            result.observedReturnMarkers_ = pairs;
            result.intervalCaptured_ = true;
        }
        catch (...) { result.observedReturnMarkers_ = 0; result.intervalCaptured_ = false; result.resourceOrObservationUnavailable_ = true; }
        return result;
    }
    bool IntervalCaptured() const noexcept { return intervalCaptured_; }
    std::size_t ObservedReturnMarkers() const noexcept { return observedReturnMarkers_; }
    bool ResourceOrObservationUnavailable() const noexcept { return resourceOrObservationUnavailable_; }
    const std::vector<V3PresenceExpectation>& Expectations() const noexcept { return expectations_; }
    const std::vector<std::string>& Unresolved() const noexcept { return unresolved_; }
    // No hypothesis match/mismatch until independent same-session provenance,
    // absence-window completeness and graph ordering have been established.
private:
    bool intervalCaptured_ = false, resourceOrObservationUnavailable_ = false;
    std::size_t observedReturnMarkers_ = 0;
    std::vector<V3PresenceExpectation> expectations_;
    std::vector<std::string> unresolved_;
};

// Source events observed during synchronous adapter calls only. The graph's
// first-call presence remains unresolved until gaps and session attribution
// are independently covered; an empty interval never proves global absence.
class V3LocalPresenceObservation final
{
public:
    static V3LocalPresenceObservation Inspect(const V3OleRunReceipt& receipt,
        const V3FamilyValidatedContext& context, const FileSourceInitialObservation& initial,
        const ole_file_source_fixture::Source& source, const MessageTrace& trace, HWND control)
    {
        V3LocalPresenceObservation result;
        try
        {
            if (!receipt.Execution().productionStarted || !receipt.JoinedIdentity() ||
                !receipt.SameCandidate(context) || !initial.MatchesSource(source) ||
                !receipt.SameSource(source) || !receipt.SameHostEpoch(trace, control, source))
            { result.unresolved_.emplace_back("v3_presence_identity_or_epoch_unavailable"); return result; }
            result.expectations_ = receipt.PresenceExpectations();
            std::size_t priorEnd = 0;
            std::size_t localCompletedCalls = 0;
            bool haveInterval = false;
            for (const auto& call : receipt.CapturedReturns())
            {
                if (!call.fileDropBefore || !call.fileDropAfter || call.fileDropBefore->failed ||
                    call.fileDropAfter->failed || call.fileDropBefore->owner != GetCurrentThreadId() ||
                    call.fileDropAfter->owner != call.fileDropBefore->owner || call.fileDropBefore->depth ||
                    call.fileDropAfter->depth || call.fileDropAfter->count < call.fileDropBefore->count ||
                    call.fileDropEvents.size() != call.fileDropAfter->count - call.fileDropBefore->count ||
                    (haveInterval && call.fileDropBefore->count < priorEnd))
                { result.unresolved_.emplace_back("v3_presence_interval_unavailable"); return result; }
                for (std::size_t i = 0; i < call.fileDropEvents.size(); ++i)
                {
                    ole_file_source_fixture::FileDropCallEvent actual;
                    const auto& saved = call.fileDropEvents[i];
                    if (!source.ReadFileDropCallEvent(call.fileDropBefore->count + i, actual) ||
                        actual.call != saved.call || actual.parent != saved.parent || actual.depth != saved.depth ||
                        actual.thread != saved.thread || actual.phase != saved.phase || actual.result != saved.result ||
                        actual.thread != GetCurrentThreadId() || !actual.call || actual.parent || actual.depth)
                    { result.unresolved_.emplace_back("v3_presence_event_or_reentrancy_unavailable"); return result; }
                    using Phase = ole_file_source_fixture::FileDropCallPhase;
                    if (actual.phase != Phase::entered || i + 1 >= call.fileDropEvents.size())
                    { result.unresolved_.emplace_back("v3_presence_event_pair_unavailable"); return result; }
                    const auto& returned = call.fileDropEvents[++i];
                    ole_file_source_fixture::FileDropCallEvent checked;
                    if (!source.ReadFileDropCallEvent(call.fileDropBefore->count + i, checked) ||
                        checked.call != actual.call || checked.parent || checked.depth ||
                        checked.thread != actual.thread || checked.phase != Phase::returned ||
                        checked.call != returned.call || checked.parent != returned.parent ||
                        checked.depth != returned.depth || checked.thread != returned.thread ||
                        checked.phase != returned.phase || checked.result != returned.result)
                    { result.unresolved_.emplace_back("v3_presence_event_pair_unavailable"); return result; }
                    ++localCompletedCalls;
                }
                priorEnd = call.fileDropAfter->count;
                haveInterval = true;
            }
            ole_file_source_fixture::FileDropCallState current;
            if (!haveInterval || !source.ReadFileDropCallState(current) || current.failed || current.depth ||
                current.owner != GetCurrentThreadId() || current.count < priorEnd)
            { result.localCompletedCalls_ = 0; result.unresolved_.emplace_back("v3_presence_final_log_unavailable"); return result; }
            result.unresolved_.emplace_back("v3_presence_source_return_marker_precedes_actual_abi_return");
            result.unresolved_.emplace_back("v3_presence_session_gaps_first_call_and_non_adapter_reentrancy_unverified");
            result.unresolved_.emplace_back("v3_presence_graph_order_and_business_result_unverified");
            result.localCompletedCalls_ = localCompletedCalls;
        }
        catch (...) { result.localCompletedCalls_ = 0; result.resourceOrObservationUnavailable_ = true; }
        return result;
    }
    // The provider's return marker is inside its scope; it is not proof that
    // the ABI return was observed by the caller.
    std::size_t LocalReturnedScopes() const noexcept { return localCompletedCalls_; }
    bool ResourceOrObservationUnavailable() const noexcept { return resourceOrObservationUnavailable_; }
    const std::vector<V3PresenceExpectation>& Expectations() const noexcept { return expectations_; }
    const std::vector<std::string>& Unresolved() const noexcept { return unresolved_; }
private:
    std::size_t localCompletedCalls_ = 0;
    bool resourceOrObservationUnavailable_ = false;
    std::vector<V3PresenceExpectation> expectations_;
    std::vector<std::string> unresolved_;
};

// A partial correlation only. The trace hook itself, possible non-adapter
// reentrancy, global host quiescence and business success remain unproven.
class V3DropTaskWindowObservation
{
public:
    static V3DropTaskWindowObservation Inspect(const V3OleRunReceipt& receipt,
        const V3FamilyValidatedContext& context, const PayloadFixtureResult& fixture,
        const FileSourceInitialObservation& initial, ole_file_source_fixture::Source& source,
        const MessageTrace& trace, std::size_t traceStart, HWND window, MessageRoute route, UINT completionMessage)
    {
        V3DropTaskWindowObservation result;
        try
        {
            if (!receipt.Execution().productionStarted || !receipt.SameSource(source) || !receipt.SameCandidate(context) ||
                !receipt.SameHostEpoch(trace, window, source) || route != MessageRoute::control_window_proc ||
                !receipt.JoinedIdentity() || receipt.GraphId().empty() ||
                trace.LostOrCrossThread() || trace.OwnerThread() != GetCurrentThreadId() ||
                !window || !IsWindow(window) || GetWindowThreadProcessId(window, nullptr) != trace.OwnerThread() ||
                traceStart > trace.Observations().size())
            { result.unresolved_.emplace_back("v3_drop_task_receipt_or_trace_unavailable"); return result; }
            if (std::find(receipt.Obligations().begin(), receipt.Obligations().end(),
                "v3_receipt_graph_revalidation_unavailable") != receipt.Obligations().end())
            { result.unresolved_.emplace_back("v3_drop_task_receipt_graph_changed"); return result; }
            const V2CapturedOleReturn* drop = nullptr;
            for (const auto& event : receipt.CapturedReturns())
                if (event.observation.kind == Kind::drop)
                {
                    if (drop) { result.unresolved_.emplace_back("v3_drop_task_multiple_drop_calls"); return result; }
                    drop = &event;
                }
            if (!drop || drop->entered.kind != snowdesktop::ole_adapter_observation::Kind::drop ||
                drop->returned.kind != snowdesktop::ole_adapter_observation::Kind::drop ||
                drop->entered.phase != snowdesktop::ole_adapter_observation::Phase::entered ||
                drop->returned.phase != snowdesktop::ole_adapter_observation::Phase::returned ||
                !drop->entered.session || !drop->entered.call || drop->entered.parent || drop->entered.depth ||
                drop->entered.session != drop->returned.session || drop->entered.call != drop->returned.call ||
                drop->entered.thread != trace.OwnerThread() || drop->returned.thread != drop->entered.thread ||
                drop->entered.adapter != drop->returned.adapter || !receipt.SameAdapterIdentity(drop->entered.adapter) ||
                drop->returned.result != drop->observation.result)
            { result.unresolved_.emplace_back("v3_drop_task_adapter_call_unavailable"); return result; }
            const auto count = V3LocalResolvedCountObservation::Inspect(context, fixture, initial, source);
            result.unresolved_.insert(result.unresolved_.end(), count.Unresolved().begin(), count.Unresolved().end());
            if (!count.ObservedCount() || !count.Task() || !count.Work())
            { result.unresolved_.emplace_back("v3_drop_task_read_count_unavailable"); return result; }
            const auto passive = InspectPassiveTransfer(static_cast<IDataObject*>(&source));
            for (const auto& reason : passive.unresolved)
                if (reason == "passive_nested_scope_unresolved" || reason == "passive_end_operation_scope_ambiguous" ||
                    reason == "passive_finished_closure_or_context_ambiguous" ||
                    reason == "passive_finished_lifecycle_unavailable" ||
                    reason == "passive_finished_file_completion_context_unavailable" ||
                    reason == "passive_transfer_adapter_scope_unavailable")
                { result.unresolved_.emplace_back("v3_drop_task_scope_or_lifecycle_ambiguous"); return result; }
            const auto& origin = passive.adapterScope;
            if (passive.task != count.Task() || !origin.observed || origin.session != drop->entered.session ||
                origin.call != drop->entered.call || origin.parent != drop->entered.parent ||
                origin.adapter != drop->entered.adapter || origin.thread != drop->entered.thread ||
                origin.depth != drop->entered.depth ||
                origin.kind != static_cast<unsigned>(snowdesktop::ole_adapter_observation::Kind::drop))
            { result.unresolved_.emplace_back("v3_drop_task_adapter_origin_unavailable"); return result; }
            const PassiveWorkerEvidence* read = nullptr;
            const PassiveWorkerEvidence* file = nullptr;
            for (const auto& worker : passive.workers)
            {
                if (worker.kind == 1) { if (read) { result.unresolved_.emplace_back("v3_drop_task_multiple_reads"); return result; } read = &worker; }
                else if (worker.kind == 2) { if (file) { result.unresolved_.emplace_back("v3_drop_task_multiple_file_work"); return result; } file = &worker; }
                else { result.unresolved_.emplace_back("v3_drop_task_unknown_work_kind"); return result; }
            }
            if (!read || !file || read->work != count.Work() || !file->work || !file->completion ||
                read->messageThread != trace.OwnerThread() || file->messageThread != trace.OwnerThread() ||
                file->queueEntries != 1 || file->queueAccepted != 1 || file->queueFailed || file->dequeued != 1 ||
                file->entered != 1 || file->returned != 1 || file->finished != 1 || file->cancelled || !file->executeResult ||
                (*file->executeResult != 0 && *file->executeResult != 1) || !file->callbackResult ||
                *file->callbackResult != *file->executeResult ||
                file->callbacks != 1 || file->posts != 1 || file->postFailed || file->messageEntered != 1 ||
                file->messageReturned != 1 || passive.finishedEntered != 1 || passive.finishedReturned != 1 ||
                passive.finishedDuplicates || passive.finishedContextMismatch || passive.finishedWork != file->work ||
                !passive.finishedInput || !passive.finishedOutcome || *passive.finishedInput != *passive.finishedOutcome ||
                passive.endEntered != 1 || passive.sourceEnds != 1 || passive.endLocalTask != passive.task ||
                passive.endLocalWork != file->work || passive.endThread != trace.OwnerThread() ||
                !passive.endRequestedStatus || !passive.endReturnedStatus ||
                source.wrongApartment || source.starts != 1 || source.ends != 1 ||
                *passive.endRequestedStatus != static_cast<std::int64_t>(source.outcome) ||
                passive.endRequestedEffect != source.endEffect)
            { result.unresolved_.emplace_back("v3_drop_task_commit_or_source_end_unavailable"); return result; }
            const auto findMessage = [&](const PassiveWorkerEvidence& work, bool fileOperation) {
                std::size_t found = MessageTrace::InvalidToken;
                const auto& messages = trace.Observations();
                for (std::size_t i = traceStart; i < messages.size(); ++i)
                {
                    const auto& item = messages[i];
                    if (item.message != completionMessage || !item.completion ||
                        item.completion->task != passive.task || item.completion->work != work.work) continue;
                    if (found != MessageTrace::InvalidToken || item.completion->identity != work.completion ||
                        item.completion->fileOperation != fileOperation || item.window != window || item.route != route ||
                        !work.callbackResult || item.completion->succeeded != (*work.callbackResult != 0) ||
                        item.enteredThread != trace.OwnerThread() || item.returnedThread != trace.OwnerThread() ||
                        !item.returned || item.returnedSequence <= item.enteredSequence)
                        return MessageTrace::InvalidToken;
                    found = i;
                }
                return found;
            };
            const auto readTrace = findMessage(*read, false);
            const auto fileTrace = findMessage(*file, true);
            if (readTrace == MessageTrace::InvalidToken || fileTrace == MessageTrace::InvalidToken)
            { result.unresolved_.emplace_back("v3_drop_task_window_completion_unavailable"); return result; }
            result.unresolved_.emplace_back("v3_drop_task_trace_fixture_provenance_unverified");
            result.unresolved_.emplace_back("v3_drop_task_non_adapter_reentrancy_and_host_epoch_unverified");
            result.unresolved_.emplace_back("v3_drop_task_worker_quiescence_and_business_result_unverified");
            result.task_ = passive.task; result.readWork_ = read->work; result.fileWork_ = file->work;
            result.readTrace_ = readTrace; result.fileTrace_ = fileTrace;
        }
        catch (const std::bad_alloc&) { result.resourceUnavailable_ = true; }
        catch (const std::exception& error) { result.unresolved_.push_back(std::string("v3_drop_task_observation_unavailable:") + error.what()); }
        catch (...) { result.unresolved_.emplace_back("v3_drop_task_observation_unavailable"); }
        return result;
    }
    std::uint64_t Task() const noexcept { return task_; }
    std::uint64_t ReadWork() const noexcept { return readWork_; }
    std::uint64_t FileWork() const noexcept { return fileWork_; }
    std::size_t ReadTraceIndex() const noexcept { return readTrace_; }
    std::size_t FileTraceIndex() const noexcept { return fileTrace_; }
    bool ResourceUnavailable() const noexcept { return resourceUnavailable_; }
    const std::vector<std::string>& Unresolved() const noexcept { return unresolved_; }
private:
    V3DropTaskWindowObservation() = default;
    std::uint64_t task_ = 0, readWork_ = 0, fileWork_ = 0;
    std::size_t readTrace_ = MessageTrace::InvalidToken, fileTrace_ = MessageTrace::InvalidToken;
    bool resourceUnavailable_ = false;
    std::vector<std::string> unresolved_;
};

// A single observed marshal/unmarshal and enclosed provider/caller interval.
// This is local diagnostic evidence only: unrelated uninstrumented reentry,
// task attribution, complete graph presence and business results remain open.
class V3FileDropAbiObservation final
{
public:
    static V3FileDropAbiObservation Inspect(const V3OleRunReceipt& receipt,
        const V3FamilyValidatedContext& context, const PayloadFixtureResult& fixture,
        const FileSourceInitialObservation& initial, ole_file_source_fixture::Source& source,
        const MessageTrace& trace, std::size_t traceStart, HWND control, UINT completionMessage)
    {
        using namespace snowdesktop::external_transfer_observation;
        V3FileDropAbiObservation result;
        try
        {
            const auto taskWindow = V3DropTaskWindowObservation::Inspect(receipt, context, fixture,
                initial, source, trace, traceStart, control, MessageRoute::control_window_proc, completionMessage);
            const auto session = V3SessionPresenceObservation::Inspect(receipt, context, initial, source, trace, control);
            if (taskWindow.ResourceUnavailable() || !taskWindow.Task() || !taskWindow.ReadWork() ||
                session.ResourceOrObservationUnavailable() || !session.IntervalCaptured() ||
                session.ObservedReturnMarkers() != 1 || !Complete() || !receipt.SourceSessionInterval())
            { result.unresolved_.emplace_back("v3_filedrop_abi_context_or_log_unavailable"); return result; }
            ReadSession diagnosticSession;
            if (!diagnosticSession.Open()) { result.unresolved_.emplace_back("v3_filedrop_abi_session_unavailable"); return result; }
            struct Mark { Event event{}; std::size_t index = 0; bool seen = false; };
            std::array<Mark, 8> marks{};
            const std::array<Phase, 8> phases{Phase::source_marshal_enter, Phase::source_marshal_return,
                Phase::source_unmarshal_enter, Phase::source_unmarshal_return,
                Phase::file_paths_getdata_enter, Phase::fixture_filedrop_getdata_enter,
                Phase::fixture_filedrop_getdata_return, Phase::file_paths_getdata_return};
            for (std::size_t i = 0; i < diagnosticSession.Count(); ++i)
            {
                Event event;
                if (!diagnosticSession.ReadEvent(i, event)) { result.unresolved_.emplace_back("v3_filedrop_abi_record_unavailable"); return result; }
                if (event.phase == Phase::fixture_filedrop_getdata_unwound ||
                    (event.phase == Phase::ambiguous_nested_scope && event.task == taskWindow.Task()))
                { result.unresolved_.emplace_back("v3_filedrop_abi_unwound_or_nested"); return result; }
                const auto found = std::find(phases.begin(), phases.end(), event.phase);
                if (found == phases.end()) continue;
                auto& mark = marks[static_cast<std::size_t>(found - phases.begin())];
                if (mark.seen) { result.unresolved_.emplace_back("v3_filedrop_abi_multiple_calls_unavailable"); return result; }
                mark = {event, i, true};
            }
            for (std::size_t i = 0; i < marks.size(); ++i)
                if (!marks[i].seen || (i && marks[i - 1].index >= marks[i].index))
                { result.unresolved_.emplace_back("v3_filedrop_abi_order_unavailable"); return result; }
            const auto& marshal = marks[0].event; const auto& marshaled = marks[1].event;
            const auto& unmarshal = marks[2].event; const auto& unmarshaled = marks[3].event;
            const auto& caller = marks[4].event; const auto& provider = marks[5].event;
            const auto& provided = marks[6].event; const auto& returned = marks[7].event;
            const auto task = taskWindow.Task(), work = taskWindow.ReadWork();
            const auto original = reinterpret_cast<std::uintptr_t>(static_cast<IDataObject*>(&source));
            const auto concrete = reinterpret_cast<std::uintptr_t>(&source);
            if (!marshal.object || marshal.object != marshaled.object || marshal.object != unmarshal.object ||
                marshal.object != unmarshaled.object || marshal.task != task || marshaled.task != task ||
                unmarshal.task != task || unmarshaled.task != task || marshal.extra != original ||
                marshal.thread != source.owner || marshaled.thread != marshal.thread ||
                marshal.localTask != task || marshaled.localTask != task ||
                !marshaled.extra || marshaled.extra != unmarshal.extra || !unmarshaled.extra ||
                FAILED(static_cast<HRESULT>(marshaled.value)) || FAILED(static_cast<HRESULT>(unmarshaled.value)) ||
                unmarshal.localTask != task || unmarshaled.localTask != task || unmarshal.localWork != work ||
                unmarshaled.localWork != work || !unmarshal.thread || unmarshal.thread != unmarshaled.thread ||
                caller.task != task || returned.task != task || caller.localTask != task || returned.localTask != task ||
                caller.localWork != work || returned.localWork != work || caller.thread != unmarshal.thread ||
                returned.thread != caller.thread || !caller.extra || caller.extra != returned.extra ||
                caller.object != unmarshaled.extra || returned.object != caller.object ||
                provider.task || provided.task || provider.object != concrete || provided.object != concrete ||
                !provider.extra || provider.extra != provided.extra || provider.thread != source.owner ||
                provided.thread != provider.thread || provider.localTask || provider.localWork ||
                provided.localTask || provided.localWork || provided.value != returned.value)
            { result.unresolved_.emplace_back("v3_filedrop_abi_identity_or_result_unavailable"); return result; }
            const auto& interval = *receipt.SourceSessionInterval();
            if (interval.events.size() != 2 || interval.events[0].call != provider.extra ||
                interval.events[1].call != provided.extra || interval.events[1].result != static_cast<HRESULT>(provided.value))
            { result.unresolved_.emplace_back("v3_filedrop_abi_source_interval_unavailable"); return result; }
            result.unresolved_.emplace_back("v3_filedrop_abi_uninstrumented_reentrancy_and_session_attribution_unverified");
            result.unresolved_.emplace_back("v3_filedrop_abi_graph_presence_and_business_result_unverified");
            result.callerResult_ = returned.value;
            result.providerResult_ = provided.value;
            result.uniqueEnvelope_ = true;
        }
        catch (...) { result.uniqueEnvelope_ = false; result.callerResult_.reset(); result.providerResult_.reset(); result.resourceOrObservationUnavailable_ = true; }
        return result;
    }
    bool UniqueEnvelopeObserved() const noexcept { return uniqueEnvelope_; }
    std::optional<std::int64_t> CallerResult() const noexcept { return callerResult_; }
    std::optional<std::int64_t> ProviderResult() const noexcept { return providerResult_; }
    bool ResourceOrObservationUnavailable() const noexcept { return resourceOrObservationUnavailable_; }
    const std::vector<std::string>& Unresolved() const noexcept { return unresolved_; }
private:
    bool uniqueEnvelope_ = false, resourceOrObservationUnavailable_ = false;
    std::optional<std::int64_t> callerResult_, providerResult_;
    std::vector<std::string> unresolved_;
};

// Execute the currently supported SINGLE session event graph. The complete
// selected variant is regenerated from the validated candidate; an arbitrary
// caller-provided graph or callback cannot bypass per-axis obligations.
// The source and adapter stay alive for every real COM call. This method
// never reports PASS: callbacks and observations remain evidence for the
// caller's separately implemented candidate-level oracle.
class GraphExecutor final
{
public:
    static Result Run(const event_graph::Context& context, const Candidate& candidate,
        const EventProgram& program, const event_graph::Variant& selected,
        const std::vector<Input>& oleInputs, OleDragDropAdapter* adapter,
        IDataObject* source, const HostBoundaries& host)
    {
        Result result;
        const auto mapping = event_graph::MapSimpleExternal(context, candidate, program);
        if (!mapping.structuralExclusion.empty())
            result.obligations.push_back("candidate_structure_excluded:" + mapping.structuralExclusion);
        if (!mapping.graphAvailable)
        {
            result.obligations.insert(result.obligations.end(), mapping.reasons.begin(), mapping.reasons.end());
            result.obligations.emplace_back("candidate_event_graph_unimplemented");
            return result;
        }

        if (!RequireLegacyGraphPresence(mapping.spec, result.obligations, "ole_graph_")) return result;
        if (!RequireLegacyGraphVersion(selected, result.obligations, "ole_graph_")) return result;

        const auto replay = event_graph::ReplayVariant(context, candidate, mapping.spec, selected.id);
        result.obligations.insert(result.obligations.end(), replay.errors.begin(), replay.errors.end());
        if (!replay.structuralExclusion.empty())
            result.obligations.push_back("candidate_structure_excluded:" + replay.structuralExclusion);
        if (!replay.identityValid) result.obligations.emplace_back("selected_graph_validation_incomplete");
        else
        {
            const auto& expected = replay.variant;
            // Ordinal is enumeration metadata, not part of stable graph identity;
            // ReplayVariant intentionally leaves it empty.
            if (selected.id != expected.id || selected.classes != expected.classes || selected.order != expected.order ||
                selected.unresolved != expected.unresolved)
                result.obligations.emplace_back("selected_graph_candidate_mismatch");
            for (const auto& reason : expected.unresolved)
                result.obligations.push_back("selected_graph_unresolved:" + reason);
        }

        // Validate the graph's exact production-call/return and host-stage
        // sequence. The present simple mapper requires one distinct class per
        // requirement and a strict linear chain. Interleaving inside a call,
        // a second session or any extra graph event remains unimplemented.
        std::vector<std::string> expectedOrder;
        bool sawTerminal = false;
        bool sawCompletionStage = false;
        bool sawBusinessStage = false;
        for (std::size_t i = 0; i < program.steps.size(); ++i)
        {
            const auto& step = program.steps[i];
            if (const auto kind = ProgramKind(step.kind))
            {
                expectedOrder.push_back("program_" + std::to_string(i) + "_call_start");
                expectedOrder.push_back("program_" + std::to_string(i) + "_call_return");
                if (*kind == Kind::drop || *kind == Kind::leave) sawTerminal = true;
            }
            else if (step.kind == "observe_completion_or_rejection" || step.kind == "assert_business_results")
            {
                expectedOrder.push_back("program_" + std::to_string(i) + "_observe");
                if (step.kind == "observe_completion_or_rejection")
                {
                    if (!sawTerminal || sawCompletionStage)
                        result.obligations.emplace_back("completion_stage_without_unique_prior_termination");
                    sawCompletionStage = true;
                }
                else
                {
                    if (!sawCompletionStage || sawBusinessStage)
                        result.obligations.emplace_back("business_stage_without_unique_prior_completion_stage");
                    sawBusinessStage = true;
                }
            }
            else result.obligations.push_back("graph_step_executor_unimplemented:" + step.kind);
        }
        if (!sawTerminal || !sawCompletionStage || !sawBusinessStage)
            result.obligations.emplace_back("terminal_or_host_stage_missing");
        if (selected.order != expectedOrder) result.obligations.emplace_back("graph_execution_order_unimplemented");

        // Reuse the same strict complete-candidate and model-value validation.
        // Its one Input per program position is expanded here only for OLE
        // positions; non-OLE slots are never dispatched or interpreted.
        std::vector<Input> expanded(program.steps.size());
        std::vector<bool> supplied(program.steps.size(), false);
        for (const auto& input : oleInputs)
        {
            if (input.programIndex >= program.steps.size() || supplied[input.programIndex] ||
                !ProgramKind(program.steps[input.programIndex].kind))
            { result.obligations.emplace_back("ole_input_index_invalid_or_duplicate"); continue; }
            expanded[input.programIndex] = input;
            supplied[input.programIndex] = true;
        }
        for (std::size_t i = 0; i < program.steps.size(); ++i)
            if (ProgramKind(program.steps[i].kind) && !supplied[i])
                result.obligations.push_back("ole_input_missing:" + std::to_string(i));
        auto inspection = Inspect(context.axes, candidate, program, expanded);
        for (auto& obligation : inspection.obligations)
        {
            // These two exact stage handlers are implemented below; no other
            // unresolved/program/axis obligation may be removed by a callback.
            if (obligation == "program_step_executor_unimplemented:observe_completion_or_rejection" ||
                obligation == "program_step_executor_unimplemented:assert_business_results") continue;
            result.obligations.push_back(std::move(obligation));
        }
        if (!adapter) result.obligations.emplace_back("production_ole_adapter_missing");
        if (!source) result.obligations.emplace_back("ole_source_missing");
        if (!host.beforeOleCall) result.obligations.emplace_back("pre_ole_observation_missing");
        if (!host.observeOleReturn) result.obligations.emplace_back("immediate_ole_observation_missing");
        if (!host.awaitProductionCompletionOrRejection) result.obligations.emplace_back("production_completion_wait_missing");
        if (!host.assertIndependentBusinessResults) result.obligations.emplace_back("independent_business_oracle_missing");
        if (!result.obligations.empty()) return result;

        Microsoft::WRL::ComPtr<OleDragDropAdapter> heldAdapter(adapter);
        Microsoft::WRL::ComPtr<IDataObject> heldSource(source);
        // Allocate recording capacity before any production call so a normal
        // post-dispatch vector growth cannot hide whether entry happened.
        result.observations.reserve(oleInputs.size());
        result.hostStagesReturned.reserve(program.steps.size());
        result.failure.reserve(512);
        result.status = Status::attempted;
        try
        {
        for (std::size_t i = 0; i < program.steps.size(); ++i)
        {
            const auto& step = program.steps[i];
            if (ProgramKind(step.kind))
            {
                try { host.beforeOleCall(expanded[i]); }
                catch (const ObservationUnavailable& error)
                { result.status = Status::observation_unavailable; result.failure = error.what(); return result; }
                catch (const std::exception& error)
                { result.status = Status::host_observation_failed; result.failure = error.what(); return result; }
                catch (...)
                { result.status = Status::host_observation_failed; result.failure = "unknown_pre_ole_observation_exception"; return result; }
                result.productionStarted = true;
                Observation observed;
                try { observed = Sequence::Dispatch(*heldAdapter.Get(), heldSource.Get(), expanded[i]); }
                catch (const std::exception& error)
                { result.status = Status::event_failed; result.failure = error.what(); return result; }
                catch (...)
                { result.status = Status::event_failed; result.failure = "unknown_ole_dispatch_exception"; return result; }
                const bool failed = FAILED(observed.result);
                result.observations.push_back(std::move(observed));
                try { host.observeOleReturn(result.observations.back()); }
                catch (const ObservationUnavailable& error)
                {
                    result.status = failed ? Status::event_failed : Status::observation_unavailable;
                    result.failure = failed ? std::string("ole_hresult_failed_and_observation_unavailable:") + error.what() : error.what();
                    return result;
                }
                catch (const std::exception& error)
                { result.status = failed ? Status::event_failed : Status::host_observation_failed; result.failure = error.what(); return result; }
                catch (...)
                { result.status = failed ? Status::event_failed : Status::host_observation_failed; result.failure = "unknown_ole_observation_exception"; return result; }
                if (failed) { result.status = Status::event_failed; return result; }
            }
            else
            {
                try
                {
                    if (step.kind == "observe_completion_or_rejection")
                    {
                        if (result.observations.empty() ||
                            (result.observations.back().kind != Kind::drop && result.observations.back().kind != Kind::leave))
                        { result.status = Status::unimplemented; return result; }
                        host.awaitProductionCompletionOrRejection(result.observations.back());
                    }
                    else if (step.kind == "assert_business_results") host.assertIndependentBusinessResults();
                    else { result.status = Status::unimplemented; return result; }
                    result.hostStagesReturned.push_back(step.kind);
                }
                catch (const ObservationUnavailable& error)
                { result.status = Status::observation_unavailable; result.failure = error.what(); return result; }
                catch (const std::exception& error)
                { result.status = Status::host_observation_failed; result.failure = error.what(); return result; }
                catch (...)
                { result.status = Status::host_observation_failed; result.failure = "unknown_host_stage_exception"; return result; }
            }
        }
        }
        catch (const std::exception& error)
        {
            result.status = Status::event_failed;
            try
            {
                result.failure.assign("ole_executor_exception:");
                const std::string_view detail(error.what());
                result.failure.append(detail.substr(0, 400));
            }
            catch (...) { result.failure.assign("ole_executor_exception_detail_unavailable"); }
        }
        catch (...)
        { result.status = Status::event_failed; result.failure = "unknown_ole_executor_exception"; }
        return result;
    }
};

// Native pointer delivery uses the production private input device, which
// queues events and sends them through the owned HWND's actual window proc.
// The fixture must route those messages to DesktopApp::WndProc, not a STATIC
// window proc, and must independently observe the controller and model.
struct NativePointInput
{
    std::size_t programIndex = 0;
    POINT screen{};
};
enum class NativeDevicePhase { initial_hover, program_input, session_end };
// The requested primary input, constructed from the driver action and its
// verified screen point and pre-dispatch HWND mapping. It is independent of
// the device state and delivery log. The observer must verify that the window
// mapping remains unchanged across dispatch before comparing the message.
struct NativeInputExpectation
{
    UINT message;
    POINT screen;
    POINT clientBeforeDispatch;
    WPARAM buttons;
};
struct NativeHostBoundaries
{
    // Read-only check of the same host's input/placement baseline before the
    // device is constructed or any event is queued.
    std::function<void()> beforeDeviceBegin;
    // Both references belong to the same Device, are valid only during this
    // call and must not be retained. Initial hover has no program index.
    // The value token identifies the actual queued event before the message
    // pump; End is not a queued input and carries no token. It does not by
    // itself prove the host consumed the event or establish a graph role.
    std::function<void(NativeDevicePhase, std::optional<std::size_t>, const snowdesktop::private_pointer::State&,
        const snowdesktop::private_pointer::DeliveryObservationLog&,
        std::optional<snowdesktop::private_pointer::QueuedEventIdentity>,
        std::optional<NativeInputExpectation>)> observeDeviceState;
    std::function<void(const EventStep&)> observeCapture;
    std::function<void(const EventStep&)> awaitCompletion;
    // State and log are borrowed from the same actual Device immediately
    // before End; references expire when this callback returns.
    std::function<void(const EventStep&, const snowdesktop::private_pointer::State&,
        const snowdesktop::private_pointer::DeliveryObservationLog&)> beforePointerSessionEnd;
    // Inspect the real cancellation/leave/capture/focus messages from End.
    std::function<void(const EventStep&)> observePointerSessionEnd;
    std::function<void(const EventStep&)> assertBusinessResults;
    std::function<void()> throwAsync;
    // Reads the same host's loss-checked production WndProc input trace from
    // the session baseline; must throw if its completeness is not established.
    std::function<bool()> productionInputEntered;
    // Called with the same Device's actual enqueue identity, before pumping
    // the event. It is an input identity only, not proof of delivery/permit.
    std::function<void(NativeDevicePhase, std::optional<std::size_t>,
        const snowdesktop::private_pointer::QueuedEventIdentity&)> beforeQueuedDelivery;
};

enum class NativeGraphActionKind { down, move, capture, up, completion, end, end_probe, business };
enum class NativeDeviceCallKind { begin, down, move, up, end };
struct NativeDeviceCallRecord
{
    NativeDeviceCallKind kind;
    std::optional<std::size_t> programIndex;
    bool returned = false;
    std::optional<bool> succeeded;
    DWORD thread = 0;
    std::size_t traceSize = 0;
};
// Optional, preallocated fixture-side journal. No callback or production
// decision depends on these values. A missing return remains unknown.
struct NativeDeviceCallJournal
{
    const MessageTrace* trace = nullptr;
    std::vector<NativeDeviceCallRecord>* records = nullptr;
    bool lost = false;
    void Record(NativeDeviceCallKind kind, std::optional<std::size_t> index,
        bool returned, std::optional<bool> succeeded = std::nullopt) noexcept
    {
        const DWORD savedError = GetLastError();
        if (!trace || !records || records->size() == records->capacity())
        { lost = true; SetLastError(savedError); return; }
        records->push_back({kind, index, returned, succeeded, GetCurrentThreadId(), trace->Observations().size()});
        SetLastError(savedError);
    }
};
struct NativeGraphAction
{
    NativeGraphActionKind kind;
    std::vector<std::size_t> programIndices;
};

struct NativeMoveClasses
{
    // Identity validity is not execution, observed event identity, or a PASS.
    bool identityValid = false;
    std::vector<std::optional<std::size_t>> byProgramIndex;
    std::vector<std::string> unresolved;
    std::vector<std::string> errors;
};

// Reconstruct the graph from the same candidate and replay its stable ID.
// Returned class numbers refer only to this specific replayed variant; a
// device sequence still needs independent production message observation.
inline NativeMoveClasses InspectNativeMoveClasses(const event_graph::Context& context,
    const Candidate& candidate, const EventProgram& program, const event_graph::Variant& selected)
{
    NativeMoveClasses result;
    const auto mapping = event_graph::MapSimpleNative(context, candidate, program);
    if (!mapping.structuralExclusion.empty())
        result.errors.push_back("structural_exclusion:" + mapping.structuralExclusion);
    if (!mapping.graphAvailable)
    {
        result.errors.insert(result.errors.end(), mapping.reasons.begin(), mapping.reasons.end());
        result.errors.emplace_back("native_graph_unavailable");
        return result;
    }
    if (!RequireLegacyGraphPresence(mapping.spec, result.errors, "native_graph_")) return result;
    if (!RequireLegacyGraphVersion(selected, result.errors, "native_graph_")) return result;
    const auto replay = event_graph::ReplayVariant(context, candidate, mapping.spec, selected.id);
    result.errors.insert(result.errors.end(), replay.errors.begin(), replay.errors.end());
    if (!replay.structuralExclusion.empty()) result.errors.push_back("structural_exclusion:" + replay.structuralExclusion);
    if (!replay.identityValid)
    { result.errors.emplace_back("native_graph_identity_invalid"); return result; }
    const auto& trusted = replay.variant;
    if (selected.id != trusted.id || selected.classes != trusted.classes ||
        selected.order != trusted.order || selected.unresolved != trusted.unresolved)
    { result.errors.emplace_back("native_graph_contents_mismatch"); return result; }

    auto requirements = mapping.spec.requirements;
    std::sort(requirements.begin(), requirements.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    if (requirements.size() != trusted.classes.size())
    { result.errors.emplace_back("native_graph_partition_size_invalid"); return result; }
    std::map<std::string, std::size_t> expectedMoves;
    for (std::size_t i = 0; i < program.steps.size(); ++i)
    {
        const auto& kind = program.steps[i].kind;
        if (kind == "move_past_drag_threshold" || kind == "pointer_direction" || kind == "hover" ||
            kind == "pointer_direction_later" || kind == "hover_later")
            expectedMoves.emplace("program_" + std::to_string(i) + "_move_requirement", i);
    }
    result.byProgramIndex.resize(program.steps.size());
    for (std::size_t i = 0; i < requirements.size(); ++i)
    {
        const auto& requirement = requirements[i];
        const auto found = expectedMoves.find(requirement.id);
        if (found == expectedMoves.end())
        {
            if (requirement.kind == "native_move_delivery_requirement")
                result.errors.push_back("unexpected_native_move_requirement:" + requirement.id);
            continue;
        }
        if (requirement.kind != "native_move_delivery_requirement" || result.byProgramIndex[found->second])
            result.errors.push_back("invalid_native_move_requirement:" + requirement.id);
        else result.byProgramIndex[found->second] = trusted.classes[i];
    }
    for (const auto& [id, index] : expectedMoves)
        if (!result.byProgramIndex[index]) result.errors.push_back("native_move_requirement_missing:" + id);
    result.unresolved = trusted.unresolved;
    result.identityValid = result.errors.empty();
    return result;
}

struct NativeSingleMoveGeometry
{
    bool bound = false;
    std::vector<NativePointInput> points;
    // Geometry construction does not close these model/graph obligations.
    std::vector<std::string> graphUnresolved;
    std::vector<std::string> unresolved;
};

// Prepare only the subset in which the threshold and first-direction
// requirements replay as ONE movement class, and every other movement
// requirement joins that same delivery. The only delivered displacement is
// therefore from start to target, so its direction has an unambiguous base.
// No product drag or landing decision is made by this geometry helper.
inline NativeSingleMoveGeometry InspectNativeSingleMoveGeometry(const event_graph::Context& context,
    const Candidate& candidate, const EventProgram& program, const event_graph::Variant& selected,
    HWND window, RECT screenBounds, POINT start, POINT target)
{
    NativeSingleMoveGeometry result;
    const auto classes = InspectNativeMoveClasses(context, candidate, program, selected);
    result.graphUnresolved = classes.unresolved;
    if (!classes.identityValid)
    {
        for (const auto& error : classes.errors) result.unresolved.push_back("native_geometry_graph:" + error);
        return result;
    }
    if (!window || !IsWindow(window) || IsRectEmpty(&screenBounds) ||
        !PtInRect(&screenBounds, start) || !PtInRect(&screenBounds, target))
    { result.unresolved.emplace_back("native_geometry_window_or_bounds_unimplemented"); return result; }
    POINT startClient = start, targetClient = target;
    if (!ScreenToClient(window, &startClient) || !ScreenToClient(window, &targetClient))
    { result.unresolved.emplace_back("native_geometry_screen_to_client_failed"); return result; }
    POINT startRoundTrip = startClient, targetRoundTrip = targetClient;
    if (!ClientToScreen(window, &startRoundTrip) || !ClientToScreen(window, &targetRoundTrip) ||
        startRoundTrip.x != start.x || startRoundTrip.y != start.y ||
        targetRoundTrip.x != target.x || targetRoundTrip.y != target.y)
    { result.unresolved.emplace_back("native_geometry_round_trip_unimplemented"); return result; }
    const auto packable = [](LONG coordinate) { return coordinate >= -32768 && coordinate <= 32767; };
    if (!packable(startClient.x) || !packable(startClient.y) || !packable(targetClient.x) || !packable(targetClient.y))
    { result.unresolved.emplace_back("native_geometry_message_coordinate_range_unimplemented"); return result; }
    const auto dx = static_cast<std::int64_t>(target.x) - start.x;
    const auto dy = static_cast<std::int64_t>(target.y) - start.y;
    if (static_cast<std::int64_t>(targetClient.x) - startClient.x != dx ||
        static_cast<std::int64_t>(targetClient.y) - startClient.y != dy)
    { result.unresolved.emplace_back("native_geometry_coordinate_delta_unimplemented"); return result; }
    // A conservative input margin, not an assertion of the production drag
    // decision. The real host must still prove it captured the source.
    if (dx >= -8 && dx <= 8 && dy >= -8 && dy <= 8)
    { result.unresolved.emplace_back("native_geometry_threshold_margin_unimplemented"); return result; }
    Candidate canonical = candidate;
    canonical.lookup = context.lookup;
    const auto direction = Value(context.axes, canonical, "H08.first_direction");
    const bool matches =
        (direction == "horizontal_positive" && dx > 0 && dy == 0) ||
        (direction == "horizontal_negative" && dx < 0 && dy == 0) ||
        (direction == "vertical_positive" && dx == 0 && dy > 0) ||
        (direction == "vertical_negative" && dx == 0 && dy < 0) ||
        (direction == "diagonal_equal_pp" && dx > 0 && dy > 0 && dx == dy) ||
        (direction == "diagonal_equal_pn" && dx > 0 && dy < 0 && dx == -dy) ||
        (direction == "diagonal_equal_np" && dx < 0 && dy > 0 && -dx == dy) ||
        (direction == "diagonal_equal_nn" && dx < 0 && dy < 0 && dx == dy);
    if (!matches)
    { result.unresolved.emplace_back("native_geometry_first_direction_unimplemented"); return result; }
    std::optional<std::size_t> moveClass;
    std::size_t thresholdCount = 0, directionCount = 0, pressCount = 0, releaseCount = 0;
    for (std::size_t i = 0; i < program.steps.size(); ++i)
    {
        const auto& kind = program.steps[i].kind;
        if (kind == "source_start") { ++pressCount; result.points.push_back({i, start}); }
        else if (kind == "release") { ++releaseCount; result.points.push_back({i, target}); }
        else if (classes.byProgramIndex[i])
        {
            if (moveClass && *moveClass != *classes.byProgramIndex[i])
                result.unresolved.push_back("native_geometry_distinct_move_class_unimplemented:" + std::to_string(i));
            else moveClass = classes.byProgramIndex[i];
            if (kind == "move_past_drag_threshold") ++thresholdCount;
            if (kind == "pointer_direction") ++directionCount;
            if (kind == "pointer_direction_later")
                result.unresolved.push_back("native_geometry_later_direction_unimplemented:" + std::to_string(i));
            result.points.push_back({i, target});
        }
    }
    if (!moveClass || thresholdCount != 1 || directionCount != 1 || pressCount != 1 || releaseCount != 1)
        result.unresolved.emplace_back("native_geometry_single_shared_move_shape_unimplemented");
    if (!result.unresolved.empty()) { result.points.clear(); return result; }
    result.bound = true;
    return result;
}

// Compile a replayed partition/order, without dispatching anything. A group of
// semantic move requirements denotes one delivery only when replay put them
// in the same class. Coordinates and the graph's message intervals still need
// independent production observations before this plan may be executed.
inline std::vector<NativeGraphAction> PlanNativeGraph(const EventProgram& program,
    const event_graph::Spec& spec, const event_graph::Variant& graph,
    const std::vector<std::optional<POINT>>& points, std::vector<std::string>& obligations)
{
    std::vector<NativeGraphAction> actions;
    if (!RequireLegacyGraphPresence(spec, obligations, "native_graph_")) return actions;
    if (!RequireLegacyGraphVersion(graph, obligations, "native_graph_")) return actions;
    auto requirements = spec.requirements;
    std::sort(requirements.begin(), requirements.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    if (requirements.size() != graph.classes.size() || points.size() != program.steps.size())
    { obligations.emplace_back("native_graph_plan_shape_invalid"); return actions; }
    std::map<std::size_t, std::vector<std::string>> members;
    std::map<std::string, std::size_t> classById;
    for (std::size_t i = 0; i < requirements.size(); ++i)
    {
        members[graph.classes[i]].push_back(requirements[i].id);
        classById.emplace(requirements[i].id, graph.classes[i]);
    }
    std::map<std::string, std::size_t> rank;
    for (std::size_t i = 0; i < graph.order.size(); ++i) rank.emplace(graph.order[i], i);
    std::map<std::size_t, std::size_t> classRank;
    for (const auto& [key, ids] : members)
    {
        const auto found = rank.find(ids.front());
        if (found == rank.end()) { obligations.push_back("native_graph_class_representative_missing:" + ids.front()); continue; }
        classRank[key] = found->second;
    }
    const auto position = [&](const std::string& id) -> std::optional<std::size_t> {
        const auto found = classById.find(id);
        if (found == classById.end()) return std::nullopt;
        const auto located = classRank.find(found->second);
        return located == classRank.end() ? std::nullopt : std::optional<std::size_t>(located->second);
    };
    const auto requireAdjacent = [&](const std::vector<std::string>& ids) {
        for (std::size_t i = 0; i < ids.size(); ++i)
        {
            const auto current = position(ids[i]);
            const auto first = position(ids.front());
            if (!current || !first || *current != *first + i)
                obligations.push_back("native_graph_call_interval_unimplemented:" + ids[i]);
        }
    };
    requireAdjacent({"device_begin_start", "device_initial_hover_enqueued", "device_begin_return",
        "initial_hover_wndproc_enter", "initial_hover_wndproc_return"});
    std::map<std::string, NativeGraphAction> actionById;
    std::map<std::string, std::size_t> moveById;
    std::map<std::string, std::size_t> probeById;
    std::vector<std::string> markers{"device_begin_start", "device_initial_hover_enqueued", "device_begin_return",
        "initial_hover_wndproc_enter", "initial_hover_wndproc_return"};
    for (std::size_t i = 0; i < program.steps.size(); ++i)
    {
        const auto& kind = program.steps[i].kind;
        const auto base = "program_" + std::to_string(i);
        if (kind == "source_start" || kind == "release")
        {
            requireAdjacent({base + "_queue_start", base + "_queue_return", base + "_wndproc_enter", base + "_wndproc_return"});
            markers.push_back(base + "_queue_return");
            markers.push_back(base + "_wndproc_enter");
            markers.push_back(base + "_wndproc_return");
            actionById.emplace(base + "_queue_start", NativeGraphAction{
                kind == "source_start" ? NativeGraphActionKind::down : NativeGraphActionKind::up, {i}});
        }
        else if (kind == "move_past_drag_threshold" || kind == "pointer_direction" || kind == "hover" ||
            kind == "pointer_direction_later" || kind == "hover_later")
            moveById.emplace(base + "_move_requirement", i);
        else if (kind == "observe_source_capture") actionById.emplace(base + "_observe", NativeGraphAction{NativeGraphActionKind::capture, {i}});
        else if (kind == "observe_completion_or_rejection") actionById.emplace(base + "_observe", NativeGraphAction{NativeGraphActionKind::completion, {i}});
        else if (kind == "end_pointer_session")
        {
            requireAdjacent({base + "_call_start", base + "_call_return"});
            markers.push_back(base + "_call_return");
            actionById.emplace(base + "_call_start", NativeGraphAction{NativeGraphActionKind::end, {i}});
            for (const auto message : {"wm_cancelmode", "wm_capturechanged", "wm_mouseleave", "conditional_focus_and_key_cleanup"})
                probeById.emplace(base + "_inspect_" + message, i);
        }
        else if (kind == "assert_business_results") actionById.emplace(base + "_observe", NativeGraphAction{NativeGraphActionKind::business, {i}});
    }
    std::size_t probeClasses = 0, probeEntries = 0;
    for (const auto& representative : graph.order)
    {
        const auto classIt = classById.find(representative);
        if (classIt == classById.end()) { obligations.push_back("native_graph_order_representative_missing:" + representative); continue; }
        const auto& ids = members.at(classIt->second);
        std::vector<std::size_t> moves, probes;
        for (const auto& id : ids)
        {
            if (const auto found = moveById.find(id); found != moveById.end()) moves.push_back(found->second);
            if (const auto found = probeById.find(id); found != probeById.end()) probes.push_back(found->second);
            if (!moveById.contains(id) && !probeById.contains(id) && !actionById.contains(id) &&
                std::find(markers.begin(), markers.end(), id) == markers.end())
                obligations.push_back("native_graph_requirement_handler_missing:" + id);
        }
        if (!moves.empty())
        {
            if (moves.size() != ids.size()) obligations.push_back("native_graph_move_class_mixed:" + representative);
            std::optional<POINT> common;
            for (const auto index : moves)
            {
                if (!points[index]) { obligations.push_back("native_graph_move_point_missing:" + std::to_string(index)); continue; }
                if (common && (common->x != points[index]->x || common->y != points[index]->y))
                    obligations.push_back("native_graph_shared_move_point_conflict:" + representative + ":" + std::to_string(index));
                else common = points[index];
            }
            actions.push_back({NativeGraphActionKind::move, std::move(moves)});
        }
        else if (!probes.empty())
        {
            ++probeClasses;
            probeEntries += probes.size();
            if (probes.size() != ids.size()) obligations.push_back("native_graph_probe_class_mixed:" + representative);
            actions.push_back({NativeGraphActionKind::end_probe, {probes.front()}});
        }
        else
        {
            if (ids.size() != 1) obligations.push_back("native_graph_control_class_mixed:" + representative);
            for (const auto& id : ids)
                if (const auto found = actionById.find(id); found != actionById.end()) actions.push_back(found->second);
        }
    }
    if (probeClasses != 1 || probeEntries != probeById.size() || probeEntries != 4)
        obligations.emplace_back("native_graph_distinct_end_probe_execution_unimplemented");
    // Even a compilable plan does not prove call/return/message identity.
    for (const auto& requirement : requirements)
        obligations.push_back("native_graph_actual_interval_observation_unimplemented:" + requirement.id);
    return actions;
}

class NativeSequence final
{
public:
    // Only one left-button session inside one owned desktop HWND is sketched
    // below. Preflight is intentionally closed until the candidate's complete
    // graph, environment, point mapping, all axes, and host delivery/observers
    // are bound. Callback presence cannot remove these obligations.
    static Result Run(const std::vector<Axis>& axes, const Candidate& candidate,
        const EventProgram& program, const event_graph::Variant& selectedGraph,
        const std::vector<NativePointInput>& points,
        HWND window, RECT screenBounds, const NativeHostBoundaries& host)
    {
        Result result;
        static const auto schema = Model();
        if (axes.size() != schema.size() || candidate.values.size() != axes.size())
        { result.obligations.emplace_back("native_candidate_schema_size_invalid"); return result; }
        for (std::size_t i = 0; i < axes.size(); ++i)
        {
            if (axes[i].id != schema[i].id || axes[i].role != schema[i].role ||
                axes[i].values != schema[i].values || axes[i].label != schema[i].label)
                result.obligations.push_back("native_schema_invalid:" + std::to_string(i));
            const auto& value = candidate.values[i];
            if ((axes[i].role == Role::Derived && !value.empty()) ||
                (axes[i].role != Role::Derived &&
                    std::find(axes[i].values.begin(), axes[i].values.end(), value) == axes[i].values.end()))
                result.obligations.push_back("native_candidate_value_invalid:" + axes[i].id);
        }
        if (!result.obligations.empty()) return result;
        Candidate canonical = candidate;
        canonical.lookup = BuildIndex(axes);
        if (canonical.lookup->size() != axes.size()) result.obligations.emplace_back("native_duplicate_axis");
        for (std::size_t i = 0; i < axes.size(); ++i)
        {
            if (axes[i].role != Role::Derived && !PrefixAllows(axes, canonical, axes[i].id, canonical.values[i]))
                result.obligations.push_back("native_prefix_invalid:" + axes[i].id);
            result.obligations.push_back(axes[i].role == Role::Derived
                ? "native_derived_observation_unimplemented:" + axes[i].id
                : "native_input_construction_unimplemented:" + axes[i].id + "=" + candidate.values[i]);
        }
        if (const auto excluded = StructuralExclusion(axes, canonical); !excluded.empty())
            result.obligations.push_back("native_structural_exclusion:" + std::string(excluded));
        const event_graph::Context graphContext(axes);
        const auto mapping = event_graph::MapSimpleNative(graphContext, canonical, program);
        std::optional<event_graph::Variant> replayedGraph;
        if (!mapping.structuralExclusion.empty())
            result.obligations.push_back("native_graph_structural_exclusion:" + mapping.structuralExclusion);
        for (const auto& reason : mapping.reasons) result.obligations.push_back("native_graph_mapping:" + reason);
        if (!mapping.graphAvailable) result.obligations.emplace_back("native_candidate_graph_unavailable");
        else if (RequireLegacyGraphPresence(mapping.spec, result.obligations, "native_graph_") &&
                 RequireLegacyGraphVersion(selectedGraph, result.obligations, "native_graph_"))
        {
            const auto replay = event_graph::ReplayVariant(graphContext, canonical, mapping.spec, selectedGraph.id);
            for (const auto& error : replay.errors) result.obligations.push_back("native_graph_replay:" + error);
            if (!replay.structuralExclusion.empty())
                result.obligations.push_back("native_graph_structural_exclusion:" + replay.structuralExclusion);
            if (!replay.identityValid) result.obligations.emplace_back("native_selected_graph_identity_invalid");
            else
            {
                const auto& trusted = replay.variant;
                // Ordinal is enumeration metadata, absent in a direct replay.
                if (selectedGraph.id != trusted.id || selectedGraph.classes != trusted.classes ||
                    selectedGraph.order != trusted.order || selectedGraph.unresolved != trusted.unresolved)
                    result.obligations.emplace_back("native_selected_graph_contents_mismatch");
                else replayedGraph = trusted;
                for (const auto& reason : trusted.unresolved)
                    result.obligations.push_back("native_selected_graph_unresolved:" + reason);
            }
        }
        const auto expected = drag_global::Program(axes, canonical);
        bool sameProgram = expected.unresolved == program.unresolved && expected.steps.size() == program.steps.size() &&
            SameProgramPostconditions(expected, program);
        if (sameProgram)
            for (std::size_t i = 0; i < program.steps.size(); ++i)
                if (expected.steps[i].kind != program.steps[i].kind || expected.steps[i].parameter != program.steps[i].parameter)
                { sameProgram = false; break; }
        if (!sameProgram) result.obligations.emplace_back("native_program_candidate_mismatch");
        for (const auto& reason : program.unresolved) result.obligations.push_back("native_program_unresolved:" + reason);
        if (Value(axes, canonical, "T01") != "internal_native" || Value(axes, canonical, "G01.button") != "left")
            result.obligations.emplace_back("native_transport_or_button_unimplemented");
        if (!window || !IsWindow(window) || IsRectEmpty(&screenBounds)) result.obligations.emplace_back("native_host_window_invalid");
        if (!host.beforeDeviceBegin || !host.observeDeviceState || !host.observeCapture || !host.awaitCompletion || !host.beforePointerSessionEnd || !host.observePointerSessionEnd ||
            !host.assertBusinessResults || !host.throwAsync || !host.productionInputEntered || !host.beforeQueuedDelivery)
            result.obligations.emplace_back("native_production_observers_missing");
        std::vector<std::optional<POINT>> byStep(program.steps.size());
        for (const auto& point : points)
        {
            if (point.programIndex >= byStep.size() || byStep[point.programIndex] || !PtInRect(&screenBounds, point.screen))
            { result.obligations.emplace_back("native_point_invalid_or_duplicate"); continue; }
            byStep[point.programIndex] = point.screen;
        }
        bool pressed = false, released = false, captureObserved = false, completionObserved = false,
            sessionEnded = false, businessObserved = false;
        for (std::size_t i = 0; i < program.steps.size(); ++i)
        {
            const auto& kind = program.steps[i].kind;
            const bool pointer = kind == "source_start" || kind == "move_past_drag_threshold" ||
                kind == "pointer_direction" || kind == "hover" || kind == "pointer_direction_later" ||
                kind == "hover_later" || kind == "release";
            const bool observer = kind == "observe_source_capture" || kind == "observe_completion_or_rejection" ||
                kind == "end_pointer_session" ||
                kind == "assert_business_results";
            if (!pointer && !observer) result.obligations.push_back("native_step_unimplemented:" + kind);
            if (pointer != byStep[i].has_value()) result.obligations.push_back("native_point_binding_invalid:" + std::to_string(i));
            if (kind == "source_start")
            {
                if (i != 0 || pressed || released) result.obligations.emplace_back("native_press_order_invalid");
                pressed = true;
            }
            else if (kind == "release")
            {
                if (!pressed || released || !byStep[i])
                    result.obligations.emplace_back("native_release_order_or_position_unimplemented");
                released = true;
            }
            else if (pointer && (!pressed || released)) result.obligations.emplace_back("native_move_order_invalid");
            else if (kind == "observe_source_capture")
            {
                if (!pressed || released || captureObserved) result.obligations.emplace_back("native_capture_stage_order_invalid");
                captureObserved = true;
            }
            else if (kind == "observe_completion_or_rejection")
            {
                if (!released || completionObserved) result.obligations.emplace_back("native_completion_stage_order_invalid");
                completionObserved = true;
            }
            else if (kind == "end_pointer_session")
            {
                if (!released || sessionEnded || businessObserved)
                    result.obligations.emplace_back("native_session_end_stage_order_invalid");
                sessionEnded = true;
            }
            else if (kind == "assert_business_results")
            {
                if (!completionObserved || !sessionEnded || businessObserved) result.obligations.emplace_back("native_business_stage_order_invalid");
                businessObserved = true;
            }
        }
        if (!pressed || !released || !captureObserved || !completionObserved || !sessionEnded || !businessObserved)
            result.obligations.emplace_back("native_session_stages_incomplete");
        std::vector<NativeGraphAction> graphActions;
        if (replayedGraph) graphActions = PlanNativeGraph(program, mapping.spec, *replayedGraph, byStep, result.obligations);
        else result.obligations.emplace_back("native_graph_action_plan_unavailable");
        std::optional<POINT> currentPoint;
        bool graphButtonDown = false, graphButtonReleased = false;
        for (const auto& action : graphActions)
        {
            if (action.programIndices.empty()) { result.obligations.emplace_back("native_graph_action_empty"); continue; }
            const auto index = action.programIndices.front();
            if (action.kind == NativeGraphActionKind::down)
            {
                if (graphButtonDown || graphButtonReleased) result.obligations.emplace_back("native_graph_press_order_unimplemented");
                graphButtonDown = true;
                currentPoint = byStep[index];
            }
            else if (action.kind == NativeGraphActionKind::move)
            {
                if (!graphButtonDown || graphButtonReleased) result.obligations.emplace_back("native_graph_move_button_state_unimplemented");
                currentPoint = byStep[index];
            }
            else if (action.kind == NativeGraphActionKind::up)
            {
                if (!graphButtonDown || graphButtonReleased) result.obligations.emplace_back("native_graph_release_order_unimplemented");
                graphButtonReleased = true;
                if (!currentPoint || !byStep[index] || currentPoint->x != byStep[index]->x || currentPoint->y != byStep[index]->y)
                    result.obligations.push_back("native_graph_release_position_unimplemented:" + std::to_string(index));
            }
        }
        // Device::Begin itself posts an initial hover; the model must first
        // represent and verify that real event. Native graph replay and all
        // point/modifier bindings are also still missing.
        result.obligations.emplace_back("native_initial_hover_event_unmodeled");
        result.obligations.emplace_back("native_graph_and_host_dispatch_binding_unimplemented");
        result.obligations.emplace_back("native_coordinates_modifiers_and_business_oracle_unimplemented");
        if (!result.obligations.empty()) return result;

        return DispatchPrepared(std::move(result), program, graphActions, byStep,
            *byStep.front(), window, screenBounds, host);
    }

private:
    friend class V4NativeSequence;
    friend class V4NativeSelectionPreparationExecutor;
    // The only physical Device path, shared after each version's independent
    // graph, input, host and observation preflight. It does not establish
    // those capabilities or authorize its own invocation.
    static Result DispatchPrepared(Result result, const EventProgram& program,
        const std::vector<NativeGraphAction>& graphActions,
        const std::vector<std::optional<POINT>>& byStep, POINT first,
        HWND window, RECT screenBounds, const NativeHostBoundaries& host,
        NativeDeviceCallJournal* calls = nullptr)
    {

        try { host.beforeDeviceBegin(); }
        catch (const std::exception& exception)
        { result.status = Status::fixture_failed; result.failure = exception.what(); return result; }
        catch (...)
        { result.status = Status::fixture_failed; result.failure = "unknown_native_pre_begin_exception"; return result; }

        // Kept behind the above complete-program gate: no test-side drag
        // decision, file operation, ownership mutation or layout commit.
        snowdesktop::private_pointer::Device device;
        // A bounded log is input observation, not an execution permit.
        // Overflow or inability to enable observation is not a product result.
        constexpr std::size_t deliveryLogCapacity = 16384;
        if (!device.EnableDeliveryObservation(deliveryLogCapacity))
        { result.status = Status::observation_unavailable; result.failure = "native_delivery_log_enable_unavailable"; return result; }
        std::string error;
        try
        {
            using snowdesktop::private_pointer::QueuedEventIdentity;
            using snowdesktop::private_pointer::DeliveryObservationKind;
            struct QueuedObservation { QueuedEventIdentity token; std::size_t logStart; };
            const auto captureQueued = [&]() {
                QueuedEventIdentity token{};
                if (!device.LastQueuedEventIdentity(token))
                    throw ObservationUnavailable("native_queued_event_identity_unavailable");
                const auto* log = device.DeliveryObservations();
                if (!log || log->failed || !log->session || log->ownerThread != GetCurrentThreadId() ||
                    token.ownerThread != log->ownerThread || token.session != log->session || !token.event)
                    throw ObservationUnavailable("native_queued_event_session_or_owner_unavailable");
                return QueuedObservation{token, log->entries.size()};
            };
            const auto expectInput = [&](UINT message, POINT screen, WPARAM buttons) {
                POINT client = screen, roundTrip{};
                if (!IsWindow(window) || !ScreenToClient(window, &client))
                    throw ObservationUnavailable("native_pre_dispatch_mapping_unavailable");
                roundTrip = client;
                if (!ClientToScreen(window, &roundTrip) || roundTrip.x != screen.x || roundTrip.y != screen.y ||
                    client.x < (std::numeric_limits<short>::min)() || client.x > (std::numeric_limits<short>::max)() ||
                    client.y < (std::numeric_limits<short>::min)() || client.y > (std::numeric_limits<short>::max)())
                    throw ObservationUnavailable("native_pre_dispatch_mapping_ambiguous_or_out_of_range");
                return NativeInputExpectation{message, screen, client, buttons};
            };
            const auto notifyQueued = [&](NativeDevicePhase phase, std::optional<std::size_t> index,
                    const QueuedEventIdentity& token) {
                // A fixture callback can reenter the message loop. If either
                // callback fails, do not assert that production was untouched.
                result.productionEntryUnknown = true;
                host.beforeQueuedDelivery(phase, index, token);
                const bool entered = host.productionInputEntered();
                result.productionStarted = result.productionStarted || entered;
                result.productionEntryUnknown = false;
            };
            if (calls) calls->Record(NativeDeviceCallKind::begin, std::nullopt, false);
            const bool began = device.Begin(screenBounds, first, [window](POINT) { return window; },
                    [window](HWND candidateWindow) { return candidateWindow == window; }, error);
            if (calls) calls->Record(NativeDeviceCallKind::begin, std::nullopt, true, began);
            if (!began)
                throw std::runtime_error(error);
            const auto initialToken = captureQueued();
            const auto initialExpected = expectInput(WM_MOUSEMOVE, first, 0);
            notifyQueued(NativeDevicePhase::initial_hover, std::nullopt, initialToken.token);
            const auto observeState = [&](NativeDevicePhase phase, std::optional<std::size_t> index,
                    std::optional<QueuedObservation> queued, std::optional<NativeInputExpectation> expectedInput) {
                const auto* log = device.DeliveryObservations();
                if (!log || log->failed || !log->session || log->ownerThread != GetCurrentThreadId())
                    throw ObservationUnavailable("native_delivery_log_unavailable_or_incomplete");
                if ((phase == NativeDevicePhase::session_end) != !queued.has_value() ||
                    queued.has_value() != expectedInput.has_value())
                    throw ObservationUnavailable("native_queued_event_phase_mismatch");
                if (queued)
                {
                    const auto& token = queued->token;
                    if (token.ownerThread != log->ownerThread || token.session != log->session || !token.event ||
                        queued->logStart > log->entries.size())
                        throw ObservationUnavailable("native_queued_event_session_or_owner_unavailable");
                    // A token names the enqueue, not a WndProc message. Require
                    // a unique completed primary delivery and reject later or
                    // nested queued inputs that make this State ambiguous.
                    std::size_t begin = log->entries.size(), returned = log->entries.size();
                    std::uint64_t delivery = 0;
                    for (std::size_t n = queued->logStart; n < log->entries.size(); ++n)
                    {
                        const auto& entry = log->entries[n];
                        const auto& identity = entry.identity;
                        if (identity.ownerThread != token.ownerThread || identity.session != token.session ||
                            !identity.queuedEvent || identity.forwarded || identity.forward != 0) continue;
                        if (identity.event == token.event && entry.kind == DeliveryObservationKind::begin)
                        {
                            if (begin != log->entries.size() || !identity.delivery)
                                throw ObservationUnavailable("native_queued_delivery_ambiguous");
                            begin = n;
                            delivery = identity.delivery;
                        }
                        else if (identity.event == token.event && entry.kind == DeliveryObservationKind::returned)
                        {
                            if (returned != log->entries.size() || !delivery || identity.delivery != delivery)
                                throw ObservationUnavailable("native_queued_delivery_ambiguous");
                            returned = n;
                        }
                        else if (identity.event != token.event && entry.kind == DeliveryObservationKind::begin)
                            throw ObservationUnavailable("native_nested_or_later_queued_event_unresolved");
                    }
                    if (begin == log->entries.size() || returned == log->entries.size() || returned <= begin)
                        throw ObservationUnavailable("native_queued_delivery_incomplete");
                }
                try
                {
                    host.observeDeviceState(phase, index, device.Current(), *log,
                        queued ? std::optional<QueuedEventIdentity>(queued->token) : std::nullopt, expectedInput);
                }
                catch (const EventAssertionFailure&)
                {
                    // Only an independent, candidate-bound event oracle may
                    // throw this. A missing log, unknown provenance or an
                    // unmet graph presence hypothesis is not a product failure.
                    result.eventOracleFailureObserved = true;
                    throw;
                }
            };
            const auto waitDelivery = [&] {
                const auto deadline = GetTickCount64() + 12000;
                while (device.Current().pendingEvents)
                {
                    host.throwAsync();
                    if (GetTickCount64() >= deadline) throw std::runtime_error("native_delivery_timeout");
                    if (MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_FAILED)
                        throw std::runtime_error("native_message_wait_failed");
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                    {
                        if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); throw std::runtime_error("native_delivery_interrupted"); }
                        result.productionEntryUnknown = true;
                        DispatchMessageW(&message);
                        const bool entered = host.productionInputEntered();
                        result.productionStarted = result.productionStarted || entered;
                        result.productionEntryUnknown = false;
                        host.throwAsync();
                        if (GetTickCount64() >= deadline) break;
                    }
                }
                host.throwAsync();
                if (!device.Current().enabled || !device.Current().error.empty())
                    throw std::runtime_error("native_delivery_failed:" + device.Current().error);
            };
            // Begin queues the initial hover; only the production HWND trace
            // can establish whether any input reached the real WndProc.
            waitDelivery();
            observeState(NativeDevicePhase::initial_hover, std::nullopt, initialToken, initialExpected);
            host.throwAsync();
            result.status = Status::attempted;
            for (const auto& action : graphActions)
            {
                const auto i = action.programIndices.front();
                const auto& step = program.steps[i];
                if (action.kind == NativeGraphActionKind::down)
                {
                    if (calls) calls->Record(NativeDeviceCallKind::down, i, false);
                    const bool queued = device.Down(snowdesktop::private_pointer::Button::Left, error);
                    if (calls) calls->Record(NativeDeviceCallKind::down, i, true, queued);
                    if (!queued) throw std::runtime_error(error);
                    const auto token = captureQueued();
                    const auto expectedInput = expectInput(WM_LBUTTONDOWN, *byStep[i], MK_LBUTTON);
                    notifyQueued(NativeDevicePhase::program_input, i, token.token);
                    waitDelivery();
                    observeState(NativeDevicePhase::program_input, i, token, expectedInput);
                    host.throwAsync();
                }
                else if (action.kind == NativeGraphActionKind::up)
                {
                    if (calls) calls->Record(NativeDeviceCallKind::up, i, false);
                    const bool queued = device.Up(snowdesktop::private_pointer::Button::Left, error);
                    if (calls) calls->Record(NativeDeviceCallKind::up, i, true, queued);
                    if (!queued) throw std::runtime_error(error);
                    const auto token = captureQueued();
                    const auto expectedInput = expectInput(WM_LBUTTONUP, *byStep[i], 0);
                    notifyQueued(NativeDevicePhase::program_input, i, token.token);
                    waitDelivery();
                    observeState(NativeDevicePhase::program_input, i, token, expectedInput);
                    host.throwAsync();
                }
                else if (action.kind == NativeGraphActionKind::move)
                {
                    if (calls) calls->Record(NativeDeviceCallKind::move, i, false);
                    const bool queued = device.Move(*byStep[i], error);
                    if (calls) calls->Record(NativeDeviceCallKind::move, i, true, queued);
                    if (!queued) throw std::runtime_error(error);
                    const auto token = captureQueued();
                    const auto expectedInput = expectInput(WM_MOUSEMOVE, *byStep[i], MK_LBUTTON);
                    notifyQueued(NativeDevicePhase::program_input, i, token.token);
                    waitDelivery();
                    // All indices in this class refer to this exact one move;
                    // no second Device::Move is synthesized for an alias.
                    for (const auto index : action.programIndices)
                    {
                        observeState(NativeDevicePhase::program_input, index, token, expectedInput);
                        host.throwAsync();
                    }
                }
                else if (action.kind == NativeGraphActionKind::capture)
                {
                    try { host.observeCapture(step); }
                    catch (const EventAssertionFailure&)
                    { result.eventOracleFailureObserved = true; throw; }
                }
                else if (action.kind == NativeGraphActionKind::completion) host.awaitCompletion(step);
                else if (action.kind == NativeGraphActionKind::end)
                {
                    host.throwAsync();
                    const auto* beforeEndLog = device.DeliveryObservations();
                    if (!beforeEndLog || beforeEndLog->failed || !beforeEndLog->session ||
                        beforeEndLog->ownerThread != GetCurrentThreadId())
                        throw ObservationUnavailable("native_before_end_delivery_log_unavailable");
                    const auto beforeEndCount = beforeEndLog->entries.size();
                    const auto beforeEndSession = beforeEndLog->session;
                    result.productionEntryUnknown = true;
                    host.beforePointerSessionEnd(step, device.Current(), *beforeEndLog);
                    const bool alreadyEntered = host.productionInputEntered();
                    result.productionStarted = result.productionStarted || alreadyEntered;
                    result.productionEntryUnknown = false;
                    const auto* unchangedLog = device.DeliveryObservations();
                    if (!unchangedLog || unchangedLog->failed || unchangedLog->session != beforeEndSession ||
                        unchangedLog->ownerThread != GetCurrentThreadId() || unchangedLog->entries.size() != beforeEndCount ||
                        !device.Current().enabled || device.Current().closing)
                        throw ObservationUnavailable("native_before_end_callback_reentrancy_unavailable");
                    result.productionEntryUnknown = true;
                    if (calls) calls->Record(NativeDeviceCallKind::end, i, false);
                    device.End();
                    if (calls) calls->Record(NativeDeviceCallKind::end, i, true);
                    const bool entered = host.productionInputEntered();
                    result.productionStarted = result.productionStarted || entered;
                    result.productionEntryUnknown = false;
                    host.throwAsync();
                    if (device.Current().enabled || !device.Current().error.empty())
                        throw std::runtime_error("native_session_end_failed:" + device.Current().error);
                    observeState(NativeDevicePhase::session_end, i, std::nullopt, std::nullopt);
                    host.throwAsync();
                }
                else if (action.kind == NativeGraphActionKind::end_probe)
                {
                    host.observePointerSessionEnd(step);
                    host.throwAsync();
                }
                else if (action.kind == NativeGraphActionKind::business)
                {
                    try { host.assertBusinessResults(step); }
                    catch (const BusinessAssertionFailure&)
                    { result.businessFailureObserved = true; throw; }
                }
                else throw std::runtime_error("native_unexpected_step:" + step.kind);
            }
        }
        catch (const ObservationUnavailable& exception)
        { result.status = Status::observation_unavailable; result.additionalObservationUnavailable = true; result.failure = exception.what(); }
        catch (const BusinessAssertionFailure& exception)
        { result.status = result.businessFailureObserved ? Status::business_failed : Status::observation_unavailable;
          result.additionalObservationUnavailable = !result.businessFailureObserved; result.failure = exception.what(); }
        catch (const EventAssertionFailure& exception)
        { result.status = result.eventOracleFailureObserved ? Status::event_oracle_failed : Status::observation_unavailable;
          result.additionalObservationUnavailable = !result.eventOracleFailureObserved; result.failure = exception.what(); }
        catch (const std::exception& exception)
        { result.status = Status::observation_unavailable; result.additionalObservationUnavailable = true; result.failure = exception.what(); }
        catch (...)
        { result.status = Status::observation_unavailable; result.additionalObservationUnavailable = true; result.failure = "unknown_native_session_exception"; }
        // An exceptional exit may still own a device. Its cancellation is
        // cleanup only, never the modeled end stage or business evidence.
        if (device.Current().enabled)
        {
            const bool priorEntryUnknown = result.productionEntryUnknown;
            result.productionEntryUnknown = true;
            device.End();
            try
            {
                const bool entered = host.productionInputEntered();
                result.productionStarted = result.productionStarted || entered;
                result.productionEntryUnknown = priorEntryUnknown;
                host.throwAsync();
            }
            catch (const std::exception& exception)
            { result.failure += std::string("; native_cleanup_exception:") + exception.what(); result.additionalObservationUnavailable = true; if (!result.businessFailureObserved && !result.eventOracleFailureObserved) result.status = Status::observation_unavailable; }
            catch (...)
            { result.failure += "; unknown_native_cleanup_exception"; result.additionalObservationUnavailable = true; if (!result.businessFailureObserved && !result.eventOracleFailureObserved) result.status = Status::observation_unavailable; }
            if (device.Current().enabled || !device.Current().error.empty())
            { result.failure += "; native_cleanup_incomplete:" + device.Current().error; result.additionalObservationUnavailable = true; if (!result.businessFailureObserved && !result.eventOracleFailureObserved) result.status = Status::observation_unavailable; }
        }
        return result;
    }
};

// Immutable replay-derived class identities for the selected V4 native graph.
// A class is graph-local, not a Device token or evidence that an event fired.
class V4NativeMoveClassEvidence final
{
public:
    static V4NativeMoveClassEvidence Inspect(const event_graph::V4FamilyIdentityContext& identity,
        const V4FamilyValidatedContext& verified, const std::vector<bool>& present,
        const event_graph::V4NativeJoinedGraphVariant& selected)
    {
        V4NativeMoveClassEvidence out;
        try
        {
            const auto mapping = event_graph::MapV4NativePresenceCandidate(identity, verified);
            const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
            if (!mapping.graphAvailable || raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty())
            { out.unresolved_.emplace_back("v4_native_move_candidate_or_mapping_unavailable"); return out; }
            const auto replay = event_graph::V4NativeJoinedGraph::Replay(identity, verified, present, selected.id);
            if (!replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
                replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
                replay.variant.unresolved != selected.unresolved)
            { out.unresolved_.emplace_back("v4_native_move_graph_replay_unavailable"); return out; }
            auto all = mapping.spec.requirements;
            std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
            if (all.size() != present.size()) { out.unresolved_.emplace_back("v4_native_move_presence_size_unavailable"); return out; }
            std::map<std::string, std::size_t> classes;
            std::map<std::size_t, std::vector<std::string>> members;
            std::size_t active = 0;
            for (std::size_t i = 0; i < all.size(); ++i)
                if (present[i])
                {
                    if (active >= selected.classes.size()) { out.unresolved_.emplace_back("v4_native_move_partition_unavailable"); return out; }
                    const auto group = selected.classes[active++];
                    classes.emplace(all[i].id, group); members[group].push_back(all[i].id);
                }
            if (active != selected.classes.size()) { out.unresolved_.emplace_back("v4_native_move_partition_unavailable"); return out; }
            const auto program = verified.TransferProgram();
            out.byProgramIndex_.resize(program.steps.size());
            std::map<std::size_t, std::vector<std::size_t>> grouped;
            for (std::size_t i = 0; i < program.steps.size(); ++i)
            {
                const auto& kind = program.steps[i].kind;
                if (kind != "move_past_drag_threshold" && kind != "pointer_direction" && kind != "hover" &&
                    kind != "pointer_direction_later" && kind != "hover_later") continue;
                const auto id = "program_" + std::to_string(i) + "_queue_start";
                const auto it = classes.find(id);
                if (it == classes.end()) { out.unresolved_.push_back("v4_native_move_start_absent:" + id); continue; }
                out.byProgramIndex_[i] = it->second; grouped[it->second].push_back(i);
            }
            for (const auto& [group, indices] : grouped)
            {
                const std::array<std::string_view, 4> suffixes = {"_queue_start", "_queue_return", "_wndproc_enter", "_wndproc_return"};
                for (const auto suffix : suffixes)
                {
                    std::optional<std::size_t> common;
                    for (const auto i : indices)
                    {
                        const auto id = "program_" + std::to_string(i) + std::string(suffix);
                        const auto it = classes.find(id);
                        if (it == classes.end() || (common && *common != it->second))
                        { out.unresolved_.push_back("v4_native_move_interval_partition_unavailable:" + id); continue; }
                        common = it->second;
                    }
                    if (common && members[*common].size() != indices.size())
                        out.unresolved_.push_back("v4_native_move_interval_class_mixed:" + std::to_string(group));
                }
            }
            out.candidateId_ = raw.id; out.graphId_ = selected.id;
            out.identityValid_ = true;
            out.completeMoveBindings_ = out.unresolved_.empty();
            out.unresolved_.insert(out.unresolved_.end(), replay.variant.unresolved.begin(), replay.variant.unresolved.end());
            out.unresolved_.emplace_back("v4_native_move_actual_device_and_message_binding_unverified");
        }
        catch (...) { out.identityValid_ = false; out.completeMoveBindings_ = false; out.resourceUnavailable_ = true; }
        return out;
    }
    bool IdentityValid() const noexcept { return identityValid_; }
    bool CompleteMoveBindings() const noexcept { return completeMoveBindings_; }
    bool ResourceUnavailable() const noexcept { return resourceUnavailable_; }
    std::string_view CandidateId() const noexcept { return candidateId_; }
    std::string_view GraphId() const noexcept { return graphId_; }
    const std::vector<std::optional<std::size_t>>& ByProgramIndex() const noexcept { return byProgramIndex_; }
    const std::vector<std::string>& Unresolved() const noexcept { return unresolved_; }
private:
    bool identityValid_ = false, completeMoveBindings_ = false, resourceUnavailable_ = false;
    std::string candidateId_, graphId_;
    std::vector<std::optional<std::size_t>> byProgramIndex_;
    std::vector<std::string> unresolved_;
};

struct V4NativeExecutionPlan
{
    bool graphIdentityValid = false;
    std::vector<NativeGraphAction> actions;
    std::vector<std::optional<POINT>> points;
    std::optional<POINT> sourcePoint;
    std::vector<std::string> preconditions, postconditions, unclassified;
};

// Independent V4 graph identity: no legacy candidate or graph conversion.
// Graph presence is an outcome hypothesis, not an instruction to synthesize
// or suppress a Windows message. Unsupported selected orders remain blocked.
inline V4NativeExecutionPlan BuildV4NativeExecutionPlan(
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const event_graph::V4NativeJoinedGraphVariant& selected,
    const std::vector<NativePointInput>& points, HWND window, RECT bounds)
{
    V4NativeExecutionPlan out;
    const auto mapping = event_graph::MapV4NativePresenceCandidate(identity, verified);
    if (!mapping.graphAvailable)
    { out.preconditions.emplace_back("v4_native_graph_unavailable"); out.preconditions.insert(out.preconditions.end(), mapping.reasons.begin(), mapping.reasons.end()); return out; }
    const auto replay = event_graph::V4NativeJoinedGraph::Replay(identity, verified, present, selected.id);
    out.preconditions.insert(out.preconditions.end(), replay.errors.begin(), replay.errors.end());
    if (!replay.identityValid || selected.id != replay.variant.id || selected.classes != replay.variant.classes ||
        selected.order != replay.variant.order || selected.unresolved != replay.variant.unresolved)
    { out.preconditions.emplace_back("v4_native_graph_identity_mismatch"); return out; }
    out.graphIdentityValid = true;
    // The preview's free-form reasons mix capabilities and future facts;
    // do not guess their stage from their spelling.
    out.unclassified = replay.variant.unresolved;
    out.unclassified.insert(out.unclassified.end(), mapping.reasons.begin(), mapping.reasons.end());
    auto requirements = mapping.spec.requirements;
    std::sort(requirements.begin(), requirements.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    if (requirements.size() != present.size()) { out.preconditions.emplace_back("v4_native_presence_size_invalid"); return out; }
    std::vector<event_graph::Requirement> active;
    for (std::size_t i = 0; i < requirements.size(); ++i)
    {
        out.postconditions.push_back(std::string("v4_native_presence_observation:") + requirements[i].id +
            (present[i] ? "=present" : "=absent"));
        if (present[i]) active.push_back(requirements[i]);
    }
    if (active.size() != selected.classes.size()) { out.preconditions.emplace_back("v4_native_partition_size_invalid"); return out; }
    const auto program = verified.TransferProgram();
    out.points.resize(program.steps.size());
    if (!window || !IsWindow(window) || IsRectEmpty(&bounds)) out.preconditions.emplace_back("v4_native_window_or_bounds_unavailable");
    for (const auto& point : points)
    {
        if (point.programIndex >= out.points.size() || out.points[point.programIndex] || !PtInRect(&bounds, point.screen))
        { out.preconditions.emplace_back("v4_native_point_invalid_or_duplicate"); continue; }
        out.points[point.programIndex] = point.screen;
    }
    std::map<std::string, NativeGraphAction> byId;
    // These graph nodes describe observed boundaries of a real device call;
    // they are not additional actions the fixture should synthesize.
    std::set<std::string> passiveIntervals{"v4_native_device_begin_start", "v4_native_device_begin_return",
        "v4_native_initial_hover_wndproc_enter", "v4_native_initial_hover_wndproc_return"};
    for (std::size_t i = 0; i < program.steps.size(); ++i)
    {
        const auto& kind = program.steps[i].kind;
        const auto base = "program_" + std::to_string(i);
        const bool move = kind == "move_past_drag_threshold" || kind == "pointer_direction" || kind == "hover" ||
            kind == "pointer_direction_later" || kind == "hover_later";
        if (kind == "source_start" || move || kind == "release")
        {
            if (!out.points[i]) out.preconditions.push_back("v4_native_point_missing:" + base);
            if (kind == "source_start" && out.points[i])
            { if (out.sourcePoint) out.preconditions.emplace_back("v4_native_multiple_source_points"); else out.sourcePoint = *out.points[i]; }
            byId.emplace(base + "_queue_start", NativeGraphAction{kind == "source_start" ? NativeGraphActionKind::down :
                kind == "release" ? NativeGraphActionKind::up : NativeGraphActionKind::move, {i}});
            passiveIntervals.insert(base + "_queue_return");
            passiveIntervals.insert(base + "_wndproc_enter");
            passiveIntervals.insert(base + "_wndproc_return");
        }
        else if (kind == "observe_source_capture") byId.emplace(base + "_observe", NativeGraphAction{NativeGraphActionKind::capture, {i}});
        else if (kind == "observe_completion_or_rejection") byId.emplace(base + "_observe", NativeGraphAction{NativeGraphActionKind::completion, {i}});
        else if (kind == "end_pointer_session")
        { byId.emplace(base + "_call_start", NativeGraphAction{NativeGraphActionKind::end, {i}}); passiveIntervals.insert(base + "_call_return"); }
        else if (kind == "assert_business_results") byId.emplace(base + "_observe", NativeGraphAction{NativeGraphActionKind::business, {i}});
        else if (kind == "configure_v4_source_input") out.preconditions.push_back("v4_native_source_construction_unimplemented:" + std::to_string(i));
        else out.preconditions.push_back("v4_native_program_step_unimplemented:" + kind);
    }
    std::map<std::string, std::size_t> classById;
    std::map<std::size_t, std::vector<std::string>> members;
    for (std::size_t i = 0; i < active.size(); ++i)
    { classById.emplace(active[i].id, selected.classes[i]); members[selected.classes[i]].push_back(active[i].id); }
    for (const auto& [id, action] : byId)
    {
        (void)action;
        if (!classById.contains(id)) out.preconditions.push_back("v4_native_selected_action_unavailable:" + id);
    }
    for (const auto& requirement : active)
        if (!byId.contains(requirement.id) && !passiveIntervals.contains(requirement.id))
            out.preconditions.push_back("v4_native_interval_handler_unimplemented:" + requirement.id);
    std::map<std::size_t, std::size_t> rankByClass;
    for (std::size_t rank = 0; rank < selected.order.size(); ++rank)
    {
        const auto entry = classById.find(selected.order[rank]);
        if (entry == classById.end() || !rankByClass.emplace(entry->second, rank).second)
            out.preconditions.emplace_back("v4_native_graph_order_ambiguous");
    }
    const auto classOf = [&](const std::string& id) -> std::optional<std::size_t> {
        const auto it = classById.find(id);
        return it == classById.end() ? std::nullopt : std::optional<std::size_t>(it->second);
    };
    const auto requireAdjacent = [&](const std::vector<std::string>& ids) {
        std::optional<std::size_t> first;
        for (std::size_t i = 0; i < ids.size(); ++i)
        {
            const auto group = classOf(ids[i]);
            if (!group || !rankByClass.contains(*group))
            { out.preconditions.push_back("v4_native_interval_unavailable:" + ids[i]); continue; }
            const auto rank = rankByClass.at(*group);
            if (!first && i == 0) first = rank;
            if (!first || rank != *first + i) out.preconditions.push_back("v4_native_interval_order_unavailable:" + ids[i]);
        }
    };
    requireAdjacent({"v4_native_device_begin_start", "v4_native_device_begin_return",
        "v4_native_initial_hover_wndproc_enter", "v4_native_initial_hover_wndproc_return"});
    for (const auto& [id, action] : byId)
    {
        if (action.kind != NativeGraphActionKind::down && action.kind != NativeGraphActionKind::move &&
            action.kind != NativeGraphActionKind::up) continue;
        const auto base = "program_" + std::to_string(action.programIndices.front());
        requireAdjacent({base + "_queue_start", base + "_queue_return", base + "_wndproc_enter", base + "_wndproc_return"});
    }
    for (const auto& representative : selected.order)
    {
        const auto found = classById.find(representative);
        if (found == classById.end()) { out.preconditions.emplace_back("v4_native_graph_representative_unavailable"); continue; }
        const auto& ids = members.at(found->second);
        std::optional<NativeGraphAction> action;
        for (const auto& id : ids)
        {
            const auto entry = byId.find(id);
            if (entry == byId.end()) continue;
            if (!action) action = entry->second;
            else if (action->kind == NativeGraphActionKind::move && entry->second.kind == NativeGraphActionKind::move)
                action->programIndices.push_back(entry->second.programIndices.front());
            else out.preconditions.push_back("v4_native_graph_mixed_action_class:" + representative);
        }
        if (action)
        {
            std::size_t actionMembers = 0;
            for (const auto& id : ids) if (byId.contains(id)) ++actionMembers;
            if (actionMembers != ids.size()) out.preconditions.push_back("v4_native_graph_action_class_mixed:" + representative);
            if (action->kind == NativeGraphActionKind::move)
            {
                std::optional<POINT> point;
                std::optional<std::size_t> returnClass, enterClass, exitClass;
                for (const auto index : action->programIndices)
                {
                    const auto base = "program_" + std::to_string(index);
                    const auto returned = classOf(base + "_queue_return");
                    const auto entered = classOf(base + "_wndproc_enter");
                    const auto exited = classOf(base + "_wndproc_return");
                    if (!returned || !entered || !exited || (returnClass && returnClass != returned) ||
                        (enterClass && enterClass != entered) || (exitClass && exitClass != exited))
                        out.preconditions.push_back("v4_native_shared_move_interval_identity_unavailable:" + base);
                    else { returnClass = returned; enterClass = entered; exitClass = exited; }
                    if (out.points[index])
                    { if (point && (point->x != out.points[index]->x || point->y != out.points[index]->y))
                          out.preconditions.push_back("v4_native_shared_move_coordinates_unavailable:" + representative);
                      else point = out.points[index]; }
                }
            }
            out.actions.push_back(std::move(*action));
        }
    }
    out.unclassified.insert(out.unclassified.end(), program.unresolved.begin(), program.unresolved.end());
    if (!out.unclassified.empty()) out.preconditions.emplace_back("v4_native_unclassified_capability_unavailable");
    for (const auto& condition : program.postconditions)
        out.postconditions.push_back("outcome:" + condition.axis + "=" + condition.value);
    // No callback, raw point, or graph preview proves these capabilities.
    out.preconditions.emplace_back("v4_native_complete_input_host_and_geometry_binding_unimplemented");
    out.preconditions.emplace_back("v4_native_selected_presence_delivery_and_intervals_unimplemented");
    out.preconditions.emplace_back("v4_native_completion_and_business_oracle_capability_unimplemented");
    return out;
}

// Construct only the concrete device actions for an isolated diagnostic
// attempt. Graph presence and unrelated candidate inputs remain unverified.
inline bool BuildRestrictedNativeDesktopActions(const V4FamilyValidatedContext& verified,
    const std::vector<NativePointInput>& supplied, HWND window, RECT bounds,
    std::vector<NativeGraphAction>& actions, std::vector<std::optional<POINT>>& points, POINT& first)
{
    const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
    if (verified.TaskBActive() || !window || !IsWindow(window) || IsRectEmpty(&bounds) ||
        value("T01") != "internal_native" || value("V4.source.kind") != "file_only" ||
        value("V4.source.file_count") != "one" || value("V4.file.member0_kind") != "file" ||
        value("V4.file.member0_identity") != "unique" || value("source.E01") != "free_desktop" ||
        value("source.E02") != "none" || value("source.E03") != "desktop_icon" ||
        value("target.E01") != "free_desktop" || value("target.E02") != "none" ||
        value("H01.region") != "empty_slot" || value("P01.primary") != "file" || value("P01.second") != "na" ||
        value("P02.source_actual_count") != "one" || value("G01.button") != "left" ||
        value("G02.press") != "none" || value("G02.release") != "none" || value("G02.release_buttons") != "none" ||
        value("G04.termination") != "drop" || value("X03.task_count") != "one" ||
        value("H09.grab_area") != "icon" || value("H09.offset") != "center" ||
        value("H09.selected_member") != "first" || value("H09.member_index") != "member_0") return false;
    const auto program = verified.TransferProgram();
    std::vector<std::optional<POINT>> byStep(program.steps.size());
    for (const auto& point : supplied)
    {
        if (point.programIndex >= byStep.size() || byStep[point.programIndex] || !PtInRect(&bounds, point.screen)) return false;
        byStep[point.programIndex] = point.screen;
    }
    const std::array<std::pair<std::string_view, NativeGraphActionKind>, 7> expected{{
        {"source_start", NativeGraphActionKind::down}, {"move_past_drag_threshold", NativeGraphActionKind::move},
        {"observe_source_capture", NativeGraphActionKind::capture}, {"release", NativeGraphActionKind::up},
        {"observe_completion_or_rejection", NativeGraphActionKind::completion},
        {"end_pointer_session", NativeGraphActionKind::end}, {"assert_business_results", NativeGraphActionKind::business}}};
    std::vector<NativeGraphAction> concrete; std::size_t next = 0, pointCount = 0;
    std::optional<POINT> lastMove, releasePoint;
    for (std::size_t i = 0; i < program.steps.size(); ++i)
    {
        const auto& step = program.steps[i];
        if (step.kind == "configure_v4_source_input") { if (next || byStep[i]) return false; continue; }
        if (next >= expected.size() || step.kind != expected[next].first) return false;
        const auto kind = expected[next++].second;
        if (kind == NativeGraphActionKind::down || kind == NativeGraphActionKind::move || kind == NativeGraphActionKind::up)
        {
            if (!byStep[i]) return false; ++pointCount;
            if (kind == NativeGraphActionKind::down) first = *byStep[i];
            if (kind == NativeGraphActionKind::move) lastMove = *byStep[i];
            if (kind == NativeGraphActionKind::up) releasePoint = *byStep[i];
        }
        else if (byStep[i]) return false;
        concrete.push_back({kind, {i}});
    }
    if (next != expected.size() || supplied.size() != 3 || pointCount != 3 || !lastMove || !releasePoint ||
        lastMove->x != releasePoint->x || lastMove->y != releasePoint->y) return false;
    actions = std::move(concrete); points = std::move(byStep); return true;
}

struct V4NativeDeviceRecord
{
    NativeDevicePhase phase;
    std::optional<std::size_t> programIndex;
    snowdesktop::private_pointer::State state;
    snowdesktop::private_pointer::DeliveryObservationLog delivery;
    std::optional<snowdesktop::private_pointer::QueuedEventIdentity> queued;
    std::optional<NativeInputExpectation> expected;
    // Same-host trace sealed before the host observation callback can reenter.
    std::vector<MessageObservation> trace;
};
struct V4NativeQueuedRecord
{
    NativeDevicePhase phase;
    std::optional<std::size_t> programIndex;
    snowdesktop::private_pointer::QueuedEventIdentity token;
};
struct V4NativeTraceRecord
{
    std::string boundary;
    std::vector<MessageObservation> messages;
};

class V4NativeDesktopObserverIdentity final
{
    friend struct ::behavior::V4NativeDesktopMoveObservation;
    V4NativeDesktopObserverIdentity() = default;
public:
    V4NativeDesktopObserverIdentity(const V4NativeDesktopObserverIdentity&) = delete;
    V4NativeDesktopObserverIdentity& operator=(const V4NativeDesktopObserverIdentity&) = delete;
    ~V4NativeDesktopObserverIdentity() = default;
};
class V4NativeDesktopExecutionKey final
{
    friend class V4NativeSequence;
    friend class V4NativeRunReceipt;
    friend struct ::behavior::V4NativeDesktopMoveObservation;
public:
    V4NativeDesktopExecutionKey(const V4NativeDesktopExecutionKey&) = delete;
    V4NativeDesktopExecutionKey& operator=(const V4NativeDesktopExecutionKey&) = delete;
private:
    enum class Phase { created, before_begin, dispatching, business, capturing, closed };
    V4NativeDesktopExecutionKey(const std::shared_ptr<NativeHostEpoch>& epoch, std::string candidate, std::string graph)
        : epoch_(epoch), candidate_(std::move(candidate)), graph_(std::move(graph)), thread_(GetCurrentThreadId()) {}
    bool Same(const std::shared_ptr<NativeHostEpoch>& epoch, const void* app, std::string_view candidate) const noexcept
    {
        const auto actual = epoch_.lock();
        return actual && epoch && actual.get() == epoch.get() && !actual.owner_before(epoch) && !epoch.owner_before(actual) &&
            thread_ == GetCurrentThreadId() && actual->app_ == reinterpret_cast<std::uintptr_t>(app) &&
            candidate == candidate_ && !graph_.empty() && actual->Matches(actual->trace_, actual->desktop_, actual->control_);
    }
    bool BindBaseline(const std::shared_ptr<NativeHostEpoch>& epoch, const void* app, std::string_view candidate,
        const std::shared_ptr<const V4NativeDesktopObserverIdentity>& observer) noexcept
    {
        if (phase_ != Phase::before_begin || bound_ || !observer || !Same(epoch, app, candidate)) return false;
        observer_ = observer; bound_ = true; return true;
    }
    bool BeginCapture(const std::shared_ptr<NativeHostEpoch>& epoch, const void* app, std::string_view candidate,
        const std::shared_ptr<const V4NativeDesktopObserverIdentity>& observer) noexcept
    {
        const auto bound = observer_.lock();
        if (phase_ != Phase::business || !bound_ || published_ || !observer || !bound ||
            bound.get() != observer.get() || bound.owner_before(observer) || observer.owner_before(bound) ||
            !Same(epoch, app, candidate)) return false;
        phase_ = Phase::capturing; return true;
    }
    bool Publish(const std::shared_ptr<NativeHostEpoch>& epoch, const void* app, std::string_view candidate,
        const std::shared_ptr<const V4NativeDesktopObserverIdentity>& observer,
        std::shared_ptr<const ::behavior::V4NativeDesktopMoveEvidence> value) noexcept
    {
        const auto bound = observer_.lock();
        if (!value || phase_ != Phase::capturing || !bound_ || published_ || !observer || !bound ||
            bound.get() != observer.get() || bound.owner_before(observer) || observer.owner_before(bound) ||
            !Same(epoch, app, candidate)) return false;
        snapshot_ = std::move(value); published_ = true; return true;
    }
    std::weak_ptr<NativeHostEpoch> epoch_;
    std::weak_ptr<const V4NativeDesktopObserverIdentity> observer_;
    std::string candidate_, graph_;
    DWORD thread_ = 0;
    Phase phase_ = Phase::created;
    bool bound_ = false, published_ = false, committed_ = false;
    std::shared_ptr<const ::behavior::V4NativeDesktopMoveEvidence> snapshot_;
};
struct V4NativeDesktopObserverBoundaries
{
    std::function<void(const std::shared_ptr<V4NativeDesktopExecutionKey>&)> bindBeforeBegin;
    std::function<void(const std::shared_ptr<V4NativeDesktopExecutionKey>&)> observeBusiness;
};

class V4NativeMemberObserverIdentity final
{
    friend struct ::behavior::GeneratedNativeMemberObservation;
    V4NativeMemberObserverIdentity() = default;
public:
    V4NativeMemberObserverIdentity(const V4NativeMemberObserverIdentity&) = delete;
    V4NativeMemberObserverIdentity& operator=(const V4NativeMemberObserverIdentity&) = delete;
    ~V4NativeMemberObserverIdentity() = default;
};
class V4NativeMemberExecutionKey final
{
    friend class V4NativeSequence;
    friend class V4NativeRunReceipt;
    friend struct ::behavior::GeneratedNativeMemberObservation;
public:
    V4NativeMemberExecutionKey(const V4NativeMemberExecutionKey&) = delete;
    V4NativeMemberExecutionKey& operator=(const V4NativeMemberExecutionKey&) = delete;
private:
    enum class Phase { created, before_begin, dispatching, business, capturing, closed };
    V4NativeMemberExecutionKey(const std::shared_ptr<NativeHostEpoch>& epoch, std::string candidate, std::string graph)
        : epoch_(epoch), candidate_(std::move(candidate)), graph_(std::move(graph)), thread_(GetCurrentThreadId()) {}
    bool Same(const std::shared_ptr<NativeHostEpoch>& epoch, const void* app, std::string_view candidate) const noexcept
    {
        const auto actual = epoch_.lock();
        return actual && epoch && actual.get() == epoch.get() && !actual.owner_before(epoch) && !epoch.owner_before(actual) &&
            thread_ == GetCurrentThreadId() && actual->app_ == reinterpret_cast<std::uintptr_t>(app) &&
            candidate == candidate_ && !graph_.empty() && actual->Matches(actual->trace_, actual->desktop_, actual->control_);
    }
    bool BindBaseline(const std::shared_ptr<NativeHostEpoch>& epoch, const void* app, std::string_view candidate,
        const std::shared_ptr<const V4NativeMemberObserverIdentity>& observer) noexcept
    {
        if (phase_ != Phase::before_begin || bound_ || !observer || !Same(epoch, app, candidate)) return false;
        observer_ = observer; bound_ = true; return true;
    }
    bool BeginCapture(const std::shared_ptr<NativeHostEpoch>& epoch, const void* app, std::string_view candidate,
        const std::shared_ptr<const V4NativeMemberObserverIdentity>& observer) noexcept
    {
        const auto bound = observer_.lock();
        if (phase_ != Phase::business || !bound_ || published_ || !observer || !bound ||
            bound.get() != observer.get() || bound.owner_before(observer) || observer.owner_before(bound) ||
            !Same(epoch, app, candidate)) return false;
        phase_ = Phase::capturing; return true;
    }
    bool Publish(const std::shared_ptr<NativeHostEpoch>& epoch, const void* app, std::string_view candidate,
        const std::shared_ptr<const V4NativeMemberObserverIdentity>& observer,
        std::shared_ptr<const ::behavior::NativeMemberBusinessEvidence> value) noexcept
    {
        const auto bound = observer_.lock();
        if (!value || phase_ != Phase::capturing || !bound_ || published_ || !observer || !bound ||
            bound.get() != observer.get() || bound.owner_before(observer) || observer.owner_before(bound) ||
            !Same(epoch, app, candidate)) return false;
        snapshot_ = std::move(value); published_ = true; return true;
    }
    std::weak_ptr<NativeHostEpoch> epoch_;
    std::weak_ptr<const V4NativeMemberObserverIdentity> observer_;
    std::string candidate_, graph_;
    DWORD thread_ = 0;
    Phase phase_ = Phase::created;
    bool bound_ = false, published_ = false, committed_ = false;
    std::shared_ptr<const ::behavior::NativeMemberBusinessEvidence> snapshot_;
};
struct V4NativeMemberObserverBoundaries
{
    std::function<void(const std::shared_ptr<V4NativeMemberExecutionKey>&)> bindBeforeBegin;
    std::function<void(const std::shared_ptr<V4NativeMemberExecutionKey>&)> observeBusiness;
};

class V4NativeCategoryObserverIdentity final
{
    friend struct ::behavior::V4NativeCategoryObservation;
    V4NativeCategoryObserverIdentity() = default;
public:
    V4NativeCategoryObserverIdentity(const V4NativeCategoryObserverIdentity&) = delete;
    V4NativeCategoryObserverIdentity& operator=(const V4NativeCategoryObserverIdentity&) = delete;
    ~V4NativeCategoryObserverIdentity() = default;
};
class V4NativeCategoryExecutionKey final
{
    friend class V4NativeSequence;
    friend class V4NativeRunReceipt;
    friend struct ::behavior::V4NativeCategoryObservation;
public:
    V4NativeCategoryExecutionKey(const V4NativeCategoryExecutionKey&) = delete;
    V4NativeCategoryExecutionKey& operator=(const V4NativeCategoryExecutionKey&) = delete;
private:
    enum class Phase { created, before_begin, dispatching, business, capturing, closed };
    V4NativeCategoryExecutionKey(const std::shared_ptr<NativeHostEpoch>& epoch, std::string candidate, std::string graph)
        : epoch_(epoch), candidate_(std::move(candidate)), graph_(std::move(graph)), thread_(GetCurrentThreadId()) {}
    bool Same(const std::shared_ptr<NativeHostEpoch>& epoch, const void* app, std::string_view candidate) const noexcept
    {
        const auto actual = epoch_.lock();
        return actual && epoch && actual.get() == epoch.get() && !actual.owner_before(epoch) && !epoch.owner_before(actual) &&
            thread_ == GetCurrentThreadId() && actual->app_ == reinterpret_cast<std::uintptr_t>(app) &&
            candidate == candidate_ && !graph_.empty() && actual->Matches(actual->trace_, actual->desktop_, actual->control_);
    }
    bool BindBaseline(const std::shared_ptr<NativeHostEpoch>& epoch, const void* app, std::string_view candidate,
        const std::shared_ptr<const V4NativeCategoryObserverIdentity>& observer) noexcept
    {
        if (phase_ != Phase::before_begin || bound_ || !observer || !Same(epoch, app, candidate)) return false;
        observer_ = observer; bound_ = true; return true;
    }
    bool BeginCapture(const std::shared_ptr<NativeHostEpoch>& epoch, const void* app, std::string_view candidate,
        const std::shared_ptr<const V4NativeCategoryObserverIdentity>& observer) noexcept
    {
        const auto bound = observer_.lock();
        if (phase_ != Phase::business || !bound_ || published_ || !observer || !bound ||
            bound.get() != observer.get() || bound.owner_before(observer) || observer.owner_before(bound) ||
            !Same(epoch, app, candidate)) return false;
        phase_ = Phase::capturing; return true;
    }
    bool Publish(const std::shared_ptr<NativeHostEpoch>& epoch, const void* app, std::string_view candidate,
        const std::shared_ptr<const V4NativeCategoryObserverIdentity>& observer,
        std::shared_ptr<const ::behavior::V4NativeCategoryEvidence> value) noexcept
    {
        const auto bound = observer_.lock();
        if (!value || phase_ != Phase::capturing || !bound_ || published_ || !observer || !bound ||
            bound.get() != observer.get() || bound.owner_before(observer) || observer.owner_before(bound) ||
            !Same(epoch, app, candidate)) return false;
        snapshot_ = std::move(value); published_ = true; return true;
    }
    std::weak_ptr<NativeHostEpoch> epoch_;
    std::weak_ptr<const V4NativeCategoryObserverIdentity> observer_;
    std::string candidate_, graph_;
    DWORD thread_ = 0;
    Phase phase_ = Phase::created;
    bool bound_ = false, published_ = false, committed_ = false;
    std::shared_ptr<const ::behavior::V4NativeCategoryEvidence> snapshot_;
};
struct V4NativeCategoryObserverBoundaries
{
    std::function<void(const std::shared_ptr<V4NativeCategoryExecutionKey>&)> bindBeforeBegin;
    std::function<void(const std::shared_ptr<V4NativeCategoryExecutionKey>&)> observeBusiness;
};
class V4NativeReorderObserverIdentity final
{
    friend struct ::behavior::V4NativeCollectionReorderObservation;
    V4NativeReorderObserverIdentity() = default;
public:
    V4NativeReorderObserverIdentity(const V4NativeReorderObserverIdentity&) = delete;
    V4NativeReorderObserverIdentity& operator=(const V4NativeReorderObserverIdentity&) = delete;
    ~V4NativeReorderObserverIdentity() = default;
};
class V4NativeReorderExecutionKey final
{
    friend class V4NativeSequence;
    friend class V4NativeRunReceipt;
    friend struct ::behavior::V4NativeCollectionReorderObservation;
public:
    V4NativeReorderExecutionKey(const V4NativeReorderExecutionKey&) = delete;
    V4NativeReorderExecutionKey& operator=(const V4NativeReorderExecutionKey&) = delete;
private:
    enum class Phase { created, before_begin, dispatching, business, capturing, closed };
    V4NativeReorderExecutionKey(const std::shared_ptr<NativeHostEpoch>& epoch, std::string candidate, std::string graph)
        : epoch_(epoch), candidate_(std::move(candidate)), graph_(std::move(graph)), thread_(GetCurrentThreadId()) {}
    bool Same(const std::shared_ptr<NativeHostEpoch>& epoch, const void* app, std::string_view candidate) const noexcept
    {
        const auto actual = epoch_.lock();
        return actual && epoch && actual.get() == epoch.get() && !actual.owner_before(epoch) && !epoch.owner_before(actual) &&
            thread_ == GetCurrentThreadId() && actual->app_ == reinterpret_cast<std::uintptr_t>(app) &&
            candidate == candidate_ && !graph_.empty() && actual->Matches(actual->trace_, actual->desktop_, actual->control_);
    }
    bool BindBaseline(const std::shared_ptr<NativeHostEpoch>& epoch, const void* app, std::string_view candidate,
        const std::shared_ptr<const V4NativeReorderObserverIdentity>& observer) noexcept
    {
        if (phase_ != Phase::before_begin || bound_ || !observer || !Same(epoch, app, candidate)) return false;
        observer_ = observer; bound_ = true; return true;
    }
    bool BeginCapture(const std::shared_ptr<NativeHostEpoch>& epoch, const void* app, std::string_view candidate,
        const std::shared_ptr<const V4NativeReorderObserverIdentity>& observer) noexcept
    {
        const auto bound = observer_.lock();
        if (phase_ != Phase::business || !bound_ || published_ || !observer || !bound ||
            bound.get() != observer.get() || bound.owner_before(observer) || observer.owner_before(bound) ||
            !Same(epoch, app, candidate)) return false;
        phase_ = Phase::capturing; return true;
    }
    bool Publish(const std::shared_ptr<NativeHostEpoch>& epoch, const void* app, std::string_view candidate,
        const std::shared_ptr<const V4NativeReorderObserverIdentity>& observer,
        std::shared_ptr<const ::behavior::V4NativeCollectionReorderEvidence> value) noexcept
    {
        const auto bound = observer_.lock();
        if (!value || phase_ != Phase::capturing || !bound_ || published_ || !observer || !bound ||
            bound.get() != observer.get() || bound.owner_before(observer) || observer.owner_before(bound) ||
            !Same(epoch, app, candidate)) return false;
        snapshot_ = std::move(value); published_ = true; return true;
    }
    std::weak_ptr<NativeHostEpoch> epoch_;
    std::weak_ptr<const V4NativeReorderObserverIdentity> observer_;
    std::string candidate_, graph_;
    DWORD thread_ = 0;
    Phase phase_ = Phase::created;
    bool bound_ = false, published_ = false, committed_ = false;
    std::shared_ptr<const ::behavior::V4NativeCollectionReorderEvidence> snapshot_;
};
struct V4NativeReorderObserverBoundaries
{
    std::function<void(const std::shared_ptr<V4NativeReorderExecutionKey>&)> bindBeforeBegin;
    std::function<void(const std::shared_ptr<V4NativeReorderExecutionKey>&)> observeBusiness;
};
class V4NativeSelectionObserverIdentity final
{
    friend struct ::behavior::V4NativeMultiSelectionObservation;
    V4NativeSelectionObserverIdentity() = default;
public:
    V4NativeSelectionObserverIdentity(const V4NativeSelectionObserverIdentity&) = delete;
    V4NativeSelectionObserverIdentity& operator=(const V4NativeSelectionObserverIdentity&) = delete;
    ~V4NativeSelectionObserverIdentity() = default;
};
class V4NativeSelectionExecutionKey final
{
    friend class V4NativeSelectionPreparationExecutor;
    friend class V4NativeSelectionPreparationReceipt;
    friend struct ::behavior::V4NativeMultiSelectionObservation;
public:
    V4NativeSelectionExecutionKey(const V4NativeSelectionExecutionKey&) = delete;
    V4NativeSelectionExecutionKey& operator=(const V4NativeSelectionExecutionKey&) = delete;
private:
    enum class Phase { created, before_begin, dispatching, business, capturing, closed };
    V4NativeSelectionExecutionKey(const std::shared_ptr<NativeHostEpoch>& epoch, std::string candidate, std::string graph)
        : epoch_(epoch), candidate_(std::move(candidate)), graph_(std::move(graph)), thread_(GetCurrentThreadId()) {}
    bool Same(const std::shared_ptr<NativeHostEpoch>& epoch, const void* app, std::string_view candidate) const noexcept
    {
        const auto actual = epoch_.lock();
        return actual && epoch && actual.get() == epoch.get() && !actual.owner_before(epoch) && !epoch.owner_before(actual) &&
            thread_ == GetCurrentThreadId() && actual->app_ == reinterpret_cast<std::uintptr_t>(app) &&
            candidate == candidate_ && !graph_.empty() && actual->Matches(actual->trace_, actual->desktop_, actual->control_);
    }
    bool BindBaseline(const std::shared_ptr<NativeHostEpoch>& epoch, const void* app, std::string_view candidate,
        const std::shared_ptr<const V4NativeSelectionObserverIdentity>& observer) noexcept
    {
        if (phase_ != Phase::before_begin || bound_ || !observer || !Same(epoch, app, candidate)) return false;
        observer_ = observer; bound_ = true; return true;
    }
    bool BeginCapture(const std::shared_ptr<NativeHostEpoch>& epoch, const void* app, std::string_view candidate,
        const std::shared_ptr<const V4NativeSelectionObserverIdentity>& observer) noexcept
    {
        const auto bound = observer_.lock();
        if (phase_ != Phase::business || !bound_ || published_ || !observer || !bound ||
            bound.get() != observer.get() || bound.owner_before(observer) || observer.owner_before(bound) ||
            !Same(epoch, app, candidate)) return false;
        phase_ = Phase::capturing; return true;
    }
    bool Publish(const std::shared_ptr<NativeHostEpoch>& epoch, const void* app, std::string_view candidate,
        const std::shared_ptr<const V4NativeSelectionObserverIdentity>& observer,
        std::shared_ptr<const ::behavior::V4NativeMultiSelectionEvidence> value) noexcept
    {
        const auto bound = observer_.lock();
        if (!value || phase_ != Phase::capturing || !bound_ || published_ || !observer || !bound ||
            bound.get() != observer.get() || bound.owner_before(observer) || observer.owner_before(bound) ||
            !Same(epoch, app, candidate)) return false;
        snapshot_ = std::move(value); published_ = true; return true;
    }
    std::weak_ptr<NativeHostEpoch> epoch_;
    std::weak_ptr<const V4NativeSelectionObserverIdentity> observer_;
    std::string candidate_, graph_;
    DWORD thread_ = 0;
    Phase phase_ = Phase::created;
    bool bound_ = false, published_ = false, committed_ = false;
    std::shared_ptr<const ::behavior::V4NativeMultiSelectionEvidence> snapshot_;
};
struct V4NativeSelectionObserverBoundaries
{
    std::function<void(const std::shared_ptr<V4NativeSelectionExecutionKey>&)> bindBeforeBegin;
    std::function<void(const std::shared_ptr<V4NativeSelectionExecutionKey>&)> observeBusiness;
};

struct V4NativeSelectionPreparationPlan
{
    struct Action
    {
        std::size_t programIndex = 0;
        std::string kind, session, button, modifiers, member, x, y;
        std::optional<POINT> screen;
        std::optional<DWORD> modifierMask;
    };
    bool identityValid = false, inputsMaterialized = false;
    bool resourceUnavailable = false;
    std::string candidateId, graphId;
    std::vector<Action> actions;
    std::vector<std::string> preconditions, graphUnresolved;
};

// A bounded, value-only preparation plan. Session numbers and End are taken
// from the authority, never inferred from a button-up or selected graph bit.
// This does not invoke Device or authorize production input.
inline V4NativeSelectionPreparationPlan BuildV4NativeSelectionPreparationPlan(
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const event_graph::V4NativeJoinedGraphVariant& selected,
    std::size_t actionBudget)
{
    V4NativeSelectionPreparationPlan out;
    try
    {
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4NativeJoinedGraph::Replay(identity, verified, present, selected.id);
        if (raw.resourceUnavailable || replay.status == event_graph::V4GraphReplayStatus::resource_unavailable)
            out.resourceUnavailable = true;
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || !replay.identityValid ||
            !replay.errors.empty() || replay.variant.id != selected.id || replay.variant.classes != selected.classes ||
            replay.variant.order != selected.order || replay.variant.unresolved != selected.unresolved)
        { out.preconditions.emplace_back("v4_selection_plan_candidate_or_graph_unavailable"); return out; }
        out.identityValid = true; out.candidateId = raw.id; out.graphId = selected.id; out.graphUnresolved = selected.unresolved;
        const auto program = verified.TransferProgram();
        const auto& axes = verified.Axes(); const auto& candidate = verified.Coordinate();
        const auto raw32 = [](std::string_view text) -> std::optional<std::uint32_t> {
            if (text.size() != 14 || !text.starts_with("exact_")) return std::nullopt;
            std::uint32_t value = 0;
            for (const char ch : text.substr(6))
            {
                const unsigned digit = ch >= '0' && ch <= '9' ? static_cast<unsigned>(ch - '0') :
                    ch >= 'a' && ch <= 'f' ? static_cast<unsigned>(ch - 'a' + 10) : 16u;
                if (digit == 16u) return std::nullopt;
                value = (value << 4) | digit;
            }
            return value;
        };
        const auto coordinate = [](std::uint32_t value) noexcept -> LONG {
            const auto signedValue = value <= 0x7fffffffu ? static_cast<std::int64_t>(value) :
                static_cast<std::int64_t>(value) - 0x100000000ll;
            return static_cast<LONG>(signedValue);
        };
        std::set<std::string> ended;
        std::string active;
        for (std::size_t i = 0; i < program.steps.size(); ++i)
        {
            const auto& step = program.steps[i];
            if (!std::string_view(step.kind).starts_with("selection_prepare_")) continue;
            if (out.actions.size() >= actionBudget)
            { out.resourceUnavailable = true; out.preconditions.emplace_back("v4_selection_plan_action_budget_unavailable"); return out; }
            const auto separator = step.parameter.find(';');
            const auto base = step.parameter.substr(0, separator);
            if (separator == std::string::npos || !std::string_view(base).starts_with("V4.selection_prepare.step_"))
            { out.preconditions.emplace_back("v4_selection_plan_step_identity_unavailable"); return out; }
            V4NativeSelectionPreparationPlan::Action action{i, step.kind, std::string(Value(axes, candidate, base + ".session")),
                std::string(Value(axes, candidate, base + ".button")), std::string(Value(axes, candidate, base + ".modifiers")),
                std::string(Value(axes, candidate, base + ".member")), std::string(Value(axes, candidate, base + ".x")),
                std::string(Value(axes, candidate, base + ".y"))};
            std::size_t ordinal = 0;
            if (!TransferExactSourceCountV3(action.session, ordinal) || ended.contains(action.session) ||
                (!active.empty() && active != action.session))
            { out.preconditions.emplace_back("v4_selection_plan_session_order_unavailable"); return out; }
            if (step.kind == "selection_prepare_session_end")
            {
                if (active.empty()) { out.preconditions.emplace_back("v4_selection_plan_end_without_session"); return out; }
                ended.insert(action.session); active.clear();
            }
            else if (step.kind == "selection_prepare_pointer_down" || step.kind == "selection_prepare_pointer_move" ||
                step.kind == "selection_prepare_pointer_up")
            {
                if (active.empty()) active = action.session;
                if (action.button == "unknown_unresolved" || action.button.empty() ||
                    !std::string_view(action.modifiers).starts_with("exact_") ||
                    !std::string_view(action.x).starts_with("exact_") || !std::string_view(action.y).starts_with("exact_"))
                { out.preconditions.emplace_back("v4_selection_plan_pointer_input_unavailable"); return out; }
                const auto modifiers = raw32(action.modifiers), x = raw32(action.x), y = raw32(action.y);
                if (!modifiers || !x || !y)
                { out.preconditions.emplace_back("v4_selection_plan_pointer_encoding_unavailable"); return out; }
                action.modifierMask = static_cast<DWORD>(*modifiers);
                action.screen = POINT{coordinate(*x), coordinate(*y)};
            }
            else { out.preconditions.emplace_back("v4_selection_plan_action_unavailable"); return out; }
            out.actions.push_back(std::move(action));
        }
        if (!active.empty()) { out.preconditions.emplace_back("v4_selection_plan_session_not_ended"); return out; }
        if (out.actions.empty()) { out.preconditions.emplace_back("v4_selection_plan_no_preparation_actions"); return out; }
        out.inputsMaterialized = true;
        out.preconditions.emplace_back("v4_selection_plan_device_session_graph_and_same_host_capability_unimplemented");
        out.preconditions.emplace_back("v4_selection_plan_baseline_selection_capture_and_single_use_unimplemented");
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; }
    catch (const std::length_error&) { out.resourceUnavailable = true; }
    catch (...) { out.preconditions.emplace_back("v4_selection_plan_observation_unavailable"); }
    return out;
}

// Each session keeps its own copied Device log and trace. Device::Begin clears
// its live log, so a later drag cannot use the live buffer as preparation
// evidence. Only the preparation executor can construct or seal this receipt.
struct V4NativeSelectionSessionRecord
{
    std::string ordinal;
    std::vector<V4NativeQueuedRecord> queued;
    std::vector<V4NativeDeviceRecord> device;
    std::vector<V4NativeTraceRecord> trace;
    std::vector<snowdesktop::private_pointer::State> beforeEndStates;
    std::vector<snowdesktop::private_pointer::DeliveryObservationLog> beforeEndDelivery;
    std::vector<NativeDeviceCallRecord> calls;
    bool callJournalLost = false;
};
class V4NativeSelectionPreparationReceipt final
{
    friend class V4NativeSelectionPreparationExecutor;
    friend class V4NativeSequence;
    friend struct ::behavior::V4NativeMultiSelectionObservation;
public:
    V4NativeSelectionPreparationReceipt(const V4NativeSelectionPreparationReceipt&) = delete;
    V4NativeSelectionPreparationReceipt& operator=(const V4NativeSelectionPreparationReceipt&) = delete;
    const std::vector<V4NativeSelectionSessionRecord>& Sessions() const noexcept { return sessions_; }
    std::string_view CandidateId() const noexcept { return candidate_; }
    std::string_view GraphId() const noexcept { return graph_; }
    std::uintptr_t HostIdentity() const noexcept { return hostIdentity_; }
    const Result& Execution() const noexcept { return execution_; }
    bool ProductionStarted() const noexcept { return productionStarted_; }
    bool ProductionEntryUnknown() const noexcept { return productionEntryUnknown_; }
    bool Consumed() const noexcept { return consumed_; }
    const std::vector<std::string>& Unresolved() const noexcept { return unresolved_; }
    const std::vector<std::string>& GraphUnresolved() const noexcept { return graphUnresolved_; }
    std::shared_ptr<const ::behavior::V4NativeMultiSelectionEvidence> SealedSelection() const noexcept
    {
        return productionStarted_ && key_ && key_->committed_ && key_->published_ &&
            key_->phase_ == V4NativeSelectionExecutionKey::Phase::closed ? key_->snapshot_ : nullptr;
    }
    // A scoped value from the actual post-End observer. It is not a graph
    // certificate and cannot be consumed to authorize a later drag.
    std::shared_ptr<const ::behavior::V4NativeMultiSelectionEvidence> LocalSelectionSnapshot(
        const std::shared_ptr<NativeHostEpoch>& epoch, const MessageTrace& trace, HWND desktop, HWND control) const noexcept
    {
        return productionStarted_ && !productionEntryUnknown_ && key_ && key_->published_ &&
            key_->phase_ == V4NativeSelectionExecutionKey::Phase::closed && SameHost(epoch, trace, desktop, control) &&
            key_->Same(epoch, reinterpret_cast<const void*>(epoch->app_), candidate_)
            ? key_->snapshot_ : nullptr;
    }
    bool SameCandidate(const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
        const std::vector<bool>& present, const event_graph::V4NativeJoinedGraphVariant& selected) const noexcept
    {
        try
        {
            if (candidate_.empty() || graph_ != selected.id) return false;
            const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
            const auto replay = event_graph::V4NativeJoinedGraph::Replay(identity, verified, present, selected.id);
            return !raw.resourceUnavailable && raw.errors.empty() && raw.id == candidate_ && replay.identityValid &&
                replay.errors.empty() && replay.variant.id == selected.id && replay.variant.classes == selected.classes &&
                replay.variant.order == selected.order && replay.variant.unresolved == selected.unresolved;
        }
        catch (...) { return false; }
    }
    bool SameHost(const std::shared_ptr<NativeHostEpoch>& epoch, const MessageTrace& trace,
        HWND desktop, HWND control) const noexcept
    {
        const auto original = epoch_.lock();
        return productionStarted_ && original && epoch && original.get() == epoch.get() &&
            !original.owner_before(epoch) && !epoch.owner_before(original) && original->Matches(&trace, desktop, control);
    }
private:
    V4NativeSelectionPreparationReceipt() = default;
    // The trusted observer calls this only after rereading the actual model
    // against its own sealed snapshot, while the same host execution lease is
    // held. Consumption is irreversible even if the following dispatch fails.
    bool ConsumeAfterRevalidation(const std::shared_ptr<NativeHostEpoch>& epoch, const void* app,
        const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
        const std::vector<bool>& present, const event_graph::V4NativeJoinedGraphVariant& selected,
        const MessageTrace& trace, HWND desktop, HWND control,
        const std::shared_ptr<const V4NativeSelectionObserverIdentity>& observer) noexcept
    {
        if (consumed_ || productionEntryUnknown_ || !SealedSelection() || !key_ || !observer ||
            !SameCandidate(identity, verified, present, selected) || !SameHost(epoch, trace, desktop, control) ||
            !epoch->executing_.load(std::memory_order_acquire) || !key_->Same(epoch, app, candidate_)) return false;
        const auto bound = key_->observer_.lock();
        if (!bound || bound.get() != observer.get() || bound.owner_before(observer) || observer.owner_before(bound)) return false;
        consumed_ = true;
        return true;
    }
    std::weak_ptr<NativeHostEpoch> epoch_;
    Result execution_;
    std::shared_ptr<V4NativeSelectionExecutionKey> key_;
    std::string candidate_, graph_;
    std::uintptr_t hostIdentity_ = 0;
    std::vector<V4NativeSelectionSessionRecord> sessions_;
    std::vector<std::string> unresolved_, graphUnresolved_;
    bool productionStarted_ = false, productionEntryUnknown_ = false, consumed_ = false;
};

class V4NativeMultiDragObserverIdentity final
{
    friend struct ::behavior::V4NativeMultiDragObservation;
    V4NativeMultiDragObserverIdentity() = default;
public:
    V4NativeMultiDragObserverIdentity(const V4NativeMultiDragObserverIdentity&) = delete;
    V4NativeMultiDragObserverIdentity& operator=(const V4NativeMultiDragObserverIdentity&) = delete;
    ~V4NativeMultiDragObserverIdentity() = default;
};
class V4NativeMultiDragExecutionKey final
{
    friend class V4NativeSequence;
    friend class V4NativeRunReceipt;
    friend struct ::behavior::V4NativeMultiDragObservation;
public:
    V4NativeMultiDragExecutionKey(const V4NativeMultiDragExecutionKey&) = delete;
    V4NativeMultiDragExecutionKey& operator=(const V4NativeMultiDragExecutionKey&) = delete;
private:
    enum class Phase { created, before_begin, dispatching, business, capturing, closed };
    V4NativeMultiDragExecutionKey(const std::shared_ptr<NativeHostEpoch>& epoch,
        const std::shared_ptr<V4NativeSelectionPreparationReceipt>& preparation, std::string candidate, std::string graph)
        : epoch_(epoch), preparation_(preparation), candidate_(std::move(candidate)), graph_(std::move(graph)),
          thread_(GetCurrentThreadId()) {}
    bool Same(const std::shared_ptr<NativeHostEpoch>& epoch, const void* app, std::string_view candidate) const noexcept
    {
        const auto actual = epoch_.lock();
        const auto preparation = preparation_.lock();
        return actual && epoch && actual.get() == epoch.get() && !actual.owner_before(epoch) && !epoch.owner_before(actual) &&
            preparation && preparation->Consumed() && preparation->CandidateId() == candidate_ && preparation->GraphId() == graph_ &&
            thread_ == GetCurrentThreadId() && actual->app_ == reinterpret_cast<std::uintptr_t>(app) &&
            candidate == candidate_ && !graph_.empty() && actual->Matches(actual->trace_, actual->desktop_, actual->control_);
    }
    bool BindBaseline(const std::shared_ptr<NativeHostEpoch>& epoch, const void* app, std::string_view candidate,
        const std::shared_ptr<const V4NativeMultiDragObserverIdentity>& observer) noexcept
    {
        if (phase_ != Phase::before_begin || bound_ || !observer || !Same(epoch, app, candidate)) return false;
        observer_ = observer; bound_ = true; return true;
    }
    bool BeginCapture(const std::shared_ptr<NativeHostEpoch>& epoch, const void* app, std::string_view candidate,
        const std::shared_ptr<const V4NativeMultiDragObserverIdentity>& observer) noexcept
    {
        const auto bound = observer_.lock();
        if (phase_ != Phase::business || !bound_ || published_ || !observer || !bound ||
            bound.get() != observer.get() || bound.owner_before(observer) || observer.owner_before(bound) ||
            !Same(epoch, app, candidate)) return false;
        phase_ = Phase::capturing; return true;
    }
    bool Publish(const std::shared_ptr<NativeHostEpoch>& epoch, const void* app, std::string_view candidate,
        const std::shared_ptr<const V4NativeMultiDragObserverIdentity>& observer,
        std::shared_ptr<const ::behavior::V4NativeMultiDragEvidence> value) noexcept
    {
        const auto bound = observer_.lock();
        if (!value || phase_ != Phase::capturing || !bound_ || published_ || !observer || !bound ||
            bound.get() != observer.get() || bound.owner_before(observer) || observer.owner_before(bound) ||
            !Same(epoch, app, candidate)) return false;
        snapshot_ = std::move(value); published_ = true; return true;
    }
    std::weak_ptr<NativeHostEpoch> epoch_;
    std::weak_ptr<V4NativeSelectionPreparationReceipt> preparation_;
    std::weak_ptr<const V4NativeMultiDragObserverIdentity> observer_;
    std::string candidate_, graph_;
    DWORD thread_ = 0;
    Phase phase_ = Phase::created;
    bool bound_ = false, published_ = false, committed_ = false;
    std::shared_ptr<const ::behavior::V4NativeMultiDragEvidence> snapshot_;
};
struct V4NativeMultiDragObserverBoundaries
{
    std::function<bool()> consumePreparation;
    std::function<void(const std::shared_ptr<V4NativeMultiDragExecutionKey>&)> bindBeforeBegin;
    std::function<void(const std::shared_ptr<V4NativeMultiDragExecutionKey>&)> observeBusiness;
};

// The device path below is limited to one explicit left-button marquee
// session. The unconditional capability obligations remain in force until
// graph occurrence attribution and the observer boundary are established.
class V4NativeSelectionPreparationExecutor final
{
public:
    static std::shared_ptr<V4NativeSelectionPreparationReceipt> RunBound(
        const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
        const std::vector<bool>& present, const event_graph::V4NativeJoinedGraphVariant& selected,
        HWND desktop, HWND control, RECT bounds, const MessageTrace& trace,
        const std::shared_ptr<NativeHostEpoch>& epoch, const NativeHostBoundaries& host,
        const V4NativeSelectionObserverBoundaries& observer, std::size_t actionBudget)
    {
        const auto pinnedEpoch = epoch;
        auto receipt = std::shared_ptr<V4NativeSelectionPreparationReceipt>(new V4NativeSelectionPreparationReceipt);
        bool dispatchEntered = false;
        try
        {
            const auto pinnedHost = host;
            const auto pinnedObserver = observer;
            const auto plan = BuildV4NativeSelectionPreparationPlan(identity, verified, present, selected, actionBudget);
            receipt->candidate_ = plan.candidateId; receipt->graph_ = plan.graphId; receipt->epoch_ = pinnedEpoch;
            if (pinnedEpoch && pinnedEpoch->Matches(&trace, desktop, control)) receipt->hostIdentity_ = pinnedEpoch->app_;
            receipt->unresolved_ = plan.preconditions;
            receipt->graphUnresolved_ = plan.graphUnresolved;
            if (plan.resourceUnavailable) receipt->execution_.additionalObservationUnavailable = true;
            if (!plan.identityValid || !plan.inputsMaterialized || !pinnedEpoch || !pinnedEpoch->Matches(&trace, desktop, control) ||
                !pinnedObserver.bindBeforeBegin || !pinnedObserver.observeBusiness || !pinnedHost.beforeDeviceBegin || !pinnedHost.observeDeviceState ||
                !pinnedHost.beforeQueuedDelivery || !pinnedHost.productionInputEntered || !pinnedHost.beforePointerSessionEnd || !pinnedHost.throwAsync)
            { receipt->unresolved_.emplace_back("v4_selection_executor_plan_or_host_unavailable"); return receipt; }
            const auto program = verified.TransferProgram();
            if (Value(verified.Axes(), verified.Coordinate(), "V4.selection_prepare.mode") != "marquee")
            { receipt->unresolved_.emplace_back("v4_selection_executor_mode_unimplemented"); return receipt; }
            std::vector<NativeGraphAction> actions;
            std::vector<std::optional<POINT>> points(program.steps.size());
            std::string session;
            std::optional<POINT> first, current;
            bool down = false, moved = false, up = false, ended = false;
            for (const auto& action : plan.actions)
            {
                if (session.empty()) session = action.session;
                if (action.session != session || action.programIndex >= points.size() || ended)
                { receipt->unresolved_.emplace_back("v4_selection_executor_multiple_sessions_unimplemented"); return receipt; }
                if (action.kind == "selection_prepare_session_end")
                {
                    if (!up) { receipt->unresolved_.emplace_back("v4_selection_executor_end_order_unavailable"); return receipt; }
                    actions.push_back({NativeGraphActionKind::end, {action.programIndex}}); ended = true; continue;
                }
                if (!action.screen || !action.modifierMask || *action.modifierMask != 0 || !PtInRect(&bounds, *action.screen))
                { receipt->unresolved_.emplace_back("v4_selection_executor_input_unavailable"); return receipt; }
                points[action.programIndex] = *action.screen;
                if (action.kind == "selection_prepare_pointer_down" && !down && !moved && !up && action.button == "left")
                {
                    first = current = *action.screen; down = true;
                    actions.push_back({NativeGraphActionKind::down, {action.programIndex}});
                }
                else if (action.kind == "selection_prepare_pointer_move" && down && !up && action.button == "none")
                {
                    current = *action.screen; moved = true;
                    actions.push_back({NativeGraphActionKind::move, {action.programIndex}});
                }
                else if (action.kind == "selection_prepare_pointer_up" && down && moved && !up && action.button == "left" &&
                    current && current->x == action.screen->x && current->y == action.screen->y)
                { up = true; actions.push_back({NativeGraphActionKind::up, {action.programIndex}}); }
                else { receipt->unresolved_.emplace_back("v4_selection_executor_action_sequence_unavailable"); return receipt; }
            }
            if (!first || !down || !moved || !up || !ended)
            { receipt->unresolved_.emplace_back("v4_selection_executor_incomplete_session"); return receipt; }
            // No graph presence or selection outcome is used as an input.
            // These obligations intentionally prevent any dispatch today.
            if (!receipt->unresolved_.empty()) return receipt;
            if (!pinnedEpoch->TryBeginExecution(&trace, desktop, control))
            { receipt->execution_.additionalObservationUnavailable = true; return receipt; }
            const auto endEpoch = [](const NativeHostEpoch* active) noexcept { active->EndExecution(); };
            const std::unique_ptr<const NativeHostEpoch, decltype(endEpoch)> guard(pinnedEpoch.get(), endEpoch);
            receipt->key_ = std::shared_ptr<V4NativeSelectionExecutionKey>(
                new V4NativeSelectionExecutionKey(pinnedEpoch, receipt->candidate_, receipt->graph_));
            receipt->sessions_.push_back({session, {}, {}, {}, {}, {}});
            auto& observed = receipt->sessions_.back();
            if (actions.size() > ((std::numeric_limits<std::size_t>::max)() - 2) / 2)
                throw std::length_error("v4_selection_call_journal_capacity");
            observed.calls.reserve(2 + actions.size() * 2);
            NativeDeviceCallJournal callJournal{&trace, &observed.calls};
            const auto snapshot = [&](const char* name) {
                if (!pinnedEpoch->Matches(&trace, desktop, control) || trace.LostOrCrossThread())
                    throw ObservationUnavailable("v4_selection_executor_host_or_trace_changed");
                observed.trace.push_back({name, trace.Observations()});
            };
            NativeHostBoundaries bound = pinnedHost;
            bool selectionBeforeBeginEntered = false, selectionBeforeBeginCompleted = false;
            bound.beforeDeviceBegin = [&] {
                selectionBeforeBeginEntered = true;
                pinnedHost.beforeDeviceBegin(); snapshot("selection_before_begin");
                auto& key = *receipt->key_; key.phase_ = V4NativeSelectionExecutionKey::Phase::before_begin;
                pinnedObserver.bindBeforeBegin(receipt->key_);
                if (!key.bound_) throw ObservationUnavailable("v4_selection_executor_baseline_unavailable");
                snapshot("selection_baseline_bound"); key.phase_ = V4NativeSelectionExecutionKey::Phase::dispatching;
                selectionBeforeBeginCompleted = true;
            };
            bound.beforeQueuedDelivery = [&](NativeDevicePhase phase, std::optional<std::size_t> index,
                const snowdesktop::private_pointer::QueuedEventIdentity& token) {
                snapshot("selection_queue_before_dispatch"); observed.queued.push_back({phase, index, token});
                pinnedHost.beforeQueuedDelivery(phase, index, token);
                snapshot("selection_queue_host_returned");
            };
            bound.observeDeviceState = [&](NativeDevicePhase phase, std::optional<std::size_t> index,
                const snowdesktop::private_pointer::State& state, const snowdesktop::private_pointer::DeliveryObservationLog& log,
                std::optional<snowdesktop::private_pointer::QueuedEventIdentity> queued,
                std::optional<NativeInputExpectation> expected) {
                snapshot("selection_device_observed");
                observed.device.push_back({phase, index, state, log, queued, expected, trace.Observations()});
                pinnedHost.observeDeviceState(phase, index, state, log, queued, expected);
                snapshot("selection_device_host_returned");
            };
            bound.beforePointerSessionEnd = [&](const EventStep& step, const snowdesktop::private_pointer::State& state,
                const snowdesktop::private_pointer::DeliveryObservationLog& log) {
                snapshot("selection_before_end"); observed.beforeEndStates.push_back(state);
                observed.beforeEndDelivery.push_back(log); pinnedHost.beforePointerSessionEnd(step, state, log);
                snapshot("selection_before_end_host_returned");
            };
            dispatchEntered = true;
            receipt->execution_ = NativeSequence::DispatchPrepared({}, program, actions, points, *first, desktop, bounds, bound, &callJournal);
            observed.callJournalLost = callJournal.lost;
            dispatchEntered = false;
            if (selectionBeforeBeginEntered && !selectionBeforeBeginCompleted && !receipt->execution_.productionStarted)
                receipt->execution_.productionEntryUnknown = true;
            receipt->productionStarted_ = receipt->execution_.productionStarted;
            receipt->productionEntryUnknown_ = receipt->execution_.productionEntryUnknown;
            if (receipt->productionStarted_ && !receipt->productionEntryUnknown_ && !observed.device.empty() &&
                observed.device.back().phase == NativeDevicePhase::session_end && receipt->execution_.status == Status::attempted)
            {
                auto& key = *receipt->key_; key.phase_ = V4NativeSelectionExecutionKey::Phase::business;
                struct CloseSelection final { V4NativeSelectionExecutionKey& key; ~CloseSelection() { key.phase_ = V4NativeSelectionExecutionKey::Phase::closed; } } close{key};
                pinnedObserver.observeBusiness(receipt->key_);
                snapshot("selection_observer_returned");
                // Graph attribution is not yet implemented: deliberately do
                // not set committed_ or expose the snapshot as sealed.
                receipt->unresolved_.emplace_back("v4_selection_executor_graph_occurrences_unverified");
            }
        }
        catch (const std::bad_alloc&) { receipt->execution_.additionalObservationUnavailable = true; receipt->productionEntryUnknown_ = dispatchEntered; }
        catch (const std::length_error&) { receipt->execution_.additionalObservationUnavailable = true; receipt->productionEntryUnknown_ = dispatchEntered; }
        catch (...) { receipt->execution_.additionalObservationUnavailable = true; receipt->productionEntryUnknown_ = dispatchEntered; }
        return receipt;
    }
};

struct V4NativePreparationProgramObservation
{
    struct Session
    {
        std::string ordinal;
        bool beginReturnedSuccess = false, initialHoverObserved = false;
        std::optional<std::size_t> beginStartIndex, beginReturnIndex, hoverTraceIndex;
        std::optional<std::uint64_t> hoverEnteredSequence, hoverReturnedSequence;
        std::optional<bool> selectedHoverEnterPresent, selectedHoverReturnPresent;
    };
    struct Step
    {
        std::size_t programIndex = 0;
        std::string kind, session;
        bool wndprocObserved = false;
        bool callReturnedNormally = false, callReturnedSuccess = false, endDeviceObserved = false;
        std::optional<std::size_t> callStartIndex, callReturnIndex;
        std::optional<std::size_t> traceIndex;
        std::optional<std::uint64_t> enteredSequence, returnedSequence;
        std::optional<bool> selectedEnterPresent, selectedReturnPresent;
    };
    bool identityValid = false, resourceUnavailable = false, observationUnavailable = true;
    bool hypothesisObservationRecorded = false, hypothesisNotMetObserved = false;
    bool localSelectionSnapshotObserved = false;
    std::vector<Session> sessions;
    std::vector<Step> steps;
    std::vector<std::string> unresolved;
};

// The private receipt supplies the actual per-session Device snapshots. This
// only attributes positive WndProc deliveries to original Program indices;
// call boundaries, selected presence/order, absence and session causality
// are separate obligations and this observation is never a skip capability.
inline V4NativePreparationProgramObservation InspectV4NativePreparationProgram(
    const V4NativeSelectionPreparationReceipt& receipt,
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const event_graph::V4NativeJoinedGraphVariant& selected,
    const std::shared_ptr<NativeHostEpoch>& epoch, const MessageTrace& trace,
    HWND desktop, HWND control, std::size_t actionBudget)
{
    using namespace snowdesktop::private_pointer;
    V4NativePreparationProgramObservation out;
    try
    {
        const auto plan = BuildV4NativeSelectionPreparationPlan(identity, verified, present, selected, actionBudget);
        out.resourceUnavailable = plan.resourceUnavailable;
        if (!plan.identityValid || !plan.inputsMaterialized || !receipt.SameCandidate(identity, verified, present, selected) ||
            !receipt.SameHost(epoch, trace, desktop, control) || receipt.ProductionEntryUnknown() || trace.LostOrCrossThread())
        { out.unresolved.emplace_back("v4_preparation_program_identity_or_host_unavailable"); return out; }
        const auto mapping = event_graph::MapV4NativePresenceCandidate(identity, verified);
        auto requirements = mapping.spec.requirements;
        std::sort(requirements.begin(), requirements.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        if (!mapping.graphAvailable || requirements.size() != present.size())
        { out.unresolved.emplace_back("v4_preparation_program_graph_requirements_unavailable"); return out; }
        std::map<std::string, bool> selectedPresence;
        for (std::size_t i = 0; i < requirements.size(); ++i)
            if (!selectedPresence.emplace(requirements[i].id, present[i]).second)
            { out.unresolved.emplace_back("v4_preparation_program_duplicate_graph_node"); return out; }
        out.identityValid = true;
        out.localSelectionSnapshotObserved = static_cast<bool>(receipt.LocalSelectionSnapshot(epoch, trace, desktop, control));
        std::set<std::string> sessionOrdinals;
        for (const auto& session : receipt.Sessions())
        {
            V4NativePreparationProgramObservation::Session item; item.ordinal = session.ordinal;
            if (!sessionOrdinals.insert(session.ordinal).second)
            { out.unresolved.emplace_back("v4_preparation_duplicate_session_unavailable"); continue; }
            const auto first = std::find_if(plan.actions.begin(), plan.actions.end(), [&](const auto& action) {
                return action.session == session.ordinal && action.screen.has_value(); });
            std::optional<std::size_t> beginStart, beginReturn;
            bool ambiguous = session.callJournalLost;
            for (std::size_t n = 0; n < session.calls.size(); ++n)
            {
                const auto& call = session.calls[n];
                if (call.kind != NativeDeviceCallKind::begin || call.programIndex) continue;
                auto& slot = call.returned ? beginReturn : beginStart;
                if (slot) { ambiguous = true; break; } slot = n;
            }
            if (!ambiguous && beginStart && beginReturn && *beginStart < *beginReturn &&
                session.calls[*beginStart].thread == GetCurrentThreadId() && session.calls[*beginReturn].thread == GetCurrentThreadId() &&
                !session.calls[*beginStart].succeeded && session.calls[*beginReturn].succeeded == true &&
                session.calls[*beginStart].traceSize <= session.calls[*beginReturn].traceSize &&
                session.calls[*beginReturn].traceSize <= trace.Observations().size())
            { item.beginReturnedSuccess = true; item.beginStartIndex = *beginStart; item.beginReturnIndex = *beginReturn; }
            const V4NativeDeviceRecord* device = nullptr;
            const V4NativeQueuedRecord* queue = nullptr;
            for (const auto& value : session.device)
                if (value.phase == NativeDevicePhase::initial_hover && !value.programIndex)
                { if (device) { ambiguous = true; break; } device = &value; }
            for (const auto& value : session.queued)
                if (value.phase == NativeDevicePhase::initial_hover && !value.programIndex)
                { if (queue) { ambiguous = true; break; } queue = &value; }
            if (!ambiguous && first != plan.actions.end() && device && queue && device->queued && device->expected &&
                device->expected->message == WM_MOUSEMOVE && device->expected->buttons == 0 &&
                device->expected->screen.x == first->screen->x && device->expected->screen.y == first->screen->y &&
                device->queued->session == queue->token.session && device->queued->event == queue->token.event &&
                device->queued->ownerThread == queue->token.ownerThread)
            {
                std::optional<DeliveryIdentity> delivery;
                for (const auto& entry : device->delivery.entries)
                    if (entry.kind == DeliveryObservationKind::begin && entry.identity.queuedEvent &&
                        !entry.identity.forwarded && !entry.identity.forward && entry.identity.session == queue->token.session &&
                        entry.identity.event == queue->token.event && entry.identity.ownerThread == queue->token.ownerThread)
                    { if (delivery || !entry.identity.delivery) { ambiguous = true; break; } delivery = entry.identity; }
                POINT point = device->expected->screen;
                if (!ambiguous && delivery && ScreenToClient(desktop, &point) &&
                    point.x == device->expected->clientBeforeDispatch.x && point.y == device->expected->clientBeforeDispatch.y)
                {
                    const auto matched = InspectNativeDelivery(trace, 0, &device->delivery, *delivery, desktop,
                        MessageRoute::desktop_window_proc, WM_MOUSEMOVE, 0,
                        MAKELPARAM(static_cast<short>(point.x), static_cast<short>(point.y)));
                    if (matched.matchedMessageAndPermit && matched.traceIndex < device->trace.size() &&
                        matched.traceIndex < trace.Observations().size())
                    {
                        const auto& saved = device->trace[matched.traceIndex]; const auto& current = trace.Observations()[matched.traceIndex];
                        if (saved.enteredSequence == current.enteredSequence && saved.returnedSequence == current.returnedSequence &&
                            saved.windowScope == current.windowScope && saved.route == current.route &&
                            saved.delivery.session == current.delivery.session && saved.delivery.event == current.delivery.event &&
                            saved.delivery.delivery == current.delivery.delivery && saved.delivery.forward == current.delivery.forward &&
                            saved.delivery.forwarded == current.delivery.forwarded && saved.delivery.queuedEvent == current.delivery.queuedEvent &&
                            saved.delivery.ownerThread == current.delivery.ownerThread && saved.window == current.window &&
                            saved.message == current.message && saved.wParam == current.wParam && saved.inputLParam == current.inputLParam &&
                            saved.returned && current.returned && saved.windowResult == current.windowResult &&
                            saved.enteredThread == queue->token.ownerThread && saved.returnedThread == queue->token.ownerThread)
                        { item.initialHoverObserved = true; item.hoverTraceIndex = matched.traceIndex;
                          item.hoverEnteredSequence = saved.enteredSequence; item.hoverReturnedSequence = saved.returnedSequence; }
                    }
                }
            }
            out.sessions.push_back(std::move(item));
            auto& recorded = out.sessions.back();
            if (recorded.initialHoverObserved)
            {
                const auto base = "v4_native_selection_session_" + session.ordinal + "_initial_hover_wndproc_";
                const auto enter = selectedPresence.find(base + "enter"), returned = selectedPresence.find(base + "return");
                if (enter != selectedPresence.end() && returned != selectedPresence.end())
                {
                    recorded.selectedHoverEnterPresent = enter->second; recorded.selectedHoverReturnPresent = returned->second;
                    out.hypothesisObservationRecorded = true;
                    if (!enter->second || !returned->second) out.hypothesisNotMetObserved = true;
                }
                else out.unresolved.emplace_back("v4_preparation_initial_hover_graph_node_unavailable");
            }
        }
        for (const auto& action : plan.actions)
        {
            V4NativePreparationProgramObservation::Step item;
            item.programIndex = action.programIndex; item.kind = action.kind; item.session = action.session;
            const V4NativeSelectionSessionRecord* session = nullptr;
            for (const auto& candidate : receipt.Sessions())
                if (candidate.ordinal == action.session)
                { if (session) { session = nullptr; break; } session = &candidate; }
            if (!session) { out.steps.push_back(std::move(item)); continue; }
            const auto callKind = action.kind == "selection_prepare_pointer_down" ? NativeDeviceCallKind::down :
                action.kind == "selection_prepare_pointer_move" ? NativeDeviceCallKind::move :
                action.kind == "selection_prepare_pointer_up" ? NativeDeviceCallKind::up : NativeDeviceCallKind::end;
            std::optional<std::size_t> callStart, callReturn;
            bool callAmbiguous = session->callJournalLost;
            for (std::size_t n = 0; n < session->calls.size(); ++n)
            {
                const auto& call = session->calls[n];
                if (call.kind != callKind || call.programIndex != action.programIndex) continue;
                auto& slot = call.returned ? callReturn : callStart;
                if (slot) { callAmbiguous = true; break; }
                slot = n;
            }
            if (!callAmbiguous && callStart && callReturn && *callStart < *callReturn &&
                session->calls[*callStart].thread == GetCurrentThreadId() &&
                session->calls[*callReturn].thread == GetCurrentThreadId() &&
                !session->calls[*callStart].succeeded &&
                session->calls[*callStart].traceSize <= session->calls[*callReturn].traceSize &&
                session->calls[*callReturn].traceSize <= trace.Observations().size())
            { item.callStartIndex = *callStart; item.callReturnIndex = *callReturn; item.callReturnedNormally = true;
              item.callReturnedSuccess = session->calls[*callReturn].succeeded == true; }
            if (action.kind == "selection_prepare_session_end")
            {
                const V4NativeDeviceRecord* ended = nullptr; bool duplicate = false;
                for (const auto& record : session->device)
                    if (record.phase == NativeDevicePhase::session_end && record.programIndex == action.programIndex)
                    { if (ended) { duplicate = true; break; } ended = &record; }
                if (!duplicate && item.callReturnedNormally && ended && !ended->queued && !ended->expected &&
                    !ended->state.enabled && !ended->state.closing && !ended->delivery.failed &&
                    ended->delivery.ownerThread == GetCurrentThreadId() && ended->delivery.session &&
                    session->beforeEndStates.size() == 1 && session->beforeEndDelivery.size() == 1 &&
                    session->beforeEndStates.front().enabled && !session->beforeEndStates.front().closing &&
                    !session->beforeEndDelivery.front().failed &&
                    session->beforeEndDelivery.front().ownerThread == ended->delivery.ownerThread &&
                    session->beforeEndDelivery.front().session == ended->delivery.session &&
                    session->beforeEndDelivery.front().entries.size() <= ended->delivery.entries.size())
                    item.endDeviceObserved = true;
                out.steps.push_back(std::move(item)); continue;
            }
            if (!action.screen) { out.steps.push_back(std::move(item)); continue; }
            const V4NativeDeviceRecord* device = nullptr;
            const V4NativeQueuedRecord* queue = nullptr;
            bool ambiguous = false;
            for (const auto& record : session->device)
                if (record.phase == NativeDevicePhase::program_input && record.programIndex == action.programIndex)
                { if (device) { ambiguous = true; break; } device = &record; }
            for (const auto& record : session->queued)
                if (record.phase == NativeDevicePhase::program_input && record.programIndex == action.programIndex)
                { if (queue) { ambiguous = true; break; } queue = &record; }
            const UINT message = action.kind == "selection_prepare_pointer_down" ? WM_LBUTTONDOWN :
                action.kind == "selection_prepare_pointer_move" ? WM_MOUSEMOVE : WM_LBUTTONUP;
            if (!ambiguous && device && queue && device->queued && device->expected &&
                device->queued->session == queue->token.session && device->queued->event == queue->token.event &&
                device->queued->ownerThread == queue->token.ownerThread && device->expected->message == message &&
                device->expected->screen.x == action.screen->x && device->expected->screen.y == action.screen->y)
            {
                POINT point = device->expected->screen;
                std::optional<DeliveryIdentity> delivery;
                for (const auto& entry : device->delivery.entries)
                    if (entry.kind == DeliveryObservationKind::begin && entry.identity.queuedEvent &&
                        !entry.identity.forwarded && !entry.identity.forward && entry.identity.session == queue->token.session &&
                        entry.identity.event == queue->token.event && entry.identity.ownerThread == queue->token.ownerThread)
                    { if (delivery || !entry.identity.delivery) { ambiguous = true; break; } delivery = entry.identity; }
                if (!ambiguous && delivery && ScreenToClient(desktop, &point) &&
                    point.x == device->expected->clientBeforeDispatch.x && point.y == device->expected->clientBeforeDispatch.y)
                {
                    const auto matched = InspectNativeDelivery(trace, 0, &device->delivery, *delivery, desktop,
                        MessageRoute::desktop_window_proc, message, device->expected->buttons,
                        MAKELPARAM(static_cast<short>(point.x), static_cast<short>(point.y)));
                    if (matched.matchedMessageAndPermit && matched.traceIndex < device->trace.size() &&
                        matched.traceIndex < trace.Observations().size())
                    {
                        const auto& saved = device->trace[matched.traceIndex];
                        const auto& current = trace.Observations()[matched.traceIndex];
                        if (saved.enteredSequence == current.enteredSequence && saved.returnedSequence == current.returnedSequence &&
                            saved.windowScope == current.windowScope && saved.route == current.route &&
                            saved.delivery.session == current.delivery.session && saved.delivery.event == current.delivery.event &&
                            saved.delivery.delivery == current.delivery.delivery && saved.delivery.forward == current.delivery.forward &&
                            saved.delivery.forwarded == current.delivery.forwarded && saved.delivery.queuedEvent == current.delivery.queuedEvent &&
                            saved.delivery.ownerThread == current.delivery.ownerThread &&
                            saved.window == current.window && saved.message == current.message && saved.wParam == current.wParam &&
                            saved.inputLParam == current.inputLParam && saved.returned && current.returned &&
                            saved.windowResult == current.windowResult && saved.enteredThread == queue->token.ownerThread &&
                            saved.returnedThread == queue->token.ownerThread)
                        { item.wndprocObserved = true; item.traceIndex = matched.traceIndex;
                          item.enteredSequence = saved.enteredSequence; item.returnedSequence = saved.returnedSequence; }
                    }
                }
            }
            out.steps.push_back(std::move(item));
            auto& recorded = out.steps.back();
            if (recorded.wndprocObserved)
            {
                const auto base = "program_" + std::to_string(action.programIndex);
                const auto enter = selectedPresence.find(base + "_wndproc_enter");
                const auto returned = selectedPresence.find(base + "_wndproc_return");
                if (enter != selectedPresence.end() && returned != selectedPresence.end())
                {
                    recorded.selectedEnterPresent = enter->second; recorded.selectedReturnPresent = returned->second;
                    out.hypothesisObservationRecorded = true;
                    if (!enter->second || !returned->second) out.hypothesisNotMetObserved = true;
                }
                else out.unresolved.emplace_back("v4_preparation_program_graph_node_unavailable");
            }
        }
        out.unresolved.emplace_back("v4_preparation_program_call_graph_occurrence_and_reentrancy_unavailable");
        out.unresolved.emplace_back("v4_preparation_program_selected_presence_order_and_unobserved_absence_unavailable");
        out.unresolved.emplace_back("v4_preparation_program_initial_hover_and_cross_session_causality_unavailable");
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; }
    catch (const std::length_error&) { out.resourceUnavailable = true; }
    catch (...) { out.unresolved.emplace_back("v4_preparation_program_observation_unavailable"); }
    return out;
}

// Only RunBound can construct this value. Stored device/log/trace values are
// snapshots of actual callbacks, not a completed graph or business verdict.
class V4NativeRunReceipt final
{
    friend class V4NativeSequence;
public:
    V4NativeRunReceipt(const V4NativeRunReceipt&) = delete;
    V4NativeRunReceipt& operator=(const V4NativeRunReceipt&) = delete;
    V4NativeRunReceipt(V4NativeRunReceipt&&) noexcept = default;
    V4NativeRunReceipt& operator=(V4NativeRunReceipt&&) noexcept = default;
    const Result& Execution() const noexcept { return execution_; }
    std::shared_ptr<const ::behavior::V4NativeDesktopMoveEvidence> SealedDesktopBusiness() const noexcept
    {
        return execution_.productionStarted && desktopKey_ && desktopKey_->committed_ && desktopKey_->published_ &&
            desktopKey_->phase_ == V4NativeDesktopExecutionKey::Phase::closed ? desktopKey_->snapshot_ : nullptr;
    }
    std::shared_ptr<const ::behavior::NativeMemberBusinessEvidence> SealedMemberBusiness() const noexcept
    {
        return execution_.productionStarted && memberKey_ && memberKey_->committed_ && memberKey_->published_ &&
            memberKey_->phase_ == V4NativeMemberExecutionKey::Phase::closed ? memberKey_->snapshot_ : nullptr;
    }
    std::shared_ptr<const ::behavior::V4NativeCategoryEvidence> SealedCategoryBusiness() const noexcept
    {
        return execution_.productionStarted && categoryKey_ && categoryKey_->committed_ && categoryKey_->published_ &&
            categoryKey_->phase_ == V4NativeCategoryExecutionKey::Phase::closed ? categoryKey_->snapshot_ : nullptr;
    }
    std::shared_ptr<const ::behavior::V4NativeCollectionReorderEvidence> SealedReorderBusiness() const noexcept
    {
        return execution_.productionStarted && reorderKey_ && reorderKey_->committed_ && reorderKey_->published_ &&
            reorderKey_->phase_ == V4NativeReorderExecutionKey::Phase::closed ? reorderKey_->snapshot_ : nullptr;
    }
    std::shared_ptr<const ::behavior::V4NativeMultiDragEvidence> SealedMultiDragBusiness() const noexcept
    {
        return execution_.productionStarted && multiDragKey_ && multiDragKey_->committed_ && multiDragKey_->published_ &&
            multiDragKey_->phase_ == V4NativeMultiDragExecutionKey::Phase::closed ? multiDragKey_->snapshot_ : nullptr;
    }
    std::shared_ptr<const V4NativeSelectionPreparationReceipt> SelectionPreparation() const noexcept { return preparation_; }
    const std::vector<V4NativeDeviceRecord>& DeviceRecords() const noexcept { return deviceRecords_; }
    const std::vector<V4NativeQueuedRecord>& QueuedRecords() const noexcept { return queuedRecords_; }
    const std::vector<V4NativeTraceRecord>& TraceRecords() const noexcept { return traceRecords_; }
    const std::vector<snowdesktop::private_pointer::State>& BeforeEndStates() const noexcept { return beforeEndStates_; }
    const std::vector<snowdesktop::private_pointer::DeliveryObservationLog>& BeforeEndDelivery() const noexcept { return beforeEndDelivery_; }
    const std::vector<std::string>& Unresolved() const noexcept { return unresolved_; }
    std::string_view CandidateId() const noexcept { return candidateId_; }
    std::string_view GraphId() const noexcept { return graphId_; }
    std::uintptr_t HostIdentity() const noexcept { return hostIdentity_; }
    bool SameCandidate(const event_graph::V4FamilyIdentityContext& identity,
        const V4FamilyValidatedContext& verified, const std::vector<bool>& present,
        const event_graph::V4NativeJoinedGraphVariant& selected) const noexcept
    {
        try
        {
            if (candidateId_.empty() || graphId_ != selected.id) return false;
            const auto candidate = identity.RawCandidateIdentity(verified.Coordinate());
            if (candidate.resourceUnavailable || !candidate.errors.empty() || candidate.id != candidateId_) return false;
            const auto replay = event_graph::V4NativeJoinedGraph::Replay(identity, verified, present, selected.id);
            return replay.identityValid && replay.errors.empty() && replay.variant.id == selected.id && replay.variant.classes == selected.classes &&
                replay.variant.order == selected.order && replay.variant.unresolved == selected.unresolved;
        }
        catch (...) { return false; }
    }
    bool SameHost(const std::shared_ptr<NativeHostEpoch>& epoch, const MessageTrace& trace,
        HWND desktop, HWND control) const noexcept
    {
        const auto original = epoch_.lock();
        return execution_.productionStarted && original && original == epoch &&
            original->Matches(&trace, desktop, control);
    }
private:
    V4NativeRunReceipt() = default;
    Result execution_;
    std::weak_ptr<NativeHostEpoch> epoch_;
    std::shared_ptr<V4NativeDesktopExecutionKey> desktopKey_;
    std::shared_ptr<V4NativeMemberExecutionKey> memberKey_;
    std::shared_ptr<V4NativeCategoryExecutionKey> categoryKey_;
    std::shared_ptr<V4NativeReorderExecutionKey> reorderKey_;
    std::shared_ptr<V4NativeMultiDragExecutionKey> multiDragKey_;
    std::shared_ptr<V4NativeSelectionPreparationReceipt> preparation_;
    std::string candidateId_, graphId_;
    std::uintptr_t hostIdentity_ = 0;
    std::vector<V4NativeDeviceRecord> deviceRecords_;
    std::vector<V4NativeQueuedRecord> queuedRecords_;
    std::vector<V4NativeTraceRecord> traceRecords_;
    std::vector<snowdesktop::private_pointer::State> beforeEndStates_;
    std::vector<snowdesktop::private_pointer::DeliveryObservationLog> beforeEndDelivery_;
    std::vector<std::string> unresolved_;
};

struct V4NativeCleanupDelivery
{
    snowdesktop::private_pointer::DeliveryObservation begin;
    std::size_t traceIndex = MessageTrace::InvalidToken;
};
struct V4NativeCleanupEvidence
{
    bool intervalObserved = false, observationUnavailable = false;
    std::vector<V4NativeCleanupDelivery> matched;
    std::vector<std::string> unresolved;
};

// Correlate real non-queued deliveries inside the sealed same-Device End
// interval. Message values are actual inputs, not independent expectations;
// no absence, graph-role, completion, or business verdict follows from this.
inline V4NativeCleanupEvidence InspectV4NativeCleanup(const V4NativeRunReceipt& receipt,
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const event_graph::V4NativeJoinedGraphVariant& selected,
    const std::shared_ptr<NativeHostEpoch>& epoch, const MessageTrace& trace, HWND desktop, HWND control)
{
    using namespace snowdesktop::private_pointer;
    V4NativeCleanupEvidence result;
    try
    {
        if (!receipt.SameCandidate(identity, verified, present, selected) || !receipt.SameHost(epoch, trace, desktop, control) ||
            receipt.BeforeEndDelivery().size() != 1 || receipt.BeforeEndStates().size() != 1)
        { result.unresolved.emplace_back("v4_native_cleanup_receipt_or_before_unavailable"); return result; }
        const V4NativeDeviceRecord* after = nullptr;
        for (const auto& record : receipt.DeviceRecords())
            if (record.phase == NativeDevicePhase::session_end)
            { if (after) { result.unresolved.emplace_back("v4_native_cleanup_multiple_end_records"); return result; } after = &record; }
        const V4NativeTraceRecord* beforeTrace = nullptr; const V4NativeTraceRecord* afterTrace = nullptr;
        for (const auto& record : receipt.TraceRecords())
        {
            if (record.boundary == "device_end_before")
            { if (beforeTrace) { result.unresolved.emplace_back("v4_native_cleanup_multiple_before_traces"); return result; } beforeTrace = &record; }
            if (record.boundary == "device_end_observed_before_host_callback")
            { if (afterTrace) { result.unresolved.emplace_back("v4_native_cleanup_multiple_after_traces"); return result; } afterTrace = &record; }
        }
        const auto& before = receipt.BeforeEndDelivery().front();
        if (!after || !beforeTrace || !afterTrace || after->queued || after->expected || after->state.enabled ||
            before.failed || after->delivery.failed || !before.session || before.session != after->delivery.session ||
            before.ownerThread != GetCurrentThreadId() || after->delivery.ownerThread != before.ownerThread ||
            before.entries.size() > after->delivery.entries.size() || beforeTrace->messages.size() > afterTrace->messages.size() ||
            afterTrace->messages.size() > trace.Observations().size())
        { result.unresolved.emplace_back("v4_native_cleanup_interval_unavailable"); return result; }
        const auto sameIdentity = [](const DeliveryIdentity& a, const DeliveryIdentity& b) {
            return a.ownerThread == b.ownerThread && a.session == b.session && a.event == b.event &&
                a.delivery == b.delivery && a.forwarded == b.forwarded && a.forward == b.forward && a.queuedEvent == b.queuedEvent;
        };
        for (std::size_t i = 0; i < before.entries.size(); ++i)
        {
            const auto& a = before.entries[i]; const auto& b = after->delivery.entries[i];
            if (a.kind != b.kind || !sameIdentity(a.identity, b.identity) || a.window != b.window || a.message != b.message ||
                a.wParam != b.wParam || a.lParam != b.lParam || a.result != b.result)
            { result.unresolved.emplace_back("v4_native_cleanup_log_prefix_changed"); return result; }
        }
        std::map<std::uint64_t, DeliveryIdentity> begun;
        for (std::size_t i = before.entries.size(); i < after->delivery.entries.size(); ++i)
        {
            const auto& item = after->delivery.entries[i];
            if (item.identity.queuedEvent || item.identity.event || item.identity.forwarded || item.identity.forward ||
                item.identity.session != before.session || item.identity.ownerThread != before.ownerThread ||
                !item.identity.delivery || item.window != desktop || item.kind == DeliveryObservationKind::permit_rejected)
            { result.unresolved.emplace_back("v4_native_cleanup_secondary_or_reentrant_delivery_unavailable"); return result; }
            if (item.kind != DeliveryObservationKind::begin) continue;
            if (!begun.emplace(item.identity.delivery, item.identity).second)
            { result.unresolved.emplace_back("v4_native_cleanup_duplicate_delivery_unavailable"); return result; }
            const auto evidence = InspectNativeDelivery(trace, beforeTrace->messages.size(), &after->delivery,
                item.identity, desktop, MessageRoute::desktop_window_proc, item.message, item.wParam, item.lParam);
            if (!evidence.matchedMessageAndPermit || evidence.traceIndex >= afterTrace->messages.size())
            { result.unresolved.emplace_back("v4_native_cleanup_delivery_or_return_unavailable"); return result; }
            const auto& observed = trace.Observations()[evidence.traceIndex];
            const auto& sealed = afterTrace->messages[evidence.traceIndex];
            if (!sameIdentity(observed.delivery, sealed.delivery) || observed.enteredSequence != sealed.enteredSequence ||
                observed.returnedSequence != sealed.returnedSequence || observed.window != sealed.window ||
                observed.message != sealed.message || observed.wParam != sealed.wParam || observed.inputLParam != sealed.inputLParam ||
                observed.returned != sealed.returned || observed.windowResult != sealed.windowResult)
            { result.unresolved.emplace_back("v4_native_cleanup_trace_snapshot_changed"); return result; }
            result.matched.push_back({item, evidence.traceIndex});
        }
        for (std::size_t i = before.entries.size(); i < after->delivery.entries.size(); ++i)
        {
            const auto& item = after->delivery.entries[i];
            const auto found = begun.find(item.identity.delivery);
            if (found == begun.end() || !sameIdentity(found->second, item.identity))
            { result.unresolved.emplace_back("v4_native_cleanup_orphan_record_unavailable"); return result; }
        }
        result.unresolved.emplace_back("v4_native_cleanup_graph_roles_and_other_reentrancy_unverified");
        result.unresolved.emplace_back("v4_native_cleanup_absence_completion_and_business_unverified");
        result.intervalObserved = true;
    }
    catch (...) { result.intervalObserved = false; result.observationUnavailable = true; }
    return result;
}

struct V4NativeLayoutRecord
{
    std::size_t index = 0;
    snowdesktop::external_transfer_observation::Event event;
    std::vector<std::byte> blob;
};
inline bool CopyV4DiagnosticBlob(const snowdesktop::external_transfer_observation::ReadSession& session,
    V4NativeLayoutRecord& record)
{
    if (record.event.blobSize > snowdesktop::external_transfer_observation::BlobLimit()) return false;
    record.blob.resize(record.event.blobSize);
    std::size_t copied = 0;
    return session.CopyBlob(record.index, record.blob.data(), record.blob.size(), copied) && copied == record.blob.size();
}
struct V4NativeLayoutSaveEvidence
{
    bool requestMarkerMatched = false, timerWindowReturned = false, writerAndFinishMarked = false;
    bool timerMarkerBound = false, writerInputsObserved = false;
    bool finishCompletionWindowReturned = false, finishSynchronousTimerReturned = false;
    std::optional<std::size_t> finishTraceIndex;
    // Internal return values are separate local facts. None proves durable
    // storage, producer quiescence or complete causal attribution.
    std::optional<bool> queueAccepted, writeSucceeded, finishReturnedSuccess, finishRevisionMatched;
    bool observationUnavailable = true;
    std::optional<std::uint64_t> requestedRevision, saveRevision;
    std::size_t releaseTraceIndex = MessageTrace::InvalidToken, timerTraceIndex = MessageTrace::InvalidToken;
    std::vector<V4NativeLayoutRecord> records;
    std::vector<std::string> unresolved;
};

// Local correlation only. A matched request marker is inside the release
// delivery scope; the timer, writer and finish markers remain distinct from
// the WndProc return, durable disk state and reloaded model oracle.
inline V4NativeLayoutSaveEvidence InspectV4NativeLayoutSave(const V4NativeRunReceipt& receipt,
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const event_graph::V4NativeJoinedGraphVariant& selected,
    const std::shared_ptr<NativeHostEpoch>& epoch, const MessageTrace& trace, HWND desktop, HWND control)
{
    namespace diagnostic = snowdesktop::external_transfer_observation;
    using namespace snowdesktop::private_pointer;
    V4NativeLayoutSaveEvidence out;
    try
    {
        const bool logComplete = diagnostic::Complete();
        diagnostic::ReadSession session;
        if (!session.Open(true)) { out.unresolved.emplace_back("v4_native_layout_session_unavailable"); return out; }
        const auto isLayout = [](diagnostic::Phase phase) {
            return (phase >= diagnostic::Phase::layout_request_enter && phase <= diagnostic::Phase::layout_finish_return) ||
                phase == diagnostic::Phase::layout_write_input_path || phase == diagnostic::Phase::layout_write_input_bytes;
        };
        if (receipt.HostIdentity())
            for (std::size_t i = 0; i < session.Count(); ++i)
            {
                diagnostic::Event event;
                if (!session.ReadEvent(i, event)) { out.unresolved.emplace_back("v4_native_layout_log_lost"); return out; }
                if (event.object == receipt.HostIdentity() && isLayout(event.phase))
                {
                    V4NativeLayoutRecord record{i, event, {}};
                    if (event.hasBlob)
                    {
                        if (!CopyV4DiagnosticBlob(session, record))
                        { out.observationUnavailable = true; out.unresolved.emplace_back("v4_native_layout_blob_unavailable"); return out; }
                    }
                    out.records.push_back(std::move(record));
                }
            }
        if (!receipt.SameCandidate(identity, verified, present, selected) || !receipt.SameHost(epoch, trace, desktop, control) ||
            !receipt.HostIdentity() || !logComplete || !diagnostic::Complete())
        { out.unresolved.emplace_back("v4_native_layout_identity_or_log_unavailable"); return out; }
        const auto program = verified.TransferProgram();
        const V4NativeDeviceRecord* release = nullptr;
        for (const auto& record : receipt.DeviceRecords())
            if (record.phase == NativeDevicePhase::program_input && record.programIndex &&
                *record.programIndex < program.steps.size() && program.steps[*record.programIndex].kind == "release")
            { if (release) { out.unresolved.emplace_back("v4_native_layout_release_ambiguous"); return out; } release = &record; }
        if (!release || !release->queued || !release->expected || release->expected->message != WM_LBUTTONUP ||
            release->expected->buttons != 0)
        { out.unresolved.emplace_back("v4_native_layout_release_input_unavailable"); return out; }
        const auto& token = *release->queued;
        const DeliveryObservation* primary = nullptr;
        for (const auto& event : release->delivery.entries)
            if (event.kind == DeliveryObservationKind::begin && event.identity.queuedEvent && !event.identity.forwarded &&
                event.identity.ownerThread == token.ownerThread && event.identity.session == token.session && event.identity.event == token.event)
            { if (primary) { out.unresolved.emplace_back("v4_native_layout_release_delivery_ambiguous"); return out; } primary = &event; }
        const auto& client = release->expected->clientBeforeDispatch;
        if (!primary || client.x < (std::numeric_limits<short>::min)() || client.x > (std::numeric_limits<short>::max)() ||
            client.y < (std::numeric_limits<short>::min)() || client.y > (std::numeric_limits<short>::max)())
        { out.unresolved.emplace_back("v4_native_layout_release_delivery_unavailable"); return out; }
        const auto lp = MAKELPARAM(static_cast<short>(client.x), static_cast<short>(client.y));
        const auto delivery = InspectNativeDelivery(trace, 0, &release->delivery, primary->identity,
            desktop, MessageRoute::desktop_window_proc, WM_LBUTTONUP, 0, lp);
        if (!delivery.matchedMessageAndPermit)
        { out.unresolved.emplace_back("v4_native_layout_release_window_return_unavailable"); return out; }
        out.releaseTraceIndex = delivery.traceIndex;
        const auto one = [&](diagnostic::Phase phase, std::optional<std::uint64_t> revision) -> const V4NativeLayoutRecord* {
            const V4NativeLayoutRecord* found = nullptr;
            for (const auto& record : out.records)
                if (record.event.phase == phase && (!revision || record.event.extra == *revision))
                { if (found) return nullptr; found = &record; }
            return found;
        };
        const auto* begin = one(diagnostic::Phase::layout_request_enter, std::nullopt);
        const auto* end = one(diagnostic::Phase::layout_request_return, std::nullopt);
        const auto sameScope = [&](const diagnostic::Event& event) {
            const auto& scope = event.nativeScope; const auto& origin = primary->identity;
            return event.thread == token.ownerThread && scope.ownerThread == origin.ownerThread &&
                scope.session == origin.session && scope.event == origin.event && scope.delivery == origin.delivery &&
                scope.forward == origin.forward && scope.forwarded == origin.forwarded && scope.queuedEvent == origin.queuedEvent;
        };
        if (!begin || !end || begin->index >= end->index || !sameScope(begin->event) || !sameScope(end->event) ||
            begin->event.extra == (std::numeric_limits<std::uint64_t>::max)() || end->event.extra != begin->event.extra + 1 ||
            (end->event.value & 3) != 3)
        { out.unresolved.emplace_back("v4_native_layout_request_scope_or_revision_unavailable"); return out; }
        out.requestMarkerMatched = true;
        const auto requested = end->event.extra;
        out.requestedRevision = requested;
        if (requested == (std::numeric_limits<std::uint64_t>::max)())
        { out.unresolved.emplace_back("v4_native_layout_revision_overflow_unavailable"); return out; }
        const auto saveRevision = requested + 1;
        out.saveRevision = saveRevision;
        const auto* timerStart = one(diagnostic::Phase::layout_timer_enter, requested);
        const auto* timerEnd = one(diagnostic::Phase::layout_timer_scope_exit, saveRevision);
        const auto* queueStart = one(diagnostic::Phase::layout_queue_enter, saveRevision);
        const auto* queueEnd = one(diagnostic::Phase::layout_queue_return, saveRevision);
        const auto* writeStart = one(diagnostic::Phase::layout_write_enter, saveRevision);
        const auto* writePath = one(diagnostic::Phase::layout_write_input_path, saveRevision);
        const auto* writeBytes = one(diagnostic::Phase::layout_write_input_bytes, saveRevision);
        const auto* writeEnd = one(diagnostic::Phase::layout_write_return, saveRevision);
        const auto* finishStart = one(diagnostic::Phase::layout_finish_enter, saveRevision);
        const auto* finishEnd = one(diagnostic::Phase::layout_finish_return, saveRevision);
        if (queueStart && queueEnd && queueStart->index < queueEnd->index && queueStart->event.thread &&
            queueStart->event.thread == queueEnd->event.thread && (queueEnd->event.value == 0 || queueEnd->event.value == 1))
            out.queueAccepted = queueEnd->event.value == 1;
        if (writeStart && writeEnd && writeStart->index < writeEnd->index && writeStart->event.thread &&
            writeStart->event.thread == writeEnd->event.thread && (writeEnd->event.value == 0 || writeEnd->event.value == 1))
            out.writeSucceeded = writeEnd->event.value == 1;
        if (finishStart && finishEnd && finishStart->index < finishEnd->index && finishStart->event.thread &&
            finishStart->event.thread == finishEnd->event.thread)
        {
            out.finishReturnedSuccess = (finishEnd->event.value & 8) != 0;
            out.finishRevisionMatched = (finishEnd->event.value & 2) != 0;
        }
        const V4NativeTraceRecord* completed = nullptr;
        for (const auto& item : receipt.TraceRecords())
            if (item.boundary == "completion_after")
            { if (completed) { out.unresolved.emplace_back("v4_native_layout_completion_trace_ambiguous"); completed = nullptr; break; } completed = &item; }
        if (completed && completed->messages.size() <= trace.Observations().size() && out.releaseTraceIndex < completed->messages.size())
        {
            bool ambiguousTimer = false;
            for (std::size_t i = out.releaseTraceIndex + 1; i < completed->messages.size(); ++i)
            {
                const auto& event = completed->messages[i];
                if (event.route != MessageRoute::control_window_proc || event.window != control || event.message != WM_TIMER ||
                    event.wParam != kLayoutScrollSaveTimerId) continue;
                if (out.timerTraceIndex != MessageTrace::InvalidToken) { ambiguousTimer = true; break; }
                const auto& live = trace.Observations()[i];
                if (!event.returned || !live.returned || event.enteredThread != GetCurrentThreadId() ||
                    event.returnedThread != event.enteredThread || event.returnedSequence <= event.enteredSequence ||
                    event.enteredSequence != live.enteredSequence || event.returnedSequence != live.returnedSequence ||
                    event.windowScope != live.windowScope || event.route != live.route || event.window != live.window ||
                    event.message != live.message || event.wParam != live.wParam || event.windowResult != live.windowResult ||
                    event.enteredThread != live.enteredThread || event.returnedThread != live.returnedThread)
                { ambiguousTimer = true; break; }
                out.timerTraceIndex = i;
            }
            if (!ambiguousTimer && out.timerTraceIndex != MessageTrace::InvalidToken) out.timerWindowReturned = true;
            else { out.timerTraceIndex = MessageTrace::InvalidToken; out.unresolved.emplace_back("v4_native_layout_timer_window_return_unavailable"); }
            if (finishStart && finishEnd && finishStart->index < finishEnd->index && finishStart->event.windowScope &&
                finishStart->event.windowScope == finishEnd->event.windowScope && finishStart->event.thread &&
                finishStart->event.thread == finishEnd->event.thread)
            {
                std::optional<std::size_t> match; bool ambiguous = false;
                for (std::size_t i = 0; i < completed->messages.size(); ++i)
                    if (completed->messages[i].windowScope == finishStart->event.windowScope)
                    { if (match) { ambiguous = true; break; } match = i; }
                if (!ambiguous && match)
                {
                    const auto& saved = completed->messages[*match]; const auto& live = trace.Observations()[*match];
                    const bool same = saved.returned && live.returned && saved.enteredThread == finishStart->event.thread &&
                        saved.returnedThread == saved.enteredThread && saved.enteredSequence < saved.returnedSequence &&
                        saved.enteredSequence == live.enteredSequence && saved.returnedSequence == live.returnedSequence &&
                        saved.windowScope == live.windowScope && saved.window == live.window && saved.route == live.route &&
                        saved.message == live.message && saved.wParam == live.wParam && saved.windowResult == live.windowResult &&
                        saved.enteredThread == live.enteredThread && saved.returnedThread == live.returnedThread;
                    if (same && saved.route == MessageRoute::control_window_proc && saved.window == control &&
                        saved.message == kBackgroundShellReadyMessage)
                    { out.finishCompletionWindowReturned = true; out.finishTraceIndex = *match; }
                    else if (same && saved.route == MessageRoute::control_window_proc && saved.window == control &&
                        saved.message == WM_TIMER && saved.wParam == kLayoutScrollSaveTimerId)
                    { out.finishSynchronousTimerReturned = true; out.finishTraceIndex = *match; }
                }
            }
        }
        else out.unresolved.emplace_back("v4_native_layout_completion_trace_unavailable");
        if (!timerStart || !timerEnd || !queueStart || !queueEnd || !writeStart || !writeEnd || !finishStart || !finishEnd ||
            timerStart->index >= timerEnd->index || queueStart->index >= queueEnd->index ||
            writeStart->index >= writeEnd->index || finishStart->index >= finishEnd->index ||
            queueEnd->event.value != 1 || writeEnd->event.value != 1 ||
            out.queueAccepted != true || out.writeSucceeded != true || out.finishReturnedSuccess != true ||
            out.finishRevisionMatched != true || (finishEnd->event.value & 15) != 11 ||
            finishEnd->event.currentRevision != saveRevision || !out.timerWindowReturned)
        { out.unresolved.emplace_back("v4_native_layout_timer_queue_write_or_finish_unavailable"); return out; }
        out.writerAndFinishMarked = true;
        if (writePath && writeBytes && writePath->event.hasBlob && writeBytes->event.hasBlob &&
            writePath->blob.size() == writePath->event.blobSize && writeBytes->blob.size() == writeBytes->event.blobSize &&
            !writePath->blob.empty() && writePath->blob.size() % sizeof(wchar_t) == 0 &&
            writeStart->event.thread && writePath->event.thread == writeStart->event.thread &&
            writeBytes->event.thread == writeStart->event.thread && writeEnd->event.thread == writeStart->event.thread &&
            writeStart->index < writePath->index && writePath->index < writeBytes->index && writeBytes->index < writeEnd->index)
            out.writerInputsObserved = true;
        else out.unresolved.emplace_back("v4_native_layout_writer_input_interval_unavailable");
        const auto& timerMessage = completed->messages[out.timerTraceIndex];
        const auto scope = timerMessage.windowScope;
        const auto scopeCount = scope ? std::count_if(completed->messages.begin(), completed->messages.end(),
            [&](const auto& message) { return message.windowScope == scope; }) : 0;
        if (scope && scopeCount == 1 && timerStart->event.windowScope == scope && timerEnd->event.windowScope == scope &&
            timerStart->event.thread == timerMessage.enteredThread && timerEnd->event.thread == timerMessage.enteredThread &&
            end->index < timerStart->index && timerStart->index < timerEnd->index)
            out.timerMarkerBound = true;
        else out.unresolved.emplace_back("v4_native_layout_timer_scope_binding_unavailable");
        out.unresolved.emplace_back("v4_native_layout_finish_message_identity_unverified");
        out.unresolved.emplace_back("v4_native_layout_reentrancy_quiescence_disk_and_reloaded_model_unverified");
    }
    catch (...) { out.observationUnavailable = true; }
    return out;
}

class V4NativeSequence final
{
public:
    static V4NativeRunReceipt RunBound(const event_graph::V4FamilyIdentityContext& identity,
        const V4FamilyValidatedContext& verified, const std::vector<bool>& present,
        const event_graph::V4NativeJoinedGraphVariant& selected, const std::vector<NativePointInput>& points,
        HWND desktop, HWND control, RECT bounds, const MessageTrace& trace,
        const std::shared_ptr<NativeHostEpoch>& epoch, const NativeHostBoundaries& host,
        const V4NativeDesktopObserverBoundaries& desktopObserver = {},
        const V4NativeMemberObserverBoundaries& memberObserver = {},
        const V4NativeCategoryObserverBoundaries& categoryObserver = {},
        const V4NativeReorderObserverBoundaries& reorderObserver = {},
        const std::shared_ptr<V4NativeSelectionPreparationReceipt>& preparation = {},
        const V4NativeMultiDragObserverBoundaries& multiDragObserver = {})
    {
        const auto pinnedEpoch = epoch;
        V4NativeRunReceipt receipt;
        bool called = false;
        try
        {
            const auto pinnedPreparation = preparation;
            const auto pinnedMultiDragObserver = multiDragObserver;
            const auto plan = BuildV4NativeExecutionPlan(identity, verified, present, selected, points, desktop, bounds);
            receipt.execution_.obligations = plan.preconditions;
            receipt.execution_.postconditionsPending = plan.postconditions;
            receipt.execution_.unclassifiedObligations = plan.unclassified;
            if (!plan.graphIdentityValid)
                return receipt;
            const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
            if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty())
            { receipt.execution_.obligations.emplace_back("v4_native_receipt_candidate_unavailable"); return receipt; }
            if (!host.beforeDeviceBegin || !host.observeDeviceState || !host.observeCapture || !host.awaitCompletion ||
                !host.beforePointerSessionEnd || !host.observePointerSessionEnd || !host.assertBusinessResults ||
                !host.throwAsync || !host.productionInputEntered || !host.beforeQueuedDelivery)
            { receipt.execution_.obligations.emplace_back("v4_native_receipt_host_observers_missing"); return receipt; }
            if (static_cast<bool>(desktopObserver.bindBeforeBegin) != static_cast<bool>(desktopObserver.observeBusiness))
            { receipt.execution_.obligations.emplace_back("v4_native_desktop_observer_incomplete"); return receipt; }
            if (static_cast<bool>(memberObserver.bindBeforeBegin) != static_cast<bool>(memberObserver.observeBusiness) ||
                (memberObserver.bindBeforeBegin && desktopObserver.bindBeforeBegin))
            { receipt.execution_.obligations.emplace_back("v4_native_member_observer_incomplete_or_conflicting"); return receipt; }
            if (static_cast<bool>(categoryObserver.bindBeforeBegin) != static_cast<bool>(categoryObserver.observeBusiness) ||
                (categoryObserver.bindBeforeBegin && (desktopObserver.bindBeforeBegin || memberObserver.bindBeforeBegin)))
            { receipt.execution_.obligations.emplace_back("v4_native_category_observer_incomplete_or_conflicting"); return receipt; }
            if (static_cast<bool>(reorderObserver.bindBeforeBegin) != static_cast<bool>(reorderObserver.observeBusiness) ||
                (reorderObserver.bindBeforeBegin && (desktopObserver.bindBeforeBegin || memberObserver.bindBeforeBegin || categoryObserver.bindBeforeBegin)))
            { receipt.execution_.obligations.emplace_back("v4_native_reorder_observer_incomplete_or_conflicting"); return receipt; }
            const bool hasMultiDrag = static_cast<bool>(pinnedMultiDragObserver.consumePreparation) ||
                static_cast<bool>(pinnedMultiDragObserver.bindBeforeBegin) || static_cast<bool>(pinnedMultiDragObserver.observeBusiness) ||
                static_cast<bool>(pinnedPreparation);
            if (hasMultiDrag && (!pinnedPreparation || !pinnedMultiDragObserver.consumePreparation ||
                !pinnedMultiDragObserver.bindBeforeBegin || !pinnedMultiDragObserver.observeBusiness ||
                desktopObserver.bindBeforeBegin || memberObserver.bindBeforeBegin || categoryObserver.bindBeforeBegin ||
                reorderObserver.bindBeforeBegin || pinnedPreparation->Consumed() || !pinnedPreparation->SealedSelection() ||
                !pinnedPreparation->SameCandidate(identity, verified, present, selected) ||
                !pinnedPreparation->SameHost(pinnedEpoch, trace, desktop, control)))
            { receipt.execution_.obligations.emplace_back("v4_native_multi_drag_preparation_or_observer_unavailable"); return receipt; }
            if (desktopObserver.bindBeforeBegin &&
                (Value(verified.Axes(), verified.Coordinate(), "T01") != "internal_native" ||
                 Value(verified.Axes(), verified.Coordinate(), "V4.source.kind") != "file_only" ||
                 Value(verified.Axes(), verified.Coordinate(), "V4.source.file_count") != "one" ||
                 Value(verified.Axes(), verified.Coordinate(), "source.E01") != "free_desktop" ||
                 Value(verified.Axes(), verified.Coordinate(), "target.E01") != "free_desktop"))
            { receipt.execution_.obligations.emplace_back("v4_native_desktop_observer_candidate_unavailable"); return receipt; }
            if (memberObserver.bindBeforeBegin &&
                (Value(verified.Axes(), verified.Coordinate(), "T01") != "internal_native" ||
                 Value(verified.Axes(), verified.Coordinate(), "source.E01") != "free_desktop" ||
                 Value(verified.Axes(), verified.Coordinate(), "source.E02") != "collection" ||
                 Value(verified.Axes(), verified.Coordinate(), "source.E03") != "widget_body" ||
                 Value(verified.Axes(), verified.Coordinate(), "target.E01") != "free_desktop" ||
                 Value(verified.Axes(), verified.Coordinate(), "target.E02") != "none" ||
                 Value(verified.Axes(), verified.Coordinate(), "H01.region") != "empty_slot" ||
                 Value(verified.Axes(), verified.Coordinate(), "P01.primary") != "file" ||
                 Value(verified.Axes(), verified.Coordinate(), "P01.second") != "na" ||
                 Value(verified.Axes(), verified.Coordinate(), "P02.source_actual_count") != "one" ||
                 Value(verified.Axes(), verified.Coordinate(), "G01.button") != "left" ||
                 Value(verified.Axes(), verified.Coordinate(), "G04.termination") != "drop" ||
                 Value(verified.Axes(), verified.Coordinate(), "X03.task_count") != "one"))
            { receipt.execution_.obligations.emplace_back("v4_native_member_observer_candidate_unavailable"); return receipt; }
            if (categoryObserver.bindBeforeBegin &&
                (Value(verified.Axes(), verified.Coordinate(), "T01") != "internal_native" ||
                 Value(verified.Axes(), verified.Coordinate(), "source.E01") != "free_desktop" ||
                 Value(verified.Axes(), verified.Coordinate(), "source.E02") != "desktop_files" ||
                 Value(verified.Axes(), verified.Coordinate(), "source.E03") != "widget_body" ||
                 Value(verified.Axes(), verified.Coordinate(), "target.E01") != "free_desktop" ||
                 Value(verified.Axes(), verified.Coordinate(), "target.E02") != "none" ||
                 Value(verified.Axes(), verified.Coordinate(), "H01.region") != "empty_slot" ||
                 Value(verified.Axes(), verified.Coordinate(), "P01.primary") != "file" ||
                 Value(verified.Axes(), verified.Coordinate(), "P01.second") != "na" ||
                 Value(verified.Axes(), verified.Coordinate(), "P02.source_actual_count") != "one" ||
                 Value(verified.Axes(), verified.Coordinate(), "G01.button") != "left" ||
                 Value(verified.Axes(), verified.Coordinate(), "G04.termination") != "drop" ||
                 Value(verified.Axes(), verified.Coordinate(), "X03.task_count") != "one"))
            { receipt.execution_.obligations.emplace_back("v4_native_category_observer_candidate_unavailable"); return receipt; }
            if (reorderObserver.bindBeforeBegin &&
                (Value(verified.Axes(), verified.Coordinate(), "T01") != "internal_native" ||
                 Value(verified.Axes(), verified.Coordinate(), "source.E01") != "free_desktop" ||
                 Value(verified.Axes(), verified.Coordinate(), "target.E01") != "free_desktop" ||
                 Value(verified.Axes(), verified.Coordinate(), "source.E02") != "collection" ||
                 Value(verified.Axes(), verified.Coordinate(), "target.E02") != "collection" ||
                 Value(verified.Axes(), verified.Coordinate(), "source.E03") != "widget_body" ||
                 Value(verified.Axes(), verified.Coordinate(), "target.E03") != "widget_body" ||
                 Value(verified.Axes(), verified.Coordinate(), "H01.region") != "before_item" ||
                 Value(verified.Axes(), verified.Coordinate(), "H05.position") != "first" ||
                 Value(verified.Axes(), verified.Coordinate(), "H05.indicated_identity") != "unselected_target" ||
                 Value(verified.Axes(), verified.Coordinate(), "H05.surface") != "members" ||
                 Value(verified.Axes(), verified.Coordinate(), "H05.filter") != "unfiltered" ||
                 Value(verified.Axes(), verified.Coordinate(), "H05.source_relation") != "adjacent_original" ||
                 Value(verified.Axes(), verified.Coordinate(), "H05.scroll") != "unchanged" ||
                 Value(verified.Axes(), verified.Coordinate(), "P01.primary") != "file" ||
                 Value(verified.Axes(), verified.Coordinate(), "P01.second") != "na" ||
                 Value(verified.Axes(), verified.Coordinate(), "P02.source_actual_count") != "one" ||
                 Value(verified.Axes(), verified.Coordinate(), "G01.button") != "left" ||
                 Value(verified.Axes(), verified.Coordinate(), "G04.termination") != "drop" ||
                 Value(verified.Axes(), verified.Coordinate(), "X03.task_count") != "one" ||
                 Value(verified.Axes(), verified.Coordinate(), "C01") != "manual" ||
                 Value(verified.Axes(), verified.Coordinate(), "C02") != "na" ||
                 Value(verified.Axes(), verified.Coordinate(), "C03") != "manual" ||
                 Value(verified.Axes(), verified.Coordinate(), "C04") != "na" ||
                 Value(verified.Axes(), verified.Coordinate(), "C05") != "off" ||
                 Value(verified.Axes(), verified.Coordinate(), "C06") != "off"))
            { receipt.execution_.obligations.emplace_back("v4_native_reorder_observer_candidate_unavailable"); return receipt; }
            receipt.candidateId_ = raw.id; receipt.graphId_ = selected.id; receipt.epoch_ = pinnedEpoch;
            if (pinnedEpoch && pinnedEpoch->Matches(&trace, desktop, control)) receipt.hostIdentity_ = pinnedEpoch->app_;
            if (desktopObserver.bindBeforeBegin)
                receipt.desktopKey_ = std::shared_ptr<V4NativeDesktopExecutionKey>(new V4NativeDesktopExecutionKey(
                    pinnedEpoch, raw.id, selected.id));
            if (memberObserver.bindBeforeBegin)
                receipt.memberKey_ = std::shared_ptr<V4NativeMemberExecutionKey>(new V4NativeMemberExecutionKey(
                    pinnedEpoch, raw.id, selected.id));
            if (categoryObserver.bindBeforeBegin)
                receipt.categoryKey_ = std::shared_ptr<V4NativeCategoryExecutionKey>(new V4NativeCategoryExecutionKey(
                    pinnedEpoch, raw.id, selected.id));
            if (reorderObserver.bindBeforeBegin)
                receipt.reorderKey_ = std::shared_ptr<V4NativeReorderExecutionKey>(new V4NativeReorderExecutionKey(
                    pinnedEpoch, raw.id, selected.id));
            if (hasMultiDrag)
            {
                receipt.preparation_ = pinnedPreparation;
                receipt.multiDragKey_ = std::shared_ptr<V4NativeMultiDragExecutionKey>(new V4NativeMultiDragExecutionKey(
                    pinnedEpoch, pinnedPreparation, raw.id, selected.id));
            }
            receipt.unresolved_ = {"v4_native_receipt_source_fixture_and_candidate_binding_unverified",
                "v4_native_receipt_message_graph_presence_and_non_device_reentrancy_unverified",
                "v4_native_receipt_completion_and_business_observation_unverified"};
            const auto snapshot = [&](const char* boundary) {
                if (!pinnedEpoch || !pinnedEpoch->Matches(&trace, desktop, control) || trace.LostOrCrossThread())
                    throw ObservationUnavailable("v4_native_receipt_host_or_trace_unavailable");
                receipt.traceRecords_.push_back({boundary, trace.Observations()});
            };
            NativeHostBoundaries bound = host;
            bool completionReturned = false, endObserved = false, releaseDelivered = false, captureCallbackReturned = false;
            bool multiBeforeBeginEntered = false, multiBeforeBeginCompleted = false;
            bound.beforeDeviceBegin = [&] {
                if (receipt.multiDragKey_) multiBeforeBeginEntered = true;
                host.beforeDeviceBegin(); snapshot("before_device_begin");
                if (receipt.multiDragKey_)
                {
                    auto& key = *receipt.multiDragKey_;
                    if (key.phase_ != V4NativeMultiDragExecutionKey::Phase::created || pinnedPreparation->Consumed() ||
                        !pinnedMultiDragObserver.consumePreparation() || !pinnedPreparation->Consumed() ||
                        !pinnedPreparation->SameCandidate(identity, verified, present, selected) ||
                        !pinnedPreparation->SameHost(pinnedEpoch, trace, desktop, control))
                        throw ObservationUnavailable("v4_native_multi_drag_preparation_consumption_unavailable");
                    snapshot("multi_drag_preparation_consumed");
                    key.phase_ = V4NativeMultiDragExecutionKey::Phase::before_begin;
                    pinnedMultiDragObserver.bindBeforeBegin(receipt.multiDragKey_);
                    if (!key.bound_) throw ObservationUnavailable("v4_native_multi_drag_baseline_unavailable");
                    snapshot("multi_drag_baseline_bound");
                    key.phase_ = V4NativeMultiDragExecutionKey::Phase::dispatching;
                    multiBeforeBeginCompleted = true;
                }
                if (receipt.desktopKey_)
                {
                    auto& key = *receipt.desktopKey_;
                    if (key.phase_ != V4NativeDesktopExecutionKey::Phase::created)
                        throw ObservationUnavailable("v4_native_desktop_duplicate_begin_unavailable");
                    key.phase_ = V4NativeDesktopExecutionKey::Phase::before_begin;
                    desktopObserver.bindBeforeBegin(receipt.desktopKey_);
                    if (!key.bound_) throw ObservationUnavailable("v4_native_desktop_baseline_unavailable");
                    snapshot("desktop_baseline_bound"); key.phase_ = V4NativeDesktopExecutionKey::Phase::dispatching;
                }
                if (receipt.memberKey_)
                {
                    auto& key = *receipt.memberKey_;
                    if (key.phase_ != V4NativeMemberExecutionKey::Phase::created)
                        throw ObservationUnavailable("v4_native_member_duplicate_begin_unavailable");
                    key.phase_ = V4NativeMemberExecutionKey::Phase::before_begin;
                    memberObserver.bindBeforeBegin(receipt.memberKey_);
                    if (!key.bound_) throw ObservationUnavailable("v4_native_member_baseline_unavailable");
                    snapshot("member_baseline_bound"); key.phase_ = V4NativeMemberExecutionKey::Phase::dispatching;
                }
                if (receipt.categoryKey_)
                {
                    auto& key = *receipt.categoryKey_;
                    if (key.phase_ != V4NativeCategoryExecutionKey::Phase::created)
                        throw ObservationUnavailable("v4_native_category_duplicate_begin_unavailable");
                    key.phase_ = V4NativeCategoryExecutionKey::Phase::before_begin;
                    categoryObserver.bindBeforeBegin(receipt.categoryKey_);
                    if (!key.bound_) throw ObservationUnavailable("v4_native_category_baseline_unavailable");
                    snapshot("category_baseline_bound"); key.phase_ = V4NativeCategoryExecutionKey::Phase::dispatching;
                }
                if (receipt.reorderKey_)
                {
                    auto& key = *receipt.reorderKey_;
                    if (key.phase_ != V4NativeReorderExecutionKey::Phase::created)
                        throw ObservationUnavailable("v4_native_reorder_duplicate_begin_unavailable");
                    key.phase_ = V4NativeReorderExecutionKey::Phase::before_begin;
                    reorderObserver.bindBeforeBegin(receipt.reorderKey_);
                    if (!key.bound_) throw ObservationUnavailable("v4_native_reorder_baseline_unavailable");
                    snapshot("reorder_baseline_bound"); key.phase_ = V4NativeReorderExecutionKey::Phase::dispatching;
                }
            };
            bound.beforeQueuedDelivery = [&](NativeDevicePhase phase, std::optional<std::size_t> index,
                const snowdesktop::private_pointer::QueuedEventIdentity& token) {
                if (!pinnedEpoch || !pinnedEpoch->Matches(&trace, desktop, control) || token.ownerThread != GetCurrentThreadId() ||
                    !token.session || !token.event)
                    throw ObservationUnavailable("v4_native_receipt_queued_identity_unavailable");
                receipt.queuedRecords_.push_back({phase, index, token});
                snapshot("queued_before_dispatch");
                host.beforeQueuedDelivery(phase, index, token);
            };
            bound.observeDeviceState = [&](NativeDevicePhase phase, std::optional<std::size_t> index,
                const snowdesktop::private_pointer::State& state,
                const snowdesktop::private_pointer::DeliveryObservationLog& delivery,
                std::optional<snowdesktop::private_pointer::QueuedEventIdentity> queued,
                std::optional<NativeInputExpectation> expected) {
                if (!pinnedEpoch || !pinnedEpoch->Matches(&trace, desktop, control) || delivery.failed ||
                    delivery.ownerThread != GetCurrentThreadId() || !delivery.session)
                    throw ObservationUnavailable("v4_native_receipt_device_or_host_unavailable");
                receipt.deviceRecords_.push_back({phase, index, state, delivery, queued, expected, trace.Observations()});
                snapshot(phase == NativeDevicePhase::session_end ? "device_end_observed_before_host_callback" :
                    "device_observed_before_host_callback");
                host.observeDeviceState(phase, index, state, delivery, queued, expected);
                snapshot("device_observed_after_host_callback");
                if (receipt.multiDragKey_ && phase == NativeDevicePhase::session_end && !state.enabled && !state.closing)
                    endObserved = true;
                if (receipt.multiDragKey_ && phase == NativeDevicePhase::program_input && queued && expected &&
                    expected->message == WM_LBUTTONUP)
                {
                    using namespace snowdesktop::private_pointer;
                    POINT point = expected->screen;
                    std::optional<DeliveryIdentity> matchedIdentity;
                    bool ambiguous = false;
                    for (const auto& item : delivery.entries)
                        if (item.kind == DeliveryObservationKind::begin && item.identity.queuedEvent &&
                            !item.identity.forwarded && !item.identity.forward && item.identity.session == queued->session &&
                            item.identity.event == queued->event && item.identity.ownerThread == queued->ownerThread)
                        { if (matchedIdentity || !item.identity.delivery) { ambiguous = true; break; } matchedIdentity = item.identity; }
                    if (!ambiguous && matchedIdentity && ScreenToClient(desktop, &point) &&
                        point.x == expected->clientBeforeDispatch.x && point.y == expected->clientBeforeDispatch.y)
                    {
                        const auto matched = InspectNativeDelivery(trace, 0, &delivery, *matchedIdentity, desktop,
                            MessageRoute::desktop_window_proc, expected->message, expected->buttons,
                            MAKELPARAM(static_cast<short>(point.x), static_cast<short>(point.y)));
                        releaseDelivered = matched.matchedMessageAndPermit;
                    }
                }
            };
            bound.observeCapture = [&](const EventStep& step) { snapshot("capture_before"); host.observeCapture(step); snapshot("capture_after"); captureCallbackReturned = true; };
            bound.awaitCompletion = [&](const EventStep& step) { snapshot("completion_before"); host.awaitCompletion(step); snapshot("completion_after"); completionReturned = true; };
            bound.beforePointerSessionEnd = [&](const EventStep& step, const snowdesktop::private_pointer::State& state,
                    const snowdesktop::private_pointer::DeliveryObservationLog& delivery) {
                snapshot("device_end_before"); receipt.beforeEndStates_.push_back(state);
                receipt.beforeEndDelivery_.push_back(delivery);
                host.beforePointerSessionEnd(step, state, delivery); snapshot("device_end_host_callback_after"); };
            bound.observePointerSessionEnd = [&](const EventStep& step) { snapshot("device_end_observe_before"); host.observePointerSessionEnd(step); snapshot("device_end_observe_after"); };
            bound.assertBusinessResults = [&](const EventStep& step) {
                snapshot("business_before");
                if (receipt.multiDragKey_)
                {
                    auto& key = *receipt.multiDragKey_;
                    if (!completionReturned || !captureCallbackReturned || !releaseDelivered || !endObserved ||
                        key.phase_ != V4NativeMultiDragExecutionKey::Phase::dispatching || !key.bound_)
                        throw ObservationUnavailable("v4_native_multi_drag_business_scope_unavailable");
                    key.phase_ = V4NativeMultiDragExecutionKey::Phase::business;
                    struct CloseMultiScope final { V4NativeMultiDragExecutionKey& key; ~CloseMultiScope() { key.phase_ = V4NativeMultiDragExecutionKey::Phase::closed; } } close{key};
                    pinnedMultiDragObserver.observeBusiness(receipt.multiDragKey_);
                    if (!key.published_ || !key.snapshot_)
                        throw ObservationUnavailable("v4_native_multi_drag_business_snapshot_unavailable");
                }
                else if (receipt.reorderKey_)
                {
                    auto& key = *receipt.reorderKey_;
                    if (!completionReturned || key.phase_ != V4NativeReorderExecutionKey::Phase::dispatching || !key.bound_)
                        throw ObservationUnavailable("v4_native_reorder_business_scope_unavailable");
                    key.phase_ = V4NativeReorderExecutionKey::Phase::business;
                    struct CloseReorderScope final { V4NativeReorderExecutionKey& key; ~CloseReorderScope() { key.phase_ = V4NativeReorderExecutionKey::Phase::closed; } } close{key};
                    reorderObserver.observeBusiness(receipt.reorderKey_);
                    if (!key.published_ || !key.snapshot_)
                        throw ObservationUnavailable("v4_native_reorder_business_snapshot_unavailable");
                }
                else if (receipt.categoryKey_)
                {
                    auto& key = *receipt.categoryKey_;
                    if (!completionReturned || key.phase_ != V4NativeCategoryExecutionKey::Phase::dispatching || !key.bound_)
                        throw ObservationUnavailable("v4_native_category_business_scope_unavailable");
                    key.phase_ = V4NativeCategoryExecutionKey::Phase::business;
                    struct CloseCategoryScope final { V4NativeCategoryExecutionKey& key; ~CloseCategoryScope() { key.phase_ = V4NativeCategoryExecutionKey::Phase::closed; } } close{key};
                    categoryObserver.observeBusiness(receipt.categoryKey_);
                    if (!key.published_ || !key.snapshot_)
                        throw ObservationUnavailable("v4_native_category_business_snapshot_unavailable");
                }
                else if (receipt.memberKey_)
                {
                    auto& key = *receipt.memberKey_;
                    if (!completionReturned || key.phase_ != V4NativeMemberExecutionKey::Phase::dispatching || !key.bound_)
                        throw ObservationUnavailable("v4_native_member_business_scope_unavailable");
                    key.phase_ = V4NativeMemberExecutionKey::Phase::business;
                    struct CloseMemberScope final { V4NativeMemberExecutionKey& key; ~CloseMemberScope() { key.phase_ = V4NativeMemberExecutionKey::Phase::closed; } } close{key};
                    memberObserver.observeBusiness(receipt.memberKey_);
                    if (!key.published_ || !key.snapshot_)
                        throw ObservationUnavailable("v4_native_member_business_snapshot_unavailable");
                }
                else if (!receipt.desktopKey_) host.assertBusinessResults(step);
                else
                {
                    auto& key = *receipt.desktopKey_;
                    if (!completionReturned || key.phase_ != V4NativeDesktopExecutionKey::Phase::dispatching || !key.bound_)
                        throw ObservationUnavailable("v4_native_desktop_business_scope_unavailable");
                    key.phase_ = V4NativeDesktopExecutionKey::Phase::business;
                    struct CloseScope final { V4NativeDesktopExecutionKey& key; ~CloseScope() { key.phase_ = V4NativeDesktopExecutionKey::Phase::closed; } } close{key};
                    desktopObserver.observeBusiness(receipt.desktopKey_);
                    if (!key.published_ || !key.snapshot_)
                        throw ObservationUnavailable("v4_native_desktop_business_snapshot_unavailable");
                }
                snapshot("business_after");
            };
            called = true;
            std::vector<NativeGraphAction> concreteActions;
            std::vector<std::optional<POINT>> concretePoints;
            POINT concreteFirst{};
            const bool restrictedDesktopAttempt = desktopObserver.bindBeforeBegin && desktopObserver.observeBusiness &&
                !memberObserver.bindBeforeBegin && !categoryObserver.bindBeforeBegin && !reorderObserver.bindBeforeBegin &&
                !hasMultiDrag && BuildRestrictedNativeDesktopActions(verified, points, desktop, bounds,
                    concreteActions, concretePoints, concreteFirst);
            if (restrictedDesktopAttempt)
            {
                Result attempt;
                attempt.unclassifiedObligations = plan.unclassified;
                attempt.postconditionsPending = plan.postconditions;
                for (const auto& reason : plan.preconditions)
                    attempt.postconditionsPending.push_back("v4_native_restricted_attempt_unverified:" + reason);
                if (!pinnedEpoch || !pinnedEpoch->Matches(&trace, desktop, control) ||
                    !pinnedEpoch->TryBeginExecution(&trace, desktop, control))
                { attempt.status = Status::observation_unavailable; attempt.failure = "v4_native_host_execution_lease_unavailable"; }
                else
                {
                    const auto endEpoch = [](const NativeHostEpoch* active) noexcept { active->EndExecution(); };
                    const std::unique_ptr<const NativeHostEpoch, decltype(endEpoch)> guard(pinnedEpoch.get(), endEpoch);
                    const auto program = verified.TransferProgram();
                    attempt = NativeSequence::DispatchPrepared(std::move(attempt), program, concreteActions, concretePoints,
                        concreteFirst, desktop, bounds, bound);
                }
                attempt.additionalObservationUnavailable = true;
                attempt.unattributedBusinessFailureObserved = attempt.businessFailureObserved;
                attempt.unattributedEventOracleFailureObserved = attempt.eventOracleFailureObserved;
                attempt.businessFailureObserved = false; attempt.eventOracleFailureObserved = false;
                attempt.status = Status::observation_unavailable;
                attempt.obligations.emplace_back("v4_native_restricted_attempt_input_graph_and_completion_unverified");
                receipt.execution_ = std::move(attempt);
            }
            else receipt.execution_ = Run(identity, verified, present, selected, points, desktop, control, bounds, trace, pinnedEpoch, bound);
            if (multiBeforeBeginEntered && !multiBeforeBeginCompleted && !receipt.execution_.productionStarted)
                receipt.execution_.productionEntryUnknown = true;
            if (receipt.desktopKey_ && receipt.execution_.productionStarted && receipt.desktopKey_->published_ &&
                receipt.desktopKey_->phase_ == V4NativeDesktopExecutionKey::Phase::closed &&
                receipt.SameCandidate(identity, verified, present, selected) && receipt.SameHost(pinnedEpoch, trace, desktop, control))
                receipt.desktopKey_->committed_ = true;
            if (receipt.memberKey_ && receipt.execution_.productionStarted && receipt.memberKey_->published_ &&
                receipt.memberKey_->phase_ == V4NativeMemberExecutionKey::Phase::closed &&
                receipt.SameCandidate(identity, verified, present, selected) && receipt.SameHost(pinnedEpoch, trace, desktop, control))
                receipt.memberKey_->committed_ = true;
            if (receipt.categoryKey_ && receipt.execution_.productionStarted && receipt.categoryKey_->published_ &&
                receipt.categoryKey_->phase_ == V4NativeCategoryExecutionKey::Phase::closed &&
                receipt.SameCandidate(identity, verified, present, selected) && receipt.SameHost(pinnedEpoch, trace, desktop, control))
                receipt.categoryKey_->committed_ = true;
            if (receipt.reorderKey_ && receipt.execution_.productionStarted && receipt.reorderKey_->published_ &&
                receipt.reorderKey_->phase_ == V4NativeReorderExecutionKey::Phase::closed &&
                receipt.SameCandidate(identity, verified, present, selected) && receipt.SameHost(pinnedEpoch, trace, desktop, control))
                receipt.reorderKey_->committed_ = true;
            if (receipt.multiDragKey_ && receipt.execution_.productionStarted && !receipt.execution_.productionEntryUnknown &&
                receipt.multiDragKey_->published_ && receipt.multiDragKey_->phase_ == V4NativeMultiDragExecutionKey::Phase::closed &&
                pinnedPreparation->Consumed() && pinnedPreparation->SameCandidate(identity, verified, present, selected) &&
                pinnedPreparation->SameHost(pinnedEpoch, trace, desktop, control) &&
                receipt.SameCandidate(identity, verified, present, selected) && receipt.SameHost(pinnedEpoch, trace, desktop, control))
                receipt.multiDragKey_->committed_ = true;
        }
        catch (const std::exception& error)
        { receipt.execution_.productionEntryUnknown = called && !receipt.execution_.productionStarted;
          receipt.execution_.status = Status::observation_unavailable; receipt.execution_.additionalObservationUnavailable = true;
          receipt.execution_.failure = error.what(); }
        catch (...)
        { receipt.execution_.productionEntryUnknown = called && !receipt.execution_.productionStarted;
          receipt.execution_.status = Status::observation_unavailable; receipt.execution_.additionalObservationUnavailable = true;
          receipt.execution_.failure = "v4_native_receipt_observation_unavailable"; }
        return receipt;
    }
    static Result Run(const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
        const std::vector<bool>& present, const event_graph::V4NativeJoinedGraphVariant& selected,
        const std::vector<NativePointInput>& points, HWND desktop, HWND control, RECT bounds,
        const MessageTrace& trace, const std::shared_ptr<NativeHostEpoch>& epoch, const NativeHostBoundaries& host)
    {
        const auto pinnedEpoch = epoch;
        Result result;
        bool dispatchEntered = false;
        try
        {
            const auto plan = BuildV4NativeExecutionPlan(identity, verified, present, selected, points, desktop, bounds);
            result.obligations = plan.preconditions;
            result.postconditionsPending = plan.postconditions;
            result.unclassifiedObligations = plan.unclassified;
            if (!pinnedEpoch || !pinnedEpoch->Matches(&trace, desktop, control)) result.obligations.emplace_back("v4_native_host_epoch_unavailable");
            if (!host.beforeDeviceBegin || !host.observeDeviceState || !host.observeCapture || !host.awaitCompletion ||
                !host.beforePointerSessionEnd || !host.observePointerSessionEnd || !host.assertBusinessResults ||
                !host.throwAsync || !host.productionInputEntered || !host.beforeQueuedDelivery)
                result.obligations.emplace_back("v4_native_host_observers_missing");
            if (!plan.graphIdentityValid || !plan.sourcePoint || !result.obligations.empty()) return result;
            if (!pinnedEpoch->TryBeginExecution(&trace, desktop, control))
            { result.status = Status::observation_unavailable; result.failure = "v4_native_host_execution_lease_unavailable"; return result; }
            const auto endEpoch = [](const NativeHostEpoch* active) noexcept { active->EndExecution(); };
            const std::unique_ptr<const NativeHostEpoch, decltype(endEpoch)> guard(pinnedEpoch.get(), endEpoch);
            const auto program = verified.TransferProgram();
            dispatchEntered = true;
            return NativeSequence::DispatchPrepared(std::move(result), program, plan.actions, plan.points,
                *plan.sourcePoint, desktop, bounds, host);
        }
        catch (const std::exception& error)
        { result.productionEntryUnknown = dispatchEntered; result.status = Status::observation_unavailable; result.additionalObservationUnavailable = true; result.failure = error.what(); }
        catch (...)
        { result.productionEntryUnknown = dispatchEntered; result.status = Status::observation_unavailable; result.additionalObservationUnavailable = true; result.failure = "v4_native_execution_observation_unavailable"; }
        return result;
    }
};

// A local positive observation is tied to one device token and the actual
// admitted WndProc entry/return. Missing records do not establish absence.
struct V4NativeGraphNodeObservation
{
    std::string id;
    bool selectedPresent = false;
    std::optional<std::size_t> selectedClass;
    bool observedLocal = false;
    std::size_t deviceIndex = MessageTrace::InvalidToken;
    std::size_t traceIndex = MessageTrace::InvalidToken;
    std::uint64_t sequence = 0;
};
struct V4NativeGraphObservation
{
    bool identityValid = false, observationUnavailable = true;
    bool hypothesisObservationRecorded = false, hypothesisNotMetObserved = false;
    std::vector<V4NativeGraphNodeObservation> nodes;
    // These are value copies from the private execution receipt, not caller
    // assertions. They remain useful even if a later comparison is unavailable.
    std::vector<V4NativeDeviceRecord> deviceRecords;
    std::vector<V4NativeQueuedRecord> queuedRecords;
    std::vector<V4NativeTraceRecord> traceRecords;
    std::vector<std::string> unresolved;
};

inline V4NativeGraphObservation InspectV4NativeGraph(const V4NativeRunReceipt& receipt,
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const event_graph::V4NativeJoinedGraphVariant& selected,
    const std::shared_ptr<NativeHostEpoch>& epoch, const MessageTrace& trace, HWND desktop, HWND control)
{
    using namespace snowdesktop::private_pointer;
    V4NativeGraphObservation out;
    try
    {
        if (!receipt.SameCandidate(identity, verified, present, selected) ||
            !receipt.SameHost(epoch, trace, desktop, control) || trace.LostOrCrossThread())
        { out.unresolved.emplace_back("v4_native_graph_receipt_or_host_unavailable"); return out; }
        const auto mapping = event_graph::MapV4NativePresenceCandidate(identity, verified);
        const auto replay = event_graph::V4NativeJoinedGraph::Replay(identity, verified, present, selected.id);
        if (!mapping.graphAvailable || !replay.identityValid || !replay.errors.empty() ||
            replay.variant.id != selected.id || replay.variant.classes != selected.classes ||
            replay.variant.order != selected.order || replay.variant.unresolved != selected.unresolved)
        { out.unresolved.emplace_back("v4_native_graph_replay_unavailable"); return out; }
        auto requirements = mapping.spec.requirements;
        std::sort(requirements.begin(), requirements.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        if (requirements.size() != present.size())
        { out.unresolved.emplace_back("v4_native_graph_presence_size_unavailable"); return out; }
        std::map<std::string, std::size_t> indexById;
        std::size_t active = 0;
        for (std::size_t i = 0; i < requirements.size(); ++i)
        {
            if (!indexById.emplace(requirements[i].id, i).second || (present[i] && active >= selected.classes.size()))
            { out.unresolved.emplace_back("v4_native_graph_partition_unavailable"); return out; }
            V4NativeGraphNodeObservation node; node.id = requirements[i].id; node.selectedPresent = present[i];
            if (present[i]) node.selectedClass = selected.classes[active++];
            out.nodes.push_back(std::move(node));
        }
        if (active != selected.classes.size())
        { out.unresolved.emplace_back("v4_native_graph_partition_unavailable"); return out; }
        std::map<std::size_t, std::size_t> rank;
        for (std::size_t i = 0; i < selected.order.size(); ++i)
        {
            const auto found = indexById.find(selected.order[i]);
            if (found == indexById.end() || !out.nodes[found->second].selectedClass ||
                !rank.emplace(*out.nodes[found->second].selectedClass, i).second)
            { out.unresolved.emplace_back("v4_native_graph_order_unavailable"); return out; }
        }
        out.deviceRecords = receipt.DeviceRecords();
        out.queuedRecords = receipt.QueuedRecords();
        out.traceRecords = receipt.TraceRecords();
        out.identityValid = true;
        const auto moveClasses = V4NativeMoveClassEvidence::Inspect(identity, verified, present, selected);
        const auto program = verified.TransferProgram();
        std::set<std::string> ambiguousNodes;
        const auto sameToken = [](const QueuedEventIdentity& a, const QueuedEventIdentity& b) {
            return a.ownerThread == b.ownerThread && a.session == b.session && a.event == b.event;
        };
        for (std::size_t n = 0; n < out.deviceRecords.size(); ++n)
        {
            const auto& record = out.deviceRecords[n];
            if (record.phase == NativeDevicePhase::session_end) continue;
            if (!record.queued || !record.expected || record.delivery.failed || record.trace.size() > trace.Observations().size())
            { out.unresolved.emplace_back("v4_native_graph_device_snapshot_unavailable"); continue; }
            const auto& token = *record.queued;
            const V4NativeQueuedRecord* queue = nullptr;
            for (const auto& q : out.queuedRecords)
                if (sameToken(q.token, token)) { if (queue) { queue = nullptr; break; } queue = &q; }
            if (!queue || queue->phase != record.phase)
            { out.unresolved.emplace_back("v4_native_graph_queue_token_ambiguous"); continue; }
            std::string base;
            if (record.phase == NativeDevicePhase::initial_hover && !record.programIndex && !queue->programIndex)
                base = "v4_native_initial_hover";
            else if (record.phase == NativeDevicePhase::program_input && record.programIndex && queue->programIndex &&
                *record.programIndex < program.steps.size() && *queue->programIndex < program.steps.size())
            {
                const auto i = *record.programIndex;
                if (i != *queue->programIndex)
                {
                    const auto& classes = moveClasses.ByProgramIndex();
                    if (!moveClasses.IdentityValid() || !moveClasses.CompleteMoveBindings() || i >= classes.size() ||
                        *queue->programIndex >= classes.size() || !classes[i] || !classes[*queue->programIndex] ||
                        classes[i] != classes[*queue->programIndex])
                    { out.unresolved.emplace_back("v4_native_graph_shared_move_binding_unavailable"); continue; }
                }
                base = "program_" + std::to_string(i);
            }
            else { out.unresolved.emplace_back("v4_native_graph_program_binding_unavailable"); continue; }
            POINT now = record.expected->screen;
            if (!ScreenToClient(desktop, &now) || now.x != record.expected->clientBeforeDispatch.x ||
                now.y != record.expected->clientBeforeDispatch.y)
            { out.unresolved.emplace_back("v4_native_graph_coordinate_mapping_changed"); continue; }
            const auto lp = MAKELPARAM(static_cast<short>(now.x), static_cast<short>(now.y));
            std::optional<DeliveryIdentity> delivery;
            bool ambiguous = false;
            for (const auto& item : record.delivery.entries)
                if (item.kind == DeliveryObservationKind::begin && item.identity.queuedEvent && !item.identity.forwarded &&
                    !item.identity.forward && item.identity.ownerThread == token.ownerThread &&
                    item.identity.session == token.session && item.identity.event == token.event)
                { if (delivery || !item.identity.delivery) { ambiguous = true; break; } delivery = item.identity; }
            if (ambiguous || !delivery)
            { out.unresolved.emplace_back("v4_native_graph_delivery_identity_unavailable"); continue; }
            const auto matched = InspectNativeDelivery(trace, 0, &record.delivery, *delivery, desktop,
                MessageRoute::desktop_window_proc, record.expected->message, record.expected->buttons, lp);
            if (!matched.matchedMessageAndPermit || matched.traceIndex >= record.trace.size())
            { out.unresolved.emplace_back("v4_native_graph_wndproc_delivery_unavailable"); continue; }
            const auto& sealed = record.trace[matched.traceIndex];
            const auto& current = trace.Observations()[matched.traceIndex];
            if (sealed.enteredSequence != current.enteredSequence || sealed.returnedSequence != current.returnedSequence ||
                sealed.window != current.window || sealed.message != current.message || sealed.wParam != current.wParam ||
                sealed.inputLParam != current.inputLParam || !sealed.returned || sealed.windowResult != current.windowResult ||
                sealed.enteredThread != token.ownerThread || sealed.returnedThread != token.ownerThread)
            { out.unresolved.emplace_back("v4_native_graph_sealed_trace_unavailable"); continue; }
            for (const auto& suffix : {std::string("_wndproc_enter"), std::string("_wndproc_return")})
            {
                const auto found = indexById.find(base + suffix);
                if (found == indexById.end()) { out.unresolved.emplace_back("v4_native_graph_node_unavailable"); continue; }
                auto& node = out.nodes[found->second];
                const auto seq = suffix == "_wndproc_enter" ? sealed.enteredSequence : sealed.returnedSequence;
                if (ambiguousNodes.contains(node.id) || (node.observedLocal && node.sequence != seq))
                { node.observedLocal = false; ambiguousNodes.insert(node.id);
                  out.unresolved.emplace_back("v4_native_graph_duplicate_occurrence_unavailable"); continue; }
                node.observedLocal = true; node.deviceIndex = n; node.traceIndex = matched.traceIndex; node.sequence = seq;
            }
        }
        for (const auto& node : out.nodes)
            if (node.observedLocal)
            { out.hypothesisObservationRecorded = true; if (!node.selectedPresent) out.hypothesisNotMetObserved = true; }
        for (std::size_t i = 0; i < out.nodes.size(); ++i)
            for (std::size_t j = i + 1; j < out.nodes.size(); ++j)
            {
                const auto& a = out.nodes[i]; const auto& b = out.nodes[j];
                if (!a.observedLocal || !b.observedLocal || !a.selectedClass || !b.selectedClass ||
                    !rank.contains(*a.selectedClass) || !rank.contains(*b.selectedClass)) continue;
                out.hypothesisObservationRecorded = true;
                if ((*a.selectedClass == *b.selectedClass && a.sequence != b.sequence) ||
                    (*a.selectedClass != *b.selectedClass &&
                        (a.sequence == b.sequence ||
                         ((rank.at(*a.selectedClass) < rank.at(*b.selectedClass)) != (a.sequence < b.sequence)))))
                    out.hypothesisNotMetObserved = true;
            }
        out.unresolved.emplace_back("v4_native_graph_unobserved_presence_and_non_device_reentrancy_unavailable");
        out.unresolved.emplace_back("v4_native_graph_capture_cleanup_layout_completion_and_business_unattributed");
    }
    catch (...) { out.observationUnavailable = true; }
    return out;
}

struct V4NativeReorderProductionObservation
{
    bool identityValid = false, releaseScopedCommitObserved = false;
    bool observationUnavailable = true, resourceUnavailable = false;
    std::uint64_t commitId = 0;
    std::size_t releaseTraceIndex = MessageTrace::InvalidToken;
    std::vector<V4NativeLayoutRecord> records;
    std::vector<std::string> unresolved;
};

// Only bind a production reorder marker to the actual returned release
// delivery. The preview's indexed key is a model value, not Slot::Item
// identity, and a return marker is not a business or durability verdict.
inline V4NativeReorderProductionObservation InspectV4NativeReorderProduction(
    const V4NativeRunReceipt& receipt, const event_graph::V4FamilyIdentityContext& identity,
    const V4FamilyValidatedContext& verified, const std::vector<bool>& present,
    const event_graph::V4NativeJoinedGraphVariant& selected, const std::shared_ptr<NativeHostEpoch>& epoch,
    const MessageTrace& trace, HWND desktop, HWND control)
{
    namespace diagnostic = snowdesktop::external_transfer_observation;
    using namespace snowdesktop::private_pointer;
    V4NativeReorderProductionObservation out;
    try
    {
        const bool complete = diagnostic::Complete();
        diagnostic::ReadSession session;
        if (!session.Open(true)) { out.unresolved.emplace_back("v4_native_reorder_session_unavailable"); return out; }
        for (std::size_t i = 0; i < session.Count(); ++i)
        {
            diagnostic::Event event;
            if (!session.ReadEvent(i, event)) { out.unresolved.emplace_back("v4_native_reorder_record_unavailable"); return out; }
            if (event.phase < diagnostic::Phase::collection_preview_slot ||
                event.phase > diagnostic::Phase::collection_reorder_return) continue;
            V4NativeLayoutRecord record{i, event, {}};
            if (event.hasBlob)
            {
                if (!CopyV4DiagnosticBlob(session, record))
                { out.unresolved.emplace_back("v4_native_reorder_blob_unavailable"); return out; }
            }
            out.records.push_back(std::move(record));
        }
        if (!complete || !diagnostic::Complete())
        { out.unresolved.emplace_back("v4_native_reorder_diagnostic_incomplete"); return out; }
        if (!receipt.SameCandidate(identity, verified, present, selected) ||
            !receipt.SameHost(epoch, trace, desktop, control) || !receipt.SealedReorderBusiness())
        { out.unresolved.emplace_back("v4_native_reorder_receipt_or_host_unavailable"); return out; }
        out.identityValid = true;
        const auto layout = InspectV4NativeLayoutSave(receipt, identity, verified, present, selected, epoch, trace, desktop, control);
        if (layout.releaseTraceIndex == MessageTrace::InvalidToken || layout.releaseTraceIndex >= trace.Observations().size())
        { out.unresolved.emplace_back("v4_native_reorder_release_delivery_unavailable"); return out; }
        const auto program = verified.TransferProgram();
        const V4NativeDeviceRecord* release = nullptr;
        for (const auto& record : receipt.DeviceRecords())
            if (record.phase == NativeDevicePhase::program_input && record.programIndex &&
                *record.programIndex < program.steps.size() && program.steps[*record.programIndex].kind == "release")
            { if (release) { out.unresolved.emplace_back("v4_native_reorder_release_ambiguous"); return out; } release = &record; }
        if (!release || !release->queued) { out.unresolved.emplace_back("v4_native_reorder_release_token_unavailable"); return out; }
        const auto& token = *release->queued;
        const DeliveryIdentity* delivery = nullptr;
        for (const auto& event : release->delivery.entries)
            if (event.kind == DeliveryObservationKind::begin && event.identity.queuedEvent && !event.identity.forwarded &&
                !event.identity.forward && event.identity.ownerThread == token.ownerThread &&
                event.identity.session == token.session && event.identity.event == token.event)
            { if (delivery) { out.unresolved.emplace_back("v4_native_reorder_delivery_ambiguous"); return out; } delivery = &event.identity; }
        const auto& message = trace.Observations()[layout.releaseTraceIndex];
        if (!delivery || !delivery->delivery || !message.windowScope || !message.returned ||
            layout.releaseTraceIndex >= release->trace.size())
        { out.unresolved.emplace_back("v4_native_reorder_release_scope_unavailable"); return out; }
        const auto& sealedMessage = release->trace[layout.releaseTraceIndex];
        if (!sealedMessage.returned || sealedMessage.windowScope != message.windowScope ||
            sealedMessage.enteredSequence != message.enteredSequence || sealedMessage.returnedSequence != message.returnedSequence ||
            sealedMessage.window != message.window || sealedMessage.message != message.message ||
            sealedMessage.wParam != message.wParam || sealedMessage.inputLParam != message.inputLParam ||
            sealedMessage.windowResult != message.windowResult)
        { out.unresolved.emplace_back("v4_native_reorder_sealed_release_trace_unavailable"); return out; }
        const V4NativeLayoutRecord *entered = nullptr, *returned = nullptr;
        for (const auto& record : out.records)
        {
            const auto& event = record.event; const auto& scope = event.nativeScope;
            if (event.phase != diagnostic::Phase::collection_reorder_enter) continue;
            if (scope.session != delivery->session || scope.event != delivery->event || scope.delivery != delivery->delivery ||
                scope.ownerThread != delivery->ownerThread || scope.forwarded || scope.forward || !scope.queuedEvent ||
                event.thread != delivery->ownerThread || event.windowScope != message.windowScope) continue;
            if (entered) { out.unresolved.emplace_back("v4_native_reorder_commit_ambiguous"); return out; }
            entered = &record;
        }
        if (!entered || !entered->event.object || !entered->event.extra)
        { out.unresolved.emplace_back("v4_native_reorder_commit_unavailable"); return out; }
        for (const auto& record : out.records)
            if (record.event.phase == diagnostic::Phase::collection_reorder_return &&
                record.event.extra == entered->event.extra && record.event.object == entered->event.object &&
                record.event.thread == entered->event.thread && record.event.windowScope == entered->event.windowScope &&
                record.event.nativeScope.session == delivery->session && record.event.nativeScope.event == delivery->event &&
                record.event.nativeScope.delivery == delivery->delivery && record.event.nativeScope.ownerThread == delivery->ownerThread &&
                record.event.nativeScope.queuedEvent && !record.event.nativeScope.forwarded && !record.event.nativeScope.forward)
            { if (returned) { out.unresolved.emplace_back("v4_native_reorder_return_ambiguous"); return out; } returned = &record; }
        if (!returned || returned->index <= entered->index ||
            std::any_of(out.records.begin(), out.records.end(), [&](const auto& record) {
                return record.event.phase == diagnostic::Phase::collection_reorder_truncated &&
                    record.event.extra == entered->event.extra; }))
        { out.unresolved.emplace_back("v4_native_reorder_return_or_values_incomplete"); return out; }
        out.commitId = entered->event.extra; out.releaseTraceIndex = layout.releaseTraceIndex;
        out.releaseScopedCommitObserved = true;
        out.unresolved.emplace_back("v4_native_reorder_slot_item_preview_identity_and_reentry_unverified");
        out.unresolved.emplace_back("v4_native_reorder_business_layout_durability_and_late_work_unverified");
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; }
    catch (const std::length_error&) { out.resourceUnavailable = true; }
    catch (...) { out.observationUnavailable = true; }
    return out;
}

struct V4NativeCategoryCallObservation
{
    struct Occurrence
    {
        struct Rule
        {
            std::size_t index = 0, declarationIndex = 0, idIndex = 0, labelIndex = 0, extensionsIndex = 0, enabledIndex = 0;
            std::optional<bool> idMatches, labelMatches, extensionsMatches, enabledMatches;
        };
        std::uint64_t callId = 0;
        std::int64_t site = 0, branch = 0;
        std::uintptr_t widget = 0, item = 0, settings = 0;
        std::size_t enterIndex = 0, returnIndex = 0, leaveIndex = 0, traceIndex = MessageTrace::InvalidToken;
        bool windowScopedInternalReturnObserved = false;
        std::optional<bool> ruleCountMatches;
        std::vector<Rule> rules;
    };
    bool identityValid = false, observationUnavailable = true, resourceUnavailable = false;
    std::vector<Occurrence> occurrences;
    std::vector<V4NativeLayoutRecord> records;
    std::vector<std::string> unresolved;
};

// A real classification call can be correlated with its actual WndProc scope
// in this receipt's completion trace. The marker precedes the C++ return;
// object addresses and that scope do not prove target membership, source file
// identity, business outcome, absence of reentry, or all late work complete.
inline V4NativeCategoryCallObservation InspectV4NativeCategoryCalls(const V4NativeRunReceipt& receipt,
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const event_graph::V4NativeJoinedGraphVariant& selected,
    const std::shared_ptr<NativeHostEpoch>& epoch, const MessageTrace& trace, HWND desktop, HWND control)
{
    namespace diagnostic = snowdesktop::external_transfer_observation;
    V4NativeCategoryCallObservation out;
    try
    {
        const bool complete = diagnostic::Complete();
        diagnostic::ReadSession session;
        if (!session.Open(true)) { out.unresolved.emplace_back("v4_category_call_session_unavailable"); return out; }
        for (std::size_t i = 0; i < session.Count(); ++i)
        {
            diagnostic::Event event;
            if (!session.ReadEvent(i, event)) { out.unresolved.emplace_back("v4_category_call_record_unavailable"); return out; }
            if (event.phase < diagnostic::Phase::category_classify_enter || event.phase > diagnostic::Phase::category_classify_unwound) continue;
            V4NativeLayoutRecord record{i, event, {}};
            if (event.hasBlob)
            {
                if (!CopyV4DiagnosticBlob(session, record))
                { out.unresolved.emplace_back("v4_category_call_blob_unavailable"); return out; }
            }
            out.records.push_back(std::move(record));
        }
        if (!complete || !diagnostic::Complete()) { out.unresolved.emplace_back("v4_category_call_log_incomplete"); return out; }
        if (!receipt.SameCandidate(identity, verified, present, selected) ||
            !receipt.SameHost(epoch, trace, desktop, control) || !receipt.HostIdentity() || trace.LostOrCrossThread())
        { out.unresolved.emplace_back("v4_category_call_receipt_or_host_unavailable"); return out; }
        const V4NativeTraceRecord* completed = nullptr;
        for (const auto& value : receipt.TraceRecords())
            if (value.boundary == "completion_after")
            { if (completed) { out.unresolved.emplace_back("v4_category_call_completion_trace_ambiguous"); return out; } completed = &value; }
        if (!completed || completed->messages.size() > trace.Observations().size())
        { out.unresolved.emplace_back("v4_category_call_completion_trace_unavailable"); return out; }
        out.identityValid = true;
        const auto& axes = verified.Axes(); const auto& candidate = verified.Coordinate();
        const auto input = [&](std::string_view id) -> std::optional<std::string_view> {
            for (std::size_t i = 0; i < axes.size(); ++i)
                if (axes[i].id == id && i < candidate.values.size()) return candidate.values[i];
            return std::nullopt;
        };
        const auto compareText = [](std::optional<std::string_view> expected, const V4NativeLayoutRecord& actual) -> std::optional<bool> {
            constexpr std::string_view prefix = "utf16le_hex_";
            if (!expected || !expected->starts_with(prefix)) return std::nullopt;
            const auto hex = expected->substr(prefix.size());
            if (!actual.event.hasBlob || actual.blob.size() != actual.event.blobSize || hex.size() % 2 || hex.size() / 2 != actual.blob.size())
                return false;
            constexpr char digits[] = "0123456789abcdef";
            for (std::size_t n = 0; n < actual.blob.size(); ++n)
            { const auto byte = std::to_integer<unsigned>(actual.blob[n]);
              if (hex[n * 2] != digits[byte >> 4] || hex[n * 2 + 1] != digits[byte & 15]) return false; }
            return true;
        };
        const auto compareFlag = [](std::optional<std::string_view> expected, std::int64_t actual) -> std::optional<bool> {
            if (!expected || (actual != 0 && actual != 1)) return std::nullopt;
            if (*expected == "yes") return actual == 1;
            if (*expected == "no") return actual == 0;
            return std::nullopt;
        };
        std::set<std::uint64_t> calls;
        for (const auto& start : out.records)
        {
            const auto& e = start.event;
            if (e.phase != diagnostic::Phase::category_classify_enter || e.object != receipt.HostIdentity()) continue;
            if (!e.extra || !e.windowScope || !e.thread || e.value < 1 || e.value > 3 || !calls.insert(e.extra).second ||
                std::count_if(out.records.begin(), out.records.end(), [&](const auto& item) {
                    return item.event.extra == e.extra && item.event.phase == diagnostic::Phase::category_classify_enter; }) != 1)
            { out.unresolved.emplace_back("v4_category_call_enter_ambiguous"); continue; }
            const V4NativeLayoutRecord *returned = nullptr, *leave = nullptr, *widget = nullptr, *item = nullptr, *settings = nullptr, *result = nullptr;
            bool ambiguous = false;
            for (const auto& record : out.records)
            {
                if (record.event.extra != e.extra) continue;
                const auto& adapter = record.event.adapterScope; const auto& enteredAdapter = e.adapterScope;
                const auto& observedNative = record.event.nativeScope; const auto& enteredNative = e.nativeScope;
                // RecordBlob deliberately has no native scope. Its association
                // is only the unique call ID, thread, window scope and bounded
                // interval; do not treat it as a native-delivery observation.
                const bool nativeScopeValid = record.event.hasBlob ?
                    (!observedNative.session && !observedNative.event && !observedNative.delivery && !observedNative.forward &&
                        !observedNative.ownerThread && !observedNative.forwarded && !observedNative.queuedEvent) :
                    (observedNative.session == enteredNative.session && observedNative.event == enteredNative.event &&
                        observedNative.delivery == enteredNative.delivery && observedNative.forward == enteredNative.forward &&
                        observedNative.ownerThread == enteredNative.ownerThread && observedNative.forwarded == enteredNative.forwarded &&
                        observedNative.queuedEvent == enteredNative.queuedEvent);
                if (record.event.phase == diagnostic::Phase::category_classify_unwound ||
                    record.event.phase == diagnostic::Phase::category_classify_rules_truncated ||
                    !nativeScopeValid || record.event.thread != e.thread || record.event.windowScope != e.windowScope ||
                    adapter.observed != enteredAdapter.observed || adapter.session != enteredAdapter.session ||
                    adapter.call != enteredAdapter.call || adapter.parent != enteredAdapter.parent || adapter.adapter != enteredAdapter.adapter ||
                    adapter.thread != enteredAdapter.thread || adapter.depth != enteredAdapter.depth || adapter.kind != enteredAdapter.kind)
                { ambiguous = true; continue; }
                const V4NativeLayoutRecord** slot = nullptr;
                if (record.event.phase == diagnostic::Phase::category_classify_return) slot = &returned;
                else if (record.event.phase == diagnostic::Phase::category_classify_leave) slot = &leave;
                else if (record.event.phase == diagnostic::Phase::category_classify_widget) slot = &widget;
                else if (record.event.phase == diagnostic::Phase::category_classify_item) slot = &item;
                else if (record.event.phase == diagnostic::Phase::category_classify_settings) slot = &settings;
                else if (record.event.phase == diagnostic::Phase::category_classify_result) slot = &result;
                if (slot) { if (*slot) ambiguous = true; else *slot = &record; }
            }
            if (ambiguous || !returned || !leave || !widget || !item || !settings || !result ||
                !widget->event.object || !item->event.object || !settings->event.object || !result->event.hasBlob ||
                returned->event.object != e.object || leave->event.object != e.object || result->event.object != e.object ||
                returned->event.value < 1 || returned->event.value > 4 || returned->index <= result->index ||
                leave->index <= returned->index ||
                result->index <= start.index || widget->index <= start.index || item->index <= start.index || settings->index <= start.index ||
                widget->index >= returned->index || item->index >= returned->index || settings->index >= returned->index)
            { out.unresolved.emplace_back("v4_category_call_interval_unavailable"); continue; }
            if (std::any_of(out.records.begin(), out.records.end(), [&](const auto& record) {
                return record.event.extra == e.extra && (record.index < start.index || record.index > leave->index ||
                    (record.index > returned->index && record.event.phase != diagnostic::Phase::category_classify_leave)); }))
            { out.unresolved.emplace_back("v4_category_call_interleaved_record_unavailable"); continue; }
            const auto required = [&](diagnostic::Phase phase, std::uintptr_t object, bool blob) {
                const V4NativeLayoutRecord* found = nullptr;
                for (const auto& record : out.records)
                    if (record.event.extra == e.extra && record.event.phase == phase)
                    { if (found) return false; found = &record; }
                return found && found->event.object == object && found->event.hasBlob == blob &&
                    (!blob || (found->blob.size() == found->event.blobSize && found->blob.size() % sizeof(wchar_t) == 0));
            };
            if (!required(diagnostic::Phase::category_classify_settings_flags, settings->event.object, false) ||
                !required(diagnostic::Phase::category_classify_item_key, item->event.object, true) ||
                !required(diagnostic::Phase::category_classify_item_name, item->event.object, true) ||
                !required(diagnostic::Phase::category_classify_item_path, item->event.object, true) ||
                !required(diagnostic::Phase::category_classify_item_clsid, item->event.object, true) ||
                !required(diagnostic::Phase::category_classify_shortcut_extension, item->event.object, true) ||
                !required(diagnostic::Phase::category_classify_namespace, e.object, true) ||
                result->blob.size() != result->event.blobSize || result->blob.size() % sizeof(wchar_t) != 0 ||
                (returned->event.value != 1 &&
                    (!required(diagnostic::Phase::category_classify_extension, e.object, true) ||
                     !required(diagnostic::Phase::category_classify_folder, e.object, false))) ||
                (returned->event.value >= 3 && !required(diagnostic::Phase::category_classify_type, e.object, true)))
            { out.unresolved.emplace_back("v4_category_call_input_snapshot_unavailable"); continue; }
            const auto& scope = e.nativeScope; const auto& endScope = returned->event.nativeScope;
            if (scope.session != endScope.session || scope.event != endScope.event || scope.delivery != endScope.delivery ||
                scope.forward != endScope.forward || scope.ownerThread != endScope.ownerThread ||
                scope.forwarded != endScope.forwarded || scope.queuedEvent != endScope.queuedEvent)
            { out.unresolved.emplace_back("v4_category_call_native_scope_changed"); continue; }
            std::optional<std::size_t> traceIndex;
            for (std::size_t i = 0; i < completed->messages.size(); ++i)
                if (completed->messages[i].windowScope == e.windowScope)
                { if (traceIndex) { ambiguous = true; break; } traceIndex = i; }
            if (ambiguous || !traceIndex) { out.unresolved.emplace_back("v4_category_call_window_scope_unavailable"); continue; }
            const auto& saved = completed->messages[*traceIndex]; const auto& current = trace.Observations()[*traceIndex];
            const bool actualWindow = (saved.route == MessageRoute::desktop_window_proc && saved.window == desktop) ||
                (saved.route == MessageRoute::control_window_proc && saved.window == control);
            if (!actualWindow || !saved.returned || !current.returned || saved.enteredThread != e.thread ||
                saved.returnedThread != e.thread || saved.enteredSequence >= saved.returnedSequence ||
                saved.enteredSequence != current.enteredSequence || saved.returnedSequence != current.returnedSequence ||
                saved.windowScope != current.windowScope || saved.route != current.route || saved.window != current.window ||
                saved.message != current.message || saved.wParam != current.wParam || saved.windowResult != current.windowResult ||
                saved.inputLParam != current.inputLParam || saved.delivery.session != scope.session ||
                saved.delivery.event != scope.event || saved.delivery.delivery != scope.delivery || saved.delivery.forward != scope.forward ||
                saved.delivery.ownerThread != scope.ownerThread || saved.delivery.forwarded != scope.forwarded ||
                saved.delivery.queuedEvent != scope.queuedEvent || saved.enteredThread != current.enteredThread ||
                saved.returnedThread != current.returnedThread)
            { out.unresolved.emplace_back("v4_category_call_window_return_unavailable"); continue; }
            V4NativeCategoryCallObservation::Occurrence occurrence;
            occurrence.callId = e.extra; occurrence.site = e.value; occurrence.branch = returned->event.value;
            occurrence.widget = widget->event.object; occurrence.item = item->event.object; occurrence.settings = settings->event.object;
            occurrence.enterIndex = start.index; occurrence.returnIndex = returned->index; occurrence.leaveIndex = leave->index;
            occurrence.traceIndex = *traceIndex;
            occurrence.windowScopedInternalReturnObserved = true;
            // Rule values are compared as raw bytes only. There is no test-side
            // classifier and no assertion that this item belongs to the case.
            if (settings->event.value >= 0 && static_cast<std::uint64_t>(settings->event.value) <= diagnostic::EventLimit() / 5)
            {
                const auto count = static_cast<std::size_t>(settings->event.value);
                std::size_t declared = 0;
                for (const auto& axis : axes)
                    if (axis.id.starts_with("V4.category_rules.rule_") && axis.id.ends_with(".id")) ++declared;
                if (input("V4.category_rules.kind") == std::optional<std::string_view>{"exact"})
                    occurrence.ruleCountMatches = count == declared;
                bool rulesComplete = true;
                occurrence.rules.reserve(count);
                std::set<std::uintptr_t> ruleObjects;
                if (static_cast<std::size_t>(std::count_if(out.records.begin(), out.records.end(), [&](const auto& record) {
                    return record.event.extra == e.extra && record.event.phase == diagnostic::Phase::category_classify_rule; })) != count)
                    rulesComplete = false;
                for (std::size_t n = 0; n < count; ++n)
                {
                    const V4NativeLayoutRecord *entry = nullptr, *enabled = nullptr, *id = nullptr, *label = nullptr, *extensions = nullptr;
                    for (const auto& record : out.records)
                        if (record.event.extra == e.extra && record.event.phase == diagnostic::Phase::category_classify_rule &&
                            record.event.value == static_cast<std::int64_t>(n))
                        { if (entry) { rulesComplete = false; break; } entry = &record; }
                    if (!entry || !entry->event.object || !ruleObjects.insert(entry->event.object).second)
                    { rulesComplete = false; break; }
                    for (const auto& record : out.records)
                    {
                        if (record.event.extra != e.extra || record.event.object != entry->event.object) continue;
                        const V4NativeLayoutRecord** slot = nullptr;
                        if (record.event.phase == diagnostic::Phase::category_classify_rule_enabled) slot = &enabled;
                        else if (record.event.phase == diagnostic::Phase::category_classify_rule_id) slot = &id;
                        else if (record.event.phase == diagnostic::Phase::category_classify_rule_label) slot = &label;
                        else if (record.event.phase == diagnostic::Phase::category_classify_rule_extensions) slot = &extensions;
                        if (slot) { if (*slot) rulesComplete = false; else *slot = &record; }
                    }
                    if (!rulesComplete || !enabled || !id || !label || !extensions || !id->event.hasBlob ||
                        !label->event.hasBlob || !extensions->event.hasBlob ||
                        id->blob.size() != id->event.blobSize || label->blob.size() != label->event.blobSize ||
                        extensions->blob.size() != extensions->event.blobSize || id->blob.size() % sizeof(wchar_t) ||
                        label->blob.size() % sizeof(wchar_t) || extensions->blob.size() % sizeof(wchar_t) ||
                        (enabled->event.value != 0 && enabled->event.value != 1) ||
                        entry->index >= enabled->index || entry->index >= id->index || entry->index >= label->index || entry->index >= extensions->index)
                    { rulesComplete = false; break; }
                    V4NativeCategoryCallObservation::Occurrence::Rule rule;
                    rule.index = n; rule.declarationIndex = entry->index; rule.enabledIndex = enabled->index;
                    rule.idIndex = id->index; rule.labelIndex = label->index; rule.extensionsIndex = extensions->index;
                    if (occurrence.ruleCountMatches && *occurrence.ruleCountMatches)
                    {
                        const auto base = "V4.category_rules.rule_" + std::to_string(n);
                        rule.idMatches = compareText(input(base + ".id"), *id);
                        rule.labelMatches = compareText(input(base + ".label"), *label);
                        rule.extensionsMatches = compareText(input(base + ".extensions"), *extensions);
                        rule.enabledMatches = compareFlag(input(base + ".enabled"), enabled->event.value);
                    }
                    occurrence.rules.push_back(std::move(rule));
                }
                if (!rulesComplete) { occurrence.ruleCountMatches.reset(); occurrence.rules.clear();
                    out.unresolved.emplace_back("v4_category_call_rule_snapshot_unavailable"); }
            }
            else out.unresolved.emplace_back("v4_category_call_rule_count_unavailable");
            out.occurrences.push_back(std::move(occurrence));
        }
        out.unresolved.emplace_back("v4_category_call_item_identity_and_candidate_membership_unverified");
        out.unresolved.emplace_back("v4_category_call_abi_return_reentry_business_and_late_work_unverified");
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; }
    catch (const std::length_error&) { out.resourceUnavailable = true; }
    catch (...) { out.observationUnavailable = true; }
    return out;
}

struct V4NativeCategoryDesktopCommitObservation
{
    bool identityValid = false, releaseScopedCommitObserved = false, observationUnavailable = true, resourceUnavailable = false;
    std::uint64_t commitId = 0;
    std::uintptr_t origin = 0;
    std::size_t releaseTraceIndex = MessageTrace::InvalidToken;
    std::optional<bool> applied, layoutRequestInsideCommit;
    std::vector<V4NativeLayoutRecord> records;
    std::vector<std::string> unresolved;
};

// Records the actual internal desktop operation, not a Shell file open or a
// completed/durable business result. Blob identity is established only by
// the unique operation ID and its same-thread returned window interval.
inline V4NativeCategoryDesktopCommitObservation InspectV4NativeCategoryDesktopCommit(
    const V4NativeRunReceipt& receipt, const event_graph::V4FamilyIdentityContext& identity,
    const V4FamilyValidatedContext& verified, const std::vector<bool>& present,
    const event_graph::V4NativeJoinedGraphVariant& selected, const std::shared_ptr<NativeHostEpoch>& epoch,
    const MessageTrace& trace, HWND desktop, HWND control)
{
    namespace diagnostic = snowdesktop::external_transfer_observation;
    using namespace snowdesktop::private_pointer;
    V4NativeCategoryDesktopCommitObservation out;
    try
    {
        const bool complete = diagnostic::Complete();
        diagnostic::ReadSession session;
        if (!session.Open(true)) { out.unresolved.emplace_back("v4_category_desktop_session_unavailable"); return out; }
        for (std::size_t i = 0; i < session.Count(); ++i)
        {
            diagnostic::Event event;
            if (!session.ReadEvent(i, event)) { out.unresolved.emplace_back("v4_category_desktop_record_unavailable"); return out; }
            if (event.phase < diagnostic::Phase::category_desktop_commit_enter || event.phase > diagnostic::Phase::category_desktop_commit_return) continue;
            V4NativeLayoutRecord record{i, event, {}};
            if (event.hasBlob)
            {
                if (!CopyV4DiagnosticBlob(session, record)) { out.unresolved.emplace_back("v4_category_desktop_blob_unavailable"); return out; }
            }
            out.records.push_back(std::move(record));
        }
        if (!complete || !diagnostic::Complete() || !receipt.SameCandidate(identity, verified, present, selected) ||
            !receipt.SameHost(epoch, trace, desktop, control) || !receipt.HostIdentity())
        { out.unresolved.emplace_back("v4_category_desktop_receipt_or_log_unavailable"); return out; }
        out.identityValid = true;
        const auto layout = InspectV4NativeLayoutSave(receipt, identity, verified, present, selected, epoch, trace, desktop, control);
        if (layout.releaseTraceIndex == MessageTrace::InvalidToken || layout.releaseTraceIndex >= trace.Observations().size())
        { out.unresolved.emplace_back("v4_category_desktop_release_unavailable"); return out; }
        const auto program = verified.TransferProgram();
        const V4NativeDeviceRecord* release = nullptr;
        for (const auto& value : receipt.DeviceRecords())
            if (value.phase == NativeDevicePhase::program_input && value.programIndex && *value.programIndex < program.steps.size() &&
                program.steps[*value.programIndex].kind == "release")
            { if (release) { out.unresolved.emplace_back("v4_category_desktop_release_ambiguous"); return out; } release = &value; }
        if (!release || !release->queued || layout.releaseTraceIndex >= release->trace.size())
        { out.unresolved.emplace_back("v4_category_desktop_release_token_unavailable"); return out; }
        const auto& token = *release->queued; const DeliveryIdentity* delivery = nullptr;
        for (const auto& value : release->delivery.entries)
            if (value.kind == DeliveryObservationKind::begin && value.identity.queuedEvent && !value.identity.forwarded &&
                !value.identity.forward && value.identity.ownerThread == token.ownerThread && value.identity.session == token.session &&
                value.identity.event == token.event)
            { if (delivery) { out.unresolved.emplace_back("v4_category_desktop_delivery_ambiguous"); return out; } delivery = &value.identity; }
        const auto& message = trace.Observations()[layout.releaseTraceIndex];
        const auto& saved = release->trace[layout.releaseTraceIndex];
        if (!delivery || !delivery->delivery || !message.windowScope || !message.returned || !saved.returned ||
            saved.windowScope != message.windowScope || saved.enteredSequence != message.enteredSequence ||
            saved.returnedSequence != message.returnedSequence || saved.window != message.window || saved.message != message.message ||
            saved.wParam != message.wParam || saved.inputLParam != message.inputLParam || saved.windowResult != message.windowResult)
        { out.unresolved.emplace_back("v4_category_desktop_window_unavailable"); return out; }
        const auto matches = [&](const diagnostic::Event& event, bool blob) {
            const auto& s = event.nativeScope;
            const bool native = blob ? (!s.session && !s.event && !s.delivery && !s.forward && !s.ownerThread && !s.forwarded && !s.queuedEvent) :
                (s.session == delivery->session && s.event == delivery->event && s.delivery == delivery->delivery &&
                 s.ownerThread == delivery->ownerThread && !s.forward && !s.forwarded && s.queuedEvent);
            return event.thread == delivery->ownerThread && event.windowScope == message.windowScope && event.hasBlob == blob && native;
        };
        const V4NativeLayoutRecord *enter = nullptr, *origin = nullptr, *remove = nullptr, *removeEnd = nullptr,
            *callback = nullptr, *callbackEnd = nullptr, *returned = nullptr;
        for (const auto& record : out.records)
            if (record.event.phase == diagnostic::Phase::category_desktop_commit_enter &&
                record.event.object == receipt.HostIdentity() && record.event.extra && matches(record.event, false))
            { if (enter) { out.unresolved.emplace_back("v4_category_desktop_commit_ambiguous"); return out; } enter = &record; }
        if (!enter) { out.unresolved.emplace_back("v4_category_desktop_commit_unavailable"); return out; }
        bool ambiguous = false;
        for (const auto& record : out.records)
        {
            if (record.event.extra != enter->event.extra) continue;
            const bool blob = record.event.phase == diagnostic::Phase::category_desktop_remove_key;
            if (!matches(record.event, blob) || record.index < enter->index ||
                record.event.phase == diagnostic::Phase::category_desktop_remove_truncated) { ambiguous = true; continue; }
            const V4NativeLayoutRecord** slot = nullptr;
            switch (record.event.phase)
            {
            case diagnostic::Phase::category_desktop_commit_enter: if (&record != enter) ambiguous = true; break;
            case diagnostic::Phase::category_desktop_commit_origin: slot = &origin; break;
            case diagnostic::Phase::category_desktop_remove_enter: slot = &remove; break;
            case diagnostic::Phase::category_desktop_remove_return: slot = &removeEnd; break;
            case diagnostic::Phase::category_desktop_commit_callback_enter: slot = &callback; break;
            case diagnostic::Phase::category_desktop_commit_callback_return: slot = &callbackEnd; break;
            case diagnostic::Phase::category_desktop_commit_return: slot = &returned; break;
            default: break;
            }
            if (slot) { if (*slot) ambiguous = true; else *slot = &record; }
        }
        if (ambiguous || !origin || !origin->event.object || !returned || returned->event.object != receipt.HostIdentity() ||
            (returned->event.value != 0 && returned->event.value != 1) || origin->index <= enter->index || returned->index <= origin->index)
        { out.unresolved.emplace_back("v4_category_desktop_interval_unavailable"); return out; }
        for (const auto& record : out.records)
            if (record.event.extra == enter->event.extra && record.index > returned->index)
            { out.unresolved.emplace_back("v4_category_desktop_interval_ambiguous"); return out; }
        out.commitId = enter->event.extra; out.origin = origin->event.object; out.releaseTraceIndex = layout.releaseTraceIndex;
        out.applied = returned->event.value == 1;
        if (!*out.applied && (remove || removeEnd || callback || callbackEnd))
        { out.unresolved.emplace_back("v4_category_desktop_return_scope_inconsistent"); return out; }
        if (*out.applied)
        {
            if (!remove || !removeEnd || !callback || !callbackEnd || remove->event.object != receipt.HostIdentity() ||
                removeEnd->event.object != receipt.HostIdentity() || callback->event.object != receipt.HostIdentity() ||
                callbackEnd->event.object != receipt.HostIdentity() || remove->event.value < 0 ||
                static_cast<std::uint64_t>(remove->event.value) > diagnostic::EventLimit() / 2 ||
                !(origin->index < remove->index && remove->index < removeEnd->index && removeEnd->index < callback->index &&
                  callback->index < callbackEnd->index && callbackEnd->index < returned->index))
            { out.unresolved.emplace_back("v4_category_desktop_callbacks_unavailable"); return out; }
            std::size_t expected = 0; const V4NativeLayoutRecord* previous = nullptr;
            for (const auto& record : out.records)
            {
                if (record.event.extra != enter->event.extra ||
                    (record.event.phase != diagnostic::Phase::category_desktop_remove_index && record.event.phase != diagnostic::Phase::category_desktop_remove_key)) continue;
                if (record.index <= remove->index || record.index >= removeEnd->index || record.event.object != receipt.HostIdentity())
                { ambiguous = true; break; }
                if (record.event.phase == diagnostic::Phase::category_desktop_remove_index)
                { if (previous || record.event.value != static_cast<std::int64_t>(expected)) { ambiguous = true; break; } previous = &record; }
                else
                { if (!previous || record.index != previous->index + 1 || record.blob.size() != record.event.blobSize ||
                      record.blob.size() % sizeof(wchar_t)) { ambiguous = true; break; } previous = nullptr; ++expected; }
            }
            if (ambiguous || previous || expected != static_cast<std::size_t>(remove->event.value))
            { out.unresolved.emplace_back("v4_category_desktop_removed_keys_unavailable"); return out; }
            const V4NativeLayoutRecord *request = nullptr, *requestEnd = nullptr;
            for (const auto& record : layout.records)
                if (record.index > callback->index && record.index < callbackEnd->index && matches(record.event, false))
                {
                    if (record.event.phase == diagnostic::Phase::layout_request_enter) { if (request) ambiguous = true; else request = &record; }
                    if (record.event.phase == diagnostic::Phase::layout_request_return) { if (requestEnd) ambiguous = true; else requestEnd = &record; }
                }
            if (!ambiguous && request && requestEnd && request->index < requestEnd->index &&
                request->event.extra != (std::numeric_limits<std::uint64_t>::max)() && requestEnd->event.extra == request->event.extra + 1)
                out.layoutRequestInsideCommit = true;
            else out.unresolved.emplace_back("v4_category_desktop_layout_request_unavailable");
        }
        out.releaseScopedCommitObserved = true;
        out.unresolved.emplace_back("v4_category_desktop_origin_member_identity_and_reentry_unverified");
        out.unresolved.emplace_back("v4_category_desktop_async_save_disk_business_and_late_work_unverified");
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; }
    catch (const std::length_error&) { out.resourceUnavailable = true; }
    catch (...) { out.observationUnavailable = true; }
    return out;
}

struct V4NativePreparationMarqueeObservation
{
    struct Occurrence
    {
        std::uint64_t observationId = 0;
        std::string session;
        std::size_t programIndex = 0, enterIndex = 0, returnIndex = 0, traceIndex = MessageTrace::InvalidToken;
    };
    bool identityValid = false, observationUnavailable = true, resourceUnavailable = false;
    std::vector<Occurrence> occurrences;
    std::vector<V4NativeLayoutRecord> records;
    std::vector<std::string> unresolved;
};

// Correlate actual marquee update markers only to the preparation receipt's
// own returned queued WM_MOUSEMOVE. Records are copied before matching so a
// missing/ambiguous marker never erases safely retained production values.
inline V4NativePreparationMarqueeObservation InspectV4NativePreparationMarquee(
    const V4NativeSelectionPreparationReceipt& receipt, const event_graph::V4FamilyIdentityContext& identity,
    const V4FamilyValidatedContext& verified, const std::vector<bool>& present,
    const event_graph::V4NativeJoinedGraphVariant& selected, const std::shared_ptr<NativeHostEpoch>& epoch,
    const MessageTrace& trace, HWND desktop, HWND control, std::size_t actionBudget)
{
    namespace diagnostic = snowdesktop::external_transfer_observation;
    using namespace snowdesktop::private_pointer;
    V4NativePreparationMarqueeObservation out;
    try
    {
        const bool complete = diagnostic::Complete();
        diagnostic::ReadSession session;
        if (!session.Open(true)) { out.unresolved.emplace_back("v4_preparation_marquee_session_unavailable"); return out; }
        for (std::size_t i = 0; i < session.Count(); ++i)
        {
            diagnostic::Event event;
            if (!session.ReadEvent(i, event)) { out.unresolved.emplace_back("v4_preparation_marquee_record_unavailable"); return out; }
            if (event.phase < diagnostic::Phase::desktop_marquee_enter || event.phase > diagnostic::Phase::desktop_marquee_return) continue;
            V4NativeLayoutRecord record{i, event, {}};
            if (event.hasBlob)
            {
                if (!CopyV4DiagnosticBlob(session, record))
                { out.unresolved.emplace_back("v4_preparation_marquee_blob_unavailable"); return out; }
            }
            out.records.push_back(std::move(record));
        }
        if (!complete || !diagnostic::Complete())
        { out.unresolved.emplace_back("v4_preparation_marquee_diagnostic_incomplete"); return out; }
        const auto plan = BuildV4NativeSelectionPreparationPlan(identity, verified, present, selected, actionBudget);
        if (plan.resourceUnavailable) out.resourceUnavailable = true;
        if (!plan.identityValid || !plan.inputsMaterialized || !receipt.SameCandidate(identity, verified, present, selected) ||
            !receipt.SameHost(epoch, trace, desktop, control) || trace.LostOrCrossThread() || !receipt.HostIdentity())
        { out.unresolved.emplace_back("v4_preparation_marquee_receipt_or_host_unavailable"); return out; }
        out.identityValid = true;
        std::set<std::uint64_t> seen;
        for (const auto& start : out.records)
        {
            const auto& e = start.event;
            if (e.phase != diagnostic::Phase::desktop_marquee_enter || e.object != receipt.HostIdentity()) continue;
            const auto& scope = e.nativeScope;
            if (!e.extra || !seen.insert(e.extra).second || !e.windowScope || !scope.session || !scope.event ||
                !scope.delivery || !scope.ownerThread || scope.forward || scope.forwarded || !scope.queuedEvent ||
                e.thread != scope.ownerThread ||
                std::count_if(out.records.begin(), out.records.end(), [&](const auto& r) {
                    return r.event.extra == e.extra && r.event.phase == diagnostic::Phase::desktop_marquee_enter; }) != 1 ||
                std::count_if(out.records.begin(), out.records.end(), [&](const auto& r) {
                    return r.event.extra == e.extra && r.event.phase == diagnostic::Phase::desktop_marquee_return; }) != 1)
            { out.unresolved.emplace_back("v4_preparation_marquee_scope_ambiguous"); continue; }
            const V4NativeLayoutRecord* returned = nullptr; bool ambiguous = false;
            for (const auto& record : out.records)
                if (record.event.extra == e.extra && record.event.phase == diagnostic::Phase::desktop_marquee_return &&
                    record.event.object == e.object && record.event.thread == e.thread && record.event.windowScope == e.windowScope &&
                    record.event.nativeScope.session == scope.session && record.event.nativeScope.event == scope.event &&
                    record.event.nativeScope.delivery == scope.delivery && record.event.nativeScope.ownerThread == scope.ownerThread &&
                    record.event.nativeScope.queuedEvent && !record.event.nativeScope.forwarded && !record.event.nativeScope.forward)
                { if (returned) { ambiguous = true; break; } returned = &record; }
            if (ambiguous || !returned || returned->index <= start.index ||
                std::any_of(out.records.begin(), out.records.end(), [&](const auto& r) {
                    return r.event.extra == e.extra && r.event.phase == diagnostic::Phase::desktop_marquee_truncated; }))
            { out.unresolved.emplace_back("v4_preparation_marquee_interval_unavailable"); continue; }
            std::optional<V4NativePreparationMarqueeObservation::Occurrence> matched;
            for (const auto& session : receipt.Sessions())
                for (const auto& device : session.device)
                {
                    if (device.phase != NativeDevicePhase::program_input || !device.programIndex || !device.queued || !device.expected ||
                        device.expected->message != WM_MOUSEMOVE || device.delivery.failed || device.queued->ownerThread != scope.ownerThread ||
                        device.queued->session != scope.session || device.queued->event != scope.event) continue;
                    const auto action = std::find_if(plan.actions.begin(), plan.actions.end(), [&](const auto& item) {
                        return item.programIndex == *device.programIndex && item.session == session.ordinal &&
                            item.kind == "selection_prepare_pointer_move"; });
                    if (action == plan.actions.end() || !action->screen ||
                        action->screen->x != device.expected->screen.x || action->screen->y != device.expected->screen.y) continue;
                    const DeliveryObservation* primary = nullptr;
                    for (const auto& item : device.delivery.entries)
                        if (item.kind == DeliveryObservationKind::begin && item.identity.queuedEvent && !item.identity.forwarded &&
                            !item.identity.forward && item.identity.ownerThread == scope.ownerThread && item.identity.session == scope.session &&
                            item.identity.event == scope.event && item.identity.delivery == scope.delivery)
                        { if (primary) { ambiguous = true; break; } primary = &item; }
                    if (ambiguous || !primary) continue;
                    const auto& client = device.expected->clientBeforeDispatch;
                    if (client.x < (std::numeric_limits<short>::min)() || client.x > (std::numeric_limits<short>::max)() ||
                        client.y < (std::numeric_limits<short>::min)() || client.y > (std::numeric_limits<short>::max)()) continue;
                    const auto delivery = InspectNativeDelivery(trace, 0, &device.delivery, primary->identity, desktop,
                        MessageRoute::desktop_window_proc, WM_MOUSEMOVE, device.expected->buttons,
                        MAKELPARAM(static_cast<short>(client.x), static_cast<short>(client.y)));
                    if (!delivery.matchedMessageAndPermit || delivery.traceIndex >= device.trace.size() ||
                        delivery.traceIndex >= trace.Observations().size()) continue;
                    const auto& saved = device.trace[delivery.traceIndex]; const auto& live = trace.Observations()[delivery.traceIndex];
                    if (!saved.returned || !live.returned || saved.windowScope != e.windowScope || saved.windowScope != live.windowScope ||
                        saved.enteredSequence != live.enteredSequence || saved.returnedSequence != live.returnedSequence ||
                        saved.route != live.route || saved.delivery.session != live.delivery.session || saved.delivery.event != live.delivery.event ||
                        saved.delivery.delivery != live.delivery.delivery || saved.delivery.forward != live.delivery.forward ||
                        saved.delivery.forwarded != live.delivery.forwarded || saved.delivery.queuedEvent != live.delivery.queuedEvent ||
                        saved.delivery.ownerThread != live.delivery.ownerThread ||
                        saved.window != live.window || saved.message != live.message || saved.wParam != live.wParam ||
                        saved.inputLParam != live.inputLParam || saved.windowResult != live.windowResult) continue;
                    if (matched) { ambiguous = true; break; }
                    matched = V4NativePreparationMarqueeObservation::Occurrence{e.extra, session.ordinal, *device.programIndex,
                        start.index, returned->index, delivery.traceIndex};
                }
            if (ambiguous || !matched) { out.unresolved.emplace_back("v4_preparation_marquee_delivery_unavailable"); continue; }
            out.occurrences.push_back(std::move(*matched));
        }
        out.unresolved.emplace_back("v4_preparation_marquee_selection_snapshot_and_reentrancy_unverified");
        out.unresolved.emplace_back("v4_preparation_marquee_full_graph_absence_and_late_work_unverified");
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; }
    catch (const std::length_error&) { out.resourceUnavailable = true; }
    catch (...) { out.observationUnavailable = true; }
    return out;
}

struct V4NativeMultiSelectionCaptureObservation
{
    bool identityValid = false, captureDeliveryObserved = false;
    bool observationUnavailable = true, resourceUnavailable = false;
    std::uint64_t captureId = 0;
    std::size_t deviceRecordIndex = MessageTrace::InvalidToken, traceIndex = MessageTrace::InvalidToken;
    std::vector<V4NativeLayoutRecord> records;
    std::vector<std::string> unresolved;
};

// Binds the actual drag-session source snapshot to one returned queued input
// in this receipt. Marquee records are retained as raw values only: a separate
// preparation receipt is required to attribute them to the same candidate.
inline V4NativeMultiSelectionCaptureObservation InspectV4NativeMultiSelectionCapture(
    const V4NativeRunReceipt& receipt, const event_graph::V4FamilyIdentityContext& identity,
    const V4FamilyValidatedContext& verified, const std::vector<bool>& present,
    const event_graph::V4NativeJoinedGraphVariant& selected, const std::shared_ptr<NativeHostEpoch>& epoch,
    const MessageTrace& trace, HWND desktop, HWND control)
{
    namespace diagnostic = snowdesktop::external_transfer_observation;
    using namespace snowdesktop::private_pointer;
    V4NativeMultiSelectionCaptureObservation out;
    try
    {
        const bool complete = diagnostic::Complete();
        diagnostic::ReadSession session;
        if (!session.Open(true)) { out.unresolved.emplace_back("v4_multiselection_session_unavailable"); return out; }
        for (std::size_t i = 0; i < session.Count(); ++i)
        {
            diagnostic::Event event;
            if (!session.ReadEvent(i, event)) { out.unresolved.emplace_back("v4_multiselection_record_unavailable"); return out; }
            if (event.phase < diagnostic::Phase::desktop_marquee_enter || event.phase > diagnostic::Phase::native_capture_return) continue;
            V4NativeLayoutRecord record{i, event, {}};
            if (event.hasBlob)
            {
                if (!CopyV4DiagnosticBlob(session, record))
                { out.unresolved.emplace_back("v4_multiselection_blob_unavailable"); return out; }
            }
            out.records.push_back(std::move(record));
        }
        if (!complete || !diagnostic::Complete())
        { out.unresolved.emplace_back("v4_multiselection_diagnostic_incomplete"); return out; }
        if (!receipt.SameCandidate(identity, verified, present, selected) ||
            !receipt.SameHost(epoch, trace, desktop, control) || trace.LostOrCrossThread())
        { out.unresolved.emplace_back("v4_multiselection_receipt_or_host_unavailable"); return out; }
        out.identityValid = true;
        const V4NativeLayoutRecord* capture = nullptr;
        for (const auto& record : out.records)
            if (record.event.phase == diagnostic::Phase::native_capture_enter &&
                record.event.object == receipt.HostIdentity())
            { if (capture) { out.unresolved.emplace_back("v4_multiselection_capture_ambiguous"); return out; } capture = &record; }
        if (!capture || !capture->event.extra || !capture->event.windowScope)
        { out.unresolved.emplace_back("v4_multiselection_capture_unavailable"); return out; }
        const auto& scope = capture->event.nativeScope;
        if (!scope.session || !scope.event || !scope.delivery || !scope.ownerThread || scope.forward || scope.forwarded ||
            !scope.queuedEvent || capture->event.thread != scope.ownerThread)
        { out.unresolved.emplace_back("v4_multiselection_capture_scope_unavailable"); return out; }
        std::optional<std::size_t> matchedDevice, matchedTrace;
        for (std::size_t i = 0; i < receipt.DeviceRecords().size(); ++i)
        {
            const auto& record = receipt.DeviceRecords()[i];
            if (record.phase != NativeDevicePhase::program_input || !record.queued || !record.expected ||
                record.expected->message != WM_MOUSEMOVE ||
                record.delivery.failed || record.queued->ownerThread != scope.ownerThread ||
                record.queued->session != scope.session || record.queued->event != scope.event) continue;
            const DeliveryObservation* primary = nullptr;
            for (const auto& event : record.delivery.entries)
                if (event.kind == DeliveryObservationKind::begin && event.identity.queuedEvent && !event.identity.forwarded &&
                    !event.identity.forward && event.identity.ownerThread == scope.ownerThread &&
                    event.identity.session == scope.session && event.identity.event == scope.event &&
                    event.identity.delivery == scope.delivery)
                { if (primary) { out.unresolved.emplace_back("v4_multiselection_delivery_ambiguous"); return out; } primary = &event; }
            if (!primary) continue;
            const auto& client = record.expected->clientBeforeDispatch;
            if (client.x < (std::numeric_limits<short>::min)() || client.x > (std::numeric_limits<short>::max)() ||
                client.y < (std::numeric_limits<short>::min)() || client.y > (std::numeric_limits<short>::max)()) continue;
            const auto lp = MAKELPARAM(static_cast<short>(client.x), static_cast<short>(client.y));
            const auto evidence = InspectNativeDelivery(trace, 0, &record.delivery, primary->identity, desktop,
                MessageRoute::desktop_window_proc, record.expected->message, record.expected->buttons, lp);
            if (!evidence.matchedMessageAndPermit || evidence.traceIndex >= record.trace.size() ||
                evidence.traceIndex >= trace.Observations().size()) continue;
            const auto& before = record.trace[evidence.traceIndex]; const auto& current = trace.Observations()[evidence.traceIndex];
            if (!before.returned || !current.returned || before.windowScope != capture->event.windowScope ||
                before.windowScope != current.windowScope || before.enteredSequence != current.enteredSequence ||
                before.returnedSequence != current.returnedSequence || before.window != current.window ||
                before.message != current.message || before.wParam != current.wParam ||
                before.inputLParam != current.inputLParam || before.windowResult != current.windowResult) continue;
            if (matchedDevice) { out.unresolved.emplace_back("v4_multiselection_capture_device_ambiguous"); return out; }
            matchedDevice = i; matchedTrace = evidence.traceIndex;
        }
        const V4NativeLayoutRecord* returned = nullptr;
        for (const auto& record : out.records)
            if (record.event.phase == diagnostic::Phase::native_capture_return && record.event.extra == capture->event.extra &&
                record.event.object == capture->event.object && record.event.thread == capture->event.thread &&
                record.event.windowScope == capture->event.windowScope &&
                record.event.nativeScope.session == scope.session && record.event.nativeScope.event == scope.event &&
                record.event.nativeScope.delivery == scope.delivery && record.event.nativeScope.ownerThread == scope.ownerThread &&
                record.event.nativeScope.queuedEvent && !record.event.nativeScope.forwarded && !record.event.nativeScope.forward)
            { if (returned) { out.unresolved.emplace_back("v4_multiselection_capture_return_ambiguous"); return out; } returned = &record; }
        if (!matchedDevice || !matchedTrace || !returned || returned->index <= capture->index ||
            std::any_of(out.records.begin(), out.records.end(), [&](const auto& record) {
                return record.event.phase == diagnostic::Phase::native_capture_truncated && record.event.extra == capture->event.extra; }))
        { out.unresolved.emplace_back("v4_multiselection_capture_delivery_or_values_incomplete"); return out; }
        out.captureId = capture->event.extra; out.deviceRecordIndex = *matchedDevice; out.traceIndex = *matchedTrace;
        out.captureDeliveryObserved = true;
        out.unresolved.emplace_back("v4_multiselection_preparation_session_receipt_and_selection_binding_unimplemented");
        out.unresolved.emplace_back("v4_multiselection_reentry_layout_business_and_late_work_unverified");
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; }
    catch (const std::length_error&) { out.resourceUnavailable = true; }
    catch (...) { out.observationUnavailable = true; }
    return out;
}

}

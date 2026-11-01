#pragma once

#include "global_drag_event_graph.inc"
#include "global_drag_payload_fixture.inc"
#include <Windows.h>
#include <objbase.h>
#include <cstring>
#include <iterator>
#include <atomic>
#include <array>
#include <mutex>
#include <set>
#include <type_traits>
#include <unordered_set>

namespace drag_global { class ScheduledV3Executor; class ScheduledV4Executor; namespace virtual_child_protocol { class Parent; class Child; } }

// Scheduling coordinates only: no fixture, production call, or business
// assertion is performed here. A caller must persist a transition before
// applying it, and must separately authenticate every candidate/graph cursor.
namespace drag_global::v3_scheduler
{
inline constexpr std::string_view Algorithm = "v3_family_round_robin_v1";

enum class WorkState { pending, resource_unavailable, schema_unavailable, observation_unavailable, completed, in_flight, outcome_unavailable };
enum class TickKind { discovered, encoding_hole, duplicate_seed, discovery_progress, discovery_deferred, visit, unavailable, invalid_state };
enum class FamilyVersion { v3, v4 };

struct AttemptHistory
{
    std::string executionId, status, failure;
    // Opaque exact bytes are audit data, not self-authenticating after reload.
    // A consumer must independently re-establish the same-run provenance; no
    // value here is a product PASS or an authorization to resume execution.
    std::string evidenceVersion, evidenceBytes;
    bool receiptConsumed = false, bridgeEntered = false, productionStarted = false;
    bool productionEntryUnknown = false, eventOracleFailureObserved = false, businessFailureObserved = false, observationUnavailable = false;
    // The validity marker distinguishes an actual same-run observation from
    // older records and runs where the hypothesis was never observed.
    bool hypothesisObservationRecorded = false, hypothesisNotMetObserved = false;
    // Older checkpoints did not capture this fact; false/false preserves
    // that absence separately from a recorded successful dispatch.
    bool eventDispatchObservationRecorded = false, eventDispatchFailureObserved = false;
    std::vector<std::string> obligations, notApplicable, graphUnresolved, postDispatchObligations, unclassifiedObligations;
};

struct CaseRecord
{
    // Stable candidate and graph identities are supplied by their authority.
    std::string candidateId, graphId, executionId;
    std::string kind, candidateOrdinal, beforeCursor, presence, reason;
    std::string graphMode = "v3_ole";
    bool directSeed = false;
    std::vector<std::string> candidateValues; // direct seeds only; revalidated on replay
    std::size_t replayWork = 0;
    std::vector<std::string> unresolved;
    WorkState state = WorkState::pending;
    std::string result; // opaque evidence, never a scheduler-generated PASS
    std::vector<AttemptHistory> history; // append only; caller data is not authenticated on recovery
};

struct PruneAuditStep
{
    std::string beforeCursor, afterCursor;
    std::size_t workUsed = 0;
    std::vector<event_graph::V3FamilyPresencePrune> presence;
    std::vector<event_graph::V3FamilyActivePrune> active;
};
struct V4TypedGrammarPrune
{
    std::size_t layer = 0;
    std::vector<std::uint32_t> prefix;
};
// Exact inputs and outputs of one authoritative typed DFS step. Stored budget
// values are part of replay: increasing a later budget can change whether a
// subtree is deferred. This is an audit record, not evidence of execution.
struct V4TypedStepAudit
{
    V4StructuredDfsCursor before, after;
    std::optional<V4StructuredDfsCursor> deferred;
    V4CoordinateBudget coordinateBudget;
    std::size_t tokenBudget = 0, workBudget = 0, workUsed = 0;
    V4StructuredDfsStatus status = V4StructuredDfsStatus::invalid;
    std::vector<std::uint32_t> prunedPrefix;
    std::string familyCoordinate;
    // Hierarchical task identity and parent-local step ordinal are audit data;
    // the scheduler must authenticate their predecessor when adopting a head.
    std::string taskId, stepOrdinal, deferredTaskId;
};
struct V4RoundAdvanceAudit
{
    std::string snapshotName, snapshotVolume, snapshotFileId, beforePosition, afterPosition;
    std::string status, taskId, nextOrdinal;
    std::optional<V4StructuredDfsCursor> itemCursor;
    bool fromDeferred = false;
    V4CoordinateBudget budget;
    std::size_t tokenBudget = 0, replayWorkBudget = 0, byteBudget = 0;
    std::size_t nodeBudget = 0, pinBudget = 0, snapshotByteBudget = 0;
};
// One chosen family in a fixed snapshot. This is replay data, not a trusted
// head, coverage claim, or execution receipt.
struct V4FamilySelectionAudit
{
    std::string snapshotName, snapshotVolume, snapshotFileId, ordinal, coordinate;
    std::string visitHeadName, visitHeadVolume, visitHeadFileId;
    bool scanExhausted = false;
    WorkState savedState = WorkState::pending;
    V4CoordinateBudget coordinateBudget;
    V4AxisBudget axisBudget;
    std::size_t tokenBudget = 0, replayWorkBudget = 0, byteBudget = 0, indexByteBudget = 0;
    std::size_t workBudget = 0, nodeBudget = 0, pinBudget = 0, snapshotByteBudget = 0;
};
struct V4FamilyRoundStepAudit
{
    V4FamilySelectionAudit selection; // snapshot, predecessor head and exact Step budgets
    std::string stepOrdinal, status;
    std::size_t workUsed = 0;
    bool hasItem = false, exhausted = false;
};
struct V4FamilySettlementAudit
{
    std::string selectionName, selectionVolume, selectionFileId;
    std::string previousName, previousVolume, previousFileId;
    std::string coordinate, outcome;
    std::string familyName, familyVolume, familyFileId;
    std::string caseName, caseVolume, caseFileId;
};
struct V4PreviewHeadAnchorAudit
{
    std::string rootVolume, rootId, directoryName, directoryVolume, directoryId;
    std::string headName, headVolume, headId, headBytes;
};
struct V4TypedPruneAudit
{
    bool grammarNoncanonical = false, resourceUnavailable = false;
    std::string_view reason;
};
// A persisted prefix is audit data, not a product exclusion or proof of DFS
// history. Re-evaluate its typed grammar independently when inspecting it.
inline V4TypedPruneAudit InspectV4TypedGrammarPrune(const V4TypedGrammarPrune& prune, std::size_t tokenBudget)
{
    if (prune.prefix.size() > prune.layer) return {false, false, "v4_typed_prune_layer_invalid"};
    const auto inspected = InspectV4StructuredPrefix(prune.prefix, tokenBudget);
    if (inspected.status == V4StructuredPrefixStatus::resource_unavailable)
        return {false, true, inspected.reason};
    if (inspected.status != V4StructuredPrefixStatus::noncanonical)
        return {false, false, "v4_typed_prune_not_noncanonical"};
    return {true, false, "v4_typed_grammar_only_not_product_rejection"};
}

struct FamilyState
{
    std::string coordinate;
    FamilyVersion version = FamilyVersion::v3;
    bool encodingHole = false; // not a model family, never a product exclusion
    bool discoverySeed = false, duplicateSeed = false;
    // These are opaque until the versioned candidate and V3JoinedGraph
    // authorities validate them against the exact family and candidate.
    // The authoritative family cursor already includes its active joined graph.
    std::string candidateCursor;
    WorkState state = WorkState::pending;
    bool scanExhausted = false; // traversal exhausted; cases can still be unexecuted
    std::size_t nextRetry = 0;
    std::vector<CaseRecord> cases;
    std::vector<PruneAuditStep> pruneSteps;
};

// Audit record only: recovered bytes do not authenticate their own origin.
// The containing checkpoint is also the association file; its trusted file ID
// and root locator must be supplied out of band when reopening.
struct OwnedCaseAssociation
{
    std::string templateCaseId, ownedCaseId;
    std::string profileVolume, profileFileId, sourceVolume, sourceFileId;
};
struct OwnedCaseTerminal
{
    std::string templateCaseId, ownedCaseId, executionId, terminalBytes;
    // Child root, checkpoint directory, pre, in-flight and terminal identities.
    std::array<std::string, 5> volumes, fileIds;
};
struct V4ExactAttemptAudit
{
    std::string caseId, executionId, sourcePathUtf16Hex, preBytes, flightBytes, previousName, previousVolume, previousFileId;
    // isolation, profile, source, checkpoint directory, pre, in-flight
    std::array<std::string, 6> volumes, fileIds;
};
// One staged projection page is an audit record, not a trusted graph or an
// execution receipt. Rebuild from the family/leaf authority and every saved
// page in order before attaching a completed projection.
struct V4NativeProjectionPageAudit
{
    std::string familyCoordinate, familyCursor, candidateId, candidateOrdinal;
    std::string leafOrdinal, leafPresence, pageOrdinal, advanceBytes, pageBytes;
    std::string baseHeadName, baseHeadVolume, baseHeadFileId;
    V4CoordinateBudget coordinateBudget;
    V4AxisBudget axisBudget;
    std::size_t advanceWorkBudget = 0, preambleBudget = 0, workBudget = 0, outputLimit = 0;
};
struct V4NativeStagedFamilyAudit
{
    std::string journalHeadName, journalHeadVolume, journalHeadFileId;
    std::string familyCoordinate, beforeCursor, afterCursor, stage, proposalBytes;
    std::size_t workBudget = 0;
};
// Locator and structured identity are data until the anchored staged journal
// and candidate are independently replayed under a trusted head.
struct V4NativeCaseIndexAudit
{
    std::string parentName, parentVolume, parentFileId;
    std::string pageName, pageVolume, pageFileId, caseId;
    std::size_t priorSteps = 0;
};
// One immutable linked index node; locator IDs are data until the caller
// supplies an independently trusted head and each file is opened by ID.
struct V4ShardIndexNode
{
    std::string generation, previousName, previousVolume, previousFileId;
    std::string dataKind, dataName, dataVolume, dataFileId;
};

struct State
{
    // This binding must include the exact model, graph, driver and external
    // input identities. It is not a hash or authorization supplied here.
    std::string binding;
    FamilyVersion familyVersion = FamilyVersion::v3;
    bool v4SeedEmitted = false, v4SeedNext = true;
    std::string v4SeedCoordinate;
    std::vector<std::string> v4TextSeedCoordinates;
    bool v4TextSeedNext = true;
    V4FieldLayerCursor v4FieldCursor;
    std::vector<V4FieldLayerCursor> v4Deferred, v4FieldHoles;
    std::size_t v4DeferredVisit = 0;
    bool v4DeferredNext = false;
    // V13 field-layer state above remains historical during explicit migration.
    bool v4TypedEnabled = false;
    V4StructuredDfsCursor v4TypedCursor;
    std::vector<V4StructuredDfsCursor> v4TypedDeferred;
    std::vector<V4TypedGrammarPrune> v4TypedPruned;
    std::size_t v4TypedDeferredVisit = 0;
    bool v4TypedDeferredNext = false;
    std::string nextFamily = "0";
    std::vector<FamilyState> families;
    std::vector<OwnedCaseAssociation> ownedAssociations;
    std::vector<OwnedCaseTerminal> ownedTerminals;
    std::vector<V4ExactAttemptAudit> exactAttempts;
    std::optional<V4ShardIndexNode> shardIndex;
    std::optional<V4TypedStepAudit> typedStep;
    std::optional<V4RoundAdvanceAudit> roundAdvance;
    std::optional<V4FamilySelectionAudit> familySelection;
    std::optional<V4FamilyRoundStepAudit> familyRoundStep;
    std::optional<V4FamilySettlementAudit> familySettlement;
    std::optional<V4PreviewHeadAnchorAudit> previewHeadAnchor;
    std::optional<V4NativeProjectionPageAudit> nativeProjectionPage;
    std::optional<V4NativeStagedFamilyAudit> nativeStagedFamily;
    std::optional<V4NativeCaseIndexAudit> nativeCaseIndex;
    std::size_t nextVisit = 0;
    // Two visits per discovery keep visitation faster than table growth.
    unsigned visitsRemaining = 0;
};

struct Tick
{
    TickKind kind = TickKind::invalid_state;
    State proposed;
    std::optional<std::size_t> familyIndex;
    std::string_view reason;
};

enum class CodecStatus { encoded, decoded, invalid, resource_unavailable };
struct CodecResult
{
    CodecStatus status = CodecStatus::invalid;
    std::string bytes;
    State state;
    bool recoveredInFlight = false;
    std::string_view reason;
};

inline std::string_view WorkStateName(WorkState state) noexcept
{
    switch (state)
    {
    case WorkState::pending: return "pending";
    case WorkState::resource_unavailable: return "resource_unavailable";
    case WorkState::schema_unavailable: return "schema_unavailable";
    case WorkState::observation_unavailable: return "observation_unavailable";
    case WorkState::completed: return "completed";
    case WorkState::in_flight: return "in_flight";
    case WorkState::outcome_unavailable: return "outcome_unavailable";
    }
    return {};
}

inline std::optional<WorkState> ParseWorkState(std::string_view value) noexcept
{
    for (const auto state : {WorkState::pending, WorkState::resource_unavailable, WorkState::schema_unavailable,
        WorkState::observation_unavailable, WorkState::completed,
        WorkState::in_flight, WorkState::outcome_unavailable})
        if (WorkStateName(state) == value) return state;
    return std::nullopt;
}

inline void AppendCheckpointToken(std::string& out, std::string_view value)
{ out += std::to_string(value.size()); out += ':'; out.append(value); }
struct CheckpointBudgetExceeded final {};

inline bool ReadCheckpointToken(std::string_view& remaining, std::string_view& value) noexcept
{
    const auto colon = remaining.find(':');
    if (colon == std::string_view::npos || !colon || (colon > 1 && remaining[0] == '0')) return false;
    std::size_t size = 0;
    for (std::size_t i = 0; i < colon; ++i)
    {
        const auto ch = remaining[i];
        if (ch < '0' || ch > '9') return false;
        const auto digit = static_cast<std::size_t>(ch - '0');
        if (size > ((std::numeric_limits<std::size_t>::max)() - digit) / 10) return false;
        size = size * 10 + digit;
    }
    remaining.remove_prefix(colon + 1);
    if (size > remaining.size()) return false;
    value = remaining.substr(0, size); remaining.remove_prefix(size);
    return true;
}

inline bool CheckpointSize(std::string_view value, std::size_t& result) noexcept
{
    if (!V3FamilyCanonicalDecimal(value)) return false;
    result = 0;
    for (const char ch : value)
    {
        const auto digit = static_cast<std::size_t>(ch - '0');
        if (result > ((std::numeric_limits<std::size_t>::max)() - digit) / 10) return false;
        result = result * 10 + digit;
    }
    return true;
}

inline bool V4TypedTaskIdCanonical(std::string_view id, std::size_t& layer, bool& isMain) noexcept
{
    std::string_view value; auto remaining = id;
    if (!ReadCheckpointToken(remaining, value) || value != "v4_typed_task_v1" ||
        !ReadCheckpointToken(remaining, value) || !CheckpointSize(value, layer)) return false;
    isMain = true;
    while (!remaining.empty())
    {
        if (!ReadCheckpointToken(remaining, value) || !V4CanonicalDecimal(value)) return false;
        isMain = false;
    }
    return true;
}
inline std::string V4TypedMainTaskId(std::size_t layer)
{
    std::string id; AppendCheckpointToken(id, "v4_typed_task_v1"); AppendCheckpointToken(id, std::to_string(layer)); return id;
}
inline std::string V4TypedDeferredTaskId(std::string_view parent, std::string_view parentStepOrdinal)
{
    std::size_t layer = 0; bool isMain = false;
    if (!V4TypedTaskIdCanonical(parent, layer, isMain) || !V4CanonicalDecimal(parentStepOrdinal)) return {};
    std::string result(parent); AppendCheckpointToken(result, parentStepOrdinal); return result;
}

// Codec does not authenticate supplied cursor or evidence strings. Consumers
// must reconstruct their model/graph authorities and validate before resuming.
inline CodecResult EncodeCheckpoint(const State& state, std::string_view expectedBinding, std::size_t byteBudget,
    bool legacyV13 = false, bool legacyV14 = false, bool legacyV15 = false, bool legacyV16 = false, bool legacyV17 = false,
    bool legacyV18 = false, bool legacyV19 = false, bool legacyV20 = false, bool legacyV21 = false, bool legacyV22 = false,
    bool legacyV23 = false, bool legacyV24 = false, bool legacyV25 = false)
{
    CodecResult result;
    try
    {
        if (static_cast<unsigned>(legacyV13) + static_cast<unsigned>(legacyV14) + static_cast<unsigned>(legacyV15) + static_cast<unsigned>(legacyV16) + static_cast<unsigned>(legacyV17) + static_cast<unsigned>(legacyV18) + static_cast<unsigned>(legacyV19) + static_cast<unsigned>(legacyV20) + static_cast<unsigned>(legacyV21) + static_cast<unsigned>(legacyV22) + static_cast<unsigned>(legacyV23) + static_cast<unsigned>(legacyV24) + static_cast<unsigned>(legacyV25) > 1 ||
            ((legacyV13 || legacyV14 || legacyV15 || legacyV16 || legacyV17 || legacyV18 || legacyV19 || legacyV20 || legacyV21 || legacyV22 || legacyV23 || legacyV24 || legacyV25) && state.nativeCaseIndex) ||
            ((legacyV13 || legacyV14 || legacyV15 || legacyV16 || legacyV17 || legacyV18 || legacyV19 || legacyV20 || legacyV21 || legacyV22 || legacyV23 || legacyV24) && (state.nativeProjectionPage || state.nativeStagedFamily)) ||
            ((legacyV13 || legacyV14 || legacyV15 || legacyV16 || legacyV17 || legacyV18 || legacyV19 || legacyV20 || legacyV21 || legacyV22 || legacyV23) && !state.exactAttempts.empty()) ||
            ((legacyV13 || legacyV14 || legacyV15 || legacyV16 || legacyV17 || legacyV18 || legacyV19 || legacyV20 || legacyV21 || legacyV22) && (state.familySelection || state.familyRoundStep || state.familySettlement || state.previewHeadAnchor)) ||
            ((legacyV13 || legacyV14 || legacyV15 || legacyV16 || legacyV17 || legacyV18 || legacyV19 || legacyV20 || legacyV21) && state.roundAdvance) ||
            ((legacyV13 || legacyV14 || legacyV15 || legacyV16 || legacyV17 || legacyV18 || legacyV19 || legacyV20) && state.typedStep) ||
            ((legacyV13 || legacyV14 || legacyV15 || legacyV16) && !state.ownedAssociations.empty()) ||
            ((legacyV13 || legacyV14 || legacyV15 || legacyV16 || legacyV17) && !state.ownedTerminals.empty()) ||
            ((legacyV13 || legacyV14 || legacyV15 || legacyV16 || legacyV17 || legacyV18) && state.shardIndex) ||
            expectedBinding.empty() || state.binding != expectedBinding ||
            (state.familyVersion != FamilyVersion::v3 && state.familyVersion != FamilyVersion::v4) || state.visitsRemaining > 2 ||
            (state.familyVersion == FamilyVersion::v3 && (state.v4SeedEmitted || !state.v4SeedNext || !state.v4SeedCoordinate.empty() ||
                !state.v4TextSeedCoordinates.empty() || !state.v4TextSeedNext)) ||
            (state.familyVersion == FamilyVersion::v4 && (state.v4SeedEmitted != !state.v4SeedCoordinate.empty())) ||
            (state.v4TextSeedCoordinates.size() > V4AdditionalTextSeedCount ||
                (!state.v4SeedEmitted && !state.v4TextSeedCoordinates.empty())) ||
            (state.families.empty() ? state.nextVisit != 0 : state.nextVisit >= state.families.size()) ||
            (state.families.empty() && state.visitsRemaining))
        { result.reason = "v3_checkpoint_state_invalid"; return result; }
        if (!V4FieldLayerCursorCanonical(state.v4FieldCursor) ||
            (state.v4Deferred.empty() ? state.v4DeferredVisit != 0 : state.v4DeferredVisit >= state.v4Deferred.size()) ||
            (state.familyVersion == FamilyVersion::v4 && state.nextFamily != "0"))
        { result.reason = "v4_field_scheduler_state_invalid"; return result; }
        const auto typedShape = [](const V4StructuredDfsCursor& cursor) {
            return cursor.stopDepth <= cursor.prefix.size() && cursor.prefix.size() <= cursor.layer &&
                cursor.nextChoice.size() == cursor.prefix.size() + 1 && !cursor.exhausted;
        };
        if (!typedShape(state.v4TypedCursor) || state.v4TypedCursor.stopDepth != 0 ||
            (state.v4TypedDeferred.empty() ? state.v4TypedDeferredVisit != 0 : state.v4TypedDeferredVisit >= state.v4TypedDeferred.size()) ||
            (state.familyVersion == FamilyVersion::v3 && state.v4TypedEnabled) ||
            (legacyV13 && state.v4TypedEnabled))
        { result.reason = "v4_typed_scheduler_state_invalid"; return result; }
        for (const auto& cursor : state.v4TypedDeferred)
            if (!typedShape(cursor))
            { result.reason = "v4_typed_deferred_cursor_invalid"; return result; }
        for (const auto& prune : state.v4TypedPruned)
            if (prune.prefix.size() > prune.layer)
            { result.reason = "v4_typed_prune_invalid"; return result; }
        if (!state.v4TypedEnabled && (state.v4TypedCursor.layer != 0 || !state.v4TypedCursor.prefix.empty() ||
            state.v4TypedCursor.nextChoice != std::vector<std::uint32_t>{0} || !state.v4TypedDeferred.empty() ||
            !state.v4TypedPruned.empty() || state.v4TypedDeferredVisit || state.v4TypedDeferredNext))
        { result.reason = "v4_typed_inactive_state_invalid"; return result; }
        const auto cursorKey = [](const V4FieldLayerCursor& cursor) {
            std::string key = cursor.activeB ? "1:" : "0:";
            key += cursor.layer;
            for (const auto& field : cursor.fields) { key += ':'; key += field; }
            return key;
        };
        std::unordered_set<std::string> fieldsSeen;
        for (const auto* entries : {&state.v4Deferred, &state.v4FieldHoles})
            for (const auto& cursor : *entries)
                if (!V4FieldLayerCursorCanonical(cursor) || !fieldsSeen.insert(cursorKey(cursor)).second)
                { result.reason = "v4_field_scheduler_cursor_duplicate_or_invalid"; return result; }
        if (fieldsSeen.contains(cursorKey(state.v4FieldCursor)))
        { result.reason = "v4_field_scheduler_current_cursor_duplicate"; return result; }
        if (state.familyVersion == FamilyVersion::v3 &&
            (!state.v4Deferred.empty() || !state.v4FieldHoles.empty() || state.v4DeferredVisit || state.v4DeferredNext ||
                state.v4FieldCursor.layer != "0" || state.v4FieldCursor.activeB))
        { result.reason = "v4_field_scheduler_v3_state_invalid"; return result; }
        std::string next = "0";
        std::unordered_set<std::string> familyCoordinates;
        bool sawSeed = false, sawSeedOriginal = false;
        for (const auto& family : state.families)
        {
            if ((state.familyVersion == FamilyVersion::v3 && !family.discoverySeed && family.coordinate != next) ||
                (state.familyVersion == FamilyVersion::v4 && (family.encodingHole || family.duplicateSeed ||
                    !V4CanonicalDecimal(family.coordinate) || !familyCoordinates.insert(family.coordinate).second)) || family.version != state.familyVersion ||
                (family.discoverySeed && (family.version != FamilyVersion::v4 || sawSeed || !state.v4SeedEmitted || family.encodingHole || family.duplicateSeed)) ||
                (family.duplicateSeed && (family.version != FamilyVersion::v4 || family.encodingHole || family.discoverySeed ||
                    !family.cases.empty() || !family.candidateCursor.empty() || !family.pruneSteps.empty())) ||
                ((family.discoverySeed || family.duplicateSeed) && family.coordinate != state.v4SeedCoordinate) ||
                (family.encodingHole && (family.version != FamilyVersion::v4 || !family.cases.empty() ||
                    !family.candidateCursor.empty() || !family.pruneSteps.empty())) || WorkStateName(family.state).empty() ||
                family.state == WorkState::completed || family.state == WorkState::in_flight ||
                family.state == WorkState::outcome_unavailable ||
                (family.cases.empty() ? family.nextRetry != 0 : family.nextRetry >= family.cases.size()))
            { result.reason = "v3_checkpoint_family_invalid"; return result; }
            if (family.discoverySeed) sawSeed = true;
            else if (state.familyVersion == FamilyVersion::v3) next = Add(next, "1");
            if (!family.encodingHole && !family.duplicateSeed && family.coordinate == state.v4SeedCoordinate)
                sawSeedOriginal = true;
            for (const auto& item : family.cases)
            {
                if (WorkStateName(item.state).empty() || item.candidateId.empty() || item.kind.empty() ||
                    (item.graphMode != "v3_ole" && item.graphMode != "v4_ole" && item.graphMode != "v4_native" && item.graphMode != "v4_unknown") ||
                    (item.graphMode == "v3_ole" && family.version != FamilyVersion::v3) ||
                    ((item.graphMode == "v4_ole" || item.graphMode == "v4_native" || item.graphMode == "v4_unknown") && family.version != FamilyVersion::v4) ||
                    (item.directSeed && (family.version != FamilyVersion::v4 ||
                        (item.graphMode != "v4_ole" && item.graphMode != "v4_native") ||
                        ((legacyV13 || legacyV14 || legacyV15) && item.graphMode == "v4_native") ||
                        !item.beforeCursor.empty() || item.replayWork || item.candidateValues.empty())) ||
                    (!item.directSeed && (item.beforeCursor.empty() || !item.replayWork || !item.candidateValues.empty())) ||
                    (item.kind == "graph_hypothesis" && item.graphMode == "v4_unknown") ||
                    (item.kind == "graph_hypothesis" && item.graphId.empty()) ||
                    (item.state != WorkState::pending && item.state != WorkState::resource_unavailable && item.executionId.empty()))
                { result.reason = "v3_checkpoint_case_invalid"; return result; }
                for (const auto& attempt : item.history)
                    if (attempt.executionId.empty() || attempt.status.empty() ||
                        (attempt.evidenceVersion.empty() && !attempt.evidenceBytes.empty()) ||
                        (attempt.hypothesisNotMetObserved && !attempt.hypothesisObservationRecorded) ||
                        (attempt.eventDispatchFailureObserved && !attempt.eventDispatchObservationRecorded) ||
                        ((legacyV13 || legacyV14 || legacyV15) && (attempt.eventDispatchObservationRecorded || attempt.eventDispatchFailureObserved)) ||
                        ((legacyV13 || legacyV14) && (attempt.hypothesisObservationRecorded || attempt.hypothesisNotMetObserved)))
                    { result.reason = "v3_checkpoint_attempt_history_invalid"; return result; }
            }
            for (const auto& audit : family.pruneSteps)
                if (audit.beforeCursor.empty() || audit.afterCursor.empty() || !audit.workUsed ||
                    (audit.presence.empty() && audit.active.empty() &&
                        (family.version != FamilyVersion::v4 || legacyV13 || legacyV14 || legacyV15 || legacyV16 || legacyV17 || legacyV18 || legacyV19)))
                { result.reason = "v3_checkpoint_prune_step_invalid"; return result; }
        }
        if ((state.familyVersion == FamilyVersion::v3 && state.nextFamily != next) || (state.v4SeedEmitted && !sawSeedOriginal))
        { result.reason = "v3_checkpoint_prefix_invalid"; return result; }
        std::unordered_set<std::string> seedCoordinates;
        for (const auto& coordinate : state.v4TextSeedCoordinates)
            if (!V4CanonicalDecimal(coordinate) || coordinate == state.v4SeedCoordinate ||
                !seedCoordinates.insert(coordinate).second || !familyCoordinates.contains(coordinate))
            { result.reason = "v4_text_seed_state_invalid"; return result; }
        auto& out = result.bytes;
        const auto put = [&](std::string_view token) {
            const auto length = std::to_string(token.size());
            if (out.size() > byteBudget || length.size() + 1 > byteBudget - out.size() ||
                token.size() > byteBudget - out.size() - length.size() - 1) throw CheckpointBudgetExceeded{};
            out += length; out += ':'; out.append(token);
        };
        put(legacyV13 ? "family_scheduler_checkpoint_v13" : legacyV14 ? "family_scheduler_checkpoint_v14" :
            legacyV15 ? "family_scheduler_checkpoint_v15" : legacyV16 ? "family_scheduler_checkpoint_v16" :
            legacyV17 ? "family_scheduler_checkpoint_v17" : legacyV18 ? "family_scheduler_checkpoint_v18" :
            legacyV19 ? "family_scheduler_checkpoint_v19" : legacyV20 ? "family_scheduler_checkpoint_v20" :
            legacyV21 ? "family_scheduler_checkpoint_v21" : legacyV22 ? "family_scheduler_checkpoint_v22" :
            legacyV23 ? "family_scheduler_checkpoint_v23" : legacyV24 ? "family_scheduler_checkpoint_v24" : legacyV25 ? "family_scheduler_checkpoint_v25" : "family_scheduler_checkpoint_v26"); put(Algorithm);
        put(state.familyVersion == FamilyVersion::v3 ? "v3" : "v4");
        put(state.familyVersion == FamilyVersion::v3 ? V3FamilyCoordinateAlgorithm : V4FamilyCoordinateAlgorithm);
        put(state.familyVersion == FamilyVersion::v3 ? event_graph::V3FamilyCandidateGraphAlgorithm : event_graph::V4FamilyCandidateGraphAlgorithm);
        put(state.binding); put(state.nextFamily); put(state.v4SeedEmitted ? "1" : "0"); put(state.v4SeedNext ? "1" : "0"); put(state.v4SeedCoordinate);
        put(state.v4TextSeedNext ? "1" : "0"); put(std::to_string(state.v4TextSeedCoordinates.size()));
        for (const auto& coordinate : state.v4TextSeedCoordinates) put(coordinate);
        put(V4FieldLayerAlgorithm);
        const auto putFieldCursor = [&](const V4FieldLayerCursor& cursor) {
            put(cursor.layer); put(cursor.activeB ? "1" : "0");
            put(std::to_string(cursor.fields.size())); for (const auto& field : cursor.fields) put(field);
        };
        putFieldCursor(state.v4FieldCursor); put(state.v4DeferredNext ? "1" : "0");
        put(std::to_string(state.v4DeferredVisit));
        for (const auto* entries : {&state.v4Deferred, &state.v4FieldHoles})
        { put(std::to_string(entries->size())); for (const auto& cursor : *entries) putFieldCursor(cursor); }
        if (!legacyV13)
        {
            put(V4StructuredPrefixAlgorithm); put(V4StructuredDfsAlgorithm); put(state.v4TypedEnabled ? "1" : "0");
            const auto putTypedCursor = [&](const V4StructuredDfsCursor& cursor) {
                put(std::to_string(cursor.layer)); put(std::to_string(cursor.stopDepth));
                put(std::to_string(cursor.prefix.size())); for (auto token : cursor.prefix) put(std::to_string(token));
                put(std::to_string(cursor.nextChoice.size())); for (auto token : cursor.nextChoice) put(std::to_string(token));
                put(cursor.exhausted ? "1" : "0");
            };
            putTypedCursor(state.v4TypedCursor); put(state.v4TypedDeferredNext ? "1" : "0");
            put(std::to_string(state.v4TypedDeferredVisit)); put(std::to_string(state.v4TypedDeferred.size()));
            for (const auto& cursor : state.v4TypedDeferred) putTypedCursor(cursor);
            put(std::to_string(state.v4TypedPruned.size()));
            for (const auto& prune : state.v4TypedPruned)
            { put(std::to_string(prune.layer)); put(std::to_string(prune.prefix.size())); for (auto token : prune.prefix) put(std::to_string(token)); }
        }
        put(std::to_string(state.nextVisit)); put(std::to_string(state.visitsRemaining));
        put(std::to_string(state.families.size()));
        for (const auto& family : state.families)
        {
            put(family.coordinate); put(family.encodingHole ? "1" : "0"); put(family.discoverySeed ? "1" : "0");
            put(family.duplicateSeed ? "1" : "0"); put(family.candidateCursor);
            put(WorkStateName(family.state)); put(family.scanExhausted ? "1" : "0");
            put(std::to_string(family.nextRetry)); put(std::to_string(family.cases.size()));
            for (const auto& item : family.cases)
            {
                put(item.candidateId); put(item.graphId); put(item.executionId); put(WorkStateName(item.state)); put(item.result);
                put(item.kind); put(item.candidateOrdinal); put(item.beforeCursor); put(item.presence); put(item.reason);
                put(item.graphMode); put(item.directSeed ? "1" : "0");
                put(std::to_string(item.candidateValues.size())); for (const auto& value : item.candidateValues) put(value);
                put(std::to_string(item.replayWork)); put(std::to_string(item.unresolved.size()));
                for (const auto& reason : item.unresolved) put(reason);
                put(std::to_string(item.history.size()));
                for (const auto& history : item.history)
                {
                    put(history.executionId); put(history.status); put(history.failure);
                    put(history.evidenceVersion); put(history.evidenceBytes);
                    for (const bool flag : {history.receiptConsumed, history.bridgeEntered, history.productionStarted,
                        history.productionEntryUnknown, history.eventOracleFailureObserved, history.businessFailureObserved, history.observationUnavailable})
                        put(flag ? "1" : "0");
                    if (!legacyV13 && !legacyV14)
                    { put(history.hypothesisObservationRecorded ? "1" : "0"); put(history.hypothesisNotMetObserved ? "1" : "0"); }
                    if (!legacyV13 && !legacyV14 && !legacyV15)
                    { put(history.eventDispatchObservationRecorded ? "1" : "0"); put(history.eventDispatchFailureObserved ? "1" : "0"); }
                    for (const auto* values : {&history.obligations, &history.notApplicable, &history.graphUnresolved,
                        &history.postDispatchObligations, &history.unclassifiedObligations})
                    { put(std::to_string(values->size())); for (const auto& value : *values) put(value); }
                }
            }
            put(std::to_string(family.pruneSteps.size()));
            for (const auto& audit : family.pruneSteps)
            {
                put(audit.beforeCursor); put(audit.afterCursor); put(std::to_string(audit.workUsed));
                put(std::to_string(audit.presence.size()));
                for (const auto& entry : audit.presence)
                {
                    put(entry.candidateOrdinal); put(entry.candidateId); put(std::to_string(entry.prune.prefix.size()));
                    for (auto choice : entry.prune.prefix) put(std::to_string(choice));
                    put(entry.prune.reason); put(entry.prune.premise); put(entry.prune.consequence);
                }
                put(std::to_string(audit.active.size()));
                for (const auto& entry : audit.active)
                {
                    put(entry.candidateOrdinal); put(entry.candidateId); put(entry.prune.presenceOrdinal);
                    std::string bits; for (const bool present : entry.prune.present) bits += present ? '1' : '0';
                    put(bits); put(entry.prune.prune.scope); put(entry.prune.prune.reason);
                    put(std::to_string(entry.prune.prune.partition.size()));
                    for (auto part : entry.prune.prune.partition) put(std::to_string(part));
                    put(entry.prune.prune.earlierRequirement); put(entry.prune.prune.currentRequirement);
                }
            }
        }
        if (!legacyV13 && !legacyV14 && !legacyV15 && !legacyV16)
        {
            std::set<std::pair<std::string, std::string>> seen;
            put(std::to_string(state.ownedAssociations.size()));
            for (const auto& association : state.ownedAssociations)
            {
                const auto validId = [](std::string_view volume, std::string_view id) {
                    return V4CanonicalDecimal(volume) && id.size() == 32 && V4LowerHex(id) &&
                        std::any_of(id.begin(), id.end(), [](char c) { return c != '0'; });
                };
                if (association.templateCaseId.empty() || association.ownedCaseId.empty() ||
                    association.templateCaseId == association.ownedCaseId ||
                    !validId(association.profileVolume, association.profileFileId) ||
                    !validId(association.sourceVolume, association.sourceFileId) ||
                    !seen.emplace(association.templateCaseId, association.ownedCaseId).second)
                { result.bytes.clear(); result.reason = "v4_owned_association_invalid"; return result; }
                put(association.templateCaseId); put(association.ownedCaseId);
                put(association.profileVolume); put(association.profileFileId);
                put(association.sourceVolume); put(association.sourceFileId);
            }
        }
        if (!legacyV13 && !legacyV14 && !legacyV15 && !legacyV16 && !legacyV17)
        {
            std::set<std::pair<std::string, std::string>> seen;
            put(std::to_string(state.ownedTerminals.size()));
            for (const auto& terminal : state.ownedTerminals)
            {
                if (terminal.templateCaseId.empty() || terminal.ownedCaseId.empty() || terminal.executionId.empty() ||
                    terminal.terminalBytes.empty() || !seen.emplace(terminal.ownedCaseId, terminal.executionId).second ||
                    std::none_of(state.ownedAssociations.begin(), state.ownedAssociations.end(), [&](const auto& a) {
                        return a.templateCaseId == terminal.templateCaseId && a.ownedCaseId == terminal.ownedCaseId; }))
                { result.bytes.clear(); result.reason = "v4_owned_terminal_invalid"; return result; }
                put(terminal.templateCaseId); put(terminal.ownedCaseId); put(terminal.executionId); put(terminal.terminalBytes);
                for (std::size_t i = 0; i < terminal.fileIds.size(); ++i)
                {
                    if (!V4CanonicalDecimal(terminal.volumes[i]) || terminal.fileIds[i].size() != 32 ||
                        !V4LowerHex(terminal.fileIds[i]) || std::none_of(terminal.fileIds[i].begin(), terminal.fileIds[i].end(), [](char c) { return c != '0'; }))
                    { result.bytes.clear(); result.reason = "v4_owned_terminal_identity_invalid"; return result; }
                    put(terminal.volumes[i]); put(terminal.fileIds[i]);
                }
            }
        }
        if (!legacyV13 && !legacyV14 && !legacyV15 && !legacyV16 && !legacyV17 && !legacyV18)
        {
            put(state.shardIndex ? "1" : "0");
            if (state.shardIndex)
            {
                const auto& node = *state.shardIndex;
                const auto validName = [](std::string_view name) {
                    return name.size() == 43 && name.starts_with("checkpoint-") &&
                        std::all_of(name.begin() + 11, name.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
                };
                const auto validId = [](std::string_view volume, std::string_view id) {
                    return V4CanonicalDecimal(volume) && id.size() == 32 && V4LowerHex(id) &&
                        std::any_of(id.begin(), id.end(), [](char c) { return c != '0'; });
                };
                const bool hasPrevious = !node.previousName.empty();
                if (state.familyVersion != FamilyVersion::v4 || !state.v4TypedEnabled || state.familySelection || state.familyRoundStep || state.familySettlement || state.previewHeadAnchor || !state.families.empty() ||
                    !state.ownedAssociations.empty() || !state.ownedTerminals.empty() ||
                    !V4CanonicalDecimal(node.generation) || node.generation == "0" ||
                    (node.dataKind != "case" && node.dataKind != "terminal" && node.dataKind != "family" && node.dataKind != "global_cursor" &&
                        (node.dataKind != "typed_step" || legacyV19 || legacyV20) &&
                        (node.dataKind != "round_step" || legacyV19 || legacyV20 || legacyV21) &&
                        (node.dataKind != "family_selection" || legacyV19 || legacyV20 || legacyV21 || legacyV22) &&
                        (node.dataKind != "family_round_step" || legacyV19 || legacyV20 || legacyV21 || legacyV22) &&
                        (node.dataKind != "family_settlement" || legacyV19 || legacyV20 || legacyV21 || legacyV22) &&
                        (node.dataKind != "native_projection_page" || legacyV19 || legacyV20 || legacyV21 || legacyV22 || legacyV23 || legacyV24) &&
                        (node.dataKind != "native_staged_family" || legacyV19 || legacyV20 || legacyV21 || legacyV22 || legacyV23 || legacyV24) &&
                        (node.dataKind != "native_staged_case" || legacyV19 || legacyV20 || legacyV21 || legacyV22 || legacyV23 || legacyV24 || legacyV25)) || !validName(node.dataName) ||
                    !validId(node.dataVolume, node.dataFileId) ||
                    (hasPrevious && (!validName(node.previousName) || !validId(node.previousVolume, node.previousFileId) || node.generation == "1")) ||
                    (!hasPrevious && (!node.previousVolume.empty() || !node.previousFileId.empty() || node.generation != "1")))
                { result.bytes.clear(); result.reason = "v4_shard_index_invalid"; return result; }
                put(node.generation); put(node.previousName); put(node.previousVolume); put(node.previousFileId);
                put(node.dataKind); put(node.dataName); put(node.dataVolume); put(node.dataFileId);
            }
        }
        if (!legacyV13 && !legacyV14 && !legacyV15 && !legacyV16 && !legacyV17 && !legacyV18 && !legacyV19 && !legacyV20)
        {
            put(state.typedStep ? "1" : "0");
            if (state.typedStep)
            {
                const auto& step = *state.typedStep;
                std::size_t taskLayer = 0; bool isMain = false;
                if (state.familyVersion != FamilyVersion::v4 || !state.v4TypedEnabled || state.shardIndex || state.familySelection || state.familyRoundStep || state.familySettlement || state.previewHeadAnchor ||
                    !state.families.empty() || !state.ownedAssociations.empty() || !state.ownedTerminals.empty() ||
                    !step.workBudget || !step.workUsed || step.workUsed > step.workBudget ||
                    !V4TypedTaskIdCanonical(step.taskId, taskLayer, isMain) || taskLayer != step.before.layer ||
                    (isMain != (step.before.stopDepth == 0)) || !V4CanonicalDecimal(step.stepOrdinal) ||
                    (step.deferred ? step.deferredTaskId != V4TypedDeferredTaskId(step.taskId, step.stepOrdinal) : !step.deferredTaskId.empty()) ||
                    step.status == V4StructuredDfsStatus::invalid || step.status == V4StructuredDfsStatus::resource_unavailable)
                { result.bytes.clear(); result.reason = "v4_typed_step_shape_invalid"; return result; }
                const auto putCursor = [&](const V4StructuredDfsCursor& c) {
                    put(std::to_string(c.layer)); put(std::to_string(c.stopDepth));
                    put(std::to_string(c.prefix.size())); for (auto value : c.prefix) put(std::to_string(value));
                    put(std::to_string(c.nextChoice.size())); for (auto value : c.nextChoice) put(std::to_string(value));
                    put(c.exhausted ? "1" : "0");
                };
                putCursor(step.before); putCursor(step.after); put(step.deferred ? "1" : "0");
                if (step.deferred) putCursor(*step.deferred);
                for (auto value : {step.coordinateBudget.decimalDigits, step.coordinateBudget.tokenBytes,
                    step.coordinateBudget.listEntries, step.coordinateBudget.arithmeticSteps, step.tokenBudget,
                    step.workBudget, step.workUsed, static_cast<std::size_t>(step.status)}) put(std::to_string(value));
                put(std::to_string(step.prunedPrefix.size())); for (auto value : step.prunedPrefix) put(std::to_string(value));
                put(step.familyCoordinate);
                put(step.taskId); put(step.stepOrdinal); put(step.deferredTaskId);
            }
        }
        if (!legacyV13 && !legacyV14 && !legacyV15 && !legacyV16 && !legacyV17 && !legacyV18 && !legacyV19 && !legacyV20 && !legacyV21)
        {
            put(state.roundAdvance ? "1" : "0");
            if (state.roundAdvance)
            {
                const auto& step = *state.roundAdvance;
                std::size_t itemLayer = 0; bool itemMain = false;
                if (state.familyVersion != FamilyVersion::v4 || !state.v4TypedEnabled || state.shardIndex || state.typedStep || state.familySelection || state.familyRoundStep || state.familySettlement || state.previewHeadAnchor ||
                    !state.families.empty() || !state.ownedAssociations.empty() || !state.ownedTerminals.empty() ||
                    step.snapshotName.empty() || !V4CanonicalDecimal(step.snapshotVolume) || step.snapshotFileId.size() != 32 ||
                    !V4LowerHex(step.snapshotFileId) || step.beforePosition.empty() || step.afterPosition.empty() || !step.nodeBudget ||
                    (step.status != "progress" && step.status != "item" && step.status != "exhausted") ||
                    ((step.status == "item") != static_cast<bool>(step.itemCursor)) ||
                    (step.itemCursor && (!V4TypedTaskIdCanonical(step.taskId, itemLayer, itemMain) ||
                        itemLayer != step.itemCursor->layer || !V4CanonicalDecimal(step.nextOrdinal))) ||
                    (step.status != "item" && (!step.taskId.empty() || !step.nextOrdinal.empty() || step.fromDeferred)))
                { result.bytes.clear(); result.reason = "v4_round_advance_shape_invalid"; return result; }
                put(step.snapshotName); put(step.snapshotVolume); put(step.snapshotFileId); put(step.beforePosition); put(step.afterPosition);
                put(step.status); put(step.taskId); put(step.nextOrdinal); put(step.fromDeferred ? "1" : "0");
                if (step.itemCursor)
                {
                    const auto& c = *step.itemCursor;
                    put(std::to_string(c.layer)); put(std::to_string(c.stopDepth));
                    put(std::to_string(c.prefix.size())); for (auto value : c.prefix) put(std::to_string(value));
                    put(std::to_string(c.nextChoice.size())); for (auto value : c.nextChoice) put(std::to_string(value));
                    put(c.exhausted ? "1" : "0");
                }
                for (auto value : {step.budget.decimalDigits, step.budget.tokenBytes, step.budget.listEntries,
                    step.budget.arithmeticSteps, step.tokenBudget, step.replayWorkBudget, step.byteBudget,
                    step.nodeBudget, step.pinBudget, step.snapshotByteBudget}) put(std::to_string(value));
            }
        }
        if (!legacyV13 && !legacyV14 && !legacyV15 && !legacyV16 && !legacyV17 && !legacyV18 && !legacyV19 && !legacyV20 && !legacyV21 && !legacyV22)
        {
            put(state.familySelection ? "1" : "0");
            if (state.familySelection)
            {
                const auto& s = *state.familySelection;
                const auto validName = [](std::string_view name) {
                    return name.size() == 43 && name.starts_with("checkpoint-") &&
                        std::all_of(name.begin() + 11, name.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
                };
                const auto validId = [](std::string_view volume, std::string_view id) {
                    return V4CanonicalDecimal(volume) && id.size() == 32 && V4LowerHex(id) &&
                        std::any_of(id.begin(), id.end(), [](char c) { return c != '0'; });
                };
                if (state.familyVersion != FamilyVersion::v4 || !state.v4TypedEnabled || state.shardIndex || state.typedStep || state.roundAdvance || state.familyRoundStep || state.familySettlement || state.previewHeadAnchor ||
                    !state.families.empty() || !state.ownedAssociations.empty() || !state.ownedTerminals.empty() ||
                    !validName(s.snapshotName) || !validId(s.snapshotVolume, s.snapshotFileId) ||
                    !validName(s.visitHeadName) || !validId(s.visitHeadVolume, s.visitHeadFileId) ||
                    !V4CanonicalDecimal(s.ordinal) || !V4CanonicalDecimal(s.coordinate) ||
                    static_cast<unsigned>(s.savedState) > static_cast<unsigned>(WorkState::outcome_unavailable) || !s.workBudget)
                { result.bytes.clear(); result.reason = "v4_family_selection_shape_invalid"; return result; }
                put(s.snapshotName); put(s.snapshotVolume); put(s.snapshotFileId); put(s.ordinal); put(s.coordinate);
                put(s.visitHeadName); put(s.visitHeadVolume); put(s.visitHeadFileId);
                put(s.scanExhausted ? "1" : "0"); put(std::to_string(static_cast<unsigned>(s.savedState)));
                for (auto n : {s.coordinateBudget.decimalDigits, s.coordinateBudget.tokenBytes, s.coordinateBudget.listEntries,
                    s.coordinateBudget.arithmeticSteps, s.axisBudget.logicalMembers, s.axisBudget.fileMembers, s.axisBudget.virtualMembers,
                    s.axisBudget.referenceMembers, s.axisBudget.payloadSlots, s.axisBudget.textSlots, s.axisBudget.requestResultSlots,
                    s.axisBudget.fileContentSlots, s.axisBudget.payloadBytes, s.axisBudget.directoryNodeSlots, s.axisBudget.directoryRootSlots, s.axisBudget.preparationSteps,
                    s.tokenBudget, s.replayWorkBudget, s.byteBudget, s.indexByteBudget, s.workBudget, s.nodeBudget,
                    s.pinBudget, s.snapshotByteBudget}) put(std::to_string(n));
            }
            put(state.familyRoundStep ? "1" : "0");
            if (state.familyRoundStep)
            {
                const auto& step = *state.familyRoundStep; const auto& s = step.selection;
                const auto validName = [](std::string_view name) {
                    return name.size() == 43 && name.starts_with("checkpoint-") &&
                        std::all_of(name.begin() + 11, name.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
                };
                const auto validId = [](std::string_view volume, std::string_view id) {
                    return V4CanonicalDecimal(volume) && id.size() == 32 && V4LowerHex(id) &&
                        std::any_of(id.begin(), id.end(), [](char c) { return c != '0'; });
                };
                if (state.familyVersion != FamilyVersion::v4 || !state.v4TypedEnabled || state.shardIndex || state.typedStep || state.roundAdvance || state.familySelection || state.familySettlement || state.previewHeadAnchor ||
                    !state.families.empty() || !state.ownedAssociations.empty() || !state.ownedTerminals.empty() ||
                    !validName(s.snapshotName) || !validId(s.snapshotVolume, s.snapshotFileId) ||
                    !validName(s.visitHeadName) || !validId(s.visitHeadVolume, s.visitHeadFileId) || !V4CanonicalDecimal(step.stepOrdinal) ||
                    !s.workBudget || step.workUsed > s.workBudget ||
                    (step.status != "progress" && step.status != "family_item" && step.status != "exhausted") ||
                    (step.hasItem != (step.status == "family_item")) || (step.exhausted != (step.status == "exhausted")) ||
                    (step.hasItem && (!V4CanonicalDecimal(s.ordinal) || !V4CanonicalDecimal(s.coordinate))) ||
                    (!step.hasItem && (!s.ordinal.empty() || !s.coordinate.empty() || s.scanExhausted || s.savedState != WorkState::pending)) ||
                    static_cast<unsigned>(s.savedState) > static_cast<unsigned>(WorkState::outcome_unavailable))
                { result.bytes.clear(); result.reason = "v4_family_round_step_shape_invalid"; return result; }
                put(s.snapshotName); put(s.snapshotVolume); put(s.snapshotFileId); put(s.visitHeadName); put(s.visitHeadVolume); put(s.visitHeadFileId);
                put(step.stepOrdinal); put(step.status); put(std::to_string(step.workUsed)); put(step.hasItem ? "1" : "0");
                put(step.exhausted ? "1" : "0"); put(s.ordinal); put(s.coordinate); put(s.scanExhausted ? "1" : "0");
                put(std::to_string(static_cast<unsigned>(s.savedState)));
                for (auto n : {s.coordinateBudget.decimalDigits, s.coordinateBudget.tokenBytes, s.coordinateBudget.listEntries,
                    s.coordinateBudget.arithmeticSteps, s.axisBudget.logicalMembers, s.axisBudget.fileMembers, s.axisBudget.virtualMembers,
                    s.axisBudget.referenceMembers, s.axisBudget.payloadSlots, s.axisBudget.textSlots, s.axisBudget.requestResultSlots,
                    s.axisBudget.fileContentSlots, s.axisBudget.payloadBytes, s.axisBudget.directoryNodeSlots, s.axisBudget.directoryRootSlots, s.axisBudget.preparationSteps,
                    s.tokenBudget, s.replayWorkBudget, s.byteBudget, s.indexByteBudget, s.workBudget, s.nodeBudget,
                    s.pinBudget, s.snapshotByteBudget}) put(std::to_string(n));
            }
            put(state.familySettlement ? "1" : "0");
            if (state.familySettlement)
            {
                const auto& s = *state.familySettlement;
                const auto valid = [](std::string_view name, std::string_view volume, std::string_view id) {
                    return name.size() == 43 && name.starts_with("checkpoint-") &&
                        std::all_of(name.begin() + 11, name.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) &&
                        V4CanonicalDecimal(volume) && id.size() == 32 && V4LowerHex(id) &&
                        std::any_of(id.begin(), id.end(), [](char c) { return c != '0'; });
                };
                const bool hasFamily = !s.familyName.empty(), hasCase = !s.caseName.empty();
                if (state.familyVersion != FamilyVersion::v4 || !state.v4TypedEnabled || state.shardIndex || state.typedStep || state.roundAdvance ||
                    state.familySelection || state.familyRoundStep || state.previewHeadAnchor || !state.families.empty() || !state.ownedAssociations.empty() || !state.ownedTerminals.empty() ||
                    !valid(s.selectionName, s.selectionVolume, s.selectionFileId) || !valid(s.previousName, s.previousVolume, s.previousFileId) ||
                    !V4CanonicalDecimal(s.coordinate) || (s.outcome != "visited" && s.outcome != "deferred" && s.outcome != "scan_exhausted") ||
                    (hasFamily != (s.outcome == "visited")) || (hasFamily && !valid(s.familyName, s.familyVolume, s.familyFileId)) ||
                    (!hasFamily && (!s.familyVolume.empty() || !s.familyFileId.empty())) || (hasCase && (!hasFamily || !valid(s.caseName, s.caseVolume, s.caseFileId))) ||
                    (!hasCase && (!s.caseVolume.empty() || !s.caseFileId.empty())))
                { result.bytes.clear(); result.reason = "v4_family_settlement_shape_invalid"; return result; }
                put(s.selectionName); put(s.selectionVolume); put(s.selectionFileId); put(s.previousName); put(s.previousVolume); put(s.previousFileId);
                put(s.coordinate); put(s.outcome); put(s.familyName); put(s.familyVolume); put(s.familyFileId); put(s.caseName); put(s.caseVolume); put(s.caseFileId);
            }
            put(state.previewHeadAnchor ? "1" : "0");
            if (state.previewHeadAnchor)
            {
                const auto& a = *state.previewHeadAnchor;
                const auto validId = [](std::string_view volume, std::string_view id) {
                    return V4CanonicalDecimal(volume) && id.size() == 32 && V4LowerHex(id) &&
                        std::any_of(id.begin(), id.end(), [](char c) { return c != '0'; });
                };
                const auto validName = [](std::string_view name, std::string_view prefix) {
                    return name.size() == prefix.size() + 32 && name.starts_with(prefix) && V4LowerHex(name.substr(prefix.size()));
                };
                if (state.familyVersion != FamilyVersion::v4 || !state.v4TypedEnabled || state.shardIndex || state.typedStep || state.roundAdvance ||
                    state.familySelection || state.familyRoundStep || state.familySettlement || !state.families.empty() ||
                    !state.ownedAssociations.empty() || !state.ownedTerminals.empty() ||
                    !validId(a.rootVolume, a.rootId) || !validName(a.directoryName, "v3-checkpoint-") ||
                    !validId(a.directoryVolume, a.directoryId) || !validName(a.headName, "checkpoint-") ||
                    !validId(a.headVolume, a.headId) || a.headBytes.empty())
                { result.bytes.clear(); result.reason = "v4_preview_head_anchor_shape_invalid"; return result; }
                put(a.rootVolume); put(a.rootId); put(a.directoryName); put(a.directoryVolume); put(a.directoryId);
                put(a.headName); put(a.headVolume); put(a.headId); put(a.headBytes);
            }
        }
        if (!legacyV13 && !legacyV14 && !legacyV15 && !legacyV16 && !legacyV17 && !legacyV18 && !legacyV19 && !legacyV20 && !legacyV21 && !legacyV22 && !legacyV23)
        {
            std::set<std::string> cases, executions;
            put(std::to_string(state.exactAttempts.size()));
            for (const auto& attempt : state.exactAttempts)
            {
                if (state.familyVersion != FamilyVersion::v4 || !state.v4TypedEnabled || state.nativeStagedFamily || state.shardIndex || state.typedStep ||
                    state.roundAdvance || state.familySelection || state.familyRoundStep || state.familySettlement || state.previewHeadAnchor ||
                    attempt.caseId.empty() || attempt.executionId.empty() || attempt.sourcePathUtf16Hex.empty() ||
                    attempt.sourcePathUtf16Hex.size() % 4 || !V4LowerHex(attempt.sourcePathUtf16Hex) ||
                    attempt.preBytes.empty() || attempt.flightBytes.empty() || attempt.previousName.size() != 43 ||
                    !attempt.previousName.starts_with("checkpoint-") || !V4LowerHex(std::string_view(attempt.previousName).substr(11)) ||
                    !V4CanonicalDecimal(attempt.previousVolume) || attempt.previousFileId.size() != 32 ||
                    !V4LowerHex(attempt.previousFileId) ||
                    std::none_of(attempt.previousFileId.begin(), attempt.previousFileId.end(), [](char c) { return c != '0'; }) ||
                    !cases.insert(attempt.caseId).second || !executions.insert(attempt.executionId).second)
                { result.bytes.clear(); result.reason = "v4_exact_attempt_shape_invalid"; return result; }
                put(attempt.caseId); put(attempt.executionId); put(attempt.sourcePathUtf16Hex); put(attempt.preBytes); put(attempt.flightBytes);
                put(attempt.previousName); put(attempt.previousVolume); put(attempt.previousFileId);
                for (std::size_t i = 0; i < attempt.fileIds.size(); ++i)
                {
                    if (!V4CanonicalDecimal(attempt.volumes[i]) || attempt.fileIds[i].size() != 32 ||
                        !V4LowerHex(attempt.fileIds[i]) || std::none_of(attempt.fileIds[i].begin(), attempt.fileIds[i].end(), [](char c) { return c != '0'; }))
                    { result.bytes.clear(); result.reason = "v4_exact_attempt_identity_invalid"; return result; }
                    put(attempt.volumes[i]); put(attempt.fileIds[i]);
                }
            }
        }
        if (!legacyV13 && !legacyV14 && !legacyV15 && !legacyV16 && !legacyV17 && !legacyV18 && !legacyV19 && !legacyV20 && !legacyV21 && !legacyV22 && !legacyV23 && !legacyV24)
        {
            put(state.nativeProjectionPage ? "1" : "0");
            if (state.nativeProjectionPage)
            {
                const auto& p = *state.nativeProjectionPage;
                if (state.familyVersion != FamilyVersion::v4 || !state.v4TypedEnabled || state.shardIndex || state.typedStep ||
                    state.roundAdvance || state.familySelection || state.familyRoundStep || state.familySettlement || state.previewHeadAnchor ||
                    !state.families.empty() || !state.ownedAssociations.empty() || !state.ownedTerminals.empty() || !state.exactAttempts.empty() ||
                    !state.v4TypedDeferred.empty() || !state.v4TypedPruned.empty() || state.v4TypedDeferredVisit || state.v4TypedDeferredNext ||
                    state.v4TypedCursor.layer || state.v4TypedCursor.stopDepth || !state.v4TypedCursor.prefix.empty() ||
                    state.v4TypedCursor.nextChoice != std::vector<std::uint32_t>{0} || state.v4TypedCursor.exhausted ||
                    !V4CanonicalDecimal(p.familyCoordinate) || p.familyCursor.empty() || p.candidateId.empty() ||
                    !V4CanonicalDecimal(p.candidateOrdinal) || !V4CanonicalDecimal(p.leafOrdinal) || !V4CanonicalDecimal(p.pageOrdinal) ||
                    p.advanceBytes.empty() || p.pageBytes.empty() || !std::all_of(p.leafPresence.begin(), p.leafPresence.end(), [](char c) { return c == '0' || c == '1'; }) ||
                    p.baseHeadName.size() != 43 || !p.baseHeadName.starts_with("checkpoint-") ||
                    !V4LowerHex(std::string_view(p.baseHeadName).substr(11)) || !V4CanonicalDecimal(p.baseHeadVolume) ||
                    p.baseHeadFileId.size() != 32 || !V4LowerHex(p.baseHeadFileId) ||
                    std::none_of(p.baseHeadFileId.begin(), p.baseHeadFileId.end(), [](char c) { return c != '0'; }))
                { result.bytes.clear(); result.reason = "v4_native_projection_page_shape_invalid"; return result; }
                put(p.familyCoordinate); put(p.familyCursor); put(p.candidateId); put(p.candidateOrdinal);
                put(p.leafOrdinal); put(p.leafPresence); put(p.pageOrdinal); put(p.advanceBytes); put(p.pageBytes);
                put(p.baseHeadName); put(p.baseHeadVolume); put(p.baseHeadFileId);
                const std::array<std::size_t, 20> numbers{p.coordinateBudget.decimalDigits, p.coordinateBudget.tokenBytes,
                    p.coordinateBudget.listEntries, p.coordinateBudget.arithmeticSteps, p.axisBudget.logicalMembers,
                    p.axisBudget.fileMembers, p.axisBudget.virtualMembers, p.axisBudget.referenceMembers, p.axisBudget.payloadSlots,
                    p.axisBudget.textSlots, p.axisBudget.requestResultSlots, p.axisBudget.fileContentSlots, p.axisBudget.payloadBytes,
                    p.axisBudget.directoryNodeSlots, p.axisBudget.directoryRootSlots, p.axisBudget.preparationSteps,
                    p.advanceWorkBudget, p.preambleBudget, p.workBudget, p.outputLimit};
                for (const auto value : numbers) put(std::to_string(value));
            }
            put(state.nativeStagedFamily ? "1" : "0");
            if (state.nativeStagedFamily)
            {
                const auto& f = *state.nativeStagedFamily;
                if (state.familyVersion != FamilyVersion::v4 || !state.v4TypedEnabled || state.nativeProjectionPage || state.shardIndex || state.typedStep ||
                    state.roundAdvance || state.familySelection || state.familyRoundStep || state.familySettlement || state.previewHeadAnchor ||
                    !state.families.empty() || !state.ownedAssociations.empty() || !state.ownedTerminals.empty() || !state.exactAttempts.empty() ||
                    !state.v4TypedDeferred.empty() || !state.v4TypedPruned.empty() || state.v4TypedDeferredVisit || state.v4TypedDeferredNext ||
                    state.v4TypedCursor.layer || state.v4TypedCursor.stopDepth || !state.v4TypedCursor.prefix.empty() ||
                    state.v4TypedCursor.nextChoice != std::vector<std::uint32_t>{0} || state.v4TypedCursor.exhausted ||
                    f.journalHeadName.size() != 43 || !f.journalHeadName.starts_with("checkpoint-") ||
                    !V4LowerHex(std::string_view(f.journalHeadName).substr(11)) || !V4CanonicalDecimal(f.journalHeadVolume) ||
                    f.journalHeadFileId.size() != 32 || !V4LowerHex(f.journalHeadFileId) ||
                    std::none_of(f.journalHeadFileId.begin(), f.journalHeadFileId.end(), [](char c) { return c != '0'; }) ||
                    !V4CanonicalDecimal(f.familyCoordinate) || f.beforeCursor.empty() || f.afterCursor.empty() || f.proposalBytes.empty() ||
                    (f.stage != "attach" && f.stage != "graph" && f.stage != "continue" && f.stage != "next_candidate"))
                { result.bytes.clear(); result.reason = "v4_native_staged_family_shape_invalid"; return result; }
                put(f.journalHeadName); put(f.journalHeadVolume); put(f.journalHeadFileId); put(f.familyCoordinate);
                put(f.beforeCursor); put(f.afterCursor); put(f.stage); put(f.proposalBytes); put(std::to_string(f.workBudget));
            }
        }
        if (!legacyV13 && !legacyV14 && !legacyV15 && !legacyV16 && !legacyV17 && !legacyV18 && !legacyV19 &&
            !legacyV20 && !legacyV21 && !legacyV22 && !legacyV23 && !legacyV24 && !legacyV25)
        {
            put(state.nativeCaseIndex ? "1" : "0");
            if (state.nativeCaseIndex)
            {
                const auto& a = *state.nativeCaseIndex;
                const auto validName = [](std::string_view name) { return name.size() == 43 && name.starts_with("checkpoint-") && V4LowerHex(name.substr(11)); };
                const auto validId = [](std::string_view volume, std::string_view id) { return V4CanonicalDecimal(volume) && id.size() == 32 && V4LowerHex(id) &&
                    std::any_of(id.begin(), id.end(), [](char c) { return c != '0'; }); };
                if (state.familyVersion != FamilyVersion::v4 || !state.v4TypedEnabled || state.nativeProjectionPage || state.nativeStagedFamily ||
                    state.shardIndex || state.typedStep || state.roundAdvance || state.familySelection || state.familyRoundStep ||
                    state.familySettlement || state.previewHeadAnchor || !state.families.empty() || !state.ownedAssociations.empty() ||
                    !state.ownedTerminals.empty() || !state.exactAttempts.empty() || !state.v4TypedDeferred.empty() || !state.v4TypedPruned.empty() ||
                    state.v4TypedDeferredVisit || state.v4TypedDeferredNext || state.v4TypedCursor.layer || state.v4TypedCursor.stopDepth ||
                    !state.v4TypedCursor.prefix.empty() || state.v4TypedCursor.nextChoice != std::vector<std::uint32_t>{0} || state.v4TypedCursor.exhausted ||
                    !validName(a.parentName) || !validId(a.parentVolume, a.parentFileId) || !validName(a.pageName) ||
                    !validId(a.pageVolume, a.pageFileId) || a.caseId.empty())
                { result.bytes.clear(); result.reason = "v4_native_case_index_shape_invalid"; return result; }
                put(a.parentName); put(a.parentVolume); put(a.parentFileId); put(a.pageName); put(a.pageVolume); put(a.pageFileId);
                put(a.caseId); put(std::to_string(a.priorSteps));
            }
        }
        if (out.size() > byteBudget)
        { result.bytes.clear(); result.status = CodecStatus::resource_unavailable; result.reason = "v3_checkpoint_byte_budget"; return result; }
        result.status = CodecStatus::encoded;
        return result;
    }
    catch (const CheckpointBudgetExceeded&) { result.bytes.clear(); result.status = CodecStatus::resource_unavailable; result.reason = "v3_checkpoint_byte_budget"; return result; }
    catch (const std::bad_alloc&) { result.bytes.clear(); result.status = CodecStatus::resource_unavailable; result.reason = "v3_checkpoint_allocation"; return result; }
    catch (const std::length_error&) { result.bytes.clear(); result.status = CodecStatus::resource_unavailable; result.reason = "v3_checkpoint_allocation"; return result; }
}

inline CodecResult DecodeCheckpoint(std::string_view bytes, std::string_view expectedBinding, std::size_t byteBudget)
{
    CodecResult result;
    if (bytes.size() > byteBudget)
    { result.status = CodecStatus::resource_unavailable; result.reason = "v3_checkpoint_byte_budget"; return result; }
    try
    {
        std::string_view remaining = bytes, token;
        const auto read = [&] { return ReadCheckpointToken(remaining, token); };
        const auto invalid = [&]() { result.state = {}; result.reason = "v3_checkpoint_encoding_invalid"; return result; };
        if (!read() || (token != "family_scheduler_checkpoint_v13" && token != "family_scheduler_checkpoint_v14" &&
            token != "family_scheduler_checkpoint_v15" && token != "family_scheduler_checkpoint_v16" &&
            token != "family_scheduler_checkpoint_v17" && token != "family_scheduler_checkpoint_v18" &&
            token != "family_scheduler_checkpoint_v19" && token != "family_scheduler_checkpoint_v20" &&
            token != "family_scheduler_checkpoint_v21" && token != "family_scheduler_checkpoint_v22" && token != "family_scheduler_checkpoint_v23" &&
            token != "family_scheduler_checkpoint_v24" && token != "family_scheduler_checkpoint_v25" && token != "family_scheduler_checkpoint_v26")) return invalid();
        const bool legacyV13 = token == "family_scheduler_checkpoint_v13";
        const bool legacyV14 = token == "family_scheduler_checkpoint_v14";
        const bool legacyV15 = token == "family_scheduler_checkpoint_v15";
        const bool legacyV16 = token == "family_scheduler_checkpoint_v16";
        const bool legacyV17 = token == "family_scheduler_checkpoint_v17";
        const bool legacyV18 = token == "family_scheduler_checkpoint_v18";
        const bool legacyV19 = token == "family_scheduler_checkpoint_v19";
        const bool legacyV20 = token == "family_scheduler_checkpoint_v20";
        const bool legacyV21 = token == "family_scheduler_checkpoint_v21";
        const bool legacyV22 = token == "family_scheduler_checkpoint_v22";
        const bool legacyV23 = token == "family_scheduler_checkpoint_v23";
        const bool legacyV24 = token == "family_scheduler_checkpoint_v24";
        const bool legacyV25 = token == "family_scheduler_checkpoint_v25";
        if (!read() || token != Algorithm || !read()) return invalid();
        if (token == "v3") result.state.familyVersion = FamilyVersion::v3;
        else if (token == "v4") result.state.familyVersion = FamilyVersion::v4;
        else return invalid();
        if (!read() || token != (result.state.familyVersion == FamilyVersion::v3 ? V3FamilyCoordinateAlgorithm : V4FamilyCoordinateAlgorithm) ||
            !read() || token != (result.state.familyVersion == FamilyVersion::v3 ? event_graph::V3FamilyCandidateGraphAlgorithm : event_graph::V4FamilyCandidateGraphAlgorithm) ||
            !read() || token != expectedBinding || token.empty()) return invalid();
        result.state.binding = token;
        if (!read()) return invalid();
        result.state.nextFamily = token;
        if (!read() || (token != "0" && token != "1")) return invalid();
        result.state.v4SeedEmitted = token == "1";
        if (!read() || (token != "0" && token != "1")) return invalid();
        result.state.v4SeedNext = token == "1";
        if (!read()) return invalid();
        result.state.v4SeedCoordinate = token;
        if (!read() || (token != "0" && token != "1")) return invalid();
        result.state.v4TextSeedNext = token == "1";
        std::size_t textSeeds = 0;
        if (!read() || !CheckpointSize(token, textSeeds) || textSeeds > V4AdditionalTextSeedCount || textSeeds > remaining.size() / 2) return invalid();
        result.state.v4TextSeedCoordinates.reserve(textSeeds);
        for (std::size_t i = 0; i < textSeeds; ++i) { if (!read()) return invalid(); result.state.v4TextSeedCoordinates.emplace_back(token); }
        if (!read() || token != V4FieldLayerAlgorithm) return invalid();
        const auto readFieldCursor = [&](V4FieldLayerCursor& cursor) {
            if (!read()) return false;
            cursor.layer = token;
            if (!read() || (token != "0" && token != "1")) return false;
            cursor.activeB = token == "1";
            std::size_t count = 0;
            if (!read() || !CheckpointSize(token, count) || count != (cursor.activeB ? 22u : 11u)) return false;
            cursor.fields.clear(); cursor.fields.reserve(count);
            for (std::size_t i = 0; i < count; ++i) { if (!read()) return false; cursor.fields.emplace_back(token); }
            return V4FieldLayerCursorCanonical(cursor);
        };
        if (!readFieldCursor(result.state.v4FieldCursor) || !read() || (token != "0" && token != "1")) return invalid();
        result.state.v4DeferredNext = token == "1";
        if (!read() || !CheckpointSize(token, result.state.v4DeferredVisit)) return invalid();
        for (auto* entries : {&result.state.v4Deferred, &result.state.v4FieldHoles})
        {
            std::size_t count = 0;
            if (!read() || !CheckpointSize(token, count) || count > remaining.size() / 28) return invalid();
            entries->reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            { V4FieldLayerCursor cursor; if (!readFieldCursor(cursor)) return invalid(); entries->push_back(std::move(cursor)); }
        }
        if (!legacyV13)
        {
            if (!read() || token != V4StructuredPrefixAlgorithm || !read() || token != V4StructuredDfsAlgorithm ||
                !read() || (token != "0" && token != "1")) return invalid();
            result.state.v4TypedEnabled = token == "1";
            const auto readTypedCursor = [&](V4StructuredDfsCursor& cursor) {
                std::size_t count = 0;
                if (!read() || !CheckpointSize(token, cursor.layer) || !read() || !CheckpointSize(token, cursor.stopDepth) ||
                    !read() || !CheckpointSize(token, count) || count > remaining.size() / 2) return false;
                cursor.prefix.clear(); cursor.prefix.reserve(count);
                for (std::size_t i = 0; i < count; ++i)
                {
                    std::size_t value = 0; if (!read() || !CheckpointSize(token, value) || value > (std::numeric_limits<std::uint32_t>::max)()) return false;
                    cursor.prefix.push_back(static_cast<std::uint32_t>(value));
                }
                if (!read() || !CheckpointSize(token, count) || count > remaining.size() / 2) return false;
                cursor.nextChoice.clear(); cursor.nextChoice.reserve(count);
                for (std::size_t i = 0; i < count; ++i)
                {
                    std::size_t value = 0; if (!read() || !CheckpointSize(token, value) || value > (std::numeric_limits<std::uint32_t>::max)()) return false;
                    cursor.nextChoice.push_back(static_cast<std::uint32_t>(value));
                }
                if (!read() || (token != "0" && token != "1")) return false;
                cursor.exhausted = token == "1"; return true;
            };
            if (!readTypedCursor(result.state.v4TypedCursor) || !read() || (token != "0" && token != "1")) return invalid();
            result.state.v4TypedDeferredNext = token == "1";
            std::size_t count = 0;
            if (!read() || !CheckpointSize(token, result.state.v4TypedDeferredVisit) ||
                !read() || !CheckpointSize(token, count) || count > remaining.size() / 12) return invalid();
            result.state.v4TypedDeferred.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            { V4StructuredDfsCursor cursor; if (!readTypedCursor(cursor)) return invalid(); result.state.v4TypedDeferred.push_back(std::move(cursor)); }
            if (!read() || !CheckpointSize(token, count) || count > remaining.size() / 4) return invalid();
            result.state.v4TypedPruned.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                V4TypedGrammarPrune prune; std::size_t tokens = 0;
                if (!read() || !CheckpointSize(token, prune.layer) || !read() || !CheckpointSize(token, tokens) || tokens > remaining.size() / 2) return invalid();
                prune.prefix.reserve(tokens);
                for (std::size_t j = 0; j < tokens; ++j)
                {
                    std::size_t value = 0; if (!read() || !CheckpointSize(token, value) || value > (std::numeric_limits<std::uint32_t>::max)()) return invalid();
                    prune.prefix.push_back(static_cast<std::uint32_t>(value));
                }
                result.state.v4TypedPruned.push_back(std::move(prune));
            }
        }
        std::size_t visit = 0, visitsRemaining = 0, familyCount = 0;
        if (!read() || !CheckpointSize(token, visit) || !read() || !CheckpointSize(token, visitsRemaining) ||
            visitsRemaining > 2 || !read() || !CheckpointSize(token, familyCount) || familyCount > remaining.size() / 12)
            return invalid();
        result.state.nextVisit = visit; result.state.visitsRemaining = static_cast<unsigned>(visitsRemaining);
        result.state.families.reserve(familyCount);
        for (std::size_t i = 0; i < familyCount; ++i)
        {
            FamilyState family;
            if (!read()) return invalid();
            family.coordinate = token;
            if (!read()) return invalid();
            if (token != "0" && token != "1") return invalid();
            family.encodingHole = token == "1";
            if (!read() || (token != "0" && token != "1")) return invalid();
            family.discoverySeed = token == "1";
            if (!read() || (token != "0" && token != "1")) return invalid();
            family.duplicateSeed = token == "1";
            family.version = result.state.familyVersion;
            if (!read()) return invalid();
            family.candidateCursor = token;
            if (!read()) return invalid();
            const auto state = ParseWorkState(token);
            if (!state || !read()) return invalid();
            family.state = *state;
            if (token != "0" && token != "1") return invalid();
            family.scanExhausted = token == "1";
            if (!read() || !CheckpointSize(token, family.nextRetry) || !read()) return invalid();
            std::size_t caseCount = 0;
            if (!CheckpointSize(token, caseCount) || caseCount > remaining.size() / 24) return invalid();
            family.cases.reserve(caseCount);
            for (std::size_t j = 0; j < caseCount; ++j)
            {
                CaseRecord item;
                if (!read()) return invalid();
                item.candidateId = token;
                if (!read()) return invalid();
                item.graphId = token;
                if (!read()) return invalid();
                item.executionId = token;
                if (!read()) return invalid();
                const auto work = ParseWorkState(token);
                if (!work || !read()) return invalid();
                item.state = *work; item.result = token;
                if (!read()) return invalid();
                item.kind = token;
                if (!read()) return invalid();
                item.candidateOrdinal = token;
                if (!read()) return invalid();
                item.beforeCursor = token;
                if (!read()) return invalid();
                item.presence = token;
                if (!read()) return invalid();
                item.reason = token;
                if (!read()) return invalid();
                item.graphMode = token;
                if (!read() || (token != "0" && token != "1")) return invalid();
                item.directSeed = token == "1";
                std::size_t valueCount = 0;
                if (!read() || !CheckpointSize(token, valueCount) || valueCount > remaining.size() / 2) return invalid();
                item.candidateValues.reserve(valueCount);
                for (std::size_t k = 0; k < valueCount; ++k) { if (!read()) return invalid(); item.candidateValues.emplace_back(token); }
                if (!read() || !CheckpointSize(token, item.replayWork)) return invalid();
                std::size_t reasons = 0;
                if (!read() || !CheckpointSize(token, reasons) || reasons > remaining.size() / 2) return invalid();
                item.unresolved.reserve(reasons);
                for (std::size_t k = 0; k < reasons; ++k)
                { if (!read()) return invalid(); item.unresolved.emplace_back(token); }
                std::size_t attempts = 0;
                if (!read() || !CheckpointSize(token, attempts) || attempts > remaining.size() / 28) return invalid();
                item.history.reserve(attempts);
                for (std::size_t k = 0; k < attempts; ++k)
                {
                    AttemptHistory history;
                    if (!read()) return invalid();
                    history.executionId = token;
                    if (!read()) return invalid();
                    history.status = token;
                    if (!read()) return invalid();
                    history.failure = token;
                    if (!read()) return invalid();
                    history.evidenceVersion = token;
                    if (!read()) return invalid();
                    history.evidenceBytes = token;
                    for (bool* flag : {&history.receiptConsumed, &history.bridgeEntered, &history.productionStarted,
                        &history.productionEntryUnknown, &history.eventOracleFailureObserved, &history.businessFailureObserved, &history.observationUnavailable})
                    { if (!read() || (token != "0" && token != "1")) return invalid(); *flag = token == "1"; }
                    if (!legacyV13 && !legacyV14)
                    {
                        if (!read() || (token != "0" && token != "1")) return invalid(); history.hypothesisObservationRecorded = token == "1";
                        if (!read() || (token != "0" && token != "1")) return invalid(); history.hypothesisNotMetObserved = token == "1";
                    }
                    if (!legacyV13 && !legacyV14 && !legacyV15)
                    {
                        if (!read() || (token != "0" && token != "1")) return invalid(); history.eventDispatchObservationRecorded = token == "1";
                        if (!read() || (token != "0" && token != "1")) return invalid(); history.eventDispatchFailureObserved = token == "1";
                    }
                    for (auto* values : {&history.obligations, &history.notApplicable, &history.graphUnresolved,
                        &history.postDispatchObligations, &history.unclassifiedObligations})
                    {
                        std::size_t count = 0;
                        if (!read() || !CheckpointSize(token, count) || count > remaining.size() / 2) return invalid();
                        values->reserve(count);
                        for (std::size_t j = 0; j < count; ++j) { if (!read()) return invalid(); values->emplace_back(token); }
                    }
                    item.history.push_back(std::move(history));
                }
                family.cases.push_back(std::move(item));
            }
            std::size_t auditCount = 0;
            if (!read() || !CheckpointSize(token, auditCount) || auditCount > remaining.size() / 10) return invalid();
            family.pruneSteps.reserve(auditCount);
            for (std::size_t j = 0; j < auditCount; ++j)
            {
                PruneAuditStep audit;
                if (!read()) return invalid();
                audit.beforeCursor = token;
                if (!read()) return invalid();
                audit.afterCursor = token;
                std::size_t presenceCount = 0;
                if (!read() || !CheckpointSize(token, audit.workUsed) || !read() ||
                    !CheckpointSize(token, presenceCount) || presenceCount > remaining.size() / 12) return invalid();
                audit.presence.reserve(presenceCount);
                for (std::size_t k = 0; k < presenceCount; ++k)
                {
                    event_graph::V3FamilyPresencePrune entry;
                    if (!read()) return invalid();
                    entry.candidateOrdinal = token;
                    if (!read()) return invalid();
                    entry.candidateId = token;
                    std::size_t prefixCount = 0;
                    if (!read() || !CheckpointSize(token, prefixCount) || prefixCount > remaining.size() / 2) return invalid();
                    entry.prune.prefix.reserve(prefixCount);
                    for (std::size_t m = 0; m < prefixCount; ++m)
                    { std::size_t value = 0; if (!read() || !CheckpointSize(token, value)) return invalid(); entry.prune.prefix.push_back(value); }
                    if (!read()) return invalid();
                    entry.prune.reason = token;
                    if (!read()) return invalid();
                    entry.prune.premise = token;
                    if (!read()) return invalid();
                    entry.prune.consequence = token;
                    audit.presence.push_back(std::move(entry));
                }
                std::size_t activeCount = 0;
                if (!read() || !CheckpointSize(token, activeCount) || activeCount > remaining.size() / 18) return invalid();
                audit.active.reserve(activeCount);
                for (std::size_t k = 0; k < activeCount; ++k)
                {
                    event_graph::V3FamilyActivePrune entry;
                    if (!read()) return invalid();
                    entry.candidateOrdinal = token;
                    if (!read()) return invalid();
                    entry.candidateId = token;
                    if (!read()) return invalid();
                    entry.prune.presenceOrdinal = token;
                    if (!read()) return invalid();
                    for (const char bit : token)
                    { if (bit != '0' && bit != '1') return invalid(); entry.prune.present.push_back(bit == '1'); }
                    if (!read()) return invalid();
                    entry.prune.prune.scope = token;
                    if (!read()) return invalid();
                    entry.prune.prune.reason = token;
                    std::size_t partitionCount = 0;
                    if (!read() || !CheckpointSize(token, partitionCount) || partitionCount > remaining.size() / 2) return invalid();
                    entry.prune.prune.partition.reserve(partitionCount);
                    for (std::size_t m = 0; m < partitionCount; ++m)
                    { std::size_t value = 0; if (!read() || !CheckpointSize(token, value)) return invalid(); entry.prune.prune.partition.push_back(value); }
                    if (!read()) return invalid();
                    entry.prune.prune.earlierRequirement = token;
                    if (!read()) return invalid();
                    entry.prune.prune.currentRequirement = token;
                    audit.active.push_back(std::move(entry));
                }
                family.pruneSteps.push_back(std::move(audit));
            }
            result.state.families.push_back(std::move(family));
        }
        if (!legacyV13 && !legacyV14 && !legacyV15 && !legacyV16)
        {
            std::size_t count = 0;
            if (!read() || !CheckpointSize(token, count) || count > remaining.size() / 12) return invalid();
            result.state.ownedAssociations.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                OwnedCaseAssociation association;
                if (!read()) return invalid(); association.templateCaseId = token;
                if (!read()) return invalid(); association.ownedCaseId = token;
                if (!read()) return invalid(); association.profileVolume = token;
                if (!read()) return invalid(); association.profileFileId = token;
                if (!read()) return invalid(); association.sourceVolume = token;
                if (!read()) return invalid(); association.sourceFileId = token;
                result.state.ownedAssociations.push_back(std::move(association));
            }
        }
        if (!legacyV13 && !legacyV14 && !legacyV15 && !legacyV16 && !legacyV17)
        {
            std::size_t count = 0;
            if (!read() || !CheckpointSize(token, count) || count > remaining.size() / 28) return invalid();
            result.state.ownedTerminals.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                OwnedCaseTerminal terminal;
                if (!read()) return invalid(); terminal.templateCaseId = token;
                if (!read()) return invalid(); terminal.ownedCaseId = token;
                if (!read()) return invalid(); terminal.executionId = token;
                if (!read()) return invalid(); terminal.terminalBytes = token;
                for (std::size_t j = 0; j < terminal.fileIds.size(); ++j)
                { if (!read()) return invalid(); terminal.volumes[j] = token;
                  if (!read()) return invalid(); terminal.fileIds[j] = token; }
                result.state.ownedTerminals.push_back(std::move(terminal));
            }
        }
        if (!legacyV13 && !legacyV14 && !legacyV15 && !legacyV16 && !legacyV17 && !legacyV18)
        {
            if (!read() || (token != "0" && token != "1")) return invalid();
            if (token == "1")
            {
                V4ShardIndexNode node;
                if (!read()) return invalid(); node.generation = token;
                if (!read()) return invalid(); node.previousName = token;
                if (!read()) return invalid(); node.previousVolume = token;
                if (!read()) return invalid(); node.previousFileId = token;
                if (!read()) return invalid(); node.dataKind = token;
                if (!read()) return invalid(); node.dataName = token;
                if (!read()) return invalid(); node.dataVolume = token;
                if (!read()) return invalid(); node.dataFileId = token;
                result.state.shardIndex = std::move(node);
            }
        }
        if (!legacyV13 && !legacyV14 && !legacyV15 && !legacyV16 && !legacyV17 && !legacyV18 && !legacyV19 && !legacyV20)
        {
            if (!read() || (token != "0" && token != "1")) return invalid();
            if (token == "1")
            {
                V4TypedStepAudit step;
                const auto readCursor = [&](V4StructuredDfsCursor& cursor) {
                    std::size_t count = 0, value = 0;
                    if (!read() || !CheckpointSize(token, cursor.layer) || !read() || !CheckpointSize(token, cursor.stopDepth) ||
                        !read() || !CheckpointSize(token, count) || count > remaining.size() / 2) return false;
                    cursor.prefix.clear(); cursor.prefix.reserve(count);
                    for (std::size_t i = 0; i < count; ++i)
                    { if (!read() || !CheckpointSize(token, value) || value > (std::numeric_limits<std::uint32_t>::max)()) return false;
                      cursor.prefix.push_back(static_cast<std::uint32_t>(value)); }
                    if (!read() || !CheckpointSize(token, count) || count > remaining.size() / 2) return false;
                    cursor.nextChoice.clear(); cursor.nextChoice.reserve(count);
                    for (std::size_t i = 0; i < count; ++i)
                    { if (!read() || !CheckpointSize(token, value) || value > (std::numeric_limits<std::uint32_t>::max)()) return false;
                      cursor.nextChoice.push_back(static_cast<std::uint32_t>(value)); }
                    if (!read() || (token != "0" && token != "1")) return false;
                    cursor.exhausted = token == "1"; return true;
                };
                if (!readCursor(step.before) || !readCursor(step.after) || !read() || (token != "0" && token != "1")) return invalid();
                if (token == "1") { V4StructuredDfsCursor c; if (!readCursor(c)) return invalid(); step.deferred = std::move(c); }
                std::array<std::size_t*, 7> numbers{&step.coordinateBudget.decimalDigits, &step.coordinateBudget.tokenBytes,
                    &step.coordinateBudget.listEntries, &step.coordinateBudget.arithmeticSteps, &step.tokenBudget, &step.workBudget, &step.workUsed};
                for (auto value : numbers) if (!read() || !CheckpointSize(token, *value)) return invalid();
                std::size_t status = 0, count = 0;
                if (!read() || !CheckpointSize(token, status) || status > static_cast<std::size_t>(V4StructuredDfsStatus::exhausted) ||
                    !read() || !CheckpointSize(token, count) || count > remaining.size() / 2) return invalid();
                step.status = static_cast<V4StructuredDfsStatus>(status); step.prunedPrefix.reserve(count);
                for (std::size_t i = 0; i < count; ++i)
                { std::size_t value = 0; if (!read() || !CheckpointSize(token, value) || value > (std::numeric_limits<std::uint32_t>::max)()) return invalid();
                  step.prunedPrefix.push_back(static_cast<std::uint32_t>(value)); }
                if (!read()) return invalid(); step.familyCoordinate = token;
                if (!read()) return invalid(); step.taskId = token;
                if (!read()) return invalid(); step.stepOrdinal = token;
                if (!read()) return invalid(); step.deferredTaskId = token;
                result.state.typedStep = std::move(step);
            }
        }
        if (!legacyV13 && !legacyV14 && !legacyV15 && !legacyV16 && !legacyV17 && !legacyV18 && !legacyV19 && !legacyV20 && !legacyV21)
        {
            if (!read() || (token != "0" && token != "1")) return invalid();
            if (token == "1")
            {
                V4RoundAdvanceAudit step;
                if (!read()) return invalid(); step.snapshotName = token;
                if (!read()) return invalid(); step.snapshotVolume = token;
                if (!read()) return invalid(); step.snapshotFileId = token;
                if (!read()) return invalid(); step.beforePosition = token;
                if (!read()) return invalid(); step.afterPosition = token;
                if (!read()) return invalid(); step.status = token;
                if (!read()) return invalid(); step.taskId = token;
                if (!read()) return invalid(); step.nextOrdinal = token;
                if (!read() || (token != "0" && token != "1")) return invalid(); step.fromDeferred = token == "1";
                if (step.status == "item")
                {
                    V4StructuredDfsCursor c; std::size_t count = 0, value = 0;
                    if (!read() || !CheckpointSize(token, c.layer) || !read() || !CheckpointSize(token, c.stopDepth) ||
                        !read() || !CheckpointSize(token, count) || count > remaining.size() / 2) return invalid();
                    c.prefix.clear(); c.prefix.reserve(count);
                    for (std::size_t i = 0; i < count; ++i)
                    { if (!read() || !CheckpointSize(token, value) || value > (std::numeric_limits<std::uint32_t>::max)()) return invalid(); c.prefix.push_back(static_cast<std::uint32_t>(value)); }
                    if (!read() || !CheckpointSize(token, count) || count > remaining.size() / 2) return invalid();
                    c.nextChoice.clear(); c.nextChoice.reserve(count);
                    for (std::size_t i = 0; i < count; ++i)
                    { if (!read() || !CheckpointSize(token, value) || value > (std::numeric_limits<std::uint32_t>::max)()) return invalid(); c.nextChoice.push_back(static_cast<std::uint32_t>(value)); }
                    if (!read() || (token != "0" && token != "1")) return invalid(); c.exhausted = token == "1";
                    step.itemCursor = std::move(c);
                }
                std::array<std::size_t*, 10> numbers{&step.budget.decimalDigits, &step.budget.tokenBytes, &step.budget.listEntries,
                    &step.budget.arithmeticSteps, &step.tokenBudget, &step.replayWorkBudget, &step.byteBudget,
                    &step.nodeBudget, &step.pinBudget, &step.snapshotByteBudget};
                for (auto value : numbers) if (!read() || !CheckpointSize(token, *value)) return invalid();
                result.state.roundAdvance = std::move(step);
            }
        }
        if (!legacyV13 && !legacyV14 && !legacyV15 && !legacyV16 && !legacyV17 && !legacyV18 && !legacyV19 && !legacyV20 && !legacyV21 && !legacyV22)
        {
            if (!read() || (token != "0" && token != "1")) return invalid();
            if (token == "1")
            {
                V4FamilySelectionAudit s; std::size_t value = 0;
                if (!read()) return invalid(); s.snapshotName = token;
                if (!read()) return invalid(); s.snapshotVolume = token;
                if (!read()) return invalid(); s.snapshotFileId = token;
                if (!read()) return invalid(); s.ordinal = token;
                if (!read()) return invalid(); s.coordinate = token;
                if (!read()) return invalid(); s.visitHeadName = token;
                if (!read()) return invalid(); s.visitHeadVolume = token;
                if (!read()) return invalid(); s.visitHeadFileId = token;
                if (!read() || (token != "0" && token != "1")) return invalid(); s.scanExhausted = token == "1";
                if (!read() || !CheckpointSize(token, value) || value > static_cast<std::size_t>(WorkState::outcome_unavailable)) return invalid();
                s.savedState = static_cast<WorkState>(value);
                std::array<std::size_t*, 24> fields{&s.coordinateBudget.decimalDigits, &s.coordinateBudget.tokenBytes,
                    &s.coordinateBudget.listEntries, &s.coordinateBudget.arithmeticSteps, &s.axisBudget.logicalMembers,
                    &s.axisBudget.fileMembers, &s.axisBudget.virtualMembers, &s.axisBudget.referenceMembers, &s.axisBudget.payloadSlots,
                    &s.axisBudget.textSlots, &s.axisBudget.requestResultSlots, &s.axisBudget.fileContentSlots, &s.axisBudget.payloadBytes,
                    &s.axisBudget.directoryNodeSlots, &s.axisBudget.directoryRootSlots, &s.axisBudget.preparationSteps, &s.tokenBudget, &s.replayWorkBudget,
                    &s.byteBudget, &s.indexByteBudget, &s.workBudget, &s.nodeBudget, &s.pinBudget, &s.snapshotByteBudget};
                for (auto n : fields) if (!read() || !CheckpointSize(token, *n)) return invalid();
                result.state.familySelection = std::move(s);
            }
            if (!read() || (token != "0" && token != "1")) return invalid();
            if (token == "1")
            {
                V4FamilyRoundStepAudit step; auto& s = step.selection; std::size_t value = 0;
                if (!read()) return invalid(); s.snapshotName = token;
                if (!read()) return invalid(); s.snapshotVolume = token;
                if (!read()) return invalid(); s.snapshotFileId = token;
                if (!read()) return invalid(); s.visitHeadName = token;
                if (!read()) return invalid(); s.visitHeadVolume = token;
                if (!read()) return invalid(); s.visitHeadFileId = token;
                if (!read()) return invalid(); step.stepOrdinal = token;
                if (!read()) return invalid(); step.status = token;
                if (!read() || !CheckpointSize(token, step.workUsed)) return invalid();
                if (!read() || (token != "0" && token != "1")) return invalid(); step.hasItem = token == "1";
                if (!read() || (token != "0" && token != "1")) return invalid(); step.exhausted = token == "1";
                if (!read()) return invalid(); s.ordinal = token;
                if (!read()) return invalid(); s.coordinate = token;
                if (!read() || (token != "0" && token != "1")) return invalid(); s.scanExhausted = token == "1";
                if (!read() || !CheckpointSize(token, value) || value > static_cast<std::size_t>(WorkState::outcome_unavailable)) return invalid();
                s.savedState = static_cast<WorkState>(value);
                std::array<std::size_t*, 24> fields{&s.coordinateBudget.decimalDigits, &s.coordinateBudget.tokenBytes,
                    &s.coordinateBudget.listEntries, &s.coordinateBudget.arithmeticSteps, &s.axisBudget.logicalMembers,
                    &s.axisBudget.fileMembers, &s.axisBudget.virtualMembers, &s.axisBudget.referenceMembers, &s.axisBudget.payloadSlots,
                    &s.axisBudget.textSlots, &s.axisBudget.requestResultSlots, &s.axisBudget.fileContentSlots, &s.axisBudget.payloadBytes,
                    &s.axisBudget.directoryNodeSlots, &s.axisBudget.directoryRootSlots, &s.axisBudget.preparationSteps, &s.tokenBudget, &s.replayWorkBudget,
                    &s.byteBudget, &s.indexByteBudget, &s.workBudget, &s.nodeBudget, &s.pinBudget, &s.snapshotByteBudget};
                for (auto n : fields) if (!read() || !CheckpointSize(token, *n)) return invalid();
                result.state.familyRoundStep = std::move(step);
            }
            if (!read() || (token != "0" && token != "1")) return invalid();
            if (token == "1")
            {
                V4FamilySettlementAudit s;
                if (!read()) return invalid(); s.selectionName = token;
                if (!read()) return invalid(); s.selectionVolume = token;
                if (!read()) return invalid(); s.selectionFileId = token;
                if (!read()) return invalid(); s.previousName = token;
                if (!read()) return invalid(); s.previousVolume = token;
                if (!read()) return invalid(); s.previousFileId = token;
                if (!read()) return invalid(); s.coordinate = token;
                if (!read()) return invalid(); s.outcome = token;
                if (!read()) return invalid(); s.familyName = token;
                if (!read()) return invalid(); s.familyVolume = token;
                if (!read()) return invalid(); s.familyFileId = token;
                if (!read()) return invalid(); s.caseName = token;
                if (!read()) return invalid(); s.caseVolume = token;
                if (!read()) return invalid(); s.caseFileId = token;
                result.state.familySettlement = std::move(s);
            }
            if (!read() || (token != "0" && token != "1")) return invalid();
            if (token == "1")
            {
                V4PreviewHeadAnchorAudit a;
                if (!read()) return invalid(); a.rootVolume = token;
                if (!read()) return invalid(); a.rootId = token;
                if (!read()) return invalid(); a.directoryName = token;
                if (!read()) return invalid(); a.directoryVolume = token;
                if (!read()) return invalid(); a.directoryId = token;
                if (!read()) return invalid(); a.headName = token;
                if (!read()) return invalid(); a.headVolume = token;
                if (!read()) return invalid(); a.headId = token;
                if (!read()) return invalid(); a.headBytes = token;
                result.state.previewHeadAnchor = std::move(a);
            }
        }
        if (!legacyV13 && !legacyV14 && !legacyV15 && !legacyV16 && !legacyV17 && !legacyV18 && !legacyV19 && !legacyV20 && !legacyV21 && !legacyV22 && !legacyV23)
        {
            std::size_t count = 0;
            if (!read() || !CheckpointSize(token, count) || count > remaining.size() / 34) return invalid();
            result.state.exactAttempts.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                V4ExactAttemptAudit attempt;
                if (!read()) return invalid(); attempt.caseId = token;
                if (!read()) return invalid(); attempt.executionId = token;
                if (!read()) return invalid(); attempt.sourcePathUtf16Hex = token;
                if (!read()) return invalid(); attempt.preBytes = token;
                if (!read()) return invalid(); attempt.flightBytes = token;
                if (!read()) return invalid(); attempt.previousName = token;
                if (!read()) return invalid(); attempt.previousVolume = token;
                if (!read()) return invalid(); attempt.previousFileId = token;
                for (std::size_t j = 0; j < attempt.fileIds.size(); ++j)
                { if (!read()) return invalid(); attempt.volumes[j] = token;
                  if (!read()) return invalid(); attempt.fileIds[j] = token; }
                result.state.exactAttempts.push_back(std::move(attempt));
            }
        }
        if (!legacyV13 && !legacyV14 && !legacyV15 && !legacyV16 && !legacyV17 && !legacyV18 && !legacyV19 && !legacyV20 && !legacyV21 && !legacyV22 && !legacyV23 && !legacyV24)
        {
            if (!read() || (token != "0" && token != "1")) return invalid();
            if (token == "1")
            {
                V4NativeProjectionPageAudit p;
                if (!read()) return invalid(); p.familyCoordinate = token;
                if (!read()) return invalid(); p.familyCursor = token;
                if (!read()) return invalid(); p.candidateId = token;
                if (!read()) return invalid(); p.candidateOrdinal = token;
                if (!read()) return invalid(); p.leafOrdinal = token;
                if (!read()) return invalid(); p.leafPresence = token;
                if (!read()) return invalid(); p.pageOrdinal = token;
                if (!read()) return invalid(); p.advanceBytes = token;
                if (!read()) return invalid(); p.pageBytes = token;
                if (!read()) return invalid(); p.baseHeadName = token;
                if (!read()) return invalid(); p.baseHeadVolume = token;
                if (!read()) return invalid(); p.baseHeadFileId = token;
                std::array<std::size_t*, 20> numbers{&p.coordinateBudget.decimalDigits, &p.coordinateBudget.tokenBytes,
                    &p.coordinateBudget.listEntries, &p.coordinateBudget.arithmeticSteps, &p.axisBudget.logicalMembers,
                    &p.axisBudget.fileMembers, &p.axisBudget.virtualMembers, &p.axisBudget.referenceMembers, &p.axisBudget.payloadSlots,
                    &p.axisBudget.textSlots, &p.axisBudget.requestResultSlots, &p.axisBudget.fileContentSlots, &p.axisBudget.payloadBytes,
                    &p.axisBudget.directoryNodeSlots, &p.axisBudget.directoryRootSlots, &p.axisBudget.preparationSteps,
                    &p.advanceWorkBudget, &p.preambleBudget, &p.workBudget, &p.outputLimit};
                for (auto value : numbers) if (!read() || !CheckpointSize(token, *value)) return invalid();
                result.state.nativeProjectionPage = std::move(p);
            }
            if (!read() || (token != "0" && token != "1")) return invalid();
            if (token == "1")
            {
                V4NativeStagedFamilyAudit f;
                if (!read()) return invalid(); f.journalHeadName = token;
                if (!read()) return invalid(); f.journalHeadVolume = token;
                if (!read()) return invalid(); f.journalHeadFileId = token;
                if (!read()) return invalid(); f.familyCoordinate = token;
                if (!read()) return invalid(); f.beforeCursor = token;
                if (!read()) return invalid(); f.afterCursor = token;
                if (!read()) return invalid(); f.stage = token;
                if (!read()) return invalid(); f.proposalBytes = token;
                if (!read() || !CheckpointSize(token, f.workBudget)) return invalid();
                result.state.nativeStagedFamily = std::move(f);
            }
        }
        if (!legacyV13 && !legacyV14 && !legacyV15 && !legacyV16 && !legacyV17 && !legacyV18 && !legacyV19 &&
            !legacyV20 && !legacyV21 && !legacyV22 && !legacyV23 && !legacyV24 && !legacyV25)
        {
            if (!read() || (token != "0" && token != "1")) return invalid();
            if (token == "1")
            {
                V4NativeCaseIndexAudit a;
                if (!read()) return invalid(); a.parentName = token;
                if (!read()) return invalid(); a.parentVolume = token;
                if (!read()) return invalid(); a.parentFileId = token;
                if (!read()) return invalid(); a.pageName = token;
                if (!read()) return invalid(); a.pageVolume = token;
                if (!read()) return invalid(); a.pageFileId = token;
                if (!read()) return invalid(); a.caseId = token;
                if (!read() || !CheckpointSize(token, a.priorSteps)) return invalid();
                result.state.nativeCaseIndex = std::move(a);
            }
        }
        if (!remaining.empty()) return invalid();
        const auto canonical = EncodeCheckpoint(result.state, expectedBinding, byteBudget, legacyV13, legacyV14, legacyV15, legacyV16, legacyV17, legacyV18, legacyV19, legacyV20, legacyV21, legacyV22, legacyV23, legacyV24, legacyV25);
        if (canonical.status == CodecStatus::resource_unavailable)
        { result.state = {}; result.status = canonical.status; result.reason = canonical.reason; return result; }
        if (canonical.status != CodecStatus::encoded || canonical.bytes != bytes) return invalid();
        result.status = CodecStatus::decoded;
        return result;
    }
    catch (const std::bad_alloc&) { result.state = {}; result.status = CodecStatus::resource_unavailable; result.reason = "v3_checkpoint_allocation"; return result; }
    catch (const std::length_error&) { result.state = {}; result.status = CodecStatus::resource_unavailable; result.reason = "v3_checkpoint_allocation"; return result; }
}

struct V4TypedMigrationProposal
{
    std::optional<State> proposed;
    std::string_view reason;
};
// Explicitly retain all V13 families, cases, attempts and field-layer audit;
// the new typed grammar starts from zero, without claiming historical DFS.
// This is only a proposal: the caller must authenticate the old checkpoint
// head and atomically publish the complete new state before adopting it.
inline V4TypedMigrationProposal PrepareV4TypedMigration(const State& old, std::string_view binding, std::size_t byteBudget)
{
    V4TypedMigrationProposal result;
    try
    {
        if (old.familyVersion != FamilyVersion::v4 || old.v4TypedEnabled ||
            EncodeCheckpoint(old, binding, byteBudget, true).status != CodecStatus::encoded)
        { result.reason = "v4_typed_migration_old_state_unavailable"; return result; }
        State next = old; next.v4TypedEnabled = true;
        if (EncodeCheckpoint(next, binding, byteBudget).status != CodecStatus::encoded)
        { result.reason = "v4_typed_migration_checkpoint_unavailable"; return result; }
        result.proposed = std::move(next); result.reason = "v4_typed_migration_uncommitted"; return result;
    }
    catch (...) { result.reason = "v4_typed_migration_resource_unavailable"; return result; }
}

// Windows-only, append-only checkpoint publication. The caller supplies an
// already verified, exclusively owned isolated root handle and its trusted
// physical identity; this class does not infer ownership from a pathname.
// Each store uses a newly created child directory and never overwrites or
// deletes any existing file. The caller must keep the root isolated from
// concurrent untrusted writers, and preserve the locator outside this store.
struct CheckpointLocator
{
    FILE_ID_INFO rootId{}, directoryId{};
    std::wstring directoryName;
};
// Diagnostic evidence about a failed or partially completed create. A name or
// a successful CreateDirectory call alone is not authority to delete or adopt
// that object. Only independently revalidated physical identity and the
// caller's root-ownership proof can support any later operation.
struct CheckpointCreateAttempt
{
    FILE_ID_INFO rootId{}, directoryId{};
    std::wstring directoryName;
    bool directoryCreateAttempted = false, directoryCreated = false, directoryIdentityObserved = false;
};
enum class StoreStatus { ready, unavailable, publication_uncertain, published_durability_unverified };
struct PublishedCheckpoint
{
    StoreStatus status = StoreStatus::unavailable;
    std::wstring fileName, temporaryName; // Names are diagnostic, not ownership proof.
    FILE_ID_INFO fileId{}; // Must be retained out of band as trusted ownership evidence.
    bool temporaryCreateAttempted = false, temporaryCreated = false, fileIdentityCaptured = false;
    bool renameAttempted = false, renameReturnedSuccess = false;
    std::string_view reason;
};

using OutcomeFacts = AttemptHistory;
struct AttemptLineage final
{
    std::atomic_bool sealed{false}, commitStarted{false}, executing{false};
};
class ScheduledV3Outcome;
class ScheduledV4Outcome;
class V4ExactOwnedFilesPermit;
class InFlightReceipt final
{
    friend class CheckpointStore;
    friend class ScheduledV3Outcome;
    friend class ScheduledV4Outcome;
    friend class V4ExactOwnedFilesPermit;
    friend class ::drag_global::ScheduledV3Executor;
    friend class ::drag_global::ScheduledV4Executor;
public:
    InFlightReceipt(const InFlightReceipt&) = delete;
    InFlightReceipt& operator=(const InFlightReceipt&) = delete;
    ~InFlightReceipt() { if (pinned_ != INVALID_HANDLE_VALUE) CloseHandle(pinned_); if (prePinned_ != INVALID_HANDLE_VALUE) CloseHandle(prePinned_);
        if (rootPinned_ != INVALID_HANDLE_VALUE) CloseHandle(rootPinned_); if (directoryPinned_ != INVALID_HANDLE_VALUE) CloseHandle(directoryPinned_); }
    // Borrowed, read-only handles remain valid only for this receipt's life.
    // They are identity evidence, not an execution or recovery authorization.
    HANDLE PreHandle() const noexcept { return prePinned_; }
    HANDLE InFlightHandle() const noexcept { return pinned_; }
    HANDLE RootHandle() const noexcept { return rootPinned_; }
    HANDLE DirectoryHandle() const noexcept { return directoryPinned_; }
    FILE_ID_INFO PreIdentity() const noexcept { return preId_; }
    FILE_ID_INFO InFlightIdentity() const noexcept { return publishedId_; }
    FILE_ID_INFO RootIdentity() const noexcept { return rootId_; }
    FILE_ID_INFO DirectoryIdentity() const noexcept { return directoryId_; }
    std::string_view ExecutionId() const noexcept { return executionId_; }

private:
    bool TryBeginExecution(FamilyVersion expectedVersion = FamilyVersion::v3) noexcept
    {
        if (version_ != expectedVersion || GetCurrentThreadId() != ownerThread_ || !lineage_ ||
            lineage_->sealed.load(std::memory_order_acquire)) return false;
        bool expected = false;
        return lineage_->executing.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }
    void EndExecution() noexcept { if (lineage_) lineage_->executing.store(false, std::memory_order_release); }
    // One attempt may consume this capability once, only on its creating
    // thread, against the identical canonical pre-state and case identity.
    bool Consume(const State& preState, std::size_t familyIndex, std::size_t caseIndex,
        std::string_view executionId, std::size_t byteBudget) noexcept
    {
        try
        {
            if (GetCurrentThreadId() != ownerThread_ || familyIndex != familyIndex_ || caseIndex != caseIndex_ ||
                preState.familyVersion != version_ || executionId != executionId_ || pinned_ == INVALID_HANDLE_VALUE || !lineage_ ||
                !lineage_->executing.load(std::memory_order_acquire) ||
                lineage_->sealed.load(std::memory_order_acquire)) return false;
            const auto encoded = EncodeCheckpoint(preState, binding_, byteBudget);
            if (encoded.status != CodecStatus::encoded || encoded.bytes != preState_) return false;
            FILE_ID_INFO current{};
            if (!GetFileInformationByHandleEx(pinned_, FileIdInfo, &current, sizeof(current)) ||
                current.VolumeSerialNumber != publishedId_.VolumeSerialNumber ||
                std::memcmp(current.FileId.Identifier, publishedId_.FileId.Identifier, sizeof(current.FileId.Identifier)) != 0)
                return false;
            bool expected = false;
            return consumed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
        }
        catch (...) { return false; }
    }
    InFlightReceipt() = default;
    HANDLE pinned_ = INVALID_HANDLE_VALUE, prePinned_ = INVALID_HANDLE_VALUE;
    HANDLE rootPinned_ = INVALID_HANDLE_VALUE, directoryPinned_ = INVALID_HANDLE_VALUE;
    FILE_ID_INFO preId_{}, publishedId_{}, rootId_{}, directoryId_{};
    DWORD ownerThread_ = 0;
    FamilyVersion version_ = FamilyVersion::v3;
    std::size_t familyIndex_ = 0, caseIndex_ = 0;
    std::string binding_, preState_, executionId_;
    std::atomic_bool consumed_{false};
    std::shared_ptr<AttemptLineage> lineage_;
};

// Facts cannot be sealed by a caller: only the concrete production executor is
// a friend. A seal is bound to one receipt and is issued at most once, on its
// owning thread. Read-only facts can contain both failures and missing evidence.
class ScheduledV3Outcome final
{
    friend class ::drag_global::ScheduledV3Executor;
    friend class CheckpointStore;
public:
    ScheduledV3Outcome(const ScheduledV3Outcome&) = delete;
    ScheduledV3Outcome& operator=(const ScheduledV3Outcome&) = delete;
    bool Valid() const noexcept { return valid_; }
    const OutcomeFacts& Facts() const noexcept { return facts_; }
private:
    ScheduledV3Outcome(const InFlightReceipt& receipt, OutcomeFacts facts)
        : lineage_(receipt.lineage_), facts_(std::move(facts))
    {
        bool expected = false;
        valid_ = receipt.version_ == FamilyVersion::v3 && lineage_ && GetCurrentThreadId() == receipt.ownerThread_ &&
            lineage_->executing.load(std::memory_order_acquire) &&
            facts_.receiptConsumed == receipt.consumed_.load(std::memory_order_acquire) &&
            lineage_->sealed.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }
    std::shared_ptr<AttemptLineage> lineage_;
    OutcomeFacts facts_;
    bool valid_ = false;
};

class ScheduledV4Outcome final
{
    friend class ::drag_global::ScheduledV4Executor;
    friend class CheckpointStore;
public:
    ScheduledV4Outcome(const ScheduledV4Outcome&) = delete;
    ScheduledV4Outcome& operator=(const ScheduledV4Outcome&) = delete;
    bool Valid() const noexcept { return valid_; }
    const OutcomeFacts& Facts() const noexcept { return facts_; }
private:
    ScheduledV4Outcome(const InFlightReceipt& receipt, OutcomeFacts facts)
        : lineage_(receipt.lineage_), facts_(std::move(facts))
    {
        bool expected = false;
        valid_ = receipt.version_ == FamilyVersion::v4 && lineage_ && GetCurrentThreadId() == receipt.ownerThread_ &&
            lineage_->executing.load(std::memory_order_acquire) &&
            facts_.receiptConsumed == receipt.consumed_.load(std::memory_order_acquire) &&
            lineage_->sealed.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }
    std::shared_ptr<AttemptLineage> lineage_;
    OutcomeFacts facts_;
    bool valid_ = false;
};

struct AttemptResult
{
    std::unique_ptr<InFlightReceipt> receipt;
    PublishedCheckpoint checkpoint;
    std::string_view reason;
};
// Created only by a successfully read-back commit of the same sealed V4
// outcome and in-flight receipt. The handles are borrowed and read-only;
// this capability does not certify a product outcome or crash durability.
class CommittedV4Receipt final
{
    friend class CheckpointStore;
public:
    CommittedV4Receipt(const CommittedV4Receipt&) = delete;
    CommittedV4Receipt& operator=(const CommittedV4Receipt&) = delete;
    ~CommittedV4Receipt()
    { if (pre_ != INVALID_HANDLE_VALUE) CloseHandle(pre_); if (flight_ != INVALID_HANDLE_VALUE) CloseHandle(flight_);
      if (terminal_ != INVALID_HANDLE_VALUE) CloseHandle(terminal_); if (root_ != INVALID_HANDLE_VALUE) CloseHandle(root_);
      if (directory_ != INVALID_HANDLE_VALUE) CloseHandle(directory_); }
    HANDLE PreHandle() const noexcept { return pre_; }
    HANDLE InFlightHandle() const noexcept { return flight_; }
    HANDLE TerminalHandle() const noexcept { return terminal_; }
    HANDLE RootHandle() const noexcept { return root_; }
    HANDLE DirectoryHandle() const noexcept { return directory_; }
    FILE_ID_INFO PreIdentity() const noexcept { return preId_; }
    FILE_ID_INFO InFlightIdentity() const noexcept { return flightId_; }
    FILE_ID_INFO TerminalIdentity() const noexcept { return terminalId_; }
    FILE_ID_INFO RootIdentity() const noexcept { return rootId_; }
    FILE_ID_INFO DirectoryIdentity() const noexcept { return directoryId_; }
    std::string_view ExecutionId() const noexcept { return executionId_; }
private:
    CommittedV4Receipt() = default;
    HANDLE pre_ = INVALID_HANDLE_VALUE, flight_ = INVALID_HANDLE_VALUE, terminal_ = INVALID_HANDLE_VALUE;
    HANDLE root_ = INVALID_HANDLE_VALUE, directory_ = INVALID_HANDLE_VALUE;
    FILE_ID_INFO preId_{}, flightId_{}, terminalId_{}, rootId_{}, directoryId_{};
    std::string executionId_;
};
struct OutcomeCommitResult
{
    PublishedCheckpoint checkpoint;
    std::optional<State> resultingState;
    std::unique_ptr<CommittedV4Receipt> committedV4;
    std::string_view reason;
};

// Only the parent that verified the owned-case handshake can mint this local
// proof; it retains the actual directory handles, not merely their names.
class OwnedAssociationProof final : public std::enable_shared_from_this<OwnedAssociationProof>
{
    friend class ::drag_global::virtual_child_protocol::Parent;
    friend class CheckpointStore;
    friend class OwnedTerminalProof;
    std::string templateCase_, ownedCase_;
    FILE_ID_INFO profileId_{}, sourceId_{};
    HANDLE profile_ = INVALID_HANDLE_VALUE, source_ = INVALID_HANDLE_VALUE;
    DWORD thread_ = 0;
    OwnedAssociationProof(std::string templateCase, std::string ownedCase, HANDLE profile,
        const FILE_ID_INFO& profileId, HANDLE source, const FILE_ID_INFO& sourceId)
        : templateCase_(std::move(templateCase)), ownedCase_(std::move(ownedCase)), profileId_(profileId),
          sourceId_(sourceId), thread_(GetCurrentThreadId())
    {
        const HANDLE process = GetCurrentProcess();
        if (!DuplicateHandle(process, profile, process, &profile_, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
            !DuplicateHandle(process, source, process, &source_, 0, FALSE, DUPLICATE_SAME_ACCESS))
        { if (profile_ && profile_ != INVALID_HANDLE_VALUE) CloseHandle(profile_); profile_ = INVALID_HANDLE_VALUE;
          if (source_ && source_ != INVALID_HANDLE_VALUE) CloseHandle(source_); source_ = INVALID_HANDLE_VALUE; }
    }
    bool Live() const noexcept
    {
        const auto match = [](HANDLE h, const FILE_ID_INFO& expected) {
            FILE_ID_INFO id{}; FILE_ATTRIBUTE_TAG_INFO tag{};
            return h && h != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof(id)) &&
                id.VolumeSerialNumber == expected.VolumeSerialNumber &&
                std::memcmp(id.FileId.Identifier, expected.FileId.Identifier, sizeof(id.FileId.Identifier)) == 0 &&
                std::any_of(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), [](BYTE x) { return x != 0; }) &&
                GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof(tag)) &&
                (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
        };
        return GetCurrentThreadId() == thread_ && !templateCase_.empty() && !ownedCase_.empty() &&
            templateCase_ != ownedCase_ && match(profile_, profileId_) && match(source_, sourceId_);
    }
public:
    OwnedAssociationProof(const OwnedAssociationProof&) = delete;
    OwnedAssociationProof& operator=(const OwnedAssociationProof&) = delete;
    ~OwnedAssociationProof() { if (profile_ != INVALID_HANDLE_VALUE) CloseHandle(profile_); if (source_ != INVALID_HANDLE_VALUE) CloseHandle(source_); }
};
class CommittedOwnedAssociation final
{
    friend class CheckpointStore;
    std::shared_ptr<const OwnedAssociationProof> proof_;
    std::wstring headName_;
    FILE_ID_INFO headId_{}, rootId_{}, directoryId_{};
    HANDLE head_ = INVALID_HANDLE_VALUE;
    DWORD thread_ = 0;
    CommittedOwnedAssociation() = default;
public:
    CommittedOwnedAssociation(const CommittedOwnedAssociation&) = delete;
    CommittedOwnedAssociation& operator=(const CommittedOwnedAssociation&) = delete;
    ~CommittedOwnedAssociation() { if (head_ != INVALID_HANDLE_VALUE) CloseHandle(head_); }
    std::wstring_view HeadName() const noexcept { return headName_; }
    const FILE_ID_INFO& HeadIdentity() const noexcept { return headId_; }
    // The association is stored inside this same checkpoint object.
    HANDLE AssociationHandle() const noexcept { return head_; }
};
struct OwnedAssociationCommitResult
{
    PublishedCheckpoint checkpoint;
    std::optional<State> resultingState;
    std::unique_ptr<CommittedOwnedAssociation> committed;
    bool resourceUnavailable = false;
    std::string_view reason;
};

class OwnedTerminalProof final : public std::enable_shared_from_this<OwnedTerminalProof>
{
    friend class ::drag_global::virtual_child_protocol::Parent;
    friend class CheckpointStore;
    std::shared_ptr<const OwnedAssociationProof> association_;
    std::string executionId_, preBytes_, flightBytes_, terminalBytes_;
    std::array<HANDLE, 5> handles_{INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
    std::array<FILE_ID_INFO, 5> ids_{};
    DWORD thread_ = 0;
    OwnedTerminalProof(std::shared_ptr<const OwnedAssociationProof> association, std::string executionId,
        std::string preBytes, std::string flightBytes, std::string terminalBytes,
        const std::array<HANDLE, 5>& handles, const std::array<FILE_ID_INFO, 5>& ids)
        : association_(std::move(association)), executionId_(std::move(executionId)), preBytes_(std::move(preBytes)),
          flightBytes_(std::move(flightBytes)), terminalBytes_(std::move(terminalBytes)), ids_(ids), thread_(GetCurrentThreadId())
    {
        const HANDLE process = GetCurrentProcess();
        for (std::size_t i = 0; i < handles.size(); ++i)
            if (!DuplicateHandle(process, handles[i], process, &handles_[i], 0, FALSE, DUPLICATE_SAME_ACCESS))
            { for (auto& h : handles_) { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); h = INVALID_HANDLE_VALUE; } break; }
    }
    bool Live() const noexcept
    {
        if (GetCurrentThreadId() != thread_ || !association_ || !association_->Live() || executionId_.empty() ||
            preBytes_.empty() || flightBytes_.empty() || terminalBytes_.empty()) return false;
        for (std::size_t i = 0; i < handles_.size(); ++i)
        {
            FILE_ID_INFO id{}; FILE_ATTRIBUTE_TAG_INFO tag{};
            if (!handles_[i] || handles_[i] == INVALID_HANDLE_VALUE ||
                !GetFileInformationByHandleEx(handles_[i], FileIdInfo, &id, sizeof(id)) ||
                id.VolumeSerialNumber != ids_[i].VolumeSerialNumber ||
                std::memcmp(id.FileId.Identifier, ids_[i].FileId.Identifier, sizeof(id.FileId.Identifier)) != 0 ||
                std::none_of(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), [](BYTE x) { return x != 0; }) ||
                !GetFileInformationByHandleEx(handles_[i], FileAttributeTagInfo, &tag, sizeof(tag)) ||
                (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != (i < 2)) return false;
        }
        return true;
    }
    bool FilesMatch(std::size_t byteBudget) const
    {
        if (!Live()) return false;
        const std::array<const std::string*, 3> expected{&preBytes_, &flightBytes_, &terminalBytes_};
        for (std::size_t i = 0; i < expected.size(); ++i)
        {
            if (expected[i]->size() > byteBudget) return false;
            HANDLE reopened = ReOpenFile(handles_[i + 2], GENERIC_READ, FILE_SHARE_READ, FILE_FLAG_OPEN_REPARSE_POINT);
            if (reopened == INVALID_HANDLE_VALUE) return false;
            struct Guard { HANDLE h; ~Guard() { CloseHandle(h); } } guard{reopened};
            FILE_ID_INFO id{}; FILE_BASIC_INFO before{}, after{}; LARGE_INTEGER size{}, endSize{};
            if (!GetFileInformationByHandleEx(reopened, FileIdInfo, &id, sizeof(id)) ||
                id.VolumeSerialNumber != ids_[i + 2].VolumeSerialNumber ||
                std::memcmp(id.FileId.Identifier, ids_[i + 2].FileId.Identifier, sizeof(id.FileId.Identifier)) != 0 ||
                !GetFileInformationByHandleEx(reopened, FileBasicInfo, &before, sizeof(before)) ||
                !GetFileSizeEx(reopened, &size) || size.QuadPart < 0 ||
                static_cast<std::uint64_t>(size.QuadPart) != expected[i]->size()) return false;
            std::size_t offset = 0; char buffer[8192];
            while (offset < expected[i]->size())
            {
                DWORD got = 0; const auto amount = static_cast<DWORD>((std::min)(sizeof(buffer), expected[i]->size() - offset));
                if (!ReadFile(reopened, buffer, amount, &got, nullptr) || !got ||
                    std::memcmp(buffer, expected[i]->data() + offset, got)) return false;
                offset += got;
            }
            if (!GetFileSizeEx(reopened, &endSize) || endSize.QuadPart != size.QuadPart ||
                !GetFileInformationByHandleEx(reopened, FileBasicInfo, &after, sizeof(after)) ||
                before.LastWriteTime.QuadPart != after.LastWriteTime.QuadPart ||
                before.ChangeTime.QuadPart != after.ChangeTime.QuadPart) return false;
        }
        return Live();
    }
public:
    OwnedTerminalProof(const OwnedTerminalProof&) = delete;
    OwnedTerminalProof& operator=(const OwnedTerminalProof&) = delete;
    ~OwnedTerminalProof() { for (auto h : handles_) if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};
class CommittedOwnedTerminal final
{
    friend class CheckpointStore;
    std::shared_ptr<const OwnedTerminalProof> proof_;
    std::wstring headName_;
    FILE_ID_INFO headId_{}, rootId_{}, directoryId_{};
    HANDLE head_ = INVALID_HANDLE_VALUE;
    DWORD thread_ = 0;
    CommittedOwnedTerminal() = default;
public:
    CommittedOwnedTerminal(const CommittedOwnedTerminal&) = delete;
    CommittedOwnedTerminal& operator=(const CommittedOwnedTerminal&) = delete;
    ~CommittedOwnedTerminal() { if (head_ != INVALID_HANDLE_VALUE) CloseHandle(head_); }
    std::wstring_view HeadName() const noexcept { return headName_; }
    const FILE_ID_INFO& HeadIdentity() const noexcept { return headId_; }
};
struct OwnedTerminalCommitResult
{
    PublishedCheckpoint checkpoint;
    std::optional<State> resultingState;
    std::unique_ptr<CommittedOwnedTerminal> committed;
    bool resourceUnavailable = false;
    std::string_view reason;
};
struct OwnedTerminalCapacity
{
    bool sufficient = false, resourceUnavailable = false;
    std::size_t maximumEncodedBytes = 0;
    std::string_view reason;
};
struct V4OwnedTerminalShardResult
{
    bool published = false, identityReplayed = false, resourceUnavailable = false;
    PublishedCheckpoint publication;
    std::optional<OwnedCaseAssociation> association;
    std::optional<OwnedCaseTerminal> terminal;
    std::string_view reason;
};

class ExactAttemptProof final : public std::enable_shared_from_this<ExactAttemptProof>
{
    friend class ::drag_global::virtual_child_protocol::Parent;
    friend class CheckpointStore;
    std::string caseId_, executionId_, preBytes_, flightBytes_;
    std::filesystem::path sourcePath_;
    std::array<HANDLE, 6> handles_{INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE,
        INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
    std::array<FILE_ID_INFO, 6> ids_{};
    DWORD thread_ = 0;
    ExactAttemptProof(std::string caseId, std::string executionId, std::filesystem::path sourcePath,
        std::string preBytes, std::string flightBytes, const std::array<HANDLE, 6>& handles,
        const std::array<FILE_ID_INFO, 6>& ids)
        : caseId_(std::move(caseId)), executionId_(std::move(executionId)), preBytes_(std::move(preBytes)),
          flightBytes_(std::move(flightBytes)), sourcePath_(std::move(sourcePath)), ids_(ids), thread_(GetCurrentThreadId())
    {
        for (std::size_t i = 0; i < handles.size(); ++i)
            if (!DuplicateHandle(GetCurrentProcess(), handles[i], GetCurrentProcess(), &handles_[i], 0, FALSE, DUPLICATE_SAME_ACCESS))
            { for (auto& h : handles_) { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); h = INVALID_HANDLE_VALUE; } break; }
    }
    bool Live() const noexcept
    {
        if (GetCurrentThreadId() != thread_ || caseId_.empty() || executionId_.empty() || sourcePath_.empty() ||
            preBytes_.empty() || flightBytes_.empty()) return false;
        for (std::size_t i = 0; i < handles_.size(); ++i)
        {
            FILE_ID_INFO actual{}; FILE_ATTRIBUTE_TAG_INFO tag{};
            if (!handles_[i] || handles_[i] == INVALID_HANDLE_VALUE || GetFileType(handles_[i]) != FILE_TYPE_DISK ||
                !GetFileInformationByHandleEx(handles_[i], FileIdInfo, &actual, sizeof(actual)) ||
                actual.VolumeSerialNumber != ids_[i].VolumeSerialNumber ||
                std::memcmp(actual.FileId.Identifier, ids_[i].FileId.Identifier, sizeof(actual.FileId.Identifier)) != 0 ||
                std::none_of(std::begin(actual.FileId.Identifier), std::end(actual.FileId.Identifier), [](BYTE x) { return x != 0; }) ||
                !GetFileInformationByHandleEx(handles_[i], FileAttributeTagInfo, &tag, sizeof(tag)) ||
                (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != (i < 4)) return false;
        }
        return true;
    }
    bool FilesMatch(std::size_t budget) const
    {
        if (!Live()) return false;
        const std::array<const std::string*, 2> values{&preBytes_, &flightBytes_};
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            if (values[i]->size() > budget) return false;
            const HANDLE h = ReOpenFile(handles_[i + 4], GENERIC_READ, FILE_SHARE_READ, FILE_FLAG_OPEN_REPARSE_POINT);
            if (h == INVALID_HANDLE_VALUE) return false;
            struct Guard { HANDLE h; ~Guard() { CloseHandle(h); } } guard{h};
            FILE_ID_INFO id{}; FILE_BASIC_INFO before{}, after{}; LARGE_INTEGER size{}, end{};
            if (!GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof(id)) || id.VolumeSerialNumber != ids_[i + 4].VolumeSerialNumber ||
                std::memcmp(id.FileId.Identifier, ids_[i + 4].FileId.Identifier, sizeof(id.FileId.Identifier)) ||
                !GetFileInformationByHandleEx(h, FileBasicInfo, &before, sizeof(before)) || !GetFileSizeEx(h, &size) ||
                size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) != values[i]->size()) return false;
            std::size_t offset = 0; char buffer[8192];
            while (offset < values[i]->size())
            {
                DWORD got = 0; const auto n = static_cast<DWORD>((std::min)(sizeof(buffer), values[i]->size() - offset));
                if (!ReadFile(h, buffer, n, &got, nullptr) || !got || std::memcmp(buffer, values[i]->data() + offset, got)) return false;
                offset += got;
            }
            if (!GetFileSizeEx(h, &end) || size.QuadPart != end.QuadPart ||
                !GetFileInformationByHandleEx(h, FileBasicInfo, &after, sizeof(after)) ||
                before.LastWriteTime.QuadPart != after.LastWriteTime.QuadPart || before.ChangeTime.QuadPart != after.ChangeTime.QuadPart) return false;
        }
        return Live();
    }
public:
    ExactAttemptProof(const ExactAttemptProof&) = delete;
    ExactAttemptProof& operator=(const ExactAttemptProof&) = delete;
    ~ExactAttemptProof() { for (auto handle : handles_) if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
class CommittedExactAttempt final
{
    friend class CheckpointStore;
    std::shared_ptr<const ExactAttemptProof> proof_;
    std::wstring headName_;
    std::string headBytes_;
    std::wstring preName_;
    std::string preBytes_;
    FILE_ID_INFO preId_{};
    FILE_ID_INFO headId_{}, rootId_{}, directoryId_{};
    HANDLE head_ = INVALID_HANDLE_VALUE;
    HANDLE pre_ = INVALID_HANDLE_VALUE;
    DWORD thread_ = 0;
    CommittedExactAttempt() = default;
public:
    CommittedExactAttempt(const CommittedExactAttempt&) = delete;
    CommittedExactAttempt& operator=(const CommittedExactAttempt&) = delete;
    ~CommittedExactAttempt() { if (head_ != INVALID_HANDLE_VALUE) CloseHandle(head_); if (pre_ != INVALID_HANDLE_VALUE) CloseHandle(pre_); }
    std::wstring_view HeadName() const noexcept { return headName_; }
    const FILE_ID_INFO& HeadIdentity() const noexcept { return headId_; }
    HANDLE HeadHandle() const noexcept { return head_; }
    std::wstring_view PreHeadName() const noexcept { return preName_; }
    const FILE_ID_INFO& PreHeadIdentity() const noexcept { return preId_; }
    HANDLE PreHeadHandle() const noexcept { return pre_; }
};
struct ExactAttemptCommitResult
{
    PublishedCheckpoint checkpoint;
    std::optional<State> resultingState;
    std::unique_ptr<CommittedExactAttempt> committed;
    bool resourceUnavailable = false;
    std::string_view reason;
};

class V4CheckpointReadPin final
{
    friend class CheckpointStore;
    explicit V4CheckpointReadPin(HANDLE handle) noexcept : handle_(handle) {}
    HANDLE handle_ = INVALID_HANDLE_VALUE;
public:
    V4CheckpointReadPin(const V4CheckpointReadPin&) = delete;
    V4CheckpointReadPin& operator=(const V4CheckpointReadPin&) = delete;
    ~V4CheckpointReadPin() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
};

struct V4ShardLocator;
class V4ForegroundLease;
class V4ForegroundReservation;
class V4ShardFamilyRecheck;
class V4NativeProjectionJournalRecovery;
class V4NativeStagedFamilyReceipt;
class V4NativeReceiptAncestryProof;
struct V4StructuredCaseReplay;
enum class V4NativeStagedOperation { graph, continue_presence, next_candidate };
struct V4NativeStagedFamilyPublication;
struct V4NativeCaseIndexPublication;
class CheckpointStore final
{
    friend class V4ForegroundLease;
    friend class V4ForegroundReservation;
public:
    CheckpointStore(const CheckpointStore&) = delete;
    CheckpointStore& operator=(const CheckpointStore&) = delete;
    ~CheckpointStore() { if (directory_ != INVALID_HANDLE_VALUE) CloseHandle(directory_); if (root_ != INVALID_HANDLE_VALUE) CloseHandle(root_); }
    // Borrowed handle: a receiver must independently pin/verify its identity.
    HANDLE DirectoryHandle() const noexcept { return StillOwned() ? directory_ : INVALID_HANDLE_VALUE; }
    FILE_ID_INFO DirectoryIdentity() const noexcept { return locator_.directoryId; }
    FILE_ID_INFO RootIdentity() const noexcept { return StillOwned() ? locator_.rootId : FILE_ID_INFO{}; }
    bool MatchesLocator(const CheckpointLocator& locator) const noexcept
    { return StillOwned() && locator.directoryName == locator_.directoryName &&
        SameIdentity(locator.rootId, locator_.rootId) && SameIdentity(locator.directoryId, locator_.directoryId); }
    // The read-only pin denies subsequent write/delete opens while held. It is
    // not proof of the file's earlier content, mapped writes, or crash history.
    std::unique_ptr<V4CheckpointReadPin> PinRead(std::wstring_view name, const FILE_ID_INFO& id) noexcept
    {
        try
        {
            if (!ValidName(name, L"checkpoint-") || !UsableIdentity(id) || !StillOwned()) return {};
            const auto path = directoryPath_ + L"\\" + std::wstring(name);
            HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (file == INVALID_HANDLE_VALUE) return {};
            FILE_ID_INFO actual{}; BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandleEx(file, FileIdInfo, &actual, sizeof(actual)) || !SameIdentity(actual, id) ||
                !GetFileInformationByHandle(file, &info) || (!info.nFileIndexHigh && !info.nFileIndexLow) ||
                (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || !StillOwned())
            { CloseHandle(file); return {}; }
            try { return std::unique_ptr<V4CheckpointReadPin>(new V4CheckpointReadPin(file)); }
            catch (...) { CloseHandle(file); return {}; }
        }
        catch (...) { return {}; }
    }
    OwnedAssociationCommitResult PublishOwnedAssociation(std::wstring_view preHeadName, const FILE_ID_INFO& preHeadId,
        std::string_view binding, const std::shared_ptr<const OwnedAssociationProof>& proof,
        const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
        std::size_t tokenBudget, std::size_t caseByteBudget, std::size_t replayWorkBudget,
        std::size_t checkpointByteBudget) noexcept;
    ExactAttemptCommitResult PublishExactAttempt(std::wstring_view preHeadName, const FILE_ID_INFO& preHeadId,
        std::string_view binding, const std::shared_ptr<const ExactAttemptProof>& proof,
        const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
        std::size_t tokenBudget, std::size_t caseByteBudget, std::size_t replayWorkBudget,
        std::size_t childByteBudget, std::size_t checkpointByteBudget) noexcept;
    bool VerifyExactAttempt(const CommittedExactAttempt&, const std::shared_ptr<const ExactAttemptProof>&,
        std::wstring_view trustedHeadName, const FILE_ID_INFO& trustedHeadId, std::string_view binding,
        const V4CoordinateBudget&, const V4AxisBudget&, std::size_t tokenBudget, std::size_t caseByteBudget,
        std::size_t replayWorkBudget, std::size_t childByteBudget, std::size_t checkpointByteBudget) noexcept;
    bool VerifyOwnedAssociation(const CommittedOwnedAssociation&, const std::shared_ptr<const OwnedAssociationProof>&,
        std::wstring_view trustedHeadName, const FILE_ID_INFO& trustedHeadId, std::string_view binding,
        std::size_t checkpointByteBudget) noexcept;
    OwnedTerminalCapacity InspectOwnedTerminalCapacity(const CommittedOwnedAssociation&,
        const std::shared_ptr<const OwnedAssociationProof>&, std::wstring_view trustedHeadName,
        const FILE_ID_INFO& trustedHeadId, std::string_view binding, std::string_view executionId,
        std::size_t childCheckpointByteBudget, std::size_t parentCheckpointByteBudget) noexcept;
    OwnedTerminalCommitResult PublishOwnedTerminal(std::wstring_view preHeadName, const FILE_ID_INFO& preHeadId,
        std::string_view binding, const CommittedOwnedAssociation&, const std::shared_ptr<const OwnedTerminalProof>&,
        const V4CoordinateBudget&, const V4AxisBudget&, std::size_t tokenBudget, std::size_t caseByteBudget,
        std::size_t childCheckpointByteBudget, std::size_t replayWorkBudget, std::size_t parentCheckpointByteBudget) noexcept;
    bool VerifyOwnedTerminal(const CommittedOwnedTerminal&, const std::shared_ptr<const OwnedTerminalProof>&,
        std::wstring_view trustedHeadName, const FILE_ID_INFO& trustedHeadId, std::string_view binding,
        std::size_t parentCheckpointByteBudget, std::size_t childCheckpointByteBudget) noexcept;
    V4OwnedTerminalShardResult PublishOwnedTerminalShard(const CommittedOwnedTerminal&,
        const std::shared_ptr<const OwnedTerminalProof>&, std::wstring_view trustedHeadName,
        const FILE_ID_INFO& trustedHeadId, std::string_view binding, std::size_t parentCheckpointByteBudget,
        std::size_t childCheckpointByteBudget) noexcept;
    V4NativeStagedFamilyPublication PublishV4NativeStagedAttachPreview(
        const std::shared_ptr<V4NativeProjectionJournalRecovery>&, const std::shared_ptr<V4ForegroundReservation>&, const V4ShardLocator& currentHead,
        const V4ShardFamilyRecheck& precheck, std::size_t byteBudget, std::size_t indexByteBudget,
        std::size_t pinBudget, std::size_t retainedByteBudget) noexcept;
    V4NativeStagedFamilyPublication PublishV4NativeStagedGraphPreview(
        const std::shared_ptr<V4NativeStagedFamilyReceipt>&, const std::shared_ptr<V4ForegroundReservation>&, const V4ShardLocator& currentHead,
        const V4ShardFamilyRecheck& precheck, std::size_t workBudget, std::size_t byteBudget, std::size_t indexByteBudget,
        std::size_t pinBudget, std::size_t retainedByteBudget) noexcept;
    V4NativeStagedFamilyPublication PublishV4NativeStagedContinuePreview(
        const std::shared_ptr<V4NativeStagedFamilyReceipt>&, const std::shared_ptr<V4ForegroundReservation>&, const V4ShardLocator& currentHead,
        const V4ShardFamilyRecheck& precheck, std::size_t workBudget, std::size_t byteBudget, std::size_t indexByteBudget,
        std::size_t pinBudget, std::size_t retainedByteBudget) noexcept;
    V4NativeStagedFamilyPublication PublishV4NativeStagedNextCandidatePreview(
        const std::shared_ptr<V4NativeStagedFamilyReceipt>&, const std::shared_ptr<V4ForegroundReservation>&, const V4ShardLocator& currentHead,
        const V4ShardFamilyRecheck& precheck, std::size_t workBudget, std::size_t byteBudget, std::size_t indexByteBudget,
        std::size_t pinBudget, std::size_t retainedByteBudget) noexcept;
    V4NativeStagedFamilyPublication PublishV4NativeStagedPagePreview(
        const std::shared_ptr<V4NativeStagedFamilyReceipt>&, const std::shared_ptr<V4ForegroundReservation>&, const V4ShardLocator& currentHead,
        const V4ShardFamilyRecheck& precheck, std::size_t workBudget, std::size_t byteBudget, std::size_t indexByteBudget,
        std::size_t pinBudget, std::size_t retainedByteBudget, V4NativeStagedOperation operation) noexcept;
    V4StructuredCaseReplay ReplayV4NativeStagedCasePreview(
        const std::shared_ptr<V4NativeStagedFamilyReceipt>& latest, const V4NativeReceiptAncestryProof&,
        std::size_t priorSteps, std::size_t ancestorBudget, const std::vector<std::uint32_t>& familyPrefix,
        const V4CoordinateBudget&, const V4AxisBudget&, std::size_t tokenBudget, std::size_t caseByteBudget) noexcept;
    V4NativeCaseIndexPublication PublishV4NativeCaseIndexPreview(
        const std::shared_ptr<V4NativeStagedFamilyReceipt>& latest, const V4NativeReceiptAncestryProof&,
        std::size_t priorSteps, std::size_t ancestorBudget, const std::vector<std::uint32_t>& familyPrefix,
        const V4CoordinateBudget&, const V4AxisBudget&, std::size_t tokenBudget, std::size_t caseByteBudget,
        std::size_t shardByteBudget, std::size_t indexByteBudget, std::size_t pinBudget,
        std::size_t retainedByteBudget) noexcept;

    AttemptResult BeginAttempt(std::wstring_view preFileName, const FILE_ID_INFO& trustedPreFileId,
        std::string_view binding, std::size_t familyIndex, std::size_t caseIndex, std::string_view executionId,
        std::size_t decimalDigitBudget, std::size_t memberBudget, std::size_t replayWorkBudget,
        std::size_t byteBudget) noexcept;
    AttemptResult BeginV4Attempt(std::wstring_view preFileName, const FILE_ID_INFO& trustedPreFileId,
        std::string_view binding, std::size_t familyIndex, std::size_t caseIndex, std::string_view executionId,
        const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
        std::size_t replayWorkBudget, std::size_t byteBudget) noexcept;
    OutcomeCommitResult CommitOutcome(InFlightReceipt& receipt, const ScheduledV3Outcome& outcome,
        std::size_t byteBudget) noexcept;
    OutcomeCommitResult CommitOutcome(InFlightReceipt& receipt, const ScheduledV4Outcome& outcome,
        std::size_t byteBudget) noexcept;

    static std::unique_ptr<CheckpointStore> Create(HANDLE trustedRoot, const FILE_ID_INFO& trustedRootId,
        CheckpointLocator& locator, CheckpointCreateAttempt& attempt) noexcept
    {
        locator = {};
        attempt = {};
        try
        {
            auto result = std::unique_ptr<CheckpointStore>(new CheckpointStore);
            if (!result->BindRoot(trustedRoot, trustedRootId)) return {};
            const auto name = NewName(L"v3-checkpoint-");
            if (name.empty()) return {};
            const auto path = result->rootPath_ + L"\\" + name;
            // Prepare all diagnostic strings before the first side effect.
            attempt.rootId = trustedRootId;
            attempt.directoryName = name;
            attempt.directoryCreateAttempted = true;
            if (!CreateDirectoryW(path.c_str(), nullptr)) return {};
            attempt.directoryCreated = true;
            result->directory_ = OpenDirectory(path);
            FILE_ID_INFO id{};
            if (result->directory_ == INVALID_HANDLE_VALUE || !DirectoryIdentity(result->directory_, id)) return {};
            attempt.directoryId = id;
            attempt.directoryIdentityObserved = true;
            result->directoryPath_ = path;
            result->locator_ = {trustedRootId, id, name};
            result->freshCreated_ = true;
            auto readyLocator = result->locator_; // copy may fail before publishing an output locator
            locator = std::move(readyLocator);
            return result;
        }
        catch (...) { return {}; }
    }

    // Locator is trusted out-of-band metadata, not data read from a checkpoint.
    static std::unique_ptr<CheckpointStore> Reopen(HANDLE trustedRoot, const CheckpointLocator& locator) noexcept
    {
        try
        {
            if (!ValidName(locator.directoryName, L"v3-checkpoint-")) return {};
            auto result = std::unique_ptr<CheckpointStore>(new CheckpointStore);
            if (!result->BindRoot(trustedRoot, locator.rootId)) return {};
            result->directoryPath_ = result->rootPath_ + L"\\" + locator.directoryName;
            result->directory_ = OpenDirectory(result->directoryPath_);
            FILE_ID_INFO id{};
            if (result->directory_ == INVALID_HANDLE_VALUE || !DirectoryIdentity(result->directory_, id) ||
                !SameIdentity(id, locator.directoryId)) return {};
            result->locator_ = locator;
            return result;
        }
        catch (...) { return {}; }
    }

    PublishedCheckpoint Publish(const State& state, std::string_view binding, std::size_t byteBudget) noexcept
    {
        PublishedCheckpoint result;
        try
        {
            if (!StillOwned()) { result.reason = "checkpoint_directory_identity_unavailable"; return result; }
            const auto encoded = EncodeCheckpoint(state, binding, byteBudget);
            if (encoded.status != CodecStatus::encoded) { result.reason = encoded.reason; return result; }
            const auto transaction = NewName(L"");
            if (transaction.empty()) { result.reason = "checkpoint_transaction_identity_unavailable"; return result; }
            const auto temporaryName = L"pending-" + transaction;
            const auto temp = directoryPath_ + L"\\" + temporaryName;
            const auto finalName = L"checkpoint-" + transaction;
            const auto finalPath = directoryPath_ + L"\\" + finalName;
            // Preserve both names before creating anything, including when a
            // later allocation, identity query, flush, or rename fails.
            result.temporaryName = temporaryName;
            result.fileName = finalName;
            result.temporaryCreateAttempted = true;
            HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (file == INVALID_HANDLE_VALUE) { result.reason = "checkpoint_temp_create_failed"; return result; }
            result.temporaryCreated = true;
            bool ok = GetFileInformationByHandleEx(file, FileIdInfo, &result.fileId, sizeof(result.fileId)) &&
                UsableIdentity(result.fileId);
            result.fileIdentityCaptured = ok;
            std::size_t offset = 0;
            while (ok && offset < encoded.bytes.size())
            {
                const auto count = static_cast<DWORD>((std::min)(encoded.bytes.size() - offset, static_cast<std::size_t>(0x40000000)));
                DWORD written = 0;
                if (!WriteFile(file, encoded.bytes.data() + offset, count, &written, nullptr) || !written)
                { ok = false; break; }
                offset += written;
            }
            if (ok && !FlushFileBuffers(file)) ok = false;
            if (!CloseHandle(file)) ok = false;
            if (!ok) { result.reason = "checkpoint_write_or_flush_failed_temp_retained"; return result; }
            if (!StillOwned()) { result.reason = "checkpoint_directory_identity_changed_temp_retained"; return result; }
            // No replace flag: an unrelated existing file is never overwritten.
            // A failed rename has an uncertain outcome; inspect by identity and
            // reconcile before retrying, never infer that execution did not occur.
            result.renameAttempted = true;
            if (!MoveFileExW(temp.c_str(), finalPath.c_str(), MOVEFILE_WRITE_THROUGH))
            { result.status = StoreStatus::publication_uncertain; result.reason = "checkpoint_rename_failed_outcome_unknown"; return result; }
            result.renameReturnedSuccess = true;
            HANDLE verify = CreateFileW(finalPath.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            FILE_ID_INFO publishedId{};
            const bool verified = verify != INVALID_HANDLE_VALUE &&
                GetFileInformationByHandleEx(verify, FileIdInfo, &publishedId, sizeof(publishedId)) &&
                SameIdentity(publishedId, result.fileId) && StillOwned();
            if (verify != INVALID_HANDLE_VALUE) CloseHandle(verify);
            if (!verified)
            { result.status = StoreStatus::publication_uncertain; result.reason = "checkpoint_published_identity_unavailable"; return result; }
            result.status = StoreStatus::published_durability_unverified;
            result.reason = "checkpoint_directory_crash_durability_unverified";
            return result;
        }
        catch (...) { result.reason = "checkpoint_resource_or_io_unavailable"; return result; }
    }

    CodecResult Read(std::wstring_view name, const FILE_ID_INFO& trustedFileId,
        std::string_view binding, std::size_t byteBudget) noexcept
    {
        CodecResult result;
        try
        {
            if (!ValidName(name, L"checkpoint-") || !UsableIdentity(trustedFileId) || !StillOwned())
            { result.reason = "checkpoint_identity_unavailable"; return result; }
            const auto path = directoryPath_ + L"\\" + std::wstring(name);
            HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (file == INVALID_HANDLE_VALUE) { result.reason = "checkpoint_open_failed"; return result; }
            struct Guard { HANDLE h; ~Guard() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } } guard{file};
            BY_HANDLE_FILE_INFORMATION before{}, after{}, byName{};
            FILE_ID_INFO observedId{};
            if (!GetFileInformationByHandleEx(file, FileIdInfo, &observedId, sizeof(observedId)) ||
                !SameIdentity(observedId, trustedFileId) || !GetFileInformationByHandle(file, &before) ||
                (!before.nFileIndexHigh && !before.nFileIndexLow) ||
                (before.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            { result.reason = "checkpoint_file_metadata_unavailable"; return result; }
            const auto size = (static_cast<std::uint64_t>(before.nFileSizeHigh) << 32) | before.nFileSizeLow;
            if (size > byteBudget || size > (std::numeric_limits<std::size_t>::max)())
            { result.status = CodecStatus::resource_unavailable; result.reason = "v3_checkpoint_byte_budget"; return result; }
            std::string bytes(static_cast<std::size_t>(size), '\0');
            std::size_t offset = 0;
            while (offset < bytes.size())
            {
                const auto count = static_cast<DWORD>((std::min)(bytes.size() - offset, static_cast<std::size_t>(0x40000000)));
                DWORD read = 0;
                if (!ReadFile(file, bytes.data() + offset, count, &read, nullptr) || !read)
                { result.reason = "checkpoint_short_read_or_io_failure"; return result; }
                offset += read;
            }
            if (!GetFileInformationByHandle(file, &after) || !SameFileSnapshot(before, after))
            { result.reason = "checkpoint_changed_during_read"; return result; }
            HANDLE pathFile = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (pathFile == INVALID_HANDLE_VALUE) { result.reason = "checkpoint_path_reopen_failed"; return result; }
            Guard pathGuard{pathFile};
            FILE_ID_INFO byNameId{};
            if (!GetFileInformationByHandleEx(pathFile, FileIdInfo, &byNameId, sizeof(byNameId)) ||
                !SameIdentity(byNameId, trustedFileId) || !GetFileInformationByHandle(pathFile, &byName) ||
                !SameFileSnapshot(after, byName) || !StillOwned())
            { result.reason = "checkpoint_path_or_directory_changed"; return result; }
            auto decoded = DecodeCheckpoint(bytes, binding, byteBudget);
            if (decoded.status == CodecStatus::decoded)
                for (auto& family : decoded.state.families)
                {
                    if (family.state == WorkState::in_flight)
                    { family.state = WorkState::outcome_unavailable; decoded.recoveredInFlight = true; }
                    for (auto& item : family.cases)
                        if (item.state == WorkState::in_flight)
                        { item.state = WorkState::outcome_unavailable; decoded.recoveredInFlight = true; }
                }
            if (decoded.status == CodecStatus::decoded) decoded.bytes = std::move(bytes);
            return decoded;
        }
        catch (...) { result.status = CodecStatus::resource_unavailable; result.reason = "checkpoint_read_resource_unavailable"; return result; }
    }

private:
    template <typename Validate>
    AttemptResult BeginAttemptCore(std::wstring_view preFileName, const FILE_ID_INFO& trustedPreFileId,
        std::string_view binding, std::size_t familyIndex, std::size_t caseIndex, std::string_view executionId,
        FamilyVersion version, std::size_t byteBudget, Validate&& validate) noexcept;
    OutcomeCommitResult CommitOutcomeCore(InFlightReceipt& receipt, const std::shared_ptr<AttemptLineage>& lineage,
        const OutcomeFacts& facts, bool valid, FamilyVersion version, std::size_t byteBudget) noexcept;
    static inline std::mutex registryMutex_;
    static inline std::unordered_set<std::string> reservedCases_, reservedAttempts_, reservedPreStates_;
    CheckpointStore() = default;
    static bool UsableIdentity(const FILE_ID_INFO& id) noexcept
    { return std::any_of(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), [](BYTE byte) { return byte != 0; }); }
    static bool SameIdentity(const FILE_ID_INFO& a, const FILE_ID_INFO& b) noexcept
    { return UsableIdentity(a) && UsableIdentity(b) && a.VolumeSerialNumber == b.VolumeSerialNumber && std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) == 0; }
    static bool SameFileSnapshot(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) noexcept
    {
        return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh &&
            a.nFileIndexLow == b.nFileIndexLow && a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow &&
            a.dwFileAttributes == b.dwFileAttributes && a.ftLastWriteTime.dwHighDateTime == b.ftLastWriteTime.dwHighDateTime &&
            a.ftLastWriteTime.dwLowDateTime == b.ftLastWriteTime.dwLowDateTime;
    }
    static bool DirectoryIdentity(HANDLE handle, FILE_ID_INFO& id) noexcept
    {
        FILE_ATTRIBUTE_TAG_INFO tag{};
        return handle && handle != INVALID_HANDLE_VALUE &&
            GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) &&
            (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) && UsableIdentity(id);
    }
    static HANDLE OpenDirectory(const std::wstring& path) noexcept
    {
        // Excluding FILE_SHARE_DELETE pins this opened directory's name while
        // the store is alive. It does not lock out writers to its contents.
        return CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    }
    static std::wstring HandlePath(HANDLE handle)
    {
        const auto needed = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!needed) return {};
        std::wstring path(needed, L'\0');
        const auto length = GetFinalPathNameByHandleW(handle, path.data(), needed, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!length || length >= needed) return {};
        path.resize(length);
        while (path.size() > 4 && path.back() == L'\\') path.pop_back();
        return path;
    }
    static std::wstring NewName(std::wstring_view prefix)
    {
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid))) return {};
        constexpr wchar_t hex[] = L"0123456789abcdef";
        const auto* bytes = reinterpret_cast<const unsigned char*>(&guid);
        std::wstring result(prefix);
        for (std::size_t i = 0; i < sizeof(guid); ++i) { result.push_back(hex[bytes[i] >> 4]); result.push_back(hex[bytes[i] & 15]); }
        return result;
    }
    static bool ValidName(std::wstring_view name, std::wstring_view prefix) noexcept
    {
        if (name.size() != prefix.size() + 32 || name.substr(0, prefix.size()) != prefix) return false;
        return std::all_of(name.begin() + prefix.size(), name.end(), [](wchar_t c) {
            return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); });
    }
    bool BindRoot(HANDLE trusted, const FILE_ID_INFO& expected)
    {
        FILE_ID_INFO id{};
        if (!DirectoryIdentity(trusted, id) || !SameIdentity(id, expected)) return false;
        rootPath_ = HandlePath(trusted);
        if (rootPath_.empty()) return false;
        root_ = OpenDirectory(rootPath_);
        return root_ != INVALID_HANDLE_VALUE && DirectoryIdentity(root_, id) && SameIdentity(id, expected);
    }
    bool StillOwned() const noexcept
    {
        FILE_ID_INFO rootId{}, directoryId{};
        return DirectoryIdentity(root_, rootId) && DirectoryIdentity(directory_, directoryId) &&
            SameIdentity(rootId, locator_.rootId) && SameIdentity(directoryId, locator_.directoryId);
    }
    HANDLE root_ = INVALID_HANDLE_VALUE, directory_ = INVALID_HANDLE_VALUE;
    std::wstring rootPath_, directoryPath_;
    CheckpointLocator locator_;
    bool freshCreated_ = false;
    std::atomic_bool freshLeaseClaimed_{false};
};

// Each discovery is followed by two visits. The scan index advances once per
// visit and wraps against the current family count. Its rate therefore exceeds
// growth of the table, so every discovered family is revisited indefinitely;
// resource failures are retained rather than blocking later families. Actual
// candidate and graph fairness still requires a bounded resumable visit adapter.
inline Tick PrepareTick(const State& original, std::string_view expectedBinding, std::size_t decimalDigitBudget,
    const V4CoordinateBudget* v4Budget = nullptr)
{
    Tick result;
    try
    {
        if (expectedBinding.empty() || original.binding != expectedBinding ||
            (original.familyVersion != FamilyVersion::v3 && original.familyVersion != FamilyVersion::v4) ||
            (original.familyVersion == FamilyVersion::v3 && (original.v4SeedEmitted || !original.v4SeedNext || !original.v4SeedCoordinate.empty() ||
                !original.v4TextSeedCoordinates.empty() || !original.v4TextSeedNext)) ||
            (original.familyVersion == FamilyVersion::v4 && (original.v4SeedEmitted != !original.v4SeedCoordinate.empty())) ||
            (original.v4TextSeedCoordinates.size() > V4AdditionalTextSeedCount ||
                (!original.v4SeedEmitted && !original.v4TextSeedCoordinates.empty())) ||
            !V3FamilyCanonicalDecimal(original.nextFamily) ||
            original.visitsRemaining > 2 ||
            (original.families.empty() ? original.nextVisit != 0 : original.nextVisit >= original.families.size()))
        { result.reason = "v3_scheduler_state_or_binding_invalid"; return result; }
        if (original.familyVersion == FamilyVersion::v4 && !original.v4TypedEnabled)
        { result.kind = TickKind::unavailable; result.reason = "v4_typed_explicit_migration_required"; return result; }
        const auto typedShape = [](const V4StructuredDfsCursor& cursor) {
            return cursor.stopDepth <= cursor.prefix.size() && cursor.prefix.size() <= cursor.layer &&
                cursor.nextChoice.size() == cursor.prefix.size() + 1 && !cursor.exhausted;
        };
        if (!typedShape(original.v4TypedCursor) || original.v4TypedCursor.stopDepth != 0 ||
            (original.v4TypedDeferred.empty() ? original.v4TypedDeferredVisit != 0 :
                original.v4TypedDeferredVisit >= original.v4TypedDeferred.size()) ||
            std::any_of(original.v4TypedDeferred.begin(), original.v4TypedDeferred.end(),
                [&](const auto& cursor) { return !typedShape(cursor); }))
        { result.reason = "v4_typed_scheduler_state_invalid"; return result; }

        // V3 retains its contiguous natural-coordinate prefix. V4 field
        // layers can revisit deferred tuples out of order, so only unique
        // family coordinates are checked here. Checkpoint bytes alone do not
        // authenticate previously published cursor or coverage history.
        if (!V4FieldLayerCursorCanonical(original.v4FieldCursor) ||
            (original.v4Deferred.empty() ? original.v4DeferredVisit != 0 : original.v4DeferredVisit >= original.v4Deferred.size()) ||
            (original.familyVersion == FamilyVersion::v4 && original.nextFamily != "0") ||
            (original.familyVersion == FamilyVersion::v3 && (!original.v4Deferred.empty() || !original.v4FieldHoles.empty() ||
                original.v4DeferredVisit || original.v4DeferredNext || original.v4FieldCursor.layer != "0" || original.v4FieldCursor.activeB)))
        { result.reason = "v4_field_scheduler_state_invalid"; return result; }
        const auto fieldKey = [](const V4FieldLayerCursor& cursor) {
            std::string key = cursor.activeB ? "1:" : "0:"; key += cursor.layer;
            for (const auto& field : cursor.fields) { key += ':'; key += field; }
            return key;
        };
        std::unordered_set<std::string> fieldKeys;
        fieldKeys.insert(fieldKey(original.v4FieldCursor));
        for (const auto* entries : {&original.v4Deferred, &original.v4FieldHoles})
            for (const auto& cursor : *entries)
                if (!V4FieldLayerCursorCanonical(cursor) || !fieldKeys.insert(fieldKey(cursor)).second)
                { result.reason = "v4_field_scheduler_cursor_duplicate_or_invalid"; return result; }
        std::string expected = "0";
        std::unordered_set<std::string> familyCoordinates;
        bool sawSeed = false, sawSeedOriginal = false;
        for (const auto& family : original.families)
        {
            if ((original.familyVersion == FamilyVersion::v3 && !family.discoverySeed && family.coordinate != expected) ||
                (original.familyVersion == FamilyVersion::v4 && (family.encodingHole || family.duplicateSeed ||
                    !V4CanonicalDecimal(family.coordinate) || !familyCoordinates.insert(family.coordinate).second)) || family.version != original.familyVersion ||
                (family.discoverySeed && (family.version != FamilyVersion::v4 || sawSeed || !original.v4SeedEmitted || family.encodingHole || family.duplicateSeed)) ||
                (family.duplicateSeed && (family.version != FamilyVersion::v4 || family.encodingHole || family.discoverySeed ||
                    !family.cases.empty() || !family.candidateCursor.empty() || !family.pruneSteps.empty())) ||
                ((family.discoverySeed || family.duplicateSeed) && family.coordinate != original.v4SeedCoordinate) ||
                (family.encodingHole && (family.version != FamilyVersion::v4 || !family.cases.empty() ||
                    !family.candidateCursor.empty() || !family.pruneSteps.empty())))
            { result.reason = "v3_scheduler_family_prefix_invalid"; return result; }
            if (family.discoverySeed) sawSeed = true;
            else if (original.familyVersion == FamilyVersion::v3) expected = Add(expected, "1");
            if (!family.encodingHole && !family.duplicateSeed && family.coordinate == original.v4SeedCoordinate)
                sawSeedOriginal = true;
        }
        if ((original.familyVersion == FamilyVersion::v3 && expected != original.nextFamily) || (original.v4SeedEmitted && !sawSeedOriginal) ||
            (original.visitsRemaining && original.families.empty()))
        { result.reason = "v3_scheduler_family_progress_invalid"; return result; }
        std::unordered_set<std::string> textSeeds;
        for (const auto& coordinate : original.v4TextSeedCoordinates)
            if (!V4CanonicalDecimal(coordinate) || coordinate == original.v4SeedCoordinate ||
                !textSeeds.insert(coordinate).second || !familyCoordinates.contains(coordinate))
            { result.reason = "v4_text_seed_state_invalid"; return result; }

        result.proposed = original;
        if (original.visitsRemaining == 0)
        {
            if (original.familyVersion == FamilyVersion::v4)
            {
                if (!v4Budget) { result.kind = TickKind::unavailable; result.reason = "v4_scheduler_coordinate_budget_required"; return result; }
                if (!original.v4SeedEmitted && original.v4SeedNext)
                {
                    const auto encoded = EncodeV4FamilyCoordinate(V4FirstDiscoverySeed(), *v4Budget);
                    if (encoded.status == V4FamilyCoordinateStatus::resource_unavailable)
                    {
                        result.proposed.v4SeedNext = false;
                        result.kind = TickKind::discovery_deferred;
                        if (!original.families.empty()) result.proposed.visitsRemaining = 2;
                        result.reason = encoded.reason; return result;
                    }
                    if (encoded.status != V4FamilyCoordinateStatus::ready)
                    { result.reason = "v4_scheduler_seed_invalid"; return result; }
                    result.proposed.v4SeedCoordinate = encoded.coordinate;
                    result.proposed.v4SeedEmitted = true;
                    const auto already = std::find_if(original.families.begin(), original.families.end(), [&](const auto& family) {
                        return !family.encodingHole && !family.duplicateSeed && family.coordinate == encoded.coordinate; });
                    if (already == original.families.end())
                    {
                        FamilyState seed; seed.coordinate = encoded.coordinate; seed.version = FamilyVersion::v4; seed.discoverySeed = true;
                        result.proposed.families.push_back(std::move(seed));
                    }
                    result.proposed.visitsRemaining = result.proposed.families.empty() ? 0 : 2;
                    result.kind = already == original.families.end() ? TickKind::discovered : TickKind::duplicate_seed;
                    return result;
                }
                if (original.v4SeedEmitted && original.v4TextSeedNext)
                {
                    // Verify persisted hint ordinals before using their count
                    // to select the next hint. Insufficient resources defer
                    // this hint turn, not unrelated families or typed DFS.
                    for (std::size_t i = 0; i < original.v4TextSeedCoordinates.size(); ++i)
                    {
                        const auto prior = V4AdditionalTextSeed(i);
                        if (!prior) { result.reason = "v4_text_seed_descriptor_invalid"; return result; }
                        const auto checked = EncodeV4FamilyCoordinate(*prior, *v4Budget);
                        if (checked.status == V4FamilyCoordinateStatus::resource_unavailable)
                        {
                            result.proposed.v4TextSeedNext = false;
                            if (!original.families.empty()) result.proposed.visitsRemaining = 2;
                            result.kind = TickKind::discovery_deferred; result.reason = "v4_text_seed_validation_unavailable"; return result;
                        }
                        if (checked.status != V4FamilyCoordinateStatus::ready || checked.coordinate != original.v4TextSeedCoordinates[i])
                        { result.proposed = original; result.reason = "v4_text_seed_identity_mismatch"; return result; }
                    }
                    if (original.v4TextSeedCoordinates.size() == V4AdditionalTextSeedCount)
                    {
                        result.proposed.v4TextSeedNext = false;
                        if (!original.families.empty()) result.proposed.visitsRemaining = 2;
                        result.kind = TickKind::discovery_progress; return result;
                    }
                    const auto family = V4AdditionalTextSeed(original.v4TextSeedCoordinates.size());
                    if (!family) { result.reason = "v4_text_seed_descriptor_invalid"; return result; }
                    const auto encoded = EncodeV4FamilyCoordinate(*family, *v4Budget);
                    result.proposed.v4TextSeedNext = false;
                    if (encoded.status == V4FamilyCoordinateStatus::resource_unavailable)
                    {
                        if (!original.families.empty()) result.proposed.visitsRemaining = 2;
                        result.kind = TickKind::discovery_deferred; result.reason = encoded.reason; return result;
                    }
                    if (encoded.status != V4FamilyCoordinateStatus::ready ||
                        encoded.coordinate == original.v4SeedCoordinate || textSeeds.contains(encoded.coordinate))
                    { result.proposed = original; result.reason = "v4_text_seed_identity_invalid"; return result; }
                    const auto existing = familyCoordinates.contains(encoded.coordinate);
                    result.proposed.v4TextSeedCoordinates.push_back(encoded.coordinate);
                    if (!existing)
                    {
                        FamilyState entry; entry.coordinate = encoded.coordinate; entry.version = FamilyVersion::v4;
                        result.proposed.families.push_back(std::move(entry));
                    }
                    if (!result.proposed.families.empty()) result.proposed.visitsRemaining = 2;
                    result.kind = existing ? TickKind::duplicate_seed : TickKind::discovered;
                    return result;
                }
                const bool retry = original.v4TypedDeferredNext && !original.v4TypedDeferred.empty();
                const auto& cursor = retry ? original.v4TypedDeferred[original.v4TypedDeferredVisit] : original.v4TypedCursor;
                // One node per discovery turn; tokenBytes bounds the retained
                // prefix size and arithmeticSteps bounds coordinate work.
                const auto step = StepV4StructuredDfs(cursor, *v4Budget, v4Budget->tokenBytes, 1);
                if (step.status == V4StructuredDfsStatus::invalid)
                { result.reason = step.reason; return result; }
                if (step.status == V4StructuredDfsStatus::resource_unavailable)
                {
                    result.proposed.v4TypedDeferredNext = !retry && !original.v4TypedDeferred.empty();
                    if (retry) result.proposed.v4TypedDeferredVisit = (original.v4TypedDeferredVisit + 1) % original.v4TypedDeferred.size();
                    if (!original.v4SeedEmitted) result.proposed.v4SeedNext = true;
                    result.proposed.v4TextSeedNext = true;
                    if (!original.families.empty()) result.proposed.visitsRemaining = 2;
                    result.kind = TickKind::discovery_deferred; result.reason = step.reason; return result;
                }
                if (retry)
                {
                    auto& pending = result.proposed.v4TypedDeferred;
                    if (step.next.exhausted) pending.erase(pending.begin() + original.v4TypedDeferredVisit);
                    else pending[original.v4TypedDeferredVisit] = step.next;
                    result.proposed.v4TypedDeferredVisit = pending.empty() ? 0 :
                        (original.v4TypedDeferredVisit + (step.next.exhausted ? 0 : 1)) % pending.size();
                }
                else if (step.next.exhausted)
                {
                    if (cursor.layer == (std::numeric_limits<std::size_t>::max)())
                    { result.proposed = original; result.kind = TickKind::unavailable; result.reason = "v4_typed_layer_machine_limit_unavailable"; return result; }
                    result.proposed.v4TypedCursor = {}; result.proposed.v4TypedCursor.layer = cursor.layer + 1;
                }
                else result.proposed.v4TypedCursor = step.next;
                if (step.deferred) result.proposed.v4TypedDeferred.push_back(*step.deferred);
                result.proposed.v4TypedDeferredNext = !retry && !result.proposed.v4TypedDeferred.empty();
                result.proposed.v4SeedNext = true; result.proposed.v4TextSeedNext = true;
                if (!original.families.empty()) result.proposed.visitsRemaining = 2;
                if (step.status == V4StructuredDfsStatus::deferred)
                { result.kind = TickKind::discovery_deferred; result.reason = step.reason; return result; }
                if (step.status == V4StructuredDfsStatus::grammar_pruned)
                {
                    result.proposed.v4TypedPruned.push_back({cursor.layer, step.prunedPrefix});
                    result.kind = TickKind::encoding_hole; result.reason = step.reason; return result;
                }
                if (step.status == V4StructuredDfsStatus::progress || step.status == V4StructuredDfsStatus::exhausted)
                { result.kind = TickKind::discovery_progress; return result; }
                if (step.status != V4StructuredDfsStatus::emitted || !step.family || step.coordinate.empty())
                { result.proposed = original; result.reason = "v4_typed_family_result_invalid"; return result; }
                const auto existing = std::find_if(original.families.begin(), original.families.end(), [&](const auto& family) {
                    return family.coordinate == step.coordinate; });
                if (existing != original.families.end())
                { result.kind = TickKind::duplicate_seed; return result; }
                FamilyState family; family.coordinate = step.coordinate; family.version = FamilyVersion::v4;
                result.proposed.families.push_back(std::move(family)); result.proposed.visitsRemaining = 2;
                result.kind = TickKind::discovered; return result;
            }
            const auto decoded = NextV3FamilyCoordinate(original.nextFamily, decimalDigitBudget);
            if (decoded.status == V3FamilyCoordinateStatus::resource_unavailable)
            {
                // Keep retrying this exact coordinate, while permitting the
                // already discovered families to make progress between retries.
                result.kind = original.families.empty() ? TickKind::unavailable : TickKind::discovery_deferred;
                if (!original.families.empty()) result.proposed.visitsRemaining = 2;
                result.reason = decoded.reason;
                return result;
            }
            if (decoded.status != V3FamilyCoordinateStatus::ready || !decoded.family ||
                decoded.coordinate != original.nextFamily || decoded.nextCoordinate != Add(original.nextFamily, "1"))
            { result.reason = "v3_scheduler_family_decode_invalid"; return result; }
            result.proposed.families.push_back(FamilyState{decoded.coordinate});
            result.proposed.nextFamily = decoded.nextCoordinate;
            result.proposed.visitsRemaining = 2;
            result.kind = TickKind::discovered;
        }
        else
        {
            result.familyIndex = original.nextVisit;
            result.proposed.nextVisit = original.nextVisit == original.families.size() - 1 ? 0 : original.nextVisit + 1;
            --result.proposed.visitsRemaining;
            result.kind = TickKind::visit;
        }
        return result;
    }
    catch (const std::bad_alloc&)
    { result = {}; result.kind = TickKind::unavailable; result.reason = "v3_scheduler_resource_unavailable"; return result; }
    catch (const std::length_error&)
    { result = {}; result.kind = TickKind::unavailable; result.reason = "v3_scheduler_resource_unavailable"; return result; }
}

struct FamilyVisitProposal
{
    Tick tick;
    std::optional<event_graph::V3FamilyCandidateRecord> unexecutedRecord;
    std::optional<event_graph::V4FamilyCandidateRecord> v4UnexecutedRecord;
    // Indices are retry hints only. Each must be replayed and checked against
    // the family/candidate/graph authority before any production adapter use.
    std::vector<std::size_t> retryIndices;
    bool skippedSeedDuplicate = false;
    std::vector<std::string> errors;
};
struct V4CaseReplay
{
    enum class Status { invalid, unavailable, resource_unavailable, ready };
    Status status = Status::invalid;
    std::optional<event_graph::V4FamilyCandidateRecord> unexecutedRecord;
    std::vector<std::string> errors;
};

// Canonical byte-exact projection-page evidence. It is only an audit payload:
// a transaction must separately bind the family/candidate/leaf and publish
// these bytes with its after cursor. Recovery must regenerate and compare it.
struct V4ProjectionPageEncoding
{
    std::string bytes;
    bool encoded = false, resourceUnavailable = false;
    std::string_view reason;
};
inline V4ProjectionPageEncoding EncodeV4ProjectionPage(const event_graph::PresenceProjectionWorkPage& page,
    std::size_t byteBudget)
{
    V4ProjectionPageEncoding out;
    try
    {
        if (page.resourceUnavailable || !page.errors.empty() || page.done == page.paused ||
            page.before.binding != page.after.binding)
        { out.reason = "v4_projection_page_unavailable"; return out; }
        const auto before = event_graph::EncodePresenceProjectionCursor(page.before);
        const auto after = event_graph::EncodePresenceProjectionCursor(page.after);
        if (before.resourceUnavailable || after.resourceUnavailable)
        { out.resourceUnavailable = true; out.reason = "v4_projection_page_cursor_resource_unavailable"; return out; }
        if (!before.cursor || !after.cursor || !before.errors.empty() || !after.errors.empty())
        { out.reason = "v4_projection_page_cursor_unavailable"; return out; }
        const auto put = [&](std::string_view value) {
            const auto length = std::to_string(value.size());
            if (out.bytes.size() > byteBudget || length.size() + 1 > byteBudget - out.bytes.size() ||
                value.size() > byteBudget - out.bytes.size() - length.size() - 1) throw CheckpointBudgetExceeded{};
            AppendCheckpointToken(out.bytes, value);
        };
        put("v4_projection_page_audit_v1"); put(before.encoded); put(after.encoded);
        put(std::to_string(page.workUsed)); put(page.done ? "1" : "0"); put(page.paused ? "1" : "0");
        put(std::to_string(page.requirements.size()));
        for (const auto& item : page.requirements)
        {
            if (item.presence != event_graph::PresencePolicy::required && item.presence != event_graph::PresencePolicy::conditional_unresolved)
            { out.bytes.clear(); out.reason = "v4_projection_page_presence_invalid"; return out; }
            put(item.id); put(item.role); put(item.kind); put(item.session); put(item.task); put(item.object); put(item.parameter);
            put(std::to_string(item.occurrence)); put(item.propertiesResolved ? "1" : "0");
            put(item.presence == event_graph::PresencePolicy::required ? "required" : "conditional_unresolved");
            put(item.presenceCondition); put(item.exclusiveSessionGroup);
        }
        put(std::to_string(page.pairs.size()));
        for (const auto& item : page.pairs)
        {
            if (static_cast<unsigned>(item.relation) > static_cast<unsigned>(event_graph::PairRelation::unresolved))
            { out.bytes.clear(); out.reason = "v4_projection_page_relation_invalid"; return out; }
            put(item.left); put(item.right);
            switch (item.relation) {
            case event_graph::PairRelation::same: put("same"); break;
            case event_graph::PairRelation::distinct: put("distinct"); break;
            case event_graph::PairRelation::either: put("either"); break;
            case event_graph::PairRelation::unresolved: put("unresolved"); break;
            }
        }
        put(std::to_string(page.strictBefore.size()));
        for (const auto& edge : page.strictBefore) { put(edge.earlier); put(edge.later); }
        put(std::to_string(page.unresolved.size())); for (const auto& item : page.unresolved) put(item);
        out.encoded = true; out.reason = "v4_projection_page_bytes_only_not_publication"; return out;
    }
    catch (const CheckpointBudgetExceeded&) { out.bytes.clear(); out.resourceUnavailable = true; out.reason = "v4_projection_page_byte_budget_unavailable"; return out; }
    catch (const std::bad_alloc&) { out.bytes.clear(); out.resourceUnavailable = true; out.reason = "v4_projection_page_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.bytes.clear(); out.resourceUnavailable = true; out.reason = "v4_projection_page_resource_unavailable"; return out; }
}

// Complete authority-generated deferred advance, including every prune. Only
// a page ending at an unconsumed projection leaf is accepted here.
inline V4ProjectionPageEncoding EncodeV4ProjectionAdvance(const V4FamilyDescriptor& family,
    const V4AxisBudget& budget, const event_graph::V4FamilyStagedNativePage& page, std::size_t byteBudget)
{
    V4ProjectionPageEncoding out;
    try
    {
        if (!page.projectionRequired || page.record || !page.candidatePruned.empty() || !page.activePruned.empty() || page.exhausted ||
            page.candidateId.empty() || page.candidateOrdinal.empty() || page.pendingOrdinal.empty())
        { out.reason = "v4_projection_advance_shape_unavailable"; return out; }
        const auto before = event_graph::V4FamilyCandidateGraph::SerializeCursor(family, budget, page.before);
        const auto after = event_graph::V4FamilyCandidateGraph::SerializeCursor(family, budget, page.after);
        if (before.status != event_graph::V4FamilyTraversalStatus::ready || after.status != event_graph::V4FamilyTraversalStatus::ready ||
            !before.errors.empty() || !after.errors.empty())
        { out.resourceUnavailable = before.status == event_graph::V4FamilyTraversalStatus::resource_unavailable ||
              after.status == event_graph::V4FamilyTraversalStatus::resource_unavailable;
          out.reason = "v4_projection_advance_cursor_unavailable"; return out; }
        const auto put = [&](std::string_view value) {
            const auto length = std::to_string(value.size());
            if (out.bytes.size() > byteBudget || length.size() + 1 > byteBudget - out.bytes.size() ||
                value.size() > byteBudget - out.bytes.size() - length.size() - 1) throw CheckpointBudgetExceeded{};
            AppendCheckpointToken(out.bytes, value);
        };
        put("v4_projection_advance_audit_v1"); put(before.encoded); put(after.encoded);
        put(page.candidate.ordinal); put(std::to_string(page.candidate.values.size()));
        for (const auto& value : page.candidate.values) put(value);
        put(page.candidateId); put(page.candidateOrdinal); put(page.pendingOrdinal);
        put(std::to_string(page.pendingPresent.size()));
        for (const bool present : page.pendingPresent) put(present ? "1" : "0");
        put(std::to_string(page.workUsed)); put(page.projectionRequired ? "1" : "0");
        put(page.exhausted ? "1" : "0"); put(page.paused ? "1" : "0");
        put(std::to_string(page.presencePruned.size()));
        for (const auto& item : page.presencePruned)
        {
            put(item.candidateOrdinal); put(item.candidateId); put(std::to_string(item.prune.prefix.size()));
            for (const auto value : item.prune.prefix) put(std::to_string(value));
            put(item.prune.reason); put(item.prune.premise); put(item.prune.consequence);
        }
        out.encoded = true; out.reason = "v4_projection_advance_bytes_only"; return out;
    }
    catch (const CheckpointBudgetExceeded&) { out.bytes.clear(); out.resourceUnavailable = true; out.reason = "v4_projection_advance_byte_budget_unavailable"; return out; }
    catch (const std::bad_alloc&) { out.bytes.clear(); out.resourceUnavailable = true; out.reason = "v4_projection_advance_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.bytes.clear(); out.resourceUnavailable = true; out.reason = "v4_projection_advance_resource_unavailable"; return out; }
}

// Portable identity for one unexecuted structured family/candidate/graph.
// All fields are length-framed and replayed against the current authorities;
// the string is not an execution receipt, a coverage claim, or authenticated
// checkpoint history. Both graph types use their own versioned graph IDs.
enum class V4StructuredCaseStatus { unexecuted, invalid, unavailable, resource_unavailable };
struct V4StructuredCaseReplay
{
    V4StructuredCaseStatus status = V4StructuredCaseStatus::invalid;
    std::string caseId, familyBinding, familyCoordinate, candidateId, graphMode, graphId;
    std::vector<bool> present;
    std::optional<V4FamilyDescriptor> family;
    std::optional<Candidate> candidate;
    std::optional<event_graph::V4JoinedGraphVariant> oleGraph;
    std::optional<event_graph::V4NativeJoinedGraphVariant> nativeGraph;
    std::vector<std::string> unresolved;
    std::string_view reason;
};

inline V4StructuredCaseReplay ReplayV4StructuredCaseId(std::string_view caseId,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
    std::size_t tokenBudget, std::size_t caseByteBudget)
{
    V4StructuredCaseReplay out;
    if (caseId.size() > caseByteBudget)
    { out.status = V4StructuredCaseStatus::resource_unavailable; out.reason = "v4_structured_case_byte_budget"; return out; }
    try
    {
        std::string_view remaining = caseId, token;
        const auto read = [&] { return event_graph::ReadGraphToken(remaining, token); };
        if (!read() || token != "v4_structured_case_v1" || !read() || token != V4StructuredPrefixAlgorithm || !read())
        { out.reason = "v4_structured_case_encoding_invalid"; return out; }
        std::size_t count = 0;
        if (!CheckpointSize(token, count)) { out.reason = "v4_structured_case_prefix_count_invalid"; return out; }
        if (count > tokenBudget) { out.status = V4StructuredCaseStatus::resource_unavailable; out.reason = "v4_structured_case_token_budget"; return out; }
        if (count > remaining.size() / 2) { out.reason = "v4_structured_case_prefix_truncated"; return out; }
        std::vector<std::uint32_t> prefix; prefix.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            std::size_t value = 0;
            if (!read() || !CheckpointSize(token, value) || value > (std::numeric_limits<std::uint32_t>::max)())
            { out.reason = "v4_structured_case_prefix_token_invalid"; return out; }
            prefix.push_back(static_cast<std::uint32_t>(value));
        }
        if (!read()) { out.reason = "v4_structured_case_family_missing"; return out; }
        const std::string familyBinding(token);
        if (!read()) { out.reason = "v4_structured_case_candidate_missing"; return out; }
        const std::string candidateId(token);
        if (!read()) { out.reason = "v4_structured_case_mode_missing"; return out; }
        const std::string graphMode(token);
        if (!read()) { out.reason = "v4_structured_case_presence_missing"; return out; }
        const std::string presence(token);
        if (!read() || !remaining.empty()) { out.reason = "v4_structured_case_graph_invalid"; return out; }
        const std::string graphId(token);
        if ((graphMode != "ole" && graphMode != "native") || graphId.empty() ||
            std::any_of(presence.begin(), presence.end(), [](char bit) { return bit != '0' && bit != '1'; }))
        { out.reason = "v4_structured_case_mode_or_presence_invalid"; return out; }
        const auto parsed = InspectV4StructuredPrefix(prefix, tokenBudget);
        if (parsed.status == V4StructuredPrefixStatus::resource_unavailable)
        { out.status = V4StructuredCaseStatus::resource_unavailable; out.reason = parsed.reason; return out; }
        if (parsed.status != V4StructuredPrefixStatus::complete || !parsed.family || V4FamilyBinding(*parsed.family) != familyBinding)
        { out.reason = "v4_structured_case_family_mismatch"; return out; }
        const auto coordinate = EncodeV4FamilyCoordinate(*parsed.family, coordinateBudget);
        if (coordinate.status == V4FamilyCoordinateStatus::resource_unavailable)
        { out.status = V4StructuredCaseStatus::resource_unavailable; out.reason = coordinate.reason; return out; }
        if (coordinate.status != V4FamilyCoordinateStatus::ready)
        { out.reason = "v4_structured_case_family_invalid"; return out; }
        const auto identity = event_graph::V4FamilyIdentityContext::Create(*parsed.family, axisBudget);
        if (identity.Status() != V4FamilyStatus::schema_only)
        { out.status = identity.Status() == V4FamilyStatus::resource_unavailable ? V4StructuredCaseStatus::resource_unavailable : V4StructuredCaseStatus::unavailable;
          out.reason = "v4_structured_case_schema_unavailable"; return out; }
        std::string_view raw = candidateId, part;
        const auto readRaw = [&] { return event_graph::ReadGraphToken(raw, part); };
        if (!readRaw() || part != "global_v4_raw_candidate_coordinate_v1" || !readRaw() || part != familyBinding ||
            !readRaw() || part != identity.SchemaBinding() || !readRaw() || !CheckpointSize(part, count) || count != identity.Axes().size())
        { out.reason = "v4_structured_case_candidate_header_invalid"; return out; }
        Candidate candidate; candidate.values.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            if (!readRaw() || part != identity.Axes()[i].id || !readRaw())
            { out.reason = "v4_structured_case_candidate_axis_invalid"; return out; }
            candidate.values.emplace_back(part);
        }
        if (!raw.empty()) { out.reason = "v4_structured_case_candidate_trailing_data"; return out; }
        const auto verified = V4FamilyValidatedContext::Create(*parsed.family, axisBudget, candidate);
        if (verified.Status() != V4CoordinateStatus::coordinate_ready)
        { out.status = verified.Status() == V4CoordinateStatus::resource_unavailable ? V4StructuredCaseStatus::resource_unavailable : V4StructuredCaseStatus::unavailable;
          out.reason = "v4_structured_case_candidate_unavailable"; return out; }
        const auto rawIdentity = identity.RawCandidateIdentity(verified.Coordinate());
        if (rawIdentity.resourceUnavailable || !rawIdentity.errors.empty() || rawIdentity.id != candidateId)
        { out.status = rawIdentity.resourceUnavailable ? V4StructuredCaseStatus::resource_unavailable : V4StructuredCaseStatus::invalid;
          out.reason = "v4_structured_case_candidate_mismatch"; return out; }
        for (char bit : presence) out.present.push_back(bit == '1');
        if (graphMode == "ole")
        {
            const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, out.present, graphId);
            if (!replay.identityValid || !replay.errors.empty() || replay.variant.id != graphId)
            { out.status = replay.status == event_graph::V4GraphReplayStatus::resource_unavailable ?
                V4StructuredCaseStatus::resource_unavailable : V4StructuredCaseStatus::unavailable;
              out.reason = "v4_structured_case_ole_graph_unavailable"; return out; }
            out.oleGraph = replay.variant; out.unresolved = replay.variant.unresolved;
        }
        else
        {
            const auto replay = event_graph::V4NativeJoinedGraph::Replay(identity, verified, out.present, graphId);
            if (!replay.identityValid || !replay.errors.empty() || replay.variant.id != graphId)
            { out.status = replay.status == event_graph::V4GraphReplayStatus::resource_unavailable ?
                V4StructuredCaseStatus::resource_unavailable : V4StructuredCaseStatus::unavailable;
              out.reason = "v4_structured_case_native_graph_unavailable"; return out; }
            out.nativeGraph = replay.variant; out.unresolved = replay.variant.unresolved;
        }
        std::string canonical;
        event_graph::AppendToken(canonical, "v4_structured_case_v1"); event_graph::AppendToken(canonical, V4StructuredPrefixAlgorithm);
        event_graph::AppendToken(canonical, std::to_string(prefix.size()));
        for (auto value : prefix) event_graph::AppendToken(canonical, std::to_string(value));
        for (const auto& value : {familyBinding, candidateId, graphMode, presence, graphId}) event_graph::AppendToken(canonical, value);
        if (canonical != caseId) { out.reason = "v4_structured_case_noncanonical"; return out; }
        out.caseId = std::move(canonical); out.familyBinding = familyBinding; out.familyCoordinate = coordinate.coordinate;
        out.candidateId = candidateId; out.graphMode = graphMode; out.graphId = graphId;
        out.family = *parsed.family; out.candidate = verified.Coordinate();
        out.unresolved.emplace_back("v4_structured_case_production_dispatch_completion_and_business_unimplemented");
        out.status = V4StructuredCaseStatus::unexecuted; return out;
    }
    catch (const std::bad_alloc&) { out.status = V4StructuredCaseStatus::resource_unavailable; out.reason = "v4_structured_case_allocation_unavailable"; return out; }
    catch (const std::length_error&) { out.status = V4StructuredCaseStatus::resource_unavailable; out.reason = "v4_structured_case_allocation_unavailable"; return out; }
}

inline V4StructuredCaseReplay MakeV4StructuredCaseId(const std::vector<std::uint32_t>& prefix,
    const Candidate& candidate, const std::vector<bool>& present, std::string_view graphMode, std::string_view graphId,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
    std::size_t tokenBudget, std::size_t caseByteBudget)
{
    V4StructuredCaseReplay out;
    try
    {
        const auto parsed = InspectV4StructuredPrefix(prefix, tokenBudget);
        if (parsed.status == V4StructuredPrefixStatus::resource_unavailable)
        { out.status = V4StructuredCaseStatus::resource_unavailable; out.reason = parsed.reason; return out; }
        if (parsed.status != V4StructuredPrefixStatus::complete || !parsed.family)
        { out.reason = "v4_structured_case_family_prefix_invalid"; return out; }
        const auto identity = event_graph::V4FamilyIdentityContext::Create(*parsed.family, axisBudget);
        if (identity.Status() != V4FamilyStatus::schema_only)
        { out.status = identity.Status() == V4FamilyStatus::resource_unavailable ? V4StructuredCaseStatus::resource_unavailable : V4StructuredCaseStatus::unavailable;
          out.reason = "v4_structured_case_schema_unavailable"; return out; }
        const auto verified = V4FamilyValidatedContext::Create(*parsed.family, axisBudget, candidate);
        if (verified.Status() != V4CoordinateStatus::coordinate_ready)
        { out.status = verified.Status() == V4CoordinateStatus::resource_unavailable ? V4StructuredCaseStatus::resource_unavailable : V4StructuredCaseStatus::unavailable;
          out.reason = "v4_structured_case_candidate_unavailable"; return out; }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty())
        { out.status = raw.resourceUnavailable ? V4StructuredCaseStatus::resource_unavailable : V4StructuredCaseStatus::invalid;
          out.reason = "v4_structured_case_candidate_unavailable"; return out; }
        std::string bits; for (const auto bit : present) bits += bit ? '1' : '0';
        std::string caseId;
        const auto append = [&](std::string_view value) {
            const auto length = std::to_string(value.size());
            if (caseId.size() > caseByteBudget || length.size() + 1 > caseByteBudget - caseId.size() ||
                value.size() > caseByteBudget - caseId.size() - length.size() - 1) return false;
            event_graph::AppendToken(caseId, value); return true;
        };
        if (!append("v4_structured_case_v1") || !append(V4StructuredPrefixAlgorithm) || !append(std::to_string(prefix.size())))
        { out.status = V4StructuredCaseStatus::resource_unavailable; out.reason = "v4_structured_case_byte_budget"; return out; }
        for (auto token : prefix) if (!append(std::to_string(token)))
        { out.status = V4StructuredCaseStatus::resource_unavailable; out.reason = "v4_structured_case_byte_budget"; return out; }
        if (!append(identity.FamilyBinding()) || !append(raw.id) || !append(graphMode) || !append(bits) || !append(graphId))
        { out.status = V4StructuredCaseStatus::resource_unavailable; out.reason = "v4_structured_case_byte_budget"; return out; }
        return ReplayV4StructuredCaseId(caseId, coordinateBudget, axisBudget, tokenBudget, caseByteBudget);
    }
    catch (const std::bad_alloc&) { out.status = V4StructuredCaseStatus::resource_unavailable; out.reason = "v4_structured_case_allocation_unavailable"; return out; }
    catch (const std::length_error&) { out.status = V4StructuredCaseStatus::resource_unavailable; out.reason = "v4_structured_case_allocation_unavailable"; return out; }
}

// Process-local enumeration only. Each finite family/task index is visited in
// every subsequent diagonal round, even if later indices keep being added.
// No returned record is an execution permit or a durable coverage claim.
struct V4MemoryFairBudget
{
    V4CoordinateBudget coordinate;
    V4AxisBudget axes;
    std::size_t tokenBudget = 0, caseByteBudget = 0, familyWork = 0;
    std::size_t maxTasks = 0, maxFamilies = 0;
};
enum class V4MemoryFairStatus { progress, pending, resource_unavailable, unavailable };
enum class V4MemoryDiscoveryMode { exact_values, file_equivalence_classes };
struct V4MemoryFairResult
{
    V4MemoryFairStatus status = V4MemoryFairStatus::unavailable;
    std::size_t familyIndex = 0, sequence = 0, workUsed = 0;
    std::string familyCoordinate, caseId, classId;
    bool requiresConcreteBinding = false;
    std::optional<event_graph::V4FamilyCandidateRecord> record;
    std::vector<event_graph::V4FamilyPresencePrune> presencePruned;
    std::vector<event_graph::V4FamilyActivePrune> activePruned;
    std::string_view reason;
};
struct V4MemoryFairSnapshotResult
{
    bool available = false, resourceUnavailable = false;
    std::string bytes;
    std::string_view reason;
};
class V4MemoryFairGenerator final
{
    struct Task { V4StructuredDfsCursor cursor; bool done = false; };
    struct Pending
    {
        std::size_t sequence = 0, workBudget = 0, workUsed = 0;
        event_graph::V4FamilyCandidateCursor after;
        std::optional<event_graph::V4FamilyCandidateRecord> record;
        std::vector<event_graph::V4FamilyPresencePrune> presencePruned;
        std::vector<event_graph::V4FamilyActivePrune> activePruned;
        std::string caseId;
        bool delivered = false;
    };
    struct Family
    {
        V4FamilyDescriptor descriptor;
        std::string coordinate;
        std::vector<std::uint32_t> prefix;
        event_graph::V4FamilyCandidateCursor cursor;
        std::optional<Pending> pending;
        std::size_t nextSequence = 0;
    };
    std::vector<Task> tasks_{Task{}};
    std::vector<Family> families_;
    std::size_t nextLayer_ = 1, taskPosition_ = 0, taskRound_ = 1, familyPosition_ = 0, familyRound_ = 1;
    unsigned turn_ = 0, discoveryTurn_ = 0;
    bool layerLimitReached_ = false;
    V4MemoryDiscoveryMode mode_ = V4MemoryDiscoveryMode::exact_values;
    static V4MemoryFairSnapshotResult PendingBytes(const Pending& pending, std::size_t byteBudget) noexcept
    {
        V4MemoryFairSnapshotResult out;
        try
        {
            const auto put = [&](std::string_view value) {
                const auto length = std::to_string(value.size());
                if (out.bytes.size() > byteBudget || length.size() + 1 > byteBudget - out.bytes.size() ||
                    value.size() > byteBudget - out.bytes.size() - length.size() - 1) throw CheckpointBudgetExceeded{};
                AppendCheckpointToken(out.bytes, value);
            };
            const auto number = [&](std::size_t value) { put(std::to_string(value)); };
            const auto strings = [&](const auto& values) { number(values.size()); for (const auto& v : values) put(v); };
            const auto bits = [&](const auto& values) { number(values.size()); for (bool v : values) put(v ? "1" : "0"); };
            put("v4_memory_pending_v1"); number(pending.workBudget); number(pending.workUsed);
            put(pending.record ? "1" : "0");
            if (pending.record)
            {
                const auto& record = *pending.record;
                put(record.kind); put(record.candidateOrdinal); put(record.candidateId); put(record.reason); strings(record.unresolved);
                put(record.coordinate ? "1" : "0");
                if (record.coordinate) { put(record.coordinate->ordinal); strings(record.coordinate->values); }
                const auto graph = [&](const auto& item) {
                    put(item.ordinal); put(item.presenceOrdinal); bits(item.present); put(item.graph.ordinal); put(item.graph.id);
                    number(item.graph.classes.size()); for (auto value : item.graph.classes) number(value);
                    strings(item.graph.order); strings(item.graph.unresolved);
                };
                put(record.graph ? "1" : "0"); if (record.graph) graph(*record.graph);
                put(record.nativeGraph ? "1" : "0"); if (record.nativeGraph) graph(*record.nativeGraph);
            }
            number(pending.presencePruned.size());
            for (const auto& p : pending.presencePruned)
            { put(p.candidateOrdinal); put(p.candidateId); number(p.prune.prefix.size()); for (auto v : p.prune.prefix) number(v);
              put(p.prune.reason); put(p.prune.premise); put(p.prune.consequence); }
            number(pending.activePruned.size());
            for (const auto& p : pending.activePruned)
            { put(p.candidateOrdinal); put(p.candidateId); put(p.prune.presenceOrdinal); bits(p.prune.present);
              const auto& d = p.prune.prune; put(d.scope); put(d.reason); number(d.partition.size()); for (auto v : d.partition) number(v);
              put(d.earlierRequirement); put(d.currentRequirement); }
            out.available = true; out.reason = "v4_memory_pending_encoding_only"; return out;
        }
        catch (const CheckpointBudgetExceeded&) { out.bytes.clear(); out.resourceUnavailable = true; out.reason = "v4_memory_pending_byte_budget"; return out; }
        catch (const std::bad_alloc&) { out.bytes.clear(); out.resourceUnavailable = true; out.reason = "v4_memory_pending_resource"; return out; }
        catch (const std::length_error&) { out.bytes.clear(); out.resourceUnavailable = true; out.reason = "v4_memory_pending_resource"; return out; }
        catch (...) { out.bytes.clear(); out.reason = "v4_memory_pending_unavailable"; return out; }
    }
    static bool NextIndex(std::size_t& position, std::size_t& round, std::size_t& index) noexcept
    {
        if (position == round)
        {
            if (round == (std::numeric_limits<std::size_t>::max)()) return false;
            ++round; position = 0;
        }
        index = position++; return true;
    }
public:
    explicit V4MemoryFairGenerator(V4MemoryDiscoveryMode mode = V4MemoryDiscoveryMode::exact_values) noexcept : mode_(mode) {}
    V4MemoryFairGenerator(const V4MemoryFairGenerator&) = delete;
    V4MemoryFairGenerator& operator=(const V4MemoryFairGenerator&) = delete;
    std::size_t FamilyCount() const noexcept { return families_.size(); }
    std::size_t TaskCount() const noexcept { return tasks_.size(); }
    V4MemoryFairSnapshotResult Snapshot(const V4MemoryFairBudget& budget, std::size_t byteBudget) const noexcept
    {
        V4MemoryFairSnapshotResult out;
        if (mode_ != V4MemoryDiscoveryMode::exact_values)
        { out.reason = "v4_file_classes_process_local_snapshot_unimplemented"; return out; }
        try
        {
            if (tasks_.size() > budget.maxTasks || families_.size() > budget.maxFamilies)
            { out.resourceUnavailable = true; out.reason = "v4_memory_snapshot_count_budget"; return out; }
            const auto put = [&](std::string_view value) {
                const auto length = std::to_string(value.size());
                if (out.bytes.size() > byteBudget || length.size() + 1 > byteBudget - out.bytes.size() ||
                    value.size() > byteBudget - out.bytes.size() - length.size() - 1) throw CheckpointBudgetExceeded{};
                AppendCheckpointToken(out.bytes, value);
            };
            const auto number = [&](std::size_t value) { put(std::to_string(value)); };
            const auto flag = [&](bool value) { put(value ? "1" : "0"); };
            const auto numbers = [&](const auto& values) { number(values.size()); for (auto value : values) number(value); };
            put("v4_memory_fair_snapshot_v1"); put(V4StructuredPrefixAlgorithm); put(V4StructuredDfsAlgorithm);
            put(V4FamilyCoordinateAlgorithm); put(event_graph::V4FamilyCandidateGraphAlgorithm);
            number(nextLayer_); number(taskPosition_); number(taskRound_); number(familyPosition_); number(familyRound_);
            number(turn_); number(discoveryTurn_); flag(layerLimitReached_);
            number(tasks_.size());
            for (const auto& task : tasks_)
            {
                number(task.cursor.layer); number(task.cursor.stopDepth); numbers(task.cursor.prefix); numbers(task.cursor.nextChoice);
                flag(task.cursor.exhausted); flag(task.done);
            }
            number(families_.size());
            for (const auto& family : families_)
            {
                const auto cursor = event_graph::V4FamilyCandidateGraph::SerializeCursor(family.descriptor, budget.axes, family.cursor);
                if (cursor.status != event_graph::V4FamilyTraversalStatus::ready || !cursor.errors.empty())
                { out.bytes.clear(); out.resourceUnavailable = cursor.status == event_graph::V4FamilyTraversalStatus::resource_unavailable;
                  out.reason = "v4_memory_snapshot_cursor_unavailable"; return out; }
                put(family.coordinate); numbers(family.prefix); put(cursor.encoded); number(family.nextSequence); flag(family.pending.has_value());
                if (family.pending)
                {
                    const auto& pending = *family.pending;
                    const auto after = event_graph::V4FamilyCandidateGraph::SerializeCursor(family.descriptor, budget.axes, pending.after);
                    const auto encoded = PendingBytes(pending, byteBudget);
                    if (after.status != event_graph::V4FamilyTraversalStatus::ready || !after.errors.empty() || !encoded.available)
                    { out.bytes.clear(); out.resourceUnavailable = after.status == event_graph::V4FamilyTraversalStatus::resource_unavailable || encoded.resourceUnavailable;
                      out.reason = "v4_memory_snapshot_pending_unavailable"; return out; }
                    number(pending.sequence); number(pending.workBudget); number(pending.workUsed); put(after.encoded); put(encoded.bytes);
                    put(pending.caseId); flag(pending.delivered);
                }
            }
            out.available = true; out.reason = "v4_memory_snapshot_caller_must_persist_execution_states"; return out;
        }
        catch (const CheckpointBudgetExceeded&) { out.bytes.clear(); out.resourceUnavailable = true; out.reason = "v4_memory_snapshot_byte_budget"; return out; }
        catch (const std::bad_alloc&) { out.bytes.clear(); out.resourceUnavailable = true; out.reason = "v4_memory_snapshot_resource"; return out; }
        catch (const std::length_error&) { out.bytes.clear(); out.resourceUnavailable = true; out.reason = "v4_memory_snapshot_resource"; return out; }
        catch (...) { out.bytes.clear(); out.reason = "v4_memory_snapshot_unavailable"; return out; }
    }
    // The caller owns snapshot authenticity and all per-case execution states.
    // In-flight or unknown cases must not be automatically dispatched again.
    // Restore rebuilds into a new object and never mutates a live generator.
    static std::unique_ptr<V4MemoryFairGenerator> Restore(std::string_view bytes, const V4MemoryFairBudget& budget,
        std::size_t byteBudget, bool& resourceUnavailable, std::string_view& reason) noexcept
    {
        resourceUnavailable = false; reason = "v4_memory_restore_unavailable";
        if (bytes.size() > byteBudget) { resourceUnavailable = true; reason = "v4_memory_restore_byte_budget"; return {}; }
        try
        {
            std::string_view remaining = bytes, token;
            const auto read = [&] { return ReadCheckpointToken(remaining, token); };
            const auto number = [&](std::size_t& value) { return read() && CheckpointSize(token, value); };
            const auto flag = [&](bool& value) { if (!read() || (token != "0" && token != "1")) return false; value = token == "1"; return true; };
            const auto numbers = [&](std::vector<std::uint32_t>& values) {
                std::size_t count = 0;
                if (!number(count)) return false;
                // A DFS frame contains one next-choice entry beyond its
                // prefix; the authoritative parser checks each actual shape.
                if (count > budget.tokenBudget && count - budget.tokenBudget > 1) { resourceUnavailable = true; return false; }
                if (count > remaining.size() / 2) return false;
                values.reserve(count);
                for (std::size_t i = 0; i < count; ++i)
                { std::size_t value = 0; if (!number(value) || value > (std::numeric_limits<std::uint32_t>::max)()) return false;
                  values.push_back(static_cast<std::uint32_t>(value)); }
                return true;
            };
            if (!read() || token != "v4_memory_fair_snapshot_v1" || !read() || token != V4StructuredPrefixAlgorithm ||
                !read() || token != V4StructuredDfsAlgorithm || !read() || token != V4FamilyCoordinateAlgorithm ||
                !read() || token != event_graph::V4FamilyCandidateGraphAlgorithm) return {};
            auto result = std::unique_ptr<V4MemoryFairGenerator>(new V4MemoryFairGenerator);
            result->tasks_.clear(); result->families_.clear();
            std::size_t turn = 0, discoveryTurn = 0;
            if (!number(result->nextLayer_) || !number(result->taskPosition_) || !number(result->taskRound_) ||
                !number(result->familyPosition_) || !number(result->familyRound_) || !number(turn) || !number(discoveryTurn) ||
                !flag(result->layerLimitReached_) || turn > 2 || discoveryTurn > 2 || !result->taskRound_ || !result->familyRound_ ||
                result->taskPosition_ > result->taskRound_ || result->familyPosition_ > result->familyRound_) return {};
            result->turn_ = static_cast<unsigned>(turn); result->discoveryTurn_ = static_cast<unsigned>(discoveryTurn);
            std::size_t taskCount = 0;
            if (!number(taskCount) || !taskCount) return {};
            if (taskCount > budget.maxTasks) { resourceUnavailable = true; reason = "v4_memory_restore_task_budget"; return {}; }
            if (taskCount > remaining.size() / 12) return {};
            result->tasks_.reserve(taskCount);
            std::set<std::size_t> mainLayers;
            for (std::size_t i = 0; i < taskCount; ++i)
            {
                Task task;
                if (!number(task.cursor.layer) || !number(task.cursor.stopDepth) || !numbers(task.cursor.prefix) ||
                    !numbers(task.cursor.nextChoice) || !flag(task.cursor.exhausted) || !flag(task.done) || task.done != task.cursor.exhausted) return {};
                const auto check = StepV4StructuredDfs(task.cursor, budget.coordinate, budget.tokenBudget, 0);
                if (check.status == V4StructuredDfsStatus::resource_unavailable)
                { resourceUnavailable = true; reason = "v4_memory_restore_task_resource"; return {}; }
                if (check.status != (task.done ? V4StructuredDfsStatus::exhausted : V4StructuredDfsStatus::progress)) return {};
                if (!task.cursor.stopDepth && !mainLayers.insert(task.cursor.layer).second) return {};
                result->tasks_.push_back(std::move(task));
            }
            if (mainLayers.empty()) return {};
            if (result->layerLimitReached_)
            { if (result->nextLayer_ != (std::numeric_limits<std::size_t>::max)() ||
                  *mainLayers.begin() != 0 || *mainLayers.rbegin() != result->nextLayer_) return {}; }
            else if (result->nextLayer_ != mainLayers.size() || *mainLayers.begin() != 0 ||
                *mainLayers.rbegin() != result->nextLayer_ - 1) return {};
            std::size_t familyCount = 0;
            if (!number(familyCount)) return {};
            if (familyCount > budget.maxFamilies) { resourceUnavailable = true; reason = "v4_memory_restore_family_budget"; return {}; }
            if (familyCount > remaining.size() / 10) return {};
            result->families_.reserve(familyCount); std::set<std::string> coordinates;
            for (std::size_t i = 0; i < familyCount; ++i)
            {
                Family family;
                if (!read()) return {}; family.coordinate = token;
                if (!coordinates.insert(family.coordinate).second || !numbers(family.prefix)) return {};
                const auto parsed = InspectV4StructuredPrefix(family.prefix, budget.tokenBudget);
                if (parsed.status == V4StructuredPrefixStatus::resource_unavailable)
                { resourceUnavailable = true; reason = "v4_memory_restore_prefix_resource"; return {}; }
                if (parsed.status != V4StructuredPrefixStatus::complete || !parsed.family) return {};
                const auto coordinate = EncodeV4FamilyCoordinate(*parsed.family, budget.coordinate);
                if (coordinate.status == V4FamilyCoordinateStatus::resource_unavailable)
                { resourceUnavailable = true; reason = "v4_memory_restore_coordinate_resource"; return {}; }
                if (coordinate.status != V4FamilyCoordinateStatus::ready || coordinate.coordinate != family.coordinate || !read()) return {};
                family.descriptor = *parsed.family;
                const auto cursor = event_graph::V4FamilyCandidateGraph::ParseCursor(family.descriptor, budget.axes, token);
                if (cursor.status != event_graph::V4FamilyTraversalStatus::ready || !cursor.errors.empty())
                { resourceUnavailable = cursor.status == event_graph::V4FamilyTraversalStatus::resource_unavailable;
                  reason = "v4_memory_restore_family_cursor_unavailable"; return {}; }
                family.cursor = cursor.cursor;
                bool hasPending = false;
                if (!number(family.nextSequence) || !flag(hasPending)) return {};
                if (hasPending)
                {
                    Pending pending; std::string after, expected;
                    if (!number(pending.sequence) || !number(pending.workBudget) || !number(pending.workUsed) || !read()) return {};
                    after = token; if (!read()) return {}; expected = token; if (!read()) return {}; pending.caseId = token;
                    if (!flag(pending.delivered) || pending.sequence == (std::numeric_limits<std::size_t>::max)() ||
                        pending.sequence + 1 != family.nextSequence) return {};
                    if (pending.workBudget > budget.familyWork)
                    { resourceUnavailable = true; reason = "v4_memory_restore_work_budget"; return {}; }
                    const auto step = event_graph::V4FamilyCandidateGraph::Step(family.descriptor, budget.axes, family.cursor, pending.workBudget);
                    if (step.status != event_graph::V4FamilyTraversalStatus::ready || !step.errors.empty())
                    { resourceUnavailable = step.status == event_graph::V4FamilyTraversalStatus::resource_unavailable;
                      reason = "v4_memory_restore_pending_replay_unavailable"; return {}; }
                    if (step.workUsed != pending.workUsed || (!step.record && step.presencePruned.empty() && step.activePruned.empty())) return {};
                    pending.after = step.cursor; pending.record = step.record;
                    pending.presencePruned = step.presencePruned; pending.activePruned = step.activePruned;
                    const auto serialized = event_graph::V4FamilyCandidateGraph::SerializeCursor(family.descriptor, budget.axes, pending.after);
                    const auto fingerprint = PendingBytes(pending, byteBudget);
                    if (serialized.status != event_graph::V4FamilyTraversalStatus::ready || !serialized.errors.empty() || !fingerprint.available)
                    { resourceUnavailable = serialized.status == event_graph::V4FamilyTraversalStatus::resource_unavailable || fingerprint.resourceUnavailable;
                      reason = "v4_memory_restore_pending_encoding_unavailable"; return {}; }
                    if (serialized.encoded != after || fingerprint.bytes != expected) return {};
                    if (pending.record && pending.record->kind == "graph_hypothesis")
                    {
                        const auto& record = *pending.record;
                        if (!record.coordinate || static_cast<bool>(record.graph) == static_cast<bool>(record.nativeGraph) ||
                            (pending.delivered && pending.caseId.empty())) return {};
                        if (!pending.caseId.empty())
                        {
                            const auto& present = record.graph ? record.graph->present : record.nativeGraph->present;
                            const auto& graphId = record.graph ? record.graph->graph.id : record.nativeGraph->graph.id;
                            const auto replay = MakeV4StructuredCaseId(family.prefix, *record.coordinate, present,
                                record.graph ? "ole" : "native", graphId, budget.coordinate, budget.axes, budget.tokenBudget, budget.caseByteBudget);
                            if (replay.status != V4StructuredCaseStatus::unexecuted || replay.caseId != pending.caseId || replay.familyCoordinate != family.coordinate)
                            { resourceUnavailable = replay.status == V4StructuredCaseStatus::resource_unavailable;
                              reason = "v4_memory_restore_case_unavailable"; return {}; }
                        }
                    }
                    else if (!pending.caseId.empty()) return {};
                    family.pending = std::move(pending);
                }
                result->families_.push_back(std::move(family));
            }
            if (!remaining.empty()) return {};
            reason = "v4_memory_restore_execution_states_owned_by_caller"; return result;
        }
        catch (const std::bad_alloc&) { resourceUnavailable = true; reason = "v4_memory_restore_resource"; return {}; }
        catch (const std::length_error&) { resourceUnavailable = true; reason = "v4_memory_restore_resource"; return {}; }
        catch (...) { return {}; }
    }
    // Acknowledgement means the caller retained the proposal; it never means
    // production started or succeeded. A stale sequence cannot advance a cursor.
    bool Acknowledge(std::size_t familyIndex, std::size_t sequence) noexcept
    {
        if (familyIndex >= families_.size()) return false;
        auto& family = families_[familyIndex];
        if (!family.pending || !family.pending->delivered || family.pending->sequence != sequence) return false;
        static_assert(std::is_nothrow_move_assignable_v<event_graph::V4FamilyCandidateCursor>);
        family.cursor = std::move(family.pending->after); family.pending.reset(); return true;
    }
    V4MemoryFairResult Step(const V4MemoryFairBudget& budget) noexcept
    {
        V4MemoryFairResult out;
        try
        {
            const bool discovery = turn_ == 0; turn_ = (turn_ + 1) % 3;
            if (discovery)
            {
                // Two task visits for each new layer; diagonal rounds, not
                // queue growth rate, determine eventual revisits.
                if (discoveryTurn_ == 2)
                {
                    discoveryTurn_ = 0;
                    if (layerLimitReached_ || tasks_.size() >= budget.maxTasks)
                    { out.status = V4MemoryFairStatus::resource_unavailable; out.reason = "v4_memory_layer_budget"; return out; }
                    Task task; task.cursor.layer = nextLayer_; tasks_.push_back(std::move(task));
                    if (nextLayer_ == (std::numeric_limits<std::size_t>::max)()) layerLimitReached_ = true; else ++nextLayer_;
                    out.status = V4MemoryFairStatus::progress; out.reason = "v4_memory_layer_added"; return out;
                }
                ++discoveryTurn_;
                if (tasks_.size() > budget.maxTasks)
                { out.status = V4MemoryFairStatus::resource_unavailable; out.reason = "v4_memory_task_budget"; return out; }
                std::size_t index = 0;
                if (!NextIndex(taskPosition_, taskRound_, index))
                { out.status = V4MemoryFairStatus::resource_unavailable; out.reason = "v4_memory_task_round_limit"; return out; }
                if (index >= tasks_.size() || tasks_[index].done)
                { out.status = V4MemoryFairStatus::progress; out.reason = "v4_memory_task_round_progress"; return out; }
                const auto before = tasks_[index].cursor;
                auto step = StepV4StructuredDfs(before, budget.coordinate, budget.tokenBudget, 1,
                    mode_ == V4MemoryDiscoveryMode::file_equivalence_classes);
                out.workUsed = step.workUsed;
                if (step.status == V4StructuredDfsStatus::resource_unavailable)
                { out.status = V4MemoryFairStatus::resource_unavailable; out.reason = step.reason; return out; }
                if (step.status == V4StructuredDfsStatus::invalid)
                { out.reason = step.reason; return out; }
                if (step.deferred && tasks_.size() >= budget.maxTasks)
                { out.status = V4MemoryFairStatus::resource_unavailable; out.reason = "v4_memory_deferred_budget"; return out; }
                if (step.family)
                {
                    const auto existing = std::find_if(families_.begin(), families_.end(), [&](const auto& f) { return f.coordinate == step.coordinate; });
                    if (existing == families_.end())
                    {
                        if (families_.size() >= budget.maxFamilies)
                        { out.status = V4MemoryFairStatus::resource_unavailable; out.reason = "v4_memory_family_budget"; return out; }
                        Family family; family.descriptor = *step.family; family.coordinate = step.coordinate;
                        if (mode_ == V4MemoryDiscoveryMode::file_equivalence_classes)
                        {
                            // Case encoding/replay uses the unchanged exact grammar;
                            // its placeholder values do not authorize execution.
                            auto encoded = EncodeV4StructuredPrefix(family.descriptor, budget.tokenBudget);
                            if (!encoded.encoded)
                            { out.status = encoded.resourceUnavailable ? V4MemoryFairStatus::resource_unavailable : V4MemoryFairStatus::unavailable;
                              out.reason = encoded.reason; return out; }
                            family.prefix = std::move(encoded.tokens);
                        }
                        else family.prefix = before.prefix;
                        families_.push_back(std::move(family));
                    }
                }
                if (step.deferred) tasks_.push_back(Task{*step.deferred, false});
                const bool done = step.next.exhausted;
                static_assert(std::is_nothrow_move_assignable_v<V4StructuredDfsCursor>);
                tasks_[index].cursor = std::move(step.next);
                tasks_[index].done = done;
                out.status = V4MemoryFairStatus::progress; out.reason = step.reason.empty() ? "v4_memory_discovery_progress" : step.reason; return out;
            }
            std::size_t index = 0;
            if (!NextIndex(familyPosition_, familyRound_, index))
            { out.status = V4MemoryFairStatus::resource_unavailable; out.reason = "v4_memory_family_round_limit"; return out; }
            if (families_.size() > budget.maxFamilies)
            { out.status = V4MemoryFairStatus::resource_unavailable; out.reason = "v4_memory_family_budget"; return out; }
            if (index >= families_.size()) { out.status = V4MemoryFairStatus::progress; out.reason = "v4_memory_family_round_progress"; return out; }
            auto& family = families_[index];
            if (!family.pending)
            {
                if (family.cursor.exhausted) { out.status = V4MemoryFairStatus::progress; out.reason = "v4_memory_family_scan_exhausted"; return out; }
                if (!budget.familyWork) { out.status = V4MemoryFairStatus::resource_unavailable; out.reason = "v4_memory_family_work_budget"; return out; }
                auto step = event_graph::V4FamilyCandidateGraph::Step(family.descriptor, budget.axes, family.cursor, budget.familyWork);
                out.workUsed = step.workUsed;
                if (step.status != event_graph::V4FamilyTraversalStatus::ready || !step.errors.empty())
                { out.status = step.status == event_graph::V4FamilyTraversalStatus::resource_unavailable ? V4MemoryFairStatus::resource_unavailable : V4MemoryFairStatus::unavailable;
                  out.reason = "v4_memory_family_step_unavailable"; return out; }
                if (!step.record && step.presencePruned.empty() && step.activePruned.empty())
                { family.cursor = std::move(step.cursor); out.status = V4MemoryFairStatus::progress; out.reason = "v4_memory_family_progress"; return out; }
                if (family.nextSequence == (std::numeric_limits<std::size_t>::max)())
                { out.status = V4MemoryFairStatus::resource_unavailable; out.reason = "v4_memory_sequence_limit"; return out; }
                Pending pending; pending.sequence = family.nextSequence; pending.workBudget = budget.familyWork; pending.workUsed = step.workUsed;
                pending.after = step.cursor; pending.record = step.record;
                pending.presencePruned = step.presencePruned; pending.activePruned = step.activePruned;
                family.pending = std::move(pending); ++family.nextSequence;
            }
            auto& pending = *family.pending;
            out.familyIndex = index; out.sequence = pending.sequence; out.familyCoordinate = family.coordinate;
            out.record = pending.record; out.presencePruned = pending.presencePruned; out.activePruned = pending.activePruned;
            if (pending.record && pending.record->kind == "graph_hypothesis" && pending.caseId.empty())
            {
                const auto& record = *pending.record;
                if (!record.coordinate || static_cast<bool>(record.graph) == static_cast<bool>(record.nativeGraph))
                { out.reason = "v4_memory_record_identity_unavailable"; return out; }
                const auto& present = record.graph ? record.graph->present : record.nativeGraph->present;
                const auto& id = record.graph ? record.graph->graph.id : record.nativeGraph->graph.id;
                const auto replay = MakeV4StructuredCaseId(family.prefix, *record.coordinate, present,
                    record.graph ? "ole" : "native", id, budget.coordinate, budget.axes, budget.tokenBudget, budget.caseByteBudget);
                if (replay.status != V4StructuredCaseStatus::unexecuted || replay.familyCoordinate != family.coordinate)
                { out.status = replay.status == V4StructuredCaseStatus::resource_unavailable ? V4MemoryFairStatus::resource_unavailable : V4MemoryFairStatus::unavailable;
                  out.reason = "v4_memory_case_replay_unavailable"; return out; }
                pending.caseId = replay.caseId;
            }
            if (mode_ == V4MemoryDiscoveryMode::file_equivalence_classes && !pending.caseId.empty())
            {
                constexpr std::string_view classPrefix = "v4_file_equivalence_class_v1|";
                if (pending.caseId.size() > budget.caseByteBudget || classPrefix.size() > budget.caseByteBudget - pending.caseId.size())
                { out.status = V4MemoryFairStatus::resource_unavailable; out.reason = "v4_file_class_id_byte_budget"; return out; }
                out.classId = std::string(classPrefix) + pending.caseId;
                out.requiresConcreteBinding = true;
            }
            else out.caseId = pending.caseId;
            out.status = V4MemoryFairStatus::pending; out.reason = "v4_memory_unexecuted_proposal"; pending.delivered = true; return out;
        }
        catch (const std::bad_alloc&) { out = {}; out.status = V4MemoryFairStatus::resource_unavailable; out.reason = "v4_memory_allocation_unavailable"; return out; }
        catch (const std::length_error&) { out = {}; out.status = V4MemoryFairStatus::resource_unavailable; out.reason = "v4_memory_allocation_unavailable"; return out; }
        catch (...) { out = {}; out.reason = "v4_memory_observation_unavailable"; return out; }
    }
};

inline bool NormalizeV4FileClassTask(V4TaskSpec& task)
{
    // Node-parent and alias classes require observing the physical tree.
    if (!task.directoryNodes.empty()) return false;
    constexpr std::string_view nameHex = "63006c006100730073005f0070006c0061006300650068006f006c00640065007200";
    for (auto* slots : {&task.filePaths, &task.virtualNames})
        for (auto& slot : *slots) if (slot.kind == V4PayloadKind::exact) slot.utf16leHex = nameHex;
    for (auto& slot : task.fileContents) if (slot.kind == V4PayloadKind::exact && !slot.hex.empty()) slot.hex = "41";
    for (auto& slot : task.payloads)
        if (slot.format == "file_contents" && slot.kind == V4PayloadKind::exact && !slot.hex.empty()) slot.hex = "41";
    return true;
}
inline bool V4FileClassConcreteAxis(std::string_view id) noexcept
{
    if (id.starts_with("task_b.")) id.remove_prefix(7);
    return id.starts_with("V4.file_path.index_") || id.starts_with("V4.virtual_name.index_") ||
        id.starts_with("V4.file_content.index_") || id.starts_with("V4.payload.file_contents.");
}

struct V4FileClassCandidatePreparation
{
    bool prepared = false, resourceUnavailable = false;
    Candidate candidate;
    std::string candidateId;
    std::string_view reason;
};
// Prepare the concrete candidate before fixture construction. The supplied
// family is a new exact identity; this does not move or rebind an old case.
inline V4FileClassCandidatePreparation PrepareV4FileClassCandidate(std::string_view classId,
    const V4FamilyDescriptor& concreteFamily, const V4CoordinateBudget& coordinateBudget,
    const V4AxisBudget& axisBudget, std::size_t tokenBudget, std::size_t caseByteBudget) noexcept
{
    V4FileClassCandidatePreparation out;
    try
    {
        constexpr std::string_view prefix = "v4_file_equivalence_class_v1|";
        if (!classId.starts_with(prefix)) { out.reason = "v4_file_class_id_invalid"; return out; }
        const auto cls = ReplayV4StructuredCaseId(classId.substr(prefix.size()), coordinateBudget, axisBudget, tokenBudget, caseByteBudget);
        if (cls.status == V4StructuredCaseStatus::resource_unavailable)
        { out.resourceUnavailable = true; out.reason = "v4_file_class_replay_resource_unavailable"; return out; }
        if (cls.status != V4StructuredCaseStatus::unexecuted || !cls.family || !cls.candidate)
        { out.reason = "v4_file_class_replay_unavailable"; return out; }
        auto normalized = concreteFamily;
        if (!NormalizeV4FileClassTask(normalized.a) || (normalized.b && !NormalizeV4FileClassTask(*normalized.b)))
        { out.reason = "v4_file_class_directory_binding_unimplemented"; return out; }
        if (V4FamilyBinding(normalized) != cls.familyBinding)
        { out.reason = "v4_file_class_family_mismatch"; return out; }
        const auto classSchema = BuildV4FamilySchema(*cls.family, axisBudget);
        const auto concreteSchema = BuildV4FamilySchema(concreteFamily, axisBudget);
        if (classSchema.status == V4FamilyStatus::resource_unavailable || concreteSchema.status == V4FamilyStatus::resource_unavailable)
        { out.resourceUnavailable = true; out.reason = "v4_file_class_schema_resource_unavailable"; return out; }
        if (classSchema.status != V4FamilyStatus::schema_only || concreteSchema.status != V4FamilyStatus::schema_only ||
            classSchema.axes.size() != concreteSchema.axes.size() || cls.candidate->values.size() != classSchema.axes.size())
        { out.reason = "v4_file_class_schema_unavailable"; return out; }
        Candidate candidate; candidate.values.reserve(concreteSchema.axes.size());
        for (std::size_t i = 0; i < concreteSchema.axes.size(); ++i)
        {
            const auto& oldAxis = classSchema.axes[i]; const auto& newAxis = concreteSchema.axes[i];
            if (oldAxis.id != newAxis.id || oldAxis.role != newAxis.role)
            { out.reason = "v4_file_class_axis_mismatch"; return out; }
            if (V4FileClassConcreteAxis(newAxis.id))
            {
                if (newAxis.values.size() != 1) { out.reason = "v4_file_class_concrete_value_unavailable"; return out; }
                candidate.values.push_back(newAxis.values.front());
            }
            else candidate.values.push_back(cls.candidate->values[i]);
        }
        const auto verified = V4FamilyValidatedContext::Create(concreteFamily, axisBudget, candidate);
        const auto identity = event_graph::V4FamilyIdentityContext::Create(concreteFamily, axisBudget);
        if (verified.Status() == V4CoordinateStatus::resource_unavailable || identity.Status() == V4FamilyStatus::resource_unavailable)
        { out.resourceUnavailable = true; out.reason = "v4_file_class_candidate_resource_unavailable"; return out; }
        if (verified.Status() != V4CoordinateStatus::coordinate_ready || identity.Status() != V4FamilyStatus::schema_only)
        { out.reason = "v4_file_class_candidate_unavailable"; return out; }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable) { out.resourceUnavailable = true; out.reason = "v4_file_class_identity_resource_unavailable"; return out; }
        if (!raw.errors.empty() || raw.id.empty()) { out.reason = "v4_file_class_identity_unavailable"; return out; }
        out.candidate = verified.Coordinate(); out.candidateId = raw.id; out.prepared = true;
        out.reason = "v4_file_class_candidate_requires_fixture_and_graph"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_file_class_candidate_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_file_class_candidate_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_file_class_candidate_unavailable"; return out; }
}

struct V4SingleFileClassRepresentative
{
    bool prepared = false, resourceUnavailable = false;
    std::optional<V4FamilyDescriptor> family;
    Candidate candidate;
    std::string candidateId;
    std::string_view reason;
};
// A concrete, isolated-root-relative .txt representative for the currently
// supported single-file document class. Creation and real classification are
// performed later by the same-candidate fixture and product observers.
inline V4SingleFileClassRepresentative PrepareV4SingleFileClassRepresentative(std::string_view classId,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
    std::size_t tokenBudget, std::size_t caseByteBudget) noexcept
{
    V4SingleFileClassRepresentative out;
    try
    {
        constexpr std::string_view prefix = "v4_file_equivalence_class_v1|";
        if (!classId.starts_with(prefix)) { out.reason = "v4_file_class_id_invalid"; return out; }
        const auto cls = ReplayV4StructuredCaseId(classId.substr(prefix.size()), coordinateBudget, axisBudget, tokenBudget, caseByteBudget);
        if (cls.status == V4StructuredCaseStatus::resource_unavailable)
        { out.resourceUnavailable = true; out.reason = "v4_file_class_replay_resource_unavailable"; return out; }
        if (cls.status != V4StructuredCaseStatus::unexecuted || !cls.family || !cls.candidate || cls.family->b ||
            cls.family->a.kind != V4SourceKind::file_only || cls.family->a.fileCount.kind != V3CountKind::exact ||
            cls.family->a.fileCount.decimal != "1" || cls.family->a.filePaths.size() != 1 ||
            cls.family->a.filePaths[0].index != "0" || cls.family->a.filePaths[0].kind != V4PayloadKind::exact ||
            cls.family->a.filePaths[0].base != V4PathBase::owned_root_relative || cls.family->a.fileContents.size() != 1 ||
            cls.family->a.fileContents[0].index != "0" || cls.family->a.fileContents[0].kind != V4PayloadKind::exact ||
            !cls.family->a.directoryNodes.empty())
        { out.reason = "v4_file_class_single_relative_representative_unimplemented"; return out; }
        const auto schema = BuildV4FamilySchema(*cls.family, axisBudget);
        if (schema.status == V4FamilyStatus::resource_unavailable)
        { out.resourceUnavailable = true; out.reason = "v4_file_class_schema_resource_unavailable"; return out; }
        if (schema.status != V4FamilyStatus::schema_only ||
            Value(schema.axes, *cls.candidate, "V4.file.member0_kind") != "file" ||
            Value(schema.axes, *cls.candidate, "V4.file.member0_identity") != "unique" ||
            Value(schema.axes, *cls.candidate, "P01.primary") != "file" ||
            Value(schema.axes, *cls.candidate, "P06.entry_class") != "document")
        { out.reason = "v4_file_class_document_representative_unimplemented"; return out; }
        auto family = *cls.family;
        // UTF-16LE for drag-example.txt; this is a new concrete candidate,
        // not an assertion that the resulting desktop item is a document.
        family.a.filePaths[0].utf16leHex = "64007200610067002d006500780061006d0070006c0065002e00740078007400";
        const auto candidate = PrepareV4FileClassCandidate(classId, family, coordinateBudget, axisBudget, tokenBudget, caseByteBudget);
        if (!candidate.prepared)
        { out.resourceUnavailable = candidate.resourceUnavailable; out.reason = candidate.reason; return out; }
        out.family = std::move(family); out.candidate = candidate.candidate; out.candidateId = candidate.candidateId;
        out.prepared = true; out.reason = "v4_single_file_class_representative_requires_observation_and_graph"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_file_class_representative_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_file_class_representative_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_file_class_representative_unavailable"; return out; }
}

// Relates an already constructed exact case to a file equivalence class.
// This checks model values only: the caller must still observe the actual
// filesystem names, relations, kinds and contents before dispatch.
struct V4FileClassComparison
{
    bool matches = false, resourceUnavailable = false;
    std::string_view reason;
};
inline V4FileClassComparison CompareV4FileClassToExact(std::string_view classId, std::string_view exactCaseId,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
    std::size_t tokenBudget, std::size_t caseByteBudget) noexcept
{
    V4FileClassComparison out;
    try
    {
        constexpr std::string_view prefix = "v4_file_equivalence_class_v1|";
        if (!classId.starts_with(prefix)) { out.reason = "v4_file_class_id_invalid"; return out; }
        const auto cls = ReplayV4StructuredCaseId(classId.substr(prefix.size()), coordinateBudget, axisBudget, tokenBudget, caseByteBudget);
        const auto exact = ReplayV4StructuredCaseId(exactCaseId, coordinateBudget, axisBudget, tokenBudget, caseByteBudget);
        if (cls.status == V4StructuredCaseStatus::resource_unavailable || exact.status == V4StructuredCaseStatus::resource_unavailable)
        { out.resourceUnavailable = true; out.reason = "v4_file_class_replay_resource_unavailable"; return out; }
        if (cls.status != V4StructuredCaseStatus::unexecuted || exact.status != V4StructuredCaseStatus::unexecuted ||
            !cls.family || !exact.family || !cls.candidate || !exact.candidate || cls.graphMode != exact.graphMode || cls.present != exact.present)
        { out.reason = "v4_file_class_replay_unavailable"; return out; }
        auto normalized = *exact.family;
        if (!NormalizeV4FileClassTask(normalized.a) || (normalized.b && !NormalizeV4FileClassTask(*normalized.b)))
        { out.reason = "v4_file_class_directory_binding_unimplemented"; return out; }
        if (V4FamilyBinding(normalized) != cls.familyBinding)
        { out.reason = "v4_file_class_family_mismatch"; return out; }
        const auto classSchema = BuildV4FamilySchema(*cls.family, axisBudget);
        const auto exactSchema = BuildV4FamilySchema(*exact.family, axisBudget);
        if (classSchema.status == V4FamilyStatus::resource_unavailable || exactSchema.status == V4FamilyStatus::resource_unavailable)
        { out.resourceUnavailable = true; out.reason = "v4_file_class_schema_resource_unavailable"; return out; }
        if (classSchema.status != V4FamilyStatus::schema_only || exactSchema.status != V4FamilyStatus::schema_only ||
            classSchema.axes.size() != exactSchema.axes.size() || cls.candidate->values.size() != classSchema.axes.size() ||
            exact.candidate->values.size() != exactSchema.axes.size())
        { out.reason = "v4_file_class_schema_unavailable"; return out; }
        for (std::size_t i = 0; i < classSchema.axes.size(); ++i)
        {
            auto id = std::string_view(classSchema.axes[i].id);
            if (id != exactSchema.axes[i].id || classSchema.axes[i].role != exactSchema.axes[i].role)
            { out.reason = "v4_file_class_axis_mismatch"; return out; }
            const bool concrete = V4FileClassConcreteAxis(id);
            if (!concrete && cls.candidate->values[i] != exact.candidate->values[i])
            { out.reason = "v4_file_class_candidate_mismatch"; return out; }
        }
        if (cls.oleGraph && exact.oleGraph)
        {
            if (cls.oleGraph->classes != exact.oleGraph->classes || cls.oleGraph->order != exact.oleGraph->order)
            { out.reason = "v4_file_class_graph_mismatch"; return out; }
        }
        else if (cls.nativeGraph && exact.nativeGraph)
        {
            if (cls.nativeGraph->classes != exact.nativeGraph->classes || cls.nativeGraph->order != exact.nativeGraph->order)
            { out.reason = "v4_file_class_graph_mismatch"; return out; }
        }
        else { out.reason = "v4_file_class_graph_unavailable"; return out; }
        out.matches = true; out.reason = "v4_file_class_model_match_physical_observation_required"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_file_class_comparison_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_file_class_comparison_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_file_class_comparison_unavailable"; return out; }
}

// Read-only plan, separate from scheduling. A case is replayed without
// rebasing any path; a different owned-root seed is a different case. Actual
// creation requires the caller's published in-flight record and parent ACK.
// The mapping describes candidate inputs only, never observed selection.
enum class V4ExactOwnedFilesStatus { unimplemented, resource_unavailable, planned };
struct V4ExactOwnedFiles
{
    V4ExactOwnedFilesStatus status = V4ExactOwnedFilesStatus::unimplemented;
    V4StructuredCaseReplay replay;
    std::vector<std::size_t> physicalToLogical;
    std::vector<std::filesystem::path> physicalPaths;
    std::shared_ptr<const V4OwnedRelativePathBinding> relativeBinding;
    std::string_view reason;
};
inline V4ExactOwnedFiles PrepareV4ExactOwnedFiles(std::string_view exactCaseId,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
    std::size_t tokenBudget, std::size_t caseByteBudget, const std::filesystem::path& isolatedRoot,
    HANDLE trustedRoot, const FILE_ID_INFO& trustedRootId, std::size_t maxFiles, std::size_t maxBytes)
{
    V4ExactOwnedFiles out;
    try
    {
        out.replay = ReplayV4StructuredCaseId(exactCaseId, coordinateBudget, axisBudget, tokenBudget, caseByteBudget);
        if (out.replay.status == V4StructuredCaseStatus::resource_unavailable)
        { out.status = V4ExactOwnedFilesStatus::resource_unavailable; out.reason = "v4_exact_owned_case_replay_resource_unavailable"; return out; }
        if (out.replay.status != V4StructuredCaseStatus::unexecuted || !out.replay.family || !out.replay.candidate ||
            out.replay.caseId != exactCaseId || out.replay.family->b)
        { out.reason = "v4_exact_owned_case_replay_or_task_unavailable"; return out; }
        const auto& family = *out.replay.family;
        const bool rawPaths = std::all_of(family.a.filePaths.begin(), family.a.filePaths.end(),
            [](const auto& slot) { return slot.base == V4PathBase::raw; });
        const bool relativePaths = !family.a.filePaths.empty() && std::all_of(family.a.filePaths.begin(), family.a.filePaths.end(),
            [](const auto& slot) { return slot.base == V4PathBase::owned_root_relative; });
        if (!rawPaths && !relativePaths) { out.reason = "v4_exact_owned_mixed_path_bases_unimplemented"; return out; }
        const auto files = V4MaterializeCount(family.a.fileCount, maxFiles, false);
        const auto logical = V4MaterializeCount(family.a.selectionCount, axisBudget.logicalMembers, false);
        if (files.status == V4AxisStatus::resource_unavailable || logical.status == V4AxisStatus::resource_unavailable)
        { out.status = V4ExactOwnedFilesStatus::resource_unavailable; out.reason = "v4_exact_owned_member_budget_unavailable"; return out; }
        if (files.status != V4AxisStatus::schema_only || logical.status != V4AxisStatus::schema_only ||
            !files.value || !logical.value)
        { out.reason = "v4_exact_owned_member_count_unimplemented"; return out; }
        const auto identity = event_graph::V4FamilyIdentityContext::Create(family, axisBudget);
        const auto verified = V4FamilyValidatedContext::Create(family, axisBudget, *out.replay.candidate);
        if (identity.Status() == V4FamilyStatus::resource_unavailable || verified.Status() == V4CoordinateStatus::resource_unavailable)
        { out.status = V4ExactOwnedFilesStatus::resource_unavailable; out.reason = "v4_exact_owned_context_resource_unavailable"; return out; }
        if (identity.Status() != V4FamilyStatus::schema_only || verified.Status() != V4CoordinateStatus::coordinate_ready ||
            identity.FamilyBinding() != out.replay.familyBinding || verified.FamilyBinding() != out.replay.familyBinding)
        { out.reason = "v4_exact_owned_context_unavailable"; return out; }
        if (relativePaths)
        {
            const auto observed = ObserveV4OwnedRelativePaths(family, verified, identity, isolatedRoot,
                trustedRoot, trustedRootId, maxFiles, axisBudget.payloadBytes);
            if (!observed.binding)
            { if (observed.resourceUnavailable) out.status = V4ExactOwnedFilesStatus::resource_unavailable;
              out.reason = observed.reason; return out; }
            out.relativeBinding = observed.binding;
        }
        out.physicalToLogical.reserve(files.value);
        out.physicalPaths.reserve(files.value);
        std::set<std::size_t> used;
        std::size_t byteCount = 0;
        for (std::size_t i = 0; i < files.value; ++i)
        {
            const auto base = "V4.file.member" + std::to_string(i);
            const auto relation = Value(verified.Axes(), verified.Coordinate(),
                base + "_logical_relation");
            std::size_t member = 0;
            if (!relation.starts_with("member_") || !CheckpointSize(relation.substr(7), member) ||
                member >= logical.value || !used.insert(member).second ||
                Value(verified.Axes(), verified.Coordinate(), base + "_kind") != "file" ||
                Value(verified.Axes(), verified.Coordinate(), base + "_identity") != "unique")
            { out.reason = "v4_exact_owned_logical_mapping_unimplemented"; return out; }
            const auto pathValue = Value(verified.Axes(), verified.Coordinate(), "V4.file_path.index_" + std::to_string(i) + ".utf16le");
            const auto bytesValue = Value(verified.Axes(), verified.Coordinate(), "V4.file_content.index_" + std::to_string(i) + ".bytes");
            if ((!relativePaths && (!pathValue.starts_with("utf16le_hex_") || pathValue.size() % 4)) ||
                !bytesValue.starts_with("hex_") || bytesValue.size() % 2)
            { out.reason = "v4_exact_owned_path_or_bytes_unimplemented"; return out; }
            const auto hex = relativePaths ? std::string_view(out.relativeBinding->Members()[i].physicalUtf16leHex) : pathValue.substr(12);
            const auto digit = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
            std::wstring name; name.reserve(hex.size() / 4);
            for (std::size_t j = 0; j < hex.size(); j += 4)
            {
                const int a = digit(hex[j]), b = digit(hex[j + 1]), c = digit(hex[j + 2]), d = digit(hex[j + 3]);
                if (a < 0 || b < 0 || c < 0 || d < 0 || !(a || b || c || d))
                { out.reason = "v4_exact_owned_path_encoding_unavailable"; return out; }
                name.push_back(static_cast<wchar_t>(((a << 4) | b) | (((c << 4) | d) << 8)));
            }
            const std::filesystem::path path(name);
            const auto slot = MakeV4OwnedRootPathSeed(isolatedRoot, path.filename().native(), trustedRoot, trustedRootId);
            if (!slot.available || slot.path != path || slot.slot.utf16leHex != hex)
            { out.reason = "v4_exact_owned_root_or_path_unavailable"; return out; }
            const auto bytes = (bytesValue.size() - 4) / 2;
            if (bytes > maxBytes - byteCount)
            { out.status = V4ExactOwnedFilesStatus::resource_unavailable; out.reason = "v4_exact_owned_byte_budget_unavailable"; return out; }
            byteCount += bytes;
            for (const auto& prior : out.physicalPaths)
            {
                const auto previous = prior.filename().native(), leaf = path.filename().native();
                if (previous.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
                    leaf.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
                { out.reason = "v4_exact_owned_path_comparison_unavailable"; return out; }
                const auto comparison = CompareStringOrdinal(previous.data(), static_cast<int>(previous.size()),
                    leaf.data(), static_cast<int>(leaf.size()), TRUE);
                if (!comparison || comparison == CSTR_EQUAL)
                { out.reason = "v4_exact_owned_path_alias_unimplemented"; return out; }
            }
            out.physicalToLogical.push_back(member);
            out.physicalPaths.push_back(path);
        }
        out.status = V4ExactOwnedFilesStatus::planned;
        out.reason = "v4_exact_owned_files_plan_only_requires_committed_attempt_and_parent_ack";
        return out;
    }
    catch (const std::bad_alloc&) { out.status = V4ExactOwnedFilesStatus::resource_unavailable; out.reason = "v4_exact_owned_files_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.status = V4ExactOwnedFilesStatus::resource_unavailable; out.reason = "v4_exact_owned_files_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_exact_owned_files_observation_unavailable"; return out; }
}

struct V4ExactOwnedFilesMaterialization
{
    bool attempted = false, resourceUnavailable = false;
    std::optional<V4IndependentFilePayload> payload;
    std::shared_ptr<const V4OwnedRelativePathBinding> relativeBinding;
    std::vector<std::size_t> physicalToLogical;
    std::string_view reason;
};

// Minted only by Child after its parent authenticated the original case,
// isolation/profile/source directory chain and the identical in-flight
// receipt. Holding this capability is not evidence of production dispatch.
class V4ExactOwnedFilesPermit final
{
    friend class ::drag_global::virtual_child_protocol::Child;
    std::shared_ptr<AttemptLineage> lineage_;
    std::string caseId_, executionId_;
    std::filesystem::path sourcePath_;
    std::array<HANDLE, 3> pins_{INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
    std::array<FILE_ID_INFO, 3> ids_{};
    HANDLE creationRoot_ = INVALID_HANDLE_VALUE;
    HANDLE parentProcess_ = INVALID_HANDLE_VALUE;
    std::array<HANDLE, 4> ledgerPins_{INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
    std::array<FILE_ID_INFO, 4> ledgerIds_{};
    std::string ledgerPreBytes_, ledgerHeadBytes_;
    bool parentLedgerBound_ = false;
    DWORD thread_ = 0;
    std::atomic_bool used_{false};
    bool ready_ = false;

    static bool SameId(const FILE_ID_INFO& a, const FILE_ID_INFO& b) noexcept
    {
        return a.VolumeSerialNumber == b.VolumeSerialNumber &&
            std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) == 0 &&
            std::any_of(std::begin(a.FileId.Identifier), std::end(a.FileId.Identifier), [](BYTE v) { return v != 0; });
    }
    static bool DirectoryMatches(HANDLE handle, const FILE_ID_INFO& expected) noexcept
    {
        FILE_ID_INFO actual{}; FILE_ATTRIBUTE_TAG_INFO tag{};
        return handle && handle != INVALID_HANDLE_VALUE && GetFileType(handle) == FILE_TYPE_DISK &&
            GetFileInformationByHandleEx(handle, FileIdInfo, &actual, sizeof(actual)) && SameId(actual, expected) &&
            GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) &&
            (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    }
    static bool BytesMatch(HANDLE handle, const FILE_ID_INFO& expected, std::string_view bytes) noexcept
    {
        const HANDLE file = ReOpenFile(handle, GENERIC_READ, FILE_SHARE_READ, FILE_FLAG_OPEN_REPARSE_POINT);
        if (file == INVALID_HANDLE_VALUE) return false;
        struct Guard { HANDLE h; ~Guard() { CloseHandle(h); } } guard{file};
        FILE_ID_INFO id{}; FILE_ATTRIBUTE_TAG_INFO tag{}; FILE_BASIC_INFO before{}, after{}; LARGE_INTEGER size{}, end{};
        if (!GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id)) || !SameId(id, expected) ||
            !GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !GetFileInformationByHandleEx(file, FileBasicInfo, &before, sizeof(before)) ||
            !GetFileSizeEx(file, &size) || size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) != bytes.size()) return false;
        std::size_t offset = 0; char buffer[8192];
        while (offset < bytes.size())
        {
            DWORD got = 0; const auto n = static_cast<DWORD>((std::min)(sizeof(buffer), bytes.size() - offset));
            if (!ReadFile(file, buffer, n, &got, nullptr) || !got || std::memcmp(buffer, bytes.data() + offset, got)) return false;
            offset += got;
        }
        return GetFileSizeEx(file, &end) && size.QuadPart == end.QuadPart &&
            GetFileInformationByHandleEx(file, FileBasicInfo, &after, sizeof(after)) &&
            before.LastWriteTime.QuadPart == after.LastWriteTime.QuadPart && before.ChangeTime.QuadPart == after.ChangeTime.QuadPart;
    }
    static std::wstring HandlePath(HANDLE handle)
    {
        constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_GUID;
        const DWORD size = GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
        if (!size || size > 32768) return {};
        std::wstring path(size, L'\0');
        const DWORD written = GetFinalPathNameByHandleW(handle, path.data(), size, flags);
        if (!written || written >= size) return {};
        path.resize(written); return path;
    }
    static bool DirectChild(std::wstring_view parent, std::wstring_view child) noexcept
    {
        return !parent.empty() && child.size() > parent.size() + 1 && child.substr(0, parent.size()) == parent &&
            child[parent.size()] == L'\\' && child.find(L'\\', parent.size() + 1) == std::wstring_view::npos;
    }
    bool RootsLive() const
    {
        for (std::size_t i = 0; i < pins_.size(); ++i)
            if (!DirectoryMatches(pins_[i], ids_[i])) return false;
        if (!DirectoryMatches(creationRoot_, ids_[2])) return false;
        const auto isolation = HandlePath(pins_[0]), profile = HandlePath(pins_[1]), source = HandlePath(pins_[2]);
        return DirectChild(isolation, profile) && DirectChild(profile, source);
    }
    V4ExactOwnedFilesPermit(const InFlightReceipt& receipt, std::string caseId,
        const std::array<HANDLE, 3>& directories, const std::array<FILE_ID_INFO, 3>& ids,
        std::filesystem::path sourcePath, HANDLE parentProcess)
        : lineage_(receipt.lineage_), caseId_(std::move(caseId)), executionId_(receipt.executionId_),
          sourcePath_(std::move(sourcePath)), ids_(ids), thread_(GetCurrentThreadId())
    {
        try
        {
        if (receipt.version_ != FamilyVersion::v4 || receipt.ownerThread_ != thread_ || !lineage_ ||
            caseId_.empty() || executionId_.empty() || sourcePath_.empty()) return;
        for (std::size_t i = 0; i < pins_.size(); ++i)
        {
            if (!DirectoryMatches(directories[i], ids_[i])) return;
            pins_[i] = ReOpenFile(directories[i], FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
            if (!DirectoryMatches(pins_[i], ids_[i])) return;
        }
        if (!DuplicateHandle(GetCurrentProcess(), directories[2], GetCurrentProcess(), &creationRoot_,
            0, FALSE, DUPLICATE_SAME_ACCESS) || !parentProcess || parentProcess == INVALID_HANDLE_VALUE ||
            !DuplicateHandle(GetCurrentProcess(), parentProcess, GetCurrentProcess(), &parentProcess_,
                SYNCHRONIZE, FALSE, 0) || WaitForSingleObject(parentProcess_, 0) != WAIT_TIMEOUT || !RootsLive()) return;
        ready_ = true;
        }
        catch (...) { ready_ = false; }
    }
    bool Ready() const noexcept { return ready_; }
    bool ParentLedgerLive() const noexcept;
    bool BindVerifiedParentLedger(InFlightReceipt& receipt, const std::array<HANDLE, 4>& handles,
        const std::array<FILE_ID_INFO, 4>& ids, std::string parentPreBytes, std::string parentHeadBytes,
        std::wstring previousName, std::string childPreBytes, std::string childFlightBytes, std::string_view parentBinding,
        const V4CoordinateBudget&, const V4AxisBudget&, std::size_t tokenBudget, std::size_t caseByteBudget,
        std::size_t replayWorkBudget, std::size_t childByteBudget, std::size_t checkpointByteBudget) noexcept;
public:
    V4ExactOwnedFilesPermit(const V4ExactOwnedFilesPermit&) = delete;
    V4ExactOwnedFilesPermit& operator=(const V4ExactOwnedFilesPermit&) = delete;
    ~V4ExactOwnedFilesPermit()
    {
        if (creationRoot_ && creationRoot_ != INVALID_HANDLE_VALUE) CloseHandle(creationRoot_);
        if (parentProcess_ && parentProcess_ != INVALID_HANDLE_VALUE) CloseHandle(parentProcess_);
        for (auto handle : pins_) if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        for (auto handle : ledgerPins_) if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
    V4ExactOwnedFilesMaterialization Materialize(InFlightReceipt& receipt,
        const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
        std::size_t tokenBudget, std::size_t caseByteBudget, std::size_t maxFiles, std::size_t maxBytes) noexcept
    {
        V4ExactOwnedFilesMaterialization out;
        try
        {
            FILE_ID_INFO flight{};
            if (!ready_ || !ParentLedgerLive() || parentProcess_ == INVALID_HANDLE_VALUE || WaitForSingleObject(parentProcess_, 0) != WAIT_TIMEOUT ||
                GetCurrentThreadId() != thread_ || receipt.ownerThread_ != thread_ ||
                receipt.version_ != FamilyVersion::v4 || !receipt.lineage_ || receipt.lineage_.get() != lineage_.get() ||
                receipt.lineage_.owner_before(lineage_) || lineage_.owner_before(receipt.lineage_) ||
                receipt.executionId_ != executionId_ || receipt.consumed_.load(std::memory_order_acquire) ||
                lineage_->sealed.load(std::memory_order_acquire) || lineage_->commitStarted.load(std::memory_order_acquire) ||
                receipt.pinned_ == INVALID_HANDLE_VALUE ||
                !GetFileInformationByHandleEx(receipt.pinned_, FileIdInfo, &flight, sizeof(flight)) ||
                !SameId(flight, receipt.publishedId_) || !RootsLive())
            { out.reason = "v4_exact_owned_permit_identity_unavailable"; return out; }
            if (!receipt.TryBeginExecution(FamilyVersion::v4))
            { out.reason = "v4_exact_owned_permit_execution_busy"; return out; }
            const auto release = [](InFlightReceipt* held) noexcept { held->EndExecution(); };
            std::unique_ptr<InFlightReceipt, decltype(release)> guard(&receipt, release);
            if (receipt.consumed_.load(std::memory_order_acquire) ||
                lineage_->sealed.load(std::memory_order_acquire) || lineage_->commitStarted.load(std::memory_order_acquire) ||
                WaitForSingleObject(parentProcess_, 0) != WAIT_TIMEOUT || !RootsLive() || !ParentLedgerLive())
            { out.reason = "v4_exact_owned_permit_revalidation_unavailable"; return out; }
            const auto plan = PrepareV4ExactOwnedFiles(caseId_, coordinateBudget, axisBudget, tokenBudget,
                caseByteBudget, sourcePath_, creationRoot_, ids_[2], maxFiles, maxBytes);
            if (plan.status != V4ExactOwnedFilesStatus::planned || !plan.replay.family || !plan.replay.candidate)
            { out.resourceUnavailable = plan.status == V4ExactOwnedFilesStatus::resource_unavailable; out.reason = plan.reason; return out; }
            const auto identity = event_graph::V4FamilyIdentityContext::Create(*plan.replay.family, axisBudget);
            const auto verified = V4FamilyValidatedContext::Create(*plan.replay.family, axisBudget, *plan.replay.candidate);
            if (identity.Status() == V4FamilyStatus::resource_unavailable || verified.Status() == V4CoordinateStatus::resource_unavailable)
            { out.resourceUnavailable = true; out.reason = "v4_exact_owned_permit_context_resource_unavailable"; return out; }
            if (identity.Status() != V4FamilyStatus::schema_only || verified.Status() != V4CoordinateStatus::coordinate_ready ||
                identity.FamilyBinding() != plan.replay.familyBinding || !RootsLive() ||
                WaitForSingleObject(parentProcess_, 0) != WAIT_TIMEOUT || !ParentLedgerLive())
            { out.reason = "v4_exact_owned_permit_context_unavailable"; return out; }
            out.relativeBinding = plan.relativeBinding; out.physicalToLogical = plan.physicalToLogical;
            V4RelativePathMaterializationKey relativeKey(out.relativeBinding);
            bool expected = false;
            const std::function<bool()> parentCheck = [this] { return ParentLedgerLive(); };
            if (!used_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            { out.reason = "v4_exact_owned_permit_already_used"; return out; }
            out.attempted = true;
            // Move construction is noexcept; even a partial failure retains
            // every handle and creation/identity flag returned by the fixture.
            out.payload.emplace(PrepareV4IndependentFilePayload(verified, identity, sourcePath_,
                creationRoot_, ids_[2], maxFiles, maxBytes, parentProcess_, &parentCheck, out.relativeBinding,
                out.relativeBinding ? &relativeKey : nullptr));
            out.resourceUnavailable = out.payload->status == V4IndependentFileStatus::resource_unavailable;
            out.reason = out.payload->reason;
            return out;
        }
        catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_exact_owned_permit_resource_unavailable"; return out; }
        catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_exact_owned_permit_resource_unavailable"; return out; }
        catch (...) { out.reason = "v4_exact_owned_permit_observation_unavailable"; return out; }
    }
};

inline V4CaseReplay ReplayV4DirectSeed(const FamilyState& family, const CaseRecord& saved,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget)
{
    V4CaseReplay result;
    try
    {
        if (family.version != FamilyVersion::v4 || family.encodingHole || !saved.directSeed ||
            (saved.graphMode != "v4_ole" && saved.graphMode != "v4_native") ||
            saved.kind != "graph_hypothesis" || saved.graphId.empty() ||
            !saved.beforeCursor.empty() || saved.replayWork || !saved.candidateOrdinal.empty() || saved.reason != "unexecuted")
        { result.errors.emplace_back("v4_direct_seed_identity_invalid"); return result; }
        const auto decoded = NextV4FamilyCoordinate(family.coordinate, coordinateBudget);
        if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family)
        { result.status = decoded.status == V4FamilyCoordinateStatus::resource_unavailable ? V4CaseReplay::Status::resource_unavailable : V4CaseReplay::Status::unavailable;
          result.errors.emplace_back("v4_direct_seed_family_unavailable"); return result; }
        Candidate input; input.values = saved.candidateValues;
        const auto identity = event_graph::V4FamilyIdentityContext::Create(*decoded.family, axisBudget);
        const auto verified = V4FamilyValidatedContext::Create(*decoded.family, axisBudget, input);
        if (identity.Status() != V4FamilyStatus::schema_only || verified.Status() != V4CoordinateStatus::coordinate_ready)
        { result.status = identity.Status() == V4FamilyStatus::resource_unavailable || verified.Status() == V4CoordinateStatus::resource_unavailable ?
              V4CaseReplay::Status::resource_unavailable : V4CaseReplay::Status::unavailable;
          result.errors.emplace_back("v4_direct_seed_coordinate_unavailable"); return result; }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id != saved.candidateId)
        { if (raw.resourceUnavailable) result.status = V4CaseReplay::Status::resource_unavailable;
          result.errors.emplace_back("v4_direct_seed_candidate_mismatch"); return result; }
        std::vector<bool> present;
        for (char bit : saved.presence)
        { if (bit != '0' && bit != '1') { result.errors.emplace_back("v4_direct_seed_presence_invalid"); return result; } present.push_back(bit == '1'); }
        event_graph::V4FamilyCandidateRecord record;
        record.kind = "graph_hypothesis"; record.candidateId = raw.id; record.reason = "unexecuted";
        record.coordinate = verified.Coordinate();
        if (saved.graphMode == "v4_ole")
        {
            const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, saved.graphId);
            if (!replay.identityValid || !replay.errors.empty() || replay.variant.id != saved.graphId || replay.variant.unresolved != saved.unresolved)
            { result.status = replay.status == event_graph::V4GraphReplayStatus::resource_unavailable ?
                  V4CaseReplay::Status::resource_unavailable : V4CaseReplay::Status::unavailable;
              result.errors = replay.errors; result.errors.emplace_back("v4_direct_seed_graph_unavailable"); return result; }
            record.unresolved = replay.variant.unresolved;
            record.graph = event_graph::V4JoinedGraphItem{"", "", present, replay.variant};
        }
        else
        {
            const auto replay = event_graph::V4NativeJoinedGraph::Replay(identity, verified, present, saved.graphId);
            if (!replay.identityValid || !replay.errors.empty() || replay.variant.id != saved.graphId || replay.variant.unresolved != saved.unresolved)
            { result.status = replay.status == event_graph::V4GraphReplayStatus::resource_unavailable ?
                  V4CaseReplay::Status::resource_unavailable : V4CaseReplay::Status::unavailable;
              result.errors = replay.errors; result.errors.emplace_back("v4_direct_seed_native_graph_unavailable"); return result; }
            record.unresolved = replay.variant.unresolved;
            record.nativeGraph = event_graph::V4NativeJoinedGraphItem{"", "", present, replay.variant};
        }
        result.unexecutedRecord = std::move(record);
        result.status = V4CaseReplay::Status::ready;
        return result;
    }
    catch (const std::bad_alloc&) { result.status = V4CaseReplay::Status::resource_unavailable; result.errors.emplace_back("v4_direct_seed_resource_unavailable"); return result; }
    catch (const std::length_error&) { result.status = V4CaseReplay::Status::resource_unavailable; result.errors.emplace_back("v4_direct_seed_resource_unavailable"); return result; }
    catch (...) { result.status = V4CaseReplay::Status::unavailable; result.errors.emplace_back("v4_direct_seed_observation_unavailable"); return result; }
}

struct V4DirectSeedProposal
{
    std::optional<State> proposed; // publish the whole state before use
    std::optional<std::size_t> caseIndex;
    std::string_view reason;
};

// Direct replay is a separate, unexecuted record: it never fabricates a DFS
// cursor or claims that earlier combinations were scanned. Existing records
// are left intact, and the usual checkpoint transaction remains mandatory.
inline V4DirectSeedProposal PrepareV4DirectSeed(const State& original, std::string_view binding,
    std::size_t familyIndex, const Candidate& candidate, const std::vector<bool>& present,
    std::string_view graphId, const V4CoordinateBudget& coordinateBudget,
    const V4AxisBudget& axisBudget, std::size_t checkpointByteBudget)
{
    V4DirectSeedProposal result;
    try
    {
        const auto initial = EncodeCheckpoint(original, binding, checkpointByteBudget);
        if (initial.status != CodecStatus::encoded || original.familyVersion != FamilyVersion::v4 || !original.v4TypedEnabled ||
            familyIndex >= original.families.size())
        { result.reason = "v4_direct_seed_state_unavailable"; return result; }
        const auto& family = original.families[familyIndex];
        if (family.encodingHole || family.duplicateSeed || graphId.empty())
        { result.reason = "v4_direct_seed_family_unavailable"; return result; }
        const auto decoded = NextV4FamilyCoordinate(family.coordinate, coordinateBudget);
        if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family || decoded.coordinate != family.coordinate)
        { result.reason = "v4_direct_seed_family_decode_unavailable"; return result; }
        const auto identity = event_graph::V4FamilyIdentityContext::Create(*decoded.family, axisBudget);
        const auto verified = V4FamilyValidatedContext::Create(*decoded.family, axisBudget, candidate);
        if (identity.Status() != V4FamilyStatus::schema_only || verified.Status() != V4CoordinateStatus::coordinate_ready)
        { result.reason = "v4_direct_seed_coordinate_unavailable"; return result; }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty())
        { result.reason = "v4_direct_seed_candidate_unavailable"; return result; }
        const auto graph = event_graph::V4JoinedGraph::Replay(identity, verified, present, graphId);
        if (!graph.identityValid || !graph.errors.empty() || graph.variant.id != graphId)
        { result.reason = "v4_direct_seed_graph_unavailable"; return result; }
        CaseRecord item; item.directSeed = true; item.graphMode = "v4_ole";
        item.kind = "graph_hypothesis"; item.reason = "unexecuted";
        item.candidateId = raw.id; item.graphId = graph.variant.id;
        item.candidateValues = verified.Coordinate().values; item.unresolved = graph.variant.unresolved;
        for (const bool bit : present) item.presence += bit ? '1' : '0';
        for (const auto& existing : family.cases)
            if (existing.candidateId == item.candidateId && existing.graphMode == item.graphMode &&
                existing.graphId == item.graphId && existing.presence == item.presence)
            {
                // Checkpoint records are caller data. Only a replayed direct
                // seed can establish equality here; ordinary DFS records
                // require an explicit bounded replay, which this API lacks.
                if (!existing.directSeed)
                { result.reason = "v4_direct_seed_existing_dfs_replay_unavailable"; return result; }
                const auto replay = ReplayV4DirectSeed(family, existing, coordinateBudget, axisBudget);
                if (!replay.unexecutedRecord || !replay.errors.empty())
                { result.reason = "v4_direct_seed_existing_replay_unavailable"; return result; }
                result.reason = "v4_direct_seed_already_recorded"; return result;
            }
        State proposed = original;
        auto& cases = proposed.families[familyIndex].cases;
        result.caseIndex = cases.size(); cases.push_back(std::move(item));
        if (EncodeCheckpoint(proposed, binding, checkpointByteBudget).status != CodecStatus::encoded)
        { result.caseIndex.reset(); result.reason = "v4_direct_seed_checkpoint_unavailable"; return result; }
        result.proposed = std::move(proposed);
        result.reason = "v4_direct_seed_unexecuted";
        return result;
    }
    catch (...) { result.caseIndex.reset(); result.proposed.reset(); result.reason = "v4_direct_seed_resource_or_observation_unavailable"; return result; }
}

// Pure proposal: the caller must publish the ENTIRE proposed state (including
// every new case record and cursor) as one checkpoint before accepting it.
// No production entry point is invoked here. Failed/unavailable traversal
// leaves that family's cursor unchanged and is retried in a future visit.
inline FamilyVisitProposal PrepareFamilyVisit(const State& original, std::string_view binding,
    std::size_t decimalDigitBudget, std::size_t memberBudget, std::size_t workBudget,
    const V4CoordinateBudget* v4Budget = nullptr, const V4AxisBudget* v4AxisBudget = nullptr,
    std::optional<std::size_t> requestedFamily = std::nullopt,
    std::size_t checkpointByteBudget = (std::numeric_limits<std::size_t>::max)())
{
    FamilyVisitProposal result;
    try
    {
        if (requestedFamily)
        {
            const auto check = EncodeCheckpoint(original, binding, checkpointByteBudget);
            if (check.status != CodecStatus::encoded || original.familyVersion != FamilyVersion::v4 ||
                !original.v4TypedEnabled || *requestedFamily >= original.families.size())
            { result.tick.kind = TickKind::unavailable; result.tick.reason = check.status == CodecStatus::resource_unavailable ?
                "v4_owned_visit_resource_unavailable" : "v4_owned_visit_state_unavailable"; return result; }
            for (const auto& family : original.families)
                if (family.state == WorkState::in_flight || std::any_of(family.cases.begin(), family.cases.end(),
                    [](const auto& item) { return item.state == WorkState::in_flight; }))
                { result.tick.kind = TickKind::unavailable; result.tick.reason = "v4_owned_visit_inflight_unavailable"; return result; }
            result.tick.proposed = original; result.tick.kind = TickKind::visit; result.tick.familyIndex = *requestedFamily;
        }
        else result.tick = PrepareTick(original, binding, decimalDigitBudget, v4Budget);
        if (result.tick.kind != TickKind::visit || !result.tick.familyIndex) return result;
        auto& family = result.tick.proposed.families[*result.tick.familyIndex];
        if (family.discoverySeed || family.duplicateSeed)
        {
            if (family.version != FamilyVersion::v4 || !v4Budget)
            { family.state = WorkState::resource_unavailable; result.errors.emplace_back("v4_seed_validation_budget_unavailable"); return result; }
            const auto seed = EncodeV4FamilyCoordinate(V4FirstDiscoverySeed(), *v4Budget);
            if (seed.status == V4FamilyCoordinateStatus::resource_unavailable)
            { family.state = WorkState::resource_unavailable; result.errors.emplace_back(seed.reason); return result; }
            if (seed.status != V4FamilyCoordinateStatus::ready || seed.coordinate != family.coordinate)
            { result.tick.kind = TickKind::invalid_state; result.errors.emplace_back("v4_seed_identity_mismatch"); return result; }
            if (family.duplicateSeed) return result;
        }
        if (family.encodingHole)
        {
            // Checkpoint bytes are not authority: never trust a persisted
            // hole flag to skip a potentially valid family coordinate.
            if (family.version != FamilyVersion::v4 || !v4Budget)
            { result.tick.kind = TickKind::invalid_state; result.errors.emplace_back("v4_scheduler_hole_validation_unavailable"); return result; }
            const auto decoded = NextV4FamilyCoordinate(family.coordinate, *v4Budget);
            if (decoded.status == V4FamilyCoordinateStatus::resource_unavailable)
            { family.state = WorkState::resource_unavailable; result.errors.emplace_back(decoded.reason); return result; }
            if (decoded.status != V4FamilyCoordinateStatus::encoding_hole || decoded.coordinate != family.coordinate)
            { result.tick.kind = TickKind::invalid_state; result.errors.emplace_back("v4_scheduler_hole_mismatch"); }
            return result;
        }
        const auto unavailable = [&](event_graph::V3FamilyTraversalStatus status, const std::vector<std::string>& errors) {
            result.errors = errors;
            switch (status)
            {
            case event_graph::V3FamilyTraversalStatus::resource_unavailable: family.state = WorkState::resource_unavailable; break;
            case event_graph::V3FamilyTraversalStatus::schema_error: family.state = WorkState::schema_unavailable; break;
            case event_graph::V3FamilyTraversalStatus::unavailable: family.state = WorkState::observation_unavailable; break;
            default: result.tick.kind = TickKind::invalid_state; break;
            }
        };
        if (family.cases.empty())
        {
            if (family.nextRetry != 0) { result.tick.kind = TickKind::invalid_state; return result; }
        }
        else
        {
            if (family.nextRetry >= family.cases.size()) { result.tick.kind = TickKind::invalid_state; return result; }
            const auto count = (std::min)(std::size_t{2}, family.cases.size());
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto index = family.nextRetry;
                family.nextRetry = index == family.cases.size() - 1 ? 0 : index + 1;
                const auto status = family.cases[index].state;
                if (status == WorkState::pending || status == WorkState::resource_unavailable)
                    result.retryIndices.push_back(index);
            }
        }
        if (family.scanExhausted)
        {
            // The cached flag is not authority. A resumed state may advance
            // past this family only after its versioned cursor validates and
            // itself reports exhaustion. Existing pending cases remain so.
            if (family.candidateCursor.empty())
            { result.tick.kind = TickKind::invalid_state; result.errors.emplace_back("scheduler_exhausted_cursor_missing"); return result; }
            if (family.version == FamilyVersion::v4)
            {
                if (!v4Budget || !v4AxisBudget)
                { family.state = WorkState::resource_unavailable; result.errors.emplace_back("v4_family_budgets_unavailable"); return result; }
                const auto decoded = NextV4FamilyCoordinate(family.coordinate, *v4Budget);
                if (decoded.status == V4FamilyCoordinateStatus::resource_unavailable)
                { family.state = WorkState::resource_unavailable; result.errors.emplace_back(decoded.reason); return result; }
                if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family)
                { result.tick.kind = TickKind::invalid_state; result.errors.emplace_back("v4_scheduler_family_decode_invalid"); return result; }
                const auto parsed = event_graph::V4FamilyCandidateGraph::ParseCursor(*decoded.family, *v4AxisBudget, family.candidateCursor);
                if (parsed.status != event_graph::V4FamilyTraversalStatus::ready || !parsed.errors.empty())
                {
                    result.errors = parsed.errors;
                    if (parsed.status == event_graph::V4FamilyTraversalStatus::resource_unavailable) family.state = WorkState::resource_unavailable;
                    else if (parsed.status == event_graph::V4FamilyTraversalStatus::schema_error) family.state = WorkState::schema_unavailable;
                    else if (parsed.status == event_graph::V4FamilyTraversalStatus::unavailable) family.state = WorkState::observation_unavailable;
                    else result.tick.kind = TickKind::invalid_state;
                    return result;
                }
                if (!parsed.cursor.exhausted)
                { result.tick.kind = TickKind::invalid_state; result.errors.emplace_back("v4_scheduler_exhaustion_mismatch"); }
                return result;
            }
            const auto decoded = NextV3FamilyCoordinate(family.coordinate, decimalDigitBudget);
            if (decoded.status == V3FamilyCoordinateStatus::resource_unavailable)
            { family.state = WorkState::resource_unavailable; result.errors.emplace_back(decoded.reason); return result; }
            if (decoded.status != V3FamilyCoordinateStatus::ready || !decoded.family)
            { result.tick.kind = TickKind::invalid_state; result.errors.emplace_back("v3_scheduler_family_decode_invalid"); return result; }
            const auto parsed = event_graph::V3FamilyCandidateGraph::ParseCursor(*decoded.family, memberBudget, family.candidateCursor);
            if (parsed.status != event_graph::V3FamilyTraversalStatus::ready || !parsed.errors.empty())
            { unavailable(parsed.status, parsed.errors); return result; }
            if (!parsed.cursor.exhausted)
            { result.tick.kind = TickKind::invalid_state; result.errors.emplace_back("v3_scheduler_exhaustion_mismatch"); }
            return result;
        }
        if (family.version == FamilyVersion::v4)
        {
            if (!v4Budget || !v4AxisBudget)
            { family.state = WorkState::resource_unavailable; result.errors.emplace_back("v4_family_budgets_unavailable"); return result; }
            const auto decoded = NextV4FamilyCoordinate(family.coordinate, *v4Budget);
            if (decoded.status == V4FamilyCoordinateStatus::resource_unavailable)
            { family.state = WorkState::resource_unavailable; result.errors.emplace_back(decoded.reason); return result; }
            if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family || decoded.coordinate != family.coordinate)
            { result.tick.kind = TickKind::invalid_state; result.errors.emplace_back("v4_scheduler_family_decode_invalid"); return result; }
            const auto& descriptor = *decoded.family;
            const auto unavailableV4 = [&](event_graph::V4FamilyTraversalStatus status, const std::vector<std::string>& errors) {
                result.errors = errors;
                switch (status)
                {
                case event_graph::V4FamilyTraversalStatus::resource_unavailable: family.state = WorkState::resource_unavailable; break;
                case event_graph::V4FamilyTraversalStatus::schema_error: family.state = WorkState::schema_unavailable; break;
                case event_graph::V4FamilyTraversalStatus::unavailable: family.state = WorkState::observation_unavailable; break;
                default: result.tick.kind = TickKind::invalid_state; break;
                }
            };
            event_graph::V4FamilyCandidateCursor cursor;
            if (!family.candidateCursor.empty())
            {
                const auto parsed = event_graph::V4FamilyCandidateGraph::ParseCursor(descriptor, *v4AxisBudget, family.candidateCursor);
                if (parsed.status != event_graph::V4FamilyTraversalStatus::ready || !parsed.errors.empty())
                { unavailableV4(parsed.status, parsed.errors); result.errors.emplace_back("v4_scheduler_cursor_unavailable"); return result; }
                cursor = parsed.cursor;
            }
            const auto before = event_graph::V4FamilyCandidateGraph::SerializeCursor(descriptor, *v4AxisBudget, cursor);
            if (before.status != event_graph::V4FamilyTraversalStatus::ready || !before.errors.empty())
            { unavailableV4(before.status, before.errors); return result; }
            const auto step = event_graph::V4FamilyCandidateGraph::Step(descriptor, *v4AxisBudget, before.cursor, workBudget);
            if (step.status != event_graph::V4FamilyTraversalStatus::ready || !step.errors.empty())
            { unavailableV4(step.status, step.errors); return result; }
            const auto after = event_graph::V4FamilyCandidateGraph::SerializeCursor(descriptor, *v4AxisBudget, step.cursor);
            if (after.status != event_graph::V4FamilyTraversalStatus::ready || !after.errors.empty())
            { unavailableV4(after.status, after.errors); return result; }
            if (!step.presencePruned.empty() || !step.activePruned.empty())
            {
                if (!step.workUsed) { result.tick.kind = TickKind::invalid_state; result.errors.emplace_back("v4_scheduler_prune_step_invalid"); return result; }
                PruneAuditStep audit; audit.beforeCursor = before.encoded; audit.afterCursor = after.encoded; audit.workUsed = step.workUsed;
                for (const auto& prune : step.presencePruned) audit.presence.push_back({prune.candidateOrdinal, prune.candidateId, prune.prune});
                for (const auto& prune : step.activePruned) audit.active.push_back({prune.candidateOrdinal, prune.candidateId, prune.prune});
                family.pruneSteps.push_back(std::move(audit));
            }
            if (step.record)
            {
                const auto& record = *step.record;
                if (!record.coordinate || record.candidateId.empty() || !step.workUsed ||
                    (record.kind == "graph_hypothesis" && (static_cast<bool>(record.graph) == static_cast<bool>(record.nativeGraph))) ||
                    (record.kind != "graph_hypothesis" && (record.graph || record.nativeGraph)) ||
                    (record.kind != "graph_hypothesis" && record.kind != "mapping_unimplemented" && record.kind != "no_graph_hypothesis"))
                { result.tick.kind = TickKind::invalid_state; result.errors.emplace_back("v4_scheduler_record_invalid"); return result; }
                CaseRecord item;
                item.candidateId = record.candidateId; item.candidateOrdinal = record.candidateOrdinal;
                item.kind = record.kind; item.reason = record.reason; item.unresolved = record.unresolved;
                item.graphMode = record.nativeGraph ? "v4_native" : record.graph ? "v4_ole" : "v4_unknown";
                item.beforeCursor = before.encoded; item.replayWork = step.workUsed;
                if (record.graph)
                {
                    item.graphId = record.graph->graph.id;
                    for (const auto present : record.graph->present) item.presence += present ? '1' : '0';
                }
                if (record.nativeGraph)
                {
                    item.graphId = record.nativeGraph->graph.id;
                    for (const auto present : record.nativeGraph->present) item.presence += present ? '1' : '0';
                }
                const auto duplicate = std::find_if(family.cases.begin(), family.cases.end(), [&](const auto& existing) {
                    return record.kind == "graph_hypothesis" && existing.directSeed && existing.graphMode == item.graphMode &&
                        existing.candidateId == item.candidateId && existing.graphId == item.graphId &&
                        existing.presence == item.presence;
                });
                if (duplicate != family.cases.end())
                {
                    // Persisted checkpoint bytes are not authority. Revalidate
                    // the direct seed before suppressing the DFS record.
                    const auto replay = ReplayV4DirectSeed(family, *duplicate, *v4Budget, *v4AxisBudget);
                    if (!replay.unexecutedRecord || !replay.errors.empty())
                    {
                        result.tick.proposed = original;
                        result.tick.kind = TickKind::unavailable;
                        result.errors = replay.errors;
                        result.errors.emplace_back("v4_scheduler_direct_seed_revalidation_unavailable");
                        return result;
                    }
                    result.skippedSeedDuplicate = true;
                }
                else { family.cases.push_back(std::move(item)); result.v4UnexecutedRecord = record; }
            }
            family.candidateCursor = after.encoded; family.scanExhausted = step.exhausted;
            family.state = WorkState::pending;
            return result;
        }
        const auto decoded = NextV3FamilyCoordinate(family.coordinate, decimalDigitBudget);
        if (decoded.status == V3FamilyCoordinateStatus::resource_unavailable)
        { family.state = WorkState::resource_unavailable; result.errors.emplace_back(decoded.reason); return result; }
        if (decoded.status != V3FamilyCoordinateStatus::ready || !decoded.family || decoded.coordinate != family.coordinate)
        { result.tick.kind = TickKind::invalid_state; result.errors.emplace_back("v3_scheduler_family_decode_invalid"); return result; }
        const auto& descriptor = *decoded.family;
        event_graph::V3FamilyCandidateCursor cursor;
        if (!family.candidateCursor.empty())
        {
            const auto parsed = event_graph::V3FamilyCandidateGraph::ParseCursor(descriptor, memberBudget, family.candidateCursor);
            if (parsed.status != event_graph::V3FamilyTraversalStatus::ready || !parsed.errors.empty())
            {
                unavailable(parsed.status, parsed.errors);
                result.errors.emplace_back("v3_scheduler_cursor_unavailable");
                return result;
            }
            cursor = parsed.cursor;
        }
        const auto before = event_graph::V3FamilyCandidateGraph::SerializeCursor(descriptor, memberBudget, cursor);
        if (before.status != event_graph::V3FamilyTraversalStatus::ready || !before.errors.empty())
        { unavailable(before.status, before.errors); return result; }
        const auto step = event_graph::V3FamilyCandidateGraph::Step(descriptor, memberBudget, before.cursor, workBudget);
        if (step.status != event_graph::V3FamilyTraversalStatus::ready || !step.errors.empty())
        { unavailable(step.status, step.errors); return result; }
        const auto after = event_graph::V3FamilyCandidateGraph::SerializeCursor(descriptor, memberBudget, step.cursor);
        if (after.status != event_graph::V3FamilyTraversalStatus::ready || !after.errors.empty())
        { unavailable(after.status, after.errors); return result; }
        if (!step.presencePruned.empty() || !step.activePruned.empty())
        {
            if (!step.workUsed)
            { result.tick.kind = TickKind::invalid_state; result.errors.emplace_back("v3_scheduler_prune_step_invalid"); return result; }
            family.pruneSteps.push_back({before.encoded, after.encoded, step.workUsed,
                step.presencePruned, step.activePruned});
        }
        if (step.record)
        {
            const auto& record = *step.record;
            if (!record.coordinate || record.candidateId.empty() || !step.workUsed ||
                (record.kind == "graph_hypothesis" && !record.graph) ||
                (record.kind != "graph_hypothesis" && record.kind != "mapping_unimplemented" && record.kind != "no_graph_hypothesis"))
            { result.tick.kind = TickKind::invalid_state; result.errors.emplace_back("v3_scheduler_record_invalid"); return result; }
            CaseRecord item;
            item.candidateId = record.candidateId; item.candidateOrdinal = record.candidateOrdinal;
            item.kind = record.kind; item.reason = record.reason; item.unresolved = record.unresolved;
            item.beforeCursor = before.encoded; item.replayWork = step.workUsed;
            if (record.graph)
            {
                item.graphId = record.graph->graph.id;
                for (const auto present : record.graph->present) item.presence += present ? '1' : '0';
            }
            // Every emitted record is pending, including missing mappings.
            // It never asserts an executed case or a business result.
            family.cases.push_back(std::move(item));
            result.unexecutedRecord = record;
        }
        family.candidateCursor = after.encoded;
        family.scanExhausted = step.exhausted;
        family.state = WorkState::pending;
        return result;
    }
    catch (const std::bad_alloc&)
    { result = {}; result.tick.kind = TickKind::unavailable; result.tick.reason = "v3_scheduler_resource_unavailable"; return result; }
    catch (const std::length_error&)
    { result = {}; result.tick.kind = TickKind::unavailable; result.tick.reason = "v3_scheduler_resource_unavailable"; return result; }
}

enum class V4OwnedFamilyStatus { proposed, already_registered, resource_unavailable, unavailable, invalid };
struct V4OwnedFamilyRegistration
{
    V4OwnedFamilyStatus status = V4OwnedFamilyStatus::unavailable;
    std::optional<std::size_t> familyIndex;
    std::optional<State> proposed;
    std::string_view reason;
};
// Register a live root-bound family in the same scheduler table. This does not
// advance either the global DFS or the family cursor; the caller publishes the
// proposal atomically before allowing a visit or any production attempt.
inline V4OwnedFamilyRegistration PrepareV4OwnedFamilyRegistration(const State& original, std::string_view binding,
    const V4OwnedFileFamilySeed& seed, const V4CoordinateBudget& coordinateBudget,
    std::size_t checkpointByteBudget)
{
    V4OwnedFamilyRegistration out;
    try
    {
        const auto initial = EncodeCheckpoint(original, binding, checkpointByteBudget);
        if (initial.status != CodecStatus::encoded || original.familyVersion != FamilyVersion::v4 || !original.v4TypedEnabled)
        { out.status = initial.status == CodecStatus::resource_unavailable ? V4OwnedFamilyStatus::resource_unavailable : V4OwnedFamilyStatus::unavailable;
          out.reason = "v4_owned_family_checkpoint_unavailable"; return out; }
        if (!seed.StillOwned()) { out.reason = "v4_owned_family_root_unavailable"; return out; }
        const auto decoded = NextV4FamilyCoordinate(seed.Coordinate(), coordinateBudget);
        if (decoded.status == V4FamilyCoordinateStatus::resource_unavailable)
        { out.status = V4OwnedFamilyStatus::resource_unavailable; out.reason = decoded.reason; return out; }
        if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family || decoded.coordinate != seed.Coordinate() ||
            V4FamilyBinding(*decoded.family) != seed.Binding())
        { out.status = V4OwnedFamilyStatus::invalid; out.reason = "v4_owned_family_identity_invalid"; return out; }
        for (const auto& family : original.families)
            if (family.state == WorkState::in_flight || std::any_of(family.cases.begin(), family.cases.end(),
                [](const auto& item) { return item.state == WorkState::in_flight; }))
            { out.reason = "v4_owned_family_inflight_unavailable"; return out; }
        for (std::size_t i = 0; i < original.families.size(); ++i)
            if (original.families[i].coordinate == seed.Coordinate())
            {
                if (original.families[i].version != FamilyVersion::v4 || original.families[i].encodingHole || original.families[i].duplicateSeed)
                { out.status = V4OwnedFamilyStatus::invalid; out.reason = "v4_owned_family_existing_invalid"; return out; }
                if (!seed.StillOwned()) { out.reason = "v4_owned_family_root_changed"; return out; }
                out.status = V4OwnedFamilyStatus::already_registered; out.familyIndex = i; out.reason = "v4_owned_family_already_registered"; return out;
            }
        State next = original; FamilyState family; family.version = FamilyVersion::v4; family.coordinate = seed.Coordinate();
        out.familyIndex = next.families.size(); next.families.push_back(std::move(family));
        const auto encoded = EncodeCheckpoint(next, binding, checkpointByteBudget);
        if (encoded.status != CodecStatus::encoded || !seed.StillOwned())
        { out.familyIndex.reset(); out.status = encoded.status == CodecStatus::resource_unavailable ? V4OwnedFamilyStatus::resource_unavailable : V4OwnedFamilyStatus::unavailable;
          out.reason = "v4_owned_family_publication_unavailable"; return out; }
        out.proposed = std::move(next); out.status = V4OwnedFamilyStatus::proposed; out.reason = "v4_owned_family_unexecuted";
    }
    catch (const std::bad_alloc&) { out = {}; out.status = V4OwnedFamilyStatus::resource_unavailable; out.reason = "v4_owned_family_resource_unavailable"; }
    catch (const std::length_error&) { out = {}; out.status = V4OwnedFamilyStatus::resource_unavailable; out.reason = "v4_owned_family_resource_unavailable"; }
    catch (...) { out = {}; out.reason = "v4_owned_family_observation_unavailable"; }
    return out;
}

struct V4OwnedFamilyVisit
{
    V4OwnedFamilyStatus status = V4OwnedFamilyStatus::unavailable;
    std::optional<FamilyVisitProposal> proposal;
    std::string_view reason;
};
// Target one already registered family but reuse the same versioned
// candidate/joined-graph DFS, records, retry queue, and prune audits as the
// fair scheduler. Other family/global traversal state is not advanced.
inline V4OwnedFamilyVisit PrepareV4OwnedFamilyVisit(const State& original, std::string_view binding,
    const V4OwnedFileFamilySeed& seed, std::size_t familyIndex, const V4CoordinateBudget& coordinateBudget,
    const V4AxisBudget& axisBudget, std::size_t workBudget, std::size_t checkpointByteBudget)
{
    V4OwnedFamilyVisit out;
    try
    {
        if (!seed.StillOwned() || familyIndex >= original.families.size() ||
            original.families[familyIndex].version != FamilyVersion::v4 ||
            original.families[familyIndex].coordinate != seed.Coordinate())
        { out.reason = "v4_owned_visit_root_or_family_unavailable"; return out; }
        const auto decoded = NextV4FamilyCoordinate(seed.Coordinate(), coordinateBudget);
        if (decoded.status == V4FamilyCoordinateStatus::resource_unavailable)
        { out.status = V4OwnedFamilyStatus::resource_unavailable; out.reason = decoded.reason; return out; }
        if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family ||
            V4FamilyBinding(*decoded.family) != seed.Binding())
        { out.status = V4OwnedFamilyStatus::invalid; out.reason = "v4_owned_visit_family_identity_invalid"; return out; }
        auto visit = PrepareFamilyVisit(original, binding, coordinateBudget.decimalDigits, axisBudget.logicalMembers,
            workBudget, &coordinateBudget, &axisBudget, familyIndex, checkpointByteBudget);
        if (visit.tick.kind != TickKind::visit || !visit.tick.familyIndex || *visit.tick.familyIndex != familyIndex)
        { out.status = visit.tick.reason == "v4_owned_visit_resource_unavailable" || visit.tick.reason == "v3_scheduler_resource_unavailable" ?
            V4OwnedFamilyStatus::resource_unavailable : V4OwnedFamilyStatus::unavailable; out.reason = visit.tick.reason; return out; }
        const auto state = visit.tick.proposed.families[familyIndex].state;
        if (state == WorkState::resource_unavailable || state == WorkState::schema_unavailable || state == WorkState::observation_unavailable)
        { out.status = state == WorkState::resource_unavailable ? V4OwnedFamilyStatus::resource_unavailable : V4OwnedFamilyStatus::unavailable;
          out.reason = "v4_owned_visit_traversal_unavailable"; return out; }
        const auto encoded = EncodeCheckpoint(visit.tick.proposed, binding, checkpointByteBudget);
        if (encoded.status != CodecStatus::encoded || !seed.StillOwned())
        { out.status = encoded.status == CodecStatus::resource_unavailable ? V4OwnedFamilyStatus::resource_unavailable : V4OwnedFamilyStatus::unavailable;
          out.reason = "v4_owned_visit_publication_unavailable"; return out; }
        out.proposal = std::move(visit); out.status = V4OwnedFamilyStatus::proposed; out.reason = "v4_owned_visit_unexecuted";
    }
    catch (const std::bad_alloc&) { out = {}; out.status = V4OwnedFamilyStatus::resource_unavailable; out.reason = "v4_owned_visit_resource_unavailable"; }
    catch (const std::length_error&) { out = {}; out.status = V4OwnedFamilyStatus::resource_unavailable; out.reason = "v4_owned_visit_resource_unavailable"; }
    catch (...) { out = {}; out.reason = "v4_owned_visit_observation_unavailable"; }
    return out;
}

struct CaseReplay
{
    std::optional<event_graph::V3FamilyCandidateRecord> unexecutedRecord;
    std::vector<std::string> errors;
};
struct PruneReplay
{
    bool matched = false; // local traversal replay only, not product coverage
    bool resourceUnavailable = false;
    std::vector<std::string> errors;
};

inline PruneReplay ReplayPruneStep(const FamilyState& family, std::size_t auditIndex,
    std::size_t decimalDigitBudget, std::size_t memberBudget, std::size_t replayWorkBudget)
{
    PruneReplay result;
    try
    {
        if (family.version != FamilyVersion::v3 || family.encodingHole)
        { result.errors.emplace_back("v3_scheduler_prune_family_version_unavailable"); return result; }
        if (auditIndex >= family.pruneSteps.size()) { result.errors.emplace_back("v3_scheduler_prune_index_invalid"); return result; }
        const auto& saved = family.pruneSteps[auditIndex];
        if (!saved.workUsed || saved.workUsed > replayWorkBudget)
        { result.errors.emplace_back("v3_scheduler_prune_work_budget_unavailable"); return result; }
        const auto decoded = NextV3FamilyCoordinate(family.coordinate, decimalDigitBudget);
        if (decoded.status != V3FamilyCoordinateStatus::ready || !decoded.family)
        { result.errors.emplace_back("v3_scheduler_prune_family_unavailable"); return result; }
        const auto before = event_graph::V3FamilyCandidateGraph::ParseCursor(*decoded.family, memberBudget, saved.beforeCursor);
        if (before.status != event_graph::V3FamilyTraversalStatus::ready || !before.errors.empty())
        { result.errors = before.errors; result.errors.emplace_back("v3_scheduler_prune_before_unavailable"); return result; }
        const auto step = event_graph::V3FamilyCandidateGraph::Step(*decoded.family, memberBudget, before.cursor, saved.workUsed);
        if (step.status != event_graph::V3FamilyTraversalStatus::ready || !step.errors.empty() ||
            step.workUsed != saved.workUsed || step.presencePruned.size() != saved.presence.size() ||
            step.activePruned.size() != saved.active.size())
        { result.errors = step.errors; result.errors.emplace_back("v3_scheduler_prune_replay_unavailable"); return result; }
        const auto after = event_graph::V3FamilyCandidateGraph::SerializeCursor(*decoded.family, memberBudget, step.cursor);
        if (after.status != event_graph::V3FamilyTraversalStatus::ready || !after.errors.empty() || after.encoded != saved.afterCursor)
        { result.errors = after.errors; result.errors.emplace_back("v3_scheduler_prune_after_mismatch"); return result; }
        for (std::size_t i = 0; i < saved.presence.size(); ++i)
        {
            const auto& a = saved.presence[i]; const auto& b = step.presencePruned[i];
            if (a.candidateOrdinal != b.candidateOrdinal || a.candidateId != b.candidateId ||
                a.prune.prefix != b.prune.prefix || a.prune.reason != b.prune.reason ||
                a.prune.premise != b.prune.premise || a.prune.consequence != b.prune.consequence)
            { result.errors.emplace_back("v3_scheduler_presence_prune_mismatch"); return result; }
        }
        for (std::size_t i = 0; i < saved.active.size(); ++i)
        {
            const auto& a = saved.active[i]; const auto& b = step.activePruned[i];
            if (a.candidateOrdinal != b.candidateOrdinal || a.candidateId != b.candidateId ||
                a.prune.presenceOrdinal != b.prune.presenceOrdinal || a.prune.present != b.prune.present ||
                a.prune.prune.scope != b.prune.prune.scope || a.prune.prune.reason != b.prune.prune.reason ||
                a.prune.prune.partition != b.prune.prune.partition ||
                a.prune.prune.earlierRequirement != b.prune.prune.earlierRequirement ||
                a.prune.prune.currentRequirement != b.prune.prune.currentRequirement)
            { result.errors.emplace_back("v3_scheduler_active_prune_mismatch"); return result; }
        }
        result.matched = true;
        return result;
    }
    catch (const std::bad_alloc&) { result.errors.emplace_back("v3_scheduler_prune_resource_unavailable"); return result; }
    catch (const std::length_error&) { result.errors.emplace_back("v3_scheduler_prune_resource_unavailable"); return result; }
}

inline PruneReplay ReplayV4PruneStep(const FamilyState& family, std::size_t auditIndex,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget, std::size_t replayWorkBudget)
{
    PruneReplay result;
    try
    {
        if (family.version != FamilyVersion::v4 || family.encodingHole || auditIndex >= family.pruneSteps.size())
        { result.errors.emplace_back("v4_scheduler_prune_identity_unavailable"); return result; }
        const auto& saved = family.pruneSteps[auditIndex];
        if (!saved.workUsed || saved.workUsed > replayWorkBudget)
        { result.resourceUnavailable = saved.workUsed > replayWorkBudget; result.errors.emplace_back("v4_scheduler_prune_work_budget_unavailable"); return result; }
        const auto decoded = NextV4FamilyCoordinate(family.coordinate, coordinateBudget);
        if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family)
        { result.resourceUnavailable = decoded.status == V4FamilyCoordinateStatus::resource_unavailable; result.errors.emplace_back("v4_scheduler_prune_family_unavailable"); return result; }
        const auto before = event_graph::V4FamilyCandidateGraph::ParseCursor(*decoded.family, axisBudget, saved.beforeCursor);
        if (before.status != event_graph::V4FamilyTraversalStatus::ready || !before.errors.empty())
        { result.resourceUnavailable = before.status == event_graph::V4FamilyTraversalStatus::resource_unavailable; result.errors = before.errors; result.errors.emplace_back("v4_scheduler_prune_before_unavailable"); return result; }
        const auto step = event_graph::V4FamilyCandidateGraph::Step(*decoded.family, axisBudget, before.cursor, saved.workUsed);
        if (step.status != event_graph::V4FamilyTraversalStatus::ready || !step.errors.empty() ||
            step.workUsed != saved.workUsed || step.presencePruned.size() != saved.presence.size() ||
            step.activePruned.size() != saved.active.size())
        { result.resourceUnavailable = step.status == event_graph::V4FamilyTraversalStatus::resource_unavailable; result.errors = step.errors; result.errors.emplace_back("v4_scheduler_prune_replay_unavailable"); return result; }
        const auto after = event_graph::V4FamilyCandidateGraph::SerializeCursor(*decoded.family, axisBudget, step.cursor);
        if (after.status != event_graph::V4FamilyTraversalStatus::ready || !after.errors.empty() || after.encoded != saved.afterCursor)
        { result.resourceUnavailable = after.status == event_graph::V4FamilyTraversalStatus::resource_unavailable; result.errors = after.errors; result.errors.emplace_back("v4_scheduler_prune_after_mismatch"); return result; }
        for (std::size_t i = 0; i < saved.presence.size(); ++i)
        {
            const auto& a = saved.presence[i]; const auto& b = step.presencePruned[i];
            if (a.candidateOrdinal != b.candidateOrdinal || a.candidateId != b.candidateId ||
                a.prune.prefix != b.prune.prefix || a.prune.reason != b.prune.reason ||
                a.prune.premise != b.prune.premise || a.prune.consequence != b.prune.consequence)
            { result.errors.emplace_back("v4_scheduler_presence_prune_mismatch"); return result; }
        }
        for (std::size_t i = 0; i < saved.active.size(); ++i)
        {
            const auto& a = saved.active[i]; const auto& b = step.activePruned[i];
            if (a.candidateOrdinal != b.candidateOrdinal || a.candidateId != b.candidateId ||
                a.prune.presenceOrdinal != b.prune.presenceOrdinal || a.prune.present != b.prune.present ||
                a.prune.prune.scope != b.prune.prune.scope || a.prune.prune.reason != b.prune.prune.reason ||
                a.prune.prune.partition != b.prune.prune.partition ||
                a.prune.prune.earlierRequirement != b.prune.prune.earlierRequirement ||
                a.prune.prune.currentRequirement != b.prune.prune.currentRequirement)
            { result.errors.emplace_back("v4_scheduler_active_prune_mismatch"); return result; }
        }
        result.matched = true;
        return result;
    }
    catch (const std::bad_alloc&) { result.resourceUnavailable = true; return result; }
    catch (const std::length_error&) { result.resourceUnavailable = true; return result; }
}

// Reconstruct, then compare every persisted identity and graph-presence field.
// This authenticates neither the history of the checkpoint nor any product
// outcome. The returned coordinate and graph still require the production
// adapter's own preconditions and same-run completion/result observations.
inline CaseReplay ReplayCase(const FamilyState& family, std::size_t caseIndex,
    std::size_t decimalDigitBudget, std::size_t memberBudget, std::size_t replayWorkBudget)
{
    CaseReplay result;
    try
    {
        if (family.version != FamilyVersion::v3 || family.encodingHole)
        { result.errors.emplace_back("v3_scheduler_case_family_version_unavailable"); return result; }
        if (caseIndex >= family.cases.size()) { result.errors.emplace_back("v3_scheduler_case_index_invalid"); return result; }
        const auto& saved = family.cases[caseIndex];
        if (saved.state != WorkState::pending && saved.state != WorkState::resource_unavailable)
        { result.errors.emplace_back("v3_scheduler_case_not_eligible_for_automatic_retry"); return result; }
        if (!saved.replayWork || saved.replayWork > replayWorkBudget)
        { result.errors.emplace_back("v3_scheduler_replay_work_budget_unavailable"); return result; }
        const auto decoded = NextV3FamilyCoordinate(family.coordinate, decimalDigitBudget);
        if (decoded.status != V3FamilyCoordinateStatus::ready || !decoded.family)
        { result.errors.emplace_back("v3_scheduler_family_unavailable"); return result; }
        const auto parsed = event_graph::V3FamilyCandidateGraph::ParseCursor(*decoded.family, memberBudget, saved.beforeCursor);
        if (parsed.status != event_graph::V3FamilyTraversalStatus::ready || !parsed.errors.empty())
        { result.errors = parsed.errors; result.errors.emplace_back("v3_scheduler_replay_cursor_unavailable"); return result; }
        const auto replay = event_graph::V3FamilyCandidateGraph::Step(*decoded.family, memberBudget, parsed.cursor, saved.replayWork);
        if (replay.status != event_graph::V3FamilyTraversalStatus::ready || !replay.errors.empty() ||
            !replay.record || !replay.record->coordinate || replay.workUsed != saved.replayWork)
        { result.errors = replay.errors; result.errors.emplace_back("v3_scheduler_replay_record_unavailable"); return result; }
        const auto& record = *replay.record;
        std::string presence, graphId;
        if (record.graph)
        {
            graphId = record.graph->graph.id;
            for (const auto present : record.graph->present) presence += present ? '1' : '0';
        }
        if (record.kind != saved.kind || record.candidateId != saved.candidateId ||
            record.candidateOrdinal != saved.candidateOrdinal || record.reason != saved.reason ||
            record.unresolved != saved.unresolved || graphId != saved.graphId || presence != saved.presence)
        { result.errors.emplace_back("v3_scheduler_replay_identity_mismatch"); return result; }
        result.unexecutedRecord = record;
        return result;
    }
    catch (const std::bad_alloc&) { result.unexecutedRecord.reset(); result.errors.emplace_back("v3_scheduler_replay_resource_unavailable"); return result; }
    catch (const std::length_error&) { result.unexecutedRecord.reset(); result.errors.emplace_back("v3_scheduler_replay_resource_unavailable"); return result; }
}

inline V4CaseReplay ReplayV4Case(const FamilyState& family, std::size_t caseIndex,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget, std::size_t replayWorkBudget)
{
    V4CaseReplay result;
    try
    {
        if (family.version != FamilyVersion::v4 || family.encodingHole || caseIndex >= family.cases.size())
        { result.errors.emplace_back("v4_scheduler_case_identity_unavailable"); return result; }
        const auto& saved = family.cases[caseIndex];
        if (saved.state != WorkState::pending && saved.state != WorkState::resource_unavailable)
        { result.status = V4CaseReplay::Status::unavailable; result.errors.emplace_back("v4_scheduler_case_not_eligible_for_automatic_retry"); return result; }
        if (saved.directSeed) return ReplayV4DirectSeed(family, saved, coordinateBudget, axisBudget);
        if (!saved.replayWork) { result.errors.emplace_back("v4_scheduler_replay_work_invalid"); return result; }
        if (saved.replayWork > replayWorkBudget)
        { result.status = V4CaseReplay::Status::resource_unavailable; result.errors.emplace_back("v4_scheduler_replay_work_budget_unavailable"); return result; }
        const auto decoded = NextV4FamilyCoordinate(family.coordinate, coordinateBudget);
        if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family)
        { result.status = decoded.status == V4FamilyCoordinateStatus::resource_unavailable ? V4CaseReplay::Status::resource_unavailable : V4CaseReplay::Status::unavailable;
          result.errors.emplace_back("v4_scheduler_family_unavailable"); return result; }
        const auto parsed = event_graph::V4FamilyCandidateGraph::ParseCursor(*decoded.family, axisBudget, saved.beforeCursor);
        if (parsed.status != event_graph::V4FamilyTraversalStatus::ready || !parsed.errors.empty())
        { result.status = parsed.status == event_graph::V4FamilyTraversalStatus::resource_unavailable ? V4CaseReplay::Status::resource_unavailable : V4CaseReplay::Status::unavailable;
          result.errors = parsed.errors; result.errors.emplace_back("v4_scheduler_replay_cursor_unavailable"); return result; }
        const auto replay = event_graph::V4FamilyCandidateGraph::Step(*decoded.family, axisBudget, parsed.cursor, saved.replayWork);
        if (replay.status != event_graph::V4FamilyTraversalStatus::ready || !replay.errors.empty() ||
            !replay.record || !replay.record->coordinate || replay.workUsed != saved.replayWork)
        { result.status = replay.status == event_graph::V4FamilyTraversalStatus::resource_unavailable ? V4CaseReplay::Status::resource_unavailable : V4CaseReplay::Status::unavailable;
          result.errors = replay.errors; result.errors.emplace_back("v4_scheduler_replay_record_unavailable"); return result; }
        const auto& record = *replay.record;
        std::string presence, graphId;
        std::string_view mode = "v4_unknown";
        if (record.graph)
        {
            mode = "v4_ole";
            graphId = record.graph->graph.id;
            for (const auto present : record.graph->present) presence += present ? '1' : '0';
        }
        if (record.nativeGraph)
        {
            if (record.graph) { result.errors.emplace_back("v4_scheduler_replay_graph_mode_invalid"); return result; }
            mode = "v4_native";
            graphId = record.nativeGraph->graph.id;
            for (const auto present : record.nativeGraph->present) presence += present ? '1' : '0';
        }
        if (record.kind != saved.kind || record.candidateId != saved.candidateId ||
            record.candidateOrdinal != saved.candidateOrdinal || record.reason != saved.reason ||
            record.unresolved != saved.unresolved || mode != saved.graphMode || graphId != saved.graphId || presence != saved.presence)
        { result.errors.emplace_back("v4_scheduler_replay_identity_mismatch"); return result; }
        result.unexecutedRecord = record;
        result.status = V4CaseReplay::Status::ready;
        return result;
    }
    catch (const std::bad_alloc&) { result.status = V4CaseReplay::Status::resource_unavailable; result.unexecutedRecord.reset(); result.errors.emplace_back("v4_scheduler_replay_resource_unavailable"); return result; }
    catch (const std::length_error&) { result.status = V4CaseReplay::Status::resource_unavailable; result.unexecutedRecord.reset(); result.errors.emplace_back("v4_scheduler_replay_resource_unavailable"); return result; }
}

// A bounded, immutable single-case shard stored in the existing isolated
// CheckpointStore. A shard is not yet a global index or a coverage claim;
// its locator must be retained by a trusted head before it can be recovered.
struct V4CaseShardResult
{
    bool published = false, identityReplayed = false, resourceUnavailable = false;
    PublishedCheckpoint publication;
    std::optional<State> shard;
    std::string_view reason;
};
inline V4CaseShardResult ReadV4CaseShard(CheckpointStore& store, std::wstring_view name, const FILE_ID_INFO& fileId,
    std::string_view binding, const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
    std::size_t replayWorkBudget, std::size_t byteBudget)
{
    V4CaseShardResult out;
    try
    {
        const auto loaded = store.Read(name, fileId, binding, byteBudget);
        if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight)
        { out.resourceUnavailable = loaded.status == CodecStatus::resource_unavailable; out.reason = "v4_case_shard_read_unavailable"; return out; }
        const auto& state = loaded.state;
        if (state.familyVersion != FamilyVersion::v4 || !state.v4TypedEnabled || state.families.size() != 1 ||
            state.families.front().cases.size() != 1 || !state.ownedAssociations.empty() || !state.ownedTerminals.empty())
        { out.reason = "v4_case_shard_shape_unavailable"; return out; }
        FamilyState temporary; temporary.version = FamilyVersion::v4; temporary.coordinate = state.families.front().coordinate;
        temporary.cases.push_back(state.families.front().cases.front());
        if (temporary.cases[0].directSeed || temporary.cases[0].state == WorkState::in_flight)
        { out.reason = "v4_case_shard_dfs_record_unavailable"; return out; }
        // Replay identity without treating a stored outcome as retryable or
        // changing the returned case/history.
        temporary.cases[0].state = WorkState::pending; temporary.cases[0].executionId.clear();
        const auto replay = ReplayV4Case(temporary, 0, coordinateBudget, axisBudget, replayWorkBudget);
        if (replay.status != V4CaseReplay::Status::ready || !replay.unexecutedRecord || !replay.errors.empty())
        { out.resourceUnavailable = replay.status == V4CaseReplay::Status::resource_unavailable;
          out.reason = "v4_case_shard_identity_unavailable"; return out; }
        const auto decoded = NextV4FamilyCoordinate(temporary.coordinate, coordinateBudget);
        if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family)
        { out.resourceUnavailable = decoded.status == V4FamilyCoordinateStatus::resource_unavailable;
          out.reason = "v4_case_shard_family_unavailable"; return out; }
        const auto parsed = event_graph::V4FamilyCandidateGraph::ParseCursor(*decoded.family, axisBudget, temporary.cases[0].beforeCursor);
        if (parsed.status != event_graph::V4FamilyTraversalStatus::ready || !parsed.errors.empty())
        { out.resourceUnavailable = parsed.status == event_graph::V4FamilyTraversalStatus::resource_unavailable;
          out.reason = "v4_case_shard_before_cursor_unavailable"; return out; }
        const auto step = event_graph::V4FamilyCandidateGraph::Step(*decoded.family, axisBudget, parsed.cursor, temporary.cases[0].replayWork);
        if (step.status != event_graph::V4FamilyTraversalStatus::ready || !step.errors.empty() || !step.record ||
            step.workUsed != temporary.cases[0].replayWork)
        { out.resourceUnavailable = step.status == event_graph::V4FamilyTraversalStatus::resource_unavailable;
          out.reason = "v4_case_shard_step_unavailable"; return out; }
        const auto after = event_graph::V4FamilyCandidateGraph::SerializeCursor(*decoded.family, axisBudget, step.cursor);
        if (after.status != event_graph::V4FamilyTraversalStatus::ready || !after.errors.empty() ||
            after.encoded != state.families.front().candidateCursor || step.exhausted != state.families.front().scanExhausted)
        { out.resourceUnavailable = after.status == event_graph::V4FamilyTraversalStatus::resource_unavailable;
          out.reason = "v4_case_shard_after_cursor_unavailable"; return out; }
        const bool hasPrunes = !step.presencePruned.empty() || !step.activePruned.empty();
        if (state.families.front().pruneSteps.size() != (hasPrunes ? 1u : 0u))
        { out.reason = "v4_case_shard_prune_shape_unavailable"; return out; }
        if (hasPrunes)
        { const auto prune = ReplayV4PruneStep(state.families.front(), 0, coordinateBudget, axisBudget, replayWorkBudget);
          if (!prune.matched) { out.resourceUnavailable = prune.resourceUnavailable; out.reason = "v4_case_shard_prune_replay_unavailable"; return out; } }
        out.shard = state; out.identityReplayed = true;
        out.reason = "v4_case_shard_identity_only_evidence_unverified"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_case_shard_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_case_shard_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_case_shard_observation_unavailable"; return out; }
}

inline V4CaseShardResult PublishV4CaseShard(CheckpointStore& store, const State& source, std::size_t familyIndex,
    std::size_t caseIndex, std::string_view binding, const V4CoordinateBudget& coordinateBudget,
    const V4AxisBudget& axisBudget, std::size_t replayWorkBudget, std::size_t byteBudget)
{
    V4CaseShardResult out;
    try
    {
        if (source.binding != binding || source.familyVersion != FamilyVersion::v4 || !source.v4TypedEnabled ||
            familyIndex >= source.families.size() || caseIndex >= source.families[familyIndex].cases.size())
        { out.reason = "v4_case_shard_source_unavailable"; return out; }
        const auto& family = source.families[familyIndex]; const auto& item = family.cases[caseIndex];
        if (family.version != FamilyVersion::v4 || family.encodingHole || item.directSeed || item.state == WorkState::in_flight ||
            !item.replayWork || item.replayWork > replayWorkBudget)
        { out.resourceUnavailable = item.replayWork > replayWorkBudget; out.reason = "v4_case_shard_source_case_unavailable"; return out; }
        FamilyState scratch; scratch.version = FamilyVersion::v4; scratch.coordinate = family.coordinate; scratch.cases.push_back(item);
        scratch.cases[0].state = WorkState::pending; scratch.cases[0].executionId.clear();
        const auto replay = ReplayV4Case(scratch, 0, coordinateBudget, axisBudget, replayWorkBudget);
        if (replay.status != V4CaseReplay::Status::ready || !replay.unexecutedRecord || !replay.errors.empty())
        { out.resourceUnavailable = replay.status == V4CaseReplay::Status::resource_unavailable;
          out.reason = "v4_case_shard_source_identity_unavailable"; return out; }
        const auto decoded = NextV4FamilyCoordinate(family.coordinate, coordinateBudget);
        if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family)
        { out.resourceUnavailable = decoded.status == V4FamilyCoordinateStatus::resource_unavailable;
          out.reason = "v4_case_shard_family_unavailable"; return out; }
        const auto before = event_graph::V4FamilyCandidateGraph::ParseCursor(*decoded.family, axisBudget, item.beforeCursor);
        if (before.status != event_graph::V4FamilyTraversalStatus::ready || !before.errors.empty())
        { out.resourceUnavailable = before.status == event_graph::V4FamilyTraversalStatus::resource_unavailable;
          out.reason = "v4_case_shard_before_cursor_unavailable"; return out; }
        const auto step = event_graph::V4FamilyCandidateGraph::Step(*decoded.family, axisBudget, before.cursor, item.replayWork);
        if (step.status != event_graph::V4FamilyTraversalStatus::ready || !step.errors.empty() || !step.record || step.workUsed != item.replayWork)
        { out.resourceUnavailable = step.status == event_graph::V4FamilyTraversalStatus::resource_unavailable;
          out.reason = "v4_case_shard_step_unavailable"; return out; }
        const auto after = event_graph::V4FamilyCandidateGraph::SerializeCursor(*decoded.family, axisBudget, step.cursor);
        if (after.status != event_graph::V4FamilyTraversalStatus::ready || !after.errors.empty())
        { out.resourceUnavailable = after.status == event_graph::V4FamilyTraversalStatus::resource_unavailable;
          out.reason = "v4_case_shard_after_cursor_unavailable"; return out; }
        State shard; shard.binding = std::string(binding); shard.familyVersion = FamilyVersion::v4; shard.v4TypedEnabled = true;
        FamilyState one; one.version = FamilyVersion::v4; one.coordinate = family.coordinate; one.candidateCursor = after.encoded;
        one.scanExhausted = step.exhausted; one.cases.push_back(item);
        if (!step.presencePruned.empty() || !step.activePruned.empty())
        {
            PruneAuditStep audit; audit.beforeCursor = item.beforeCursor; audit.afterCursor = after.encoded; audit.workUsed = item.replayWork;
            for (const auto& p : step.presencePruned) audit.presence.push_back({p.candidateOrdinal, p.candidateId, p.prune});
            for (const auto& p : step.activePruned) audit.active.push_back({p.candidateOrdinal, p.candidateId, p.prune});
            one.pruneSteps.push_back(std::move(audit));
        }
        shard.families.push_back(std::move(one));
        const auto expected = EncodeCheckpoint(shard, binding, byteBudget);
        if (expected.status != CodecStatus::encoded)
        { out.resourceUnavailable = expected.status == CodecStatus::resource_unavailable; out.reason = "v4_case_shard_budget_unavailable"; return out; }
        out.publication = store.Publish(shard, binding, byteBudget);
        if (out.publication.status != StoreStatus::published_durability_unverified || !out.publication.fileIdentityCaptured || !out.publication.renameReturnedSuccess)
        { out.reason = "v4_case_shard_publication_unavailable"; return out; }
        auto readback = ReadV4CaseShard(store, out.publication.fileName, out.publication.fileId, binding,
            coordinateBudget, axisBudget, replayWorkBudget, byteBudget);
        if (!readback.identityReplayed || !readback.shard)
        { out.resourceUnavailable = readback.resourceUnavailable; out.reason = "v4_case_shard_readback_unavailable"; return out; }
        const auto actual = EncodeCheckpoint(*readback.shard, binding, byteBudget);
        if (actual.status != CodecStatus::encoded || actual.bytes != expected.bytes)
        { out.resourceUnavailable = actual.status == CodecStatus::resource_unavailable; out.reason = "v4_case_shard_content_mismatch"; return out; }
        out.shard = std::move(readback.shard); out.identityReplayed = true; out.published = true;
        out.reason = "v4_case_shard_published_not_linked_or_authenticated"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_case_shard_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_case_shard_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_case_shard_observation_unavailable"; return out; }
}

// Reconstruct the exact structured case from a published DFS shard. Missing
// mapping/graph stays unavailable, and replay never authorizes production.
inline V4StructuredCaseReplay ReplayV4CaseShardAsStructured(CheckpointStore& store, std::wstring_view name,
    const FILE_ID_INFO& id, std::string_view binding, const V4CoordinateBudget& coordinateBudget,
    const V4AxisBudget& axisBudget, std::size_t replayWorkBudget, std::size_t tokenBudget,
    std::size_t caseByteBudget, std::size_t shardByteBudget)
{
    V4StructuredCaseReplay out;
    try
    {
        const auto shard = ReadV4CaseShard(store, name, id, binding, coordinateBudget, axisBudget, replayWorkBudget, shardByteBudget);
        if (!shard.identityReplayed || !shard.shard || shard.shard->families.size() != 1 ||
            shard.shard->families.front().cases.size() != 1)
        { out.status = shard.resourceUnavailable ? V4StructuredCaseStatus::resource_unavailable : V4StructuredCaseStatus::unavailable;
          out.reason = "v4_case_shard_structured_replay_unavailable"; return out; }
        auto family = shard.shard->families.front();
        if (family.cases.front().state != WorkState::pending && family.cases.front().state != WorkState::resource_unavailable)
        { out.status = V4StructuredCaseStatus::unavailable; out.reason = "v4_case_shard_not_pending"; return out; }
        family.cases.front().state = WorkState::pending; family.cases.front().executionId.clear();
        const auto replay = ReplayV4Case(family, 0, coordinateBudget, axisBudget, replayWorkBudget);
        if (replay.status != V4CaseReplay::Status::ready || !replay.unexecutedRecord || !replay.errors.empty())
        { out.status = replay.status == V4CaseReplay::Status::resource_unavailable ? V4StructuredCaseStatus::resource_unavailable : V4StructuredCaseStatus::unavailable;
          out.reason = "v4_case_shard_structured_candidate_unavailable"; return out; }
        const auto& record = *replay.unexecutedRecord;
        if (record.kind != "graph_hypothesis" || !record.coordinate || static_cast<bool>(record.graph) == static_cast<bool>(record.nativeGraph))
        { out.status = V4StructuredCaseStatus::unavailable; out.reason = "v4_case_shard_mapping_or_graph_unimplemented"; return out; }
        const auto decoded = NextV4FamilyCoordinate(family.coordinate, coordinateBudget);
        if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family)
        { out.status = decoded.status == V4FamilyCoordinateStatus::resource_unavailable ? V4StructuredCaseStatus::resource_unavailable : V4StructuredCaseStatus::unavailable;
          out.reason = "v4_case_shard_family_unavailable"; return out; }
        const auto prefix = EncodeV4StructuredPrefix(*decoded.family, tokenBudget);
        if (!prefix.encoded)
        { out.status = prefix.resourceUnavailable ? V4StructuredCaseStatus::resource_unavailable : V4StructuredCaseStatus::unavailable;
          out.reason = "v4_case_shard_family_prefix_unavailable"; return out; }
        if (record.graph)
            out = MakeV4StructuredCaseId(prefix.tokens, *record.coordinate, record.graph->present, "ole", record.graph->graph.id,
                coordinateBudget, axisBudget, tokenBudget, caseByteBudget);
        else
            out = MakeV4StructuredCaseId(prefix.tokens, *record.coordinate, record.nativeGraph->present, "native", record.nativeGraph->graph.id,
                coordinateBudget, axisBudget, tokenBudget, caseByteBudget);
        if (out.status == V4StructuredCaseStatus::unexecuted && (out.familyCoordinate != family.coordinate || out.candidateId != record.candidateId))
        { out = {}; out.status = V4StructuredCaseStatus::unavailable; out.reason = "v4_case_shard_structured_identity_mismatch"; }
        return out;
    }
    catch (const std::bad_alloc&) { out.status = V4StructuredCaseStatus::resource_unavailable; out.reason = "v4_case_shard_structured_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.status = V4StructuredCaseStatus::resource_unavailable; out.reason = "v4_case_shard_structured_resource_unavailable"; return out; }
    catch (...) { out.status = V4StructuredCaseStatus::unavailable; out.reason = "v4_case_shard_structured_unavailable"; return out; }
}

struct V4FamilyCursorShardResult
{
    bool published = false, cursorValidated = false, resourceUnavailable = false;
    PublishedCheckpoint publication;
    std::optional<FamilyState> family;
    std::string_view reason;
};
inline V4FamilyCursorShardResult ReadV4FamilyCursorShard(CheckpointStore& store, std::wstring_view name,
    const FILE_ID_INFO& fileId, std::string_view binding, const V4CoordinateBudget& coordinateBudget,
    const V4AxisBudget& axisBudget, std::size_t byteBudget, std::size_t replayWorkBudget = 0)
{
    V4FamilyCursorShardResult out;
    try
    {
        const auto loaded = store.Read(name, fileId, binding, byteBudget);
        if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight)
        { out.resourceUnavailable = loaded.status == CodecStatus::resource_unavailable; out.reason = "v4_family_shard_read_unavailable"; return out; }
        if (loaded.state.familyVersion != FamilyVersion::v4 || !loaded.state.v4TypedEnabled || loaded.state.families.size() != 1 ||
            !loaded.state.ownedAssociations.empty() || !loaded.state.ownedTerminals.empty() || loaded.state.shardIndex)
        { out.reason = "v4_family_shard_shape_unavailable"; return out; }
        const auto& family = loaded.state.families.front();
        if (!family.cases.empty() || family.pruneSteps.size() > 1 || family.nextRetry || family.encodingHole || family.duplicateSeed ||
            family.candidateCursor.empty() || family.state == WorkState::in_flight)
        { out.reason = "v4_family_shard_cursor_unavailable"; return out; }
        const auto decoded = NextV4FamilyCoordinate(family.coordinate, coordinateBudget);
        if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family)
        { out.resourceUnavailable = decoded.status == V4FamilyCoordinateStatus::resource_unavailable; out.reason = "v4_family_shard_identity_unavailable"; return out; }
        const auto parsed = event_graph::V4FamilyCandidateGraph::ParseCursor(*decoded.family, axisBudget, family.candidateCursor);
        if (parsed.status != event_graph::V4FamilyTraversalStatus::ready || !parsed.errors.empty() || parsed.cursor.exhausted != family.scanExhausted)
        { out.resourceUnavailable = parsed.status == event_graph::V4FamilyTraversalStatus::resource_unavailable;
          out.reason = "v4_family_shard_cursor_unavailable"; return out; }
        if (!family.pruneSteps.empty())
        {
            const auto& audit = family.pruneSteps.front();
            if (audit.workUsed > replayWorkBudget)
            { out.resourceUnavailable = true; out.reason = "v4_family_shard_step_replay_budget_unavailable"; return out; }
            if (audit.afterCursor != family.candidateCursor)
            { out.reason = "v4_family_shard_step_audit_unavailable"; return out; }
            const auto before = event_graph::V4FamilyCandidateGraph::ParseCursor(*decoded.family, axisBudget, audit.beforeCursor);
            if (before.status != event_graph::V4FamilyTraversalStatus::ready || !before.errors.empty())
            { out.resourceUnavailable = before.status == event_graph::V4FamilyTraversalStatus::resource_unavailable;
              out.reason = "v4_family_shard_step_before_unavailable"; return out; }
            const auto step = event_graph::V4FamilyCandidateGraph::Step(*decoded.family, axisBudget, before.cursor, audit.workUsed);
            if (step.status != event_graph::V4FamilyTraversalStatus::ready || !step.errors.empty() || step.record ||
                step.workUsed != audit.workUsed || step.exhausted != family.scanExhausted)
            { out.resourceUnavailable = step.status == event_graph::V4FamilyTraversalStatus::resource_unavailable;
              out.reason = "v4_family_shard_step_record_unavailable"; return out; }
            const auto prune = ReplayV4PruneStep(family, 0, coordinateBudget, axisBudget, audit.workUsed);
            if (!prune.matched)
            { out.resourceUnavailable = prune.resourceUnavailable; out.reason = "v4_family_shard_step_audit_unavailable"; return out; }
        }
        out.family = family; out.cursorValidated = true; out.reason = "v4_family_shard_cursor_only_not_coverage"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_family_shard_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_family_shard_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_family_shard_observation_unavailable"; return out; }
}
inline V4FamilyCursorShardResult PublishV4FamilyCursorShard(CheckpointStore& store, const FamilyState& source,
    std::string_view binding, const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget, std::size_t byteBudget,
    const PruneAuditStep* stepAudit = nullptr, std::size_t replayWorkBudget = 0)
{
    V4FamilyCursorShardResult out;
    try
    {
        if (source.version != FamilyVersion::v4 || source.encodingHole || source.duplicateSeed || source.state == WorkState::in_flight)
        { out.reason = "v4_family_shard_source_unavailable"; return out; }
        const auto decoded = NextV4FamilyCoordinate(source.coordinate, coordinateBudget);
        if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family)
        { out.resourceUnavailable = decoded.status == V4FamilyCoordinateStatus::resource_unavailable; out.reason = "v4_family_shard_identity_unavailable"; return out; }
        event_graph::V4FamilyCandidateCursor cursor;
        if (!source.candidateCursor.empty())
        {
            const auto parsed = event_graph::V4FamilyCandidateGraph::ParseCursor(*decoded.family, axisBudget, source.candidateCursor);
            if (parsed.status != event_graph::V4FamilyTraversalStatus::ready || !parsed.errors.empty())
            { out.resourceUnavailable = parsed.status == event_graph::V4FamilyTraversalStatus::resource_unavailable;
              out.reason = "v4_family_shard_cursor_unavailable"; return out; }
            cursor = parsed.cursor;
        }
        const auto serialized = event_graph::V4FamilyCandidateGraph::SerializeCursor(*decoded.family, axisBudget, cursor);
        if (serialized.status != event_graph::V4FamilyTraversalStatus::ready || !serialized.errors.empty() ||
            serialized.cursor.exhausted != source.scanExhausted)
        { out.resourceUnavailable = serialized.status == event_graph::V4FamilyTraversalStatus::resource_unavailable;
          out.reason = "v4_family_shard_cursor_unavailable"; return out; }
        State shard; shard.binding = std::string(binding); shard.familyVersion = FamilyVersion::v4; shard.v4TypedEnabled = true;
        FamilyState family; family.version = FamilyVersion::v4; family.coordinate = source.coordinate;
        family.candidateCursor = serialized.encoded; family.scanExhausted = source.scanExhausted; family.state = source.state;
        if (stepAudit) family.pruneSteps.push_back(*stepAudit);
        shard.families.push_back(std::move(family));
        const auto expected = EncodeCheckpoint(shard, binding, byteBudget);
        if (expected.status != CodecStatus::encoded)
        { out.resourceUnavailable = expected.status == CodecStatus::resource_unavailable; out.reason = "v4_family_shard_budget_unavailable"; return out; }
        out.publication = store.Publish(shard, binding, byteBudget);
        if (out.publication.status != StoreStatus::published_durability_unverified || !out.publication.fileIdentityCaptured || !out.publication.renameReturnedSuccess)
        { out.reason = "v4_family_shard_publication_unavailable"; return out; }
        auto readback = ReadV4FamilyCursorShard(store, out.publication.fileName, out.publication.fileId, binding, coordinateBudget, axisBudget, byteBudget, replayWorkBudget);
        if (!readback.cursorValidated || !readback.family)
        { out.resourceUnavailable = readback.resourceUnavailable; out.reason = "v4_family_shard_readback_unavailable"; return out; }
        const auto loaded = store.Read(out.publication.fileName, out.publication.fileId, binding, byteBudget);
        const auto actual = loaded.status == CodecStatus::decoded && !loaded.recoveredInFlight ?
            EncodeCheckpoint(loaded.state, binding, byteBudget) : CodecResult{};
        if (actual.status != CodecStatus::encoded || actual.bytes != expected.bytes)
        { out.resourceUnavailable = loaded.status == CodecStatus::resource_unavailable || actual.status == CodecStatus::resource_unavailable;
          out.reason = "v4_family_shard_content_mismatch"; return out; }
        out.family = std::move(readback.family); out.published = true; out.cursorValidated = true;
        out.reason = "v4_family_shard_published_not_linked_or_coverage"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_family_shard_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_family_shard_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_family_shard_observation_unavailable"; return out; }
}

// Local checkpoint consistency only. Even a successful result is not a
// proof that a production event occurred or that a child is trustworthy.
enum class V4AttemptTransitionStatus { consistent_unverified, invalid, unavailable, resource_unavailable };
struct V4AttemptTransitionAudit
{
    V4AttemptTransitionStatus status = V4AttemptTransitionStatus::invalid;
    std::string_view reason;
    std::optional<std::size_t> familyIndex, caseIndex;
};

inline V4AttemptTransitionAudit VerifyV4AttemptTransitionCore(std::string_view preBytes,
    std::string_view inFlightBytes, std::optional<std::string_view> terminalBytes, std::string_view binding,
    std::string_view exactCaseId, std::string_view executionId,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
    std::size_t tokenBudget, std::size_t caseByteBudget, std::size_t checkpointByteBudget,
    std::size_t replayWorkBudget)
{
    V4AttemptTransitionAudit out;
    try
    {
        if (binding.empty() || exactCaseId.empty() || executionId.empty())
        { out.reason = "v4_attempt_audit_identity_invalid"; return out; }
        const auto pre = DecodeCheckpoint(preBytes, binding, checkpointByteBudget);
        const auto inFlight = DecodeCheckpoint(inFlightBytes, binding, checkpointByteBudget);
        CodecResult terminal;
        if (terminalBytes) terminal = DecodeCheckpoint(*terminalBytes, binding, checkpointByteBudget);
        if (pre.status == CodecStatus::resource_unavailable || inFlight.status == CodecStatus::resource_unavailable ||
            (terminalBytes && terminal.status == CodecStatus::resource_unavailable))
        { out.status = V4AttemptTransitionStatus::resource_unavailable; out.reason = "v4_attempt_audit_checkpoint_budget"; return out; }
        if (pre.status != CodecStatus::decoded || inFlight.status != CodecStatus::decoded ||
            (terminalBytes && terminal.status != CodecStatus::decoded) ||
            pre.state.familyVersion != FamilyVersion::v4 || !pre.state.v4TypedEnabled)
        { out.reason = "v4_attempt_audit_checkpoint_invalid"; return out; }
        // Only current canonical bytes are accepted. In particular do not use
        // Read(), which intentionally converts recovered in-flight states.
        const auto canonicalPre = EncodeCheckpoint(pre.state, binding, checkpointByteBudget);
        const auto canonicalFlight = EncodeCheckpoint(inFlight.state, binding, checkpointByteBudget);
        CodecResult canonicalTerminal;
        if (terminalBytes) canonicalTerminal = EncodeCheckpoint(terminal.state, binding, checkpointByteBudget);
        if (canonicalPre.status == CodecStatus::resource_unavailable || canonicalFlight.status == CodecStatus::resource_unavailable ||
            (terminalBytes && canonicalTerminal.status == CodecStatus::resource_unavailable))
        { out.status = V4AttemptTransitionStatus::resource_unavailable; out.reason = "v4_attempt_audit_encoding_budget"; return out; }
        if (canonicalPre.status != CodecStatus::encoded || canonicalFlight.status != CodecStatus::encoded ||
            (terminalBytes && canonicalTerminal.status != CodecStatus::encoded) || canonicalPre.bytes != preBytes ||
            canonicalFlight.bytes != inFlightBytes || (terminalBytes && canonicalTerminal.bytes != *terminalBytes))
        { out.reason = "v4_attempt_audit_checkpoint_version_or_canonical_invalid"; return out; }
        const auto requested = ReplayV4StructuredCaseId(exactCaseId, coordinateBudget, axisBudget, tokenBudget, caseByteBudget);
        if (requested.status == V4StructuredCaseStatus::resource_unavailable)
        { out.status = V4AttemptTransitionStatus::resource_unavailable; out.reason = "v4_attempt_audit_case_budget"; return out; }
        if (requested.status != V4StructuredCaseStatus::unexecuted || !requested.candidate)
        { out.status = V4AttemptTransitionStatus::unavailable; out.reason = "v4_attempt_audit_case_replay_unavailable"; return out; }
        std::string presence;
        for (bool bit : requested.present) presence.push_back(bit ? '1' : '0');
        const auto mode = requested.graphMode == "ole" ? "v4_ole" : requested.graphMode == "native" ? "v4_native" : "";
        for (std::size_t f = 0; f < pre.state.families.size(); ++f)
        {
            const auto& family = pre.state.families[f];
            if (family.state == WorkState::in_flight)
            { out.reason = "v4_attempt_audit_pre_inflight_invalid"; return out; }
            for (std::size_t c = 0; c < family.cases.size(); ++c)
            {
                const auto& item = family.cases[c];
                if (item.state == WorkState::in_flight)
                { out.reason = "v4_attempt_audit_pre_inflight_invalid"; return out; }
                if (item.executionId == executionId || std::any_of(item.history.begin(), item.history.end(),
                    [&](const auto& previous) { return previous.executionId == executionId; }))
                { out.reason = "v4_attempt_audit_execution_id_reused"; return out; }
                if (family.coordinate != requested.familyCoordinate || item.candidateId != requested.candidateId ||
                    item.graphId != requested.graphId || item.graphMode != mode || item.presence != presence) continue;
                if (out.familyIndex) { out.reason = "v4_attempt_audit_duplicate_case_invalid"; return out; }
                out.familyIndex = f; out.caseIndex = c;
            }
        }
        if (!out.familyIndex || !out.caseIndex)
        { out.status = V4AttemptTransitionStatus::unavailable; out.reason = "v4_attempt_audit_case_missing"; return out; }
        const auto f = *out.familyIndex, c = *out.caseIndex;
        const auto& original = pre.state.families[f].cases[c];
        if ((original.state != WorkState::pending && original.state != WorkState::resource_unavailable) ||
            !original.executionId.empty() || original.kind != "graph_hypothesis")
        { out.reason = "v4_attempt_audit_pre_case_invalid"; return out; }
        const auto replay = ReplayV4Case(pre.state.families[f], c, coordinateBudget, axisBudget, replayWorkBudget);
        if (replay.status == V4CaseReplay::Status::resource_unavailable)
        { out.status = V4AttemptTransitionStatus::resource_unavailable; out.reason = "v4_attempt_audit_replay_budget"; return out; }
        if (replay.status != V4CaseReplay::Status::ready || !replay.unexecutedRecord || !replay.unexecutedRecord->coordinate)
        { out.status = V4AttemptTransitionStatus::unavailable; out.reason = "v4_attempt_audit_record_replay_unavailable"; return out; }
        State expectedFlight = pre.state;
        auto& flying = expectedFlight.families[f].cases[c]; flying.state = WorkState::in_flight; flying.executionId = executionId;
        const auto expectedFlightBytes = EncodeCheckpoint(expectedFlight, binding, checkpointByteBudget);
        if (expectedFlightBytes.status == CodecStatus::resource_unavailable)
        { out.status = V4AttemptTransitionStatus::resource_unavailable; out.reason = "v4_attempt_audit_flight_budget"; return out; }
        if (expectedFlightBytes.status != CodecStatus::encoded || expectedFlightBytes.bytes != inFlightBytes)
        { out.reason = "v4_attempt_audit_flight_transition_invalid"; return out; }
        if (!terminalBytes)
        { out.status = V4AttemptTransitionStatus::consistent_unverified;
          out.reason = "v4_attempt_start_only_not_execution_authorization"; return out; }
        if (f >= terminal.state.families.size() || c >= terminal.state.families[f].cases.size())
        { out.reason = "v4_attempt_audit_terminal_case_invalid"; return out; }
        const auto& finalCase = terminal.state.families[f].cases[c];
        if (finalCase.state != WorkState::outcome_unavailable || finalCase.executionId != executionId ||
            finalCase.history.size() != original.history.size() + 1 || finalCase.history.back().executionId != executionId)
        { out.reason = "v4_attempt_audit_terminal_transition_invalid"; return out; }
        const auto& facts = finalCase.history.back();
        if (facts.status.empty() || (facts.bridgeEntered && !facts.receiptConsumed) ||
            ((facts.productionStarted || facts.productionEntryUnknown) && !facts.bridgeEntered))
        { out.reason = "v4_attempt_audit_terminal_stages_invalid"; return out; }
        State expectedTerminal = pre.state;
        auto& finished = expectedTerminal.families[f].cases[c];
        finished.state = WorkState::outcome_unavailable; finished.executionId = executionId; finished.history.push_back(facts);
        const auto expectedTerminalBytes = EncodeCheckpoint(expectedTerminal, binding, checkpointByteBudget);
        if (expectedTerminalBytes.status == CodecStatus::resource_unavailable)
        { out.status = V4AttemptTransitionStatus::resource_unavailable; out.reason = "v4_attempt_audit_terminal_budget"; return out; }
        if (expectedTerminalBytes.status != CodecStatus::encoded || expectedTerminalBytes.bytes != *terminalBytes)
        { out.reason = "v4_attempt_audit_terminal_state_invalid"; return out; }
        out.status = V4AttemptTransitionStatus::consistent_unverified;
        out.reason = "v4_attempt_transition_only_not_production_authentication";
        return out;
    }
    catch (const std::bad_alloc&) { out.status = V4AttemptTransitionStatus::resource_unavailable; out.reason = "v4_attempt_audit_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.status = V4AttemptTransitionStatus::resource_unavailable; out.reason = "v4_attempt_audit_resource_unavailable"; return out; }
    catch (...) { out.status = V4AttemptTransitionStatus::unavailable; out.reason = "v4_attempt_audit_observation_unavailable"; return out; }
}

inline V4AttemptTransitionAudit VerifyV4AttemptStart(std::string_view preBytes,
    std::string_view inFlightBytes, std::string_view binding, std::string_view exactCaseId, std::string_view executionId,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
    std::size_t tokenBudget, std::size_t caseByteBudget, std::size_t checkpointByteBudget, std::size_t replayWorkBudget)
{
    return VerifyV4AttemptTransitionCore(preBytes, inFlightBytes, std::nullopt, binding, exactCaseId, executionId,
        coordinateBudget, axisBudget, tokenBudget, caseByteBudget, checkpointByteBudget, replayWorkBudget);
}

inline V4AttemptTransitionAudit VerifyV4AttemptTransition(std::string_view preBytes,
    std::string_view inFlightBytes, std::string_view terminalBytes, std::string_view binding,
    std::string_view exactCaseId, std::string_view executionId,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
    std::size_t tokenBudget, std::size_t caseByteBudget, std::size_t checkpointByteBudget, std::size_t replayWorkBudget)
{
    return VerifyV4AttemptTransitionCore(preBytes, inFlightBytes, terminalBytes, binding, exactCaseId, executionId,
        coordinateBudget, axisBudget, tokenBudget, caseByteBudget, checkpointByteBudget, replayWorkBudget);
}

enum class V4StructuredCaseProposalStatus { proposed, already_recorded, invalid, unavailable, resource_unavailable };
struct V4StructuredCaseProposal
{
    V4StructuredCaseProposalStatus status = V4StructuredCaseProposalStatus::invalid;
    std::optional<State> proposed; // publish the complete state before use
    std::optional<std::size_t> familyIndex, caseIndex;
    std::string_view reason;
};

// Register precisely one authority-replayed case, without advancing or
// fabricating the family DFS cursor. This is not an execution permit; the
// ordinary checkpoint/BeginV4Attempt and adapter gates remain mandatory.
// Existing records are revalidated, including bounded DFS replay, before
// duplicate suppression. An ineligible or unprovable record is unavailable.
inline V4StructuredCaseProposal PrepareV4StructuredCase(const State& original, std::string_view binding,
    std::string_view caseId, const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
    std::size_t tokenBudget, std::size_t caseByteBudget, std::size_t checkpointByteBudget,
    std::size_t replayWorkBudget)
{
    V4StructuredCaseProposal out;
    try
    {
        const auto initial = EncodeCheckpoint(original, binding, checkpointByteBudget);
        if (initial.status != CodecStatus::encoded || original.familyVersion != FamilyVersion::v4 || !original.v4TypedEnabled)
        { out.status = initial.status == CodecStatus::resource_unavailable ? V4StructuredCaseProposalStatus::resource_unavailable : V4StructuredCaseProposalStatus::unavailable;
          out.reason = "v4_structured_proposal_state_unavailable"; return out; }
        const auto replay = ReplayV4StructuredCaseId(caseId, coordinateBudget, axisBudget, tokenBudget, caseByteBudget);
        if (replay.status != V4StructuredCaseStatus::unexecuted || !replay.family || !replay.candidate ||
            (static_cast<bool>(replay.oleGraph) == static_cast<bool>(replay.nativeGraph)))
        { out.status = replay.status == V4StructuredCaseStatus::resource_unavailable ? V4StructuredCaseProposalStatus::resource_unavailable :
              replay.status == V4StructuredCaseStatus::invalid ? V4StructuredCaseProposalStatus::invalid : V4StructuredCaseProposalStatus::unavailable;
          out.reason = replay.reason; return out; }
        const std::string mode = replay.oleGraph ? "v4_ole" : "v4_native";
        std::string bits; for (bool bit : replay.present) bits += bit ? '1' : '0';
        auto found = std::find_if(original.families.begin(), original.families.end(), [&](const FamilyState& family) {
            return family.coordinate == replay.familyCoordinate; });
        const std::size_t index = static_cast<std::size_t>(std::distance(original.families.begin(), found));
        if (found != original.families.end())
        {
            if (found->version != FamilyVersion::v4 || found->encodingHole || found->duplicateSeed)
            { out.status = V4StructuredCaseProposalStatus::unavailable; out.reason = "v4_structured_proposal_family_unavailable"; return out; }
            for (std::size_t i = 0; i < found->cases.size(); ++i)
            {
                const auto& old = found->cases[i];
                if (old.candidateId != replay.candidateId || old.graphMode != mode || old.graphId != replay.graphId || old.presence != bits) continue;
                const auto checked = ReplayV4Case(*found, i, coordinateBudget, axisBudget, replayWorkBudget);
                if (checked.status != V4CaseReplay::Status::ready || !checked.unexecutedRecord || !checked.errors.empty())
                { out.status = checked.status == V4CaseReplay::Status::resource_unavailable ? V4StructuredCaseProposalStatus::resource_unavailable : V4StructuredCaseProposalStatus::unavailable;
                  out.reason = "v4_structured_proposal_existing_replay_unavailable"; return out; }
                out.familyIndex = index; out.caseIndex = i; out.status = V4StructuredCaseProposalStatus::already_recorded;
                out.reason = "v4_structured_case_already_recorded"; return out;
            }
        }
        State next = original;
        if (found == original.families.end())
        {
            FamilyState family; family.version = FamilyVersion::v4; family.coordinate = replay.familyCoordinate;
            next.families.push_back(std::move(family));
        }
        CaseRecord item; item.directSeed = true; item.graphMode = mode; item.kind = "graph_hypothesis";
        item.reason = "unexecuted"; item.candidateId = replay.candidateId; item.graphId = replay.graphId;
        item.presence = std::move(bits); item.candidateValues = replay.candidate->values;
        item.unresolved = replay.oleGraph ? replay.oleGraph->unresolved : replay.nativeGraph->unresolved;
        auto& family = next.families[index];
        const auto caseIndex = family.cases.size(); family.cases.push_back(std::move(item));
        const auto encoded = EncodeCheckpoint(next, binding, checkpointByteBudget);
        if (encoded.status != CodecStatus::encoded)
        { out.status = encoded.status == CodecStatus::resource_unavailable ? V4StructuredCaseProposalStatus::resource_unavailable : V4StructuredCaseProposalStatus::unavailable;
          out.reason = "v4_structured_proposal_checkpoint_unavailable"; return out; }
        out.familyIndex = index; out.caseIndex = caseIndex; out.proposed = std::move(next);
        out.status = V4StructuredCaseProposalStatus::proposed; out.reason = "v4_structured_case_unexecuted"; return out;
    }
    catch (const std::bad_alloc&) { out = {}; out.status = V4StructuredCaseProposalStatus::resource_unavailable; out.reason = "v4_structured_proposal_resource_unavailable"; return out; }
    catch (const std::length_error&) { out = {}; out.status = V4StructuredCaseProposalStatus::resource_unavailable; out.reason = "v4_structured_proposal_resource_unavailable"; return out; }
    catch (...) { out = {}; out.status = V4StructuredCaseProposalStatus::unavailable; out.reason = "v4_structured_proposal_observation_unavailable"; return out; }
}

enum class V4RoundAction { publish_only, ole_candidate, native_candidate, retry_unavailable, invalid };
struct V4RoundProposal
{
    FamilyVisitProposal visit;
    V4RoundAction action = V4RoundAction::invalid;
    std::optional<std::size_t> familyIndex, caseIndex;
    std::string candidateId, graphId;
    V4CaseReplay::Status replayStatus = V4CaseReplay::Status::unavailable;
    std::vector<std::string> errors;
};
// Pure selection, not an execution permit. Publish the ENTIRE proposed State
// first; BeginV4Attempt must read that exact committed head and replay again.
// Unknown mappings and failed replay stay pending, never become a PASS or a
// structural product exclusion.
inline V4RoundProposal PrepareV4Round(const State& original, std::string_view binding,
    std::size_t decimalDigitBudget, std::size_t memberBudget, std::size_t workBudget,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget, std::size_t replayWorkBudget)
{
    V4RoundProposal result;
    try
    {
        if (original.familyVersion != FamilyVersion::v4 || !original.v4TypedEnabled)
        { result.errors.emplace_back("v4_round_migration_or_version_unavailable"); return result; }
        result.visit = PrepareFamilyVisit(original, binding, decimalDigitBudget, memberBudget, workBudget,
            &coordinateBudget, &axisBudget);
        if (result.visit.tick.kind == TickKind::invalid_state || result.visit.tick.kind == TickKind::unavailable)
        { result.errors = result.visit.errors; result.errors.emplace_back(result.visit.tick.reason); return result; }
        result.action = V4RoundAction::publish_only;
        if (result.visit.tick.kind != TickKind::visit || !result.visit.tick.familyIndex || result.visit.retryIndices.empty()) return result;
        const auto familyIndex = *result.visit.tick.familyIndex;
        const auto caseIndex = result.visit.retryIndices.front();
        if (familyIndex >= result.visit.tick.proposed.families.size())
        { result.action = V4RoundAction::invalid; result.errors.emplace_back("v4_round_family_index_invalid"); return result; }
        auto& family = result.visit.tick.proposed.families[familyIndex];
        if (caseIndex >= family.cases.size())
        { result.action = V4RoundAction::invalid; result.errors.emplace_back("v4_round_case_index_invalid"); return result; }
        // This facade selects only one of the visit's retry hints. Resume at
        // the following case so its second hint cannot be skipped forever.
        family.nextRetry = caseIndex + 1 == family.cases.size() ? 0 : caseIndex + 1;
        result.familyIndex = familyIndex; result.caseIndex = caseIndex;
        const auto replay = ReplayV4Case(family, caseIndex, coordinateBudget, axisBudget, replayWorkBudget);
        result.replayStatus = replay.status;
        if (!replay.unexecutedRecord || !replay.errors.empty())
        { result.action = V4RoundAction::retry_unavailable; result.errors = replay.errors; return result; }
        const auto& record = *replay.unexecutedRecord;
        if (record.kind != "graph_hypothesis" || !record.coordinate ||
            static_cast<bool>(record.graph) == static_cast<bool>(record.nativeGraph))
        { result.action = V4RoundAction::retry_unavailable; result.errors.emplace_back("v4_round_mapping_or_graph_unavailable"); return result; }
        result.candidateId = record.candidateId;
        if (record.graph) { result.action = V4RoundAction::ole_candidate; result.graphId = record.graph->graph.id; }
        else { result.action = V4RoundAction::native_candidate; result.graphId = record.nativeGraph->graph.id; }
        return result;
    }
    catch (...) { result = {}; result.errors.emplace_back("v4_round_resource_unavailable"); return result; }
}

enum class V4ForegroundCycleStatus { proposal_only, owned_file_template, resource_unavailable, unavailable };
struct V4ForegroundCycleProposal
{
    V4ForegroundCycleStatus status = V4ForegroundCycleStatus::unavailable;
    std::optional<V4RoundProposal> round;
    std::string templateCaseId;
    std::string_view reason;
};
// One bounded step of the existing global scheduler. The full round, including
// discovery, DFS cursor, prunes, retry position and emitted cases, must be
// durably published and read back before the template may be sent to a child.
// A child-created root produces a distinct case ID; a child outcome cannot be
// used as an execution result for this template's original exact path.
inline V4ForegroundCycleProposal PrepareV4ForegroundCycle(const State& original, std::string_view binding,
    std::size_t decimalDigitBudget, std::size_t memberBudget, std::size_t workBudget,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
    std::size_t replayWorkBudget, std::size_t tokenBudget, std::size_t caseByteBudget,
    std::size_t checkpointByteBudget)
{
    V4ForegroundCycleProposal out;
    try
    {
        const auto initial = EncodeCheckpoint(original, binding, checkpointByteBudget);
        if (initial.status != CodecStatus::encoded || original.familyVersion != FamilyVersion::v4 || !original.v4TypedEnabled)
        { out.status = initial.status == CodecStatus::resource_unavailable ? V4ForegroundCycleStatus::resource_unavailable : V4ForegroundCycleStatus::unavailable;
          out.reason = "v4_cycle_checkpoint_unavailable"; return out; }
        for (const auto& family : original.families)
            if (family.state == WorkState::in_flight || std::any_of(family.cases.begin(), family.cases.end(),
                [](const auto& item) { return item.state == WorkState::in_flight; }))
            { out.reason = "v4_cycle_inflight_unavailable"; return out; }
        auto round = PrepareV4Round(original, binding, decimalDigitBudget, memberBudget, workBudget,
            coordinateBudget, axisBudget, replayWorkBudget);
        if (round.action == V4RoundAction::invalid)
        { out.reason = "v4_cycle_round_unavailable"; return out; }
        const auto encoded = EncodeCheckpoint(round.visit.tick.proposed, binding, checkpointByteBudget);
        if (encoded.status != CodecStatus::encoded)
        { out.status = encoded.status == CodecStatus::resource_unavailable ? V4ForegroundCycleStatus::resource_unavailable : V4ForegroundCycleStatus::unavailable;
          out.reason = "v4_cycle_proposal_unavailable"; return out; }
        out.status = V4ForegroundCycleStatus::proposal_only; out.reason = "v4_cycle_no_owned_file_template";
        if (round.action == V4RoundAction::retry_unavailable)
        {
            out.status = round.replayStatus == V4CaseReplay::Status::resource_unavailable ?
                V4ForegroundCycleStatus::resource_unavailable : V4ForegroundCycleStatus::proposal_only;
            out.reason = "v4_cycle_case_retry_unavailable"; out.round = std::move(round); return out;
        }
        if (round.visit.tick.kind == TickKind::visit && round.visit.tick.familyIndex &&
            *round.visit.tick.familyIndex < round.visit.tick.proposed.families.size())
        {
            const auto state = round.visit.tick.proposed.families[*round.visit.tick.familyIndex].state;
            if (state == WorkState::resource_unavailable || state == WorkState::schema_unavailable ||
                state == WorkState::observation_unavailable)
            {
                out.status = state == WorkState::resource_unavailable ? V4ForegroundCycleStatus::resource_unavailable : V4ForegroundCycleStatus::proposal_only;
                out.reason = "v4_cycle_family_traversal_unavailable"; out.round = std::move(round); return out;
            }
        }
        if (round.action != V4RoundAction::ole_candidate || !round.familyIndex || !round.caseIndex)
        { out.round = std::move(round); return out; }
        const auto& familyState = round.visit.tick.proposed.families.at(*round.familyIndex);
        const auto decoded = NextV4FamilyCoordinate(familyState.coordinate, coordinateBudget);
        if (decoded.status == V4FamilyCoordinateStatus::resource_unavailable)
        { out.status = V4ForegroundCycleStatus::resource_unavailable; out.reason = "v4_cycle_family_resource_unavailable";
          out.round = std::move(round); return out; }
        if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family || decoded.coordinate != familyState.coordinate)
        { out.reason = "v4_cycle_family_unavailable"; out.round = std::move(round); return out; }
        const auto& family = *decoded.family;
        if (family.b || family.a.kind != V4SourceKind::file_only || family.a.fileCount.kind != V3CountKind::exact ||
            family.a.fileCount.decimal != "1" || family.a.filePaths.size() != 1 || family.a.filePaths[0].index != "0" ||
            family.a.filePaths[0].kind != V4PayloadKind::exact || family.a.filePaths[0].base != V4PathBase::raw)
        { out.round = std::move(round); return out; }
        const auto replay = ReplayV4Case(familyState, *round.caseIndex, coordinateBudget, axisBudget, replayWorkBudget);
        if (replay.status != V4CaseReplay::Status::ready || !replay.unexecutedRecord || !replay.errors.empty())
        { out.status = replay.status == V4CaseReplay::Status::resource_unavailable ? V4ForegroundCycleStatus::resource_unavailable : V4ForegroundCycleStatus::proposal_only;
          out.reason = "v4_cycle_case_replay_unavailable"; out.round = std::move(round); return out; }
        const auto& record = *replay.unexecutedRecord;
        if (record.kind != "graph_hypothesis" || !record.coordinate || !record.graph || record.nativeGraph ||
            record.candidateId != round.candidateId || record.graph->graph.id != round.graphId)
        { out.reason = "v4_cycle_case_identity_unavailable"; out.round = std::move(round); return out; }
        const auto prefix = EncodeV4StructuredPrefix(family, tokenBudget);
        if (!prefix.encoded)
        { out.status = prefix.resourceUnavailable ? V4ForegroundCycleStatus::resource_unavailable : V4ForegroundCycleStatus::proposal_only;
          out.reason = "v4_cycle_prefix_unavailable"; out.round = std::move(round); return out; }
        const auto structured = MakeV4StructuredCaseId(prefix.tokens, *record.coordinate, record.graph->present,
            "ole", record.graph->graph.id, coordinateBudget, axisBudget, tokenBudget, caseByteBudget);
        if (structured.status != V4StructuredCaseStatus::unexecuted || structured.candidateId != round.candidateId ||
            structured.familyCoordinate != familyState.coordinate || structured.graphId != round.graphId)
        { out.status = structured.status == V4StructuredCaseStatus::resource_unavailable ? V4ForegroundCycleStatus::resource_unavailable : V4ForegroundCycleStatus::proposal_only;
          out.reason = "v4_cycle_structured_identity_unavailable"; out.round = std::move(round); return out; }
        out.templateCaseId = structured.caseId; out.status = V4ForegroundCycleStatus::owned_file_template;
        out.reason = "v4_cycle_owned_file_template_unexecuted"; out.round = std::move(round);
    }
    catch (const std::bad_alloc&) { out = {}; out.status = V4ForegroundCycleStatus::resource_unavailable; out.reason = "v4_cycle_resource_unavailable"; }
    catch (const std::length_error&) { out = {}; out.status = V4ForegroundCycleStatus::resource_unavailable; out.reason = "v4_cycle_resource_unavailable"; }
    catch (...) { out = {}; out.reason = "v4_cycle_observation_unavailable"; }
    return out;
}

// Reconstruct the sole parent-ledger transition from independently pinned
// file bytes and caller-authenticated locators. Consistency is not execution
// evidence and does not authenticate a head or a process by itself.
inline V4AttemptTransitionAudit VerifyV4ExactParentAttempt(std::string_view parentPreBytes, std::string_view parentHeadBytes,
    std::string_view binding, std::string_view exactCaseId, std::string_view executionId,
    const std::filesystem::path& sourcePath, HANDLE sourceRoot, const std::array<FILE_ID_INFO, 6>& ids,
    std::string_view childPreBytes, std::string_view childFlightBytes, std::wstring_view previousName,
    const FILE_ID_INFO& previousId, const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
    std::size_t tokenBudget, std::size_t caseByteBudget, std::size_t replayWorkBudget,
    std::size_t childByteBudget, std::size_t checkpointByteBudget)
{
    V4AttemptTransitionAudit out;
    try
    {
        const auto pre = DecodeCheckpoint(parentPreBytes, binding, checkpointByteBudget);
        const auto head = DecodeCheckpoint(parentHeadBytes, binding, checkpointByteBudget);
        if (pre.status != CodecStatus::decoded || head.status != CodecStatus::decoded)
        { out.status = pre.status == CodecStatus::resource_unavailable || head.status == CodecStatus::resource_unavailable ?
            V4AttemptTransitionStatus::resource_unavailable : V4AttemptTransitionStatus::unavailable;
          out.reason = "v4_exact_parent_checkpoint_unavailable"; return out; }
        const auto replay = ReplayV4StructuredCaseId(exactCaseId, coordinateBudget, axisBudget, tokenBudget, caseByteBudget);
        const auto plan = PrepareV4ExactOwnedFiles(exactCaseId, coordinateBudget, axisBudget, tokenBudget, caseByteBudget,
            sourcePath, sourceRoot, ids[2], axisBudget.fileMembers, axisBudget.payloadBytes);
        const auto childBinding = std::string("v4_virtual_child_v1:") + std::string(exactCaseId);
        const auto child = VerifyV4AttemptStart(childPreBytes, childFlightBytes, childBinding, exactCaseId,
            executionId, coordinateBudget, axisBudget, tokenBudget, caseByteBudget, childByteBudget, replayWorkBudget);
        if (replay.status != V4StructuredCaseStatus::unexecuted || plan.status != V4ExactOwnedFilesStatus::planned ||
            child.status != V4AttemptTransitionStatus::consistent_unverified)
        { out.status = replay.status == V4StructuredCaseStatus::resource_unavailable || plan.status == V4ExactOwnedFilesStatus::resource_unavailable ||
              child.status == V4AttemptTransitionStatus::resource_unavailable ? V4AttemptTransitionStatus::resource_unavailable : V4AttemptTransitionStatus::unavailable;
          out.reason = "v4_exact_parent_case_or_child_unavailable"; return out; }
        State expected = pre.state; std::string bits; for (bool bit : replay.present) bits += bit ? '1' : '0';
        CaseRecord* chosen = nullptr; std::size_t matches = 0;
        for (std::size_t f = 0; f < expected.families.size(); ++f)
        {
            auto& family = expected.families[f];
            if (family.state == WorkState::in_flight) { out.reason = "v4_exact_parent_other_inflight"; return out; }
            for (std::size_t i = 0; i < family.cases.size(); ++i)
            {
                auto& item = family.cases[i];
                if (item.state == WorkState::in_flight || item.executionId == executionId ||
                    std::any_of(item.history.begin(), item.history.end(), [&](const auto& h) { return h.executionId == executionId; }))
                { out.reason = "v4_exact_parent_inflight_or_execution_reused"; return out; }
                if (family.coordinate != replay.familyCoordinate || item.candidateId != replay.candidateId || item.graphId != replay.graphId ||
                    item.graphMode != (replay.graphMode == "ole" ? "v4_ole" : "v4_native") || item.presence != bits) continue;
                const auto candidate = ReplayV4Case(family, i, coordinateBudget, axisBudget, replayWorkBudget);
                if ((item.state != WorkState::pending && item.state != WorkState::resource_unavailable) || !item.executionId.empty() ||
                    candidate.status != V4CaseReplay::Status::ready || !candidate.unexecutedRecord || !candidate.errors.empty())
                { out.status = candidate.status == V4CaseReplay::Status::resource_unavailable ? V4AttemptTransitionStatus::resource_unavailable : V4AttemptTransitionStatus::unavailable;
                  out.reason = "v4_exact_parent_case_unavailable"; return out; }
                chosen = &item; ++matches; out.familyIndex = f; out.caseIndex = i;
            }
        }
        if (matches != 1 || !chosen || std::any_of(expected.exactAttempts.begin(), expected.exactAttempts.end(),
            [&](const auto& value) { return value.caseId == exactCaseId || value.executionId == executionId; }))
        { out.reason = "v4_exact_parent_case_ambiguous"; return out; }
        const auto hexId = [](const FILE_ID_INFO& id) {
            constexpr char digits[] = "0123456789abcdef"; std::string value; value.reserve(32);
            for (auto byte : id.FileId.Identifier) { value.push_back(digits[byte >> 4]); value.push_back(digits[byte & 15]); }
            return value;
        };
        V4ExactAttemptAudit audit; audit.caseId = exactCaseId; audit.executionId = executionId;
        audit.preBytes = childPreBytes; audit.flightBytes = childFlightBytes;
        for (auto ch : sourcePath.native())
        {
            const auto unit = static_cast<std::uint16_t>(ch); constexpr char digits[] = "0123456789abcdef";
            for (unsigned shift : {0u, 8u}) { const auto byte = (unit >> shift) & 255u;
                audit.sourcePathUtf16Hex.push_back(digits[byte >> 4]); audit.sourcePathUtf16Hex.push_back(digits[byte & 15]); }
        }
        for (wchar_t ch : previousName) { if (ch > 127) return out; audit.previousName.push_back(static_cast<char>(ch)); }
        audit.previousVolume = std::to_string(previousId.VolumeSerialNumber); audit.previousFileId = hexId(previousId);
        for (std::size_t i = 0; i < ids.size(); ++i)
        { audit.volumes[i] = std::to_string(ids[i].VolumeSerialNumber); audit.fileIds[i] = hexId(ids[i]); }
        chosen->state = WorkState::in_flight; chosen->executionId = executionId;
        expected.exactAttempts.push_back(std::move(audit));
        const auto encoded = EncodeCheckpoint(expected, binding, checkpointByteBudget);
        if (encoded.status != CodecStatus::encoded)
        { out.status = encoded.status == CodecStatus::resource_unavailable ? V4AttemptTransitionStatus::resource_unavailable : V4AttemptTransitionStatus::unavailable;
          out.reason = "v4_exact_parent_encoding_unavailable"; return out; }
        if (encoded.bytes != parentHeadBytes) { out.reason = "v4_exact_parent_transition_mismatch"; return out; }
        out.status = V4AttemptTransitionStatus::consistent_unverified;
        out.reason = "v4_exact_parent_transition_only_not_execution_proof"; return out;
    }
    catch (const std::bad_alloc&) { out.status = V4AttemptTransitionStatus::resource_unavailable; out.reason = "v4_exact_parent_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.status = V4AttemptTransitionStatus::resource_unavailable; out.reason = "v4_exact_parent_resource_unavailable"; return out; }
    catch (...) { out.status = V4AttemptTransitionStatus::unavailable; out.reason = "v4_exact_parent_observation_unavailable"; return out; }
}

inline bool V4ExactOwnedFilesPermit::ParentLedgerLive() const noexcept
{
    try
    {
        if (!parentLedgerBound_ || GetCurrentThreadId() != thread_ || parentProcess_ == INVALID_HANDLE_VALUE ||
            WaitForSingleObject(parentProcess_, 0) != WAIT_TIMEOUT || !RootsLive() ||
            !DirectoryMatches(ledgerPins_[0], ledgerIds_[0]) || !DirectoryMatches(ledgerPins_[1], ledgerIds_[1]) ||
            !BytesMatch(ledgerPins_[2], ledgerIds_[2], ledgerPreBytes_) || !BytesMatch(ledgerPins_[3], ledgerIds_[3], ledgerHeadBytes_)) return false;
        const auto parent = HandlePath(ledgerPins_[0]), store = HandlePath(ledgerPins_[1]);
        const auto pre = HandlePath(ledgerPins_[2]), head = HandlePath(ledgerPins_[3]), isolation = HandlePath(pins_[0]);
        return DirectChild(parent, store) && DirectChild(store, pre) && DirectChild(store, head) && pre != head &&
            parent != isolation && store != isolation &&
            !(parent.size() > isolation.size() && parent.starts_with(isolation) && parent[isolation.size()] == L'\\') &&
            !(store.size() > isolation.size() && store.starts_with(isolation) && store[isolation.size()] == L'\\');
    }
    catch (...) { return false; }
}

inline bool V4ExactOwnedFilesPermit::BindVerifiedParentLedger(InFlightReceipt& receipt,
    const std::array<HANDLE, 4>& handles, const std::array<FILE_ID_INFO, 4>& ids,
    std::string parentPreBytes, std::string parentHeadBytes, std::wstring previousName,
    std::string childPreBytes, std::string childFlightBytes, std::string_view parentBinding,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
    std::size_t tokenBudget, std::size_t caseByteBudget, std::size_t replayWorkBudget,
    std::size_t childByteBudget, std::size_t checkpointByteBudget) noexcept
{
    try
    {
        if (!ready_ || parentLedgerBound_ || used_.load(std::memory_order_acquire) || GetCurrentThreadId() != thread_ ||
            !receipt.lineage_ || receipt.lineage_.get() != lineage_.get() || receipt.lineage_.owner_before(lineage_) ||
            lineage_.owner_before(receipt.lineage_) || receipt.executionId_ != executionId_ || receipt.ownerThread_ != thread_ ||
            receipt.consumed_.load(std::memory_order_acquire) || !SameId(receipt.rootId_, ids_[0]) ||
            !RootsLive() || WaitForSingleObject(parentProcess_, 0) != WAIT_TIMEOUT || parentBinding.empty() ||
            parentPreBytes.empty() || parentHeadBytes.empty() || parentPreBytes.size() > checkpointByteBudget ||
            parentHeadBytes.size() > checkpointByteBudget || childPreBytes.size() > childByteBudget || childFlightBytes.size() > childByteBudget)
            return false;
        if (!receipt.TryBeginExecution(FamilyVersion::v4)) return false;
        const auto release = [](InFlightReceipt* held) noexcept { held->EndExecution(); };
        std::unique_ptr<InFlightReceipt, decltype(release)> guard(&receipt, release);
        if (receipt.consumed_.load(std::memory_order_acquire) || lineage_->sealed.load(std::memory_order_acquire) ||
            lineage_->commitStarted.load(std::memory_order_acquire) ||
            !BytesMatch(receipt.prePinned_, receipt.preId_, childPreBytes) || !BytesMatch(receipt.pinned_, receipt.publishedId_, childFlightBytes)) return false;
        std::array<HANDLE, 4> pins{INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
        struct PinsGuard { std::array<HANDLE, 4>& pins; ~PinsGuard() { for (auto h : pins) if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } } pinsGuard{pins};
        for (std::size_t i = 0; i < pins.size(); ++i)
        {
            pins[i] = ReOpenFile(handles[i], i < 2 ? FILE_READ_ATTRIBUTES : GENERIC_READ,
                i < 2 ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ,
                i < 2 ? FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT : FILE_FLAG_OPEN_REPARSE_POINT);
            if (pins[i] == INVALID_HANDLE_VALUE) return false;
        }
        if (!DirectoryMatches(pins[0], ids[0]) || !DirectoryMatches(pins[1], ids[1]) ||
            !BytesMatch(pins[2], ids[2], parentPreBytes) || !BytesMatch(pins[3], ids[3], parentHeadBytes)) return false;
        const auto parent = HandlePath(pins[0]), store = HandlePath(pins[1]);
        const auto pre = HandlePath(pins[2]), head = HandlePath(pins[3]), isolation = HandlePath(pins_[0]);
        if (!DirectChild(parent, store) || !DirectChild(store, pre) || !DirectChild(store, head) || pre == head ||
            pre != store + L"\\" + previousName || parent == isolation || store == isolation ||
            (parent.size() > isolation.size() && parent.starts_with(isolation) && parent[isolation.size()] == L'\\') ||
            (store.size() > isolation.size() && store.starts_with(isolation) && store[isolation.size()] == L'\\')) return false;
        const std::array<FILE_ID_INFO, 6> childIds{ids_[0], ids_[1], ids_[2], receipt.directoryId_, receipt.preId_, receipt.publishedId_};
        const auto checked = VerifyV4ExactParentAttempt(parentPreBytes, parentHeadBytes, parentBinding, caseId_, executionId_,
            sourcePath_, creationRoot_, childIds, childPreBytes, childFlightBytes, previousName, ids[2], coordinateBudget,
            axisBudget, tokenBudget, caseByteBudget, replayWorkBudget, childByteBudget, checkpointByteBudget);
        if (checked.status != V4AttemptTransitionStatus::consistent_unverified || !RootsLive() ||
            WaitForSingleObject(parentProcess_, 0) != WAIT_TIMEOUT) return false;
        ledgerIds_ = ids; ledgerPreBytes_ = std::move(parentPreBytes); ledgerHeadBytes_ = std::move(parentHeadBytes);
        ledgerPins_ = pins; pins.fill(INVALID_HANDLE_VALUE); parentLedgerBound_ = true;
        if (!ParentLedgerLive())
        {
            parentLedgerBound_ = false; ledgerPreBytes_.clear(); ledgerHeadBytes_.clear();
            for (auto& h : ledgerPins_) { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); h = INVALID_HANDLE_VALUE; }
            return false;
        }
        return true;
    }
    catch (...) { return false; }
}

inline ExactAttemptCommitResult CheckpointStore::PublishExactAttempt(std::wstring_view preHeadName,
    const FILE_ID_INFO& preHeadId, std::string_view binding, const std::shared_ptr<const ExactAttemptProof>& proof,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
    std::size_t tokenBudget, std::size_t caseByteBudget, std::size_t replayWorkBudget,
    std::size_t childByteBudget, std::size_t checkpointByteBudget) noexcept
{
    ExactAttemptCommitResult out;
    try
    {
        const auto canonical = proof ? proof->weak_from_this().lock() : std::shared_ptr<const ExactAttemptProof>{};
        if (proof && (proof->preBytes_.size() > childByteBudget || proof->flightBytes_.size() > childByteBudget))
        { out.resourceUnavailable = true; out.reason = "v4_exact_attempt_child_byte_budget_unavailable"; return out; }
        if (!proof || canonical.get() != proof.get() || canonical.owner_before(proof) || proof.owner_before(canonical) ||
            !proof->FilesMatch(childByteBudget) || !StillOwned() || !ValidName(preHeadName, L"checkpoint-"))
        { out.reason = "v4_exact_attempt_proof_unavailable"; return out; }
        auto committed = std::unique_ptr<CommittedExactAttempt>(new CommittedExactAttempt());
        const auto path = directoryPath_ + L"\\" + std::wstring(preHeadName);
        committed->pre_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        FILE_ID_INFO preId{};
        if (committed->pre_ == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(committed->pre_, FileIdInfo, &preId, sizeof(preId)) || !SameIdentity(preId, preHeadId))
        { out.reason = "v4_exact_attempt_pre_pin_unavailable"; return out; }
        const auto pre = Read(preHeadName, preHeadId, binding, checkpointByteBudget);
        if (pre.status != CodecStatus::decoded || pre.recoveredInFlight || pre.state.familyVersion != FamilyVersion::v4 || !pre.state.v4TypedEnabled)
        { out.resourceUnavailable = pre.status == CodecStatus::resource_unavailable; out.reason = "v4_exact_attempt_pre_head_unavailable"; return out; }
        const auto replay = ReplayV4StructuredCaseId(proof->caseId_, coordinateBudget, axisBudget, tokenBudget, caseByteBudget);
        const auto plan = PrepareV4ExactOwnedFiles(proof->caseId_, coordinateBudget, axisBudget, tokenBudget, caseByteBudget,
            proof->sourcePath_, proof->handles_[2], proof->ids_[2], axisBudget.fileMembers, axisBudget.payloadBytes);
        const auto childBinding = std::string("v4_virtual_child_v1:") + proof->caseId_;
        const auto child = VerifyV4AttemptStart(proof->preBytes_, proof->flightBytes_, childBinding, proof->caseId_,
            proof->executionId_, coordinateBudget, axisBudget, tokenBudget, caseByteBudget, childByteBudget, replayWorkBudget);
        if (replay.status != V4StructuredCaseStatus::unexecuted || replay.caseId != proof->caseId_ ||
            plan.status != V4ExactOwnedFilesStatus::planned || child.status != V4AttemptTransitionStatus::consistent_unverified)
        { out.resourceUnavailable = replay.status == V4StructuredCaseStatus::resource_unavailable ||
              plan.status == V4ExactOwnedFilesStatus::resource_unavailable || child.status == V4AttemptTransitionStatus::resource_unavailable;
          out.reason = "v4_exact_attempt_replay_unavailable"; return out; }
        State next = pre.state; std::size_t matches = 0; CaseRecord* selected = nullptr;
        std::string bits; for (bool bit : replay.present) bits += bit ? '1' : '0';
        for (auto& family : next.families)
        {
            if (family.state == WorkState::in_flight) { out.reason = "v4_exact_attempt_other_inflight"; return out; }
            for (std::size_t i = 0; i < family.cases.size(); ++i)
            {
                auto& item = family.cases[i];
                if (item.state == WorkState::in_flight) { out.reason = "v4_exact_attempt_other_inflight"; return out; }
                if (item.executionId == proof->executionId_ ||
                    std::any_of(item.history.begin(), item.history.end(), [&](const auto& history) { return history.executionId == proof->executionId_; }))
                { out.reason = "v4_exact_attempt_execution_id_reused"; return out; }
                if (family.coordinate != replay.familyCoordinate || item.candidateId != replay.candidateId || item.graphId != replay.graphId ||
                    item.graphMode != (replay.graphMode == "ole" ? "v4_ole" : "v4_native") || item.presence != bits) continue;
                const auto verified = ReplayV4Case(family, i, coordinateBudget, axisBudget, replayWorkBudget);
                if ((item.state != WorkState::pending && item.state != WorkState::resource_unavailable) || !item.executionId.empty() ||
                    verified.status != V4CaseReplay::Status::ready || !verified.unexecutedRecord || !verified.errors.empty())
                { out.resourceUnavailable = verified.status == V4CaseReplay::Status::resource_unavailable;
                  out.reason = "v4_exact_attempt_parent_case_unavailable"; return out; }
                selected = &item; ++matches;
            }
        }
        if (matches != 1 || !selected || std::any_of(next.exactAttempts.begin(), next.exactAttempts.end(),
            [&](const auto& prior) { return prior.caseId == proof->caseId_ || prior.executionId == proof->executionId_; }))
        { out.reason = "v4_exact_attempt_parent_case_ambiguous"; return out; }
        const auto hexId = [](const FILE_ID_INFO& id) {
            constexpr char digits[] = "0123456789abcdef"; std::string value; value.reserve(32);
            for (auto byte : id.FileId.Identifier) { value.push_back(digits[byte >> 4]); value.push_back(digits[byte & 15]); }
            return value;
        };
        V4ExactAttemptAudit audit; audit.caseId = proof->caseId_; audit.executionId = proof->executionId_;
        audit.preBytes = proof->preBytes_; audit.flightBytes = proof->flightBytes_;
        for (auto ch : proof->sourcePath_.native())
        {
            const auto unit = static_cast<std::uint16_t>(ch); constexpr char digits[] = "0123456789abcdef";
            for (unsigned shift : {0u, 8u}) { const auto byte = (unit >> shift) & 255u;
                audit.sourcePathUtf16Hex.push_back(digits[byte >> 4]); audit.sourcePathUtf16Hex.push_back(digits[byte & 15]); }
        }
        for (wchar_t ch : preHeadName) { if (ch > 127) { out.reason = "v4_exact_attempt_pre_name_unavailable"; return out; } audit.previousName.push_back(static_cast<char>(ch)); }
        audit.previousVolume = std::to_string(preHeadId.VolumeSerialNumber); audit.previousFileId = hexId(preHeadId);
        for (std::size_t i = 0; i < proof->ids_.size(); ++i)
        { audit.volumes[i] = std::to_string(proof->ids_[i].VolumeSerialNumber); audit.fileIds[i] = hexId(proof->ids_[i]); }
        selected->state = WorkState::in_flight; selected->executionId = proof->executionId_;
        next.exactAttempts.push_back(std::move(audit));
        const auto expected = EncodeCheckpoint(next, binding, checkpointByteBudget);
        if (expected.status != CodecStatus::encoded || !proof->FilesMatch(childByteBudget) || !StillOwned())
        { out.resourceUnavailable = expected.status == CodecStatus::resource_unavailable; out.reason = "v4_exact_attempt_encoding_unavailable"; return out; }
        const auto again = Read(preHeadName, preHeadId, binding, checkpointByteBudget);
        if (again.status != CodecStatus::decoded || again.bytes != pre.bytes || again.recoveredInFlight)
        { out.reason = "v4_exact_attempt_pre_changed"; return out; }
        committed->proof_ = proof; committed->thread_ = GetCurrentThreadId(); committed->rootId_ = locator_.rootId;
        committed->directoryId_ = locator_.directoryId; committed->preName_ = preHeadName; committed->preId_ = preHeadId;
        committed->preBytes_ = pre.bytes; committed->headBytes_ = expected.bytes;
        out.checkpoint = Publish(next, binding, checkpointByteBudget);
        if (out.checkpoint.status != StoreStatus::published_durability_unverified || !out.checkpoint.fileIdentityCaptured || !out.checkpoint.renameReturnedSuccess)
        { out.reason = "v4_exact_attempt_publication_unavailable"; return out; }
        const auto headPath = directoryPath_ + L"\\" + out.checkpoint.fileName;
        committed->head_ = CreateFileW(headPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        FILE_ID_INFO headId{};
        if (committed->head_ == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(committed->head_, FileIdInfo, &headId, sizeof(headId)) ||
            !SameIdentity(headId, out.checkpoint.fileId)) { out.reason = "v4_exact_attempt_head_pin_unavailable"; return out; }
        committed->headName_ = out.checkpoint.fileName; committed->headId_ = out.checkpoint.fileId;
        if (!VerifyExactAttempt(*committed, proof, committed->headName_, committed->headId_, binding, coordinateBudget, axisBudget,
            tokenBudget, caseByteBudget, replayWorkBudget, childByteBudget, checkpointByteBudget))
        { out.reason = "v4_exact_attempt_readback_unavailable"; return out; }
        out.resultingState = std::move(next); out.committed = std::move(committed);
        out.reason = "v4_exact_attempt_recorded_not_execution_proof"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_exact_attempt_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_exact_attempt_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_exact_attempt_observation_unavailable"; return out; }
}

inline bool CheckpointStore::VerifyExactAttempt(const CommittedExactAttempt& committed,
    const std::shared_ptr<const ExactAttemptProof>& proof, std::wstring_view trustedHeadName,
    const FILE_ID_INFO& trustedHeadId, std::string_view binding, const V4CoordinateBudget& coordinateBudget,
    const V4AxisBudget& axisBudget, std::size_t tokenBudget, std::size_t caseByteBudget,
    std::size_t replayWorkBudget, std::size_t childByteBudget, std::size_t checkpointByteBudget) noexcept
{
    try
    {
        if (!proof || !committed.proof_ || proof.get() != committed.proof_.get() || proof.owner_before(committed.proof_) ||
            committed.proof_.owner_before(proof) || committed.thread_ != GetCurrentThreadId() || !proof->FilesMatch(childByteBudget) ||
            !StillOwned() || !SameIdentity(committed.rootId_, locator_.rootId) || !SameIdentity(committed.directoryId_, locator_.directoryId) ||
            trustedHeadName != committed.headName_ || !SameIdentity(trustedHeadId, committed.headId_)) return false;
        FILE_ID_INFO headId{}, preId{};
        if (!GetFileInformationByHandleEx(committed.head_, FileIdInfo, &headId, sizeof(headId)) || !SameIdentity(headId, committed.headId_) ||
            !GetFileInformationByHandleEx(committed.pre_, FileIdInfo, &preId, sizeof(preId)) || !SameIdentity(preId, committed.preId_)) return false;
        const auto head = Read(trustedHeadName, trustedHeadId, binding, checkpointByteBudget);
        const auto pre = Read(committed.preName_, committed.preId_, binding, checkpointByteBudget);
        if (head.status != CodecStatus::decoded || head.bytes != committed.headBytes_ || pre.status != CodecStatus::decoded ||
            pre.recoveredInFlight || pre.bytes != committed.preBytes_) return false;
        const auto exact = DecodeCheckpoint(head.bytes, binding, checkpointByteBudget);
        if (exact.status != CodecStatus::decoded || exact.state.exactAttempts.size() != pre.state.exactAttempts.size() + 1) return false;
        const auto& record = exact.state.exactAttempts.back();
        if (record.caseId != proof->caseId_ || record.executionId != proof->executionId_ ||
            record.preBytes != proof->preBytes_ || record.flightBytes != proof->flightBytes_) return false;
        const auto parentTransition = VerifyV4ExactParentAttempt(pre.bytes, head.bytes, binding, proof->caseId_, proof->executionId_,
            proof->sourcePath_, proof->handles_[2], proof->ids_, proof->preBytes_, proof->flightBytes_, committed.preName_,
            committed.preId_, coordinateBudget, axisBudget, tokenBudget, caseByteBudget, replayWorkBudget, childByteBudget, checkpointByteBudget);
        if (parentTransition.status != V4AttemptTransitionStatus::consistent_unverified) return false;
        const auto plan = PrepareV4ExactOwnedFiles(proof->caseId_, coordinateBudget, axisBudget, tokenBudget, caseByteBudget,
            proof->sourcePath_, proof->handles_[2], proof->ids_[2], axisBudget.fileMembers, axisBudget.payloadBytes);
        const auto childBinding = std::string("v4_virtual_child_v1:") + proof->caseId_;
        const auto transition = VerifyV4AttemptStart(proof->preBytes_, proof->flightBytes_, childBinding, proof->caseId_,
            proof->executionId_, coordinateBudget, axisBudget, tokenBudget, caseByteBudget, childByteBudget, replayWorkBudget);
        return plan.status == V4ExactOwnedFilesStatus::planned &&
            transition.status == V4AttemptTransitionStatus::consistent_unverified && proof->FilesMatch(childByteBudget) && StillOwned();
    }
    catch (...) { return false; }
}

struct V4ExactRecoveryInspection
{
    V4AttemptTransitionStatus status = V4AttemptTransitionStatus::unavailable;
    // This is a recovery disposition, never a claim of execution or PASS.
    bool sameCaseOutcomeUnknown = true;
    bool automaticRetryAuthorized = false;
    std::string_view reason;
};

// External trust must supply both locator IDs and complete expected bytes.
// Passing bytes read from the same untrusted checkpoint is not authentication.
// Replayed in-flight attempts deliberately remain unknown after process loss.
inline V4ExactRecoveryInspection InspectV4ExactRecovery(CheckpointStore& store,
    std::wstring_view headName, const FILE_ID_INFO& headId, std::string_view trustedHeadBytes,
    std::wstring_view previousName, const FILE_ID_INFO& previousId, std::string_view trustedPreviousBytes,
    std::string_view binding, std::string_view exactCaseId, std::string_view executionId,
    const std::filesystem::path& sourcePath, HANDLE sourceRoot, const std::array<FILE_ID_INFO, 6>& ids,
    std::string_view childPreBytes, std::string_view childFlightBytes,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
    std::size_t tokenBudget, std::size_t caseByteBudget, std::size_t replayWorkBudget,
    std::size_t childByteBudget, std::size_t checkpointByteBudget) noexcept
{
    V4ExactRecoveryInspection out;
    try
    {
        if (trustedHeadBytes.empty() || trustedPreviousBytes.empty())
        { out.reason = "v4_exact_recovery_external_head_bytes_required"; return out; }
        if (trustedHeadBytes.size() > checkpointByteBudget || trustedPreviousBytes.size() > checkpointByteBudget ||
            childPreBytes.size() > childByteBudget || childFlightBytes.size() > childByteBudget)
        { out.status = V4AttemptTransitionStatus::resource_unavailable; out.reason = "v4_exact_recovery_byte_budget_unavailable"; return out; }
        auto headPin = store.PinRead(headName, headId), prePin = store.PinRead(previousName, previousId);
        if (!headPin || !prePin) { out.reason = "v4_exact_recovery_file_pin_unavailable"; return out; }
        const auto head = store.Read(headName, headId, binding, checkpointByteBudget);
        const auto pre = store.Read(previousName, previousId, binding, checkpointByteBudget);
        if (head.status != CodecStatus::decoded || pre.status != CodecStatus::decoded ||
            head.bytes != trustedHeadBytes || pre.bytes != trustedPreviousBytes || pre.recoveredInFlight)
        { out.status = head.status == CodecStatus::resource_unavailable || pre.status == CodecStatus::resource_unavailable ?
              V4AttemptTransitionStatus::resource_unavailable : V4AttemptTransitionStatus::unavailable;
          out.reason = "v4_exact_recovery_external_head_mismatch"; return out; }
        const auto transition = VerifyV4ExactParentAttempt(pre.bytes, head.bytes, binding, exactCaseId, executionId,
            sourcePath, sourceRoot, ids, childPreBytes, childFlightBytes, previousName, previousId, coordinateBudget,
            axisBudget, tokenBudget, caseByteBudget, replayWorkBudget, childByteBudget, checkpointByteBudget);
        if (transition.status != V4AttemptTransitionStatus::consistent_unverified)
        { out.status = transition.status; out.reason = transition.reason; return out; }
        const auto headAgain = store.Read(headName, headId, binding, checkpointByteBudget);
        const auto preAgain = store.Read(previousName, previousId, binding, checkpointByteBudget);
        if (headAgain.status != CodecStatus::decoded || preAgain.status != CodecStatus::decoded ||
            headAgain.bytes != trustedHeadBytes || preAgain.bytes != trustedPreviousBytes)
        { out.reason = "v4_exact_recovery_recheck_unavailable"; return out; }
        out.status = V4AttemptTransitionStatus::consistent_unverified;
        out.reason = "v4_exact_recovery_inflight_outcome_unknown_no_retry";
        return out;
    }
    catch (const std::bad_alloc&) { out.status = V4AttemptTransitionStatus::resource_unavailable; out.reason = "v4_exact_recovery_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.status = V4AttemptTransitionStatus::resource_unavailable; out.reason = "v4_exact_recovery_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_exact_recovery_observation_unavailable"; return out; }
}

inline OwnedAssociationCommitResult CheckpointStore::PublishOwnedAssociation(std::wstring_view preHeadName,
    const FILE_ID_INFO& preHeadId, std::string_view binding, const std::shared_ptr<const OwnedAssociationProof>& proof,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
    std::size_t tokenBudget, std::size_t caseByteBudget, std::size_t replayWorkBudget,
    std::size_t checkpointByteBudget) noexcept
{
    OwnedAssociationCommitResult out;
    try
    {
        const auto canonicalProof = proof ? proof->weak_from_this().lock() : std::shared_ptr<const OwnedAssociationProof>{};
        if (!proof || canonicalProof != proof || canonicalProof.owner_before(proof) || proof.owner_before(canonicalProof) ||
            !proof->Live() || !StillOwned()) { out.reason = "owned_association_proof_unavailable"; return out; }
        const auto pre = Read(preHeadName, preHeadId, binding, checkpointByteBudget);
        if (pre.status != CodecStatus::decoded || pre.recoveredInFlight || pre.state.familyVersion != FamilyVersion::v4 || !pre.state.v4TypedEnabled)
        { out.resourceUnavailable = pre.status == CodecStatus::resource_unavailable; out.reason = "owned_association_pre_head_unavailable"; return out; }
        const auto templ = ReplayV4StructuredCaseId(proof->templateCase_, coordinateBudget, axisBudget, tokenBudget, caseByteBudget);
        const auto owned = ReplayV4StructuredCaseId(proof->ownedCase_, coordinateBudget, axisBudget, tokenBudget, caseByteBudget);
        if (templ.status != V4StructuredCaseStatus::unexecuted || owned.status != V4StructuredCaseStatus::unexecuted ||
            !templ.family || !templ.candidate || !templ.oleGraph || !owned.family || !owned.candidate || !owned.oleGraph ||
            templ.caseId != proof->templateCase_ || owned.caseId != proof->ownedCase_)
        { out.resourceUnavailable = templ.status == V4StructuredCaseStatus::resource_unavailable ||
              owned.status == V4StructuredCaseStatus::resource_unavailable;
          out.reason = "owned_association_case_replay_unavailable"; return out; }
        std::size_t matches = 0;
        for (const auto& family : pre.state.families)
        {
            if (family.state == WorkState::in_flight) { out.reason = "owned_association_inflight_unavailable"; return out; }
            for (std::size_t i = 0; i < family.cases.size(); ++i)
            {
                const auto& item = family.cases[i];
                if (item.state == WorkState::in_flight) { out.reason = "owned_association_inflight_unavailable"; return out; }
                if (family.coordinate != templ.familyCoordinate || item.candidateId != templ.candidateId ||
                    item.graphId != templ.graphId || item.graphMode != "v4_ole") continue;
                std::string bits; for (bool bit : templ.present) bits += bit ? '1' : '0';
                if (item.presence != bits) continue;
                if ((item.state != WorkState::pending && item.state != WorkState::resource_unavailable) || !item.executionId.empty())
                { out.reason = "owned_association_template_not_pending"; return out; }
                const auto replay = ReplayV4Case(family, i, coordinateBudget, axisBudget, replayWorkBudget);
                if (replay.status != V4CaseReplay::Status::ready || !replay.unexecutedRecord || !replay.errors.empty())
                { out.resourceUnavailable = replay.status == V4CaseReplay::Status::resource_unavailable;
                  out.reason = "owned_association_template_record_unavailable"; return out; }
                ++matches;
            }
        }
        if (matches != 1) { out.reason = "owned_association_template_identity_ambiguous"; return out; }
        for (const auto& existing : pre.state.ownedAssociations)
            if (existing.templateCaseId == proof->templateCase_ && existing.ownedCaseId == proof->ownedCase_)
            { out.reason = "owned_association_already_recorded"; return out; }
        const auto hexId = [](const FILE_ID_INFO& id) {
            constexpr char digits[] = "0123456789abcdef"; std::string hex; hex.reserve(32);
            for (const auto byte : id.FileId.Identifier) { hex.push_back(digits[byte >> 4]); hex.push_back(digits[byte & 15]); }
            return hex;
        };
        OwnedCaseAssociation association{proof->templateCase_, proof->ownedCase_,
            std::to_string(proof->profileId_.VolumeSerialNumber), hexId(proof->profileId_),
            std::to_string(proof->sourceId_.VolumeSerialNumber), hexId(proof->sourceId_)};
        State next = pre.state; next.ownedAssociations.push_back(std::move(association));
        const auto expected = EncodeCheckpoint(next, binding, checkpointByteBudget);
        if (expected.status != CodecStatus::encoded || !proof->Live() || !StillOwned())
        { out.resourceUnavailable = expected.status == CodecStatus::resource_unavailable; out.reason = "owned_association_encoding_unavailable"; return out; }
        auto committed = std::unique_ptr<CommittedOwnedAssociation>(new CommittedOwnedAssociation());
        committed->proof_ = proof; committed->thread_ = GetCurrentThreadId();
        committed->rootId_ = locator_.rootId; committed->directoryId_ = locator_.directoryId;
        out.checkpoint = Publish(next, binding, checkpointByteBudget);
        if (out.checkpoint.status != StoreStatus::published_durability_unverified || !out.checkpoint.fileIdentityCaptured ||
            !out.checkpoint.renameReturnedSuccess)
        { out.reason = "owned_association_publication_unavailable"; return out; }
        const auto path = directoryPath_ + L"\\" + out.checkpoint.fileName;
        committed->head_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        FILE_ID_INFO pinned{}; FILE_ATTRIBUTE_TAG_INFO tag{};
        if (committed->head_ == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(committed->head_, FileIdInfo, &pinned, sizeof(pinned)) ||
            !SameIdentity(pinned, out.checkpoint.fileId) ||
            !GetFileInformationByHandleEx(committed->head_, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        { out.reason = "owned_association_published_pin_unavailable"; return out; }
        const auto readback = Read(out.checkpoint.fileName, out.checkpoint.fileId, binding, checkpointByteBudget);
        const auto encoded = readback.status == CodecStatus::decoded && !readback.recoveredInFlight ?
            EncodeCheckpoint(readback.state, binding, checkpointByteBudget) : CodecResult{};
        if (encoded.status != CodecStatus::encoded || encoded.bytes != expected.bytes || !proof->Live() || !StillOwned())
        { out.reason = "owned_association_readback_unavailable"; return out; }
        committed->headName_ = out.checkpoint.fileName; committed->headId_ = out.checkpoint.fileId;
        out.resultingState = readback.state; out.committed = std::move(committed);
        out.reason = "owned_association_published_durability_unverified"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "owned_association_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "owned_association_resource_unavailable"; return out; }
    catch (...) { out.reason = "owned_association_observation_unavailable"; return out; }
}

inline bool CheckpointStore::VerifyOwnedAssociation(const CommittedOwnedAssociation& committed,
    const std::shared_ptr<const OwnedAssociationProof>& proof, std::wstring_view trustedHeadName,
    const FILE_ID_INFO& trustedHeadId, std::string_view binding, std::size_t checkpointByteBudget) noexcept
{
    try
    {
        if (!proof || !committed.proof_ || proof != committed.proof_ || proof.owner_before(committed.proof_) ||
            committed.proof_.owner_before(proof) || !proof->Live() || GetCurrentThreadId() != committed.thread_ ||
            !StillOwned() || !SameIdentity(locator_.rootId, committed.rootId_) ||
            !SameIdentity(locator_.directoryId, committed.directoryId_) || trustedHeadName != committed.headName_ ||
            !SameIdentity(trustedHeadId, committed.headId_)) return false;
        FILE_ID_INFO id{};
        if (committed.head_ == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(committed.head_, FileIdInfo, &id, sizeof(id)) ||
            !SameIdentity(id, committed.headId_)) return false;
        const auto readback = Read(trustedHeadName, trustedHeadId, binding, checkpointByteBudget);
        if (readback.status != CodecStatus::decoded || readback.recoveredInFlight) return false;
        const auto hexId = [](const FILE_ID_INFO& value) {
            constexpr char digits[] = "0123456789abcdef"; std::string hex; hex.reserve(32);
            for (const auto byte : value.FileId.Identifier) { hex.push_back(digits[byte >> 4]); hex.push_back(digits[byte & 15]); }
            return hex;
        };
        std::size_t matches = 0;
        for (const auto& association : readback.state.ownedAssociations)
            if (association.templateCaseId == proof->templateCase_ && association.ownedCaseId == proof->ownedCase_ &&
                association.profileVolume == std::to_string(proof->profileId_.VolumeSerialNumber) &&
                association.profileFileId == hexId(proof->profileId_) &&
                association.sourceVolume == std::to_string(proof->sourceId_.VolumeSerialNumber) &&
                association.sourceFileId == hexId(proof->sourceId_)) ++matches;
        return matches == 1 && proof->Live() && StillOwned();
    }
    catch (...) { return false; }
}

inline OwnedTerminalCapacity CheckpointStore::InspectOwnedTerminalCapacity(const CommittedOwnedAssociation& association,
    const std::shared_ptr<const OwnedAssociationProof>& proof, std::wstring_view trustedHeadName,
    const FILE_ID_INFO& trustedHeadId, std::string_view binding, std::string_view executionId,
    std::size_t childCheckpointByteBudget, std::size_t parentCheckpointByteBudget) noexcept
{
    OwnedTerminalCapacity out;
    try
    {
        if (!proof || executionId.empty() || !VerifyOwnedAssociation(association, proof, trustedHeadName,
            trustedHeadId, binding, parentCheckpointByteBudget))
        { out.reason = "owned_terminal_capacity_association_unavailable"; return out; }
        if (!childCheckpointByteBudget || childCheckpointByteBudget > parentCheckpointByteBudget)
        { out.resourceUnavailable = true; out.reason = "owned_terminal_capacity_budget_unavailable"; return out; }
        const auto head = Read(trustedHeadName, trustedHeadId, binding, parentCheckpointByteBudget);
        if (head.status != CodecStatus::decoded || head.recoveredInFlight)
        { out.resourceUnavailable = head.status == CodecStatus::resource_unavailable; out.reason = "owned_terminal_capacity_head_unavailable"; return out; }
        for (const auto& terminal : head.state.ownedTerminals)
            if (terminal.ownedCaseId == proof->ownedCase_ && terminal.executionId == executionId)
            { out.reason = "owned_terminal_capacity_execution_already_recorded"; return out; }
        State upper = head.state;
        OwnedCaseTerminal placeholder;
        placeholder.templateCaseId = proof->templateCase_; placeholder.ownedCaseId = proof->ownedCase_;
        placeholder.executionId = executionId; placeholder.terminalBytes.assign(childCheckpointByteBudget, '\0');
        for (std::size_t i = 0; i < placeholder.fileIds.size(); ++i)
        { placeholder.volumes[i] = "18446744073709551615"; placeholder.fileIds[i] = std::string(32, 'f'); }
        upper.ownedTerminals.push_back(std::move(placeholder));
        const auto encoded = EncodeCheckpoint(upper, binding, parentCheckpointByteBudget);
        if (encoded.status != CodecStatus::encoded)
        { out.resourceUnavailable = encoded.status == CodecStatus::resource_unavailable;
          out.reason = "owned_terminal_capacity_budget_unavailable"; return out; }
        if (!VerifyOwnedAssociation(association, proof, trustedHeadName, trustedHeadId, binding, parentCheckpointByteBudget))
        { out.reason = "owned_terminal_capacity_head_changed"; return out; }
        out.sufficient = true; out.maximumEncodedBytes = encoded.bytes.size();
        out.reason = "owned_terminal_capacity_only_not_execution"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "owned_terminal_capacity_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "owned_terminal_capacity_resource_unavailable"; return out; }
    catch (...) { out.reason = "owned_terminal_capacity_observation_unavailable"; return out; }
}

inline OwnedTerminalCommitResult CheckpointStore::PublishOwnedTerminal(std::wstring_view preHeadName,
    const FILE_ID_INFO& preHeadId, std::string_view binding, const CommittedOwnedAssociation& association,
    const std::shared_ptr<const OwnedTerminalProof>& proof, const V4CoordinateBudget& coordinateBudget,
    const V4AxisBudget& axisBudget, std::size_t tokenBudget, std::size_t caseByteBudget,
    std::size_t childCheckpointByteBudget, std::size_t replayWorkBudget, std::size_t parentCheckpointByteBudget) noexcept
{
    OwnedTerminalCommitResult out;
    try
    {
        const auto canonical = proof ? proof->weak_from_this().lock() : std::shared_ptr<const OwnedTerminalProof>{};
        if (!proof || canonical != proof || canonical.owner_before(proof) || proof.owner_before(canonical) ||
            !proof->association_ || !VerifyOwnedAssociation(association, proof->association_, preHeadName, preHeadId,
                binding, parentCheckpointByteBudget))
        { out.reason = "owned_terminal_association_unavailable"; return out; }
        if (proof->preBytes_.size() > childCheckpointByteBudget || proof->flightBytes_.size() > childCheckpointByteBudget ||
            proof->terminalBytes_.size() > childCheckpointByteBudget)
        { out.resourceUnavailable = true; out.reason = "owned_terminal_child_budget_unavailable"; return out; }
        if (!proof->FilesMatch(childCheckpointByteBudget)) { out.reason = "owned_terminal_child_files_unavailable"; return out; }
        const std::string childBinding = std::string("v4_virtual_child_v1:") + proof->association_->ownedCase_;
        const auto transition = VerifyV4AttemptTransition(proof->preBytes_, proof->flightBytes_, proof->terminalBytes_,
            childBinding, proof->association_->ownedCase_, proof->executionId_, coordinateBudget, axisBudget,
            tokenBudget, caseByteBudget, childCheckpointByteBudget, replayWorkBudget);
        if (transition.status != V4AttemptTransitionStatus::consistent_unverified)
        { out.resourceUnavailable = transition.status == V4AttemptTransitionStatus::resource_unavailable;
          out.reason = "owned_terminal_transition_unavailable"; return out; }
        const auto pre = Read(preHeadName, preHeadId, binding, parentCheckpointByteBudget);
        if (pre.status != CodecStatus::decoded || pre.recoveredInFlight)
        { out.resourceUnavailable = pre.status == CodecStatus::resource_unavailable; out.reason = "owned_terminal_pre_head_unavailable"; return out; }
        for (const auto& terminal : pre.state.ownedTerminals)
            if (terminal.ownedCaseId == proof->association_->ownedCase_ && terminal.executionId == proof->executionId_)
            { out.reason = "owned_terminal_already_recorded"; return out; }
        const auto hexId = [](const FILE_ID_INFO& value) {
            constexpr char digits[] = "0123456789abcdef"; std::string hex; hex.reserve(32);
            for (const auto byte : value.FileId.Identifier) { hex.push_back(digits[byte >> 4]); hex.push_back(digits[byte & 15]); }
            return hex;
        };
        OwnedCaseTerminal terminal;
        terminal.templateCaseId = proof->association_->templateCase_; terminal.ownedCaseId = proof->association_->ownedCase_;
        terminal.executionId = proof->executionId_; terminal.terminalBytes = proof->terminalBytes_;
        for (std::size_t i = 0; i < proof->ids_.size(); ++i)
        { terminal.volumes[i] = std::to_string(proof->ids_[i].VolumeSerialNumber); terminal.fileIds[i] = hexId(proof->ids_[i]); }
        State next = pre.state; next.ownedTerminals.push_back(std::move(terminal));
        const auto expected = EncodeCheckpoint(next, binding, parentCheckpointByteBudget);
        if (expected.status != CodecStatus::encoded || !proof->FilesMatch(childCheckpointByteBudget) || !StillOwned())
        { out.resourceUnavailable = expected.status == CodecStatus::resource_unavailable; out.reason = "owned_terminal_encoding_unavailable"; return out; }
        auto committed = std::unique_ptr<CommittedOwnedTerminal>(new CommittedOwnedTerminal());
        committed->proof_ = proof; committed->thread_ = GetCurrentThreadId();
        committed->rootId_ = locator_.rootId; committed->directoryId_ = locator_.directoryId;
        out.checkpoint = Publish(next, binding, parentCheckpointByteBudget);
        if (out.checkpoint.status != StoreStatus::published_durability_unverified || !out.checkpoint.fileIdentityCaptured ||
            !out.checkpoint.renameReturnedSuccess)
        { out.reason = "owned_terminal_publication_unavailable"; return out; }
        const auto path = directoryPath_ + L"\\" + out.checkpoint.fileName;
        committed->head_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        FILE_ID_INFO id{}; FILE_ATTRIBUTE_TAG_INFO tag{};
        if (committed->head_ == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(committed->head_, FileIdInfo, &id, sizeof(id)) ||
            !SameIdentity(id, out.checkpoint.fileId) || !GetFileInformationByHandleEx(committed->head_, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        { out.reason = "owned_terminal_published_pin_unavailable"; return out; }
        const auto readback = Read(out.checkpoint.fileName, out.checkpoint.fileId, binding, parentCheckpointByteBudget);
        const auto encoded = readback.status == CodecStatus::decoded && !readback.recoveredInFlight ?
            EncodeCheckpoint(readback.state, binding, parentCheckpointByteBudget) : CodecResult{};
        if (encoded.status != CodecStatus::encoded || encoded.bytes != expected.bytes ||
            !proof->FilesMatch(childCheckpointByteBudget) || !StillOwned())
        { out.resourceUnavailable = encoded.status == CodecStatus::resource_unavailable; out.reason = "owned_terminal_readback_unavailable"; return out; }
        committed->headName_ = out.checkpoint.fileName; committed->headId_ = out.checkpoint.fileId;
        out.resultingState = readback.state; out.committed = std::move(committed);
        out.reason = "owned_terminal_checkpoint_consistent_not_product_result"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "owned_terminal_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "owned_terminal_resource_unavailable"; return out; }
    catch (...) { out.reason = "owned_terminal_observation_unavailable"; return out; }
}

inline bool CheckpointStore::VerifyOwnedTerminal(const CommittedOwnedTerminal& committed,
    const std::shared_ptr<const OwnedTerminalProof>& proof, std::wstring_view trustedHeadName,
    const FILE_ID_INFO& trustedHeadId, std::string_view binding, std::size_t parentCheckpointByteBudget,
    std::size_t childCheckpointByteBudget) noexcept
{
    try
    {
        if (!proof || !committed.proof_ || proof != committed.proof_ || proof.owner_before(committed.proof_) ||
            committed.proof_.owner_before(proof) || GetCurrentThreadId() != committed.thread_ ||
            !proof->FilesMatch(childCheckpointByteBudget) || !StillOwned() || !SameIdentity(locator_.rootId, committed.rootId_) ||
            !SameIdentity(locator_.directoryId, committed.directoryId_) || trustedHeadName != committed.headName_ ||
            !SameIdentity(trustedHeadId, committed.headId_)) return false;
        FILE_ID_INFO id{};
        if (committed.head_ == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(committed.head_, FileIdInfo, &id, sizeof(id)) ||
            !SameIdentity(id, committed.headId_)) return false;
        const auto readback = Read(trustedHeadName, trustedHeadId, binding, parentCheckpointByteBudget);
        if (readback.status != CodecStatus::decoded || readback.recoveredInFlight) return false;
        const auto hexId = [](const FILE_ID_INFO& value) {
            constexpr char digits[] = "0123456789abcdef"; std::string hex; hex.reserve(32);
            for (const auto byte : value.FileId.Identifier) { hex.push_back(digits[byte >> 4]); hex.push_back(digits[byte & 15]); }
            return hex;
        };
        std::size_t matches = 0;
        for (const auto& terminal : readback.state.ownedTerminals)
        {
            if (terminal.templateCaseId != proof->association_->templateCase_ || terminal.ownedCaseId != proof->association_->ownedCase_ ||
                terminal.executionId != proof->executionId_ || terminal.terminalBytes != proof->terminalBytes_) continue;
            bool idsMatch = true;
            for (std::size_t i = 0; i < proof->ids_.size(); ++i)
                idsMatch = idsMatch && terminal.volumes[i] == std::to_string(proof->ids_[i].VolumeSerialNumber) &&
                    terminal.fileIds[i] == hexId(proof->ids_[i]);
            if (idsMatch) ++matches;
        }
        return matches == 1 && proof->FilesMatch(childCheckpointByteBudget) && StillOwned();
    }
    catch (...) { return false; }
}

inline V4OwnedTerminalShardResult CheckpointStore::PublishOwnedTerminalShard(const CommittedOwnedTerminal& committed,
    const std::shared_ptr<const OwnedTerminalProof>& proof, std::wstring_view trustedHeadName,
    const FILE_ID_INFO& trustedHeadId, std::string_view binding, std::size_t parentCheckpointByteBudget,
    std::size_t childCheckpointByteBudget) noexcept
{
    V4OwnedTerminalShardResult out;
    try
    {
        if (!VerifyOwnedTerminal(committed, proof, trustedHeadName, trustedHeadId, binding,
            parentCheckpointByteBudget, childCheckpointByteBudget))
        { out.reason = "owned_terminal_shard_source_unavailable"; return out; }
        const auto source = Read(trustedHeadName, trustedHeadId, binding, parentCheckpointByteBudget);
        if (source.status != CodecStatus::decoded || source.recoveredInFlight)
        { out.resourceUnavailable = source.status == CodecStatus::resource_unavailable; out.reason = "owned_terminal_shard_head_unavailable"; return out; }
        State shard; shard.binding = std::string(binding); shard.familyVersion = FamilyVersion::v4; shard.v4TypedEnabled = true;
        for (const auto& association : source.state.ownedAssociations)
            if (association.templateCaseId == proof->association_->templateCase_ && association.ownedCaseId == proof->association_->ownedCase_)
                shard.ownedAssociations.push_back(association);
        for (const auto& terminal : source.state.ownedTerminals)
            if (terminal.templateCaseId == proof->association_->templateCase_ && terminal.ownedCaseId == proof->association_->ownedCase_ &&
                terminal.executionId == proof->executionId_ && terminal.terminalBytes == proof->terminalBytes_)
                shard.ownedTerminals.push_back(terminal);
        if (shard.ownedAssociations.size() != 1 || shard.ownedTerminals.size() != 1)
        { out.reason = "owned_terminal_shard_record_ambiguous"; return out; }
        const auto expected = EncodeCheckpoint(shard, binding, parentCheckpointByteBudget);
        if (expected.status != CodecStatus::encoded)
        { out.resourceUnavailable = expected.status == CodecStatus::resource_unavailable; out.reason = "owned_terminal_shard_budget_unavailable"; return out; }
        out.publication = Publish(shard, binding, parentCheckpointByteBudget);
        if (out.publication.status != StoreStatus::published_durability_unverified || !out.publication.fileIdentityCaptured ||
            !out.publication.renameReturnedSuccess)
        { out.reason = "owned_terminal_shard_publication_unavailable"; return out; }
        const auto readback = Read(out.publication.fileName, out.publication.fileId, binding, parentCheckpointByteBudget);
        const auto actual = readback.status == CodecStatus::decoded && !readback.recoveredInFlight ?
            EncodeCheckpoint(readback.state, binding, parentCheckpointByteBudget) : CodecResult{};
        if (actual.status != CodecStatus::encoded || actual.bytes != expected.bytes ||
            !VerifyOwnedTerminal(committed, proof, trustedHeadName, trustedHeadId, binding,
                parentCheckpointByteBudget, childCheckpointByteBudget))
        { out.resourceUnavailable = actual.status == CodecStatus::resource_unavailable; out.reason = "owned_terminal_shard_readback_unavailable"; return out; }
        out.association = readback.state.ownedAssociations.front(); out.terminal = readback.state.ownedTerminals.front();
        out.published = true; out.identityReplayed = true;
        out.reason = "owned_terminal_shard_published_not_linked_or_product_result"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "owned_terminal_shard_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "owned_terminal_shard_resource_unavailable"; return out; }
    catch (...) { out.reason = "owned_terminal_shard_observation_unavailable"; return out; }
}

// Read-only shard recovery exposes complete raw bytes but does not
// authenticate history, prove the transition, or assert product success.
inline V4OwnedTerminalShardResult ReadOwnedTerminalShard(CheckpointStore& store, std::wstring_view name,
    const FILE_ID_INFO& fileId, std::string_view binding, const V4CoordinateBudget& coordinateBudget,
    const V4AxisBudget& axisBudget, std::size_t tokenBudget, std::size_t caseByteBudget,
    std::size_t childCheckpointByteBudget, std::size_t shardByteBudget)
{
    V4OwnedTerminalShardResult out;
    try
    {
        const auto readback = store.Read(name, fileId, binding, shardByteBudget);
        if (readback.status != CodecStatus::decoded || readback.recoveredInFlight)
        { out.resourceUnavailable = readback.status == CodecStatus::resource_unavailable; out.reason = "owned_terminal_shard_read_unavailable"; return out; }
        const auto& state = readback.state;
        if (state.familyVersion != FamilyVersion::v4 || !state.v4TypedEnabled || state.nativeProjectionPage || !state.families.empty() ||
            state.ownedAssociations.size() != 1 || state.ownedTerminals.size() != 1)
        { out.reason = "owned_terminal_shard_shape_unavailable"; return out; }
        const auto& association = state.ownedAssociations.front(); const auto& terminal = state.ownedTerminals.front();
        if (association.templateCaseId != terminal.templateCaseId || association.ownedCaseId != terminal.ownedCaseId)
        { out.reason = "owned_terminal_shard_association_mismatch"; return out; }
        const auto templ = ReplayV4StructuredCaseId(terminal.templateCaseId, coordinateBudget, axisBudget, tokenBudget, caseByteBudget);
        const auto owned = ReplayV4StructuredCaseId(terminal.ownedCaseId, coordinateBudget, axisBudget, tokenBudget, caseByteBudget);
        if (templ.status != V4StructuredCaseStatus::unexecuted || owned.status != V4StructuredCaseStatus::unexecuted)
        { out.resourceUnavailable = templ.status == V4StructuredCaseStatus::resource_unavailable || owned.status == V4StructuredCaseStatus::resource_unavailable;
          out.reason = "owned_terminal_shard_case_replay_unavailable"; return out; }
        const auto child = DecodeCheckpoint(terminal.terminalBytes, std::string("v4_virtual_child_v1:") + terminal.ownedCaseId,
            childCheckpointByteBudget);
        if (child.status != CodecStatus::decoded)
        { out.resourceUnavailable = child.status == CodecStatus::resource_unavailable; out.reason = "owned_terminal_shard_child_checkpoint_unavailable"; return out; }
        out.association = association; out.terminal = terminal; out.identityReplayed = true;
        out.reason = "owned_terminal_shard_raw_bytes_not_authenticated_or_product_result"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "owned_terminal_shard_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "owned_terminal_shard_resource_unavailable"; return out; }
    catch (...) { out.reason = "owned_terminal_shard_observation_unavailable"; return out; }
}

struct V4ShardLocator { std::wstring name; FILE_ID_INFO fileId{}; };
struct V4TypedCursorShardResult
{
    bool published = false, cursorValidated = false, resourceUnavailable = false, deferred = false;
    PublishedCheckpoint publication;
    std::optional<V4StructuredDfsCursor> cursor;
    std::string_view reason;
};
// One global DFS cursor or one deferred subtree per immutable file. Other
// deferred work, discovery hints and dedup state need their own trusted index;
// this single cursor is never a claim of complete global traversal history.
inline V4TypedCursorShardResult ReadV4TypedCursorShard(CheckpointStore& store, std::wstring_view name,
    const FILE_ID_INFO& fileId, std::string_view binding, const V4CoordinateBudget& coordinateBudget,
    std::size_t tokenBudget, std::size_t byteBudget)
{
    V4TypedCursorShardResult out;
    try
    {
        const auto loaded = store.Read(name, fileId, binding, byteBudget);
        if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight)
        { out.resourceUnavailable = loaded.status == CodecStatus::resource_unavailable; out.reason = "v4_typed_shard_read_unavailable"; return out; }
        const auto& state = loaded.state;
        if (state.familyVersion != FamilyVersion::v4 || !state.v4TypedEnabled || state.nativeProjectionPage || state.nativeStagedFamily || !state.families.empty() ||
            !state.ownedAssociations.empty() || !state.ownedTerminals.empty() || state.shardIndex ||
            state.v4TypedDeferred.size() > 1 || !state.v4TypedPruned.empty() || state.v4TypedDeferredVisit || state.v4TypedDeferredNext)
        { out.reason = "v4_typed_shard_shape_unavailable"; return out; }
        out.deferred = !state.v4TypedDeferred.empty();
        const auto& cursor = out.deferred ? state.v4TypedDeferred.front() : state.v4TypedCursor;
        if (out.deferred && (state.v4TypedCursor.layer || state.v4TypedCursor.stopDepth || !state.v4TypedCursor.prefix.empty() ||
            state.v4TypedCursor.nextChoice != std::vector<std::uint32_t>{0} || state.v4TypedCursor.exhausted))
        { out.reason = "v4_typed_shard_ambiguous"; return out; }
        const auto checked = StepV4StructuredDfs(cursor, coordinateBudget, tokenBudget, 0);
        if (checked.status == V4StructuredDfsStatus::invalid || checked.status == V4StructuredDfsStatus::resource_unavailable)
        { out.resourceUnavailable = checked.status == V4StructuredDfsStatus::resource_unavailable;
          out.reason = "v4_typed_shard_cursor_unavailable"; return out; }
        out.cursor = cursor; out.cursorValidated = true; out.reason = "v4_typed_shard_cursor_only_not_history"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_typed_shard_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_typed_shard_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_typed_shard_observation_unavailable"; return out; }
}
inline V4TypedCursorShardResult PublishV4TypedCursorShard(CheckpointStore& store, const V4StructuredDfsCursor& cursor,
    bool deferred, std::string_view binding, const V4CoordinateBudget& coordinateBudget,
    std::size_t tokenBudget, std::size_t byteBudget)
{
    V4TypedCursorShardResult out;
    try
    {
        const auto checked = StepV4StructuredDfs(cursor, coordinateBudget, tokenBudget, 0);
        if (checked.status == V4StructuredDfsStatus::invalid || checked.status == V4StructuredDfsStatus::resource_unavailable || cursor.exhausted ||
            (!deferred && cursor.stopDepth))
        { out.resourceUnavailable = checked.status == V4StructuredDfsStatus::resource_unavailable; out.reason = "v4_typed_shard_cursor_unavailable"; return out; }
        State shard; shard.binding = std::string(binding); shard.familyVersion = FamilyVersion::v4; shard.v4TypedEnabled = true;
        if (deferred) shard.v4TypedDeferred.push_back(cursor); else shard.v4TypedCursor = cursor;
        const auto expected = EncodeCheckpoint(shard, binding, byteBudget);
        if (expected.status != CodecStatus::encoded)
        { out.resourceUnavailable = expected.status == CodecStatus::resource_unavailable; out.reason = "v4_typed_shard_budget_unavailable"; return out; }
        out.publication = store.Publish(shard, binding, byteBudget);
        if (out.publication.status != StoreStatus::published_durability_unverified || !out.publication.fileIdentityCaptured || !out.publication.renameReturnedSuccess)
        { out.reason = "v4_typed_shard_publication_unavailable"; return out; }
        const auto readback = ReadV4TypedCursorShard(store, out.publication.fileName, out.publication.fileId, binding, coordinateBudget, tokenBudget, byteBudget);
        const auto loaded = store.Read(out.publication.fileName, out.publication.fileId, binding, byteBudget);
        const auto actual = loaded.status == CodecStatus::decoded && !loaded.recoveredInFlight ? EncodeCheckpoint(loaded.state, binding, byteBudget) : CodecResult{};
        if (!readback.cursorValidated || actual.status != CodecStatus::encoded || actual.bytes != expected.bytes)
        { out.resourceUnavailable = readback.resourceUnavailable || actual.status == CodecStatus::resource_unavailable;
          out.reason = "v4_typed_shard_readback_unavailable"; return out; }
        out.cursor = readback.cursor; out.deferred = deferred; out.cursorValidated = true; out.published = true;
        out.reason = "v4_typed_shard_published_not_linked_or_history"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_typed_shard_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_typed_shard_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_typed_shard_observation_unavailable"; return out; }
}
struct V4TypedShardStepResult
{
    std::optional<V4StructuredDfsStep> step;
    bool resourceUnavailable = false;
    std::string_view reason;
};
struct V4TypedStepShardResult
{
    bool published = false, replayed = false, resourceUnavailable = false;
    PublishedCheckpoint publication;
    std::optional<V4TypedStepAudit> audit;
    std::string_view reason;
};
struct V4RoundAdvanceShardResult
{
    bool read = false, resourceUnavailable = false;
    std::optional<V4RoundAdvanceAudit> audit;
    std::string_view reason;
};
struct V4FamilySelectionShardResult
{
    bool read = false, resourceUnavailable = false;
    std::optional<V4FamilySelectionAudit> audit;
    std::string_view reason;
};
struct V4FamilyRoundStepShardResult
{
    bool read = false, resourceUnavailable = false;
    std::optional<V4FamilyRoundStepAudit> audit;
    std::string_view reason;
};
struct V4FamilySettlementShardResult
{
    bool read = false, resourceUnavailable = false;
    std::optional<V4FamilySettlementAudit> audit;
    std::string_view reason;
};
inline V4FamilySettlementShardResult ReadV4FamilySettlementShard(CheckpointStore& store, std::wstring_view name,
    const FILE_ID_INFO& id, std::string_view binding, std::size_t byteBudget)
{
    V4FamilySettlementShardResult out;
    try
    {
        const auto loaded = store.Read(name, id, binding, byteBudget);
        if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight || !loaded.state.familySettlement)
        { out.resourceUnavailable = loaded.status == CodecStatus::resource_unavailable; out.reason = "v4_family_settlement_read_unavailable"; return out; }
        out.audit = *loaded.state.familySettlement; out.read = true; out.reason = "v4_family_settlement_unreplayed_unauthenticated"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_family_settlement_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_family_settlement_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_family_settlement_unavailable"; return out; }
}
inline V4FamilyRoundStepShardResult ReadV4FamilyRoundStepShard(CheckpointStore& store, std::wstring_view name,
    const FILE_ID_INFO& id, std::string_view binding, std::size_t byteBudget)
{
    V4FamilyRoundStepShardResult out;
    try
    {
        const auto loaded = store.Read(name, id, binding, byteBudget);
        if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight || !loaded.state.familyRoundStep)
        { out.resourceUnavailable = loaded.status == CodecStatus::resource_unavailable; out.reason = "v4_family_round_step_read_unavailable"; return out; }
        out.audit = *loaded.state.familyRoundStep; out.read = true;
        out.reason = "v4_family_round_step_unreplayed_unauthenticated"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_family_round_step_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_family_round_step_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_family_round_step_unavailable"; return out; }
}
inline V4FamilySelectionShardResult ReadV4FamilySelectionShard(CheckpointStore& store, std::wstring_view name,
    const FILE_ID_INFO& id, std::string_view binding, std::size_t byteBudget)
{
    V4FamilySelectionShardResult out;
    try
    {
        const auto loaded = store.Read(name, id, binding, byteBudget);
        if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight || !loaded.state.familySelection)
        { out.resourceUnavailable = loaded.status == CodecStatus::resource_unavailable; out.reason = "v4_family_selection_read_unavailable"; return out; }
        out.audit = *loaded.state.familySelection; out.read = true;
        out.reason = "v4_family_selection_unreplayed_unauthenticated"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_family_selection_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_family_selection_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_family_selection_unavailable"; return out; }
}
inline V4RoundAdvanceShardResult ReadV4RoundAdvanceShard(CheckpointStore& store, std::wstring_view name,
    const FILE_ID_INFO& id, std::string_view binding, std::size_t byteBudget)
{
    V4RoundAdvanceShardResult out;
    try
    {
        const auto loaded = store.Read(name, id, binding, byteBudget);
        if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight || !loaded.state.roundAdvance)
        { out.resourceUnavailable = loaded.status == CodecStatus::resource_unavailable; out.reason = "v4_round_advance_read_unavailable"; return out; }
        out.audit = *loaded.state.roundAdvance; out.read = true;
        out.reason = "v4_round_advance_unreplayed_and_unauthenticated"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_round_advance_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_round_advance_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_round_advance_unavailable"; return out; }
}
inline bool V4SameTypedCursor(const V4StructuredDfsCursor& a, const V4StructuredDfsCursor& b) noexcept
{
    return a.layer == b.layer && a.stopDepth == b.stopDepth && a.prefix == b.prefix &&
        a.nextChoice == b.nextChoice && a.exhausted == b.exhausted;
}
inline V4TypedStepShardResult ReadV4TypedStepShard(CheckpointStore& store, std::wstring_view name,
    const FILE_ID_INFO& id, std::string_view binding, const V4CoordinateBudget& budgetLimit,
    std::size_t tokenLimit, std::size_t workLimit, std::size_t byteBudget)
{
    V4TypedStepShardResult out;
    try
    {
        const auto loaded = store.Read(name, id, binding, byteBudget);
        if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight || !loaded.state.typedStep)
        { out.resourceUnavailable = loaded.status == CodecStatus::resource_unavailable; out.reason = "v4_typed_step_read_unavailable"; return out; }
        const auto& saved = *loaded.state.typedStep; const auto& b = saved.coordinateBudget;
        if (b.decimalDigits > budgetLimit.decimalDigits || b.tokenBytes > budgetLimit.tokenBytes ||
            b.listEntries > budgetLimit.listEntries || b.arithmeticSteps > budgetLimit.arithmeticSteps ||
            saved.tokenBudget > tokenLimit || saved.workBudget > workLimit)
        { out.resourceUnavailable = true; out.reason = "v4_typed_step_replay_budget_unavailable"; return out; }
        const auto step = StepV4StructuredDfs(saved.before, b, saved.tokenBudget, saved.workBudget);
        if (step.status == V4StructuredDfsStatus::resource_unavailable)
        { out.resourceUnavailable = true; out.reason = "v4_typed_step_replay_resource_unavailable"; return out; }
        if (step.status != saved.status || step.workUsed != saved.workUsed || !V4SameTypedCursor(step.next, saved.after) ||
            static_cast<bool>(step.deferred) != static_cast<bool>(saved.deferred) ||
            (step.deferred && !V4SameTypedCursor(*step.deferred, *saved.deferred)) ||
            step.prunedPrefix != saved.prunedPrefix || step.coordinate != saved.familyCoordinate ||
            static_cast<bool>(step.family) != (saved.status == V4StructuredDfsStatus::emitted))
        { out.reason = "v4_typed_step_replay_mismatch"; return out; }
        out.audit = saved; out.replayed = true; out.reason = "v4_typed_step_replayed_not_authenticated_history"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_typed_step_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_typed_step_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_typed_step_observation_unavailable"; return out; }
}
inline V4TypedStepShardResult PublishV4TypedStepShard(CheckpointStore& store, const V4StructuredDfsCursor& before,
    std::string_view binding, const V4CoordinateBudget& budget, std::size_t tokenBudget,
    std::size_t workBudget, std::size_t byteBudget, std::string_view taskId, std::string_view stepOrdinal)
{
    V4TypedStepShardResult out;
    try
    {
        const auto step = StepV4StructuredDfs(before, budget, tokenBudget, workBudget);
        if (step.status == V4StructuredDfsStatus::invalid || step.status == V4StructuredDfsStatus::resource_unavailable || !step.workUsed)
        { out.resourceUnavailable = step.status == V4StructuredDfsStatus::resource_unavailable || !workBudget;
          out.reason = "v4_typed_step_proposal_unavailable"; return out; }
        V4TypedStepAudit audit; audit.before = before; audit.after = step.next; audit.deferred = step.deferred;
        audit.coordinateBudget = budget; audit.tokenBudget = tokenBudget; audit.workBudget = workBudget;
        audit.workUsed = step.workUsed; audit.status = step.status; audit.prunedPrefix = step.prunedPrefix;
        audit.familyCoordinate = step.coordinate;
        audit.taskId = taskId; audit.stepOrdinal = stepOrdinal;
        if (audit.deferred) audit.deferredTaskId = V4TypedDeferredTaskId(taskId, stepOrdinal);
        State state; state.binding = std::string(binding); state.familyVersion = FamilyVersion::v4;
        state.v4TypedEnabled = true; state.typedStep = audit;
        const auto expected = EncodeCheckpoint(state, binding, byteBudget);
        if (expected.status != CodecStatus::encoded)
        { out.resourceUnavailable = expected.status == CodecStatus::resource_unavailable; out.reason = "v4_typed_step_encode_unavailable"; return out; }
        out.publication = store.Publish(state, binding, byteBudget);
        if (out.publication.status != StoreStatus::published_durability_unverified || !out.publication.fileIdentityCaptured || !out.publication.renameReturnedSuccess)
        { out.reason = "v4_typed_step_publication_unavailable"; return out; }
        auto checked = ReadV4TypedStepShard(store, out.publication.fileName, out.publication.fileId,
            binding, budget, tokenBudget, workBudget, byteBudget);
        if (!checked.replayed)
        { out.resourceUnavailable = checked.resourceUnavailable; out.reason = "v4_typed_step_readback_unavailable"; return out; }
        const auto loaded = store.Read(out.publication.fileName, out.publication.fileId, binding, byteBudget);
        const auto actual = loaded.status == CodecStatus::decoded && !loaded.recoveredInFlight ? EncodeCheckpoint(loaded.state, binding, byteBudget) : CodecResult{};
        if (actual.status != CodecStatus::encoded || actual.bytes != expected.bytes)
        { out.resourceUnavailable = actual.status == CodecStatus::resource_unavailable; out.reason = "v4_typed_step_bytes_unavailable"; return out; }
        out.audit = std::move(checked.audit); out.replayed = true; out.published = true;
        out.reason = "v4_typed_step_published_not_linked_or_authenticated"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_typed_step_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_typed_step_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_typed_step_observation_unavailable"; return out; }
}
// This is the existing typed global DFS applied to a re-read cursor shard.
// It is only a proposal: main cursor, deferred work, emitted family and
// grammar-prune evidence must be linked together before a head is adopted.
inline V4TypedShardStepResult StepV4TypedCursorShard(CheckpointStore& store, std::wstring_view name,
    const FILE_ID_INFO& fileId, std::string_view binding, const V4CoordinateBudget& coordinateBudget,
    std::size_t tokenBudget, std::size_t byteBudget, std::size_t workBudget)
{
    V4TypedShardStepResult out;
    try
    {
        const auto shard = ReadV4TypedCursorShard(store, name, fileId, binding, coordinateBudget, tokenBudget, byteBudget);
        if (!shard.cursorValidated || !shard.cursor)
        { out.resourceUnavailable = shard.resourceUnavailable; out.reason = "v4_typed_shard_step_cursor_unavailable"; return out; }
        auto step = StepV4StructuredDfs(*shard.cursor, coordinateBudget, tokenBudget, workBudget);
        if (step.status == V4StructuredDfsStatus::resource_unavailable || step.status == V4StructuredDfsStatus::invalid)
        { out.resourceUnavailable = step.status == V4StructuredDfsStatus::resource_unavailable;
          out.reason = "v4_typed_shard_step_unavailable"; return out; }
        out.step = std::move(step); out.reason = "v4_typed_shard_step_uncommitted_not_coverage"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_typed_shard_step_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_typed_shard_step_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_typed_shard_step_observation_unavailable"; return out; }
}
struct V4ShardIndexCursor
{
    std::optional<V4ShardLocator> next;
    std::string expectedGeneration; // empty only for an initial trusted head
};
struct V4ShardIndexPage
{
    bool read = false, exhausted = false, resourceUnavailable = false;
    std::string kind, generation;
    std::optional<V4ShardLocator> data;
    V4ShardIndexCursor next;
    std::string_view reason;
};
struct V4ShardIndexAppend
{
    bool published = false, resourceUnavailable = false;
    PublishedCheckpoint publication;
    std::optional<V4ShardLocator> head;
    std::string generation;
    std::string_view reason;
};
inline std::string V4ShardIdHex(const FILE_ID_INFO& id)
{
    constexpr char digits[] = "0123456789abcdef"; std::string result; result.reserve(32);
    for (const auto byte : id.FileId.Identifier) { result.push_back(digits[byte >> 4]); result.push_back(digits[byte & 15]); }
    return result;
}
inline bool V4ShardLocatorFromTokens(std::string_view name, std::string_view volume, std::string_view hex,
    V4ShardLocator& out)
{
    if (!V4CanonicalDecimal(volume) || hex.size() != 32 || !V4LowerHex(hex)) return false;
    try
    {
        out.name.assign(name.begin(), name.end()); out.fileId = {};
        out.fileId.VolumeSerialNumber = std::stoull(std::string(volume));
        const auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10; };
        for (std::size_t i = 0; i < 16; ++i) out.fileId.FileId.Identifier[i] = static_cast<BYTE>((digit(hex[2*i]) << 4) | digit(hex[2*i+1]));
        return true;
    }
    catch (...) { return false; }
}
// One index node per call. The caller persists the returned next cursor if
// interrupted; head locator trust is external and no record is treated as a
// product result merely because it appears in this linked index.
inline V4ShardIndexPage ReadV4ShardIndexPage(CheckpointStore& store, const V4ShardIndexCursor& cursor,
    std::string_view binding, std::size_t byteBudget)
{
    V4ShardIndexPage out; out.next = cursor;
    if (!cursor.next) { out.exhausted = true; out.reason = "v4_shard_index_end"; return out; }
    try
    {
        const auto loaded = store.Read(cursor.next->name, cursor.next->fileId, binding, byteBudget);
        if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight || !loaded.state.shardIndex)
        { out.resourceUnavailable = loaded.status == CodecStatus::resource_unavailable; out.reason = "v4_shard_index_node_unavailable"; return out; }
        const auto& node = *loaded.state.shardIndex;
        // Older traversal consumers cannot authenticate the staged case's
        // journal. Do not let them silently skip it while that recovery path
        // and case-aware lookup are still pending.
        if (node.dataKind == "native_staged_case")
        { out.reason = "v4_native_case_index_replay_required"; return out; }
        if (!cursor.expectedGeneration.empty() && node.generation != cursor.expectedGeneration)
        { out.reason = "v4_shard_index_generation_mismatch"; return out; }
        V4ShardLocator data;
        if (!V4ShardLocatorFromTokens(node.dataName, node.dataVolume, node.dataFileId, data))
        { out.reason = "v4_shard_index_data_locator_unavailable"; return out; }
        V4ShardIndexCursor next;
        if (!node.previousName.empty())
        {
            V4ShardLocator previous;
            if (!V4ShardLocatorFromTokens(node.previousName, node.previousVolume, node.previousFileId, previous))
            { out.reason = "v4_shard_index_previous_locator_unavailable"; return out; }
            next.next = std::move(previous); next.expectedGeneration = Subtract(node.generation, "1");
        }
        out.data = std::move(data); out.kind = node.dataKind; out.generation = node.generation;
        out.next = std::move(next); out.read = true; out.exhausted = !out.next.next;
        out.reason = "v4_shard_index_locator_only_not_coverage"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_shard_index_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_shard_index_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_shard_index_observation_unavailable"; return out; }
}

struct V4PreviewHeadAnchorResult
{
    bool published = false, identityMatched = false, resourceUnavailable = false;
    PublishedCheckpoint publication;
    std::optional<V4ShardLocator> head;
    std::string anchorBytes;
    std::unique_ptr<V4CheckpointReadPin> anchorPin, headPin;
    std::string_view reason;
};
// Persists a separate immutable anchor for an already-adopted preview head.
// The caller must retain its returned locator and bytes in a separately trusted
// place; this record does not prove it is latest, durable after power loss, or
// authorize an execution. All side effects remain in publication on failure.
inline V4PreviewHeadAnchorResult PublishV4PreviewHeadAnchor(CheckpointStore& store,
    const CheckpointLocator& locator, const V4ShardLocator& adoptedHead, std::string_view binding,
    std::size_t headByteBudget, std::size_t anchorByteBudget)
{
    V4PreviewHeadAnchorResult out;
    try
    {
        if (!store.MatchesLocator(locator)) { out.reason = "v4_preview_anchor_root_unavailable"; return out; }
        auto pin = store.PinRead(adoptedHead.name, adoptedHead.fileId);
        if (!pin) { out.reason = "v4_preview_anchor_head_pin_unavailable"; return out; }
        const auto head = store.Read(adoptedHead.name, adoptedHead.fileId, binding, headByteBudget);
        if (head.status != CodecStatus::decoded || head.recoveredInFlight || !head.state.shardIndex)
        { out.resourceUnavailable = head.status == CodecStatus::resource_unavailable; out.reason = "v4_preview_anchor_head_unavailable"; return out; }
        V4PreviewHeadAnchorAudit a; a.rootVolume = std::to_string(locator.rootId.VolumeSerialNumber); a.rootId = V4ShardIdHex(locator.rootId);
        a.directoryName.assign(locator.directoryName.begin(), locator.directoryName.end());
        a.directoryVolume = std::to_string(locator.directoryId.VolumeSerialNumber); a.directoryId = V4ShardIdHex(locator.directoryId);
        a.headName.assign(adoptedHead.name.begin(), adoptedHead.name.end()); a.headVolume = std::to_string(adoptedHead.fileId.VolumeSerialNumber);
        a.headId = V4ShardIdHex(adoptedHead.fileId); a.headBytes = head.bytes;
        State state; state.binding = std::string(binding); state.familyVersion = FamilyVersion::v4; state.v4TypedEnabled = true;
        state.previewHeadAnchor = std::move(a);
        const auto encoded = EncodeCheckpoint(state, binding, anchorByteBudget);
        if (encoded.status != CodecStatus::encoded)
        { out.resourceUnavailable = encoded.status == CodecStatus::resource_unavailable; out.reason = "v4_preview_anchor_budget_unavailable"; return out; }
        out.anchorBytes = encoded.bytes; out.publication = store.Publish(state, binding, anchorByteBudget);
        if (out.publication.status != StoreStatus::published_durability_unverified || !out.publication.fileIdentityCaptured || !out.publication.renameReturnedSuccess)
        { out.reason = "v4_preview_anchor_publication_unavailable"; return out; }
        auto anchorPin = store.PinRead(out.publication.fileName, out.publication.fileId);
        const auto read = store.Read(out.publication.fileName, out.publication.fileId, binding, anchorByteBudget);
        const auto headAgain = store.Read(adoptedHead.name, adoptedHead.fileId, binding, headByteBudget);
        if (!anchorPin || read.status != CodecStatus::decoded || read.recoveredInFlight || read.bytes != out.anchorBytes ||
            headAgain.status != CodecStatus::decoded || headAgain.recoveredInFlight || headAgain.bytes != head.bytes || !store.MatchesLocator(locator))
        { out.resourceUnavailable = read.status == CodecStatus::resource_unavailable || headAgain.status == CodecStatus::resource_unavailable;
          out.reason = "v4_preview_anchor_readback_unavailable"; return out; }
        out.head = adoptedHead; out.headPin = std::move(pin); out.anchorPin = std::move(anchorPin); out.published = out.identityMatched = true;
        out.reason = "v4_preview_anchor_published_not_authenticated_or_durable"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_preview_anchor_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_preview_anchor_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_preview_anchor_unavailable"; return out; }
}

// `expectedAnchorBytes` and the anchor locator must come from an independent
// trusted source. Even a matching record is preview-only; caller must replay
// its journal/settlement and separately establish execution authorization.
inline V4PreviewHeadAnchorResult ReadV4PreviewHeadAnchor(CheckpointStore& store,
    const CheckpointLocator& locator, const V4ShardLocator& anchor, std::string_view expectedAnchorBytes,
    std::string_view binding, std::size_t headByteBudget, std::size_t anchorByteBudget)
{
    V4PreviewHeadAnchorResult out;
    try
    {
        if (expectedAnchorBytes.empty() || !store.MatchesLocator(locator)) { out.reason = "v4_preview_anchor_external_locator_unavailable"; return out; }
        auto anchorPin = store.PinRead(anchor.name, anchor.fileId); if (!anchorPin) { out.reason = "v4_preview_anchor_pin_unavailable"; return out; }
        const auto read = store.Read(anchor.name, anchor.fileId, binding, anchorByteBudget);
        if (read.status != CodecStatus::decoded || read.recoveredInFlight || !read.state.previewHeadAnchor || std::string_view(read.bytes) != expectedAnchorBytes)
        { out.resourceUnavailable = read.status == CodecStatus::resource_unavailable; out.reason = "v4_preview_anchor_external_bytes_unavailable"; return out; }
        const auto& a = *read.state.previewHeadAnchor;
        const std::string directory(locator.directoryName.begin(), locator.directoryName.end());
        if (a.rootVolume != std::to_string(locator.rootId.VolumeSerialNumber) || a.rootId != V4ShardIdHex(locator.rootId) ||
            a.directoryName != directory || a.directoryVolume != std::to_string(locator.directoryId.VolumeSerialNumber) ||
            a.directoryId != V4ShardIdHex(locator.directoryId)) { out.reason = "v4_preview_anchor_root_identity_mismatch"; return out; }
        V4ShardLocator head; if (!V4ShardLocatorFromTokens(a.headName, a.headVolume, a.headId, head))
        { out.reason = "v4_preview_anchor_head_locator_unavailable"; return out; }
        auto headPin = store.PinRead(head.name, head.fileId); if (!headPin) { out.reason = "v4_preview_anchor_head_pin_unavailable"; return out; }
        const auto content = store.Read(head.name, head.fileId, binding, headByteBudget);
        const auto anchorAgain = store.Read(anchor.name, anchor.fileId, binding, anchorByteBudget);
        if (content.status != CodecStatus::decoded || content.recoveredInFlight || !content.state.shardIndex || content.bytes != a.headBytes ||
            anchorAgain.status != CodecStatus::decoded || anchorAgain.recoveredInFlight || std::string_view(anchorAgain.bytes) != expectedAnchorBytes || !store.MatchesLocator(locator))
        { out.resourceUnavailable = content.status == CodecStatus::resource_unavailable || anchorAgain.status == CodecStatus::resource_unavailable;
          out.reason = "v4_preview_anchor_head_content_unavailable"; return out; }
        out.head = std::move(head); out.anchorBytes = std::string(expectedAnchorBytes); out.headPin = std::move(headPin); out.anchorPin = std::move(anchorPin);
        out.identityMatched = true; out.reason = "v4_preview_anchor_matched_external_authority_unverified"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_preview_anchor_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_preview_anchor_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_preview_anchor_unavailable"; return out; }
}

struct V4NativeProjectionPageShardResult
{
    bool read = false, resourceUnavailable = false;
    PublishedCheckpoint publication;
    std::optional<V4NativeProjectionPageAudit> audit;
    std::string bytes;
    std::string_view reason;
};
struct V4NativeStagedFamilyShardResult
{
    bool read = false, resourceUnavailable = false;
    PublishedCheckpoint publication;
    std::optional<V4NativeStagedFamilyAudit> audit;
    std::string bytes;
    std::string_view reason;
};
inline V4NativeStagedFamilyShardResult ReadV4NativeStagedFamilyShard(CheckpointStore& store,
    std::wstring_view name, const FILE_ID_INFO& id, std::string_view binding, std::size_t byteBudget)
{
    V4NativeStagedFamilyShardResult out;
    try
    {
        const auto loaded = store.Read(name, id, binding, byteBudget);
        if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight || !loaded.state.nativeStagedFamily)
        { out.resourceUnavailable = loaded.status == CodecStatus::resource_unavailable; out.reason = "v4_native_staged_family_read_unavailable"; return out; }
        out.audit = *loaded.state.nativeStagedFamily; out.bytes = loaded.bytes; out.read = true;
        out.reason = "v4_native_staged_family_shape_only_not_replayed"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_native_staged_family_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_native_staged_family_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_native_staged_family_read_unavailable"; return out; }
}
struct V4NativeCaseIndexShardResult
{
    bool read = false, resourceUnavailable = false;
    std::optional<V4NativeCaseIndexAudit> audit;
    std::string bytes;
    std::string_view reason;
};
inline V4NativeCaseIndexShardResult ReadV4NativeCaseIndexShard(CheckpointStore& store,
    std::wstring_view name, const FILE_ID_INFO& id, std::string_view binding, std::size_t byteBudget)
{
    V4NativeCaseIndexShardResult out;
    try
    {
        const auto loaded = store.Read(name, id, binding, byteBudget);
        if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight || !loaded.state.nativeCaseIndex)
        { out.resourceUnavailable = loaded.status == CodecStatus::resource_unavailable; out.reason = "v4_native_case_index_read_unavailable"; return out; }
        out.audit = *loaded.state.nativeCaseIndex; out.bytes = loaded.bytes; out.read = true;
        out.reason = "v4_native_case_index_shape_only_not_authenticated"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_native_case_index_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_native_case_index_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_native_case_index_read_unavailable"; return out; }
}
inline V4NativeProjectionPageShardResult ReadV4NativeProjectionPageShard(CheckpointStore& store,
    std::wstring_view name, const FILE_ID_INFO& id, std::string_view binding, std::size_t byteBudget)
{
    V4NativeProjectionPageShardResult out;
    try
    {
        const auto loaded = store.Read(name, id, binding, byteBudget);
        if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight || !loaded.state.nativeProjectionPage)
        { out.resourceUnavailable = loaded.status == CodecStatus::resource_unavailable;
          out.reason = "v4_native_projection_page_read_unavailable"; return out; }
        out.audit = *loaded.state.nativeProjectionPage; out.bytes = loaded.bytes; out.read = true;
        out.reason = "v4_native_projection_page_shape_only_not_replayed"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_native_projection_page_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_native_projection_page_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_native_projection_page_read_unavailable"; return out; }
}

// Publishes exact audit bytes and reads them back. This alone neither
// authenticates a journal nor adopts an index head or family cursor.
inline V4NativeProjectionPageShardResult PublishV4NativeProjectionPageShard(CheckpointStore& store,
    const V4NativeProjectionPageAudit& audit, std::string_view binding, std::size_t byteBudget)
{
    V4NativeProjectionPageShardResult out;
    try
    {
        State shard; shard.binding = std::string(binding); shard.familyVersion = FamilyVersion::v4;
        shard.v4TypedEnabled = true; shard.nativeProjectionPage = audit;
        const auto expected = EncodeCheckpoint(shard, binding, byteBudget);
        if (expected.status != CodecStatus::encoded)
        { out.resourceUnavailable = expected.status == CodecStatus::resource_unavailable; out.reason = "v4_native_projection_page_encode_unavailable"; return out; }
        out.publication = store.Publish(shard, binding, byteBudget);
        if (out.publication.status != StoreStatus::published_durability_unverified ||
            !out.publication.fileIdentityCaptured || !out.publication.renameReturnedSuccess)
        { out.reason = "v4_native_projection_page_publish_unavailable"; return out; }
        auto readback = ReadV4NativeProjectionPageShard(store, out.publication.fileName, out.publication.fileId, binding, byteBudget);
        if (!readback.read || readback.bytes != expected.bytes)
        { out.resourceUnavailable = readback.resourceUnavailable; out.reason = "v4_native_projection_page_readback_unavailable"; return out; }
        out.audit = std::move(readback.audit); out.bytes = std::move(readback.bytes); out.read = true;
        out.reason = "v4_native_projection_page_published_unreplayed"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_native_projection_page_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_native_projection_page_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_native_projection_page_publish_unavailable"; return out; }
}

struct V4NativeProjectionReplayLimits
{
    V4CoordinateBudget coordinate;
    V4AxisBudget axes;
    std::size_t pages = 0, bytes = 0, preamble = 0, advanceWork = 0, totalPageWork = 0, outputPerPage = 0, familyReplayWork = 0;
};
struct V4NativeProjectionReplayResult
{
    bool replayed = false, complete = false, resourceUnavailable = false;
    std::size_t pagesReplayed = 0, workUsed = 0;
    std::string_view reason;
};
// Replay exact saved pages from a pinned sequence. The supplied base locator
// and list are requests, not proof of index membership or latest-head origin.
// This read-only result does not enable Attach or production execution.
inline V4NativeProjectionReplayResult ReplayV4NativeProjectionPages(CheckpointStore& store,
    const V4ShardLocator& baseHead, const std::vector<V4ShardLocator>& pages, std::string_view binding,
    const V4NativeProjectionReplayLimits& limits)
{
    V4NativeProjectionReplayResult out;
    try
    {
        if (pages.empty() || pages.size() > limits.pages) { out.resourceUnavailable = pages.size() > limits.pages; out.reason = "v4_projection_replay_page_budget_unavailable"; return out; }
        auto basePin = store.PinRead(baseHead.name, baseHead.fileId);
        if (!basePin) { out.reason = "v4_projection_replay_base_pin_unavailable"; return out; }
        const auto base = store.Read(baseHead.name, baseHead.fileId, binding, limits.bytes);
        if (base.status != CodecStatus::decoded || base.recoveredInFlight || !base.state.shardIndex)
        { out.resourceUnavailable = base.status == CodecStatus::resource_unavailable; out.reason = "v4_projection_replay_base_read_unavailable"; return out; }
        struct Snapshot { V4ShardLocator locator; std::unique_ptr<V4CheckpointReadPin> pin; std::string bytes; V4NativeProjectionPageAudit audit; };
        std::vector<Snapshot> snapshots; snapshots.reserve(pages.size());
        std::size_t bytesUsed = base.bytes.size();
        for (const auto& locator : pages)
        {
            auto pin = store.PinRead(locator.name, locator.fileId);
            if (!pin) { out.reason = "v4_projection_replay_pin_unavailable"; return out; }
            auto page = ReadV4NativeProjectionPageShard(store, locator.name, locator.fileId, binding, limits.bytes - bytesUsed);
            if (!page.read || !page.audit)
            { out.resourceUnavailable = page.resourceUnavailable; out.reason = "v4_projection_replay_page_unavailable"; return out; }
            if (page.bytes.size() > limits.bytes - bytesUsed) { out.resourceUnavailable = true; out.reason = "v4_projection_replay_byte_budget_unavailable"; return out; }
            bytesUsed += page.bytes.size();
            snapshots.push_back({locator, std::move(pin), std::move(page.bytes), std::move(*page.audit)});
        }
        const auto& first = snapshots.front().audit;
        const auto budgetValues = [](const V4NativeProjectionPageAudit& p) {
            return std::array<std::size_t, 16>{p.coordinateBudget.decimalDigits, p.coordinateBudget.tokenBytes,
                p.coordinateBudget.listEntries, p.coordinateBudget.arithmeticSteps, p.axisBudget.logicalMembers,
                p.axisBudget.fileMembers, p.axisBudget.virtualMembers, p.axisBudget.referenceMembers, p.axisBudget.payloadSlots,
                p.axisBudget.textSlots, p.axisBudget.requestResultSlots, p.axisBudget.fileContentSlots, p.axisBudget.payloadBytes,
                p.axisBudget.directoryNodeSlots, p.axisBudget.directoryRootSlots, p.axisBudget.preparationSteps};
        };
        V4NativeProjectionPageAudit ceiling; ceiling.coordinateBudget = limits.coordinate; ceiling.axisBudget = limits.axes;
        const auto requested = budgetValues(first), maximum = budgetValues(ceiling);
        for (std::size_t i = 0; i < requested.size(); ++i)
            if (requested[i] > maximum[i]) { out.resourceUnavailable = true; out.reason = "v4_projection_replay_model_budget_unavailable"; return out; }
        if (first.preambleBudget > limits.preamble || first.advanceWorkBudget > limits.advanceWork ||
            first.baseHeadName != std::string(baseHead.name.begin(), baseHead.name.end()) ||
            first.baseHeadVolume != std::to_string(baseHead.fileId.VolumeSerialNumber) || first.baseHeadFileId != V4ShardIdHex(baseHead.fileId))
        { out.resourceUnavailable = first.preambleBudget > limits.preamble || first.advanceWorkBudget > limits.advanceWork;
          out.reason = "v4_projection_replay_base_unavailable"; return out; }
        const auto decoded = NextV4FamilyCoordinate(first.familyCoordinate, first.coordinateBudget);
        if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family)
        { out.resourceUnavailable = decoded.status == V4FamilyCoordinateStatus::resource_unavailable; out.reason = "v4_projection_replay_family_unavailable"; return out; }
        const auto before = event_graph::V4FamilyCandidateGraph::ParseCursor(*decoded.family, first.axisBudget, first.familyCursor);
        if (before.status != event_graph::V4FamilyTraversalStatus::ready || !before.errors.empty())
        { out.resourceUnavailable = before.status == event_graph::V4FamilyTraversalStatus::resource_unavailable; out.reason = "v4_projection_replay_cursor_unavailable"; return out; }
        const auto advance = event_graph::V4FamilyCandidateGraph::PrepareStagedNativeAdvance(*decoded.family, first.axisBudget, before.cursor, first.advanceWorkBudget);
        if (advance.status != event_graph::V4FamilyTraversalStatus::ready || !advance.proposal || !advance.errors.empty())
        { out.resourceUnavailable = advance.status == event_graph::V4FamilyTraversalStatus::resource_unavailable; out.reason = "v4_projection_replay_advance_unavailable"; return out; }
        const auto& ap = advance.proposal->Page();
        const auto encodedAdvance = EncodeV4ProjectionAdvance(*decoded.family, first.axisBudget, ap, limits.bytes);
        std::string bits; bits.reserve(ap.pendingPresent.size()); for (const bool bit : ap.pendingPresent) bits += bit ? '1' : '0';
        if (!ap.projectionRequired || ap.pendingOrdinal.empty() || !encodedAdvance.encoded || encodedAdvance.bytes != first.advanceBytes || ap.candidateId != first.candidateId ||
            ap.candidateOrdinal != first.candidateOrdinal || ap.pendingOrdinal != first.leafOrdinal || bits != first.leafPresence)
        { out.resourceUnavailable = encodedAdvance.resourceUnavailable; out.reason = "v4_projection_replay_advance_mismatch"; return out; }
        const auto identity = V4FamilyIdentityContext::Create(*decoded.family, first.axisBudget);
        const auto verified = V4FamilyValidatedContext::Create(*decoded.family, first.axisBudget, ap.candidate);
        if (identity.Status() != V4FamilyStatus::schema_only || verified.Status() != V4CoordinateStatus::coordinate_ready)
        { out.resourceUnavailable = identity.Status() == V4FamilyStatus::resource_unavailable || verified.Status() == V4CoordinateStatus::resource_unavailable;
          out.reason = "v4_projection_replay_candidate_unavailable"; return out; }
        bool resource = false; std::vector<std::string> errors;
        auto work = event_graph::V4NativeJoinedGraph::CreateProjectionWork(identity, verified, ap.pendingPresent, first.preambleBudget, resource, errors);
        if (!work || resource || !errors.empty()) { out.resourceUnavailable = resource; out.reason = "v4_projection_replay_work_unavailable"; return out; }
        for (std::size_t i = 0; i < snapshots.size(); ++i)
        {
            const auto& p = snapshots[i].audit;
            if (work->Cursor().stage == event_graph::PresenceProjectionWorkStage::done)
            { out.reason = "v4_projection_replay_page_after_done"; return out; }
            if (p.familyCoordinate != first.familyCoordinate || p.familyCursor != first.familyCursor || p.candidateId != first.candidateId ||
                p.candidateOrdinal != first.candidateOrdinal || p.leafOrdinal != first.leafOrdinal || p.leafPresence != first.leafPresence ||
                p.baseHeadName != first.baseHeadName || p.baseHeadVolume != first.baseHeadVolume || p.baseHeadFileId != first.baseHeadFileId ||
                p.advanceBytes != first.advanceBytes || budgetValues(p) != requested || p.advanceWorkBudget != first.advanceWorkBudget ||
                p.preambleBudget != first.preambleBudget || p.pageOrdinal != std::to_string(i))
            { out.reason = "v4_projection_replay_page_identity_mismatch"; return out; }
            if (p.workBudget > limits.totalPageWork - out.workUsed || p.outputLimit > limits.outputPerPage)
            { out.resourceUnavailable = true; out.reason = "v4_projection_replay_work_budget_unavailable"; return out; }
            auto proposal = work->PrepareStep(p.workBudget, p.outputLimit, resource);
            if (!proposal || resource) { out.resourceUnavailable = resource; out.reason = "v4_projection_replay_page_prepare_unavailable"; return out; }
            const auto encoded = EncodeV4ProjectionPage(proposal->Page(), limits.bytes);
            if (!encoded.encoded || encoded.bytes != p.pageBytes)
            { out.resourceUnavailable = encoded.resourceUnavailable; out.reason = "v4_projection_replay_page_mismatch"; return out; }
            const auto prior = event_graph::EncodePresenceProjectionCursor(proposal->Page().before);
            const auto next = event_graph::EncodePresenceProjectionCursor(proposal->Page().after);
            if (prior.resourceUnavailable || next.resourceUnavailable) { out.resourceUnavailable = true; out.reason = "v4_projection_replay_cursor_resource_unavailable"; return out; }
            if (!prior.cursor || !next.cursor || (prior.encoded == next.encoded && !proposal->Page().workUsed))
            { out.reason = "v4_projection_replay_page_no_progress"; return out; }
            if (!work->Commit(*proposal, resource)) { out.resourceUnavailable = resource; out.reason = "v4_projection_replay_commit_unavailable"; return out; }
            out.workUsed += proposal->Page().workUsed; ++out.pagesReplayed;
        }
        for (const auto& saved : snapshots)
        {
            const auto reread = store.Read(saved.locator.name, saved.locator.fileId, binding, limits.bytes);
            if (reread.status != CodecStatus::decoded || reread.recoveredInFlight || reread.bytes != saved.bytes)
            { out.resourceUnavailable = reread.status == CodecStatus::resource_unavailable; out.reason = "v4_projection_replay_page_changed"; return out; }
        }
        const auto baseAgain = store.Read(baseHead.name, baseHead.fileId, binding, limits.bytes);
        if (baseAgain.status != CodecStatus::decoded || baseAgain.recoveredInFlight || baseAgain.bytes != base.bytes)
        { out.resourceUnavailable = baseAgain.status == CodecStatus::resource_unavailable; out.reason = "v4_projection_replay_base_changed"; return out; }
        out.complete = work->CanAuthorizeActiveGraph(); out.replayed = true;
        out.reason = "v4_projection_replayed_index_and_authority_unverified"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_projection_replay_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_projection_replay_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_projection_replay_unavailable"; return out; }
}

// Link one previously published and authority-replayed shard to a caller's
// trusted current index head. The returned head becomes current only after
// the caller atomically adopts its locator; no old shard is removed.
inline V4ShardIndexAppend AppendV4ShardIndex(CheckpointStore& store, const std::optional<V4ShardLocator>& previous,
    const V4ShardLocator& data, std::string_view kind, std::string_view binding,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget, std::size_t tokenBudget,
    std::size_t caseByteBudget, std::size_t childCheckpointByteBudget, std::size_t replayWorkBudget,
    std::size_t shardByteBudget, std::size_t indexByteBudget)
{
    V4ShardIndexAppend out;
    try
    {
        if (kind == "case")
        {
            const auto checked = ReadV4CaseShard(store, data.name, data.fileId, binding, coordinateBudget, axisBudget, replayWorkBudget, shardByteBudget);
            if (!checked.identityReplayed) { out.resourceUnavailable = checked.resourceUnavailable; out.reason = "v4_shard_index_case_unavailable"; return out; }
        }
        else if (kind == "terminal")
        {
            const auto checked = ReadOwnedTerminalShard(store, data.name, data.fileId, binding, coordinateBudget, axisBudget,
                tokenBudget, caseByteBudget, childCheckpointByteBudget, shardByteBudget);
            if (!checked.identityReplayed) { out.resourceUnavailable = checked.resourceUnavailable; out.reason = "v4_shard_index_terminal_unavailable"; return out; }
        }
        else if (kind == "family")
        {
            const auto checked = ReadV4FamilyCursorShard(store, data.name, data.fileId, binding,
                coordinateBudget, axisBudget, shardByteBudget, replayWorkBudget);
            if (!checked.cursorValidated) { out.resourceUnavailable = checked.resourceUnavailable; out.reason = "v4_shard_index_family_unavailable"; return out; }
        }
        else if (kind == "global_cursor")
        {
            const auto checked = ReadV4TypedCursorShard(store, data.name, data.fileId, binding,
                coordinateBudget, tokenBudget, shardByteBudget);
            if (!checked.cursorValidated) { out.resourceUnavailable = checked.resourceUnavailable; out.reason = "v4_shard_index_global_cursor_unavailable"; return out; }
        }
        else if (kind == "typed_step")
        {
            const auto checked = ReadV4TypedStepShard(store, data.name, data.fileId, binding,
                coordinateBudget, tokenBudget, replayWorkBudget, shardByteBudget);
            if (!checked.replayed) { out.resourceUnavailable = checked.resourceUnavailable; out.reason = "v4_shard_index_typed_step_unavailable"; return out; }
        }
        else if (kind == "round_step")
        {
            const auto checked = ReadV4RoundAdvanceShard(store, data.name, data.fileId, binding, shardByteBudget);
            if (!checked.read) { out.resourceUnavailable = checked.resourceUnavailable; out.reason = "v4_shard_index_round_step_unavailable"; return out; }
        }
        else if (kind == "family_selection")
        {
            const auto checked = ReadV4FamilySelectionShard(store, data.name, data.fileId, binding, shardByteBudget);
            if (!checked.read) { out.resourceUnavailable = checked.resourceUnavailable; out.reason = "v4_shard_index_family_selection_unavailable"; return out; }
        }
        else if (kind == "family_round_step")
        {
            const auto checked = ReadV4FamilyRoundStepShard(store, data.name, data.fileId, binding, shardByteBudget);
            if (!checked.read) { out.resourceUnavailable = checked.resourceUnavailable; out.reason = "v4_shard_index_family_round_step_unavailable"; return out; }
        }
        else if (kind == "family_settlement")
        {
            const auto checked = ReadV4FamilySettlementShard(store, data.name, data.fileId, binding, shardByteBudget);
            if (!checked.read) { out.resourceUnavailable = checked.resourceUnavailable; out.reason = "v4_shard_index_family_settlement_unavailable"; return out; }
        }
        else if (kind == "native_projection_page")
        {
            const auto checked = ReadV4NativeProjectionPageShard(store, data.name, data.fileId, binding, shardByteBudget);
            if (!checked.read) { out.resourceUnavailable = checked.resourceUnavailable; out.reason = "v4_shard_index_projection_page_unavailable"; return out; }
        }
        else if (kind == "native_staged_family")
        {
            const auto checked = ReadV4NativeStagedFamilyShard(store, data.name, data.fileId, binding, shardByteBudget);
            if (!checked.read) { out.resourceUnavailable = checked.resourceUnavailable; out.reason = "v4_shard_index_staged_family_unavailable"; return out; }
        }
        else if (kind == "native_staged_case")
        {
            const auto checked = ReadV4NativeCaseIndexShard(store, data.name, data.fileId, binding, shardByteBudget);
            if (!checked.read) { out.resourceUnavailable = checked.resourceUnavailable; out.reason = "v4_shard_index_staged_case_unavailable"; return out; }
        }
        else { out.reason = "v4_shard_index_kind_unavailable"; return out; }
        V4ShardIndexNode node; node.generation = "1"; node.dataKind = kind;
        node.dataName.assign(data.name.begin(), data.name.end()); node.dataVolume = std::to_string(data.fileId.VolumeSerialNumber);
        node.dataFileId = V4ShardIdHex(data.fileId);
        if (previous)
        {
            const auto old = ReadV4ShardIndexPage(store, V4ShardIndexCursor{previous, {}}, binding, indexByteBudget);
            if (!old.read) { out.resourceUnavailable = old.resourceUnavailable; out.reason = "v4_shard_index_head_unavailable"; return out; }
            node.generation = old.generation; Increment(node.generation);
            node.previousName.assign(previous->name.begin(), previous->name.end());
            node.previousVolume = std::to_string(previous->fileId.VolumeSerialNumber); node.previousFileId = V4ShardIdHex(previous->fileId);
        }
        State index; index.binding = std::string(binding); index.familyVersion = FamilyVersion::v4; index.v4TypedEnabled = true;
        index.shardIndex = node;
        const auto expected = EncodeCheckpoint(index, binding, indexByteBudget);
        if (expected.status != CodecStatus::encoded)
        { out.resourceUnavailable = expected.status == CodecStatus::resource_unavailable; out.reason = "v4_shard_index_budget_unavailable"; return out; }
        out.publication = store.Publish(index, binding, indexByteBudget);
        if (out.publication.status != StoreStatus::published_durability_unverified || !out.publication.fileIdentityCaptured || !out.publication.renameReturnedSuccess)
        { out.reason = "v4_shard_index_publication_unavailable"; return out; }
        const auto readback = store.Read(out.publication.fileName, out.publication.fileId, binding, indexByteBudget);
        const auto actual = readback.status == CodecStatus::decoded && !readback.recoveredInFlight ?
            EncodeCheckpoint(readback.state, binding, indexByteBudget) : CodecResult{};
        if (actual.status != CodecStatus::encoded || actual.bytes != expected.bytes)
        { out.resourceUnavailable = actual.status == CodecStatus::resource_unavailable; out.reason = "v4_shard_index_readback_unavailable"; return out; }
        out.head = V4ShardLocator{out.publication.fileName, out.publication.fileId}; out.generation = node.generation;
        out.published = true; out.reason = "v4_shard_index_head_unadopted_not_coverage"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_shard_index_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_shard_index_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_shard_index_observation_unavailable"; return out; }
}

struct V4ForegroundLeaseAttempt
{
    bool markerCreateAttempted = false, markerCreated = false, markerIdentityObserved = false, lockAcquired = false;
    FILE_ID_INFO markerId{};
    std::wstring markerPath;
    std::string_view reason;
};
struct V4ForegroundGenesisResult
{
    bool ready = false, resourceUnavailable = false;
    std::array<PublishedCheckpoint, 2> publications;
    std::optional<V4ShardLocator> head;
    std::uint64_t generation = 0;
    std::string_view reason;
};
class V4ForegroundReservation;
class V4NativeProjectionLiveJournal;
class V4NativeStagedFamilyPending;
class V4NativeReceiptAncestryProof;
// A newly created store can establish a single live foreground owner. The
// marker is exclusively created and never removed; another handle/process
// cannot acquire its byte-range lock. This does not prove a restart's latest
// head, exclude non-participating writers or stop mapped writes.
class V4ForegroundLease final
{
    friend class V4ForegroundReservation;
    friend class V4NativeProjectionLiveJournal;
    friend class V4NativeStagedFamilyPending;
    std::shared_ptr<CheckpointStore> store_;
    std::weak_ptr<V4ForegroundLease> self_;
    HANDLE marker_ = INVALID_HANDLE_VALUE;
    FILE_ID_INFO markerId_{};
    std::wstring markerPath_;
    std::string binding_, headBytes_, indexGeneration_;
    std::optional<V4ShardLocator> head_;
    std::unique_ptr<V4CheckpointReadPin> headPin_;
    std::atomic_bool busy_{false};
    std::uint64_t generation_ = 0;
    DWORD ownerThread_ = 0;
    std::atomic_bool poisoned_{false};
    bool locked_ = false;
    V4ForegroundLease() = default;
    bool MarkerLive() const noexcept
    {
        if (!store_ || !store_->StillOwned() || marker_ == INVALID_HANDLE_VALUE || !ownerThread_ || GetCurrentThreadId() != ownerThread_) return false;
        FILE_ID_INFO held{}, named{}; BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandleEx(marker_, FileIdInfo, &held, sizeof(held)) || !CheckpointStore::SameIdentity(held, markerId_) ||
            !GetFileInformationByHandle(marker_, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
        HANDLE name = CreateFileW(markerPath_.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (name == INVALID_HANDLE_VALUE) return false;
        const bool same = GetFileInformationByHandleEx(name, FileIdInfo, &named, sizeof(named)) &&
            CheckpointStore::SameIdentity(named, markerId_); CloseHandle(name); return same && store_->StillOwned();
    }
public:
    V4ForegroundLease(const V4ForegroundLease&) = delete;
    V4ForegroundLease& operator=(const V4ForegroundLease&) = delete;
    ~V4ForegroundLease()
    {
        if (marker_ != INVALID_HANDLE_VALUE)
        { if (locked_) { OVERLAPPED region{}; UnlockFileEx(marker_, 0, 1, 0, &region); } CloseHandle(marker_); }
    }
    static std::shared_ptr<V4ForegroundLease> AcquireFresh(std::shared_ptr<CheckpointStore> store,
        std::string_view binding, V4ForegroundLeaseAttempt& attempt) noexcept
    {
        attempt = {};
        bool claimed = false;
        try
        {
            if (!store || !store->freshCreated_ || !store->StillOwned() || binding.empty())
            { attempt.reason = "v4_foreground_fresh_store_unavailable"; return {}; }
            bool expected = false;
            if (!store->freshLeaseClaimed_.compare_exchange_strong(expected, true))
            { attempt.reason = "v4_foreground_store_already_claimed"; return {}; }
            claimed = true;
            auto lease = std::shared_ptr<V4ForegroundLease>(new V4ForegroundLease);
            lease->self_ = lease;
            lease->store_ = store; lease->binding_ = std::string(binding); lease->ownerThread_ = GetCurrentThreadId();
            lease->markerPath_ = store->directoryPath_ + L"\\v4-foreground.lock"; attempt.markerPath = lease->markerPath_;
            attempt.markerCreateAttempted = true;
            lease->marker_ = CreateFileW(lease->markerPath_.c_str(), GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (lease->marker_ == INVALID_HANDLE_VALUE) { attempt.reason = "v4_foreground_marker_create_unavailable"; return {}; }
            attempt.markerCreated = true; BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandleEx(lease->marker_, FileIdInfo, &lease->markerId_, sizeof(lease->markerId_)) ||
                !CheckpointStore::UsableIdentity(lease->markerId_) || !GetFileInformationByHandle(lease->marker_, &info) ||
                (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            { attempt.reason = "v4_foreground_marker_identity_unavailable"; return {}; }
            attempt.markerId = lease->markerId_; attempt.markerIdentityObserved = true;
            OVERLAPPED region{};
            if (!LockFileEx(lease->marker_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &region))
            { attempt.reason = "v4_foreground_marker_lock_unavailable"; return {}; }
            lease->locked_ = true; attempt.lockAcquired = true;
            if (!lease->MarkerLive()) { attempt.reason = "v4_foreground_marker_revalidation_unavailable"; return {}; }
            attempt.reason = "v4_foreground_lock_held_genesis_pending"; return lease;
        }
        catch (const std::bad_alloc&) { if (claimed && !attempt.markerCreateAttempted) store->freshLeaseClaimed_.store(false); attempt.reason = "v4_foreground_resource_unavailable"; return {}; }
        catch (const std::length_error&) { if (claimed && !attempt.markerCreateAttempted) store->freshLeaseClaimed_.store(false); attempt.reason = "v4_foreground_resource_unavailable"; return {}; }
        catch (...) { if (claimed && !attempt.markerCreateAttempted) store->freshLeaseClaimed_.store(false); attempt.reason = "v4_foreground_acquire_unavailable"; return {}; }
    }
    V4ForegroundGenesisResult InitializeTypedGenesis(const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
        std::size_t tokenBudget, std::size_t byteBudget, std::size_t indexByteBudget) noexcept
    {
        V4ForegroundGenesisResult out;
        bool expectedBusy = false;
        if (!busy_.compare_exchange_strong(expectedBusy, true)) { out.reason = "v4_foreground_operation_in_progress"; return out; }
        struct Guard { std::atomic_bool& busy; ~Guard() { busy.store(false); } } guard{busy_};
        const auto attempted = [&] { return out.publications[0].temporaryCreateAttempted || out.publications[1].temporaryCreateAttempted ||
            out.publications[0].temporaryCreated || out.publications[1].temporaryCreated || out.publications[0].renameAttempted || out.publications[1].renameAttempted; };
        try
        {
            if (poisoned_ || head_ || generation_ || !MarkerLive()) { out.reason = "v4_foreground_genesis_unavailable"; return out; }
            const V4StructuredDfsCursor initial;
            auto genesis = PublishV4TypedCursorShard(*store_, initial, false, binding_, coordinateBudget, tokenBudget, byteBudget);
            out.publications[0] = std::move(genesis.publication);
            if (!genesis.published)
            { poisoned_ = attempted(); out.resourceUnavailable = genesis.resourceUnavailable; out.reason = "v4_foreground_genesis_shard_unavailable"; return out; }
            V4ShardLocator data{out.publications[0].fileName, out.publications[0].fileId};
            auto index = AppendV4ShardIndex(*store_, std::nullopt, data, "global_cursor", binding_, coordinateBudget,
                axisBudget, tokenBudget, byteBudget, byteBudget, 0, byteBudget, indexByteBudget);
            out.publications[1] = std::move(index.publication);
            if (!index.published || !index.head)
            { poisoned_ = true; out.resourceUnavailable = index.resourceUnavailable; out.reason = "v4_foreground_genesis_index_unavailable"; return out; }
            auto pin = store_->PinRead(index.head->name, index.head->fileId);
            const auto read = store_->Read(index.head->name, index.head->fileId, binding_, indexByteBudget);
            if (!pin || read.status != CodecStatus::decoded || read.recoveredInFlight || !read.state.shardIndex || !MarkerLive())
            { poisoned_ = true; out.resourceUnavailable = read.status == CodecStatus::resource_unavailable; out.reason = "v4_foreground_genesis_readback_unavailable"; return out; }
            const auto& node = *read.state.shardIndex;
            if (node.generation != "1" || !node.previousName.empty() || node.dataKind != "global_cursor" ||
                node.dataName != std::string(data.name.begin(), data.name.end()) || node.dataVolume != std::to_string(data.fileId.VolumeSerialNumber) ||
                node.dataFileId != V4ShardIdHex(data.fileId))
            { poisoned_ = true; out.reason = "v4_foreground_genesis_link_unavailable"; return out; }
            auto headCopy = *index.head; auto bytesCopy = read.bytes; out.head = headCopy;
            head_ = std::move(headCopy); headBytes_ = std::move(bytesCopy); indexGeneration_ = "1"; headPin_ = std::move(pin); generation_ = 1;
            out.generation = generation_; out.ready = true; out.reason = "v4_foreground_genesis_live_preview_only"; return out;
        }
        catch (const std::bad_alloc&) { poisoned_ = attempted(); out.resourceUnavailable = true; out.reason = "v4_foreground_genesis_resource_unavailable"; return out; }
        catch (const std::length_error&) { poisoned_ = attempted(); out.resourceUnavailable = true; out.reason = "v4_foreground_genesis_resource_unavailable"; return out; }
        catch (...) { poisoned_ = attempted(); out.reason = "v4_foreground_genesis_unavailable"; return out; }
    }
    bool Current(std::uint64_t generation, const V4ShardLocator& head, std::size_t byteBudget,
        bool* resourceUnavailable = nullptr) noexcept
    {
        if (resourceUnavailable) *resourceUnavailable = false;
        try
        {
            if (poisoned_ || !head_ || !headPin_ || generation_ != generation || !MarkerLive() ||
                head_->name != head.name || !CheckpointStore::SameIdentity(head_->fileId, head.fileId)) return false;
            const auto current = store_->Read(head.name, head.fileId, binding_, byteBudget);
            if (resourceUnavailable) *resourceUnavailable = current.status == CodecStatus::resource_unavailable;
            return current.status == CodecStatus::decoded && !current.recoveredInFlight && current.bytes == headBytes_;
        }
        catch (const std::bad_alloc&) { if (resourceUnavailable) *resourceUnavailable = true; return false; }
        catch (const std::length_error&) { if (resourceUnavailable) *resourceUnavailable = true; return false; }
        catch (...) { return false; }
    }
    std::uint64_t Generation() const noexcept { return ownerThread_ && GetCurrentThreadId() == ownerThread_ && !poisoned_ ? generation_ : 0; }
    const V4ShardLocator* Head() const noexcept { return Generation() && head_ ? &*head_ : nullptr; }
    std::shared_ptr<V4ForegroundReservation> Reserve(std::uint64_t expectedGeneration,
        const V4ShardLocator& expectedHead, std::size_t byteBudget, bool& resourceUnavailable) noexcept;
};

// One reservation fences a single head transition until it is committed or
// abandoned. Once filesystem mutation has been attempted, abandonment poisons
// this live lease rather than allowing a fork from the old head.
class V4ForegroundReservation final
{
    friend class V4ForegroundLease;
    friend class V4NativeProjectionLiveJournal;
    friend class V4NativeStagedFamilyPending;
    friend class CheckpointStore;
    friend class V4NativeStagedFamilyRecovery;
    friend class V4NativeReceiptAncestryRecheck;
    friend class V4NativeProjectionJournalRecovery;
    friend class V4NativeCaseIndexPending;
    std::shared_ptr<V4ForegroundLease> lease_;
    std::weak_ptr<V4ForegroundReservation> self_;
    V4ShardLocator before_;
    std::uint64_t generation_ = 0;
    std::size_t headBudget_ = 0;
    std::shared_ptr<std::atomic_bool> operationFence_ = std::make_shared<std::atomic_bool>(false);
    bool active_ = true, attempted_ = false, advanced_ = false;
    V4ForegroundReservation() = default;
    static bool Canonical(const std::shared_ptr<V4ForegroundReservation>& value) noexcept
    {
        const auto owner = value ? value->self_.lock() : std::shared_ptr<V4ForegroundReservation>{};
        return owner && !owner.owner_before(value) && !value.owner_before(owner);
    }
    bool Matches(CheckpointStore& store, const V4ShardLocator& head, std::size_t byteBudget,
        bool* resourceUnavailable = nullptr) noexcept
    {
        if (resourceUnavailable) *resourceUnavailable = false;
        return active_ && lease_ && lease_->store_.get() == &store && lease_->generation_ == generation_ &&
            before_.name == head.name && CheckpointStore::SameIdentity(before_.fileId, head.fileId) &&
            lease_->Current(generation_, head, byteBudget, resourceUnavailable);
    }
    void MarkAttempted() noexcept { attempted_ = true; }
    void FailAttempt() noexcept { attempted_ = true; if (lease_) lease_->poisoned_.store(true); }
    bool Commit(const V4ShardLocator& after, std::size_t oldBudget, std::size_t newBudget, bool& resourceUnavailable) noexcept
    {
        resourceUnavailable = false;
        try
        {
            if (active_ && advanced_ && lease_ && generation_ != (std::numeric_limits<std::uint64_t>::max)())
                return lease_->Current(generation_ + 1, after, newBudget, &resourceUnavailable);
            if (!active_ || !lease_ || !lease_->store_ || !Matches(*lease_->store_, before_, oldBudget) ||
                generation_ == (std::numeric_limits<std::uint64_t>::max)()) return false;
            auto pin = lease_->store_->PinRead(after.name, after.fileId);
            const auto read = lease_->store_->Read(after.name, after.fileId, lease_->binding_, newBudget);
            if (!pin || read.status != CodecStatus::decoded || read.recoveredInFlight || !read.state.shardIndex)
            { resourceUnavailable = read.status == CodecStatus::resource_unavailable; return false; }
            const auto& node = *read.state.shardIndex;
            auto nextIndexGeneration = lease_->indexGeneration_; Increment(nextIndexGeneration);
            if (node.previousName != std::string(before_.name.begin(), before_.name.end()) ||
                node.previousVolume != std::to_string(before_.fileId.VolumeSerialNumber) || node.previousFileId != V4ShardIdHex(before_.fileId) ||
                node.generation != nextIndexGeneration || !lease_->MarkerLive()) return false;
            auto newHead = after; auto newBytes = read.bytes;
            lease_->head_ = std::move(newHead); lease_->headBytes_ = std::move(newBytes); lease_->indexGeneration_ = std::move(nextIndexGeneration); lease_->headPin_ = std::move(pin);
            ++lease_->generation_; advanced_ = true; return true;
        }
        catch (const std::bad_alloc&) { resourceUnavailable = true; return false; }
        catch (const std::length_error&) { resourceUnavailable = true; return false; }
        catch (...) { return false; }
    }
    void Finish() noexcept { if (active_ && lease_ && advanced_) { active_ = false; lease_->busy_.store(false); } }
    void FinishReadOnly() noexcept { if (active_ && lease_ && !attempted_ && !advanced_) { active_ = false; lease_->busy_.store(false); } }
public:
    V4ForegroundReservation(const V4ForegroundReservation&) = delete;
    V4ForegroundReservation& operator=(const V4ForegroundReservation&) = delete;
    ~V4ForegroundReservation()
    {
        if (active_ && lease_)
        { if (attempted_) lease_->poisoned_ = true; lease_->busy_.store(false); }
    }
};
inline std::shared_ptr<V4ForegroundReservation> V4ForegroundLease::Reserve(std::uint64_t expectedGeneration,
    const V4ShardLocator& expectedHead, std::size_t byteBudget, bool& resourceUnavailable) noexcept
{
    resourceUnavailable = false;
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true)) return {};
    try
    {
        const auto self = self_.lock();
        if (!self || self.get() != this || !Current(expectedGeneration, expectedHead, byteBudget, &resourceUnavailable)) { busy_.store(false); return {}; }
        auto reservation = std::shared_ptr<V4ForegroundReservation>(new V4ForegroundReservation);
        reservation->lease_ = self; reservation->self_ = reservation; reservation->before_ = expectedHead;
        reservation->generation_ = expectedGeneration; reservation->headBudget_ = byteBudget; return reservation;
    }
    catch (const std::bad_alloc&) { busy_.store(false); resourceUnavailable = true; return {}; }
    catch (const std::length_error&) { busy_.store(false); resourceUnavailable = true; return {}; }
    catch (...) { busy_.store(false); return {}; }
}

struct V4ShardCaseKey
{
    std::string familyCoordinate, candidateId, kind, graphMode, graphId, presence;
    bool operator==(const V4ShardCaseKey&) const = default;
};
inline std::optional<V4ShardCaseKey> V4ShardReplayedCaseKey(const V4CaseShardResult& shard)
{
    if (!shard.identityReplayed || !shard.shard || shard.shard->families.size() != 1 ||
        shard.shard->families.front().cases.size() != 1) return std::nullopt;
    const auto& family = shard.shard->families.front(); const auto& item = family.cases.front();
    return V4ShardCaseKey{family.coordinate, item.candidateId, item.kind, item.graphMode, item.graphId, item.presence};
}
enum class V4ShardLookupStatus { progress, found, not_found, resource_unavailable, unavailable };
struct V4ShardLookupStep
{
    V4ShardLookupStatus status = V4ShardLookupStatus::unavailable;
    std::optional<V4ShardLocator> matched;
    std::size_t workUsed = 0;
    std::string_view reason;
};
// A process-local bounded scan anchored to one caller-trusted immutable head.
// Its private cursor cannot be advanced by persisted untrusted bytes. On a
// restart, rebuild from the trusted head; a saved offset alone is not proof.
class V4ShardCaseLookup final
{
    V4ShardCaseKey key_;
    V4ShardIndexCursor cursor_;
    bool done_ = false;
    V4ShardCaseLookup(V4ShardLocator head, V4ShardCaseKey key)
        : key_(std::move(key)), cursor_{std::move(head), {}} {}
public:
    V4ShardCaseLookup(const V4ShardCaseLookup&) = delete;
    V4ShardCaseLookup& operator=(const V4ShardCaseLookup&) = delete;
    static std::unique_ptr<V4ShardCaseLookup> Begin(V4ShardLocator trustedHead, V4ShardCaseKey key)
    { return std::unique_ptr<V4ShardCaseLookup>(new V4ShardCaseLookup(std::move(trustedHead), std::move(key))); }
    V4ShardLookupStep Step(CheckpointStore& store, std::string_view binding, const V4CoordinateBudget& coordinateBudget,
        const V4AxisBudget& axisBudget, std::size_t replayWorkBudget, std::size_t shardByteBudget,
        std::size_t indexByteBudget, std::size_t nodeBudget)
    {
        V4ShardLookupStep out;
        try
        {
            if (done_) { out.reason = "v4_shard_lookup_already_finished"; return out; }
            auto cursor = cursor_;
            while (out.workUsed < nodeBudget)
            {
                const auto page = ReadV4ShardIndexPage(store, cursor, binding, indexByteBudget);
                if (!page.read && !page.exhausted)
                { out.status = page.resourceUnavailable ? V4ShardLookupStatus::resource_unavailable : V4ShardLookupStatus::unavailable;
                  out.reason = "v4_shard_lookup_index_unavailable"; out.workUsed = 0; return out; }
                if (!page.read)
                { cursor_ = std::move(cursor); done_ = true; out.status = V4ShardLookupStatus::not_found;
                  out.reason = "v4_shard_lookup_absent_from_this_live_head_only"; return out; }
                ++out.workUsed;
                if (page.kind == "native_projection_page" || page.kind == "native_staged_family" || page.kind == "native_staged_case")
                { out.status = V4ShardLookupStatus::unavailable; out.reason = "v4_staged_native_history_requires_replay"; return out; }
                if (page.kind == "case")
                {
                    const auto shard = ReadV4CaseShard(store, page.data->name, page.data->fileId, binding,
                        coordinateBudget, axisBudget, replayWorkBudget, shardByteBudget);
                    const auto key = V4ShardReplayedCaseKey(shard);
                    if (!key)
                    { out.status = shard.resourceUnavailable ? V4ShardLookupStatus::resource_unavailable : V4ShardLookupStatus::unavailable;
                      out.reason = "v4_shard_lookup_case_unavailable"; out.workUsed = 0; return out; }
                    if (*key == key_)
                    { auto next = page.next; out.matched = page.data; out.status = V4ShardLookupStatus::found;
                      out.reason = "v4_shard_lookup_replayed_identity_only"; cursor_ = std::move(next); done_ = true; return out; }
                }
                cursor = page.next;
                if (page.exhausted)
                { cursor_ = std::move(cursor); done_ = true; out.status = V4ShardLookupStatus::not_found;
                  out.reason = "v4_shard_lookup_absent_from_this_live_head_only"; return out; }
            }
            cursor_ = std::move(cursor); out.status = V4ShardLookupStatus::progress; out.reason = "v4_shard_lookup_pending";
        }
        catch (const std::bad_alloc&) { out = {}; out.status = V4ShardLookupStatus::resource_unavailable; out.reason = "v4_shard_lookup_resource_unavailable"; }
        catch (const std::length_error&) { out = {}; out.status = V4ShardLookupStatus::resource_unavailable; out.reason = "v4_shard_lookup_resource_unavailable"; }
        catch (...) { out = {}; out.reason = "v4_shard_lookup_observation_unavailable"; }
        return out;
    }
};

// Reverse traversal returns the newest cursor shard for this exact family.
// It is a bounded process-local lookup over one trusted head, not a claim
// that unlinked shards or historic results are covered.
class V4ShardFamilyRecheck;
class V4ShardFamilyLookup final
{
    friend class V4ShardFamilyRecheck;
    friend class V4TypedShardPreviewRound;
    friend class V4FamilyCasePending;
    friend class V4NativeProjectionLiveJournal;
    friend class V4NativeProjectionJournalRecovery;
    friend class V4NativeStagedFamilyRecovery;
    friend class CheckpointStore;
    friend class V4NativeStagedFamilyPending;
    friend class V4NativeReceiptAncestryRecheck;
    struct Snapshot { V4ShardLocator locator; std::unique_ptr<V4CheckpointReadPin> pin; std::string bytes; std::size_t byteBudget = 0; };
    std::string coordinate_;
    V4ShardLocator head_;
    V4ShardIndexCursor cursor_;
    std::optional<V4ShardLocator> matched_;
    CheckpointStore* store_ = nullptr;
    std::string binding_;
    std::vector<Snapshot> snapshots_;
    std::size_t snapshotBytes_ = 0;
    std::shared_ptr<const unsigned char> identity_ = std::make_shared<const unsigned char>(0);
    mutable std::uint64_t recheckSerial_ = 0;
    bool done_ = false, absent_ = false, failed_ = false, mainMode_ = false;
    bool verifying_ = false, tentativeAbsent_ = false;
    std::size_t verifyIndex_ = 0;
    std::optional<V4ShardLocator> tentativeMatch_;
    V4ShardIndexCursor tentativeCursor_;
    V4ShardFamilyLookup(V4ShardLocator head, std::string coordinate)
        : coordinate_(std::move(coordinate)), head_(head), cursor_{std::move(head), {}} {}
public:
    V4ShardFamilyLookup(const V4ShardFamilyLookup&) = delete;
    V4ShardFamilyLookup& operator=(const V4ShardFamilyLookup&) = delete;
    std::size_t RetainedPins() const noexcept { return snapshots_.size(); }
    std::size_t RetainedBytes() const noexcept { return snapshotBytes_; }
    static std::unique_ptr<V4ShardFamilyLookup> Begin(V4ShardLocator trustedHead, std::string coordinate)
    { return std::unique_ptr<V4ShardFamilyLookup>(new V4ShardFamilyLookup(std::move(trustedHead), std::move(coordinate))); }
    static std::unique_ptr<V4ShardFamilyLookup> BeginMain(V4ShardLocator trustedHead, std::size_t layer)
    {
        auto result = std::unique_ptr<V4ShardFamilyLookup>(new V4ShardFamilyLookup(std::move(trustedHead), std::to_string(layer)));
        result->mainMode_ = true; return result;
    }
    bool Authorizes(CheckpointStore& store, const V4ShardLocator& currentHead, std::string_view binding,
        std::string_view coordinate) const noexcept
    {
        return !mainMode_ && done_ && matched_ && store_ == &store && binding_ == binding && coordinate_ == coordinate &&
            head_.name == currentHead.name && head_.fileId.VolumeSerialNumber == currentHead.fileId.VolumeSerialNumber &&
            std::memcmp(head_.fileId.FileId.Identifier, currentHead.fileId.FileId.Identifier,
                sizeof(head_.fileId.FileId.Identifier)) == 0;
    }
    const std::optional<V4ShardLocator>& Matched() const noexcept { return matched_; }
    bool Revalidate() const noexcept
    {
        if (!store_ || snapshots_.empty()) return false;
        for (const auto& entry : snapshots_)
        {
            const auto loaded = store_->Read(entry.locator.name, entry.locator.fileId, binding_, entry.byteBudget);
            if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight || loaded.bytes != entry.bytes) return false;
        }
        return true;
    }
    bool AuthorizesAbsent(CheckpointStore& store, const V4ShardLocator& currentHead, std::string_view binding,
        std::string_view coordinate) const noexcept
    {
        return !mainMode_ && done_ && absent_ && !matched_ && store_ == &store && binding_ == binding && coordinate_ == coordinate &&
            head_.name == currentHead.name && head_.fileId.VolumeSerialNumber == currentHead.fileId.VolumeSerialNumber &&
            std::memcmp(head_.fileId.FileId.Identifier, currentHead.fileId.FileId.Identifier,
                sizeof(head_.fileId.FileId.Identifier)) == 0;
    }
    bool AuthorizesMainAbsent(CheckpointStore& store, const V4ShardLocator& currentHead, std::string_view binding,
        std::string_view layer) const noexcept
    {
        return mainMode_ && done_ && absent_ && !matched_ && store_ == &store && binding_ == binding && coordinate_ == layer &&
            head_.name == currentHead.name && head_.fileId.VolumeSerialNumber == currentHead.fileId.VolumeSerialNumber &&
            std::memcmp(head_.fileId.FileId.Identifier, currentHead.fileId.FileId.Identifier,
                sizeof(head_.fileId.FileId.Identifier)) == 0;
    }
    V4ShardLookupStep Step(CheckpointStore& store, std::string_view binding, const V4CoordinateBudget& coordinateBudget,
        const V4AxisBudget& axisBudget, std::size_t shardByteBudget, std::size_t indexByteBudget, std::size_t nodeBudget,
        std::size_t replayWorkBudget = 0, std::size_t pinBudget = 0, std::size_t snapshotByteBudget = 0,
        std::size_t genesisTokenBudget = 0)
    {
        V4ShardLookupStep out;
        try
        {
            if (done_ || failed_ || !V4CanonicalDecimal(coordinate_)) { out.reason = "v4_shard_family_lookup_unavailable"; return out; }
            if (store_ && (store_ != &store || binding_ != binding))
            { out.reason = "v4_shard_family_lookup_owner_mismatch"; return out; }
            if (!store_) { binding_ = std::string(binding); store_ = &store; }
            if (snapshots_.size() > pinBudget || snapshotBytes_ > snapshotByteBudget)
            { out.status = V4ShardLookupStatus::resource_unavailable; out.reason = "v4_shard_family_lookup_budget_unavailable"; return out; }
            bool resource = false;
            const auto snapshot = [&](const V4ShardLocator& locator, std::size_t limit) {
                auto found = std::find_if(snapshots_.begin(), snapshots_.end(), [&](const auto& entry) {
                    return entry.locator.name == locator.name && entry.locator.fileId.VolumeSerialNumber == locator.fileId.VolumeSerialNumber &&
                        std::memcmp(entry.locator.fileId.FileId.Identifier, locator.fileId.FileId.Identifier, sizeof(locator.fileId.FileId.Identifier)) == 0;
                });
                if (found == snapshots_.end())
                {
                    if (snapshots_.size() >= pinBudget) { resource = true; return false; }
                    auto pin = store.PinRead(locator.name, locator.fileId); if (!pin) return false;
                    auto loaded = store.Read(locator.name, locator.fileId, binding, limit);
                    if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight)
                    { resource = loaded.status == CodecStatus::resource_unavailable; return false; }
                    if (snapshotBytes_ > snapshotByteBudget || loaded.bytes.size() > snapshotByteBudget - snapshotBytes_)
                    { resource = true; return false; }
                    const auto n = loaded.bytes.size(); snapshots_.push_back({locator, std::move(pin), std::move(loaded.bytes), limit}); snapshotBytes_ += n; return true;
                }
                const auto loaded = store.Read(locator.name, locator.fileId, binding, limit);
                if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight)
                { resource = loaded.status == CodecStatus::resource_unavailable; return false; }
                return loaded.bytes == found->bytes;
            };
            const auto verify = [&]() {
                while (verifyIndex_ < snapshots_.size() && out.workUsed < nodeBudget)
                {
                    const auto& entry = snapshots_[verifyIndex_];
                    const auto loaded = store.Read(entry.locator.name, entry.locator.fileId, binding, entry.byteBudget);
                    if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight || loaded.bytes != entry.bytes)
                    { out.status = loaded.status == CodecStatus::resource_unavailable ? V4ShardLookupStatus::resource_unavailable : V4ShardLookupStatus::unavailable;
                      if (out.status == V4ShardLookupStatus::unavailable) failed_ = true;
                      out.reason = "v4_shard_family_lookup_snapshot_changed"; return false; }
                    ++verifyIndex_; ++out.workUsed;
                }
                if (verifyIndex_ != snapshots_.size())
                { out.status = V4ShardLookupStatus::progress; out.reason = "v4_shard_family_lookup_recheck_pending"; return true; }
                out.matched = tentativeMatch_;
                cursor_ = std::move(tentativeCursor_); matched_ = std::move(tentativeMatch_); absent_ = tentativeAbsent_;
                verifying_ = false; done_ = true;
                out.status = absent_ ? V4ShardLookupStatus::not_found : V4ShardLookupStatus::found;
                out.reason = absent_ ? "v4_shard_family_absent_from_this_live_head_only" : "v4_shard_family_latest_cursor_only";
                return true;
            };
            if (verifying_) { (void)verify(); return out; }
            auto cursor = cursor_;
            while (out.workUsed < nodeBudget)
            {
                if (cursor.next && !snapshot(*cursor.next, indexByteBudget))
                { out.status = resource ? V4ShardLookupStatus::resource_unavailable : V4ShardLookupStatus::unavailable; out.workUsed = 0; return out; }
                const auto page = ReadV4ShardIndexPage(store, cursor, binding, indexByteBudget);
                if (!page.read && !page.exhausted)
                { out.status = page.resourceUnavailable ? V4ShardLookupStatus::resource_unavailable : V4ShardLookupStatus::unavailable;
                  out.reason = "v4_shard_family_lookup_index_unavailable"; out.workUsed = 0; return out; }
                if (!page.read)
                { tentativeCursor_ = std::move(cursor); tentativeAbsent_ = true; tentativeMatch_.reset();
                  verifying_ = true; verifyIndex_ = 0; (void)verify(); return out; }
                ++out.workUsed;
                if (page.kind == "native_projection_page" || page.kind == "native_staged_family" || page.kind == "native_staged_case")
                { out.status = V4ShardLookupStatus::unavailable; out.reason = "v4_staged_native_history_requires_replay"; return out; }
                if (page.kind == "global_cursor" && mainMode_)
                {
                    if (!page.data || !snapshot(*page.data, shardByteBudget))
                    { out.status = resource ? V4ShardLookupStatus::resource_unavailable : V4ShardLookupStatus::unavailable; out.workUsed = 0; return out; }
                    const auto shard = ReadV4TypedCursorShard(store, page.data->name, page.data->fileId, binding,
                        coordinateBudget, genesisTokenBudget, shardByteBudget);
                    if (!shard.cursorValidated || !shard.cursor || shard.deferred || shard.cursor->stopDepth ||
                        !shard.cursor->prefix.empty() || shard.cursor->nextChoice != std::vector<std::uint32_t>{0} || shard.cursor->exhausted)
                    { out.status = shard.resourceUnavailable ? V4ShardLookupStatus::resource_unavailable : V4ShardLookupStatus::unavailable;
                      out.reason = "v4_main_genesis_lookup_cursor_unavailable"; return out; }
                    if (std::to_string(shard.cursor->layer) == coordinate_)
                    { tentativeCursor_ = page.next; tentativeAbsent_ = false; tentativeMatch_ = page.data;
                      verifying_ = true; verifyIndex_ = 0; (void)verify(); return out; }
                }
                if (page.kind == "family" && !mainMode_)
                {
                    if (!page.data || !snapshot(*page.data, shardByteBudget))
                    { out.status = resource ? V4ShardLookupStatus::resource_unavailable : V4ShardLookupStatus::unavailable; out.workUsed = 0; return out; }
                    const auto shard = ReadV4FamilyCursorShard(store, page.data->name, page.data->fileId, binding,
                        coordinateBudget, axisBudget, shardByteBudget, replayWorkBudget);
                    if (!shard.cursorValidated || !shard.family)
                    { out.status = shard.resourceUnavailable ? V4ShardLookupStatus::resource_unavailable : V4ShardLookupStatus::unavailable;
                      out.reason = "v4_shard_family_lookup_data_unavailable"; out.workUsed = 0; return out; }
                    if (shard.family->coordinate == coordinate_)
                    { tentativeCursor_ = page.next; tentativeAbsent_ = false; tentativeMatch_ = page.data;
                      verifying_ = true; verifyIndex_ = 0; (void)verify(); return out; }
                }
                cursor = page.next;
                if (page.exhausted)
                { tentativeCursor_ = std::move(cursor); tentativeAbsent_ = true; tentativeMatch_.reset();
                  verifying_ = true; verifyIndex_ = 0; (void)verify(); return out; }
            }
            cursor_ = std::move(cursor); out.status = V4ShardLookupStatus::progress; out.reason = "v4_shard_family_lookup_pending";
        }
        catch (const std::bad_alloc&) { out.status = V4ShardLookupStatus::resource_unavailable; out.reason = "v4_shard_family_lookup_resource_unavailable"; }
        catch (const std::length_error&) { out.status = V4ShardLookupStatus::resource_unavailable; out.reason = "v4_shard_family_lookup_resource_unavailable"; }
        catch (...) { out = {}; out.reason = "v4_shard_family_lookup_observation_unavailable"; }
        return out;
    }
};

// Bounded re-read of a completed family lookup. This is process-local and
// retains no authority after the lookup/pins are destroyed. A system read may
// itself block; the budget counts files, not elapsed time.
class V4ShardFamilyRecheck final
{
    const V4ShardFamilyLookup* lookup_ = nullptr;
    std::weak_ptr<const unsigned char> identity_;
    std::uint64_t serial_ = 0;
    std::size_t next_ = 0;
    bool complete_ = false, failed_ = false;
    explicit V4ShardFamilyRecheck(const V4ShardFamilyLookup& lookup, std::uint64_t serial) noexcept
        : lookup_(&lookup), identity_(lookup.identity_), serial_(serial) {}
public:
    V4ShardFamilyRecheck(const V4ShardFamilyRecheck&) = delete;
    V4ShardFamilyRecheck& operator=(const V4ShardFamilyRecheck&) = delete;
    static std::unique_ptr<V4ShardFamilyRecheck> Begin(const V4ShardFamilyLookup& lookup)
    {
        if (!lookup.done_ || lookup.failed_ || !lookup.store_ || lookup.snapshots_.empty() ||
            lookup.recheckSerial_ == (std::numeric_limits<std::uint64_t>::max)()) return {};
        const auto serial = ++lookup.recheckSerial_;
        return std::unique_ptr<V4ShardFamilyRecheck>(new V4ShardFamilyRecheck(lookup, serial));
    }
    V4ShardLookupStep Step(std::size_t fileBudget) noexcept
    {
        V4ShardLookupStep out;
        const auto liveIdentity = identity_.lock();
        if (!lookup_ || !liveIdentity || failed_ || complete_ || !lookup_->done_ || lookup_->failed_ || !lookup_->store_)
        { out.reason = "v4_family_recheck_unavailable"; return out; }
        while (next_ < lookup_->snapshots_.size() && out.workUsed < fileBudget)
        {
            const auto& entry = lookup_->snapshots_[next_];
            const auto loaded = lookup_->store_->Read(entry.locator.name, entry.locator.fileId, lookup_->binding_, entry.byteBudget);
            if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight || loaded.bytes != entry.bytes)
            { out.status = loaded.status == CodecStatus::resource_unavailable ? V4ShardLookupStatus::resource_unavailable : V4ShardLookupStatus::unavailable;
              if (out.status == V4ShardLookupStatus::unavailable) failed_ = true;
              out.reason = "v4_family_recheck_snapshot_changed"; return out; }
            ++next_; ++out.workUsed;
        }
        complete_ = next_ == lookup_->snapshots_.size();
        out.status = complete_ ? V4ShardLookupStatus::found : V4ShardLookupStatus::progress;
        out.reason = complete_ ? "v4_family_recheck_complete_not_authenticated" : "v4_family_recheck_pending"; return out;
    }
    bool Matches(const V4ShardFamilyLookup& lookup) const noexcept
    {
        const auto identity = identity_.lock();
        return complete_ && !failed_ && lookup_ == &lookup && identity && lookup.identity_ &&
            !identity.owner_before(lookup.identity_) && !lookup.identity_.owner_before(identity) && lookup.done_ && !lookup.failed_;
    }
    std::uint64_t Serial() const noexcept { return serial_; }
};

struct V4NativeProjectionPublication
{
    bool staged = false, resourceUnavailable = false;
    std::optional<V4ShardLocator> head, page;
    std::array<PublishedCheckpoint, 2> publications;
    std::string_view reason;
};
// Single-foreground journal writer. It retains the same authority Work and
// never advances it until the new indexed page has been read back and adopted.
// It is a preview capability, not proof of a trusted restart or execution.
class V4NativeProjectionLiveJournal final
{
    friend class V4NativeProjectionJournalRecovery;
    std::shared_ptr<CheckpointStore> store_;
    std::shared_ptr<const V4ShardFamilyLookup> lookup_;
    std::shared_ptr<const V4NativeStagedFamilyReceipt> nextParent_;
    std::shared_ptr<V4ForegroundReservation> initialReservation_;
    V4ShardLocator baseHead_, head_;
    V4FamilyDescriptor family_;
    V4NativeProjectionPageAudit seed_;
    std::string binding_;
    std::unique_ptr<event_graph::PresenceProjectionWork> work_;
    std::unique_ptr<event_graph::PresenceProjectionWorkProposal> pendingProposal_;
    std::shared_ptr<V4ForegroundReservation> pendingReservation_;
    std::optional<V4ShardLocator> pendingHead_, pendingPage_;
    std::string pendingPageBytes_, pendingHeadBytes_;
    std::unique_ptr<V4CheckpointReadPin> pendingPagePin_, pendingHeadPin_;
    std::size_t byteBudget_ = 0, indexByteBudget_ = 0, tokenBudget_ = 0, replayBudget_ = 0;
    std::size_t pageCount_ = 0;
    std::uint64_t precheckSerial_ = 0;
    DWORD ownerThread_ = 0;
    std::shared_ptr<std::atomic_bool> activeFence_;
    bool failed_ = false, ownsParent_ = false, ownsActivity_ = false;
    V4NativeProjectionLiveJournal() = default;
    static bool Same(const V4ShardLocator& a, const V4ShardLocator& b) noexcept
    { return a.name == b.name && a.fileId.VolumeSerialNumber == b.fileId.VolumeSerialNumber &&
        std::memcmp(a.fileId.FileId.Identifier, b.fileId.FileId.Identifier, sizeof(a.fileId.FileId.Identifier)) == 0; }
public:
    V4NativeProjectionLiveJournal(const V4NativeProjectionLiveJournal&) = delete;
    V4NativeProjectionLiveJournal& operator=(const V4NativeProjectionLiveJournal&) = delete;
    ~V4NativeProjectionLiveJournal();
    static std::unique_ptr<V4NativeProjectionLiveJournal> BeginNext(
        const V4NativeReceiptAncestryProof&, const V4ShardFamilyRecheck&,
        std::size_t preambleBudget, std::size_t tokenBudget, std::size_t replayBudget,
        std::size_t byteBudget, std::size_t indexByteBudget, bool& resourceUnavailable);
    static std::unique_ptr<V4NativeProjectionLiveJournal> Begin(std::shared_ptr<CheckpointStore> store,
        std::shared_ptr<const V4ShardFamilyLookup> lookup, const V4ShardFamilyRecheck& precheck,
        const V4ShardLocator& head, std::string_view coordinate, std::string_view binding,
        const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
        std::size_t advanceWorkBudget, std::size_t preambleBudget, std::size_t tokenBudget,
        std::size_t replayBudget, std::size_t byteBudget, std::size_t indexByteBudget,
        bool& resourceUnavailable)
    {
        resourceUnavailable = false;
        try
        {
            if (!store || !lookup || !advanceWorkBudget || !lookup->Authorizes(*store, head, binding, coordinate) ||
                !lookup->Matched() || !precheck.Matches(*lookup)) return {};
            const auto family = ReadV4FamilyCursorShard(*store, lookup->Matched()->name, lookup->Matched()->fileId,
                binding, coordinateBudget, axisBudget, byteBudget, replayBudget);
            if (!family.cursorValidated || !family.family || family.family->coordinate != coordinate || family.family->scanExhausted)
            { resourceUnavailable = family.resourceUnavailable; return {}; }
            const auto decoded = NextV4FamilyCoordinate(coordinate, coordinateBudget);
            if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family)
            { resourceUnavailable = decoded.status == V4FamilyCoordinateStatus::resource_unavailable; return {}; }
            const auto before = event_graph::V4FamilyCandidateGraph::ParseCursor(*decoded.family, axisBudget, family.family->candidateCursor);
            if (before.status != event_graph::V4FamilyTraversalStatus::ready || !before.errors.empty())
            { resourceUnavailable = before.status == event_graph::V4FamilyTraversalStatus::resource_unavailable; return {}; }
            const auto advance = event_graph::V4FamilyCandidateGraph::PrepareStagedNativeAdvance(*decoded.family, axisBudget, before.cursor, advanceWorkBudget);
            if (advance.status != event_graph::V4FamilyTraversalStatus::ready || !advance.proposal || !advance.errors.empty())
            { resourceUnavailable = advance.status == event_graph::V4FamilyTraversalStatus::resource_unavailable; return {}; }
            const auto& page = advance.proposal->Page();
            const auto encoded = EncodeV4ProjectionAdvance(*decoded.family, axisBudget, page, byteBudget);
            if (!encoded.encoded) { resourceUnavailable = encoded.resourceUnavailable; return {}; }
            const auto identity = V4FamilyIdentityContext::Create(*decoded.family, axisBudget);
            const auto verified = V4FamilyValidatedContext::Create(*decoded.family, axisBudget, page.candidate);
            if (identity.Status() != V4FamilyStatus::schema_only || verified.Status() != V4CoordinateStatus::coordinate_ready)
            { resourceUnavailable = identity.Status() == V4FamilyStatus::resource_unavailable || verified.Status() == V4CoordinateStatus::resource_unavailable; return {}; }
            std::vector<std::string> errors;
            auto work = event_graph::V4NativeJoinedGraph::CreateProjectionWork(identity, verified, page.pendingPresent,
                preambleBudget, resourceUnavailable, errors);
            if (!work || resourceUnavailable || !errors.empty()) return {};
            auto result = std::unique_ptr<V4NativeProjectionLiveJournal>(new V4NativeProjectionLiveJournal);
            result->store_ = std::move(store); result->lookup_ = std::move(lookup); result->baseHead_ = head; result->head_ = head;
            result->family_ = std::move(*decoded.family); result->binding_ = std::string(binding); result->work_ = std::move(work);
            result->byteBudget_ = byteBudget; result->indexByteBudget_ = indexByteBudget; result->tokenBudget_ = tokenBudget;
            result->replayBudget_ = replayBudget; result->precheckSerial_ = precheck.Serial(); result->ownerThread_ = GetCurrentThreadId();
            auto& seed = result->seed_; seed.familyCoordinate = coordinate; seed.familyCursor = family.family->candidateCursor;
            seed.candidateId = page.candidateId; seed.candidateOrdinal = page.candidateOrdinal; seed.leafOrdinal = page.pendingOrdinal;
            for (bool bit : page.pendingPresent) seed.leafPresence += bit ? '1' : '0';
            seed.advanceBytes = encoded.bytes; seed.baseHeadName.assign(head.name.begin(), head.name.end());
            seed.baseHeadVolume = std::to_string(head.fileId.VolumeSerialNumber); seed.baseHeadFileId = V4ShardIdHex(head.fileId);
            seed.coordinateBudget = coordinateBudget; seed.axisBudget = axisBudget; seed.advanceWorkBudget = advanceWorkBudget;
            seed.preambleBudget = preambleBudget;
            return result;
        }
        catch (const std::bad_alloc&) { resourceUnavailable = true; return {}; }
        catch (const std::length_error&) { resourceUnavailable = true; return {}; }
        catch (...) { return {}; }
    }
    V4NativeProjectionPublication PublishPage(const std::shared_ptr<V4ForegroundReservation>& reservation, const V4ShardLocator& currentHead,
        const V4ShardFamilyRecheck& precheck, std::size_t workBudget, std::size_t outputLimit)
    {
        V4NativeProjectionPublication out;
        const auto attempted = [&] { return out.publications[0].temporaryCreateAttempted || out.publications[1].temporaryCreateAttempted ||
            out.publications[0].temporaryCreated || out.publications[1].temporaryCreated ||
            out.publications[0].renameAttempted || out.publications[1].renameAttempted; };
        try
        {
            // Nested projection replay must retain/revalidate every ancestor
            // journal; until its durable recovery is connected, keep this leaf
            // prepared and do not publish a head that cannot be recovered.
            if (nextParent_) { out.reason = "v4_nested_projection_journal_recovery_unimplemented"; return out; }
            auto activity = reservation ? reservation->operationFence_ : std::shared_ptr<std::atomic_bool>{};
            bool idle = false;
            if (!activity || !activity->compare_exchange_strong(idle, true))
            { out.reason = "v4_projection_journal_operation_in_progress"; return out; }
            bool keepActivity = false;
            auto release = [&](void*) noexcept {
                if (!keepActivity) { if (attempted() && reservation) reservation->FailAttempt(); activity->store(false); }
            };
            std::unique_ptr<void, decltype(release)> activityGuard(&keepActivity, release);
            if (!V4ForegroundReservation::Canonical(reservation) || (nextParent_ && !pageCount_ &&
                    (!initialReservation_ || initialReservation_.owner_before(reservation) || reservation.owner_before(initialReservation_))) ||
                failed_ || !ownerThread_ || GetCurrentThreadId() != ownerThread_ || pendingProposal_ || !store_ ||
                !reservation->Matches(*store_, currentHead, reservation->headBudget_) || !lookup_ || !work_ || !Same(head_, currentHead) ||
                !precheck.Matches(*lookup_) || precheck.Serial() < precheckSerial_ ||
                work_->Cursor().stage == event_graph::PresenceProjectionWorkStage::done ||
                pageCount_ == (std::numeric_limits<std::size_t>::max)())
            { out.reason = "v4_projection_journal_state_unavailable"; return out; }
            bool resource = false;
            auto proposal = work_->PrepareStep(workBudget, outputLimit, resource);
            if (!proposal || resource) { out.resourceUnavailable = resource; out.reason = "v4_projection_journal_prepare_unavailable"; return out; }
            const auto encoded = EncodeV4ProjectionPage(proposal->Page(), byteBudget_);
            const auto before = event_graph::EncodePresenceProjectionCursor(proposal->Page().before);
            const auto after = event_graph::EncodePresenceProjectionCursor(proposal->Page().after);
            if (!encoded.encoded || !before.cursor || !after.cursor ||
                (before.encoded == after.encoded && !proposal->Page().workUsed))
            { out.resourceUnavailable = encoded.resourceUnavailable || before.resourceUnavailable || after.resourceUnavailable;
              out.reason = "v4_projection_journal_page_unavailable"; return out; }
            auto audit = seed_; audit.pageOrdinal = std::to_string(pageCount_); audit.pageBytes = encoded.bytes;
            audit.workBudget = workBudget; audit.outputLimit = outputLimit;
            auto page = PublishV4NativeProjectionPageShard(*store_, audit, binding_, byteBudget_);
            out.publications[0] = std::move(page.publication);
            if (attempted()) reservation->MarkAttempted();
            if (!page.read) { failed_ = attempted(); out.resourceUnavailable = page.resourceUnavailable; out.reason = "v4_projection_journal_page_publish_unavailable"; return out; }
            out.page = V4ShardLocator{out.publications[0].fileName, out.publications[0].fileId};
            auto index = AppendV4ShardIndex(*store_, head_, *out.page, "native_projection_page", binding_,
                seed_.coordinateBudget, seed_.axisBudget, tokenBudget_, byteBudget_, byteBudget_, replayBudget_, byteBudget_, indexByteBudget_);
            out.publications[1] = std::move(index.publication);
            if (!index.published || !index.head) { failed_ = true; out.resourceUnavailable = index.resourceUnavailable; out.reason = "v4_projection_journal_index_unavailable"; return out; }
            auto pagePin = store_->PinRead(out.page->name, out.page->fileId);
            auto headPin = store_->PinRead(index.head->name, index.head->fileId);
            const auto headRead = store_->Read(index.head->name, index.head->fileId, binding_, indexByteBudget_);
            if (!pagePin || !headPin || headRead.status != CodecStatus::decoded || headRead.recoveredInFlight || !headRead.state.shardIndex)
            { failed_ = true; out.resourceUnavailable = headRead.status == CodecStatus::resource_unavailable; out.reason = "v4_projection_journal_pin_unavailable"; return out; }
            const auto& link = *headRead.state.shardIndex;
            if (link.dataKind != "native_projection_page" || link.dataName != std::string(out.page->name.begin(), out.page->name.end()) ||
                link.dataVolume != std::to_string(out.page->fileId.VolumeSerialNumber) || link.dataFileId != V4ShardIdHex(out.page->fileId) ||
                link.previousName != std::string(head_.name.begin(), head_.name.end()) ||
                link.previousVolume != std::to_string(head_.fileId.VolumeSerialNumber) || link.previousFileId != V4ShardIdHex(head_.fileId))
            { failed_ = true; out.reason = "v4_projection_journal_index_link_mismatch"; return out; }
            pendingPageBytes_ = std::move(page.bytes); pendingHeadBytes_ = headRead.bytes;
            pendingPagePin_ = std::move(pagePin); pendingHeadPin_ = std::move(headPin);
            pendingPage_ = *out.page; pendingHead_ = *index.head; pendingReservation_ = reservation; pendingProposal_ = std::move(proposal);
            precheckSerial_ = lookup_->recheckSerial_; out.head = *index.head; out.staged = true;
            out.reason = "v4_projection_journal_postcheck_and_adoption_pending";
            activeFence_ = std::move(activity); ownsActivity_ = true; keepActivity = true; return out;
        }
        catch (const std::bad_alloc&) { if (attempted() && reservation) reservation->MarkAttempted(); failed_ = attempted(); out.resourceUnavailable = true; out.reason = "v4_projection_journal_resource_unavailable"; return out; }
        catch (const std::length_error&) { if (attempted() && reservation) reservation->MarkAttempted(); failed_ = attempted(); out.resourceUnavailable = true; out.reason = "v4_projection_journal_resource_unavailable"; return out; }
        catch (...) { if (attempted() && reservation) reservation->MarkAttempted(); failed_ = attempted(); out.reason = "v4_projection_journal_publish_unavailable"; return out; }
    }
    bool Adopt(const V4ShardLocator& adoptedHead, const V4ShardFamilyRecheck& postcheck, bool& resourceUnavailable) noexcept
    {
        resourceUnavailable = false;
        try
        {
            if (failed_ || !ownsActivity_ || !activeFence_ || !activeFence_->load() || !ownerThread_ || GetCurrentThreadId() != ownerThread_ || !pendingProposal_ || !V4ForegroundReservation::Canonical(pendingReservation_) || !pendingPage_ || !pendingHead_ || !pendingPagePin_ || !pendingHeadPin_ ||
                !Same(*pendingHead_, adoptedHead) || !postcheck.Matches(*lookup_) || postcheck.Serial() <= precheckSerial_) return false;
            const auto page = store_->Read(pendingPage_->name, pendingPage_->fileId, binding_, byteBudget_);
            const auto head = store_->Read(pendingHead_->name, pendingHead_->fileId, binding_, indexByteBudget_);
            if (page.status != CodecStatus::decoded || head.status != CodecStatus::decoded || page.recoveredInFlight || head.recoveredInFlight ||
                page.bytes != pendingPageBytes_ || head.bytes != pendingHeadBytes_)
            { resourceUnavailable = page.status == CodecStatus::resource_unavailable || head.status == CodecStatus::resource_unavailable; return false; }
            if (!pendingReservation_->Commit(*pendingHead_, pendingReservation_->headBudget_, indexByteBudget_, resourceUnavailable)) return false;
            if (!work_->Commit(*pendingProposal_, resourceUnavailable)) return false;
            head_ = std::move(*pendingHead_); ++pageCount_; precheckSerial_ = postcheck.Serial();
            pendingReservation_->Finish(); pendingReservation_.reset(); pendingProposal_.reset(); pendingPage_.reset(); pendingHead_.reset(); pendingPagePin_.reset(); pendingHeadPin_.reset();
            pendingPageBytes_.clear(); pendingHeadBytes_.clear(); ownsActivity_ = false; activeFence_->store(false); activeFence_.reset(); return true;
        }
        catch (const std::bad_alloc&) { resourceUnavailable = true; return false; }
        catch (const std::length_error&) { resourceUnavailable = true; return false; }
        catch (...) { return false; }
    }
};

struct V4NativeProjectionRecoveryStep
{
    bool complete = false, projectionComplete = false, resourceUnavailable = false, unavailable = false;
    std::size_t workUsed = 0;
    std::string_view reason;
};
// Reconstruct a fixed indexed journal in bounded file/page steps. Pins and
// observed bytes are retained throughout the session; limits may be raised on
// a later call. Byte limits count serialized snapshots, not all heap overhead;
// a system call itself can block. This does not authenticate a latest locator.
class V4NativeProjectionJournalRecovery final
{
    friend class CheckpointStore;
    friend class V4NativeStagedFamilyPending;
    friend class V4NativeStagedFamilyReceipt;
    friend class V4NativeCaseIndexPending;
    friend class V4NativeStagedFamilyRecovery;
    friend class V4NativeReceiptAncestryRecheck;
    friend class V4NativeProjectionLiveJournal;
    struct Entry
    {
        V4ShardLocator node, page;
        std::unique_ptr<V4CheckpointReadPin> nodePin, pagePin;
        std::string nodeBytes, pageBytes;
        V4NativeProjectionPageAudit audit;
    };
    std::shared_ptr<CheckpointStore> store_;
    std::shared_ptr<const V4ShardFamilyLookup> lookup_;
    std::shared_ptr<const V4NativeStagedFamilyReceipt> nestedParent_;
    std::shared_ptr<V4ForegroundReservation> nestedReservation_;
    std::shared_ptr<std::atomic_bool> nestedActivity_;
    std::string nestedBefore_, nestedAdvance_;
    std::size_t nestedParentPins_ = 0, nestedParentBytes_ = 0;
    std::weak_ptr<V4NativeProjectionJournalRecovery> self_;
    V4ShardLocator base_, final_;
    V4ShardIndexCursor cursor_;
    std::string binding_;
    V4NativeProjectionReplayLimits limits_;
    std::vector<Entry> entries_; // newest first, replayed in reverse
    std::unique_ptr<event_graph::PresenceProjectionWork> work_;
    V4FamilyDescriptor family_;
    event_graph::V4FamilyCandidateCursor leafCursor_;
    std::unique_ptr<V4CheckpointReadPin> basePin_;
    std::string baseBytes_;
    std::size_t retainedBytes_ = 0, replayIndex_ = 0, verifyIndex_ = 0, pageWork_ = 0;
    std::uint64_t initialSerial_ = 0, verifySerial_ = 0, readySerial_ = 0;
    DWORD ownerThread_ = 0;
    bool finalSerialFrozen_ = false;
    mutable std::atomic_bool stagingBlocked_{false};
    enum class Phase { scan, ancestry, initialize, replay, verify, ready, failed } phase_ = Phase::scan;
    V4NativeProjectionJournalRecovery() = default;
    static bool Same(const V4ShardLocator& a, const V4ShardLocator& b) noexcept
    { return a.name == b.name && a.fileId.VolumeSerialNumber == b.fileId.VolumeSerialNumber &&
        std::memcmp(a.fileId.FileId.Identifier, b.fileId.FileId.Identifier, sizeof(a.fileId.FileId.Identifier)) == 0; }
    static auto BudgetValues(const V4CoordinateBudget& c, const V4AxisBudget& a) noexcept
    { return std::array<std::size_t, 16>{c.decimalDigits,c.tokenBytes,c.listEntries,c.arithmeticSteps,
        a.logicalMembers,a.fileMembers,a.virtualMembers,a.referenceMembers,a.payloadSlots,a.textSlots,a.requestResultSlots,
        a.fileContentSlots,a.payloadBytes,a.directoryNodeSlots,a.directoryRootSlots,a.preparationSteps}; }
public:
    V4NativeProjectionJournalRecovery(const V4NativeProjectionJournalRecovery&) = delete;
    V4NativeProjectionJournalRecovery& operator=(const V4NativeProjectionJournalRecovery&) = delete;
    ~V4NativeProjectionJournalRecovery() { if (nestedActivity_) nestedActivity_->store(false); }
    // Increase only ceilings, never the saved per-page replay inputs.
    bool RaiseLimits(const V4NativeProjectionReplayLimits& next) noexcept
    {
        if (!ownerThread_ || GetCurrentThreadId() != ownerThread_) return false;
        const auto prior = BudgetValues(limits_.coordinate, limits_.axes), candidate = BudgetValues(next.coordinate, next.axes);
        for (std::size_t i = 0; i < prior.size(); ++i) if (candidate[i] < prior[i]) return false;
        if (next.pages < limits_.pages || next.bytes < limits_.bytes || next.preamble < limits_.preamble ||
            next.advanceWork < limits_.advanceWork || next.totalPageWork < limits_.totalPageWork || next.outputPerPage < limits_.outputPerPage ||
            next.familyReplayWork < limits_.familyReplayWork) return false;
        limits_ = next; return true;
    }
    static std::shared_ptr<V4NativeProjectionJournalRecovery> Begin(std::shared_ptr<CheckpointStore> store,
        std::shared_ptr<const V4ShardFamilyLookup> lookup, const V4ShardFamilyRecheck& precheck,
        const V4ShardLocator& base, const V4ShardLocator& finalHead, std::string_view coordinate,
        std::string_view binding, const V4NativeProjectionReplayLimits& limits, bool& resourceUnavailable)
    {
        resourceUnavailable = false;
        try
        {
            if (!store || !lookup || Same(base, finalHead) || !lookup->Authorizes(*store, base, binding, coordinate) ||
                !lookup->Matched() || !precheck.Matches(*lookup)) return {};
            auto pin = store->PinRead(base.name, base.fileId); if (!pin) return {};
            const auto read = store->Read(base.name, base.fileId, binding, limits.bytes);
            if (read.status != CodecStatus::decoded || read.recoveredInFlight || !read.state.shardIndex)
            { resourceUnavailable = read.status == CodecStatus::resource_unavailable; return {}; }
            auto result = std::shared_ptr<V4NativeProjectionJournalRecovery>(new V4NativeProjectionJournalRecovery);
            result->self_ = result;
            result->store_ = std::move(store); result->lookup_ = std::move(lookup); result->base_ = base; result->final_ = finalHead;
            result->cursor_ = V4ShardIndexCursor{finalHead, {}}; result->binding_ = std::string(binding);
            result->limits_ = limits; result->basePin_ = std::move(pin); result->baseBytes_ = read.bytes;
            result->retainedBytes_ = read.bytes.size(); result->initialSerial_ = precheck.Serial(); result->ownerThread_ = GetCurrentThreadId(); return result;
        }
        catch (const std::bad_alloc&) { resourceUnavailable = true; return {}; }
        catch (const std::length_error&) { resourceUnavailable = true; return {}; }
        catch (...) { return {}; }
    }
    // The live journal must itself own the parent and already have adopted
    // child pages. Scanning does not authorize attachment or execution.
    static std::shared_ptr<V4NativeProjectionJournalRecovery> BeginNested(
        const V4NativeProjectionLiveJournal& journal, const std::shared_ptr<V4ForegroundReservation>& reservation,
        const V4ShardFamilyRecheck& precheck, const V4NativeProjectionReplayLimits& limits,
        bool& resourceUnavailable) noexcept;
    bool AcceptNestedAncestry(const V4NativeReceiptAncestryProof& proof,
        bool& resourceUnavailable) noexcept;
    V4NativeProjectionRecoveryStep Step(std::size_t stepBudget, std::size_t pinBudget, std::size_t retainedByteBudget,
        const V4ShardFamilyRecheck* postcheck = nullptr) noexcept
    {
        V4NativeProjectionRecoveryStep out;
        try
        {
            if (phase_ == Phase::failed || !store_ || !lookup_ || !basePin_ || !ownerThread_ || GetCurrentThreadId() != ownerThread_)
            { out.unavailable = true; out.reason = "v4_projection_recovery_failed"; return out; }
            if (nestedParent_ && (!V4ForegroundReservation::Canonical(nestedReservation_) || !nestedActivity_ ||
                !nestedReservation_->operationFence_ || nestedActivity_.owner_before(nestedReservation_->operationFence_) ||
                nestedReservation_->operationFence_.owner_before(nestedActivity_) || !nestedActivity_->load() ||
                !nestedReservation_->Matches(*store_, final_, nestedReservation_->headBudget_, &out.resourceUnavailable)))
            { out.unavailable = !out.resourceUnavailable; out.reason = "v4_projection_recovery_nested_head_unavailable"; return out; }
            // Parent receipt and every distinct ancestor projection stay live
            // throughout this recovery. Lookup/lease and allocator overhead
            // are outside these snapshot/pin budgets.
            if (nestedParentPins_ > pinBudget || nestedParentBytes_ > retainedByteBudget)
            { out.resourceUnavailable = true; out.reason = "v4_projection_recovery_nested_retention_budget"; return out; }
            pinBudget -= nestedParentPins_; retainedByteBudget -= nestedParentBytes_;
            if (retainedBytes_ > retainedByteBudget || entries_.size() > (pinBudget > 0 ? (pinBudget - 1) / 2 : 0) || !pinBudget)
            { out.resourceUnavailable = true; out.reason = "v4_projection_recovery_retention_budget"; return out; }
            while (out.workUsed < stepBudget)
            {
                if (phase_ == Phase::scan)
                {
                    if (!cursor_.next) { phase_ = Phase::failed; out.unavailable = true; out.reason = "v4_projection_recovery_base_missing"; return out; }
                    if (Same(*cursor_.next, base_))
                    { if (entries_.empty()) { phase_ = Phase::failed; out.unavailable = true; out.reason = "v4_projection_recovery_empty"; return out; }
                      phase_ = nestedParent_ ? Phase::ancestry : Phase::initialize; continue; }
                    if (entries_.size() >= limits_.pages || entries_.size() >= (pinBudget - 1) / 2)
                    { out.resourceUnavailable = true; out.reason = "v4_projection_recovery_pin_or_page_budget"; return out; }
                    const auto nodeLocator = *cursor_.next;
                    auto nodePin = store_->PinRead(nodeLocator.name, nodeLocator.fileId);
                    if (!nodePin) { out.unavailable = true; out.reason = "v4_projection_recovery_index_pin"; return out; }
                    auto nodeRead = store_->Read(nodeLocator.name, nodeLocator.fileId, binding_, limits_.bytes);
                    const auto index = ReadV4ShardIndexPage(*store_, cursor_, binding_, limits_.bytes);
                    if (nodeRead.status != CodecStatus::decoded || nodeRead.recoveredInFlight || !index.read ||
                        index.kind != "native_projection_page" || !index.data || !index.next.next)
                    { out.resourceUnavailable = nodeRead.status == CodecStatus::resource_unavailable || index.resourceUnavailable;
                      out.unavailable = !out.resourceUnavailable; out.reason = "v4_projection_recovery_index_unavailable"; return out; }
                    auto pagePin = store_->PinRead(index.data->name, index.data->fileId);
                    if (!pagePin) { out.unavailable = true; out.reason = "v4_projection_recovery_page_pin"; return out; }
                    auto page = ReadV4NativeProjectionPageShard(*store_, index.data->name, index.data->fileId, binding_, limits_.bytes);
                    if (!page.read || !page.audit)
                    { out.resourceUnavailable = page.resourceUnavailable; out.unavailable = !out.resourceUnavailable; out.reason = "v4_projection_recovery_page_unavailable"; return out; }
                    if (nodeRead.bytes.size() > retainedByteBudget - retainedBytes_ ||
                        page.bytes.size() > retainedByteBudget - retainedBytes_ - nodeRead.bytes.size())
                    { out.resourceUnavailable = true; out.reason = "v4_projection_recovery_byte_budget"; return out; }
                    auto next = index.next;
                    const auto added = nodeRead.bytes.size() + page.bytes.size();
                    entries_.push_back({nodeLocator, *index.data, std::move(nodePin), std::move(pagePin),
                        std::move(nodeRead.bytes), std::move(page.bytes), std::move(*page.audit)});
                    cursor_ = std::move(next); retainedBytes_ += added; ++out.workUsed; continue;
                }
                if (phase_ == Phase::ancestry)
                { out.reason = "v4_projection_recovery_nested_ancestry_pending"; return out; }
                if (phase_ == Phase::initialize)
                {
                    const auto& p = entries_.back().audit;
                    const auto requested = BudgetValues(p.coordinateBudget, p.axisBudget), maximum = BudgetValues(limits_.coordinate, limits_.axes);
                    for (std::size_t i = 0; i < requested.size(); ++i)
                        if (requested[i] > maximum[i]) { out.resourceUnavailable = true; out.reason = "v4_projection_recovery_model_budget"; return out; }
                    if (p.preambleBudget > limits_.preamble || p.advanceWorkBudget > limits_.advanceWork)
                    { out.resourceUnavailable = true; out.reason = "v4_projection_recovery_preamble_budget"; return out; }
                    if (p.pageOrdinal != "0" || p.baseHeadName != std::string(base_.name.begin(), base_.name.end()) ||
                        p.baseHeadVolume != std::to_string(base_.fileId.VolumeSerialNumber) || p.baseHeadFileId != V4ShardIdHex(base_.fileId))
                    { phase_ = Phase::failed; out.unavailable = true; out.reason = "v4_projection_recovery_base_mismatch"; return out; }
                    if (nestedParent_)
                    {
                        if (p.familyCursor != nestedBefore_ || p.advanceBytes != nestedAdvance_)
                        { phase_ = Phase::failed; out.unavailable = true; out.reason = "v4_projection_recovery_nested_parent_mismatch"; return out; }
                    }
                    else
                    {
                        const auto family = ReadV4FamilyCursorShard(*store_, lookup_->Matched()->name, lookup_->Matched()->fileId,
                            binding_, p.coordinateBudget, p.axisBudget, limits_.bytes, limits_.familyReplayWork);
                        if (!family.cursorValidated || !family.family || family.family->coordinate != p.familyCoordinate || family.family->candidateCursor != p.familyCursor)
                        { out.resourceUnavailable = family.resourceUnavailable; out.unavailable = !out.resourceUnavailable; out.reason = "v4_projection_recovery_family_mismatch"; return out; }
                    }
                    const auto decoded = NextV4FamilyCoordinate(p.familyCoordinate, p.coordinateBudget);
                    if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family)
                    { out.resourceUnavailable = decoded.status == V4FamilyCoordinateStatus::resource_unavailable; out.unavailable = !out.resourceUnavailable; out.reason = "v4_projection_recovery_family_unavailable"; return out; }
                    const auto before = event_graph::V4FamilyCandidateGraph::ParseCursor(*decoded.family, p.axisBudget, p.familyCursor);
                    if (before.status != event_graph::V4FamilyTraversalStatus::ready || !before.errors.empty())
                    { out.resourceUnavailable = before.status == event_graph::V4FamilyTraversalStatus::resource_unavailable; out.unavailable = !out.resourceUnavailable; out.reason = "v4_projection_recovery_cursor_unavailable"; return out; }
                    const auto advance = event_graph::V4FamilyCandidateGraph::PrepareStagedNativeAdvance(*decoded.family, p.axisBudget, before.cursor, p.advanceWorkBudget);
                    if (advance.status != event_graph::V4FamilyTraversalStatus::ready || !advance.proposal || !advance.errors.empty())
                    { out.resourceUnavailable = advance.status == event_graph::V4FamilyTraversalStatus::resource_unavailable; out.unavailable = !out.resourceUnavailable; out.reason = "v4_projection_recovery_advance_unavailable"; return out; }
                    const auto& leaf = advance.proposal->Page();
                    const auto encoded = EncodeV4ProjectionAdvance(*decoded.family, p.axisBudget, leaf, limits_.bytes);
                    std::string bits; bits.reserve(leaf.pendingPresent.size()); for (bool bit : leaf.pendingPresent) bits += bit ? '1' : '0';
                    if (!encoded.encoded || encoded.bytes != p.advanceBytes || leaf.candidateId != p.candidateId ||
                        leaf.candidateOrdinal != p.candidateOrdinal || leaf.pendingOrdinal != p.leafOrdinal || bits != p.leafPresence)
                    { out.resourceUnavailable = encoded.resourceUnavailable; out.unavailable = !out.resourceUnavailable; out.reason = "v4_projection_recovery_advance_mismatch"; return out; }
                    const auto identity = V4FamilyIdentityContext::Create(*decoded.family, p.axisBudget);
                    const auto verified = V4FamilyValidatedContext::Create(*decoded.family, p.axisBudget, leaf.candidate);
                    if (identity.Status() != V4FamilyStatus::schema_only || verified.Status() != V4CoordinateStatus::coordinate_ready)
                    { out.resourceUnavailable = identity.Status() == V4FamilyStatus::resource_unavailable || verified.Status() == V4CoordinateStatus::resource_unavailable;
                      out.unavailable = !out.resourceUnavailable; out.reason = "v4_projection_recovery_candidate_unavailable"; return out; }
                    bool resource = false; std::vector<std::string> errors;
                    auto work = event_graph::V4NativeJoinedGraph::CreateProjectionWork(identity, verified, leaf.pendingPresent, p.preambleBudget, resource, errors);
                    if (!work || resource || !errors.empty()) { out.resourceUnavailable = resource; out.unavailable = !resource; out.reason = "v4_projection_recovery_work_unavailable"; return out; }
                    auto familyCopy = *decoded.family; auto leafCopy = leaf.after;
                    family_ = std::move(familyCopy); leafCursor_ = std::move(leafCopy);
                    work_ = std::move(work); phase_ = Phase::replay; ++out.workUsed; continue;
                }
                if (phase_ == Phase::replay)
                {
                    if (replayIndex_ == entries_.size()) { phase_ = Phase::verify; continue; }
                    const auto& first = entries_.back().audit; const auto& p = entries_[entries_.size() - 1 - replayIndex_].audit;
                    if (work_->Cursor().stage == event_graph::PresenceProjectionWorkStage::done || p.pageOrdinal != std::to_string(replayIndex_) ||
                        p.familyCoordinate != first.familyCoordinate || p.familyCursor != first.familyCursor || p.candidateId != first.candidateId ||
                        p.candidateOrdinal != first.candidateOrdinal || p.leafOrdinal != first.leafOrdinal || p.leafPresence != first.leafPresence ||
                        p.baseHeadName != first.baseHeadName || p.baseHeadVolume != first.baseHeadVolume || p.baseHeadFileId != first.baseHeadFileId ||
                        p.advanceBytes != first.advanceBytes || p.advanceWorkBudget != first.advanceWorkBudget || p.preambleBudget != first.preambleBudget ||
                        BudgetValues(p.coordinateBudget,p.axisBudget) != BudgetValues(first.coordinateBudget,first.axisBudget))
                    { phase_ = Phase::failed; out.unavailable = true; out.reason = "v4_projection_recovery_page_identity_mismatch"; return out; }
                    if (p.workBudget > limits_.totalPageWork - pageWork_ || p.outputLimit > limits_.outputPerPage)
                    { out.resourceUnavailable = true; out.reason = "v4_projection_recovery_work_budget"; return out; }
                    bool resource = false; auto proposal = work_->PrepareStep(p.workBudget, p.outputLimit, resource);
                    if (!proposal || resource) { out.resourceUnavailable = resource; out.unavailable = !resource; out.reason = "v4_projection_recovery_page_prepare"; return out; }
                    const auto encoded = EncodeV4ProjectionPage(proposal->Page(), limits_.bytes);
                    const auto before = event_graph::EncodePresenceProjectionCursor(proposal->Page().before);
                    const auto after = event_graph::EncodePresenceProjectionCursor(proposal->Page().after);
                    if (!encoded.encoded || encoded.bytes != p.pageBytes || !before.cursor || !after.cursor ||
                        (before.encoded == after.encoded && !proposal->Page().workUsed))
                    { out.resourceUnavailable = encoded.resourceUnavailable || before.resourceUnavailable || after.resourceUnavailable;
                      out.unavailable = !out.resourceUnavailable; out.reason = "v4_projection_recovery_page_mismatch"; return out; }
                    if (!work_->Commit(*proposal, resource)) { out.resourceUnavailable = resource; out.unavailable = !resource; out.reason = "v4_projection_recovery_commit_unavailable"; return out; }
                    pageWork_ += proposal->Page().workUsed; ++replayIndex_; ++out.workUsed; continue;
                }
                if (phase_ == Phase::verify)
                {
                    if (verifyIndex_ < entries_.size() * 2)
                    {
                        const auto& entry = entries_[verifyIndex_ / 2]; const bool node = verifyIndex_ % 2 == 0;
                        const auto& locator = node ? entry.node : entry.page; const auto& expected = node ? entry.nodeBytes : entry.pageBytes;
                        const auto read = store_->Read(locator.name, locator.fileId, binding_, limits_.bytes);
                        if (read.status != CodecStatus::decoded || read.recoveredInFlight || read.bytes != expected)
                        { out.resourceUnavailable = read.status == CodecStatus::resource_unavailable; out.unavailable = !out.resourceUnavailable; out.reason = "v4_projection_recovery_snapshot_changed"; return out; }
                        ++verifyIndex_; ++out.workUsed; continue;
                    }
                    if (!finalSerialFrozen_)
                    {
                        const auto base = store_->Read(base_.name, base_.fileId, binding_, limits_.bytes);
                        if (base.status != CodecStatus::decoded || base.recoveredInFlight || base.bytes != baseBytes_)
                        { out.resourceUnavailable = base.status == CodecStatus::resource_unavailable; out.unavailable = !out.resourceUnavailable; out.reason = "v4_projection_recovery_base_changed"; return out; }
                        verifySerial_ = lookup_->recheckSerial_; finalSerialFrozen_ = true; ++out.workUsed;
                        out.reason = "v4_projection_recovery_postcheck_pending"; return out;
                    }
                    if (!postcheck || !postcheck->Matches(*lookup_) || postcheck->Serial() <= verifySerial_ || postcheck->Serial() <= initialSerial_)
                    { out.reason = "v4_projection_recovery_postcheck_pending"; return out; }
                    readySerial_ = postcheck->Serial(); phase_ = Phase::ready; ++out.workUsed; continue;
                }
                if (phase_ == Phase::ready)
                { out.complete = true; out.projectionComplete = work_ && work_->CanAuthorizeActiveGraph(); out.reason = "v4_projection_recovery_preview_not_execution_authority"; return out; }
            }
            out.reason = "v4_projection_recovery_step_pending"; return out;
        }
        catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_projection_recovery_resource_unavailable"; return out; }
        catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_projection_recovery_resource_unavailable"; return out; }
        catch (...) { phase_ = Phase::failed; out.unavailable = true; out.reason = "v4_projection_recovery_unavailable"; return out; }
    }
};

class V4NativeStagedFamilyPending;
struct V4NativeStagedFamilyPublication
{
    bool staged = false, resourceUnavailable = false;
    std::array<PublishedCheckpoint, 2> publications;
    std::unique_ptr<V4NativeStagedFamilyPending> pending;
    std::string_view reason;
};
class V4NativeStagedFamilyReceipt final
{
    friend class CheckpointStore;
    friend class V4NativeStagedFamilyPending;
    friend class V4NativeStagedFamilyRecovery;
    friend class V4NativeReceiptAncestryRecheck;
    friend class V4NativeProjectionLiveJournal;
    friend class V4NativeProjectionJournalRecovery;
    friend class V4NativeCaseIndexPending;
    std::shared_ptr<V4NativeProjectionJournalRecovery> recovery_;
    std::weak_ptr<V4NativeStagedFamilyReceipt> self_;
    std::shared_ptr<const V4NativeStagedFamilyReceipt> prior_;
    V4ShardLocator page_, head_;
    std::unique_ptr<V4CheckpointReadPin> pagePin_, headPin_;
    std::string pageBytes_, headBytes_;
    std::size_t pageBudget_ = 0, headBudget_ = 0, depth_ = 0, retainedBytes_ = 0;
    event_graph::V4FamilyCandidateCursor cursor_;
    std::optional<event_graph::V4FamilyCandidateRecord> record_;
    std::vector<event_graph::CandidatePrefixPrune> candidatePruned_;
    std::vector<event_graph::V4FamilyPresencePrune> presencePruned_;
    std::vector<event_graph::V4FamilyActivePrune> activePruned_;
    bool projectionRequired_ = false;
    std::vector<bool> pendingPresent_;
    std::string pendingOrdinal_;
    event_graph::V4FamilyCandidateCursor pendingBeforeCursor_;
    std::string pendingAdvanceBytes_;
    std::size_t pendingAdvanceWorkBudget_ = 0;
    std::uint64_t postcheckSerial_ = 0;
    mutable std::atomic_bool stagingBlocked_{false};
    V4NativeStagedFamilyReceipt() = default;
    // Counts receipt and projection pinned files and saved checkpoint bytes;
    // lookup/lease pins, allocator overhead and decoded audits are excluded.
    // This is not a wall-clock bound.
    // Counts retained staged pages and each distinct projection's base/pages.
    // A recovery can occur more than once in the chain, so compare ownership
    // rather than object addresses and charge its resources only once.
    bool RetainedResources(std::size_t& pins, std::size_t& bytes, bool& resourceUnavailable) const
    {
        pins = 0; bytes = 0; resourceUnavailable = false;
        const auto maximum = (std::numeric_limits<std::size_t>::max)();
        std::vector<std::shared_ptr<V4NativeProjectionJournalRecovery>> seen;
        std::size_t visited = 0;
        for (auto current = this; current; current = current->prior_.get())
        {
            if (!current->pagePin_ || !current->headPin_ || !current->recovery_) return false;
            if (visited == maximum || pins > maximum - 2 || current->pageBytes_.size() > maximum - bytes ||
                current->headBytes_.size() > maximum - bytes - current->pageBytes_.size())
            { resourceUnavailable = true; return false; }
            ++visited; pins += 2; bytes += current->pageBytes_.size() + current->headBytes_.size();
            const auto& projection = current->recovery_;
            const bool known = std::any_of(seen.begin(), seen.end(), [&](const auto& prior) {
                return !prior.owner_before(projection) && !projection.owner_before(prior);
            });
            if (known) continue;
            if (!projection->basePin_) return false;
            if (projection->entries_.size() > (maximum - 1) / 2) { resourceUnavailable = true; return false; }
            for (const auto& entry : projection->entries_)
                if (!entry.nodePin || !entry.pagePin) return false;
            const auto projectionPins = 1 + projection->entries_.size() * 2;
            if (projectionPins > maximum - pins || projection->retainedBytes_ > maximum - bytes)
            { resourceUnavailable = true; return false; }
            seen.push_back(projection); pins += projectionPins; bytes += projection->retainedBytes_;
        }
        return visited == depth_;
    }
public:
    V4NativeStagedFamilyReceipt(const V4NativeStagedFamilyReceipt&) = delete;
    V4NativeStagedFamilyReceipt& operator=(const V4NativeStagedFamilyReceipt&) = delete;
    const V4ShardLocator& Head() const noexcept { return head_; }
    const std::optional<event_graph::V4FamilyCandidateRecord>& UnexecutedRecord() const noexcept { return record_; }
    const std::vector<event_graph::CandidatePrefixPrune>& CandidatePrunes() const noexcept { return candidatePruned_; }
    const std::vector<event_graph::V4FamilyPresencePrune>& PresencePrunes() const noexcept { return presencePruned_; }
    const std::vector<event_graph::V4FamilyActivePrune>& ActivePrunes() const noexcept { return activePruned_; }
    bool ProjectionPending() const noexcept { return projectionRequired_; }
    const V4NativeStagedFamilyReceipt* Prior() const noexcept { return prior_.get(); }
};
class V4NativeReceiptAncestryProof final
{
    friend class V4NativeReceiptAncestryRecheck;
    friend class V4NativeProjectionLiveJournal;
    friend class V4NativeProjectionJournalRecovery;
    friend class CheckpointStore;
    friend class V4NativeCaseIndexPending;
    std::shared_ptr<const V4NativeStagedFamilyReceipt> receipt_;
    std::shared_ptr<V4ForegroundReservation> reservation_;
    std::weak_ptr<V4NativeProjectionJournalRecovery> nestedRecovery_;
    std::uint64_t postcheckSerial_ = 0;
    mutable std::atomic_bool used_{false};
    V4NativeReceiptAncestryProof() = default;
public:
    V4NativeReceiptAncestryProof(const V4NativeReceiptAncestryProof&) = delete;
    V4NativeReceiptAncestryProof& operator=(const V4NativeReceiptAncestryProof&) = delete;
};
// Re-read every stage and every distinct nested projection while retaining all
// receipt-owned pins. fileBudget counts ancestor entries only: each Step also
// synchronously rechecks the reservation's marker/head, even at budget zero.
// Neither fileBudget nor the preamble establishes a wall-clock bound.
class V4NativeReceiptAncestryRecheck final
{
    std::shared_ptr<const V4NativeStagedFamilyReceipt> first_, current_;
    std::shared_ptr<V4ForegroundReservation> reservation_;
    std::shared_ptr<V4NativeProjectionJournalRecovery> nestedRecovery_;
    V4ShardLocator expectedHead_;
    std::shared_ptr<V4NativeProjectionJournalRecovery> lastProjection_;
    std::size_t phase_ = 0, projectionIndex_ = 0;
    std::uint64_t serial_ = 0;
    bool frozen_ = false, done_ = false, failed_ = false;
    V4NativeReceiptAncestryRecheck() = default;
public:
    V4NativeReceiptAncestryRecheck(const V4NativeReceiptAncestryRecheck&) = delete;
    V4NativeReceiptAncestryRecheck& operator=(const V4NativeReceiptAncestryRecheck&) = delete;
    static std::unique_ptr<V4NativeReceiptAncestryRecheck> Begin(
        const std::shared_ptr<V4NativeStagedFamilyReceipt>& receipt,
        const std::shared_ptr<V4ForegroundReservation>& reservation, bool& resourceUnavailable)
    {
        resourceUnavailable = false;
        try
        {
            const auto identity = receipt ? receipt->self_.lock() : std::shared_ptr<V4NativeStagedFamilyReceipt>{};
            if (!receipt || receipt->stagingBlocked_ || !identity || identity.owner_before(receipt) || receipt.owner_before(identity) ||
                !V4ForegroundReservation::Canonical(reservation) || !receipt->recovery_ ||
                !receipt->recovery_->store_ || GetCurrentThreadId() != receipt->recovery_->ownerThread_ ||
                !reservation->Matches(*receipt->recovery_->store_, receipt->head_, reservation->headBudget_, &resourceUnavailable) ||
                !receipt->pagePin_ || !receipt->headPin_) return {};
            auto out = std::unique_ptr<V4NativeReceiptAncestryRecheck>(new V4NativeReceiptAncestryRecheck);
            out->first_ = receipt; out->current_ = receipt; out->reservation_ = reservation; out->expectedHead_ = receipt->head_; return out;
        }
        catch (const std::bad_alloc&) { resourceUnavailable = true; return {}; }
        catch (const std::length_error&) { resourceUnavailable = true; return {}; }
        catch (...) { return {}; }
    }
    // Only a private recovery which has scanned the complete child index
    // chain back to its retained parent can ask for a descendant-bound check.
    static std::unique_ptr<V4NativeReceiptAncestryRecheck> BeginNested(
        const std::shared_ptr<V4NativeProjectionJournalRecovery>& recovery, bool& resourceUnavailable)
    {
        resourceUnavailable = false;
        try
        {
            const auto identity = recovery ? recovery->self_.lock() : std::shared_ptr<V4NativeProjectionJournalRecovery>{};
            if (!recovery || !identity || identity.owner_before(recovery) || recovery.owner_before(identity) ||
                recovery->phase_ != V4NativeProjectionJournalRecovery::Phase::ancestry || !recovery->nestedParent_ ||
                !recovery->nestedParent_->stagingBlocked_ || !recovery->store_ || !recovery->lookup_ ||
                !recovery->basePin_ || recovery->entries_.empty() || !recovery->cursor_.next ||
                !V4NativeProjectionJournalRecovery::Same(*recovery->cursor_.next, recovery->base_) ||
                !V4NativeProjectionJournalRecovery::Same(recovery->base_, recovery->nestedParent_->head_) ||
                !V4ForegroundReservation::Canonical(recovery->nestedReservation_) || !recovery->nestedActivity_ ||
                !recovery->nestedReservation_->operationFence_ ||
                recovery->nestedActivity_.owner_before(recovery->nestedReservation_->operationFence_) ||
                recovery->nestedReservation_->operationFence_.owner_before(recovery->nestedActivity_) || !recovery->nestedActivity_->load() ||
                !recovery->nestedReservation_->Matches(*recovery->store_, recovery->final_,
                    recovery->nestedReservation_->headBudget_, &resourceUnavailable) ||
                GetCurrentThreadId() != recovery->ownerThread_) return {};
            auto out = std::unique_ptr<V4NativeReceiptAncestryRecheck>(new V4NativeReceiptAncestryRecheck);
            out->first_ = recovery->nestedParent_; out->current_ = recovery->nestedParent_;
            out->reservation_ = recovery->nestedReservation_; out->expectedHead_ = recovery->final_;
            out->nestedRecovery_ = recovery; return out;
        }
        catch (const std::bad_alloc&) { resourceUnavailable = true; return {}; }
        catch (const std::length_error&) { resourceUnavailable = true; return {}; }
        catch (...) { return {}; }
    }
    V4NativeProjectionRecoveryStep Step(std::size_t fileBudget) noexcept
    {
        V4NativeProjectionRecoveryStep out;
        try
        {
            if (failed_ || done_ || !first_ || !first_->recovery_ || !first_->recovery_->store_ ||
                !V4ForegroundReservation::Canonical(reservation_) ||
                GetCurrentThreadId() != first_->recovery_->ownerThread_)
            { out.unavailable = true; out.reason = "v4_receipt_ancestry_unavailable"; return out; }
            auto& store = *first_->recovery_->store_;
            if (!reservation_->Matches(store, expectedHead_, reservation_->headBudget_, &out.resourceUnavailable))
            { out.unavailable = !out.resourceUnavailable; out.reason = "v4_receipt_ancestry_head_unavailable"; return out; }
            while (out.workUsed < fileBudget && current_)
            {
                const auto recovery = current_->recovery_;
                if (!recovery || recovery->store_.get() != &store || recovery->binding_ != first_->recovery_->binding_ ||
                    !recovery->lookup_ || recovery->lookup_.get() != first_->recovery_->lookup_.get() ||
                    !current_->pagePin_ || !current_->headPin_ || !recovery->basePin_)
                { failed_ = true; out.unavailable = true; out.reason = "v4_receipt_ancestry_owner_mismatch"; return out; }
                const V4ShardLocator* locator = nullptr; const std::string* expected = nullptr; std::size_t limit = 0;
                if (phase_ == 0) { locator = &current_->page_; expected = &current_->pageBytes_; limit = current_->pageBudget_; }
                else if (phase_ == 1) { locator = &current_->head_; expected = &current_->headBytes_; limit = current_->headBudget_; }
                else
                {
                    const auto sameProjection = lastProjection_ && !lastProjection_.owner_before(recovery) && !recovery.owner_before(lastProjection_);
                    if (sameProjection) { current_ = current_->prior_; phase_ = 0; projectionIndex_ = 0; continue; }
                    if (recovery->entries_.size() > ((std::numeric_limits<std::size_t>::max)() - 1) / 2)
                    { out.resourceUnavailable = true; out.reason = "v4_receipt_ancestry_count_unavailable"; return out; }
                    const auto count = recovery->entries_.size() * 2;
                    if (projectionIndex_ < count)
                    {
                        const auto& entry = recovery->entries_[projectionIndex_ / 2]; const bool node = projectionIndex_ % 2 == 0;
                        if (!entry.nodePin || !entry.pagePin)
                        { failed_ = true; out.unavailable = true; out.reason = "v4_receipt_ancestry_projection_pin_unavailable"; return out; }
                        locator = node ? &entry.node : &entry.page; expected = node ? &entry.nodeBytes : &entry.pageBytes; limit = recovery->limits_.bytes;
                    }
                    else if (projectionIndex_ == count)
                    { locator = &recovery->base_; expected = &recovery->baseBytes_; limit = recovery->limits_.bytes; }
                    else { lastProjection_ = recovery; current_ = current_->prior_; phase_ = 0; projectionIndex_ = 0; continue; }
                }
                const auto read = store.Read(locator->name, locator->fileId, recovery->binding_, limit);
                if (read.status != CodecStatus::decoded || read.recoveredInFlight || read.bytes != *expected)
                { out.resourceUnavailable = read.status == CodecStatus::resource_unavailable; out.unavailable = !out.resourceUnavailable;
                  if (out.unavailable) failed_ = true; out.reason = "v4_receipt_ancestry_snapshot_changed"; return out; }
                if (phase_ < 2) ++phase_; else ++projectionIndex_;
                ++out.workUsed;
            }
            if (!current_ && !frozen_)
            { serial_ = first_->recovery_->lookup_->recheckSerial_; frozen_ = true; }
            out.complete = frozen_; out.reason = frozen_ ? "v4_receipt_ancestry_postcheck_pending" : "v4_receipt_ancestry_recheck_pending";
            return out;
        }
        catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_receipt_ancestry_resource_unavailable"; return out; }
        catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_receipt_ancestry_resource_unavailable"; return out; }
        catch (...) { failed_ = true; out.unavailable = true; out.reason = "v4_receipt_ancestry_unavailable"; return out; }
    }
    std::unique_ptr<V4NativeReceiptAncestryProof> Take(const V4ShardFamilyRecheck& postcheck, bool& resourceUnavailable) noexcept
    {
        resourceUnavailable = false;
        try
        {
            if (!frozen_ || failed_ || done_ || !first_ || (first_->stagingBlocked_ && !nestedRecovery_) || !first_->recovery_ || !first_->recovery_->lookup_ ||
                !postcheck.Matches(*first_->recovery_->lookup_) || postcheck.Serial() <= serial_ ||
                !V4ForegroundReservation::Canonical(reservation_) ||
                !reservation_->Matches(*first_->recovery_->store_, expectedHead_, reservation_->headBudget_, &resourceUnavailable)) return {};
            auto proof = std::unique_ptr<V4NativeReceiptAncestryProof>(new V4NativeReceiptAncestryProof);
            proof->receipt_ = first_; proof->reservation_ = reservation_; proof->nestedRecovery_ = nestedRecovery_;
            proof->postcheckSerial_ = postcheck.Serial(); done_ = true; return proof;
        }
        catch (const std::bad_alloc&) { resourceUnavailable = true; return {}; }
        catch (const std::length_error&) { resourceUnavailable = true; return {}; }
        catch (...) { return {}; }
    }
};
// One graph value from an already replayed receipt chain. This capability is
// process-local and does not publish an index or establish execution.
inline V4StructuredCaseReplay CheckpointStore::ReplayV4NativeStagedCasePreview(
    const std::shared_ptr<V4NativeStagedFamilyReceipt>& latest, const V4NativeReceiptAncestryProof& proof,
    std::size_t priorSteps, std::size_t ancestorBudget, const std::vector<std::uint32_t>& familyPrefix,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
    std::size_t tokenBudget, std::size_t caseByteBudget) noexcept
{
    V4StructuredCaseReplay out; out.status = V4StructuredCaseStatus::unavailable;
    try
    {
        if (priorSteps > ancestorBudget)
        { out.status = V4StructuredCaseStatus::resource_unavailable; out.reason = "v4_staged_case_ancestor_budget"; return out; }
        const auto owner = latest ? latest->self_.lock() : std::shared_ptr<V4NativeStagedFamilyReceipt>{};
        const auto& reservation = proof.reservation_;
        bool resource = false;
        if (!latest || !owner || owner.owner_before(latest) || latest.owner_before(owner) || latest->stagingBlocked_ ||
            proof.receipt_.owner_before(latest) || latest.owner_before(proof.receipt_) || !proof.nestedRecovery_.expired() ||
            !V4ForegroundReservation::Canonical(reservation) || !latest->recovery_ || latest->recovery_->store_.get() != this ||
            !latest->recovery_->lookup_ || GetCurrentThreadId() != latest->recovery_->ownerThread_ ||
            proof.postcheckSerial_ <= latest->postcheckSerial_ ||
            !reservation->Matches(*this, latest->head_, reservation->headBudget_, &resource))
        { out.status = resource ? V4StructuredCaseStatus::resource_unavailable : V4StructuredCaseStatus::unavailable;
          out.reason = "v4_staged_case_receipt_unavailable"; return out; }
        auto target = std::shared_ptr<const V4NativeStagedFamilyReceipt>(latest);
        for (std::size_t i = 0; i < priorSteps && target; ++i) target = target->prior_;
        if (!target || !target->pagePin_ || !target->headPin_ || !target->recovery_ ||
            target->recovery_->store_.get() != this || !target->recovery_->work_ ||
            !target->recovery_->work_->CanAuthorizeActiveGraph() || target->recovery_->entries_.empty() || !target->record_ ||
            target->record_->kind != "graph_hypothesis" || !target->record_->coordinate || target->record_->graph || !target->record_->nativeGraph)
        { out.reason = "v4_staged_case_record_unavailable"; return out; }
        const auto parsed = InspectV4StructuredPrefix(familyPrefix, tokenBudget);
        if (parsed.status != V4StructuredPrefixStatus::complete || !parsed.family)
        { out.status = parsed.status == V4StructuredPrefixStatus::resource_unavailable ? V4StructuredCaseStatus::resource_unavailable : V4StructuredCaseStatus::unavailable;
          out.reason = "v4_staged_case_prefix_unavailable"; return out; }
        const auto identity = event_graph::V4FamilyIdentityContext::Create(*parsed.family, axisBudget);
        const auto& origin = target->recovery_->entries_.back().audit;
        const auto coordinate = NextV4FamilyCoordinate(origin.familyCoordinate, coordinateBudget);
        if (coordinate.status != V4FamilyCoordinateStatus::ready || !coordinate.family || identity.Status() != V4FamilyStatus::schema_only ||
            identity.FamilyBinding() != V4FamilyBinding(*coordinate.family) ||
            identity.FamilyBinding() != V4FamilyBinding(target->recovery_->family_))
        { out.status = coordinate.status == V4FamilyCoordinateStatus::resource_unavailable || identity.Status() == V4FamilyStatus::resource_unavailable ?
                V4StructuredCaseStatus::resource_unavailable : V4StructuredCaseStatus::unavailable;
          out.reason = "v4_staged_case_family_unavailable"; return out; }
        const auto verified = V4FamilyValidatedContext::Create(*parsed.family, axisBudget, *target->record_->coordinate);
        if (verified.Status() != V4CoordinateStatus::coordinate_ready)
        { out.status = verified.Status() == V4CoordinateStatus::resource_unavailable ? V4StructuredCaseStatus::resource_unavailable : V4StructuredCaseStatus::unavailable;
          out.reason = "v4_staged_case_candidate_unavailable"; return out; }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id != target->record_->candidateId)
        { out.status = raw.resourceUnavailable ? V4StructuredCaseStatus::resource_unavailable : V4StructuredCaseStatus::unavailable;
          out.reason = "v4_staged_case_candidate_mismatch"; return out; }
        const auto& item = *target->record_->nativeGraph;
        const auto graph = event_graph::V4FamilyCandidateGraph::ReplayStagedNativeGraph(identity, verified, item.present,
            *target->recovery_->work_, item.graph.id);
        if (!graph.identityValid || !graph.errors.empty() || graph.variant.id != item.graph.id)
        { out.status = graph.status == event_graph::V4GraphReplayStatus::resource_unavailable ? V4StructuredCaseStatus::resource_unavailable : V4StructuredCaseStatus::unavailable;
          out.reason = "v4_staged_case_graph_unavailable"; return out; }
        if (item.present.size() > caseByteBudget)
        { out.status = V4StructuredCaseStatus::resource_unavailable; out.reason = "v4_staged_case_byte_budget"; return out; }
        std::string bits; bits.reserve(item.present.size()); for (bool bit : item.present) bits += bit ? '1' : '0';
        std::string id;
        const auto append = [&](std::string_view value) {
            const auto length = std::to_string(value.size());
            if (id.size() > caseByteBudget || length.size() + 1 > caseByteBudget - id.size() ||
                value.size() > caseByteBudget - id.size() - length.size() - 1) return false;
            event_graph::AppendToken(id, value); return true;
        };
        if (!append("v4_structured_case_v1") || !append(V4StructuredPrefixAlgorithm) || !append(std::to_string(familyPrefix.size())))
        { out.status = V4StructuredCaseStatus::resource_unavailable; out.reason = "v4_staged_case_byte_budget"; return out; }
        for (auto token : familyPrefix) if (!append(std::to_string(token)))
        { out.status = V4StructuredCaseStatus::resource_unavailable; out.reason = "v4_staged_case_byte_budget"; return out; }
        if (!append(identity.FamilyBinding()) || !append(raw.id) || !append("native") || !append(bits) || !append(graph.variant.id))
        { out.status = V4StructuredCaseStatus::resource_unavailable; out.reason = "v4_staged_case_byte_budget"; return out; }
        const auto page = Read(target->page_.name, target->page_.fileId, target->recovery_->binding_, target->pageBudget_);
        const auto head = Read(latest->head_.name, latest->head_.fileId, latest->recovery_->binding_, latest->headBudget_);
        if (page.status != CodecStatus::decoded || head.status != CodecStatus::decoded || page.recoveredInFlight || head.recoveredInFlight ||
            page.bytes != target->pageBytes_ || head.bytes != latest->headBytes_ ||
            !reservation->Matches(*this, latest->head_, reservation->headBudget_, &resource))
        { out.status = resource || page.status == CodecStatus::resource_unavailable || head.status == CodecStatus::resource_unavailable ?
                V4StructuredCaseStatus::resource_unavailable : V4StructuredCaseStatus::unavailable;
          out.reason = "v4_staged_case_snapshot_unavailable"; return out; }
        out.caseId = std::move(id); out.familyBinding = identity.FamilyBinding(); out.familyCoordinate = origin.familyCoordinate;
        out.candidateId = raw.id; out.graphMode = "native"; out.graphId = graph.variant.id;
        out.family = *parsed.family; out.candidate = verified.Coordinate(); out.present = item.present; out.nativeGraph = graph.variant;
        out.unresolved = graph.variant.unresolved;
        out.unresolved.emplace_back("v4_staged_case_process_local_non_atomic_preview_not_execution_authority");
        bool unused = false;
        if (!proof.used_.compare_exchange_strong(unused, true)) { out = {}; out.status = V4StructuredCaseStatus::unavailable; out.reason = "v4_staged_case_proof_consumed"; return out; }
        out.status = V4StructuredCaseStatus::unexecuted; return out;
    }
    catch (const std::bad_alloc&) { out = {}; out.status = V4StructuredCaseStatus::resource_unavailable; out.reason = "v4_staged_case_resource_unavailable"; return out; }
    catch (const std::length_error&) { out = {}; out.status = V4StructuredCaseStatus::resource_unavailable; out.reason = "v4_staged_case_resource_unavailable"; return out; }
    catch (...) { out = {}; out.status = V4StructuredCaseStatus::unavailable; out.reason = "v4_staged_case_unavailable"; return out; }
}

class V4NativeCaseIndexPending final
{
    friend class CheckpointStore;
    std::shared_ptr<V4NativeStagedFamilyReceipt> latest_;
    std::shared_ptr<V4ForegroundReservation> reservation_;
    std::shared_ptr<std::atomic_bool> fence_;
    V4ShardLocator page_, head_;
    std::unique_ptr<V4CheckpointReadPin> pagePin_, headPin_;
    std::string pageBytes_, headBytes_;
    std::size_t pageBudget_ = 0, headBudget_ = 0;
    std::uint64_t publishSerial_ = 0;
    bool consumed_ = false;
    V4NativeCaseIndexPending() = default;
public:
    V4NativeCaseIndexPending(const V4NativeCaseIndexPending&) = delete;
    V4NativeCaseIndexPending& operator=(const V4NativeCaseIndexPending&) = delete;
    ~V4NativeCaseIndexPending()
    {
        if (!consumed_ && reservation_ && reservation_->attempted_) reservation_->FailAttempt();
        if (fence_) fence_->store(false);
    }
    const V4ShardLocator& ProposedHead() const noexcept { return head_; }
    // The caller first rechecks every ancestor and obtains a fresh proof.
    // This adopts only the index head, not a production result.
    bool Finalize(const V4NativeReceiptAncestryProof& proof, bool& resourceUnavailable) noexcept
    {
        resourceUnavailable = false;
        try
        {
            if (consumed_ || !latest_ || latest_->stagingBlocked_ || !latest_->recovery_ || !latest_->recovery_->store_ ||
                !V4ForegroundReservation::Canonical(reservation_) || !fence_ || !reservation_->operationFence_ ||
                fence_.owner_before(reservation_->operationFence_) || reservation_->operationFence_.owner_before(fence_) || !fence_->load() ||
                proof.receipt_.owner_before(latest_) || latest_.owner_before(proof.receipt_) ||
                proof.reservation_.owner_before(reservation_) || reservation_.owner_before(proof.reservation_) ||
                !proof.nestedRecovery_.expired() || proof.postcheckSerial_ <= publishSerial_ || !pagePin_ || !headPin_ ||
                GetCurrentThreadId() != latest_->recovery_->ownerThread_ ||
                !reservation_->Matches(*latest_->recovery_->store_, latest_->head_, reservation_->headBudget_, &resourceUnavailable)) return false;
            auto& store = *latest_->recovery_->store_;
            const auto page = store.Read(page_.name, page_.fileId, latest_->recovery_->binding_, pageBudget_);
            const auto head = store.Read(head_.name, head_.fileId, latest_->recovery_->binding_, headBudget_);
            if (page.status != CodecStatus::decoded || head.status != CodecStatus::decoded || page.recoveredInFlight || head.recoveredInFlight ||
                page.bytes != pageBytes_ || head.bytes != headBytes_)
            { resourceUnavailable = page.status == CodecStatus::resource_unavailable || head.status == CodecStatus::resource_unavailable; return false; }
            bool unused = false; if (!proof.used_.compare_exchange_strong(unused, true)) return false;
            if (!reservation_->Commit(head_, reservation_->headBudget_, headBudget_, resourceUnavailable)) return false;
            reservation_->Finish(); consumed_ = true; fence_->store(false); fence_.reset(); return true;
        }
        catch (const std::bad_alloc&) { resourceUnavailable = true; return false; }
        catch (const std::length_error&) { resourceUnavailable = true; return false; }
        catch (...) { return false; }
    }
};
struct V4NativeCaseIndexPublication
{
    bool staged = false, resourceUnavailable = false;
    std::array<PublishedCheckpoint, 2> publications;
    std::unique_ptr<V4NativeCaseIndexPending> pending;
    std::string_view reason;
};

inline V4NativeCaseIndexPublication CheckpointStore::PublishV4NativeCaseIndexPreview(
    const std::shared_ptr<V4NativeStagedFamilyReceipt>& latest, const V4NativeReceiptAncestryProof& proof,
    std::size_t priorSteps, std::size_t ancestorBudget, const std::vector<std::uint32_t>& familyPrefix,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
    std::size_t tokenBudget, std::size_t caseByteBudget, std::size_t shardByteBudget,
    std::size_t indexByteBudget, std::size_t pinBudget, std::size_t retainedByteBudget) noexcept
{
    V4NativeCaseIndexPublication out;
    const auto attempted = [&] { return out.publications[0].temporaryCreateAttempted || out.publications[1].temporaryCreateAttempted ||
        out.publications[0].temporaryCreated || out.publications[1].temporaryCreated || out.publications[0].renameAttempted || out.publications[1].renameAttempted; };
    try
    {
        const auto reservation = proof.reservation_;
        if (!V4ForegroundReservation::Canonical(reservation) || !reservation->operationFence_) { out.reason = "v4_native_case_index_reservation_unavailable"; return out; }
        auto fence = reservation->operationFence_; bool idle = false;
        if (!fence->compare_exchange_strong(idle, true)) { out.reason = "v4_native_case_index_operation_in_progress"; return out; }
        bool keep = false;
        auto release = [&](void*) noexcept { if (!keep) { if (attempted()) reservation->FailAttempt(); fence->store(false); } };
        std::unique_ptr<void, decltype(release)> guard(&keep, release);
        if (!latest) { out.reason = "v4_native_case_index_receipt_unavailable"; return out; }
        std::size_t pins = 0, bytes = 0; bool resource = false;
        if (!latest->RetainedResources(pins, bytes, resource))
        { out.resourceUnavailable = resource; out.reason = "v4_native_case_index_retention_unavailable"; return out; }
        if (pinBudget < 2 || pins > pinBudget - 2 || bytes > retainedByteBudget ||
            shardByteBudget > retainedByteBudget - bytes || indexByteBudget > retainedByteBudget - bytes - shardByteBudget)
        { out.resourceUnavailable = true; out.reason = "v4_native_case_index_retention_budget"; return out; }
        const auto replay = ReplayV4NativeStagedCasePreview(latest, proof, priorSteps, ancestorBudget, familyPrefix,
            coordinateBudget, axisBudget, tokenBudget, caseByteBudget);
        if (replay.status != V4StructuredCaseStatus::unexecuted)
        { out.resourceUnavailable = replay.status == V4StructuredCaseStatus::resource_unavailable; out.reason = "v4_native_case_index_replay_unavailable"; return out; }
        auto target = std::shared_ptr<const V4NativeStagedFamilyReceipt>(latest);
        for (std::size_t i = 0; i < priorSteps && target; ++i) target = target->prior_;
        if (!target) { out.reason = "v4_native_case_index_target_unavailable"; return out; }
        V4NativeCaseIndexAudit audit;
        audit.parentName.assign(latest->head_.name.begin(), latest->head_.name.end());
        audit.parentVolume = std::to_string(latest->head_.fileId.VolumeSerialNumber); audit.parentFileId = V4ShardIdHex(latest->head_.fileId);
        audit.pageName.assign(target->page_.name.begin(), target->page_.name.end());
        audit.pageVolume = std::to_string(target->page_.fileId.VolumeSerialNumber); audit.pageFileId = V4ShardIdHex(target->page_.fileId);
        audit.caseId = replay.caseId; audit.priorSteps = priorSteps;
        State shard; shard.binding = latest->recovery_->binding_; shard.familyVersion = FamilyVersion::v4; shard.v4TypedEnabled = true;
        shard.nativeCaseIndex = std::move(audit);
        const auto expected = EncodeCheckpoint(shard, shard.binding, shardByteBudget);
        if (expected.status != CodecStatus::encoded)
        { out.resourceUnavailable = expected.status == CodecStatus::resource_unavailable; out.reason = "v4_native_case_index_encode_unavailable"; return out; }
        out.publications[0] = Publish(shard, shard.binding, shardByteBudget);
        if (attempted()) reservation->MarkAttempted();
        if (out.publications[0].status != StoreStatus::published_durability_unverified || !out.publications[0].fileIdentityCaptured ||
            !out.publications[0].renameReturnedSuccess)
        { out.reason = "v4_native_case_index_publish_unavailable"; return out; }
        V4ShardLocator page{out.publications[0].fileName, out.publications[0].fileId};
        const auto read = ReadV4NativeCaseIndexShard(*this, page.name, page.fileId, shard.binding, shardByteBudget);
        if (!read.read || read.bytes != expected.bytes)
        { out.resourceUnavailable = read.resourceUnavailable; out.reason = "v4_native_case_index_readback_unavailable"; return out; }
        auto index = AppendV4ShardIndex(*this, latest->head_, page, "native_staged_case", shard.binding,
            coordinateBudget, axisBudget, tokenBudget, shardByteBudget, shardByteBudget, 0, shardByteBudget, indexByteBudget);
        out.publications[1] = std::move(index.publication);
        if (!index.published || !index.head)
        { out.resourceUnavailable = index.resourceUnavailable; out.reason = "v4_native_case_index_head_unavailable"; return out; }
        auto pagePin = PinRead(page.name, page.fileId); auto headPin = PinRead(index.head->name, index.head->fileId);
        const auto head = Read(index.head->name, index.head->fileId, shard.binding, indexByteBudget);
        if (!pagePin || !headPin || head.status != CodecStatus::decoded || head.recoveredInFlight || !head.state.shardIndex)
        { out.resourceUnavailable = head.status == CodecStatus::resource_unavailable; out.reason = "v4_native_case_index_pin_unavailable"; return out; }
        const auto& link = *head.state.shardIndex;
        if (link.dataKind != "native_staged_case" || link.dataName != std::string(page.name.begin(), page.name.end()) ||
            link.dataVolume != std::to_string(page.fileId.VolumeSerialNumber) || link.dataFileId != V4ShardIdHex(page.fileId) ||
            link.previousName != std::string(latest->head_.name.begin(), latest->head_.name.end()) ||
            link.previousVolume != std::to_string(latest->head_.fileId.VolumeSerialNumber) ||
            link.previousFileId != V4ShardIdHex(latest->head_.fileId))
        { out.reason = "v4_native_case_index_link_mismatch"; return out; }
        auto pending = std::unique_ptr<V4NativeCaseIndexPending>(new V4NativeCaseIndexPending);
        pending->latest_ = latest; pending->reservation_ = reservation; pending->page_ = page; pending->head_ = *index.head;
        pending->pageBytes_ = read.bytes; pending->headBytes_ = head.bytes; pending->pageBudget_ = shardByteBudget; pending->headBudget_ = indexByteBudget;
        pending->pagePin_ = std::move(pagePin); pending->headPin_ = std::move(headPin);
        pending->publishSerial_ = latest->recovery_->lookup_->recheckSerial_; pending->fence_ = fence;
        out.pending = std::move(pending); out.staged = true; out.reason = "v4_native_case_index_postcheck_pending_preview_only";
        keep = true; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_native_case_index_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_native_case_index_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_native_case_index_unavailable"; return out; }
}

inline std::shared_ptr<V4NativeProjectionJournalRecovery> V4NativeProjectionJournalRecovery::BeginNested(
    const V4NativeProjectionLiveJournal& journal, const std::shared_ptr<V4ForegroundReservation>& reservation,
    const V4ShardFamilyRecheck& precheck, const V4NativeProjectionReplayLimits& limits,
    bool& resourceUnavailable) noexcept
{
    resourceUnavailable = false;
    try
    {
        const auto parent = journal.nextParent_;
        if (!parent || !journal.ownsParent_ || !parent->stagingBlocked_ || !parent->projectionRequired_ ||
            !journal.pageCount_ || journal.pendingProposal_ || journal.ownsActivity_ || journal.failed_ || !journal.store_ || !journal.lookup_ ||
            !parent->recovery_ || parent->recovery_->store_.get() != journal.store_.get() ||
            parent->recovery_->lookup_.get() != journal.lookup_.get() ||
            !V4ForegroundReservation::Canonical(reservation) || !journal.ownerThread_ ||
            GetCurrentThreadId() != journal.ownerThread_ || !Same(journal.baseHead_, parent->head_) ||
            Same(journal.baseHead_, journal.head_) || !precheck.Matches(*journal.lookup_) ||
            precheck.Serial() < journal.precheckSerial_ ||
            !reservation->Matches(*journal.store_, journal.head_, reservation->headBudget_, &resourceUnavailable) ||
            journal.seed_.advanceBytes != parent->pendingAdvanceBytes_ ||
            journal.seed_.advanceWorkBudget != parent->pendingAdvanceWorkBudget_ ||
            journal.seed_.leafOrdinal != parent->pendingOrdinal_) return {};
        std::size_t parentPins = 0, parentBytes = 0;
        if (!parent->RetainedResources(parentPins, parentBytes, resourceUnavailable)) return {};
        auto pin = journal.store_->PinRead(journal.baseHead_.name, journal.baseHead_.fileId);
        if (!pin) return {};
        const auto read = journal.store_->Read(journal.baseHead_.name, journal.baseHead_.fileId, journal.binding_, limits.bytes);
        if (read.status != CodecStatus::decoded || read.recoveredInFlight || !read.state.shardIndex)
        { resourceUnavailable = read.status == CodecStatus::resource_unavailable; return {}; }
        auto result = std::shared_ptr<V4NativeProjectionJournalRecovery>(new V4NativeProjectionJournalRecovery);
        result->self_ = result; result->store_ = journal.store_; result->lookup_ = journal.lookup_;
        result->nestedParent_ = parent; result->nestedReservation_ = reservation;
        result->nestedParentPins_ = parentPins; result->nestedParentBytes_ = parentBytes;
        result->nestedBefore_ = journal.seed_.familyCursor; result->nestedAdvance_ = journal.seed_.advanceBytes;
        result->base_ = journal.baseHead_; result->final_ = journal.head_;
        result->cursor_ = V4ShardIndexCursor{journal.head_, {}}; result->binding_ = journal.binding_; result->limits_ = limits;
        result->basePin_ = std::move(pin); result->baseBytes_ = read.bytes; result->retainedBytes_ = read.bytes.size();
        result->initialSerial_ = precheck.Serial(); result->ownerThread_ = journal.ownerThread_;
        auto activity = reservation->operationFence_; if (!activity) return {}; bool idle = false;
        if (!activity->compare_exchange_strong(idle, true)) return {};
        if (!reservation->Matches(*journal.store_, journal.head_, reservation->headBudget_, &resourceUnavailable))
        { activity->store(false); return {}; }
        result->nestedActivity_ = std::move(activity); return result;
    }
    catch (const std::bad_alloc&) { resourceUnavailable = true; return {}; }
    catch (const std::length_error&) { resourceUnavailable = true; return {}; }
    catch (...) { return {}; }
}

inline bool V4NativeProjectionJournalRecovery::AcceptNestedAncestry(
    const V4NativeReceiptAncestryProof& proof, bool& resourceUnavailable) noexcept
{
    resourceUnavailable = false;
    const auto identity = self_.lock(), certified = proof.nestedRecovery_.lock();
    if (!identity || !certified || identity.owner_before(certified) || certified.owner_before(identity) ||
        phase_ != Phase::ancestry || !nestedParent_ || !nestedParent_->stagingBlocked_ || !nestedReservation_ || !nestedActivity_ ||
        !nestedReservation_->operationFence_ || nestedActivity_.owner_before(nestedReservation_->operationFence_) ||
        nestedReservation_->operationFence_.owner_before(nestedActivity_) || !nestedActivity_->load() ||
        proof.receipt_.owner_before(nestedParent_) || nestedParent_.owner_before(proof.receipt_) ||
        proof.reservation_.owner_before(nestedReservation_) || nestedReservation_.owner_before(proof.reservation_) ||
        proof.postcheckSerial_ <= initialSerial_ || !store_ || GetCurrentThreadId() != ownerThread_ ||
        !nestedReservation_->Matches(*store_, final_, nestedReservation_->headBudget_, &resourceUnavailable)) return false;
    bool unused = false;
    if (!proof.used_.compare_exchange_strong(unused, true)) return false;
    phase_ = Phase::initialize; return true;
}

// Create the next independent Work from the authenticated, still-pending leaf.
// This is preparation only; durable ancestry and latest-head checks remain.
inline std::unique_ptr<V4NativeProjectionLiveJournal> V4NativeProjectionLiveJournal::BeginNext(
    const V4NativeReceiptAncestryProof& proof, const V4ShardFamilyRecheck& precheck,
    std::size_t preambleBudget, std::size_t tokenBudget, std::size_t replayBudget,
    std::size_t byteBudget, std::size_t indexByteBudget, bool& resourceUnavailable)
{
    resourceUnavailable = false;
    try
    {
        const auto& parent = proof.receipt_;
        const auto& reservation = proof.reservation_;
        const auto recovery = parent ? parent->recovery_ : std::shared_ptr<V4NativeProjectionJournalRecovery>{};
        if (!parent || parent->stagingBlocked_ || !recovery || !recovery->store_ || !recovery->lookup_ || !parent->projectionRequired_ ||
            parent->pendingOrdinal_.empty() || parent->pendingAdvanceBytes_.empty() ||
            !recovery->ownerThread_ || GetCurrentThreadId() != recovery->ownerThread_ ||
            !V4ForegroundReservation::Canonical(reservation) ||
            !reservation->Matches(*recovery->store_, parent->head_, reservation->headBudget_, &resourceUnavailable) ||
            !precheck.Matches(*recovery->lookup_) || precheck.Serial() < proof.postcheckSerial_ ||
            recovery->entries_.empty()) return {};
        const auto& origin = recovery->entries_.back().audit;
        const auto proposal = event_graph::V4FamilyCandidateGraph::PrepareStagedNativeAdvance(
            recovery->family_, origin.axisBudget, parent->pendingBeforeCursor_, parent->pendingAdvanceWorkBudget_);
        if (proposal.status != event_graph::V4FamilyTraversalStatus::ready || !proposal.proposal || !proposal.errors.empty())
        { resourceUnavailable = proposal.status == event_graph::V4FamilyTraversalStatus::resource_unavailable; return {}; }
        const auto& page = proposal.proposal->Page();
        const auto advance = EncodeV4ProjectionAdvance(recovery->family_, origin.axisBudget, page, byteBudget);
        std::string bits; bits.reserve(page.pendingPresent.size()); for (bool bit : page.pendingPresent) bits += bit ? '1' : '0';
        std::string expectedBits; expectedBits.reserve(parent->pendingPresent_.size());
        for (bool bit : parent->pendingPresent_) expectedBits += bit ? '1' : '0';
        if (!advance.encoded || advance.bytes != parent->pendingAdvanceBytes_ ||
            page.pendingOrdinal != parent->pendingOrdinal_ || bits != expectedBits)
        { resourceUnavailable = advance.resourceUnavailable; return {}; }
        const auto before = event_graph::V4FamilyCandidateGraph::SerializeCursor(recovery->family_, origin.axisBudget, page.before);
        if (before.status != event_graph::V4FamilyTraversalStatus::ready || !before.errors.empty())
        { resourceUnavailable = before.status == event_graph::V4FamilyTraversalStatus::resource_unavailable; return {}; }
        const auto identity = V4FamilyIdentityContext::Create(recovery->family_, origin.axisBudget);
        const auto verified = V4FamilyValidatedContext::Create(recovery->family_, origin.axisBudget, page.candidate);
        if (identity.Status() != V4FamilyStatus::schema_only || verified.Status() != V4CoordinateStatus::coordinate_ready)
        { resourceUnavailable = identity.Status() == V4FamilyStatus::resource_unavailable || verified.Status() == V4CoordinateStatus::resource_unavailable; return {}; }
        std::vector<std::string> errors;
        auto work = event_graph::V4NativeJoinedGraph::CreateProjectionWork(identity, verified, page.pendingPresent,
            preambleBudget, resourceUnavailable, errors);
        if (!work || resourceUnavailable || !errors.empty()) return {};
        auto result = std::unique_ptr<V4NativeProjectionLiveJournal>(new V4NativeProjectionLiveJournal);
        result->store_ = recovery->store_; result->lookup_ = recovery->lookup_; result->nextParent_ = parent;
        result->initialReservation_ = reservation; result->baseHead_ = parent->head_; result->head_ = parent->head_;
        result->family_ = recovery->family_; result->binding_ = recovery->binding_; result->work_ = std::move(work);
        result->byteBudget_ = byteBudget; result->indexByteBudget_ = indexByteBudget; result->tokenBudget_ = tokenBudget;
        result->replayBudget_ = replayBudget; result->precheckSerial_ = precheck.Serial(); result->ownerThread_ = recovery->ownerThread_;
        auto& seed = result->seed_; seed.familyCoordinate = origin.familyCoordinate; seed.familyCursor = before.encoded;
        seed.candidateId = page.candidateId; seed.candidateOrdinal = page.candidateOrdinal; seed.leafOrdinal = page.pendingOrdinal;
        seed.leafPresence = bits; seed.advanceBytes = advance.bytes;
        seed.baseHeadName.assign(parent->head_.name.begin(), parent->head_.name.end());
        seed.baseHeadVolume = std::to_string(parent->head_.fileId.VolumeSerialNumber); seed.baseHeadFileId = V4ShardIdHex(parent->head_.fileId);
        seed.coordinateBudget = origin.coordinateBudget; seed.axisBudget = origin.axisBudget;
        seed.advanceWorkBudget = parent->pendingAdvanceWorkBudget_; seed.preambleBudget = preambleBudget;
        bool expectedProof = false;
        if (!proof.used_.compare_exchange_strong(expectedProof, true)) return {};
        bool expectedParent = false;
        if (!parent->stagingBlocked_.compare_exchange_strong(expectedParent, true)) return {};
        result->ownsParent_ = true;
        return result;
    }
    catch (const std::bad_alloc&) { resourceUnavailable = true; return {}; }
    catch (const std::length_error&) { resourceUnavailable = true; return {}; }
    catch (...) { return {}; }
}

inline V4NativeProjectionLiveJournal::~V4NativeProjectionLiveJournal()
{
    if (ownsActivity_ && activeFence_)
    { if (pendingReservation_) pendingReservation_->FailAttempt(); activeFence_->store(false); }
    if (ownsParent_ && nextParent_ && !pageCount_ && !pendingProposal_ && !failed_)
        nextParent_->stagingBlocked_.store(false);
}

// Retains the replayed Work and all journal pins through the final postcheck.
// The returned head is a single-process preview, not a production capability.
class V4NativeStagedFamilyPending final
{
    friend class CheckpointStore;
    friend class V4NativeStagedFamilyRecovery;
    std::shared_ptr<V4NativeProjectionJournalRecovery> recovery_;
    std::shared_ptr<V4ForegroundReservation> reservation_;
    std::shared_ptr<const V4NativeStagedFamilyReceipt> prior_;
    std::shared_ptr<const V4NativeStagedFamilyReceipt> verifyPrior_;
    V4ShardLocator page_, head_;
    std::unique_ptr<V4CheckpointReadPin> pagePin_, headPin_;
    std::string pageBytes_, headBytes_;
    std::size_t pageBudget_ = 0, headBudget_ = 0;
    event_graph::V4FamilyCandidateCursor afterCursor_;
    std::optional<event_graph::V4FamilyCandidateRecord> record_;
    std::vector<event_graph::CandidatePrefixPrune> candidatePruned_;
    std::vector<event_graph::V4FamilyPresencePrune> presencePruned_;
    std::vector<event_graph::V4FamilyActivePrune> activePruned_;
    bool projectionRequired_ = false;
    std::vector<bool> pendingPresent_;
    std::string pendingOrdinal_;
    event_graph::V4FamilyCandidateCursor pendingBeforeCursor_;
    std::string pendingAdvanceBytes_;
    std::size_t pendingAdvanceWorkBudget_ = 0;
    std::uint64_t publishSerial_ = 0;
    std::size_t verifyIndex_ = 0;
    // A receipt chain can cross projection journals. Retain and re-read each
    // distinct contiguous ancestor journal as well as the staged pages above.
    std::shared_ptr<const V4NativeStagedFamilyReceipt> verifyAncestor_;
    std::shared_ptr<V4NativeProjectionJournalRecovery> verifyLastProjection_;
    std::size_t verifyProjectionIndex_ = 0;
    bool verifyAncestorsStarted_ = false;
    bool consumed_ = false, verified_ = false, verificationFailed_ = false, existingHead_ = false;
    V4NativeStagedFamilyPending() = default;
public:
    V4NativeStagedFamilyPending(const V4NativeStagedFamilyPending&) = delete;
    V4NativeStagedFamilyPending& operator=(const V4NativeStagedFamilyPending&) = delete;
    ~V4NativeStagedFamilyPending()
    {
        // A published proposal cannot be silently abandoned while a caller
        // still holds the same reservation. Read-only recovery did not write.
        if (!consumed_ && !existingHead_ && reservation_ && reservation_->attempted_)
            reservation_->FailAttempt();
    }
    V4NativeProjectionRecoveryStep VerifyStep(std::size_t fileBudget) noexcept
    {
        V4NativeProjectionRecoveryStep out;
        try
        {
            if (consumed_ || verificationFailed_ || !V4ForegroundReservation::Canonical(reservation_) || !recovery_ || !recovery_->ownerThread_ || GetCurrentThreadId() != recovery_->ownerThread_ || !recovery_->store_ || !recovery_->lookup_ || !pagePin_ || !headPin_ ||
                recovery_->phase_ != V4NativeProjectionJournalRecovery::Phase::ready || !recovery_->work_ ||
                !recovery_->work_->CanAuthorizeActiveGraph())
            { out.unavailable = true; out.reason = "v4_native_staged_verify_unavailable"; return out; }
            if (verified_) { out.complete = true; out.reason = "v4_native_staged_postcheck_pending"; return out; }
            const auto priorDepth = prior_ ? prior_->depth_ : 0;
            if (priorDepth > ((std::numeric_limits<std::size_t>::max)() - 3) / 2 ||
                recovery_->entries_.size() > ((std::numeric_limits<std::size_t>::max)() - 3) / 2 - priorDepth)
            { out.resourceUnavailable = true; out.reason = "v4_native_staged_verify_budget_unavailable"; return out; }
            const auto baseIndex = recovery_->entries_.size() * 2;
            const auto priorEnd = baseIndex + 1 + priorDepth * 2;
            const auto count = priorEnd + 2;
            while (verifyIndex_ < count && out.workUsed < fileBudget)
            {
                const V4ShardLocator* locator = nullptr; const std::string* expected = nullptr; std::size_t limit = 0;
                if (verifyIndex_ < recovery_->entries_.size() * 2)
                {
                    const auto& entry = recovery_->entries_[verifyIndex_ / 2]; const bool node = verifyIndex_ % 2 == 0;
                    locator = node ? &entry.node : &entry.page; expected = node ? &entry.nodeBytes : &entry.pageBytes; limit = recovery_->limits_.bytes;
                }
                else if (verifyIndex_ == baseIndex)
                { locator = &recovery_->base_; expected = &recovery_->baseBytes_; limit = recovery_->limits_.bytes; }
                else if (verifyIndex_ < priorEnd)
                {
                    const auto offset = verifyIndex_ - baseIndex - 1;
                    if (!verifyPrior_ && offset == 0) verifyPrior_ = prior_;
                    if (!verifyPrior_ || !verifyPrior_->pagePin_ || !verifyPrior_->headPin_)
                    { verificationFailed_ = true; out.unavailable = true; out.reason = "v4_native_staged_prior_unavailable"; return out; }
                    const bool node = offset % 2 == 0; locator = node ? &verifyPrior_->page_ : &verifyPrior_->head_;
                    expected = node ? &verifyPrior_->pageBytes_ : &verifyPrior_->headBytes_; limit = node ? verifyPrior_->pageBudget_ : verifyPrior_->headBudget_;
                }
                else if (verifyIndex_ == priorEnd)
                { locator = &page_; expected = &pageBytes_; limit = pageBudget_; }
                else { locator = &head_; expected = &headBytes_; limit = headBudget_; }
                const auto read = recovery_->store_->Read(locator->name, locator->fileId, recovery_->binding_, limit);
                if (read.status != CodecStatus::decoded || read.recoveredInFlight || read.bytes != *expected)
                { out.resourceUnavailable = read.status == CodecStatus::resource_unavailable; out.unavailable = !out.resourceUnavailable;
                  if (out.unavailable) verificationFailed_ = true; out.reason = "v4_native_staged_snapshot_changed"; return out; }
                if (verifyIndex_ > baseIndex && verifyIndex_ < priorEnd && (verifyIndex_ - baseIndex - 1) % 2 == 1)
                    verifyPrior_ = verifyPrior_->prior_;
                ++verifyIndex_; ++out.workUsed;
            }
            if (verifyIndex_ == count)
            {
                if (!verifyAncestorsStarted_)
                { verifyAncestor_ = prior_; verifyLastProjection_ = recovery_; verifyAncestorsStarted_ = true; }
                while (verifyAncestor_ && out.workUsed < fileBudget)
                {
                    const auto ancestor = verifyAncestor_->recovery_;
                    if (!ancestor || ancestor->store_.get() != recovery_->store_.get() ||
                        ancestor->binding_ != recovery_->binding_ || !ancestor->lookup_ ||
                        ancestor->lookup_.get() != recovery_->lookup_.get() ||
                        ancestor->lookup_.owner_before(recovery_->lookup_) || recovery_->lookup_.owner_before(ancestor->lookup_) ||
                        ancestor->phase_ != V4NativeProjectionJournalRecovery::Phase::ready || !ancestor->basePin_ ||
                        !ancestor->work_ || !ancestor->work_->CanAuthorizeActiveGraph())
                    { verificationFailed_ = true; out.unavailable = true; out.reason = "v4_native_staged_ancestor_projection_unavailable"; return out; }
                    const bool same = verifyLastProjection_ && !verifyLastProjection_.owner_before(ancestor) &&
                        !ancestor.owner_before(verifyLastProjection_);
                    if (same)
                    { verifyAncestor_ = verifyAncestor_->prior_; verifyProjectionIndex_ = 0; continue; }
                    if (ancestor->entries_.size() > ((std::numeric_limits<std::size_t>::max)() - 1) / 2)
                    { out.resourceUnavailable = true; out.reason = "v4_native_staged_ancestor_projection_budget"; return out; }
                    const auto entries = ancestor->entries_.size() * 2;
                    const V4ShardLocator* locator = &ancestor->base_;
                    const std::string* expected = &ancestor->baseBytes_;
                    if (verifyProjectionIndex_ < entries)
                    {
                        const auto& entry = ancestor->entries_[verifyProjectionIndex_ / 2];
                        if (!entry.nodePin || !entry.pagePin)
                        { verificationFailed_ = true; out.unavailable = true; out.reason = "v4_native_staged_ancestor_projection_pin"; return out; }
                        const bool node = verifyProjectionIndex_ % 2 == 0;
                        locator = node ? &entry.node : &entry.page;
                        expected = node ? &entry.nodeBytes : &entry.pageBytes;
                    }
                    const auto read = recovery_->store_->Read(locator->name, locator->fileId, recovery_->binding_, ancestor->limits_.bytes);
                    if (read.status != CodecStatus::decoded || read.recoveredInFlight || read.bytes != *expected)
                    { out.resourceUnavailable = read.status == CodecStatus::resource_unavailable; out.unavailable = !out.resourceUnavailable;
                      if (out.unavailable) verificationFailed_ = true; out.reason = "v4_native_staged_ancestor_projection_changed"; return out; }
                    ++verifyProjectionIndex_; ++out.workUsed;
                    if (verifyProjectionIndex_ == entries + 1)
                    { verifyLastProjection_ = ancestor; verifyAncestor_ = verifyAncestor_->prior_; verifyProjectionIndex_ = 0; }
                }
                if (!verifyAncestor_)
                { publishSerial_ = recovery_->lookup_->recheckSerial_; verified_ = true; out.complete = true; out.reason = "v4_native_staged_postcheck_pending"; return out; }
            }
            out.reason = "v4_native_staged_verify_pending"; return out;
        }
        catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_native_staged_verify_resource_unavailable"; return out; }
        catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_native_staged_verify_resource_unavailable"; return out; }
        catch (...) { verificationFailed_ = true; out.unavailable = true; out.reason = "v4_native_staged_verify_unavailable"; return out; }
    }
    std::shared_ptr<V4NativeStagedFamilyReceipt> Finalize(const V4ShardFamilyRecheck& postcheck, bool& resourceUnavailable) noexcept
    {
        resourceUnavailable = false;
        try
        {
            if (consumed_ || !verified_ || verificationFailed_ || !V4ForegroundReservation::Canonical(reservation_) || !recovery_ || !recovery_->ownerThread_ || GetCurrentThreadId() != recovery_->ownerThread_ || !recovery_->store_ || !recovery_->lookup_ || !pagePin_ || !headPin_ ||
                recovery_->phase_ != V4NativeProjectionJournalRecovery::Phase::ready || !recovery_->work_ ||
                !recovery_->work_->CanAuthorizeActiveGraph() || !postcheck.Matches(*recovery_->lookup_) ||
                postcheck.Serial() <= publishSerial_) return {};
            const auto page = recovery_->store_->Read(page_.name, page_.fileId, recovery_->binding_, pageBudget_);
            const auto head = recovery_->store_->Read(head_.name, head_.fileId, recovery_->binding_, headBudget_);
            if (page.status != CodecStatus::decoded || head.status != CodecStatus::decoded || page.recoveredInFlight || head.recoveredInFlight ||
                page.bytes != pageBytes_ || head.bytes != headBytes_)
            { resourceUnavailable = page.status == CodecStatus::resource_unavailable || head.status == CodecStatus::resource_unavailable; return {}; }
            const auto priorBytes = prior_ ? prior_->retainedBytes_ : std::size_t{0};
            const auto maximum = (std::numeric_limits<std::size_t>::max)();
            if (pageBytes_.size() > maximum - priorBytes || headBytes_.size() > maximum - priorBytes - pageBytes_.size())
            { resourceUnavailable = true; return {}; }
            auto result = std::shared_ptr<V4NativeStagedFamilyReceipt>(new V4NativeStagedFamilyReceipt);
            result->self_ = result;
            result->recovery_ = recovery_; result->prior_ = prior_; result->page_ = page_; result->head_ = head_;
            result->pageBytes_ = pageBytes_; result->headBytes_ = headBytes_; result->cursor_ = afterCursor_; result->record_ = record_;
            result->candidatePruned_ = candidatePruned_; result->presencePruned_ = presencePruned_; result->activePruned_ = activePruned_;
            result->projectionRequired_ = projectionRequired_; result->pendingPresent_ = pendingPresent_; result->pendingOrdinal_ = pendingOrdinal_;
            result->pendingBeforeCursor_ = pendingBeforeCursor_; result->pendingAdvanceBytes_ = pendingAdvanceBytes_;
            result->pendingAdvanceWorkBudget_ = pendingAdvanceWorkBudget_;
            result->pageBudget_ = pageBudget_; result->headBudget_ = headBudget_; result->depth_ = prior_ ? prior_->depth_ + 1 : 1;
            result->retainedBytes_ = priorBytes + pageBytes_.size() + headBytes_.size();
            if (existingHead_)
            {
                if (reservation_->attempted_ || reservation_->advanced_ || !reservation_->Matches(*recovery_->store_, head_, reservation_->headBudget_)) return {};
            }
            else if (!reservation_->Commit(head_, reservation_->headBudget_, headBudget_, resourceUnavailable)) return {};
            result->postcheckSerial_ = postcheck.Serial(); result->pagePin_ = std::move(pagePin_); result->headPin_ = std::move(headPin_);
            if (existingHead_) reservation_->FinishReadOnly(); else reservation_->Finish();
            consumed_ = true; return result;
        }
        catch (const std::bad_alloc&) { resourceUnavailable = true; return {}; }
        catch (const std::length_error&) { resourceUnavailable = true; return {}; }
        catch (...) { return {}; }
    }
};

inline V4NativeStagedFamilyPublication CheckpointStore::PublishV4NativeStagedAttachPreview(
    const std::shared_ptr<V4NativeProjectionJournalRecovery>& recovery, const std::shared_ptr<V4ForegroundReservation>& reservation, const V4ShardLocator& currentHead,
    const V4ShardFamilyRecheck& precheck, std::size_t byteBudget, std::size_t indexByteBudget,
    std::size_t pinBudget, std::size_t retainedByteBudget) noexcept
{
    V4NativeStagedFamilyPublication out;
    const auto attempted = [&] { return out.publications[0].temporaryCreateAttempted || out.publications[1].temporaryCreateAttempted ||
        out.publications[0].temporaryCreated || out.publications[1].temporaryCreated ||
        out.publications[0].renameAttempted || out.publications[1].renameAttempted; };
    try
    {
        const auto recoveryIdentity = recovery ? recovery->self_.lock() : std::shared_ptr<V4NativeProjectionJournalRecovery>{};
        if (!V4ForegroundReservation::Canonical(reservation) || !reservation->Matches(*this, currentHead, reservation->headBudget_) ||
            !recovery || !recoveryIdentity || recoveryIdentity.owner_before(recovery) || recovery.owner_before(recoveryIdentity) ||
            !recovery->ownerThread_ || GetCurrentThreadId() != recovery->ownerThread_ || recovery->store_.get() != this || !recovery->lookup_ || recovery->stagingBlocked_ ||
            recovery->phase_ != V4NativeProjectionJournalRecovery::Phase::ready || !recovery->work_ ||
            !recovery->work_->CanAuthorizeActiveGraph() || !V4NativeProjectionJournalRecovery::Same(recovery->final_, currentHead) ||
            (recovery->nestedParent_ && (!recovery->nestedParent_->stagingBlocked_ || !recovery->nestedReservation_ || !recovery->nestedActivity_ ||
                !reservation->operationFence_ || recovery->nestedActivity_.owner_before(reservation->operationFence_) ||
                reservation->operationFence_.owner_before(recovery->nestedActivity_) || !recovery->nestedActivity_->load() ||
                recovery->nestedReservation_.owner_before(reservation) || reservation.owner_before(recovery->nestedReservation_))) ||
            !precheck.Matches(*recovery->lookup_) || precheck.Serial() < recovery->readySerial_ || recovery->entries_.empty())
        { out.reason = "v4_native_attach_recovery_unavailable"; return out; }
        if (recovery->nestedParentPins_ > pinBudget || recovery->nestedParentBytes_ > retainedByteBudget)
        { out.resourceUnavailable = true; out.reason = "v4_native_attach_retention_budget_unavailable"; return out; }
        const auto childPinBudget = pinBudget - recovery->nestedParentPins_;
        const auto childByteBudget = retainedByteBudget - recovery->nestedParentBytes_;
        if (childPinBudget < 3 || recovery->entries_.size() > (childPinBudget - 3) / 2 || recovery->retainedBytes_ > childByteBudget ||
            byteBudget > childByteBudget - recovery->retainedBytes_ ||
            indexByteBudget > childByteBudget - recovery->retainedBytes_ - byteBudget)
        { out.resourceUnavailable = true; out.reason = "v4_native_attach_retention_budget_unavailable"; return out; }
        const auto& origin = recovery->entries_.back().audit;
        const auto proposal = event_graph::V4FamilyCandidateGraph::PrepareStagedNativeAttach(
            recovery->family_, origin.axisBudget, recovery->leafCursor_, *recovery->work_);
        if (proposal.status != event_graph::V4FamilyTraversalStatus::ready || !proposal.proposal || !proposal.errors.empty())
        { out.resourceUnavailable = proposal.status == event_graph::V4FamilyTraversalStatus::resource_unavailable; out.reason = "v4_native_attach_proposal_unavailable"; return out; }
        const auto encoded = event_graph::V4FamilyCandidateGraph::EncodeStagedNativePage(
            recovery->family_, origin.axisBudget, *proposal.proposal, recovery->work_.get(), byteBudget);
        const auto before = event_graph::V4FamilyCandidateGraph::SerializeCursor(recovery->family_, origin.axisBudget, proposal.proposal->Page().before);
        const auto after = event_graph::V4FamilyCandidateGraph::SerializeStagedNativeCursor(
            recovery->family_, origin.axisBudget, proposal.proposal->Page().after, *recovery->work_);
        if (!encoded.encoded || before.status != event_graph::V4FamilyTraversalStatus::ready || !before.errors.empty() ||
            after.status != event_graph::V4FamilyTraversalStatus::ready || !after.errors.empty())
        { out.resourceUnavailable = encoded.resourceUnavailable || before.status == event_graph::V4FamilyTraversalStatus::resource_unavailable ||
              after.status == event_graph::V4FamilyTraversalStatus::resource_unavailable;
          out.reason = "v4_native_attach_encoding_unavailable"; return out; }
        V4NativeStagedFamilyAudit audit; audit.journalHeadName.assign(currentHead.name.begin(), currentHead.name.end());
        audit.journalHeadVolume = std::to_string(currentHead.fileId.VolumeSerialNumber); audit.journalHeadFileId = V4ShardIdHex(currentHead.fileId);
        audit.familyCoordinate = origin.familyCoordinate; audit.beforeCursor = before.encoded; audit.afterCursor = after.encoded;
        audit.stage = "attach"; audit.proposalBytes = encoded.bytes; audit.workBudget = 1;
        State shard; shard.binding = recovery->binding_; shard.familyVersion = FamilyVersion::v4; shard.v4TypedEnabled = true;
        shard.nativeStagedFamily = std::move(audit);
        const auto expected = EncodeCheckpoint(shard, recovery->binding_, byteBudget);
        if (expected.status != CodecStatus::encoded)
        { out.resourceUnavailable = expected.status == CodecStatus::resource_unavailable; out.reason = "v4_native_attach_budget_unavailable"; return out; }
        out.publications[0] = Publish(shard, recovery->binding_, byteBudget);
        if (attempted()) reservation->MarkAttempted();
        if (out.publications[0].status != StoreStatus::published_durability_unverified || !out.publications[0].fileIdentityCaptured ||
            !out.publications[0].renameReturnedSuccess)
        { recovery->stagingBlocked_ = attempted(); out.reason = "v4_native_attach_publish_unavailable"; return out; }
        V4ShardLocator page{out.publications[0].fileName, out.publications[0].fileId};
        const auto pageRead = ReadV4NativeStagedFamilyShard(*this, page.name, page.fileId, recovery->binding_, byteBudget);
        if (!pageRead.read || pageRead.bytes != expected.bytes)
        { recovery->stagingBlocked_ = true; out.resourceUnavailable = pageRead.resourceUnavailable; out.reason = "v4_native_attach_readback_unavailable"; return out; }
        auto index = AppendV4ShardIndex(*this, currentHead, page, "native_staged_family", recovery->binding_, origin.coordinateBudget,
            origin.axisBudget, 0, byteBudget, byteBudget, 0, byteBudget, indexByteBudget);
        out.publications[1] = std::move(index.publication);
        if (!index.published || !index.head)
        { recovery->stagingBlocked_ = true; out.resourceUnavailable = index.resourceUnavailable; out.reason = "v4_native_attach_index_unavailable"; return out; }
        auto pagePin = PinRead(page.name, page.fileId); auto headPin = PinRead(index.head->name, index.head->fileId);
        const auto headRead = Read(index.head->name, index.head->fileId, recovery->binding_, indexByteBudget);
        if (!pagePin || !headPin || headRead.status != CodecStatus::decoded || headRead.recoveredInFlight || !headRead.state.shardIndex)
        { recovery->stagingBlocked_ = true; out.resourceUnavailable = headRead.status == CodecStatus::resource_unavailable; out.reason = "v4_native_attach_pin_unavailable"; return out; }
        const auto& link = *headRead.state.shardIndex;
        if (link.dataKind != "native_staged_family" || link.dataName != std::string(page.name.begin(), page.name.end()) ||
            link.dataVolume != std::to_string(page.fileId.VolumeSerialNumber) || link.dataFileId != V4ShardIdHex(page.fileId) ||
            link.previousName != std::string(currentHead.name.begin(), currentHead.name.end()) ||
            link.previousVolume != std::to_string(currentHead.fileId.VolumeSerialNumber) || link.previousFileId != V4ShardIdHex(currentHead.fileId))
        { recovery->stagingBlocked_ = true; out.reason = "v4_native_attach_link_mismatch"; return out; }
        auto pending = std::unique_ptr<V4NativeStagedFamilyPending>(new V4NativeStagedFamilyPending);
        pending->recovery_ = recovery; pending->reservation_ = reservation; pending->prior_ = recovery->nestedParent_;
        pending->page_ = page; pending->head_ = *index.head;
        pending->pagePin_ = std::move(pagePin); pending->headPin_ = std::move(headPin);
        pending->pageBytes_ = pageRead.bytes; pending->headBytes_ = headRead.bytes;
        pending->afterCursor_ = proposal.proposal->Page().after;
        pending->record_ = proposal.proposal->Page().record;
        pending->candidatePruned_ = proposal.proposal->Page().candidatePruned;
        pending->presencePruned_ = proposal.proposal->Page().presencePruned;
        pending->activePruned_ = proposal.proposal->Page().activePruned;
        pending->projectionRequired_ = proposal.proposal->Page().projectionRequired;
        pending->pendingPresent_ = proposal.proposal->Page().pendingPresent;
        pending->pendingOrdinal_ = proposal.proposal->Page().pendingOrdinal;
        pending->pageBudget_ = byteBudget; pending->headBudget_ = indexByteBudget;
        pending->publishSerial_ = recovery->lookup_->recheckSerial_;
        recovery->stagingBlocked_ = true; out.pending = std::move(pending); out.staged = true;
        out.reason = "v4_native_attach_postcheck_pending_preview_only"; return out;
    }
    catch (const std::bad_alloc&) { if (attempted() && reservation) reservation->MarkAttempted(); if (recovery && attempted()) recovery->stagingBlocked_ = true; out.resourceUnavailable = true; out.reason = "v4_native_attach_resource_unavailable"; return out; }
    catch (const std::length_error&) { if (attempted() && reservation) reservation->MarkAttempted(); if (recovery && attempted()) recovery->stagingBlocked_ = true; out.resourceUnavailable = true; out.reason = "v4_native_attach_resource_unavailable"; return out; }
    catch (...) { if (attempted() && reservation) reservation->MarkAttempted(); if (recovery && attempted()) recovery->stagingBlocked_ = true; out.reason = "v4_native_attach_publish_unavailable"; return out; }
}

inline V4NativeStagedFamilyPublication CheckpointStore::PublishV4NativeStagedPagePreview(
    const std::shared_ptr<V4NativeStagedFamilyReceipt>& receipt, const std::shared_ptr<V4ForegroundReservation>& reservation, const V4ShardLocator& currentHead,
    const V4ShardFamilyRecheck& precheck, std::size_t workBudget, std::size_t byteBudget, std::size_t indexByteBudget,
    std::size_t pinBudget, std::size_t retainedByteBudget, V4NativeStagedOperation operation) noexcept
{
    V4NativeStagedFamilyPublication out;
    const auto attempted = [&] { return out.publications[0].temporaryCreateAttempted || out.publications[1].temporaryCreateAttempted ||
        out.publications[0].temporaryCreated || out.publications[1].temporaryCreated ||
        out.publications[0].renameAttempted || out.publications[1].renameAttempted; };
    try
    {
        const auto recovery = receipt ? receipt->recovery_ : std::shared_ptr<V4NativeProjectionJournalRecovery>{};
        const auto receiptIdentity = receipt ? receipt->self_.lock() : std::shared_ptr<V4NativeStagedFamilyReceipt>{};
        if ((operation != V4NativeStagedOperation::graph && operation != V4NativeStagedOperation::continue_presence && operation != V4NativeStagedOperation::next_candidate) ||
            !V4ForegroundReservation::Canonical(reservation) || !reservation->Matches(*this, currentHead, reservation->headBudget_) ||
            !receipt || !receiptIdentity || receiptIdentity.owner_before(receipt) || receipt.owner_before(receiptIdentity) ||
            !recovery || !recovery->ownerThread_ || GetCurrentThreadId() != recovery->ownerThread_ ||
            recovery->store_.get() != this || !recovery->lookup_ || receipt->stagingBlocked_ || receipt->projectionRequired_ || !receipt->pagePin_ || !receipt->headPin_ ||
            recovery->phase_ != V4NativeProjectionJournalRecovery::Phase::ready || !recovery->work_ ||
            !recovery->work_->CanAuthorizeActiveGraph() || !V4NativeProjectionJournalRecovery::Same(receipt->head_, currentHead) ||
            !precheck.Matches(*recovery->lookup_) || precheck.Serial() < receipt->postcheckSerial_ || recovery->entries_.empty())
        { out.reason = "v4_native_graph_receipt_unavailable"; return out; }
        std::size_t retainedPins = 0, retainedBytes = 0; bool resource = false;
        if (!receipt->RetainedResources(retainedPins, retainedBytes, resource))
        { out.resourceUnavailable = resource; out.reason = "v4_native_graph_retention_unavailable"; return out; }
        if (pinBudget < 2 || retainedPins > pinBudget - 2 ||
            retainedBytes > retainedByteBudget || byteBudget > retainedByteBudget - retainedBytes ||
            indexByteBudget > retainedByteBudget - retainedBytes - byteBudget)
        { out.resourceUnavailable = true; out.reason = "v4_native_graph_retention_budget_unavailable"; return out; }
        const auto& origin = recovery->entries_.back().audit;
        const auto proposal = operation == V4NativeStagedOperation::continue_presence ? event_graph::V4FamilyCandidateGraph::PrepareStagedNativeContinue(
            recovery->family_, origin.axisBudget, receipt->cursor_, workBudget) : operation == V4NativeStagedOperation::next_candidate ?
            event_graph::V4FamilyCandidateGraph::PrepareStagedNativeNextCandidate(
                recovery->family_, origin.axisBudget, receipt->cursor_, workBudget) :
            event_graph::V4FamilyCandidateGraph::PrepareStagedNativeGraphs(
                recovery->family_, origin.axisBudget, receipt->cursor_, *recovery->work_, workBudget);
        if (proposal.status != event_graph::V4FamilyTraversalStatus::ready || !proposal.proposal || !proposal.errors.empty())
        { out.resourceUnavailable = proposal.status == event_graph::V4FamilyTraversalStatus::resource_unavailable; out.reason = "v4_native_graph_proposal_unavailable"; return out; }
        const auto& page = proposal.proposal->Page();
        V4ProjectionPageEncoding nextAdvance;
        if (page.projectionRequired)
        {
            nextAdvance = EncodeV4ProjectionAdvance(recovery->family_, origin.axisBudget, page, byteBudget);
            if (!nextAdvance.encoded) { out.resourceUnavailable = nextAdvance.resourceUnavailable; out.reason = "v4_native_next_leaf_encoding_unavailable"; return out; }
        }
        const auto encoded = event_graph::V4FamilyCandidateGraph::EncodeStagedNativePage(
            recovery->family_, origin.axisBudget, *proposal.proposal, recovery->work_.get(), byteBudget);
        const auto before = event_graph::V4FamilyCandidateGraph::SerializeAnyStagedCursor(
            recovery->family_, origin.axisBudget, page.before, recovery->work_.get());
        const auto after = event_graph::V4FamilyCandidateGraph::SerializeAnyStagedCursor(
            recovery->family_, origin.axisBudget, page.after, recovery->work_.get());
        if (!encoded.encoded || before.status != event_graph::V4FamilyTraversalStatus::ready || !before.errors.empty() ||
            after.status != event_graph::V4FamilyTraversalStatus::ready || !after.errors.empty() ||
                    (before.encoded == after.encoded && !page.workUsed && !page.record && !page.projectionRequired && page.candidatePruned.empty() && page.activePruned.empty() && page.presencePruned.empty()))
        { out.resourceUnavailable = encoded.resourceUnavailable || before.status == event_graph::V4FamilyTraversalStatus::resource_unavailable ||
              after.status == event_graph::V4FamilyTraversalStatus::resource_unavailable;
          out.reason = "v4_native_graph_encoding_or_progress_unavailable"; return out; }
        auto nextCursor = page.after; auto nextRecord = page.record;
        V4NativeStagedFamilyAudit audit; audit.journalHeadName.assign(currentHead.name.begin(), currentHead.name.end());
        audit.journalHeadVolume = std::to_string(currentHead.fileId.VolumeSerialNumber); audit.journalHeadFileId = V4ShardIdHex(currentHead.fileId);
        audit.familyCoordinate = origin.familyCoordinate; audit.beforeCursor = before.encoded; audit.afterCursor = after.encoded;
        audit.stage = operation == V4NativeStagedOperation::continue_presence ? "continue" :
            operation == V4NativeStagedOperation::next_candidate ? "next_candidate" : "graph";
        audit.proposalBytes = encoded.bytes; audit.workBudget = workBudget;
        State shard; shard.binding = recovery->binding_; shard.familyVersion = FamilyVersion::v4; shard.v4TypedEnabled = true;
        shard.nativeStagedFamily = std::move(audit);
        const auto expected = EncodeCheckpoint(shard, recovery->binding_, byteBudget);
        if (expected.status != CodecStatus::encoded)
        { out.resourceUnavailable = expected.status == CodecStatus::resource_unavailable; out.reason = "v4_native_graph_budget_unavailable"; return out; }
        out.publications[0] = Publish(shard, recovery->binding_, byteBudget);
        if (attempted()) reservation->MarkAttempted();
        if (out.publications[0].status != StoreStatus::published_durability_unverified || !out.publications[0].fileIdentityCaptured ||
            !out.publications[0].renameReturnedSuccess)
        { receipt->stagingBlocked_ = attempted(); out.reason = "v4_native_graph_publish_unavailable"; return out; }
        V4ShardLocator data{out.publications[0].fileName, out.publications[0].fileId};
        const auto dataRead = ReadV4NativeStagedFamilyShard(*this, data.name, data.fileId, recovery->binding_, byteBudget);
        if (!dataRead.read || dataRead.bytes != expected.bytes)
        { receipt->stagingBlocked_ = true; out.resourceUnavailable = dataRead.resourceUnavailable; out.reason = "v4_native_graph_readback_unavailable"; return out; }
        auto index = AppendV4ShardIndex(*this, currentHead, data, "native_staged_family", recovery->binding_, origin.coordinateBudget,
            origin.axisBudget, 0, byteBudget, byteBudget, 0, byteBudget, indexByteBudget);
        out.publications[1] = std::move(index.publication);
        if (!index.published || !index.head)
        { receipt->stagingBlocked_ = true; out.resourceUnavailable = index.resourceUnavailable; out.reason = "v4_native_graph_index_unavailable"; return out; }
        auto dataPin = PinRead(data.name, data.fileId); auto headPin = PinRead(index.head->name, index.head->fileId);
        const auto headRead = Read(index.head->name, index.head->fileId, recovery->binding_, indexByteBudget);
        if (!dataPin || !headPin || headRead.status != CodecStatus::decoded || headRead.recoveredInFlight || !headRead.state.shardIndex)
        { receipt->stagingBlocked_ = true; out.resourceUnavailable = headRead.status == CodecStatus::resource_unavailable; out.reason = "v4_native_graph_pin_unavailable"; return out; }
        const auto& link = *headRead.state.shardIndex;
        if (link.dataKind != "native_staged_family" || link.dataName != std::string(data.name.begin(), data.name.end()) ||
            link.dataVolume != std::to_string(data.fileId.VolumeSerialNumber) || link.dataFileId != V4ShardIdHex(data.fileId) ||
            link.previousName != std::string(currentHead.name.begin(), currentHead.name.end()) ||
            link.previousVolume != std::to_string(currentHead.fileId.VolumeSerialNumber) || link.previousFileId != V4ShardIdHex(currentHead.fileId))
        { receipt->stagingBlocked_ = true; out.reason = "v4_native_graph_link_mismatch"; return out; }
        auto pending = std::unique_ptr<V4NativeStagedFamilyPending>(new V4NativeStagedFamilyPending);
        pending->recovery_ = recovery; pending->reservation_ = reservation; pending->prior_ = receipt; pending->page_ = data; pending->head_ = *index.head;
        pending->pagePin_ = std::move(dataPin); pending->headPin_ = std::move(headPin);
        pending->pageBytes_ = dataRead.bytes; pending->headBytes_ = headRead.bytes;
        pending->afterCursor_ = std::move(nextCursor); pending->record_ = std::move(nextRecord);
        pending->candidatePruned_ = page.candidatePruned; pending->presencePruned_ = page.presencePruned;
        pending->activePruned_ = page.activePruned;
        pending->projectionRequired_ = page.projectionRequired; pending->pendingPresent_ = page.pendingPresent; pending->pendingOrdinal_ = page.pendingOrdinal;
        if (page.projectionRequired)
        { pending->pendingBeforeCursor_ = page.before; pending->pendingAdvanceBytes_ = std::move(nextAdvance.bytes); pending->pendingAdvanceWorkBudget_ = workBudget; }
        pending->pageBudget_ = byteBudget; pending->headBudget_ = indexByteBudget;
        pending->publishSerial_ = recovery->lookup_->recheckSerial_;
        receipt->stagingBlocked_ = true; out.pending = std::move(pending); out.staged = true;
        out.reason = "v4_native_graph_postcheck_pending_preview_only"; return out;
    }
    catch (const std::bad_alloc&) { if (attempted() && reservation) reservation->MarkAttempted(); if (receipt && attempted()) receipt->stagingBlocked_ = true; out.resourceUnavailable = true; out.reason = "v4_native_graph_resource_unavailable"; return out; }
    catch (const std::length_error&) { if (attempted() && reservation) reservation->MarkAttempted(); if (receipt && attempted()) receipt->stagingBlocked_ = true; out.resourceUnavailable = true; out.reason = "v4_native_graph_resource_unavailable"; return out; }
    catch (...) { if (attempted() && reservation) reservation->MarkAttempted(); if (receipt && attempted()) receipt->stagingBlocked_ = true; out.reason = "v4_native_graph_publish_unavailable"; return out; }
}

inline V4NativeStagedFamilyPublication CheckpointStore::PublishV4NativeStagedGraphPreview(
    const std::shared_ptr<V4NativeStagedFamilyReceipt>& receipt, const std::shared_ptr<V4ForegroundReservation>& reservation, const V4ShardLocator& currentHead,
    const V4ShardFamilyRecheck& precheck, std::size_t workBudget, std::size_t byteBudget, std::size_t indexByteBudget,
    std::size_t pinBudget, std::size_t retainedByteBudget) noexcept
{ return PublishV4NativeStagedPagePreview(receipt, reservation, currentHead, precheck, workBudget, byteBudget, indexByteBudget, pinBudget, retainedByteBudget, V4NativeStagedOperation::graph); }

inline V4NativeStagedFamilyPublication CheckpointStore::PublishV4NativeStagedContinuePreview(
    const std::shared_ptr<V4NativeStagedFamilyReceipt>& receipt, const std::shared_ptr<V4ForegroundReservation>& reservation, const V4ShardLocator& currentHead,
    const V4ShardFamilyRecheck& precheck, std::size_t workBudget, std::size_t byteBudget, std::size_t indexByteBudget,
    std::size_t pinBudget, std::size_t retainedByteBudget) noexcept
{ return PublishV4NativeStagedPagePreview(receipt, reservation, currentHead, precheck, workBudget, byteBudget, indexByteBudget, pinBudget, retainedByteBudget, V4NativeStagedOperation::continue_presence); }

inline V4NativeStagedFamilyPublication CheckpointStore::PublishV4NativeStagedNextCandidatePreview(
    const std::shared_ptr<V4NativeStagedFamilyReceipt>& receipt, const std::shared_ptr<V4ForegroundReservation>& reservation, const V4ShardLocator& currentHead,
    const V4ShardFamilyRecheck& precheck, std::size_t workBudget, std::size_t byteBudget, std::size_t indexByteBudget,
    std::size_t pinBudget, std::size_t retainedByteBudget) noexcept
{ return PublishV4NativeStagedPagePreview(receipt, reservation, currentHead, precheck, workBudget, byteBudget, indexByteBudget, pinBudget, retainedByteBudget, V4NativeStagedOperation::next_candidate); }

struct V4NativeStagedFamilyRecoveryStep
{
    bool ready = false, resourceUnavailable = false, unavailable = false;
    std::size_t workUsed = 0;
    std::string_view reason;
};
// Rebuilds an indexed attach/graph chain using the retained, fully replayed
// projection. It returns a pending verifier, never an execution capability.
class V4NativeStagedFamilyRecovery final
{
    struct Entry
    {
        V4ShardLocator node, page;
        std::unique_ptr<V4CheckpointReadPin> nodePin, pagePin;
        std::string nodeBytes, pageBytes;
        V4NativeStagedFamilyAudit audit;
    };
    std::shared_ptr<V4NativeProjectionJournalRecovery> projection_;
    V4ShardLocator final_;
    V4ShardIndexCursor scan_;
    std::vector<Entry> entries_; // newest first
    std::shared_ptr<V4NativeStagedFamilyReceipt> last_;
    event_graph::V4FamilyCandidateCursor cursor_;
    std::size_t byteLimit_ = 0, pageLimit_ = 0, totalWorkLimit_ = 0, retained_ = 0, replayIndex_ = 0, graphWork_ = 0;
    enum class Phase { scan, replay, ready, taken, failed } phase_ = Phase::scan;
    V4NativeStagedFamilyRecovery() = default;
    static bool Same(const V4ShardLocator& a, const V4ShardLocator& b) noexcept
    { return a.name == b.name && a.fileId.VolumeSerialNumber == b.fileId.VolumeSerialNumber &&
        std::memcmp(a.fileId.FileId.Identifier, b.fileId.FileId.Identifier, sizeof(a.fileId.FileId.Identifier)) == 0; }
public:
    V4NativeStagedFamilyRecovery(const V4NativeStagedFamilyRecovery&) = delete;
    V4NativeStagedFamilyRecovery& operator=(const V4NativeStagedFamilyRecovery&) = delete;
    static std::unique_ptr<V4NativeStagedFamilyRecovery> Begin(
        std::shared_ptr<V4NativeProjectionJournalRecovery> projection, const V4ShardLocator& finalHead,
        const V4ShardFamilyRecheck& precheck, std::size_t pages, std::size_t bytes, std::size_t totalWork,
        bool& resourceUnavailable)
    {
        resourceUnavailable = false;
        try
        {
            const auto identity = projection ? projection->self_.lock() : std::shared_ptr<V4NativeProjectionJournalRecovery>{};
            if (!projection || !identity || identity.owner_before(projection) || projection.owner_before(identity) ||
                !projection->ownerThread_ || GetCurrentThreadId() != projection->ownerThread_ || !projection->store_ ||
                !projection->lookup_ || projection->phase_ != V4NativeProjectionJournalRecovery::Phase::ready ||
                !projection->work_ || !projection->work_->CanAuthorizeActiveGraph() ||
                !precheck.Matches(*projection->lookup_) || precheck.Serial() < projection->readySerial_ ||
                Same(finalHead, projection->final_)) return {};
            auto result = std::unique_ptr<V4NativeStagedFamilyRecovery>(new V4NativeStagedFamilyRecovery);
            result->projection_ = std::move(projection); result->final_ = finalHead;
            result->scan_ = V4ShardIndexCursor{finalHead, {}}; result->cursor_ = result->projection_->leafCursor_;
            result->pageLimit_ = pages; result->byteLimit_ = bytes; result->totalWorkLimit_ = totalWork; return result;
        }
        catch (const std::bad_alloc&) { resourceUnavailable = true; return {}; }
        catch (const std::length_error&) { resourceUnavailable = true; return {}; }
        catch (...) { return {}; }
    }
    bool RaiseLimits(std::size_t pages, std::size_t bytes, std::size_t totalWork) noexcept
    {
        if (!projection_ || GetCurrentThreadId() != projection_->ownerThread_ || pages < pageLimit_ || bytes < byteLimit_ || totalWork < totalWorkLimit_) return false;
        pageLimit_ = pages; byteLimit_ = bytes; totalWorkLimit_ = totalWork; return true;
    }
    V4NativeStagedFamilyRecoveryStep Step(std::size_t stepBudget, std::size_t pinBudget, std::size_t retainedByteBudget) noexcept
    {
        V4NativeStagedFamilyRecoveryStep out;
        try
        {
            if (!projection_ || !projection_->store_ || !projection_->lookup_ || GetCurrentThreadId() != projection_->ownerThread_ ||
                phase_ == Phase::failed || phase_ == Phase::taken)
            { out.unavailable = true; out.reason = "v4_native_staged_recovery_unavailable"; return out; }
            if (projection_->nestedParentPins_ > pinBudget || projection_->nestedParentBytes_ > retainedByteBudget)
            { out.resourceUnavailable = true; out.reason = "v4_native_staged_recovery_ancestor_budget"; return out; }
            pinBudget -= projection_->nestedParentPins_; retainedByteBudget -= projection_->nestedParentBytes_;
            if (!pinBudget || projection_->entries_.size() > (pinBudget - 1) / 2 ||
                projection_->retainedBytes_ > retainedByteBudget || retained_ > retainedByteBudget - projection_->retainedBytes_ ||
                entries_.size() > (pinBudget - 1 - projection_->entries_.size() * 2) / 2)
            { out.resourceUnavailable = true; out.reason = "v4_native_staged_recovery_retention_budget"; return out; }
            while (out.workUsed < stepBudget)
            {
                if (phase_ == Phase::scan)
                {
                    if (!scan_.next) { phase_ = Phase::failed; out.unavailable = true; out.reason = "v4_native_staged_recovery_journal_missing"; return out; }
                    if (Same(*scan_.next, projection_->final_))
                    {
                        if (entries_.empty()) { phase_ = Phase::failed; out.unavailable = true; out.reason = "v4_native_staged_recovery_empty"; return out; }
                        const auto journal = ReadV4ShardIndexPage(*projection_->store_, scan_, projection_->binding_, byteLimit_);
                        if (!journal.read) { out.resourceUnavailable = journal.resourceUnavailable; out.unavailable = !out.resourceUnavailable; out.reason = "v4_native_staged_recovery_journal_head_unavailable"; return out; }
                        phase_ = Phase::replay; ++out.workUsed; continue;
                    }
                    if (entries_.size() >= pageLimit_ || entries_.size() >= (pinBudget - 1 - projection_->entries_.size() * 2) / 2)
                    { out.resourceUnavailable = true; out.reason = "v4_native_staged_recovery_page_budget"; return out; }
                    const auto node = *scan_.next; auto nodePin = projection_->store_->PinRead(node.name, node.fileId);
                    if (!nodePin) { out.unavailable = true; out.reason = "v4_native_staged_recovery_index_pin"; return out; }
                    auto nodeRead = projection_->store_->Read(node.name, node.fileId, projection_->binding_, byteLimit_);
                    const auto index = ReadV4ShardIndexPage(*projection_->store_, scan_, projection_->binding_, byteLimit_);
                    if (nodeRead.status != CodecStatus::decoded || nodeRead.recoveredInFlight || !index.read ||
                        index.kind != "native_staged_family" || !index.data || !index.next.next)
                    { out.resourceUnavailable = nodeRead.status == CodecStatus::resource_unavailable || index.resourceUnavailable;
                      out.unavailable = !out.resourceUnavailable; out.reason = "v4_native_staged_recovery_index_unavailable"; return out; }
                    auto pagePin = projection_->store_->PinRead(index.data->name, index.data->fileId);
                    if (!pagePin) { out.unavailable = true; out.reason = "v4_native_staged_recovery_page_pin"; return out; }
                    auto page = ReadV4NativeStagedFamilyShard(*projection_->store_, index.data->name, index.data->fileId, projection_->binding_, byteLimit_);
                    if (!page.read || !page.audit)
                    { out.resourceUnavailable = page.resourceUnavailable; out.unavailable = !out.resourceUnavailable; out.reason = "v4_native_staged_recovery_page_unavailable"; return out; }
                    const auto& p = *page.audit; const auto& previous = *index.next.next;
                    if (p.journalHeadName != std::string(previous.name.begin(), previous.name.end()) ||
                        p.journalHeadVolume != std::to_string(previous.fileId.VolumeSerialNumber) || p.journalHeadFileId != V4ShardIdHex(previous.fileId))
                    { phase_ = Phase::failed; out.unavailable = true; out.reason = "v4_native_staged_recovery_link_mismatch"; return out; }
                    if (nodeRead.bytes.size() > retainedByteBudget - projection_->retainedBytes_ - retained_ ||
                        page.bytes.size() > retainedByteBudget - projection_->retainedBytes_ - retained_ - nodeRead.bytes.size())
                    { out.resourceUnavailable = true; out.reason = "v4_native_staged_recovery_byte_budget"; return out; }
                    auto next = index.next; const auto added = nodeRead.bytes.size() + page.bytes.size();
                    entries_.push_back({node, *index.data, std::move(nodePin), std::move(pagePin), std::move(nodeRead.bytes), std::move(page.bytes), std::move(*page.audit)});
                    scan_ = std::move(next); retained_ += added; ++out.workUsed; continue;
                }
                if (phase_ == Phase::replay)
                {
                    if (replayIndex_ == entries_.size()) { phase_ = Phase::ready; continue; }
                    auto& entry = entries_[entries_.size() - 1 - replayIndex_]; const auto& audit = entry.audit;
                    const auto& origin = projection_->entries_.back().audit;
                    if (audit.familyCoordinate != origin.familyCoordinate || (last_ && last_->projectionRequired_) ||
                        (replayIndex_ == 0 ? (audit.stage != "attach" || audit.workBudget != 1) :
                            (audit.stage != "graph" && audit.stage != "continue" && audit.stage != "next_candidate")))
                    { phase_ = Phase::failed; out.unavailable = true; out.reason = "v4_native_staged_recovery_stage_mismatch"; return out; }
                    if (audit.workBudget > totalWorkLimit_ - graphWork_)
                    { out.resourceUnavailable = true; out.reason = "v4_native_staged_recovery_work_budget"; return out; }
                    const auto proposal = replayIndex_ == 0 ?
                        event_graph::V4FamilyCandidateGraph::PrepareStagedNativeAttach(projection_->family_, origin.axisBudget, cursor_, *projection_->work_) :
                        audit.stage == "continue" ? event_graph::V4FamilyCandidateGraph::PrepareStagedNativeContinue(
                            projection_->family_, origin.axisBudget, cursor_, audit.workBudget) :
                        audit.stage == "next_candidate" ? event_graph::V4FamilyCandidateGraph::PrepareStagedNativeNextCandidate(
                            projection_->family_, origin.axisBudget, cursor_, audit.workBudget) :
                        event_graph::V4FamilyCandidateGraph::PrepareStagedNativeGraphs(projection_->family_, origin.axisBudget, cursor_, *projection_->work_, audit.workBudget);
                    if (proposal.status != event_graph::V4FamilyTraversalStatus::ready || !proposal.proposal || !proposal.errors.empty())
                    { out.resourceUnavailable = proposal.status == event_graph::V4FamilyTraversalStatus::resource_unavailable;
                      out.unavailable = !out.resourceUnavailable; out.reason = "v4_native_staged_recovery_proposal_unavailable"; return out; }
                    const auto encoded = event_graph::V4FamilyCandidateGraph::EncodeStagedNativePage(
                        projection_->family_, origin.axisBudget, *proposal.proposal, projection_->work_.get(), byteLimit_);
                    const auto& page = proposal.proposal->Page();
                    const auto before = event_graph::V4FamilyCandidateGraph::SerializeAnyStagedCursor(
                        projection_->family_, origin.axisBudget, page.before, projection_->work_.get());
                    const auto after = event_graph::V4FamilyCandidateGraph::SerializeAnyStagedCursor(
                        projection_->family_, origin.axisBudget, page.after, projection_->work_.get());
                    if (page.workUsed > audit.workBudget || !encoded.encoded || before.status != event_graph::V4FamilyTraversalStatus::ready || after.status != event_graph::V4FamilyTraversalStatus::ready ||
                        !before.errors.empty() || !after.errors.empty() || encoded.bytes != audit.proposalBytes || before.encoded != audit.beforeCursor || after.encoded != audit.afterCursor ||
                        (before.encoded == after.encoded && !page.workUsed && !page.record && !page.projectionRequired && page.candidatePruned.empty() && page.activePruned.empty() && page.presencePruned.empty()))
                    { out.resourceUnavailable = encoded.resourceUnavailable || before.status == event_graph::V4FamilyTraversalStatus::resource_unavailable || after.status == event_graph::V4FamilyTraversalStatus::resource_unavailable;
                      out.unavailable = !out.resourceUnavailable; out.reason = "v4_native_staged_recovery_proposal_mismatch"; return out; }
                    auto receipt = std::shared_ptr<V4NativeStagedFamilyReceipt>(new V4NativeStagedFamilyReceipt);
                    const auto prior = last_ ? std::shared_ptr<const V4NativeStagedFamilyReceipt>(last_) : projection_->nestedParent_;
                    if (prior && prior->depth_ == (std::numeric_limits<std::size_t>::max)())
                    { out.resourceUnavailable = true; out.reason = "v4_native_staged_recovery_depth_overflow"; return out; }
                    receipt->self_ = receipt; receipt->recovery_ = projection_; receipt->prior_ = prior; receipt->page_ = entry.page; receipt->head_ = entry.node;
                    receipt->pageBytes_ = entry.pageBytes; receipt->headBytes_ = entry.nodeBytes; receipt->pageBudget_ = byteLimit_; receipt->headBudget_ = byteLimit_;
                    receipt->cursor_ = page.after; receipt->record_ = page.record; receipt->depth_ = prior ? prior->depth_ + 1 : 1;
                    receipt->candidatePruned_ = page.candidatePruned; receipt->presencePruned_ = page.presencePruned;
                    receipt->activePruned_ = page.activePruned;
                    receipt->projectionRequired_ = page.projectionRequired; receipt->pendingPresent_ = page.pendingPresent; receipt->pendingOrdinal_ = page.pendingOrdinal;
                    if (page.projectionRequired)
                    {
                        const auto advance = EncodeV4ProjectionAdvance(projection_->family_, origin.axisBudget, page, byteLimit_);
                        if (!advance.encoded) { out.resourceUnavailable = advance.resourceUnavailable; out.unavailable = !out.resourceUnavailable; out.reason = "v4_native_staged_next_leaf_unavailable"; return out; }
                        receipt->pendingBeforeCursor_ = page.before; receipt->pendingAdvanceBytes_ = advance.bytes; receipt->pendingAdvanceWorkBudget_ = audit.workBudget;
                    }
                    const auto priorBytes = prior ? prior->retainedBytes_ : std::size_t{0}; const auto maximum = (std::numeric_limits<std::size_t>::max)();
                    if (entry.pageBytes.size() > maximum - priorBytes || entry.nodeBytes.size() > maximum - priorBytes - entry.pageBytes.size())
                    { out.resourceUnavailable = true; out.reason = "v4_native_staged_recovery_retained_overflow"; return out; }
                    receipt->retainedBytes_ = priorBytes + entry.pageBytes.size() + entry.nodeBytes.size();
                    auto nextCursor = page.after;
                    receipt->pagePin_ = std::move(entry.pagePin); receipt->headPin_ = std::move(entry.nodePin);
                    cursor_ = std::move(nextCursor); last_ = std::move(receipt); graphWork_ += page.workUsed; ++replayIndex_; ++out.workUsed; continue;
                }
                if (phase_ == Phase::ready) { out.ready = true; out.reason = "v4_native_staged_recovery_verify_pending"; return out; }
            }
            out.reason = "v4_native_staged_recovery_step_pending"; return out;
        }
        catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_native_staged_recovery_resource_unavailable"; return out; }
        catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_native_staged_recovery_resource_unavailable"; return out; }
        catch (...) { phase_ = Phase::failed; out.unavailable = true; out.reason = "v4_native_staged_recovery_unavailable"; return out; }
    }
    std::unique_ptr<V4NativeStagedFamilyPending> TakePending(const std::shared_ptr<V4ForegroundReservation>& reservation,
        bool& resourceUnavailable) noexcept
    {
        resourceUnavailable = false;
        try
        {
            if (!V4ForegroundReservation::Canonical(reservation) || phase_ != Phase::ready || !last_ || !projection_ ||
                !projection_->store_ || !reservation->Matches(*projection_->store_, final_, reservation->headBudget_) || GetCurrentThreadId() != projection_->ownerThread_ ||
                !last_->pagePin_ || !last_->headPin_ || !Same(last_->head_, final_)) return {};
            auto pending = std::unique_ptr<V4NativeStagedFamilyPending>(new V4NativeStagedFamilyPending);
            pending->recovery_ = projection_; pending->reservation_ = reservation; pending->existingHead_ = true;
            pending->prior_ = last_->prior_; pending->page_ = last_->page_; pending->head_ = last_->head_;
            pending->pageBytes_ = last_->pageBytes_; pending->headBytes_ = last_->headBytes_;
            pending->pageBudget_ = last_->pageBudget_; pending->headBudget_ = last_->headBudget_;
            pending->afterCursor_ = last_->cursor_; pending->record_ = last_->record_;
            pending->candidatePruned_ = last_->candidatePruned_; pending->presencePruned_ = last_->presencePruned_;
            pending->activePruned_ = last_->activePruned_;
            pending->projectionRequired_ = last_->projectionRequired_; pending->pendingPresent_ = last_->pendingPresent_; pending->pendingOrdinal_ = last_->pendingOrdinal_;
            pending->pendingBeforeCursor_ = last_->pendingBeforeCursor_; pending->pendingAdvanceBytes_ = last_->pendingAdvanceBytes_;
            pending->pendingAdvanceWorkBudget_ = last_->pendingAdvanceWorkBudget_;
            pending->pagePin_ = std::move(last_->pagePin_); pending->headPin_ = std::move(last_->headPin_);
            phase_ = Phase::taken; return pending;
        }
        catch (const std::bad_alloc&) { resourceUnavailable = true; return {}; }
        catch (const std::length_error&) { resourceUnavailable = true; return {}; }
        catch (...) { return {}; }
    }
};

// One complete, bounded family visit that emits one DFS case. All immutable
// files are written before the caller adopts the final index head. Intermediate
// files remain diagnostic side effects on failure, never a committed visit.
// A no-record step stores its complete before/after/work/prune audit in the
// family cursor shard and is replayed on every read. A single foreground owner
// must serialize head adoption.
class V4FamilyCasePending;
struct V4ShardFamilyCaseTransaction
{
    bool ready = false, postcheckRequired = false, resourceUnavailable = false;
    std::optional<V4ShardLocator> head, caseShard, familyShard;
    std::array<PublishedCheckpoint, 4> publications;
    std::unique_ptr<V4FamilyCasePending> pending;
    std::string_view reason;
};
class V4FamilyCasePending final
{
    friend V4ShardFamilyCaseTransaction PublishV4ShardFamilyCaseStep(CheckpointStore&, const V4ShardLocator&,
        std::string_view, std::string_view, const V4CoordinateBudget&, const V4AxisBudget&,
        std::size_t, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t,
        const V4ShardFamilyLookup*, std::size_t, std::size_t, const V4ShardFamilyRecheck*);
    V4ShardLocator head_;
    const V4ShardFamilyLookup* lookup_ = nullptr;
    std::weak_ptr<const unsigned char> identity_;
    std::uint64_t afterSerial_ = 0;
    bool consumed_ = false;
    V4FamilyCasePending(const V4ShardFamilyLookup& lookup, V4ShardLocator head)
        : head_(std::move(head)), lookup_(&lookup), identity_(lookup.identity_), afterSerial_(lookup.recheckSerial_) {}
public:
    V4FamilyCasePending(const V4FamilyCasePending&) = delete;
    V4FamilyCasePending& operator=(const V4FamilyCasePending&) = delete;
    std::optional<V4ShardLocator> Finalize(const V4ShardFamilyLookup& lookup, const V4ShardFamilyRecheck& postcheck)
    {
        const auto identity = identity_.lock();
        if (consumed_ || lookup_ != &lookup || !identity || !lookup.identity_ || identity.owner_before(lookup.identity_) ||
            lookup.identity_.owner_before(identity) || !postcheck.Matches(lookup) || postcheck.Serial() <= afterSerial_) return std::nullopt;
        auto result = head_; consumed_ = true; return result;
    }
};
inline V4ShardFamilyCaseTransaction PublishV4ShardFamilyCaseStep(
    CheckpointStore& store, const V4ShardLocator& trustedHead, std::string_view familyCoordinate,
    std::string_view binding, const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
    std::size_t tokenBudget, std::size_t workBudget, std::size_t lookupNodeBudget,
    std::size_t replayWorkBudget, std::size_t shardByteBudget, std::size_t indexByteBudget,
    const V4ShardFamilyLookup* completedLookup = nullptr, std::size_t lookupPinBudget = 0,
    std::size_t lookupSnapshotByteBudget = 0, const V4ShardFamilyRecheck* precheck = nullptr)
{
    V4ShardFamilyCaseTransaction out;
    try
    {
        (void)lookupNodeBudget; (void)lookupPinBudget; (void)lookupSnapshotByteBudget;
        if (!workBudget || replayWorkBudget < workBudget || !V4CanonicalDecimal(familyCoordinate))
        { out.resourceUnavailable = replayWorkBudget < workBudget || !workBudget;
          out.reason = "v4_family_transaction_budget_or_coordinate_unavailable"; return out; }
        if (!completedLookup) { out.reason = "v4_family_transaction_completed_lookup_required"; return out; }
        if (!completedLookup->Authorizes(store, trustedHead, binding, familyCoordinate) || !completedLookup->Matched() ||
            !precheck || !precheck->Matches(*completedLookup))
        { out.reason = "v4_family_transaction_lookup_head_mismatch"; return out; }
        const auto& matched = *completedLookup->Matched();
        const auto family = ReadV4FamilyCursorShard(store, matched.name, matched.fileId,
            binding, coordinateBudget, axisBudget, shardByteBudget, replayWorkBudget);
        if (!family.cursorValidated || !family.family || family.family->coordinate != familyCoordinate || family.family->scanExhausted)
        { out.resourceUnavailable = family.resourceUnavailable; out.reason = "v4_family_transaction_cursor_unavailable"; return out; }
        State base; base.binding = std::string(binding); base.familyVersion = FamilyVersion::v4;
        base.v4TypedEnabled = true; base.families.push_back(*family.family);
        base.families.front().pruneSteps.clear();
        const auto visit = PrepareFamilyVisit(base, binding, coordinateBudget.decimalDigits, axisBudget.logicalMembers,
            workBudget, &coordinateBudget, &axisBudget, std::size_t{0}, shardByteBudget);
        if (visit.tick.kind != TickKind::visit || !visit.errors.empty() ||
            visit.tick.proposed.families.size() != 1 || visit.tick.proposed.families[0].cases.size() > 1)
        { out.resourceUnavailable = visit.tick.proposed.families.size() == 1 &&
              visit.tick.proposed.families[0].state == WorkState::resource_unavailable;
          out.reason = "v4_family_transaction_visit_unavailable"; return out; }
        if (!visit.v4UnexecutedRecord && visit.tick.proposed.families[0].cases.empty())
        {
            const auto decoded = NextV4FamilyCoordinate(family.family->coordinate, coordinateBudget);
            if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family)
            { out.resourceUnavailable = decoded.status == V4FamilyCoordinateStatus::resource_unavailable;
              out.reason = "v4_family_transaction_step_family_unavailable"; return out; }
            const auto before = event_graph::V4FamilyCandidateGraph::ParseCursor(*decoded.family, axisBudget, family.family->candidateCursor);
            if (before.status != event_graph::V4FamilyTraversalStatus::ready || !before.errors.empty())
            { out.resourceUnavailable = before.status == event_graph::V4FamilyTraversalStatus::resource_unavailable;
              out.reason = "v4_family_transaction_step_before_unavailable"; return out; }
            const auto step = event_graph::V4FamilyCandidateGraph::Step(*decoded.family, axisBudget, before.cursor, workBudget);
            if (step.status != event_graph::V4FamilyTraversalStatus::ready || !step.errors.empty() || step.record || !step.workUsed)
            { out.resourceUnavailable = step.status == event_graph::V4FamilyTraversalStatus::resource_unavailable;
              out.reason = "v4_family_transaction_step_unavailable"; return out; }
            const auto after = event_graph::V4FamilyCandidateGraph::SerializeCursor(*decoded.family, axisBudget, step.cursor);
            if (after.status != event_graph::V4FamilyTraversalStatus::ready || !after.errors.empty() ||
                after.encoded != visit.tick.proposed.families[0].candidateCursor ||
                step.exhausted != visit.tick.proposed.families[0].scanExhausted)
            { out.resourceUnavailable = after.status == event_graph::V4FamilyTraversalStatus::resource_unavailable;
              out.reason = "v4_family_transaction_step_after_unavailable"; return out; }
            PruneAuditStep audit; audit.beforeCursor = family.family->candidateCursor; audit.afterCursor = after.encoded;
            audit.workUsed = step.workUsed;
            for (const auto& p : step.presencePruned) audit.presence.push_back({p.candidateOrdinal, p.candidateId, p.prune});
            for (const auto& p : step.activePruned) audit.active.push_back({p.candidateOrdinal, p.candidateId, p.prune});
            auto next = PublishV4FamilyCursorShard(store, visit.tick.proposed.families[0], binding,
                coordinateBudget, axisBudget, shardByteBudget, &audit, replayWorkBudget);
            out.publications[2] = std::move(next.publication);
            if (!next.published)
            { out.resourceUnavailable = next.resourceUnavailable; out.reason = "v4_family_transaction_step_shard_unavailable"; return out; }
            out.familyShard = V4ShardLocator{out.publications[2].fileName, out.publications[2].fileId};
            auto index = AppendV4ShardIndex(store, trustedHead, *out.familyShard, "family", binding,
                coordinateBudget, axisBudget, tokenBudget, shardByteBudget, shardByteBudget,
                replayWorkBudget, shardByteBudget, indexByteBudget);
            out.publications[3] = std::move(index.publication);
            if (!index.published || !index.head)
            { out.resourceUnavailable = index.resourceUnavailable; out.reason = "v4_family_transaction_step_index_unavailable"; return out; }
            out.pending.reset(new V4FamilyCasePending(*completedLookup, *index.head)); out.head = std::move(index.head);
            out.postcheckRequired = true; out.reason = "v4_family_transaction_step_postcheck_pending"; return out;
        }
        if (!visit.v4UnexecutedRecord || visit.tick.proposed.families[0].cases.size() != 1)
        { out.reason = "v4_family_transaction_case_shape_unavailable"; return out; }
        auto caseResult = PublishV4CaseShard(store, visit.tick.proposed, 0, 0, binding,
            coordinateBudget, axisBudget, replayWorkBudget, shardByteBudget);
        out.publications[0] = std::move(caseResult.publication);
        if (!caseResult.published)
        { out.resourceUnavailable = caseResult.resourceUnavailable; out.reason = "v4_family_transaction_case_unavailable"; return out; }
        out.caseShard = V4ShardLocator{out.publications[0].fileName, out.publications[0].fileId};
        auto caseIndex = AppendV4ShardIndex(store, trustedHead, *out.caseShard, "case", binding,
            coordinateBudget, axisBudget, tokenBudget, shardByteBudget, shardByteBudget,
            replayWorkBudget, shardByteBudget, indexByteBudget);
        out.publications[1] = std::move(caseIndex.publication);
        if (!caseIndex.published || !caseIndex.head)
        { out.resourceUnavailable = caseIndex.resourceUnavailable; out.reason = "v4_family_transaction_case_index_unavailable"; return out; }
        auto familyResult = PublishV4FamilyCursorShard(store, visit.tick.proposed.families[0], binding,
            coordinateBudget, axisBudget, shardByteBudget);
        out.publications[2] = std::move(familyResult.publication);
        if (!familyResult.published)
        { out.resourceUnavailable = familyResult.resourceUnavailable; out.reason = "v4_family_transaction_next_cursor_unavailable"; return out; }
        out.familyShard = V4ShardLocator{out.publications[2].fileName, out.publications[2].fileId};
        auto familyIndex = AppendV4ShardIndex(store, *caseIndex.head, *out.familyShard, "family", binding,
            coordinateBudget, axisBudget, tokenBudget, shardByteBudget, shardByteBudget,
            replayWorkBudget, shardByteBudget, indexByteBudget);
        out.publications[3] = std::move(familyIndex.publication);
        if (!familyIndex.published || !familyIndex.head)
        { out.resourceUnavailable = familyIndex.resourceUnavailable; out.reason = "v4_family_transaction_final_index_unavailable"; return out; }
        out.pending.reset(new V4FamilyCasePending(*completedLookup, *familyIndex.head)); out.head = std::move(familyIndex.head);
        out.postcheckRequired = true; out.reason = "v4_family_transaction_postcheck_pending"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_family_transaction_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_family_transaction_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_family_transaction_observation_unavailable"; return out; }
}

enum class V4TypedPreviewStatus { progress, item, exhausted, resource_unavailable, unavailable };
struct V4TypedPreviewItem
{
    std::string taskId, nextOrdinal;
    V4StructuredDfsCursor cursor;
    bool fromDeferred = false;
};
struct V4TypedPreviewStep
{
    V4TypedPreviewStatus status = V4TypedPreviewStatus::unavailable;
    std::optional<V4TypedPreviewItem> item;
    std::size_t workUsed = 0;
    std::string_view reason;
};
struct V4PinnedSnapshotCheck { bool matched = false, resourceUnavailable = false; };
struct V4TypedRoundPublication
{
    bool published = false, postcheckRequired = false, resourceUnavailable = false;
    std::optional<V4ShardLocator> head;
    std::array<PublishedCheckpoint, 2> publications;
    V4TypedPreviewStep preview;
    std::string_view reason;
};
class V4RoundAncestryRecheck;
class V4RoundAncestryCheck final
{
    friend class V4RoundAncestryRecheck;
    friend class V4TypedShardPreviewRound;
    struct Snapshot { V4ShardLocator locator; std::unique_ptr<V4CheckpointReadPin> pin; std::string bytes; };
    V4ShardLocator head_, snapshot_;
    V4ShardIndexCursor cursor_;
    std::vector<Snapshot> snapshots_;
    std::size_t bytes_ = 0, verifyIndex_ = 0;
    CheckpointStore* store_ = nullptr;
    std::string binding_;
    std::shared_ptr<const unsigned char> identity_ = std::make_shared<const unsigned char>(0);
    mutable std::uint64_t recheckSerial_ = 0;
    bool verifying_ = false, done_ = false, failed_ = false;
    V4RoundAncestryCheck(V4ShardLocator head, V4ShardLocator snapshot)
        : head_(head), snapshot_(std::move(snapshot)), cursor_{std::move(head), {}} {}
    static bool Same(const V4ShardLocator& a, const V4ShardLocator& b) noexcept
    { return a.name == b.name && a.fileId.VolumeSerialNumber == b.fileId.VolumeSerialNumber &&
        std::memcmp(a.fileId.FileId.Identifier, b.fileId.FileId.Identifier, sizeof(a.fileId.FileId.Identifier)) == 0; }
public:
    V4RoundAncestryCheck(const V4RoundAncestryCheck&) = delete;
    V4RoundAncestryCheck& operator=(const V4RoundAncestryCheck&) = delete;
    static std::unique_ptr<V4RoundAncestryCheck> Begin(V4ShardLocator head, V4ShardLocator snapshot)
    { return std::unique_ptr<V4RoundAncestryCheck>(new V4RoundAncestryCheck(std::move(head), std::move(snapshot))); }
    bool Matches(CheckpointStore& store, const V4ShardLocator& head, const V4ShardLocator& snapshot,
        std::string_view binding) const noexcept
    { return done_ && !failed_ && store_ == &store && binding_ == binding && Same(head_, head) && Same(snapshot_, snapshot); }
    V4ShardLookupStep Step(CheckpointStore& store, std::string_view binding, std::size_t byteBudget,
        std::size_t workBudget, std::size_t pinBudget, std::size_t snapshotByteBudget)
    {
        V4ShardLookupStep out;
        try
        {
            if (done_ || failed_ || (store_ && (store_ != &store || binding_ != binding)))
            { out.reason = "v4_ancestry_check_unavailable"; return out; }
            if (!store_) { binding_ = std::string(binding); store_ = &store; }
            if (snapshots_.size() > pinBudget || bytes_ > snapshotByteBudget)
            { out.status = V4ShardLookupStatus::resource_unavailable; return out; }
            while (out.workUsed < workBudget)
            {
                if (verifying_)
                {
                    if (verifyIndex_ == snapshots_.size())
                    { done_ = true; out.status = V4ShardLookupStatus::found; out.reason = "v4_ancestry_preview_matched"; return out; }
                    const auto& entry = snapshots_[verifyIndex_]; const auto loaded = store.Read(entry.locator.name, entry.locator.fileId, binding, byteBudget);
                    if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight || loaded.bytes != entry.bytes)
                    { out.status = loaded.status == CodecStatus::resource_unavailable ? V4ShardLookupStatus::resource_unavailable : V4ShardLookupStatus::unavailable;
                      if (out.status == V4ShardLookupStatus::unavailable) failed_ = true; return out; }
                    ++verifyIndex_; ++out.workUsed; continue;
                }
                if (!cursor_.next) { failed_ = true; out.reason = "v4_ancestry_snapshot_not_ancestor"; return out; }
                if (snapshots_.size() >= pinBudget) { out.status = V4ShardLookupStatus::resource_unavailable; return out; }
                const auto locator = *cursor_.next; auto pin = store.PinRead(locator.name, locator.fileId);
                if (!pin) { out.reason = "v4_ancestry_pin_unavailable"; return out; }
                auto loaded = store.Read(locator.name, locator.fileId, binding, byteBudget);
                if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight)
                { out.status = loaded.status == CodecStatus::resource_unavailable ? V4ShardLookupStatus::resource_unavailable : V4ShardLookupStatus::unavailable; return out; }
                if (bytes_ > snapshotByteBudget || loaded.bytes.size() > snapshotByteBudget - bytes_)
                { out.status = V4ShardLookupStatus::resource_unavailable; return out; }
                const auto page = ReadV4ShardIndexPage(store, cursor_, binding, byteBudget);
                if (!page.read) { out.status = page.resourceUnavailable ? V4ShardLookupStatus::resource_unavailable : V4ShardLookupStatus::unavailable; return out; }
                auto next = page.next;
                const auto n = loaded.bytes.size(); snapshots_.push_back({locator, std::move(pin), std::move(loaded.bytes)}); bytes_ += n;
                static_assert(std::is_nothrow_move_assignable_v<V4ShardIndexCursor>);
                cursor_ = std::move(next); ++out.workUsed;
                if (Same(locator, snapshot_)) { verifying_ = true; verifyIndex_ = 0; }
            }
            out.status = V4ShardLookupStatus::progress; out.reason = "v4_ancestry_check_pending"; return out;
        }
        catch (const std::bad_alloc&) { out.status = V4ShardLookupStatus::resource_unavailable; return out; }
        catch (const std::length_error&) { out.status = V4ShardLookupStatus::resource_unavailable; return out; }
        catch (...) { failed_ = true; out = {}; out.reason = "v4_ancestry_check_unavailable"; return out; }
    }
};
class V4RoundAncestryRecheck final
{
    const V4RoundAncestryCheck* check_ = nullptr;
    std::weak_ptr<const unsigned char> identity_;
    std::uint64_t serial_ = 0;
    std::size_t next_ = 0;
    bool complete_ = false, failed_ = false;
    V4RoundAncestryRecheck(const V4RoundAncestryCheck& check, std::uint64_t serial) noexcept
        : check_(&check), identity_(check.identity_), serial_(serial) {}
public:
    V4RoundAncestryRecheck(const V4RoundAncestryRecheck&) = delete;
    V4RoundAncestryRecheck& operator=(const V4RoundAncestryRecheck&) = delete;
    static std::unique_ptr<V4RoundAncestryRecheck> Begin(const V4RoundAncestryCheck& check)
    {
        if (!check.done_ || check.failed_ || !check.store_ || check.snapshots_.empty() ||
            check.recheckSerial_ == (std::numeric_limits<std::uint64_t>::max)()) return {};
        return std::unique_ptr<V4RoundAncestryRecheck>(new V4RoundAncestryRecheck(check, ++check.recheckSerial_));
    }
    V4ShardLookupStep Step(std::size_t fileBudget) noexcept
    {
        V4ShardLookupStep out; const auto identity = identity_.lock();
        if (!identity || !check_ || failed_ || complete_) { out.reason = "v4_ancestry_recheck_unavailable"; return out; }
        while (next_ < check_->snapshots_.size() && out.workUsed < fileBudget)
        {
            const auto& entry = check_->snapshots_[next_];
            const auto loaded = check_->store_->Read(entry.locator.name, entry.locator.fileId, check_->binding_, entry.bytes.size());
            if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight || loaded.bytes != entry.bytes)
            { out.status = loaded.status == CodecStatus::resource_unavailable ? V4ShardLookupStatus::resource_unavailable : V4ShardLookupStatus::unavailable;
              if (out.status == V4ShardLookupStatus::unavailable) failed_ = true; return out; }
            ++next_; ++out.workUsed;
        }
        complete_ = next_ == check_->snapshots_.size(); out.status = complete_ ? V4ShardLookupStatus::found : V4ShardLookupStatus::progress;
        out.reason = complete_ ? "v4_ancestry_recheck_complete_not_authenticated" : "v4_ancestry_recheck_pending"; return out;
    }
    bool Matches(const V4RoundAncestryCheck& check) const noexcept
    { const auto identity = identity_.lock(); return complete_ && !failed_ && check_ == &check && identity && check.identity_ &&
        !identity.owner_before(check.identity_) && !check.identity_.owner_before(identity) && check.done_ && !check.failed_; }
    std::uint64_t Serial() const noexcept { return serial_; }
};
struct V4TypedSelectedPublication
{
    bool published = false, postcheckRequired = false, resourceUnavailable = false, familyRegistered = false;
    bool mainGenesisRegistered = false;
    std::optional<V4ShardLocator> head;
    std::array<PublishedCheckpoint, 6> publications;
    std::optional<V4TypedStepAudit> audit;
    std::string_view reason;
};
// Fixed-head, process-local preview only. Pins and exact first-read bytes live
// for the whole round; a mismatch or exhausted pin/cache budget stops it.
// The direct predecessor is replayed, but the head and complete ancestry are
// not authenticated and the result is never an execution authorization.
class V4TypedShardPreviewRound final
{
    struct Snapshot { V4ShardLocator locator; std::unique_ptr<V4CheckpointReadPin> pin; std::string bytes; };
    struct Position
    {
        V4ShardIndexCursor outer, probe, older;
        std::optional<V4ShardLocator> node;
        std::optional<V4TypedStepAudit> audit;
        bool mainGenesis = false;
        unsigned branch = 0, phase = 0; // branch self/deferred; phase select/newer/older/yield/genesis-scan
        std::size_t checkIndex = 0;
        bool ending = false, done = false;
    };
    V4ShardLocator head_;
    Position position_;
    std::vector<Snapshot> snapshots_;
    std::size_t snapshotBytes_ = 0;
    CheckpointStore* store_ = nullptr;
    std::string binding_;
    std::optional<Position> pending_;
    std::optional<V4ShardLocator> pendingHead_;
    std::optional<V4ShardLocator> pendingPreviousHead_;
    std::optional<V4TypedPreviewItem> pendingItem_;
    bool pendingItemProcessed_ = false;
    const V4RoundAncestryCheck* pendingAncestry_ = nullptr;
    std::weak_ptr<const unsigned char> pendingAncestryIdentity_;
    std::uint64_t pendingAncestryAfterSerial_ = 0;
    bool pendingAncestryChecked_ = false;
    std::optional<V4ShardLocator> selectedStagedHead_;
    const V4ShardFamilyLookup* selectedLookup_ = nullptr;
    std::weak_ptr<const unsigned char> selectedLookupIdentity_;
    std::uint64_t selectedAfterSerial_ = 0;
    const V4ShardFamilyLookup* selectedMainLookup_ = nullptr;
    std::weak_ptr<const unsigned char> selectedMainIdentity_;
    std::uint64_t selectedMainAfterSerial_ = 0;
    explicit V4TypedShardPreviewRound(V4ShardLocator head) : head_(std::move(head)) { position_.outer.next = head_; }
    static bool Same(const V4ShardLocator& a, const V4ShardLocator& b) noexcept
    { return a.name == b.name && a.fileId.VolumeSerialNumber == b.fileId.VolumeSerialNumber &&
        std::memcmp(a.fileId.FileId.Identifier, b.fileId.FileId.Identifier, sizeof(a.fileId.FileId.Identifier)) == 0; }
    static std::string EncodePosition(const Position& p)
    {
        std::string result;
        const auto put = [&](std::string_view value) { AppendCheckpointToken(result, value); };
        const auto locator = [&](const std::optional<V4ShardLocator>& value) {
            put(value ? "1" : "0"); if (!value) return;
            put(std::string(value->name.begin(), value->name.end()));
            put(std::to_string(value->fileId.VolumeSerialNumber)); put(V4ShardIdHex(value->fileId));
        };
        const auto cursor = [&](const V4ShardIndexCursor& value) { locator(value.next); put(value.expectedGeneration); };
        put("v4_preview_round_position_v2"); cursor(p.outer); cursor(p.probe); cursor(p.older); locator(p.node);
        put(std::to_string(p.branch)); put(std::to_string(p.phase)); put(std::to_string(p.checkIndex));
        put(p.ending ? "1" : "0"); put(p.done ? "1" : "0"); put(p.mainGenesis ? "1" : "0");
        put(p.audit ? "1" : "0");
        if (p.audit) { put(p.audit->taskId); put(p.audit->stepOrdinal); put(p.audit->deferredTaskId); }
        return result;
    }
public:
    V4TypedShardPreviewRound(const V4TypedShardPreviewRound&) = delete;
    V4TypedShardPreviewRound& operator=(const V4TypedShardPreviewRound&) = delete;
    static std::unique_ptr<V4TypedShardPreviewRound> Begin(V4ShardLocator trustedHead)
    { return std::unique_ptr<V4TypedShardPreviewRound>(new V4TypedShardPreviewRound(std::move(trustedHead))); }
    bool AdoptPublished(const V4ShardLocator& adoptedHead) noexcept
    {
        if (!pending_ || !pendingHead_ || !pendingAncestryChecked_ || !Same(*pendingHead_, adoptedHead) || (pendingItem_ && !pendingItemProcessed_)) return false;
        position_ = std::move(*pending_); pending_.reset(); pendingHead_.reset(); pendingPreviousHead_.reset();
        pendingItem_.reset(); pendingItemProcessed_ = false; selectedStagedHead_.reset(); selectedLookup_ = nullptr;
        selectedLookupIdentity_.reset(); selectedAfterSerial_ = 0; pendingAncestry_ = nullptr;
        selectedMainLookup_ = nullptr; selectedMainIdentity_.reset(); selectedMainAfterSerial_ = 0;
        pendingAncestryIdentity_.reset(); pendingAncestryAfterSerial_ = 0; pendingAncestryChecked_ = false; return true;
    }
    // The caller must supply its still-current pre-publication head; orphaned
    // files are retained and this method makes no statement about them.
    bool DiscardUnadopted(const V4ShardLocator& currentHead) noexcept
    {
        if (!pending_ || !pendingPreviousHead_ || !Same(*pendingPreviousHead_, currentHead)) return false;
        pending_.reset(); pendingHead_.reset(); pendingPreviousHead_.reset(); pendingItem_.reset(); pendingItemProcessed_ = false;
        selectedStagedHead_.reset(); selectedLookup_ = nullptr; selectedLookupIdentity_.reset(); selectedAfterSerial_ = 0;
        selectedMainLookup_ = nullptr; selectedMainIdentity_.reset(); selectedMainAfterSerial_ = 0;
        pendingAncestry_ = nullptr; pendingAncestryIdentity_.reset(); pendingAncestryAfterSerial_ = 0; pendingAncestryChecked_ = false; return true;
    }
    V4StructuredDfsStep InspectPendingTypedStep(const V4CoordinateBudget& budget,
        std::size_t tokenBudget, std::size_t workBudget) const
    {
        if (!pending_ || !pendingItem_ || pendingItemProcessed_) return {};
        return StepV4StructuredDfs(pendingItem_->cursor, budget, tokenBudget, workBudget);
    }
    std::string PositionForAudit() const { return EncodePosition(position_); }
    std::size_t PinnedSnapshotCount() const noexcept { return snapshots_.size(); }
    V4PinnedSnapshotCheck RevalidatePinnedAt(CheckpointStore& store, std::string_view binding,
        std::size_t index, std::size_t byteBudget) const noexcept
    {
        if (store_ != &store || binding_ != binding || index >= snapshots_.size()) return {};
        const auto& entry = snapshots_[index];
        const auto loaded = store.Read(entry.locator.name, entry.locator.fileId, binding, byteBudget);
        return {loaded.status == CodecStatus::decoded && !loaded.recoveredInFlight && loaded.bytes == entry.bytes,
            loaded.status == CodecStatus::resource_unavailable};
    }
    V4TypedPreviewStep Step(CheckpointStore& store, std::string_view binding, const V4CoordinateBudget& budget,
        std::size_t tokenBudget, std::size_t replayWorkBudget, std::size_t byteBudget,
        std::size_t nodeBudget, std::size_t pinBudget, std::size_t snapshotByteBudget)
    {
        V4TypedPreviewStep out;
        try
        {
            if (pending_) { out.reason = "v4_typed_round_publication_not_adopted"; return out; }
            if (store_ && (store_ != &store || binding_ != binding))
            { out.reason = "v4_typed_round_owner_mismatch"; return out; }
            if (!store_) { binding_ = std::string(binding); store_ = &store; }
            if (snapshots_.size() > pinBudget || snapshotBytes_ > snapshotByteBudget)
            { out.status = V4TypedPreviewStatus::resource_unavailable; out.reason = "v4_typed_round_existing_snapshot_budget_unavailable"; return out; }
            auto current = position_;
            bool resource = false;
            const auto snapshot = [&](const V4ShardLocator& locator) {
                auto found = std::find_if(snapshots_.begin(), snapshots_.end(), [&](const auto& x) { return Same(x.locator, locator); });
                if (found == snapshots_.end())
                {
                    if (snapshots_.size() >= pinBudget) { resource = true; return false; }
                    auto pin = store.PinRead(locator.name, locator.fileId); if (!pin) return false;
                    auto loaded = store.Read(locator.name, locator.fileId, binding, byteBudget);
                    if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight)
                    { resource = loaded.status == CodecStatus::resource_unavailable; return false; }
                    if (snapshotBytes_ > snapshotByteBudget || loaded.bytes.size() > snapshotByteBudget - snapshotBytes_)
                    { resource = true; return false; }
                    const auto n = loaded.bytes.size();
                    snapshots_.push_back({locator, std::move(pin), std::move(loaded.bytes)}); snapshotBytes_ += n;
                    return true;
                }
                const auto loaded = store.Read(locator.name, locator.fileId, binding, byteBudget);
                if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight)
                { resource = loaded.status == CodecStatus::resource_unavailable; return false; }
                return loaded.bytes == found->bytes;
            };
            const auto checkedPage = [&](const V4ShardIndexCursor& cursor, V4ShardIndexPage& page) {
                if (!cursor.next) { page.exhausted = true; return true; }
                if (!snapshot(*cursor.next)) return false;
                page = ReadV4ShardIndexPage(store, cursor, binding, byteBudget);
                if (!page.read) { resource = page.resourceUnavailable; return false; }
                return true;
            };
            const auto checkedAudit = [&](const V4ShardLocator& locator, V4TypedStepAudit& audit) {
                if (!snapshot(locator)) return false;
                auto checked = ReadV4TypedStepShard(store, locator.name, locator.fileId, binding,
                    budget, tokenBudget, replayWorkBudget, byteBudget);
                if (!checked.replayed || !checked.audit) { resource = checked.resourceUnavailable; return false; }
                audit = std::move(*checked.audit); return true;
            };
            const auto checkedMainGenesis = [&](const V4ShardLocator& locator, V4StructuredDfsCursor& cursor) {
                if (!snapshot(locator)) return false;
                const auto checked = ReadV4TypedCursorShard(store, locator.name, locator.fileId, binding, budget, tokenBudget, byteBudget);
                if (!checked.cursorValidated || !checked.cursor) { resource = checked.resourceUnavailable; return false; }
                if (checked.deferred || checked.cursor->stopDepth || !checked.cursor->prefix.empty() ||
                    checked.cursor->nextChoice != std::vector<std::uint32_t>{0} || checked.cursor->exhausted) return false;
                cursor = *checked.cursor; return true;
            };
            const auto fail = [&] {
                out.status = resource ? V4TypedPreviewStatus::resource_unavailable : V4TypedPreviewStatus::unavailable;
                out.workUsed = 0; out.reason = "v4_typed_round_snapshot_or_replay_unavailable"; return out;
            };
            while (out.workUsed < nodeBudget && !current.done)
            {
                if (current.ending)
                {
                    if (current.checkIndex < snapshots_.size())
                    { if (!snapshot(snapshots_[current.checkIndex].locator)) return fail(); ++current.checkIndex; ++out.workUsed; continue; }
                    current.done = true; break;
                }
                if (!current.audit)
                {
                    if (!current.outer.next) { current.ending = true; current.checkIndex = 0; continue; }
                    const auto node = *current.outer.next; V4ShardIndexPage page;
                    if (!checkedPage(current.outer, page)) return fail(); ++out.workUsed;
                    current.outer = page.next;
                    if (page.kind != "typed_step" && page.kind != "global_cursor") continue;
                    V4TypedStepAudit audit;
                    current.mainGenesis = page.kind == "global_cursor";
                    if (current.mainGenesis)
                    {
                        V4StructuredDfsCursor root;
                        if (!page.data || !checkedMainGenesis(*page.data, root)) return fail();
                        audit.before = root; audit.after = root; audit.taskId = V4TypedMainTaskId(root.layer); audit.stepOrdinal = "0";
                    }
                    else if (!page.data || !checkedAudit(*page.data, audit)) return fail();
                    current.node = node; current.audit = std::move(audit); current.branch = 0; current.phase = 0;
                }
                const auto& audit = *current.audit;
                if (current.branch >= 2) { current.audit.reset(); current.node.reset(); current.mainGenesis = false; continue; }
                if (current.branch == 1 && !audit.deferred)
                { current.audit.reset(); current.node.reset(); continue; }
                const auto& candidateId = current.branch == 0 ? audit.taskId : audit.deferredTaskId;
                const auto& candidateCursor = current.branch == 0 ? audit.after : *audit.deferred;
                if (current.phase == 0) { current.probe = {head_, {}}; current.phase = 1; }
                if (current.phase == 1)
                {
                    if (!current.probe.next) return fail();
                    if (Same(*current.probe.next, *current.node))
                    {
                        if (current.branch == 1 || current.mainGenesis) { current.older = current.outer; current.phase = 4; }
                        else { current.older = current.outer; current.phase = 2; }
                        continue;
                    }
                    V4ShardIndexPage page; if (!checkedPage(current.probe, page)) return fail(); ++out.workUsed;
                    current.probe = page.next;
                    if (page.kind == "typed_step")
                    {
                        V4TypedStepAudit newer; if (!page.data || !checkedAudit(*page.data, newer)) return fail();
                        if (newer.deferredTaskId == candidateId) return fail(); // duplicate genesis
                        if (newer.taskId == candidateId) { ++current.branch; current.phase = 0; }
                    }
                    else if (page.kind == "global_cursor")
                    {
                        V4StructuredDfsCursor other; if (!page.data || !checkedMainGenesis(*page.data, other)) return fail();
                        if (V4TypedMainTaskId(other.layer) == candidateId) return fail();
                    }
                    continue;
                }
                if (current.phase == 2)
                {
                    if (audit.stepOrdinal == "0")
                    {
                        std::size_t layer = 0; bool main = false;
                        if (!V4TypedTaskIdCanonical(audit.taskId, layer, main)) return fail();
                        if (main)
                        {
                            if (audit.before.layer != layer || audit.before.stopDepth || !audit.before.prefix.empty() ||
                                audit.before.nextChoice != std::vector<std::uint32_t>{0} || audit.before.exhausted) return fail();
                        }
                    }
                    if (!current.older.next) return fail();
                    V4ShardIndexPage page; if (!checkedPage(current.older, page)) return fail(); ++out.workUsed;
                    current.older = page.next;
                    if (page.kind == "global_cursor" && audit.stepOrdinal == "0")
                    {
                        V4StructuredDfsCursor root; if (!page.data || !checkedMainGenesis(*page.data, root)) return fail();
                        if (V4TypedMainTaskId(root.layer) == audit.taskId)
                        { if (!V4SameTypedCursor(root, audit.before)) return fail(); current.phase = 3; }
                        continue;
                    }
                    if (page.kind != "typed_step") continue;
                    V4TypedStepAudit prior; if (!page.data || !checkedAudit(*page.data, prior)) return fail();
                    if (audit.stepOrdinal == "0" && prior.deferredTaskId == audit.taskId)
                    { if (!prior.deferred || !V4SameTypedCursor(*prior.deferred, audit.before)) return fail(); current.phase = 3; }
                    else if (audit.stepOrdinal != "0" && prior.taskId == audit.taskId)
                    { if (prior.stepOrdinal != Subtract(audit.stepOrdinal, "1") || prior.after.exhausted ||
                          !V4SameTypedCursor(prior.after, audit.before)) return fail(); current.phase = 3; }
                    continue;
                }
                if (current.phase == 4)
                {
                    if (!current.older.next) { current.phase = 3; continue; }
                    V4ShardIndexPage page; if (!checkedPage(current.older, page)) return fail(); ++out.workUsed;
                    current.older = page.next;
                    if (page.kind == "typed_step")
                    {
                        V4TypedStepAudit older; if (!page.data || !checkedAudit(*page.data, older)) return fail();
                        if (older.taskId == candidateId || older.deferredTaskId == candidateId) return fail();
                    }
                    else if (page.kind == "global_cursor")
                    {
                        V4StructuredDfsCursor older; if (!page.data || !checkedMainGenesis(*page.data, older)) return fail();
                        if (V4TypedMainTaskId(older.layer) == candidateId) return fail();
                    }
                    continue;
                }
                if (current.branch == 0 && candidateCursor.exhausted)
                { ++current.branch; current.phase = 0; continue; }
                if (current.checkIndex < snapshots_.size())
                { if (!snapshot(snapshots_[current.checkIndex].locator)) return fail(); ++current.checkIndex; ++out.workUsed; continue; }
                V4TypedPreviewItem item; item.taskId = candidateId; item.cursor = candidateCursor;
                item.fromDeferred = current.branch == 1;
                item.nextOrdinal = (item.fromDeferred || current.mainGenesis) ? "0" : Add(audit.stepOrdinal, "1");
                out.item = std::move(item);
                ++current.branch; current.phase = 0; current.checkIndex = 0; position_ = std::move(current);
                out.status = V4TypedPreviewStatus::item;
                out.reason = "v4_typed_round_item_unexecuted_history_unauthenticated"; return out;
            }
            position_ = std::move(current);
            out.status = position_.done ? V4TypedPreviewStatus::exhausted : V4TypedPreviewStatus::progress;
            out.reason = position_.done ? "v4_typed_round_snapshot_exhausted_not_coverage" : "v4_typed_round_progress"; return out;
        }
        catch (const std::bad_alloc&) { out = {}; out.status = V4TypedPreviewStatus::resource_unavailable; out.reason = "v4_typed_round_resource_unavailable"; return out; }
        catch (const std::length_error&) { out = {}; out.status = V4TypedPreviewStatus::resource_unavailable; out.reason = "v4_typed_round_resource_unavailable"; return out; }
        catch (...) { out = {}; out.reason = "v4_typed_round_unavailable"; return out; }
    }
    // A foreground caller adopts only the returned final head. On any failure
    // the in-memory position rolls back; previously created files remain as
    // explicit side-effect evidence. The record is untrusted until replayed.
    V4TypedRoundPublication PublishAdvance(CheckpointStore& store, const V4ShardLocator& currentHead,
        std::string_view binding, const V4CoordinateBudget& budget, const V4AxisBudget& axisBudget,
        std::size_t tokenBudget, std::size_t replayWorkBudget, std::size_t byteBudget,
        std::size_t nodeBudget, std::size_t pinBudget, std::size_t snapshotByteBudget,
        std::size_t indexByteBudget, const V4RoundAncestryCheck& ancestry,
        const V4RoundAncestryRecheck& precheck)
    {
        V4TypedRoundPublication out;
        try
        {
            if (!nodeBudget || position_.done || pending_)
            { out.resourceUnavailable = !nodeBudget; out.reason = "v4_round_publication_not_progressable"; return out; }
            if (!ancestry.Matches(store, currentHead, head_, binding) || !precheck.Matches(ancestry))
            { out.reason = "v4_round_publication_ancestry_or_precheck_unavailable"; return out; }
            Position before = position_;
            static_assert(std::is_nothrow_move_assignable_v<Position> && std::is_nothrow_move_constructible_v<Position> &&
                std::is_nothrow_move_constructible_v<V4ShardLocator>);
            struct Rollback final {
                Position& position; Position prior; bool accepted = false;
                ~Rollback() { if (!accepted) position = std::move(prior); }
            } rollback{position_, std::move(before)};
            const auto beforePosition = EncodePosition(rollback.prior);
            auto step = Step(store, binding, budget, tokenBudget, replayWorkBudget, byteBudget,
                nodeBudget, pinBudget, snapshotByteBudget);
            if (step.status != V4TypedPreviewStatus::progress && step.status != V4TypedPreviewStatus::item &&
                step.status != V4TypedPreviewStatus::exhausted)
            { out.resourceUnavailable = step.status == V4TypedPreviewStatus::resource_unavailable;
              out.reason = "v4_round_publication_step_unavailable"; return out; }
            V4RoundAdvanceAudit audit;
            audit.snapshotName.assign(head_.name.begin(), head_.name.end());
            audit.snapshotVolume = std::to_string(head_.fileId.VolumeSerialNumber); audit.snapshotFileId = V4ShardIdHex(head_.fileId);
            audit.beforePosition = beforePosition; audit.afterPosition = EncodePosition(position_);
            audit.status = step.status == V4TypedPreviewStatus::item ? "item" :
                step.status == V4TypedPreviewStatus::exhausted ? "exhausted" : "progress";
            if (step.item)
            { audit.taskId = step.item->taskId; audit.nextOrdinal = step.item->nextOrdinal;
              audit.itemCursor = step.item->cursor; audit.fromDeferred = step.item->fromDeferred; }
            audit.budget = budget; audit.tokenBudget = tokenBudget; audit.replayWorkBudget = replayWorkBudget;
            audit.byteBudget = byteBudget; audit.nodeBudget = nodeBudget; audit.pinBudget = pinBudget;
            audit.snapshotByteBudget = snapshotByteBudget;
            State state; state.binding = std::string(binding); state.familyVersion = FamilyVersion::v4;
            state.v4TypedEnabled = true; state.roundAdvance = std::move(audit);
            const auto expected = EncodeCheckpoint(state, binding, byteBudget);
            if (expected.status != CodecStatus::encoded)
            { out.resourceUnavailable = expected.status == CodecStatus::resource_unavailable; out.reason = "v4_round_publication_encode_unavailable"; return out; }
            out.publications[0] = store.Publish(state, binding, byteBudget);
            if (out.publications[0].status != StoreStatus::published_durability_unverified ||
                !out.publications[0].fileIdentityCaptured || !out.publications[0].renameReturnedSuccess)
            { out.reason = "v4_round_publication_file_unavailable"; return out; }
            const auto readback = store.Read(out.publications[0].fileName, out.publications[0].fileId, binding, byteBudget);
            if (readback.status != CodecStatus::decoded || readback.recoveredInFlight || readback.bytes != expected.bytes)
            { out.resourceUnavailable = readback.status == CodecStatus::resource_unavailable;
              out.reason = "v4_round_publication_readback_unavailable"; return out; }
            V4ShardLocator data{out.publications[0].fileName, out.publications[0].fileId};
            auto indexed = AppendV4ShardIndex(store, currentHead, data, "round_step", binding, budget, axisBudget,
                tokenBudget, byteBudget, byteBudget, replayWorkBudget, byteBudget, indexByteBudget);
            out.publications[1] = std::move(indexed.publication);
            if (!indexed.published || !indexed.head)
            { out.resourceUnavailable = indexed.resourceUnavailable; out.reason = "v4_round_publication_index_unavailable"; return out; }
            auto proposedHead = *indexed.head; auto proposedPrevious = currentHead; auto proposedItem = step.item;
            out.head = std::move(indexed.head); out.preview = std::move(step);
            out.reason = "v4_round_publication_head_unadopted_history_unauthenticated";
            pending_.emplace(std::move(position_)); pendingHead_.emplace(std::move(proposedHead));
            pendingPreviousHead_.emplace(std::move(proposedPrevious));
            pendingItem_ = std::move(proposedItem); pendingItemProcessed_ = false;
            pendingAncestry_ = &ancestry; pendingAncestryIdentity_ = ancestry.identity_;
            pendingAncestryAfterSerial_ = ancestry.recheckSerial_; pendingAncestryChecked_ = false;
            position_ = std::move(rollback.prior); rollback.accepted = true;
            out.postcheckRequired = true; out.reason = "v4_round_publication_postcheck_pending"; return out;
        }
        catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_round_publication_resource_unavailable"; return out; }
        catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_round_publication_resource_unavailable"; return out; }
        catch (...) { out.reason = "v4_round_publication_unavailable"; return out; }
    }
    bool FinalizeRoundAdvance(const V4ShardLocator& stagedHead, const V4RoundAncestryCheck& ancestry,
        const V4RoundAncestryRecheck& postcheck) noexcept
    {
        const auto identity = pendingAncestryIdentity_.lock();
        if (!pending_ || !pendingHead_ || !Same(*pendingHead_, stagedHead) || pendingAncestry_ != &ancestry ||
            !identity || !ancestry.identity_ || identity.owner_before(ancestry.identity_) || ancestry.identity_.owner_before(identity) ||
            !postcheck.Matches(ancestry) || postcheck.Serial() <= pendingAncestryAfterSerial_) return false;
        pendingAncestryChecked_ = true; return true;
    }
    // Complete the selected preview item in the same unadopted head chain.
    // An emitted family requires a completed lookup on that exact staged head;
    // absence is only local to this unauthenticated preview index.
    V4TypedSelectedPublication PublishSelectedTypedStep(CheckpointStore& store, const V4ShardLocator& stagedHead,
        std::string_view binding, const V4CoordinateBudget& budget, const V4AxisBudget& axisBudget,
        std::size_t tokenBudget, std::size_t workBudget, std::size_t replayWorkBudget,
        std::size_t byteBudget, std::size_t indexByteBudget, const V4ShardFamilyLookup* familyLookup = nullptr,
        const V4ShardFamilyRecheck* precheck = nullptr, const V4ShardFamilyLookup* mainLookup = nullptr,
        const V4ShardFamilyRecheck* mainPrecheck = nullptr)
    {
        V4TypedSelectedPublication out;
        try
        {
            if (!pending_ || !pendingHead_ || !pendingAncestryChecked_ || !pendingItem_ || pendingItemProcessed_ || selectedStagedHead_ || !Same(*pendingHead_, stagedHead) ||
                store_ != &store || binding_ != binding || !workBudget || replayWorkBudget < workBudget)
            { out.resourceUnavailable = !workBudget || replayWorkBudget < workBudget;
              out.reason = "v4_selected_step_staged_identity_unavailable"; return out; }
            const auto predicted = InspectPendingTypedStep(budget, tokenBudget, workBudget);
            if (predicted.status == V4StructuredDfsStatus::invalid || predicted.status == V4StructuredDfsStatus::resource_unavailable || !predicted.workUsed)
            { out.resourceUnavailable = predicted.status == V4StructuredDfsStatus::resource_unavailable;
              out.reason = "v4_selected_step_prediction_unavailable"; return out; }
            bool createFamily = false;
            std::size_t taskLayer = 0; bool mainTask = false;
            if (!V4TypedTaskIdCanonical(pendingItem_->taskId, taskLayer, mainTask))
            { out.reason = "v4_selected_step_task_identity_unavailable"; return out; }
            const bool createMain = mainTask && predicted.next.exhausted;
            std::size_t nextLayer = taskLayer;
            if (createMain)
            {
                if (taskLayer == (std::numeric_limits<std::size_t>::max)())
                { out.resourceUnavailable = true; out.reason = "v4_selected_step_next_layer_unavailable"; return out; }
                nextLayer = taskLayer + 1;
                const auto layer = std::to_string(nextLayer);
                if (!mainLookup || !mainPrecheck || !mainLookup->AuthorizesMainAbsent(store, stagedHead, binding, layer) ||
                    !mainPrecheck->Matches(*mainLookup))
                { out.reason = "v4_selected_step_main_genesis_lookup_unavailable"; return out; }
            }
            if (predicted.status == V4StructuredDfsStatus::emitted)
            {
                if (!predicted.family || predicted.coordinate.empty() || !familyLookup)
                { out.reason = "v4_selected_step_family_lookup_required"; return out; }
                const bool found = familyLookup->Authorizes(store, stagedHead, binding, predicted.coordinate);
                const bool absent = familyLookup->AuthorizesAbsent(store, stagedHead, binding, predicted.coordinate);
                if ((!found && !absent) || !precheck || !precheck->Matches(*familyLookup))
                { out.reason = "v4_selected_step_family_lookup_or_precheck_unavailable"; return out; }
                createFamily = absent;
            }
            auto step = PublishV4TypedStepShard(store, pendingItem_->cursor, binding, budget, tokenBudget,
                workBudget, byteBudget, pendingItem_->taskId, pendingItem_->nextOrdinal);
            out.publications[0] = std::move(step.publication);
            if (!step.published || !step.audit)
            { out.resourceUnavailable = step.resourceUnavailable; out.reason = "v4_selected_step_shard_unavailable"; return out; }
            V4ShardLocator stepLocator{out.publications[0].fileName, out.publications[0].fileId};
            auto stepIndex = AppendV4ShardIndex(store, stagedHead, stepLocator, "typed_step", binding, budget, axisBudget,
                tokenBudget, byteBudget, byteBudget, replayWorkBudget, byteBudget, indexByteBudget);
            out.publications[1] = std::move(stepIndex.publication);
            if (!stepIndex.published || !stepIndex.head)
            { out.resourceUnavailable = stepIndex.resourceUnavailable; out.reason = "v4_selected_step_index_unavailable"; return out; }
            auto finalHead = *stepIndex.head;
            if (createFamily)
            {
                FamilyState family; family.version = FamilyVersion::v4; family.coordinate = predicted.coordinate;
                auto familyShard = PublishV4FamilyCursorShard(store, family, binding, budget, axisBudget, byteBudget);
                out.publications[2] = std::move(familyShard.publication);
                if (!familyShard.published)
                { out.resourceUnavailable = familyShard.resourceUnavailable; out.reason = "v4_selected_step_family_shard_unavailable"; return out; }
                V4ShardLocator familyLocator{out.publications[2].fileName, out.publications[2].fileId};
                auto familyIndex = AppendV4ShardIndex(store, finalHead, familyLocator, "family", binding, budget, axisBudget,
                    tokenBudget, byteBudget, byteBudget, replayWorkBudget, byteBudget, indexByteBudget);
                out.publications[3] = std::move(familyIndex.publication);
                if (!familyIndex.published || !familyIndex.head)
                { out.resourceUnavailable = familyIndex.resourceUnavailable; out.reason = "v4_selected_step_family_index_unavailable"; return out; }
                finalHead = std::move(*familyIndex.head); out.familyRegistered = true;
            }
            if (createMain)
            {
                V4StructuredDfsCursor genesis; genesis.layer = nextLayer;
                auto mainShard = PublishV4TypedCursorShard(store, genesis, false, binding, budget, tokenBudget, byteBudget);
                out.publications[4] = std::move(mainShard.publication);
                if (!mainShard.published)
                { out.resourceUnavailable = mainShard.resourceUnavailable; out.reason = "v4_selected_step_main_shard_unavailable"; return out; }
                V4ShardLocator mainLocator{out.publications[4].fileName, out.publications[4].fileId};
                auto mainIndex = AppendV4ShardIndex(store, finalHead, mainLocator, "global_cursor", binding, budget, axisBudget,
                    tokenBudget, byteBudget, byteBudget, replayWorkBudget, byteBudget, indexByteBudget);
                out.publications[5] = std::move(mainIndex.publication);
                if (!mainIndex.published || !mainIndex.head)
                { out.resourceUnavailable = mainIndex.resourceUnavailable; out.reason = "v4_selected_step_main_index_unavailable"; return out; }
                finalHead = std::move(*mainIndex.head); out.mainGenesisRegistered = true;
            }
            out.head = finalHead; out.audit = std::move(step.audit);
            if (predicted.status == V4StructuredDfsStatus::emitted || createMain)
            {
                selectedStagedHead_ = std::move(finalHead);
                selectedLookup_ = predicted.status == V4StructuredDfsStatus::emitted ? familyLookup : nullptr;
                if (predicted.status == V4StructuredDfsStatus::emitted)
                { selectedLookupIdentity_ = familyLookup->identity_; selectedAfterSerial_ = familyLookup->recheckSerial_; }
                selectedMainLookup_ = createMain ? mainLookup : nullptr;
                if (createMain) { selectedMainIdentity_ = mainLookup->identity_; selectedMainAfterSerial_ = mainLookup->recheckSerial_; }
                out.postcheckRequired = true; out.reason = "v4_selected_step_postcheck_pending_not_adoptable"; return out;
            }
            pendingHead_ = std::move(finalHead); pendingItemProcessed_ = true;
            out.published = true; out.reason = "v4_selected_step_chain_unadopted_history_unauthenticated"; return out;
        }
        catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_selected_step_resource_unavailable"; return out; }
        catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_selected_step_resource_unavailable"; return out; }
        catch (...) { out.reason = "v4_selected_step_unavailable"; return out; }
    }
    bool FinalizeSelectedTypedStep(const V4ShardLocator& stagedHead, const V4ShardFamilyLookup* lookup,
        const V4ShardFamilyRecheck* postcheck, const V4ShardFamilyLookup* mainLookup = nullptr,
        const V4ShardFamilyRecheck* mainPostcheck = nullptr) noexcept
    {
        const auto identity = selectedLookupIdentity_.lock();
        const auto mainIdentity = selectedMainIdentity_.lock();
        if (!pending_ || !pendingItem_ || pendingItemProcessed_ || !selectedStagedHead_ || !Same(*selectedStagedHead_, stagedHead) ||
            (selectedLookup_ && (!lookup || !postcheck || selectedLookup_ != lookup || !identity || !lookup->identity_ ||
                identity.owner_before(lookup->identity_) || lookup->identity_.owner_before(identity) ||
                !postcheck->Matches(*lookup) || postcheck->Serial() <= selectedAfterSerial_)) ||
            (selectedMainLookup_ && (!mainLookup || !mainPostcheck || selectedMainLookup_ != mainLookup || !mainIdentity || !mainLookup->identity_ ||
                mainIdentity.owner_before(mainLookup->identity_) || mainLookup->identity_.owner_before(mainIdentity) ||
                !mainPostcheck->Matches(*mainLookup) || mainPostcheck->Serial() <= selectedMainAfterSerial_)))
            return false;
        pendingHead_ = std::move(*selectedStagedHead_); selectedStagedHead_.reset(); selectedLookup_ = nullptr;
        selectedLookupIdentity_.reset(); selectedAfterSerial_ = 0; selectedMainLookup_ = nullptr;
        selectedMainIdentity_.reset(); selectedMainAfterSerial_ = 0; pendingItemProcessed_ = true; return true;
    }
};

struct V4GlobalPreviewBudgets
{
    V4CoordinateBudget coordinate;
    V4AxisBudget axes;
    std::size_t token = 0, replayWork = 0, bytes = 0, indexBytes = 0;
    std::size_t work = 0, node = 0, pins = 0, snapshotBytes = 0;
};
enum class V4GlobalPreviewStatus { progress, family_item, ready_to_adopt, resource_unavailable, unavailable, stopped };
struct V4GlobalPreviewTick
{
    V4GlobalPreviewStatus status = V4GlobalPreviewStatus::unavailable;
    std::size_t workUsed = 0;
    std::string_view reason;
};
// Single foreground coordinator for one typed preview round. It owns all
// intermediate sessions and never invokes a production adapter. A failed
// publication stops the coordinator with its partial-file evidence retained.
class V4GlobalPreviewCycle final
{
    enum class Phase { ancestry, ancestry_pre, publish_round, ancestry_post, inspect_item,
        family_lookup, family_pre, main_lookup, main_pre, publish_selected, family_post, main_post, ready, stopped };
    V4ShardLocator currentHead_, snapshotHead_;
    std::unique_ptr<V4TypedShardPreviewRound> round_;
    std::unique_ptr<V4RoundAncestryCheck> ancestry_;
    std::unique_ptr<V4RoundAncestryRecheck> ancestryPre_, ancestryPost_;
    std::unique_ptr<V4ShardFamilyLookup> family_;
    std::unique_ptr<V4ShardFamilyRecheck> familyPre_, familyPost_;
    std::unique_ptr<V4ShardFamilyLookup> main_;
    std::unique_ptr<V4ShardFamilyRecheck> mainPre_, mainPost_;
    bool needFamily_ = false, needMain_ = false;
    std::size_t nextLayer_ = 0;
    std::optional<V4TypedRoundPublication> roundPublication_;
    std::optional<V4TypedSelectedPublication> selectedPublication_;
    std::optional<V4ShardLocator> finalHead_;
    Phase phase_ = Phase::ancestry;
    explicit V4GlobalPreviewCycle(V4ShardLocator head) : currentHead_(head), snapshotHead_(head),
        round_(V4TypedShardPreviewRound::Begin(std::move(head))) {}
public:
    V4GlobalPreviewCycle(const V4GlobalPreviewCycle&) = delete;
    V4GlobalPreviewCycle& operator=(const V4GlobalPreviewCycle&) = delete;
    static std::unique_ptr<V4GlobalPreviewCycle> Begin(V4ShardLocator trustedHead)
    { return std::unique_ptr<V4GlobalPreviewCycle>(new V4GlobalPreviewCycle(std::move(trustedHead))); }
    const V4TypedRoundPublication* RoundPublication() const noexcept
    { return roundPublication_ ? &*roundPublication_ : nullptr; }
    const V4TypedSelectedPublication* SelectedPublication() const noexcept
    { return selectedPublication_ ? &*selectedPublication_ : nullptr; }
    const std::optional<V4ShardLocator>& ProposedHead() const noexcept { return finalHead_; }
    bool IsStopped() const noexcept { return phase_ == Phase::stopped; }
    V4GlobalPreviewTick Step(CheckpointStore& store, std::string_view binding, const V4GlobalPreviewBudgets& b)
    {
        V4GlobalPreviewTick out;
        try
        {
            if (phase_ == Phase::stopped) { out.status = V4GlobalPreviewStatus::stopped; out.reason = "v4_global_preview_stopped"; return out; }
            if (phase_ == Phase::ready) { out.status = V4GlobalPreviewStatus::ready_to_adopt; out.reason = "v4_global_preview_head_unadopted"; return out; }
            const auto check = [&](const V4ShardLookupStep& step, Phase next) {
                out.workUsed = step.workUsed;
                if (step.status == V4ShardLookupStatus::found || step.status == V4ShardLookupStatus::not_found)
                { phase_ = next; out.status = V4GlobalPreviewStatus::progress; }
                else if (step.status == V4ShardLookupStatus::progress) out.status = V4GlobalPreviewStatus::progress;
                else out.status = step.status == V4ShardLookupStatus::resource_unavailable ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable;
                out.reason = step.reason;
            };
            const auto hasSideEffects = [](const auto& publication) {
                return std::any_of(publication.publications.begin(), publication.publications.end(), [](const auto& item) {
                    return item.temporaryCreateAttempted || item.temporaryCreated || item.renameAttempted || item.renameReturnedSuccess;
                });
            };
            switch (phase_)
            {
            case Phase::ancestry:
                if (!ancestry_) ancestry_ = V4RoundAncestryCheck::Begin(currentHead_, snapshotHead_);
                check(ancestry_->Step(store, binding, b.indexBytes, b.work, b.pins, b.snapshotBytes), Phase::ancestry_pre); return out;
            case Phase::ancestry_pre:
                if (!ancestryPre_) ancestryPre_ = V4RoundAncestryRecheck::Begin(*ancestry_);
                if (!ancestryPre_) { out.reason = "v4_global_preview_ancestry_pre_unavailable"; return out; }
                check(ancestryPre_->Step(b.work), Phase::publish_round); return out;
            case Phase::publish_round:
                roundPublication_ = round_->PublishAdvance(store, currentHead_, binding, b.coordinate, b.axes,
                    b.token, b.replayWork, b.bytes, b.node, b.pins, b.snapshotBytes, b.indexBytes, *ancestry_, *ancestryPre_);
                if (!roundPublication_->postcheckRequired || !roundPublication_->head)
                { if (hasSideEffects(*roundPublication_)) { phase_ = Phase::stopped; out.status = V4GlobalPreviewStatus::stopped; }
                  else out.status = roundPublication_->resourceUnavailable ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable;
                  out.reason = roundPublication_->reason; return out; }
                ancestryPost_ = V4RoundAncestryRecheck::Begin(*ancestry_);
                if (!ancestryPost_) { phase_ = Phase::stopped; out.status = V4GlobalPreviewStatus::stopped; out.reason = "v4_global_preview_ancestry_post_unavailable"; return out; }
                phase_ = Phase::ancestry_post; out.status = V4GlobalPreviewStatus::progress; out.reason = "v4_global_preview_round_staged"; return out;
            case Phase::ancestry_post:
            {
                const auto step = ancestryPost_->Step(b.work); check(step, Phase::inspect_item);
                if (phase_ == Phase::inspect_item && !round_->FinalizeRoundAdvance(*roundPublication_->head, *ancestry_, *ancestryPost_))
                { phase_ = Phase::stopped; out.status = V4GlobalPreviewStatus::stopped; out.reason = "v4_global_preview_round_finalize_unavailable"; }
                return out;
            }
            case Phase::inspect_item:
            {
                if (!roundPublication_->preview.item)
                { finalHead_ = roundPublication_->head; phase_ = Phase::ready; out.status = V4GlobalPreviewStatus::ready_to_adopt; out.reason = "v4_global_preview_round_unadopted"; return out; }
                const auto predicted = round_->InspectPendingTypedStep(b.coordinate, b.token, b.node);
                if (predicted.status == V4StructuredDfsStatus::resource_unavailable)
                { out.status = V4GlobalPreviewStatus::resource_unavailable; out.reason = predicted.reason; return out; }
                if (predicted.status == V4StructuredDfsStatus::invalid || !predicted.workUsed)
                { out.reason = "v4_global_preview_typed_step_unavailable"; return out; }
                std::size_t layer = 0; bool mainTask = false;
                if (!V4TypedTaskIdCanonical(roundPublication_->preview.item->taskId, layer, mainTask))
                { out.reason = "v4_global_preview_task_identity_unavailable"; return out; }
                needMain_ = mainTask && predicted.next.exhausted;
                if (needMain_ && layer == (std::numeric_limits<std::size_t>::max)())
                { out.status = V4GlobalPreviewStatus::resource_unavailable; out.reason = "v4_global_preview_next_layer_unavailable"; return out; }
                nextLayer_ = needMain_ ? layer + 1 : 0;
                needFamily_ = predicted.status == V4StructuredDfsStatus::emitted;
                if (needFamily_)
                { family_ = V4ShardFamilyLookup::Begin(*roundPublication_->head, predicted.coordinate); phase_ = Phase::family_lookup; }
                else if (needMain_) { main_ = V4ShardFamilyLookup::BeginMain(*roundPublication_->head, nextLayer_); phase_ = Phase::main_lookup; }
                else phase_ = Phase::publish_selected;
                out.status = V4GlobalPreviewStatus::progress; out.reason = "v4_global_preview_item_selected"; return out;
            }
            case Phase::family_lookup:
                check(family_->Step(store, binding, b.coordinate, b.axes, b.bytes, b.indexBytes, b.work,
                    b.replayWork, b.pins, b.snapshotBytes), Phase::family_pre); return out;
            case Phase::family_pre:
                if (!familyPre_) familyPre_ = V4ShardFamilyRecheck::Begin(*family_);
                if (!familyPre_) { out.reason = "v4_global_preview_family_pre_unavailable"; return out; }
                if (needMain_ && !main_) main_ = V4ShardFamilyLookup::BeginMain(*roundPublication_->head, nextLayer_);
            {
                const auto step = familyPre_->Step(b.work); check(step, needMain_ ? Phase::main_lookup : Phase::publish_selected);
                return out;
            }
            case Phase::main_lookup:
            {
                const auto step = main_->Step(store, binding, b.coordinate, b.axes, b.bytes, b.indexBytes, b.work,
                    b.replayWork, b.pins, b.snapshotBytes, b.token);
                out.workUsed = step.workUsed; out.reason = step.reason;
                if (step.status == V4ShardLookupStatus::not_found) { phase_ = Phase::main_pre; out.status = V4GlobalPreviewStatus::progress; }
                else if (step.status == V4ShardLookupStatus::progress) out.status = V4GlobalPreviewStatus::progress;
                else out.status = step.status == V4ShardLookupStatus::resource_unavailable ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable;
                return out;
            }
            case Phase::main_pre:
                if (!mainPre_) mainPre_ = V4ShardFamilyRecheck::Begin(*main_);
                if (!mainPre_) { out.reason = "v4_global_preview_main_pre_unavailable"; return out; }
                check(mainPre_->Step(b.work), Phase::publish_selected); return out;
            case Phase::publish_selected:
                selectedPublication_ = round_->PublishSelectedTypedStep(store, *roundPublication_->head, binding, b.coordinate,
                    b.axes, b.token, b.node, b.replayWork, b.bytes, b.indexBytes, family_.get(), familyPre_.get(), main_.get(), mainPre_.get());
                if (!selectedPublication_->head || (!selectedPublication_->published && !selectedPublication_->postcheckRequired))
                { if (hasSideEffects(*selectedPublication_)) { phase_ = Phase::stopped; out.status = V4GlobalPreviewStatus::stopped; }
                  else { out.status = selectedPublication_->resourceUnavailable ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable;
                      family_.reset(); familyPre_.reset(); familyPost_.reset(); main_.reset(); mainPre_.reset(); mainPost_.reset();
                      needFamily_ = needMain_ = false; phase_ = Phase::inspect_item; }
                  out.reason = selectedPublication_->reason; return out; }
                if (selectedPublication_->postcheckRequired)
                {
                    if (needFamily_) { familyPost_ = V4ShardFamilyRecheck::Begin(*family_); if (!familyPost_)
                        { phase_ = Phase::stopped; out.status = V4GlobalPreviewStatus::stopped; out.reason = "v4_global_preview_family_post_unavailable"; return out; }
                        phase_ = Phase::family_post; }
                    else { mainPost_ = V4ShardFamilyRecheck::Begin(*main_); if (!mainPost_)
                        { phase_ = Phase::stopped; out.status = V4GlobalPreviewStatus::stopped; out.reason = "v4_global_preview_main_post_unavailable"; return out; }
                        phase_ = Phase::main_post; }
                    out.status = V4GlobalPreviewStatus::progress; out.reason = "v4_global_preview_selected_staged"; return out;
                }
                finalHead_ = selectedPublication_->head; phase_ = Phase::ready; out.status = V4GlobalPreviewStatus::ready_to_adopt;
                out.reason = "v4_global_preview_selected_unadopted"; return out;
            case Phase::family_post:
            {
                auto final = *selectedPublication_->head;
                if (needMain_ && !mainPost_) mainPost_ = V4ShardFamilyRecheck::Begin(*main_);
                if (needMain_ && !mainPost_) { phase_ = Phase::stopped; out.status = V4GlobalPreviewStatus::stopped;
                    out.reason = "v4_global_preview_main_post_unavailable"; return out; }
                const auto step = familyPost_->Step(b.work); out.workUsed = step.workUsed; out.reason = step.reason;
                if (step.status == V4ShardLookupStatus::found)
                {
                    if (needMain_)
                    { phase_ = Phase::main_post; out.status = V4GlobalPreviewStatus::progress; return out; }
                    if (!round_->FinalizeSelectedTypedStep(*selectedPublication_->head, family_.get(), familyPost_.get()))
                    { phase_ = Phase::stopped; out.status = V4GlobalPreviewStatus::stopped; out.reason = "v4_global_preview_selected_finalize_unavailable"; }
                    else { finalHead_ = std::move(final); phase_ = Phase::ready; out.status = V4GlobalPreviewStatus::ready_to_adopt; out.reason = "v4_global_preview_selected_unadopted"; }
                }
                else out.status = step.status == V4ShardLookupStatus::progress ? V4GlobalPreviewStatus::progress :
                    step.status == V4ShardLookupStatus::resource_unavailable ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable;
                return out;
            }
            case Phase::main_post:
            {
                auto final = *selectedPublication_->head;
                const auto step = mainPost_->Step(b.work); out.workUsed = step.workUsed; out.reason = step.reason;
                if (step.status == V4ShardLookupStatus::found)
                {
                    if (!round_->FinalizeSelectedTypedStep(*selectedPublication_->head, family_.get(), familyPost_.get(), main_.get(), mainPost_.get()))
                    { phase_ = Phase::stopped; out.status = V4GlobalPreviewStatus::stopped; out.reason = "v4_global_preview_main_finalize_unavailable"; }
                    else { finalHead_ = std::move(final); phase_ = Phase::ready; out.status = V4GlobalPreviewStatus::ready_to_adopt; out.reason = "v4_global_preview_selected_unadopted"; }
                }
                else out.status = step.status == V4ShardLookupStatus::progress ? V4GlobalPreviewStatus::progress :
                    step.status == V4ShardLookupStatus::resource_unavailable ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable;
                return out;
            }
            default: break;
            }
        }
        catch (const std::bad_alloc&) { if ((phase_ == Phase::publish_round && roundPublication_ && roundPublication_->postcheckRequired) ||
                (phase_ == Phase::publish_selected && selectedPublication_ && (selectedPublication_->published || selectedPublication_->postcheckRequired)))
            { phase_ = Phase::stopped; out.status = V4GlobalPreviewStatus::stopped; } else out.status = V4GlobalPreviewStatus::resource_unavailable;
            out.reason = "v4_global_preview_resource_unavailable"; return out; }
        catch (const std::length_error&) { if ((phase_ == Phase::publish_round && roundPublication_ && roundPublication_->postcheckRequired) ||
                (phase_ == Phase::publish_selected && selectedPublication_ && (selectedPublication_->published || selectedPublication_->postcheckRequired)))
            { phase_ = Phase::stopped; out.status = V4GlobalPreviewStatus::stopped; } else out.status = V4GlobalPreviewStatus::resource_unavailable;
            out.reason = "v4_global_preview_resource_unavailable"; return out; }
        catch (...) { phase_ = Phase::stopped; out.status = V4GlobalPreviewStatus::stopped; out.reason = "v4_global_preview_unavailable"; return out; }
        out.reason = "v4_global_preview_phase_unavailable"; return out;
    }
    bool Adopt(const V4ShardLocator& adoptedHead)
    {
        if (phase_ != Phase::ready || !finalHead_) return false;
        const bool nextRound = roundPublication_ && roundPublication_->preview.status == V4TypedPreviewStatus::exhausted;
        auto newHead = adoptedHead; auto newSnapshot = adoptedHead;
        auto newRound = nextRound ? V4TypedShardPreviewRound::Begin(newSnapshot) : nullptr;
        if (!round_->AdoptPublished(adoptedHead)) return false;
        currentHead_ = std::move(newHead);
        if (nextRound) { snapshotHead_ = std::move(newSnapshot); round_ = std::move(newRound); }
        ancestry_.reset(); ancestryPre_.reset(); ancestryPost_.reset(); family_.reset(); familyPre_.reset(); familyPost_.reset();
        main_.reset(); mainPre_.reset(); mainPost_.reset(); needFamily_ = needMain_ = false; nextLayer_ = 0;
        roundPublication_.reset(); selectedPublication_.reset(); finalHead_.reset(); phase_ = Phase::ancestry; return true;
    }
    bool CanRebase(const V4ShardLocator& expectedHead) const noexcept
    {
        return phase_ == Phase::ancestry && !ancestry_ && currentHead_.name == expectedHead.name &&
            currentHead_.fileId.VolumeSerialNumber == expectedHead.fileId.VolumeSerialNumber &&
            std::memcmp(currentHead_.fileId.FileId.Identifier, expectedHead.fileId.FileId.Identifier,
                sizeof(expectedHead.fileId.FileId.Identifier)) == 0;
    }
    bool RebaseFromFamily(const V4ShardLocator& expectedHead, V4ShardLocator&& newHead) noexcept
    { if (!CanRebase(expectedHead)) return false; currentHead_ = std::move(newHead); return true; }
    // Only transient, pre-publication work may be abandoned to visit a family.
    // A staged file or unadopted head must be resolved by its own protocol.
    bool SuspendBeforePublication() noexcept
    {
        if (phase_ != Phase::ancestry && phase_ != Phase::ancestry_pre && phase_ != Phase::publish_round) return false;
        if (roundPublication_ && std::any_of(roundPublication_->publications.begin(), roundPublication_->publications.end(), [](const auto& item) {
            return item.temporaryCreateAttempted || item.temporaryCreated || item.renameAttempted || item.renameReturnedSuccess;
        })) return false;
        ancestryPre_.reset(); ancestryPost_.reset(); ancestry_.reset(); roundPublication_.reset(); phase_ = Phase::ancestry; return true;
    }
};

// One registered family's authoritative candidate/graph DFS step, staged and
// read back before a caller adopts its head. No case is dispatched here.
class V4FamilyPreviewCycle final
{
    enum class Phase { lookup, precheck, publish, postcheck, ready, stopped, adopted };
    V4ShardLocator head_;
    std::string coordinate_;
    std::unique_ptr<V4ShardFamilyLookup> lookup_;
    std::unique_ptr<V4ShardFamilyRecheck> pre_, post_;
    std::optional<V4ShardFamilyCaseTransaction> publication_;
    std::optional<V4ShardLocator> finalHead_;
    Phase phase_ = Phase::lookup;
    V4FamilyPreviewCycle(V4ShardLocator head, std::string coordinate)
        : head_(head), coordinate_(std::move(coordinate)), lookup_(V4ShardFamilyLookup::Begin(std::move(head), coordinate_)) {}
public:
    V4FamilyPreviewCycle(const V4FamilyPreviewCycle&) = delete;
    V4FamilyPreviewCycle& operator=(const V4FamilyPreviewCycle&) = delete;
    static std::unique_ptr<V4FamilyPreviewCycle> Begin(V4ShardLocator trustedHead, std::string coordinate)
    { return std::unique_ptr<V4FamilyPreviewCycle>(new V4FamilyPreviewCycle(std::move(trustedHead), std::move(coordinate))); }
    const V4ShardFamilyCaseTransaction* Publication() const noexcept { return publication_ ? &*publication_ : nullptr; }
    const std::optional<V4ShardLocator>& ProposedHead() const noexcept { return finalHead_; }
    bool CanDiscardBeforePublication() const noexcept
    {
        if (phase_ != Phase::lookup && phase_ != Phase::precheck && phase_ != Phase::publish) return false;
        return !publication_ || std::none_of(publication_->publications.begin(), publication_->publications.end(), [](const auto& item) {
            return item.temporaryCreateAttempted || item.temporaryCreated || item.renameAttempted || item.renameReturnedSuccess;
        });
    }
    V4GlobalPreviewTick Step(CheckpointStore& store, std::string_view binding, const V4GlobalPreviewBudgets& b)
    {
        V4GlobalPreviewTick out;
        try
        {
            if (phase_ == Phase::stopped || phase_ == Phase::adopted)
            { out.status = V4GlobalPreviewStatus::stopped; out.reason = "v4_family_preview_stopped_or_adopted"; return out; }
            if (phase_ == Phase::ready)
            { out.status = V4GlobalPreviewStatus::ready_to_adopt; out.reason = "v4_family_preview_head_unadopted"; return out; }
            if (phase_ == Phase::lookup)
            {
                const auto step = lookup_->Step(store, binding, b.coordinate, b.axes, b.bytes, b.indexBytes,
                    b.work, b.replayWork, b.pins, b.snapshotBytes);
                out.workUsed = step.workUsed; out.reason = step.reason;
                if (step.status == V4ShardLookupStatus::found) { phase_ = Phase::precheck; out.status = V4GlobalPreviewStatus::progress; }
                else if (step.status == V4ShardLookupStatus::progress) out.status = V4GlobalPreviewStatus::progress;
                else out.status = step.status == V4ShardLookupStatus::resource_unavailable ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable;
                return out;
            }
            if (phase_ == Phase::precheck)
            {
                if (!pre_) pre_ = V4ShardFamilyRecheck::Begin(*lookup_);
                if (!pre_) { out.reason = "v4_family_preview_precheck_unavailable"; return out; }
                const auto step = pre_->Step(b.work); out.workUsed = step.workUsed; out.reason = step.reason;
                if (step.status == V4ShardLookupStatus::found) { phase_ = Phase::publish; out.status = V4GlobalPreviewStatus::progress; }
                else out.status = step.status == V4ShardLookupStatus::progress ? V4GlobalPreviewStatus::progress :
                    step.status == V4ShardLookupStatus::resource_unavailable ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable;
                return out;
            }
            if (phase_ == Phase::publish)
            {
                publication_ = PublishV4ShardFamilyCaseStep(store, head_, coordinate_, binding, b.coordinate, b.axes,
                    b.token, b.node, b.work, b.replayWork, b.bytes, b.indexBytes, lookup_.get(), b.pins, b.snapshotBytes, pre_.get());
                if (!publication_->postcheckRequired || !publication_->pending || !publication_->head)
                {
                    const bool sideEffects = std::any_of(publication_->publications.begin(), publication_->publications.end(), [](const auto& item) {
                        return item.temporaryCreateAttempted || item.temporaryCreated || item.renameAttempted || item.renameReturnedSuccess; });
                    if (sideEffects) { phase_ = Phase::stopped; out.status = V4GlobalPreviewStatus::stopped; }
                    else out.status = publication_->resourceUnavailable ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable;
                    out.reason = publication_->reason; return out;
                }
                post_ = V4ShardFamilyRecheck::Begin(*lookup_);
                if (!post_) { phase_ = Phase::stopped; out.status = V4GlobalPreviewStatus::stopped; out.reason = "v4_family_preview_postcheck_unavailable"; return out; }
                phase_ = Phase::postcheck; out.status = V4GlobalPreviewStatus::progress; out.reason = "v4_family_preview_staged"; return out;
            }
            if (phase_ == Phase::postcheck)
            {
                const auto step = post_->Step(b.work); out.workUsed = step.workUsed; out.reason = step.reason;
                if (step.status == V4ShardLookupStatus::found)
                {
                    auto final = publication_->pending->Finalize(*lookup_, *post_);
                    if (!final) { phase_ = Phase::stopped; out.status = V4GlobalPreviewStatus::stopped; out.reason = "v4_family_preview_finalize_unavailable"; return out; }
                    finalHead_ = std::move(final); phase_ = Phase::ready; out.status = V4GlobalPreviewStatus::ready_to_adopt;
                    out.reason = "v4_family_preview_head_unadopted"; return out;
                }
                out.status = step.status == V4ShardLookupStatus::progress ? V4GlobalPreviewStatus::progress :
                    step.status == V4ShardLookupStatus::resource_unavailable ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable;
                return out;
            }
        }
        catch (const std::bad_alloc&) { if (phase_ == Phase::publish && publication_ && publication_->postcheckRequired) { phase_ = Phase::stopped; out.status = V4GlobalPreviewStatus::stopped; }
            else out.status = V4GlobalPreviewStatus::resource_unavailable; out.reason = "v4_family_preview_resource_unavailable"; return out; }
        catch (const std::length_error&) { if (phase_ == Phase::publish && publication_ && publication_->postcheckRequired) { phase_ = Phase::stopped; out.status = V4GlobalPreviewStatus::stopped; }
            else out.status = V4GlobalPreviewStatus::resource_unavailable; out.reason = "v4_family_preview_resource_unavailable"; return out; }
        catch (...) { phase_ = Phase::stopped; out.status = V4GlobalPreviewStatus::stopped; out.reason = "v4_family_preview_unavailable"; return out; }
        out.reason = "v4_family_preview_phase_unavailable"; return out;
    }
    bool Adopt(const V4ShardLocator& adoptedHead) noexcept
    {
        if (phase_ != Phase::ready || !finalHead_ || adoptedHead.name != finalHead_->name ||
            adoptedHead.fileId.VolumeSerialNumber != finalHead_->fileId.VolumeSerialNumber ||
            std::memcmp(adoptedHead.fileId.FileId.Identifier, finalHead_->fileId.FileId.Identifier,
                sizeof(adoptedHead.fileId.FileId.Identifier)) != 0) return false;
        phase_ = Phase::adopted; return true;
    }
};

struct V4FamilyRoundItem
{
    std::string coordinate;
    bool scanExhausted = false; // never a case completion or product verdict
    WorkState savedState = WorkState::pending; // untrusted audit value only
    std::string ordinal; // ordinal within this fixed snapshot, not coverage
};
struct V4FamilyRoundStep
{
    V4GlobalPreviewStatus status = V4GlobalPreviewStatus::unavailable;
    std::optional<V4FamilyRoundItem> item;
    bool exhausted = false;
    std::size_t workUsed = 0;
    std::string_view reason;
};
// Bounded fixed-head family selection. This is an in-process preview; cursor
// restoration and the head's content authentication remain separate work.
class V4FamilyPreviewRound final
{
    struct Snapshot { V4ShardLocator locator; std::unique_ptr<V4CheckpointReadPin> pin; std::string bytes; };
    V4ShardLocator head_;
    V4ShardIndexCursor outer_, nextOuter_;
    std::optional<V4ShardLocator> candidate_;
    std::optional<FamilyState> family_;
    std::unique_ptr<V4ShardFamilyLookup> lookup_;
    std::unique_ptr<V4ShardFamilyRecheck> recheck_;
    std::vector<Snapshot> snapshots_;
    std::size_t bytes_ = 0, checkIndex_ = 0;
    CheckpointStore* store_ = nullptr;
    std::string binding_;
    std::string ordinal_ = "0";
    bool found_ = false, ending_ = false, done_ = false, failed_ = false;
    explicit V4FamilyPreviewRound(V4ShardLocator head) : head_(head), outer_{std::move(head), {}} {}
    static bool Same(const V4ShardLocator& a, const V4ShardLocator& b) noexcept
    { return a.name == b.name && a.fileId.VolumeSerialNumber == b.fileId.VolumeSerialNumber &&
        std::memcmp(a.fileId.FileId.Identifier, b.fileId.FileId.Identifier, sizeof(a.fileId.FileId.Identifier)) == 0; }
public:
    V4FamilyPreviewRound(const V4FamilyPreviewRound&) = delete;
    V4FamilyPreviewRound& operator=(const V4FamilyPreviewRound&) = delete;
    static std::unique_ptr<V4FamilyPreviewRound> Begin(V4ShardLocator fixedHead)
    { return std::unique_ptr<V4FamilyPreviewRound>(new V4FamilyPreviewRound(std::move(fixedHead))); }
    const V4ShardLocator& SnapshotHead() const noexcept { return head_; }
    V4FamilyRoundStep Step(CheckpointStore& store, std::string_view binding, const V4GlobalPreviewBudgets& b)
    {
        V4FamilyRoundStep out;
        try
        {
            if (failed_ || done_ || (store_ && (store_ != &store || binding_ != binding)))
            { out.reason = "v4_family_round_unavailable"; return out; }
            if (!store_) { binding_ = std::string(binding); store_ = &store; }
            if (snapshots_.size() > b.pins || bytes_ > b.snapshotBytes)
            { out.status = V4GlobalPreviewStatus::resource_unavailable; out.reason = "v4_family_round_budget_unavailable"; return out; }
            bool resource = false;
            const auto snapshot = [&](const V4ShardLocator& locator, std::size_t budget) {
                const auto found = std::find_if(snapshots_.begin(), snapshots_.end(), [&](const auto& entry) { return Same(entry.locator, locator); });
                if (found != snapshots_.end())
                {
                    const auto loaded = store.Read(locator.name, locator.fileId, binding, budget);
                    if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight)
                    { resource = loaded.status == CodecStatus::resource_unavailable; return false; }
                    return loaded.bytes == found->bytes;
                }
                if (snapshots_.size() >= b.pins) { resource = true; return false; }
                auto pin = store.PinRead(locator.name, locator.fileId); if (!pin) return false;
                auto loaded = store.Read(locator.name, locator.fileId, binding, budget);
                if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight)
                { resource = loaded.status == CodecStatus::resource_unavailable; return false; }
                if (bytes_ > b.snapshotBytes || loaded.bytes.size() > b.snapshotBytes - bytes_) { resource = true; return false; }
                const auto n = loaded.bytes.size(); snapshots_.push_back({locator, std::move(pin), std::move(loaded.bytes)}); bytes_ += n; return true;
            };
            while (out.workUsed < b.work)
            {
                if (ending_)
                {
                    if (checkIndex_ < snapshots_.size())
                    { if (!snapshot(snapshots_[checkIndex_].locator, (std::max)(b.bytes, b.indexBytes)))
                      { failed_ = !resource; out.status = resource ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable; return out; }
                      ++checkIndex_; ++out.workUsed; continue; }
                    done_ = true; out.exhausted = true; out.status = V4GlobalPreviewStatus::progress;
                    out.reason = "v4_family_round_snapshot_exhausted_not_coverage"; return out;
                }
                if (found_)
                {
                    if (checkIndex_ < snapshots_.size())
                    { if (!snapshot(snapshots_[checkIndex_].locator, (std::max)(b.bytes, b.indexBytes)))
                      { failed_ = !resource; out.status = resource ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable; return out; }
                      ++checkIndex_; ++out.workUsed; continue; }
                    if (!recheck_) recheck_ = V4ShardFamilyRecheck::Begin(*lookup_);
                    if (!recheck_) { out.reason = "v4_family_round_recheck_unavailable"; return out; }
                    V4FamilyRoundItem item{family_->coordinate, family_->scanExhausted, family_->state, ordinal_};
                    auto nextOrdinal = ordinal_; Increment(nextOrdinal);
                    const auto check = recheck_->Step(b.work - out.workUsed); out.workUsed += check.workUsed;
                    if (check.status == V4ShardLookupStatus::progress) { out.status = V4GlobalPreviewStatus::progress; return out; }
                    if (check.status != V4ShardLookupStatus::found)
                    { out.status = check.status == V4ShardLookupStatus::resource_unavailable ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable; return out; }
                    out.item = std::move(item); outer_ = std::move(nextOuter_); ordinal_ = std::move(nextOrdinal); candidate_.reset(); family_.reset();
                    lookup_.reset(); recheck_.reset(); found_ = false; checkIndex_ = 0;
                    out.status = V4GlobalPreviewStatus::family_item; out.reason = "v4_family_round_item_unexecuted_untrusted"; return out;
                }
                if (lookup_)
                {
                    const auto checked = lookup_->Step(store, binding, b.coordinate, b.axes, b.bytes, b.indexBytes,
                        b.work - out.workUsed, b.replayWork, b.pins - snapshots_.size(), b.snapshotBytes - bytes_);
                    out.workUsed += checked.workUsed;
                    if (checked.status == V4ShardLookupStatus::progress) { out.status = V4GlobalPreviewStatus::progress; return out; }
                    if (checked.status != V4ShardLookupStatus::found || !checked.matched)
                    { out.status = checked.status == V4ShardLookupStatus::resource_unavailable ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable; return out; }
                    if (!Same(*candidate_, *checked.matched))
                    { outer_ = std::move(nextOuter_); candidate_.reset(); family_.reset(); lookup_.reset(); continue; }
                    found_ = true; checkIndex_ = 0; continue;
                }
                if (!outer_.next)
                { ending_ = true; checkIndex_ = 0; continue; }
                if (!snapshot(*outer_.next, b.indexBytes))
                { failed_ = !resource; out.status = resource ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable; return out; }
                const auto page = ReadV4ShardIndexPage(store, outer_, binding, b.indexBytes);
                if (!page.read || !page.data) { out.status = page.resourceUnavailable ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable; return out; }
                ++out.workUsed;
                if (page.kind != "family") { outer_ = page.next; continue; }
                if (!snapshot(*page.data, b.bytes))
                { failed_ = !resource; out.status = resource ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable; return out; }
                const auto shard = ReadV4FamilyCursorShard(store, page.data->name, page.data->fileId,
                    binding, b.coordinate, b.axes, b.bytes, b.replayWork);
                if (!shard.cursorValidated || !shard.family)
                { out.status = shard.resourceUnavailable ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable; return out; }
                auto next = page.next; auto candidate = *page.data; auto family = *shard.family;
                auto lookup = V4ShardFamilyLookup::Begin(head_, family.coordinate);
                nextOuter_ = std::move(next); candidate_ = std::move(candidate); family_ = std::move(family); lookup_ = std::move(lookup);
            }
            out.status = V4GlobalPreviewStatus::progress; out.reason = "v4_family_round_progress"; return out;
        }
        catch (const std::bad_alloc&) { out.status = V4GlobalPreviewStatus::resource_unavailable; out.reason = "v4_family_round_resource_unavailable"; return out; }
        catch (const std::length_error&) { out.status = V4GlobalPreviewStatus::resource_unavailable; out.reason = "v4_family_round_resource_unavailable"; return out; }
        catch (...) { failed_ = true; out.reason = "v4_family_round_unavailable"; return out; }
    }
};

// Replays a persisted selection from the fixed snapshot genesis. The index,
// audit and snapshot are pinned only for this session; external authentication
// of a head/locator and previously open writers remain outside this protocol.
class V4FamilySelectionReplaySession final
{
    V4ShardLocator index_, auditLocator_;
    std::unique_ptr<V4CheckpointReadPin> indexPin_, auditPin_;
    std::string indexBytes_, auditBytes_, binding_;
    V4FamilySelectionAudit audit_;
    V4GlobalPreviewBudgets saved_;
    std::unique_ptr<V4FamilyPreviewRound> round_;
    std::unique_ptr<V4RoundAncestryCheck> ancestry_;
    std::unique_ptr<V4RoundAncestryRecheck> ancestryPost_;
    CheckpointStore* store_ = nullptr;
    std::optional<V4FamilyRoundItem> matched_;
    unsigned recheckPhase_ = 0;
    bool ancestryReady_ = false, ancestryPostReady_ = false, complete_ = false, failed_ = false;
    static bool Same(const V4ShardLocator& a, const V4ShardLocator& b) noexcept
    { return a.name == b.name && a.fileId.VolumeSerialNumber == b.fileId.VolumeSerialNumber &&
        std::memcmp(a.fileId.FileId.Identifier, b.fileId.FileId.Identifier, sizeof(a.fileId.FileId.Identifier)) == 0; }
public:
    V4FamilySelectionReplaySession(const V4FamilySelectionReplaySession&) = delete;
    V4FamilySelectionReplaySession& operator=(const V4FamilySelectionReplaySession&) = delete;
    V4FamilySelectionReplaySession() = default;
    static std::unique_ptr<V4FamilySelectionReplaySession> Begin(CheckpointStore& store,
        const V4ShardLocator& finalHead, std::string_view binding, const V4GlobalPreviewBudgets& limit,
        bool& resourceUnavailable)
    {
        resourceUnavailable = false;
        try
        {
            auto out = std::make_unique<V4FamilySelectionReplaySession>(); out->store_ = &store; out->index_ = finalHead; out->binding_ = std::string(binding);
            out->indexPin_ = store.PinRead(finalHead.name, finalHead.fileId); if (!out->indexPin_) return {};
            auto loaded = store.Read(finalHead.name, finalHead.fileId, binding, limit.indexBytes);
            if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight)
            { resourceUnavailable = loaded.status == CodecStatus::resource_unavailable; return {}; }
            out->indexBytes_ = std::move(loaded.bytes);
            auto page = ReadV4ShardIndexPage(store, V4ShardIndexCursor{finalHead, {}}, binding, limit.indexBytes);
            if (!page.read || page.kind != "family_selection" || !page.data || !page.next.next)
            { resourceUnavailable = page.resourceUnavailable; return {}; }
            out->auditLocator_ = *page.data; out->auditPin_ = store.PinRead(page.data->name, page.data->fileId);
            if (!out->auditPin_) return {};
            auto content = store.Read(page.data->name, page.data->fileId, binding, limit.bytes);
            if (content.status != CodecStatus::decoded || content.recoveredInFlight || !content.state.familySelection)
            { resourceUnavailable = content.status == CodecStatus::resource_unavailable; return {}; }
            out->auditBytes_ = std::move(content.bytes); out->audit_ = *content.state.familySelection;
            V4ShardLocator snapshot, visit;
            if (!V4ShardLocatorFromTokens(out->audit_.snapshotName, out->audit_.snapshotVolume, out->audit_.snapshotFileId, snapshot) ||
                !V4ShardLocatorFromTokens(out->audit_.visitHeadName, out->audit_.visitHeadVolume, out->audit_.visitHeadFileId, visit) ||
                !Same(visit, *page.next.next)) return {};
            const auto& a = out->audit_; auto& b = out->saved_;
            b.coordinate = a.coordinateBudget; b.axes = a.axisBudget; b.token = a.tokenBudget; b.replayWork = a.replayWorkBudget;
            b.bytes = a.byteBudget; b.indexBytes = a.indexByteBudget; b.work = a.workBudget; b.node = a.nodeBudget;
            b.pins = a.pinBudget; b.snapshotBytes = a.snapshotByteBudget;
            const std::array<std::size_t, 24> wanted{b.coordinate.decimalDigits, b.coordinate.tokenBytes, b.coordinate.listEntries,
                b.coordinate.arithmeticSteps, b.axes.logicalMembers, b.axes.fileMembers, b.axes.virtualMembers, b.axes.referenceMembers,
                b.axes.payloadSlots, b.axes.textSlots, b.axes.requestResultSlots, b.axes.fileContentSlots, b.axes.payloadBytes,
                b.axes.directoryNodeSlots, b.axes.directoryRootSlots, b.axes.preparationSteps, b.token, b.replayWork, b.bytes, b.indexBytes,
                b.work, b.node, b.pins, b.snapshotBytes};
            const std::array<std::size_t, 24> maximum{limit.coordinate.decimalDigits, limit.coordinate.tokenBytes, limit.coordinate.listEntries,
                limit.coordinate.arithmeticSteps, limit.axes.logicalMembers, limit.axes.fileMembers, limit.axes.virtualMembers, limit.axes.referenceMembers,
                limit.axes.payloadSlots, limit.axes.textSlots, limit.axes.requestResultSlots, limit.axes.fileContentSlots, limit.axes.payloadBytes,
                limit.axes.directoryNodeSlots, limit.axes.directoryRootSlots, limit.axes.preparationSteps, limit.token, limit.replayWork, limit.bytes, limit.indexBytes,
                limit.work, limit.node, limit.pins, limit.snapshotBytes};
            for (std::size_t i = 0; i < wanted.size(); ++i) if (wanted[i] > maximum[i]) { resourceUnavailable = true; return {}; }
            out->ancestry_ = V4RoundAncestryCheck::Begin(visit, snapshot);
            out->round_ = V4FamilyPreviewRound::Begin(std::move(snapshot)); return out;
        }
        catch (const std::bad_alloc&) { resourceUnavailable = true; return {}; }
        catch (const std::length_error&) { resourceUnavailable = true; return {}; }
        catch (...) { return {}; }
    }
    V4FamilyRoundStep Step(CheckpointStore& store)
    {
        V4FamilyRoundStep out;
        if (failed_ || complete_ || !round_ || store_ != &store) { out.reason = "v4_family_selection_replay_unavailable"; return out; }
        try
        {
            if (!ancestryReady_)
            {
                const auto step = ancestry_->Step(store, binding_, saved_.indexBytes, saved_.work, saved_.pins, saved_.snapshotBytes);
                out.workUsed = step.workUsed; out.reason = step.reason;
                if (step.status == V4ShardLookupStatus::found) { ancestryReady_ = true; out.status = V4GlobalPreviewStatus::progress; }
                else out.status = step.status == V4ShardLookupStatus::progress ? V4GlobalPreviewStatus::progress :
                    step.status == V4ShardLookupStatus::resource_unavailable ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable;
                return out;
            }
            if (!matched_)
            {
                out = round_->Step(store, binding_, saved_);
                if (out.exhausted) { failed_ = true; out.exhausted = false; out.status = V4GlobalPreviewStatus::unavailable; out.reason = "v4_family_selection_ordinal_missing"; return out; }
                if (!out.item) return out;
                const auto& item = *out.item;
                if (item.ordinal != audit_.ordinal) { out.item.reset(); out.status = V4GlobalPreviewStatus::progress; return out; }
                if (item.coordinate != audit_.coordinate || item.scanExhausted != audit_.scanExhausted || item.savedState != audit_.savedState)
                { failed_ = true; out.item.reset(); out.status = V4GlobalPreviewStatus::unavailable; out.reason = "v4_family_selection_replay_mismatch"; return out; }
                matched_ = std::move(out.item); out.item.reset();
            }
            if (!ancestryPostReady_)
            {
                if (!ancestryPost_) ancestryPost_ = V4RoundAncestryRecheck::Begin(*ancestry_);
                if (!ancestryPost_) { failed_ = true; out.status = V4GlobalPreviewStatus::unavailable; out.reason = "v4_family_selection_ancestry_post_unavailable"; return out; }
                const auto step = ancestryPost_->Step(saved_.work - out.workUsed); out.workUsed += step.workUsed; out.reason = step.reason;
                if (step.status == V4ShardLookupStatus::found) ancestryPostReady_ = true;
                else { out.status = step.status == V4ShardLookupStatus::progress ? V4GlobalPreviewStatus::progress :
                    step.status == V4ShardLookupStatus::resource_unavailable ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable; return out; }
            }
            while (recheckPhase_ < 2 && out.workUsed < saved_.work)
            {
                const auto& locator = recheckPhase_ == 0 ? index_ : auditLocator_;
                const auto loaded = store.Read(locator.name, locator.fileId, binding_, recheckPhase_ == 0 ? saved_.indexBytes : saved_.bytes);
                const auto& expected = recheckPhase_ == 0 ? indexBytes_ : auditBytes_;
                ++out.workUsed;
                if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight || loaded.bytes != expected)
                { out.status = loaded.status == CodecStatus::resource_unavailable ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable;
                  failed_ = out.status == V4GlobalPreviewStatus::unavailable; out.reason = "v4_family_selection_recheck_unavailable"; return out; }
                ++recheckPhase_;
            }
            if (recheckPhase_ != 2) { out.status = V4GlobalPreviewStatus::progress; out.reason = "v4_family_selection_recheck_pending"; return out; }
            out.item = *matched_; complete_ = true; out.status = V4GlobalPreviewStatus::family_item;
            out.reason = "v4_family_selection_replayed_not_authenticated"; return out;
        }
        catch (const std::bad_alloc&) { out.item.reset(); out.status = V4GlobalPreviewStatus::resource_unavailable; return out; }
        catch (const std::length_error&) { out.item.reset(); out.status = V4GlobalPreviewStatus::resource_unavailable; return out; }
        catch (...) { failed_ = true; out.item.reset(); out.status = V4GlobalPreviewStatus::unavailable; return out; }
    }
    std::unique_ptr<V4FamilyPreviewRound> TakeRound() noexcept
    { return complete_ && !failed_ ? std::move(round_) : nullptr; }
};

struct V4FamilySelectionPublication
{
    bool staged = false, resourceUnavailable = false;
    std::array<PublishedCheckpoint, 2> publications;
    std::optional<V4ShardLocator> head;
    std::string_view reason;
};
static_assert(std::is_nothrow_move_constructible_v<V4FamilySelectionPublication> &&
    std::is_nothrow_move_assignable_v<V4FamilySelectionPublication>);
inline V4FamilySelectionPublication PublishV4FamilySelection(CheckpointStore& store,
    const V4ShardLocator& previousHead, const V4ShardLocator& snapshot, const V4FamilyRoundItem& item,
    std::string_view binding, const V4GlobalPreviewBudgets& b)
{
    V4FamilySelectionPublication out;
    try
    {
        V4FamilySelectionAudit a;
        a.snapshotName.assign(snapshot.name.begin(), snapshot.name.end()); a.snapshotVolume = std::to_string(snapshot.fileId.VolumeSerialNumber);
        a.snapshotFileId = V4ShardIdHex(snapshot.fileId); a.ordinal = item.ordinal; a.coordinate = item.coordinate;
        a.visitHeadName.assign(previousHead.name.begin(), previousHead.name.end()); a.visitHeadVolume = std::to_string(previousHead.fileId.VolumeSerialNumber);
        a.visitHeadFileId = V4ShardIdHex(previousHead.fileId); a.scanExhausted = item.scanExhausted; a.savedState = item.savedState;
        a.coordinateBudget = b.coordinate; a.axisBudget = b.axes; a.tokenBudget = b.token; a.replayWorkBudget = b.replayWork;
        a.byteBudget = b.bytes; a.indexByteBudget = b.indexBytes; a.workBudget = b.work; a.nodeBudget = b.node;
        a.pinBudget = b.pins; a.snapshotByteBudget = b.snapshotBytes;
        State state; state.binding = std::string(binding); state.familyVersion = FamilyVersion::v4;
        state.v4TypedEnabled = true; state.familySelection = std::move(a);
        const auto expected = EncodeCheckpoint(state, binding, b.bytes);
        if (expected.status != CodecStatus::encoded)
        { out.resourceUnavailable = expected.status == CodecStatus::resource_unavailable; out.reason = "v4_family_selection_encode_unavailable"; return out; }
        out.publications[0] = store.Publish(state, binding, b.bytes);
        if (out.publications[0].status != StoreStatus::published_durability_unverified || !out.publications[0].fileIdentityCaptured || !out.publications[0].renameReturnedSuccess)
        { out.reason = "v4_family_selection_publish_unavailable"; return out; }
        V4ShardLocator shard{out.publications[0].fileName, out.publications[0].fileId};
        const auto read = store.Read(shard.name, shard.fileId, binding, b.bytes);
        if (read.status != CodecStatus::decoded || read.recoveredInFlight || read.bytes != expected.bytes)
        { out.resourceUnavailable = read.status == CodecStatus::resource_unavailable; out.reason = "v4_family_selection_readback_unavailable"; return out; }
        auto index = AppendV4ShardIndex(store, previousHead, shard, "family_selection", binding,
            b.coordinate, b.axes, b.token, b.bytes, b.bytes, b.replayWork, b.bytes, b.indexBytes);
        out.publications[1] = std::move(index.publication);
        if (!index.published || !index.head)
        { out.resourceUnavailable = index.resourceUnavailable; out.reason = "v4_family_selection_index_unavailable"; return out; }
        out.head = std::move(index.head); out.staged = true;
        out.reason = "v4_family_selection_staged_replay_required"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_family_selection_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_family_selection_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_family_selection_observation_unavailable"; return out; }
}

// Publishes exactly one completed round Step as a data shard and one index
// node. The caller must replay its predecessor chain before adopting head.
inline V4FamilySelectionPublication PublishV4FamilyRoundStep(CheckpointStore& store,
    const V4ShardLocator& previousHead, const V4ShardLocator& snapshot, std::string_view stepOrdinal,
    const V4FamilyRoundStep& step, std::string_view binding, const V4GlobalPreviewBudgets& b,
    const V4GlobalPreviewBudgets& storageBudget)
{
    V4FamilySelectionPublication out;
    try
    {
        if (!V4CanonicalDecimal(stepOrdinal) || !b.work || step.workUsed > b.work ||
            (step.status != V4GlobalPreviewStatus::progress && step.status != V4GlobalPreviewStatus::family_item) ||
            (static_cast<bool>(step.item) != (step.status == V4GlobalPreviewStatus::family_item)) ||
            (step.exhausted && step.item))
        { out.reason = "v4_family_round_step_not_publishable"; return out; }
        V4FamilyRoundStepAudit a; auto& s = a.selection;
        s.snapshotName.assign(snapshot.name.begin(), snapshot.name.end()); s.snapshotVolume = std::to_string(snapshot.fileId.VolumeSerialNumber);
        s.snapshotFileId = V4ShardIdHex(snapshot.fileId); s.visitHeadName.assign(previousHead.name.begin(), previousHead.name.end());
        s.visitHeadVolume = std::to_string(previousHead.fileId.VolumeSerialNumber); s.visitHeadFileId = V4ShardIdHex(previousHead.fileId);
        a.stepOrdinal = std::string(stepOrdinal); a.status = step.exhausted ? "exhausted" : step.item ? "family_item" : "progress";
        a.workUsed = step.workUsed; a.exhausted = step.exhausted; a.hasItem = static_cast<bool>(step.item);
        if (step.item) { s.ordinal = step.item->ordinal; s.coordinate = step.item->coordinate;
            s.scanExhausted = step.item->scanExhausted; s.savedState = step.item->savedState; }
        s.coordinateBudget = b.coordinate; s.axisBudget = b.axes; s.tokenBudget = b.token; s.replayWorkBudget = b.replayWork;
        s.byteBudget = b.bytes; s.indexByteBudget = b.indexBytes; s.workBudget = b.work; s.nodeBudget = b.node;
        s.pinBudget = b.pins; s.snapshotByteBudget = b.snapshotBytes;
        State state; state.binding = std::string(binding); state.familyVersion = FamilyVersion::v4;
        state.v4TypedEnabled = true; state.familyRoundStep = std::move(a);
        const auto expected = EncodeCheckpoint(state, binding, storageBudget.bytes);
        if (expected.status != CodecStatus::encoded)
        { out.resourceUnavailable = expected.status == CodecStatus::resource_unavailable; out.reason = "v4_family_round_step_encode_unavailable"; return out; }
        out.publications[0] = store.Publish(state, binding, storageBudget.bytes);
        if (out.publications[0].status != StoreStatus::published_durability_unverified || !out.publications[0].fileIdentityCaptured || !out.publications[0].renameReturnedSuccess)
        { out.reason = "v4_family_round_step_publish_unavailable"; return out; }
        V4ShardLocator shard{out.publications[0].fileName, out.publications[0].fileId};
        const auto read = store.Read(shard.name, shard.fileId, binding, storageBudget.bytes);
        if (read.status != CodecStatus::decoded || read.recoveredInFlight || read.bytes != expected.bytes)
        { out.resourceUnavailable = read.status == CodecStatus::resource_unavailable; out.reason = "v4_family_round_step_readback_unavailable"; return out; }
        auto index = AppendV4ShardIndex(store, previousHead, shard, "family_round_step", binding,
            b.coordinate, b.axes, b.token, storageBudget.bytes, storageBudget.bytes, b.replayWork, storageBudget.bytes, storageBudget.indexBytes);
        out.publications[1] = std::move(index.publication);
        if (!index.published || !index.head)
        { out.resourceUnavailable = index.resourceUnavailable; out.reason = "v4_family_round_step_index_unavailable"; return out; }
        out.head = std::move(index.head); out.staged = true; out.reason = "v4_family_round_step_staged_replay_required"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_family_round_step_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_family_round_step_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_family_round_step_observation_unavailable"; return out; }
}

inline V4FamilySelectionPublication PublishV4FamilySettlement(CheckpointStore& store,
    const V4ShardLocator& selectionHead, const V4ShardLocator& previousHead, const V4FamilyRoundItem& selected,
    std::string_view outcome, const std::optional<V4ShardLocator>& familyShard, const std::optional<V4ShardLocator>& caseShard,
    std::string_view binding, const V4GlobalPreviewBudgets& b)
{
    V4FamilySelectionPublication out;
    const auto same = [](const V4ShardLocator& x, const V4ShardLocator& y) noexcept {
        return x.name == y.name && x.fileId.VolumeSerialNumber == y.fileId.VolumeSerialNumber &&
            std::memcmp(x.fileId.FileId.Identifier, y.fileId.FileId.Identifier, sizeof(x.fileId.FileId.Identifier)) == 0;
    };
    try
    {
        const auto choice = ReadV4ShardIndexPage(store, V4ShardIndexCursor{selectionHead, {}}, binding, b.indexBytes);
        if (!choice.read || choice.kind != "family_round_step" || !choice.data)
        { out.resourceUnavailable = choice.resourceUnavailable; out.reason = "v4_family_settlement_choice_unavailable"; return out; }
        const auto choiceData = ReadV4FamilyRoundStepShard(store, choice.data->name, choice.data->fileId, binding, b.bytes);
        if (!choiceData.read || !choiceData.audit || !choiceData.audit->hasItem ||
            choiceData.audit->selection.coordinate != selected.coordinate || choiceData.audit->selection.ordinal != selected.ordinal ||
            choiceData.audit->selection.scanExhausted != selected.scanExhausted || choiceData.audit->selection.savedState != selected.savedState)
        { out.resourceUnavailable = choiceData.resourceUnavailable; out.reason = "v4_family_settlement_choice_mismatch"; return out; }
        if (outcome == "visited")
        {
            if (selected.scanExhausted || !familyShard) { out.reason = "v4_family_settlement_visit_invalid"; return out; }
            const auto familyIndex = ReadV4ShardIndexPage(store, V4ShardIndexCursor{previousHead, {}}, binding, b.indexBytes);
            if (!familyIndex.read || familyIndex.kind != "family" || !familyIndex.data || !same(*familyIndex.data, *familyShard) || !familyIndex.next.next)
            { out.resourceUnavailable = familyIndex.resourceUnavailable; out.reason = "v4_family_settlement_family_link_unavailable"; return out; }
            const auto family = ReadV4FamilyCursorShard(store, familyShard->name, familyShard->fileId, binding, b.coordinate, b.axes, b.bytes, b.replayWork);
            if (!family.cursorValidated || !family.family || family.family->coordinate != selected.coordinate)
            { out.resourceUnavailable = family.resourceUnavailable; out.reason = "v4_family_settlement_family_unavailable"; return out; }
            auto base = *familyIndex.next.next; auto baseGeneration = familyIndex.next.expectedGeneration;
            if (caseShard)
            {
                const auto caseIndex = ReadV4ShardIndexPage(store, familyIndex.next, binding, b.indexBytes);
                if (!caseIndex.read || caseIndex.kind != "case" || !caseIndex.data || !same(*caseIndex.data, *caseShard) || !caseIndex.next.next)
                { out.resourceUnavailable = caseIndex.resourceUnavailable; out.reason = "v4_family_settlement_case_link_unavailable"; return out; }
                const auto saved = ReadV4CaseShard(store, caseShard->name, caseShard->fileId, binding, b.coordinate, b.axes, b.replayWork, b.bytes);
                if (!saved.identityReplayed || !saved.shard || saved.shard->families.front().coordinate != selected.coordinate ||
                    saved.shard->families.front().candidateCursor != family.family->candidateCursor || saved.shard->families.front().scanExhausted != family.family->scanExhausted)
                { out.resourceUnavailable = saved.resourceUnavailable; out.reason = "v4_family_settlement_case_unavailable"; return out; }
                base = *caseIndex.next.next; baseGeneration = caseIndex.next.expectedGeneration;
            }
            if (!same(base, selectionHead) || baseGeneration != choice.generation)
            { out.reason = "v4_family_settlement_visit_origin_unavailable"; return out; }
        }
        else if ((outcome != "deferred" && outcome != "scan_exhausted") || !same(previousHead, selectionHead) || familyShard || caseShard ||
            ((outcome == "scan_exhausted") != selected.scanExhausted))
        { out.reason = "v4_family_settlement_nonvisit_invalid"; return out; }
        V4FamilySettlementAudit a;
        const auto put = [](const V4ShardLocator& v, std::string& name, std::string& volume, std::string& id) {
            name.assign(v.name.begin(), v.name.end()); volume = std::to_string(v.fileId.VolumeSerialNumber); id = V4ShardIdHex(v.fileId); };
        put(selectionHead, a.selectionName, a.selectionVolume, a.selectionFileId); put(previousHead, a.previousName, a.previousVolume, a.previousFileId);
        a.coordinate = selected.coordinate; a.outcome = std::string(outcome);
        if (familyShard) put(*familyShard, a.familyName, a.familyVolume, a.familyFileId);
        if (caseShard) put(*caseShard, a.caseName, a.caseVolume, a.caseFileId);
        State state; state.binding = std::string(binding); state.familyVersion = FamilyVersion::v4;
        state.v4TypedEnabled = true; state.familySettlement = std::move(a);
        const auto expected = EncodeCheckpoint(state, binding, b.bytes);
        if (expected.status != CodecStatus::encoded)
        { out.resourceUnavailable = expected.status == CodecStatus::resource_unavailable; out.reason = "v4_family_settlement_encode_unavailable"; return out; }
        out.publications[0] = store.Publish(state, binding, b.bytes);
        if (out.publications[0].status != StoreStatus::published_durability_unverified || !out.publications[0].fileIdentityCaptured || !out.publications[0].renameReturnedSuccess)
        { out.reason = "v4_family_settlement_publish_unavailable"; return out; }
        V4ShardLocator shard{out.publications[0].fileName, out.publications[0].fileId};
        const auto read = store.Read(shard.name, shard.fileId, binding, b.bytes);
        if (read.status != CodecStatus::decoded || read.recoveredInFlight || read.bytes != expected.bytes)
        { out.resourceUnavailable = read.status == CodecStatus::resource_unavailable; out.reason = "v4_family_settlement_readback_unavailable"; return out; }
        auto index = AppendV4ShardIndex(store, previousHead, shard, "family_settlement", binding,
            b.coordinate, b.axes, b.token, b.bytes, b.bytes, b.replayWork, b.bytes, b.indexBytes);
        out.publications[1] = std::move(index.publication);
        if (!index.published || !index.head)
        { out.resourceUnavailable = index.resourceUnavailable; out.reason = "v4_family_settlement_index_unavailable"; return out; }
        out.head = std::move(index.head); out.staged = true; out.reason = "v4_family_settlement_staged_unverified"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_family_settlement_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_family_settlement_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_family_settlement_unavailable"; return out; }
}

// Rebuild a family round from its committed step chain. Each call budgets
// index/data reads or one authoritative Step; the pins are process-local and
// do not authenticate an external locator or exclude pre-existing writers.
class V4FamilyRoundJournalReplay final
{
    struct Snapshot { V4ShardLocator locator; std::unique_ptr<V4CheckpointReadPin> pin; std::string bytes; };
    V4ShardIndexCursor scan_;
    std::optional<V4ShardLocator> snapshot_;
    std::vector<Snapshot> snapshots_;
    std::vector<V4FamilyRoundStepAudit> reverse_;
    std::unique_ptr<V4FamilyPreviewRound> round_;
    std::size_t bytes_ = 0, replayIndex_ = 0, recheckIndex_ = 0, roundPins_ = 0, roundBytes_ = 0;
    CheckpointStore* store_ = nullptr;
    std::string binding_;
    bool first_ = true, scanning_ = true, replayed_ = false, complete_ = false, failed_ = false;
    explicit V4FamilyRoundJournalReplay(V4ShardLocator head) : scan_{std::move(head), {}} {}
    static bool Same(const V4ShardLocator& a, const V4ShardLocator& b) noexcept
    { return a.name == b.name && a.fileId.VolumeSerialNumber == b.fileId.VolumeSerialNumber &&
        std::memcmp(a.fileId.FileId.Identifier, b.fileId.FileId.Identifier, sizeof(a.fileId.FileId.Identifier)) == 0; }
    static V4GlobalPreviewBudgets Budget(const V4FamilySelectionAudit& a) noexcept
    {
        V4GlobalPreviewBudgets b; b.coordinate = a.coordinateBudget; b.axes = a.axisBudget; b.token = a.tokenBudget;
        b.replayWork = a.replayWorkBudget; b.bytes = a.byteBudget; b.indexBytes = a.indexByteBudget;
        b.work = a.workBudget; b.node = a.nodeBudget; b.pins = a.pinBudget; b.snapshotBytes = a.snapshotByteBudget; return b;
    }
    static bool Fits(const V4GlobalPreviewBudgets& b, const V4GlobalPreviewBudgets& limit) noexcept
    {
        const std::array<std::size_t, 24> a{b.coordinate.decimalDigits, b.coordinate.tokenBytes, b.coordinate.listEntries,
            b.coordinate.arithmeticSteps, b.axes.logicalMembers, b.axes.fileMembers, b.axes.virtualMembers, b.axes.referenceMembers,
            b.axes.payloadSlots, b.axes.textSlots, b.axes.requestResultSlots, b.axes.fileContentSlots, b.axes.payloadBytes,
            b.axes.directoryNodeSlots, b.axes.directoryRootSlots, b.axes.preparationSteps, b.token, b.replayWork, b.bytes, b.indexBytes, b.work, b.node, b.pins, b.snapshotBytes};
        const std::array<std::size_t, 24> z{limit.coordinate.decimalDigits, limit.coordinate.tokenBytes, limit.coordinate.listEntries,
            limit.coordinate.arithmeticSteps, limit.axes.logicalMembers, limit.axes.fileMembers, limit.axes.virtualMembers, limit.axes.referenceMembers,
            limit.axes.payloadSlots, limit.axes.textSlots, limit.axes.requestResultSlots, limit.axes.fileContentSlots, limit.axes.payloadBytes,
            limit.axes.directoryNodeSlots, limit.axes.directoryRootSlots, limit.axes.preparationSteps, limit.token, limit.replayWork, limit.bytes, limit.indexBytes, limit.work, limit.node, limit.pins, limit.snapshotBytes};
        for (std::size_t i = 0; i < a.size(); ++i) if (a[i] > z[i]) return false;
        return true;
    }
public:
    V4FamilyRoundJournalReplay(const V4FamilyRoundJournalReplay&) = delete;
    V4FamilyRoundJournalReplay& operator=(const V4FamilyRoundJournalReplay&) = delete;
    static std::unique_ptr<V4FamilyRoundJournalReplay> Begin(V4ShardLocator head)
    { return std::unique_ptr<V4FamilyRoundJournalReplay>(new V4FamilyRoundJournalReplay(std::move(head))); }
    V4FamilyRoundStep Step(CheckpointStore& store, std::string_view binding, const V4GlobalPreviewBudgets& limit)
    {
        V4FamilyRoundStep out;
        try
        {
            if (failed_ || complete_ || (store_ && (store_ != &store || binding_ != binding)))
            { out.reason = "v4_family_journal_replay_unavailable"; return out; }
            if (!store_) { binding_ = std::string(binding); store_ = &store; }
            if (!limit.work || roundPins_ > limit.pins || roundBytes_ > limit.snapshotBytes ||
                snapshots_.size() > limit.pins - roundPins_ || bytes_ > limit.snapshotBytes - roundBytes_)
            { out.status = V4GlobalPreviewStatus::resource_unavailable; out.reason = "v4_family_journal_budget_unavailable"; return out; }
            bool resource = false;
            const auto pin = [&](const V4ShardLocator& locator, std::size_t budget) {
                const auto found = std::find_if(snapshots_.begin(), snapshots_.end(), [&](const auto& e) { return Same(e.locator, locator); });
                if (found != snapshots_.end()) return true;
                if (snapshots_.size() >= limit.pins) { resource = true; return false; }
                auto held = store.PinRead(locator.name, locator.fileId); if (!held) return false;
                auto read = store.Read(locator.name, locator.fileId, binding, budget);
                if (read.status != CodecStatus::decoded || read.recoveredInFlight)
                { resource = read.status == CodecStatus::resource_unavailable; return false; }
                if (bytes_ > limit.snapshotBytes || read.bytes.size() > limit.snapshotBytes - bytes_) { resource = true; return false; }
                const auto n = read.bytes.size(); snapshots_.push_back({locator, std::move(held), std::move(read.bytes)}); bytes_ += n; return true;
            };
            while (scanning_ && out.workUsed < limit.work)
            {
                struct Rollback final {
                    std::vector<V4FamilyRoundStepAudit>& entries; std::size_t size; bool committed = false;
                    ~Rollback() { if (!committed) entries.resize(size); }
                } rollback{reverse_, reverse_.size()};
                if (!scan_.next) { failed_ = true; out.reason = "v4_family_journal_snapshot_missing"; return out; }
                const auto current = *scan_.next;
                if (!pin(current, limit.indexBytes)) { out.status = resource ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable; return out; }
                const auto page = ReadV4ShardIndexPage(store, scan_, binding, limit.indexBytes);
                if (!page.read || !page.data) { out.status = page.resourceUnavailable ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable; return out; }
                ++out.workUsed;
                if (first_ && page.kind != "family_round_step") { failed_ = true; out.reason = "v4_family_journal_head_not_step"; return out; }
                if (page.kind == "family_round_step")
                {
                    if (!pin(*page.data, limit.bytes)) { out.status = resource ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable; return out; }
                    const auto data = ReadV4FamilyRoundStepShard(store, page.data->name, page.data->fileId, binding, limit.bytes);
                    if (!data.read || !data.audit) { out.status = data.resourceUnavailable ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable; return out; }
                    V4ShardLocator fixed, previous; const auto& s = data.audit->selection;
                    if (!V4ShardLocatorFromTokens(s.snapshotName, s.snapshotVolume, s.snapshotFileId, fixed) ||
                        !V4ShardLocatorFromTokens(s.visitHeadName, s.visitHeadVolume, s.visitHeadFileId, previous) ||
                        !page.next.next || !Same(previous, *page.next.next))
                    { failed_ = true; out.reason = "v4_family_journal_step_link_invalid"; return out; }
                    if (first_) snapshot_ = fixed;
                    if (snapshot_ && Same(fixed, *snapshot_)) reverse_.push_back(*data.audit);
                }
                const bool atSnapshot = snapshot_ && Same(current, *snapshot_);
                auto next = page.next; std::unique_ptr<V4FamilyPreviewRound> initial;
                if (atSnapshot)
                {
                    std::string expected = "0";
                    for (auto it = reverse_.rbegin(); it != reverse_.rend(); ++it)
                    { if (it->stepOrdinal != expected) { failed_ = true; out.reason = "v4_family_journal_step_sequence_invalid"; return out; } Increment(expected); }
                    if (reverse_.empty()) { failed_ = true; out.reason = "v4_family_journal_empty"; return out; }
                    initial = V4FamilyPreviewRound::Begin(*snapshot_);
                }
                static_assert(std::is_nothrow_move_assignable_v<V4ShardIndexCursor>);
                scan_ = std::move(next); first_ = false; rollback.committed = true;
                if (atSnapshot) { scanning_ = false; round_ = std::move(initial); break; }
            }
            if (scanning_) { out.status = V4GlobalPreviewStatus::progress; out.reason = "v4_family_journal_scan_pending"; return out; }
            if (!replayed_)
            {
                if (replayIndex_ == reverse_.size()) { replayed_ = true; }
                else
                {
                    const auto& expected = reverse_[reverse_.size() - 1 - replayIndex_]; const auto budget = Budget(expected.selection);
                    const auto reservePins = (std::max)(roundPins_, budget.pins), reserveBytes = (std::max)(roundBytes_, budget.snapshotBytes);
                    if (!Fits(budget, limit) || reservePins > limit.pins || reserveBytes > limit.snapshotBytes ||
                        snapshots_.size() > limit.pins - reservePins || bytes_ > limit.snapshotBytes - reserveBytes)
                    { out.status = V4GlobalPreviewStatus::resource_unavailable; out.reason = "v4_family_journal_original_budget_unavailable"; return out; }
                    if (budget.work > limit.work - out.workUsed)
                    { out.status = V4GlobalPreviewStatus::progress; out.reason = "v4_family_journal_replay_next_step_pending"; return out; }
                    if (!round_) round_ = V4FamilyPreviewRound::Begin(*snapshot_);
                    auto actual = round_->Step(store, binding, budget); out.workUsed += actual.workUsed;
                    const auto status = actual.exhausted ? "exhausted" : actual.item ? "family_item" : "progress";
                    if (actual.status == V4GlobalPreviewStatus::resource_unavailable || actual.status == V4GlobalPreviewStatus::unavailable)
                    { round_.reset(); replayIndex_ = 0; roundPins_ = roundBytes_ = 0; out.status = actual.status; out.reason = "v4_family_journal_step_replay_unavailable"; return out; }
                    if ((actual.status != V4GlobalPreviewStatus::progress && actual.status != V4GlobalPreviewStatus::family_item) ||
                        expected.status != status || actual.workUsed != expected.workUsed || actual.exhausted != expected.exhausted ||
                        static_cast<bool>(actual.item) != expected.hasItem || (actual.item &&
                            (actual.item->ordinal != expected.selection.ordinal || actual.item->coordinate != expected.selection.coordinate ||
                             actual.item->scanExhausted != expected.selection.scanExhausted || actual.item->savedState != expected.selection.savedState)))
                    { failed_ = true; out.reason = "v4_family_journal_step_replay_mismatch"; return out; }
                    roundPins_ = reservePins; roundBytes_ = reserveBytes;
                    ++replayIndex_; out.status = V4GlobalPreviewStatus::progress; out.reason = "v4_family_journal_replay_pending"; return out;
                }
            }
            while (recheckIndex_ < snapshots_.size() && out.workUsed < limit.work)
            {
                const auto& entry = snapshots_[recheckIndex_]; const auto loaded = store.Read(entry.locator.name, entry.locator.fileId, binding, entry.bytes.size());
                ++out.workUsed;
                if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight || loaded.bytes != entry.bytes)
                { out.status = loaded.status == CodecStatus::resource_unavailable ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable;
                  if (out.status == V4GlobalPreviewStatus::unavailable) failed_ = true; return out; }
                ++recheckIndex_;
            }
            if (recheckIndex_ != snapshots_.size()) { out.status = V4GlobalPreviewStatus::progress; out.reason = "v4_family_journal_recheck_pending"; return out; }
            const auto& last = reverse_.front();
            if (last.hasItem) out.item = V4FamilyRoundItem{last.selection.coordinate, last.selection.scanExhausted, last.selection.savedState, last.selection.ordinal};
            out.exhausted = last.exhausted; out.status = out.item ? V4GlobalPreviewStatus::family_item : V4GlobalPreviewStatus::progress;
            complete_ = true; out.reason = "v4_family_journal_replayed_not_authenticated"; return out;
        }
        catch (const std::bad_alloc&) { out.status = V4GlobalPreviewStatus::resource_unavailable; out.reason = "v4_family_journal_resource_unavailable"; return out; }
        catch (const std::length_error&) { out.status = V4GlobalPreviewStatus::resource_unavailable; out.reason = "v4_family_journal_resource_unavailable"; return out; }
        catch (...) { failed_ = true; out.reason = "v4_family_journal_observation_unavailable"; return out; }
    }
    std::unique_ptr<V4FamilyPreviewRound> TakeRound() noexcept
    { return complete_ && !failed_ ? std::move(round_) : nullptr; }
    bool Complete() const noexcept { return complete_ && !failed_; }
    std::optional<V4FamilyRoundItem> LastItem() const
    {
        if (!Complete() || reverse_.empty() || !reverse_.front().hasItem) return {};
        const auto& s = reverse_.front().selection;
        return V4FamilyRoundItem{s.coordinate, s.scanExhausted, s.savedState, s.ordinal};
    }
    std::size_t RetainedPins() const noexcept
    { return snapshots_.size() > (std::numeric_limits<std::size_t>::max)() - roundPins_ ? (std::numeric_limits<std::size_t>::max)() : snapshots_.size() + roundPins_; }
    std::size_t RetainedBytes() const noexcept
    { return bytes_ > (std::numeric_limits<std::size_t>::max)() - roundBytes_ ? (std::numeric_limits<std::size_t>::max)() : bytes_ + roundBytes_; }
};

enum class V4FamilySettlementRecoveryStatus { progress, consistent_unverified, resource_unavailable, unavailable };
struct V4FamilySettlementRecoveryStep
{
    V4FamilySettlementRecoveryStatus status = V4FamilySettlementRecoveryStatus::unavailable;
    std::size_t workUsed = 0;
    std::string_view reason;
};
// Read-only recovery of one settlement. The complete index/selection/cursor
// chain is replayed, but bytes and locators still require external authority.
class V4FamilySettlementRecoverySession final
{
    struct Snapshot { V4ShardLocator locator; std::unique_ptr<V4CheckpointReadPin> pin; std::string bytes; };
    enum class Phase { index, audit, selection, family_index, family_data, case_index, case_data, selection_generation, lookup, old_family, lookup_recheck, snapshots, done, failed };
    V4ShardLocator head_, selection_, previous_, family_, case_;
    std::optional<V4ShardLocator> data_;
    V4ShardIndexCursor previousCursor_;
    std::optional<V4ShardIndexCursor> familyIndexNext_;
    std::optional<V4FamilySettlementAudit> audit_;
    std::optional<V4FamilyRoundItem> item_;
    std::string beforeCursor_, afterCursor_, selectionExpectedGeneration_;
    bool afterExhausted_ = false;
    std::vector<Snapshot> snapshots_;
    std::size_t bytes_ = 0, recheck_ = 0;
    CheckpointStore* store_ = nullptr;
    std::string binding_;
    std::unique_ptr<V4FamilyRoundJournalReplay> selectionReplay_;
    std::unique_ptr<V4ShardFamilyLookup> lookup_;
    std::unique_ptr<V4ShardFamilyRecheck> lookupRecheck_;
    Phase phase_ = Phase::index;
    explicit V4FamilySettlementRecoverySession(V4ShardLocator head) : head_(std::move(head)) {}
    static bool Same(const V4ShardLocator& a, const V4ShardLocator& b) noexcept
    { return a.name == b.name && a.fileId.VolumeSerialNumber == b.fileId.VolumeSerialNumber &&
        std::memcmp(a.fileId.FileId.Identifier, b.fileId.FileId.Identifier, sizeof(a.fileId.FileId.Identifier)) == 0; }
public:
    V4FamilySettlementRecoverySession(const V4FamilySettlementRecoverySession&) = delete;
    V4FamilySettlementRecoverySession& operator=(const V4FamilySettlementRecoverySession&) = delete;
    static std::unique_ptr<V4FamilySettlementRecoverySession> Begin(V4ShardLocator head)
    { return std::unique_ptr<V4FamilySettlementRecoverySession>(new V4FamilySettlementRecoverySession(std::move(head))); }
    const V4FamilySettlementAudit* Audit() const noexcept { return phase_ == Phase::done ? &*audit_ : nullptr; }
    const V4FamilyRoundItem* Item() const noexcept { return phase_ == Phase::done ? &*item_ : nullptr; }
    std::unique_ptr<V4FamilyPreviewRound> TakeRound() noexcept
    { return phase_ == Phase::done && selectionReplay_ ? selectionReplay_->TakeRound() : nullptr; }
    V4FamilySettlementRecoveryStep Step(CheckpointStore& store, std::string_view binding, const V4GlobalPreviewBudgets& limit)
    {
        V4FamilySettlementRecoveryStep out;
        try
        {
            if (phase_ == Phase::done || phase_ == Phase::failed || (store_ && (store_ != &store || binding_ != binding)))
            { out.reason = "v4_settlement_recovery_unavailable"; return out; }
            if (!store_) { binding_ = std::string(binding); store_ = &store; }
            const auto selectionPins = selectionReplay_ ? selectionReplay_->RetainedPins() : 0;
            const auto selectionBytes = selectionReplay_ ? selectionReplay_->RetainedBytes() : 0;
            const auto lookupPins = lookup_ ? lookup_->RetainedPins() : 0;
            const auto lookupBytes = lookup_ ? lookup_->RetainedBytes() : 0;
            if (!limit.work || selectionPins > limit.pins || lookupPins > limit.pins - selectionPins ||
                snapshots_.size() > limit.pins - selectionPins - lookupPins || selectionBytes > limit.snapshotBytes ||
                lookupBytes > limit.snapshotBytes - selectionBytes || bytes_ > limit.snapshotBytes - selectionBytes - lookupBytes)
            { out.status = V4FamilySettlementRecoveryStatus::resource_unavailable; out.reason = "v4_settlement_recovery_budget_unavailable"; return out; }
            auto remaining = limit; remaining.pins -= snapshots_.size() + lookupPins;
            remaining.snapshotBytes -= bytes_ + lookupBytes;
            bool resource = false;
            const auto pin = [&](const V4ShardLocator& locator, std::size_t budget) {
                const auto found = std::find_if(snapshots_.begin(), snapshots_.end(), [&](const auto& e) { return Same(e.locator, locator); });
                if (found != snapshots_.end()) return true;
                if (snapshots_.size() >= limit.pins - selectionPins - lookupPins) { resource = true; return false; }
                auto held = store.PinRead(locator.name, locator.fileId); if (!held) return false;
                auto loaded = store.Read(locator.name, locator.fileId, binding, budget);
                if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight)
                { resource = loaded.status == CodecStatus::resource_unavailable; return false; }
                const auto available = limit.snapshotBytes - selectionBytes - lookupBytes;
                if (bytes_ > available || loaded.bytes.size() > available - bytes_) { resource = true; return false; }
                const auto n = loaded.bytes.size(); snapshots_.push_back({locator, std::move(held), std::move(loaded.bytes)}); bytes_ += n; return true;
            };
            const auto pinFailure = [&] { out.status = resource ? V4FamilySettlementRecoveryStatus::resource_unavailable : V4FamilySettlementRecoveryStatus::unavailable;
                out.reason = "v4_settlement_recovery_pin_unavailable"; return out; };
            if (phase_ == Phase::index)
            {
                if (!pin(head_, limit.indexBytes)) return pinFailure();
                const auto page = ReadV4ShardIndexPage(store, V4ShardIndexCursor{head_, {}}, binding, limit.indexBytes); out.workUsed = 1;
                if (!page.read || page.kind != "family_settlement" || !page.data || !page.next.next)
                { out.status = page.resourceUnavailable ? V4FamilySettlementRecoveryStatus::resource_unavailable : V4FamilySettlementRecoveryStatus::unavailable; return out; }
                data_ = *page.data; previous_ = *page.next.next; previousCursor_ = page.next; phase_ = Phase::audit;
            }
            else if (phase_ == Phase::audit)
            {
                if (!pin(*data_, limit.bytes)) return pinFailure();
                const auto loaded = ReadV4FamilySettlementShard(store, data_->name, data_->fileId, binding, limit.bytes); out.workUsed = 1;
                if (!loaded.read || !loaded.audit) { out.status = loaded.resourceUnavailable ? V4FamilySettlementRecoveryStatus::resource_unavailable : V4FamilySettlementRecoveryStatus::unavailable; return out; }
                V4ShardLocator selection, previous, family, file;
                const auto& a = *loaded.audit;
                if (!V4ShardLocatorFromTokens(a.selectionName, a.selectionVolume, a.selectionFileId, selection) ||
                    !V4ShardLocatorFromTokens(a.previousName, a.previousVolume, a.previousFileId, previous) || !Same(previous, previous_) ||
                    (a.outcome == "visited" && !V4ShardLocatorFromTokens(a.familyName, a.familyVolume, a.familyFileId, family)) ||
                    (!a.caseName.empty() && !V4ShardLocatorFromTokens(a.caseName, a.caseVolume, a.caseFileId, file)))
                { phase_ = Phase::failed; out.reason = "v4_settlement_recovery_locator_mismatch"; return out; }
                auto replay = V4FamilyRoundJournalReplay::Begin(selection);
                audit_ = *loaded.audit; selection_ = std::move(selection); family_ = std::move(family); case_ = std::move(file);
                selectionReplay_ = std::move(replay); phase_ = Phase::selection;
            }
            else if (phase_ == Phase::selection)
            {
                if (!selectionReplay_->Complete())
                {
                    const auto replay = selectionReplay_->Step(store, binding, remaining); out.workUsed = replay.workUsed;
                    if (!selectionReplay_->Complete())
                    { out.status = replay.status == V4GlobalPreviewStatus::progress ? V4FamilySettlementRecoveryStatus::progress :
                        replay.status == V4GlobalPreviewStatus::resource_unavailable ? V4FamilySettlementRecoveryStatus::resource_unavailable : V4FamilySettlementRecoveryStatus::unavailable;
                      out.reason = replay.reason; return out; }
                }
                const auto selected = selectionReplay_->LastItem();
                if (!selected || selected->coordinate != audit_->coordinate ||
                    (audit_->outcome == "scan_exhausted") != selected->scanExhausted ||
                    (audit_->outcome != "visited" && !Same(selection_, previous_)))
                { phase_ = Phase::failed; out.reason = "v4_settlement_recovery_selection_mismatch"; return out; }
                item_ = *selected;
                if (audit_->outcome != "visited") selectionExpectedGeneration_ = previousCursor_.expectedGeneration;
                phase_ = audit_->outcome == "visited" ? Phase::family_index : Phase::selection_generation;
            }
            else if (phase_ == Phase::family_index)
            {
                if (!pin(previous_, limit.indexBytes)) return pinFailure();
                const auto page = ReadV4ShardIndexPage(store, previousCursor_, binding, limit.indexBytes); out.workUsed = 1;
                if (!page.read || page.kind != "family" || !page.data || !Same(*page.data, family_) || !page.next.next)
                { out.status = page.resourceUnavailable ? V4FamilySettlementRecoveryStatus::resource_unavailable : V4FamilySettlementRecoveryStatus::unavailable; return out; }
                familyIndexNext_ = page.next;
                if (audit_->caseName.empty() && !Same(*familyIndexNext_->next, selection_)) { phase_ = Phase::failed; out.reason = "v4_settlement_recovery_family_origin_mismatch"; return out; }
                if (audit_->caseName.empty()) selectionExpectedGeneration_ = familyIndexNext_->expectedGeneration;
                phase_ = Phase::family_data;
            }
            else if (phase_ == Phase::family_data)
            {
                if (!pin(family_, limit.bytes)) return pinFailure();
                const auto family = ReadV4FamilyCursorShard(store, family_.name, family_.fileId, binding, limit.coordinate, limit.axes, limit.bytes, limit.replayWork); out.workUsed = 1;
                if (!family.cursorValidated || !family.family || family.family->coordinate != audit_->coordinate)
                { out.status = family.resourceUnavailable ? V4FamilySettlementRecoveryStatus::resource_unavailable : V4FamilySettlementRecoveryStatus::unavailable; return out; }
                auto after = family.family->candidateCursor; const auto exhausted = family.family->scanExhausted;
                if (audit_->caseName.empty())
                {
                    if (family.family->pruneSteps.size() != 1) { phase_ = Phase::failed; out.reason = "v4_settlement_recovery_step_audit_missing"; return out; }
                    beforeCursor_ = family.family->pruneSteps.front().beforeCursor;
                }
                afterCursor_ = std::move(after); afterExhausted_ = exhausted;
                phase_ = audit_->caseName.empty() ? Phase::selection_generation : Phase::case_index;
            }
            else if (phase_ == Phase::case_index)
            {
                if (!pin(*familyIndexNext_->next, limit.indexBytes)) return pinFailure();
                const auto page = ReadV4ShardIndexPage(store, *familyIndexNext_, binding, limit.indexBytes); out.workUsed = 1;
                if (!page.read || page.kind != "case" || !page.data || !Same(*page.data, case_) || !page.next.next || !Same(*page.next.next, selection_))
                { out.status = page.resourceUnavailable ? V4FamilySettlementRecoveryStatus::resource_unavailable : V4FamilySettlementRecoveryStatus::unavailable; return out; }
                selectionExpectedGeneration_ = page.next.expectedGeneration;
                phase_ = Phase::case_data;
            }
            else if (phase_ == Phase::case_data)
            {
                if (!pin(case_, limit.bytes)) return pinFailure();
                const auto saved = ReadV4CaseShard(store, case_.name, case_.fileId, binding, limit.coordinate, limit.axes, limit.replayWork, limit.bytes); out.workUsed = 1;
                if (!saved.identityReplayed || !saved.shard || saved.shard->families.front().coordinate != audit_->coordinate ||
                    saved.shard->families.front().candidateCursor != afterCursor_ || saved.shard->families.front().scanExhausted != afterExhausted_)
                { out.status = saved.resourceUnavailable ? V4FamilySettlementRecoveryStatus::resource_unavailable : V4FamilySettlementRecoveryStatus::unavailable; return out; }
                beforeCursor_ = saved.shard->families.front().cases.front().beforeCursor; phase_ = Phase::selection_generation;
            }
            else if (phase_ == Phase::selection_generation)
            {
                if (selectionExpectedGeneration_.empty()) { phase_ = Phase::failed; out.reason = "v4_settlement_recovery_generation_missing"; return out; }
                if (!pin(selection_, limit.indexBytes)) return pinFailure();
                const auto page = ReadV4ShardIndexPage(store, V4ShardIndexCursor{selection_, selectionExpectedGeneration_}, binding, limit.indexBytes); out.workUsed = 1;
                if (!page.read || page.kind != "family_round_step")
                { out.status = page.resourceUnavailable ? V4FamilySettlementRecoveryStatus::resource_unavailable : V4FamilySettlementRecoveryStatus::unavailable; return out; }
                phase_ = audit_->outcome == "visited" ? Phase::lookup : Phase::snapshots;
            }
            else if (phase_ == Phase::lookup)
            {
                if (!lookup_) lookup_ = V4ShardFamilyLookup::Begin(selection_, audit_->coordinate);
                const auto step = lookup_->Step(store, binding, limit.coordinate, limit.axes, limit.bytes, limit.indexBytes, limit.work,
                    limit.replayWork, limit.pins - snapshots_.size() - selectionPins, limit.snapshotBytes - bytes_ - selectionBytes);
                out.workUsed = step.workUsed;
                if (step.status == V4ShardLookupStatus::found && lookup_->Matched()) phase_ = Phase::old_family;
                else { out.status = step.status == V4ShardLookupStatus::progress ? V4FamilySettlementRecoveryStatus::progress :
                    step.status == V4ShardLookupStatus::resource_unavailable ? V4FamilySettlementRecoveryStatus::resource_unavailable : V4FamilySettlementRecoveryStatus::unavailable;
                    out.reason = step.reason; return out; }
            }
            else if (phase_ == Phase::old_family)
            {
                const auto old = ReadV4FamilyCursorShard(store, lookup_->Matched()->name, lookup_->Matched()->fileId, binding, limit.coordinate, limit.axes, limit.bytes, limit.replayWork); out.workUsed = 1;
                if (!old.cursorValidated || !old.family || old.family->coordinate != audit_->coordinate || old.family->candidateCursor != beforeCursor_ || old.family->scanExhausted)
                { out.status = old.resourceUnavailable ? V4FamilySettlementRecoveryStatus::resource_unavailable : V4FamilySettlementRecoveryStatus::unavailable; return out; }
                lookupRecheck_ = V4ShardFamilyRecheck::Begin(*lookup_);
                if (!lookupRecheck_) { phase_ = Phase::failed; out.reason = "v4_settlement_recovery_lookup_recheck_missing"; return out; }
                phase_ = Phase::lookup_recheck;
            }
            else if (phase_ == Phase::lookup_recheck)
            {
                const auto check = lookupRecheck_->Step(limit.work); out.workUsed = check.workUsed;
                if (check.status == V4ShardLookupStatus::found) phase_ = Phase::snapshots;
                else { out.status = check.status == V4ShardLookupStatus::progress ? V4FamilySettlementRecoveryStatus::progress :
                    check.status == V4ShardLookupStatus::resource_unavailable ? V4FamilySettlementRecoveryStatus::resource_unavailable : V4FamilySettlementRecoveryStatus::unavailable;
                    out.reason = check.reason; return out; }
            }
            else if (phase_ == Phase::snapshots)
            {
                while (recheck_ < snapshots_.size() && out.workUsed < limit.work)
                {
                    const auto& entry = snapshots_[recheck_]; const auto read = store.Read(entry.locator.name, entry.locator.fileId, binding, entry.bytes.size()); ++out.workUsed;
                    if (read.status != CodecStatus::decoded || read.recoveredInFlight || read.bytes != entry.bytes)
                    { out.status = read.status == CodecStatus::resource_unavailable ? V4FamilySettlementRecoveryStatus::resource_unavailable : V4FamilySettlementRecoveryStatus::unavailable;
                      if (out.status == V4FamilySettlementRecoveryStatus::unavailable) phase_ = Phase::failed; return out; }
                    ++recheck_;
                }
                if (recheck_ == snapshots_.size())
                { phase_ = Phase::done; out.status = V4FamilySettlementRecoveryStatus::consistent_unverified; out.reason = "v4_settlement_chain_consistent_not_authenticated"; return out; }
            }
            out.status = V4FamilySettlementRecoveryStatus::progress; out.reason = "v4_settlement_recovery_pending"; return out;
        }
        catch (const std::bad_alloc&) { out.status = V4FamilySettlementRecoveryStatus::resource_unavailable; out.reason = "v4_settlement_recovery_resource_unavailable"; return out; }
        catch (const std::length_error&) { out.status = V4FamilySettlementRecoveryStatus::resource_unavailable; out.reason = "v4_settlement_recovery_resource_unavailable"; return out; }
        catch (...) { phase_ = Phase::failed; out.reason = "v4_settlement_recovery_observation_unavailable"; return out; }
    }
};

struct V4FreshPreviewStart;
V4FreshPreviewStart StartV4FreshPreview(HANDLE, const FILE_ID_INFO&, std::string_view, const V4GlobalPreviewBudgets&);
class V4FreshPreviewDriver final
{
    friend V4FreshPreviewStart StartV4FreshPreview(HANDLE, const FILE_ID_INFO&, std::string_view, const V4GlobalPreviewBudgets&);
    std::unique_ptr<CheckpointStore> store_;
    std::string binding_;
    V4ShardLocator head_;
    std::unique_ptr<V4GlobalPreviewCycle> global_;
    std::unique_ptr<V4FamilyPreviewCycle> family_;
    std::unique_ptr<V4FamilyPreviewRound> familyRound_;
    std::optional<V4FamilyRoundItem> pendingAutoFamily_, lastAutoFamily_;
    std::optional<V4ShardLocator> lastCaseShard_;
    std::optional<V4FamilyRoundStep> journalStep_;
    std::optional<V4GlobalPreviewBudgets> journalBudgets_;
    std::optional<V4FamilySelectionPublication> journalPublication_;
    std::unique_ptr<V4FamilyRoundJournalReplay> journalReplay_;
    std::unique_ptr<V4FamilyRoundJournalReplay> resumeReplay_;
    std::optional<V4ShardLocator> lastJournalHead_;
    std::optional<V4ShardLocator> pendingSelectionHead_, settlementData_;
    std::optional<V4FamilySelectionPublication> settlementPublication_;
    std::unique_ptr<V4FamilyRoundJournalReplay> settlementReplay_;
    std::unique_ptr<V4CheckpointReadPin> settlementHeadPin_, settlementDataPin_;
    std::string settlementHeadBytes_, settlementDataBytes_;
    std::string_view settlementOutcome_;
    unsigned settlementRecheckPhase_ = 0;
    bool settlementReplayDone_ = false, settlementReady_ = false, settlementStopped_ = false;
    std::string journalOrdinal_ = "0";
    bool journalReady_ = false, journalStopped_ = false, journalNeedsRecovery_ = false;
    bool globalBoundary_ = true;
    bool automaticFamilyTurn_ = false;
    V4FreshPreviewDriver(std::unique_ptr<CheckpointStore> store, std::string binding, V4ShardLocator head)
        : store_(std::move(store)), binding_(std::move(binding)), head_(head), global_(V4GlobalPreviewCycle::Begin(std::move(head))) {}
public:
    V4FreshPreviewDriver(const V4FreshPreviewDriver&) = delete;
    V4FreshPreviewDriver& operator=(const V4FreshPreviewDriver&) = delete;
    CheckpointStore& Store() const noexcept { return *store_; }
    const V4ShardLocator& Head() const noexcept { return head_; }
    const V4GlobalPreviewCycle& Global() const noexcept { return *global_; }
    const V4FamilyPreviewCycle* Family() const noexcept { return family_.get(); }
    const std::optional<V4FamilyRoundItem>& LastAutomaticFamily() const noexcept { return lastAutoFamily_; }
    const std::optional<V4ShardLocator>& LastCaseShard() const noexcept { return lastCaseShard_; }
    const V4FamilySelectionPublication* FamilyRoundPublication() const noexcept
    { return journalPublication_ ? &*journalPublication_ : nullptr; }
    const V4FamilySelectionPublication* SettlementPublication() const noexcept
    { return settlementPublication_ ? &*settlementPublication_ : nullptr; }
    V4PreviewHeadAnchorResult PublishHeadAnchor(const CheckpointLocator& locator,
        std::size_t headByteBudget, std::size_t anchorByteBudget)
    {
        if (!globalBoundary_ || !global_->CanRebase(head_) || family_ || journalStep_ || journalPublication_ ||
            settlementPublication_ || !settlementOutcome_.empty() || journalNeedsRecovery_ || journalStopped_ || settlementStopped_)
        { V4PreviewHeadAnchorResult out; out.reason = "v4_preview_anchor_uncommitted_work_pending"; return out; }
        return PublishV4PreviewHeadAnchor(*store_, locator, head_, binding_, headByteBudget, anchorByteBudget);
    }
    V4StructuredCaseReplay ReplayLastCase(const V4GlobalPreviewBudgets& budgets, std::size_t caseByteBudget)
    {
        if (!lastCaseShard_)
        { V4StructuredCaseReplay out; out.status = V4StructuredCaseStatus::unavailable;
          out.reason = "v4_fresh_preview_no_case_shard"; return out; }
        return ReplayV4CaseShardAsStructured(*store_, lastCaseShard_->name, lastCaseShard_->fileId, binding_,
            budgets.coordinate, budgets.axes, budgets.replayWork, budgets.token, caseByteBudget, budgets.bytes);
    }
    V4GlobalPreviewTick StepGlobal(const V4GlobalPreviewBudgets& budgets)
    {
        if (journalPublication_ || settlementPublication_ || !settlementOutcome_.empty() || pendingSelectionHead_)
            return {V4GlobalPreviewStatus::unavailable, 0, "v4_fresh_preview_staged_head_pending"};
        if (family_) return {V4GlobalPreviewStatus::unavailable, 0, "v4_fresh_preview_family_active"};
        if (global_->IsStopped()) return {V4GlobalPreviewStatus::stopped, 0, "v4_fresh_preview_global_stopped"};
        if (global_->ProposedHead()) return {V4GlobalPreviewStatus::ready_to_adopt, 0, "v4_fresh_preview_head_unadopted"};
        if (!budgets.work || !budgets.node)
        {
            if (global_->SuspendBeforePublication()) globalBoundary_ = true;
            return {V4GlobalPreviewStatus::resource_unavailable, 0, "v4_fresh_preview_work_budget_unavailable"};
        }
        globalBoundary_ = false;
        auto result = global_->Step(*store_, binding_, budgets);
        if ((result.status == V4GlobalPreviewStatus::resource_unavailable || result.status == V4GlobalPreviewStatus::unavailable) &&
            global_->SuspendBeforePublication()) globalBoundary_ = true;
        return result;
    }
    bool AdoptGlobal()
    {
        if (journalPublication_ || settlementPublication_ || !settlementOutcome_.empty() || pendingSelectionHead_) return false;
        const auto& proposed = global_->ProposedHead(); if (!proposed) return false;
        auto head = *proposed; if (!global_->Adopt(head)) return false;
        head_ = std::move(head); globalBoundary_ = true; return true;
    }
    bool BeginFamily(std::string coordinate)
    {
        if (!globalBoundary_ || family_ || journalStep_ || journalPublication_ || settlementPublication_ || !settlementOutcome_.empty() ||
            !global_->CanRebase(head_) || !V4CanonicalDecimal(coordinate)) return false;
        family_ = V4FamilyPreviewCycle::Begin(head_, std::move(coordinate)); return static_cast<bool>(family_);
    }
    V4GlobalPreviewTick StepFamily(const V4GlobalPreviewBudgets& budgets)
    {
        if (!family_) return {V4GlobalPreviewStatus::unavailable, 0, "v4_fresh_preview_family_missing"};
        return family_->Step(*store_, binding_, budgets);
    }
    bool CancelFamilyBeforePublication() noexcept
    {
        if (!family_ || !family_->CanDiscardBeforePublication()) return false;
        family_.reset(); return true;
    }
    bool AdoptFamily()
    {
        if (journalPublication_ || settlementPublication_ || !settlementOutcome_.empty() || pendingSelectionHead_) return false;
        if (!family_ || !family_->ProposedHead() || !globalBoundary_ || !global_->CanRebase(head_)) return false;
        auto head = *family_->ProposedHead(); auto rebase = head;
        std::optional<V4ShardLocator> caseShard;
        if (const auto* publication = family_->Publication()) caseShard = publication->caseShard;
        static_assert(std::is_nothrow_move_assignable_v<V4ShardLocator>);
        static_assert(std::is_nothrow_move_assignable_v<std::optional<V4ShardLocator>>);
        if (!family_->Adopt(head) || !global_->RebaseFromFamily(head_, std::move(rebase))) return false;
        head_ = std::move(head); lastCaseShard_ = std::move(caseShard); family_.reset(); return true;
    }
    // A foreground caller must use either these automatic methods or the
    // manual methods above, without interleaving them. Fixed family-round
    // snapshots remain preview-only and are not a crash-recovery authority.
    V4GlobalPreviewTick StepAutomatic(const V4GlobalPreviewBudgets& budgets)
    {
        try
        {
            if (settlementStopped_) return {V4GlobalPreviewStatus::stopped, 0, "v4_automatic_settlement_stopped"};
            if (settlementReady_) return {V4GlobalPreviewStatus::ready_to_adopt, 0, "v4_automatic_settlement_head_unadopted"};
            if (!settlementOutcome_.empty())
            {
                if (!pendingAutoFamily_ || !pendingSelectionHead_ || !globalBoundary_ || !global_->CanRebase(head_))
                    return {V4GlobalPreviewStatus::unavailable, 0, "v4_automatic_settlement_context_unavailable"};
                if (!budgets.work || budgets.pins < 2)
                    return {V4GlobalPreviewStatus::resource_unavailable, 0, "v4_automatic_settlement_budget_unavailable"};
                if (settlementHeadBytes_.size() > budgets.snapshotBytes ||
                    settlementDataBytes_.size() > budgets.snapshotBytes - settlementHeadBytes_.size())
                    return {V4GlobalPreviewStatus::resource_unavailable, 0, "v4_automatic_settlement_snapshot_budget_unavailable"};
                if (!settlementPublication_)
                {
                    auto previous = head_; std::optional<V4ShardLocator> familyShard, caseShard;
                    if (settlementOutcome_ == "visited")
                    {
                        if (!family_ || !family_->ProposedHead() || !family_->Publication())
                            return {V4GlobalPreviewStatus::unavailable, 0, "v4_automatic_settlement_family_missing"};
                        previous = *family_->ProposedHead(); familyShard = family_->Publication()->familyShard; caseShard = family_->Publication()->caseShard;
                    }
                    settlementPublication_ = PublishV4FamilySettlement(*store_, *pendingSelectionHead_, previous, *pendingAutoFamily_,
                        settlementOutcome_, familyShard, caseShard, binding_, budgets);
                    if (!settlementPublication_->staged || !settlementPublication_->head)
                    {
                        const bool sideEffects = std::any_of(settlementPublication_->publications.begin(), settlementPublication_->publications.end(), [](const auto& p) {
                            return p.temporaryCreateAttempted || p.temporaryCreated || p.renameAttempted || p.renameReturnedSuccess; });
                        if (sideEffects) { settlementStopped_ = true; return {V4GlobalPreviewStatus::stopped, 0, settlementPublication_->reason}; }
                        const auto status = settlementPublication_->resourceUnavailable ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable;
                        const auto reason = settlementPublication_->reason; settlementPublication_.reset(); return {status, 0, reason};
                    }
                }
                if (!settlementHeadPin_)
                {
                    auto held = store_->PinRead(settlementPublication_->head->name, settlementPublication_->head->fileId);
                    if (!held) return {V4GlobalPreviewStatus::unavailable, 0, "v4_automatic_settlement_head_pin_unavailable"};
                    auto loaded = store_->Read(settlementPublication_->head->name, settlementPublication_->head->fileId, binding_, budgets.indexBytes);
                    if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight)
                        return {loaded.status == CodecStatus::resource_unavailable ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable, 0, "v4_automatic_settlement_head_unavailable"};
                    if (loaded.bytes.size() > budgets.snapshotBytes)
                        return {V4GlobalPreviewStatus::resource_unavailable, 0, "v4_automatic_settlement_snapshot_budget_unavailable"};
                    const auto page = ReadV4ShardIndexPage(*store_, V4ShardIndexCursor{*settlementPublication_->head, {}}, binding_, budgets.indexBytes);
                    const auto expectedPrevious = settlementOutcome_ == "visited" && family_ && family_->ProposedHead() ? *family_->ProposedHead() : head_;
                    const auto same = [](const V4ShardLocator& a, const V4ShardLocator& b) noexcept {
                        return a.name == b.name && a.fileId.VolumeSerialNumber == b.fileId.VolumeSerialNumber &&
                            std::memcmp(a.fileId.FileId.Identifier, b.fileId.FileId.Identifier, sizeof(a.fileId.FileId.Identifier)) == 0; };
                    if (!page.read || page.kind != "family_settlement" || !page.data || !page.next.next || !same(*page.next.next, expectedPrevious))
                        return {page.resourceUnavailable ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable, 0, "v4_automatic_settlement_index_unavailable"};
                    settlementData_ = *page.data; settlementHeadBytes_ = std::move(loaded.bytes); settlementHeadPin_ = std::move(held);
                    return {V4GlobalPreviewStatus::progress, 1, "v4_automatic_settlement_head_pinned"};
                }
                if (!settlementDataPin_)
                {
                    auto held = store_->PinRead(settlementData_->name, settlementData_->fileId);
                    if (!held) return {V4GlobalPreviewStatus::unavailable, 0, "v4_automatic_settlement_data_pin_unavailable"};
                    auto loaded = store_->Read(settlementData_->name, settlementData_->fileId, binding_, budgets.bytes);
                    if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight || !loaded.state.familySettlement)
                        return {loaded.status == CodecStatus::resource_unavailable ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable, 0, "v4_automatic_settlement_data_unavailable"};
                    if (settlementHeadBytes_.size() > budgets.snapshotBytes || loaded.bytes.size() > budgets.snapshotBytes - settlementHeadBytes_.size())
                        return {V4GlobalPreviewStatus::resource_unavailable, 0, "v4_automatic_settlement_snapshot_budget_unavailable"};
                    V4ShardLocator selected, previous;
                    const auto& saved = *loaded.state.familySettlement;
                    const auto expectedPrevious = settlementOutcome_ == "visited" && family_ && family_->ProposedHead() ? *family_->ProposedHead() : head_;
                    const auto same = [](const V4ShardLocator& a, const V4ShardLocator& b) noexcept {
                        return a.name == b.name && a.fileId.VolumeSerialNumber == b.fileId.VolumeSerialNumber &&
                            std::memcmp(a.fileId.FileId.Identifier, b.fileId.FileId.Identifier, sizeof(a.fileId.FileId.Identifier)) == 0; };
                    if (!V4ShardLocatorFromTokens(saved.selectionName, saved.selectionVolume, saved.selectionFileId, selected) ||
                        !V4ShardLocatorFromTokens(saved.previousName, saved.previousVolume, saved.previousFileId, previous) ||
                        !same(selected, *pendingSelectionHead_) || !same(previous, expectedPrevious) ||
                        saved.coordinate != pendingAutoFamily_->coordinate || std::string_view(saved.outcome) != settlementOutcome_)
                        return {V4GlobalPreviewStatus::unavailable, 0, "v4_automatic_settlement_identity_mismatch"};
                    if (settlementOutcome_ == "visited")
                    {
                        if (!family_ || !family_->Publication() || !family_->Publication()->familyShard)
                            return {V4GlobalPreviewStatus::unavailable, 0, "v4_automatic_settlement_family_missing"};
                        V4ShardLocator savedFamily, savedCase;
                        if (!V4ShardLocatorFromTokens(saved.familyName, saved.familyVolume, saved.familyFileId, savedFamily) ||
                            !same(savedFamily, *family_->Publication()->familyShard) ||
                            (static_cast<bool>(family_->Publication()->caseShard) != !saved.caseName.empty()) ||
                            (!saved.caseName.empty() && (!V4ShardLocatorFromTokens(saved.caseName, saved.caseVolume, saved.caseFileId, savedCase) ||
                                !same(savedCase, *family_->Publication()->caseShard))))
                            return {V4GlobalPreviewStatus::unavailable, 0, "v4_automatic_settlement_shard_mismatch"};
                    }
                    settlementDataBytes_ = std::move(loaded.bytes); settlementDataPin_ = std::move(held);
                    return {V4GlobalPreviewStatus::progress, 1, "v4_automatic_settlement_data_pinned"};
                }
                if (!settlementReplayDone_)
                {
                    if (budgets.pins < 2 || settlementHeadBytes_.size() > budgets.snapshotBytes ||
                        settlementDataBytes_.size() > budgets.snapshotBytes - settlementHeadBytes_.size())
                        return {V4GlobalPreviewStatus::resource_unavailable, 0, "v4_automatic_settlement_budget_unavailable"};
                    auto remaining = budgets; remaining.pins -= 2; remaining.snapshotBytes -= settlementHeadBytes_.size() + settlementDataBytes_.size();
                    if (!settlementReplay_) settlementReplay_ = V4FamilyRoundJournalReplay::Begin(*pendingSelectionHead_);
                    auto replay = settlementReplay_->Step(*store_, binding_, remaining);
                    if (!settlementReplay_->Complete()) return {replay.status, replay.workUsed, replay.reason};
                    if (!replay.item || replay.item->coordinate != pendingAutoFamily_->coordinate || replay.item->ordinal != pendingAutoFamily_->ordinal ||
                        replay.item->scanExhausted != pendingAutoFamily_->scanExhausted || replay.item->savedState != pendingAutoFamily_->savedState)
                    { settlementStopped_ = true; return {V4GlobalPreviewStatus::stopped, replay.workUsed, "v4_automatic_settlement_selection_mismatch"}; }
                    settlementReplayDone_ = true; return {V4GlobalPreviewStatus::progress, replay.workUsed, "v4_automatic_settlement_recheck_pending"};
                }
                std::size_t used = 0;
                while (settlementRecheckPhase_ < 2 && used < budgets.work)
                {
                    const auto& locator = settlementRecheckPhase_ == 0 ? *settlementPublication_->head : *settlementData_;
                    const auto& expected = settlementRecheckPhase_ == 0 ? settlementHeadBytes_ : settlementDataBytes_;
                    const auto read = store_->Read(locator.name, locator.fileId, binding_, settlementRecheckPhase_ == 0 ? budgets.indexBytes : budgets.bytes); ++used;
                    if (read.status != CodecStatus::decoded || read.recoveredInFlight || read.bytes != expected)
                    { settlementStopped_ = true; return {V4GlobalPreviewStatus::stopped, used, "v4_automatic_settlement_recheck_unavailable"}; }
                    ++settlementRecheckPhase_;
                }
                if (settlementRecheckPhase_ != 2) return {V4GlobalPreviewStatus::progress, used, "v4_automatic_settlement_recheck_pending"};
                settlementReady_ = true; return {V4GlobalPreviewStatus::ready_to_adopt, used, "v4_automatic_settlement_replayed_unadopted"};
            }
            if (journalStopped_) return {V4GlobalPreviewStatus::stopped, 0, "v4_automatic_journal_stopped"};
            if (journalReady_) return {V4GlobalPreviewStatus::ready_to_adopt, 0, "v4_automatic_journal_head_unadopted"};
            if (journalNeedsRecovery_)
            {
                if (!familyRound_) return {V4GlobalPreviewStatus::unavailable, 0, "v4_automatic_round_recovery_context_missing"};
                if (lastJournalHead_) resumeReplay_ = V4FamilyRoundJournalReplay::Begin(*lastJournalHead_);
                else familyRound_ = V4FamilyPreviewRound::Begin(familyRound_->SnapshotHead());
                journalNeedsRecovery_ = false; automaticFamilyTurn_ = false;
                return {V4GlobalPreviewStatus::progress, 0, "v4_automatic_round_recovery_prepared"};
            }
            if (!automaticFamilyTurn_)
            {
                auto result = StepGlobal(budgets);
                if ((result.status == V4GlobalPreviewStatus::resource_unavailable || result.status == V4GlobalPreviewStatus::unavailable) && globalBoundary_)
                    automaticFamilyTurn_ = true;
                return result;
            }
            if (family_)
            {
                auto result = StepFamily(budgets);
                if (result.status == V4GlobalPreviewStatus::ready_to_adopt)
                { settlementOutcome_ = "visited"; return {V4GlobalPreviewStatus::progress, result.workUsed, "v4_automatic_family_settlement_pending"}; }
                if ((result.status == V4GlobalPreviewStatus::resource_unavailable || result.status == V4GlobalPreviewStatus::unavailable) &&
                    CancelFamilyBeforePublication())
                {
                    settlementOutcome_ = "deferred";
                    return {V4GlobalPreviewStatus::progress, result.workUsed, "v4_automatic_family_deferred_settlement_pending"};
                }
                return result;
            }
            if (!globalBoundary_) return {V4GlobalPreviewStatus::unavailable, 0, "v4_automatic_global_not_at_boundary"};
            if (resumeReplay_)
            {
                auto replay = resumeReplay_->Step(*store_, binding_, budgets);
                if (resumeReplay_->Complete())
                {
                    auto recovered = resumeReplay_->TakeRound();
                    if (!recovered) return {V4GlobalPreviewStatus::unavailable, replay.workUsed, "v4_automatic_round_recovery_unavailable"};
                    familyRound_ = std::move(recovered); resumeReplay_.reset();
                    return {V4GlobalPreviewStatus::progress, replay.workUsed, "v4_automatic_round_recovered_unexecuted"};
                }
                if (replay.status == V4GlobalPreviewStatus::resource_unavailable || replay.status == V4GlobalPreviewStatus::unavailable)
                    automaticFamilyTurn_ = false;
                return {replay.status, replay.workUsed, replay.reason};
            }
            if (pendingAutoFamily_)
            {
                if (pendingAutoFamily_->scanExhausted)
                {
                    settlementOutcome_ = "scan_exhausted";
                    return {V4GlobalPreviewStatus::progress, 0, "v4_automatic_family_exhausted_settlement_pending"};
                }
                if (!BeginFamily(pendingAutoFamily_->coordinate))
                    return {V4GlobalPreviewStatus::unavailable, 0, "v4_automatic_family_begin_unavailable"};
                return {V4GlobalPreviewStatus::progress, 0, "v4_automatic_family_started_unexecuted"};
            }
            if (!familyRound_) familyRound_ = V4FamilyPreviewRound::Begin(head_);
            if (!journalStep_)
            {
                if (!budgets.work)
                { automaticFamilyTurn_ = false; return {V4GlobalPreviewStatus::resource_unavailable, 0, "v4_automatic_round_work_budget_unavailable"}; }
                auto result = familyRound_->Step(*store_, binding_, budgets);
                if (result.status != V4GlobalPreviewStatus::progress && result.status != V4GlobalPreviewStatus::family_item)
                {
                    journalNeedsRecovery_ = true;
                    automaticFamilyTurn_ = false;
                    return {result.status, result.workUsed, "v4_automatic_round_uncommitted_retry_pending"};
                }
                journalStep_ = std::move(result); journalBudgets_ = budgets;
                return {V4GlobalPreviewStatus::progress, journalStep_->workUsed, "v4_automatic_round_step_uncommitted"};
            }
            if (!journalBudgets_) return {V4GlobalPreviewStatus::unavailable, 0, "v4_automatic_round_budget_missing"};
            if (!journalPublication_)
            {
                journalPublication_ = PublishV4FamilyRoundStep(*store_, head_, familyRound_->SnapshotHead(), journalOrdinal_,
                    *journalStep_, binding_, *journalBudgets_, budgets);
                if (!journalPublication_->staged || !journalPublication_->head)
                {
                    const bool sideEffects = std::any_of(journalPublication_->publications.begin(), journalPublication_->publications.end(), [](const auto& p) {
                        return p.temporaryCreateAttempted || p.temporaryCreated || p.renameAttempted || p.renameReturnedSuccess; });
                    if (sideEffects) { journalStopped_ = true; return {V4GlobalPreviewStatus::stopped, 0, journalPublication_->reason}; }
                    const auto status = journalPublication_->resourceUnavailable ? V4GlobalPreviewStatus::resource_unavailable : V4GlobalPreviewStatus::unavailable;
                    const auto reason = journalPublication_->reason; journalPublication_.reset(); automaticFamilyTurn_ = false; return {status, 0, reason};
                }
            }
            if (!journalReplay_) journalReplay_ = V4FamilyRoundJournalReplay::Begin(*journalPublication_->head);
            const auto replay = journalReplay_->Step(*store_, binding_, budgets);
            if (journalReplay_->Complete())
            { journalReady_ = true; return {V4GlobalPreviewStatus::ready_to_adopt, replay.workUsed, "v4_automatic_round_step_replayed_unadopted"}; }
            return {replay.status, replay.workUsed, replay.reason};
        }
        catch (const std::bad_alloc&) { return {V4GlobalPreviewStatus::resource_unavailable, 0, "v4_automatic_resource_unavailable"}; }
        catch (const std::length_error&) { return {V4GlobalPreviewStatus::resource_unavailable, 0, "v4_automatic_resource_unavailable"}; }
        catch (...) { return {V4GlobalPreviewStatus::unavailable, 0, "v4_automatic_unavailable"}; }
    }
    bool AdoptAutomatic()
    {
        if (settlementReady_)
        {
            if (!settlementPublication_ || !settlementPublication_->head || !pendingAutoFamily_ || !pendingSelectionHead_ ||
                !globalBoundary_ || !global_->CanRebase(head_)) return false;
            auto head = *settlementPublication_->head; auto rebase = head;
            std::optional<V4ShardLocator> caseShard;
            if (settlementOutcome_ == "visited")
            {
                if (!family_ || !family_->ProposedHead() || !family_->Publication()) return false;
                caseShard = family_->Publication()->caseShard;
                if (!family_->Adopt(*family_->ProposedHead())) return false;
            }
            if (!global_->RebaseFromFamily(head_, std::move(rebase))) return false;
            head_ = std::move(head); lastCaseShard_ = std::move(caseShard); family_.reset();
            lastAutoFamily_ = std::move(pendingAutoFamily_); pendingAutoFamily_.reset(); pendingSelectionHead_.reset();
            settlementPublication_.reset(); settlementReplay_.reset(); settlementData_.reset(); settlementHeadPin_.reset(); settlementDataPin_.reset();
            settlementHeadBytes_.clear(); settlementDataBytes_.clear(); settlementOutcome_ = {}; settlementRecheckPhase_ = 0;
            settlementReplayDone_ = false; settlementReady_ = false;
            automaticFamilyTurn_ = false; return true;
        }
        if (settlementPublication_ || !settlementOutcome_.empty()) return false;
        if (journalReady_)
        {
            if (!journalPublication_ || !journalPublication_->head || !journalReplay_ || !journalStep_ ||
                !globalBoundary_ || !global_->CanRebase(head_)) return false;
            auto head = *journalPublication_->head; auto rebase = head; auto journalHead = head;
            const bool exhausted = journalStep_->exhausted;
            auto nextOrdinal = exhausted ? std::string("0") : journalOrdinal_; if (!exhausted) Increment(nextOrdinal);
            std::optional<V4ShardLocator> selectedHead; if (journalStep_->item) selectedHead = head;
            auto restoredRound = journalReplay_->TakeRound(); if (!restoredRound) return false;
            if (!global_->RebaseFromFamily(head_, std::move(rebase))) return false;
            head_ = std::move(head); familyRound_ = std::move(restoredRound); journalOrdinal_ = std::move(nextOrdinal);
            lastJournalHead_ = std::move(journalHead);
            if (journalStep_->item) { pendingAutoFamily_ = std::move(journalStep_->item); pendingSelectionHead_ = std::move(selectedHead); }
            journalStep_.reset(); journalBudgets_.reset(); journalPublication_.reset(); journalReplay_.reset(); journalReady_ = false;
            if (exhausted) { familyRound_.reset(); lastJournalHead_.reset(); automaticFamilyTurn_ = false; }
            return true;
        }
        if (journalPublication_ || journalStep_) return false;
        if (family_) return false; // automatic family visits require settlement
        if (!AdoptGlobal()) return false;
        automaticFamilyTurn_ = true; return true;
    }
};
struct V4FreshPreviewStart
{
    std::unique_ptr<V4FreshPreviewDriver> driver;
    CheckpointLocator locator;
    CheckpointCreateAttempt create;
    PublishedCheckpoint genesis, index;
    bool resourceUnavailable = false;
    std::string_view reason;
};
// Creates a new isolated store only. The supplied root must remain owned by the
// caller for the driver lifetime. The head and all outputs are unexecuted and
// do not authenticate history or survive a crash without an external anchor.
inline V4FreshPreviewStart StartV4FreshPreview(HANDLE ownedRoot, const FILE_ID_INFO& ownedRootId,
    std::string_view binding, const V4GlobalPreviewBudgets& budgets)
{
    V4FreshPreviewStart out;
    try
    {
        if (binding.empty()) { out.reason = "v4_fresh_preview_binding_unavailable"; return out; }
        auto store = CheckpointStore::Create(ownedRoot, ownedRootId, out.locator, out.create);
        if (!store) { out.reason = "v4_fresh_preview_store_unavailable"; return out; }
        V4StructuredDfsCursor initial;
        auto genesis = PublishV4TypedCursorShard(*store, initial, false, binding, budgets.coordinate, budgets.token, budgets.bytes);
        out.genesis = std::move(genesis.publication);
        if (!genesis.published)
        { out.resourceUnavailable = genesis.resourceUnavailable; out.reason = "v4_fresh_preview_genesis_unavailable"; return out; }
        V4ShardLocator data{out.genesis.fileName, out.genesis.fileId};
        auto index = AppendV4ShardIndex(*store, std::nullopt, data, "global_cursor", binding, budgets.coordinate, budgets.axes,
            budgets.token, budgets.bytes, budgets.bytes, budgets.replayWork, budgets.bytes, budgets.indexBytes);
        out.index = std::move(index.publication);
        if (!index.published || !index.head)
        { out.resourceUnavailable = index.resourceUnavailable; out.reason = "v4_fresh_preview_index_unavailable"; return out; }
        out.driver.reset(new V4FreshPreviewDriver(std::move(store), std::string(binding), std::move(*index.head)));
        out.reason = "v4_fresh_preview_unexecuted_head_untrusted"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_fresh_preview_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_fresh_preview_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_fresh_preview_unavailable"; return out; }
}

enum class V4RoundReplayStatus { progress, replayed, resource_unavailable, unavailable };
struct V4RoundReplayStep
{
    V4RoundReplayStatus status = V4RoundReplayStatus::unavailable;
    std::size_t workUsed = 0, records = 0;
    std::string_view reason;
};
// Replays a fixed snapshot's appended round records from genesis, in bounded
// calls. Pins and first-read bytes are retained for this session; the external
// head's authenticity, prior mapped writes and crash locator remain unknown.
class V4RoundReplaySession final
{
    struct Snapshot { V4ShardLocator locator; std::unique_ptr<V4CheckpointReadPin> pin; std::string bytes; };
    V4ShardLocator snapshotHead_;
    V4ShardIndexCursor scan_;
    std::vector<Snapshot> snapshots_;
    std::vector<V4RoundAdvanceAudit> records_; // collected newest first
    std::optional<V4TypedStepAudit> laterTyped_;
    std::optional<FamilyState> laterFamily_;
    std::optional<V4StructuredDfsCursor> laterMain_;
    std::unique_ptr<V4TypedShardPreviewRound> round_;
    std::size_t bytes_ = 0, replayRemaining_ = 0;
    CheckpointStore* store_ = nullptr;
    std::string binding_;
    std::size_t checkIndex_ = 0;
    bool scanning_ = true, checking_ = false, done_ = false, failed_ = false;
    V4RoundReplaySession(V4ShardLocator snapshotHead, V4ShardLocator currentHead)
        : snapshotHead_(std::move(snapshotHead)), scan_{std::move(currentHead), {}} {}
    static bool Same(const V4ShardLocator& a, const V4ShardLocator& b) noexcept
    { return a.name == b.name && a.fileId.VolumeSerialNumber == b.fileId.VolumeSerialNumber &&
        std::memcmp(a.fileId.FileId.Identifier, b.fileId.FileId.Identifier, sizeof(a.fileId.FileId.Identifier)) == 0; }
public:
    V4RoundReplaySession(const V4RoundReplaySession&) = delete;
    V4RoundReplaySession& operator=(const V4RoundReplaySession&) = delete;
    static std::unique_ptr<V4RoundReplaySession> Begin(V4ShardLocator snapshotHead, V4ShardLocator currentHead)
    { return std::unique_ptr<V4RoundReplaySession>(new V4RoundReplaySession(std::move(snapshotHead), std::move(currentHead))); }
    // Borrowed only while this replay session keeps its read pins alive.
    // The returned cursor is a preview, never an execution capability.
    V4TypedShardPreviewRound* ReplayedPreview() const noexcept
    { return done_ && !failed_ ? round_.get() : nullptr; }
    V4RoundReplayStep Step(CheckpointStore& store, std::string_view binding, const V4CoordinateBudget& budgetLimit,
        const V4AxisBudget& axisBudget,
        std::size_t tokenLimit, std::size_t replayWorkLimit, std::size_t byteLimit, std::size_t savedNodeLimit,
        std::size_t pinLimit, std::size_t snapshotByteLimit, std::size_t recordLimit, std::size_t workBudget)
    {
        V4RoundReplayStep out;
        try
        {
            if (failed_ || done_ || (store_ && (store_ != &store || binding_ != binding)))
            { out.reason = "v4_round_replay_session_unavailable"; return out; }
            if (!store_) { binding_ = std::string(binding); store_ = &store; }
            if (snapshots_.size() > pinLimit || bytes_ > snapshotByteLimit || records_.size() > recordLimit)
            { out.status = V4RoundReplayStatus::resource_unavailable; out.reason = "v4_round_replay_existing_budget_unavailable"; return out; }
            bool resource = false;
            const auto snapshot = [&](const V4ShardLocator& locator) {
                auto found = std::find_if(snapshots_.begin(), snapshots_.end(), [&](const auto& x) { return Same(x.locator, locator); });
                if (found == snapshots_.end())
                {
                    if (snapshots_.size() >= pinLimit) { resource = true; return false; }
                    auto pin = store.PinRead(locator.name, locator.fileId); if (!pin) return false;
                    auto loaded = store.Read(locator.name, locator.fileId, binding, byteLimit);
                    if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight)
                    { resource = loaded.status == CodecStatus::resource_unavailable; return false; }
                    if (bytes_ > snapshotByteLimit || loaded.bytes.size() > snapshotByteLimit - bytes_)
                    { resource = true; return false; }
                    const auto n = loaded.bytes.size(); snapshots_.push_back({locator, std::move(pin), std::move(loaded.bytes)}); bytes_ += n;
                    return true;
                }
                const auto loaded = store.Read(locator.name, locator.fileId, binding, byteLimit);
                if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight)
                { resource = loaded.status == CodecStatus::resource_unavailable; return false; }
                return loaded.bytes == found->bytes;
            };
            while (out.workUsed < workBudget && !done_)
            {
                if (checking_)
                {
                    if (checkIndex_ < snapshots_.size())
                    {
                        const auto& entry = snapshots_[checkIndex_];
                        const auto loaded = store.Read(entry.locator.name, entry.locator.fileId, binding, byteLimit);
                        if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight || loaded.bytes != entry.bytes)
                        { failed_ = loaded.status != CodecStatus::resource_unavailable;
                          out.status = loaded.status == CodecStatus::resource_unavailable ? V4RoundReplayStatus::resource_unavailable : V4RoundReplayStatus::unavailable; return out; }
                        ++checkIndex_; ++out.workUsed; continue;
                    }
                    const auto roundIndex = checkIndex_ - snapshots_.size();
                    if (round_ && roundIndex < round_->PinnedSnapshotCount())
                    {
                        const auto checked = round_->RevalidatePinnedAt(store, binding, roundIndex, byteLimit);
                        if (!checked.matched) { failed_ = !checked.resourceUnavailable;
                            out.status = checked.resourceUnavailable ? V4RoundReplayStatus::resource_unavailable : V4RoundReplayStatus::unavailable; return out; }
                        ++checkIndex_; ++out.workUsed; continue;
                    }
                    done_ = true; break;
                }
                if (scanning_)
                {
                    if (!scan_.next) { failed_ = true; out.reason = "v4_round_replay_snapshot_not_ancestor"; return out; }
                    if (Same(*scan_.next, snapshotHead_))
                    {
                        if (laterTyped_ || laterFamily_ || laterMain_) { failed_ = true; out.reason = "v4_round_replay_unpaired_step"; return out; }
                        if (!snapshot(snapshotHead_)) { out.status = resource ? V4RoundReplayStatus::resource_unavailable : V4RoundReplayStatus::unavailable; return out; }
                        const auto rootPage = ReadV4ShardIndexPage(store, scan_, binding, byteLimit);
                        if (!rootPage.read) { out.status = rootPage.resourceUnavailable ? V4RoundReplayStatus::resource_unavailable : V4RoundReplayStatus::unavailable; return out; }
                        round_ = V4TypedShardPreviewRound::Begin(snapshotHead_); replayRemaining_ = records_.size(); scanning_ = false; continue;
                    }
                    if (!snapshot(*scan_.next)) { out.status = resource ? V4RoundReplayStatus::resource_unavailable : V4RoundReplayStatus::unavailable; return out; }
                    const auto page = ReadV4ShardIndexPage(store, scan_, binding, byteLimit);
                    if (!page.read || !page.data) { out.status = page.resourceUnavailable ? V4RoundReplayStatus::resource_unavailable : V4RoundReplayStatus::unavailable; return out; }
                    if (page.kind == "typed_step")
                    {
                        if (laterTyped_ || !snapshot(*page.data)) { failed_ = !resource; out.status = resource ? V4RoundReplayStatus::resource_unavailable : V4RoundReplayStatus::unavailable; return out; }
                        const auto typed = ReadV4TypedStepShard(store, page.data->name, page.data->fileId, binding,
                            budgetLimit, tokenLimit, replayWorkLimit, byteLimit);
                        if (!typed.replayed || !typed.audit) { out.status = typed.resourceUnavailable ? V4RoundReplayStatus::resource_unavailable : V4RoundReplayStatus::unavailable; return out; }
                        if (laterFamily_ && (typed.audit->status != V4StructuredDfsStatus::emitted || laterFamily_->coordinate != typed.audit->familyCoordinate))
                        { failed_ = true; out.reason = "v4_round_replay_family_step_mismatch"; return out; }
                        std::size_t layer = 0; bool mainTask = false;
                        if (!V4TypedTaskIdCanonical(typed.audit->taskId, layer, mainTask))
                        { failed_ = true; out.reason = "v4_round_replay_task_id_unavailable"; return out; }
                        const bool needsMain = mainTask && typed.audit->after.exhausted;
                        if (needsMain != static_cast<bool>(laterMain_) ||
                            (needsMain && (layer == (std::numeric_limits<std::size_t>::max)() || laterMain_->layer != layer + 1)))
                        { failed_ = true; out.reason = "v4_round_replay_next_main_mismatch"; return out; }
                        laterTyped_ = *typed.audit;
                    }
                    else if (page.kind == "global_cursor")
                    {
                        if (laterMain_ || laterFamily_ || laterTyped_ || !snapshot(*page.data))
                        { failed_ = !resource; out.status = resource ? V4RoundReplayStatus::resource_unavailable : V4RoundReplayStatus::unavailable; return out; }
                        const auto main = ReadV4TypedCursorShard(store, page.data->name, page.data->fileId, binding,
                            budgetLimit, tokenLimit, byteLimit);
                        if (!main.cursorValidated || !main.cursor || main.deferred || main.cursor->stopDepth ||
                            !main.cursor->prefix.empty() || main.cursor->nextChoice != std::vector<std::uint32_t>{0} || main.cursor->exhausted)
                        { out.status = main.resourceUnavailable ? V4RoundReplayStatus::resource_unavailable : V4RoundReplayStatus::unavailable; return out; }
                        laterMain_ = *main.cursor;
                    }
                    else if (page.kind == "family")
                    {
                        if (laterFamily_ || laterTyped_ || !snapshot(*page.data)) { failed_ = !resource; out.status = resource ? V4RoundReplayStatus::resource_unavailable : V4RoundReplayStatus::unavailable; return out; }
                        const auto family = ReadV4FamilyCursorShard(store, page.data->name, page.data->fileId,
                            binding, budgetLimit, axisBudget, byteLimit, replayWorkLimit);
                        if (!family.cursorValidated || !family.family) { out.status = family.resourceUnavailable ? V4RoundReplayStatus::resource_unavailable : V4RoundReplayStatus::unavailable; return out; }
                        if (family.family->scanExhausted || !family.family->pruneSteps.empty() || family.family->state != WorkState::pending)
                        { failed_ = true; out.reason = "v4_round_replay_family_not_initial"; return out; }
                        const auto decoded = NextV4FamilyCoordinate(family.family->coordinate, budgetLimit);
                        if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family)
                        { out.status = decoded.status == V4FamilyCoordinateStatus::resource_unavailable ? V4RoundReplayStatus::resource_unavailable : V4RoundReplayStatus::unavailable; return out; }
                        const auto initial = event_graph::V4FamilyCandidateGraph::SerializeCursor(*decoded.family, axisBudget, {});
                        if (initial.status != event_graph::V4FamilyTraversalStatus::ready || !initial.errors.empty() || initial.encoded != family.family->candidateCursor)
                        { out.status = initial.status == event_graph::V4FamilyTraversalStatus::resource_unavailable ? V4RoundReplayStatus::resource_unavailable : V4RoundReplayStatus::unavailable; return out; }
                        laterFamily_ = *family.family;
                    }
                    else if (page.kind == "round_step")
                    {
                        if (!snapshot(*page.data)) { out.status = resource ? V4RoundReplayStatus::resource_unavailable : V4RoundReplayStatus::unavailable; return out; }
                        const auto shard = ReadV4RoundAdvanceShard(store, page.data->name, page.data->fileId, binding, byteLimit);
                        if (!shard.read || !shard.audit) { out.status = shard.resourceUnavailable ? V4RoundReplayStatus::resource_unavailable : V4RoundReplayStatus::unavailable; return out; }
                        V4ShardLocator fixed;
                        if (!V4ShardLocatorFromTokens(shard.audit->snapshotName, shard.audit->snapshotVolume, shard.audit->snapshotFileId, fixed))
                        { failed_ = true; out.reason = "v4_round_replay_snapshot_identity_invalid"; return out; }
                        if (Same(fixed, snapshotHead_))
                        {
                          if (shard.audit->status == "item")
                          {
                              if (!laterTyped_ || !shard.audit->itemCursor || laterTyped_->taskId != shard.audit->taskId ||
                                  laterTyped_->stepOrdinal != shard.audit->nextOrdinal || !V4SameTypedCursor(laterTyped_->before, *shard.audit->itemCursor))
                              { failed_ = true; out.reason = "v4_round_replay_item_step_mismatch"; return out; }
                              if (laterTyped_->status == V4StructuredDfsStatus::emitted)
                              {
                                  if (!page.next.next && !laterFamily_) { failed_ = true; out.reason = "v4_round_replay_family_missing"; return out; }
                                  auto familyScan = page.next; bool found = false; std::size_t checked = 0;
                                  while (familyScan.next && checked < savedNodeLimit)
                                  {
                                      if (!snapshot(*familyScan.next)) { out.status = resource ? V4RoundReplayStatus::resource_unavailable : V4RoundReplayStatus::unavailable; return out; }
                                      const auto prior = ReadV4ShardIndexPage(store, familyScan, binding, byteLimit);
                                      if (!prior.read || !prior.data) { out.status = prior.resourceUnavailable ? V4RoundReplayStatus::resource_unavailable : V4RoundReplayStatus::unavailable; return out; }
                                      ++checked; familyScan = prior.next;
                                      if (prior.kind != "family") continue;
                                      if (!snapshot(*prior.data)) { out.status = resource ? V4RoundReplayStatus::resource_unavailable : V4RoundReplayStatus::unavailable; return out; }
                                      const auto family = ReadV4FamilyCursorShard(store, prior.data->name, prior.data->fileId,
                                          binding, budgetLimit, axisBudget, byteLimit, replayWorkLimit);
                                      if (!family.cursorValidated || !family.family)
                                      { out.status = family.resourceUnavailable ? V4RoundReplayStatus::resource_unavailable : V4RoundReplayStatus::unavailable; return out; }
                                      if (family.family->coordinate == laterTyped_->familyCoordinate) { found = true; break; }
                                  }
                                  if (!found && familyScan.next)
                                  { out.status = V4RoundReplayStatus::resource_unavailable; out.reason = "v4_round_replay_family_scan_budget_unavailable"; return out; }
                                  if (found == static_cast<bool>(laterFamily_))
                                  { failed_ = true; out.reason = "v4_round_replay_family_registration_mismatch"; return out; }
                              }
                              if (laterMain_)
                              {
                                  auto mainScan = page.next; std::size_t checked = 0; bool duplicate = false;
                                  while (mainScan.next && checked < savedNodeLimit)
                                  {
                                      if (!snapshot(*mainScan.next)) { out.status = resource ? V4RoundReplayStatus::resource_unavailable : V4RoundReplayStatus::unavailable; return out; }
                                      const auto prior = ReadV4ShardIndexPage(store, mainScan, binding, byteLimit);
                                      if (!prior.read || !prior.data) { out.status = prior.resourceUnavailable ? V4RoundReplayStatus::resource_unavailable : V4RoundReplayStatus::unavailable; return out; }
                                      ++checked; mainScan = prior.next;
                                      if (prior.kind != "global_cursor") continue;
                                      if (!snapshot(*prior.data)) { out.status = resource ? V4RoundReplayStatus::resource_unavailable : V4RoundReplayStatus::unavailable; return out; }
                                      const auto old = ReadV4TypedCursorShard(store, prior.data->name, prior.data->fileId,
                                          binding, budgetLimit, tokenLimit, byteLimit);
                                      if (!old.cursorValidated || !old.cursor || old.deferred || old.cursor->stopDepth ||
                                          !old.cursor->prefix.empty() || old.cursor->nextChoice != std::vector<std::uint32_t>{0} || old.cursor->exhausted)
                                      { out.status = old.resourceUnavailable ? V4RoundReplayStatus::resource_unavailable : V4RoundReplayStatus::unavailable; return out; }
                                      if (old.cursor->layer == laterMain_->layer) { duplicate = true; break; }
                                  }
                                  if (duplicate) { failed_ = true; out.reason = "v4_round_replay_duplicate_main_genesis"; return out; }
                                  if (mainScan.next) { out.status = V4RoundReplayStatus::resource_unavailable; out.reason = "v4_round_replay_main_scan_budget_unavailable"; return out; }
                              }
                          }
                          else if (laterTyped_ || laterFamily_ || laterMain_)
                          { failed_ = true; out.reason = "v4_round_replay_unexpected_selected_step"; return out; }
                          if (records_.size() >= recordLimit) { out.status = V4RoundReplayStatus::resource_unavailable; return out; }
                          records_.push_back(*shard.audit); }
                        else if (laterTyped_ || laterFamily_ || laterMain_)
                        { failed_ = true; out.reason = "v4_round_replay_mixed_snapshot_step_unavailable"; return out; }
                        laterTyped_.reset(); laterFamily_.reset(); laterMain_.reset();
                    }
                    else if (laterTyped_ || laterFamily_ || laterMain_) { failed_ = true; out.reason = "v4_round_replay_interleaved_step"; return out; }
                    scan_ = page.next; ++out.workUsed; continue;
                }
                if (!replayRemaining_) { checking_ = true; checkIndex_ = 0; continue; }
                const auto& saved = records_[replayRemaining_ - 1]; const auto& b = saved.budget;
                if (b.decimalDigits > budgetLimit.decimalDigits || b.tokenBytes > budgetLimit.tokenBytes ||
                    b.listEntries > budgetLimit.listEntries || b.arithmeticSteps > budgetLimit.arithmeticSteps ||
                    saved.tokenBudget > tokenLimit || saved.replayWorkBudget > replayWorkLimit || saved.byteBudget > byteLimit ||
                    saved.nodeBudget > savedNodeLimit || saved.pinBudget > pinLimit - snapshots_.size() ||
                    saved.snapshotByteBudget > snapshotByteLimit - bytes_)
                { out.status = V4RoundReplayStatus::resource_unavailable; out.reason = "v4_round_replay_saved_budget_unavailable"; return out; }
                if (!round_ || round_->PositionForAudit() != saved.beforePosition)
                { failed_ = true; out.reason = "v4_round_replay_before_mismatch"; return out; }
                const auto observed = round_->Step(store, binding, b, saved.tokenBudget, saved.replayWorkBudget,
                    saved.byteBudget, saved.nodeBudget, saved.pinBudget, saved.snapshotByteBudget);
                if (observed.status == V4TypedPreviewStatus::resource_unavailable)
                { out.status = V4RoundReplayStatus::resource_unavailable; out.reason = "v4_round_replay_step_resource_unavailable"; return out; }
                const std::string_view status = observed.status == V4TypedPreviewStatus::item ? "item" :
                    observed.status == V4TypedPreviewStatus::exhausted ? "exhausted" :
                    observed.status == V4TypedPreviewStatus::progress ? "progress" : "unavailable";
                if (status != saved.status || round_->PositionForAudit() != saved.afterPosition ||
                    static_cast<bool>(observed.item) != static_cast<bool>(saved.itemCursor) ||
                    (observed.item && (observed.item->taskId != saved.taskId || observed.item->nextOrdinal != saved.nextOrdinal ||
                        observed.item->fromDeferred != saved.fromDeferred || !V4SameTypedCursor(observed.item->cursor, *saved.itemCursor))))
                { failed_ = true; out.reason = "v4_round_replay_transition_mismatch"; return out; }
                --replayRemaining_; ++out.workUsed;
            }
            out.records = records_.size(); out.status = done_ ? V4RoundReplayStatus::replayed : V4RoundReplayStatus::progress;
            out.reason = done_ ? "v4_round_replayed_head_and_history_unauthenticated" : "v4_round_replay_progress"; return out;
        }
        catch (const std::bad_alloc&) { failed_ = true; out = {}; out.status = V4RoundReplayStatus::resource_unavailable; out.reason = "v4_round_replay_resource_unavailable_restart"; return out; }
        catch (const std::length_error&) { failed_ = true; out = {}; out.status = V4RoundReplayStatus::resource_unavailable; out.reason = "v4_round_replay_resource_unavailable_restart"; return out; }
        catch (...) { failed_ = true; out = {}; out.reason = "v4_round_replay_unavailable"; return out; }
    }
};

enum class V4ShardPendingStatus { progress, candidate, round_exhausted, resource_unavailable, unavailable };
struct V4ShardPendingStep
{
    V4ShardPendingStatus status = V4ShardPendingStatus::unavailable;
    std::optional<V4ShardLocator> caseShard;
    std::optional<V4ShardCaseKey> key;
    std::size_t workUsed = 0;
    std::string_view reason;
};
// A round visits one immutable head snapshot. Before yielding a case key,
// it rescans newer entries to suppress older versions of the same exact key.
// This uses bounded memory and resumable work, but can take quadratic I/O.
// After restart it must begin from a trusted head again; saved cursors alone
// are not authenticated history or permission to execute.
class V4ShardPendingRound final
{
    V4ShardLocator head_;
    V4ShardIndexCursor outer_, probe_;
    std::optional<V4ShardLocator> candidate_, candidateNode_;
    std::optional<V4ShardCaseKey> key_;
    bool finished_ = false;
    explicit V4ShardPendingRound(V4ShardLocator head) : head_(std::move(head)), outer_{head_, {}} {}
    static bool SameLocator(const V4ShardLocator& a, const V4ShardLocator& b) noexcept
    { return a.name == b.name && a.fileId.VolumeSerialNumber == b.fileId.VolumeSerialNumber &&
        std::memcmp(a.fileId.FileId.Identifier, b.fileId.FileId.Identifier, sizeof(a.fileId.FileId.Identifier)) == 0; }
public:
    V4ShardPendingRound(const V4ShardPendingRound&) = delete;
    V4ShardPendingRound& operator=(const V4ShardPendingRound&) = delete;
    static std::unique_ptr<V4ShardPendingRound> Begin(V4ShardLocator trustedHead)
    { return std::unique_ptr<V4ShardPendingRound>(new V4ShardPendingRound(std::move(trustedHead))); }
    V4ShardPendingStep Step(CheckpointStore& store, std::string_view binding, const V4CoordinateBudget& coordinateBudget,
        const V4AxisBudget& axisBudget, std::size_t replayWorkBudget, std::size_t shardByteBudget,
        std::size_t indexByteBudget, std::size_t nodeBudget)
    {
        V4ShardPendingStep out;
        try
        {
            if (finished_) { out.reason = "v4_shard_pending_round_already_finished"; return out; }
            auto outer = outer_, probe = probe_; auto candidate = candidate_, candidateNode = candidateNode_; auto key = key_;
            bool finished = false;
            while (out.workUsed < nodeBudget)
            {
                if (!candidate)
                {
                    if (!outer.next) { finished = true; break; }
                    const auto indexNode = *outer.next;
                    const auto page = ReadV4ShardIndexPage(store, outer, binding, indexByteBudget);
                    if (!page.read)
                    { out.status = page.resourceUnavailable ? V4ShardPendingStatus::resource_unavailable : V4ShardPendingStatus::unavailable;
                      out.workUsed = 0; out.reason = "v4_shard_pending_index_unavailable"; return out; }
                    ++out.workUsed; outer = page.next;
                    if (page.kind != "case") continue;
                    const auto shard = ReadV4CaseShard(store, page.data->name, page.data->fileId, binding,
                        coordinateBudget, axisBudget, replayWorkBudget, shardByteBudget);
                    const auto itemKey = V4ShardReplayedCaseKey(shard);
                    if (!itemKey)
                    { out.status = shard.resourceUnavailable ? V4ShardPendingStatus::resource_unavailable : V4ShardPendingStatus::unavailable;
                      out.workUsed = 0; out.reason = "v4_shard_pending_case_unavailable"; return out; }
                    // Stored execution state/history is not authenticated by
                    // identity replay. Yield its key even if it claims to be
                    // completed; the caller must independently authenticate
                    // the latest state before deciding whether to dispatch.
                    candidate = page.data; candidateNode = indexNode; key = *itemKey; probe = V4ShardIndexCursor{head_, {}};
                    continue;
                }
                if (!probe.next || !candidateNode || !key)
                { out.workUsed = 0; out.reason = "v4_shard_pending_probe_invalid"; return out; }
                if (SameLocator(*probe.next, *candidateNode))
                {
                    out.caseShard = candidate; out.key = key; out.status = V4ShardPendingStatus::candidate;
                    out.reason = "v4_shard_case_state_and_execution_require_authentication";
                    candidate.reset(); candidateNode.reset(); key.reset(); probe = {};
                    outer_ = std::move(outer); probe_ = std::move(probe); candidate_ = std::move(candidate);
                    candidateNode_ = std::move(candidateNode); key_ = std::move(key); return out;
                }
                const auto page = ReadV4ShardIndexPage(store, probe, binding, indexByteBudget);
                if (!page.read)
                { out.status = page.resourceUnavailable ? V4ShardPendingStatus::resource_unavailable : V4ShardPendingStatus::unavailable;
                  out.workUsed = 0; out.reason = "v4_shard_pending_probe_unavailable"; return out; }
                ++out.workUsed; probe = page.next;
                if (page.kind != "case") continue;
                const auto newer = ReadV4CaseShard(store, page.data->name, page.data->fileId, binding,
                    coordinateBudget, axisBudget, replayWorkBudget, shardByteBudget);
                const auto newerKey = V4ShardReplayedCaseKey(newer);
                if (!newerKey)
                { out.status = newer.resourceUnavailable ? V4ShardPendingStatus::resource_unavailable : V4ShardPendingStatus::unavailable;
                  out.workUsed = 0; out.reason = "v4_shard_pending_newer_case_unavailable"; return out; }
                if (*newerKey == *key) { candidate.reset(); candidateNode.reset(); key.reset(); probe = {}; }
            }
            outer_ = std::move(outer); probe_ = std::move(probe); candidate_ = std::move(candidate);
            candidateNode_ = std::move(candidateNode); key_ = std::move(key); finished_ = finished;
            out.status = finished ? V4ShardPendingStatus::round_exhausted : V4ShardPendingStatus::progress;
            out.reason = finished ? "v4_shard_pending_round_ended_not_coverage" : "v4_shard_pending_round_in_progress";
        }
        catch (const std::bad_alloc&) { out = {}; out.status = V4ShardPendingStatus::resource_unavailable; out.reason = "v4_shard_pending_resource_unavailable"; }
        catch (const std::length_error&) { out = {}; out.status = V4ShardPendingStatus::resource_unavailable; out.reason = "v4_shard_pending_resource_unavailable"; }
        catch (...) { out = {}; out.reason = "v4_shard_pending_observation_unavailable"; }
        return out;
    }
};

template <typename Validate>
inline AttemptResult CheckpointStore::BeginAttemptCore(std::wstring_view preFileName, const FILE_ID_INFO& trustedPreFileId,
    std::string_view binding, std::size_t familyIndex, std::size_t caseIndex, std::string_view executionId,
    FamilyVersion version, std::size_t byteBudget, Validate&& validate) noexcept
{
    AttemptResult result;
    try
    {
        if (executionId.empty() || !StillOwned()) { result.reason = "attempt_identity_or_directory_unavailable"; return result; }
        // Read is the recovery interface: it turns any previously in-flight
        // case into outcome_unavailable before it can be selected again.
        const auto loaded = Read(preFileName, trustedPreFileId, binding, byteBudget);
        if (loaded.status != CodecStatus::decoded || loaded.recoveredInFlight || loaded.state.familyVersion != version ||
            (version == FamilyVersion::v4 && !loaded.state.v4TypedEnabled) ||
            familyIndex >= loaded.state.families.size())
        { result.reason = "attempt_pre_checkpoint_unavailable"; return result; }
        const auto& family = loaded.state.families[familyIndex];
        if (caseIndex >= family.cases.size()) { result.reason = "attempt_case_unavailable"; return result; }
        const auto& item = family.cases[caseIndex];
        if ((item.state != WorkState::pending && item.state != WorkState::resource_unavailable) ||
            !item.executionId.empty() || item.kind != "graph_hypothesis")
        { result.reason = "attempt_case_not_new_or_graph_unavailable"; return result; }
        if (version == FamilyVersion::v4)
            for (const auto& existingFamily : loaded.state.families)
                for (const auto& existing : existingFamily.cases)
                    if (existing.executionId == executionId || std::any_of(existing.history.begin(), existing.history.end(),
                        [&](const auto& previous) { return previous.executionId == executionId; }))
                    { result.reason = "attempt_execution_id_already_recorded"; return result; }
        if (!validate(family, caseIndex))
        { result.reason = "attempt_authoritative_replay_unavailable"; return result; }
        const auto preBytes = EncodeCheckpoint(loaded.state, binding, byteBudget);
        if (preBytes.status != CodecStatus::encoded) { result.reason = "attempt_pre_encoding_unavailable"; return result; }
        auto next = loaded.state;
        auto& nextCase = next.families[familyIndex].cases[caseIndex];
        nextCase.state = WorkState::in_flight;
        nextCase.executionId = executionId;
        const auto nextBytes = EncodeCheckpoint(next, binding, byteBudget);
        if (nextBytes.status != CodecStatus::encoded) { result.reason = "attempt_post_encoding_unavailable"; return result; }
        auto capability = std::unique_ptr<InFlightReceipt>(new InFlightReceipt);
        capability->lineage_ = std::make_shared<AttemptLineage>();
        capability->binding_ = binding; capability->preState_ = preBytes.bytes;
        capability->executionId_ = executionId; capability->familyIndex_ = familyIndex;
        capability->caseIndex_ = caseIndex; capability->ownerThread_ = GetCurrentThreadId();
        capability->version_ = version;
        capability->rootId_ = locator_.rootId; capability->directoryId_ = locator_.directoryId;
        if (version == FamilyVersion::v4)
        {
            HANDLE duplicate = nullptr;
            if (!StillOwned() || !DuplicateHandle(GetCurrentProcess(), root_, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
            { result.reason = "attempt_root_duplicate_unavailable"; return result; }
            capability->rootPinned_ = duplicate; duplicate = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), directory_, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
            { result.reason = "attempt_directory_duplicate_unavailable"; return result; }
            capability->directoryPinned_ = duplicate;
            const auto prePath = directoryPath_ + L"\\" + std::wstring(preFileName);
            capability->prePinned_ = CreateFileW(prePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            FILE_ID_INFO id{}; FILE_ATTRIBUTE_TAG_INFO tag{}; LARGE_INTEGER size{};
            if (capability->prePinned_ == INVALID_HANDLE_VALUE ||
                !GetFileInformationByHandleEx(capability->prePinned_, FileIdInfo, &id, sizeof(id)) ||
                !SameIdentity(id, trustedPreFileId) ||
                !GetFileInformationByHandleEx(capability->prePinned_, FileAttributeTagInfo, &tag, sizeof(tag)) ||
                (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                !GetFileSizeEx(capability->prePinned_, &size) || size.QuadPart < 0 ||
                static_cast<std::uint64_t>(size.QuadPart) != preBytes.bytes.size() || !StillOwned())
            { result.reason = "attempt_pre_pinned_identity_unavailable"; return result; }
            char preBuffer[8192]; std::size_t preOffset = 0;
            while (preOffset < preBytes.bytes.size())
            {
                const auto amount = static_cast<DWORD>((std::min)(preBytes.bytes.size() - preOffset, sizeof(preBuffer)));
                DWORD read = 0;
                if (!ReadFile(capability->prePinned_, preBuffer, amount, &read, nullptr) || !read ||
                    std::memcmp(preBuffer, preBytes.bytes.data() + preOffset, read) != 0)
                { result.reason = "attempt_pre_pinned_readback_unavailable"; return result; }
                preOffset += read;
            }
            if (!StillOwned()) { result.reason = "attempt_pre_directory_changed"; return result; }
            capability->preId_ = id;
        }

        // Prevent duplicate issuance in this process even across reopened
        // CheckpointStore instances. This is intentionally conservative: a
        // failed/uncertain publish leaves the reservation and needs external
        // reconciliation rather than an automatic second attempt.
        std::string caseKey, attemptKey, preKey;
        AppendCheckpointToken(caseKey, version == FamilyVersion::v3 ? "v3" : "v4");
        for (const auto value : {binding, std::string_view(family.coordinate), std::string_view(item.candidateId),
            std::string_view(item.graphId), std::string_view(item.presence), std::string_view(item.kind)})
            AppendCheckpointToken(caseKey, value);
        AppendCheckpointToken(attemptKey, binding); AppendCheckpointToken(attemptKey, executionId);
        // Identical pre-state published under another filename is still the
        // same transition source and cannot issue a second in-flight branch.
        preKey = preBytes.bytes;
        {
            std::lock_guard lock(registryMutex_);
            if (reservedCases_.contains(caseKey) || reservedAttempts_.contains(attemptKey) || reservedPreStates_.contains(preKey))
            { result.reason = "attempt_already_reserved_in_process"; return result; }
            reservedCases_.insert(std::move(caseKey));
            reservedAttempts_.insert(std::move(attemptKey));
            reservedPreStates_.insert(std::move(preKey));
        }
        result.checkpoint = Publish(next, binding, byteBudget);
        if (result.checkpoint.status != StoreStatus::published_durability_unverified ||
            !result.checkpoint.fileIdentityCaptured || !result.checkpoint.renameReturnedSuccess)
        { result.reason = "attempt_checkpoint_publication_unavailable"; return result; }

        // Pin the published file without sharing write/delete and read the
        // exact bytes back from that same identified handle before issuance.
        const auto path = directoryPath_ + L"\\" + result.checkpoint.fileName;
        capability->pinned_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        FILE_ID_INFO observed{};
        FILE_ATTRIBUTE_TAG_INFO tag{};
        LARGE_INTEGER size{};
        if (capability->pinned_ == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(capability->pinned_, FileIdInfo, &observed, sizeof(observed)) ||
            !SameIdentity(observed, result.checkpoint.fileId) ||
            !GetFileInformationByHandleEx(capability->pinned_, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !GetFileSizeEx(capability->pinned_, &size) || size.QuadPart < 0 ||
            static_cast<std::uint64_t>(size.QuadPart) != nextBytes.bytes.size() || !StillOwned())
        { result.reason = "attempt_checkpoint_pinned_identity_unavailable"; return result; }
        char buffer[8192];
        std::size_t offset = 0;
        while (offset < nextBytes.bytes.size())
        {
            const auto count = static_cast<DWORD>((std::min)(nextBytes.bytes.size() - offset, sizeof(buffer)));
            DWORD received = 0;
            if (!ReadFile(capability->pinned_, buffer, count, &received, nullptr) || !received ||
                std::memcmp(buffer, nextBytes.bytes.data() + offset, received) != 0)
            { result.reason = "attempt_checkpoint_readback_unavailable"; return result; }
            offset += received;
        }
        if (!StillOwned()) { result.reason = "attempt_directory_changed_during_readback"; return result; }
        capability->publishedId_ = observed;
        result.receipt = std::move(capability);
        result.reason = "attempt_process_receipt_ready_crash_durability_unverified";
        return result;
    }
    catch (...) { result.reason = "attempt_resource_or_observation_unavailable"; return result; }
}

inline AttemptResult CheckpointStore::BeginAttempt(std::wstring_view preFileName, const FILE_ID_INFO& trustedPreFileId,
    std::string_view binding, std::size_t familyIndex, std::size_t caseIndex, std::string_view executionId,
    std::size_t decimalDigitBudget, std::size_t memberBudget, std::size_t replayWorkBudget,
    std::size_t byteBudget) noexcept
{
    return BeginAttemptCore(preFileName, trustedPreFileId, binding, familyIndex, caseIndex, executionId,
        FamilyVersion::v3, byteBudget, [&](const FamilyState& family, std::size_t index) {
            const auto replay = ReplayCase(family, index, decimalDigitBudget, memberBudget, replayWorkBudget);
            return replay.unexecutedRecord && replay.unexecutedRecord->coordinate && replay.unexecutedRecord->graph;
        });
}

inline AttemptResult CheckpointStore::BeginV4Attempt(std::wstring_view preFileName, const FILE_ID_INFO& trustedPreFileId,
    std::string_view binding, std::size_t familyIndex, std::size_t caseIndex, std::string_view executionId,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
    std::size_t replayWorkBudget, std::size_t byteBudget) noexcept
{
    return BeginAttemptCore(preFileName, trustedPreFileId, binding, familyIndex, caseIndex, executionId,
        FamilyVersion::v4, byteBudget, [&](const FamilyState& family, std::size_t index) {
            const auto replay = ReplayV4Case(family, index, coordinateBudget, axisBudget, replayWorkBudget);
            if (!replay.unexecutedRecord || !replay.unexecutedRecord->coordinate) return false;
            const auto& record = *replay.unexecutedRecord;
            // Exactly one graph type must survive authoritative replay. The
            // executor must dispatch according to this type; a receipt alone
            // does not authorize any production input.
            return static_cast<bool>(record.graph) != static_cast<bool>(record.nativeGraph);
        });
}

inline OutcomeCommitResult CheckpointStore::CommitOutcomeCore(InFlightReceipt& receipt,
    const std::shared_ptr<AttemptLineage>& lineage, const OutcomeFacts& facts, bool valid,
    FamilyVersion version, std::size_t byteBudget) noexcept
{
    OutcomeCommitResult result;
    try
    {
        if (!valid || receipt.version_ != version || !receipt.lineage_ || lineage != receipt.lineage_ ||
            GetCurrentThreadId() != receipt.ownerThread_ || !StillOwned() || receipt.pinned_ == INVALID_HANDLE_VALUE ||
            !SameIdentity(receipt.rootId_, locator_.rootId) || !SameIdentity(receipt.directoryId_, locator_.directoryId) ||
            facts.executionId != receipt.executionId_ || facts.status.empty() ||
            facts.receiptConsumed != receipt.consumed_.load(std::memory_order_acquire) ||
            (facts.bridgeEntered && !facts.receiptConsumed) ||
            ((facts.productionStarted || facts.productionEntryUnknown) && !facts.bridgeEntered))
        { result.reason = "attempt_outcome_identity_or_stage_unavailable"; return result; }
        FILE_ID_INFO pinnedId{};
        if (!GetFileInformationByHandleEx(receipt.pinned_, FileIdInfo, &pinnedId, sizeof(pinnedId)) ||
            !SameIdentity(pinnedId, receipt.publishedId_))
        { result.reason = "attempt_inflight_file_identity_unavailable"; return result; }
        auto decoded = DecodeCheckpoint(receipt.preState_, receipt.binding_, byteBudget);
        if (decoded.status != CodecStatus::decoded || receipt.familyIndex_ >= decoded.state.families.size() ||
            receipt.caseIndex_ >= decoded.state.families[receipt.familyIndex_].cases.size())
        { result.reason = "attempt_pre_state_unavailable"; return result; }
        auto& item = decoded.state.families[receipt.familyIndex_].cases[receipt.caseIndex_];
        if ((item.state != WorkState::pending && item.state != WorkState::resource_unavailable) || !item.executionId.empty())
        { result.reason = "attempt_pre_case_unavailable"; return result; }
        // Until a trusted head transaction and reconciliation exist, even an
        // observed no-entry outcome stays unavailable for automatic retry.
        item.executionId = receipt.executionId_;
        item.state = WorkState::outcome_unavailable;
        item.history.push_back(facts);
        const auto expected = EncodeCheckpoint(decoded.state, receipt.binding_, byteBudget);
        if (expected.status != CodecStatus::encoded) { result.reason = "attempt_outcome_encoding_unavailable"; return result; }
        bool notStarted = false;
        if (!receipt.lineage_->commitStarted.compare_exchange_strong(notStarted, true, std::memory_order_acq_rel))
        { result.reason = "attempt_outcome_commit_already_started"; return result; }
        result.checkpoint = Publish(decoded.state, receipt.binding_, byteBudget);
        if (result.checkpoint.status != StoreStatus::published_durability_unverified ||
            !result.checkpoint.fileIdentityCaptured || !result.checkpoint.renameReturnedSuccess)
        { result.reason = "attempt_outcome_publication_unavailable"; return result; }
        const auto readback = Read(result.checkpoint.fileName, result.checkpoint.fileId, receipt.binding_, byteBudget);
        if (readback.status != CodecStatus::decoded || readback.recoveredInFlight)
        { result.reason = "attempt_outcome_readback_unavailable"; return result; }
        const auto checked = EncodeCheckpoint(readback.state, receipt.binding_, byteBudget);
        if (checked.status != CodecStatus::encoded || checked.bytes != expected.bytes)
        { result.reason = "attempt_outcome_readback_mismatch"; return result; }
        std::unique_ptr<CommittedV4Receipt> committed;
        if (version == FamilyVersion::v4)
        {
            if (receipt.prePinned_ == INVALID_HANDLE_VALUE || !UsableIdentity(receipt.preId_))
            { result.reason = "attempt_committed_pre_handle_unavailable"; return result; }
            committed = std::unique_ptr<CommittedV4Receipt>(new CommittedV4Receipt);
            committed->executionId_ = receipt.executionId_;
            committed->preId_ = receipt.preId_; committed->flightId_ = receipt.publishedId_;
            committed->terminalId_ = result.checkpoint.fileId; committed->rootId_ = receipt.rootId_;
            committed->directoryId_ = receipt.directoryId_;
            HANDLE duplicate = nullptr;
            if (receipt.rootPinned_ == INVALID_HANDLE_VALUE || receipt.directoryPinned_ == INVALID_HANDLE_VALUE ||
                !DuplicateHandle(GetCurrentProcess(), receipt.rootPinned_, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
            { result.reason = "attempt_committed_root_duplicate_unavailable"; return result; }
            committed->root_ = duplicate; duplicate = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), receipt.directoryPinned_, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
            { result.reason = "attempt_committed_directory_duplicate_unavailable"; return result; }
            committed->directory_ = duplicate; duplicate = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), receipt.prePinned_, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
            { result.reason = "attempt_committed_pre_duplicate_unavailable"; return result; }
            committed->pre_ = duplicate; duplicate = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), receipt.pinned_, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
            { result.reason = "attempt_committed_flight_duplicate_unavailable"; return result; }
            committed->flight_ = duplicate;
            const auto path = directoryPath_ + L"\\" + result.checkpoint.fileName;
            committed->terminal_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            FILE_ID_INFO id{}; FILE_ATTRIBUTE_TAG_INFO tag{}; LARGE_INTEGER size{};
            if (committed->terminal_ == INVALID_HANDLE_VALUE ||
                !GetFileInformationByHandleEx(committed->terminal_, FileIdInfo, &id, sizeof(id)) ||
                !SameIdentity(id, result.checkpoint.fileId) ||
                !GetFileInformationByHandleEx(committed->terminal_, FileAttributeTagInfo, &tag, sizeof(tag)) ||
                (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                !GetFileSizeEx(committed->terminal_, &size) || size.QuadPart < 0 ||
                static_cast<std::uint64_t>(size.QuadPart) != expected.bytes.size() || !StillOwned())
            { result.reason = "attempt_committed_terminal_identity_unavailable"; return result; }
            char buffer[8192]; std::size_t offset = 0;
            while (offset < expected.bytes.size())
            {
                const auto amount = static_cast<DWORD>((std::min)(expected.bytes.size() - offset, sizeof(buffer)));
                DWORD read = 0;
                if (!ReadFile(committed->terminal_, buffer, amount, &read, nullptr) || !read ||
                    std::memcmp(buffer, expected.bytes.data() + offset, read) != 0)
                { result.reason = "attempt_committed_terminal_readback_unavailable"; return result; }
                offset += read;
            }
            if (!StillOwned()) { result.reason = "attempt_committed_directory_changed"; return result; }
        }
        result.resultingState = std::move(decoded.state);
        result.committedV4 = std::move(committed);
        result.reason = "attempt_outcome_recorded_head_and_crash_durability_unverified";
        return result;
    }
    catch (...) { result.reason = "attempt_outcome_resource_or_observation_unavailable"; return result; }
}

inline OutcomeCommitResult CheckpointStore::CommitOutcome(InFlightReceipt& receipt,
    const ScheduledV3Outcome& outcome, std::size_t byteBudget) noexcept
{ return CommitOutcomeCore(receipt, outcome.lineage_, outcome.facts_, outcome.valid_, FamilyVersion::v3, byteBudget); }

inline OutcomeCommitResult CheckpointStore::CommitOutcome(InFlightReceipt& receipt,
    const ScheduledV4Outcome& outcome, std::size_t byteBudget) noexcept
{ return CommitOutcomeCore(receipt, outcome.lineage_, outcome.facts_, outcome.valid_, FamilyVersion::v4, byteBudget); }

// There is deliberately no implicit checkpoint or execution method: the store
// requires caller-verified ownership and external identity preservation; the
// production adapter dispatch, independent result observation, and a
// transaction owner still need integration. An
// in-flight case recovered after a crash is outcome_unavailable until its
// actual side effect is independently reconciled; it must not be replayed as
// an exactly-once operation merely because a checkpoint exists.
} // namespace drag_global::v3_scheduler

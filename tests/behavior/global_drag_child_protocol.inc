#pragma once
#include "../../src/common/background_work_observation.h"

// Include after global_drag_v3_scheduler.h and global_drag_virtual_staging.inc.
// This is a one-case transport, not an execution authority. The child must
// establish ChildIsolation at its earliest entry, before calling AttachChild.
// A reported outcome still needs independently authenticated checkpoint and
// same-case provenance; process exit and this shared mapping cannot confer it.
namespace drag_global::virtual_child_protocol
{
inline constexpr DWORD kMagic = 0x56434434; // VCD4 local shared-memory transport
inline constexpr DWORD kVersion = 6; // v6 adds a separate external+background VM ceiling; older frames are rejected
inline constexpr std::size_t kLimit = 2 * 1024 * 1024;
struct Shared final
{
    DWORD magic = kMagic, version = kVersion;
    volatile LONG state = 0; // 14/15: separate pending-report offer/ack; 2 remains the legacy inline report
    std::uint64_t requestSize = 0, requestHandle = 0;
    DWORD responseSize = 0, childPid = 0, parentPid = 0;
    FILETIME parentCreated{};
    std::uint64_t childRootHandle = 0;
    FILE_ID_INFO childRootId{};
    DWORD requestKind = 0, leafSize = 0; // 0 fixed case; 1 owned-file template; 2 exact source root
    wchar_t leaf[256]{};
    alignas(8) volatile LONG64 ownedScanGeneration = 0, ownedScanWork = 0, ownedScanLastWork = 0;
    std::uint64_t ownedProfileHandle = 0, ownedSourceHandle = 0;
    FILE_ID_INFO ownedProfileId{}, ownedSourceId{};
    std::uint64_t checkpointHandles[5]{}; // root, directory, pre, in-flight, terminal
    FILE_ID_INFO checkpointIds[5]{};
    // Parent-owned immutable ledger handles, offered only after its private
    // committed capability and trusted output root were verified.
    std::uint64_t parentLedgerHandles[4]{}; // output root, store directory, pre head, new head
    FILE_ID_INFO parentLedgerIds[4]{};
    DWORD parentPreNameSize = 0, parentHeadNameSize = 0, parentBindingSize = 0;
    wchar_t parentPreName[256]{}, parentHeadName[256]{};
    std::uint64_t parentCheckpointBudget = 0;
    char parentBinding[kLimit]{};
    DWORD executionSize = 0;
    char executionId[256]{};
    char nonce[32]{};
    std::uint64_t budgets[28]{};
    std::uint64_t pendingReportHandle = 0, pendingReportSize = 0;
    char response[kLimit]{};
};

struct DiagnosticBudgets final
{
    // A declaration is not a capacity guarantee or execution authority. Zero
    // is an explicit value only when declared is true; otherwise all fields
    // must be zero and diagnostic coverage remains unavailable.
    bool declared = false;
    std::size_t eventSlots = 0, blobBytes = 0, stagingHandles = 0, totalBytes = 0;
    std::size_t backgroundSlots = 0, oleSlots = 0, nativeSlots = 0, evidenceBytes = 0;
    // v6: only the two optional VM allocations, not all process memory.
    std::size_t sharedExternalBackgroundVmBytes = 0;
};
struct Budgets final
{
    V4CoordinateBudget coordinates{};
    V4AxisBudget axes{};
    std::size_t tokenBudget = 0, caseByteBudget = 0;
    DiagnosticBudgets diagnostics{};
};

inline std::array<std::uint64_t, 28> EncodeBudgets(const Budgets& b) noexcept
{
    return {b.coordinates.decimalDigits, b.coordinates.tokenBytes, b.coordinates.listEntries, b.coordinates.arithmeticSteps,
        b.axes.logicalMembers, b.axes.fileMembers, b.axes.virtualMembers, b.axes.referenceMembers, b.axes.payloadSlots,
        b.axes.textSlots, b.axes.requestResultSlots, b.axes.fileContentSlots, b.axes.payloadBytes,
        b.axes.directoryNodeSlots, b.axes.directoryRootSlots, b.axes.preparationSteps, b.tokenBudget, b.caseByteBudget,
        b.diagnostics.declared ? 1u : 0u, b.diagnostics.eventSlots, b.diagnostics.blobBytes, b.diagnostics.stagingHandles,
        b.diagnostics.totalBytes, b.diagnostics.backgroundSlots, b.diagnostics.oleSlots, b.diagnostics.nativeSlots, b.diagnostics.evidenceBytes,
        b.diagnostics.sharedExternalBackgroundVmBytes};
}
inline bool DecodeBudgets(const std::uint64_t (&values)[28], Budgets& out) noexcept
{
    for (std::size_t i = 0; i < 18; ++i)
        if (values[i] > (std::numeric_limits<std::size_t>::max)()) return false;
    if (values[18] > 1) return false;
    for (std::size_t i = 19; i < 28; ++i)
        if (values[i] > (std::numeric_limits<std::size_t>::max)() || (!values[18] && values[i])) return false;
    out.coordinates = {static_cast<std::size_t>(values[0]), static_cast<std::size_t>(values[1]),
        static_cast<std::size_t>(values[2]), static_cast<std::size_t>(values[3])};
    out.axes = {static_cast<std::size_t>(values[4]), static_cast<std::size_t>(values[5]),
        static_cast<std::size_t>(values[6]), static_cast<std::size_t>(values[7]), static_cast<std::size_t>(values[8]),
        static_cast<std::size_t>(values[9]), static_cast<std::size_t>(values[10]), static_cast<std::size_t>(values[11]),
        static_cast<std::size_t>(values[12]), static_cast<std::size_t>(values[13]), static_cast<std::size_t>(values[14]),
        static_cast<std::size_t>(values[15])};
    out.tokenBudget = static_cast<std::size_t>(values[16]); out.caseByteBudget = static_cast<std::size_t>(values[17]);
    out.diagnostics = {values[18] != 0, static_cast<std::size_t>(values[19]), static_cast<std::size_t>(values[20]),
        static_cast<std::size_t>(values[21]), static_cast<std::size_t>(values[22]), static_cast<std::size_t>(values[23]),
        static_cast<std::size_t>(values[24]), static_cast<std::size_t>(values[25]), static_cast<std::size_t>(values[26]),
        static_cast<std::size_t>(values[27])};
    return true;
}

inline bool ValidNonce(std::string_view nonce) noexcept
{
    return nonce.size() == 32 && std::all_of(nonce.begin(), nonce.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// Length-framed bytes are bounded before append. They are an audit payload,
// not a signed receipt, and may contain NULs in evidenceBytes.
inline bool Append(std::string& out, std::string_view value)
{
    const auto prefix = std::to_string(value.size());
    if (value.size() > kLimit || prefix.size() + 1 > kLimit - value.size() ||
        out.size() > kLimit - value.size() - prefix.size() - 1) return false;
    out.append(prefix); out.push_back(':'); out.append(value); return true;
}
inline bool AppendBounded(std::string& out, std::string_view value, std::size_t budget)
{
    const auto prefix = std::to_string(value.size());
    if (value.size() > budget || prefix.size() + 1 > budget - value.size() ||
        out.size() > budget - value.size() - prefix.size() - 1) return false;
    out.append(prefix); out.push_back(':'); out.append(value); return true;
}

struct ChildDiagnosticConfiguration final
{
    bool frameBoundBefore = false, frameBoundAfter = false, configured = false, resourceUnavailable = false;
    bool otherRecorderBudgetsUnresolved = true;
    std::size_t eventCapacity = 0, blobCapacity = 0, stagingHandleCapacity = 0, committedBytes = 0;
    std::string_view reason;
    bool backgroundAttempted = false, backgroundConfigured = false, backgroundResourceUnavailable = false;
    bool sharedBoundAfter = false;
    std::size_t sharedVmBudget = 0, backgroundCapacity = 0, backgroundCommittedBytes = 0;
    std::string_view backgroundReason;
};
struct PendingDiagnosticEvidence final
{
    bool configurationRecorded = false, frameBoundBefore = false, frameBoundAfter = false, configured = false;
    bool resourceUnavailable = false, otherRecorderBudgetsUnresolved = true;
    std::size_t eventCapacity = 0, blobCapacity = 0, stagingHandleCapacity = 0, committedBytes = 0;
    std::string configurationReason;
    snowdesktop::external_transfer_observation::BudgetObservation budget;
    bool backgroundAttempted = false, backgroundConfigured = false, backgroundResourceUnavailable = false;
    bool sharedBoundAfter = false;
    std::size_t sharedVmBudget = 0, backgroundCapacity = 0, backgroundCommittedBytes = 0;
    std::string backgroundReason;
    std::optional<snowdesktop::background_work_observation::BudgetObservation> backgroundBudget;
};
struct PendingReportSummary final
{
    std::string reason;
    std::optional<PendingDiagnosticEvidence> diagnostics;
};
struct PendingReportPublication final
{
    bool createAttempted = false, created = false, flushSucceeded = false, readbackMatched = false;
    bool resourceUnavailable = false;
    std::uint64_t bytesWritten = 0;
    std::optional<FILE_ID_INFO> rootId, fileId;
    std::wstring rootPath, leaf;
    std::string_view reason;
    // Values reported by the child; publication does not authenticate them as
    // production completion or a durable latest locator.
    std::optional<PendingReportSummary> pendingReport;
};
enum class PendingReportDisposition { unavailable, inline_published, offered };
// Parsing authenticates neither execution nor the producer. The caller must
// separately bind these full bytes to its live child, case and locator.
inline bool ParsePendingReport(std::string_view bytes, std::string_view expectedCase, PendingReportSummary& out)
{
    std::string_view token;
    if (!event_graph::ReadGraphToken(bytes, token) || (token != "v4_child_report_v1" && token != "v4_child_report_v2" && token != "v4_child_report_v3")) return false;
    const bool v2 = token != "v4_child_report_v1", v3 = token == "v4_child_report_v3";
    if (!event_graph::ReadGraphToken(bytes, token) || token != expectedCase ||
        !event_graph::ReadGraphToken(bytes, token) || token != "pending" ||
        !event_graph::ReadGraphToken(bytes, token) || token != "unexecuted" ||
        !event_graph::ReadGraphToken(bytes, token)) return false;
    out.reason.assign(token);
    if (!v2) return bytes.empty();
    PendingDiagnosticEvidence value;
    const auto number = [&](std::size_t& dest) {
        return event_graph::ReadGraphToken(bytes, token) && event_graph::ParseCanonicalSize(token, dest);
    };
    const auto flag = [&](bool& dest) {
        std::size_t raw = 0; if (!number(raw) || raw > 1) return false; dest = raw != 0; return true;
    };
    if (!flag(value.configurationRecorded)) return false;
    if (value.configurationRecorded &&
        (!flag(value.frameBoundBefore) || !flag(value.frameBoundAfter) || !flag(value.configured) ||
         !flag(value.resourceUnavailable) || !flag(value.otherRecorderBudgetsUnresolved) ||
         !number(value.eventCapacity) || !number(value.blobCapacity) || !number(value.stagingHandleCapacity) ||
         !number(value.committedBytes) || !event_graph::ReadGraphToken(bytes, token))) return false;
    if (value.configurationRecorded) value.configurationReason.assign(token);
    if (!number(value.budget.eventCapacity) || !number(value.budget.blobCapacity) || !number(value.budget.stagingHandleCapacity) ||
        !number(value.budget.committedBytes) || !number(value.budget.recordedEvents) || !number(value.budget.recordedBlobBytes) ||
        !number(value.budget.recordedStagingHandles) || !flag(value.budget.captureClaimed) || !flag(value.budget.writersDrained) ||
        !flag(value.budget.recordsLost) || !flag(value.budget.capacityExceeded)) return false;
    if (v3)
    {
        if (!flag(value.backgroundAttempted) || !flag(value.backgroundConfigured) || !flag(value.backgroundResourceUnavailable) ||
            !flag(value.sharedBoundAfter) || !number(value.sharedVmBudget) || !number(value.backgroundCapacity) ||
            !number(value.backgroundCommittedBytes) || !event_graph::ReadGraphToken(bytes, token)) return false;
        value.backgroundReason.assign(token);
        snowdesktop::background_work_observation::BudgetObservation actual;
        if (!number(actual.eventCapacity) || !number(actual.committedBytes) || !number(actual.recordedEvents) ||
            !flag(actual.captureClaimed) || !flag(actual.writersDrained) || !flag(actual.recordsLost) || !flag(actual.capacityExceeded)) return false;
        value.backgroundBudget = actual;
    }
    if (!bytes.empty()) return false;
    out.diagnostics = std::move(value); return true;
}
inline bool ReportFileMatches(HANDLE file, std::string_view expected) noexcept
{
    FILE_STANDARD_INFO before{}, after{}; FILE_BASIC_INFO basicBefore{}, basicAfter{};
    if (!file || file == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandleEx(file, FileStandardInfo, &before, sizeof(before)) ||
        !GetFileInformationByHandleEx(file, FileBasicInfo, &basicBefore, sizeof(basicBefore)) ||
        before.EndOfFile.QuadPart < 0 || static_cast<std::uint64_t>(before.EndOfFile.QuadPart) != expected.size()) return false;
    LARGE_INTEGER zero{}; if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) return false;
    std::array<char, 65536> buffer{}; std::size_t offset = 0;
    while (offset < expected.size())
    {
        const auto wanted = static_cast<DWORD>((std::min)(buffer.size(), expected.size() - offset)); DWORD got = 0;
        if (!ReadFile(file, buffer.data(), wanted, &got, nullptr) || got != wanted ||
            std::memcmp(buffer.data(), expected.data() + offset, wanted) != 0) return false;
        offset += wanted;
    }
    return GetFileInformationByHandleEx(file, FileStandardInfo, &after, sizeof(after)) &&
        GetFileInformationByHandleEx(file, FileBasicInfo, &basicAfter, sizeof(basicAfter)) &&
        before.EndOfFile.QuadPart == after.EndOfFile.QuadPart &&
        basicBefore.LastWriteTime.QuadPart == basicAfter.LastWriteTime.QuadPart &&
        basicBefore.ChangeTime.QuadPart == basicAfter.ChangeTime.QuadPart;
}

struct ParentResult final
{
    bool started = false, exited = false, reportReceived = false, rootTransferred = false;
    bool reportedPending = false;
    DWORD childPid = 0, exitCode = 0, error = 0;
    std::string caseId, report, pendingReason;
    std::optional<PendingDiagnosticEvidence> pendingDiagnostics;
    // Even a valid child report is not an authenticated outcome checkpoint.
    bool sameCaseOutcomeAuthenticated = false;
    bool checkpointTransitionVerified = false;
    // These retained handles prevent child-side directory removal. This is
    // not a claim that cleanup was attempted or that producers have stopped.
    bool ownedDirectoryPinsHeld = false;
};

inline bool SameId(const FILE_ID_INFO& a, const FILE_ID_INFO& b) noexcept
{
    return a.VolumeSerialNumber == b.VolumeSerialNumber &&
        std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) == 0 &&
        std::any_of(std::begin(a.FileId.Identifier), std::end(a.FileId.Identifier), [](BYTE x) { return x != 0; });
}
inline bool HandleIdentity(HANDLE handle, const FILE_ID_INFO& expected, bool directory) noexcept
{
    FILE_ID_INFO id{}; FILE_ATTRIBUTE_TAG_INFO tag{};
    return handle && handle != INVALID_HANDLE_VALUE && GetFileType(handle) == FILE_TYPE_DISK &&
        GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) && SameId(id, expected) &&
        GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) &&
        !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
}
inline std::wstring HandlePath(HANDLE handle)
{
    const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_GUID;
    const DWORD size = GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
    if (!size || size > 32768) return {};
    std::wstring path(size, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(handle, path.data(), size, flags);
    if (!written || written >= size) return {};
    path.resize(written); return path;
}
inline bool DirectChild(std::wstring_view parent, std::wstring_view child) noexcept
{
    if (parent.empty() || child.size() <= parent.size() + 1 || child.substr(0, parent.size()) != parent ||
        child[parent.size()] != L'\\') return false;
    return child.find(L'\\', parent.size() + 1) == std::wstring_view::npos;
}
inline bool ReadPinned(HANDLE handle, std::string& bytes, std::size_t budget)
{
    LARGE_INTEGER size{}; FILE_BASIC_INFO before{}, after{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > budget ||
        !GetFileInformationByHandleEx(handle, FileBasicInfo, &before, sizeof(before))) return false;
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    LARGE_INTEGER zero{}; if (!SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN)) return false;
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        DWORD read = 0; const auto count = (std::min)(bytes.size() - offset, static_cast<std::size_t>(MAXDWORD));
        if (!ReadFile(handle, bytes.data() + offset, static_cast<DWORD>(count), &read, nullptr) || !read) return false;
        offset += read;
    }
    LARGE_INTEGER afterSize{};
    return GetFileSizeEx(handle, &afterSize) && size.QuadPart == afterSize.QuadPart &&
        GetFileInformationByHandleEx(handle, FileBasicInfo, &after, sizeof(after)) &&
        before.LastWriteTime.QuadPart == after.LastWriteTime.QuadPart && before.ChangeTime.QuadPart == after.ChangeTime.QuadPart;
}

// Rebase only the selected file-path input onto the newly owned source root.
// The old structured case is a DFS/cursor selection template, never the new
// execution identity. Both graphs are independently replayed and their
// complete presence, partition and order must remain identical.
struct OwnedFileCaseAudit final
{
    bool verified = false, resourceUnavailable = false;
    std::string_view reason;
};
inline OwnedFileCaseAudit VerifyOwnedFileCase(std::string_view templateCase, std::string_view ownedCase,
    std::wstring_view leaf, HANDLE sourceRoot, const FILE_ID_INFO& sourceRootId, const Budgets& budgets)
{
    OwnedFileCaseAudit out;
    try
    {
        const auto oldCase = v3_scheduler::ReplayV4StructuredCaseId(templateCase, budgets.coordinates, budgets.axes,
            budgets.tokenBudget, budgets.caseByteBudget);
        const auto fresh = v3_scheduler::ReplayV4StructuredCaseId(ownedCase, budgets.coordinates, budgets.axes,
            budgets.tokenBudget, budgets.caseByteBudget);
        if (oldCase.status == v3_scheduler::V4StructuredCaseStatus::resource_unavailable ||
            fresh.status == v3_scheduler::V4StructuredCaseStatus::resource_unavailable)
        { out.resourceUnavailable = true; out.reason = "owned_case_replay_budget_unavailable"; return out; }
        if (oldCase.status != v3_scheduler::V4StructuredCaseStatus::unexecuted ||
            fresh.status != v3_scheduler::V4StructuredCaseStatus::unexecuted || !oldCase.family || !fresh.family ||
            !oldCase.candidate || !fresh.candidate || !oldCase.oleGraph || !fresh.oleGraph ||
            oldCase.graphMode != "ole" || fresh.graphMode != "ole" || templateCase == ownedCase ||
            oldCase.family->b || fresh.family->b || oldCase.family->a.kind != V4SourceKind::file_only ||
            fresh.family->a.kind != V4SourceKind::file_only || oldCase.family->a.fileCount.kind != V3CountKind::exact ||
            oldCase.family->a.fileCount.decimal != "1" || oldCase.family->a.filePaths.size() != 1 ||
            oldCase.family->a.filePaths[0].kind != V4PayloadKind::exact || oldCase.family->a.filePaths[0].index != "0" ||
            oldCase.family->a.filePaths[0].base != V4PathBase::raw ||
            fresh.family->a.filePaths.size() != 1 || fresh.family->a.filePaths[0].kind != V4PayloadKind::exact ||
            fresh.family->a.filePaths[0].index != "0" || fresh.family->a.filePaths[0].base != V4PathBase::raw)
        { out.reason = "owned_case_template_or_source_unavailable"; return out; }
        const auto& hex = fresh.family->a.filePaths[0].utf16leHex;
        if (hex.empty() || hex.size() % 4 || hex.size() / 2 > kLimit) { out.reason = "owned_case_path_encoding_invalid"; return out; }
        const auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
        std::wstring text; text.reserve(hex.size() / 4);
        for (std::size_t i = 0; i < hex.size(); i += 4)
        {
            const int a = digit(hex[i]), b = digit(hex[i + 1]), c = digit(hex[i + 2]), d = digit(hex[i + 3]);
            if (a < 0 || b < 0 || c < 0 || d < 0) { out.reason = "owned_case_path_encoding_invalid"; return out; }
            const auto unit = static_cast<wchar_t>((a << 4 | b) | ((c << 4 | d) << 8));
            if (!unit) { out.reason = "owned_case_path_encoding_invalid"; return out; }
            text.push_back(unit);
        }
        const auto& oldHex = oldCase.family->a.filePaths[0].utf16leHex;
        if (oldHex.empty() || oldHex.size() % 4 || oldHex.size() / 2 > kLimit)
        { out.reason = "owned_case_template_path_encoding_invalid"; return out; }
        std::wstring oldText; oldText.reserve(oldHex.size() / 4);
        for (std::size_t i = 0; i < oldHex.size(); i += 4)
        {
            const int a = digit(oldHex[i]), b = digit(oldHex[i + 1]), c = digit(oldHex[i + 2]), d = digit(oldHex[i + 3]);
            if (a < 0 || b < 0 || c < 0 || d < 0) { out.reason = "owned_case_template_path_encoding_invalid"; return out; }
            const auto unit = static_cast<wchar_t>((a << 4 | b) | ((c << 4 | d) << 8));
            if (!unit) { out.reason = "owned_case_template_path_encoding_invalid"; return out; }
            oldText.push_back(unit);
        }
        const std::filesystem::path oldPath(oldText);
        const auto oldName = oldPath.filename().native();
        if (!oldPath.is_absolute() || std::wstring_view(oldName) != leaf)
        { out.reason = "owned_case_template_filename_changed"; return out; }
        const std::filesystem::path actualPath(text);
        const auto seed = MakeV4OwnedRootPathSeed(actualPath.parent_path(), leaf, sourceRoot, sourceRootId);
        if (!seed.available || seed.path != actualPath) { out.reason = "owned_case_root_or_leaf_unavailable"; return out; }
        auto expectedFamily = *oldCase.family; expectedFamily.a.filePaths[0] = seed.slot;
        if (V4FamilyBinding(expectedFamily).empty() || V4FamilyBinding(expectedFamily) != V4FamilyBinding(*fresh.family))
        { out.reason = "owned_case_family_changed"; return out; }
        const auto oldContext = V4FamilyValidatedContext::Create(*oldCase.family, budgets.axes, *oldCase.candidate);
        const auto newContext = V4FamilyValidatedContext::Create(*fresh.family, budgets.axes, *fresh.candidate);
        if (oldContext.Status() == V4CoordinateStatus::resource_unavailable || newContext.Status() == V4CoordinateStatus::resource_unavailable)
        { out.resourceUnavailable = true; out.reason = "owned_case_schema_budget_unavailable"; return out; }
        if (oldContext.Status() != V4CoordinateStatus::coordinate_ready || newContext.Status() != V4CoordinateStatus::coordinate_ready ||
            oldContext.Axes().size() != newContext.Axes().size() || oldContext.Coordinate().values.size() != newContext.Coordinate().values.size())
        { out.reason = "owned_case_schema_unavailable"; return out; }
        std::size_t pathAxes = 0;
        for (std::size_t i = 0; i < oldContext.Axes().size(); ++i)
        {
            const auto& a = oldContext.Axes()[i]; const auto& b = newContext.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.label != b.label) { out.reason = "owned_case_axis_changed"; return out; }
            if (a.id == "V4.file_path.index_0.utf16le") { ++pathAxes; continue; }
            if (a.values != b.values || oldContext.Coordinate().values[i] != newContext.Coordinate().values[i])
            { out.reason = "owned_case_nonpath_selection_changed"; return out; }
        }
        if (pathAxes != 1 || oldCase.present != fresh.present || oldCase.oleGraph->classes != fresh.oleGraph->classes ||
            oldCase.oleGraph->order != fresh.oleGraph->order)
        { out.reason = "owned_case_graph_selection_changed"; return out; }
        out.verified = true; out.reason = "owned_case_selection_only_not_execution"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "owned_case_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "owned_case_resource_unavailable"; return out; }
    catch (...) { out.reason = "owned_case_observation_unavailable"; return out; }
}

class Parent final
{
public:
    Parent(const Parent&) = delete;
    Parent& operator=(const Parent&) = delete;
    ~Parent() { exactCommitted_.reset(); exactAttemptProof_.reset(); ownedTerminalCommitted_.reset(); ownedTerminalProof_.reset(); ownedCommitted_.reset(); ownedAssociationProof_.reset(); for (auto handle : exactLedgerHandles_) if (handle) CloseHandle(handle); for (auto handle : pinned_) if (handle) CloseHandle(handle); if (reportFile_) CloseHandle(reportFile_); if (reportRoot_) CloseHandle(reportRoot_); if (ownedLedgerRoot_) CloseHandle(ownedLedgerRoot_); if (ownedSource_) CloseHandle(ownedSource_); if (ownedProfile_) CloseHandle(ownedProfile_); if (root_) CloseHandle(root_); if (caseView_) UnmapViewOfFile(caseView_); if (caseMapping_) CloseHandle(caseMapping_); if (view_) UnmapViewOfFile(view_); if (mapping_) CloseHandle(mapping_); if (process_) CloseHandle(process_); }

    // Only this mapping is inheritable by the child. This method neither waits
    // nor terminates a child; a timeout or crash leaves execution unknown.
    static std::unique_ptr<Parent> Start(std::string_view nonce, std::string_view caseId,
        const V4CoordinateBudget& coordinates, const V4AxisBudget& axes,
        std::size_t tokenBudget, std::size_t caseByteBudget,
        bool ownedFile = false, std::wstring_view ownedLeaf = {}, bool exactSource = false,
        DiagnosticBudgets diagnosticBudgets = {})
    {
        auto out = std::unique_ptr<Parent>(new Parent);
        if (!ValidNonce(nonce) || caseId.empty() || caseId.size() > caseByteBudget) return out;
        try
        {
            const Budgets supplied{coordinates, axes, tokenBudget, caseByteBudget, diagnosticBudgets};
            out->budgets_ = supplied;
            const auto encodedBudgets = EncodeBudgets(supplied);
            if (!diagnosticBudgets.declared && std::any_of(encodedBudgets.begin() + 19, encodedBudgets.end(), [](auto value) { return value != 0; })) return out;
            const auto replay = v3_scheduler::ReplayV4StructuredCaseId(caseId, coordinates, axes, tokenBudget, caseByteBudget);
            if (ownedFile && exactSource) return out;
            if (replay.status != v3_scheduler::V4StructuredCaseStatus::unexecuted || replay.caseId != caseId ||
                (ownedFile ? (replay.graphMode != "ole" || !replay.oleGraph) :
                    !((replay.graphMode == "ole" && replay.oleGraph) ||
                      (replay.graphMode == "native" && replay.nativeGraph)))) return out;
            if (exactSource && (!replay.family || replay.family->b || replay.family->a.kind != V4SourceKind::file_only ||
                !ownedLeaf.empty())) return out;
            if (ownedFile && (!replay.family || replay.family->b || replay.family->a.kind != V4SourceKind::file_only ||
                replay.family->a.fileCount.kind != V3CountKind::exact || replay.family->a.fileCount.decimal != "1" ||
                ownedLeaf.empty() || ownedLeaf.size() > 256 || ownedLeaf.find(L'\0') != std::wstring_view::npos ||
                ownedLeaf == L"." || ownedLeaf == L".." || ownedLeaf.back() == L'.' || ownedLeaf.back() == L' ' ||
                ownedLeaf.find_first_of(L"\\/:*?<>|\"") != std::wstring_view::npos)) return out;
            out->caseId_ = std::string(caseId); out->nonce_ = std::string(nonce);
            out->requestCaseId_ = out->caseId_; out->ownedFile_ = ownedFile; out->exactSource_ = exactSource;
            if (ownedFile) { out->ownedLeaf_.assign(ownedLeaf); out->expectedExecutionId_.assign(nonce); }
            if (exactSource) out->expectedExecutionId_.assign(nonce);
            // Fill a separate mapping, then keep and inherit only a read-only
            // handle. No case bytes are truncated to the control-frame limit.
            const auto caseBytes = static_cast<std::uint64_t>(caseId.size());
            HANDLE writableCase = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                static_cast<DWORD>(caseBytes >> 32), static_cast<DWORD>(caseBytes), nullptr);
            if (!writableCase) { out->error_ = GetLastError(); return out; }
            struct CloseCase { HANDLE h; ~CloseCase() { if (h) CloseHandle(h); } } closeCase{writableCase};
            void* writableView = MapViewOfFile(writableCase, FILE_MAP_WRITE, 0, 0, caseId.size());
            if (!writableView) { out->error_ = GetLastError(); return out; }
            struct UnmapCase { void* p; ~UnmapCase() { if (p) UnmapViewOfFile(p); } } unmapCase{writableView};
            std::memcpy(writableView, caseId.data(), caseId.size());
            if (!UnmapViewOfFile(writableView)) { out->error_ = GetLastError(); return out; }
            unmapCase.p = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), writableCase, GetCurrentProcess(), &out->caseMapping_,
                FILE_MAP_READ, TRUE, 0)) { out->error_ = GetLastError(); return out; }
            struct ClearInheritance final { HANDLE handle; ~ClearInheritance() {
                const DWORD saved = GetLastError(); SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0); SetLastError(saved);
            } } clearCaseInheritance{out->caseMapping_};
            if (!CloseHandle(closeCase.h)) { out->error_ = GetLastError(); return out; }
            closeCase.h = nullptr;
            out->caseView_ = static_cast<const char*>(MapViewOfFile(out->caseMapping_, FILE_MAP_READ, 0, 0, caseId.size()));
            if (!out->caseView_ || std::memcmp(out->caseView_, caseId.data(), caseId.size()) != 0) return out;
            SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
            out->mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0,
                static_cast<DWORD>(sizeof(Shared)), nullptr);
            if (!out->mapping_) { out->error_ = GetLastError(); return out; }
            ClearInheritance clearInheritance{out->mapping_};
            out->view_ = static_cast<Shared*>(MapViewOfFile(out->mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
            if (!out->view_) { out->error_ = GetLastError(); return out; }
            // The mapping is zero initialized by Windows, but its C++ object
            // lifetime and header must be initialized explicitly.
            new (out->view_) Shared{};
            out->view_->parentPid = GetCurrentProcessId();
            FILETIME exited{}, kernel{}, user{};
            if (!GetProcessTimes(GetCurrentProcess(), &out->parentCreated_, &exited, &kernel, &user)) return out;
            out->view_->parentCreated = out->parentCreated_;
            std::memcpy(out->view_->nonce, nonce.data(), nonce.size());
            std::copy(encodedBudgets.begin(), encodedBudgets.end(), out->view_->budgets);
            out->view_->requestKind = ownedFile ? 1 : exactSource ? 2 : 0;
            if (ownedFile) { out->view_->leafSize = static_cast<DWORD>(ownedLeaf.size());
                std::copy(ownedLeaf.begin(), ownedLeaf.end(), out->view_->leaf); }
            out->view_->requestSize = caseBytes;
            out->view_->requestHandle = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(out->caseMapping_));
            InterlockedExchange(&out->view_->state, 1);
            std::array<wchar_t, 32768> executable{};
            const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
            if (!length || length >= executable.size()) { out->error_ = GetLastError(); return out; }
            SIZE_T bytes = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
            auto* attrs = static_cast<PPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, bytes));
            if (!attrs) { out->error_ = ERROR_NOT_ENOUGH_MEMORY; return out; }
            struct AttrGuard { PPROC_THREAD_ATTRIBUTE_LIST p; bool initialized = false;
                ~AttrGuard() { if (initialized) DeleteProcThreadAttributeList(p); HeapFree(GetProcessHeap(), 0, p); } } guard{attrs};
            if (!InitializeProcThreadAttributeList(attrs, 1, 0, &bytes)) { out->error_ = GetLastError(); return out; }
            guard.initialized = true;
            HANDLE allowed[] = {out->mapping_, out->caseMapping_};
            if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, allowed, sizeof(allowed), nullptr, nullptr))
            { out->error_ = GetLastError(); return out; }
            std::wstring command = L"\"" + std::wstring(executable.data(), length) +
                (ownedFile ? L"\" --v4-owned-file-child " : exactSource ? L"\" --v4-exact-source-child " : L"\" --v4-virtual-staging-child ");
            command.append(nonce.begin(), nonce.end());
            command += L" " + std::to_wstring(reinterpret_cast<std::uintptr_t>(out->mapping_));
            STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attrs;
            PROCESS_INFORMATION process{};
            if (!CreateProcessW(executable.data(), command.data(), nullptr, nullptr, TRUE,
                EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr, &startup.StartupInfo, &process))
            { out->error_ = GetLastError(); return out; }
            CloseHandle(process.hThread); out->process_ = process.hProcess; out->childPid_ = process.dwProcessId;
            return out;
        }
        catch (const std::bad_alloc&) { out->error_ = ERROR_NOT_ENOUGH_MEMORY; return out; }
        catch (const std::length_error&) { out->error_ = ERROR_NOT_ENOUGH_MEMORY; return out; }
        catch (...) { out->error_ = ERROR_GEN_FAILURE; return out; }
    }

    // Publish only the child's offered pending/unexecuted report into a
    // caller-supplied, ID-checked output root. Creation is exclusive and a
    // failed attempt is never silently retried or removed.
    PendingReportPublication PublishOfferedPendingReport(HANDLE trustedOutputRoot, const FILE_ID_INFO& rootId,
        std::wstring_view freshLeaf, std::size_t reportByteBudget) noexcept
    {
        PendingReportPublication out;
        try
        {
            if (freshLeaf.empty() || freshLeaf.size() > (std::numeric_limits<USHORT>::max)() / sizeof(wchar_t))
            { out.reason = "v5_pending_report_leaf_unavailable"; return out; }
            out.leaf.assign(freshLeaf);
            const auto& leaf = out.leaf;
            if (reportCreateAttempted_ || reportStored_ || !view_ || !process_ || !root_ || !BudgetsUnchanged() ||
                !budgets_.diagnostics.declared || !reportByteBudget || reportByteBudget != budgets_.diagnostics.evidenceBytes ||
                view_->childPid != childPid_ || WaitForSingleObject(process_, 0) != WAIT_TIMEOUT ||
                InterlockedCompareExchange(&view_->state, 14, 14) != 14 ||
                !HandleIdentity(trustedOutputRoot, rootId, true) || leaf == L"." || leaf == L".." ||
                leaf.back() == L'.' || leaf.back() == L' ' || leaf.find(L'\0') != std::wstring::npos ||
                leaf.find_first_of(L"\\/:*?<>|\"") != std::wstring::npos)
            { out.reason = "v5_pending_report_context_unavailable"; return out; }
            const auto reportSize = view_->pendingReportSize, reportHandle = view_->pendingReportHandle;
            if (!reportSize || reportSize > reportByteBudget || reportSize > (std::numeric_limits<std::size_t>::max)() ||
                !reportHandle || reportHandle > (std::numeric_limits<std::uintptr_t>::max)())
            { out.resourceUnavailable = reportSize > reportByteBudget || reportSize > (std::numeric_limits<std::size_t>::max)();
              out.reason = "v5_pending_report_budget_or_handle_unavailable"; return out; }
            HANDLE reportMapping = nullptr;
            if (!DuplicateHandle(process_, reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(reportHandle)),
                GetCurrentProcess(), &reportMapping, FILE_MAP_READ, FALSE, 0))
            { out.reason = "v5_pending_report_mapping_unavailable"; return out; }
            struct HandleGuard { HANDLE h; ~HandleGuard() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } } mappingGuard{reportMapping};
            const auto* mapped = static_cast<const char*>(MapViewOfFile(reportMapping, FILE_MAP_READ, 0, 0, static_cast<std::size_t>(reportSize)));
            if (!mapped) { out.reason = "v5_pending_report_view_unavailable"; return out; }
            struct ViewGuard { const void* p; ~ViewGuard() { if (p) UnmapViewOfFile(p); } } viewGuard{mapped};
            std::string bytes(mapped, static_cast<std::size_t>(reportSize));
            PendingReportSummary summary;
            if (!ParsePendingReport(bytes, caseId_, summary) ||
                view_->pendingReportSize != reportSize || view_->pendingReportHandle != reportHandle ||
                view_->childPid != childPid_ || !BudgetsUnchanged() || WaitForSingleObject(process_, 0) != WAIT_TIMEOUT ||
                InterlockedCompareExchange(&view_->state, 14, 14) != 14)
            { out.reason = "v5_pending_report_frame_unavailable"; return out; }
            out.pendingReport.emplace(std::move(summary));
            out.rootId = rootId;
            HANDLE root = ReOpenFile(trustedOutputRoot, FILE_ADD_FILE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
            if (root == INVALID_HANDLE_VALUE) { out.reason = "v5_pending_report_root_pin_unavailable"; return out; }
            HandleGuard rootGuard{root};
            if (!HandleIdentity(root, rootId, true)) { out.reason = "v5_pending_report_root_changed"; return out; }
            const auto rootPath = HandlePath(root), childPath = HandlePath(root_);
            if (rootPath.empty() || childPath.empty() || SameId(rootId, rootId_) || rootPath == childPath ||
                (rootPath.size() > childPath.size() && rootPath.substr(0, childPath.size()) == childPath && rootPath[childPath.size()] == L'\\'))
            { out.reason = "v5_pending_report_root_inside_child_unavailable"; return out; }
            out.rootPath = rootPath;
            using NtCreate = NTSTATUS (NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
            const auto module = GetModuleHandleW(L"ntdll.dll"); const auto address = module ? GetProcAddress(module, "NtCreateFile") : nullptr;
            NtCreate create = nullptr; static_assert(sizeof(create) == sizeof(address)); std::memcpy(&create, &address, sizeof(create));
            if (!create) { out.reason = "v5_pending_report_relative_create_unavailable"; return out; }
            UNICODE_STRING name{}; name.Buffer = const_cast<PWSTR>(leaf.data());
            name.Length = static_cast<USHORT>(leaf.size() * sizeof(wchar_t)); name.MaximumLength = name.Length;
            OBJECT_ATTRIBUTES attributes{}; attributes.Length = sizeof(attributes); attributes.RootDirectory = root;
            attributes.ObjectName = &name; attributes.Attributes = 0x40;
            IO_STATUS_BLOCK io{}; HANDLE file = nullptr;
            constexpr ULONG createNew = 2, nonDirectory = 0x40, synchronous = 0x20, openReparse = 0x00200000;
            if (!BudgetsUnchanged() || !HandleIdentity(root, rootId, true) || view_->pendingReportSize != reportSize ||
                view_->pendingReportHandle != reportHandle || view_->childPid != childPid_ || WaitForSingleObject(process_, 0) != WAIT_TIMEOUT ||
                InterlockedCompareExchange(&view_->state, 14, 14) != 14)
            { out.reason = "v5_pending_report_changed_before_create"; return out; }
            reportCreateAttempted_ = true; out.createAttempted = true;
            const auto status = create(&file, FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                &attributes, &io, nullptr, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, createNew,
                nonDirectory | synchronous | openReparse, nullptr, 0);
            HandleGuard fileGuard{file};
            if (status < 0 || !file || file == INVALID_HANDLE_VALUE) { out.reason = "v5_pending_report_create_unavailable"; return out; }
            out.created = true; FILE_ID_INFO fileId{};
            if (!GetFileInformationByHandleEx(file, FileIdInfo, &fileId, sizeof(fileId)) || !SameId(fileId, fileId) ||
                !HandleIdentity(file, fileId, false)) { out.reason = "v5_pending_report_file_identity_unavailable"; return out; }
            out.fileId = fileId;
            while (out.bytesWritten < bytes.size())
            {
                const auto remaining = bytes.size() - static_cast<std::size_t>(out.bytesWritten);
                const auto wanted = static_cast<DWORD>((std::min)(remaining, static_cast<std::size_t>(MAXDWORD)));
                DWORD wrote = 0;
                if (!WriteFile(file, bytes.data() + static_cast<std::size_t>(out.bytesWritten), wanted, &wrote, nullptr) || !wrote)
                { out.reason = "v5_pending_report_write_unavailable"; return out; }
                out.bytesWritten += wrote;
            }
            out.flushSucceeded = FlushFileBuffers(file) != FALSE;
            if (!out.flushSucceeded || !HandleIdentity(root, rootId, true) || !HandleIdentity(file, fileId, false) ||
                !ReportFileMatches(file, bytes) || !BudgetsUnchanged() || view_->pendingReportSize != reportSize ||
                view_->pendingReportHandle != reportHandle || view_->childPid != childPid_ || WaitForSingleObject(process_, 0) != WAIT_TIMEOUT ||
                InterlockedCompareExchange(&view_->state, 14, 14) != 14)
            { out.reason = "v5_pending_report_flush_or_readback_unavailable"; return out; }
            out.readbackMatched = true;
            reportRootId_ = rootId; reportFileId_ = fileId; reportLeaf_ = out.leaf; reportBytes_ = std::move(bytes);
            reportOfferHandle_ = reportHandle; reportOfferSize_ = reportSize;
            reportRoot_ = root; reportFile_ = file; rootGuard.h = nullptr; fileGuard.h = nullptr; reportStored_ = true;
            out.reason = "v5_pending_report_locator_and_ack_pending"; return out;
        }
        catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v5_pending_report_resource_unavailable"; return out; }
        catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v5_pending_report_resource_unavailable"; return out; }
        catch (...) { out.reason = "v5_pending_report_unavailable"; return out; }
    }
    // State 14 is only an offer. Its bytes and output root have not yet been
    // validated or published, and it confers no outcome or locator authority.
    bool PendingReportOffered() const noexcept
    {
        return view_ && process_ && root_ && BudgetsUnchanged() && OriginalCaseMatches() &&
            view_->childPid == childPid_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT &&
            InterlockedCompareExchange(&view_->state, 14, 14) == 14;
    }
    // This is a pending-report ACK, not an outcome or durability verdict. The
    // caller must preserve the returned trusted root/leaf/file ID locator first.
    bool AcknowledgePendingReport() noexcept
    {
        try
        {
            if (!reportStored_ || !reportLocatorVerified_ || reportAcknowledged_ || !view_ || !process_ || !BudgetsUnchanged() ||
                !HandleIdentity(reportRoot_, reportRootId_, true) || !HandleIdentity(reportFile_, reportFileId_, false) ||
                !ReportFileMatches(reportFile_, reportBytes_) || view_->pendingReportSize != reportOfferSize_ ||
                view_->pendingReportHandle != reportOfferHandle_ || view_->childPid != childPid_ || WaitForSingleObject(process_, 0) != WAIT_TIMEOUT ||
                InterlockedCompareExchange(&view_->state, 14, 14) != 14) return false;
            const auto rootPath = HandlePath(reportRoot_), filePath = HandlePath(reportFile_);
            if (!DirectChild(rootPath, filePath) || filePath.substr(rootPath.size() + 1) != reportLeaf_) return false;
            if (InterlockedCompareExchange(&view_->state, 15, 14) != 14) return false;
            reportAcknowledged_ = true; return true;
        }
        catch (...) { return false; }
    }

    ParentResult Poll() const
    {
        ParentResult result; result.caseId = caseId_; result.error = error_;
        result.checkpointTransitionVerified = terminalVerified_;
        result.ownedDirectoryPinsHeld = ownedProfile_ && ownedSource_;
        if (!process_) return result;
        result.started = true; result.childPid = childPid_; result.rootTransferred = root_ != nullptr;
        const DWORD waited = WaitForSingleObject(process_, 0);
        if (waited == WAIT_TIMEOUT) return result;
        if (waited != WAIT_OBJECT_0) { result.error = GetLastError(); return result; }
        result.exited = true;
        if (!GetExitCodeProcess(process_, &result.exitCode)) { result.error = GetLastError(); return result; }
        if (!view_) return result;
        const DWORD responseSize = view_->responseSize;
        if (!BudgetsUnchanged() || InterlockedCompareExchange(&view_->state, 2, 2) != 2 || view_->magic != kMagic ||
            view_->version != kVersion || view_->childPid != childPid_ || !OriginalCaseMatches() ||
            responseSize > kLimit) return result;
        result.report.assign(view_->response, responseSize);
        if (view_->responseSize != responseSize || !BudgetsUnchanged() || InterlockedCompareExchange(&view_->state, 2, 2) != 2) return result;
        result.reportReceived = true;
        PendingReportSummary summary;
        if (ParsePendingReport(result.report, caseId_, summary))
        {
            result.pendingReason = std::move(summary.reason);
            result.pendingDiagnostics = std::move(summary.diagnostics);
            result.reportedPending = true;
        }
        return result; // exitCode is diagnostic, never a product result.
    }
    // Call while the child remains alive. The duplicate pins exactly the
    // child-created root; its path and checkpoint contents are not certified
    // by this transfer. The borrowed handle expires with Parent.
    bool AcknowledgeRoot() noexcept
    {
        if (root_ || !process_ || !view_ || !BudgetsUnchanged() || WaitForSingleObject(process_, 0) != WAIT_TIMEOUT ||
            InterlockedCompareExchange(&view_->state, 3, 3) != 3 || view_->childPid != childPid_ ||
            !view_->childRootHandle || view_->childRootHandle > (std::numeric_limits<std::uintptr_t>::max)()) return false;
        HANDLE duplicate = nullptr;
        const auto remote = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(view_->childRootHandle));
        if (!DuplicateHandle(process_, remote, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) return false;
        FILE_ID_INFO actual{}; FILE_ATTRIBUTE_TAG_INFO tag{};
        const bool valid = GetFileInformationByHandleEx(duplicate, FileIdInfo, &actual, sizeof(actual)) &&
            GetFileInformationByHandleEx(duplicate, FileAttributeTagInfo, &tag, sizeof(tag)) &&
            (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            actual.VolumeSerialNumber == view_->childRootId.VolumeSerialNumber &&
            std::memcmp(actual.FileId.Identifier, view_->childRootId.FileId.Identifier, sizeof(actual.FileId.Identifier)) == 0 &&
            std::any_of(std::begin(actual.FileId.Identifier), std::end(actual.FileId.Identifier), [](BYTE value) { return value != 0; });
        if (!valid || !BudgetsUnchanged() || InterlockedCompareExchange(&view_->state, 4, 3) != 3) { CloseHandle(duplicate); return false; }
        root_ = duplicate; rootId_ = actual; return true;
    }
    HANDLE RootHandle() const noexcept { return root_; }
    std::optional<FILE_ID_INFO> RootIdentity() const noexcept
    { return root_ ? std::optional<FILE_ID_INFO>(rootId_) : std::nullopt; }

    // Exact cases retain their original paths. This verifies and pins the
    // source directory chain; it never rewrites a candidate or creates files.
    bool AcknowledgeExactSource() noexcept
    {
        try
        {
            if (!view_) return false;
            const DWORD responseSize = view_->responseSize;
            if (!exactSource_ || exactSourceVerified_ || !root_ || !process_ || !view_ || !BudgetsUnchanged() ||
                WaitForSingleObject(process_, 0) != WAIT_TIMEOUT || InterlockedCompareExchange(&view_->state, 12, 12) != 12 ||
                view_->magic != kMagic || view_->version != kVersion || view_->requestKind != 2 || view_->childPid != childPid_ ||
                !OriginalCaseMatches() ||
                !view_->ownedProfileHandle || !view_->ownedSourceHandle || !responseSize ||
                responseSize > 32768u * sizeof(wchar_t) || responseSize % sizeof(wchar_t) ||
                view_->ownedProfileHandle > (std::numeric_limits<std::uintptr_t>::max)() ||
                view_->ownedSourceHandle > (std::numeric_limits<std::uintptr_t>::max)()) return false;
            const auto profileId = view_->ownedProfileId, sourceId = view_->ownedSourceId;
            std::wstring path(responseSize / sizeof(wchar_t), L'\0');
            std::memcpy(path.data(), view_->response, responseSize);
            if (view_->responseSize != responseSize) return false;
            if (path.find(L'\0') != std::wstring::npos) return false;
            HANDLE profile = nullptr, source = nullptr;
            struct Guard { HANDLE& a; HANDLE& b; ~Guard() { if (a) CloseHandle(a); if (b) CloseHandle(b); } } guard{profile, source};
            if (!DuplicateHandle(process_, reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(view_->ownedProfileHandle)),
                    GetCurrentProcess(), &profile, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
                !DuplicateHandle(process_, reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(view_->ownedSourceHandle)),
                    GetCurrentProcess(), &source, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
                !HandleIdentity(root_, rootId_, true) || !HandleIdentity(profile, profileId, true) ||
                !HandleIdentity(source, sourceId, true) || SameId(rootId_, profileId) ||
                SameId(rootId_, sourceId) || SameId(profileId, sourceId)) return false;
            const DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
            HANDLE pinnedRoot = ReOpenFile(root_, FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE, flags);
            if (pinnedRoot == INVALID_HANDLE_VALUE) return false;
            struct RootGuard { HANDLE h; ~RootGuard() { if (h) CloseHandle(h); } } rootGuard{pinnedRoot};
            if (!HandleIdentity(pinnedRoot, rootId_, true)) return false;
            HANDLE pin = ReOpenFile(profile, FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE, flags);
            if (pin == INVALID_HANDLE_VALUE) return false;
            CloseHandle(profile); profile = pin;
            pin = ReOpenFile(source, FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE, flags);
            if (pin == INVALID_HANDLE_VALUE) return false;
            CloseHandle(source); source = pin;
            if (!HandleIdentity(profile, profileId, true) || !HandleIdentity(source, sourceId, true) ||
                !DirectChild(HandlePath(pinnedRoot), HandlePath(profile)) || !DirectChild(HandlePath(profile), HandlePath(source))) return false;
            const auto plan = v3_scheduler::PrepareV4ExactOwnedFiles(requestCaseId_, budgets_.coordinates, budgets_.axes,
                budgets_.tokenBudget, budgets_.caseByteBudget, std::filesystem::path(path), source, sourceId,
                budgets_.axes.fileMembers, budgets_.axes.payloadBytes);
            if (plan.status != v3_scheduler::V4ExactOwnedFilesStatus::planned || !BudgetsUnchanged() ||
                !SameId(view_->ownedProfileId, profileId) || !SameId(view_->ownedSourceId, sourceId) ||
                view_->responseSize != path.size() * sizeof(wchar_t) ||
                std::memcmp(view_->response, path.data(), path.size() * sizeof(wchar_t)) != 0 ||
                WaitForSingleObject(process_, 0) != WAIT_TIMEOUT || InterlockedCompareExchange(&view_->state, 12, 12) != 12) return false;
            exactSourcePath_ = std::filesystem::path(path);
            ownedProfile_ = profile; ownedSource_ = source; profile = source = nullptr;
            ownedProfileId_ = profileId; ownedSourceId_ = sourceId;
            CloseHandle(root_); root_ = pinnedRoot; rootGuard.h = nullptr;
            if (InterlockedCompareExchange(&view_->state, 13, 12) != 12)
            { CloseHandle(ownedProfile_); CloseHandle(ownedSource_); ownedProfile_ = ownedSource_ = nullptr; return false; }
            exactSourceVerified_ = true;
            return true;
        }
        catch (...) { return false; }
    }

    // A source root is accepted only while the child and its Profile are
    // alive, as a direct child of the child's already pinned isolation root.
    // Replaying the new case does not authorize production execution.
    bool AcknowledgeOwnedCase() noexcept
    {
        try
        {
            if (!view_) return false;
            const DWORD responseSize = view_->responseSize;
            if (!ownedFile_ || ownedCaseVerified_ || ownedCasePrepared_ || !root_ || !process_ || !view_ || !BudgetsUnchanged() ||
                WaitForSingleObject(process_, 0) != WAIT_TIMEOUT || InterlockedCompareExchange(&view_->state, 9, 9) != 9 ||
                view_->magic != kMagic || view_->version != kVersion || view_->requestKind != 1 || view_->childPid != childPid_ ||
                !OriginalCaseMatches() ||
                view_->leafSize != ownedLeaf_.size() || std::memcmp(view_->leaf, ownedLeaf_.data(), ownedLeaf_.size() * sizeof(wchar_t)) != 0 ||
                !responseSize || responseSize > kLimit || !view_->ownedProfileHandle || !view_->ownedSourceHandle ||
                view_->ownedProfileHandle > (std::numeric_limits<std::uintptr_t>::max)() ||
                view_->ownedSourceHandle > (std::numeric_limits<std::uintptr_t>::max)()) return false;
            HANDLE profile = nullptr, source = nullptr;
            struct Guard { HANDLE& a; HANDLE& b; ~Guard() { if (a) CloseHandle(a); if (b) CloseHandle(b); } } guard{profile, source};
            if (!DuplicateHandle(process_, reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(view_->ownedProfileHandle)),
                    GetCurrentProcess(), &profile, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
                !DuplicateHandle(process_, reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(view_->ownedSourceHandle)),
                    GetCurrentProcess(), &source, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
                !HandleIdentity(root_, rootId_, true) ||
                !HandleIdentity(profile, view_->ownedProfileId, true) || !HandleIdentity(source, view_->ownedSourceId, true) ||
                SameId(rootId_, view_->ownedProfileId) || SameId(rootId_, view_->ownedSourceId) ||
                SameId(view_->ownedProfileId, view_->ownedSourceId)) return false;
            // Acquire our own deny-delete handles, rather than assuming the
            // sharing mode of a handle offered by the child.
            const DWORD directoryFlags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
            HANDLE pinnedProfile = ReOpenFile(profile, FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE, directoryFlags);
            if (pinnedProfile == INVALID_HANDLE_VALUE) return false;
            CloseHandle(profile); profile = pinnedProfile;
            HANDLE pinnedSource = ReOpenFile(source, FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE, directoryFlags);
            if (pinnedSource == INVALID_HANDLE_VALUE) return false;
            CloseHandle(source); source = pinnedSource;
            if (!HandleIdentity(profile, view_->ownedProfileId, true) ||
                !HandleIdentity(source, view_->ownedSourceId, true)) return false;
            const auto rootPath = HandlePath(root_), profilePath = HandlePath(profile), sourcePath = HandlePath(source);
            if (!DirectChild(rootPath, profilePath) || !DirectChild(profilePath, sourcePath)) return false;
            std::string actualCase(view_->response, responseSize);
            if (view_->responseSize != responseSize) return false;
            const auto audit = VerifyOwnedFileCase(requestCaseId_, actualCase, ownedLeaf_, source, view_->ownedSourceId, budgets_);
            if (!audit.verified)
            {
                if (audit.resourceUnavailable && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT &&
                    InterlockedCompareExchange(&view_->state, 11, 9) == 9) ownedCaseResourceUnavailable_ = true;
                return false;
            }
            if (WaitForSingleObject(process_, 0) != WAIT_TIMEOUT ||
                InterlockedCompareExchange(&view_->state, 9, 9) != 9) return false;
            auto proof = std::shared_ptr<const v3_scheduler::OwnedAssociationProof>(
                new v3_scheduler::OwnedAssociationProof(requestCaseId_, actualCase, profile, view_->ownedProfileId,
                    source, view_->ownedSourceId));
            if (!proof->Live()) return false;
            ownedProfile_ = profile; ownedSource_ = source; profile = source = nullptr;
            ownedProfileId_ = view_->ownedProfileId; ownedSourceId_ = view_->ownedSourceId;
            preparedOwnedCase_.swap(actualCase); ownedAssociationProof_ = std::move(proof); ownedCasePrepared_ = true;
            // Do not ACK yet. The parent must first publish and authenticate
            // a durable association in its own checkpoint store. A child
            // report, temporary directory or caller-provided bool cannot do so.
            return false;
        }
        catch (...) { return false; }
    }
    bool OwnedCaseVerified() const noexcept { return ownedCaseVerified_; }
    bool OwnedCasePrepared() const noexcept { return ownedCasePrepared_; }
    const std::optional<v3_scheduler::OwnedTerminalCapacity>& OwnedTerminalCapacity() const noexcept
    { return ownedTerminalCapacity_; }
    std::string_view PreparedOwnedCaseId() const noexcept { return preparedOwnedCase_; }
    HANDLE OwnedProfileHandle() const noexcept { return ownedProfile_; }
    HANDLE OwnedSourceHandle() const noexcept { return ownedSource_; }
    FILE_ID_INFO OwnedProfileIdentity() const noexcept { return ownedProfileId_; }
    FILE_ID_INFO OwnedSourceIdentity() const noexcept { return ownedSourceId_; }
    std::shared_ptr<const v3_scheduler::OwnedAssociationProof> OwnedAssociationEvidence() const noexcept
    {
        return ownedCasePrepared_ && !ownedCaseVerified_ && process_ && view_ && BudgetsUnchanged() &&
            WaitForSingleObject(process_, 0) == WAIT_TIMEOUT && InterlockedCompareExchange(&view_->state, 9, 9) == 9
            ? ownedAssociationProof_ : nullptr;
    }
    // Only a CheckpointStore-issued capability for this exact proof and its
    // freshly published, verified trusted head can advance the child. The
    // caller supplies an independently owned parent output root, not a child
    // temporary directory. Cross-process recovery still needs that locator.
    bool ConfirmOwnedCase(v3_scheduler::CheckpointStore& store,
        std::unique_ptr<v3_scheduler::CommittedOwnedAssociation>& committed,
        std::wstring_view trustedHeadName, const FILE_ID_INFO& trustedHeadId, std::string_view binding,
        std::size_t checkpointByteBudget, HANDLE parentOutputRoot, const FILE_ID_INFO& parentOutputRootId) noexcept
    {
        try
        {
            if (!ownedCasePrepared_ || ownedCaseVerified_ || !ownedAssociationProof_ || !committed || !process_ || !view_ || !BudgetsUnchanged() ||
                WaitForSingleObject(process_, 0) != WAIT_TIMEOUT || InterlockedCompareExchange(&view_->state, 9, 9) != 9 ||
                !HandleIdentity(parentOutputRoot, parentOutputRootId, true) || !HandleIdentity(root_, rootId_, true) ||
                !HandleIdentity(ownedProfile_, ownedProfileId_, true) || !HandleIdentity(ownedSource_, ownedSourceId_, true)) return false;
            HANDLE pinnedRoot = ReOpenFile(parentOutputRoot, FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
            if (pinnedRoot == INVALID_HANDLE_VALUE) return false;
            struct Guard { HANDLE h; ~Guard() { if (h) CloseHandle(h); } } guard{pinnedRoot};
            if (!HandleIdentity(pinnedRoot, parentOutputRootId, true)) return false;
            const auto parentPath = HandlePath(pinnedRoot), childPath = HandlePath(root_);
            const auto directoryPath = HandlePath(store.DirectoryHandle());
            const auto profilePath = HandlePath(ownedProfile_), sourcePath = HandlePath(ownedSource_);
            if (parentPath.empty() || childPath.empty() || parentPath == childPath ||
                (parentPath.size() > childPath.size() && parentPath.substr(0, childPath.size()) == childPath &&
                 parentPath[childPath.size()] == L'\\') || !DirectChild(parentPath, directoryPath) ||
                directoryPath == childPath || (directoryPath.size() > childPath.size() &&
                    directoryPath.substr(0, childPath.size()) == childPath && directoryPath[childPath.size()] == L'\\') ||
                !DirectChild(childPath, profilePath) || !DirectChild(profilePath, sourcePath) ||
                !VerifyOwnedFileCase(requestCaseId_, preparedOwnedCase_, ownedLeaf_, ownedSource_, ownedSourceId_, budgets_).verified ||
                !store.VerifyOwnedAssociation(*committed, ownedAssociationProof_, trustedHeadName, trustedHeadId,
                    binding, checkpointByteBudget)) return false;
            // The owned child runner uses kLimit for its checkpoint, and the
            // nonce is also required as the execution ID at attempt handoff.
            // This bounds only the parent's encoded envelope, not whether all
            // real child evidence fits that child budget or can be persisted.
            ownedTerminalCapacity_ = store.InspectOwnedTerminalCapacity(*committed, ownedAssociationProof_,
                trustedHeadName, trustedHeadId, binding, expectedExecutionId_, kLimit, checkpointByteBudget);
            if (!ownedTerminalCapacity_->sufficient)
            {
                if (ownedTerminalCapacity_->resourceUnavailable) (void)RejectOwnedForResources();
                return false;
            }
            if (!BudgetsUnchanged() || WaitForSingleObject(process_, 0) != WAIT_TIMEOUT ||
                InterlockedCompareExchange(&view_->state, 10, 9) != 9) return false;
            ownedLedgerRoot_ = pinnedRoot; ownedLedgerRootId_ = parentOutputRootId; guard.h = nullptr; ownedCommitted_ = std::move(committed);
            caseId_.swap(preparedOwnedCase_); ownedCaseVerified_ = true; return true;
        }
        catch (...) { return false; }
    }
    bool OwnedCaseResourceUnavailable() const noexcept { return ownedCaseResourceUnavailable_; }
    bool RejectOwnedForResources() noexcept
    {
        if (!ownedFile_ || !ownedCasePrepared_ || ownedCaseVerified_ || !process_ || !view_ ||
            WaitForSingleObject(process_, 0) != WAIT_TIMEOUT || InterlockedCompareExchange(&view_->state, 11, 9) != 9) return false;
        ownedCaseResourceUnavailable_ = true; return true;
    }
    std::string_view SelectedCaseId() const noexcept { return caseId_; }
    std::string_view TemplateCaseId() const noexcept { return requestCaseId_; }
    // Progress only controls the wait deadline. It is neither an execution
    // receipt nor proof of coverage; child code reports actual work advances.
    struct OwnedScanProgress { std::uint64_t pages = 0, totalWork = 0, lastWork = 0; };
    std::optional<OwnedScanProgress> ConsumeOwnedScanProgress() noexcept
    {
        if (!ownedFile_ || !root_ || !process_ || !view_ || WaitForSingleObject(process_, 0) != WAIT_TIMEOUT ||
            view_->childPid != childPid_ || InterlockedCompareExchange(&view_->state, 4, 4) != 4) return std::nullopt;
        const auto generation = InterlockedCompareExchange64(&view_->ownedScanGeneration, 0, 0);
        if (generation <= 0 || (generation & 1)) return std::nullopt;
        const auto work = InterlockedCompareExchange64(&view_->ownedScanWork, 0, 0);
        const auto last = InterlockedCompareExchange64(&view_->ownedScanLastWork, 0, 0);
        if (InterlockedCompareExchange64(&view_->ownedScanGeneration, 0, 0) != generation || work < 0 || last < 0 || last > work)
            return std::nullopt;
        const auto pages = static_cast<std::uint64_t>(generation / 2);
        if (pages <= lastOwnedScanPages_ || static_cast<std::uint64_t>(work) < lastOwnedScanWork_) return std::nullopt;
        lastOwnedScanPages_ = pages; lastOwnedScanWork_ = static_cast<std::uint64_t>(work);
        return OwnedScanProgress{pages, lastOwnedScanWork_, static_cast<std::uint64_t>(last)};
    }

    // The child is blocked at this boundary. Only a privately minted
    // InFlightReceipt can offer it; the parent still independently pins and
    // checks every file and reconstructs the authoritative case/graph.
    bool AcknowledgeAttempt(std::size_t replayWorkBudget) noexcept
    {
        try
        {
            if (!view_) return false;
            const DWORD executionSize = view_->executionSize;
            if (!root_ || !process_ || !view_ || !BudgetsUnchanged() || (ownedFile_ && !ownedCaseVerified_) ||
                (exactSource_ && (!exactSourceVerified_ || !HandleIdentity(ownedProfile_, ownedProfileId_, true) ||
                    !HandleIdentity(ownedSource_, ownedSourceId_, true) || !SameId(view_->ownedProfileId, ownedProfileId_) ||
                    !SameId(view_->ownedSourceId, ownedSourceId_) || !DirectChild(HandlePath(root_), HandlePath(ownedProfile_)) ||
                    !DirectChild(HandlePath(ownedProfile_), HandlePath(ownedSource_)))) || WaitForSingleObject(process_, 0) != WAIT_TIMEOUT ||
                InterlockedCompareExchange(&view_->state, 5, 5) != 5 || attemptVerified_ || exactAttemptPrepared_ ||
                view_->magic != kMagic || view_->version != kVersion || view_->requestKind != (ownedFile_ ? 1u : exactSource_ ? 2u : 0u) || view_->childPid != childPid_ ||
                !OriginalCaseMatches() ||
                !executionSize || executionSize > sizeof(view_->executionId) ||
                ((ownedFile_ || exactSource_) && (executionSize != expectedExecutionId_.size() ||
                    std::memcmp(view_->executionId, expectedExecutionId_.data(), expectedExecutionId_.size()) != 0)) ||
                !SameId(view_->checkpointIds[0], rootId_)) return false;
            std::array<HANDLE, 5> handles{};
            struct Guard { std::array<HANDLE, 5>& h; ~Guard() { for (auto x : h) if (x) CloseHandle(x); } } guard{handles};
            const std::array<FILE_ID_INFO, 5> ids{view_->checkpointIds[0], view_->checkpointIds[1],
                view_->checkpointIds[2], view_->checkpointIds[3], {}};
            for (std::size_t i = 0; i < 4; ++i)
                if (!DuplicateOffered(i, handles[i])) return false;
            const auto rootPath = HandlePath(root_), offeredRootPath = HandlePath(handles[0]);
            const auto directoryPath = HandlePath(handles[1]);
            if (rootPath.empty() || rootPath != offeredRootPath || !DirectChild(rootPath, directoryPath) ||
                !DirectChild(directoryPath, HandlePath(handles[2])) || !DirectChild(directoryPath, HandlePath(handles[3])) ||
                SameId(ids[2], ids[3])) return false;
            if (exactSource_)
            {
                const auto& sourcePath = exactSourcePath_.native();
                if (view_->responseSize != sourcePath.size() * sizeof(wchar_t) ||
                    std::memcmp(view_->response, sourcePath.data(), sourcePath.size() * sizeof(wchar_t)) != 0 ||
                    v3_scheduler::PrepareV4ExactOwnedFiles(requestCaseId_, budgets_.coordinates, budgets_.axes,
                        budgets_.tokenBudget, budgets_.caseByteBudget, exactSourcePath_, ownedSource_, ownedSourceId_,
                        budgets_.axes.fileMembers, budgets_.axes.payloadBytes).status != v3_scheduler::V4ExactOwnedFilesStatus::planned)
                    return false;
            }
            std::string pre, flight;
            if (!ReadPinned(handles[2], pre, kLimit) || !ReadPinned(handles[3], flight, kLimit)) return false;
            std::string execution(view_->executionId, executionSize);
            if (view_->executionSize != executionSize) return false;
            const std::string binding = std::string("v4_virtual_child_v1:") + caseId_;
            const auto audit = v3_scheduler::VerifyV4AttemptStart(pre, flight, binding, caseId_, execution,
                budgets_.coordinates, budgets_.axes, budgets_.tokenBudget, budgets_.caseByteBudget, kLimit, replayWorkBudget);
            if (audit.status != v3_scheduler::V4AttemptTransitionStatus::consistent_unverified || !BudgetsUnchanged() ||
                view_->executionSize != executionSize || std::memcmp(view_->executionId, execution.data(), execution.size()) != 0 ||
                WaitForSingleObject(process_, 0) != WAIT_TIMEOUT || InterlockedCompareExchange(&view_->state, 5, 5) != 5) return false;
            if (exactSource_)
            {
                pinned_ = handles; handles.fill(nullptr); pinnedIds_ = ids;
                preBytes_ = std::move(pre); flightBytes_ = std::move(flight); executionId_ = std::move(execution);
                exactAttemptPrepared_ = true;
                // A child transition is only evidence for the parent ledger;
                // it cannot itself authorize fixture creation or state 6.
                return false;
            }
            if (InterlockedCompareExchange(&view_->state, 6, 5) != 5) return false;
            pinned_ = handles; handles.fill(nullptr); pinnedIds_ = ids;
            preBytes_ = std::move(pre); flightBytes_ = std::move(flight); executionId_ = std::move(execution);
            attemptVerified_ = true; return true;
        }
        catch (...) { return false; }
    }
    bool ExactAttemptPrepared() const noexcept { return exactSource_ && exactAttemptPrepared_; }
    // A private, value-and-handle proof of the already checked child offer.
    // This is not a parent ledger publication and never advances state 5.
    std::shared_ptr<const v3_scheduler::ExactAttemptProof> ExactAttemptEvidence(std::size_t replayWorkBudget) noexcept
    {
        try
        {
            if (!ExactAttemptPrepared() || !process_ || !view_ || !BudgetsUnchanged() ||
                WaitForSingleObject(process_, 0) != WAIT_TIMEOUT || InterlockedCompareExchange(&view_->state, 5, 5) != 5 ||
                view_->magic != kMagic || view_->version != kVersion || view_->childPid != childPid_ ||
                view_->requestKind != 2 || !OriginalCaseMatches() || caseId_ != requestCaseId_ ||
                view_->executionSize != executionId_.size() ||
                std::memcmp(view_->executionId, executionId_.data(), executionId_.size()) != 0 ||
                !SameId(view_->childRootId, rootId_) || !HandleIdentity(root_, rootId_, true) || !HandleIdentity(ownedProfile_, ownedProfileId_, true) ||
                !HandleIdentity(ownedSource_, ownedSourceId_, true) ||
                !SameId(view_->ownedProfileId, ownedProfileId_) || !SameId(view_->ownedSourceId, ownedSourceId_) ||
                !DirectChild(HandlePath(root_), HandlePath(ownedProfile_)) ||
                !DirectChild(HandlePath(ownedProfile_), HandlePath(ownedSource_))) return nullptr;
            const auto& sourcePath = exactSourcePath_.native();
            if (view_->responseSize != sourcePath.size() * sizeof(wchar_t) ||
                std::memcmp(view_->response, sourcePath.data(), sourcePath.size() * sizeof(wchar_t)) != 0) return nullptr;
            for (std::size_t i = 0; i < 4; ++i)
                if (!SameId(view_->checkpointIds[i], pinnedIds_[i]) || !HandleIdentity(pinned_[i], pinnedIds_[i], i < 2)) return nullptr;
            std::string pre, flight;
            if (!ReadPinned(pinned_[2], pre, kLimit) || pre != preBytes_ ||
                !ReadPinned(pinned_[3], flight, kLimit) || flight != flightBytes_) return nullptr;
            const auto plan = v3_scheduler::PrepareV4ExactOwnedFiles(caseId_, budgets_.coordinates, budgets_.axes,
                budgets_.tokenBudget, budgets_.caseByteBudget, exactSourcePath_, ownedSource_, ownedSourceId_,
                budgets_.axes.fileMembers, budgets_.axes.payloadBytes);
            const std::string binding = std::string("v4_virtual_child_v1:") + caseId_;
            const auto audit = v3_scheduler::VerifyV4AttemptStart(pre, flight, binding, caseId_, executionId_,
                budgets_.coordinates, budgets_.axes, budgets_.tokenBudget, budgets_.caseByteBudget, kLimit, replayWorkBudget);
            if (plan.status != v3_scheduler::V4ExactOwnedFilesStatus::planned ||
                audit.status != v3_scheduler::V4AttemptTransitionStatus::consistent_unverified) return nullptr;
            if (!exactAttemptProof_)
            {
                const std::array<HANDLE, 6> handles{root_, ownedProfile_, ownedSource_, pinned_[1], pinned_[2], pinned_[3]};
                const std::array<FILE_ID_INFO, 6> ids{rootId_, ownedProfileId_, ownedSourceId_, pinnedIds_[1], pinnedIds_[2], pinnedIds_[3]};
                auto proof = std::shared_ptr<const v3_scheduler::ExactAttemptProof>(new v3_scheduler::ExactAttemptProof(
                    caseId_, executionId_, exactSourcePath_, preBytes_, flightBytes_, handles, ids));
                if (!proof->Live()) return nullptr;
                exactAttemptProof_ = std::move(proof);
            }
            return exactAttemptProof_->Live() && BudgetsUnchanged() && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT &&
                InterlockedCompareExchange(&view_->state, 5, 5) == 5 ? exactAttemptProof_ : nullptr;
        }
        catch (...) { return nullptr; }
    }
    // state 6 confirms the parent's separately published ledger only. Child
    // materialization remains blocked until the child independently pins and
    // replays that ledger and the one-use permit is fully bound to it.
    bool ConfirmExactAttempt(v3_scheduler::CheckpointStore& store,
        std::unique_ptr<v3_scheduler::CommittedExactAttempt>& committed,
        std::wstring_view trustedHeadName, const FILE_ID_INFO& trustedHeadId, std::string_view binding,
        const V4CoordinateBudget& coordinates, const V4AxisBudget& axes,
        std::size_t tokenBudget, std::size_t caseByteBudget, std::size_t replayWorkBudget,
        std::size_t childByteBudget, std::size_t checkpointByteBudget,
        HANDLE parentOutputRoot, const FILE_ID_INFO& parentOutputRootId) noexcept
    {
        try
        {
            if (!exactSource_ || !exactAttemptPrepared_ || attemptVerified_ || !committed || childByteBudget != kLimit ||
                !process_ || !view_ || !BudgetsUnchanged() || WaitForSingleObject(process_, 0) != WAIT_TIMEOUT ||
                InterlockedCompareExchange(&view_->state, 5, 5) != 5 ||
                !HandleIdentity(parentOutputRoot, parentOutputRootId, true)) return false;
            auto supplied = budgets_; supplied.coordinates = coordinates; supplied.axes = axes;
            supplied.tokenBudget = tokenBudget; supplied.caseByteBudget = caseByteBudget;
            if (EncodeBudgets(supplied) != EncodeBudgets(budgets_)) return false;
            const auto proof = ExactAttemptEvidence(replayWorkBudget);
            if (!proof) return false;
            HANDLE pin = ReOpenFile(parentOutputRoot, FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
            if (pin == INVALID_HANDLE_VALUE) return false;
            struct Guard { HANDLE h; ~Guard() { if (h) CloseHandle(h); } } guard{pin};
            if (!HandleIdentity(pin, parentOutputRootId, true)) return false;
            const auto parentPath = HandlePath(pin), childPath = HandlePath(root_), storePath = HandlePath(store.DirectoryHandle());
            if (parentPath.empty() || childPath.empty() || parentPath == childPath ||
                (parentPath.size() > childPath.size() && parentPath.substr(0, childPath.size()) == childPath &&
                    parentPath[childPath.size()] == L'\\') || !DirectChild(parentPath, storePath) ||
                storePath == childPath || (storePath.size() > childPath.size() && storePath.substr(0, childPath.size()) == childPath &&
                    storePath[childPath.size()] == L'\\') ||
                !store.VerifyExactAttempt(*committed, proof, trustedHeadName, trustedHeadId, binding, coordinates, axes,
                    tokenBudget, caseByteBudget, replayWorkBudget, childByteBudget, checkpointByteBudget) ||
                !ExactAttemptEvidence(replayWorkBudget) || !BudgetsUnchanged()) return false;
            const auto preName = committed->PreHeadName(), headName = committed->HeadName();
            if (preName.empty() || preName.size() > 256 || headName.empty() || headName.size() > 256 ||
                binding.empty() || binding.size() > kLimit || !checkpointByteBudget) return false;
            std::array<HANDLE, 4> offered{};
            struct OfferedGuard { std::array<HANDLE, 4>& handles; ~OfferedGuard() { for (auto h : handles) if (h) CloseHandle(h); } } offeredGuard{offered};
            const std::array<HANDLE, 4> actual{pin, store.DirectoryHandle(), committed->PreHeadHandle(), committed->HeadHandle()};
            std::array<FILE_ID_INFO, 4> ids{parentOutputRootId, {}, committed->PreHeadIdentity(), committed->HeadIdentity()};
            if (!GetFileInformationByHandleEx(actual[1], FileIdInfo, &ids[1], sizeof(ids[1]))) return false;
            for (std::size_t i = 0; i < offered.size(); ++i)
                if (!DuplicateHandle(GetCurrentProcess(), actual[i], GetCurrentProcess(), &offered[i], 0, FALSE, DUPLICATE_SAME_ACCESS) ||
                    !HandleIdentity(offered[i], ids[i], i < 2)) return false;
            if (SameId(ids[2], ids[3]) || !BudgetsUnchanged() || WaitForSingleObject(process_, 0) != WAIT_TIMEOUT ||
                InterlockedCompareExchange(&view_->state, 5, 5) != 5) return false;
            for (std::size_t i = 0; i < offered.size(); ++i)
            { view_->parentLedgerHandles[i] = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(offered[i])); view_->parentLedgerIds[i] = ids[i]; }
            view_->parentPreNameSize = static_cast<DWORD>(preName.size());
            view_->parentHeadNameSize = static_cast<DWORD>(headName.size());
            std::copy(preName.begin(), preName.end(), view_->parentPreName);
            std::copy(headName.begin(), headName.end(), view_->parentHeadName);
            view_->parentBindingSize = static_cast<DWORD>(binding.size());
            std::memcpy(view_->parentBinding, binding.data(), binding.size());
            view_->parentCheckpointBudget = checkpointByteBudget;
            exactLedgerHandles_ = offered; offered.fill(nullptr);
            if (InterlockedCompareExchange(&view_->state, 6, 5) != 5)
            { for (auto& h : exactLedgerHandles_) { if (h) CloseHandle(h); h = nullptr; } return false; }
            ownedLedgerRoot_ = pin; ownedLedgerRootId_ = parentOutputRootId; guard.h = nullptr;
            exactCommitted_ = std::move(committed); attemptVerified_ = true;
            return true;
        }
        catch (...) { return false; }
    }
    bool AcknowledgeTerminal(std::size_t replayWorkBudget) noexcept
    {
        try
        {
            if (!attemptVerified_ || terminalPrepared_ || terminalVerified_ || !view_ || !BudgetsUnchanged() || WaitForSingleObject(process_, 0) != WAIT_TIMEOUT ||
                InterlockedCompareExchange(&view_->state, 7, 7) != 7 || view_->executionSize != executionId_.size() ||
                view_->magic != kMagic || view_->version != kVersion || view_->requestKind != (ownedFile_ ? 1u : exactSource_ ? 2u : 0u) || view_->childPid != childPid_ ||
                !OriginalCaseMatches() ||
                std::memcmp(view_->executionId, executionId_.data(), executionId_.size()) != 0) return false;
            for (std::size_t i = 0; i < 4; ++i)
                if (!SameId(view_->checkpointIds[i], pinnedIds_[i]) || !HandleIdentity(pinned_[i], pinnedIds_[i], i < 2)) return false;
            HANDLE terminal = nullptr;
            if (!DuplicateOffered(4, terminal)) return false;
            struct Guard { HANDLE h; ~Guard() { if (h) CloseHandle(h); } } guard{terminal};
            if (!DirectChild(HandlePath(pinned_[1]), HandlePath(terminal)) ||
                SameId(view_->checkpointIds[4], pinnedIds_[2]) || SameId(view_->checkpointIds[4], pinnedIds_[3])) return false;
            std::string pre, flight, finalBytes;
            if (!ReadPinned(pinned_[2], pre, kLimit) || pre != preBytes_ ||
                !ReadPinned(pinned_[3], flight, kLimit) || flight != flightBytes_ || !ReadPinned(terminal, finalBytes, kLimit)) return false;
            const std::string binding = std::string("v4_virtual_child_v1:") + caseId_;
            const auto audit = v3_scheduler::VerifyV4AttemptTransition(pre, flight, finalBytes, binding, caseId_, executionId_,
                budgets_.coordinates, budgets_.axes, budgets_.tokenBudget, budgets_.caseByteBudget, kLimit, replayWorkBudget);
            if (audit.status != v3_scheduler::V4AttemptTransitionStatus::consistent_unverified || !BudgetsUnchanged() ||
                WaitForSingleObject(process_, 0) != WAIT_TIMEOUT || InterlockedCompareExchange(&view_->state, 7, 7) != 7) return false;
            // Exact-source results need an independently published trusted
            // parent ledger. The generic child transition alone is not that
            // authority, so leave state 7 pending until it is available.
            if (exactSource_) return false;
            if (ownedFile_)
            {
                // The verified child terminal is only locally pinned. Do not
                // ACK until the same raw terminal has been published and read
                // back in the independent parent ledger. Crash durability is
                // not established by this publication.
                if (!ownedCaseVerified_ || !ownedAssociationProof_ || !ownedCommitted_) return false;
                auto handles = pinned_; auto ids = pinnedIds_;
                handles[4] = terminal; ids[4] = view_->checkpointIds[4];
                auto proof = std::shared_ptr<const v3_scheduler::OwnedTerminalProof>(
                    new v3_scheduler::OwnedTerminalProof(ownedAssociationProof_, executionId_, preBytes_, flightBytes_,
                        finalBytes, handles, ids));
                if (!proof->Live()) return false;
                pinned_[4] = terminal; guard.h = nullptr; pinnedIds_[4] = view_->checkpointIds[4];
                terminalBytes_ = std::move(finalBytes); ownedTerminalProof_ = std::move(proof);
                terminalPrepared_ = true; return false;
            }
            if (InterlockedCompareExchange(&view_->state, 8, 7) != 7) return false;
            pinned_[4] = terminal; guard.h = nullptr; pinnedIds_[4] = view_->checkpointIds[4];
            terminalBytes_ = std::move(finalBytes); terminalVerified_ = true; return true;
        }
        catch (...) { return false; }
    }
    std::string_view VerifiedTerminalBytes() const noexcept { return terminalVerified_ ? std::string_view(terminalBytes_) : std::string_view{}; }
    bool OwnedTerminalPrepared() const noexcept { return ownedFile_ && terminalPrepared_; }
    std::shared_ptr<const v3_scheduler::OwnedTerminalProof> OwnedTerminalEvidence() const noexcept
    {
        return OwnedTerminalPrepared() && !terminalVerified_ && process_ && view_ &&
            WaitForSingleObject(process_, 0) == WAIT_TIMEOUT && InterlockedCompareExchange(&view_->state, 7, 7) == 7
            ? ownedTerminalProof_ : nullptr;
    }
    const v3_scheduler::CommittedOwnedAssociation* OwnedCommittedAssociation() const noexcept
    { return OwnedTerminalEvidence() ? ownedCommitted_.get() : nullptr; }
    bool ConfirmOwnedTerminal(v3_scheduler::CheckpointStore& store,
        std::unique_ptr<v3_scheduler::CommittedOwnedTerminal>& committed,
        std::wstring_view trustedHeadName, const FILE_ID_INFO& trustedHeadId, std::string_view binding,
        std::size_t parentCheckpointByteBudget, std::size_t childCheckpointByteBudget,
        std::size_t replayWorkBudget) noexcept
    {
        try
        {
            if (!ownedFile_ || !ownedCaseVerified_ || !BudgetsUnchanged() || childCheckpointByteBudget != kLimit ||
                !OwnedTerminalEvidence() || !committed || !ownedCommitted_ ||
                !HandleIdentity(ownedLedgerRoot_, ownedLedgerRootId_, true) || !HandleIdentity(root_, rootId_, true) ||
                !HandleIdentity(ownedProfile_, ownedProfileId_, true) || !HandleIdentity(ownedSource_, ownedSourceId_, true)) return false;
            const auto parentPath = HandlePath(ownedLedgerRoot_), directoryPath = HandlePath(store.DirectoryHandle());
            const auto childPath = HandlePath(root_), profilePath = HandlePath(ownedProfile_), sourcePath = HandlePath(ownedSource_);
            if (!DirectChild(parentPath, directoryPath) || !DirectChild(childPath, profilePath) ||
                !DirectChild(profilePath, sourcePath) || !ownedTerminalProof_->FilesMatch(childCheckpointByteBudget)) return false;
            const std::string childBinding = std::string("v4_virtual_child_v1:") + caseId_;
            const auto audit = v3_scheduler::VerifyV4AttemptTransition(preBytes_, flightBytes_, terminalBytes_,
                childBinding, caseId_, executionId_, budgets_.coordinates, budgets_.axes, budgets_.tokenBudget,
                budgets_.caseByteBudget, childCheckpointByteBudget, replayWorkBudget);
            if (audit.status != v3_scheduler::V4AttemptTransitionStatus::consistent_unverified ||
                !store.VerifyOwnedTerminal(*committed, ownedTerminalProof_, trustedHeadName, trustedHeadId,
                    binding, parentCheckpointByteBudget, childCheckpointByteBudget) ||
                !BudgetsUnchanged() || WaitForSingleObject(process_, 0) != WAIT_TIMEOUT || InterlockedCompareExchange(&view_->state, 8, 7) != 7) return false;
            ownedTerminalCommitted_ = std::move(committed); terminalVerified_ = true; return true;
        }
        catch (...) { return false; }
    }
private:
    Parent() = default;
    bool OriginalCaseMatches() const noexcept
    {
        return view_ && caseMapping_ && caseView_ && view_->requestSize == requestCaseId_.size() &&
            view_->requestHandle == static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(caseMapping_)) &&
            std::memcmp(caseView_, requestCaseId_.data(), requestCaseId_.size()) == 0;
    }
    bool BudgetsUnchanged() const noexcept
    {
        if (!view_ || view_->magic != kMagic || view_->version != kVersion || !ValidNonce(nonce_) ||
            std::memcmp(view_->nonce, nonce_.data(), nonce_.size()) != 0 || !OriginalCaseMatches() ||
            view_->requestKind != (ownedFile_ ? 1u : exactSource_ ? 2u : 0u) || view_->leafSize != ownedLeaf_.size() ||
            (view_->leafSize && std::memcmp(view_->leaf, ownedLeaf_.data(), ownedLeaf_.size() * sizeof(wchar_t)) != 0) ||
            view_->parentPid != GetCurrentProcessId() ||
            view_->parentCreated.dwLowDateTime != parentCreated_.dwLowDateTime ||
            view_->parentCreated.dwHighDateTime != parentCreated_.dwHighDateTime) return false;
        const auto expected = EncodeBudgets(budgets_);
        return std::equal(expected.begin(), expected.end(), view_->budgets);
    }
    bool DuplicateOffered(std::size_t index, HANDLE& out) noexcept
    {
        if (index >= 5 || !view_->checkpointHandles[index] || view_->checkpointHandles[index] > (std::numeric_limits<std::uintptr_t>::max)()) return false;
        HANDLE duplicate = nullptr;
        if (!DuplicateHandle(process_, reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(view_->checkpointHandles[index])),
                GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) return false;
        const bool directory = index < 2;
        if (!HandleIdentity(duplicate, view_->checkpointIds[index], directory)) { CloseHandle(duplicate); return false; }
        if (directory) { out = duplicate; return true; }
        // Reopen independently: DuplicateHandle shares the child's file offset.
        // Denying write/delete also rejects an already-open writer.
        HANDLE pinned = ReOpenFile(duplicate, GENERIC_READ, FILE_SHARE_READ, FILE_FLAG_OPEN_REPARSE_POINT);
        CloseHandle(duplicate);
        if (pinned == INVALID_HANDLE_VALUE) return false;
        if (!HandleIdentity(pinned, view_->checkpointIds[index], false)) { CloseHandle(pinned); return false; }
        out = pinned; return true;
    }
    HANDLE mapping_ = nullptr, caseMapping_ = nullptr, process_ = nullptr, root_ = nullptr, ownedProfile_ = nullptr, ownedSource_ = nullptr, ownedLedgerRoot_ = nullptr;
    HANDLE reportRoot_ = nullptr, reportFile_ = nullptr;
    Shared* view_ = nullptr;
    const char* caseView_ = nullptr;
    FILE_ID_INFO rootId_{}, ownedProfileId_{}, ownedSourceId_{}, ownedLedgerRootId_{};
    FILE_ID_INFO reportRootId_{}, reportFileId_{};
    DWORD childPid_ = 0, error_ = 0;
    FILETIME parentCreated_{};
    std::string caseId_, requestCaseId_, nonce_, preparedOwnedCase_, expectedExecutionId_, executionId_, preBytes_, flightBytes_, terminalBytes_;
    std::wstring ownedLeaf_;
    std::wstring reportLeaf_;
    std::string reportBytes_;
    std::uint64_t reportOfferHandle_ = 0, reportOfferSize_ = 0;
    // No caller-controlled boolean may certify a durable locator. A future
    // private ledger capability must establish this before the state-15 ACK.
    bool reportCreateAttempted_ = false, reportStored_ = false, reportLocatorVerified_ = false, reportAcknowledged_ = false;
    std::filesystem::path exactSourcePath_;
    bool ownedFile_ = false, exactSource_ = false, exactSourceVerified_ = false,
        ownedCasePrepared_ = false, ownedCaseVerified_ = false, ownedCaseResourceUnavailable_ = false;
    std::optional<v3_scheduler::OwnedTerminalCapacity> ownedTerminalCapacity_;
    std::uint64_t lastOwnedScanPages_ = 0, lastOwnedScanWork_ = 0;
    std::shared_ptr<const v3_scheduler::OwnedAssociationProof> ownedAssociationProof_;
    std::shared_ptr<const v3_scheduler::ExactAttemptProof> exactAttemptProof_;
    std::unique_ptr<v3_scheduler::CommittedExactAttempt> exactCommitted_;
    std::unique_ptr<v3_scheduler::CommittedOwnedAssociation> ownedCommitted_;
    std::shared_ptr<const v3_scheduler::OwnedTerminalProof> ownedTerminalProof_;
    std::unique_ptr<v3_scheduler::CommittedOwnedTerminal> ownedTerminalCommitted_;
    Budgets budgets_{};
    std::array<HANDLE, 5> pinned_{};
    std::array<HANDLE, 4> exactLedgerHandles_{};
    std::array<FILE_ID_INFO, 5> pinnedIds_{};
    bool attemptVerified_ = false, exactAttemptPrepared_ = false, terminalPrepared_ = false, terminalVerified_ = false;
};

struct ExactParentLedgerInspection final
{
    bool consistent = false, permitLedgerBound = false, resourceUnavailable = false;
    std::string_view reason;
};
class Child final
{
public:
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    ~Child() { for (auto handle : exactParentLedgerPins_) if (handle) CloseHandle(handle); for (auto handle : exactRoots_) if (handle) CloseHandle(handle); if (parent_) CloseHandle(parent_); if (pendingReportMapping_) CloseHandle(pendingReportMapping_); if (caseView_) UnmapViewOfFile(caseView_); if (caseMapping_) CloseHandle(caseMapping_); if (view_) UnmapViewOfFile(view_); if (mapping_) CloseHandle(mapping_); }
    const std::string& CaseId() const noexcept { return caseId_; }

    // Inherited handle must come from the child's command line. The caller
    // owns and retains isolation through all later work and process teardown.
    static std::unique_ptr<Child> Attach(HANDLE inherited, std::string_view nonce,
        const virtual_staging_fixture::ChildIsolation& isolation)
    {
        auto out = std::unique_ptr<Child>(new Child);
        if (!inherited || inherited == INVALID_HANDLE_VALUE || !ValidNonce(nonce) || !isolation.StillOwned()) return out;
        out->mapping_ = inherited;
        out->view_ = static_cast<Shared*>(MapViewOfFile(inherited, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
        if (!out->view_) return out;
        const auto requestKind = out->view_->requestKind, leafSize = out->view_->leafSize;
        if (out->view_->magic != kMagic || out->view_->version != kVersion ||
            InterlockedCompareExchange(&out->view_->state, 1, 1) != 1 ||
            std::memcmp(out->view_->nonce, nonce.data(), nonce.size()) != 0 || !out->view_->requestSize ||
            out->view_->requestSize > (std::numeric_limits<std::size_t>::max)() || !out->view_->requestHandle ||
            out->view_->requestHandle > (std::numeric_limits<std::uintptr_t>::max)() ||
            out->view_->requestHandle == static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(inherited)) ||
            requestKind > 2 || leafSize > 256 || (requestKind == 0 && leafSize) ||
            (requestKind == 1 && !leafSize) || (requestKind == 2 && leafSize) || !DecodeBudgets(out->view_->budgets, out->budgets_)) return out;
        try
        {
            const auto requestSize = out->view_->requestSize, requestHandle = out->view_->requestHandle;
            if (!requestSize || requestSize > (std::numeric_limits<std::size_t>::max)() ||
                requestSize > out->budgets_.caseByteBudget || !requestHandle ||
                requestHandle > (std::numeric_limits<std::uintptr_t>::max)() ||
                requestHandle == static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(inherited))) return out;
            const auto caseHandle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(requestHandle));
            if (caseHandle == INVALID_HANDLE_VALUE) return out;
            const auto caseView = static_cast<const char*>(MapViewOfFile(caseHandle, FILE_MAP_READ, 0, 0,
                static_cast<std::size_t>(requestSize)));
            if (!caseView) return out;
            out->caseMapping_ = caseHandle; out->caseView_ = caseView;
            if (!SetHandleInformation(out->caseMapping_, HANDLE_FLAG_INHERIT, 0) ||
                out->view_->requestSize != requestSize || out->view_->requestHandle != requestHandle) return out;
            out->caseId_.assign(out->caseView_, static_cast<std::size_t>(requestSize));
            out->requestCaseId_ = out->caseId_; out->nonce_.assign(nonce);
            out->ownedFile_ = requestKind == 1;
            out->exactSource_ = requestKind == 2;
            if (out->exactSource_)
            {
                if (!out->view_->parentPid) return out;
                out->parentPid_ = out->view_->parentPid;
                out->parentCreated_ = out->view_->parentCreated;
                out->parent_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE,
                    FALSE, out->parentPid_);
                if (!out->parent_ || WaitForSingleObject(out->parent_, 0) != WAIT_TIMEOUT) return out;
                FILETIME created{}, exited{}, kernel{}, user{};
                if (!GetProcessTimes(out->parent_, &created, &exited, &kernel, &user) ||
                    created.dwLowDateTime != out->parentCreated_.dwLowDateTime ||
                    created.dwHighDateTime != out->parentCreated_.dwHighDateTime) return out;
            }
            if (out->ownedFile_) out->ownedLeaf_.assign(out->view_->leaf, leafSize);
            if (out->view_->requestKind != requestKind || out->view_->leafSize != leafSize ||
                (leafSize && std::memcmp(out->view_->leaf, out->ownedLeaf_.data(), leafSize * sizeof(wchar_t)) != 0)) return out;
            const auto replay = v3_scheduler::ReplayV4StructuredCaseId(out->caseId_, out->budgets_.coordinates,
                out->budgets_.axes, out->budgets_.tokenBudget, out->budgets_.caseByteBudget);
            out->valid_ = replay.status == v3_scheduler::V4StructuredCaseStatus::unexecuted && replay.caseId == out->caseId_ &&
                (out->ownedFile_ ? (replay.graphMode == "ole" && replay.oleGraph.has_value()) :
                    ((replay.graphMode == "ole" && replay.oleGraph) || (replay.graphMode == "native" && replay.nativeGraph)));
            out->nativeGraph_ = out->valid_ && replay.graphMode == "native" && replay.nativeGraph.has_value();
            if (out->exactSource_)
                out->valid_ = out->valid_ && replay.family && !replay.family->b && replay.family->a.kind == V4SourceKind::file_only;
            if (out->ownedFile_)
                out->valid_ = out->valid_ && replay.family && !replay.family->b && replay.family->a.kind == V4SourceKind::file_only &&
                    replay.family->a.fileCount.kind == V3CountKind::exact && replay.family->a.fileCount.decimal == "1" &&
                    out->ownedLeaf_.find(L'\0') == std::wstring::npos;
            out->valid_ = out->valid_ && out->FrameMatches();
        }
        catch (...) { out->valid_ = false; }
        return out;
    }
    bool Valid() const noexcept { return valid_; }
    bool IsOwnedFileRequest() const noexcept { return valid_ && ownedFile_; }
    bool IsNativeGraphRequest() const noexcept { return valid_ && nativeGraph_ && FrameMatches(); }
    bool IsExactSourceRequest() const noexcept { return valid_ && exactSource_; }
    std::wstring_view OwnedLeaf() const noexcept { return ownedLeaf_; }
    const Budgets& WorkBudgets() const noexcept { return budgets_; }
    bool PublishRoot(const virtual_staging_fixture::ChildIsolation& isolation) noexcept
    {
        if (!FrameMatches() || InterlockedCompareExchange(&view_->state, 1, 1) != 1) return false;
        const auto handle = isolation.RootHandle(); const auto id = isolation.RootIdentity();
        if (!handle || !id) return false;
        view_->childRootHandle = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle));
        view_->childRootId = *id; view_->childPid = GetCurrentProcessId();
        return InterlockedCompareExchange(&view_->state, 3, 1) == 1;
    }
    bool RootAcknowledged() const noexcept
    { return FrameMatches() && InterlockedCompareExchange(&view_->state, 4, 4) == 4; }
    // Same frame/case/nonce comparison only; no configuration, allocation,
    // sufficient evidence bound or execution authority is implied.
    bool DiagnosticBudgetHandshakeObserved() const noexcept
    { return RootAcknowledged() && budgets_.diagnostics.declared; }
    // Only the same acknowledged child frame can request this one-time
    // external recorder allocation. The other recorder/evidence budgets and
    // all execution capabilities remain independently unresolved.
    ChildDiagnosticConfiguration ConfigureExternalDiagnostics(const virtual_staging_fixture::ChildIsolation& isolation) noexcept
    {
        if (diagnosticConfiguration_)
        {
            auto prior = *diagnosticConfiguration_;
            if (!DiagnosticBudgetHandshakeObserved() || !view_ || view_->childPid != GetCurrentProcessId() || !isolation.StillOwned())
                prior.frameBoundAfter = false;
            return prior;
        }
        ChildDiagnosticConfiguration out;
        if (!DiagnosticBudgetHandshakeObserved() || view_->childPid != GetCurrentProcessId() || !isolation.StillOwned())
        { out.reason = "v5_diagnostic_frame_or_child_unavailable"; diagnosticConfiguration_ = out; return out; }
        out.frameBoundBefore = true;
        const auto& b = budgets_.diagnostics;
        out.sharedVmBudget = b.sharedExternalBackgroundVmBytes;
        const auto externalCeiling = b.sharedExternalBackgroundVmBytes
            ? (std::min)(b.totalBytes, b.sharedExternalBackgroundVmBytes) : b.totalBytes;
        const auto configured = snowdesktop::external_transfer_observation::ConfigureBudgetOnce(
            {b.eventSlots, b.blobBytes, b.stagingHandles, externalCeiling});
        out.configured = configured.configured; out.resourceUnavailable = configured.resourceUnavailable;
        out.committedBytes = configured.committedBytes;
        if (configured.configured)
        {
            const auto actual = snowdesktop::external_transfer_observation::ObserveBudget();
            out.eventCapacity = actual.eventCapacity; out.blobCapacity = actual.blobCapacity;
            out.stagingHandleCapacity = actual.stagingHandleCapacity;
            out.frameBoundAfter = DiagnosticBudgetHandshakeObserved() && view_->childPid == GetCurrentProcessId() &&
                isolation.StillOwned() && actual.eventCapacity == b.eventSlots && actual.blobCapacity == b.blobBytes &&
                actual.stagingHandleCapacity == b.stagingHandles && actual.committedBytes == configured.committedBytes &&
                configured.committedBytes <= b.totalBytes;
        }
        out.reason = !configured.configured ? "v5_diagnostic_configuration_unavailable" :
            out.frameBoundAfter ? "v5_external_diagnostic_configured_other_obligations_pending" : "v5_diagnostic_frame_changed_after_configuration";
        out.backgroundReason = "v6_background_shared_budget_not_declared";
        if (b.sharedExternalBackgroundVmBytes)
        {
            out.backgroundReason = "v6_background_external_or_frame_unavailable";
            if (configured.configured && out.frameBoundAfter && configured.committedBytes <= b.sharedExternalBackgroundVmBytes)
            {
                out.backgroundAttempted = true;
                const auto remaining = b.sharedExternalBackgroundVmBytes - configured.committedBytes;
                const auto bg = snowdesktop::background_work_observation::ConfigureBudgetOnce(b.backgroundSlots, remaining);
                out.backgroundConfigured = bg.configured; out.backgroundResourceUnavailable = bg.resourceUnavailable;
                out.backgroundCommittedBytes = bg.committedBytes;
                const auto actual = snowdesktop::background_work_observation::ObserveBudget();
                const auto external = snowdesktop::external_transfer_observation::ObserveBudget();
                if (bg.configured) out.backgroundCapacity = actual.eventCapacity;
                out.sharedBoundAfter = bg.configured && DiagnosticBudgetHandshakeObserved() &&
                    view_->childPid == GetCurrentProcessId() && isolation.StillOwned() &&
                    external.eventCapacity == b.eventSlots && external.blobCapacity == b.blobBytes &&
                    external.stagingHandleCapacity == b.stagingHandles && external.committedBytes == configured.committedBytes &&
                    actual.eventCapacity == b.backgroundSlots && actual.committedBytes == bg.committedBytes &&
                    bg.committedBytes <= remaining;
                out.backgroundReason = !bg.configured ? "v6_background_configuration_unavailable" :
                    out.sharedBoundAfter ? "v6_shared_vm_configured_other_obligations_pending" : "v6_shared_vm_frame_changed_after_configuration";
            }
        }
        diagnosticConfiguration_ = out;
        return out;
    }
    const std::optional<ChildDiagnosticConfiguration>& DiagnosticConfiguration() const noexcept { return diagnosticConfiguration_; }

    bool OfferExactSource(const virtual_staging_fixture::ChildIsolation& isolation, HANDLE profile,
        const FILE_ID_INFO& profileId, HANDLE source, const FILE_ID_INFO& sourceId,
        const std::filesystem::path& sourcePath) noexcept
    {
        try
        {
            if (!valid_ || !exactSource_ || exactSourceAccepted_ || exactRoots_[0] || !view_ || !parent_ ||
                WaitForSingleObject(parent_, 0) != WAIT_TIMEOUT || !isolation.StillOwned() ||
                InterlockedCompareExchange(&view_->state, 4, 4) != 4) return false;
            const auto rootId = isolation.RootIdentity();
            const HANDLE root = isolation.RootHandle();
            const auto& path = sourcePath.native();
            if (!rootId || path.empty() || path.size() > 32768 || path.find(L'\0') != std::wstring::npos ||
                !HandleIdentity(root, *rootId, true) || !HandleIdentity(profile, profileId, true) ||
                !HandleIdentity(source, sourceId, true) || !DirectChild(HandlePath(root), HandlePath(profile)) ||
                !DirectChild(HandlePath(profile), HandlePath(source))) return false;
            const auto plan = v3_scheduler::PrepareV4ExactOwnedFiles(caseId_, budgets_.coordinates, budgets_.axes,
                budgets_.tokenBudget, budgets_.caseByteBudget, sourcePath, source, sourceId,
                budgets_.axes.fileMembers, budgets_.axes.payloadBytes);
            if (plan.status != v3_scheduler::V4ExactOwnedFilesStatus::planned) return false;
            std::array<HANDLE, 3> owned{};
            struct Guard { std::array<HANDLE, 3>& h; ~Guard() { for (auto x : h) if (x) CloseHandle(x); } } guard{owned};
            const std::array<HANDLE, 3> input{root, profile, source};
            const std::array<FILE_ID_INFO, 3> ids{*rootId, profileId, sourceId};
            for (std::size_t i = 0; i < input.size(); ++i)
                if (!DuplicateHandle(GetCurrentProcess(), input[i], GetCurrentProcess(), &owned[i], 0, FALSE, DUPLICATE_SAME_ACCESS) ||
                    !HandleIdentity(owned[i], ids[i], true)) return false;
            exactSourcePath_ = sourcePath;
            exactRoots_ = owned; exactRootIds_ = ids; owned = {};
            view_->ownedProfileHandle = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(exactRoots_[1]));
            view_->ownedSourceHandle = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(exactRoots_[2]));
            view_->ownedProfileId = profileId; view_->ownedSourceId = sourceId;
            view_->responseSize = static_cast<DWORD>(path.size() * sizeof(wchar_t));
            std::memcpy(view_->response, path.data(), path.size() * sizeof(wchar_t));
            if (WaitForSingleObject(parent_, 0) != WAIT_TIMEOUT || InterlockedCompareExchange(&view_->state, 12, 4) != 4)
            { for (auto& handle : exactRoots_) { if (handle) CloseHandle(handle); handle = nullptr; } return false; }
            return true;
        }
        catch (...) { return false; }
    }
    bool AcceptExactSource() noexcept
    {
        if (!exactSource_ || exactSourceAccepted_ || !view_ || !parent_ ||
            WaitForSingleObject(parent_, 0) != WAIT_TIMEOUT || InterlockedCompareExchange(&view_->state, 13, 13) != 13 ||
            !HandleIdentity(exactRoots_[0], exactRootIds_[0], true) || !HandleIdentity(exactRoots_[1], exactRootIds_[1], true) ||
            !HandleIdentity(exactRoots_[2], exactRootIds_[2], true) || !SameId(view_->childRootId, exactRootIds_[0]) ||
            !SameId(view_->ownedProfileId, exactRootIds_[1]) || !SameId(view_->ownedSourceId, exactRootIds_[2]) ||
            view_->responseSize != exactSourcePath_.native().size() * sizeof(wchar_t) ||
            std::memcmp(view_->response, exactSourcePath_.native().data(), exactSourcePath_.native().size() * sizeof(wchar_t)) != 0) return false;
        exactSourceAccepted_ = true; return true;
    }

    bool ReportOwnedScanProgress(const V4OwnedFileFamilySeed& seed, bool cursorAdvanced, std::size_t newlyCompletedWork) noexcept
    {
        if (!ownedFile_ || !valid_ || !view_ || !cursorAdvanced || !seed.StillOwned() ||
            InterlockedCompareExchange(&view_->state, 4, 4) != 4 ||
            ownedScanPages_ >= static_cast<std::uint64_t>((std::numeric_limits<LONG64>::max)()) / 2 ||
            newlyCompletedWork > static_cast<std::uint64_t>((std::numeric_limits<LONG64>::max)()) - ownedScanWork_) return false;
        InterlockedIncrement64(&view_->ownedScanGeneration); // odd: record being written
        ownedScanWork_ += newlyCompletedWork;
        ++ownedScanPages_;
        InterlockedExchange64(&view_->ownedScanWork, static_cast<LONG64>(ownedScanWork_));
        InterlockedExchange64(&view_->ownedScanLastWork, static_cast<LONG64>(newlyCompletedWork));
        InterlockedIncrement64(&view_->ownedScanGeneration); // even: complete page
        return true;
    }

    bool OfferOwnedCase(const virtual_staging_fixture::ChildIsolation& isolation, HANDLE profileRoot,
        const FILE_ID_INFO& profileRootId, const V4OwnedFileFamilySeed& sourceSeed, std::string_view ownedCase) noexcept
    {
        try
        {
            if (!ownedFile_ || !valid_ || !view_ || !isolation.StillOwned() || !sourceSeed.StillOwned() ||
                InterlockedCompareExchange(&view_->state, 4, 4) != 4 || ownedCase.empty() || ownedCase.size() > kLimit ||
                !HandleIdentity(profileRoot, profileRootId, true) || !HandleIdentity(sourceSeed.Root(), sourceSeed.RootId(), true)) return false;
            const auto audit = VerifyOwnedFileCase(caseId_, ownedCase, ownedLeaf_, sourceSeed.Root(), sourceSeed.RootId(), budgets_);
            if (!audit.verified) return false;
            offeredCase_.assign(ownedCase);
            view_->ownedProfileHandle = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(profileRoot));
            view_->ownedSourceHandle = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(sourceSeed.Root()));
            view_->ownedProfileId = profileRootId; view_->ownedSourceId = sourceSeed.RootId();
            std::memcpy(view_->response, offeredCase_.data(), offeredCase_.size());
            view_->responseSize = static_cast<DWORD>(offeredCase_.size());
            return InterlockedCompareExchange(&view_->state, 9, 4) == 4;
        }
        catch (...) { return false; }
    }
    bool AcceptOwnedCase() noexcept
    {
        if (!ownedFile_ || ownedAccepted_ || offeredCase_.empty() || !view_ || InterlockedCompareExchange(&view_->state, 10, 10) != 10) return false;
        caseId_.swap(offeredCase_); ownedAccepted_ = true; return true;
    }
    bool OwnedCaseBudgetRejected() const noexcept
    { return ownedFile_ && view_ && InterlockedCompareExchange(&view_->state, 11, 11) == 11; }

    bool OfferAttempt(const v3_scheduler::InFlightReceipt& receipt) noexcept
    {
        if (ownedFile_ && !ownedAccepted_) return false;
        try
        {
            if (exactSource_)
            {
                if (!exactSourceAccepted_ || exactPermit_ || exactPermitIssued_ || !ExactSourceIntact() ||
                    !SameId(receipt.RootIdentity(), exactRootIds_[0]) || receipt.ExecutionId().empty() ||
                    receipt.ExecutionId() != std::string_view(view_->nonce, sizeof(view_->nonce))) return false;
                auto permit = std::unique_ptr<v3_scheduler::V4ExactOwnedFilesPermit>(new v3_scheduler::V4ExactOwnedFilesPermit(
                    receipt, caseId_, exactRoots_, exactRootIds_, exactSourcePath_, parent_));
                if (!permit->Ready()) return false;
                offeredExecution_ = receipt.ExecutionId();
                if (!Offer(receipt.RootHandle(), receipt.DirectoryHandle(), receipt.PreHandle(), receipt.InFlightHandle(), nullptr,
                    receipt.RootIdentity(), receipt.DirectoryIdentity(), receipt.PreIdentity(), receipt.InFlightIdentity(), {},
                    receipt.ExecutionId(), 13, 5)) return false;
                exactPermit_ = std::move(permit);
                return true;
            }
            return Offer(receipt.RootHandle(), receipt.DirectoryHandle(), receipt.PreHandle(), receipt.InFlightHandle(), nullptr,
                receipt.RootIdentity(), receipt.DirectoryIdentity(), receipt.PreIdentity(), receipt.InFlightIdentity(), {},
                receipt.ExecutionId(), ownedFile_ ? 10 : 4, 5);
        }
        catch (...) { return false; }
    }
    bool AttemptAcknowledged() const noexcept
    { return view_ && InterlockedCompareExchange(&view_->state, 6, 6) == 6; }
    // Independently copy the live parent's actual ledger handles, compare
    // them with the caller's already trusted output-root handle, and replay
    // the exact parent and child checkpoint transitions. This inspection is
    // not a latest-head, crash-durability or production-execution proof.
    ExactParentLedgerInspection InspectExactParentLedger(v3_scheduler::InFlightReceipt& receipt,
        HANDLE trustedOutputRoot, const FILE_ID_INFO& trustedOutputRootId, std::string_view trustedBinding,
        std::size_t replayWorkBudget, std::size_t maximumParentCheckpointBytes) noexcept
    {
        ExactParentLedgerInspection out;
        try
        {
            if (exactParentLedgerObserved_ && exactPermitLedgerBound_)
            {
                // A successful binding is already sealed to its receipt. Do
                // not attribute that result to another caller-supplied receipt.
                out.reason = "v4_exact_parent_binding_already_observed";
                return out;
            }
            if (exactParentLedgerObserved_)
            {
                // A failed binding has no filesystem side effects. Release
                // only this attempt's read pins and repeat the full audit.
                for (auto& h : exactParentLedgerPins_) { if (h) CloseHandle(h); h = nullptr; }
                exactParentPreBytes_.clear(); exactParentHeadBytes_.clear(); exactChildPreBytes_.clear();
                exactChildFlightBytes_.clear(); exactParentBinding_.clear(); exactParentPreName_.clear(); exactParentHeadName_.clear();
                exactParentLedgerObserved_ = false;
            }
            if (!view_) { out.reason = "v4_exact_parent_inspection_context_unavailable"; return out; }
            const DWORD preNameSize = view_->parentPreNameSize, headNameSize = view_->parentHeadNameSize,
                bindingSize = view_->parentBindingSize;
            const auto parentBudget = view_->parentCheckpointBudget;
            if (!exactSource_ || !exactPermit_ || exactPermitIssued_ || !AttemptAcknowledged() ||
                !ExactSourceIntact() || !HandleIdentity(trustedOutputRoot, trustedOutputRootId, true) ||
                !SameId(view_->parentLedgerIds[0], trustedOutputRootId) || receipt.ExecutionId() != offeredExecution_ ||
                !SameId(receipt.RootIdentity(), exactRootIds_[0]) || !bindingSize ||
                bindingSize > kLimit || trustedBinding.size() != bindingSize ||
                std::memcmp(view_->parentBinding, trustedBinding.data(), trustedBinding.size()) != 0 ||
                !preNameSize || preNameSize > 256 || !headNameSize || headNameSize > 256 || !parentBudget)
            { out.reason = "v4_exact_parent_inspection_context_unavailable"; return out; }
            if (parentBudget > maximumParentCheckpointBytes || parentBudget > (std::numeric_limits<std::size_t>::max)())
            { out.resourceUnavailable = true; out.reason = "v4_exact_parent_inspection_budget_unavailable"; return out; }
            const auto budget = static_cast<std::size_t>(parentBudget);
            std::wstring preName(view_->parentPreName, preNameSize);
            std::wstring headName(view_->parentHeadName, headNameSize);
            if (view_->parentPreNameSize != preNameSize || view_->parentHeadNameSize != headNameSize)
            { out.reason = "v4_exact_parent_inspection_frame_changed"; return out; }
            std::string bindingCopy(trustedBinding);
            if (preName == headName || preName.find_first_of(L"\\/:") != std::wstring::npos ||
                headName.find_first_of(L"\\/:") != std::wstring::npos ||
                preName.find(L'\0') != std::wstring::npos || headName.find(L'\0') != std::wstring::npos)
            { out.reason = "v4_exact_parent_inspection_name_unavailable"; return out; }
            std::array<HANDLE, 4> pins{};
            struct Guard { std::array<HANDLE, 4>& handles; ~Guard() { for (auto h : handles) if (h) CloseHandle(h); } } guard{pins};
            std::array<FILE_ID_INFO, 4> ids{};
            std::array<std::uint64_t, 4> offeredHandles{};
            for (std::size_t i = 0; i < pins.size(); ++i)
            {
                ids[i] = view_->parentLedgerIds[i];
                offeredHandles[i] = view_->parentLedgerHandles[i];
                if (!view_->parentLedgerHandles[i] || view_->parentLedgerHandles[i] > (std::numeric_limits<std::uintptr_t>::max)())
                { out.reason = "v4_exact_parent_inspection_handle_unavailable"; return out; }
                HANDLE duplicate = nullptr;
                if (!DuplicateHandle(parent_, reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(view_->parentLedgerHandles[i])),
                    GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
                { out.reason = "v4_exact_parent_inspection_duplicate_unavailable"; return out; }
                const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT | (i < 2 ? FILE_FLAG_BACKUP_SEMANTICS : 0);
                pins[i] = ReOpenFile(duplicate, i < 2 ? FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY : GENERIC_READ,
                    i < 2 ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ, flags);
                CloseHandle(duplicate);
                if (pins[i] == INVALID_HANDLE_VALUE) { pins[i] = nullptr; out.reason = "v4_exact_parent_inspection_pin_unavailable"; return out; }
                if (!HandleIdentity(pins[i], ids[i], i < 2))
                { out.reason = "v4_exact_parent_inspection_identity_unavailable"; return out; }
            }
            const auto rootPath = HandlePath(pins[0]), directoryPath = HandlePath(pins[1]);
            const auto prePath = HandlePath(pins[2]), headPath = HandlePath(pins[3]);
            const auto childRootPath = HandlePath(exactRoots_[0]);
            if (!SameId(ids[0], trustedOutputRootId) || HandlePath(trustedOutputRoot) != rootPath ||
                !DirectChild(rootPath, directoryPath) || !DirectChild(directoryPath, prePath) || !DirectChild(directoryPath, headPath) ||
                std::filesystem::path(prePath).filename().native() != preName ||
                std::filesystem::path(headPath).filename().native() != headName || SameId(ids[2], ids[3]) ||
                rootPath == childRootPath || (rootPath.size() > childRootPath.size() &&
                    rootPath.substr(0, childRootPath.size()) == childRootPath && rootPath[childRootPath.size()] == L'\\'))
            { out.reason = "v4_exact_parent_inspection_locator_unavailable"; return out; }
            std::array<FILE_ID_INFO, 6> childIds{exactRootIds_[0], exactRootIds_[1], exactRootIds_[2],
                receipt.DirectoryIdentity(), receipt.PreIdentity(), receipt.InFlightIdentity()};
            for (std::size_t i = 0; i < 4; ++i)
                if (!SameId(view_->checkpointIds[i], i == 0 ? receipt.RootIdentity() :
                    i == 1 ? receipt.DirectoryIdentity() : i == 2 ? receipt.PreIdentity() : receipt.InFlightIdentity()))
                { out.reason = "v4_exact_parent_inspection_child_checkpoint_unavailable"; return out; }
            HANDLE childPre = ReOpenFile(receipt.PreHandle(), GENERIC_READ, FILE_SHARE_READ, FILE_FLAG_OPEN_REPARSE_POINT);
            HANDLE childFlight = ReOpenFile(receipt.InFlightHandle(), GENERIC_READ, FILE_SHARE_READ, FILE_FLAG_OPEN_REPARSE_POINT);
            struct ChildGuard { HANDLE a, b; ~ChildGuard() { if (a != INVALID_HANDLE_VALUE) CloseHandle(a); if (b != INVALID_HANDLE_VALUE) CloseHandle(b); } } childGuard{childPre, childFlight};
            if (childPre == INVALID_HANDLE_VALUE || childFlight == INVALID_HANDLE_VALUE ||
                !HandleIdentity(receipt.RootHandle(), receipt.RootIdentity(), true) ||
                !HandleIdentity(receipt.DirectoryHandle(), childIds[3], true) ||
                !HandleIdentity(childPre, childIds[4], false) || !HandleIdentity(childFlight, childIds[5], false) ||
                !DirectChild(childRootPath, HandlePath(receipt.DirectoryHandle())) ||
                !DirectChild(HandlePath(receipt.DirectoryHandle()), HandlePath(childPre)) ||
                !DirectChild(HandlePath(receipt.DirectoryHandle()), HandlePath(childFlight)))
            { out.reason = "v4_exact_parent_inspection_child_pin_unavailable"; return out; }
            std::string pre, head, childPreBytes, childFlightBytes;
            if (!ReadPinned(pins[2], pre, budget) || !ReadPinned(pins[3], head, budget) ||
                !ReadPinned(childPre, childPreBytes, kLimit) || !ReadPinned(childFlight, childFlightBytes, kLimit))
            { out.reason = "v4_exact_parent_inspection_read_unavailable"; return out; }
            const auto audit = v3_scheduler::VerifyV4ExactParentAttempt(pre, head, trustedBinding, caseId_, offeredExecution_,
                exactSourcePath_, exactRoots_[2], childIds, childPreBytes, childFlightBytes, preName, ids[2],
                budgets_.coordinates, budgets_.axes, budgets_.tokenBudget, budgets_.caseByteBudget, replayWorkBudget, kLimit, budget);
            if (audit.status != v3_scheduler::V4AttemptTransitionStatus::consistent_unverified)
            { out.resourceUnavailable = audit.status == v3_scheduler::V4AttemptTransitionStatus::resource_unavailable;
              out.reason = "v4_exact_parent_inspection_replay_unavailable"; return out; }
            std::string preAgain, headAgain, childPreAgain, childFlightAgain;
            if (!ReadPinned(pins[2], preAgain, budget) || preAgain != pre || !ReadPinned(pins[3], headAgain, budget) || headAgain != head ||
                !ReadPinned(childPre, childPreAgain, kLimit) || childPreAgain != childPreBytes ||
                !ReadPinned(childFlight, childFlightAgain, kLimit) || childFlightAgain != childFlightBytes ||
                !ExactSourceIntact() || !AttemptAcknowledged() || !HandleIdentity(trustedOutputRoot, trustedOutputRootId, true) ||
                HandlePath(trustedOutputRoot) != rootPath || view_->parentCheckpointBudget != budget ||
                view_->parentPreNameSize != preName.size() || view_->parentHeadNameSize != headName.size() ||
                std::memcmp(view_->parentPreName, preName.data(), preName.size() * sizeof(wchar_t)) != 0 ||
                std::memcmp(view_->parentHeadName, headName.data(), headName.size() * sizeof(wchar_t)) != 0 ||
                view_->parentBindingSize != bindingCopy.size() ||
                std::memcmp(view_->parentBinding, bindingCopy.data(), bindingCopy.size()) != 0 ||
                !HandleIdentity(receipt.RootHandle(), receipt.RootIdentity(), true) ||
                !HandleIdentity(receipt.DirectoryHandle(), childIds[3], true) ||
                !HandleIdentity(childPre, childIds[4], false) || !HandleIdentity(childFlight, childIds[5], false))
            { out.reason = "v4_exact_parent_inspection_changed"; return out; }
            for (std::size_t i = 0; i < pins.size(); ++i)
                if (!HandleIdentity(pins[i], ids[i], i < 2) || !SameId(view_->parentLedgerIds[i], ids[i]) ||
                    view_->parentLedgerHandles[i] != offeredHandles[i])
                { out.reason = "v4_exact_parent_inspection_changed"; return out; }
            exactParentLedgerPins_ = pins; pins.fill(nullptr);
            exactParentLedgerIds_ = ids; exactParentPreBytes_ = std::move(pre); exactParentHeadBytes_ = std::move(head);
            exactChildPreBytes_ = std::move(childPreBytes); exactChildFlightBytes_ = std::move(childFlightBytes);
            exactParentPreName_ = std::move(preName); exactParentHeadName_ = std::move(headName); exactParentBinding_ = std::move(bindingCopy);
            exactParentLedgerObserved_ = true;
            out.consistent = true;
            out.permitLedgerBound = exactPermit_->BindVerifiedParentLedger(receipt, exactParentLedgerPins_, exactParentLedgerIds_,
                exactParentPreBytes_, exactParentHeadBytes_, exactParentPreName_, exactChildPreBytes_, exactChildFlightBytes_,
                exactParentBinding_, budgets_.coordinates, budgets_.axes, budgets_.tokenBudget, budgets_.caseByteBudget,
                replayWorkBudget, kLimit, budget);
            exactPermitLedgerBound_ = out.permitLedgerBound;
            out.reason = out.permitLedgerBound ? "v4_exact_parent_bound_not_latest_or_execution_proof" :
                "v4_exact_parent_consistent_permit_binding_unavailable";
            return out;
        }
        catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_exact_parent_inspection_resource_unavailable"; return out; }
        catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_exact_parent_inspection_resource_unavailable"; return out; }
        catch (...) { out.reason = "v4_exact_parent_inspection_unavailable"; return out; }
    }
    std::unique_ptr<v3_scheduler::V4ExactOwnedFilesPermit> TakeExactFilesPermit() noexcept
    {
        if (!exactSource_ || !exactParentLedgerVerified_ || !exactPermit_ || exactPermitIssued_ || !AttemptAcknowledged() || !ExactSourceIntact() ||
            view_->executionSize != offeredExecution_.size() ||
            std::memcmp(view_->executionId, offeredExecution_.data(), offeredExecution_.size()) != 0) return nullptr;
        exactPermitIssued_ = true;
        return std::move(exactPermit_);
    }
    bool OfferTerminal(const v3_scheduler::CommittedV4Receipt& receipt) noexcept
    {
        return Offer(receipt.RootHandle(), receipt.DirectoryHandle(), receipt.PreHandle(), receipt.InFlightHandle(), receipt.TerminalHandle(),
            receipt.RootIdentity(), receipt.DirectoryIdentity(), receipt.PreIdentity(), receipt.InFlightIdentity(), receipt.TerminalIdentity(),
            receipt.ExecutionId(), 6, 7);
    }
    bool TerminalAcknowledged() const noexcept
    { return view_ && InterlockedCompareExchange(&view_->state, 8, 8) == 8; }
    bool PublishCheckpointReport(const virtual_staging_fixture::ChildIsolation& isolation) noexcept
    {
        if (!isolation.StillOwned() || !TerminalAcknowledged()) return false;
        try
        {
            std::string bytes;
            if (!Append(bytes, "v4_child_report_v1") || !Append(bytes, caseId_) ||
                !Append(bytes, "checkpoint_consistent_unverified")) return false;
            return Publish(bytes, 8);
        }
        catch (...) { return false; }
    }

    // Pending is the only report that can be emitted without a sealed actual
    // executor outcome. It makes no claim about dispatch or product success.
    bool PublishPending(const virtual_staging_fixture::ChildIsolation& isolation, std::string_view reason) noexcept
    {
        if (!isolation.StillOwned()) return false;
        try { std::string bytes; return BuildPendingReport(bytes, kLimit, reason) &&
            Publish(bytes, exactSourceAccepted_ ? 13 : ownedAccepted_ ? 10 : OwnedCaseBudgetRejected() ? 11 : 4); }
        catch (...) { return false; }
    }
    // Separate report offer remains alive until a parent ACK or child exit.
    // Only pending/unexecuted is supported; no caller-supplied outcome can be
    // promoted into a product verdict by this transport.
    bool OfferPendingReport(const virtual_staging_fixture::ChildIsolation& isolation, std::string_view reason) noexcept
    {
        if (!FrameMatches() || view_->childPid != GetCurrentProcessId() || !isolation.StillOwned() || pendingReportMapping_ ||
            !budgets_.diagnostics.declared || !budgets_.diagnostics.evidenceBytes) return false;
        try
        {
            const auto budget = budgets_.diagnostics.evidenceBytes;
            std::string bytes;
            if (!BuildPendingReport(bytes, budget, reason) || bytes.empty()) return false;
            const auto size = static_cast<std::uint64_t>(bytes.size());
            HANDLE writable = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                static_cast<DWORD>(size >> 32), static_cast<DWORD>(size), nullptr);
            if (!writable) return false;
            struct HandleGuard { HANDLE h; ~HandleGuard() { if (h) CloseHandle(h); } } source{writable};
            void* writableView = MapViewOfFile(writable, FILE_MAP_WRITE, 0, 0, bytes.size());
            if (!writableView) return false;
            struct ViewGuard { void* p; ~ViewGuard() { if (p) UnmapViewOfFile(p); } } view{writableView};
            std::memcpy(writableView, bytes.data(), bytes.size());
            if (!UnmapViewOfFile(writableView)) return false;
            view.p = nullptr;
            HANDLE readOnly = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), writable, GetCurrentProcess(), &readOnly, FILE_MAP_READ, FALSE, 0)) return false;
            HandleGuard readonly{readOnly};
            if (!CloseHandle(source.h)) return false;
            source.h = nullptr;
            const auto* check = static_cast<const char*>(MapViewOfFile(readOnly, FILE_MAP_READ, 0, 0, bytes.size()));
            if (!check) return false;
            struct ConstViewGuard { const void* p; ~ConstViewGuard() { if (p) UnmapViewOfFile(p); } } checkGuard{check};
            if (std::memcmp(check, bytes.data(), bytes.size()) != 0 || !FrameMatches() || view_->childPid != GetCurrentProcessId()) return false;
            const LONG expected = exactSourceAccepted_ ? 13 : ownedAccepted_ ? 10 : OwnedCaseBudgetRejected() ? 11 : 4;
            if (InterlockedCompareExchange(&view_->state, expected, expected) != expected) return false;
            pendingReportMapping_ = readOnly; readonly.h = nullptr;
            view_->pendingReportHandle = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pendingReportMapping_));
            view_->pendingReportSize = size;
            return InterlockedCompareExchange(&view_->state, 14, expected) == expected;
        }
        catch (...) { return false; }
    }
    // Caller must keep this Child and its isolation alive after an offer while
    // the parent attempts publication. Absence of an ACK remains unknown;
    // returning an offer does not assert that a durable locator exists.
    PendingReportDisposition PublishPendingOrOffer(const virtual_staging_fixture::ChildIsolation& isolation,
        std::string_view reason) noexcept
    {
        if (PublishPending(isolation, reason)) return PendingReportDisposition::inline_published;
        return OfferPendingReport(isolation, reason) ? PendingReportDisposition::offered : PendingReportDisposition::unavailable;
    }
    bool PendingReportAcknowledged() const noexcept
    { return pendingReportMapping_ && FrameMatches() && InterlockedCompareExchange(&view_->state, 15, 15) == 15; }
    // No outcome publishing API exists yet: ScheduledV4Outcome is sealed to
    // an in-process receipt, but cannot currently prove that receipt belongs
    // to this transport's case. Such a binding must be added before reporting
    // an actual execution across processes.
private:
    bool BuildPendingReport(std::string& out, std::size_t budget, std::string_view reason) const
    {
        const auto add = [&](std::string_view value) { return AppendBounded(out, value, budget); };
        const auto number = [&](std::size_t value) { return add(std::to_string(value)); };
        const auto flag = [&](bool value) { return number(value ? 1 : 0); };
        if (!add("v4_child_report_v3") || !add(caseId_) || !add("pending") || !add("unexecuted") || !add(reason) ||
            !flag(diagnosticConfiguration_.has_value())) return false;
        if (diagnosticConfiguration_)
        {
            const auto& value = *diagnosticConfiguration_;
            if (!flag(value.frameBoundBefore) || !flag(value.frameBoundAfter) || !flag(value.configured) ||
                !flag(value.resourceUnavailable) || !flag(value.otherRecorderBudgetsUnresolved) ||
                !number(value.eventCapacity) || !number(value.blobCapacity) || !number(value.stagingHandleCapacity) ||
                !number(value.committedBytes) || !add(value.reason)) return false;
        }
        const auto actual = snowdesktop::external_transfer_observation::ObserveBudget();
        if (!(number(actual.eventCapacity) && number(actual.blobCapacity) && number(actual.stagingHandleCapacity) &&
            number(actual.committedBytes) && number(actual.recordedEvents) && number(actual.recordedBlobBytes) &&
            number(actual.recordedStagingHandles) && flag(actual.captureClaimed) && flag(actual.writersDrained) &&
            flag(actual.recordsLost) && flag(actual.capacityExceeded))) return false;
        const ChildDiagnosticConfiguration empty{};
        const auto& configured = diagnosticConfiguration_ ? *diagnosticConfiguration_ : empty;
        const auto background = snowdesktop::background_work_observation::ObserveBudget();
        return flag(configured.backgroundAttempted) && flag(configured.backgroundConfigured) &&
            flag(configured.backgroundResourceUnavailable) && flag(configured.sharedBoundAfter) &&
            number(configured.sharedVmBudget) && number(configured.backgroundCapacity) &&
            number(configured.backgroundCommittedBytes) && add(configured.backgroundReason) &&
            number(background.eventCapacity) && number(background.committedBytes) && number(background.recordedEvents) &&
            flag(background.captureClaimed) && flag(background.writersDrained) && flag(background.recordsLost) && flag(background.capacityExceeded);
    }
    bool FrameMatches() const noexcept
    {
        if (!valid_ || !view_ || view_->magic != kMagic || view_->version != kVersion || !ValidNonce(nonce_) ||
            std::memcmp(view_->nonce, nonce_.data(), nonce_.size()) != 0 || !OriginalCaseMatches() ||
            view_->requestKind != (ownedFile_ ? 1u : exactSource_ ? 2u : 0u) || view_->leafSize != ownedLeaf_.size() ||
            (view_->leafSize && std::memcmp(view_->leaf, ownedLeaf_.data(), ownedLeaf_.size() * sizeof(wchar_t)) != 0)) return false;
        const auto expected = EncodeBudgets(budgets_);
        return std::equal(expected.begin(), expected.end(), view_->budgets);
    }
    bool OriginalCaseMatches() const noexcept
    {
        return view_ && caseMapping_ && caseView_ && view_->requestSize == requestCaseId_.size() &&
            view_->requestHandle == static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(caseMapping_)) &&
            std::memcmp(caseView_, requestCaseId_.data(), requestCaseId_.size()) == 0;
    }
    Child() = default;
    bool ExactSourceIntact() const noexcept
    {
        try
        {
            if (!exactSource_ || !exactSourceAccepted_ || !view_ || !parent_ || parentPid_ != view_->parentPid ||
                parentCreated_.dwLowDateTime != view_->parentCreated.dwLowDateTime ||
                parentCreated_.dwHighDateTime != view_->parentCreated.dwHighDateTime ||
                WaitForSingleObject(parent_, 0) != WAIT_TIMEOUT || view_->magic != kMagic || view_->version != kVersion ||
                view_->requestKind != 2 || !OriginalCaseMatches() || caseId_ != requestCaseId_ ||
                !SameId(view_->childRootId, exactRootIds_[0]) || !SameId(view_->ownedProfileId, exactRootIds_[1]) ||
                !SameId(view_->ownedSourceId, exactRootIds_[2]) ||
                view_->ownedProfileHandle != static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(exactRoots_[1])) ||
                view_->ownedSourceHandle != static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(exactRoots_[2])) ||
                view_->responseSize != exactSourcePath_.native().size() * sizeof(wchar_t) ||
                std::memcmp(view_->response, exactSourcePath_.native().data(), exactSourcePath_.native().size() * sizeof(wchar_t)) != 0) return false;
            const auto expectedBudgets = EncodeBudgets(budgets_);
            if (!std::equal(expectedBudgets.begin(), expectedBudgets.end(), view_->budgets)) return false;
            for (std::size_t i = 0; i < exactRoots_.size(); ++i)
                if (!HandleIdentity(exactRoots_[i], exactRootIds_[i], true)) return false;
            return DirectChild(HandlePath(exactRoots_[0]), HandlePath(exactRoots_[1])) &&
                DirectChild(HandlePath(exactRoots_[1]), HandlePath(exactRoots_[2]));
        }
        catch (...) { return false; }
    }
    bool Offer(HANDLE root, HANDLE directory, HANDLE pre, HANDLE flight, HANDLE terminal,
        const FILE_ID_INFO& rootId, const FILE_ID_INFO& directoryId, const FILE_ID_INFO& preId,
        const FILE_ID_INFO& flightId, const FILE_ID_INFO& terminalId, std::string_view execution,
        LONG expectedState, LONG offeredState) noexcept
    {
        if (!FrameMatches() || InterlockedCompareExchange(&view_->state, expectedState, expectedState) != expectedState ||
            execution.empty() || execution.size() > sizeof(view_->executionId)) return false;
        const std::array<HANDLE, 5> handles{root, directory, pre, flight, terminal};
        const std::array<FILE_ID_INFO, 5> ids{rootId, directoryId, preId, flightId, terminalId};
        for (std::size_t i = 0; i < (terminal ? 5u : 4u); ++i)
            if (!HandleIdentity(handles[i], ids[i], i < 2)) return false;
        for (std::size_t i = 0; i < handles.size(); ++i)
        { view_->checkpointHandles[i] = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handles[i])); view_->checkpointIds[i] = ids[i]; }
        std::memcpy(view_->executionId, execution.data(), execution.size());
        view_->executionSize = static_cast<DWORD>(execution.size());
        return InterlockedCompareExchange(&view_->state, offeredState, expectedState) == expectedState;
    }
    bool Publish(const std::string& bytes, LONG expectedState = 4) noexcept
    {
        if (!FrameMatches() || bytes.size() > kLimit || InterlockedCompareExchange(&view_->state, expectedState, expectedState) != expectedState) return false;
        std::memcpy(view_->response, bytes.data(), bytes.size());
        view_->responseSize = static_cast<DWORD>(bytes.size()); view_->childPid = GetCurrentProcessId();
        return InterlockedCompareExchange(&view_->state, 2, expectedState) == expectedState;
    }
    HANDLE mapping_ = nullptr, caseMapping_ = nullptr, pendingReportMapping_ = nullptr, parent_ = nullptr;
    DWORD parentPid_ = 0;
    FILETIME parentCreated_{};
    Shared* view_ = nullptr;
    const char* caseView_ = nullptr;
    std::string caseId_, requestCaseId_, nonce_, offeredCase_;
    std::wstring ownedLeaf_;
    Budgets budgets_{};
    std::optional<ChildDiagnosticConfiguration> diagnosticConfiguration_;
    bool valid_ = false, nativeGraph_ = false, ownedFile_ = false, ownedAccepted_ = false, exactSource_ = false, exactSourceAccepted_ = false;
    // No transport bit or child checkpoint can establish a trusted parent
    // ledger. Until a private verified-ledger capability is wired here this
    // stays false, so a filesystem mutation permit cannot be issued.
    bool exactParentLedgerVerified_ = false;
    std::array<HANDLE, 3> exactRoots_{};
    std::array<HANDLE, 4> exactParentLedgerPins_{};
    std::array<FILE_ID_INFO, 4> exactParentLedgerIds_{};
    std::string exactParentPreBytes_, exactParentHeadBytes_, exactChildPreBytes_, exactChildFlightBytes_, exactParentBinding_;
    std::wstring exactParentPreName_, exactParentHeadName_;
    bool exactParentLedgerObserved_ = false, exactPermitLedgerBound_ = false;
    std::array<FILE_ID_INFO, 3> exactRootIds_{};
    std::filesystem::path exactSourcePath_;
    std::string offeredExecution_;
    std::unique_ptr<v3_scheduler::V4ExactOwnedFilesPermit> exactPermit_;
    bool exactPermitIssued_ = false;
    std::uint64_t ownedScanPages_ = 0, ownedScanWork_ = 0;
};
} // namespace drag_global::virtual_child_protocol

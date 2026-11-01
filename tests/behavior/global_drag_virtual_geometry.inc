#pragma once
#include "data/layout_storage.h"

// Include after Profile/Access and the native OLE driver, before the adapter.
// This prepares controllable coordinates only; production still performs
// hit-testing, selection, materialization and layout commits.
namespace behavior
{
struct V4VirtualDesktopGeometry
{
    bool prepared = false, resourceUnavailable = false;
    std::string candidateId;
    POINT client{}, screen{};
    GridCell expectedCell;
    Access::TextDesktopInitial initial;
    std::vector<std::optional<POINTL>> pointsByProgramIndex;
    std::vector<std::string> unresolved;
};

inline V4VirtualDesktopGeometry PrepareV4VirtualDesktopGeometry(OwnedProfileDesktopRoot& root,
    const drag_global::V4FamilyValidatedContext& verified,
    const drag_global::event_graph::V4FamilyIdentityContext& identity, POINT proposedClient)
{
    using namespace drag_global;
    V4VirtualDesktopGeometry result;
    try
    {
        if (!root.profile || !root.profile->app || !root.StillOwned() ||
            verified.Status() != V4CoordinateStatus::coordinate_ready || identity.Status() != V4FamilyStatus::schema_only ||
            verified.TaskBActive() || verified.FamilyBinding() != identity.FamilyBinding() ||
            verified.Axes().size() != identity.Axes().size())
        { result.unresolved.emplace_back("virtual_geometry_context_or_host_unavailable"); return result; }
        for (std::size_t i = 0; i < identity.Axes().size(); ++i)
        {
            const auto& a = verified.Axes()[i]; const auto& b = identity.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.values != b.values || a.label != b.label)
            { result.unresolved.emplace_back("virtual_geometry_schema_mismatch"); return result; }
        }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable) result.resourceUnavailable = true;
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty())
        { result.unresolved.emplace_back("virtual_geometry_candidate_unavailable"); return result; }
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        if (value("V4.source.kind") != "content_only" || value("T01") != "external_ingress" ||
            value("T02.virtual_files") != "yes" || value("target.E01") != "free_desktop" ||
            value("target.E02") != "none" || value("target.E19") != "named" ||
            value("H01.region") != "empty_slot" || value("H06.page_record") != "saved" ||
            value("H06.page_dimensions") != "valid" || value("H07.occupancy") != "partial" ||
            value("H08.first_direction") != "unobserved" || value("H08.later_direction") != "na" ||
            value("G02.hover_first") != "na" || value("G02.hover_later") != "na" ||
            value("G04.termination") != "drop")
        { result.unresolved.emplace_back("virtual_geometry_scene_or_motion_unimplemented"); return result; }
        auto& profile = *root.profile;
        if (Access::asyncOwner != profile.app || Access::productionDispatch != 1 || !Access::completionWindow ||
            !IsWindow(Access::completionWindow) || GetWindowThreadProcessId(Access::completionWindow, nullptr) != GetCurrentThreadId())
        { result.unresolved.emplace_back("virtual_geometry_window_unavailable"); return result; }
        const auto initial = Access::InspectTextDesktopInitial(*profile.app, profile.desktop / L"Sentinel.txt", proposedClient);
        const auto dimension = [](std::string_view label) -> int {
            if (label == "one") return 1; if (label == "two") return 2; if (label == "three") return 3;
            if (label == "four") return 4; if (label == "eight") return 8; if (label == "twelve") return 12;
            return -1;
        };
        if (initial.pageName.empty() || !initial.pageRecorded || !initial.savedColumns || !initial.savedRows ||
            *initial.savedColumns != initial.columns || *initial.savedRows != initial.rows ||
            dimension(value("target.E23")) != initial.columns || dimension(value("target.E24")) != initial.rows)
        { result.unresolved.emplace_back("virtual_geometry_page_or_dimensions_unavailable"); return result; }
        POINT screen = proposedClient, roundTrip{};
        if (!ClientToScreen(Access::completionWindow, &screen))
        { result.unresolved.emplace_back("virtual_geometry_client_to_screen_unavailable"); return result; }
        roundTrip = screen;
        if (!ScreenToClient(Access::completionWindow, &roundTrip) || roundTrip.x != proposedClient.x || roundTrip.y != proposedClient.y)
        { result.unresolved.emplace_back("virtual_geometry_screen_roundtrip_unavailable"); return result; }
        const auto program = verified.TransferProgram();
        result.unresolved.insert(result.unresolved.end(), program.unresolved.begin(), program.unresolved.end());
        result.pointsByProgramIndex.resize(program.steps.size());
        bool entered = false, dropped = false;
        for (std::size_t i = 0; i < program.steps.size(); ++i)
        {
            const auto kind = ole_driver::ProgramKind(program.steps[i].kind);
            if (!kind)
            {
                if (program.steps[i].kind != "configure_v4_source_input" &&
                    program.steps[i].kind != "observe_completion_or_rejection" &&
                    program.steps[i].kind != "assert_business_results")
                    result.unresolved.push_back("virtual_geometry_non_ole_stage_unimplemented:" + program.steps[i].kind);
                continue;
            }
            if ((*kind != ole_driver::Kind::enter && *kind != ole_driver::Kind::drop) ||
                (*kind == ole_driver::Kind::enter && entered) || (*kind == ole_driver::Kind::drop && (!entered || dropped)))
            { result.pointsByProgramIndex.clear(); result.unresolved.emplace_back("virtual_geometry_program_motion_unimplemented"); return result; }
            if (*kind == ole_driver::Kind::enter) entered = true; else dropped = true;
            result.pointsByProgramIndex[i] = POINTL{screen.x, screen.y};
        }
        if (!entered || !dropped)
        { result.pointsByProgramIndex.clear(); result.unresolved.emplace_back("virtual_geometry_program_terminal_unavailable"); return result; }
        result.candidateId = raw.id; result.client = proposedClient; result.screen = screen;
        result.expectedCell = initial.targetCell; result.initial = initial; result.prepared = true;
        result.unresolved.emplace_back("virtual_geometry_actual_drag_target_and_landing_unobserved");
        result.unresolved.emplace_back("virtual_geometry_pointer_screen_monitor_anchor_and_other_axes_unimplemented");
        result.unresolved.emplace_back("virtual_geometry_host_can_change_between_snapshots");
    }
    catch (const std::bad_alloc&) { result.prepared = false; result.resourceUnavailable = true; }
    catch (const std::length_error&) { result.prepared = false; result.resourceUnavailable = true; }
    catch (...) { result.prepared = false; result.unresolved.emplace_back("virtual_geometry_observation_unavailable"); }
    return result;
}

// Re-observe the same input immediately before an OLE call. This does not
// close the interval between the observation and production hit-testing.
inline bool RevalidateV4VirtualDesktopGeometry(OwnedProfileDesktopRoot& root,
    const drag_global::V4FamilyValidatedContext& verified,
    const drag_global::event_graph::V4FamilyIdentityContext& identity,
    const V4VirtualDesktopGeometry& baseline, std::size_t programIndex)
{
    if (!baseline.prepared || baseline.resourceUnavailable || programIndex >= baseline.pointsByProgramIndex.size() ||
        !baseline.pointsByProgramIndex[programIndex]) return false;
    const auto current = PrepareV4VirtualDesktopGeometry(root, verified, identity, baseline.client);
    if (!current.prepared || current.resourceUnavailable || current.candidateId != baseline.candidateId ||
        current.pointsByProgramIndex.size() != baseline.pointsByProgramIndex.size() ||
        !current.pointsByProgramIndex[programIndex] || current.screen.x != baseline.screen.x || current.screen.y != baseline.screen.y)
        return false;
    const auto& a = baseline.initial; const auto& b = current.initial;
    return a.pageId == b.pageId && a.pageName == b.pageName && a.sentinelKey == b.sentinelKey &&
        a.columns == b.columns && a.rows == b.rows && Same(a.sentinelCell, b.sentinelCell) &&
        Same(a.targetCell, b.targetCell) && a.sentinelSpan.columns == b.sentinelSpan.columns &&
        a.sentinelSpan.rows == b.sentinelSpan.rows && EqualRect(&a.pageBounds, &b.pageBounds) &&
        EqualRect(&a.targetBounds, &b.targetBounds) && a.layoutRevision == b.layoutRevision &&
        a.pageRecorded == b.pageRecorded && a.savedColumns == b.savedColumns && a.savedRows == b.savedRows;
}

struct V4VirtualLayoutDiskSnapshot
{
    struct ModelItem { std::wstring key; fs::path path; GridCell cell; };
    bool captured = false, parsed = false, resourceUnavailable = false;
    std::string candidateId, rawBytes, parseError;
    fs::path path;
    std::optional<FILE_ID_INFO> fileId;
    std::optional<std::uint64_t> layoutRevision;
    std::vector<ModelItem> modelItems;
    std::vector<snowdesktop::layout_storage::ItemRecord> diskItems;
    std::vector<std::string> unresolved;
};

// Read the actual primary layout and current model without writing either.
// The production parser is used only to expose values alongside the raw
// bytes; this is not an independent parser or a same-operation proof.
inline V4VirtualLayoutDiskSnapshot CaptureV4VirtualLayoutDiskSnapshot(OwnedProfileDesktopRoot& root,
    const drag_global::V4FamilyValidatedContext& verified,
    const drag_global::event_graph::V4FamilyIdentityContext& identity, std::size_t byteBudget)
{
    using namespace drag_global;
    V4VirtualLayoutDiskSnapshot out;
    try
    {
        if (!root.profile || !root.profile->app || !root.StillOwned() ||
            verified.Status() != V4CoordinateStatus::coordinate_ready || identity.Status() != V4FamilyStatus::schema_only ||
            verified.TaskBActive() || verified.FamilyBinding() != identity.FamilyBinding())
        { out.unresolved.emplace_back("virtual_layout_context_or_root_unavailable"); return out; }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable) out.resourceUnavailable = true;
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty())
        { out.unresolved.emplace_back("virtual_layout_candidate_unavailable"); return out; }
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        if (value("V4.source.kind") != "content_only" || value("T02.virtual_files") != "yes" ||
            value("T01") != "external_ingress" || value("target.E01") != "free_desktop")
        { out.unresolved.emplace_back("virtual_layout_candidate_scope_unimplemented"); return out; }
        auto& profile = *root.profile;
        if (Access::asyncOwner != profile.app || !Access::FileDropIdle(*profile.app) || !Access::LayoutSaveIdle(*profile.app))
        { out.unresolved.emplace_back("virtual_layout_host_not_idle"); return out; }
        const auto revision = Access::LayoutRevision(*profile.app);
        out.path = fs::path(Access::LayoutPath(*profile.app));
        if (out.path != profile.root / L"layout" / L"SnowDesktop.layout.json")
        { out.unresolved.emplace_back("virtual_layout_isolated_path_unavailable"); return out; }
        struct Guard { HANDLE h = INVALID_HANDLE_VALUE; ~Guard() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
        Guard directory{CreateFileW(out.path.parent_path().c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        FILE_ATTRIBUTE_TAG_INFO directoryTag{};
        if (directory.h == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(directory.h, FileAttributeTagInfo, &directoryTag, sizeof(directoryTag)) ||
            !(directoryTag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (directoryTag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        { out.unresolved.emplace_back("virtual_layout_directory_unavailable"); return out; }
        Guard file{CreateFileW(out.path.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        FILE_ID_INFO id{}, namedId{}; FILE_ATTRIBUTE_TAG_INFO tag{}, namedTag{}; FILE_STANDARD_INFO before{}, after{}, namedSize{};
        FILE_BASIC_INFO beforeTime{}, afterTime{}, namedTime{};
        if (file.h == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(file.h, FileIdInfo, &id, sizeof(id)) ||
            !std::any_of(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), [](BYTE b) { return b != 0; }) ||
            !GetFileInformationByHandleEx(file.h, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !GetFileInformationByHandleEx(file.h, FileStandardInfo, &before, sizeof(before)) ||
            !GetFileInformationByHandleEx(file.h, FileBasicInfo, &beforeTime, sizeof(beforeTime)) || before.EndOfFile.QuadPart < 0)
        { out.unresolved.emplace_back("virtual_layout_file_identity_unavailable"); return out; }
        const auto size = static_cast<std::uint64_t>(before.EndOfFile.QuadPart);
        if (size > byteBudget || size > (std::numeric_limits<std::size_t>::max)())
        { out.resourceUnavailable = true; out.unresolved.emplace_back("virtual_layout_byte_budget"); return out; }
        out.rawBytes.resize(static_cast<std::size_t>(size));
        std::size_t offset = 0;
        while (offset < out.rawBytes.size())
        {
            const auto amount = static_cast<DWORD>((std::min)(out.rawBytes.size() - offset, static_cast<std::size_t>(4096)));
            DWORD count = 0;
            if (!ReadFile(file.h, out.rawBytes.data() + offset, amount, &count, nullptr) || !count)
            { out.rawBytes.resize(offset); out.unresolved.emplace_back("virtual_layout_read_unavailable"); return out; }
            offset += count;
        }
        Guard named{CreateFileW(out.path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (!GetFileInformationByHandleEx(file.h, FileStandardInfo, &after, sizeof(after)) ||
            !GetFileInformationByHandleEx(file.h, FileBasicInfo, &afterTime, sizeof(afterTime)) ||
            before.EndOfFile.QuadPart != after.EndOfFile.QuadPart ||
            beforeTime.LastWriteTime.QuadPart != afterTime.LastWriteTime.QuadPart ||
            named.h == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(named.h, FileIdInfo, &namedId, sizeof(namedId)) ||
            !GetFileInformationByHandleEx(named.h, FileAttributeTagInfo, &namedTag, sizeof(namedTag)) ||
            (namedTag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !GetFileInformationByHandleEx(named.h, FileStandardInfo, &namedSize, sizeof(namedSize)) ||
            !GetFileInformationByHandleEx(named.h, FileBasicInfo, &namedTime, sizeof(namedTime)) ||
            namedSize.EndOfFile.QuadPart != after.EndOfFile.QuadPart ||
            namedTime.LastWriteTime.QuadPart != afterTime.LastWriteTime.QuadPart ||
            !OwnedProfileDesktopRoot::SameId(id, namedId) || !root.StillOwned() ||
            !Access::LayoutSaveIdle(*profile.app) || Access::LayoutRevision(*profile.app) != revision)
        { out.unresolved.emplace_back("virtual_layout_changed_during_observation"); return out; }
        out.fileId = id; out.candidateId = raw.id; out.layoutRevision = revision;
        for (const auto& item : Access::Items(*profile.app)) out.modelItems.push_back({item.layoutKey, fs::path(item.parsingName), item.gridCell});
        snowdesktop::layout_storage::Document document;
        out.parsed = snowdesktop::layout_storage::ParseDocument(out.rawBytes, document, &out.parseError);
        if (out.parsed) out.diskItems = std::move(document.items);
        out.captured = true;
        out.unresolved.emplace_back("virtual_layout_same_run_write_and_reload_causality_unverified");
        out.unresolved.emplace_back("virtual_layout_parser_shared_with_production_and_snapshot_nonatomic");
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.captured = false; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.captured = false; }
    catch (...) { out.captured = false; out.unresolved.emplace_back("virtual_layout_observation_unavailable"); }
    return out;
}

struct V4VirtualLayoutEventEvidence
{
    struct Record { std::size_t index = 0; snowdesktop::external_transfer_observation::Event event{}; };
    bool sameReceipt = false, modelTimerObserved = false, synchronousSaveObserved = false;
    std::optional<bool> snapshotRevisionMatches;
    bool writeInputObserved = false;
    std::optional<bool> snapshotPathMatchesWrite, snapshotBytesMatchWrite;
    std::vector<std::byte> writePathBytes, writeInputBytes;
    std::optional<std::uint64_t> saveRevision;
    std::vector<Record> records;
    // The immutable same-receipt observer snapshot was taken before its
    // explicit reload. The later snapshot remains separate evidence.
    std::optional<V4VirtualLayoutDiskSnapshot> beforeReloadDisk;
    std::optional<V4VirtualLayoutDiskSnapshot> disk;
    std::vector<std::string> unresolved;
};

// Rebuild all source/flow identities from the same real receipt. The current
// revision and event values are local observations; a revision alone does not
// bind the bytes on disk to a particular write or prove durable completion.
inline V4VirtualLayoutEventEvidence InspectV4VirtualLayoutEvents(OwnedProfileDesktopRoot& root,
    const drag_global::ole_driver::V4ConfiguredContentOleRunReceipt& receipt,
    const drag_global::V4FamilyValidatedContext& verified,
    const drag_global::event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const drag_global::event_graph::V4JoinedGraphVariant& selected,
    const drag_global::mixed_fixture::ConfiguredContentBinding& binding,
    const std::shared_ptr<drag_global::mixed_fixture::ConfiguredContentOwner>& owner,
    OleDragDropAdapter* adapter, const std::shared_ptr<const drag_global::ole_driver::V3HostEpoch>& epoch,
    const drag_global::ole_driver::MessageTrace& trace, HWND control,
    const drag_global::mixed_fixture::VirtualExpectedManifest& manifest,
    std::size_t contentByteBudget, std::size_t virtualBudget, std::size_t layoutByteBudget)
{
    namespace diagnostic = snowdesktop::external_transfer_observation;
    using namespace drag_global::ole_driver;
    V4VirtualLayoutEventEvidence out;
    try
    {
        if (!root.profile || !root.profile->app || !root.StillOwned() || !receipt.SealedBusiness() ||
            !receipt.SameBoundHost(binding, owner, adapter, epoch, trace, control) ||
            receipt.HostIdentity() != reinterpret_cast<std::uintptr_t>(root.profile->app) || !diagnostic::Complete())
        { out.unresolved.emplace_back("virtual_layout_receipt_or_log_unavailable"); return out; }
        const auto flow = InspectV4VirtualFlow(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control, manifest, contentByteBudget, virtualBudget);
        const auto refresh = InspectV4VirtualRefresh(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control, manifest, contentByteBudget, virtualBudget);
        out.unresolved.insert(out.unresolved.end(), flow.unresolved.begin(), flow.unresolved.end());
        out.unresolved.insert(out.unresolved.end(), refresh.unresolved.begin(), refresh.unresolved.end());
        if (!flow.identityValid || !flow.batchObserved || !flow.pipelineReturned || !flow.callbackReturned ||
            !refresh.localRefreshBound || !refresh.modelSnapshotConsumed || flow.batch != refresh.batch)
        { out.unresolved.emplace_back("virtual_layout_flow_or_model_consumption_unavailable"); return out; }
        // The actual restricted path consumes the Shell snapshot on a desktop
        // timer and calls SaveLayoutSlots synchronously inside that same scope.
        // Refresh inspected the unique model/write/finish markers and the real
        // timer WndProc return; it did not infer an async layout queue.
        out.sameReceipt = true;
        out.modelTimerObserved = refresh.modelSnapshotConsumed;
        if (refresh.modelOutcome != std::optional<std::int64_t>{4} || !refresh.modelLayoutSaveObserved ||
            !refresh.modelLayoutSaveRevision)
        { out.unresolved.emplace_back("virtual_layout_synchronous_model_save_unavailable"); return out; }
        out.synchronousSaveObserved = true;
        out.saveRevision = *refresh.modelLayoutSaveRevision;
        for (const auto& entry : refresh.appRecords)
            if (entry.event.phase >= diagnostic::Phase::layout_request_enter &&
                entry.event.phase <= diagnostic::Phase::layout_finish_return)
                out.records.push_back({entry.index, entry.event});
        const V4VirtualLayoutEventEvidence::Record* write = nullptr;
        const V4VirtualLayoutEventEvidence::Record* written = nullptr;
        for (const auto& entry : out.records)
        {
            if (entry.event.phase == diagnostic::Phase::layout_write_enter && entry.event.extra == *out.saveRevision)
            { if (write) { out.unresolved.emplace_back("virtual_layout_write_ambiguous"); return out; } write = &entry; }
            if (entry.event.phase == diagnostic::Phase::layout_write_return && entry.event.extra == *out.saveRevision)
            { if (written) { out.unresolved.emplace_back("virtual_layout_write_return_ambiguous"); return out; } written = &entry; }
        }
        if (!write || !written || write->index >= written->index)
        { out.unresolved.emplace_back("virtual_layout_write_interval_unavailable"); return out; }
        const auto writeStartIndex = write->index, writeEndIndex = written->index;
        const auto writeThread = write->event.thread;
        const auto writeScope = write->event.windowScope;
        bool pathSeen = false, contentsSeen = false;
        diagnostic::ReadSession blobSession;
        if (!blobSession.Open()) { out.unresolved.emplace_back("virtual_layout_write_blob_log_lost"); return out; }
        for (std::size_t i = 0; i < blobSession.Count(); ++i)
        {
            diagnostic::Event event;
            if (!blobSession.ReadEvent(i, event)) { out.unresolved.emplace_back("virtual_layout_write_blob_log_lost"); return out; }
            if (event.phase != diagnostic::Phase::layout_write_input_path && event.phase != diagnostic::Phase::layout_write_input_bytes) continue;
            if (event.object != receipt.HostIdentity() || event.extra != *out.saveRevision) continue;
            if (i <= writeStartIndex || i >= writeEndIndex || event.thread != writeThread ||
                event.windowScope != writeScope || !event.hasBlob)
            { out.unresolved.emplace_back("virtual_layout_write_blob_identity_unavailable"); return out; }
            auto& seen = event.phase == diagnostic::Phase::layout_write_input_path ? pathSeen : contentsSeen;
            auto& target = event.phase == diagnostic::Phase::layout_write_input_path ? out.writePathBytes : out.writeInputBytes;
            if (seen) { out.unresolved.emplace_back("virtual_layout_write_blob_ambiguous"); return out; }
            if (event.blobSize > diagnostic::BlobLimit())
            { out.unresolved.emplace_back("virtual_layout_write_blob_unavailable"); return out; }
            std::vector<std::byte> copied(event.blobSize); std::size_t size = 0;
            if (!blobSession.CopyBlob(i, copied.data(), copied.size(), size) || size != copied.size())
            { out.unresolved.emplace_back("virtual_layout_write_blob_unavailable"); return out; }
            seen = true; target = std::move(copied);
            out.records.push_back({i, event});
        }
        if (!pathSeen || !contentsSeen || out.writePathBytes.empty())
        { out.unresolved.emplace_back("virtual_layout_write_input_unavailable"); return out; }
        out.writeInputObserved = true;
        const auto business = receipt.SealedBusiness();
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (business && business->beforeReloadLayout && !raw.resourceUnavailable && raw.errors.empty() &&
            !raw.id.empty() && business->beforeReloadLayout->captured &&
            !business->beforeReloadLayout->resourceUnavailable &&
            business->beforeReloadLayout->candidateId == raw.id &&
            business->beforeReloadLayout->path == root.profile->root / L"layout" / L"SnowDesktop.layout.json" &&
            business->beforeReloadLayout->rawBytes.size() <= layoutByteBudget)
            out.beforeReloadDisk = *business->beforeReloadLayout;
        else out.unresolved.emplace_back("virtual_layout_before_reload_snapshot_unavailable");
        if (out.beforeReloadDisk && out.beforeReloadDisk->layoutRevision)
        {
            out.snapshotRevisionMatches = *out.beforeReloadDisk->layoutRevision == *out.saveRevision;
            const auto& actualPath = out.beforeReloadDisk->path.native();
            out.snapshotPathMatchesWrite = actualPath.size() <= (std::numeric_limits<std::size_t>::max)() / sizeof(wchar_t) &&
                out.writePathBytes.size() == actualPath.size() * sizeof(wchar_t) &&
                std::memcmp(out.writePathBytes.data(), actualPath.data(), out.writePathBytes.size()) == 0;
            out.snapshotBytesMatchWrite = out.writeInputBytes.size() == out.beforeReloadDisk->rawBytes.size() &&
                (out.writeInputBytes.empty() || std::memcmp(out.writeInputBytes.data(), out.beforeReloadDisk->rawBytes.data(), out.writeInputBytes.size()) == 0);
        }
        if (out.beforeReloadDisk) out.unresolved.insert(out.unresolved.end(), out.beforeReloadDisk->unresolved.begin(), out.beforeReloadDisk->unresolved.end());
        out.disk = CaptureV4VirtualLayoutDiskSnapshot(root, verified, identity, layoutByteBudget);
        out.unresolved.emplace_back("virtual_layout_late_snapshot_after_business_reload_not_initial_save");
        out.unresolved.emplace_back("virtual_layout_write_path_value_does_not_bind_physical_file_identity");
        out.unresolved.emplace_back("virtual_layout_reentrancy_late_work_and_durability_unverified");
        if (out.disk) out.unresolved.insert(out.unresolved.end(), out.disk->unresolved.begin(), out.disk->unresolved.end());
    }
    catch (...) { out.unresolved.emplace_back("virtual_layout_event_observation_unavailable"); }
    return out;
}
}

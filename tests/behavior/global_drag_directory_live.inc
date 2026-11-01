#pragma once

// Include after the directory fixture and directory lease.
namespace drag_global::directory_fixture
{
// Re-read the already-created direct tree and the exact Shell construction
// input without querying IDataObject. This is a bounded, non-atomic snapshot;
// it does not provide a source lease or bind a later host read/commit.
struct LiveSourceObservation
{
    bool captured = false, resourceUnavailable = false;
    std::string candidateId;
    std::filesystem::path source;
    FILE_ID_INFO rootIdentity{};
    DWORD ownerThread = 0;
    std::uintptr_t sourceAddress = 0;
    std::weak_ptr<const ole_file_source_fixture::Source::LifetimeToken> sourceLifetime;
    std::vector<NodeObservation> nodes;
    std::vector<std::string> unresolved;
};

inline LiveSourceObservation ObserveLiveSource(const V4FamilyDescriptor& family,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const FixtureObservation& fixture, const std::shared_ptr<DirectoryCopyLease>& lease,
    const ole_file_source_fixture::Source& source,
    std::size_t maxNodes, std::size_t maxBytes)
{
    LiveSourceObservation out;
    try
    {
        // Do not access an arbitrary caller-supplied path. The lease owns the
        // candidate-bound root/children and its pinned parent throughout this
        // observation. Revalidation remains a non-atomic read, not a permit.
        const auto held = RevalidateDirectoryCopyLease(family, verified, identity, lease);
        if (!held.inputObserved || held.resourceUnavailable || !lease || lease->Source() != fixture.source ||
            lease->CandidateId() != fixture.candidateId || lease->OwnerThread() != GetCurrentThreadId())
        { out.resourceUnavailable = held.resourceUnavailable; out.unresolved = held.unresolved;
          out.unresolved.emplace_back("directory_live_bound_lease_unavailable"); return out; }
        if (verified.Status() != V4CoordinateStatus::coordinate_ready || identity.Status() != V4FamilyStatus::schema_only ||
            verified.TaskBActive() || family.b || verified.FamilyBinding() != identity.FamilyBinding() ||
            V4FamilyBinding(family) != identity.FamilyBinding() || verified.Axes().size() != identity.Axes().size())
        { out.unresolved.emplace_back("directory_live_authority_unavailable"); return out; }
        for (std::size_t i = 0; i < identity.Axes().size(); ++i)
        {
            const auto& a = verified.Axes()[i]; const auto& b = identity.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.values != b.values || a.label != b.label)
            { out.unresolved.emplace_back("directory_live_schema_mismatch"); return out; }
        }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable) out.resourceUnavailable = true;
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || raw.id != fixture.candidateId ||
            !fixture.inputsObserved || !fixture.rootCreated || !fixture.rootIdentityObserved ||
            family.a.kind != V4SourceKind::file_only || family.a.fileCount.kind != V3CountKind::exact ||
            family.a.fileCount.decimal != "1" || family.a.filePaths.size() != 1 || family.a.filePaths.front().index != "0" ||
            family.a.filePaths.front().kind != V4PayloadKind::exact || family.a.filePaths.front().base != V4PathBase::raw ||
            family.a.directoryRoots.size() != 1 ||
            family.a.directoryRoots.front().rootIndex != "0" ||
            family.a.directoryRoots.front().completeness != V4DirectoryCompleteness::complete ||
            Value(verified.Axes(), verified.Coordinate(), "V4.file.member0_kind") != "directory" ||
            Value(verified.Axes(), verified.Coordinate(), "V4.file.member0_identity") != "unique")
        { out.unresolved.emplace_back("directory_live_candidate_or_tree_unavailable"); return out; }
        if (family.a.directoryNodes.size() > maxNodes)
        { out.resourceUnavailable = true; out.unresolved.emplace_back("directory_live_node_budget"); return out; }
        if (family.a.filePaths.front().utf16leHex.size() > 131068)
        { out.resourceUnavailable = true; out.unresolved.emplace_back("directory_live_path_budget"); return out; }
        std::filesystem::path modelSource;
        if (!DecodePath(family.a.filePaths.front().utf16leHex, modelSource) || modelSource != fixture.source ||
            !modelSource.is_absolute() || !SafeRelative(modelSource.filename()))
        { out.unresolved.emplace_back("directory_live_source_path_unavailable"); return out; }
        auto native = modelSource; native.make_preferred();
        ole_file_source_fixture::FileDropCallState calls;
        if (!source.HasConstructionShell() || source.ConstructionPaths().size() != 1 || source.ConstructionPaths().front() != native ||
            source.owner != GetCurrentThreadId() || !source.asynchronous || source.wrongApartment || source.starts || source.ends ||
            source.dataReads || source.fileReads || source.identityReads || !source.fileDropReads.empty() || !source.formatProbes.empty() ||
            source.fileDropObservationFailed || source.formatProbeObservationFailed ||
            !source.ReadFileDropCallState(calls) || calls.failed || calls.count || calls.depth)
        { out.unresolved.emplace_back("directory_live_shell_source_baseline_unavailable"); return out; }
        struct Handle { HANDLE h = INVALID_HANDLE_VALUE; ~Handle() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
        const auto observeDirectory = [&](const std::filesystem::path& path, FILE_ID_INFO& id) {
            Handle h{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
            FILE_ATTRIBUTE_TAG_INFO tag{};
            return h.h != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(h.h, FileAttributeTagInfo, &tag, sizeof(tag)) &&
                (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                GetFileInformationByHandleEx(h.h, FileIdInfo, &id, sizeof(id));
        };
        FILE_ID_INFO rootId{};
        if (!observeDirectory(modelSource, rootId) || !SameId(rootId, fixture.rootIdentity) ||
            fixture.nodes.size() != family.a.directoryNodes.size())
        { out.unresolved.emplace_back("directory_live_root_or_node_count_unavailable"); return out; }
        std::vector<std::wstring> expectedNames;
        std::set<std::string> slots, objectIds;
        std::size_t total = 0;
        for (const auto& node : family.a.directoryNodes)
        {
            NodeObservation observed; std::vector<std::byte> expected;
            if (node.relativePathUtf16Hex.size() > 131068 ||
                (node.contentKind == V4PayloadKind::exact && node.contentHex.size() / 2 > maxBytes - total))
            { out.resourceUnavailable = true; out.unresolved.emplace_back("directory_live_node_payload_budget"); return out; }
            if (node.rootIndex != "0" || node.pathKind != V4PayloadKind::exact ||
                !DecodePath(node.relativePathUtf16Hex, observed.relative) || !SafeRelative(observed.relative) ||
                observed.relative.has_parent_path() || node.objectIdentity.kind != V3CountKind::exact ||
                !slots.insert(node.slotIndex).second || !objectIds.insert(node.objectIdentity.decimal).second ||
                (node.nodeKind != V4DirectoryNodeKind::file && node.nodeKind != V4DirectoryNodeKind::directory))
            { out.unresolved.emplace_back("directory_live_child_shape_unimplemented"); return out; }
            observed.slot = node.slotIndex; observed.directory = node.nodeKind == V4DirectoryNodeKind::directory;
            if ((observed.directory && node.contentKind != V4PayloadKind::not_applicable) ||
                (!observed.directory && (node.contentKind != V4PayloadKind::exact || !DecodeHex(node.contentHex, expected))))
            { out.unresolved.emplace_back("directory_live_child_content_unavailable"); return out; }
            if (expected.size() > maxBytes - total)
            { out.resourceUnavailable = true; out.unresolved.emplace_back("directory_live_byte_budget"); return out; }
            total += expected.size();
            const auto prior = std::find_if(fixture.nodes.begin(), fixture.nodes.end(),
                [&](const auto& item) { return item.slot == observed.slot; });
            if (prior == fixture.nodes.end() || prior->relative != observed.relative || prior->directory != observed.directory ||
                !prior->created || !prior->identityObserved || (!observed.directory && (!prior->bytesObserved || prior->actualBytes != expected)))
            { out.unresolved.emplace_back("directory_live_fixture_node_unavailable"); return out; }
            for (const auto& prev : out.nodes)
            {
                const auto a = prev.relative.native(), b = observed.relative.native();
                if (a.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
                    b.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
                { out.unresolved.emplace_back("directory_live_name_comparison_unavailable"); return out; }
                const int compared = CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE);
                if (!compared || compared == CSTR_EQUAL)
                { out.unresolved.emplace_back("directory_live_child_alias_or_comparison_unavailable"); return out; }
            }
            const auto path = modelSource / observed.relative;
            if (observed.directory)
            {
                if (!observeDirectory(path, observed.identity))
                { out.unresolved.emplace_back("directory_live_child_directory_unavailable"); return out; }
                std::error_code error; std::filesystem::directory_iterator it(path, error), end;
                if (error || it != end) { out.unresolved.emplace_back("directory_live_nested_contents_unimplemented"); return out; }
            }
            else
            {
                Handle file{CreateFileW(path.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
                FILE_ATTRIBUTE_TAG_INFO tag{}; FILE_BASIC_INFO before{}, after{}; LARGE_INTEGER size{}, afterSize{};
                if (file.h == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(file.h, FileAttributeTagInfo, &tag, sizeof(tag)) ||
                    (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                    !GetFileInformationByHandleEx(file.h, FileIdInfo, &observed.identity, sizeof(observed.identity)) ||
                    !GetFileInformationByHandleEx(file.h, FileBasicInfo, &before, sizeof(before)) ||
                    !GetFileSizeEx(file.h, &size) || size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) != expected.size())
                { out.unresolved.emplace_back("directory_live_child_file_unavailable"); return out; }
                observed.actualBytes.resize(expected.size());
                for (std::size_t offset = 0; offset < expected.size();)
                {
                    const DWORD amount = static_cast<DWORD>((std::min)(expected.size() - offset, std::size_t{4096})); DWORD read = 0;
                    if (!ReadFile(file.h, observed.actualBytes.data() + offset, amount, &read, nullptr) || !read || read > amount)
                    { out.unresolved.emplace_back("directory_live_child_read_unavailable"); return out; }
                    offset += read;
                }
                if (!GetFileInformationByHandleEx(file.h, FileBasicInfo, &after, sizeof(after)) ||
                    !GetFileSizeEx(file.h, &afterSize) || size.QuadPart != afterSize.QuadPart ||
                    before.LastWriteTime.QuadPart != after.LastWriteTime.QuadPart || observed.actualBytes != expected)
                { out.unresolved.emplace_back("directory_live_child_changed_or_mismatched"); return out; }
                observed.bytesObserved = true;
            }
            if (!SameId(observed.identity, prior->identity))
            { out.unresolved.emplace_back("directory_live_child_identity_mismatch"); return out; }
            for (const auto& previous : out.nodes)
                if (SameId(previous.identity, observed.identity))
                { out.unresolved.emplace_back("directory_live_physical_alias_unimplemented"); return out; }
            observed.created = true; observed.identityObserved = true;
            expectedNames.push_back(observed.relative.native()); out.nodes.push_back(std::move(observed));
        }
        std::sort(expectedNames.begin(), expectedNames.end());
        std::vector<std::wstring> actualNames; std::error_code error;
        std::filesystem::directory_iterator it(modelSource, error), end;
        if (error) { out.unresolved.emplace_back("directory_live_enumeration_unavailable"); return out; }
        for (; it != end; it.increment(error))
        {
            if (error) { out.unresolved.emplace_back("directory_live_enumeration_unavailable"); return out; }
            actualNames.push_back(it->path().filename().native());
        }
        std::sort(actualNames.begin(), actualNames.end());
        FILE_ID_INFO finalRoot{};
        if (error || actualNames != expectedNames || !observeDirectory(modelSource, finalRoot) || !SameId(finalRoot, rootId))
        { out.unresolved.emplace_back("directory_live_enumeration_or_root_changed"); return out; }
        const auto heldAfter = RevalidateDirectoryCopyLease(family, verified, identity, lease);
        if (!heldAfter.inputObserved || heldAfter.resourceUnavailable)
        { out.resourceUnavailable = heldAfter.resourceUnavailable; out.unresolved = heldAfter.unresolved;
          out.unresolved.emplace_back("directory_live_bound_lease_changed"); return out; }
        out.candidateId = raw.id; out.source = modelSource; out.rootIdentity = rootId;
        out.ownerThread = source.owner; out.sourceAddress = reinterpret_cast<std::uintptr_t>(&source);
        out.sourceLifetime = source.LifetimeIdentity(); out.captured = true;
        out.unresolved = heldAfter.unresolved;
        out.unresolved.insert(out.unresolved.end(), {"directory_live_snapshot_non_atomic", "directory_live_other_axes_unimplemented",
            "directory_live_host_read_completion_and_business_unimplemented"});
        return out;
    }
    catch (const std::bad_alloc&) { out.captured = false; out.resourceUnavailable = true; return out; }
    catch (const std::length_error&) { out.captured = false; out.resourceUnavailable = true; return out; }
    catch (...) { out.captured = false; return out; }
}
} // namespace drag_global::directory_fixture

#pragma once

// Include after Profile, native accessors and file observers, before the v4
// adapter. This component only observes a real desktop gesture's state; it
// never dispatches pointer input or commits a layout.
namespace behavior
{
struct V4NativeDesktopLayoutSnapshot
{
    fs::path path;
    std::string rawBytes;
    std::optional<FILE_ID_INFO> fileId;
    std::optional<std::uint64_t> revision;
    bool captured = false, resourceUnavailable = false;
    std::vector<std::string> unresolved;
};
inline V4NativeDesktopLayoutSnapshot CaptureV4NativeDesktopLayout(OwnedProfileDesktopRoot& root, std::size_t byteBudget)
{
    V4NativeDesktopLayoutSnapshot out;
    try
    {
        if (!root.profile || !root.profile->app || !root.StillOwned())
        { out.unresolved.emplace_back("native_layout_root_unavailable"); return out; }
        auto& profile = *root.profile;
        if (Access::asyncOwner != profile.app || !Access::NativeDragIdle(*profile.app) ||
            !Access::FileDropIdle(*profile.app) || !Access::LayoutSaveIdle(*profile.app))
        { out.unresolved.emplace_back("native_layout_host_not_idle"); return out; }
        const auto revision = Access::LayoutRevision(*profile.app);
        out.path = fs::path(Access::LayoutPath(*profile.app));
        if (out.path != profile.root / L"layout" / L"SnowDesktop.layout.json")
        { out.unresolved.emplace_back("native_layout_path_unavailable"); return out; }
        struct Guard { HANDLE h = INVALID_HANDLE_VALUE; ~Guard() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
        Guard directory{CreateFileW(out.path.parent_path().c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        FILE_ATTRIBUTE_TAG_INFO tag{};
        if (directory.h == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(directory.h, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        { out.unresolved.emplace_back("native_layout_directory_unavailable"); return out; }
        Guard file{CreateFileW(out.path.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        FILE_ID_INFO id{}, namedId{}; FILE_STANDARD_INFO before{}, after{}, namedSize{};
        FILE_BASIC_INFO beforeTime{}, afterTime{}, namedTime{}; FILE_ATTRIBUTE_TAG_INFO namedTag{};
        if (file.h == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(file.h, FileIdInfo, &id, sizeof(id)) ||
            !std::any_of(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), [](BYTE b) { return b != 0; }) ||
            !GetFileInformationByHandleEx(file.h, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !GetFileInformationByHandleEx(file.h, FileStandardInfo, &before, sizeof(before)) ||
            !GetFileInformationByHandleEx(file.h, FileBasicInfo, &beforeTime, sizeof(beforeTime)) || before.EndOfFile.QuadPart < 0)
        { out.unresolved.emplace_back("native_layout_file_unavailable"); return out; }
        const auto size = static_cast<std::uint64_t>(before.EndOfFile.QuadPart);
        if (size > byteBudget || size > (std::numeric_limits<std::size_t>::max)())
        { out.resourceUnavailable = true; out.unresolved.emplace_back("native_layout_byte_budget"); return out; }
        out.rawBytes.resize(static_cast<std::size_t>(size)); std::size_t offset = 0;
        while (offset < out.rawBytes.size())
        {
            const auto amount = static_cast<DWORD>((std::min)(out.rawBytes.size() - offset, static_cast<std::size_t>(4096)));
            DWORD count = 0;
            if (!ReadFile(file.h, out.rawBytes.data() + offset, amount, &count, nullptr) || !count)
            { out.rawBytes.resize(offset); out.unresolved.emplace_back("native_layout_read_unavailable"); return out; }
            offset += count;
        }
        Guard named{CreateFileW(out.path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (!GetFileInformationByHandleEx(file.h, FileStandardInfo, &after, sizeof(after)) ||
            !GetFileInformationByHandleEx(file.h, FileBasicInfo, &afterTime, sizeof(afterTime)) ||
            before.EndOfFile.QuadPart != after.EndOfFile.QuadPart || beforeTime.LastWriteTime.QuadPart != afterTime.LastWriteTime.QuadPart ||
            beforeTime.ChangeTime.QuadPart != afterTime.ChangeTime.QuadPart || named.h == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(named.h, FileIdInfo, &namedId, sizeof(namedId)) ||
            !GetFileInformationByHandleEx(named.h, FileAttributeTagInfo, &namedTag, sizeof(namedTag)) ||
            (namedTag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !GetFileInformationByHandleEx(named.h, FileStandardInfo, &namedSize, sizeof(namedSize)) ||
            !GetFileInformationByHandleEx(named.h, FileBasicInfo, &namedTime, sizeof(namedTime)) ||
            namedSize.EndOfFile.QuadPart != after.EndOfFile.QuadPart || namedTime.LastWriteTime.QuadPart != afterTime.LastWriteTime.QuadPart ||
            namedTime.ChangeTime.QuadPart != afterTime.ChangeTime.QuadPart || !OwnedProfileDesktopRoot::SameId(id, namedId) ||
            !root.StillOwned() || !Access::NativeDragIdle(*profile.app) || !Access::LayoutSaveIdle(*profile.app) ||
            Access::LayoutRevision(*profile.app) != revision)
        { out.unresolved.emplace_back("native_layout_changed_during_observation"); return out; }
        out.fileId = id; out.revision = revision; out.captured = true;
        out.unresolved.emplace_back("native_layout_non_atomic_not_durability_or_same_run_proof"); return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; return out; }
    catch (...) { out.unresolved.emplace_back("native_layout_observation_unavailable"); return out; }
}
struct V4NativeDesktopMoveEvidence
{
    std::string candidateId;
    fs::path source;
    std::vector<std::byte> expectedBytes;
    std::optional<FILE_ID_INFO> sourceId;
    std::optional<GridCell> initialCell, finalCell, reloadedCell;
    std::optional<std::uint64_t> initialRevision, finalRevision, reloadedRevision;
    std::vector<std::pair<fs::path, GridCell>> initialMembers, finalMembers, reloadedMembers;
    std::optional<bool> targetMatched, membersPreserved, sourcePreserved, sentinelPreserved, reloadMatched;
    std::optional<bool> sourceDiskCellMatches, sentinelDiskCellMatches;
    bool diskParsed = false;
    std::optional<V4NativeDesktopLayoutSnapshot> beforeReloadLayout;
    bool initialObserved = false, finalObserved = false, reloadObserved = false;
    std::vector<std::string> unresolved{
        "native_desktop_device_capture_and_release_causality_unverified",
        "native_desktop_target_geometry_and_all_candidate_axes_unimplemented",
        "native_desktop_layout_write_completion_and_disk_reload_unverified",
        "native_desktop_filesystem_and_model_snapshots_non_atomic"};
};

struct V4NativeDesktopMoveObservation
{
    OwnedProfileDesktopRoot& root;
    Profile& profile;
    fs::path source;
    GridCell requestedLanding;
private:
    friend struct V4NativeDesktopDeviceObservation;
    std::unique_ptr<ObservedPhysicalFile> sourceFile, sentinelFile;
    std::shared_ptr<const drag_global::V4OwnedRelativePathBinding> relativeBinding;
    V4NativeDesktopMoveEvidence evidence;
    bool baselineBound_ = false, captureStarted_ = false;
    std::shared_ptr<const drag_global::ole_driver::V4NativeDesktopObserverIdentity> identity_{
        new drag_global::ole_driver::V4NativeDesktopObserverIdentity};

public:
    V4NativeDesktopMoveObservation(const V4NativeDesktopMoveObservation&) = delete;
    V4NativeDesktopMoveObservation& operator=(const V4NativeDesktopMoveObservation&) = delete;
    V4NativeDesktopMoveEvidence Evidence() const { return evidence; }

    static Profile& BoundProfile(OwnedProfileDesktopRoot& owned)
    {
        RequireObservation(owned.profile && owned.profile->app && owned.StillOwned(), "native desktop profile unavailable");
        return *owned.profile;
    }

    static bool SamePath(const fs::path& a, const fs::path& b)
    {
        std::error_code error;
        const bool same = fs::equivalent(a, b, error);
        RequireObservation(!error, "native desktop member identity unavailable");
        return same;
    }

    GridCell FindCell(const std::vector<std::pair<fs::path, GridCell>>& members, const fs::path& path) const
    {
        std::optional<GridCell> found;
        for (const auto& member : members)
            if (SamePath(member.first, path))
            {
                RequireObservation(!found, "native desktop member identity ambiguous");
                found = member.second;
            }
        RequireObservation(found.has_value(), "native desktop member unavailable");
        return *found;
    }

    std::vector<std::pair<fs::path, GridCell>> ReadMembers() const
    {
        std::vector<std::pair<fs::path, GridCell>> members;
        for (const auto& item : Access::Items(*profile.app)) members.emplace_back(fs::path(item.parsingName), item.gridCell);
        return members;
    }

    V4NativeDesktopMoveObservation(OwnedProfileDesktopRoot& owned, const drag_global::V4SingleFileFixture& fixture,
        const drag_global::V4FamilyValidatedContext& verified,
        const drag_global::event_graph::V4FamilyIdentityContext& identity, GridCell landing)
        : root(owned), profile(BoundProfile(owned)), source(fixture.file), requestedLanding(std::move(landing)),
          relativeBinding(fixture.relativeBinding)
    {
        using namespace drag_global;
        RequireObservation(owned.profile && owned.profile->app && owned.StillOwned() &&
            verified.Status() == V4CoordinateStatus::coordinate_ready && identity.Status() == V4FamilyStatus::schema_only &&
            !verified.TaskBActive() && verified.FamilyBinding() == identity.FamilyBinding(),
            "native desktop family or host unavailable");
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        RequireObservation(!raw.resourceUnavailable && raw.errors.empty() && !raw.id.empty() && raw.id == fixture.candidateId &&
            value("T01") == "internal_native" && value("V4.source.kind") == "file_only" &&
            value("V4.source.file_count") == "one" && value("V4.file.member0_kind") == "file" &&
            value("V4.file.member0_identity") == "unique" && value("source.E01") == "free_desktop" &&
            value("target.E01") == "free_desktop" && fixture.inputPrepared && fixture.created && fixture.fileIdentityObserved &&
            Access::asyncOwner == profile.app && Access::productionDispatch == 1 && Access::nativePointerDispatch &&
            Access::messageTrace && !Access::messageTrace->LostOrCrossThread(), "native desktop candidate or fixture unavailable");
        RequireObservation(fixture.root == profile.desktop && source.parent_path() == profile.desktop &&
            OwnedProfileDesktopRoot::SameId(fixture.rootId, root.desktopId), "native desktop input root differs from host");
        constexpr char digits[] = "0123456789abcdef";
        const auto append = [&](std::string& text, unsigned byte) {
            text.push_back(digits[(byte >> 4) & 15]); text.push_back(digits[byte & 15]);
        };
        std::string pathValue = "utf16le_hex_", bytesValue = "hex_";
        static_assert(sizeof(wchar_t) == 2, "Windows UTF-16 code units required");
        for (const auto unit : source.native())
        { const auto code = static_cast<std::uint16_t>(unit); append(pathValue, code & 255); append(pathValue, code >> 8); }
        for (const auto byte : fixture.bytes) append(bytesValue, std::to_integer<unsigned>(byte));
        const auto selectedPath = value("V4.file_path.index_0.utf16le");
        constexpr std::string_view relativePrefix = "owned_root_relative_utf16le_hex_";
        bool pathMatches = false;
        if (selectedPath.starts_with(relativePrefix))
        {
            if (relativeBinding && relativeBinding->StillBound() && relativeBinding->CandidateId() == raw.id &&
                relativeBinding->FamilyBinding() == identity.FamilyBinding() &&
                OwnedProfileDesktopRoot::SameId(relativeBinding->RootId(), root.desktopId) &&
                relativeBinding->RootPath() == fixture.root && relativeBinding->Members().size() == 1)
            {
                const auto& member = relativeBinding->Members().front();
                pathMatches = member.physicalIndex == 0 && member.physicalPath == source &&
                    member.relativeUtf16leHex == selectedPath.substr(relativePrefix.size()) &&
                    pathValue == std::string("utf16le_hex_") + member.physicalUtf16leHex;
            }
        }
        else pathMatches = !relativeBinding && selectedPath == pathValue;
        RequireObservation(pathMatches && value("V4.file_content.index_0.bytes") == bytesValue,
            "native desktop candidate input differs from fixture");
        sourceFile = std::make_unique<ObservedPhysicalFile>(source);
        RequireObservation(OwnedProfileDesktopRoot::SameId(sourceFile->Identity(), fixture.fileId) &&
            sourceFile->PathStillNamesObject(source) && sourceFile->HasBytes(fixture.bytes),
            "native desktop file identity or bytes unavailable");
        Wait([&] { return Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app) &&
            Access::NativeDragIdle(*profile.app); });
        const auto& moving = Find(*profile.app, source);
        RequireObservation(!moving.selected && moving.gridSpan.columns == 1 && moving.gridSpan.rows == 1 &&
            !Same(moving.gridCell, requestedLanding), "native desktop source baseline unavailable");
        for (const auto& item : Access::Items(*profile.app))
        {
            if (SamePath(fs::path(item.parsingName), source)) continue;
            RequireObservation(item.gridCell.pageId != requestedLanding.pageId || requestedLanding.column < item.gridCell.column ||
                requestedLanding.row < item.gridCell.row || requestedLanding.column >= item.gridCell.column + item.gridSpan.columns ||
                requestedLanding.row >= item.gridCell.row + item.gridSpan.rows, "native desktop target is occupied");
        }
        evidence.candidateId = raw.id; evidence.source = source; evidence.expectedBytes = fixture.bytes;
        evidence.sourceId = sourceFile->Identity(); evidence.initialCell = moving.gridCell;
        evidence.initialMembers = ReadMembers();
        const auto sentinelPath = profile.desktop / L"Sentinel.txt";
        RequireObservation(evidence.initialMembers.size() == 2 && !SamePath(source, sentinelPath) &&
            Same(FindCell(evidence.initialMembers, source), moving.gridCell), "native desktop input members unavailable");
        (void)FindCell(evidence.initialMembers, sentinelPath);
        sentinelFile = std::make_unique<ObservedPhysicalFile>(sentinelPath);
        const std::vector<std::byte> sentinelBytes{std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'},
            std::byte{'u'}, std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}};
        RequireObservation(!sourceFile->SameObject(*sentinelFile) && sentinelFile->HasBytes(sentinelBytes),
            "native desktop sentinel identity or bytes unavailable");
        evidence.initialRevision = Access::LayoutRevision(*profile.app); evidence.initialObserved = true;
    }

private:
    void CaptureAfterIdle()
    {
        RequireObservation(evidence.initialObserved && root.StillOwned() && Access::NativeDragIdle(*profile.app) &&
            Access::LayoutSaveIdle(*profile.app) && Access::FileDropIdle(*profile.app), "native desktop postflight unavailable");
        evidence.finalMembers = ReadMembers(); evidence.finalCell = FindCell(evidence.finalMembers, source);
        evidence.targetMatched = Same(*evidence.finalCell, requestedLanding);
        bool preserved = evidence.initialMembers.size() == evidence.finalMembers.size();
        for (const auto& member : evidence.initialMembers)
        {
            if (SamePath(member.first, source)) continue;
            const auto current = FindCell(evidence.finalMembers, member.first);
            preserved = preserved && Same(current, member.second);
        }
        evidence.membersPreserved = preserved;
        evidence.sourcePreserved = sourceFile->PathStillNamesObject(source) && sourceFile->HasBytes(evidence.expectedBytes);
        const std::vector<std::byte> sentinelBytes{std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'},
            std::byte{'u'}, std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}};
        evidence.sentinelPreserved = sentinelFile->PathStillNamesObject(profile.desktop / L"Sentinel.txt") &&
            sentinelFile->HasBytes(sentinelBytes);
        evidence.finalRevision = Access::LayoutRevision(*profile.app); evidence.finalObserved = true;
        evidence.beforeReloadLayout = CaptureV4NativeDesktopLayout(root, 4u * 1024u * 1024u);
        const auto& layout = *evidence.beforeReloadLayout;
        if (layout.captured && !layout.resourceUnavailable)
        {
            snowdesktop::layout_storage::Document document;
            std::string error;
            evidence.diskParsed = snowdesktop::layout_storage::ParseDocument(layout.rawBytes, document, &error);
            if (evidence.diskParsed)
            {
                const auto sentinelPath = profile.desktop / L"Sentinel.txt";
                const auto& moving = Find(*profile.app, source);
                const auto& sentinel = Find(*profile.app, sentinelPath);
                const auto inspect = [&](const std::wstring& key, const GridCell& cell) -> std::optional<bool> {
                    const snowdesktop::layout_storage::ItemRecord* found = nullptr;
                    const auto normalized = ToUpperInvariant(key);
                    for (const auto& item : document.items)
                        if (ToUpperInvariant(Utf8ToWide(item.key)) == normalized)
                        { if (found) return std::nullopt; found = &item; }
                    if (!found) return false;
                    if (!found->page || !found->column || !found->row) return std::nullopt;
                    return Utf8ToWide(*found->page) == cell.pageId && *found->column == cell.column && *found->row == cell.row;
                };
                evidence.sourceDiskCellMatches = inspect(moving.layoutKey, *evidence.finalCell);
                evidence.sentinelDiskCellMatches = inspect(sentinel.layoutKey, FindCell(evidence.finalMembers, sentinelPath));
            }
            else evidence.unresolved.emplace_back("native_desktop_layout_parse_unavailable");
        }
    }

    // The production disk-read path is invoked without fabricating a final
    // layout. An unchanged in-memory result alone does not prove disk reload.
    void ReloadAndCapture()
    {
        RequireObservation(evidence.finalObserved && root.StillOwned(), "native desktop postflight not observed");
        Access::RequestAsync(*profile.app, true);
        Wait([&] { return Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app) &&
            Access::NativeDragIdle(*profile.app); });
        evidence.reloadedMembers = ReadMembers(); evidence.reloadedCell = FindCell(evidence.reloadedMembers, source);
        bool matches = evidence.reloadedMembers.size() == evidence.finalMembers.size();
        for (const auto& member : evidence.finalMembers)
            matches = matches && Same(FindCell(evidence.reloadedMembers, member.first), member.second);
        const std::vector<std::byte> sentinelBytes{std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'},
            std::byte{'u'}, std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}};
        evidence.reloadMatched = matches && sourceFile->PathStillNamesObject(source) && sourceFile->HasBytes(evidence.expectedBytes) &&
            sentinelFile->PathStillNamesObject(profile.desktop / L"Sentinel.txt") && sentinelFile->HasBytes(sentinelBytes);
        evidence.reloadedRevision = Access::LayoutRevision(*profile.app); evidence.reloadObserved = true;
    }
public:
    void BindExecution(const std::shared_ptr<drag_global::ole_driver::V4NativeDesktopExecutionKey>& key,
        const std::shared_ptr<drag_global::ole_driver::NativeHostEpoch>& epoch)
    {
        RequireObservation(key && !baselineBound_ && !captureStarted_ && evidence.initialObserved && evidence.initialRevision &&
            root.StillOwned() && (!relativeBinding || relativeBinding->StillBound()) &&
            root.profile.get() == &profile && profile.app && Access::asyncOwner == profile.app &&
            Access::NativeDragIdle(*profile.app) && Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app) &&
            Access::LayoutRevision(*profile.app) == *evidence.initialRevision && sourceFile && sentinelFile &&
            sourceFile->PathStillNamesObject(source) && sourceFile->HasBytes(evidence.expectedBytes),
            "native desktop same-run baseline unavailable");
        const std::vector<std::byte> sentinelBytes{std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'},
            std::byte{'u'}, std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}};
        RequireObservation(sentinelFile->PathStillNamesObject(profile.desktop / L"Sentinel.txt") && sentinelFile->HasBytes(sentinelBytes),
            "native desktop sentinel baseline unavailable");
        const auto members = ReadMembers();
        RequireObservation(members.size() == evidence.initialMembers.size(), "native desktop baseline members changed");
        for (const auto& member : evidence.initialMembers)
            RequireObservation(Same(FindCell(members, member.first), member.second), "native desktop baseline cell changed");
        RequireObservation(root.StillOwned() && key->BindBaseline(epoch, profile.app, evidence.candidateId, identity_),
            "native desktop same-run binding unavailable");
        baselineBound_ = true;
    }
    void CaptureExecution(const std::shared_ptr<drag_global::ole_driver::V4NativeDesktopExecutionKey>& key,
        const std::shared_ptr<drag_global::ole_driver::NativeHostEpoch>& epoch)
    {
        RequireObservation(key && baselineBound_ && !captureStarted_ &&
            (!relativeBinding || relativeBinding->StillBound()) &&
            key->BeginCapture(epoch, profile.app, evidence.candidateId, identity_), "native desktop capture unavailable");
        captureStarted_ = true;
        try { CaptureAfterIdle(); ReloadAndCapture(); }
        catch (const std::exception& error) { evidence.unresolved.push_back(std::string("native_desktop_business_snapshot_incomplete:") + error.what()); }
        catch (...) { evidence.unresolved.emplace_back("native_desktop_business_snapshot_incomplete"); }
        auto snapshot = std::make_shared<const V4NativeDesktopMoveEvidence>(evidence);
        RequireObservation(key->Publish(epoch, profile.app, evidence.candidateId, identity_, std::move(snapshot)),
            "native desktop same-run sealing unavailable");
    }
};

// Construct only a restricted source-centre -> empty-cell-centre gesture from
// this candidate's authoritative program. This is an input proposal, not a
// hit-test, capture observation, completed move, or authorization to dispatch.
struct V4NativeDesktopPointProposal
{
    bool prepared = false, resourceUnavailable = false;
    std::string candidateId;
    POINT source{}, target{};
    RECT screenPage{};
    std::uint64_t layoutRevision = 0;
    std::vector<drag_global::ole_driver::NativePointInput> points;
    std::vector<std::string> unresolved;
};

inline V4NativeDesktopPointProposal PrepareV4NativeDesktopPoints(
    V4NativeDesktopMoveObservation& observed,
    const drag_global::event_graph::V4FamilyIdentityContext& identity,
    const drag_global::V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const drag_global::event_graph::V4NativeJoinedGraphVariant& graph)
{
    using namespace drag_global;
    V4NativeDesktopPointProposal out;
    try
    {
        const auto evidence = observed.Evidence();
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4NativeJoinedGraph::Replay(identity, verified, present, graph.id);
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() ||
            raw.id != evidence.candidateId || !replay.identityValid || !replay.errors.empty() ||
            replay.variant.id != graph.id || replay.variant.classes != graph.classes ||
            replay.variant.order != graph.order || replay.variant.unresolved != graph.unresolved ||
            !observed.root.StillOwned() || !evidence.initialObserved ||
            value("H08.first_direction") != "unobserved" || value("H08.later_direction") != "na" ||
            value("G02.hover_first") != "na" || value("G02.hover_later") != "na")
        { out.unresolved.emplace_back("native_desktop_point_candidate_or_motion_unavailable"); return out; }
        const auto source = Access::InspectNativeDesktopSource(*observed.profile.app, observed.source);
        const auto target = Access::InspectNativeDesktopLanding(*observed.profile.app, observed.requestedLanding);
        if (!evidence.initialCell || !Same(source.cell, *evidence.initialCell) ||
            !evidence.initialRevision || source.layoutRevision != *evidence.initialRevision ||
            target.layoutRevision != source.layoutRevision || !PtInRect(&target.screenPage, source.screenCenter) ||
            !PtInRect(&target.screenPage, target.screenCenter))
        { out.unresolved.emplace_back("native_desktop_point_scene_changed"); return out; }
        const auto dx = static_cast<std::int64_t>(target.screenCenter.x) - source.screenCenter.x;
        const auto dy = static_cast<std::int64_t>(target.screenCenter.y) - source.screenCenter.y;
        const auto abs64 = [](std::int64_t n) { return n < 0 ? -n : n; };
        if (abs64(dx) <= (std::max)(8, GetSystemMetrics(SM_CXDRAG)) &&
            abs64(dy) <= (std::max)(8, GetSystemMetrics(SM_CYDRAG)))
        { out.unresolved.emplace_back("native_desktop_point_drag_threshold_unavailable"); return out; }
        const auto program = verified.TransferProgram();
        std::size_t starts = 0, moves = 0, releases = 0;
        for (std::size_t i = 0; i < program.steps.size(); ++i)
        {
            const auto& kind = program.steps[i].kind;
            if (kind == "source_start") { ++starts; out.points.push_back({i, source.screenCenter}); }
            else if (kind == "move_past_drag_threshold") { ++moves; out.points.push_back({i, target.screenCenter}); }
            else if (kind == "release") { ++releases; out.points.push_back({i, target.screenCenter}); }
            else if (kind == "pointer_direction" || kind == "hover" || kind == "pointer_direction_later" || kind == "hover_later")
            { out.points.clear(); out.unresolved.emplace_back("native_desktop_point_extra_motion_unimplemented"); return out; }
        }
        if (starts != 1 || moves != 1 || releases != 1)
        { out.points.clear(); out.unresolved.emplace_back("native_desktop_point_program_unavailable"); return out; }
        const auto plan = ole_driver::BuildV4NativeExecutionPlan(identity, verified, present, graph,
            out.points, Access::completionWindow, target.screenPage);
        if (!plan.graphIdentityValid || !plan.sourcePoint || plan.points.size() != program.steps.size())
        { out.points.clear(); out.unresolved.emplace_back("native_desktop_point_graph_unavailable"); return out; }
        out.candidateId = raw.id; out.source = source.screenCenter; out.target = target.screenCenter;
        out.screenPage = target.screenPage; out.layoutRevision = source.layoutRevision; out.prepared = true;
        out.unresolved.emplace_back("native_desktop_point_geometry_must_be_revalidated_before_dispatch");
        out.unresolved.emplace_back("native_desktop_point_actual_hit_capture_and_commit_unobserved");
        out.unresolved.insert(out.unresolved.end(), plan.preconditions.begin(), plan.preconditions.end());
        out.unresolved.insert(out.unresolved.end(), plan.unclassified.begin(), plan.unclassified.end());
    }
    catch (const std::bad_alloc&) { out.prepared = false; out.resourceUnavailable = true; }
    catch (const std::length_error&) { out.prepared = false; out.resourceUnavailable = true; }
    catch (...) { out.prepared = false; out.unresolved.emplace_back("native_desktop_point_observation_unavailable"); }
    return out;
}

// Token and WndProc evidence for the same candidate/graph and host epoch.
// This observer cannot authorize dispatch or establish durable layout commit.
struct V4NativeDesktopDeviceObservation
{
    V4NativeDesktopMoveObservation& business;
    drag_global::EventProgram program;
    std::shared_ptr<drag_global::ole_driver::NativeHostEpoch> epoch;
    POINT startScreen{}, targetScreen{};
    std::size_t traceStart = 0, beforeEnd = (std::numeric_limits<std::size_t>::max)();
    bool initialHover = false, captureObserved = false, ended = false;
    std::vector<bool> seen;
    std::vector<std::optional<std::size_t>> moveClasses;
    std::map<std::size_t, snowdesktop::private_pointer::QueuedEventIdentity> classTokens;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::size_t> tokenClasses;
    std::optional<snowdesktop::private_pointer::QueuedEventIdentity> lastQueued;

    V4NativeDesktopDeviceObservation(V4NativeDesktopMoveObservation& observed,
        const drag_global::event_graph::V4FamilyIdentityContext& identity,
        const drag_global::V4FamilyValidatedContext& verified, const std::vector<bool>& present,
        const drag_global::event_graph::V4NativeJoinedGraphVariant& graph,
        std::shared_ptr<drag_global::ole_driver::NativeHostEpoch> host, POINT start, POINT target)
        : business(observed), program(verified.TransferProgram()), epoch(std::move(host)),
          startScreen(start), targetScreen(target), seen(program.steps.size(), false)
    {
        RequireHost();
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        RequireObservation(!raw.resourceUnavailable && raw.errors.empty() && raw.id == business.evidence.candidateId,
            "native desktop device candidate unavailable");
        const auto replay = drag_global::event_graph::V4NativeJoinedGraph::Replay(identity, verified, present, graph.id);
        RequireObservation(replay.identityValid && replay.errors.empty() && replay.variant.id == graph.id &&
            replay.variant.classes == graph.classes && replay.variant.order == graph.order &&
            replay.variant.unresolved == graph.unresolved, "native desktop graph identity unavailable");
        // The restricted concrete Program issues exactly one move. Its token
        // is bound to that actual call, even if the selected hypothesis says
        // that a corresponding event is absent; later comparison records the
        // mismatch without suppressing the production gesture.
        moveClasses.resize(program.steps.size());
        std::size_t moves = 0;
        for (std::size_t i = 0; i < program.steps.size(); ++i)
        {
            const auto& kind = program.steps[i].kind;
            if (kind == "move_past_drag_threshold") { moveClasses[i] = 0; ++moves; }
            else RequireObservation(kind != "pointer_direction" && kind != "hover" &&
                kind != "pointer_direction_later" && kind != "hover_later", "native desktop extra move unavailable");
        }
        RequireObservation(moves == 1, "native desktop single move unavailable");
        traceStart = Access::messageTrace->Observations().size();
    }
    void RequireHost() const
    { RequireObservation(Access::SameNativeHostEpoch(*business.profile.app, epoch), "native desktop host epoch unavailable"); }
    void BeforeBegin() const
    {
        RequireHost();
        RequireObservation(business.root.StillOwned() && (!business.relativeBinding || business.relativeBinding->StillBound()) &&
            Access::NativeDragIdle(*business.profile.app) &&
            Access::LayoutSaveIdle(*business.profile.app) && Access::FileDropIdle(*business.profile.app) &&
            business.sourceFile->PathStillNamesObject(business.source) &&
            business.sourceFile->HasBytes(business.evidence.expectedBytes), "native desktop baseline unavailable");
        const auto& item = Find(*business.profile.app, business.source);
        RequireObservation(!item.selected && business.evidence.initialCell && Same(item.gridCell, *business.evidence.initialCell),
            "native desktop source baseline changed");
        const auto x = static_cast<std::int64_t>(item.bounds.left) +
            (static_cast<std::int64_t>(item.bounds.right) - item.bounds.left) / 2;
        const auto y = static_cast<std::int64_t>(item.bounds.top) +
            (static_cast<std::int64_t>(item.bounds.bottom) - item.bounds.top) / 2;
        RequireObservation(item.bounds.right > item.bounds.left && item.bounds.bottom > item.bounds.top &&
            x >= (std::numeric_limits<LONG>::min)() && x <= (std::numeric_limits<LONG>::max)() &&
            y >= (std::numeric_limits<LONG>::min)() && y <= (std::numeric_limits<LONG>::max)(),
            "native desktop source geometry unavailable");
        const auto actual = Access::ExternalPoint({static_cast<LONG>(x), static_cast<LONG>(y)});
        RequireObservation(actual.x == startScreen.x && actual.y == startScreen.y,
            "native desktop source point changed");
        // Target geometry is deliberately not inferred from the caller's point.
    }
    void BeforeQueued(drag_global::ole_driver::NativeDevicePhase phase, std::optional<std::size_t> index,
        const snowdesktop::private_pointer::QueuedEventIdentity& token)
    {
        RequireHost();
        RequireObservation(token.ownerThread == GetCurrentThreadId() && token.session && token.event &&
            ((phase == drag_global::ole_driver::NativeDevicePhase::initial_hover && !index) ||
             (phase == drag_global::ole_driver::NativeDevicePhase::program_input && index && *index < program.steps.size())),
            "native desktop queued token unavailable");
        lastQueued = token;
    }
    void Observe(drag_global::ole_driver::NativeDevicePhase phase, std::optional<std::size_t> index,
        const snowdesktop::private_pointer::State& state,
        const snowdesktop::private_pointer::DeliveryObservationLog& log,
        std::optional<snowdesktop::private_pointer::QueuedEventIdentity> queued,
        std::optional<drag_global::ole_driver::NativeInputExpectation> expected)
    {
        using namespace drag_global::ole_driver;
        using namespace snowdesktop::private_pointer;
        RequireHost();
        RequireObservation(!log.failed && log.session && log.ownerThread == GetCurrentThreadId() && state.error.empty() &&
            state.pendingEvents == 0 && std::none_of(state.keys.begin(), state.keys.end(), [](bool pressed) { return pressed; }),
            "native desktop device log unavailable");
        if (phase == NativeDevicePhase::session_end)
        {
            RequireObservation(!queued && !expected && index && *index < program.steps.size() && !state.enabled &&
                program.steps[*index].kind == "end_pointer_session" && state.buttons == 0 && initialHover && !ended,
                "native desktop session end unavailable");
            ended = true; return;
        }
        RequireObservation(queued && expected && lastQueued && queued->ownerThread == log.ownerThread &&
            queued->session == log.session && queued->ownerThread == lastQueued->ownerThread &&
            queued->session == lastQueued->session && queued->event && queued->event == lastQueued->event && Access::messageTrace,
            "native desktop queued delivery identity unavailable");
        const DeliveryObservation* primary = nullptr;
        for (const auto& event : log.entries)
        {
            if (event.kind != DeliveryObservationKind::begin || !event.identity.queuedEvent || event.identity.forwarded ||
                event.identity.ownerThread != queued->ownerThread || event.identity.session != queued->session ||
                event.identity.event != queued->event) continue;
            RequireObservation(!primary, "native desktop queued delivery ambiguous"); primary = &event;
        }
        RequireObservation(primary && primary->window == Access::completionWindow, "native desktop delivery unavailable");
        POINT client = expected->screen;
        RequireObservation(ScreenToClient(Access::completionWindow, &client) &&
            client.x == expected->clientBeforeDispatch.x && client.y == expected->clientBeforeDispatch.y &&
            client.x >= (std::numeric_limits<short>::min)() && client.x <= (std::numeric_limits<short>::max)() &&
            client.y >= (std::numeric_limits<short>::min)() && client.y <= (std::numeric_limits<short>::max)(),
            "native desktop client coordinate unavailable");
        POINT roundTrip = client;
        RequireObservation(ClientToScreen(Access::completionWindow, &roundTrip) && roundTrip.x == expected->screen.x &&
            roundTrip.y == expected->screen.y, "native desktop coordinate roundtrip unavailable");
        const auto point = MAKELPARAM(static_cast<short>(client.x), static_cast<short>(client.y));
        RequireObservation(primary->message == expected->message && primary->wParam == expected->buttons && primary->lParam == point,
            "native desktop delivery differs from controlled input");
        const auto delivery = InspectNativeDelivery(*Access::messageTrace, traceStart, &log, primary->identity,
            primary->window, MessageRoute::desktop_window_proc, expected->message, expected->buttons, point);
        RequireObservation(delivery.matchedMessageAndPermit, "native desktop production WndProc or permit unavailable");
        if (phase == NativeDevicePhase::initial_hover)
        {
            RequireObservation(!index && !initialHover && state.enabled && state.buttons == 0 &&
                expected->screen.x == startScreen.x && expected->screen.y == startScreen.y,
                "native desktop initial hover unavailable");
            initialHover = true; return;
        }
        RequireObservation(initialHover && index && *index < seen.size() && !seen[*index], "native desktop program identity unavailable");
        const auto& kind = program.steps[*index].kind;
        if (kind == "source_start")
            RequireObservation(expected->message == WM_LBUTTONDOWN && expected->buttons == MK_LBUTTON &&
                expected->screen.x == startScreen.x && expected->screen.y == startScreen.y, "native desktop press unavailable");
        else if (kind == "release")
            RequireObservation(expected->message == WM_LBUTTONUP && expected->buttons == 0 &&
                expected->screen.x == targetScreen.x && expected->screen.y == targetScreen.y, "native desktop release unavailable");
        else
        {
            RequireObservation(expected->message == WM_MOUSEMOVE && expected->buttons == MK_LBUTTON && moveClasses[*index],
                "native desktop movement class unavailable");
            const auto classId = *moveClasses[*index];
            const auto [classEntry, classInserted] = classTokens.emplace(classId, *queued);
            RequireObservation(classInserted || (classEntry->second.ownerThread == queued->ownerThread &&
                classEntry->second.session == queued->session && classEntry->second.event == queued->event),
                "native desktop shared class has different tokens");
            const auto [tokenEntry, tokenInserted] = tokenClasses.emplace(std::make_pair(queued->session, queued->event), classId);
            RequireObservation(tokenInserted || tokenEntry->second == classId, "native desktop distinct classes share a token");
        }
        seen[*index] = true;
    }
    bool ProductionInputEntered() const
    {
        RequireHost();
        const auto& events = Access::messageTrace->Observations();
        RequireObservation(lastQueued && traceStart <= events.size(), "native desktop trace baseline unavailable");
        const bool delivered = std::any_of(events.begin() + static_cast<std::ptrdiff_t>(traceStart), events.end(), [&](const auto& event) {
            return event.window == Access::completionWindow && event.route == drag_global::ole_driver::MessageRoute::desktop_window_proc &&
                event.enteredThread == GetCurrentThreadId() && event.inputLParam && event.delivery.queuedEvent && !event.delivery.forwarded &&
                event.delivery.ownerThread == lastQueued->ownerThread && event.delivery.session == lastQueued->session &&
                event.delivery.event == lastQueued->event && event.delivery.delivery &&
                (event.message == WM_MOUSEMOVE || event.message == WM_LBUTTONDOWN || event.message == WM_LBUTTONUP);
        });
        if (delivered) return true;
        if (beforeEnd != (std::numeric_limits<std::size_t>::max)())
            throw drag_global::ole_driver::ObservationUnavailable("native desktop cleanup entry cannot be excluded");
        return false;
    }
    void BeforeEnd(const snowdesktop::private_pointer::State& state,
        const snowdesktop::private_pointer::DeliveryObservationLog& log)
    {
        RequireHost();
        RequireObservation(initialHover && !ended && lastQueued && !log.failed && log.session == lastQueued->session &&
            log.ownerThread == lastQueued->ownerThread && beforeEnd == (std::numeric_limits<std::size_t>::max)() &&
            captureObserved && state.enabled && !state.closing && state.pendingEvents == 0 && state.buttons == 0 && !state.captured &&
            state.point.x == targetScreen.x && state.point.y == targetScreen.y, "native desktop release boundary unavailable");
        for (std::size_t i = 0; i < program.steps.size(); ++i)
        {
            const auto& kind = program.steps[i].kind;
            if (kind == "source_start" || kind == "move_past_drag_threshold" || kind == "pointer_direction" ||
                kind == "hover" || kind == "pointer_direction_later" || kind == "hover_later" || kind == "release")
                RequireObservation(seen[i], "native desktop program input not observed before end");
        }
        beforeEnd = Access::messageTrace->Observations().size();
    }
    void Ended() const
    {
        RequireHost();
        const auto& events = Access::messageTrace->Observations();
        RequireObservation(ended && beforeEnd != (std::numeric_limits<std::size_t>::max)() && beforeEnd <= events.size(),
            "native desktop end boundary unavailable");
        bool cancel = false, leave = false;
        for (std::size_t i = beforeEnd; i < events.size(); ++i)
        {
            const auto& event = events[i];
            if (event.window != Access::completionWindow || event.route != drag_global::ole_driver::MessageRoute::desktop_window_proc ||
                !event.returned || event.enteredThread != GetCurrentThreadId() || event.returnedThread != event.enteredThread) continue;
            if (!cancel && event.message == WM_CANCELMODE) cancel = true;
            else if (cancel && event.message == WM_MOUSELEAVE) leave = true;
        }
        RequireObservation(cancel && leave, "native desktop end messages unavailable");
    }
    drag_global::ole_driver::NativeHostBoundaries Boundaries()
    {
        return {
            [this] { BeforeBegin(); },
            [this](auto phase, auto index, const auto& state, const auto& log, auto queued, auto expected) {
                Observe(phase, index, state, log, queued, expected); },
            [this](const auto&) {
                RequireHost(); RequireObservation(Access::NativeDragCaptured(*business.profile.app, business.source),
                    "native desktop production capture unavailable"); captureObserved = true; RequireHost(); },
            [this](const auto&) {
                RequireHost(); RequireObservation(captureObserved, "native desktop capture not observed");
                Wait([&] { return Access::NativeDragIdle(*business.profile.app) && Access::LayoutSaveIdle(*business.profile.app) &&
                    Access::FileDropIdle(*business.profile.app); }); RequireHost(); },
            [this](const auto&, const auto& state, const auto& log) { BeforeEnd(state, log); },
            [this](const auto&) { Ended(); },
            [](const auto&) { throw drag_global::ole_driver::ObservationUnavailable("native desktop requires sealed execution observer"); },
            [this] { RequireHost(); Access::ThrowAsyncException(); },
            [this] { return ProductionInputEntered(); },
            [this](auto phase, auto index, const auto& token) { BeforeQueued(phase, index, token); }};
    }
};
}

#pragma once

// Isolated observations for an actual Enter -> DragLeave, not a simulated
// cancellation or an assertion that later asynchronous work is impossible.
namespace behavior
{
struct V4LeaveLayoutSnapshot
{
    bool captured = false, resourceUnavailable = false;
    std::string candidateId, rawBytes;
    fs::path path;
    std::optional<FILE_ID_INFO> fileId;
    std::optional<std::uint64_t> revision;
    std::vector<std::string> unresolved;
};
inline V4LeaveLayoutSnapshot CaptureV4LeaveLayoutSnapshot(OwnedProfileDesktopRoot& root,
    const drag_global::V4FamilyValidatedContext& verified,
    const drag_global::event_graph::V4FamilyIdentityContext& identity, std::size_t byteBudget)
{
    using namespace drag_global;
    V4LeaveLayoutSnapshot out;
    try
    {
        if (!root.profile || !root.profile->app || !root.StillOwned() || verified.Status() != V4CoordinateStatus::coordinate_ready ||
            identity.Status() != V4FamilyStatus::schema_only || verified.TaskBActive() || verified.FamilyBinding() != identity.FamilyBinding())
        { out.unresolved.emplace_back("leave_layout_context_unavailable"); return out; }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable) { out.resourceUnavailable = true; return out; }
        const auto termination = Value(verified.Axes(), verified.Coordinate(), "G04.termination");
        const bool rejectedDrop = termination == "drop" && Value(verified.Axes(), verified.Coordinate(), "T03") == "none" &&
            Value(verified.Axes(), verified.Coordinate(), "T03.extra_protocol_bits") == "none";
        if (!raw.errors.empty() || raw.id.empty() || Value(verified.Axes(), verified.Coordinate(), "V4.source.kind") != "file_only" ||
            (termination != "leave" && !rejectedDrop))
        { out.unresolved.emplace_back("leave_layout_candidate_unavailable"); return out; }
        auto& profile = *root.profile;
        if (Access::asyncOwner != profile.app || !Access::FileDropIdle(*profile.app) || !Access::LayoutSaveIdle(*profile.app))
        { out.unresolved.emplace_back("leave_layout_host_not_idle"); return out; }
        const auto revision = Access::LayoutRevision(*profile.app); out.path = fs::path(Access::LayoutPath(*profile.app));
        if (out.path != profile.root / L"layout" / L"SnowDesktop.layout.json")
        { out.unresolved.emplace_back("leave_layout_path_unavailable"); return out; }
        struct Guard { HANDLE h = INVALID_HANDLE_VALUE; ~Guard() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
        Guard directory{CreateFileW(out.path.parent_path().c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        FILE_ATTRIBUTE_TAG_INFO tag{};
        if (directory.h == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(directory.h, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        { out.unresolved.emplace_back("leave_layout_directory_unavailable"); return out; }
        Guard file{CreateFileW(out.path.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        FILE_ID_INFO id{}, namedId{}; FILE_STANDARD_INFO before{}, after{}, namedSize{};
        FILE_BASIC_INFO beforeTime{}, afterTime{}, namedTime{}; FILE_ATTRIBUTE_TAG_INFO namedTag{};
        if (file.h == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(file.h, FileIdInfo, &id, sizeof(id)) ||
            !std::any_of(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), [](BYTE b) { return b != 0; }) ||
            !GetFileInformationByHandleEx(file.h, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !GetFileInformationByHandleEx(file.h, FileStandardInfo, &before, sizeof(before)) ||
            !GetFileInformationByHandleEx(file.h, FileBasicInfo, &beforeTime, sizeof(beforeTime)) || before.EndOfFile.QuadPart < 0)
        { out.unresolved.emplace_back("leave_layout_file_unavailable"); return out; }
        const auto size = static_cast<std::uint64_t>(before.EndOfFile.QuadPart);
        if (size > byteBudget || size > (std::numeric_limits<std::size_t>::max)())
        { out.resourceUnavailable = true; out.unresolved.emplace_back("leave_layout_byte_budget"); return out; }
        out.rawBytes.resize(static_cast<std::size_t>(size)); std::size_t offset = 0;
        while (offset < out.rawBytes.size())
        {
            const auto amount = static_cast<DWORD>((std::min)(out.rawBytes.size() - offset, static_cast<std::size_t>(4096)));
            DWORD count = 0;
            if (!ReadFile(file.h, out.rawBytes.data() + offset, amount, &count, nullptr) || !count)
            { out.rawBytes.resize(offset); out.unresolved.emplace_back("leave_layout_read_unavailable"); return out; }
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
            !root.StillOwned() || !Access::LayoutSaveIdle(*profile.app) || Access::LayoutRevision(*profile.app) != revision)
        { out.unresolved.emplace_back("leave_layout_changed_during_observation"); return out; }
        out.fileId = id; out.candidateId = raw.id; out.revision = revision; out.captured = true;
        out.unresolved.emplace_back("leave_layout_snapshot_non_atomic_and_not_durability_proof");
        out.unresolved.emplace_back("leave_layout_parent_identity_and_ancestor_aliases_unverified"); return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; return out; }
    catch (...) { out.unresolved.emplace_back("leave_layout_observation_unavailable"); return out; }
}

struct V4FileLeaveEvidence
{
    std::string candidateId;
    fs::path source, sentinel;
    std::vector<std::byte> expectedBytes;
    std::optional<FILE_ID_INFO> sourceId, sentinelId;
    std::string initialLayout, finalLayout;
    std::optional<V4LeaveLayoutSnapshot> initialLayoutSnapshot, beforeLeaveLayoutSnapshot, finalLayoutSnapshot;
    std::optional<std::uint64_t> initialRevision, finalRevision;
    std::optional<std::size_t> initialItemCount, finalItemCount, outputCount;
    std::optional<int> preLeaveStarts, preLeaveEnds, preLeaveFileReads;
    std::optional<std::size_t> preLeaveCallRecordCount;
    std::optional<int> starts, ends, fileReads;
    std::optional<bool> sourcePreserved, sentinelPreserved, initialCellsPreserved, layoutBytesUnchanged,
        layoutFileIdentityUnchanged, sourceLogAvailable;
    bool collectionTarget = false;
    std::vector<fs::path> initialCollectionMembers, finalCollectionMembers, reloadedCollectionMembers;
    std::optional<bool> collectionWidgetPreserved, collectionMembersPreserved, reloadedCollectionMembersPreserved;
    std::optional<std::uint64_t> reloadedRevision;
    bool inputObserved = false, baselineRevalidated = false, leaveAwaitReturned = false, finalObserved = false;
    std::vector<std::string> unresolved{"leave_absence_requires_complete_bounded_event_interval",
        "leave_background_quiescence_and_unobserved_reentry_unverified", "leave_full_graph_and_business_unimplemented"};
};

struct V4FileLeaveObservation
{
private:
    OwnedProfileDesktopRoot& root;
    Profile& profile;
    std::shared_ptr<drag_global::V4IndependentFilePayload> payload;
    std::shared_ptr<drag_global::V4IndependentShellOwner> owner;
    ole_file_source_fixture::Source& source;
    const drag_global::V4FamilyValidatedContext& verified_;
    const drag_global::event_graph::V4FamilyIdentityContext& familyIdentity_;
    V4FileLeaveEvidence evidence;
    std::unique_ptr<ObservedPhysicalFile> sourceFile, sentinelFile;
    std::vector<std::pair<fs::path, GridCell>> initialCells;
    bool collectionTarget_ = false;
    std::optional<DesktopWidget> initialCollection_;
    bool awaitStarted_ = false, awaitCompleted_ = false;
    std::shared_ptr<const drag_global::ole_driver::V4LeaveObserverIdentity> identity_{
        new drag_global::ole_driver::V4LeaveObserverIdentity};
    static ole_file_source_fixture::Source& BoundSource(const drag_global::V4IndependentShellSource& binding,
        const std::shared_ptr<drag_global::V4IndependentShellOwner>& owner)
    {
        auto* actual = binding.SourceObject();
        RequireObservation(actual && binding.inputPrepared && !binding.resourceUnavailable && binding.SameOwner(owner),
            "leave Shell source unavailable"); return *actual;
    }
    static Profile& BoundProfile(OwnedProfileDesktopRoot& ownedRoot)
    {
        RequireObservation(ownedRoot.profile && ownedRoot.profile->app && ownedRoot.StillOwned(), "leave profile unavailable");
        return *ownedRoot.profile;
    }
    static std::vector<std::byte> SentinelBytes()
    { return {std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'}, std::byte{'u'},
        std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}}; }
    bool CollectionBaselineMatches() const
    {
        if (!collectionTarget_) return true;
        if (!initialCollection_ || !Access::ContainerWorkIdle(*profile.app)) return false;
        const auto& widget = ContainerWidget(*profile.app, L"ingress-root");
        const auto members = Access::ContainerMembers(*profile.app, L"ingress-root");
        std::error_code error;
        const bool same = members.size() == 1 && fs::equivalent(members.front(), evidence.sentinel, error);
        return !error && same && widget.type == initialCollection_->type && widget.id == initialCollection_->id &&
            widget.title == initialCollection_->title && widget.customTitle == initialCollection_->customTitle &&
            widget.autoCollect == initialCollection_->autoCollect && widget.singleCellIconMode == initialCollection_->singleCellIconMode &&
            widget.gridSpan.columns == initialCollection_->gridSpan.columns && widget.gridSpan.rows == initialCollection_->gridSpan.rows &&
            Same(widget.gridCell, initialCollection_->gridCell) && widget.childWidgetIds == initialCollection_->childWidgetIds &&
            widget.activeCategoryId == initialCollection_->activeCategoryId;
    }
public:
    V4FileLeaveObservation(const V4FileLeaveObservation&) = delete;
    V4FileLeaveObservation& operator=(const V4FileLeaveObservation&) = delete;
    V4FileLeaveEvidence Evidence() const { return evidence; }
    V4FileLeaveObservation(OwnedProfileDesktopRoot& ownedRoot,
        const std::shared_ptr<drag_global::V4IndependentFilePayload>& input,
        const drag_global::V4IndependentShellSource& binding,
        const std::shared_ptr<drag_global::V4IndependentShellOwner>& sourceOwner,
        const drag_global::V4FamilyValidatedContext& verified,
        const drag_global::event_graph::V4FamilyIdentityContext& identity)
        : root(ownedRoot), profile(BoundProfile(ownedRoot)), payload(input), owner(sourceOwner), source(BoundSource(binding, sourceOwner)),
          verified_(verified), familyIdentity_(identity)
    {
        using namespace drag_global;
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        RequireObservation(root.StillOwned() && root.profile && root.profile->app &&
            verified.Status() == V4CoordinateStatus::coordinate_ready && identity.Status() == V4FamilyStatus::schema_only &&
            verified.FamilyBinding() == identity.FamilyBinding() && !raw.resourceUnavailable && raw.errors.empty() &&
            !raw.id.empty() && payload && payload->files.size() == 1 && payload->candidateId == raw.id &&
            binding.candidateId == raw.id && binding.SamePayload(payload) && Access::v4IndependentShellOwner == owner &&
            Access::asyncOwner == profile.app && Access::productionDispatch == 1 && Access::fixtureControlWindow &&
            Access::messageTrace && !Access::messageTrace->LostOrCrossThread() && source.asynchronous &&
            source.owner == GetCurrentThreadId() && value("V4.source.kind") == "file_only" &&
            value("V4.source.file_count") == "one" && value("V4.file.member0_kind") == "file" &&
            value("V4.file.member0_identity") == "unique" && value("T01") == "external_ingress" &&
            value("G04.termination") == "leave" && value("target.E01") == "free_desktop" &&
            ((value("target.E02") == "none" && value("H01.region") == "empty_slot") ||
             (value("target.E02") == "collection" && value("target.E03") == "widget_body" &&
              value("H01.region") == "widget_body")), "leave candidate or host unavailable");
        const auto snapshot = ole_driver::InspectV4IndependentFileInputs(verified, identity, *payload, source);
        RequireObservation(snapshot.captured && !snapshot.resourceUnavailable && snapshot.candidateId == raw.id &&
            snapshot.files.size() == 1, "leave source input unavailable");
        evidence.candidateId = raw.id; evidence.source = snapshot.files.front().path;
        evidence.expectedBytes = snapshot.files.front().bytes; evidence.sentinel = profile.desktop / L"Sentinel.txt";
        collectionTarget_ = value("target.E02") == "collection"; evidence.collectionTarget = collectionTarget_;
        if (collectionTarget_)
        {
            fs::path existing;
            const ContainerRow row{"v4_collection_leave", DesktopWidgetType::Collection, DesktopWidgetType::Collection, false, false};
            SeedDestination(profile, row, existing);
            RequireObservation(existing == evidence.sentinel, "leave collection initial member unavailable");
            Wait([&] { return Access::ContainerWorkIdle(*profile.app) && Access::FileDropIdle(*profile.app) &&
                Access::LayoutSaveIdle(*profile.app); });
            initialCollection_ = ContainerWidget(*profile.app, L"ingress-root");
            evidence.initialCollectionMembers = Access::ContainerMembers(*profile.app, L"ingress-root");
            std::error_code memberError;
            RequireObservation(evidence.initialCollectionMembers.size() == 1 &&
                fs::equivalent(evidence.initialCollectionMembers.front(), evidence.sentinel, memberError) && !memberError,
                "leave collection initial membership unavailable");
        }
        sourceFile = std::make_unique<ObservedPhysicalFile>(evidence.source);
        sentinelFile = std::make_unique<ObservedPhysicalFile>(evidence.sentinel);
        RequireObservation(OwnedProfileDesktopRoot::SameId(sourceFile->Identity(), snapshot.files.front().id) &&
            sourceFile->PathStillNamesObject(evidence.source) && sourceFile->HasBytes(evidence.expectedBytes) &&
            sentinelFile->PathStillNamesObject(evidence.sentinel) && sentinelFile->HasBytes(SentinelBytes()),
            "leave source or sentinel baseline unavailable");
        evidence.sourceId = sourceFile->Identity(); evidence.sentinelId = sentinelFile->Identity();
        RequireObservation(Outputs(profile).empty(), "leave initial desktop outputs unavailable");
        for (const auto& item : Access::Items(*profile.app)) initialCells.emplace_back(fs::path(item.parsingName), item.gridCell);
        RequireObservation(initialCells.size() == 1, "leave initial desktop model unavailable");
        std::error_code error; const bool sentinelMatches = fs::equivalent(initialCells.front().first, evidence.sentinel, error);
        RequireObservation(!error && sentinelMatches && source.starts == 0 && source.ends == 0 &&
            source.fileReads == 0 && !source.wrongApartment && !source.fileDropObservationFailed,
            "leave initial sentinel or source state unavailable");
        evidence.initialItemCount = initialCells.size(); evidence.initialRevision = Access::LayoutRevision(*profile.app);
        auto disk = CaptureV4LeaveLayoutSnapshot(root, verified_, familyIdentity_, 4u * 1024u * 1024u);
        RequireObservation(disk.captured && !disk.resourceUnavailable && disk.candidateId == raw.id && disk.fileId,
            "leave initial layout snapshot unavailable");
        evidence.initialLayout = disk.rawBytes; evidence.initialLayoutSnapshot = std::move(disk); evidence.inputObserved = true;
    }
    void BeforeOleCall(const drag_global::ole_driver::Input& input) const
    {
        if (input.kind == drag_global::ole_driver::Kind::leave)
            RequireObservation(collectionTarget_ ? Access::Target(*profile.app) == Access::MemberContainer(*profile.app, L"ingress-root") :
                (Access::Target(*profile.app) == Access::Desktop(*profile.app) && Access::Region(*profile.app) == HitRegion::Empty),
                "leave actual target unavailable");
    }
    void Await(const drag_global::ole_driver::Observation& terminal)
    {
        using namespace drag_global::ole_driver;
        RequireObservation(!awaitStarted_, "leave completion already requested"); awaitStarted_ = true;
        RequireObservation(terminal.kind == Kind::leave && SUCCEEDED(terminal.result), "leave terminal observation unavailable");
        Wait([&] { return Access::ExternalLeaveIdle(*profile.app) && Access::ContainerWorkIdle(*profile.app) && Access::FileDropIdle(*profile.app) &&
            Access::LayoutSaveIdle(*profile.app); });
        evidence.leaveAwaitReturned = true; awaitCompleted_ = true;
    }
    void BindExecution(const std::shared_ptr<drag_global::ole_driver::V4LeaveExecutionKey>& key,
        const std::shared_ptr<drag_global::V4IndependentShellOwner>& sourceOwner,
        const std::shared_ptr<const drag_global::V4PinnedCopyLease>& lease,
        const std::shared_ptr<const drag_global::ole_driver::V3HostEpoch>& epoch,
        const drag_global::V4FamilyValidatedContext& verified,
        const drag_global::event_graph::V4FamilyIdentityContext& identity)
    {
        RequireObservation(key && !awaitStarted_ && evidence.inputObserved && !evidence.baselineRevalidated &&
            root.StillOwned() && root.profile.get() == &profile && profile.app && payload && sourceOwner &&
            owner == sourceOwner && !owner.owner_before(sourceOwner) && !sourceOwner.owner_before(owner) &&
            lease && owner->MatchesPinnedCopyLease(verified, identity, payload, lease) &&
            Access::asyncOwner == profile.app && Access::v4IndependentShellOwner == owner &&
            source.starts >= 0 && source.ends == source.starts && source.fileReads >= 0 && !source.wrongApartment &&
            !source.fileDropObservationFailed && !source.formatProbeObservationFailed &&
            source.fileDropReads.size() == static_cast<std::size_t>(source.fileReads) &&
            Access::FileDropIdle(*profile.app) && evidence.initialRevision &&
            Access::LayoutRevision(*profile.app) == *evidence.initialRevision && Outputs(profile).empty() && CollectionBaselineMatches() &&
            sourceFile->PathStillNamesObject(evidence.source) && sourceFile->HasBytes(evidence.expectedBytes) &&
            sentinelFile->PathStillNamesObject(evidence.sentinel) && sentinelFile->HasBytes(SentinelBytes()),
            "leave same-run baseline unavailable");
        evidence.preLeaveStarts = source.starts; evidence.preLeaveEnds = source.ends; evidence.preLeaveFileReads = source.fileReads;
        ole_file_source_fixture::FileDropCallState beforeCalls;
        RequireObservation(source.ReadFileDropCallState(beforeCalls) && !beforeCalls.failed && beforeCalls.depth == 0 &&
            beforeCalls.owner == GetCurrentThreadId(), "leave source call baseline unavailable");
        evidence.preLeaveCallRecordCount = beforeCalls.count;
        auto disk = CaptureV4LeaveLayoutSnapshot(root, verified_, familyIdentity_, 4u * 1024u * 1024u);
        evidence.beforeLeaveLayoutSnapshot = std::move(disk);
        const auto& beforeLeave = *evidence.beforeLeaveLayoutSnapshot;
        RequireObservation(beforeLeave.captured && !beforeLeave.resourceUnavailable && beforeLeave.candidateId == evidence.candidateId && beforeLeave.fileId &&
            evidence.initialLayoutSnapshot && evidence.initialLayoutSnapshot->fileId &&
            OwnedProfileDesktopRoot::SameId(*beforeLeave.fileId, *evidence.initialLayoutSnapshot->fileId) &&
            beforeLeave.rawBytes == evidence.initialLayout, "leave pre-terminal layout snapshot changed or unavailable");
        ole_file_source_fixture::FileDropCallState afterCalls;
        RequireObservation(source.ReadFileDropCallState(afterCalls) && !afterCalls.failed && afterCalls.depth == 0 &&
            afterCalls.owner == beforeCalls.owner && afterCalls.count == beforeCalls.count &&
            source.starts == *evidence.preLeaveStarts && source.ends == *evidence.preLeaveEnds &&
            source.fileReads == *evidence.preLeaveFileReads && !source.wrongApartment &&
            !source.fileDropObservationFailed && !source.formatProbeObservationFailed && Access::FileDropIdle(*profile.app),
            "leave source changed during baseline observation");
        RequireObservation(key->BindBaseline(sourceOwner, lease, epoch, profile.app, static_cast<IDataObject*>(&source),
            evidence.candidateId, identity_), "leave same-run key unavailable");
        evidence.baselineRevalidated = true;
    }
    void CaptureExecution(const std::shared_ptr<drag_global::ole_driver::V4LeaveExecutionKey>& key,
        const std::shared_ptr<drag_global::V4IndependentShellOwner>& sourceOwner,
        const std::shared_ptr<const drag_global::V4PinnedCopyLease>& lease,
        const std::shared_ptr<const drag_global::ole_driver::V3HostEpoch>& epoch)
    {
        RequireObservation(key && evidence.baselineRevalidated && awaitCompleted_ && payload &&
            key->BeginCapture(sourceOwner, lease, epoch, profile.app, static_cast<IDataObject*>(&source), evidence.candidateId, identity_),
            "leave same-run capture unavailable");
        try
        {
            evidence.starts = source.starts; evidence.ends = source.ends; evidence.fileReads = source.fileReads;
            evidence.sourceLogAvailable = !source.wrongApartment && !source.fileDropObservationFailed &&
                !source.formatProbeObservationFailed && source.fileReads >= 0 &&
                source.fileDropReads.size() == static_cast<std::size_t>(source.fileReads);
            evidence.outputCount = Outputs(profile).size(); evidence.finalItemCount = Access::Items(*profile.app).size();
            evidence.finalRevision = Access::LayoutRevision(*profile.app);
            auto disk = CaptureV4LeaveLayoutSnapshot(root, verified_, familyIdentity_, 4u * 1024u * 1024u);
            evidence.finalLayoutSnapshot = std::move(disk);
            const auto& finalDisk = *evidence.finalLayoutSnapshot;
            RequireObservation(finalDisk.captured && !finalDisk.resourceUnavailable && finalDisk.candidateId == evidence.candidateId && finalDisk.fileId &&
                evidence.initialLayoutSnapshot && evidence.initialLayoutSnapshot->fileId, "leave final layout snapshot unavailable");
            evidence.finalLayout = finalDisk.rawBytes; evidence.layoutBytesUnchanged = evidence.initialLayout == evidence.finalLayout;
            evidence.layoutFileIdentityUnchanged = OwnedProfileDesktopRoot::SameId(*finalDisk.fileId, *evidence.initialLayoutSnapshot->fileId);
            evidence.sourcePreserved = sourceFile->PathStillNamesObject(evidence.source) && sourceFile->HasBytes(evidence.expectedBytes);
            evidence.sentinelPreserved = sentinelFile->PathStillNamesObject(evidence.sentinel) && sentinelFile->HasBytes(SentinelBytes());
            bool cells = evidence.finalItemCount == evidence.initialItemCount;
            for (const auto& [path, cell] : initialCells) cells = cells && Same(FindCopyBusiness(*profile.app, path).gridCell, cell);
            evidence.initialCellsPreserved = cells; evidence.finalObserved = true;
            if (collectionTarget_)
            {
                const auto& widget = ContainerWidget(*profile.app, L"ingress-root");
                RequireObservation(initialCollection_.has_value(), "leave collection baseline unavailable");
                evidence.collectionWidgetPreserved = widget.type == initialCollection_->type && widget.id == initialCollection_->id &&
                    widget.title == initialCollection_->title && widget.customTitle == initialCollection_->customTitle &&
                    widget.autoCollect == initialCollection_->autoCollect && widget.singleCellIconMode == initialCollection_->singleCellIconMode &&
                    widget.gridSpan.columns == initialCollection_->gridSpan.columns && widget.gridSpan.rows == initialCollection_->gridSpan.rows &&
                    Same(widget.gridCell, initialCollection_->gridCell) && widget.childWidgetIds == initialCollection_->childWidgetIds &&
                    widget.activeCategoryId == initialCollection_->activeCategoryId;
                evidence.finalCollectionMembers = Access::ContainerMembers(*profile.app, L"ingress-root");
                std::error_code error;
                const bool same = evidence.finalCollectionMembers.size() == 1 &&
                    fs::equivalent(evidence.finalCollectionMembers.front(), evidence.sentinel, error);
                RequireObservation(!error, "leave collection final identity unavailable");
                evidence.collectionMembersPreserved = same;
                RefreshContainer(profile, true, L"ingress-root", false);
                Wait([&] { return Access::ContainerWorkIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
                evidence.reloadedCollectionMembers = Access::ContainerMembers(*profile.app, L"ingress-root");
                error.clear();
                const bool reloaded = evidence.reloadedCollectionMembers.size() == 1 &&
                    fs::equivalent(evidence.reloadedCollectionMembers.front(), evidence.sentinel, error);
                RequireObservation(!error, "leave collection reloaded identity unavailable");
                evidence.reloadedCollectionMembersPreserved = reloaded;
                evidence.reloadedRevision = Access::LayoutRevision(*profile.app);
            }
        }
        catch (const std::exception& error) { evidence.unresolved.push_back(std::string("leave_business_snapshot_incomplete:") + error.what()); }
        catch (...) { evidence.unresolved.emplace_back("leave_business_snapshot_incomplete"); }
        auto snapshot = std::make_shared<const V4FileLeaveEvidence>(evidence);
        RequireObservation(key->Publish(sourceOwner, lease, epoch, profile.app, static_cast<IDataObject*>(&source),
            evidence.candidateId, identity_, std::move(snapshot)), "leave same-run sealing unavailable");
    }
};

// The rejected Drop has a separate identity and sealed observation.
struct V4FileRejectEvidence
{
    std::string candidateId;
    fs::path source, sentinel;
    std::vector<std::byte> expectedBytes;
    std::optional<FILE_ID_INFO> sourceId, sentinelId;
    std::string initialLayout, finalLayout;
    std::optional<V4LeaveLayoutSnapshot> initialLayoutSnapshot, beforeDropLayoutSnapshot, finalLayoutSnapshot;
    std::optional<std::uint64_t> initialRevision, finalRevision;
    std::optional<std::size_t> initialItemCount, finalItemCount, outputCount;
    std::optional<int> preDropStarts, preDropEnds, preDropFileReads;
    std::optional<std::size_t> preDropCallRecordCount;
    std::optional<int> starts, ends, fileReads;
    std::optional<bool> sourcePreserved, sentinelPreserved, initialCellsPreserved, layoutBytesUnchanged,
        layoutFileIdentityUnchanged, sourceLogAvailable;
    bool inputObserved = false, baselineRevalidated = false, rejectAwaitReturned = false, finalObserved = false;
    std::vector<std::string> unresolved{"reject_absence_requires_complete_bounded_event_interval",
        "reject_background_quiescence_and_unobserved_reentry_unverified", "reject_full_graph_and_business_unimplemented"};
};

struct V4FileRejectObservation
{
private:
    OwnedProfileDesktopRoot& root;
    Profile& profile;
    std::shared_ptr<drag_global::V4IndependentFilePayload> payload;
    std::shared_ptr<drag_global::V4IndependentShellOwner> owner;
    ole_file_source_fixture::Source& source;
    const drag_global::V4FamilyValidatedContext& verified_;
    const drag_global::event_graph::V4FamilyIdentityContext& familyIdentity_;
    V4FileRejectEvidence evidence;
    std::unique_ptr<ObservedPhysicalFile> sourceFile, sentinelFile;
    std::vector<std::pair<fs::path, GridCell>> initialCells;
    bool awaitStarted_ = false, awaitCompleted_ = false;
    std::shared_ptr<const drag_global::ole_driver::V4RejectObserverIdentity> identity_{
        new drag_global::ole_driver::V4RejectObserverIdentity};
    static ole_file_source_fixture::Source& BoundSource(const drag_global::V4IndependentShellSource& binding,
        const std::shared_ptr<drag_global::V4IndependentShellOwner>& owner)
    {
        auto* actual = binding.SourceObject();
        RequireObservation(actual && binding.inputPrepared && !binding.resourceUnavailable && binding.SameOwner(owner),
            "reject Shell source unavailable"); return *actual;
    }
    static Profile& BoundProfile(OwnedProfileDesktopRoot& ownedRoot)
    {
        RequireObservation(ownedRoot.profile && ownedRoot.profile->app && ownedRoot.StillOwned(), "reject profile unavailable");
        return *ownedRoot.profile;
    }
    static std::vector<std::byte> SentinelBytes()
    { return {std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'}, std::byte{'u'},
        std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}}; }
public:
    V4FileRejectObservation(const V4FileRejectObservation&) = delete;
    V4FileRejectObservation& operator=(const V4FileRejectObservation&) = delete;
    V4FileRejectEvidence Evidence() const { return evidence; }
    V4FileRejectObservation(OwnedProfileDesktopRoot& ownedRoot,
        const std::shared_ptr<drag_global::V4IndependentFilePayload>& input,
        const drag_global::V4IndependentShellSource& binding,
        const std::shared_ptr<drag_global::V4IndependentShellOwner>& sourceOwner,
        const drag_global::V4FamilyValidatedContext& verified,
        const drag_global::event_graph::V4FamilyIdentityContext& identity)
        : root(ownedRoot), profile(BoundProfile(ownedRoot)), payload(input), owner(sourceOwner), source(BoundSource(binding, sourceOwner)),
          verified_(verified), familyIdentity_(identity)
    {
        using namespace drag_global;
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        RequireObservation(root.StillOwned() && root.profile && root.profile->app &&
            verified.Status() == V4CoordinateStatus::coordinate_ready && identity.Status() == V4FamilyStatus::schema_only &&
            verified.FamilyBinding() == identity.FamilyBinding() && !raw.resourceUnavailable && raw.errors.empty() &&
            !raw.id.empty() && payload && payload->files.size() == 1 && payload->candidateId == raw.id &&
            binding.candidateId == raw.id && binding.SamePayload(payload) && Access::v4IndependentShellOwner == owner &&
            Access::asyncOwner == profile.app && Access::productionDispatch == 1 && Access::fixtureControlWindow &&
            Access::messageTrace && !Access::messageTrace->LostOrCrossThread() && source.asynchronous &&
            source.owner == GetCurrentThreadId() && value("V4.source.kind") == "file_only" &&
            value("V4.source.file_count") == "one" && value("V4.file.member0_kind") == "file" &&
            value("V4.file.member0_identity") == "unique" && value("T01") == "external_ingress" &&
            value("G04.termination") == "drop" && value("T03") == "none" && value("T03.extra_protocol_bits") == "none" && value("target.E01") == "free_desktop" &&
            value("target.E02") == "none" && value("H01.region") == "empty_slot", "reject candidate or host unavailable");
        const auto snapshot = ole_driver::InspectV4IndependentFileInputs(verified, identity, *payload, source);
        RequireObservation(snapshot.captured && !snapshot.resourceUnavailable && snapshot.candidateId == raw.id &&
            snapshot.files.size() == 1, "reject source input unavailable");
        evidence.candidateId = raw.id; evidence.source = snapshot.files.front().path;
        evidence.expectedBytes = snapshot.files.front().bytes; evidence.sentinel = profile.desktop / L"Sentinel.txt";
        sourceFile = std::make_unique<ObservedPhysicalFile>(evidence.source);
        sentinelFile = std::make_unique<ObservedPhysicalFile>(evidence.sentinel);
        RequireObservation(OwnedProfileDesktopRoot::SameId(sourceFile->Identity(), snapshot.files.front().id) &&
            sourceFile->PathStillNamesObject(evidence.source) && sourceFile->HasBytes(evidence.expectedBytes) &&
            sentinelFile->PathStillNamesObject(evidence.sentinel) && sentinelFile->HasBytes(SentinelBytes()),
            "reject source or sentinel baseline unavailable");
        evidence.sourceId = sourceFile->Identity(); evidence.sentinelId = sentinelFile->Identity();
        RequireObservation(Outputs(profile).empty(), "reject initial desktop outputs unavailable");
        for (const auto& item : Access::Items(*profile.app)) initialCells.emplace_back(fs::path(item.parsingName), item.gridCell);
        RequireObservation(initialCells.size() == 1, "reject initial desktop model unavailable");
        std::error_code error; const bool sentinelMatches = fs::equivalent(initialCells.front().first, evidence.sentinel, error);
        RequireObservation(!error && sentinelMatches && source.starts == 0 && source.ends == 0 &&
            source.fileReads == 0 && !source.wrongApartment && !source.fileDropObservationFailed,
            "reject initial sentinel or source state unavailable");
        evidence.initialItemCount = initialCells.size(); evidence.initialRevision = Access::LayoutRevision(*profile.app);
        auto disk = CaptureV4LeaveLayoutSnapshot(root, verified_, familyIdentity_, 4u * 1024u * 1024u);
        RequireObservation(disk.captured && !disk.resourceUnavailable && disk.candidateId == raw.id && disk.fileId,
            "reject initial layout snapshot unavailable");
        evidence.initialLayout = disk.rawBytes; evidence.initialLayoutSnapshot = std::move(disk); evidence.inputObserved = true;
    }
    void BeforeOleCall(const drag_global::ole_driver::Input& input) const
    {
        if (input.kind == drag_global::ole_driver::Kind::drop)
            RequireObservation(Access::Target(*profile.app) == Access::Desktop(*profile.app) &&
                Access::Region(*profile.app) == HitRegion::Empty, "reject actual target unavailable");
    }
    void Await(const drag_global::ole_driver::Observation& terminal)
    {
        using namespace drag_global::ole_driver;
        RequireObservation(!awaitStarted_, "reject completion already requested"); awaitStarted_ = true;
        RequireObservation(terminal.kind == Kind::drop && SUCCEEDED(terminal.result) && terminal.returnedEffect && *terminal.returnedEffect == DROPEFFECT_NONE, "reject terminal observation unavailable");
        Wait([&] { return Access::ExternalLeaveIdle(*profile.app) && Access::FileDropIdle(*profile.app) &&
            Access::LayoutSaveIdle(*profile.app); });
        evidence.rejectAwaitReturned = true; awaitCompleted_ = true;
    }
    void BindExecution(const std::shared_ptr<drag_global::ole_driver::V4RejectExecutionKey>& key,
        const std::shared_ptr<drag_global::V4IndependentShellOwner>& sourceOwner,
        const std::shared_ptr<const drag_global::V4PinnedCopyLease>& lease,
        const std::shared_ptr<const drag_global::ole_driver::V3HostEpoch>& epoch,
        const drag_global::V4FamilyValidatedContext& verified,
        const drag_global::event_graph::V4FamilyIdentityContext& identity)
    {
        RequireObservation(key && !awaitStarted_ && evidence.inputObserved && !evidence.baselineRevalidated &&
            root.StillOwned() && root.profile.get() == &profile && profile.app && payload && sourceOwner &&
            owner == sourceOwner && !owner.owner_before(sourceOwner) && !sourceOwner.owner_before(owner) &&
            lease && owner->MatchesPinnedCopyLease(verified, identity, payload, lease) &&
            Access::asyncOwner == profile.app && Access::v4IndependentShellOwner == owner &&
            source.starts >= 0 && source.ends == source.starts && source.fileReads >= 0 && !source.wrongApartment &&
            !source.fileDropObservationFailed && !source.formatProbeObservationFailed &&
            source.fileDropReads.size() == static_cast<std::size_t>(source.fileReads) &&
            Access::FileDropIdle(*profile.app) && evidence.initialRevision &&
            Access::LayoutRevision(*profile.app) == *evidence.initialRevision && Outputs(profile).empty() &&
            sourceFile->PathStillNamesObject(evidence.source) && sourceFile->HasBytes(evidence.expectedBytes) &&
            sentinelFile->PathStillNamesObject(evidence.sentinel) && sentinelFile->HasBytes(SentinelBytes()),
            "reject same-run baseline unavailable");
        evidence.preDropStarts = source.starts; evidence.preDropEnds = source.ends;
        evidence.preDropFileReads = source.fileReads;
        ole_file_source_fixture::FileDropCallState beforeCalls;
        RequireObservation(source.ReadFileDropCallState(beforeCalls) && !beforeCalls.failed && beforeCalls.depth == 0 &&
            beforeCalls.owner == GetCurrentThreadId(), "reject source call baseline unavailable");
        evidence.preDropCallRecordCount = beforeCalls.count;
        auto disk = CaptureV4LeaveLayoutSnapshot(root, verified_, familyIdentity_, 4u * 1024u * 1024u);
        evidence.beforeDropLayoutSnapshot = std::move(disk);
        const auto& beforeDrop = *evidence.beforeDropLayoutSnapshot;
        RequireObservation(beforeDrop.captured && !beforeDrop.resourceUnavailable && beforeDrop.candidateId == evidence.candidateId && beforeDrop.fileId &&
            evidence.initialLayoutSnapshot && evidence.initialLayoutSnapshot->fileId &&
            OwnedProfileDesktopRoot::SameId(*beforeDrop.fileId, *evidence.initialLayoutSnapshot->fileId) &&
            beforeDrop.rawBytes == evidence.initialLayout, "reject pre-terminal layout snapshot changed or unavailable");
        ole_file_source_fixture::FileDropCallState afterCalls;
        RequireObservation(source.ReadFileDropCallState(afterCalls) && !afterCalls.failed && afterCalls.depth == 0 &&
            afterCalls.owner == beforeCalls.owner && afterCalls.count == beforeCalls.count &&
            source.starts == *evidence.preDropStarts && source.ends == *evidence.preDropEnds &&
            source.fileReads == *evidence.preDropFileReads && !source.wrongApartment &&
            !source.fileDropObservationFailed && !source.formatProbeObservationFailed && Access::FileDropIdle(*profile.app),
            "reject source changed during baseline observation");
        RequireObservation(key->BindBaseline(sourceOwner, lease, epoch, profile.app, static_cast<IDataObject*>(&source),
            evidence.candidateId, identity_), "reject same-run key unavailable");
        evidence.baselineRevalidated = true;
    }
    void CaptureExecution(const std::shared_ptr<drag_global::ole_driver::V4RejectExecutionKey>& key,
        const std::shared_ptr<drag_global::V4IndependentShellOwner>& sourceOwner,
        const std::shared_ptr<const drag_global::V4PinnedCopyLease>& lease,
        const std::shared_ptr<const drag_global::ole_driver::V3HostEpoch>& epoch)
    {
        RequireObservation(key && evidence.baselineRevalidated && awaitCompleted_ && payload &&
            key->BeginCapture(sourceOwner, lease, epoch, profile.app, static_cast<IDataObject*>(&source), evidence.candidateId, identity_),
            "reject same-run capture unavailable");
        try
        {
            evidence.starts = source.starts; evidence.ends = source.ends; evidence.fileReads = source.fileReads;
            evidence.sourceLogAvailable = !source.wrongApartment && !source.fileDropObservationFailed &&
                !source.formatProbeObservationFailed && source.fileReads >= 0 &&
                source.fileDropReads.size() == static_cast<std::size_t>(source.fileReads);
            evidence.outputCount = Outputs(profile).size(); evidence.finalItemCount = Access::Items(*profile.app).size();
            evidence.finalRevision = Access::LayoutRevision(*profile.app);
            auto disk = CaptureV4LeaveLayoutSnapshot(root, verified_, familyIdentity_, 4u * 1024u * 1024u);
            evidence.finalLayoutSnapshot = std::move(disk);
            const auto& finalDisk = *evidence.finalLayoutSnapshot;
            RequireObservation(finalDisk.captured && !finalDisk.resourceUnavailable && finalDisk.candidateId == evidence.candidateId && finalDisk.fileId &&
                evidence.initialLayoutSnapshot && evidence.initialLayoutSnapshot->fileId, "reject final layout snapshot unavailable");
            evidence.finalLayout = finalDisk.rawBytes; evidence.layoutBytesUnchanged = evidence.initialLayout == evidence.finalLayout;
            evidence.layoutFileIdentityUnchanged = OwnedProfileDesktopRoot::SameId(*finalDisk.fileId, *evidence.initialLayoutSnapshot->fileId);
            evidence.sourcePreserved = sourceFile->PathStillNamesObject(evidence.source) && sourceFile->HasBytes(evidence.expectedBytes);
            evidence.sentinelPreserved = sentinelFile->PathStillNamesObject(evidence.sentinel) && sentinelFile->HasBytes(SentinelBytes());
            bool cells = evidence.finalItemCount == evidence.initialItemCount;
            for (const auto& [path, cell] : initialCells) cells = cells && Same(FindCopyBusiness(*profile.app, path).gridCell, cell);
            evidence.initialCellsPreserved = cells; evidence.finalObserved = true;
        }
        catch (const std::exception& error) { evidence.unresolved.push_back(std::string("reject_business_snapshot_incomplete:") + error.what()); }
        catch (...) { evidence.unresolved.emplace_back("reject_business_snapshot_incomplete"); }
        auto snapshot = std::make_shared<const V4FileRejectEvidence>(evidence);
        RequireObservation(key->Publish(sourceOwner, lease, epoch, profile.app, static_cast<IDataObject*>(&source),
            evidence.candidateId, identity_, std::move(snapshot)), "reject same-run sealing unavailable");
    }
};
}

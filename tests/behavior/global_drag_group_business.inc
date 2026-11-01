#pragma once

// Isolated initial state and read-only postflight for a real CollectionGroup
// or FileGroup ingress. The observer never copies a file or changes membership after Drop.
namespace behavior
{
struct V4GroupCopyEvidence
{
    std::string candidateId;
    fs::path source, output, existing, mappedDirectory;
    std::vector<std::byte> expectedBytes;
    std::optional<FILE_ID_INFO> sourceId, outputId, existingId, mappedDirectoryId, sentinelId;
    std::vector<fs::path> initialMembers, finalMembers, reloadedMembers;
    std::vector<fs::path> initialInactiveMembers, finalInactiveMembers, reloadedInactiveMembers;
    std::optional<drag_global::ole_driver::CompletionEvidence> completion;
    std::optional<std::uint64_t> initialRevision, finalRevision, reloadedRevision;
    std::optional<bool> sourcePreserved, existingPreserved, outputBytesMatch, membershipMatches,
        reloadedMembershipMatches, rootPreserved, activePreserved, inactivePreserved,
        reloadedRootPreserved, reloadedActivePreserved, reloadedInactivePreserved, noDesktopDuplicate,
        mappedDirectoryPreserved, sentinelPreserved, mappedParentVerifiedBeforeDrop, mappedDistinctFromSourceAndDesktop;
    std::optional<bool> inactiveMembershipMatches, reloadedInactiveMembershipMatches,
        activeMembersUnchanged, reloadedActiveMembersUnchanged, inactiveActivated, reloadedInactiveActivated,
        rootStateExcludingActivationMatches, reloadedRootStateExcludingActivationMatches,
        inactiveSettingsPreserved, reloadedInactiveSettingsPreserved;
    bool targetInactive = false;
    bool fileGroup = false, mapped = false, inputObserved = false, baselineRevalidated = false, finalObserved = false, reloadObserved = false;
    std::vector<std::string> unresolved{"group_candidate_axes_and_event_graph_unimplemented",
        "group_source_read_commit_completion_causality_unverified", "group_snapshots_non_atomic",
        "group_mapped_parent_continuity_and_ancestor_aliases_unverified"};
};

struct V4GroupCopyObservation
{
private:
    Profile& profile;
    std::shared_ptr<drag_global::V4IndependentFilePayload> payload;
    std::shared_ptr<drag_global::V4IndependentShellOwner> owner;
    ole_file_source_fixture::Source& source;
    V4GroupCopyEvidence evidence;
    DesktopWidget initialRoot, initialActive, initialInactive;
    std::unique_ptr<ObservedPhysicalFile> sourceFile, existingFile, outputFile, sentinelFile;
    struct DirectoryGuard { HANDLE handle = INVALID_HANDLE_VALUE; ~DirectoryGuard() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); } } mappedDirectory_;
    std::size_t traceStart = 0;
    drag_global::ole_driver::CompletionHostState initialCompletion;
    bool fileGroup_ = false, mapped_ = false, targetInactive_ = false, awaitStarted_ = false, awaitCompleted_ = false;
    std::shared_ptr<const drag_global::ole_driver::V4GroupObserverIdentity> identity_{
        new drag_global::ole_driver::V4GroupObserverIdentity};

    static ole_file_source_fixture::Source& BoundSource(const drag_global::V4IndependentShellSource& binding,
        const std::shared_ptr<drag_global::V4IndependentShellOwner>& owner)
    {
        auto* actual = binding.SourceObject();
        RequireObservation(actual && binding.inputPrepared && !binding.resourceUnavailable && binding.SameOwner(owner),
            "group Shell source unavailable");
        return *actual;
    }
    static bool SameMembers(const std::vector<fs::path>& actual, const std::vector<fs::path>& expected)
    {
        if (actual.size() != expected.size()) return false;
        for (const auto& path : expected)
        {
            std::size_t count = 0;
            for (const auto& item : actual)
            {
                std::error_code error; const bool same = fs::equivalent(item, path, error);
                RequireObservation(!error, "group member identity unavailable");
                if (same) ++count;
            }
            if (count != 1) return false;
        }
        return true;
    }
    static bool SameWidget(const DesktopWidget& actual, const DesktopWidget& initial)
    {
        return actual.type == initial.type && actual.title == initial.title && actual.customTitle == initial.customTitle &&
            actual.autoCollect == initial.autoCollect && actual.privacyMode == initial.privacyMode &&
            actual.singleCellIconMode == initial.singleCellIconMode && actual.gridSpan.columns == initial.gridSpan.columns &&
            actual.gridSpan.rows == initial.gridSpan.rows && Same(actual.gridCell, initial.gridCell) &&
            actual.childWidgetIds == initial.childWidgetIds && actual.activeCategoryId == initial.activeCategoryId &&
            actual.sourceFolderPath == initial.sourceFolderPath;
    }
    bool RootMatches() const { return SameWidget(ContainerWidget(*profile.app, L"ingress-root"), initialRoot); }
    bool RootMatchesIgnoringActivation() const
    {
        auto expected = initialRoot;
        expected.activeCategoryId = ContainerWidget(*profile.app, L"ingress-root").activeCategoryId;
        return SameWidget(ContainerWidget(*profile.app, L"ingress-root"), expected);
    }
    bool ActiveMatches() const { return SameWidget(ContainerWidget(*profile.app, L"ingress-active"), initialActive); }
    bool InactiveMatches() const
    {
        const auto& inactive = ContainerWidget(*profile.app, L"ingress-inactive");
        return SameWidget(inactive, initialInactive) && inactive.itemKeys == initialInactive.itemKeys;
    }
    static std::vector<std::byte> SentinelBytes()
    { return {std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'}, std::byte{'u'},
        std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}}; }
    std::vector<std::byte> ExistingBytes() const
    { return mapped_ ? std::vector<std::byte>{std::byte{'e'}, std::byte{'x'}, std::byte{'i'}, std::byte{'s'},
        std::byte{'t'}, std::byte{'i'}, std::byte{'n'}, std::byte{'g'}} : SentinelBytes(); }
    bool DirectoryStillBound() const
    {
        if (!mapped_) return true;
        if (!evidence.mappedDirectoryId || mappedDirectory_.handle == INVALID_HANDLE_VALUE) return false;
        if (std::none_of(std::begin(evidence.mappedDirectoryId->FileId.Identifier),
            std::end(evidence.mappedDirectoryId->FileId.Identifier), [](BYTE byte) { return byte != 0; })) return false;
        FILE_ID_INFO held{}; FILE_ATTRIBUTE_TAG_INFO tag{};
        if (!GetFileInformationByHandleEx(mappedDirectory_.handle, FileIdInfo, &held, sizeof(held)) ||
            !OwnedProfileDesktopRoot::SameId(held, *evidence.mappedDirectoryId) ||
            !GetFileInformationByHandleEx(mappedDirectory_.handle, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        struct Handle { HANDLE value; ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } } named{
            CreateFileW(evidence.mappedDirectory.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        FILE_ID_INFO actual{};
        return named.value != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(named.value, FileIdInfo, &actual, sizeof(actual)) &&
            OwnedProfileDesktopRoot::SameId(actual, held) &&
            GetFileInformationByHandleEx(named.value, FileAttributeTagInfo, &tag, sizeof(tag)) &&
            (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    }
    bool ParentMatchesOwnedRoot(const OwnedProfileDesktopRoot& root) const
    {
        if (!mapped_) return true;
        if (!root.StillOwned() || !DirectoryStillBound() || !evidence.mappedDirectoryId || !payload) return false;
        struct Handle { HANDLE value; ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } } parent{
            CreateFileW(evidence.mappedDirectory.parent_path().c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        FILE_ID_INFO id{}; FILE_ATTRIBUTE_TAG_INFO tag{};
        return parent.value != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(parent.value, FileIdInfo, &id, sizeof(id)) &&
            OwnedProfileDesktopRoot::SameId(id, root.rootId) &&
            GetFileInformationByHandleEx(parent.value, FileAttributeTagInfo, &tag, sizeof(tag)) &&
            (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            !OwnedProfileDesktopRoot::SameId(*evidence.mappedDirectoryId, root.rootId) &&
            !OwnedProfileDesktopRoot::SameId(*evidence.mappedDirectoryId, root.desktopId) &&
            !OwnedProfileDesktopRoot::SameId(*evidence.mappedDirectoryId, payload->rootId) && root.StillOwned();
    }
public:
    V4GroupCopyObservation(const V4GroupCopyObservation&) = delete;
    V4GroupCopyObservation& operator=(const V4GroupCopyObservation&) = delete;
    V4GroupCopyEvidence Evidence() const { return evidence; }
    V4GroupCopyObservation(Profile& host, const std::shared_ptr<drag_global::V4IndependentFilePayload>& input,
        const drag_global::V4IndependentShellSource& binding,
        const std::shared_ptr<drag_global::V4IndependentShellOwner>& sourceOwner,
        const drag_global::V4FamilyValidatedContext& verified,
        const drag_global::event_graph::V4FamilyIdentityContext& identity)
        : profile(host), payload(input), owner(sourceOwner), source(BoundSource(binding, sourceOwner))
    {
        using namespace drag_global;
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        const bool collectionGroup = value("target.E02") == "collection_group" &&
            value("target.E08.active_child_type") == "collection";
        fileGroup_ = value("target.E02") == "file_group" &&
            (value("target.E08.active_child_type") == "desktop_files" || value("target.E08.active_child_type") == "folder_mapping");
        mapped_ = fileGroup_ && value("target.E08.active_child_type") == "folder_mapping";
        targetInactive_ = collectionGroup && value("target.E08.hit_relation") == "inactive_valid" &&
            value("target.E08.hit_position") == "last_child";
        RequireObservation(verified.Status() == V4CoordinateStatus::coordinate_ready &&
            identity.Status() == V4FamilyStatus::schema_only && !verified.TaskBActive() &&
            verified.FamilyBinding() == identity.FamilyBinding() && !raw.resourceUnavailable && raw.errors.empty() &&
            !raw.id.empty() && payload && payload->files.size() == 1 && payload->candidateId == raw.id &&
            binding.candidateId == raw.id && binding.SamePayload(payload) && Access::v4IndependentShellOwner == owner &&
            Access::asyncOwner == profile.app && Access::productionDispatch == 1 && Access::fixtureControlWindow &&
            Access::messageTrace && !Access::messageTrace->LostOrCrossThread() && source.asynchronous && source.owner == GetCurrentThreadId() &&
            value("V4.source.kind") == "file_only" && value("V4.source.file_count") == "one" &&
            value("V4.file.member0_kind") == "file" && value("V4.file.member0_identity") == "unique" &&
            value("target.E01") == "free_desktop" && (collectionGroup || fileGroup_) &&
            value("target.E03") == (targetInactive_ ? "group_label" : "widget_body") && value("target.E08") == "first_child" &&
            ((value("target.E08.hit_relation") == "active" && value("target.E08.hit_position") == "first_child") ||
                targetInactive_), "group candidate/source unavailable");
        const auto inputObservation = ole_driver::InspectV4IndependentFileInputs(verified, identity, *payload, source);
        RequireObservation(inputObservation.captured && !inputObservation.resourceUnavailable && inputObservation.candidateId == raw.id,
            "group source input unavailable");
        const auto& file = payload->files.front();
        RequireObservation(file.created && file.identityObserved && file.bytesObserved && file.copyObservationHandle &&
            source.HasConstructionShell() && source.ConstructionPaths().size() == 1, "group source lease unavailable");
        std::error_code error; const bool same = fs::equivalent(source.ConstructionPaths().front(), file.path, error);
        RequireObservation(!error && same, "group Shell path unavailable");
        evidence.candidateId = raw.id; evidence.fileGroup = fileGroup_; evidence.mapped = mapped_; evidence.targetInactive = targetInactive_;
        evidence.source = file.path; evidence.expectedBytes = file.bytes;
        sourceFile = std::make_unique<ObservedPhysicalFile>(evidence.source);
        RequireObservation(OwnedProfileDesktopRoot::SameId(sourceFile->Identity(), file.identity) &&
            sourceFile->PathStillNamesObject(evidence.source) && sourceFile->HasBytes(evidence.expectedBytes),
            "group source identity unavailable");
        evidence.sourceId = sourceFile->Identity();
        const ContainerRow row{"v4_group", fileGroup_ ? DesktopWidgetType::FileGroup : DesktopWidgetType::CollectionGroup,
            mapped_ ? DesktopWidgetType::FolderMapping : fileGroup_ ? DesktopWidgetType::FileCategories : DesktopWidgetType::Collection, true, mapped_};
        SeedDestination(profile, row, evidence.existing);
        Wait([&] { return Access::ContainerWorkIdle(*profile.app) && Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
        initialRoot = ContainerWidget(*profile.app, L"ingress-root");
        initialActive = ContainerWidget(*profile.app, L"ingress-active");
        initialInactive = ContainerWidget(*profile.app, L"ingress-inactive");
        RequireObservation(initialRoot.type == (fileGroup_ ? DesktopWidgetType::FileGroup : DesktopWidgetType::CollectionGroup) &&
            initialActive.type == (mapped_ ? DesktopWidgetType::FolderMapping : fileGroup_ ? DesktopWidgetType::FileCategories : DesktopWidgetType::Collection) &&
            initialRoot.childWidgetIds.size() == 2 && initialRoot.childWidgetIds[0] == L"ingress-active" &&
            initialRoot.childWidgetIds[1] == L"ingress-inactive" && initialRoot.activeCategoryId == L"ingress-active" &&
            initialInactive.itemKeys.empty(), "group initial child selection unavailable");
        evidence.initialMembers = Access::ContainerMembers(*profile.app, L"ingress-active");
        RequireObservation(SameMembers(evidence.initialMembers, {evidence.existing}), "group initial members unavailable");
        if (targetInactive_)
        {
            RequireObservation(initialInactive.type == DesktopWidgetType::Collection, "inactive collection unavailable");
            evidence.initialInactiveMembers = Access::ContainerMembers(*profile.app, L"ingress-inactive");
            RequireObservation(evidence.initialInactiveMembers.empty(), "inactive collection baseline unavailable");
            evidence.unresolved.emplace_back("inactive_group_hit_activation_and_membership_same_run_unverified");
        }
        if (mapped_)
        {
            evidence.mappedDirectory = evidence.existing.parent_path();
            mappedDirectory_.handle = CreateFileW(evidence.mappedDirectory.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            FILE_ID_INFO id{}; FILE_ATTRIBUTE_TAG_INFO tag{};
            RequireObservation(mappedDirectory_.handle != INVALID_HANDLE_VALUE &&
                GetFileInformationByHandleEx(mappedDirectory_.handle, FileIdInfo, &id, sizeof(id)) &&
                std::any_of(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), [](BYTE byte) { return byte != 0; }) &&
                GetFileInformationByHandleEx(mappedDirectory_.handle, FileAttributeTagInfo, &tag, sizeof(tag)) &&
                (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT),
                "group mapped directory identity unavailable");
            evidence.mappedDirectoryId = id;
            RequireObservation(DirectoryStillBound() && fs::path(initialActive.sourceFolderPath) == evidence.mappedDirectory,
                "group mapped directory binding unavailable");
            sentinelFile = std::make_unique<ObservedPhysicalFile>(profile.desktop / L"Sentinel.txt");
            RequireObservation(sentinelFile->HasBytes(SentinelBytes()), "group desktop sentinel unavailable");
            evidence.sentinelId = sentinelFile->Identity();
        }
        evidence.output = (mapped_ ? evidence.mappedDirectory : profile.desktop) / file.path.filename();
        SetLastError(ERROR_SUCCESS);
        RequireObservation(GetFileAttributesW(evidence.output.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND &&
            evidence.output != evidence.source, "group output baseline unavailable");
        existingFile = std::make_unique<ObservedPhysicalFile>(evidence.existing);
        RequireObservation(existingFile->HasBytes(ExistingBytes()), "group existing member bytes unavailable");
        evidence.existingId = existingFile->Identity(); evidence.initialRevision = Access::LayoutRevision(*profile.app);
        RequireObservation(source.starts == 0 && source.ends == 0, "group source operation already active");
        traceStart = Access::messageTrace->Observations().size();
        initialCompletion = {true, static_cast<std::uint64_t>(source.starts), static_cast<std::uint64_t>(source.ends)};
        evidence.inputObserved = true;
    }
    void BeforeOleCall(const drag_global::ole_driver::Input& input) const
    {
        if (input.kind == drag_global::ole_driver::Kind::drop)
            RequireObservation(Access::Target(*profile.app) == Access::MemberContainer(*profile.app, L"ingress-root"),
                "group target unavailable at Drop");
    }
    void Await(const drag_global::ole_driver::Observation& terminal)
    {
        using namespace drag_global::ole_driver;
        RequireObservation(!awaitStarted_, "group completion already requested"); awaitStarted_ = true;
        RequireObservation(terminal.kind == Kind::drop && terminal.returnedEffect && SUCCEEDED(terminal.result) &&
            *terminal.returnedEffect == DROPEFFECT_COPY && Access::messageTrace && !Access::messageTrace->LostOrCrossThread(),
            "group copy Drop or trace unavailable");
        evidence.completion = WaitForCompletion(*Access::messageTrace, traceStart, Access::fixtureControlWindow,
            MessageRoute::control_window_proc, terminal, initialCompletion, kShellFileOperationCompletedMessage, 12000,
            [&] { return CompletionHostState{Access::FileDropIdle(*profile.app),
                static_cast<std::uint64_t>(source.starts), static_cast<std::uint64_t>(source.ends)}; },
            [] { Access::ThrowAsyncException(); });
        RequireObservation(evidence.completion->signal == CompletionSignal::completed_messages, "group completion unavailable");
        Wait([&] { return Access::ContainerWorkIdle(*profile.app) && Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
        awaitCompleted_ = true;
    }
private:
    void CaptureResults()
    {
        RequireObservation(awaitCompleted_ && evidence.completion, "group completion observation unavailable");
        evidence.finalMembers = Access::ContainerMembers(*profile.app, L"ingress-active");
        if (targetInactive_) evidence.finalInactiveMembers = Access::ContainerMembers(*profile.app, L"ingress-inactive");
        evidence.sourcePreserved = sourceFile->PathStillNamesObject(evidence.source) && sourceFile->HasBytes(evidence.expectedBytes);
        evidence.existingPreserved = existingFile->PathStillNamesObject(evidence.existing) && existingFile->HasBytes(ExistingBytes());
        if (mapped_)
        {
            evidence.sentinelPreserved = sentinelFile->PathStillNamesObject(profile.desktop / L"Sentinel.txt") && sentinelFile->HasBytes(SentinelBytes());
            RequireObservation(DirectoryStillBound(), "group mapped directory observation unavailable");
            evidence.mappedDirectoryPreserved = true;
        }
        evidence.rootPreserved = RootMatches(); evidence.activePreserved = ActiveMatches(); evidence.inactivePreserved = InactiveMatches();
        std::error_code error; const bool exists = fs::exists(evidence.output, error);
        RequireObservation(!error, "group output existence unavailable");
        if (exists)
        {
            outputFile = std::make_unique<ObservedPhysicalFile>(evidence.output); evidence.outputId = outputFile->Identity();
            evidence.outputBytesMatch = outputFile->HasBytes(evidence.expectedBytes) && !outputFile->SameObject(*sourceFile);
            if (targetInactive_)
            {
                evidence.activeMembersUnchanged = SameMembers(evidence.finalMembers, evidence.initialMembers);
                evidence.inactiveMembershipMatches = SameMembers(evidence.finalInactiveMembers, {evidence.output});
            }
            else evidence.membershipMatches = SameMembers(evidence.finalMembers, {evidence.existing, evidence.output});
            evidence.noDesktopDuplicate = Access::FreeDesktopCopies(*profile.app, evidence.output) == 0;
        }
        else { evidence.outputBytesMatch = false; if (targetInactive_) evidence.inactiveMembershipMatches = false; else evidence.membershipMatches = false; }
        if (targetInactive_)
        {
            evidence.inactiveActivated = ContainerWidget(*profile.app, L"ingress-root").activeCategoryId == L"ingress-inactive";
            evidence.rootStateExcludingActivationMatches = RootMatchesIgnoringActivation();
            evidence.inactiveSettingsPreserved = SameWidget(ContainerWidget(*profile.app, L"ingress-inactive"), initialInactive);
        }
        evidence.finalRevision = Access::LayoutRevision(*profile.app); evidence.finalObserved = true;
        RefreshContainer(profile, true, targetInactive_ ? L"ingress-inactive" : L"ingress-active", mapped_);
        Wait([&] { return Access::ContainerWorkIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
        RequireObservation(DirectoryStillBound(), "group mapped directory after reload unavailable");
        evidence.reloadedMembers = Access::ContainerMembers(*profile.app, L"ingress-active");
        if (targetInactive_)
        {
            evidence.reloadedInactiveMembers = Access::ContainerMembers(*profile.app, L"ingress-inactive");
            evidence.reloadedActiveMembersUnchanged = SameMembers(evidence.reloadedMembers, evidence.initialMembers);
            evidence.reloadedInactiveMembershipMatches = outputFile && outputFile->PathStillNamesObject(evidence.output) &&
                outputFile->HasBytes(evidence.expectedBytes) && SameMembers(evidence.reloadedInactiveMembers, {evidence.output});
            evidence.reloadedInactiveActivated = ContainerWidget(*profile.app, L"ingress-root").activeCategoryId == L"ingress-inactive";
            evidence.reloadedRootStateExcludingActivationMatches = RootMatchesIgnoringActivation();
            evidence.reloadedInactiveSettingsPreserved = SameWidget(ContainerWidget(*profile.app, L"ingress-inactive"), initialInactive);
        }
        else evidence.reloadedMembershipMatches = outputFile && outputFile->PathStillNamesObject(evidence.output) &&
            outputFile->HasBytes(evidence.expectedBytes) && SameMembers(evidence.reloadedMembers, {evidence.existing, evidence.output});
        evidence.reloadedRootPreserved = RootMatches(); evidence.reloadedActivePreserved = ActiveMatches();
        evidence.reloadedInactivePreserved = InactiveMatches();
        evidence.reloadedRevision = Access::LayoutRevision(*profile.app); evidence.reloadObserved = true;
    }
public:
    void BindExecution(const std::shared_ptr<drag_global::ole_driver::V4GroupExecutionKey>& key,
        const std::shared_ptr<drag_global::V4IndependentShellOwner>& sourceOwner,
        const std::shared_ptr<const drag_global::ole_driver::V3HostEpoch>& epoch, OwnedProfileDesktopRoot& root)
    {
        RequireObservation(key && !awaitStarted_ && evidence.inputObserved && !evidence.baselineRevalidated && root.profile &&
            root.profile.get() == &profile && root.StillOwned() && profile.app && payload && payload->files.size() == 1 &&
            payload->candidateId == evidence.candidateId && sourceOwner && owner == sourceOwner &&
            !owner.owner_before(sourceOwner) && !sourceOwner.owner_before(owner) && Access::asyncOwner == profile.app &&
            Access::v4IndependentShellOwner == owner && Access::ContainerWorkIdle(*profile.app) &&
            Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app) && evidence.initialRevision &&
            Access::LayoutRevision(*profile.app) == *evidence.initialRevision && source.starts == 0 && source.ends == 0 &&
            RootMatches() && ActiveMatches() && InactiveMatches() &&
            SameMembers(Access::ContainerMembers(*profile.app, L"ingress-active"), evidence.initialMembers) &&
            (!targetInactive_ || SameMembers(Access::ContainerMembers(*profile.app, L"ingress-inactive"), evidence.initialInactiveMembers)) &&
            sourceFile && sourceFile->PathStillNamesObject(evidence.source) && sourceFile->HasBytes(evidence.expectedBytes) &&
            existingFile && existingFile->PathStillNamesObject(evidence.existing) && existingFile->HasBytes(ExistingBytes()) &&
            DirectoryStillBound() && (!mapped_ || (sentinelFile && sentinelFile->PathStillNamesObject(profile.desktop / L"Sentinel.txt") &&
                sentinelFile->HasBytes(SentinelBytes()))),
            "group same-run baseline unavailable");
        SetLastError(ERROR_SUCCESS); const DWORD attributes = GetFileAttributesW(evidence.output.c_str());
        const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        RequireObservation(attributes == INVALID_FILE_ATTRIBUTES && error == ERROR_FILE_NOT_FOUND && ParentMatchesOwnedRoot(root) &&
            key->BindBaseline(sourceOwner, epoch, profile.app, static_cast<IDataObject*>(&source), payload->candidateId, identity_),
            "group same-run binding unavailable");
        if (mapped_) { evidence.mappedParentVerifiedBeforeDrop = true; evidence.mappedDistinctFromSourceAndDesktop = true; }
        evidence.baselineRevalidated = true;
    }
    void CaptureExecution(const std::shared_ptr<drag_global::ole_driver::V4GroupExecutionKey>& key,
        const std::shared_ptr<drag_global::V4IndependentShellOwner>& sourceOwner,
        const std::shared_ptr<const drag_global::ole_driver::V3HostEpoch>& epoch)
    {
        RequireObservation(key && evidence.baselineRevalidated && awaitCompleted_ && payload &&
            key->BeginCapture(sourceOwner, epoch, profile.app, static_cast<IDataObject*>(&source), payload->candidateId, identity_),
            "group same-run capture unavailable");
        try { CaptureResults(); }
        catch (const std::exception& error) { evidence.unresolved.push_back(std::string("group_business_snapshot_incomplete:") + error.what()); }
        catch (...) { evidence.unresolved.emplace_back("group_business_snapshot_incomplete"); }
        auto snapshot = std::make_shared<const V4GroupCopyEvidence>(evidence);
        RequireObservation(key->Publish(sourceOwner, epoch, profile.app, static_cast<IDataObject*>(&source), payload->candidateId,
            identity_, std::move(snapshot)), "group same-run sealing unavailable");
    }
};

// This is isolated input preparation only. It cannot certify a pointer hit,
// group capture, member transfer, completion or persisted result.
struct V4NativeGroupMemberInitial
{
    std::string candidateId;
    fs::path moving, sentinel;
    std::vector<std::byte> expectedBytes;
    std::optional<FILE_ID_INFO> movingId, sentinelId;
    std::optional<Access::V4NativeGroupMemberSource> source;
    std::unique_ptr<ObservedPhysicalFile> movingHandle, sentinelHandle;
    bool preparationAttempted = false, prepared = false, resourceUnavailable = false;
    std::vector<std::string> unresolved;
};
inline V4NativeGroupMemberInitial PrepareV4NativeGroupMemberInitial(
    OwnedProfileDesktopRoot& root, const drag_global::V4SingleFileFixture& fixture,
    const drag_global::V4FamilyValidatedContext& verified,
    const drag_global::event_graph::V4FamilyIdentityContext& identity,
    std::size_t maxChildren, std::size_t maxWidgets, std::size_t maxUtf16Bytes, std::size_t maxFileBytes)
{
    V4NativeGroupMemberInitial out;
    try
    {
        using namespace drag_global;
        if (!root.StillOwned() || !root.profile || !root.profile->app ||
            verified.Status() != V4CoordinateStatus::coordinate_ready || verified.TaskBActive() ||
            identity.Status() != V4FamilyStatus::schema_only || verified.FamilyBinding() != identity.FamilyBinding() ||
            verified.Axes().size() != identity.Axes().size())
        { out.unresolved.emplace_back("v4_native_group_initial_context_unavailable"); return out; }
        for (std::size_t i = 0; i < verified.Axes().size(); ++i)
        {
            const auto& a = verified.Axes()[i]; const auto& b = identity.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.label != b.label || a.values != b.values)
            { out.unresolved.emplace_back("v4_native_group_initial_schema_mismatch"); return out; }
        }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty())
        { out.resourceUnavailable = raw.resourceUnavailable; out.unresolved.emplace_back("v4_native_group_initial_candidate_unavailable"); return out; }
        if (fixture.bytes.size() > maxFileBytes || fixture.file.native().size() > maxUtf16Bytes / sizeof(wchar_t))
        { out.resourceUnavailable = true; out.unresolved.emplace_back("v4_native_group_initial_input_budget_unavailable"); return out; }
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        if (value("T01") != "internal_native" || value("source.E02") != "collection_group" ||
            value("source.E03") != "widget_body" || value("source.E08") != "first_child" ||
            value("source.E08.active_child_type") != "collection" || value("V4.source.kind") != "file_only" ||
            value("V4.source.file_count") != "one" || value("V4.file.member0_kind") != "file" ||
            value("V4.file.member0_identity") != "unique" || !fixture.inputPrepared || !fixture.created ||
            !fixture.fileIdentityObserved || fixture.candidateId != raw.id || fixture.root != root.profile->desktop ||
            fixture.file.parent_path() != fixture.root || !OwnedProfileDesktopRoot::SameId(fixture.rootId, root.desktopId))
        { out.unresolved.emplace_back("v4_native_group_initial_candidate_shape_unavailable"); return out; }
        constexpr char digits[] = "0123456789abcdef";
        const auto appendByte = [&](std::string& text, unsigned byte) {
            text.push_back(digits[(byte >> 4) & 15]); text.push_back(digits[byte & 15]); };
        std::string pathValue = "utf16le_hex_", bytesValue = "hex_";
        for (const auto unit : fixture.file.native())
        { const auto bits = static_cast<std::uint16_t>(unit); appendByte(pathValue, bits & 255); appendByte(pathValue, bits >> 8); }
        for (const auto byte : fixture.bytes) appendByte(bytesValue, std::to_integer<unsigned>(byte));
        if (value("V4.file_path.index_0.utf16le") != pathValue || value("V4.file_content.index_0.bytes") != bytesValue)
        { out.unresolved.emplace_back("v4_native_group_initial_file_axis_mismatch"); return out; }
        auto& profile = *root.profile;
        if (Access::asyncOwner != profile.app || Access::productionDispatch != 1 || !Access::nativePointerDispatch ||
            !Access::fixtureControlWindow || !Access::messageTrace || !Access::NativeDragIdle(*profile.app) ||
            !Access::ContainerWorkIdle(*profile.app) || !Access::LayoutSaveIdle(*profile.app))
        { out.unresolved.emplace_back("v4_native_group_initial_host_unavailable"); return out; }
        out.candidateId = raw.id; out.moving = fixture.file; out.sentinel = profile.desktop / L"Sentinel.txt";
        out.expectedBytes = fixture.bytes;
        out.movingHandle = std::make_unique<ObservedPhysicalFile>(out.moving);
        out.sentinelHandle = std::make_unique<ObservedPhysicalFile>(out.sentinel);
        const std::vector<std::byte> sentinelBytes{std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'},
            std::byte{'u'}, std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}};
        if (!OwnedProfileDesktopRoot::SameId(out.movingHandle->Identity(), fixture.fileId) ||
            OwnedProfileDesktopRoot::SameId(out.movingHandle->Identity(), out.sentinelHandle->Identity()) ||
            !out.movingHandle->PathStillNamesObject(out.moving) || !out.movingHandle->HasBytes(out.expectedBytes) ||
            !out.sentinelHandle->PathStillNamesObject(out.sentinel) || !out.sentinelHandle->HasBytes(sentinelBytes))
        { out.unresolved.emplace_back("v4_native_group_initial_file_identity_unavailable"); return out; }
        out.movingId = out.movingHandle->Identity(); out.sentinelId = out.sentinelHandle->Identity();
        out.preparationAttempted = true;
        Access::ResetContainerScene(*profile.app, out.sentinel);
        Access::SeedExternalStorage(*profile.app, DesktopWidgetType::CollectionGroup, DesktopWidgetType::Collection, false, out.moving);
        RequireObservation(Access::Save(*profile.app), "native group initial save unavailable");
        RefreshContainer(profile, false, L"ingress-active", false);
        Wait([&] { return Access::NativeDragIdle(*profile.app) && Access::ContainerWorkIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
        bool resource = false;
        out.source = Access::InspectNativeGroupMemberSource(*profile.app, out.moving, L"ingress-root",
            maxChildren, maxWidgets, maxUtf16Bytes, resource);
        out.resourceUnavailable = resource;
        if (!out.source || !root.StillOwned() || !out.movingHandle->PathStillNamesObject(out.moving) ||
            !out.movingHandle->HasBytes(out.expectedBytes) || !out.sentinelHandle->PathStillNamesObject(out.sentinel) ||
            !out.sentinelHandle->HasBytes(sentinelBytes) ||
            !OwnedProfileDesktopRoot::SameId(out.source->fileId, *out.movingId))
        { out.unresolved.emplace_back("v4_native_group_initial_snapshot_unavailable"); return out; }
        out.prepared = true;
        out.unresolved.emplace_back("v4_native_group_candidate_axes_and_ordered_children_unverified");
        out.unresolved.emplace_back("v4_native_group_device_capture_completion_and_business_unverified");
        out.unresolved.emplace_back("v4_native_group_snapshot_non_atomic_and_file_mutation_unverified");
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.unresolved.emplace_back("v4_native_group_initial_resource_unavailable"); }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.unresolved.emplace_back("v4_native_group_initial_resource_unavailable"); }
    catch (...) { out.unresolved.emplace_back("v4_native_group_initial_observation_unavailable"); }
    return out;
}
}

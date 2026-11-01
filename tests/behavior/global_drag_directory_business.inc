// Include after the behavior Profile, filesystem observers and completion
// helpers have been defined, before the generated adapter. This observer never
// copies files, chooses drag routing or commits layout.
namespace behavior
{
struct V4DirectoryMemberEvidence
{
    std::string slot, objectIdentity;
    fs::path relative;
    bool directory = false;
    std::vector<std::byte> expectedBytes;
    std::optional<FILE_ID_INFO> originalId, sourceId, outputId;
    std::optional<ole_content_output_observation::Evidence> outputBytes, reloadedBytes;
};
struct V4DirectoryCopyEvidence
{
    std::string candidateId;
    fs::path source, output;
    std::optional<FILE_ID_INFO> originalRootId, sourceRootId, outputRootId;
    std::vector<V4DirectoryMemberEvidence> members;
    std::optional<drag_global::ole_driver::CompletionEvidence> completion;
    std::optional<GridCell> outputCell, reloadedCell;
    std::optional<std::uint64_t> initialRevision, committedRevision;
    std::optional<std::size_t> initialCount, finalCount, reloadedCount;
    std::optional<bool> landingMatched;
    std::optional<std::string> localBusinessMismatch;
    bool sourcePayloadObserved = false, baselineRevalidated = false, independentAssertionsCompleted = false, assertionsCompleted = false;
    std::vector<std::string> unresolved{
        "directory_identity_lifetime_and_snapshot_atomicity_unverified",
        "directory_read_to_commit_and_landing_causality_unverified",
        "directory_other_candidate_axes_and_complete_event_graph_unimplemented"};
};

// The complete direct-child tree is re-derived from the authoritative family
// and compared with its input observation. The latter is not a live lease or a
// proof that the filesystem cannot change during the production operation.
struct V4DirectoryCopyObservation
{
private:
    Profile& profile;
    ole_file_source_fixture::Source& source;
    GridCell landing;
    std::vector<std::unique_ptr<ObservedPhysicalFile>> originalFiles;
    std::unique_ptr<ObservedPhysicalFile> sentinel;
    std::vector<std::pair<fs::path, GridCell>> initialCells;
    std::size_t traceStart = 0;
    drag_global::ole_driver::CompletionHostState initialCompletion;
    V4DirectoryCopyEvidence evidence;
    bool awaitStarted_ = false, awaitCompleted_ = false;
    std::shared_ptr<const drag_global::ole_driver::V4DirectoryObserverIdentity> identity_{
        new drag_global::ole_driver::V4DirectoryObserverIdentity};

    static FILE_ID_INFO DirectoryId(const fs::path& path)
    {
        struct Handle { HANDLE h; ~Handle() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
        Handle handle{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        FILE_ID_INFO id{}; FILE_ATTRIBUTE_TAG_INFO tag{};
        RequireObservation(handle.h != INVALID_HANDLE_VALUE &&
            GetFileInformationByHandleEx(handle.h, FileAttributeTagInfo, &tag, sizeof(tag)) &&
            (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            GetFileInformationByHandleEx(handle.h, FileIdInfo, &id, sizeof(id)) &&
            std::any_of(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), [](BYTE b) { return b != 0; }),
            "directory identity or attributes unavailable");
        return id;
    }
    static std::vector<std::wstring> Entries(const fs::path& directory)
    {
        std::error_code error;
        fs::directory_iterator current(directory, error), end;
        RequireObservation(!error, "directory enumeration unavailable");
        std::vector<std::wstring> names;
        for (; current != end; current.increment(error))
        {
            RequireObservation(!error, "directory enumeration failed");
            names.push_back(current->path().filename().native());
        }
        RequireObservation(!error, "directory enumeration failed");
        std::sort(names.begin(), names.end());
        return names;
    }
    std::vector<std::wstring> ExpectedNames() const
    {
        std::vector<std::wstring> names;
        for (const auto& member : evidence.members) names.push_back(member.relative.native());
        std::sort(names.begin(), names.end());
        return names;
    }

public:
    V4DirectoryCopyObservation(const V4DirectoryCopyObservation&) = delete;
    V4DirectoryCopyObservation& operator=(const V4DirectoryCopyObservation&) = delete;
    V4DirectoryCopyEvidence Evidence() const { return evidence; }
    V4DirectoryCopyObservation(Profile& host, ole_file_source_fixture::Source& provider,
        const drag_global::V4FamilyDescriptor& family, const drag_global::V4FamilyValidatedContext& verified,
        const drag_global::event_graph::V4FamilyIdentityContext& identity,
        const drag_global::directory_fixture::FixtureObservation& input, GridCell expectedLanding)
        : profile(host), source(provider), landing(std::move(expectedLanding))
    {
        using namespace drag_global;
        RequireObservation(verified.Status() == V4CoordinateStatus::coordinate_ready &&
            identity.Status() == V4FamilyStatus::schema_only && !verified.TaskBActive() && !family.b &&
            verified.FamilyBinding() == identity.FamilyBinding() && V4FamilyBinding(family) == identity.FamilyBinding(),
            "directory candidate family unavailable");
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        RequireObservation(!raw.resourceUnavailable && raw.errors.empty() && !raw.id.empty() && raw.id == input.candidateId &&
            input.inputsObserved && input.rootCreated && input.rootIdentityObserved && source.asynchronous &&
            source.owner == GetCurrentThreadId() && Access::asyncOwner == profile.app && Access::productionDispatch == 1 &&
            Access::fixtureControlWindow && Access::messageTrace && !Access::messageTrace->LostOrCrossThread(),
            "directory source fixture or production host unavailable");
        RequireObservation(family.a.kind == V4SourceKind::file_only && family.a.fileCount.kind == V3CountKind::exact &&
            family.a.fileCount.decimal == "1" && family.a.filePaths.size() == 1 && family.a.filePaths.front().index == "0" &&
            family.a.filePaths.front().kind == V4PayloadKind::exact && family.a.filePaths.front().base == V4PathBase::raw &&
            family.a.directoryRoots.size() == 1 &&
            family.a.directoryRoots.front().rootIndex == "0" &&
            family.a.directoryRoots.front().completeness == V4DirectoryCompleteness::complete &&
            Value(verified.Axes(), verified.Coordinate(), "V4.file.member0_kind") == "directory" &&
            Value(verified.Axes(), verified.Coordinate(), "V4.file.member0_identity") == "unique",
            "directory model is not a complete single source");
        RequireObservation(family.a.filePaths.front().utf16leHex.size() <= 131068 &&
            family.a.directoryNodes.size() <= 16384, "directory observation budget unavailable");
        fs::path modelSource;
        RequireObservation(drag_global::directory_fixture::DecodePath(family.a.filePaths.front().utf16leHex, modelSource) &&
            modelSource == input.source && modelSource.is_absolute() && drag_global::directory_fixture::SafeRelative(modelSource.filename()),
            "directory source path unavailable");
        auto providerPath = modelSource; providerPath.make_preferred();
        RequireObservation(source.HasConstructionShell() && source.ConstructionPaths().size() == 1 &&
            source.ConstructionPaths().front() == providerPath, "directory Shell construction input differs from candidate");
        evidence.candidateId = raw.id; evidence.source = modelSource;
        evidence.output = profile.desktop / modelSource.filename();
        RequireObservation(evidence.source != evidence.output, "directory copy requires a different target");
        std::error_code error;
        const auto status = fs::symlink_status(evidence.output, error);
        RequireObservation((!error || error == std::errc::no_such_file_or_directory) &&
            status.type() == fs::file_type::not_found, "directory target is not an observed vacant name");
        evidence.originalRootId = DirectoryId(evidence.source);
        RequireObservation(drag_global::directory_fixture::SameId(*evidence.originalRootId, input.rootIdentity),
            "directory root identity differs from input observation");
        RequireObservation(input.nodes.size() == family.a.directoryNodes.size(), "directory node input incomplete");
        std::size_t totalBytes = 0;
        for (const auto& node : family.a.directoryNodes)
        {
            fs::path relative; std::vector<std::byte> bytes;
            constexpr std::size_t maxBytes = 64u * 1024u * 1024u;
            RequireObservation(node.relativePathUtf16Hex.size() <= 131068 &&
                (node.contentKind != V4PayloadKind::exact || node.contentHex.size() / 2 <= maxBytes - totalBytes),
                "directory child observation budget unavailable");
            RequireObservation(node.rootIndex == "0" && node.pathKind == V4PayloadKind::exact &&
                drag_global::directory_fixture::DecodePath(node.relativePathUtf16Hex, relative) &&
                drag_global::directory_fixture::SafeRelative(relative) && !relative.has_parent_path() &&
                node.objectIdentity.kind == V3CountKind::exact &&
                (node.nodeKind == V4DirectoryNodeKind::directory || node.nodeKind == V4DirectoryNodeKind::file),
                "directory child is not a supported direct node");
            const bool directory = node.nodeKind == V4DirectoryNodeKind::directory;
            RequireObservation((directory && node.contentKind == V4PayloadKind::not_applicable) ||
                (!directory && node.contentKind == V4PayloadKind::exact && drag_global::directory_fixture::DecodeHex(node.contentHex, bytes)),
                "directory child content unavailable");
            totalBytes += bytes.size();
            const auto found = std::find_if(input.nodes.begin(), input.nodes.end(),
                [&](const auto& entry) { return entry.slot == node.slotIndex; });
            RequireObservation(found != input.nodes.end() && found->relative == relative && found->directory == directory &&
                found->created && found->identityObserved && (directory || (found->bytesObserved && found->actualBytes == bytes)),
                "directory child input snapshot unavailable");
            for (const auto& existing : evidence.members)
            {
                RequireObservation(existing.slot != node.slotIndex && existing.objectIdentity != node.objectIdentity.decimal,
                    "directory child object alias is unsupported");
                const auto left = existing.relative.native(), right = relative.native();
                RequireObservation(left.size() <= static_cast<std::size_t>((std::numeric_limits<int>::max)()) &&
                    right.size() <= static_cast<std::size_t>((std::numeric_limits<int>::max)()), "directory name comparison unavailable");
                const int compared = CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                    right.data(), static_cast<int>(right.size()), TRUE);
                RequireObservation(compared != 0 && compared != CSTR_EQUAL, "directory child name aliases are unsupported");
            }
            V4DirectoryMemberEvidence member;
            member.slot = node.slotIndex; member.objectIdentity = node.objectIdentity.decimal;
            member.relative = relative; member.directory = directory; member.expectedBytes = bytes;
            if (directory)
            {
                member.originalId = DirectoryId(evidence.source / relative);
                RequireObservation(Entries(evidence.source / relative).empty(), "nested directory contents are unsupported");
                originalFiles.push_back(nullptr);
            }
            else
            {
                auto file = std::make_unique<ObservedPhysicalFile>(evidence.source / relative);
                member.originalId = file->Identity();
                RequireObservation(file->HasBytes(bytes), "source child bytes differ from model input");
                originalFiles.push_back(std::move(file));
            }
            RequireObservation(drag_global::directory_fixture::SameId(*member.originalId, found->identity), "source child identity changed");
            for (const auto& existing : evidence.members)
                RequireObservation(!drag_global::directory_fixture::SameId(*existing.originalId, *member.originalId),
                    "directory child physical aliases are unsupported");
            evidence.members.push_back(std::move(member));
        }
        RequireObservation(Entries(evidence.source) == ExpectedNames(), "source tree contains an unmodelled child");
        Wait([&] { return Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
        RequireObservation(source.starts == 0 && source.ends == 0 && Outputs(profile).empty(),
            "directory copy requires an idle isolated starting scene");
        sentinel = std::make_unique<ObservedPhysicalFile>(profile.desktop / L"Sentinel.txt");
        const std::vector<std::byte> sentinelBytes{std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'},
            std::byte{'u'}, std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}};
        RequireObservation(sentinel->HasBytes(sentinelBytes), "sentinel input unavailable");
        for (const auto& item : Access::Items(*profile.app))
        {
            RequireObservation(item.gridCell.pageId != landing.pageId || landing.column < item.gridCell.column ||
                landing.row < item.gridCell.row || landing.column >= item.gridCell.column + item.gridSpan.columns ||
                landing.row >= item.gridCell.row + item.gridSpan.rows, "directory landing initially occupied");
            initialCells.emplace_back(fs::path(item.parsingName), item.gridCell);
        }
        RequireObservation(initialCells.size() == 1, "directory copy requires the isolated sentinel model");
        std::error_code sentinelError;
        const bool sameSentinel = fs::equivalent(initialCells.front().first, profile.desktop / L"Sentinel.txt", sentinelError);
        RequireObservation(!sentinelError && sameSentinel, "directory initial model must contain its actual sentinel");
        evidence.initialCount = initialCells.size(); evidence.initialRevision = Access::LayoutRevision(*profile.app);
        traceStart = Access::messageTrace->Observations().size();
        initialCompletion = {true, static_cast<std::uint64_t>(source.starts), static_cast<std::uint64_t>(source.ends)};
    }

    void BeforeOleCall(const drag_global::ole_driver::Input& input) const
    {
        if (input.kind == drag_global::ole_driver::Kind::drop)
            RequireEvent(Access::Target(*profile.app) == Access::Desktop(*profile.app) &&
                Access::Region(*profile.app) == HitRegion::Empty, "directory copy reaches empty desktop");
    }
    void Await(const drag_global::ole_driver::Observation& terminal)
    {
        using namespace drag_global::ole_driver;
        RequireObservation(!awaitStarted_, "directory completion already requested"); awaitStarted_ = true;
        RequireObservation(Access::messageTrace && !Access::messageTrace->LostOrCrossThread(), "directory completion trace unavailable");
        RequireObservation(terminal.returnedEffect.has_value(), "directory Drop effect observation unavailable");
        RequireEvent(terminal.kind == Kind::drop && SUCCEEDED(terminal.result) &&
            *terminal.returnedEffect == DROPEFFECT_COPY, "directory Drop reports copy");
        evidence.completion = WaitForCompletion(*Access::messageTrace, traceStart, Access::fixtureControlWindow,
            MessageRoute::control_window_proc, terminal, initialCompletion, kShellFileOperationCompletedMessage, 12000,
            [&] { return CompletionHostState{Access::FileDropIdle(*profile.app),
                static_cast<std::uint64_t>(source.starts), static_cast<std::uint64_t>(source.ends)}; },
            [] { Access::ThrowAsyncException(); });
        RequireEvent(evidence.completion->signal != CompletionSignal::failed_message, "directory transfer reports failure");
        RequireObservation(evidence.completion->signal == CompletionSignal::completed_messages,
            "directory copy completion messages unavailable");
        Wait([&] { return Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
        awaitCompleted_ = true;
    }

private:
    void AssertResults()
    {
        RequireObservation(evidence.completion && evidence.completion->signal == drag_global::ole_driver::CompletionSignal::completed_messages &&
            !source.wrongApartment && !source.fileDropObservationFailed && !source.formatProbeObservationFailed &&
            source.fileReads >= 0 && source.fileDropReads.size() == static_cast<std::size_t>(source.fileReads),
            "directory source or completion observations unavailable");
        RequireBusiness(source.starts == 1 && source.ends == 1 && SUCCEEDED(source.outcome) &&
            source.endEffect == DROPEFFECT_COPY && !source.fileReadBeforeStart, "directory source lease completes");
        for (const auto& read : source.fileDropReads)
        {
            RequireObservation(read.threadId == source.owner && read.startsAtEntry > read.endsAtEntry, "directory read outside source lease");
            if (FAILED(read.result)) continue;
            RequireObservation(read.capture == ole_file_source_fixture::FileDropCapture::captured && read.paths.size() == 1 &&
                read.paths.front() == evidence.source &&
                drag_global::directory_fixture::SameId(DirectoryId(read.paths.front()), *evidence.originalRootId),
                "directory source payload unavailable or different");
            evidence.sourcePayloadObserved = true;
        }
        RequireObservation(evidence.sourcePayloadObserved, "directory payload was not observed");
        const auto outputs = Outputs(profile);
        RequireBusiness(outputs.size() == 1 && outputs.front() == evidence.output, "directory output path must be present exactly once");
        evidence.sourceRootId = DirectoryId(evidence.source); evidence.outputRootId = DirectoryId(evidence.output);
        RequireBusiness(drag_global::directory_fixture::SameId(*evidence.originalRootId, *evidence.sourceRootId) &&
            !drag_global::directory_fixture::SameId(*evidence.sourceRootId, *evidence.outputRootId), "directory source remains and output is distinct");
        const auto names = ExpectedNames();
        RequireBusiness(Entries(evidence.source) == names && Entries(evidence.output) == names,
            "source and output directory contain the modelled direct children");
        for (std::size_t i = 0; i < evidence.members.size(); ++i)
        {
            auto& member = evidence.members[i]; const auto sourcePath = evidence.source / member.relative;
            const auto outputPath = evidence.output / member.relative;
            if (member.directory)
            {
                member.sourceId = DirectoryId(sourcePath); member.outputId = DirectoryId(outputPath);
                RequireBusiness(Entries(sourcePath).empty() && Entries(outputPath).empty(), "copied child directories remain empty");
            }
            else
            {
                ObservedPhysicalFile original(sourcePath), copied(outputPath);
                member.sourceId = original.Identity(); member.outputId = copied.Identity();
                member.outputBytes = ole_content_output_observation::ExactBytes(outputPath, member.expectedBytes);
                RequireObservation(member.outputBytes->observed, "copied child bytes unavailable");
                RequireBusiness(original.HasBytes(member.expectedBytes) && copied.HasBytes(member.expectedBytes) &&
                    member.outputBytes->matches, "copied child bytes match the model");
            }
            RequireBusiness(drag_global::directory_fixture::SameId(*member.originalId, *member.sourceId) &&
                !drag_global::directory_fixture::SameId(*member.sourceId, *member.outputId), "child source remains and output is distinct");
        }
        evidence.finalCount = Access::Items(*profile.app).size(); evidence.outputCell = FindCopyBusiness(*profile.app, evidence.output).gridCell;
        RequireBusiness(*evidence.finalCount == initialCells.size() + 1, "directory becomes one desktop member");
        // The selected read and the asynchronous commit are not yet causally
        // bound. Preserve the real location comparison, without turning it into
        // a pass or a product failure until that association is observed.
        evidence.landingMatched = Same(*evidence.outputCell, landing);
        for (const auto& [path, cell] : initialCells)
            RequireBusiness(Same(FindCopyBusiness(*profile.app, path).gridCell, cell), "existing desktop placement is retained");
        evidence.committedRevision = Access::LayoutRevision(*profile.app);
        RequireBusiness(*evidence.committedRevision > *evidence.initialRevision, "directory placement commits a layout revision");
        Access::PerturbBeforeDiskRead(*profile.app); Access::RequestAsync(*profile.app, true);
        Wait([&] { return Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
        evidence.reloadedCount = Access::Items(*profile.app).size(); evidence.reloadedCell = FindCopyBusiness(*profile.app, evidence.output).gridCell;
        RequireBusiness(*evidence.reloadedCount == initialCells.size() + 1 && Same(*evidence.reloadedCell, *evidence.outputCell),
            "actual directory placement survives production reload");
        for (const auto& [path, cell] : initialCells)
            RequireBusiness(Same(FindCopyBusiness(*profile.app, path).gridCell, cell), "existing placement survives reload");
        RequireBusiness(drag_global::directory_fixture::SameId(DirectoryId(evidence.source), *evidence.originalRootId) &&
            drag_global::directory_fixture::SameId(DirectoryId(evidence.output), *evidence.outputRootId) &&
            Entries(evidence.source) == names && Entries(evidence.output) == names, "directory objects and children survive reload");
        for (std::size_t i = 0; i < evidence.members.size(); ++i)
        {
            auto& member = evidence.members[i];
            if (member.directory)
                RequireBusiness(drag_global::directory_fixture::SameId(DirectoryId(evidence.source / member.relative), *member.originalId) &&
                    drag_global::directory_fixture::SameId(DirectoryId(evidence.output / member.relative), *member.outputId) &&
                    Entries(evidence.source / member.relative).empty() && Entries(evidence.output / member.relative).empty(),
                    "source and copied child directories survive reload");
            else
            {
                ObservedPhysicalFile copied(evidence.output / member.relative);
                member.reloadedBytes = ole_content_output_observation::ExactBytes(evidence.output / member.relative, member.expectedBytes);
                RequireObservation(member.reloadedBytes->observed, "reloaded child bytes unavailable");
                RequireBusiness(originalFiles[i]->PathStillNamesObject(evidence.source / member.relative) &&
                    originalFiles[i]->HasBytes(member.expectedBytes) &&
                    drag_global::directory_fixture::SameId(copied.Identity(), *member.outputId) &&
                    copied.HasBytes(member.expectedBytes) && member.reloadedBytes->matches,
                    "source and copied child identity and bytes survive reload");
            }
        }
        const std::vector<std::byte> sentinelBytes{std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'},
            std::byte{'u'}, std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}};
        RequireBusiness(sentinel->PathStillNamesObject(profile.desktop / L"Sentinel.txt") && sentinel->HasBytes(sentinelBytes),
            "sentinel identity and bytes survive directory copy");
        evidence.independentAssertionsCompleted = true;
        // Full same-request graph, source-to-commit and landing correlation
        // remain unresolved; assertionsCompleted deliberately stays false.
    }
public:
    void BindExecution(const std::shared_ptr<drag_global::ole_driver::V4DirectoryExecutionKey>& key,
        const std::shared_ptr<drag_global::directory_fixture::DirectoryShellOwner>& owner,
        const std::shared_ptr<drag_global::directory_fixture::DirectoryCopyLease>& lease,
        const std::shared_ptr<const drag_global::ole_driver::V3HostEpoch>& epoch,
        OwnedProfileDesktopRoot& root, const drag_global::V4FamilyDescriptor& family,
        const drag_global::V4FamilyValidatedContext& verified,
        const drag_global::event_graph::V4FamilyIdentityContext& identity,
        const drag_global::directory_fixture::DirectoryShellBinding& binding)
    {
        RequireObservation(key && !awaitStarted_ && !evidence.baselineRevalidated && root.profile &&
            root.profile.get() == &profile && root.StillOwned() && profile.app && owner && lease &&
            Access::asyncOwner == profile.app && Access::v4DirectoryShellOwner == owner &&
            binding.SourceObject() == &source && binding.candidateId == evidence.candidateId &&
            source.starts == 0 && source.ends == 0 && evidence.originalRootId && evidence.initialRevision &&
            Access::LayoutRevision(*profile.app) == *evidence.initialRevision && Access::FileDropIdle(*profile.app) &&
            Access::LayoutSaveIdle(*profile.app) && sentinel, "directory same-run baseline unavailable");
        const auto current = drag_global::directory_fixture::RevalidateDirectoryCopyLease(family, verified, identity, lease);
        RequireObservation(current.inputObserved && !current.resourceUnavailable && current.fixtureLeaseOverlapObserved &&
            current.candidateId == evidence.candidateId &&
            drag_global::directory_fixture::SameId(DirectoryId(evidence.source), *evidence.originalRootId) &&
            Entries(evidence.source) == ExpectedNames(), "directory same-run source lease unavailable");
        const std::vector<std::byte> sentinelBytes{std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'},
            std::byte{'u'}, std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}};
        RequireObservation(sentinel->PathStillNamesObject(profile.desktop / L"Sentinel.txt") && sentinel->HasBytes(sentinelBytes),
            "directory sentinel baseline unavailable");
        for (const auto& [path, cell] : initialCells)
            RequireObservation(Same(FindCopyBusiness(*profile.app, path).gridCell, cell), "directory initial placement changed");
        SetLastError(ERROR_SUCCESS); const DWORD attributes = GetFileAttributesW(evidence.output.c_str());
        const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        RequireObservation(attributes == INVALID_FILE_ATTRIBUTES && error == ERROR_FILE_NOT_FOUND && root.StillOwned() &&
            key->BindBaseline(owner, lease, epoch, profile.app, static_cast<IDataObject*>(&source), evidence.candidateId, identity_),
            "directory same-run binding unavailable");
        evidence.baselineRevalidated = true;
    }
    void CaptureExecution(const std::shared_ptr<drag_global::ole_driver::V4DirectoryExecutionKey>& key,
        const std::shared_ptr<drag_global::directory_fixture::DirectoryShellOwner>& owner,
        const std::shared_ptr<drag_global::directory_fixture::DirectoryCopyLease>& lease,
        const std::shared_ptr<const drag_global::ole_driver::V3HostEpoch>& epoch)
    {
        RequireObservation(key && evidence.baselineRevalidated && awaitCompleted_ &&
            key->BeginCapture(owner, lease, epoch, profile.app, static_cast<IDataObject*>(&source), evidence.candidateId, identity_),
            "directory same-run capture unavailable");
        try { AssertResults(); }
        catch (const drag_global::ole_driver::BusinessAssertionFailure& error)
        { evidence.localBusinessMismatch = error.what(); evidence.unresolved.emplace_back("directory_business_mismatch_attribution_unverified"); }
        catch (const std::exception& error) { evidence.unresolved.push_back(std::string("directory_business_snapshot_incomplete:") + error.what()); }
        catch (...) { evidence.unresolved.emplace_back("directory_business_snapshot_incomplete"); }
        auto snapshot = std::make_shared<const V4DirectoryCopyEvidence>(evidence);
        RequireObservation(key->Publish(owner, lease, epoch, profile.app, static_cast<IDataObject*>(&source), evidence.candidateId,
            identity_, std::move(snapshot)), "directory same-run sealing unavailable");
    }
};
} // namespace behavior

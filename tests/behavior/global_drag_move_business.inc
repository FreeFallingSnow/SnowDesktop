#pragma once

// Read-only observations for a real single-file move-capable desktop Drop.
// The source lease permits rename/delete; no action or outcome is configured here.
namespace behavior
{
struct V4FileMoveEvidence
{
    std::string candidateId;
    fs::path source, output, sentinel;
    std::vector<std::byte> expectedBytes;
    std::optional<FILE_ID_INFO> sourceId, outputId, sentinelId;
    std::optional<drag_global::V4SharedMoveSourceAfter> sourceAfter;
    std::optional<drag_global::ole_driver::CompletionEvidence> completion;
    std::optional<bool> outputPresent, outputBytesMatch, outputSameSource, sentinelPreserved,
        reloadedOutputBytesMatch, reloadedOutputIdentitySame, reloadedCellMatches;
    std::optional<GridCell> outputCell, reloadedCell;
    std::optional<std::uint64_t> initialRevision, finalRevision, reloadedRevision;
    bool inputObserved = false, baselineRevalidated = false, finalObserved = false, reloadObserved = false;
    std::vector<std::string> unresolved{"move_actual_action_and_shell_source_identity_unverified",
        "move_same_run_completion_layout_and_business_unverified", "move_snapshots_non_atomic_and_late_work_unverified"};
};

struct V4FileMoveObservation
{
private:
    OwnedProfileDesktopRoot& root;
    Profile& profile;
    std::shared_ptr<drag_global::V4IndependentFilePayload> payload;
    std::shared_ptr<drag_global::V4IndependentShellOwner> owner;
    std::shared_ptr<const drag_global::V4SharedMoveLease> lease;
    ole_file_source_fixture::Source& source;
    V4FileMoveEvidence evidence;
    std::unique_ptr<ObservedPhysicalFile> sentinel, outputFile;
    std::size_t traceStart = 0;
    drag_global::ole_driver::CompletionHostState initialCompletion;
    bool awaitStarted = false, awaitCompleted = false;
    std::shared_ptr<const drag_global::ole_driver::V4MoveObserverIdentity> identity_{
        new drag_global::ole_driver::V4MoveObserverIdentity};
    static Profile& BoundProfile(OwnedProfileDesktopRoot& root)
    {
        RequireObservation(root.profile && root.profile->app && root.StillOwned(), "move profile unavailable");
        return *root.profile;
    }
    static ole_file_source_fixture::Source& BoundSource(const drag_global::V4IndependentShellSource& binding,
        const std::shared_ptr<drag_global::V4IndependentShellOwner>& owner)
    {
        auto* actual = binding.SourceObject();
        RequireObservation(actual && binding.inputPrepared && !binding.resourceUnavailable && binding.SameOwner(owner),
            "move source unavailable"); return *actual;
    }
    static std::vector<std::byte> SentinelBytes()
    { return {std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'}, std::byte{'u'},
        std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}}; }
    std::optional<bool> OutputPresent() const
    {
        if (!root.StillOwned()) return std::nullopt;
        SetLastError(ERROR_SUCCESS);
        const DWORD value = GetFileAttributesW(evidence.output.c_str());
        if (value != INVALID_FILE_ATTRIBUTES) return true;
        return GetLastError() == ERROR_FILE_NOT_FOUND ? std::optional<bool>{false} : std::nullopt;
    }
public:
    V4FileMoveObservation(const V4FileMoveObservation&) = delete;
    V4FileMoveObservation& operator=(const V4FileMoveObservation&) = delete;
    V4FileMoveEvidence Evidence() const { return evidence; }
    V4FileMoveObservation(OwnedProfileDesktopRoot& ownedRoot,
        const std::shared_ptr<drag_global::V4IndependentFilePayload>& input,
        const drag_global::V4IndependentShellSource& binding,
        const std::shared_ptr<drag_global::V4IndependentShellOwner>& sourceOwner,
        const std::shared_ptr<const drag_global::V4SharedMoveLease>& moveLease,
        const drag_global::V4FamilyValidatedContext& verified,
        const drag_global::event_graph::V4FamilyIdentityContext& identity)
        : root(ownedRoot), profile(BoundProfile(ownedRoot)), payload(input), owner(sourceOwner), lease(moveLease),
          source(BoundSource(binding, sourceOwner))
    {
        using namespace drag_global;
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        RequireObservation(verified.Status() == V4CoordinateStatus::coordinate_ready && identity.Status() == V4FamilyStatus::schema_only &&
            !verified.TaskBActive() && verified.FamilyBinding() == identity.FamilyBinding() && !raw.resourceUnavailable &&
            raw.errors.empty() && !raw.id.empty() && payload && payload->files.size() == 1 && payload->candidateId == raw.id &&
            binding.candidateId == raw.id && binding.SamePayload(payload) && owner && lease &&
            owner->MatchesSharedMoveLeaseBefore(verified, identity, payload, lease) && Access::v4IndependentShellOwner == owner &&
            Access::asyncOwner == profile.app && Access::productionDispatch == 1 && Access::fixtureControlWindow && Access::messageTrace &&
            !Access::messageTrace->LostOrCrossThread() && source.asynchronous && source.owner == GetCurrentThreadId() &&
            value("V4.source.kind") == "file_only" && value("V4.source.file_count") == "one" &&
            value("V4.file.member0_kind") == "file" && value("V4.file.member0_identity") == "unique" &&
            value("T01") == "external_ingress" && value("G04.termination") == "drop" &&
            value("target.E01") == "free_desktop" && value("target.E02") == "none" && value("H01.region") == "empty_slot",
            "move candidate or host unavailable");
        const auto inputObservation = ole_driver::InspectV4IndependentFileInputs(verified, identity, *payload, source);
        RequireObservation(inputObservation.captured && !inputObservation.resourceUnavailable && inputObservation.candidateId == raw.id &&
            inputObservation.files.size() == 1, "move source initial observation unavailable");
        evidence.candidateId = raw.id; evidence.source = inputObservation.files.front().path;
        evidence.expectedBytes = inputObservation.files.front().bytes; evidence.sourceId = inputObservation.files.front().id;
        evidence.output = profile.desktop / evidence.source.filename(); evidence.sentinel = profile.desktop / L"Sentinel.txt";
        const auto absent = OutputPresent();
        RequireObservation(absent && !*absent && evidence.output != evidence.source && source.starts == 0 && source.ends == 0,
            "move initial destination or source unavailable");
        sentinel = std::make_unique<ObservedPhysicalFile>(evidence.sentinel);
        RequireObservation(sentinel->PathStillNamesObject(evidence.sentinel) && sentinel->HasBytes(SentinelBytes()),
            "move sentinel baseline unavailable");
        evidence.sentinelId = sentinel->Identity(); evidence.initialRevision = Access::LayoutRevision(*profile.app);
        traceStart = Access::messageTrace->Observations().size();
        initialCompletion = {true, static_cast<std::uint64_t>(source.starts), static_cast<std::uint64_t>(source.ends)};
        evidence.inputObserved = true;
    }
    void BeforeOleCall(const drag_global::ole_driver::Input& input) const
    {
        if (input.kind == drag_global::ole_driver::Kind::drop)
            RequireObservation(Access::Target(*profile.app) == Access::Desktop(*profile.app) &&
                Access::Region(*profile.app) == HitRegion::Empty, "move actual target unavailable");
    }
    void Await(const drag_global::ole_driver::Observation& terminal)
    {
        using namespace drag_global::ole_driver;
        RequireObservation(!awaitStarted && terminal.kind == Kind::drop && SUCCEEDED(terminal.result) &&
            Access::messageTrace && !Access::messageTrace->LostOrCrossThread(), "move terminal or trace unavailable");
        awaitStarted = true;
        evidence.completion = WaitForCompletion(*Access::messageTrace, traceStart, Access::fixtureControlWindow,
            MessageRoute::control_window_proc, terminal, initialCompletion, kShellFileOperationCompletedMessage, 12000,
            [&] { return CompletionHostState{Access::FileDropIdle(*profile.app), static_cast<std::uint64_t>(source.starts),
                static_cast<std::uint64_t>(source.ends)}; }, [] { Access::ThrowAsyncException(); });
        RequireObservation(evidence.completion->signal == CompletionSignal::completed_messages,
            "move same-source completion messages unavailable");
        Wait([&] { return Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
        awaitCompleted = true;
    }
    void BindExecution(const std::shared_ptr<drag_global::ole_driver::V4MoveExecutionKey>& key,
        const std::shared_ptr<drag_global::V4IndependentShellOwner>& sourceOwner,
        const std::shared_ptr<const drag_global::V4SharedMoveLease>& moveLease,
        const std::shared_ptr<const drag_global::ole_driver::V3HostEpoch>& epoch,
        const drag_global::V4FamilyValidatedContext& verified,
        const drag_global::event_graph::V4FamilyIdentityContext& identity)
    {
        const auto absent = OutputPresent();
        RequireObservation(key && !awaitStarted && evidence.inputObserved && !evidence.baselineRevalidated &&
            owner == sourceOwner && lease == moveLease && root.StillOwned() && profile.app && payload && sourceOwner && moveLease &&
            sourceOwner->MatchesSharedMoveLeaseBefore(verified, identity, payload, moveLease) && absent && !*absent &&
            Access::asyncOwner == profile.app && Access::v4IndependentShellOwner == owner && evidence.initialRevision &&
            Access::LayoutRevision(*profile.app) == *evidence.initialRevision && sentinel &&
            sentinel->PathStillNamesObject(evidence.sentinel) && sentinel->HasBytes(SentinelBytes()),
            "move pre-Drop baseline unavailable");
        RequireObservation(key->BindBaseline(sourceOwner, moveLease, epoch, profile.app, static_cast<IDataObject*>(&source),
            evidence.candidateId, identity_), "move same-run key unavailable");
        evidence.baselineRevalidated = true;
    }
    void CaptureExecution(const std::shared_ptr<drag_global::ole_driver::V4MoveExecutionKey>& key,
        const std::shared_ptr<drag_global::V4IndependentShellOwner>& sourceOwner,
        const std::shared_ptr<const drag_global::V4SharedMoveLease>& moveLease,
        const std::shared_ptr<const drag_global::ole_driver::V3HostEpoch>& epoch)
    {
        RequireObservation(key && evidence.baselineRevalidated && awaitCompleted && payload && sourceOwner && moveLease &&
            sourceOwner->OwnsSharedMoveLease(payload, moveLease) &&
            key->BeginCapture(sourceOwner, moveLease, epoch, profile.app, static_cast<IDataObject*>(&source), evidence.candidateId, identity_),
            "move same-run capture unavailable");
        try
        {
            evidence.sourceAfter = moveLease->ObserveSourceAfter();
            evidence.outputPresent = OutputPresent();
            evidence.sentinelPreserved = sentinel->PathStillNamesObject(evidence.sentinel) && sentinel->HasBytes(SentinelBytes());
            if (evidence.outputPresent && *evidence.outputPresent)
            {
                outputFile = std::make_unique<ObservedPhysicalFile>(evidence.output);
                evidence.outputId = outputFile->Identity(); evidence.outputBytesMatch = outputFile->HasBytes(evidence.expectedBytes);
                if (evidence.sourceId) evidence.outputSameSource = OwnedProfileDesktopRoot::SameId(*evidence.sourceId, *evidence.outputId);
                evidence.outputCell = FindCopyBusiness(*profile.app, evidence.output).gridCell;
            }
            evidence.finalRevision = Access::LayoutRevision(*profile.app); evidence.finalObserved = true;
            if (evidence.outputCell)
            {
                Access::PerturbBeforeDiskRead(*profile.app); Access::RequestAsync(*profile.app, true);
                Wait([&] { return Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
                evidence.reloadedCell = FindCopyBusiness(*profile.app, evidence.output).gridCell;
                evidence.reloadedCellMatches = Same(*evidence.reloadedCell, *evidence.outputCell);
                evidence.reloadedOutputIdentitySame = outputFile->PathStillNamesObject(evidence.output);
                evidence.reloadedOutputBytesMatch = outputFile->HasBytes(evidence.expectedBytes);
                evidence.reloadedRevision = Access::LayoutRevision(*profile.app); evidence.reloadObserved = true;
            }
        }
        catch (const std::exception& error) { evidence.unresolved.push_back(std::string("move_business_snapshot_incomplete:") + error.what()); }
        catch (...) { evidence.unresolved.emplace_back("move_business_snapshot_incomplete"); }
        auto snapshot = std::make_shared<const V4FileMoveEvidence>(evidence);
        RequireObservation(key->Publish(sourceOwner, moveLease, epoch, profile.app, static_cast<IDataObject*>(&source),
            evidence.candidateId, identity_, std::move(snapshot)), "move same-run sealing unavailable");
    }
};
}

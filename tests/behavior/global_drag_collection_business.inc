#pragma once

// Include after container_scenarios.h, before the generated adapter. This
// prepares an isolated initial collection and reads the resulting production
// state; it never routes a drag or changes membership after the operation.
namespace behavior
{
struct V4CollectionCopyEvidence
{
    std::string candidateId;
    fs::path source, output, existing;
    std::vector<std::byte> expectedBytes;
    std::optional<FILE_ID_INFO> sourceId, outputId, existingId;
    std::vector<fs::path> initialMembers, finalMembers, reloadedMembers;
    std::optional<drag_global::ole_driver::CompletionEvidence> completion;
    std::optional<std::uint64_t> initialRevision, finalRevision, reloadedRevision;
    std::optional<bool> sourcePreserved, existingPreserved, outputBytesMatch;
    std::optional<bool> membershipMatches, reloadedMembershipMatches, noDesktopDuplicate;
    std::optional<bool> widgetPreserved, reloadedWidgetPreserved;
    bool inputObserved = false, baselineRevalidated = false, finalObserved = false, reloadObserved = false;
    std::vector<std::string> unresolved{
        "collection_candidate_axes_and_event_graph_unimplemented",
        "collection_source_read_to_commit_and_completion_causality_unverified",
        "collection_filesystem_and_model_snapshots_non_atomic"};
};

// This is an observation component, not an execution authority. The bridge
// must bind the same candidate, actual provider/host and completed operation;
// the booleans below are local observations until that linkage is established.
struct V4CollectionCopyObservation
{
private:
    Profile& profile;
    std::shared_ptr<drag_global::V4IndependentFilePayload> payload;
    std::shared_ptr<drag_global::V4IndependentShellOwner> owner;
    ole_file_source_fixture::Source& source;
    V4CollectionCopyEvidence evidence;
    DesktopWidget initialWidget;
    std::unique_ptr<ObservedPhysicalFile> sourceFile, existingFile, outputFile;
    std::size_t traceStart = 0;
    drag_global::ole_driver::CompletionHostState initialCompletion;
    bool awaitStarted_ = false, awaitCompleted_ = false;
    std::shared_ptr<const drag_global::ole_driver::V4CollectionObserverIdentity> identity_{
        new drag_global::ole_driver::V4CollectionObserverIdentity};

    static bool SameMembers(const std::vector<fs::path>& actual, const std::vector<fs::path>& expected)
    {
        if (actual.size() != expected.size()) return false;
        for (const auto& path : expected)
        {
            std::size_t count = 0;
            for (const auto& item : actual)
            {
                std::error_code error;
                const bool same = fs::equivalent(item, path, error);
                RequireObservation(!error, "collection member identity unavailable");
                if (same) ++count;
            }
            if (count != 1) return false;
        }
        return true;
    }

    bool WidgetMatches() const
    {
        const auto& widget = ContainerWidget(*profile.app, L"ingress-root");
        return widget.type == initialWidget.type && widget.title == initialWidget.title &&
            widget.customTitle == initialWidget.customTitle && widget.autoCollect == initialWidget.autoCollect &&
            widget.singleCellIconMode == initialWidget.singleCellIconMode &&
            widget.gridSpan.columns == initialWidget.gridSpan.columns && widget.gridSpan.rows == initialWidget.gridSpan.rows &&
            Same(widget.gridCell, initialWidget.gridCell) && widget.childWidgetIds == initialWidget.childWidgetIds &&
            widget.activeCategoryId == initialWidget.activeCategoryId;
    }

    static ole_file_source_fixture::Source& BoundSource(const drag_global::V4IndependentShellSource& binding,
        const std::shared_ptr<drag_global::V4IndependentShellOwner>& sourceOwner)
    {
        auto* actual = binding.SourceObject();
        RequireObservation(actual && binding.inputPrepared && !binding.resourceUnavailable && binding.SameOwner(sourceOwner),
            "collection Shell owner or source unavailable");
        return *actual;
    }

public:
    V4CollectionCopyObservation(const V4CollectionCopyObservation&) = delete;
    V4CollectionCopyObservation& operator=(const V4CollectionCopyObservation&) = delete;
    V4CollectionCopyObservation(V4CollectionCopyObservation&&) = delete;
    V4CollectionCopyObservation& operator=(V4CollectionCopyObservation&&) = delete;
    V4CollectionCopyEvidence Evidence() const { return evidence; }

    V4CollectionCopyObservation(Profile& host, const std::shared_ptr<drag_global::V4IndependentFilePayload>& input,
        const drag_global::V4IndependentShellSource& binding,
        const std::shared_ptr<drag_global::V4IndependentShellOwner>& sourceOwner,
        const drag_global::V4FamilyValidatedContext& verified,
        const drag_global::event_graph::V4FamilyIdentityContext& identity)
        : profile(host), payload(input), owner(sourceOwner), source(BoundSource(binding, sourceOwner))
    {
        using namespace drag_global;
        RequireObservation(verified.Status() == V4CoordinateStatus::coordinate_ready &&
            identity.Status() == V4FamilyStatus::schema_only && !verified.TaskBActive() &&
            verified.FamilyBinding() == identity.FamilyBinding(), "collection family unavailable");
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        RequireObservation(payload && binding.SamePayload(payload) && payload->files.size() == 1 &&
            binding.candidateId == payload->candidateId &&
            !raw.resourceUnavailable && raw.errors.empty() && !raw.id.empty() && raw.id == payload->candidateId &&
            value("V4.source.kind") == "file_only" && value("T01") == "external_ingress" &&
            value("V4.source.file_count") == "one" &&
            value("target.E01") == "free_desktop" && value("target.E02") == "collection" &&
            value("target.E03") == "widget_body" && value("V4.file.member0_kind") == "file" &&
            value("V4.file.member0_identity") == "unique" && source.asynchronous && source.owner == GetCurrentThreadId() &&
            Access::asyncOwner == profile.app && Access::v4IndependentShellOwner == owner &&
            Access::productionDispatch == 1 && Access::fixtureControlWindow &&
            Access::messageTrace && !Access::messageTrace->LostOrCrossThread(), "collection source or host unavailable");
        const auto inputObservation = ole_driver::InspectV4IndependentFileInputs(verified, identity, *payload, source);
        RequireObservation(inputObservation.captured && !inputObservation.resourceUnavailable &&
            inputObservation.candidateId == raw.id, "collection file input observation unavailable");
        const auto& fixture = payload->files.front();
        RequireObservation(fixture.created && fixture.identityObserved && fixture.bytesObserved && fixture.copyObservationHandle,
            "collection copied source lease unavailable");
        RequireObservation(source.HasConstructionShell() && source.ConstructionPaths().size() == 1,
            "collection requires one real Shell source path");
        constexpr char digits[] = "0123456789abcdef";
        const auto appendByte = [&](std::string& out, unsigned byte) {
            out.push_back(digits[(byte >> 4) & 15]); out.push_back(digits[byte & 15]);
        };
        std::string pathValue = "utf16le_hex_", bytesValue = "hex_";
        static_assert(sizeof(wchar_t) == 2, "Windows UTF-16 code units required");
        for (const auto unit : fixture.path.native())
        {
            const auto value = static_cast<std::uint16_t>(unit);
            appendByte(pathValue, value & 0xff); appendByte(pathValue, value >> 8);
        }
        for (const auto byte : fixture.bytes) appendByte(bytesValue, std::to_integer<unsigned>(byte));
        RequireObservation(value("V4.file_path.index_0.utf16le") == pathValue &&
            value("V4.file_content.index_0.bytes") == bytesValue,
            "collection source path or bytes differ from candidate");
        std::error_code error;
        const bool sourceMatches = fs::equivalent(source.ConstructionPaths().front(), fixture.path, error);
        RequireObservation(!error && sourceMatches, "collection Shell input does not match its candidate file");
        evidence.candidateId = raw.id; evidence.source = fixture.path; evidence.expectedBytes = fixture.bytes;
        evidence.output = profile.desktop / fixture.path.filename();
        const bool outputExists = fs::exists(evidence.output, error);
        RequireObservation(!error && !outputExists && evidence.output != evidence.source,
            "collection output baseline unavailable");
        sourceFile = std::make_unique<ObservedPhysicalFile>(evidence.source);
        RequireObservation(OwnedProfileDesktopRoot::SameId(sourceFile->Identity(), fixture.identity) &&
            sourceFile->PathStillNamesObject(evidence.source) && sourceFile->HasBytes(evidence.expectedBytes),
            "collection source identity or bytes unavailable");
        evidence.sourceId = sourceFile->Identity();
        const ContainerRow row{"v4_collection", DesktopWidgetType::Collection, DesktopWidgetType::Collection, false, false};
        SeedDestination(profile, row, evidence.existing);
        Wait([&] { return Access::ContainerWorkIdle(*profile.app) && Access::FileDropIdle(*profile.app) &&
            Access::LayoutSaveIdle(*profile.app); });
        initialWidget = ContainerWidget(*profile.app, L"ingress-root");
        evidence.initialMembers = Access::ContainerMembers(*profile.app, L"ingress-root");
        RequireObservation(SameMembers(evidence.initialMembers, {evidence.existing}), "collection baseline membership unavailable");
        existingFile = std::make_unique<ObservedPhysicalFile>(evidence.existing);
        const std::vector<std::byte> sentinelBytes{std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'},
            std::byte{'u'}, std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}};
        RequireObservation(existingFile->HasBytes(sentinelBytes), "collection existing member bytes unavailable");
        evidence.existingId = existingFile->Identity();
        evidence.initialRevision = Access::LayoutRevision(*profile.app);
        RequireObservation(source.starts == 0 && source.ends == 0, "collection source operation already active");
        traceStart = Access::messageTrace->Observations().size();
        initialCompletion = {true, static_cast<std::uint64_t>(source.starts), static_cast<std::uint64_t>(source.ends)};
        evidence.inputObserved = true;
    }

    void BeforeOleCall(const drag_global::ole_driver::Input& input) const
    {
        if (input.kind == drag_global::ole_driver::Kind::drop)
            RequireObservation(Access::Target(*profile.app) == Access::MemberContainer(*profile.app, L"ingress-root"),
                "collection target was not observed at Drop");
    }

    void Await(const drag_global::ole_driver::Observation& terminal)
    {
        using namespace drag_global::ole_driver;
        RequireObservation(!awaitStarted_ && terminal.kind == Kind::drop && terminal.returnedEffect && SUCCEEDED(terminal.result) &&
            *terminal.returnedEffect == DROPEFFECT_COPY && Access::messageTrace && !Access::messageTrace->LostOrCrossThread(),
            "collection copy Drop or trace unavailable");
        awaitStarted_ = true;
        evidence.completion = WaitForCompletion(*Access::messageTrace, traceStart, Access::fixtureControlWindow,
            MessageRoute::control_window_proc, terminal, initialCompletion, kShellFileOperationCompletedMessage, 12000,
            [&] { return CompletionHostState{Access::FileDropIdle(*profile.app),
                static_cast<std::uint64_t>(source.starts), static_cast<std::uint64_t>(source.ends)}; },
            [] { Access::ThrowAsyncException(); });
        RequireObservation(evidence.completion->signal == CompletionSignal::completed_messages,
            "collection completion messages unavailable");
        Wait([&] { return Access::ContainerWorkIdle(*profile.app) && Access::FileDropIdle(*profile.app) &&
            Access::LayoutSaveIdle(*profile.app); });
        awaitCompleted_ = true;
    }

private:
    void CaptureResults()
    {
        RequireObservation(evidence.inputObserved && evidence.completion &&
            evidence.completion->signal == drag_global::ole_driver::CompletionSignal::completed_messages,
            "collection completion observation unavailable");
        evidence.finalMembers = Access::ContainerMembers(*profile.app, L"ingress-root");
        evidence.sourcePreserved = sourceFile->PathStillNamesObject(evidence.source) && sourceFile->HasBytes(evidence.expectedBytes);
        const std::vector<std::byte> sentinelBytes{std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'},
            std::byte{'u'}, std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}};
        evidence.existingPreserved = existingFile->PathStillNamesObject(evidence.existing) && existingFile->HasBytes(sentinelBytes);
        evidence.widgetPreserved = WidgetMatches();
        std::error_code error;
        const bool exists = fs::exists(evidence.output, error);
        RequireObservation(!error, "collection output existence unavailable");
        if (exists)
        {
            outputFile = std::make_unique<ObservedPhysicalFile>(evidence.output);
            evidence.outputId = outputFile->Identity();
            evidence.outputBytesMatch = outputFile->HasBytes(evidence.expectedBytes) && !outputFile->SameObject(*sourceFile);
            evidence.membershipMatches = SameMembers(evidence.finalMembers, {evidence.existing, evidence.output});
            evidence.noDesktopDuplicate = Access::FreeDesktopCopies(*profile.app, evidence.output) == 0;
        }
        else { evidence.outputBytesMatch = false; evidence.membershipMatches = false; }
        evidence.finalRevision = Access::LayoutRevision(*profile.app);
        evidence.finalObserved = true;
    }

    void ReloadAndCapture()
    {
        RequireObservation(evidence.finalObserved, "collection final observation unavailable");
        RefreshContainer(profile, true, L"ingress-root", false);
        Wait([&] { return Access::ContainerWorkIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
        evidence.reloadedMembers = Access::ContainerMembers(*profile.app, L"ingress-root");
        evidence.reloadedMembershipMatches = outputFile && outputFile->PathStillNamesObject(evidence.output) &&
            outputFile->HasBytes(evidence.expectedBytes) && SameMembers(evidence.reloadedMembers, {evidence.existing, evidence.output});
        evidence.reloadedRevision = Access::LayoutRevision(*profile.app);
        evidence.reloadedWidgetPreserved = WidgetMatches() && existingFile->PathStillNamesObject(evidence.existing);
        evidence.reloadObserved = true;
    }
public:
    void BindExecution(const std::shared_ptr<drag_global::ole_driver::V4CollectionExecutionKey>& key,
        const std::shared_ptr<drag_global::V4IndependentShellOwner>& sourceOwner,
        const std::shared_ptr<const drag_global::ole_driver::V3HostEpoch>& epoch,
        OwnedProfileDesktopRoot& root)
    {
        RequireObservation(key && !awaitStarted_ && evidence.inputObserved && !evidence.baselineRevalidated &&
            root.profile && root.profile.get() == &profile && root.StillOwned() && profile.app && payload &&
            payload->files.size() == 1 && payload->candidateId == evidence.candidateId &&
            sourceOwner && owner == sourceOwner && !owner.owner_before(sourceOwner) && !sourceOwner.owner_before(owner) &&
            Access::asyncOwner == profile.app && Access::v4IndependentShellOwner == owner &&
            Access::ContainerWorkIdle(*profile.app) && Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app) &&
            evidence.initialRevision && Access::LayoutRevision(*profile.app) == *evidence.initialRevision &&
            source.starts == 0 && source.ends == 0 && sourceFile && existingFile && WidgetMatches() &&
            SameMembers(Access::ContainerMembers(*profile.app, L"ingress-root"), evidence.initialMembers),
            "collection same-run initial model unavailable");
        const std::vector<std::byte> sentinelBytes{std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'},
            std::byte{'u'}, std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}};
        RequireObservation(sourceFile->PathStillNamesObject(evidence.source) && sourceFile->HasBytes(evidence.expectedBytes) &&
            existingFile->PathStillNamesObject(evidence.existing) && existingFile->HasBytes(sentinelBytes),
            "collection same-run source or existing member changed");
        SetLastError(ERROR_SUCCESS);
        const DWORD attributes = GetFileAttributesW(evidence.output.c_str());
        const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        RequireObservation(attributes == INVALID_FILE_ATTRIBUTES && error == ERROR_FILE_NOT_FOUND && root.StillOwned(),
            "collection same-run output baseline unavailable");
        RequireObservation(key->BindBaseline(sourceOwner, epoch, profile.app, static_cast<IDataObject*>(&source),
            payload->candidateId, identity_), "collection same-run baseline binding unavailable");
        evidence.baselineRevalidated = true;
    }
    void CaptureExecution(const std::shared_ptr<drag_global::ole_driver::V4CollectionExecutionKey>& key,
        const std::shared_ptr<drag_global::V4IndependentShellOwner>& sourceOwner,
        const std::shared_ptr<const drag_global::ole_driver::V3HostEpoch>& epoch)
    {
        RequireObservation(key && evidence.baselineRevalidated && awaitCompleted_ && payload &&
            key->BeginCapture(sourceOwner, epoch, profile.app, static_cast<IDataObject*>(&source), payload->candidateId, identity_),
            "collection same-run business capture unavailable");
        try { CaptureResults(); ReloadAndCapture(); }
        catch (const std::exception& error)
        { evidence.unresolved.push_back(std::string("collection_business_snapshot_incomplete:") + error.what()); }
        catch (...) { evidence.unresolved.emplace_back("collection_business_snapshot_incomplete"); }
        auto snapshot = std::make_shared<const V4CollectionCopyEvidence>(evidence);
        RequireObservation(key->Publish(sourceOwner, epoch, profile.app, static_cast<IDataObject*>(&source),
            payload->candidateId, identity_, std::move(snapshot)), "collection same-run business sealing unavailable");
    }
};
}

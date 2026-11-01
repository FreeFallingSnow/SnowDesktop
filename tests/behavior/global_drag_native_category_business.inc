#pragma once

// FileCategories observations preserve its real filtered view and its owning
// widget separately; snapshots do not establish drag outcome or durability.
namespace behavior
{
struct V4NativeCategorySnapshot
{
    std::optional<Access::NativeCategorySourceState> source;
    std::optional<std::size_t> freeCopies;
    std::optional<GridCell> movingCell;
    std::optional<ole_content_output_observation::Evidence> movingBytes, sentinelBytes;
    std::optional<bool> movingIdentityPreserved, sentinelIdentityPreserved;
};
struct V4NativeCategoryEvidence
{
    std::string candidateId;
    fs::path moving, sentinel;
    std::wstring movingKey, sentinelKey;
    std::vector<std::byte> expectedMoving, expectedSentinel;
    std::optional<FILE_ID_INFO> movingId, sentinelId;
    std::optional<Access::NativeCategorySourceGeometry> initialGeometry;
    std::optional<Access::NativeCategorySourceState> initialState;
    V4NativeCategorySnapshot afterDrop, afterReload;
    std::optional<V4NativeDesktopLayoutSnapshot> beforeReloadLayout;
    bool inputObserved = false, baselineRevalidated = false, captureObserved = false, finalObserved = false, reloadObserved = false;
    std::vector<std::string> unresolved{"native_category_other_input_axes_and_auto_collection_changes_unverified",
        "native_category_capture_release_membership_and_layout_causality_unverified",
        "native_category_non_atomic_snapshot_late_work_and_durability_unverified"};
};
struct V4NativeCategoryDeviceObservation;
struct V4NativeCategoryObservation
{
private:
    friend struct V4NativeCategoryDeviceObservation;
    OwnedProfileDesktopRoot& root;
    Profile& profile;
    fs::path moving, sentinel;
    std::vector<std::byte> expectedMoving;
    GridCell expectedLanding;
    std::unique_ptr<ObservedPhysicalFile> movingFile, sentinelFile;
    V4NativeCategoryEvidence evidence;
    bool captured = false, baselineBound = false, captureStarted = false;
    std::uint64_t initialRevision = 0;
    std::shared_ptr<const drag_global::ole_driver::V4NativeCategoryObserverIdentity> identity_{
        new drag_global::ole_driver::V4NativeCategoryObserverIdentity};
    static Profile& BoundProfile(OwnedProfileDesktopRoot& root)
    {
        RequireObservation(root.profile && root.profile->app && root.StillOwned(), "native category root unavailable");
        return *root.profile;
    }
    static std::vector<std::byte> SentinelBytes()
    { return {std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'}, std::byte{'u'},
        std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}}; }
    void Snapshot(V4NativeCategorySnapshot& out)
    {
        out.source = Access::InspectNativeCategoryState(*profile.app);
        out.freeCopies = Access::ObserveFreeDesktopCopies(*profile.app, moving);
        RequireObservation(out.freeCopies.has_value(), "native category desktop identities unavailable");
        if (*out.freeCopies) out.movingCell = FindCopyBusiness(*profile.app, moving).gridCell;
        out.movingBytes = ole_content_output_observation::ExactBytes(moving, expectedMoving);
        out.sentinelBytes = ole_content_output_observation::ExactBytes(sentinel, evidence.expectedSentinel);
        RequireObservation(out.movingBytes->observed && out.sentinelBytes->observed, "native category bytes unavailable");
        out.movingIdentityPreserved = movingFile->PathStillNamesObject(moving) && movingFile->HasBytes(expectedMoving);
        out.sentinelIdentityPreserved = sentinelFile->PathStillNamesObject(sentinel) && sentinelFile->HasBytes(evidence.expectedSentinel);
    }
    void VerifyBaseline() const
    {
        RequireObservation(root.StillOwned() && evidence.initialState && evidence.initialGeometry &&
            Access::NativeDragIdle(*profile.app) && Access::ContainerWorkIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app),
            "native category baseline unavailable");
        const auto state = Access::InspectNativeCategoryState(*profile.app);
        const auto geo = Access::InspectNativeCategorySource(*profile.app, moving);
        const auto& before = *evidence.initialState; const auto& point = *evidence.initialGeometry;
        RequireObservation(state.host == before.host && state.container == before.container && state.ownerThread == before.ownerThread &&
            state.widget.type == DesktopWidgetType::FileCategories && !state.widget.autoCollect &&
            state.widget.id == before.widget.id && state.widget.itemKeys == before.widget.itemKeys &&
            state.activeCategory == before.activeCategory && state.visibleKeys == before.visibleKeys &&
            state.layoutRevision == initialRevision && geo.layoutRevision == state.layoutRevision &&
            geo.host == state.host && geo.container == state.container && geo.ownerThread == state.ownerThread &&
            geo.memberKey == evidence.movingKey && geo.memberKey == point.memberKey &&
            std::count(state.widget.itemKeys.begin(), state.widget.itemKeys.end(), evidence.movingKey) == 1 &&
            std::count(state.widget.itemKeys.begin(), state.widget.itemKeys.end(), evidence.sentinelKey) == 1 &&
            geo.visibleIndex == point.visibleIndex &&
            geo.visibleKeys == point.visibleKeys && geo.activeCategory == point.activeCategory &&
            geo.screen.x == point.screen.x && geo.screen.y == point.screen.y &&
            EqualRect(&geo.memberBounds, &point.memberBounds) && EqualRect(&geo.viewport, &point.viewport) &&
            movingFile->PathStillNamesObject(moving) && movingFile->HasBytes(expectedMoving) &&
            sentinelFile->PathStillNamesObject(sentinel) && sentinelFile->HasBytes(evidence.expectedSentinel),
            "native category baseline changed");
    }
    void ObserveCapture()
    {
        RequireObservation(evidence.initialGeometry &&
            Access::NativeCategoryDragCaptured(*profile.app, moving, evidence.initialGeometry->container),
            "native category actual capture unavailable");
        captured = true; evidence.captureObserved = true;
    }
    void AwaitCompletion() const
    {
        RequireObservation(captured, "native category capture unavailable");
        Wait([&] { return Access::NativeDragIdle(*profile.app) && Access::ContainerWorkIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
    }
public:
    V4NativeCategoryObservation(const V4NativeCategoryObservation&) = delete;
    V4NativeCategoryObservation& operator=(const V4NativeCategoryObservation&) = delete;
    V4NativeCategoryEvidence Evidence() const { return evidence; }
    const GridCell& ExpectedLanding() const noexcept { return expectedLanding; }
    V4NativeCategoryObservation(OwnedProfileDesktopRoot& ownedRoot, fs::path path, std::vector<std::byte> bytes, GridCell landing,
        const drag_global::V4FamilyValidatedContext& verified, const drag_global::event_graph::V4FamilyIdentityContext& identity)
        : root(ownedRoot), profile(BoundProfile(ownedRoot)), moving(std::move(path)), sentinel(profile.desktop / L"Sentinel.txt"),
          expectedMoving(std::move(bytes)), expectedLanding(std::move(landing))
    {
        using namespace drag_global;
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto value = [&](std::string_view axis) { return Value(verified.Axes(), verified.Coordinate(), axis); };
        RequireObservation(verified.Status() == V4CoordinateStatus::coordinate_ready && identity.Status() == V4FamilyStatus::schema_only &&
            !verified.TaskBActive() && verified.FamilyBinding() == identity.FamilyBinding() && !raw.resourceUnavailable &&
            raw.errors.empty() && !raw.id.empty() && value("T01") == "internal_native" &&
            value("source.E02") == "desktop_files" && value("source.E03") == "widget_body" && value("C05") == "off" &&
            value("target.E01") == "free_desktop" && value("target.E02") == "none" && value("H01.region") == "empty_slot",
            "native category candidate unavailable");
        evidence.candidateId = raw.id; evidence.moving = moving; evidence.sentinel = sentinel;
        evidence.expectedMoving = expectedMoving; evidence.expectedSentinel = SentinelBytes();
        Wait([&] { return Access::NativeDragIdle(*profile.app) && Access::ContainerWorkIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
        movingFile = std::make_unique<ObservedPhysicalFile>(moving); sentinelFile = std::make_unique<ObservedPhysicalFile>(sentinel);
        RequireObservation(!movingFile->SameObject(*sentinelFile) && movingFile->PathStillNamesObject(moving) &&
            sentinelFile->PathStillNamesObject(sentinel) && movingFile->HasBytes(expectedMoving) && sentinelFile->HasBytes(evidence.expectedSentinel),
            "native category physical baseline unavailable");
        evidence.movingId = movingFile->Identity(); evidence.sentinelId = sentinelFile->Identity();
        evidence.initialGeometry = Access::InspectNativeCategorySource(*profile.app, moving);
        evidence.initialState = Access::InspectNativeCategoryState(*profile.app);
        const auto& state = *evidence.initialState; const auto& geometry = *evidence.initialGeometry;
        const auto& sentinelItem = Find(*profile.app, sentinel);
        RequireObservation(!state.widget.autoCollect && state.widget.type == DesktopWidgetType::FileCategories &&
            state.host == geometry.host && state.container == geometry.container && state.ownerThread == geometry.ownerThread &&
            state.layoutRevision == geometry.layoutRevision && state.activeCategory == geometry.activeCategory &&
            state.visibleKeys == geometry.visibleKeys && !sentinelItem.selected &&
            std::count(state.widget.itemKeys.begin(), state.widget.itemKeys.end(), geometry.memberKey) == 1 &&
            std::count(state.widget.itemKeys.begin(), state.widget.itemKeys.end(), sentinelItem.layoutKey) == 1 &&
            geometry.memberKey != sentinelItem.layoutKey, "native category actual membership unavailable");
        evidence.movingKey = geometry.memberKey; evidence.sentinelKey = sentinelItem.layoutKey;
        RequireObservation(Access::Save(*profile.app), "native category initial layout save unavailable");
        Wait([&] { return Access::LayoutSaveIdle(*profile.app); });
        evidence.initialGeometry = Access::InspectNativeCategorySource(*profile.app, moving);
        evidence.initialState = Access::InspectNativeCategoryState(*profile.app);
        initialRevision = evidence.initialState->layoutRevision; evidence.inputObserved = true;
        VerifyBaseline();
    }
    void BindExecution(const std::shared_ptr<drag_global::ole_driver::V4NativeCategoryExecutionKey>& key,
        const std::shared_ptr<drag_global::ole_driver::NativeHostEpoch>& epoch)
    {
        RequireObservation(key && !baselineBound && !captureStarted && !captured && root.StillOwned() &&
            Access::SameNativeHostEpoch(*profile.app, epoch), "native category execution baseline unavailable");
        VerifyBaseline();
        RequireObservation(key->BindBaseline(epoch, profile.app, evidence.candidateId, identity_), "native category key unavailable");
        baselineBound = true; evidence.baselineRevalidated = true;
    }
    void CaptureExecution(const std::shared_ptr<drag_global::ole_driver::V4NativeCategoryExecutionKey>& key,
        const std::shared_ptr<drag_global::ole_driver::NativeHostEpoch>& epoch)
    {
        RequireObservation(key && baselineBound && !captureStarted && captured && root.StillOwned() &&
            key->BeginCapture(epoch, profile.app, evidence.candidateId, identity_), "native category capture unavailable");
        captureStarted = true;
        try
        {
            Snapshot(evidence.afterDrop); evidence.finalObserved = true;
            evidence.beforeReloadLayout = CaptureV4NativeDesktopLayout(root, 4u * 1024u * 1024u);
            Access::PerturbBeforeDiskRead(*profile.app); Access::RequestAsync(*profile.app, true);
            Wait([&] { return Access::ContainerWorkIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
            Snapshot(evidence.afterReload); evidence.reloadObserved = true;
        }
        catch (const std::exception& error) { evidence.unresolved.push_back(std::string("native_category_snapshot_incomplete:") + error.what()); }
        catch (...) { evidence.unresolved.emplace_back("native_category_snapshot_incomplete"); }
        auto snapshot = std::make_shared<const V4NativeCategoryEvidence>(evidence);
        RequireObservation(key->Publish(epoch, profile.app, evidence.candidateId, identity_, std::move(snapshot)),
            "native category sealing unavailable");
    }
};

struct V4NativeCategoryDeviceObservation
{
    V4NativeCategoryObservation& business;
    drag_global::EventProgram program;
    std::shared_ptr<drag_global::ole_driver::NativeHostEpoch> epoch;
    POINT startScreen{}, targetScreen{};
    std::size_t traceStart = 0, beforeEnd = (std::numeric_limits<std::size_t>::max)();
    bool initialHover = false, ended = false;
    std::vector<bool> seen;
    std::vector<std::optional<std::size_t>> moveClasses;
    std::map<std::size_t, snowdesktop::private_pointer::QueuedEventIdentity> classTokens;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::size_t> tokenClasses;
    std::optional<snowdesktop::private_pointer::QueuedEventIdentity> lastQueued;

    V4NativeCategoryDeviceObservation(V4NativeCategoryObservation& observer,
        const drag_global::event_graph::V4FamilyIdentityContext& identity,
        const drag_global::V4FamilyValidatedContext& verified, const std::vector<bool>& present,
        const drag_global::event_graph::V4NativeJoinedGraphVariant& graph,
        std::shared_ptr<drag_global::ole_driver::NativeHostEpoch> host, POINT start, POINT target)
        : business(observer), program(verified.TransferProgram()), epoch(std::move(host)), startScreen(start), targetScreen(target),
          seen(program.steps.size(), false)
    {
        RequireHost();
        const auto classes = drag_global::ole_driver::V4NativeMoveClassEvidence::Inspect(identity, verified, present, graph);
        RequireObservation(classes.IdentityValid() && classes.CompleteMoveBindings() && !classes.ResourceUnavailable() &&
            classes.ByProgramIndex().size() == program.steps.size(), "native category selected move classes unavailable");
        moveClasses = classes.ByProgramIndex();
        traceStart = Access::messageTrace->Observations().size();
    }
    void RequireHost() const
    {
        RequireObservation(Access::SameNativeHostEpoch(*business.profile.app, epoch),
            "native category same-host epoch unavailable");
    }
    void BeforeBegin() const
    {
        RequireHost();
        RequireObservation(Access::NativeDragIdle(*business.profile.app) && Access::ContainerWorkIdle(*business.profile.app) &&
            Access::LayoutSaveIdle(*business.profile.app), "native category pre-input host unavailable");
        business.VerifyBaseline();
        const auto client = Access::InspectNativeCategorySource(*business.profile.app, business.moving).client;
        const auto screen = Access::ExternalPoint(client);
        RequireObservation(screen.x == startScreen.x && screen.y == startScreen.y,
            "native category candidate coordinates changed before input");
        const auto landing = Access::InspectNativeLanding(*business.profile.app, business.expectedLanding);
        RequireObservation(landing.screenCenter.x == targetScreen.x && landing.screenCenter.y == targetScreen.y &&
            landing.layoutRevision == business.initialRevision, "native category landing baseline changed before input");
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
        RequireObservation(!log.failed && log.session && log.ownerThread == GetCurrentThreadId() &&
            state.error.empty() && state.pendingEvents == 0 &&
            std::none_of(state.keys.begin(), state.keys.end(), [](bool pressed) { return pressed; }),
            "native category device observation unavailable");
        if (phase == NativeDevicePhase::session_end)
        {
            RequireObservation(!queued && !expected && index && *index < program.steps.size() && !state.enabled &&
                program.steps[*index].kind == "end_pointer_session" && state.buttons == 0 && initialHover && !ended,
                "native category end state unavailable");
            ended = true;
            return;
        }
        RequireObservation(queued && expected && queued->ownerThread == log.ownerThread && queued->session == log.session &&
            queued->event && lastQueued && queued->ownerThread == lastQueued->ownerThread &&
            queued->session == lastQueued->session && queued->event == lastQueued->event && Access::messageTrace,
            "native category queued input identity unavailable");
        const DeliveryObservation* primary = nullptr;
        for (const auto& event : log.entries)
        {
            if (event.kind != DeliveryObservationKind::begin || !event.identity.queuedEvent || event.identity.forwarded ||
                event.identity.ownerThread != queued->ownerThread || event.identity.session != queued->session ||
                event.identity.event != queued->event) continue;
            RequireObservation(!primary, "native category queued input delivery ambiguous");
            primary = &event;
        }
        RequireObservation(primary && primary->window == Access::completionWindow,
            "native category queued input delivery unavailable");
        POINT client = expected->screen;
        RequireObservation(ScreenToClient(Access::completionWindow, &client) != FALSE &&
            client.x == expected->clientBeforeDispatch.x && client.y == expected->clientBeforeDispatch.y &&
            client.x >= (std::numeric_limits<short>::min)() && client.x <= (std::numeric_limits<short>::max)() &&
            client.y >= (std::numeric_limits<short>::min)() && client.y <= (std::numeric_limits<short>::max)(),
            "native category input coordinate observation unavailable");
        POINT roundTrip = client;
        RequireObservation(ClientToScreen(Access::completionWindow, &roundTrip) != FALSE &&
            roundTrip.x == expected->screen.x && roundTrip.y == expected->screen.y,
            "native category input coordinate mapping is not reversible");
        const auto point = MAKELPARAM(static_cast<short>(client.x), static_cast<short>(client.y));
        RequireObservation(primary->message == expected->message && primary->wParam == expected->buttons && primary->lParam == point,
            "native category input differs from requested delivery");
        const auto evidence = InspectNativeDelivery(*Access::messageTrace, traceStart, &log, primary->identity,
            primary->window, MessageRoute::desktop_window_proc, expected->message, expected->buttons, point);
        RequireObservation(evidence.matchedMessageAndPermit, "native category production window return unavailable");
        if (phase == NativeDevicePhase::initial_hover)
        {
            RequireObservation(!index && !initialHover && state.enabled && state.buttons == 0 &&
                expected->screen.x == startScreen.x && expected->screen.y == startScreen.y,
                "native category initial hover unavailable");
            initialHover = true;
        }
        else
        {
            RequireObservation(initialHover && index && *index < seen.size() && !seen[*index],
                "native category program input identity unavailable");
            const auto& kind = program.steps[*index].kind;
            if (kind == "source_start")
                RequireObservation(expected->message == WM_LBUTTONDOWN && expected->buttons == MK_LBUTTON &&
                    expected->screen.x == startScreen.x && expected->screen.y == startScreen.y,
                    "native category press input unavailable");
            else if (kind == "release")
                RequireObservation(expected->message == WM_LBUTTONUP && expected->buttons == 0 &&
                    expected->screen.x == targetScreen.x && expected->screen.y == targetScreen.y,
                    "native category release input unavailable");
            else
            {
                RequireObservation(expected->message == WM_MOUSEMOVE && expected->buttons == MK_LBUTTON,
                    "native category movement input unavailable");
                RequireObservation(moveClasses[*index].has_value(), "native category movement graph class unavailable");
                const auto classId = *moveClasses[*index];
                const auto [classEntry, classInserted] = classTokens.emplace(classId, *queued);
                RequireObservation(classInserted || (classEntry->second.ownerThread == queued->ownerThread &&
                    classEntry->second.session == queued->session && classEntry->second.event == queued->event),
                    "native category shared movement class has different queued tokens");
                const auto [tokenEntry, tokenInserted] = tokenClasses.emplace(std::make_pair(queued->session, queued->event), classId);
                RequireObservation(tokenInserted || tokenEntry->second == classId,
                    "native category distinct movement classes share a queued token");
            }
            seen[*index] = true;
        }
    }
    bool ProductionInputEntered() const
    {
        RequireHost();
        const auto& events = Access::messageTrace->Observations();
        RequireObservation(traceStart <= events.size(), "native category input trace baseline unavailable");
        RequireObservation(lastQueued.has_value(), "native category pre-dispatch token unavailable");
        const bool queuedInput = std::any_of(events.begin() + static_cast<std::ptrdiff_t>(traceStart), events.end(), [&](const auto& event) {
            return event.window == Access::completionWindow && event.route == drag_global::ole_driver::MessageRoute::desktop_window_proc &&
                event.enteredThread == GetCurrentThreadId() && event.inputLParam && event.delivery.queuedEvent &&
                !event.delivery.forwarded && event.delivery.ownerThread == lastQueued->ownerThread &&
                event.delivery.session == lastQueued->session && event.delivery.event == lastQueued->event && event.delivery.delivery &&
                (event.message == WM_MOUSEMOVE || event.message == WM_LBUTTONDOWN || event.message == WM_LBUTTONUP);
        });
        if (queuedInput) return true;
        if (beforeEnd != (std::numeric_limits<std::size_t>::max)())
        {
            RequireObservation(beforeEnd <= events.size(), "native category cleanup trace baseline unavailable");
            const bool cleanupInput = std::any_of(events.begin() + static_cast<std::ptrdiff_t>(beforeEnd), events.end(), [&](const auto& event) {
                return event.window == Access::completionWindow && event.route == drag_global::ole_driver::MessageRoute::desktop_window_proc &&
                    event.enteredThread == GetCurrentThreadId() && !event.delivery.queuedEvent && !event.delivery.forwarded &&
                    event.delivery.ownerThread == lastQueued->ownerThread && event.delivery.session == lastQueued->session &&
                    event.delivery.event == 0 && event.delivery.delivery;
            });
            if (cleanupInput) return true;
            throw drag_global::ole_driver::ObservationUnavailable("native category cleanup entry cannot be excluded");
        }
        return false;
    }
    void BeforeQueued(drag_global::ole_driver::NativeDevicePhase phase, std::optional<std::size_t> index,
        const snowdesktop::private_pointer::QueuedEventIdentity& token)
    {
        RequireHost();
        RequireObservation(token.ownerThread == GetCurrentThreadId() && token.session && token.event &&
            ((phase == drag_global::ole_driver::NativeDevicePhase::initial_hover && !index) ||
                (phase == drag_global::ole_driver::NativeDevicePhase::program_input && index && *index < program.steps.size())),
            "native category pre-dispatch token unavailable");
        lastQueued = token;
    }
    void BeforeEnd(const snowdesktop::private_pointer::State& state,
        const snowdesktop::private_pointer::DeliveryObservationLog& log)
    {
        RequireHost();
        RequireObservation(initialHover && !ended && lastQueued && !log.failed && log.session == lastQueued->session &&
            log.ownerThread == lastQueued->ownerThread && beforeEnd == (std::numeric_limits<std::size_t>::max)() &&
            state.enabled && !state.closing && state.pendingEvents == 0 && state.buttons == 0 && !state.captured &&
            state.point.x == targetScreen.x && state.point.y == targetScreen.y,
            "native category released pointer boundary unavailable");
        beforeEnd = Access::messageTrace->Observations().size();
    }
    void Ended() const
    {
        RequireHost();
        const auto& events = Access::messageTrace->Observations();
        RequireObservation(ended && beforeEnd != (std::numeric_limits<std::size_t>::max)() && beforeEnd <= events.size(),
            "native category pointer end boundary unavailable");
        bool cancel = false, leave = false;
        for (std::size_t i = beforeEnd; i < events.size(); ++i)
        {
            const auto& event = events[i];
            if (event.window != Access::completionWindow || event.route != drag_global::ole_driver::MessageRoute::desktop_window_proc ||
                !event.returned || event.enteredThread != GetCurrentThreadId() || event.returnedThread != event.enteredThread) continue;
            if (!cancel && event.message == WM_CANCELMODE) cancel = true;
            else if (cancel && event.message == WM_MOUSELEAVE) leave = true;
        }
        RequireObservation(cancel && leave, "native category pointer-end messages unavailable");
    }
    drag_global::ole_driver::NativeHostBoundaries Boundaries()
    {
        return {
            [this] { BeforeBegin(); },
            [this](auto phase, auto index, const auto& state, const auto& log, auto queued, auto expected) {
                Observe(phase, index, state, log, queued, expected); },
            [this](const auto&) { RequireHost(); business.ObserveCapture(); RequireHost(); },
            [this](const auto&) { RequireHost(); business.AwaitCompletion(); RequireHost(); },
            [this](const auto&, const auto& state, const auto& log) { BeforeEnd(state, log); },
            [this](const auto&) { Ended(); },
            [](const auto&) { throw drag_global::ole_driver::ObservationUnavailable("native category requires sealed execution observer"); },
            [this] { RequireHost(); Access::ThrowAsyncException(); },
            [this] { return ProductionInputEntered(); },
            [this](auto phase, auto index, const auto& token) { BeforeQueued(phase, index, token); }};
    }
};
}

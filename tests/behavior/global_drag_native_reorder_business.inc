#pragma once

// An actual same-Collection gesture with a distinct target member. The
// proposed point is input only; the production hit and commit are observed
// separately and no callback manufactures a reorder.
namespace behavior
{
struct V4NativeCollectionReorderSnapshot
{
    std::vector<std::wstring> itemKeys;
    std::optional<bool> sourcePresent, manualOrder, expectedOrderObserved;
    std::optional<ole_content_output_observation::Evidence> movingBytes, targetBytes;
    std::optional<bool> movingIdentityPreserved, targetIdentityPreserved;
    std::optional<std::uint64_t> layoutRevision;
};
struct V4NativeCollectionReorderEvidence
{
    std::string candidateId;
    fs::path moving, target;
    std::vector<std::byte> expectedMoving, expectedTarget;
    std::optional<FILE_ID_INFO> movingId, targetId;
    std::optional<Access::NativeCollectionReorderGeometry> initialGeometry;
    V4NativeCollectionReorderSnapshot afterDrop, afterReload;
    std::optional<V4NativeDesktopLayoutSnapshot> beforeReloadLayout;
    bool inputObserved = false, baselineRevalidated = false, captureObserved = false, routeObserved = false;
    bool finalObserved = false, reloadObserved = false;
    std::vector<std::string> unresolved{"native_reorder_target_slot_and_commit_causality_unverified",
        "native_reorder_non_atomic_snapshot_reentry_late_work_and_durability_unverified"};
};
struct V4NativeCollectionReorderDeviceObservation;
struct V4NativeCollectionReorderObservation
{
private:
    friend struct V4NativeCollectionReorderDeviceObservation;
    OwnedProfileDesktopRoot& root;
    Profile& profile;
    fs::path moving, target;
    std::vector<std::byte> expectedMoving, expectedTarget;
    std::unique_ptr<ObservedPhysicalFile> movingFile, targetFile;
    V4NativeCollectionReorderEvidence evidence;
    bool captured = false, baselineBound = false, captureStarted = false;
    std::uint64_t initialRevision = 0;
    std::shared_ptr<const drag_global::ole_driver::V4NativeReorderObserverIdentity> identity_{
        new drag_global::ole_driver::V4NativeReorderObserverIdentity};
    static Profile& BoundProfile(OwnedProfileDesktopRoot& root)
    {
        RequireObservation(root.profile && root.profile->app && root.StillOwned(), "native reorder root unavailable");
        return *root.profile;
    }
    static std::vector<std::byte> SentinelBytes()
    { return {std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'}, std::byte{'u'},
        std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}}; }
    void VerifyBaseline() const
    {
        RequireObservation(root.StillOwned() && evidence.initialGeometry && Access::NativeDragIdle(*profile.app) &&
            Access::ContainerWorkIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app), "native reorder baseline unavailable");
        const auto now = Access::InspectNativeCollectionReorderGeometry(*profile.app, moving, target);
        const auto& before = *evidence.initialGeometry;
        const auto& widget = ContainerWidget(*profile.app, L"native-origin");
        RequireObservation(now.host == before.host && now.container == before.container && now.ownerThread == before.ownerThread &&
            now.movingKey == before.movingKey && now.targetKey == before.targetKey && now.itemKeys == before.itemKeys &&
            now.movingIndex == before.movingIndex && now.targetIndex == before.targetIndex &&
            now.layoutRevision == initialRevision && now.movingScreen.x == before.movingScreen.x && now.movingScreen.y == before.movingScreen.y &&
            now.targetScreen.x == before.targetScreen.x && now.targetScreen.y == before.targetScreen.y &&
            EqualRect(&now.movingBounds, &before.movingBounds) && EqualRect(&now.targetBounds, &before.targetBounds) &&
            EqualRect(&now.viewport, &before.viewport) && widget.type == DesktopWidgetType::Collection && !widget.autoCollect &&
            widget.contentSortColumn == snowdesktop::list_detail_rules::Column::None &&
            movingFile->PathStillNamesObject(moving) && movingFile->HasBytes(expectedMoving) &&
            targetFile->PathStillNamesObject(target) && targetFile->HasBytes(expectedTarget), "native reorder baseline changed");
    }
    void ObserveCapture()
    {
        RequireObservation(evidence.initialGeometry && Access::NativeCollectionReorderSourceCaptured(*profile.app, moving,
            evidence.initialGeometry->container), "native reorder actual source capture unavailable");
        captured = true; evidence.captureObserved = true;
    }
    void ObserveRoute()
    {
        RequireObservation(captured && evidence.initialGeometry && Access::NativeCollectionReorderRouteObserved(*profile.app,
            moving, evidence.initialGeometry->container), "native reorder actual same-source route unavailable");
        evidence.routeObserved = true;
    }
    void AwaitCompletion() const
    {
        RequireObservation(captured && evidence.routeObserved, "native reorder source or route unavailable");
        Wait([&] { return Access::NativeDragIdle(*profile.app) && Access::ContainerWorkIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
    }
    void Snapshot(V4NativeCollectionReorderSnapshot& out)
    {
        const auto& widget = ContainerWidget(*profile.app, L"native-origin");
        out.sourcePresent = widget.type == DesktopWidgetType::Collection;
        out.itemKeys = widget.itemKeys;
        out.manualOrder = widget.contentSortColumn == snowdesktop::list_detail_rules::Column::None;
        if (evidence.initialGeometry)
            out.expectedOrderObserved = widget.itemKeys == std::vector<std::wstring>{evidence.initialGeometry->movingKey, evidence.initialGeometry->targetKey};
        out.layoutRevision = Access::LayoutRevision(*profile.app);
        out.movingBytes = ole_content_output_observation::ExactBytes(moving, expectedMoving);
        out.targetBytes = ole_content_output_observation::ExactBytes(target, expectedTarget);
        RequireObservation(out.movingBytes->observed && out.targetBytes->observed, "native reorder bytes unavailable");
        out.movingIdentityPreserved = movingFile->PathStillNamesObject(moving) && movingFile->HasBytes(expectedMoving);
        out.targetIdentityPreserved = targetFile->PathStillNamesObject(target) && targetFile->HasBytes(expectedTarget);
    }
public:
    V4NativeCollectionReorderObservation(const V4NativeCollectionReorderObservation&) = delete;
    V4NativeCollectionReorderObservation& operator=(const V4NativeCollectionReorderObservation&) = delete;
    V4NativeCollectionReorderEvidence Evidence() const { return evidence; }
    V4NativeCollectionReorderObservation(OwnedProfileDesktopRoot& ownedRoot, fs::path path, std::vector<std::byte> bytes,
        const drag_global::V4FamilyValidatedContext& verified, const drag_global::event_graph::V4FamilyIdentityContext& identity)
        : root(ownedRoot), profile(BoundProfile(ownedRoot)), moving(std::move(path)), target(profile.desktop / L"Sentinel.txt"),
          expectedMoving(std::move(bytes)), expectedTarget(SentinelBytes())
    {
        using namespace drag_global;
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto value = [&](std::string_view axis) { return Value(verified.Axes(), verified.Coordinate(), axis); };
        RequireObservation(verified.Status() == V4CoordinateStatus::coordinate_ready && identity.Status() == V4FamilyStatus::schema_only &&
            !verified.TaskBActive() && verified.FamilyBinding() == identity.FamilyBinding() && !raw.resourceUnavailable && raw.errors.empty() &&
            !raw.id.empty() && value("T01") == "internal_native" && value("source.E02") == "collection" &&
            value("target.E02") == "collection" && value("source.E03") == "widget_body" && value("target.E03") == "widget_body" &&
            value("source.E01") == "free_desktop" && value("target.E01") == "free_desktop" &&
            value("P01.primary") == "file" && value("P01.second") == "na" && value("P02.source_actual_count") == "one" &&
            value("G01.button") == "left" && value("G04.termination") == "drop" && value("X03.task_count") == "one" &&
            value("H01.region") == "before_item" && value("H05.position") == "first" &&
            value("H05.indicated_identity") == "unselected_target" && value("H05.surface") == "members" &&
            value("H05.filter") == "unfiltered" && value("H05.source_relation") == "adjacent_original" &&
            value("H05.scroll") == "unchanged" && value("C01") == "manual" && value("C02") == "na" &&
            value("C03") == "manual" && value("C04") == "na" &&
            value("C05") == "off" && value("C06") == "off", "native reorder candidate unavailable");
        evidence.candidateId = raw.id; evidence.moving = moving; evidence.target = target;
        evidence.expectedMoving = expectedMoving; evidence.expectedTarget = expectedTarget;
        movingFile = std::make_unique<ObservedPhysicalFile>(moving); targetFile = std::make_unique<ObservedPhysicalFile>(target);
        RequireObservation(!movingFile->SameObject(*targetFile) && movingFile->PathStillNamesObject(moving) &&
            targetFile->PathStillNamesObject(target) && movingFile->HasBytes(expectedMoving) && targetFile->HasBytes(expectedTarget),
            "native reorder physical baseline unavailable");
        evidence.movingId = movingFile->Identity(); evidence.targetId = targetFile->Identity();
        RequireObservation(Access::Save(*profile.app), "native reorder initial layout save unavailable");
        Wait([&] { return Access::LayoutSaveIdle(*profile.app); });
        evidence.initialGeometry = Access::InspectNativeCollectionReorderGeometry(*profile.app, moving, target);
        const auto& initial = *evidence.initialGeometry;
        RequireObservation(initial.itemKeys.size() == 2 && initial.targetIndex == 0 && initial.movingIndex == 1,
            "native reorder initial order unavailable");
        initialRevision = initial.layoutRevision; evidence.inputObserved = true; VerifyBaseline();
    }
    void BindExecution(const std::shared_ptr<drag_global::ole_driver::V4NativeReorderExecutionKey>& key,
        const std::shared_ptr<drag_global::ole_driver::NativeHostEpoch>& epoch)
    {
        RequireObservation(key && !baselineBound && !captureStarted && !captured && root.StillOwned() &&
            Access::SameNativeHostEpoch(*profile.app, epoch), "native reorder execution baseline unavailable");
        VerifyBaseline();
        RequireObservation(key->BindBaseline(epoch, profile.app, evidence.candidateId, identity_), "native reorder key unavailable");
        baselineBound = true; evidence.baselineRevalidated = true;
    }
    void CaptureExecution(const std::shared_ptr<drag_global::ole_driver::V4NativeReorderExecutionKey>& key,
        const std::shared_ptr<drag_global::ole_driver::NativeHostEpoch>& epoch)
    {
        RequireObservation(key && baselineBound && !captureStarted && captured && evidence.routeObserved && root.StillOwned() &&
            key->BeginCapture(epoch, profile.app, evidence.candidateId, identity_), "native reorder capture unavailable");
        captureStarted = true;
        try
        {
            Snapshot(evidence.afterDrop); evidence.finalObserved = true;
            evidence.beforeReloadLayout = CaptureV4NativeDesktopLayout(root, 4u * 1024u * 1024u);
            Access::PerturbBeforeDiskRead(*profile.app); Access::RequestAsync(*profile.app, true);
            Wait([&] { return Access::ContainerWorkIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
            Snapshot(evidence.afterReload); evidence.reloadObserved = true;
        }
        catch (const std::exception& error) { evidence.unresolved.push_back(std::string("native_reorder_snapshot_incomplete:") + error.what()); }
        catch (...) { evidence.unresolved.emplace_back("native_reorder_snapshot_incomplete"); }
        auto snapshot = std::make_shared<const V4NativeCollectionReorderEvidence>(evidence);
        RequireObservation(key->Publish(epoch, profile.app, evidence.candidateId, identity_, std::move(snapshot)), "native reorder sealing unavailable");
    }
};

struct V4NativeCollectionReorderDeviceObservation
{
    V4NativeCollectionReorderObservation& business;
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

    V4NativeCollectionReorderDeviceObservation(V4NativeCollectionReorderObservation& observer,
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
            classes.ByProgramIndex().size() == program.steps.size(), "native reorder selected move classes unavailable");
        moveClasses = classes.ByProgramIndex();
        traceStart = Access::messageTrace->Observations().size();
    }
    void RequireHost() const
    {
        RequireObservation(Access::SameNativeHostEpoch(*business.profile.app, epoch),
            "native reorder same-host epoch unavailable");
    }
    void BeforeBegin() const
    {
        RequireHost();
        RequireObservation(Access::NativeDragIdle(*business.profile.app) && Access::ContainerWorkIdle(*business.profile.app) &&
            Access::LayoutSaveIdle(*business.profile.app), "native reorder pre-input host unavailable");
        business.VerifyBaseline();
        const auto geometry = Access::InspectNativeCollectionReorderGeometry(*business.profile.app, business.moving, business.target);
        RequireObservation(geometry.movingScreen.x == startScreen.x && geometry.movingScreen.y == startScreen.y &&
            geometry.targetScreen.x == targetScreen.x && geometry.targetScreen.y == targetScreen.y &&
            geometry.layoutRevision == business.initialRevision, "native reorder coordinates changed before input");
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
            "native reorder device observation unavailable");
        if (phase == NativeDevicePhase::session_end)
        {
            RequireObservation(!queued && !expected && index && *index < program.steps.size() && !state.enabled &&
                program.steps[*index].kind == "end_pointer_session" && state.buttons == 0 && initialHover && !ended,
                "native reorder end state unavailable");
            ended = true;
            return;
        }
        RequireObservation(queued && expected && queued->ownerThread == log.ownerThread && queued->session == log.session &&
            queued->event && lastQueued && queued->ownerThread == lastQueued->ownerThread &&
            queued->session == lastQueued->session && queued->event == lastQueued->event && Access::messageTrace,
            "native reorder queued input identity unavailable");
        const DeliveryObservation* primary = nullptr;
        for (const auto& event : log.entries)
        {
            if (event.kind != DeliveryObservationKind::begin || !event.identity.queuedEvent || event.identity.forwarded ||
                event.identity.ownerThread != queued->ownerThread || event.identity.session != queued->session ||
                event.identity.event != queued->event) continue;
            RequireObservation(!primary, "native reorder queued input delivery ambiguous");
            primary = &event;
        }
        RequireObservation(primary && primary->window == Access::completionWindow,
            "native reorder queued input delivery unavailable");
        POINT client = expected->screen;
        RequireObservation(ScreenToClient(Access::completionWindow, &client) != FALSE &&
            client.x == expected->clientBeforeDispatch.x && client.y == expected->clientBeforeDispatch.y &&
            client.x >= (std::numeric_limits<short>::min)() && client.x <= (std::numeric_limits<short>::max)() &&
            client.y >= (std::numeric_limits<short>::min)() && client.y <= (std::numeric_limits<short>::max)(),
            "native reorder input coordinate observation unavailable");
        POINT roundTrip = client;
        RequireObservation(ClientToScreen(Access::completionWindow, &roundTrip) != FALSE &&
            roundTrip.x == expected->screen.x && roundTrip.y == expected->screen.y,
            "native reorder input coordinate mapping is not reversible");
        const auto point = MAKELPARAM(static_cast<short>(client.x), static_cast<short>(client.y));
        RequireObservation(primary->message == expected->message && primary->wParam == expected->buttons && primary->lParam == point,
            "native reorder input differs from requested delivery");
        const auto evidence = InspectNativeDelivery(*Access::messageTrace, traceStart, &log, primary->identity,
            primary->window, MessageRoute::desktop_window_proc, expected->message, expected->buttons, point);
        RequireObservation(evidence.matchedMessageAndPermit, "native reorder production window return unavailable");
        if (phase == NativeDevicePhase::initial_hover)
        {
            RequireObservation(!index && !initialHover && state.enabled && state.buttons == 0 &&
                expected->screen.x == startScreen.x && expected->screen.y == startScreen.y,
                "native reorder initial hover unavailable");
            initialHover = true;
        }
        else
        {
            RequireObservation(initialHover && index && *index < seen.size() && !seen[*index],
                "native reorder program input identity unavailable");
            const auto& kind = program.steps[*index].kind;
            if (kind == "source_start")
                RequireObservation(expected->message == WM_LBUTTONDOWN && expected->buttons == MK_LBUTTON &&
                    expected->screen.x == startScreen.x && expected->screen.y == startScreen.y,
                    "native reorder press input unavailable");
            else if (kind == "release")
                RequireObservation(expected->message == WM_LBUTTONUP && expected->buttons == 0 &&
                    expected->screen.x == targetScreen.x && expected->screen.y == targetScreen.y,
                    "native reorder release input unavailable");
            else
            {
                RequireObservation(expected->message == WM_MOUSEMOVE && expected->buttons == MK_LBUTTON,
                    "native reorder movement input unavailable");
                RequireObservation(moveClasses[*index].has_value(), "native reorder movement graph class unavailable");
                const auto classId = *moveClasses[*index];
                const auto [classEntry, classInserted] = classTokens.emplace(classId, *queued);
                RequireObservation(classInserted || (classEntry->second.ownerThread == queued->ownerThread &&
                    classEntry->second.session == queued->session && classEntry->second.event == queued->event),
                    "native reorder shared movement class has different queued tokens");
                const auto [tokenEntry, tokenInserted] = tokenClasses.emplace(std::make_pair(queued->session, queued->event), classId);
                RequireObservation(tokenInserted || tokenEntry->second == classId,
                    "native reorder distinct movement classes share a queued token");
            }
            seen[*index] = true;
        }
    }
    bool ProductionInputEntered() const
    {
        RequireHost();
        const auto& events = Access::messageTrace->Observations();
        RequireObservation(traceStart <= events.size(), "native reorder input trace baseline unavailable");
        RequireObservation(lastQueued.has_value(), "native reorder pre-dispatch token unavailable");
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
            RequireObservation(beforeEnd <= events.size(), "native reorder cleanup trace baseline unavailable");
            const bool cleanupInput = std::any_of(events.begin() + static_cast<std::ptrdiff_t>(beforeEnd), events.end(), [&](const auto& event) {
                return event.window == Access::completionWindow && event.route == drag_global::ole_driver::MessageRoute::desktop_window_proc &&
                    event.enteredThread == GetCurrentThreadId() && !event.delivery.queuedEvent && !event.delivery.forwarded &&
                    event.delivery.ownerThread == lastQueued->ownerThread && event.delivery.session == lastQueued->session &&
                    event.delivery.event == 0 && event.delivery.delivery;
            });
            if (cleanupInput) return true;
            throw drag_global::ole_driver::ObservationUnavailable("native reorder cleanup entry cannot be excluded");
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
            "native reorder pre-dispatch token unavailable");
        if (phase == drag_global::ole_driver::NativeDevicePhase::program_input && index &&
            program.steps[*index].kind == "release") business.ObserveRoute();
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
            "native reorder released pointer boundary unavailable");
        beforeEnd = Access::messageTrace->Observations().size();
    }
    void Ended() const
    {
        RequireHost();
        const auto& events = Access::messageTrace->Observations();
        RequireObservation(ended && beforeEnd != (std::numeric_limits<std::size_t>::max)() && beforeEnd <= events.size(),
            "native reorder pointer end boundary unavailable");
        bool cancel = false, leave = false;
        for (std::size_t i = beforeEnd; i < events.size(); ++i)
        {
            const auto& event = events[i];
            if (event.window != Access::completionWindow || event.route != drag_global::ole_driver::MessageRoute::desktop_window_proc ||
                !event.returned || event.enteredThread != GetCurrentThreadId() || event.returnedThread != event.enteredThread) continue;
            if (!cancel && event.message == WM_CANCELMODE) cancel = true;
            else if (cancel && event.message == WM_MOUSELEAVE) leave = true;
        }
        RequireObservation(cancel && leave, "native reorder pointer-end messages unavailable");
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
            [](const auto&) { throw drag_global::ole_driver::ObservationUnavailable("native reorder requires sealed execution observer"); },
            [this] { RequireHost(); Access::ThrowAsyncException(); },
            [this] { return ProductionInputEntered(); },
            [this](auto phase, auto index, const auto& token) { BeforeQueued(phase, index, token); }};
    }
};
}

void TestRecoverableFailurePresentation()
{
    using namespace snowdesktop::operation_feedback;
    using namespace snowdesktop::taskbar_hook;
    SharedState state;
    state.explorerProcessId = 12;
    state.status = kStatusFailed;
    state.lastError = ERROR_INVALID_WINDOW_HANDLE;
    Snapshot snapshot;
    snapshot.ownerProcessId = 34;
    snapshot.enabled = true;
    snapshot.appearanceEnabled = true;
    const AppearanceFailureKey key{34, 12, ERROR_INVALID_WINDOW_HANDLE};
    std::vector<Failure> pending;
    SetReporter([&](const Failure& failure) { pending.push_back(failure); });
    Report({"app.settings.taskbar_connect_failed", {}, ERROR_INVALID_WINDOW_HANDLE,
        true, [&] { return key.Matches(state, snapshot); }});
    SetReporter({});
    Expect(pending.size() == 1, "a recoverable failure is queued before presentation");
    // The actual worker has recovered before the UI drains its error queue.
    state.status = kStatusApplied;
    state.lastError = ERROR_SUCCESS;
    unsigned shown = 0;
    Expect(!PresentIfCurrent(pending.front(), [&] { ++shown; }) && shown == 0,
        "recovered appearance cancels the queued error before any dialog is presented");
    state.status = kStatusFailed;
    state.lastError = ERROR_INVALID_WINDOW_HANDLE;
    Expect(PresentIfCurrent(pending.front(), [&] { ++shown; }) && shown == 1,
        "a still-current failure remains visible instead of being globally muted");
    state.status = kStatusInjecting;
    state.lastError = ERROR_SUCCESS;
    Expect(ShouldPresent(pending.front()), "retrying without recovery must not erase a genuine failure hint");
    state.status = kStatusApplied;
    Expect(!ShouldPresent(pending.front()), "the displayed recoverable failure becomes obsolete after recovery");
    state.status = kStatusFailed;
    state.lastError = ERROR_INVALID_WINDOW_HANDLE;
    snapshot.ownerProcessId = 35;
    Expect(!ShouldPresent(pending.front()), "a later host cannot receive an old host's failure");
    snapshot.ownerProcessId = 34;
    state.lastError = ERROR_ACCESS_DENIED;
    Expect(!ShouldPresent(pending.front()), "a different current error invalidates the queued failure");
    state.lastError = ERROR_INVALID_WINDOW_HANDLE;
    snapshot.appearanceEnabled = false;
    Expect(!ShouldPresent(pending.front()), "turning off appearance invalidates its queued failure");
    Failure ordinary{"app.operation.fileFailed", L"persistent file operation", ERROR_ACCESS_DENIED};
    Expect(PresentIfCurrent(ordinary, [&] { ++shown; }) && shown == 2,
        "ordinary operation failures retain their existing presentation behavior");
}

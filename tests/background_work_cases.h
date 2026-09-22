// Included by slot_runtime_contract_tests.cpp. These gates replace only the
// uninterruptible provider edge, using the same scheduler as production icons,
// folder reads, clipboard reads and app indexing.
void TestDockLocalIconsBypassShell()
{
    struct Gate
    {
        HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~Gate() { CloseHandle(entered); CloseHandle(release); }
    };
    auto gate = std::make_shared<Gate>();
    snowdesktop::dock_icon_work::Work work(1);
    bool first = false, next = false, refined = false;
    int stale = 0;
    work.Submit(L"dock:slow", [] { return 11; }, [gate] {
        SetEvent(gate->entered);
        WaitForSingleObject(gate->release, 10000);
        return 12;
    }, [&](int value, bool shell) {
        if (shell) ++stale;
        else first = value == 11;
    }, nullptr, 0);
    const auto wait = [&](const auto& done) {
        const auto deadline = GetTickCount64() + 2000;
        while (!done() && GetTickCount64() < deadline) { work.Drain(); SwitchToThread(); }
        return done();
    };
    Check(wait([&] { return first && WaitForSingleObject(gate->entered, 0) == WAIT_OBJECT_0; }),
        "Dock local pixels arrive while its Shell refinement is blocked");
    work.Submit(L"dock:slow", [] { return 99; }, [] { return 99; },
        [&](int, bool) { ++stale; }, nullptr, 0);
    work.Submit(L"dock:next", [] { return 21; }, [] { return 22; },
        [&](int value, bool shell) { if (shell) refined = value == 22; else next = value == 21; }, nullptr, 0);
    Check(wait([&] { return next; }) && !refined && stale == 0,
        "another Dock item gets first pixels with Shell occupied, without resubmitting the finished local stage");
    work.Cancel(L"dock:slow");
    SetEvent(gate->release);
    Check(wait([&] { return refined; }) && stale == 0,
        "late cancelled Shell results are retired while unrelated refinement remains deliverable");
    work.Submit(L"dock:closed", [] { return 1; }, [] { return 2; },
        [&](int, bool) { ++stale; }, nullptr, 0);
    work.Stop();
    work.Drain();
    Check(stale == 0, "closing the Dock scheduler suppresses both local and Shell deliveries");
}

void TestBackgroundShellWorkIsolation()
{
    struct Gate
    {
        HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE returned = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~Gate() { CloseHandle(entered); CloseHandle(release); CloseHandle(returned); }
    };
    const auto pumpUntil = [](const auto& done, snowdesktop::BackgroundWork& work) {
        const auto deadline = GetTickCount64() + 2000;
        while (!done() && GetTickCount64() < deadline)
        {
            work.Drain();
            SwitchToThread();
        }
        return done();
    };
    auto gate = std::make_shared<Gate>();
    auto fast = std::make_shared<Gate>();
    const auto uiThread = GetCurrentThreadId();
    std::atomic<DWORD> providerThread = 0;
    std::atomic<bool> sta = false;
    auto staleLifetime = std::make_shared<int>(1);
    std::weak_ptr<int> staleRetired = staleLifetime;
    int slowApplied = 0, fastApplied = 0;
    snowdesktop::BackgroundWork queue(2, 2);
    Check(queue.Submit(L"blocked", [gate, staleLifetime, &providerThread, &sta] {
        providerThread = GetCurrentThreadId();
        APTTYPE type{}; APTTYPEQUALIFIER qualifier{};
        sta = SUCCEEDED(CoGetApartmentType(&type, &qualifier)) &&
            (type == APTTYPE_STA || type == APTTYPE_MAINSTA);
        SetEvent(gate->entered);
        WaitForSingleObject(gate->release, 5000);
        SetEvent(gate->returned);
        return 41;
    }, [&](int value) { slowApplied = value; }, nullptr, 0), "blocked icon request is accepted");
    Check(WaitForSingleObject(gate->entered, 2000) == WAIT_OBJECT_0,
        "controlled Shell provider enters its uninterruptible wait");
    Check(providerThread != uiThread && sta, "Shell work runs in a separate initialized STA");
    Check(queue.Submit(L"blocked", [] { return 99; }, [&](int) { ++slowApplied; }, nullptr, 0),
        "repainting the same pending icon coalesces rather than adding threads");
    Check(queue.Submit(L"ready", [fast] { SetEvent(fast->returned); return 7; },
        [&](int value) {
            Check(GetCurrentThreadId() == uiThread, "model completion executes only on the UI owner");
            fastApplied = value;
        }, nullptr, 0), "another source can run while the first provider is blocked");
    Check(WaitForSingleObject(fast->returned, 2000) == WAIT_OBJECT_0 && fastApplied == 0,
        "worker completion cannot mutate the UI before its owner drains it");
    Check(!queue.Submit(L"overflow", [] { return 0; }, [](int) {}, nullptr, 0),
        "capacity includes active and completed requests, not only queued jobs");
    Check(pumpUntil([&] { return fastApplied == 7; }, queue) && slowApplied == 0,
        "ready icons become visible without waiting for a blocked source");

    queue.Cancel(L"blocked");
    Check(queue.Submit(L"blocked", [] { return 12; }, [&](int value) { slowApplied = value; }, nullptr, 0),
        "a newer generation can use a cancelled source key");
    Check(pumpUntil([&] { return slowApplied == 12; }, queue), "new generation commits before the old provider returns");
    SetEvent(gate->release);
    Check(WaitForSingleObject(gate->returned, 2000) == WAIT_OBJECT_0,
        "the obsolete provider is released without modifying its data");
    staleLifetime.reset();
    const auto retiredDeadline = GetTickCount64() + 2000;
    while (!staleRetired.expired() && GetTickCount64() < retiredDeadline) SwitchToThread();
    Check(staleRetired.expired(), "the obsolete job has reached its completion fence");
    Check(queue.Submit(L"barrier", [] { return 1; }, [&](int) { fastApplied = 8; }, nullptr, 0),
        "completion barrier is accepted");
    Check(pumpUntil([&] { return fastApplied == 8; }, queue) && slowApplied == 12,
        "late results cannot replace newer source data");

    auto stopGate = std::make_shared<Gate>();
    auto state = std::make_shared<int>(1);
    std::weak_ptr<int> weak = state;
    auto stopping = std::make_unique<snowdesktop::BackgroundWork>(1);
    Check(stopping->Submit(L"closing", [stopGate, state] {
        SetEvent(stopGate->entered);
        WaitForSingleObject(stopGate->release, 5000);
        SetEvent(stopGate->returned);
        return state;
    }, [](auto) { Check(false, "destroyed owner must never receive a completion"); }, nullptr, 0),
        "shutdown scenario submits real mailbox work");
    state.reset();
    Check(WaitForSingleObject(stopGate->entered, 2000) == WAIT_OBJECT_0,
        "shutdown reaches a blocked provider");
    const auto started = GetTickCount64();
    stopping.reset();
    Check(GetTickCount64() - started < 500, "owner destruction never joins an uninterruptible provider");
    SetEvent(stopGate->release);
    Check(WaitForSingleObject(stopGate->returned, 2000) == WAIT_OBJECT_0,
        "abandoned provider can finish after its owner is gone");
    const auto deadline = GetTickCount64() + 2000;
    while (!weak.expired() && GetTickCount64() < deadline) SwitchToThread();
    Check(weak.expired(), "late results and captured resources are reclaimed without a host callback");
}

// Prevent startup membership from waiting for Shell image-list queries, and
// prevent ready bitmaps from returning to placeholders as snapshots catch up.
void TestStartupDesktopMetadataDeferral()
{
    using namespace snowdesktop::shell_refresh;
    Check(LocalDesktopDisplayName(L"C:\\Desktop\\My.App.LnK", false) == L"My.App" &&
        LocalDesktopDisplayName(L"C:\\Desktop\\Website.URL", false) == L"Website" &&
        LocalDesktopDisplayName(L"C:\\Desktop\\notes.txt", false) == L"notes.txt" &&
        LocalDesktopDisplayName(L"C:\\Desktop\\folder.lnk", true) == L"folder.lnk" &&
        LocalDesktopDisplayName(L"C:\\Desktop\\.lnk", false) == L".lnk",
        "startup labels hide shortcut suffixes before Shell returns, without changing ordinary files or directory names");
    MetadataCache cache;
    std::unordered_set<std::wstring> seen;
    FileStamp stamp;
    stamp.modified = {7, 0};
    int queries = 0;
    const auto shell = [&](SHFILEINFOW& info) {
        ++queries;
        info.iIcon = 19;
        wcscpy_s(info.szTypeName, L"Application shortcut");
        return true;
    };
    const auto local = ReadDesktopMetadata(true, L"C:\\app.lnk", L"C:\\APP.LNK",
        stamp, true, &cache, seen, shell);
    Check(queries == 0 && local.iIcon == -1 && local.szTypeName[0] == 0 &&
            cache.desktop.empty() && seen.empty(),
        "local startup items publish an unknown icon without entering Shell or inventing cache metadata");
    const auto full = ReadDesktopMetadata(false, L"C:\\app.lnk", L"C:\\APP.LNK",
        stamp, true, &cache, seen, shell);
    Check(queries == 1 && full.iIcon == 19 && seen.contains(L"C:\\APP.LNK"),
        "the independent authoritative read still resolves Shell metadata");
    const auto cached = ReadDesktopMetadata(false, L"C:\\app.lnk", L"C:\\APP.LNK",
        stamp, true, &cache, seen, shell);
    Check(queries == 1 && cached.iIcon == 19 && cache.hits == 1,
        "ordinary refreshes retain the existing metadata cache");
    ++stamp.modified.dwLowDateTime;
    ReadDesktopMetadata(false, L"C:\\app.lnk", L"C:\\APP.LNK",
        stamp, true, &cache, seen, shell);
    Check(queries == 2, "an overwritten shortcut queries fresh Shell metadata");
}

// Exercise the production startup mailbox with an actual private message-only
// window. A completed read must wake the consumer without a 2-second UI timer.
void TestStartupReadWakesConsumer()
{
    using namespace snowdesktop::shell_refresh;
    struct Window
    {
        HWND value = CreateWindowExW(0, L"STATIC", L"StartupReadTest", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~Window() { if (value) DestroyWindow(value); }
    } window;
    Check(window.value != nullptr, "startup wake fixture owns a private message-only window");
    if (!window.value) return;
    constexpr UINT wake = WM_APP + 199;
    struct Gate
    {
        HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE returned = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~Gate() { CloseHandle(entered); CloseHandle(release); CloseHandle(returned); }
    };
    const auto takeMessage = [&] {
        MSG message{};
        return PeekMessageW(&message, window.value, wake, wake, PM_REMOVE) != FALSE;
    };
    const auto waitMessage = [&] {
        const auto deadline = GetTickCount64() + 1000;
        while (GetTickCount64() < deadline)
        {
            if (takeMessage()) return true;
            MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_POSTMESSAGE, MWMO_INPUTAVAILABLE);
        }
        return false;
    };
    auto gate = std::make_shared<Gate>();
    StartupRead reader([gate](const Request& request, Snapshot& snapshot) {
        for (int i = 0; i < 32; ++i)
        {
            DesktopItem item;
            item.layoutKey = std::to_wstring(i);
            request.publishDesktopItem(item);
            snapshot.desktopItems.push_back(std::move(item));
        }
        SetEvent(gate->entered);
        WaitForSingleObject(gate->release, 5000);
        SetEvent(gate->returned);
        return true;
    });
    Check(reader.Start({}, window.value, wake) &&
        WaitForSingleObject(gate->entered, 2000) == WAIT_OBJECT_0,
        "startup publishes a batch before entering a blocked Shell call");
    Check(waitMessage() && !takeMessage(),
        "a burst of ready startup items coalesces into one consumer wake");
    const auto progress = reader.TakeProgress();
    Check(progress.size() == 32 && !reader.TakeReady(),
        "the wake exposes incremental items while the remaining read is still blocked");
    SetEvent(gate->release);
    const bool notified = waitMessage();
    auto result = reader.TakeReady(std::chrono::milliseconds(1000));
    Check(notified && result && result->desktopComplete && result->desktopItems.size() == 32,
        "final completion wakes the consumer again after it consumed the partial batch");

    auto stoppedGate = std::make_shared<Gate>();
    StartupRead stopped([stoppedGate](const Request&, Snapshot&) {
        SetEvent(stoppedGate->entered);
        WaitForSingleObject(stoppedGate->release, 5000);
        SetEvent(stoppedGate->returned);
        return true;
    });
    Check(stopped.Start({}, window.value, wake) &&
        WaitForSingleObject(stoppedGate->entered, 2000) == WAIT_OBJECT_0,
        "retirement fixture starts before its provider completes");
    stopped.Stop();
    SetEvent(stoppedGate->release);
    Check(WaitForSingleObject(stoppedGate->returned, 2000) == WAIT_OBJECT_0 &&
        !stopped.TakeReady() && !takeMessage(),
        "a retired startup reader never delivers into its former consumer");
}

void TestStartupIconSurvivesMetadataArrival()
{
    using namespace snowdesktop::shell_refresh;
    DesktopItem ready;
    ready.modifiedTime = FILETIME{7, 0};
    ready.fileSize = 42;
    ready.sysIconIndex = -1;
    ready.iconBitmap = CreateBitmap(1, 1, 1, 32, nullptr);
    ready.iconState = IconState::IconReady;
    ready.selected = true;
    ready.gridCell = {L"saved", 2, 3};
    const auto bitmap = ready.iconBitmap;
    Check(bitmap != nullptr, "startup regression owns a real icon bitmap");
    DesktopItem full;
    full.modifiedTime = ready.modifiedTime;
    full.fileSize = ready.fileSize;
    full.sysIconIndex = 19;
    full.typeName = L"Application shortcut";
    PreserveRuntime(full, ready);
    Check(full.iconBitmap == bitmap && !ready.iconBitmap &&
            full.iconState == IconState::IconReady && full.sysIconIndex == 19 &&
            full.selected && full.gridCell.column == 2,
        "late authoritative metadata retains the first bitmap and current placement");
    DesktopItem incremental;
    incremental.modifiedTime = full.modifiedTime;
    incremental.fileSize = full.fileSize;
    PreserveRuntime(incremental, full);
    Check(incremental.iconBitmap == bitmap && incremental.sysIconIndex == 19 &&
            incremental.typeName == L"Application shortcut" && !full.iconBitmap,
        "a later membership-only snapshot cannot erase a ready icon or its Shell metadata");
    DesktopItem overwritten;
    overwritten.modifiedTime = FILETIME{8, 0};
    overwritten.fileSize = 42;
    PreserveRuntime(overwritten, incremental);
    Check(overwritten.iconState == IconState::Loading,
        "unknown image-list indices do not bypass changed-file invalidation");
}

void TestFirstIconsDoNotWaitForDetails()
{
    struct Gate
    {
        HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE returned = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~Gate() { CloseHandle(entered); CloseHandle(release); CloseHandle(returned); }
    };
    auto gate = std::make_shared<Gate>();
    snowdesktop::shell_icon_request::Work work(1, 1);
    bool firstApplied = false;
    int detailApplied = 0;
    Check(work.Submit(true, L"popup:detail", [gate] {
        SetEvent(gate->entered);
        WaitForSingleObject(gate->release, 5000);
        SetEvent(gate->returned);
        return 1;
    }, [&](int) { ++detailApplied; }, nullptr, 0), "detail request enters the production icon scheduler");
    Check(WaitForSingleObject(gate->entered, 2000) == WAIT_OBJECT_0,
        "detail provider occupies every refinement worker");
    Check(work.Submit(false, L"desktop:first", [] { return 42; },
        [&](int value) { firstApplied = value == 42; }, nullptr, 0),
        "another desktop item can request its first bitmap");
    const auto deadline = GetTickCount64() + 2000;
    while (!firstApplied && GetTickCount64() < deadline)
    { work.Drain(); SwitchToThread(); }
    Check(firstApplied && WaitForSingleObject(gate->returned, 0) == WAIT_TIMEOUT,
        "the first bitmap is delivered while all detail workers remain blocked");
    bool cancelledFirstApplied = false;
    work.Submit(false, L"popup:first", [] { return 1; },
        [&](int) { cancelledFirstApplied = true; }, nullptr, 0);
    work.Cancel(L"popup:");
    SetEvent(gate->release);
    Check(WaitForSingleObject(gate->returned, 2000) == WAIT_OBJECT_0,
        "retired detail provider returns without holding the host");
    bool barrier = false;
    work.Submit(true, L"detail-barrier", [] { return 1; },
        [&](int) { barrier = true; }, nullptr, 0);
    const auto retiredDeadline = GetTickCount64() + 2000;
    while (!barrier && GetTickCount64() < retiredDeadline)
    { work.Drain(); SwitchToThread(); }
    Check(barrier && detailApplied == 0 && !cancelledFirstApplied,
        "popup cancellation rejects both first and detail results without cancelling unrelated work");
    work.Stop();
    Check(!work.Submit(false, L"late-first", [] { return 1; }, [](int) {}, nullptr, 0) &&
            !work.Submit(true, L"late-detail", [] { return 1; }, [](int) {}, nullptr, 0),
        "shutdown closes both icon stages");
}

struct ShortcutClassificationGate
{
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE returned = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~ShortcutClassificationGate()
    { CloseHandle(entered); CloseHandle(release); CloseHandle(returned); }
    bool Run()
    {
        SetEvent(entered);
        WaitForSingleObject(release, 10000);
        SetEvent(returned);
        return true;
    }
};

template<class Predicate>
bool DrainIconsUntil(snowdesktop::shell_icon_request::Work& work, Predicate ready)
{
    const auto deadline = GetTickCount64() + 2000;
    while (!ready() && GetTickCount64() < deadline)
    { work.Drain(); SwitchToThread(); }
    return ready();
}

template<class Image, class Classify, class ApplyImage, class ApplyShortcut>
bool SubmitLocalIcon(snowdesktop::shell_icon_request::Work& work, std::wstring key,
    Image image, Classify classify, ApplyImage applyImage, ApplyShortcut applyShortcut,
    HWND window, UINT message)
{
    using Result = decltype(image());
    return work.SubmitFirstWithFallback(std::move(key), std::move(image),
        [] { return Result{}; }, [](const auto&) { return true; },
        std::move(classify), std::move(applyImage), std::move(applyShortcut), window, message);
}

// The actual first-image/classification submission used by QueueIconTask.
// Only Shell providers are substituted; scheduling and result ordering are real.
void TestShortcutClassificationDoesNotGateIcons()
{
    using namespace snowdesktop::shell_icon_request;
    auto gate = std::make_shared<ShortcutClassificationGate>();
    Work work(1, 1, 1);
    DesktopItem item;
    bool firstApplied = false, nextApplied = false, detailApplied = false;
    int classifications = 0;
    SubmitLocalIcon(work, L"desktop:adobe", [] {
        auto pixels = std::make_shared<snowdesktop::BackgroundBitmap>();
        pixels->bitmap = CreateBitmap(1, 1, 1, 32, nullptr);
        return pixels;
    }, [gate] { return gate->Run(); },
        [&](std::shared_ptr<snowdesktop::BackgroundBitmap> pixels) {
            item.iconBitmap = std::exchange(pixels->bitmap, nullptr);
            ApplyPresentation(item, Phase::Phase1, false, false);
            firstApplied = item.iconBitmap != nullptr;
            return firstApplied;
        }, [&](bool application) {
            ApplyPresentation(item, Phase::Shortcut, true, application);
            ++classifications;
        }, nullptr, 0);
    Check(DrainIconsUntil(work, [&] { return firstApplied &&
            WaitForSingleObject(gate->entered, 0) == WAIT_OBJECT_0; }),
        "first bitmap reaches the model before its slow shortcut classifier returns");
    const auto firstBitmap = item.iconBitmap;
    SubmitLocalIcon(work, L"desktop:next", [] { return 42; }, [] { return false; },
        [&](int value) { nextApplied = value == 42; return true; },
        [&](bool) { ++classifications; }, nullptr, 0);
    work.Submit(true, L"desktop:detail", [] { return 1; }, [&](int) {
        ApplyPresentation(item, Phase::Phase2, false, false);
        detailApplied = true;
    }, nullptr, 0);
    Check(DrainIconsUntil(work, [&] { return nextApplied && detailApplied; }) &&
            classifications == 0 && WaitForSingleObject(gate->returned, 0) == WAIT_TIMEOUT,
        "later first bitmaps and full-quality refinement complete while every classification worker is occupied");
    SetEvent(gate->release);
    Check(DrainIconsUntil(work, [&] { return classifications == 2; }),
        "classification catches up independently after its provider is released");
    Check(firstBitmap && item.iconBitmap == firstBitmap &&
            item.iconState == IconState::FullQuality && item.isApplicationShortcut &&
            item.isShortcut && !item.shortcutArrow,
        "late application classification preserves displayed pixels and full quality");
    ApplyPresentation(item, Phase::Phase1, false, false);
    Check(item.isApplicationShortcut && item.isShortcut && !item.shortcutArrow,
        "a bitmap-only refresh retains known shortcut metadata");
    FolderEntry entry;
    entry.iconState = IconState::FullQuality;
    ApplyPresentation(entry, Phase::Shortcut, true, false);
    Check(entry.iconState == IconState::FullQuality && entry.isShortcut &&
            !entry.isApplicationShortcut && entry.shortcutArrow,
        "folder shortcut classification updates the arrow without resetting icon quality");
}

void TestShortcutClassificationCancellation()
{
    using namespace snowdesktop::shell_icon_request;
    Work work(1, 1, 1);
    std::atomic<bool> rejectedClassified = false;
    bool rejected = false;
    SubmitLocalIcon(work, L"stale", [] { return 1; }, [&] {
        rejectedClassified = true; return 1;
    }, [&](int) { rejected = true; return false; }, [](int) {}, nullptr, 0);
    Check(DrainIconsUntil(work, [&] { return rejected; }),
        "a stale first result reaches the host acceptance boundary");
    auto gate = std::make_shared<ShortcutClassificationGate>();
    int cancelledApplied = 0;
    SubmitLocalIcon(work, L"popup:old", [] { return 1; }, [gate] { return gate->Run(); },
        [](int) { return true; }, [&](bool) { ++cancelledApplied; }, nullptr, 0);
    Check(DrainIconsUntil(work, [&] {
        return WaitForSingleObject(gate->entered, 0) == WAIT_OBJECT_0;
    }), "classification is in flight before the popup closes");
    work.Cancel(L"popup:");
    SetEvent(gate->release);
    bool replacementApplied = false;
    SubmitLocalIcon(work, L"popup:old", [] { return 2; }, [] { return 2; },
        [](int value) { return value == 2; },
        [&](int value) { replacementApplied = value == 2; }, nullptr, 0);
    Check(DrainIconsUntil(work, [&] { return replacementApplied; }) &&
            cancelledApplied == 0 && !rejectedClassified,
        "cancelled classification cannot deliver into a reused key and rejected images never schedule classification");

    auto retired = std::make_unique<Work>(1, 1, 1);
    auto destroyedGate = std::make_shared<ShortcutClassificationGate>();
    std::atomic<bool> destroyedApplied = false;
    SubmitLocalIcon(*retired, L"destroyed", [] { return 1; },
        [destroyedGate] { return destroyedGate->Run(); }, [](int) { return true; },
        [&](bool) { destroyedApplied = true; }, nullptr, 0);
    Check(DrainIconsUntil(*retired, [&] {
        return WaitForSingleObject(destroyedGate->entered, 0) == WAIT_OBJECT_0;
    }), "classification enters before its owner is destroyed");
    retired.reset();
    SetEvent(destroyedGate->release);
    Check(WaitForSingleObject(destroyedGate->returned, 2000) == WAIT_OBJECT_0 && !destroyedApplied,
        "destroying the scheduler drops late classification without joining its blocked provider");
    work.Stop();
    Check(!SubmitLocalIcon(work, L"stopped", [] { return 1; }, [] { return 1; },
        [](int) { return true; }, [](int) {}, nullptr, 0),
        "shutdown also rejects the production first-image/classification entry point");
}

// Production fallback routing: all reserved Shell-first workers are blocked,
// yet later local first images must reach the host without waiting for them.
void TestLocalIconsBypassBlockedShellFallback()
{
    using namespace snowdesktop::shell_icon_request;
    Work work(1, 1, 1, 1);
    auto gate = std::make_shared<ShortcutClassificationGate>();
    int slowApplied = 0, slowClassified = 0;
    const auto ready = [](int value) { return value != 0; };
    work.SubmitFirstWithFallback(L"slow", [] { return 0; }, [gate] {
        gate->Run(); return 7;
    }, ready, [] { return 1; }, [&](int value) {
        slowApplied = value; return true;
    }, [&](int value) { slowClassified += value; }, nullptr, 0);
    Check(DrainIconsUntil(work, [&] {
        return WaitForSingleObject(gate->entered, 0) == WAIT_OBJECT_0;
    }), "a local miss enters the bounded Shell fallback lane");
    bool localApplied = false, detailApplied = false;
    int localClassified = 0;
    std::atomic<int> unnecessaryShellCalls = 0;
    work.SubmitFirstWithFallback(L"local", [] { return 42; }, [&] {
        ++unnecessaryShellCalls; return 99;
    }, ready, [] { return 1; }, [&](int value) {
        localApplied = value == 42; return true;
    }, [&](int value) { localClassified += value; }, nullptr, 0);
    work.Submit(true, L"detail", [] { return 8; },
        [&](int value) { detailApplied = value == 8; }, nullptr, 0);
    Check(DrainIconsUntil(work, [&] { return localApplied && localClassified == 1 && detailApplied; }) &&
        slowApplied == 0 && slowClassified == 0 && unnecessaryShellCalls == 0 &&
        WaitForSingleObject(gate->returned, 0) == WAIT_TIMEOUT,
        "local pixels, classification and details continue while every Shell-first worker is stuck");
    SetEvent(gate->release);
    Check(DrainIconsUntil(work, [&] { return slowApplied == 7 && slowClassified == 1; }),
        "a late Shell bitmap delivers once and starts classification only after acceptance");

    auto cancelled = std::make_shared<ShortcutClassificationGate>();
    int staleCallbacks = 0;
    work.SubmitFirstWithFallback(L"popup:old", [] { return 0; }, [cancelled] {
        cancelled->Run(); return 1;
    }, ready, [] { return 1; }, [&](int) { ++staleCallbacks; return true; },
        [&](int) { ++staleCallbacks; }, nullptr, 0);
    Check(DrainIconsUntil(work, [&] {
        return WaitForSingleObject(cancelled->entered, 0) == WAIT_OBJECT_0;
    }), "fallback is running when its popup is closed");
    work.Cancel(L"popup:");
    bool replacement = false, barrier = false;
    work.SubmitFirstWithFallback(L"popup:old", [] { return 2; }, [] { return 0; }, ready,
        [] { return 0; }, [&](int value) { replacement = value == 2; return false; },
        [&](int) { ++staleCallbacks; }, nullptr, 0);
    SetEvent(cancelled->release);
    work.SubmitFirstWithFallback(L"barrier", [] { return 0; }, [] { return 3; }, ready,
        [] { return 0; }, [&](int value) { barrier = value == 3; return false; },
        [&](int) { ++staleCallbacks; }, nullptr, 0);
    Check(DrainIconsUntil(work, [&] { return replacement && barrier; }) && staleCallbacks == 0,
        "cancelled fallback cannot overwrite a reused key or start stale classification");

    auto retired = std::make_unique<Work>(1, 1, 1, 1);
    auto destroyed = std::make_shared<ShortcutClassificationGate>();
    std::atomic<bool> lateApplied = false;
    retired->SubmitFirstWithFallback(L"destroyed", [] { return 0; }, [destroyed] {
        destroyed->Run(); return 1;
    }, ready, [] { return 0; }, [&](int) { lateApplied = true; return true; },
        [&](int) { lateApplied = true; }, nullptr, 0);
    Check(DrainIconsUntil(*retired, [&] {
        return WaitForSingleObject(destroyed->entered, 0) == WAIT_OBJECT_0;
    }), "Shell fallback enters before owner destruction");
    retired.reset();
    SetEvent(destroyed->release);
    Check(WaitForSingleObject(destroyed->returned, 2000) == WAIT_OBJECT_0 && !lateApplied,
        "destruction drops in-flight fallback delivery without retaining the host");
    work.Stop();
    Check(!work.SubmitFirstWithFallback(L"stopped", [] { return 1; }, [] { return 1; }, ready,
        [] { return 0; }, [](int) { return true; }, [](int) {}, nullptr, 0),
        "shutdown rejects local/fallback submissions");
}

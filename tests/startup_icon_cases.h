// Prevent startup membership from waiting for Shell image-list queries, and
// prevent ready bitmaps from returning to placeholders as snapshots catch up.
void TestStartupDesktopMetadataDeferral()
{
    using namespace snowdesktop::shell_refresh;
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

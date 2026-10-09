// Included inside the slot runtime test namespace. Exercise the production
// subscription, event routing and read-selection boundaries without a desktop
// host or real user data. The filesystem reader is replaced only in scope tests.
void TestPopupTargetRebinding()
{
    using namespace snowdesktop::dock_folder_popup_read;
    using namespace snowdesktop::shell_refresh;
    snowdesktop::dock_refresh_cache::Cache<std::wstring> targets;
    const auto old = targets.Read(L"shortcut", L"v1");
    targets.Publish(L"shortcut", old.ticket, L"C:\\A");
    const auto changed = targets.Read(L"shortcut", L"v2");
    std::wstring path = L"C:\\A";
    bool available = true, loading = false;
    const bool pending = TargetPending(false, changed.fresh, changed.sameSourceVersion);
    Check(BindTarget(*changed.value, pending, path, available, loading) &&
        path.empty() && !available && loading,
        "changing a shortcut clears its obsolete directory binding before the replacement target resolves");
    int reads = 0, applies = 0;
    const auto queue = [&](const auto&) { ++reads; };
    const auto apply = [&](const auto&) { ++applies; };
    FolderSnapshot oldListing;
    oldListing.path = L"C:\\A";
    oldListing.complete = true;
    Check(Refresh(path, nullptr, available, loading, queue, apply, pending) && reads == 0 &&
        !Refresh(path, &oldListing, available, loading, queue, apply, pending) &&
        !available && loading && applies == 0,
        "a successful old-directory result cannot re-enable a pending shortcut or publish its files");
    Check(!targets.Publish(L"shortcut", old.ticket, L"C:\\A"),
        "a late target from the previous source version is rejected");
    targets.Publish(L"shortcut", changed.ticket, L"C:\\B");
    const auto current = targets.Read(L"shortcut", L"v2");
    Check(BindTarget(*current.value, TargetPending(false, current.fresh, current.sameSourceVersion),
        path, available, loading) && path == L"C:\\B" && !available && loading,
        "the confirmed replacement binds the open popup without enabling it before directory validation");
    Check(Refresh(path, nullptr, available, loading, queue, apply) && reads == 1 &&
        !Refresh(path, &oldListing, available, loading, queue, apply) && !available,
        "the replacement queues its own read and rejects the old folder after rebinding");
    FolderSnapshot newListing;
    newListing.path = path;
    newListing.complete = true;
    Check(Refresh(path, &newListing, available, loading, queue, apply) && available && !loading && applies == 1,
        "only the confirmed replacement listing enables popup operations");
    Check(!TargetPending(true, false, false) && !TargetPending(false, false, true),
        "direct mappings and unchanged cached source versions keep their independent read path");
}

void TestFolderPopupControlReservation()
{
    using namespace snowdesktop::dock_folder_popup_read;
    using snowdesktop::shell_refresh::FolderSnapshot;
    const std::wstring path = L"C:\\popup";
    bool available = false, loading = false;
    std::size_t count = 0;
    constexpr std::size_t knownCount = 13;
    const auto apply = [&](const auto& result) { count = result.entries.size(); };
    Check(Refresh(path, nullptr, available, loading, [](const auto&) {}, apply) &&
        HasCategorizedContent(count, loading, knownCount),
        "a known nonempty folder reserves search and tabs before the opening snapshot is sized");

    FolderSnapshot listing;
    listing.path = path;
    listing.complete = true;
    listing.entries.resize(knownCount);
    Check(Refresh(path, &listing, available, loading, [](const auto&) {}, apply) &&
        !loading && HasCategorizedContent(count, loading, knownCount),
        "the loaded folder keeps the same control footprint at animation handoff");
    Check(HasCategorizedContent(count, false, 0),
        "a full nonempty source keeps search accessible when filtering has no results");

    listing.complete = false;
    listing.error = ERROR_ACCESS_DENIED;
    Check(Refresh(path, &listing, available, loading, [](const auto&) {}, apply) &&
        !available && count == knownCount && HasCategorizedContent(count, loading, knownCount),
        "a failed refresh retains the existing source and its controls");

    listing.complete = true;
    listing.error = ERROR_SUCCESS;
    listing.entries.clear();
    Check(Refresh(path, &listing, available, loading, [](const auto&) {}, apply) &&
        available && !loading && !HasCategorizedContent(count, loading, knownCount),
        "a confirmed empty folder releases search and tab space despite an older known count");
    Check(!HasCategorizedContent(0, true, 0) && !HasCategorizedContent(0, false, 0),
        "an unknown or confirmed empty source does not create control placeholders");
}

void TestFolderFirstListing()
{
    using namespace snowdesktop::dock_folder_popup_read;
    using namespace snowdesktop::popup_animation_rules;
    using snowdesktop::shell_refresh::FolderSnapshot;
    Check(HasFanContent(true, 0) && HasFanContent(true, 13) &&
        !HasFanContent(false, 0) && HasFanContent(false, 1),
        "a folder's pending/empty listing cannot downgrade its fan preference; empty collections retain their fallback");
    // Exercise the production listing and content-refresh dispatch together for
    // fan/grid scale and fade effects, including native and fallback animation.
    for (const auto& effect : std::array<std::pair<bool, double>, 3>{{
            {false, 2.4}, {false, 1.0}, {true, 1.0}}})
    {
        for (const bool native : {false, true})
        {
            State animation;
            animation.Configure(effect.first, effect.second);
            const auto duration = static_cast<std::uint64_t>(animation.DurationMilliseconds(true));
            FolderSnapshot listing;
            listing.path = L"C:\\popup";
            listing.complete = true;
            bool available = false, loading = true;
            std::size_t count = 0;
            int queued = 0, prepared = 0, retired = 0;
            const auto refresh = [&](std::uint64_t now, bool compositor) {
                Check(Refresh(listing.path, &listing, available, loading,
                    [](const auto&) {}, [&](const auto& result) { count = result.entries.size(); }),
                    "the current folder listing is accepted before refreshing popup content");
                return RefreshContent(animation, now, compositor,
                    [&] { ++queued; }, [&] { ++prepared; }, [&] { ++retired; });
            };
            animation.Open(100);
            animation.Advance(100 + duration);
            refresh(110 + duration, false); // The open popup is still empty.
            listing.entries.resize(13);
            Check(refresh(120 + duration, false) == ContentRefreshAction::Stable &&
                count == 13 && available && !loading && !animation.IsAnimating() &&
                animation.GetVisual().progress == 1.0f && animation.IsInteractive(),
                "a completed empty popup accepts its first files without replaying opening");

            animation.ResetHidden();
            animation.Open(1000); // Only a user opening starts a fresh timeline.
            count = 0;
            queued = prepared = retired = 0;
            const auto halfway = 1000 + duration / 2;
            const auto action = refresh(halfway, native);
            Check(count == 13 && animation.IsAnimating() && animation.IsInteractive() &&
                action == (native ? ContentRefreshAction::ContinueCompositor :
                    ContentRefreshAction::ContinueAnimation) &&
                queued == (native ? 1 : 0) && prepared == (native ? 0 : 1) &&
                retired == (native ? 0 : 1),
                "first files refresh the existing native surface or fallback frame during opening");
            animation.Advance(1000 + duration);
            Check(!animation.IsAnimating() && animation.GetVisual().progress == 1.0f,
                "first files retain the original opening deadline for fan, grid and fade effects");

            animation.Close(2000);
            count = 0;
            refresh(2000 + duration / 2, native);
            Check(count == 13 && animation.IsClosing() && !animation.IsInteractive(),
                "a late first listing cannot reverse the user's close action");
            animation.Advance(2000 + duration);
            Check(animation.IsHidden() && !animation.IsAnimating(),
                "closing still completes at its original deadline after files arrive");
            count = 0;
            Check(refresh(2010 + duration, false) == ContentRefreshAction::Stable &&
                count == 13 && animation.IsHidden(),
                "a late listing cannot reopen a hidden popup");
        }
    }
}

void TestFolderRefreshScopeAndReads()
{
    TestFolderPopupControlReservation();
    TestFolderFirstListing();
    TestPopupTargetRebinding();
    using namespace snowdesktop::shell_refresh;
    // A completed destination listing must be visible before button-up, while
    // the source model stays untouched until release and obsolete reads expire.
    FolderReadDelivery delivery;
    std::uint64_t version = 1;
    int previews = 0, modelApplies = 0;
    bool dragLoading = true, dragAvailable = false;
    auto ready = std::make_shared<Snapshot>();
    auto& destination = ready->folders[L"C:\\DESTINATION"];
    destination.path = L"C:\\destination";
    destination.complete = true;
    destination.entries.emplace_back();
    const auto preview = [&](auto& result) {
        ++previews;
        const auto& folder = result.folders.at(L"C:\\DESTINATION");
        snowdesktop::dock_folder_popup_read::Refresh(folder.path, &folder,
            dragAvailable, dragLoading, [](const auto&) {}, [](const auto&) {});
    };
    const auto publish = [&](auto&) { ++modelApplies; };
    const auto current = [&](const auto&) { return version; };
    delivery.Deliver(L"C:\\DESTINATION", version, ready, true, preview, publish);
    Check(previews == 1 && !dragLoading && dragAvailable && modelApplies == 0,
        "a held drag receives its finished destination listing without replacing the source model");
    delivery.Drain(current, publish);
    delivery.Drain(current, publish);
    Check(modelApplies == 1, "button-up publishes the retained read exactly once without re-reading");
    delivery.Deliver(L"C:\\DESTINATION", version, ready, true, preview, publish);
    ++version;
    delivery.Drain(current, publish);
    Check(modelApplies == 1, "a directory change during dragging rejects the obsolete retained read");
    delivery.Deliver(L"C:\\DESTINATION", version, ready, true, preview, publish);
    delivery.Deliver(L"C:\\DESTINATION", version, ready, false, preview, publish);
    delivery.Drain(current, publish);
    Check(modelApplies == 2, "a newer idle result supersedes the retained drag result");
    delivery.Deliver(L"C:\\DESTINATION", version, ready, true, preview, publish);
    delivery.Clear();
    delivery.Drain(current, publish);
    Check(modelApplies == 2, "shutdown discards queued folder publication");
    const std::wstring first = L"C:\\mapped";
    const std::wstring second = L"C:\\other";
    Check(FolderKey(L"c:/mapped/.") == FolderKey(first) &&
            FolderKey(L"C:\\") == L"C:\\" &&
            FolderKey(L"\\\\server\\share\\") == L"\\\\SERVER\\SHARE",
        "root paths, UNC roots and alternate separators keep one subscription identity");
    const auto key = FolderKey(first);
    Check(FolderContainsChange(key, first + L"\\new.txt", SHCNE_CREATE) &&
            FolderContainsChange(key, first, SHCNE_UPDATEDIR) &&
            !FolderContainsChange(key, first + L"-other\\new.txt", SHCNE_CREATE) &&
            !FolderContainsChange(key, first + L"\\sub\\new.txt", SHCNE_CREATE),
        "directory changes route only to that listing, including coalesced events but not siblings or descendants");
    FolderRefreshScope scope;
    scope.Add({first}, false);
    Revision revision;
    const auto reading = revision.Begin();
    scope.Add({second}, true);
    revision.Invalidate();
    Check(!revision.Finish(*reading) && scope.Includes(first) && scope.Includes(second) &&
            !scope.Includes(L"C:\\unrelated"),
        "a second folder event rejects the old read while retaining both pending directories");
    Request request;
    request.folders = {first, L"c:/mapped/", second, L"C:\\unrelated"};
    request.iconVisibility[L"desktop-icon"] = true;
    SelectFolders(request, scope);
    Snapshot snapshot;
    int desktopReads = 0;
    std::vector<std::wstring> foldersRead;
    const auto desktop = [&](const Request&, Snapshot&) { ++desktopReads; return true; };
    const auto folder = [&](const std::wstring& path, MetadataCache&) {
        foldersRead.push_back(FolderKey(path));
        FolderSnapshot result;
        result.path = path;
        result.complete = true;
        return result;
    };
    Check(ReadSources(request, snapshot, desktop, folder) &&
            snapshot.foldersOnly && snapshot.folders.size() == 2 &&
            foldersRead.size() == 2 && desktopReads == 0 &&
            snapshot.folders.contains(L"C:\\MAPPED") && snapshot.folders.contains(L"C:\\OTHER") &&
            request.iconVisibility.empty(),
        "a mapped-folder event reads each affected directory once without enumerating desktop or unrelated mappings");
    // Keep filesystem failure separate from an empty successfully read folder.
    Snapshot failed;
    ReadSources(request, failed, desktop,
        [](const std::wstring& path, MetadataCache&) {
            FolderSnapshot result; result.path = path; return result;
        });
    const auto failedFolder = failed.folders.find(key);
    Check(failedFolder != failed.folders.end() && !failedFolder->second.complete,
        "a failed folder read stays incomplete so applying it cannot erase the current listing");
    scope.Full();
    scope.Add({first}, true);
    Request full;
    full.folders = {first, second};
    SelectFolders(full, scope);
    Snapshot complete;
    Check(!full.foldersOnly && ReadSources(full, complete, desktop, folder) &&
            desktopReads == 1,
        "a desktop/manual refresh dominates folder requests and retains full enumeration");
    scope.Add({second}, false);
    Check(scope.FoldersOnly() && scope.Includes(second) && !scope.Includes(first),
        "a new batch after completion or failure does not inherit obsolete folder scope");

    // Startup target resolution is still pending (available=false). Exercise the
    // same read/apply boundary as the popup; only the filesystem and UI are fake.
    bool available = false, loading = false;
    int reads = 0, applies = 0;
    size_t displayed = 0;
    const auto queue = [&](const auto& path) { Check(path == first, "popup queues its current folder"); ++reads; };
    const auto apply = [&](const auto& result) { displayed = result.entries.size(); ++applies; };
    const auto refresh = [&](const FolderSnapshot* result) {
        return snowdesktop::dock_folder_popup_read::Refresh(first, result, available, loading, queue, apply);
    };
    Check(refresh(nullptr) && loading && !available && reads == 1,
        "opening before target resolution starts reading instead of reporting unavailable");
    FolderSnapshot listing;
    listing.path = first;
    listing.complete = true;
    listing.entries.emplace_back();
    Check(refresh(&listing) && available && !loading && displayed == 1 && applies == 1,
        "completed folder read replaces startup unavailable state and publishes entries");
    FolderSnapshot denied;
    denied.path = first;
    denied.error = ERROR_ACCESS_DENIED;
    Check(refresh(&denied) && !available && !loading && displayed == 1 && applies == 1,
        "real access failure stops loading and retains the last listing without permitting operations");
    Check(refresh(nullptr) && loading && refresh(&listing) && available && !loading && applies == 2,
        "a subsequent successful read recovers from unavailable without reopening the popup");
    FolderSnapshot other;
    other.path = second;
    other.complete = true;
    Check(!refresh(&other) && available && displayed == 1 && applies == 2,
        "late completion from another folder cannot replace the current popup");
    listing.entries.clear();
    listing.error = ERROR_PATH_NOT_FOUND;
    Check(refresh(&listing) && !available && !loading && displayed == 0,
        "a missing folder clears obsolete entries but does not masquerade as an accessible empty folder");
    listing.error = ERROR_FILE_NOT_FOUND;
    Check(refresh(&listing) && available && !loading && displayed == 0,
        "an accessible empty wildcard listing displays empty rather than unavailable");
}

void TestFolderShellSubscriptions()
{
    using namespace snowdesktop::shell_refresh;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto root = std::filesystem::temp_directory_path() /
        (L"SnowDesktopFolderNotify-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()));
    struct Cleanup
    {
        std::filesystem::path path;
        HRESULT initialized;
        ~Cleanup() {
            std::error_code error; std::filesystem::remove_all(path, error);
            if (SUCCEEDED(initialized)) CoUninitialize();
        }
    } cleanup{root, initialized};
    std::filesystem::create_directories(root / L"mapped");
    std::filesystem::create_directories(root / L"dock");
    std::filesystem::create_directories(root / L"next");
    const auto mapped = (root / L"mapped").wstring();
    const auto dock = (root / L"dock").wstring();
    const auto next = (root / L"next").wstring();
    constexpr UINT message = WM_APP + 101;
    std::vector<std::optional<ShellChangeNotification>> notifications;
    const wchar_t* className = L"SnowDesktopFolderSubscriptionTest";
    WNDCLASSW windowClass{};
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = className;
    windowClass.lpfnWndProc = [](HWND window, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
        if (msg == WM_NCCREATE)
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(
                reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        if (msg == WM_APP + 101)
        {
            auto* received = reinterpret_cast<std::vector<std::optional<ShellChangeNotification>>*>(
                GetWindowLongPtrW(window, GWLP_USERDATA));
            if (received) received->push_back(ReadShellChangeNotification(wp, lp));
            return 0;
        }
        return DefWindowProcW(window, msg, wp, lp);
    };
    Check(RegisterClassW(&windowClass) != 0, "isolated notification window class is registered");
    const HWND window = CreateWindowExW(0, className, L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, windowClass.hInstance, &notifications);
    Check(window != nullptr, "isolated Shell notification receiver is created");
    if (!window) { UnregisterClassW(className, windowClass.hInstance); return; }
    FolderNotifications subscriptions;
    constexpr UINT readyMessage = WM_APP + 102;
    auto waitFor = [&](auto&& condition) {
        const auto deadline = GetTickCount64() + 6000;
        for (;;)
        {
            MSG received{};
            while (PeekMessageW(&received, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&received);
            if (condition()) return true;
            const auto now = GetTickCount64();
            if (now >= deadline) return false;
            MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(deadline - now),
                QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    };
    // Startup registers before desktopPidl_ exists. A null registration used to
    // observe this application's own log writes and retire every completed read.
    // Keep a wildcard control to prove that Shell really delivered the event.
    std::vector<std::optional<ShellChangeNotification>> globalNotifications;
    const HWND globalWindow = CreateWindowExW(0, className, L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, windowClass.hInstance, &globalNotifications);
    const SHChangeNotifyEntry globalEntry{nullptr, FALSE};
    const auto globalId = SHChangeNotifyRegister(globalWindow,
        SHCNRF_ShellLevel | SHCNRF_NewDelivery, SHCNE_UPDATEITEM,
        message, 1, &globalEntry);
    const auto desktopId = RegisterDesktopShellNotifications(window, message, nullptr, nullptr);
    Check(globalWindow && globalId && desktopId, "startup notification scope fixtures register successfully");
    const auto unrelatedLog = (root / L"SnowDesktop.log").wstring();
    { std::ofstream(unrelatedLog) << "isolated application log"; }
    const auto containsLog = [&](const auto& changes) {
        return std::any_of(changes.begin(), changes.end(), [&](const auto& change) {
            return change && FolderKey(change->source) == FolderKey(unrelatedLog);
        });
    };
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSH, unrelatedLog.c_str(), nullptr);
    Check(waitFor([&] { return containsLog(globalNotifications); }),
        "wildcard control receives the unrelated log update");
    Check(!containsLog(notifications),
        "startup desktop subscription excludes unrelated log writes before its first snapshot");
    if (desktopId) SHChangeNotifyDeregister(desktopId);
    if (globalId) SHChangeNotifyDeregister(globalId);
    if (globalWindow) DestroyWindow(globalWindow);
    notifications.clear();
    std::vector<std::wstring> added;
    auto syncUntil = [&](const std::vector<std::wstring>& paths, size_t count) {
        added.clear();
        return waitFor([&] {
            auto current = subscriptions.Sync(window, message, readyMessage, paths);
            added.insert(added.end(), current.begin(), current.end());
            return subscriptions.Size() == count;
        });
    };
    Check(syncUntil({mapped, mapped + L"\\", dock}, 2), "background directory resolution finishes");
    Check(subscriptions.Size() == 2 && added.size() == 2 &&
            subscriptions.Sync(window, message, readyMessage, {mapped, dock}).empty(),
        "multiple mapped widgets and a Dock alias share their path registration without re-registering on rebuild");
    // Separate Shell subscription setup from the ordinary I/O being tested.
    // Registration and the first write can otherwise share the same tick,
    // before the interrupt watcher is ready. Flush preparation notifications
    // and drain them so they cannot satisfy the subsequent file-write checks.
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSH, mapped.c_str(), nullptr);
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSH, dock.c_str(), nullptr);
    MSG setupMessage{};
    while (PeekMessageW(&setupMessage, window, message, message, PM_REMOVE))
        DispatchMessageW(&setupMessage);
    notifications.clear();
    auto affected = subscriptions.Affected(ShellChangeNotification{
        SHCNE_RENAMEITEM, mapped + L"\\old.txt", dock + L"\\new.txt"});
    Check(affected.size() == 2,
        "both source and destination listings are invalidated by a cross-directory rename");
    Check(subscriptions.Affected(ShellChangeNotification{
            SHCNE_UPDATEDIR, dock, {}}) == std::vector<std::wstring>{FolderKey(dock)},
        "a coalesced Dock update does not refresh an unrelated mapped widget");

    // Ordinary Win32 writes, no synthetic file notification: prove that the
    // InterruptLevel subscription sees changes made outside Shell operations.
    auto waitForFolder = [&](const std::wstring& expected) {
        const auto deadline = GetTickCount64() + 6000;
        while (GetTickCount64() < deadline)
        {
            MSG received{};
            while (PeekMessageW(&received, nullptr, 0, 0, PM_REMOVE))
                DispatchMessageW(&received);
            // Shell can deliver through SendMessage while the UI thread pumps;
            // inspect observations from WndProc, not only posted queue entries.
            for (const auto& notification : std::exchange(notifications, {}))
            {
                if (!notification) continue;
                const auto paths = subscriptions.Affected(notification);
                if (std::find(paths.begin(), paths.end(), FolderKey(expected)) != paths.end())
                    return true;
            }
            const auto now = GetTickCount64();
            if (now >= deadline) break;
            MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(deadline - now),
                QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        return false;
    };
    { std::ofstream(root / L"mapped" / L"new.txt") << "external file"; }
    Check(waitForFolder(mapped), "external file creation wakes the mapped-folder subscription");
    { std::ofstream(root / L"dock" / L"new.txt") << "Dock file"; }
    Check(waitForFolder(dock), "external file creation wakes the open ordinary Dock directory");

    subscriptions.Sync(window, message, readyMessage, {mapped});
    Check(subscriptions.Size() == 1 && subscriptions.Affected(
            ShellChangeNotification{SHCNE_CREATE, dock + L"\\late.txt", {}}).empty(),
        "closing the Dock popup releases its registration and ignores queued events from that directory");
    Check(syncUntil({next}, 1), "retargeted directory resolution finishes");
    Check(subscriptions.Size() == 1 && subscriptions.Affected(
            ShellChangeNotification{SHCNE_CREATE, mapped + L"\\late.txt", {}}).empty(),
        "removing or retargeting the last mapped widget releases its old directory");
    Check(subscriptions.Affected(std::nullopt) == std::vector<std::wstring>{FolderKey(next)},
        "an undecodable directory notification conservatively refreshes only active subscriptions");
    subscriptions.Clear();
    Check(subscriptions.Size() == 0, "host teardown deregisters every directory");
    Check(syncUntil({mapped}, 1) && added.size() == 1,
        "host recovery can register the same mapped directory again");
    subscriptions.Clear();

    // Hold only the Shell provider boundary. The production queue, Sync,
    // registration, retirement and teardown still run without replacements.
    struct Gate
    {
        std::mutex mutex;
        std::condition_variable changed;
        int entered = 0;
        int exited = 0;
        int failedCalls = 0;
        bool release = false;
        bool timedOut = false;
        DWORD resolverThread = 0;
    };
    const auto gate = std::make_shared<Gate>();
    auto blocked = std::make_unique<FolderNotifications>([gate, mapped, next](const std::wstring& path) {
        if (path == FolderKey(next))
        {
            std::lock_guard lock(gate->mutex);
            ++gate->failedCalls;
            gate->changed.notify_all();
            return FolderNotifications::ResolvedFolder{};
        }
        if (path == FolderKey(mapped))
        {
            std::unique_lock lock(gate->mutex);
            ++gate->entered;
            gate->resolverThread = GetCurrentThreadId();
            gate->changed.notify_all();
            if (!gate->changed.wait_for(lock, std::chrono::seconds(6), [&] { return gate->release; }))
                gate->timedOut = true;
            ++gate->exited;
            gate->changed.notify_all();
        }
        return FolderNotifications::Resolve(path);
    });
    Check(blocked->Sync(window, message, readyMessage, {mapped, dock}).empty(),
        "Sync returns before a held Shell provider completes");
    {
        std::unique_lock lock(gate->mutex);
        Check(gate->changed.wait_for(lock, std::chrono::seconds(2), [&] { return gate->entered == 1; }) &&
                !gate->timedOut && gate->resolverThread != GetCurrentThreadId(),
            "Shell path resolution runs off the caller thread while its provider is held");
    }
    Check(waitFor([&] {
        blocked->Sync(window, message, readyMessage, {mapped, dock});
        return blocked->Size() == 1;
    }) && blocked->Affected(std::nullopt) == std::vector<std::wstring>{FolderKey(dock)},
        "one stalled directory does not prevent another directory from subscribing");
    blocked->Sync(window, message, readyMessage, {dock, next});
    Check(waitFor([&] {
        blocked->Sync(window, message, readyMessage, {dock, next});
        std::lock_guard lock(gate->mutex);
        return gate->failedCalls == 1;
    }), "unavailable directory completes a failed background attempt");
    for (int i = 0; i < 20; ++i) blocked->Sync(window, message, readyMessage, {dock, next});
    {
        std::lock_guard lock(gate->mutex);
        Check(gate->failedCalls == 1 && gate->entered == 1 && !gate->timedOut,
            "rebuilds do not duplicate blocked work or immediately retry failed paths");
        gate->release = true;
    }
    gate->changed.notify_all();
    {
        std::unique_lock lock(gate->mutex);
        Check(gate->changed.wait_for(lock, std::chrono::seconds(2), [&] { return gate->exited == 1; }),
            "retired provider can return independently of its former subscription");
    }
    // Queue a fresh request for the same path while the obsolete one can still
    // finish canonicalization; ids must distinguish both generations.
    {
        std::lock_guard lock(gate->mutex);
        gate->release = false;
    }
    blocked->Sync(window, message, readyMessage, {mapped});
    {
        std::unique_lock lock(gate->mutex);
        Check(gate->changed.wait_for(lock, std::chrono::seconds(2), [&] { return gate->entered == 2; }),
            "re-added path receives a distinct background request");
    }
    blocked->Sync(window, message, readyMessage, {mapped});
    Check(blocked->Size() == 0 && blocked->Affected(std::nullopt).empty(),
        "a removed path's late result cannot register its re-added generation");
    blocked->Clear();
    blocked.reset();
    {
        std::unique_lock lock(gate->mutex);
        Check(gate->exited == 1 && !gate->timedOut,
            "Clear and destruction return while the provider is still held");
        gate->release = true;
        gate->changed.notify_all();
        Check(gate->changed.wait_for(lock, std::chrono::seconds(2), [&] { return gate->exited == 2; }),
            "a provider can finish safely after the subscription owner is destroyed");
    }
    // Drain shared-memory notifications before destroying the receiver.
    MSG received{};
    while (PeekMessageW(&received, window, message, message, PM_REMOVE))
        (void)ReadShellChangeNotification(received.wParam, received.lParam);
    DestroyWindow(window);
    UnregisterClassW(className, windowClass.hInstance);
}

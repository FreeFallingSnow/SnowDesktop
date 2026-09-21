// Included inside the slot runtime test namespace. Exercise the production
// subscription, event routing and read-selection boundaries without a desktop
// host or real user data. The filesystem reader is replaced only in scope tests.
void TestFolderRefreshScopeAndReads()
{
    using namespace snowdesktop::shell_refresh;
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
    request.dockPaths = {L"C:\\desktop-file"};
    request.iconVisibility[L"desktop-icon"] = true;
    SelectFolders(request, scope);
    Snapshot snapshot;
    int desktopReads = 0, dockReads = 0;
    std::vector<std::wstring> foldersRead;
    const auto desktop = [&](const Request&, Snapshot&) { ++desktopReads; return true; };
    const auto folder = [&](const std::wstring& path, MetadataCache&) {
        foldersRead.push_back(FolderKey(path));
        FolderSnapshot result;
        result.path = path;
        result.complete = true;
        return result;
    };
    const auto missing = [&](const std::wstring&) { ++dockReads; return false; };
    Check(ReadSources(request, snapshot, desktop, folder, missing) &&
            snapshot.foldersOnly && snapshot.folders.size() == 2 &&
            foldersRead.size() == 2 && desktopReads == 0 && dockReads == 0 &&
            snapshot.folders.contains(L"C:\\MAPPED") && snapshot.folders.contains(L"C:\\OTHER") &&
            request.iconVisibility.empty(),
        "a mapped-folder event reads each affected directory once without enumerating desktop, unrelated mappings or Dock targets");
    // Keep filesystem failure separate from an empty successfully read folder.
    Snapshot failed;
    ReadSources(request, failed, desktop,
        [](const std::wstring& path, MetadataCache&) {
            FolderSnapshot result; result.path = path; return result;
        }, missing);
    const auto failedFolder = failed.folders.find(key);
    Check(failedFolder != failed.folders.end() && !failedFolder->second.complete,
        "a failed folder read stays incomplete so applying it cannot erase the current listing");
    scope.Full();
    scope.Add({first}, true);
    Request full;
    full.folders = {first, second};
    full.dockPaths = {L"C:\\desktop-file"};
    SelectFolders(full, scope);
    Snapshot complete;
    Check(!full.foldersOnly && ReadSources(full, complete, desktop, folder, missing) &&
            desktopReads == 1 && dockReads == 1,
        "a desktop/manual refresh dominates folder requests and retains full enumeration and Dock checks");
    scope.Add({second}, false);
    Check(scope.FoldersOnly() && scope.Includes(second) && !scope.Includes(first),
        "a new batch after completion or failure does not inherit obsolete folder scope");
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
    const auto added = subscriptions.Sync(window, message, {mapped, mapped + L"\\", dock});
    Check(subscriptions.Size() == 2 && added.size() == 2 &&
            subscriptions.Sync(window, message, {mapped, dock}).empty(),
        "multiple mapped widgets and a Dock alias share their path registration without re-registering on rebuild");
    auto affected = subscriptions.Affected(ShellChangeNotification{
        SHCNE_RENAMEITEM, mapped + L"\\old.txt", dock + L"\\new.txt"});
    Check(affected.size() == 2,
        "both source and destination listings are invalidated by a cross-directory rename");
    Check(subscriptions.Affected(ShellChangeNotification{
            SHCNE_UPDATEDIR, dock, {}}) == std::vector<std::wstring>{FolderKey(dock)},
        "a coalesced Dock update does not refresh an unrelated mapped widget");

    // Ordinary Win32 writes, no synthetic SHChangeNotify: prove that the
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

    subscriptions.Sync(window, message, {mapped});
    Check(subscriptions.Size() == 1 && subscriptions.Affected(
            ShellChangeNotification{SHCNE_CREATE, dock + L"\\late.txt", {}}).empty(),
        "closing the Dock popup releases its registration and ignores queued events from that directory");
    subscriptions.Sync(window, message, {next});
    Check(subscriptions.Size() == 1 && subscriptions.Affected(
            ShellChangeNotification{SHCNE_CREATE, mapped + L"\\late.txt", {}}).empty(),
        "removing or retargeting the last mapped widget releases its old directory");
    Check(subscriptions.Affected(std::nullopt) == std::vector<std::wstring>{FolderKey(next)},
        "an undecodable directory notification conservatively refreshes only active subscriptions");
    subscriptions.Clear();
    Check(subscriptions.Size() == 0, "host teardown deregisters every directory");
    Check(subscriptions.Sync(window, message, {mapped}).size() == 1,
        "host recovery can register the same mapped directory again");
    subscriptions.Clear();
    // Drain shared-memory notifications before destroying the receiver.
    MSG received{};
    while (PeekMessageW(&received, window, message, message, PM_REMOVE))
        (void)ReadShellChangeNotification(received.wParam, received.lParam);
    DestroyWindow(window);
    UnregisterClassW(className, windowClass.hInstance);
}

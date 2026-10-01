// Shell reads are represented by explicitly ordered completions. The actual
// UI-owned cache and running-app matcher execute; no timing/sleeps are needed.
void CheckDockProcessSnapshotReuse()
{
    using Snapshot = std::unordered_map<std::uint32_t, std::uint32_t>;
    unsigned queries = 0;
    const auto query = [&] { ++queries; return Snapshot{{2, 1}}; };
    {
        std::optional<Snapshot> pass;
        const Snapshot* first = nullptr;
        for (int window = 0; window < 128; ++window)
        {
            const auto& parents = snowdesktop::dock_process_snapshot::Read(pass, query);
            if (!first) first = &parents;
            Check(&parents == first && parents.at(2) == 1,
                "all probes in one enumeration borrow one consistent parent snapshot");
        }
        Check(queries == 1, "many preview probes query the process snapshot once");
    }
    {
        std::optional<Snapshot> nextPass;
        snowdesktop::dock_process_snapshot::Read(nextPass, query);
        Check(queries == 2, "a new enumeration gets a fresh process snapshot");
    }
    unsigned unavailableQueries = 0;
    std::optional<Snapshot> failedPass;
    const auto unavailable = [&] { ++unavailableQueries; return Snapshot{}; };
    Check(snowdesktop::dock_process_snapshot::Read(failedPass, unavailable).empty() &&
            snowdesktop::dock_process_snapshot::Read(failedPass, unavailable).empty() &&
            unavailableQueries == 1, "an unavailable snapshot retries on the next pass rather than every window");
    std::cout << "preview synthetic128 probes: process snapshots=1; next pass fresh\n";
}

void CheckBoundedIconCache()
{
    snowdesktop::BoundedLruCache<int, std::unique_ptr<int>> cache(2);
    cache.Insert(1, std::make_unique<int>(10));
    cache.Insert(2, std::make_unique<int>(20));
    Check(cache.Find(1) && **cache.Find(1) == 10, "a hit preserves the owned value");
    cache.Insert(3, std::make_unique<int>(30));
    Check(cache.Size() == 2 && !cache.Find(2) && cache.Find(1),
        "inserting evicts only the least recently used icon");
    cache.Insert(1, std::make_unique<int>(40));
    Check(cache.Size() == 2 && **cache.Find(1) == 40, "replacement keeps one entry");
    cache.Clear();
    Check(cache.Size() == 0 && !cache.Find(1), "device/style reset releases all owned values");
    snowdesktop::BoundedLruCache<int, int> disabled(0);
    Check(!disabled.Insert(1, 2) && disabled.Size() == 0, "zero capacity keeps no value");
    snowdesktop::BoundedLruCache<int, int> reflection(64);
    unsigned misses = 0;
    for (int size = 0; size < 64; ++size) reflection.Insert(size, size);
    for (int size = 64; size < 128; ++size)
    {
        if (!reflection.Find(63)) ++misses;
        reflection.Insert(size, size);
        Check(reflection.Size() <= 64, "mixed sizes retain the configured bound");
    }
    Check(misses == 0 && reflection.Find(63), "mixed rare sizes keep the frequently drawn reflection");
    std::cout << "reflection synthetic mixed sizes: hot-key misses=0; retained<=64\n";
}

void CheckDockRefreshContinuity()
{
    CheckBoundedIconCache();
    CheckDockProcessSnapshotReuse();
    {
        // Exercise the production cache policy used by window AppID reads.
        // Shell itself is the controlled completion boundary, not mocked UI
        // enumeration. Empty IDs are valid negative metadata with short age.
        using AppIds = snowdesktop::dock_refresh_cache::Cache<std::wstring, std::uintptr_t>;
        AppIds cache;
        const AppIds::Clock::time_point start{};
        unsigned requests = 0, changes = 0;
        for (int second = 0; second < 60; ++second)
        {
            const auto now = start + std::chrono::seconds(second);
            cache.ReadOrSubmit(7, L"pid:thread", [&](std::uint64_t ticket) {
                ++requests;
                const auto changed = cache.PublishWithLifetimeChanged(7, ticket,
                    L"stable.app", std::chrono::seconds(30), now);
                if (changed && *changed) ++changes;
            }, now);
        }
        Check(requests == 2 && changes == 1,
            "stable AppID reads revalidate by age, not every maintenance pass");
        const auto reusedAt = start + std::chrono::seconds(60);
        const auto old = cache.Read(7, L"pid:thread", reusedAt);
        const auto reused = cache.Read(7, L"other-pid:thread", reusedAt);
        Check(!reused.sameSourceVersion && !cache.PublishWithLifetime(7, old.ticket,
            L"stale.app", std::chrono::seconds(30), reusedAt),
            "reused window identity rejects the obsolete completion");
        cache.PublishWithLifetime(7, reused.ticket, L"", std::chrono::seconds(5), reusedAt);
        Check(cache.Read(7, L"other-pid:thread", reusedAt + std::chrono::seconds(4)).fresh &&
            !cache.Read(7, L"other-pid:thread", reusedAt + std::chrono::seconds(5)).fresh,
            "negative AppIDs retry after their short lifetime");
        const auto pending = cache.Read(7, L"other-pid:thread", reusedAt + std::chrono::seconds(5));
        cache.Invalidate();
        const auto replacement = cache.Read(7, L"other-pid:thread", reusedAt + std::chrono::seconds(5));
        Check(!cache.PublishWithLifetime(7, pending.ticket, L"late", std::chrono::seconds(30), reusedAt + std::chrono::seconds(5)) &&
            replacement.ticket != pending.ticket,
            "window events retire in-flight identity tickets");
        cache.Retain([](auto) { return false; });
        Check(!cache.PeekValue(7, L"other-pid:thread"), "dead windows release their metadata");
        std::cout << "AppID synthetic 60s: requests=2, first-identity refresh=1, identical-result refresh=0\n";
    }
    using snowdesktop::dock_refresh_cache::Cache;
    {
        Cache<int> pixels;
        const auto request = pixels.Read(L"mapped", L"96");
        Check(pixels.Publish(L"mapped", request.ticket, 11, false) &&
            pixels.Publish(L"mapped", request.ticket, 22) &&
            !pixels.Publish(L"mapped", request.ticket, 11, false) &&
            pixels.Read(L"mapped", L"96").value == 22,
            "late local pixels cannot replace a refined Dock folder icon for the same request");
        pixels.Invalidate();
        const auto changed = pixels.Read(L"mapped", L"96");
        Check(!pixels.Publish(L"mapped", request.ticket, 99) &&
            pixels.Publish(L"mapped", changed.ticket, 33, false) &&
            pixels.Read(L"mapped", L"96").value == 33,
            "a refreshed folder accepts new local pixels while rejecting an obsolete Shell result");
        pixels.Retain([](const auto&) { return false; });
        const auto recreated = pixels.Read(L"mapped", L"192");
        Check(recreated.ticket != changed.ticket && !recreated.value &&
            !pixels.Publish(L"mapped", changed.ticket, 44),
            "graphics/style cache reset cannot accept a bitmap created for the retired request");
        const Cache<int>::Clock::time_point now{};
        pixels.PublishFailure(L"mapped", recreated.ticket, std::chrono::seconds(5), now);
        pixels.Publish(L"mapped", recreated.ticket, 55, false);
        const auto retry = pixels.Read(L"mapped", L"192", now + std::chrono::seconds(5));
        Check(!retry.fresh && retry.ticket != recreated.ticket && retry.value == 55,
            "local pixels arriving after Shell failure preserve the refinement retry and visible icon");
    }
    using snowdesktop::dock_refresh_cache::SourceKey;
    const auto key = SourceKey(L"PIN", L"C:\\DESKTOP\\EDITOR.LNK");
    const std::wstring editor = L"C:\\APPS\\EDITOR.EXE";
    const std::wstring other = L"C:\\APPS\\OTHER.EXE";
    Cache<std::wstring> identities;
    auto initial = identities.Read(key, L"v1");
    Check(!initial.value && !initial.fresh,
        "a new pinned identity starts pending, not confirmed as a non-app");
    identities.Publish(key, initial.ticket, editor);
    auto remainsPinned = [&](const auto& lookup, const std::wstring& executable) {
        return identityRules::MatchesRunningApp(DockAppIdentityKind::Executable,
            lookup.value.value_or(L""), L"", L"", executable, L"");
    };
    // This is the user's regression: the next discovery pass occurs between
    // invalidation (F5/Shell snapshot) and the asynchronous .lnk completion.
    identities.Invalidate();
    auto refresh = identities.Read(key, L"v1");
    Check(!refresh.fresh && remainsPinned(refresh, editor),
        "Shell refresh must not briefly expose a pinned running app in the running area");
    Check(refresh.sameSourceVersion,
        "an unchanged source remains usable while its metadata is revalidated");
    Check(!identities.Publish(key, initial.ticket, other),
        "a result from before refresh cannot overwrite the retained pinned identity");
    identities.Publish(key, refresh.ticket, editor);
    Check(remainsPinned(identities.Read(key, L"v1"), editor),
        "completing an unchanged refresh preserves pinned membership");

    auto changed = identities.Read(key, L"v2");
    Check(!changed.fresh && !changed.sameSourceVersion && remainsPinned(changed, editor),
        "a changed shortcut keeps its last confirmed section until replacement metadata arrives");
    identities.Invalidate();
    Check(!identities.Publish(key, changed.ticket, other),
        "a refresh invalidates in-flight results even before the next lookup");
    auto newest = identities.Read(key, L"v3");
    identities.Publish(key, newest.ticket, other);
    Check(!identities.Publish(key, changed.ticket, editor),
        "an out-of-order shortcut completion must not restore its previous target");
    const auto replaced = identities.Read(key, L"v3");
    Check(replaced.fresh && !remainsPinned(replaced, editor) && remainsPinned(replaced, other),
        "a confirmed target change updates matching instead of retaining the old app forever");
    auto nonApp = identities.Read(key, L"v4");
    identities.Publish(key, nonApp.ticket, L"");
    Check(identities.Read(key, L"v4").fresh && !remainsPinned(identities.Read(key, L"v4"), other),
        "a completed non-app result is distinct from an unfinished query");

    const auto movedKey = SourceKey(L"PIN", L"D:\\OTHER\\EDITOR.LNK");
    Check(!identities.Read(movedKey, L"v1").value,
        "a different source path cannot inherit another shortcut's identity");
    identities.Retain([](const auto&) { return false; });
    auto recreated = identities.Read(key, L"v1");
    Check(!recreated.value && !identities.Publish(key, nonApp.ticket, editor),
        "removing and recreating a source rejects its old completions and cached membership");

    namespace location = snowdesktop::item_location;
    Cache<location::FolderTarget> folders;
    const auto folderKey = SourceKey(L"FOLDER", L"C:\\DESKTOP\\FOLDER.LNK");
    auto firstFolder = folders.Read(folderKey, L"v1");
    folders.Publish(folderKey, firstFolder.ticket,
        {L"D:\\FILES", location::FolderTargetKind::Shortcut, true});
    folders.Invalidate();
    auto pendingFolder = folders.Read(folderKey, L"v1");
    Check(pendingFolder.value && pendingFolder.value->kind == location::FolderTargetKind::Shortcut,
        "an asynchronous folder refresh must keep the item in the file section");
    const auto editedFolder = folders.Read(folderKey, L"v2");
    Check(editedFolder.value && !editedFolder.sameSourceVersion,
        "a modified shortcut preserves classification but flags the retained target as outdated");
    folders.Publish(folderKey, editedFolder.ticket, {});
    Check(folders.Read(folderKey, L"v2").value->kind == location::FolderTargetKind::None,
        "a confirmed folder-to-file change can leave the file section");

    Cache<int> icons;
    auto firstIcon = icons.Read(folderKey);
    icons.Publish(folderKey, firstIcon.ticket, 42);
    icons.Invalidate();
    Check(icons.Read(folderKey).value == 42 && !icons.Read(folderKey).fresh,
        "folder revalidation keeps its icon instead of flashing a placeholder");

    const Cache<int>::Clock::time_point now{};
    const auto retryDelay = std::chrono::seconds(5);
    Cache<int> failedIcons;
    const auto request = failedIcons.Read(folderKey, {}, now);
    Check(failedIcons.PublishFailure(folderKey, request.ticket, retryDelay, now),
        "a failed mapped-folder icon lookup is recorded without caching index -1");
    const auto waiting = failedIcons.Read(folderKey, {}, now + std::chrono::seconds(4));
    Check(waiting.fresh && !waiting.value && waiting.ticket == request.ticket,
        "failure enables a folder fallback and throttles redraw-driven retries");
    const auto retry = failedIcons.Read(folderKey, {}, now + retryDelay);
    Check(!retry.fresh && retry.ticket != request.ticket,
        "the mapped-folder entry retries after failure instead of showing loading dots forever");
    Check(!failedIcons.PublishFailure(folderKey, request.ticket, retryDelay, now),
        "a late failure cannot postpone a newer mapped-folder icon request");
    failedIcons.Publish(folderKey, retry.ticket, 7);
    const auto recovered = failedIcons.Read(folderKey, {}, now + std::chrono::seconds(30));
    Check(recovered.fresh && recovered.value == 7,
        "a successful retry replaces fallback and clears the retry deadline");
    failedIcons.Invalidate();
    const auto refreshIcon = failedIcons.Read(folderKey, {}, now);
    failedIcons.PublishFailure(folderKey, refreshIcon.ticket, retryDelay, now);
    Check(failedIcons.Read(folderKey, {}, now).value == 7,
        "a failed refresh retains the last working folder icon");
    failedIcons.Retain([](const auto&) { return false; });
    Check(!failedIcons.PublishFailure(folderKey, refreshIcon.ticket, retryDelay, now),
        "a removed folder rejects its late failure");
}

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

class CacheComReference final : public IUnknown
{
public:
    explicit CacheComReference(unsigned& released) : released_(released) {}
    virtual ~CacheComReference() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *result = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --references_;
        if (!remaining) { ++released_; delete this; }
        return remaining;
    }
private:
    ULONG references_ = 1;
    unsigned& released_;
};

void CheckCacheComPtrOwnership()
{
    using Owned = Microsoft::WRL::ComPtr<CacheComReference>;
    unsigned released = 0;
    snowdesktop::BoundedLruCache<int, Owned> cache(1);
    auto make = [&] { Owned owned; owned.Attach(new CacheComReference(released)); return owned; };
    const auto* inserted = cache.Insert(1, make());
    Check(inserted && inserted->Get() && released == 0,
        "returning an inserted ComPtr address does not release its resource");
    const auto* hit = cache.Find(1);
    Check(hit && hit->Get() && released == 0,
        "returning a hit ComPtr address does not release its resource");
    const auto* replaced = cache.Insert(1, make());
    Check(replaced && replaced->Get() && released == 1,
        "replacement releases only the old resource and preserves the new ComPtr");
    const auto* evicted = cache.Insert(2, make());
    Check(evicted && evicted->Get() && released == 2 && !cache.Find(1),
        "LRU eviction releases one resource and preserves the returned ComPtr");
    cache.Clear();
    Check(released == 3 && cache.Size() == 0,
        "Clear releases each remaining COM resource exactly once");
}

void CheckBoundedIconCache()
{
    CheckCacheComPtrOwnership();
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

void CheckEverythingIconRows()
{
    snowdesktop::IconRowIndex<int> rows;
    for (unsigned row = 0; row < 1024; ++row) rows.Add(static_cast<int>(row % 64), row);
    unsigned visits = 0;
    for (int icon = 0; icon < 64; ++icon)
        rows.Visit(icon, 1024, [&](std::size_t row) {
            ++visits;
            Check(row % 64 == static_cast<unsigned>(icon), "completion visits matching rows only");
        });
    Check(visits == 1024, "64 completions touch1024 matching rows rather than65536 rows");
    rows.Add(100, 2000);
    rows.Visit(100, 1024, [&](std::size_t) { Check(false, "out-of-range rows are skipped"); });
    rows.Clear();
    rows.Add(200, 0);
    rows.Visit(1, 1, [&](std::size_t) { Check(false, "obsolete query row index is retired"); });
    unsigned current = 0;
    rows.Visit(200, 1, [&](std::size_t row) { ++current; Check(row == 0, "new query owns its row"); });
    Check(current == 1, "replacement rows accept their matching completion");
    std::cout << "Everything synthetic1024 rows/64 icons: completion visits=1024; re-queries=0\n";
}

void CheckSlowCallLimit()
{
    using Phase = snowdesktop::SlowCallPhase;
    snowdesktop::SlowCallLimiter limiter;
    unsigned writes = 0;
    std::uint64_t reported = 0;
    for (int event = 0; event < 100; ++event)
    {
        if (const auto result = limiter.Record(Phase::Message, event * 10.0, event + 60.0))
        { ++writes; reported += result->count; }
    }
    Check(writes == 1, "100 slow calls in one second emit one phase record");
    const auto next = limiter.Record(Phase::Message, 1000, 70);
    Check(next && next->count == 99 && next->maximumMs == 159,
        "next phase record carries the suppressed count and maximum");
    Check(limiter.Record(Phase::Due, 1000, 80).has_value(), "other phases have independent quotas");
    limiter.Record(Phase::Due, 1001, 90);
    const auto final = limiter.Flush(Phase::Due);
    Check(final && final->count == 1 && final->maximumMs == 90 && !limiter.Flush(Phase::Due),
        "normal shutdown reports pending samples exactly once");
    Check(limiter.Record(Phase::Message, 1, 80).has_value(), "clock reversal starts a new quota");
    Check(reported == 0, "first record has no fabricated prior samples");
    std::cout << "slow-call synthetic100 events/1s: sink writes=1; next record suppressed=99\n";
}

void CheckDockRefreshContinuity()
{
    CheckSlowCallLimit();
    CheckEverythingIconRows();
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
        // A single task-window event must not requeue Shell identity reads
        // for every other running window. Keep UI identity during refinement.
        Cache<std::wstring, std::uintptr_t> cache;
        unsigned requests = 0;
        const auto read = [&](std::uintptr_t window) {
            return cache.ReadOrSubmit(window, L"pid:thread", [&](std::uint64_t ticket) {
                ++requests;
                cache.Publish(window, ticket, L"known.app");
            });
        };
        for (std::uintptr_t window = 1; window <= 128; ++window) read(window);
        const auto retired = cache.Read(7, L"pid:thread");
        cache.Invalidate(7);
        Check(!cache.Publish(7, retired.ticket, L"late.app"),
            "targeted window invalidation rejects a retired in-flight completion before another read");
        const auto refining = cache.Read(7, L"pid:thread");
        Check(!refining.fresh && refining.sameSourceVersion && refining.value == L"known.app",
            "the changed window retains visible identity while its replacement is queued");
        for (std::uintptr_t window = 1; window <= 128; ++window) read(window);
        Check(requests == 129 && cache.Read(8, L"pid:thread").fresh,
            "one task-window event revalidates one identity instead of all 128 running windows");
    }
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
            !pixels.Publish(L"mapped", changed.ticket, 33, false) &&
            pixels.Read(L"mapped", L"96").value == 22 &&
            !pixels.Read(L"mapped", L"96").fresh,
            "a folder refresh retains its final icon instead of flashing new provisional local pixels");
        Check(pixels.Publish(L"mapped", changed.ticket, 44) &&
            pixels.Read(L"mapped", L"96").value == 44 &&
            pixels.Read(L"mapped", L"96").fresh &&
            !pixels.Publish(L"mapped", changed.ticket, 33, false),
            "the current Shell result replaces the retained icon and cannot be downgraded by late local pixels");
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
        Check(!pixels.Publish(L"mapped", retry.ticket, 66, false) &&
            pixels.Read(L"mapped", L"192", now + std::chrono::seconds(5)).value == 55,
            "retrying a failed refinement keeps the visible icon rather than replaying fast local pixels");
        Check(pixels.PublishFailure(L"mapped", retry.ticket, std::chrono::seconds(5),
                now + std::chrono::seconds(5)) &&
            pixels.Read(L"mapped", L"192", now + std::chrono::seconds(9)).value == 55 &&
            pixels.Read(L"mapped", L"192", now + std::chrono::seconds(9)).fresh,
            "a failed refresh retains the existing icon while throttling the next refinement attempt");
        const auto recovered = pixels.Read(L"mapped", L"192", now + std::chrono::seconds(10));
        Check(!recovered.fresh && recovered.ticket != retry.ticket &&
            pixels.Publish(L"mapped", recovered.ticket, 77) &&
            pixels.Read(L"mapped", L"192", now + std::chrono::seconds(10)).value == 77,
            "a successful later refinement replaces the icon retained through failed refreshes");
        const auto resized = pixels.Read(L"mapped", L"384", now + std::chrono::seconds(11));
        Check(!resized.fresh && !resized.sameSourceVersion && resized.value == 77 &&
            !pixels.Publish(L"mapped", resized.ticket, 88, false) &&
            pixels.Read(L"mapped", L"384", now + std::chrono::seconds(11)).value == 77 &&
            pixels.Publish(L"mapped", resized.ticket, 99),
            "a size change keeps the existing icon until the newly requested complete bitmap is ready");
        const auto newSource = pixels.Read(L"other-folder", L"384", now + std::chrono::seconds(11));
        Check(!newSource.value && pixels.Publish(L"other-folder", newSource.ticket, 11, false),
            "a different folder still gets its first local pixels without borrowing another source's icon");
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

// Shell reads are represented by explicitly ordered completions. The actual
// UI-owned cache and running-app matcher execute; no timing/sleeps are needed.
void CheckDockRefreshContinuity()
{
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

#include "app/app.h"
#include "diagnostics/diagnostic_log.h"
#include "diagnostics/performance_trace.h"
#include "common/utils.h"
#include "dock/dock_folder_rules.h"
#include "desktop/desktop_item_reference_migration.h"
#include "data/data_paths.h"
#include "app/shell/shell_icon_request.h"
#include "dock_folder_popup_read.h"
#include "dock/dock_usage_storage.h"
#include "dock_platform_helpers.h"
#include "dock/dock_pin_cleanup.h"
#include "dock/dock_drop_model.h"

// Dock entry identity, grouping, usage history and drag-out mutation.

size_t DesktopApp::FindWidgetIndexById(const std::wstring& id) const
{
    for (size_t i = 0; i < widgets_.size(); ++i)
        if (widgets_[i].id == id) return i;
    return static_cast<size_t>(-1);
}

bool DesktopApp::IsDockExclusiveItemKey(const std::wstring& key) const
{
    const std::wstring upper = ToUpperInvariant(key);
    return std::any_of(dockEntries_.begin(), dockEntries_.end(), [&](const DockEntry& entry) {
        return entry.type == DockEntryType::DesktopItem && !entry.keepOnDesktop &&
            ToUpperInvariant(entry.reference) == upper;
    });
}

bool DesktopApp::IsDockExclusiveWidgetId(const std::wstring& id) const
{
    return std::any_of(dockEntries_.begin(), dockEntries_.end(), [&](const DockEntry& entry) {
        return (IsWidgetDockEntryType(entry.type)) &&
            entry.reference == id;
    });
}

snowdesktop::item_location::FolderTarget
DesktopApp::ResolveDockFolderTarget(const DockEntry& entry, bool* targetPending, bool resolveNow) const
{
    using namespace snowdesktop::item_location;
    if (targetPending) *targetPending = false;
    std::wstring path, stamp;
    const bool mapping = entry.type == DockEntryType::FolderMapping;
    if (mapping)
    {
        const auto index = FindWidgetIndexById(entry.reference);
        if (index >= widgets_.size()) return {};
        path = widgets_[index].sourceFolderPath;
    }
    else
    {
        if (entry.type != DockEntryType::DesktopItem || IsRecycleBinDockEntry(entry)) return {};
        const auto index = FindItemIndexByKey(entry.reference);
        path = index < items_.size() ? items_[index].parsingName : entry.reference;
        if (index < items_.size()) stamp = snowdesktop::shell_icon_request::Stamp(items_[index]);
    }
    const auto key = (mapping ? L"M:" : L"I:") +
        ToUpperInvariant(snowdesktop::dock_refresh_cache::SourceKey(entry.reference, path));
    const auto cached = dockFolderTargetCache_.Read(key, stamp);
    // A regular folder resolves to its own path. Its last-write stamp can
    // change with the contents without invalidating that path. Shortcuts
    // still require a target from the current version of the link file.
    const bool knownDirectory = cached.value && cached.value->kind == FolderTargetKind::Directory;
    const bool needsTarget = snowdesktop::dock_folder_popup_read::TargetPending(
        mapping || knownDirectory, cached.fresh, cached.sameSourceVersion);
    if (targetPending) *targetPending = needsTarget;
    if (cached.fresh) return cached.value.value_or(FolderTarget{});
    const bool wasFolder = cached.value && cached.value->kind != FolderTargetKind::None;
    auto* owner = const_cast<DesktopApp*>(this);
    if (resolveNow)
    {
        // Capture a cold drag source once, before its first target sample.
        // Paint/hit queries keep the ordinary asynchronous cache path below.
        auto target = ResolveFolderTarget(path);
        if (mapping && target.kind == FolderTargetKind::None && !path.empty())
            target = {path, FolderTargetKind::Directory, false};
        owner->dockFolderTargetCache_.Publish(key, cached.ticket, target);
        if (targetPending) *targetPending = false;
        return target;
    }
    shellVisualWork_.Submit(L"dock-target:" + key + L"\n" + std::to_wstring(cached.ticket), [path, mapping] {
        auto target = ResolveFolderTarget(path);
        if (mapping && target.kind == FolderTargetKind::None && !path.empty())
            target = {path, FolderTargetKind::Directory, false};
        return target;
    }, [owner, key, entry, path, stamp, mapping, wasFolder, ticket = cached.ticket](FolderTarget target) {
        if (mapping)
        {
            const auto index = owner->FindWidgetIndexById(entry.reference);
            if (index >= owner->widgets_.size() || owner->widgets_[index].sourceFolderPath != path) return;
        }
        else
        {
            const auto index = owner->FindItemIndexByKey(entry.reference);
            if (index < owner->items_.size())
            {
                const auto& item = owner->items_[index];
                if (item.parsingName != path || snowdesktop::shell_icon_request::Stamp(item) != stamp) return;
            }
            else if (!stamp.empty()) return;
        }
        const bool isFolder = target.kind != FolderTargetKind::None;
        const auto targetPath = target.path;
        if (!owner->dockFolderTargetCache_.Publish(key, ticket, std::move(target))) return;
        snowdesktop::performance::Scope trace("dock", "folder.target.delivery");
        if (wasFolder != isFolder) owner->NormalizeDockRecycleBinPosition();
        owner->InvalidateDockContainers();
        const bool hasDesktopDock = std::any_of(owner->containers_.begin(), owner->containers_.end(),
            [owner](const auto& container) {
                const auto* dock = dynamic_cast<const DockContainer*>(container.get());
                return dock && !owner->IsDockHostedByPersistentHost(dock);
            });
        // Persistent Dock windows update their glass when they repaint below.
        owner->InvalidateDragStaticScene(hasDesktopDock);
        owner->UpdateFloatingDockWindowBounds(false);
        // Coalesce asynchronous target results into the next Dock paint.
        owner->InvalidateDockRects(FALSE, false);
        const auto sourceId = std::to_wstring(static_cast<int>(entry.type)) +
            L":" + ToUpperInvariant(entry.reference);
        if (!mapping && owner->dockFolderPopupOpen_ &&
            owner->dockFolderPopupSourceId_ == sourceId && owner->popupAnimation_.IsInteractive() &&
            (owner->dockFolderPopupWidget_.sourceFolderPath != targetPath ||
                (targetPath.empty() && owner->dockFolderPopupLoading_)))
            owner->RefreshDockFolderPopup();
    }, hwnd_, kBackgroundShellReadyMessage);
    if (cached.value)
    {
        auto pending = *cached.value;
        // Retain section membership, but don't execute an obsolete shortcut
        // target after the source itself changed.
        if (needsTarget)
        {
            pending.available = false;
            pending.path.clear();
        }
        return pending;
    }
    FolderTarget pending;
    if (mapping) { pending.kind = FolderTargetKind::Directory; pending.path = path; }
    return pending;
}

void DesktopApp::InvalidateDockShellMetadata()
{
    dockAppIdentityCache_.Invalidate();
    dockFolderTargetCache_.Invalidate();
    dockFolderBitmapCache_.Invalidate();
    dockIconWork_.Cancel(L"dock-folder:");
    dockIdentityWork_.Cancel();
    shellVisualWork_.Cancel(L"dock-target:");
}

void DesktopApp::PruneDockShellMetadata()
{
    using snowdesktop::dock_refresh_cache::SourceKey;
    std::unordered_set<std::wstring> identities, folders, icons;
    for (const auto& item : items_)
    {
        const auto key = DockItemWindowKey(item);
        identities.insert(SourceKey(key, item.parsingName));
        folders.insert(L"I:" + ToUpperInvariant(SourceKey(key, item.parsingName)));
    }
    for (const auto& entry : dockEntries_)
    {
        if (entry.type == DockEntryType::DesktopItem && FindItemIndexByKey(entry.reference) >= items_.size())
            folders.insert(L"I:" + ToUpperInvariant(SourceKey(entry.reference, entry.reference)));
    }
    for (const auto& widget : widgets_)
    {
        if (widget.type != DesktopWidgetType::FolderMapping) continue;
        const auto key = ToUpperInvariant(SourceKey(widget.id, widget.sourceFolderPath));
        folders.insert(L"M:" + key);
        icons.insert(key);
    }
    dockAppIdentityCache_.Retain([&](const auto& key) { return identities.contains(key); });
    dockFolderTargetCache_.Retain([&](const auto& key) { return folders.contains(key); });
    dockFolderBitmapCache_.Retain([&](const auto& key) { return icons.contains(key); });

    if (!desktopItemsReady_) return; // Never prune against partial startup reads.
    std::vector<std::wstring> candidates;
    for (const auto& entry : dockEntries_)
        if (entry.type == DockEntryType::DesktopItem && !IsRecycleBinDockEntry(entry) &&
            FindItemIndexByKey(entry.reference) >= items_.size())
            candidates.push_back(entry.reference);
    if (candidates.empty()) return;
    const auto revision = shellRefreshRevision_.Current();
    shellVisualWork_.Submit(L"dock-missing-pins:" + std::to_wstring(revision),
        [candidates = std::move(candidates)] {
            std::vector<std::wstring> missing;
            for (const auto& path : candidates)
                if (path.empty() || snowdesktop::dock_pin_cleanup::IsMissingLocalFile(path))
                    missing.push_back(path);
            return missing;
        }, [this, revision](const std::vector<std::wstring>& missing) {
            // Shell events invalidate this revision before queued results drain.
            // The common drain also fences drags, menus, renames and file writes.
            if (exitRequested_ || !desktopItemsReady_ || missing.empty() ||
                !shellRefreshRevision_.IsCurrent(revision)) return;
            bool closesFolderPopup = false;
            const auto removed = std::erase_if(dockEntries_, [&](const DockEntry& entry) {
                const bool remove = entry.type == DockEntryType::DesktopItem && !IsRecycleBinDockEntry(entry) &&
                    FindItemIndexByKey(entry.reference) >= items_.size() &&
                    std::any_of(missing.begin(), missing.end(), [&](const auto& path) {
                        return _wcsicmp(path.c_str(), entry.reference.c_str()) == 0;
                    });
                if (remove && dockFolderPopupOpen_ && dockFolderPopupSourceId_ ==
                    std::to_wstring(static_cast<int>(entry.type)) + L":" + ToUpperInvariant(entry.reference))
                    closesFolderPopup = true;
                return remove;
            });
            if (!removed) return;
            if (closesFolderPopup) CloseCollectionPopup();
            ClearDockPressedState();
            ClearSelection();
            NormalizeDockRecycleBinPosition();
            RefreshCollectedKeysCache();
            SaveLayoutSlots();
            RebuildContainersAndItems();
            LayoutItems();
            InvalidateDragStaticScene();
            InvalidateDockRects();
            const auto message = L"Removed confirmed missing local Dock pins: " + std::to_wstring(removed);
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Dock, message.c_str());
        }, controlHwnd_ ? controlHwnd_ : hwnd_, kBackgroundShellReadyMessage);
}

bool DesktopApp::IsFolderDockEntry(const DockEntry& entry) const
{
    return entry.type == DockEntryType::DesktopFiles ||
        entry.type == DockEntryType::FileGroup ||
        entry.type == DockEntryType::FolderMapping ||
        ResolveDockFolderTarget(entry).kind !=
            snowdesktop::item_location::FolderTargetKind::None;
}

size_t DesktopApp::DockMainEntryCount() const
{
    const auto services = const_cast<DesktopApp*>(this)->DockDropServices();
    return static_cast<size_t>(std::count_if(
        dockEntries_.begin(), dockEntries_.end(),
        [&](const DockEntry& entry) {
            return snowdesktop::dock_drop_model::GroupOf(entry, services) == snowdesktop::dock_folder_rules::EntryGroup::Main;
        }));
}

size_t DesktopApp::DockFolderEntryCount() const
{
    const auto services = const_cast<DesktopApp*>(this)->DockDropServices();
    return static_cast<size_t>(std::count_if(
        dockEntries_.begin(), dockEntries_.end(),
        [&](const DockEntry& entry) {
            return snowdesktop::dock_drop_model::GroupOf(entry, services) == snowdesktop::dock_folder_rules::EntryGroup::Folder;
        }));
}

bool DesktopApp::IsRecycleBinDockEntry(const DockEntry& entry) const
{
    return snowdesktop::dock_drop_model::IsRecycleBinEntry(entry);
}

void DesktopApp::NormalizeDockRecycleBinPosition()
{
    const auto services = DockDropServices();
    snowdesktop::dock_drop_model::Normalize(dockEntries_, services);
    snowdesktop::dock_drop_model::EnsureQuickSearch(dockEntries_, services);
}

void DesktopApp::LoadDockUsageStats()
{
    snowdesktop::dock_usage::Load(GetDataFilePath(L"SnowDesktop.dock-usage.json"), dockUsageStats_);
}

void DesktopApp::SaveDockUsageStats() const
{
    snowdesktop::dock_usage::Save(GetDataFilePath(L"SnowDesktop.dock-usage.json"), dockUsageStats_,
        [this](const std::wstring& key) {
            const size_t index = FindItemIndexByKey(key);
            return index < items_.size() && IsDockUsageEligibleItem(items_[index]);
        });
}

bool DesktopApp::IsDockUsageEligibleItem(const DesktopItem& item) const
{
    if (!item.desktopIconClsid.empty() || item.parsingName.empty())
        return false;
    const wchar_t* extension = PathFindExtensionW(item.parsingName.c_str());
    return extension &&
        (_wcsicmp(extension, L".lnk") == 0 || _wcsicmp(extension, L".url") == 0);
}

bool DesktopApp::RemoveDockDragOutItems(const std::vector<Item*>& sourceItems)
{
    bool usageChanged = false;
    std::vector<size_t> mappedEntryIndices;
    for (Item* source : sourceItems)
    {
        if (const auto* frequentItem = dynamic_cast<DockFrequentItem*>(source))
        {
            if (frequentItem->GetItemIndex() >= items_.size()) continue;
            const DesktopItem& item = items_[frequentItem->GetItemIndex()];
            const std::wstring key = ToUpperInvariant(
                item.layoutKey.empty() ? item.parsingName : item.layoutKey);
            if (!key.empty())
                usageChanged = dockUsageStats_.erase(key) > 0 || usageChanged;
            continue;
        }
        if (const auto* dockItem = dynamic_cast<DockEntryItem*>(source))
        {
            const size_t index = dockItem->GetEntryIndex();
            if (index < dockEntries_.size() && dockEntries_[index].keepOnDesktop)
                mappedEntryIndices.push_back(index);
        }
    }

    for (const auto index : mappedEntryIndices)
        for (const auto& container : containers_)
            if (auto* dock = dynamic_cast<DockContainer*>(container.get()))
                dock->CancelEntryRemoval(dockEntries_[index]);
    const bool mappingsChanged = snowdesktop::dock_drop_model::RemoveMappings(
        dockEntries_, std::move(mappedEntryIndices), DockDropServices());
    if (usageChanged) SaveDockUsageStats();
    if (mappingsChanged) RefreshCollectedKeysCache();
    if (!usageChanged && !mappingsChanged) return false;
    // The drag still owns Dock item wrappers here. Let its release/unwind
    // refresh running membership after those references have been released.
    dockRunningWindowsRefreshTick_ = 0;
    InvalidateDockContainers();
    InvalidateDragStaticScene();
    return true;
}

bool DesktopApp::RemoveDockMappingAt(
    size_t entryIndex)
{
    if (entryIndex >= dockEntries_.size() || dockEntries_[entryIndex].type != DockEntryType::DesktopItem ||
        !dockEntries_[entryIndex].keepOnDesktop) return false;
    for (const auto& container : containers_)
        if (auto* dock = dynamic_cast<DockContainer*>(container.get()))
            dock->PrepareEntryRemoval(entryIndex);
    if (!snowdesktop::
            desktop_item_reference_migration::
                RemoveDockMappingAt(
                    dockEntries_, entryIndex))
    {
        return false;
    }

    NormalizeDockRecycleBinPosition();
    RefreshCollectedKeysCache();
    ClearSelection();
    SaveLayoutSlots();
    RebuildContainersAndItems();
    LayoutItems();
    // Let normal maintenance reconcile running membership after this exit.
    // A synchronous all-window scan can block the first removal paint and
    // replace the running layout while the pin-removal caller is unwinding.
    dockRunningWindowsRefreshTick_ = 0;
    InvalidateDragStaticScene();
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

void DesktopApp::RecordDockItemUsage(size_t itemIndex)
{
    if (itemIndex >= items_.size()) return;
    const DesktopItem& item = items_[itemIndex];
    if (!IsDockUsageEligibleItem(item)) return;
    const std::wstring key = ToUpperInvariant(
        item.layoutKey.empty() ? item.parsingName : item.layoutKey);
    if (key.empty()) return;

    DockUsageRecord& record = dockUsageStats_[key];
    record.launchCount = std::min(record.launchCount + 1, std::numeric_limits<int>::max());
    const std::time_t now = std::time(nullptr);
    record.lastUsed = now > 0
        ? static_cast<int>(std::min<std::time_t>(now, std::numeric_limits<int>::max()))
        : record.lastUsed;
    SaveDockUsageStats();

    if (dockSettings_.showFrequentItems ||
        dockSettings_.lastMonitorOverride.showFrequentItems.value_or(false) ||
        dockSettings_.otherMonitorOverride.showFrequentItems.value_or(false))
    {
        InvalidateDockContainers();
        InvalidateDragStaticScene();
        if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

std::vector<size_t> DesktopApp::GetFrequentDockItemIndices()
{
    return GetFrequentDockItemIndices(dockSettings_);
}

std::vector<size_t> DesktopApp::GetFrequentDockItemIndices(const DockLayoutSettings& layout)
{
    std::vector<size_t> result;
    if (!layout.showFrequentItems || layout.frequentItemCount <= 0)
        return result;

    std::unordered_set<std::wstring> fixedKeys;
    for (const DockEntry& entry : dockEntries_)
        if (entry.type == DockEntryType::DesktopItem)
            fixedKeys.insert(ToUpperInvariant(entry.reference));

    struct Candidate
    {
        size_t itemIndex = static_cast<size_t>(-1);
        DockUsageRecord usage;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(dockUsageStats_.size());
    for (const auto& [key, usage] : dockUsageStats_)
    {
        if (usage.launchCount <= 0 || fixedKeys.contains(key)) continue;
        const size_t itemIndex = FindItemIndexByKey(key);
        if (itemIndex >= items_.size() || !IsDockUsageEligibleItem(items_[itemIndex]) ||
            _wcsicmp(items_[itemIndex].desktopIconClsid.c_str(),
                kDesktopIconClsidRecycleBin) == 0)
            continue;
        candidates.push_back({ itemIndex, usage });
    }

    std::stable_sort(candidates.begin(), candidates.end(),
        [this](const Candidate& a, const Candidate& b) {
            if (a.usage.launchCount != b.usage.launchCount)
                return a.usage.launchCount > b.usage.launchCount;
            if (a.usage.lastUsed != b.usage.lastUsed)
                return a.usage.lastUsed > b.usage.lastUsed;
            return ToUpperInvariant(items_[a.itemIndex].name) <
                ToUpperInvariant(items_[b.itemIndex].name);
        });

    const size_t limit = static_cast<size_t>(
        std::clamp(layout.frequentItemCount, 1, 8));
    for (const Candidate& candidate : candidates)
    {
        const DockAppIdentity identity = ResolveDockAppIdentity(candidate.itemIndex);
        const bool isShownAsRunning = layout.showRunningArea &&
            std::any_of(dockUnpinnedRunningApps_.begin(),
            dockUnpinnedRunningApps_.end(), [&](const DockRunningAppInfo& running) {
                return snowdesktop::dock_app_identity_rules::
                    MatchesRunningApp(
                        identity.kind,
                        identity.executablePath,
                        identity.appUserModelId,
                        identity.steamInstallDirectory,
                        running.executablePath,
                        running.appUserModelId,
                        running.ancestorExecutablePaths, identity.validatedPayloadPaths);
            });
        if (isShownAsRunning) continue;
        result.push_back(candidate.itemIndex);
        if (result.size() >= limit) break;
    }
    return result;
}

std::optional<size_t>
DesktopApp::FindDesktopItemForDockRunningApp(
    const DockRunningAppInfo& running)
{
    std::optional<size_t> match;
    for (size_t itemIndex = 0;
        itemIndex < items_.size(); ++itemIndex)
    {
        const DockAppIdentity identity =
            ResolveDockAppIdentity(itemIndex);
        if (!snowdesktop::dock_app_identity_rules::
                MatchesRunningApp(
                    identity.kind,
                    identity.executablePath,
                    identity.appUserModelId,
                    identity.steamInstallDirectory,
                    running.executablePath,
                    running.appUserModelId,
                    running.ancestorExecutablePaths, identity.validatedPayloadPaths))
            continue;
        if (match)
            return std::nullopt;
        match = itemIndex;
    }
    return match;
}

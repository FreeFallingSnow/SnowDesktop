#include "app.h"
#include "shell_icon_request.h"
#include "dock_folder_popup_read.h"
#include "../json_value.h"
#include "dock_platform_helpers.h"

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
DesktopApp::ResolveDockFolderTarget(const DockEntry& entry, bool* targetPending) const
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
    if (targetPending) *targetPending = snowdesktop::dock_folder_popup_read::TargetPending(
        mapping, cached.fresh, cached.sameSourceVersion);
    if (cached.fresh) return cached.value.value_or(FolderTarget{});
    const bool wasFolder = cached.value && cached.value->kind != FolderTargetKind::None;
    auto* owner = const_cast<DesktopApp*>(this);
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
        if (wasFolder != isFolder) owner->NormalizeDockRecycleBinPosition();
        owner->InvalidateDockContainers();
        owner->InvalidateDragStaticScene();
        owner->UpdateFloatingDockWindowBounds(false);
        owner->InvalidateDockRects();
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
        if (!cached.sameSourceVersion)
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
    shellVisualWork_.Cancel(L"dock-identity:");
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
}

bool DesktopApp::IsFolderDockEntry(const DockEntry& entry) const
{
    return entry.type == DockEntryType::DesktopFiles ||
        entry.type == DockEntryType::FolderMapping ||
        ResolveDockFolderTarget(entry).kind !=
            snowdesktop::item_location::FolderTargetKind::None;
}

size_t DesktopApp::DockMainEntryCount() const
{
    return static_cast<size_t>(std::count_if(
        dockEntries_.begin(), dockEntries_.end(),
        [this](const DockEntry& entry) {
            return !IsRecycleBinDockEntry(entry) &&
                !IsFolderDockEntry(entry);
        }));
}

size_t DesktopApp::DockFolderEntryCount() const
{
    return static_cast<size_t>(std::count_if(
        dockEntries_.begin(), dockEntries_.end(),
        [this](const DockEntry& entry) {
            return !IsRecycleBinDockEntry(entry) &&
                IsFolderDockEntry(entry);
        }));
}

size_t DesktopApp::FindCollectionGroupIndexForChild(
    const std::wstring& childId) const
{
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        const DesktopWidget& widget = widgets_[i];
        if (widget.type != DesktopWidgetType::CollectionGroup) continue;
        if (std::find(widget.childWidgetIds.begin(),
            widget.childWidgetIds.end(), childId) !=
            widget.childWidgetIds.end())
            return i;
    }
    return static_cast<size_t>(-1);
}

bool DesktopApp::IsGroupedCollection(
    const DesktopWidget& widget) const
{
    return widget.type == DesktopWidgetType::Collection &&
        FindCollectionGroupIndexForChild(widget.id) < widgets_.size();
}

size_t DesktopApp::FindFileGroupIndexForChild(
    const std::wstring& childId) const
{
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        const DesktopWidget& widget = widgets_[i];
        if (widget.type != DesktopWidgetType::FileGroup) continue;
        if (std::find(widget.childWidgetIds.begin(),
                widget.childWidgetIds.end(), childId) !=
            widget.childWidgetIds.end())
            return i;
    }
    return static_cast<size_t>(-1);
}

bool DesktopApp::IsGroupedWidget(
    const DesktopWidget& widget) const
{
    if (IsGroupedCollection(widget)) return true;
    return (widget.type == DesktopWidgetType::FileCategories ||
            widget.type == DesktopWidgetType::FolderMapping) &&
        FindFileGroupIndexForChild(widget.id) < widgets_.size();
}

bool DesktopApp::IsRecycleBinDockEntry(const DockEntry& entry) const
{
    return entry.type == DockEntryType::DesktopItem &&
        _wcsicmp(entry.reference.c_str(), kDesktopIconClsidRecycleBin) == 0;
}

void DesktopApp::NormalizeDockRecycleBinPosition()
{
    snowdesktop::dock_folder_rules::StableNormalize(
        dockEntries_,
        [this](const DockEntry& entry) {
            if (IsRecycleBinDockEntry(entry))
                return snowdesktop::dock_folder_rules::
                    EntryGroup::Recycle;
            return IsFolderDockEntry(entry)
                ? snowdesktop::dock_folder_rules::
                    EntryGroup::Folder
                : snowdesktop::dock_folder_rules::
                    EntryGroup::Main;
        });
}

void DesktopApp::LoadDockUsageStats()
{
    dockUsageStats_.clear();
    std::ifstream file(GetDataFilePath(L"SnowDesktop.dock-usage.json"), std::ios::binary);
    if (!file) return;
    std::ostringstream stream;
    stream << file.rdbuf();
    JsonValue root;
    if (!ParseJson(stream.str(), root) || !root.IsObject()) return;
    const JsonValue* entries = root.Find("entries");
    if (!entries || !entries->IsArray()) return;

    auto readInteger = [](const JsonValue& object,
        std::string_view name, int& output)
    {
        const JsonValue* value = object.Find(name);
        if (!value || !value->IsNumber() ||
            !std::isfinite(value->number) ||
            std::trunc(value->number) != value->number ||
            value->number < std::numeric_limits<int>::min() ||
            value->number > std::numeric_limits<int>::max())
        {
            return false;
        }
        output = static_cast<int>(value->number);
        return true;
    };

    for (const JsonValue& entry : entries->array)
    {
        if (!entry.IsObject()) continue;
        const JsonValue* key = entry.Find("key");
        int launchCount = 0;
        int lastUsed = 0;
        if (key && key->IsString() &&
            readInteger(entry, "launchCount", launchCount) &&
            launchCount > 0)
        {
            readInteger(entry, "lastUsed", lastUsed);
            const std::wstring normalizedKey =
                ToUpperInvariant(Utf8ToWide(key->string));
            if (!normalizedKey.empty())
                dockUsageStats_[normalizedKey] =
                    { launchCount, std::max(0, lastUsed) };
        }
    }
}

void DesktopApp::SaveDockUsageStats() const
{
    std::ofstream file(GetDataFilePath(L"SnowDesktop.dock-usage.json"),
        std::ios::binary | std::ios::trunc);
    if (!file) return;
    file << "{\n  \"entries\": [\n";
    size_t written = 0;
    for (const auto& [key, record] : dockUsageStats_)
    {
        if (key.empty() || record.launchCount <= 0) continue;
        const size_t itemIndex = FindItemIndexByKey(key);
        if (itemIndex >= items_.size() || !IsDockUsageEligibleItem(items_[itemIndex]))
            continue;
        if (written++ > 0) file << ",\n";
        file << "    { \"key\": \"" << JsonEscapeUtf8(key)
             << "\", \"launchCount\": " << record.launchCount
             << ", \"lastUsed\": " << record.lastUsed << " }";
    }
    file << "\n  ]\n}\n";
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

    std::sort(mappedEntryIndices.begin(), mappedEntryIndices.end());
    mappedEntryIndices.erase(
        std::unique(mappedEntryIndices.begin(), mappedEntryIndices.end()),
        mappedEntryIndices.end());
    for (auto it = mappedEntryIndices.rbegin(); it != mappedEntryIndices.rend(); ++it)
        dockEntries_.erase(dockEntries_.begin() + static_cast<std::ptrdiff_t>(*it));

    if (usageChanged) SaveDockUsageStats();
    if (!mappedEntryIndices.empty())
    {
        NormalizeDockRecycleBinPosition();
        RefreshCollectedKeysCache();
    }
    if (!usageChanged && mappedEntryIndices.empty()) return false;
    InvalidateDockContainers();
    InvalidateDragStaticScene();
    return true;
}

bool DesktopApp::RemoveDockMappingAt(
    size_t entryIndex)
{
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

    if (dockSettings_.showFrequentItems)
    {
        InvalidateDockContainers();
        InvalidateDragStaticScene();
        if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

std::vector<size_t> DesktopApp::GetFrequentDockItemIndices()
{
    std::vector<size_t> result;
    if (!dockSettings_.showFrequentItems || dockSettings_.frequentItemCount <= 0)
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
        std::clamp(dockSettings_.frequentItemCount, 1, 8));
    for (const Candidate& candidate : candidates)
    {
        const DockAppIdentity identity = ResolveDockAppIdentity(candidate.itemIndex);
        const bool isShownAsRunning =
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
                        running.ancestorExecutablePaths);
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
                    running.ancestorExecutablePaths))
            continue;
        if (match)
            return std::nullopt;
        match = itemIndex;
    }
    return match;
}

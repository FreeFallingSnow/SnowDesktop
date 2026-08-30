#include "app.h"
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
        return (entry.type == DockEntryType::Collection ||
                entry.type == DockEntryType::FolderMapping) &&
            entry.reference == id;
    });
}

snowdesktop::item_location::FolderTarget
DesktopApp::ResolveDockFolderTarget(const DockEntry& entry) const
{
    std::wstring sourcePath;
    if (entry.type == DockEntryType::FolderMapping)
    {
        std::wstring cacheKey =
            L"M:" + ToUpperInvariant(entry.reference);
        if (const auto cached = dockFolderTargetCache_.find(cacheKey);
            cached != dockFolderTargetCache_.end())
            return cached->second;

        const size_t widgetIndex = FindWidgetIndexById(entry.reference);
        if (widgetIndex >= widgets_.size() ||
            widgets_[widgetIndex].type != DesktopWidgetType::FolderMapping)
            return {};
        sourcePath = widgets_[widgetIndex].sourceFolderPath;
        auto target =
            snowdesktop::item_location::ResolveFolderTarget(
                sourcePath);
        if (target.kind ==
                snowdesktop::item_location::
                    FolderTargetKind::None &&
            !sourcePath.empty())
        {
            target.path = sourcePath;
            target.kind =
                snowdesktop::item_location::
                    FolderTargetKind::Directory;
            target.available = false;
        }
        dockFolderTargetCache_.insert_or_assign(
            std::move(cacheKey), target);
        return target;
    }
    if (entry.type != DockEntryType::DesktopItem ||
        IsRecycleBinDockEntry(entry))
        return {};

    std::wstring cacheKey =
        L"I:" + ToUpperInvariant(entry.reference);
    if (const auto cached = dockFolderTargetCache_.find(cacheKey);
        cached != dockFolderTargetCache_.end())
        return cached->second;

    const size_t itemIndex = FindItemIndexByKey(entry.reference);
    const std::wstring& path = itemIndex < items_.size() &&
            !items_[itemIndex].parsingName.empty()
        ? items_[itemIndex].parsingName
        : entry.reference;
    auto target =
        snowdesktop::item_location::ResolveFolderTarget(path);
    dockFolderTargetCache_.insert_or_assign(
        std::move(cacheKey), target);
    return target;
}

bool DesktopApp::IsFolderDockEntry(const DockEntry& entry) const
{
    return entry.type == DockEntryType::FolderMapping ||
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
                        running.appUserModelId);
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
                    running.appUserModelId))
            continue;
        if (match)
            return std::nullopt;
        match = itemIndex;
    }
    return match;
}

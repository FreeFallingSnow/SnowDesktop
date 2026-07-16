#pragma once

#include <ctime>

extern inline const GridPage* FindGridPage(
    const std::vector<GridPage>& pages, const std::wstring& pageId);
extern inline int SlotFromCell(
    const std::vector<GridPage>& pages, const GridCell& cell);

inline size_t DesktopApp::FindWidgetIndexById(const std::wstring& id) const
{
    for (size_t i = 0; i < widgets_.size(); ++i)
        if (widgets_[i].id == id) return i;
    return static_cast<size_t>(-1);
}

inline bool DesktopApp::IsDockExclusiveItemKey(const std::wstring& key) const
{
    const std::wstring upper = ToUpperInvariant(key);
    return std::any_of(dockEntries_.begin(), dockEntries_.end(), [&](const DockEntry& entry) {
        return entry.type == DockEntryType::DesktopItem && !entry.keepOnDesktop &&
            ToUpperInvariant(entry.reference) == upper;
    });
}

inline bool DesktopApp::IsDockExclusiveWidgetId(const std::wstring& id) const
{
    return std::any_of(dockEntries_.begin(), dockEntries_.end(), [&](const DockEntry& entry) {
        return entry.type == DockEntryType::Collection && entry.reference == id;
    });
}

inline bool DesktopApp::IsRecycleBinDockEntry(const DockEntry& entry) const
{
    return entry.type == DockEntryType::DesktopItem &&
        _wcsicmp(entry.reference.c_str(), kDesktopIconClsidRecycleBin) == 0;
}

inline void DesktopApp::NormalizeDockRecycleBinPosition()
{
    std::stable_partition(dockEntries_.begin(), dockEntries_.end(),
        [this](const DockEntry& entry) { return !IsRecycleBinDockEntry(entry); });
}

inline void DesktopApp::LoadDockUsageStats()
{
    dockUsageStats_.clear();
    std::ifstream file(GetDataFilePath(L"SnowDesktop.dock-usage.json"), std::ios::binary);
    if (!file) return;
    std::ostringstream stream;
    stream << file.rdbuf();
    const std::string text = stream.str();

    const size_t entriesName = text.find("\"entries\"");
    const size_t arrayStart = entriesName == std::string::npos
        ? std::string::npos : text.find('[', entriesName);
    const size_t arrayEnd = arrayStart == std::string::npos
        ? std::string::npos : FindJsonArrayEnd(text, arrayStart);
    size_t position = arrayStart == std::string::npos ? 0 : arrayStart + 1;
    while (arrayEnd != std::string::npos &&
        (position = text.find('{', position)) != std::string::npos && position < arrayEnd)
    {
        const size_t objectEnd = FindJsonObjectEnd(text, position);
        if (objectEnd == std::string::npos || objectEnd > arrayEnd) break;
        const std::string object = text.substr(position, objectEnd - position + 1);
        std::string keyUtf8;
        int launchCount = 0;
        int lastUsed = 0;
        if (ReadJsonStringField(object, "key", keyUtf8) &&
            ReadJsonIntField(object, "launchCount", launchCount) && launchCount > 0)
        {
            ReadJsonIntField(object, "lastUsed", lastUsed);
            const std::wstring key = ToUpperInvariant(Utf8ToWide(keyUtf8));
            if (!key.empty())
                dockUsageStats_[key] = { launchCount, std::max(0, lastUsed) };
        }
        position = objectEnd + 1;
    }
}

inline void DesktopApp::SaveDockUsageStats() const
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

inline bool DesktopApp::IsDockUsageEligibleItem(const DesktopItem& item) const
{
    if (!item.desktopIconClsid.empty() || item.parsingName.empty())
        return false;
    const wchar_t* extension = PathFindExtensionW(item.parsingName.c_str());
    return extension &&
        (_wcsicmp(extension, L".lnk") == 0 || _wcsicmp(extension, L".url") == 0);
}

inline bool DesktopApp::RemoveDockDragOutItems(const std::vector<Item*>& sourceItems)
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
    if (DockContainer* dock = GetDockContainer())
        dock->InvalidateSlots();
    InvalidateDragStaticScene();
    return true;
}

inline void DesktopApp::RecordDockItemUsage(size_t itemIndex)
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
        if (DockContainer* dock = GetDockContainer())
            dock->InvalidateSlots();
        InvalidateDragStaticScene();
        if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

inline std::vector<size_t> DesktopApp::GetFrequentDockItemIndices() const
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
    for (size_t i = 0; i < std::min(limit, candidates.size()); ++i)
        result.push_back(candidates[i].itemIndex);
    return result;
}

inline bool DesktopApp::LaunchDesktopItem(size_t itemIndex)
{
    if (itemIndex >= items_.size() || items_[itemIndex].parsingName.empty())
        return false;
    HINSTANCE result = ShellExecuteW(hwnd_, L"open", items_[itemIndex].parsingName.c_str(),
        nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32)
        return false;
    RecordDockItemUsage(itemIndex);
    return true;
}

inline DockContainer* DesktopApp::GetDockContainer() const
{
    for (const auto& container : containers_)
        if (auto* dock = dynamic_cast<DockContainer*>(container.get())) return dock;
    return nullptr;
}

inline int DesktopApp::GetGridPageItemIconSize(const GridPage& page) const
{
    const int pitchX = page.cellWidth + (page.columns > 1 ? page.gapX : 0);
    const int pitchY = page.cellHeight + (page.rows > 1 ? page.gapY : 0);
    const float layoutScale = std::max(0.1f, std::min(
        static_cast<float>(std::max(1, pitchX)) / static_cast<float>(kCellWidth),
        static_cast<float>(std::max(1, pitchY)) / static_cast<float>(kMinCellHeight)));
    const int inset = std::max(1, static_cast<int>(std::round(2.0f * layoutScale)));
    if (page.cellHeight < static_cast<int>(std::round(50.0f * layoutScale)))
    {
        return std::max(1, std::min({
            static_cast<int>(std::round(32.0f * layoutScale)),
            std::max(1, page.cellWidth - inset * 2),
            std::max(1, page.cellHeight - inset * 2) }));
    }
    const float lineHeight = itemFontSize_ * 7.0f / 6.0f * layoutScale;
    const int textHeight = std::max(1,
        static_cast<int>(std::floor(lineHeight * 2.0f)) - 1);
    return std::max(1, std::min(
        std::max(1, page.cellWidth - inset * 2),
        std::max(1, page.cellHeight - textHeight - inset * 2)));
}

inline int DesktopApp::GetDockItemIconSize() const
{
    const GridPage* page = GetFirstPageGridPage();
    return page ? GetGridPageItemIconSize(*page) : kIconSize;
}

inline void DesktopApp::ApplyDockWorkAreaReservation()
{
    dockArea_ = {};
    if (!generalSettings_.dockEnabled || gridPages_.empty()) return;

    const GridPage* first = GetFirstPageGridPage();
    if (!first) return;
    auto it = std::find_if(gridPages_.begin(), gridPages_.end(),
        [&](const GridPage& page) { return &page == first; });
    if (it == gridPages_.end()) return;

    const RECT originalWorkArea = it->workArea;
    const int width = std::max(1, static_cast<int>(originalWorkArea.right - originalWorkArea.left));
    const int height = std::max(1, static_cast<int>(originalWorkArea.bottom - originalWorkArea.top));
    const bool vertical = dockSettings_.position == DockPosition::Left ||
        dockSettings_.position == DockPosition::Right;
    const int edgeExtent = vertical ? width : height;
    auto reserveEdge = [&](GridPage& page, int reserved, RECT* dockArea) {
        page.workArea = originalWorkArea;
        RECT area{};
        switch (dockSettings_.position)
        {
        case DockPosition::Top:
            area = RECT{ originalWorkArea.left, originalWorkArea.top,
                originalWorkArea.right, originalWorkArea.top + reserved };
            page.workArea.top = area.bottom;
            break;
        case DockPosition::Left:
            area = RECT{ originalWorkArea.left, originalWorkArea.top,
                originalWorkArea.left + reserved, originalWorkArea.bottom };
            page.workArea.left = area.right;
            break;
        case DockPosition::Right:
            area = RECT{ originalWorkArea.right - reserved, originalWorkArea.top,
                originalWorkArea.right, originalWorkArea.bottom };
            page.workArea.right = area.left;
            break;
        case DockPosition::Bottom:
        default:
            area = RECT{ originalWorkArea.left, originalWorkArea.bottom - reserved,
                originalWorkArea.right, originalWorkArea.bottom };
            page.workArea.bottom = area.top;
            break;
        }
        if (dockArea) *dockArea = area;
    };

    // Match the Dock thickness and its two edge gaps to the recalculated 1x1
    // component thickness and the grid's component-side margin.
    GridPage bestPage = *it;
    int bestReserved = 1;
    int bestError = INT_MAX;
    for (int reserved = 1; reserved < edgeExtent; ++reserved)
    {
        GridPage candidate = *it;
        reserveEdge(candidate, reserved, nullptr);
        ApplyIconSpacingToPage(candidate);
        const int componentMargin = vertical
            ? candidate.marginX : candidate.marginY;
        const int edgeDistance = std::max(kDockSpacing, componentMargin);
        const int innerGap = edgeDistance - componentMargin;
        const int panelThickness = GetGridPageItemIconSize(candidate) +
            kDockSpacing * 2;
        const int desiredReservation = dockSettings_.edgeAttached
            ? panelThickness + innerGap
            : panelThickness + edgeDistance + innerGap;
        const int error = std::abs(desiredReservation - reserved);
        if (error < bestError)
        {
            bestError = error;
            bestReserved = reserved;
            bestPage = candidate;
            if (error == 0) break;
        }
    }

    *it = bestPage;
    reserveEdge(*it, bestReserved, &dockArea_);
    ApplyIconSpacingToPage(*it);
}

inline void DesktopApp::CommitDockDrop(const std::vector<Item*>& sourceItems,
    Container* origin, size_t insertIndex, int mods)
{
    DockContainer* dock = GetDockContainer();
    if (!dock || sourceItems.empty()) return;

    if (origin == dock)
    {
        std::vector<size_t> indices;
        for (Item* item : sourceItems)
            if (auto* dockItem = dynamic_cast<DockEntryItem*>(item))
            {
                const size_t index = dockItem->GetEntryIndex();
                if (index < dockEntries_.size() &&
                    !IsRecycleBinDockEntry(dockEntries_[index]))
                    indices.push_back(index);
            }
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        if (indices.empty()) return;

        std::vector<DockEntry> moving;
        for (size_t index : indices)
            if (index < dockEntries_.size()) moving.push_back(dockEntries_[index]);
        for (auto it = indices.rbegin(); it != indices.rend(); ++it)
        {
            if (*it < insertIndex) --insertIndex;
            dockEntries_.erase(dockEntries_.begin() + static_cast<std::ptrdiff_t>(*it));
        }
        auto recycleBin = std::find_if(dockEntries_.begin(), dockEntries_.end(),
            [this](const DockEntry& entry) { return IsRecycleBinDockEntry(entry); });
        insertIndex = std::min(insertIndex,
            static_cast<size_t>(std::distance(dockEntries_.begin(), recycleBin)));
        dockEntries_.insert(dockEntries_.begin() + static_cast<std::ptrdiff_t>(insertIndex),
            moving.begin(), moving.end());
        NormalizeDockRecycleBinPosition();
        dock->InvalidateSlots();
        return;
    }

    const bool keepSource = (mods & MK_CONTROL) != 0;
    std::vector<DockEntry> additions;
    for (Item* source : sourceItems)
    {
        if (auto* icon = dynamic_cast<DesktopIcon*>(source))
        {
            DesktopItem* item = icon->GetDesktopItem();
            if (!item || item->layoutKey.empty()) continue;
            additions.push_back({ DockEntryType::DesktopItem,
                ToUpperInvariant(item->layoutKey), keepSource });
            continue;
        }

        auto* widget = dynamic_cast<Widget*>(source);
        DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
        if (data && data->type == DesktopWidgetType::Collection)
            additions.push_back({ DockEntryType::Collection, data->id, false });
    }
    if (additions.empty()) return;

    size_t genuinelyNew = 0;
    for (const DockEntry& addition : additions)
    {
        bool exists = std::any_of(dockEntries_.begin(), dockEntries_.end(),
            [&](const DockEntry& current) {
                return current.type == addition.type &&
                    ToUpperInvariant(current.reference) == ToUpperInvariant(addition.reference);
            });
        if (!exists) ++genuinelyNew;
    }
    if (!dock->HasCapacity(genuinelyNew))
    {
        MessageBeep(MB_ICONWARNING);
        return;
    }

    for (const DockEntry& addition : additions)
    {
        auto existing = std::find_if(dockEntries_.begin(), dockEntries_.end(),
            [&](const DockEntry& current) {
                return current.type == addition.type &&
                    ToUpperInvariant(current.reference) == ToUpperInvariant(addition.reference);
            });
        if (existing != dockEntries_.end())
        {
            const bool becomingExclusive = existing->keepOnDesktop && !addition.keepOnDesktop;
            existing->keepOnDesktop = addition.keepOnDesktop;
            if (addition.type == DockEntryType::Collection)
            {
                existing->keepOnDesktop = false;
                size_t widgetIndex = FindWidgetIndexById(addition.reference);
                if (widgetIndex < widgets_.size())
                    widgets_[widgetIndex].gridCell = { kDockPageId, 0, 0 };
                continue;
            }
            if (becomingExclusive)
            {
                RemoveDesktopKeysFromWidgets({ addition.reference });
                size_t itemIndex = FindItemIndexByKey(addition.reference);
                if (itemIndex < items_.size())
                    items_[itemIndex].gridCell = { kDockPageId, 0, 0 };
            }
            continue;
        }
        auto recycleBin = std::find_if(dockEntries_.begin(), dockEntries_.end(),
            [this](const DockEntry& entry) { return IsRecycleBinDockEntry(entry); });
        const size_t sortableEnd = static_cast<size_t>(
            std::distance(dockEntries_.begin(), recycleBin));
        insertIndex = IsRecycleBinDockEntry(addition)
            ? dockEntries_.size()
            : std::min(insertIndex, sortableEnd);
        dockEntries_.insert(dockEntries_.begin() + static_cast<std::ptrdiff_t>(insertIndex), addition);
        if (!IsRecycleBinDockEntry(addition))
            ++insertIndex;

        if (addition.keepOnDesktop && addition.type == DockEntryType::DesktopItem) continue;
        if (addition.type == DockEntryType::DesktopItem)
        {
            RemoveDesktopKeysFromWidgets({ addition.reference });
            size_t itemIndex = FindItemIndexByKey(addition.reference);
            if (itemIndex < items_.size()) items_[itemIndex].gridCell = { kDockPageId, 0, 0 };
        }
        else
        {
            size_t widgetIndex = FindWidgetIndexById(addition.reference);
            if (widgetIndex < widgets_.size()) widgets_[widgetIndex].gridCell = { kDockPageId, 0, 0 };
        }
    }
    NormalizeDockRecycleBinPosition();
    RefreshCollectedKeysCache();
    dock->InvalidateSlots();
    InvalidateDragStaticScene();
}

inline void DesktopApp::AddExternalItemsToDock(
    const std::vector<std::wstring>& newKeys, size_t insertIndex)
{
    DockContainer* dock = GetDockContainer();
    if (!dock || !dock->HasCapacity(newKeys.size())) return;
    for (const std::wstring& key : newKeys)
    {
        const std::wstring upper = ToUpperInvariant(key);
        if (upper.empty()) continue;
        bool exists = std::any_of(dockEntries_.begin(), dockEntries_.end(),
            [&](const DockEntry& entry) {
                return entry.type == DockEntryType::DesktopItem &&
                    ToUpperInvariant(entry.reference) == upper;
            });
        if (exists) continue;
        auto recycleBin = std::find_if(dockEntries_.begin(), dockEntries_.end(),
            [this](const DockEntry& entry) { return IsRecycleBinDockEntry(entry); });
        insertIndex = std::min(insertIndex,
            static_cast<size_t>(std::distance(dockEntries_.begin(), recycleBin)));
        dockEntries_.insert(dockEntries_.begin() + static_cast<std::ptrdiff_t>(insertIndex),
            DockEntry{ DockEntryType::DesktopItem, upper, false });
        ++insertIndex;
        size_t itemIndex = FindItemIndexByKey(upper);
        if (itemIndex < items_.size()) items_[itemIndex].gridCell = { kDockPageId, 0, 0 };
    }
    NormalizeDockRecycleBinPosition();
    RefreshCollectedKeysCache();
    RebuildContainersAndItems();
    LayoutItems();
}

inline bool DesktopApp::FindDockReturnCell(
    std::unordered_set<std::wstring>& usedSlots,
    const std::wstring& preferredPageId, int startSlot, GridSpan span,
    GridCell& result)
{
    span.columns = std::max(1, span.columns);
    span.rows = std::max(1, span.rows);
    if (TryFindFreeCell(span, usedSlots, result, preferredPageId, startSlot))
        return true;

    // Try saved pages that are currently off-screen before allocating a new one.
    for (const std::wstring& pageId : savedPageIds_)
    {
        auto columnsIt = savedPageColumns_.find(pageId);
        auto rowsIt = savedPageRows_.find(pageId);
        if (columnsIt == savedPageColumns_.end() || rowsIt == savedPageRows_.end()) continue;
        const int columns = std::max(1, columnsIt->second);
        const int rows = std::max(1, rowsIt->second);
        for (int slot = 0; slot < columns * rows; ++slot)
        {
            GridCell candidate{ pageId, slot / rows, slot % rows };
            if (candidate.column + span.columns <= columns &&
                candidate.row + span.rows <= rows &&
                !AreGridSlotsMarked(usedSlots, candidate, span))
            {
                result = candidate;
                return true;
            }
        }
    }

    if (gridPages_.empty()) return false;
    const std::vector<size_t> order = BuildMonitorRenderOrder();
    const GridPage& reference = gridPages_[order.empty() ? 0 : order.back()];
    const std::wstring pageId = GeneratePageId();
    RememberSavedPageId(pageId);
    savedPageColumns_[pageId] = std::max(span.columns, reference.columns);
    savedPageRows_[pageId] = std::max(span.rows, reference.rows);
    result = { pageId, 0, 0 };
    return true;
}

inline bool DesktopApp::DropItemsIntoDockCollection(
    const std::vector<Item*>& sourceItems, Container* origin,
    DockEntryItem* targetItem, int mods)
{
    if (!targetItem || targetItem->GetEntryType() != DockEntryType::Collection)
        return false;
    size_t widgetIndex = FindWidgetIndexById(targetItem->GetReference());
    if (widgetIndex >= widgets_.size() ||
        widgets_[widgetIndex].type != DesktopWidgetType::Collection)
        return false;

    Collection collection(&widgets_[widgetIndex], this);
    DragSourceList sourceList = BuildDragSourceList(sourceItems, origin);
    DropPreviewList preview = BuildDropPreviewList(sourceList, &collection,
        nullptr, HitRegion::SortAfter, mods, dragSession_.CurrentPoint());
    return ExecuteDropPipeline(sourceList, preview);
}

inline void DesktopApp::MoveDockItemsToDesktop(
    const std::vector<Item*>& sourceItems, GridCell targetCell)
{
    std::vector<size_t> indices;
    for (Item* source : sourceItems)
        if (auto* item = dynamic_cast<DockEntryItem*>(source))
            indices.push_back(item->GetEntryIndex());
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    if (indices.empty()) return;

    std::vector<DockEntry> moving;
    for (size_t index : indices)
        if (index < dockEntries_.size()) moving.push_back(dockEntries_[index]);

    std::unordered_set<std::wstring> usedSlots;
    for (const auto& widget : widgets_)
        if (widget.gridCell.pageId != kDockPageId)
            MarkGridArea(usedSlots, widget.gridCell, widget.gridSpan);
    for (const auto& item : items_)
        if (!item.name.empty() && item.gridCell.pageId != kDockPageId && !IsItemInAnyWidget(item))
            MarkGridArea(usedSlots, item.gridCell, item.gridSpan);

    const GridPage* targetPage = FindGridPage(gridPages_, targetCell.pageId);
    int startSlot = targetPage ? SlotFromCell(gridPages_, targetCell) : 0;
    for (const DockEntry& entry : moving)
    {
        if (entry.keepOnDesktop) continue;
        GridSpan span{ 1, 1 };
        if (entry.type == DockEntryType::Collection)
        {
            size_t widgetIndex = FindWidgetIndexById(entry.reference);
            if (widgetIndex < widgets_.size()) span = widgets_[widgetIndex].gridSpan;
        }
        GridCell freeCell;
        if (!FindDockReturnCell(usedSlots, targetCell.pageId, startSlot, span, freeCell))
            continue;
        MarkGridArea(usedSlots, freeCell, span);
        ++startSlot;
        if (entry.type == DockEntryType::DesktopItem)
        {
            size_t itemIndex = FindItemIndexByKey(entry.reference);
            if (itemIndex < items_.size()) items_[itemIndex].gridCell = freeCell;
        }
        else
        {
            size_t widgetIndex = FindWidgetIndexById(entry.reference);
            if (widgetIndex < widgets_.size()) widgets_[widgetIndex].gridCell = freeCell;
        }
    }

    for (auto it = indices.rbegin(); it != indices.rend(); ++it)
        dockEntries_.erase(dockEntries_.begin() + static_cast<std::ptrdiff_t>(*it));
    RefreshCollectedKeysCache();
    RebuildContainersAndItems();
    LayoutItems();
    InvalidateDragStaticScene();
}

inline void DesktopApp::RestoreDockEntriesToDesktop()
{
    if (dockEntries_.empty()) return;
    const GridPage* first = GetFirstPageGridPage();
    const std::wstring preferredPage = first ? first->id : L"";
    std::unordered_set<std::wstring> usedSlots;
    for (const auto& widget : widgets_)
        if (widget.gridCell.pageId != kDockPageId)
            MarkGridArea(usedSlots, widget.gridCell, widget.gridSpan);
    for (const auto& item : items_)
        if (!item.name.empty() && item.gridCell.pageId != kDockPageId && !IsItemInAnyWidget(item))
            MarkGridArea(usedSlots, item.gridCell, item.gridSpan);

    int startSlot = 0;
    for (const DockEntry& entry : dockEntries_)
    {
        if (entry.keepOnDesktop) continue;
        GridSpan span{ 1, 1 };
        if (entry.type == DockEntryType::Collection)
        {
            size_t widgetIndex = FindWidgetIndexById(entry.reference);
            if (widgetIndex < widgets_.size()) span = widgets_[widgetIndex].gridSpan;
        }
        GridCell cell;
        if (!FindDockReturnCell(usedSlots, preferredPage, startSlot, span, cell)) continue;
        MarkGridArea(usedSlots, cell, span);
        ++startSlot;
        if (entry.type == DockEntryType::DesktopItem)
        {
            size_t itemIndex = FindItemIndexByKey(entry.reference);
            if (itemIndex < items_.size()) items_[itemIndex].gridCell = cell;
        }
        else
        {
            size_t widgetIndex = FindWidgetIndexById(entry.reference);
            if (widgetIndex < widgets_.size()) widgets_[widgetIndex].gridCell = cell;
        }
    }
    dockEntries_.clear();
    RefreshCollectedKeysCache();
}

inline bool DesktopApp::DrawDockControlBackground(
    ID2D1DeviceContext* ctx, RECT rect, int state)
{
    if (!ctx || IsRectEmptyRect(rect)) return false;
    PersonalizationSettings appearance = !dockSettings_.followPersonalization
        ? dockSettings_.appearance
        : PersonalizationSettings::DarkPreset();
    if (settingsWindow_)
        appearance = settingsWindow_->GetDockAppearance();

    const float luminance = appearance.widgetBgR * 0.2126f +
        appearance.widgetBgG * 0.7152f + appearance.widgetBgB * 0.0722f;
    const bool lightSurface = luminance > 0.58f && appearance.widgetAlpha > 0.10f;
    const bool active = state > 0;
    const D2D1_COLOR_F fill = active
        ? D2D1::ColorF(0.39f, 0.66f, 1.0f, lightSurface ? 0.20f : 0.25f)
        : (lightSurface
            ? D2D1::ColorF(0.08f, 0.11f, 0.16f, 0.075f)
            : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.11f));
    const D2D1_COLOR_F border = active
        ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.88f)
        : (lightSurface
            ? D2D1::ColorF(0.08f, 0.11f, 0.16f, 0.14f)
            : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f));
    const int width = std::max(1, static_cast<int>(rect.right - rect.left));
    const int height = std::max(1, static_cast<int>(rect.bottom - rect.top));
    const float scale = static_cast<float>(std::min(width, height)) / 52.0f;
    DrawBeautifiedIconPlate(ctx, rect, fill, border,
        (active ? 1.6f : 1.0f) * std::max(0.75f, scale));
    return lightSurface;
}

inline void DesktopApp::DrawDockEntry(ID2D1DeviceContext* ctx,
    const DockEntry& entry, RECT rect, int state)
{
    if (!ctx) return;
    const int iconSize = GetDockItemIconSize();
    RECT iconRect{
        rect.left + (rect.right - rect.left - iconSize) / 2,
        rect.top + (rect.bottom - rect.top - iconSize) / 2,
        rect.left + (rect.right - rect.left + iconSize) / 2,
        rect.top + (rect.bottom - rect.top + iconSize) / 2
    };

    auto drawDesktopItem = [&](const DesktopItem& item, RECT target, int visualState) {
        RECT bitmapTarget = target;
        const bool recycleBin = _wcsicmp(item.desktopIconClsid.c_str(),
            kDesktopIconClsidRecycleBin) == 0;
        if (recycleBin)
        {
            DrawDockControlBackground(ctx, target, visualState);
            const int shortSide = std::max(1, static_cast<int>(std::min(
                target.right - target.left, target.bottom - target.top)));
            const int inset = std::max(1, static_cast<int>(std::round(shortSide * 0.16f)));
            InflateRect(&bitmapTarget, -inset, -inset);
            visualState = 0;
        }
        if (visualState > 0)
        {
            DrawD2DRoundedRectangle(ctx, target, 10.0f,
                visualState == 2
                    ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.24f)
                    : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f),
                visualState == 2
                    ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.72f)
                    : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f));
        }
        const float alpha = item.isCut ? 0.4f : 1.0f;
        if (item.iconState == IconState::Loading)
            DrawPlaceholderIcon(ctx, item.sysIconIndex, bitmapTarget, alpha, !recycleBin);
        else if (ID2D1Bitmap1* bitmap = recycleBin
            ? GetOrCreateD2DBitmap(item.iconBitmap, false)
            : GetOrCreateD2DBitmap(item.iconBitmap))
            ctx->DrawBitmap(bitmap, ToD2DRect(bitmapTarget), alpha,
                D2D1_INTERPOLATION_MODE_LINEAR);
        else
            DrawPlaceholderIcon(ctx, item.sysIconIndex, bitmapTarget, alpha, !recycleBin);
        if (ShouldDrawShortcutArrow(item.isShortcut, item.isApplicationShortcut) &&
            item.iconState != IconState::Loading)
            DrawShortcutArrowOverlay(ctx, bitmapTarget, alpha);
    };

    if (entry.type == DockEntryType::DesktopItem)
    {
        size_t index = FindItemIndexByKey(entry.reference);
        if (index >= items_.size()) return;
        drawDesktopItem(items_[index], iconRect, state);
        return;
    }

    size_t widgetIndex = FindWidgetIndexById(entry.reference);
    if (widgetIndex >= widgets_.size()) return;
    const DesktopWidget& widget = widgets_[widgetIndex];
    const int innerSize = std::max(1, static_cast<int>(
        std::min(iconRect.right - iconRect.left, iconRect.bottom - iconRect.top)));
    const int smallIconSize = std::max(1, (innerSize - kDockSpacing) / 2);
    const int groupSize = smallIconSize * 2 + kDockSpacing;
    const int groupLeft = iconRect.left + (innerSize - groupSize) / 2;
    const int groupTop = iconRect.top + (innerSize - groupSize) / 2;
    for (size_t i = 0; i < std::min<size_t>(4, widget.itemKeys.size()); ++i)
    {
        size_t itemIndex = FindItemIndexByKey(widget.itemKeys[i]);
        if (itemIndex >= items_.size()) continue;
        int col = static_cast<int>(i % 2);
        int row = static_cast<int>(i / 2);
        const int left = groupLeft + col * (smallIconSize + kDockSpacing);
        const int top = groupTop + row * (smallIconSize + kDockSpacing);
        RECT cell{ left, top, left + smallIconSize, top + smallIconSize };
        drawDesktopItem(items_[itemIndex], cell, 0);
    }
}

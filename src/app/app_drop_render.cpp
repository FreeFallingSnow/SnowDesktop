#include "app.h"
#include "../pending_drop_rules.h"

// Desktop drop-preview rendering, caching and deferred placement.

bool DesktopApp::ApplyPendingFolderPlacements(
    DesktopWidget& targetWidget,
    const std::wstring& widgetId,
    const std::wstring& popupSourceId)
{
    if (!pendingLandingCache_.active ||
        pendingLandingCache_.folderPlacements.empty())
        return false;
    if (pendingLandingCache_.tick != 0 &&
        GetTickCount() - pendingLandingCache_.tick > 10000)
    {
        pendingLandingCache_.Clear();
        return false;
    }

    bool changed = false;
    std::vector<PendingFolderPlacement> remaining;
    remaining.reserve(
        pendingLandingCache_.folderPlacements.size());
    for (auto& placement :
        pendingLandingCache_.folderPlacements)
    {
        const bool widgetMatches =
            !widgetId.empty() &&
            !placement.widgetId.empty() &&
            placement.widgetId == widgetId;
        const bool popupMatches =
            !popupSourceId.empty() &&
            !placement.popupSourceId.empty() &&
            placement.popupSourceId == popupSourceId;
        const bool pathMatches =
            placement.sourceFolderPath.empty() ||
            PathsEqualInsensitive(
                placement.sourceFolderPath,
                targetWidget.sourceFolderPath);
        if ((!widgetMatches && !popupMatches) || !pathMatches)
        {
            remaining.push_back(std::move(placement));
            continue;
        }

        std::vector<FolderEntry> inserted =
            snowdesktop::pending_drop_rules::ExtractMatching(
                targetWidget.folderEntries,
                [&](const FolderEntry& entry) {
                    return !placement.existingPaths.contains(
                        ToUpperInvariant(entry.fullPath));
                });

        if (!placement.sourceNames.empty() &&
            inserted.size() > 1)
        {
            std::vector<FolderEntry> ordered;
            ordered.reserve(inserted.size());
            for (const auto& sourceName :
                placement.sourceNames)
            {
                auto match = std::find_if(
                    inserted.begin(), inserted.end(),
                    [&](const FolderEntry& entry) {
                        return MatchPendingName(
                            entry.name, sourceName);
                    });
                if (match == inserted.end())
                    continue;
                ordered.push_back(std::move(*match));
                inserted.erase(match);
            }
            std::move(
                inserted.begin(), inserted.end(),
                std::back_inserter(ordered));
            inserted = std::move(ordered);
        }

        if (!inserted.empty())
        {
            snowdesktop::pending_drop_rules::InsertAt(
                targetWidget.folderEntries,
                placement.insertIndex,
                std::move(inserted));
            snowdesktop::folder_sort_rules::RewriteOrderKeys(
                targetWidget.folderEntries,
                targetWidget.itemKeys);
            targetWidget.folderSortMode =
                snowdesktop::folder_sort_rules::kManual;
            changed = true;
        }
        // A successful file operation gets exactly one reconciliation pass.
        // If it overwrote an existing path there is no new member to move, and
        // retaining this snapshot could capture an unrelated later file.
    }

    pendingLandingCache_.folderPlacements =
        std::move(remaining);
    pendingLandingCache_.active =
        !pendingLandingCache_.entries.empty() ||
        !pendingLandingCache_.folderPlacements.empty();
    return changed;
}

void DesktopApp::DrawDesktopDropPreviewList(ID2D1DeviceContext* ctx,
    const DropPreviewList& preview)
{
    if (!ctx) return;
    for (const auto& landing : preview.landings)
    {
        if (landing.kind != DropLandingKind::DesktopCell) continue;
        GridSpan span{
            std::max(1, landing.span.columns),
            std::max(1, landing.span.rows)
        };
        RECT targetRect = GetGridRect(gridPages_, landing.cell, span);
        DrawD2DRoundedRectangle(ctx, targetRect, 6.0f,
            D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.12f),
            D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.50f), 2.0f);
    }
}

/**
 * @brief 获取或重建缓存的桌面放置预览。
 *
 * 拖拽渲染每帧调用 DrawDropPreview → BuildDropPreviewList → BuildDesktopLandings，
 * 后者遍历全部 items/widgets 搜索空位。当鼠标位置/动作/目标不变时复用缓存，
 * 避免每帧重建导致卡顿（尤其阶段2-4全页/跨页/新建页搜索）。
 */
const DropPreviewList& DesktopApp::GetCachedDesktopDropPreview(
    bool hasItemDrag, const DragSourceList& sourceList,
    Container* target, Slot* slot, HitRegion region, int mods, POINT dragPoint)
{
    const size_t sourceCount = sourceList.entries.size();
    // 判断缓存是否有效：位置、动作、目标、源数量均未变
    const bool cacheValid = !cachedDropPreview_.landings.empty() &&
        cachedDropPreviewHasItems_ == hasItemDrag &&
        cachedDropPreviewPoint_.x == dragPoint.x &&
        cachedDropPreviewPoint_.y == dragPoint.y &&
        cachedDropPreviewMods_ == mods &&
        cachedDropPreviewTarget_ == target &&
        cachedDropPreviewSlot_ == slot &&
        cachedDropPreviewRegion_ == region &&
        cachedDropPreviewSourceCount_ == sourceCount;

    if (!cacheValid)
    {
        if (hasItemDrag)
        {
            cachedDropPreview_ = BuildDropPreviewList(sourceList, target, slot, region, mods, dragPoint);
        }
        else
        {
            GridCell targetCell = CellFromPoint(dragPoint);
            if (targetCell.pageId.empty())
                cachedDropPreview_ = {};
            else
                cachedDropPreview_ = BuildExternalDesktopPreviewList(targetCell,
                    static_cast<size_t>(std::max(
                        1, dragDropController_.
                            ExternalSummary().fileCount)));
        }
        cachedDropPreviewPoint_ = dragPoint;
        cachedDropPreviewMods_ = mods;
        cachedDropPreviewTarget_ = target;
        cachedDropPreviewSlot_ = slot;
        cachedDropPreviewRegion_ = region;
        cachedDropPreviewHasItems_ = hasItemDrag;
        cachedDropPreviewSourceCount_ = sourceCount;
    }
    return cachedDropPreview_;
}

/**
 * @brief 应用缓存的放置结果，将新创建的文件分配到正确的网格位置或组件中。
 */
void DesktopApp::ApplyPendingPlacement()
{
    if (!pendingLandingCache_.active) return;
    if (GetTickCount() - pendingLandingCache_.tick > 10000)
    {
        pendingLandingCache_.Clear();
        return;
    }

    std::unordered_set<std::wstring> usedSlots;
    for (const auto& w : widgets_)
        if (!IsGroupedWidget(w))
            MarkGridArea(usedSlots, w.gridCell, w.gridSpan);
    for (const auto& item : items_)
    {
        std::wstring key = ToUpperInvariant(item.layoutKey);
        if (!key.empty() && !pendingLandingCache_.existingDesktopKeys.contains(key))
            continue;
        if (!item.name.empty() && !IsItemInAnyWidget(item))
            MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }

    auto findWidgetContainer = [&](const std::wstring& widgetId) -> WidgetContainer* {
        for (auto& container : containers_)
        {
            auto* widget = dynamic_cast<WidgetContainer*>(container.get());
            DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
            if (data && data->id == widgetId)
                return widget;
        }
        const size_t groupedIndex =
            FindCollectionGroupIndexForChild(widgetId);
        if (groupedIndex < widgets_.size())
            for (auto& container : containers_)
            {
                auto* widget =
                    dynamic_cast<WidgetContainer*>(
                        container.get());
                if (widget &&
                    widget->GetWidgetData() ==
                        &widgets_[groupedIndex])
                    return widget;
            }
        return nullptr;
    };

    std::vector<bool> entryUsed(pendingLandingCache_.entries.size(), false);
    bool changed = false;
    for (auto& widget : widgets_)
    {
        if (widget.type != DesktopWidgetType::FolderMapping)
            continue;
        changed = ApplyPendingFolderPlacements(
            widget, widget.id) || changed;
    }
    for (size_t itemIndex = 0; itemIndex < items_.size(); ++itemIndex)
    {
        auto& item = items_[itemIndex];
        std::wstring key = ToUpperInvariant(item.layoutKey);
        if (key.empty() || pendingLandingCache_.existingDesktopKeys.contains(key))
            continue;

        for (size_t e = 0; e < pendingLandingCache_.entries.size(); ++e)
        {
            if (entryUsed[e]) continue;
            const auto& landing = pendingLandingCache_.entries[e];
            bool matchesLanding = false;
            if (!landing.createdPath.empty())
            {
                matchesLanding =
                    PathsEqualInsensitive(item.parsingName, landing.createdPath) ||
                    PathsEqualInsensitive(FileNameFromPath(item.parsingName),
                        FileNameFromPath(landing.createdPath)) ||
                    PathsEqualInsensitive(item.name, FileNameFromPath(landing.createdPath));
            }
            if (!matchesLanding)
            {
                matchesLanding =
                    MatchPendingName(item.name, landing.sourceName) ||
                    (!item.parsingName.empty() &&
                     MatchPendingName(FileNameFromPath(item.parsingName), landing.sourceName));
            }
            if (!matchesLanding) continue;

            if (landing.kind == DropLandingKind::WidgetIndex && !landing.widgetId.empty())
            {
                WidgetContainer* widget = findWidgetContainer(landing.widgetId);
                const size_t widgetIndex =
                    FindWidgetIndexById(landing.widgetId);
                DesktopWidget* widgetData =
                    widgetIndex < widgets_.size()
                        ? &widgets_[widgetIndex]
                        : nullptr;
                if (!widgetData) break;

                item.gridCell = widgetData->gridCell;
                bool allowKey = !widget || landing.action == DropAction::Link || widget->AllowsDesktopKey(key);
                if (allowKey)
                {
                    // Auto-collect may already have appended this new key.
                    // Remove every provisional owner, then restore the exact
                    // preview boundary in the requested target.
                    RemoveDesktopKeysFromWidgets({key});
                    std::vector<std::wstring> insertedKey{key};
                    snowdesktop::pending_drop_rules::InsertAt(
                        widgetData->itemKeys,
                        landing.insertIndex,
                        std::move(insertedKey));
                    RefreshCollectedKeysCache();
                    if (widget) widget->InvalidateSlots();
                }
            }
            else if (landing.kind == DropLandingKind::DesktopCell)
            {
                GridSpan span = item.gridSpan;
                span.columns = std::max(1, span.columns);
                span.rows = std::max(1, span.rows);

                GridCell cell = landing.cell;

                // 预分配的新溢出页：若 pageId 不在 savedPageIds_ 里，先创建
                if (!cell.pageId.empty() &&
                    std::find(savedPageIds_.begin(), savedPageIds_.end(), cell.pageId) == savedPageIds_.end())
                {
                    RememberSavedPageId(cell.pageId);
                    // 参考末屏显示器的网格维度
                    auto monitorOrder = BuildMonitorRenderOrder();
                    const GridPage* refPage = !monitorOrder.empty()
                        ? &gridPages_[monitorOrder.back()] : GetFirstPageGridPage();
                    if (!refPage) break;
                    savedPageColumns_[cell.pageId] = std::max(1, refPage->columns);
                    savedPageRows_[cell.pageId] = std::max(1, refPage->rows);
                }

                bool found = false;
                if (IsGridAreaValid(cell, span) && !AreGridSlotsMarked(usedSlots, cell, span))
                {
                    found = true;
                }
                else
                {
                    found = TryFindFreeCell(span, usedSlots, cell, landing.cell.pageId,
                        SlotFromCell(gridPages_, landing.cell));
                }
                if (!found) break;
                item.gridCell = cell;
                item.slot = SlotFromCell(gridPages_, cell);
                item.selected = true;
                MarkGridArea(usedSlots, cell, span);
            }

            entryUsed[e] = true;
            changed = true;
            break;
        }
    }

    std::vector<PendingLandingEntry> remaining;
    for (size_t i = 0; i < pendingLandingCache_.entries.size(); ++i)
        if (!entryUsed[i])
            remaining.push_back(pendingLandingCache_.entries[i]);

    pendingLandingCache_.entries = std::move(remaining);
    pendingLandingCache_.active =
        !pendingLandingCache_.entries.empty() ||
        !pendingLandingCache_.folderPlacements.empty();
    if (!pendingLandingCache_.active)
        pendingLandingCache_.existingDesktopKeys.clear();

    if (changed)
    {
        LayoutItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

// ── 网格全局函数 ──────────────────────────────────────────

/**
 * @brief 根据页面 ID 在页面列表中查找对应的网格页面。
 * @param pages 页面列表。
 * @param pageId 页面 ID。
 * @return 找到的页面指针，未找到返回 nullptr。
 */

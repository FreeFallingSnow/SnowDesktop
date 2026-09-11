#include "app.h"
#include "../pending_drop_rules.h"
#include "pending_drop_completion.h"

// Desktop drop-preview rendering, caching and deferred placement.

bool DesktopApp::ApplyPendingFolderPlacements(DesktopWidget& targetWidget,
    const std::wstring& widgetId, const std::wstring& popupSourceId)
{
    bool changed = false;
    for (auto& cache : pendingLandingCaches_)
        changed = ApplyPendingFolderPlacements(cache, targetWidget, widgetId, popupSourceId) || changed;
    std::erase_if(pendingLandingCaches_, [](const auto& cache) { return !cache.active; });
    return changed;
}

bool DesktopApp::ApplyPendingFolderPlacements(
    PendingLandingCache& cache, DesktopWidget& targetWidget,
    const std::wstring& widgetId,
    const std::wstring& popupSourceId)
{
    if (!cache.active ||
        cache.folderPlacements.empty())
        return false;
    bool changed = false;
    std::vector<PendingFolderPlacement> remaining;
    remaining.reserve(
        cache.folderPlacements.size());
    for (auto& placement :
        cache.folderPlacements)
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
                    if (placement.existingPaths.contains(ToUpperInvariant(entry.fullPath))) return false;
                    if (!placement.createdPaths.empty())
                        return std::any_of(placement.createdPaths.begin(), placement.createdPaths.end(),
                            [&](const auto& path) { return snowdesktop::pending_drop::MatchesExactPath(entry.fullPath, path); });
                    return placement.sourceNames.empty() ||
                        std::any_of(placement.sourceNames.begin(), placement.sourceNames.end(),
                            [&](const auto& name) { return MatchPendingName(entry.name, name); });
                });

        if ((!placement.createdPaths.empty() || !placement.sourceNames.empty()) &&
            inserted.size() > 1)
        {
            std::vector<FolderEntry> ordered;
            ordered.reserve(inserted.size());
            for (const auto& sourceName :
                (placement.createdPaths.empty() ? placement.sourceNames : placement.createdPaths))
            {
                auto match = std::find_if(
                    inserted.begin(), inserted.end(),
                    [&](const FolderEntry& entry) {
                        return placement.createdPaths.empty() ? MatchPendingName(entry.name, sourceName) :
                            snowdesktop::pending_drop::MatchesExactPath(entry.fullPath, sourceName);
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
            targetWidget.contentSortColumn =
                snowdesktop::list_detail_rules::Column::None;
            changed = true;
        }
        // A successful file operation gets exactly one reconciliation pass.
        // If it overwrote an existing path there is no new member to move, and
        // retaining this snapshot could capture an unrelated later file.
    }

    cache.folderPlacements =
        std::move(remaining);
    cache.active =
        !cache.entries.empty() ||
        !cache.folderPlacements.empty();
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
    bool changed = false;
    std::unordered_set<std::wstring> claimed;
    for (auto& cache : pendingLandingCaches_)
        changed = ApplyPendingPlacement(cache, claimed) || changed;
    std::erase_if(pendingLandingCaches_, [](const auto& cache) { return !cache.active; });
    if (changed)
    {
        LayoutItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

bool DesktopApp::ApplyPendingPlacement(PendingLandingCache& cache,
    std::unordered_set<std::wstring>& claimed)
{
    if (!cache.active) return false;
    std::unordered_set<std::wstring> usedSlots;
    for (const auto& w : widgets_)
        if (!IsGroupedWidget(w))
            MarkGridArea(usedSlots, w.gridCell, w.gridSpan);
    for (const auto& item : items_)
    {
        std::wstring key = ToUpperInvariant(item.layoutKey);
        if (!key.empty() && !claimed.contains(key) && !cache.existingDesktopKeys.contains(key))
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

    bool changed = false;
    for (auto& widget : widgets_)
    {
        if (widget.type != DesktopWidgetType::FolderMapping)
            continue;
        changed = ApplyPendingFolderPlacements(
            cache, widget, widget.id, {}) || changed;
    }
    for (size_t e = 0; e < cache.entries.size(); ++e)
    {
        const auto& landing = cache.entries[e];
        for (size_t itemIndex = 0; itemIndex < items_.size(); ++itemIndex)
        {
            auto& item = items_[itemIndex];
            const std::wstring key = ToUpperInvariant(item.layoutKey);
            if (key.empty() || claimed.contains(key) || cache.existingDesktopKeys.contains(key)) continue;
            const bool matchesLanding = !landing.createdPath.empty()
                ? snowdesktop::pending_drop::MatchesExactPath(item.parsingName, landing.createdPath)
                : MatchPendingName(item.name, landing.sourceName) ||
                    (!item.parsingName.empty() && MatchPendingName(FileNameFromPath(item.parsingName), landing.sourceName));
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
                    if (!snowdesktop::pending_drop::CommitKeyedLanding(widgets_, item, landing, key)) break;
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

            claimed.insert(key);
            changed = true;
            break;
        }
    }

    cache.entries.clear();
    cache.active = !cache.folderPlacements.empty();
    return changed;
}

// ── 网格全局函数 ──────────────────────────────────────────

/**
 * @brief 根据页面 ID 在页面列表中查找对应的网格页面。
 * @param pages 页面列表。
 * @param pageId 页面 ID。
 * @return 找到的页面指针，未找到返回 nullptr。
 */

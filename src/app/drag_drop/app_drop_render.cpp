#include "app/app.h"
#include "drag_drop/pending_drop_rules.h"
#include "drag_drop/pending_drop_completion.h"
#include "drag_drop/pending_drop_placement.h"
#include "drag_drop/desktop_output_placement.h"
#include "icons/large_icon_renderer.h"
#include "desktop_preview_source.h"
#include "desktop_preview_cache.h"
#include "widgets/folder_member_order.h"

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
    return snowdesktop::pending_drop::ApplyFolder(cache, targetWidget, widgetId, popupSourceId,
        MatchPendingName, ToUpperInvariant, PathsEqualInsensitive);
}

void DesktopApp::ApplyPendingFolderPreviewPlacements(
    const snowdesktop::shell_refresh::FolderSnapshot& snapshot)
{
    if (!snapshot.complete || pendingLandingCaches_.empty()) return;
    const auto key = snowdesktop::shell_refresh::FolderKey(snapshot.path);
    const auto popupId = L"folder-preview:" + key;
    const auto matches = [&](const PendingFolderPlacement& placement) {
        return placement.popupSourceId == popupId;
    };
    if (std::none_of(pendingLandingCaches_.begin(), pendingLandingCaches_.end(),
            [&](const auto& cache) {
                return std::any_of(cache.folderPlacements.begin(), cache.folderPlacements.end(), matches);
            })) return;

    auto sort = folderPreviewSort_.Get(key);
    if (sort.mode != snowdesktop::folder_sort_rules::kManual)
    {
        // The user may select a new sort rule while the operation is running.
        for (auto& cache : pendingLandingCaches_)
        {
            std::erase_if(cache.folderPlacements, matches);
            cache.active = !cache.entries.empty() || !cache.folderPlacements.empty();
        }
        std::erase_if(pendingLandingCaches_, [](const auto& cache) { return !cache.active; });
        return;
    }

    DesktopWidget target;
    target.sourceFolderPath = snapshot.path;
    target.folderEntries = snapshot.entries;
    target.folderSortMode = sort.mode;
    target.folderSortAscending = sort.ascending;
    target.itemKeys = std::move(sort.order);
    snowdesktop::folder_member_order::Refresh(target, ToUpperInvariant);
    if (ApplyPendingFolderPlacements(target, {}, popupId) &&
        !folderPreviewSort_.Set(key, {sort.mode, sort.ascending, std::move(target.itemKeys)}))
        MessageBeep(MB_ICONWARNING);
}

void DesktopApp::DrawDesktopDropPreviewList(ID2D1DeviceContext* ctx,
    const DropPreviewList& preview)
{
    if (!ctx) return;
    const snowdesktop::desktop_preview_source::Lookup sourceLookup(items_, dragSession_.IsActive());
    for (const auto& landing : preview.landings)
    {
        if (landing.kind != DropLandingKind::DesktopCell) continue;
        GridSpan span{
            std::max(1, landing.span.columns),
            std::max(1, landing.span.rows)
        };
        RECT targetRect = GetGridRect(gridPages_, landing.cell, span);
        if (dragSession_.IsActive())
        {
            const auto& sources = dragSession_.SourceList().entries;
            const auto source = std::find_if(sources.begin(), sources.end(),
                [&](const auto& entry) { return entry.sourceIndex == landing.sourceIndex; });
            const auto* sourceItem = source != sources.end() ? sourceLookup.Find(*source) : nullptr;
            if (sourceItem)
            {
                DesktopWidget geometry; geometry.bounds = targetRect; geometry.gridCell = landing.cell;
                if (const auto* page = FindGridPage(gridPages_, landing.cell.pageId)) geometry.cellScale = GetGridPageCuScale(*page);
                const auto config = snowdesktop::large_icon_render_rules::ResolveComponentRadius(
                    EffectiveLargeIconConfig(*sourceItem), CurrentPersonalization().cornerRadius);
                snowdesktop::large_icon_renderer::DrawPlacementPreview(ctx, config, GetStandaloneWidgetFrameRect(geometry),
                    GetItemLayoutScale(targetRect), true, &targetRect);
                continue;
            }
        }
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
    const size_t externalCount = hasItemDrag ? 0 : static_cast<size_t>(std::max(1, dragDropController_.ExternalSummary().fileCount));
    return snowdesktop::desktop_preview_cache::Read(
        {cachedDropPreview_, cachedDropPreviewPoint_, cachedDropPreviewMods_, cachedDropPreviewTarget_, cachedDropPreviewSlot_,
            cachedDropPreviewRegion_, cachedDropPreviewHasItems_, cachedDropPreviewSourceCount_,
            cachedDropPreviewSceneRevision_, cachedDropPreviewExternalCount_},
        {hasItemDrag, dragPoint, mods, target, slot, region, sourceList.entries.size(), dragSession_.StaticSceneRevision(), externalCount},
        [&] { return BuildDropPreviewList(sourceList, target, slot, region, mods, dragPoint); },
        [&]() -> DropPreviewList {
            GridCell targetCell = CellFromPoint(dragPoint);
            if (targetCell.pageId.empty()) return {};
            return BuildExternalDesktopPreviewList(targetCell, externalCount);
        });
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
        RequestLayoutSave();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

bool DesktopApp::ApplyPendingPlacement(PendingLandingCache& cache,
    std::unordered_set<std::wstring>& claimed)
{
    if (!cache.active) return false;
    auto usedSlots = snowdesktop::desktop_output_placement::Used(widgets_, items_, claimed, cache.existingDesktopKeys,
        ToUpperInvariant, [this](const auto& widget) { return IsGroupedWidget(widget); },
        [this](const auto& item) { return IsItemInAnyWidget(item); });

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

    // Mapped-folder snapshots arrive independently of the desktop read.
    // Only their fresh enumeration can reconcile folder placements. Using
    // the current widget entries here could consume a batch before its files
    // appear in that old directory list.
    const bool entriesChanged = snowdesktop::pending_drop::ApplyEntries(cache, widgets_, items_, claimed,
        ToUpperInvariant,
        [](const DesktopItem& item, const std::wstring& sourceName) {
            return MatchPendingName(item.name, sourceName) ||
                (!item.parsingName.empty() && MatchPendingName(FileNameFromPath(item.parsingName), sourceName));
        },
        [&](const PendingLandingEntry& landing, const std::wstring& key) {
            auto* widget = findWidgetContainer(landing.widgetId);
            return !widget || landing.action == DropAction::Link || widget->AllowsDesktopKey(key);
        },
        [&](DesktopItem& item, const PendingLandingEntry& landing) {
            snowdesktop::desktop_output_placement::Services services;
            services.reference = [this]() -> const GridPage* {
                auto monitorOrder = BuildMonitorRenderOrder();
                return !monitorOrder.empty()
                    ? &gridPages_[monitorOrder.back()] : GetFirstPageGridPage();
            };
            services.findFree = [this](GridSpan span, auto& used, GridCell& found, const auto& page, int slot) {
                return TryFindFreeCell(span, used, found, page, slot);
            };
            services.slot = [this](const auto& cell) { return SlotFromCell(gridPages_, cell); };
            return snowdesktop::desktop_output_placement::Place(
                {savedPageIds_, savedPageColumns_, savedPageRows_, gridPages_}, item, landing, usedSlots, services);
        },
        [&](const std::wstring& id) {
            RefreshCollectedKeysCache();
            if (auto* widget = findWidgetContainer(id)) widget->InvalidateSlots();
        });
    return entriesChanged;
}

// ── 网格全局函数 ──────────────────────────────────────────

/**
 * @brief 根据页面 ID 在页面列表中查找对应的网格页面。
 * @param pages 页面列表。
 * @param pageId 页面 ID。
 * @return 找到的页面指针，未找到返回 nullptr。
 */

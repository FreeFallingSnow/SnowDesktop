#include "app/app.h"
#include "drag_drop/pending_drop_rules.h"
#include "drag_drop/pending_drop_completion.h"
#include "drag_drop/pending_drop_placement.h"
#include "drag_drop/desktop_output_placement.h"
#include "app/layout/new_item_placement.h"
#include "shell/shell_new_item_capture.h"
#include "icons/large_icon_renderer.h"
#include "desktop_preview_source.h"
#include "desktop_preview_cache.h"

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
    std::erase_if(pendingNewItemCaptures_, [&](const auto& capture) {
        return capture->Consume([&](const auto& widgetId, const auto& path, size_t& insertIndex) {
            const auto result = snowdesktop::new_item_placement::Apply(
                widgets_, items_, widgetId, path, insertIndex, ToUpperInvariant);
            if (result == snowdesktop::new_item_placement::Result::Applied)
            {
                ++insertIndex;
                changed = true;
            }
            return result != snowdesktop::new_item_placement::Result::Pending;
        });
    });
    if (changed) RefreshCollectedKeysCache();
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

    bool changed = false;
    for (auto& widget : widgets_)
    {
        if (widget.type != DesktopWidgetType::FolderMapping)
            continue;
        changed = ApplyPendingFolderPlacements(
            cache, widget, widget.id, {}) || changed;
    }
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
    return entriesChanged || changed;
}

// ── 网格全局函数 ──────────────────────────────────────────

/**
 * @brief 根据页面 ID 在页面列表中查找对应的网格页面。
 * @param pages 页面列表。
 * @param pageId 页面 ID。
 * @return 找到的页面指针，未找到返回 nullptr。
 */

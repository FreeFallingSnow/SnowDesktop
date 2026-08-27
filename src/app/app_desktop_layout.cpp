#include "app.h"
#include "../widgets/lua_logical_slot.h"

// Desktop-item layout and container rebuild.

void DesktopApp::LayoutItems()
{
    // Guide is a temporary empty-page placeholder. Do not mutate the model
    // during a live drag preview; the committed layout pass removes it once
    // another visible item or standalone widget actually occupies the page.
    if (!dragSession_.IsActive())
        RemoveRedundantGuideWidgets();

    for (auto& item : items_)
    {
        if (item.name.empty()) { item.bounds = {}; continue; }
        if (!gridPages_.empty() && item.gridCell.pageId.empty())
        {
            const GridPage* firstPage = GetFirstPageGridPage();
            if (firstPage) item.gridCell.pageId = firstPage->id;
        }
        auto* page = FindGridPage(gridPages_, item.gridCell.pageId);
        if (page)
        {
            item.gridSpan.columns = std::clamp(item.gridSpan.columns, 1, std::max(1, page->columns));
            item.gridSpan.rows    = std::clamp(item.gridSpan.rows,    1, std::max(1, page->rows));
            item.gridCell.column  = std::clamp(item.gridCell.column,  0, std::max(0, page->columns - item.gridSpan.columns));
            item.gridCell.row     = std::clamp(item.gridCell.row,     0, std::max(0, page->rows    - item.gridSpan.rows));
        }
        item.slot   = SlotFromCell(gridPages_, item.gridCell);
        item.bounds = GetGridRect(gridPages_, item.gridCell, item.gridSpan);
    }

    // Layout widgets
    for (auto& widget : widgets_)
    {
        if (IsGroupedWidget(widget))
        {
            widget.bounds = {};
            widget.cellScale = 1.0f;
            continue;
        }
        if (!gridPages_.empty() && widget.gridCell.pageId.empty())
        {
            const GridPage* firstPage = GetFirstPageGridPage();
            if (firstPage) widget.gridCell.pageId = firstPage->id;
        }
        const GridPage* page = FindGridPage(gridPages_, widget.gridCell.pageId);
        if (page)
        {
            widget.gridSpan = ClampWidgetGridSpan(widget, widget.gridSpan,
                page->columns, page->rows);
            widget.gridCell.column  = std::clamp(widget.gridCell.column,  0, std::max(0, page->columns - widget.gridSpan.columns));
            widget.gridCell.row     = std::clamp(widget.gridCell.row,     0, std::max(0, page->rows    - widget.gridSpan.rows));
            widget.cellScale = GetGridPageCuScale(*page);
        }
        else
            widget.cellScale = 1.0f;
        widget.bounds = GetGridRect(gridPages_, widget.gridCell, widget.gridSpan);
    }

    RebuildContainersAndItems();
}

/**
 * @brief 重建容器和项目对象层次结构（DesktopGrid、DesktopIcon、WidgetContainer 等）。
 *
 * 根据 items_ 和 widgets_ 数据重建运行时的 Container 和 Item 对象，
 * 并重新绑定拖拽源。
 */
void DesktopApp::RebuildContainersAndItems()
{
    demoCollectionIdentityCache_.clear();
    const bool wasDragging = dragSession_.IsActive();
    if (wasDragging)
        dragSession_.DetachRuntimeBindings();

    // All of these point into the runtime object tree that is about to be
    // destroyed. Clear them before releasing containers and item wrappers.
    mouseDownHit_ = nullptr;
    pendingCtrlToggleWidgetItem_ = nullptr;
    ClearDockPressedState();
    widgetDockTargetContainer_ = nullptr;
    ClearPopupDragTarget();
    ClearPopupMouseDownItem();

    floatingDockContainer_ = nullptr;
    containers_.clear();
    items_oo_.clear();

    // Collect keys of items that belong to widgets.
    RefreshCollectedKeysCache();
    const auto& collectedKeys = collectedKeysCache_;

    // DesktopGrid
    auto grid = std::make_unique<DesktopGrid>(&gridPages_, &items_, this);
    containers_.push_back(std::move(grid));

    // DesktopIcon for each NON-collected DesktopItem
    for (auto& item : items_)
    {
        if (item.name.empty()) continue;
        if (collectedKeys.contains(ToUpperInvariant(item.layoutKey))) continue;
        auto icon = std::make_unique<DesktopIcon>(&item, containers_.back().get(), this);
        items_oo_.push_back(std::move(icon));
    }

    // Widgets
    for (auto& w : widgets_)
    {
        // Collection popups need their WidgetContainer for selection, sorting
        // and drops. Keep grouped collections in the runtime tree at all
        // times; their empty model bounds keep them off the desktop. Other
        // grouped source widgets remain hosted exclusively by their group.
        if (IsGroupedWidget(w) &&
            w.type != DesktopWidgetType::Collection)
            continue;
        const bool dockExclusive = IsDockExclusiveWidgetId(w.id);
        auto widget = CreateWidget(&w, this);
        if (!widget) continue;

        if (auto* wc = dynamic_cast<WidgetContainer*>(widget.get()))
        {
            // Dock-exclusive collections still need a runtime container so
            // their popup can participate in selection, reorder and drops.
            // Their saved Dock page has no grid rect, so normal widget drawing
            // remains suppressed by the empty bounds.
            widget.release();
            containers_.push_back(std::unique_ptr<Container>(wc));
        }
        else if (!dockExclusive)
        {
            items_oo_.push_back(std::move(widget));
            if (w.type == DesktopWidgetType::LuaScript && widgetEngine_ &&
                widgetEngine_->EnsureWidgetLoaded(w.id, w.packageId))
            {
                const LuaWidgetManifest manifest =
                    WidgetEngine::GetWidgetManifest(w.packageId);
                for (const auto& [slotId, declaration] :
                    manifest.logicalSlots)
                {
                    (void)declaration;
                    const std::wstring widgetId = w.id;
                    containers_.push_back(
                        std::make_unique<LuaLogicalSlotContainer>(
                            widgetId, slotId,
                            [this, widgetId, slotId]() {
                                return widgetEngine_
                                    ? widgetEngine_->RuntimeLogicalSlotSurface(
                                        widgetId, slotId)
                                    : std::optional<LogicalSlotHostSurface>{};
                            },
                            [this, widgetId, slotId](
                                const std::vector<Item*>& sourceItems,
                                std::size_t targetIndex) {
                                return CommitLuaLogicalSlotDrop(widgetId,
                                    slotId, sourceItems, targetIndex);
                            }));
                }
            }
        }
    }

    if (generalSettings_.dockEnabled)
    {
        for (const RECT& dockArea : dockAreas_)
        {
            if (!IsRectEmptyRect(dockArea))
                containers_.push_back(
                    std::make_unique<DockContainer>(this, &dockEntries_, dockArea));
        }
    }
    // The selected Dock is always rendered by its persistent top-level host.
    // Rebind after every runtime-tree rebuild because DockContainer pointers
    // are invalidated above even when the monitor and HWND stay unchanged.
    SyncPersistentDockHost(floatingDockMonitor_);
    RebindDragSourceAfterRebuild();
    if (wasDragging && !dragSession_.IsActive())
    {
        mouseDown_ = false;
        mouseDownWidgetIndex_ = static_cast<size_t>(-1);
        detailColumnResizeActive_ = false;
        detailColumnResizePopup_ = false;
        detailColumnResizeColumn_ =
            snowdesktop::list_detail_rules::Column::None;
        HideDragHintWindow();
        ReleaseCapture();
    }
    InvalidateDragStaticScene();
}

/**
 * @brief 枚举文件夹映射组件对应的物理目录，填充 folderEntries 列表。
 * @param widget 目标组件。
 */

#include "desktop.h"
#include "slot.h"
#include "item.h"
#include "widget.h"
#include "app.h"
#include "types.h"
#include "constants.h"
#include "utils.h"
#include <algorithm>
#include <shlobj.h>
#include <shlwapi.h>
#include <unordered_set>

DesktopGrid::DesktopGrid(std::vector<GridPage>* pages, std::vector<DesktopItem>* items, DesktopApp* app)
    : pages_(pages), items_(items), app_(app)
{
    if (pages_ && !pages_->empty())
    {
        bounds_ = pages_->front().bounds;
        for (const auto& p : *pages_)
        {
            bounds_.left   = std::min(bounds_.left,   p.bounds.left);
            bounds_.top    = std::min(bounds_.top,    p.bounds.top);
            bounds_.right  = std::max(bounds_.right,  p.bounds.right);
            bounds_.bottom = std::max(bounds_.bottom, p.bounds.bottom);
        }
    }
}

DesktopGrid::~DesktopGrid() = default;

std::wstring DesktopGrid::GetTitle() const { return L"Desktop"; }
RECT DesktopGrid::GetBounds() const { return bounds_; }

std::vector<std::unique_ptr<Slot>> DesktopGrid::BuildSlots()
{
    std::vector<std::unique_ptr<Slot>> slots;
    if (!pages_) return slots;

    for (const auto& page : *pages_)
    {
        for (int row = 0; row < page.rows; ++row)
        {
            for (int col = 0; col < page.columns; ++col)
            {
                RECT cellBounds = {
                    page.bounds.left + page.marginX + col * (page.cellWidth + page.gapX),
                    page.bounds.top  + page.marginY + row * (page.cellHeight + page.gapY),
                    page.bounds.left + page.marginX + col * (page.cellWidth + page.gapX) + page.cellWidth,
                    page.bounds.top  + page.marginY + row * (page.cellHeight + page.gapY) + page.cellHeight
                };
                slots.push_back(std::make_unique<Slot>(this, cellBounds, slots.size()));
            }
        }
    }
    return slots;
}

HitRegion DesktopGrid::HitTestAtPoint(POINT pt, Slot*& outSlot)
{
    outSlot = nullptr;
    if (!pages_ || pages_->empty()) return HitRegion::None;

    const GridPage* page = nullptr;
    for (const auto& p : *pages_)
    {
        if (PtInRect(&p.bounds, pt)) { page = &p; break; }
    }
    if (!page) return HitRegion::None;

    auto axisIndex = [](const GridPage& pg, int coord, bool horizontal) -> int {
        int margin = horizontal ? pg.marginX : pg.marginY;
        int cellSize = horizontal ? pg.cellWidth : pg.cellHeight;
        int gap = horizontal ? pg.gapX : pg.gapY;
        int origin = horizontal ? pg.bounds.left : pg.bounds.top;
        int idx = (coord - origin - margin + gap / 2) / (cellSize + gap);
        return std::clamp(idx, 0, horizontal ? pg.columns - 1 : pg.rows - 1);
    };

    int col = axisIndex(*page, pt.x, true);
    int row = axisIndex(*page, pt.y, false);

    size_t idx = 0;
    for (const auto& p : *pages_)
    {
        if (p.id == page->id) { idx += col + row * p.columns; break; }
        idx += p.columns * p.rows;
    }

    auto& slots = GetSlots();
    if (idx >= slots.size()) return HitRegion::Empty;
    outSlot = slots[idx].get();

    DesktopItem* occupiedBy = nullptr;
    for (auto& item : *items_)
    {
        if (app_ && app_->collectedKeysCache_.count(ToUpperInvariant(item.layoutKey))) continue;
        if (item.gridCell.pageId == page->id &&
            item.gridCell.column == col && item.gridCell.row == row &&
            item.bounds.left < item.bounds.right && item.bounds.top < item.bounds.bottom)
        {
            occupiedBy = &item;
            break;
        }
    }

    if (!occupiedBy) return HitRegion::Empty;
    return HitRegion::SortBefore;
}

void DesktopGrid::OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
    Slot* targetSlot, HitRegion region, int mods)
{
    if (!app_) return;

    // Handoff: delegate to shell via DropSelectedItemsOnTarget
    if (region == HitRegion::Handoff)
    {
        int hit = app_->HitTestItem(app_->dragSession_.CurrentPoint());
        if (hit >= 0 && !(*items_)[hit].selected)
            app_->DropSelectedItemsOnTarget(hit);
        app_->SaveLayoutSlots();
        app_->ClearSelection();
        app_->ReloadItems();
        return;
    }

    DragSourceList sourceList = app_->BuildDragSourceList(sourceItems, origin);
    DropPreviewList preview = app_->BuildDropPreviewList(sourceList, this, targetSlot, region, mods,
        app_->dragSession_.CurrentPoint());
    app_->ExecuteDropPipeline(sourceList, preview);
}

std::vector<Item*> DesktopGrid::GetSelectedItems() const
{
    dragSourceCache_.clear();
    std::vector<Item*> result;
    if (!app_) return result;

    for (auto& oo : app_->GetItemsOO())
    {
        auto* icon = dynamic_cast<DesktopIcon*>(oo.get());
        if (!icon) continue;
        DesktopItem* di = icon->GetDesktopItem();
        if (di && di->selected && !di->name.empty())
            result.push_back(icon);
    }
    (void)dragSourceCache_; // items_oo_ owns the icons, no temp wrappers needed
    return result;
}

HitRegion DesktopGrid::HitTestDrag(POINT pt, Slot*& outSlot)
{
    outSlot = nullptr;
    HitRegion region = HitTestAtPoint(pt, outSlot);
    if (region == HitRegion::SortBefore && app_)
    {
        // Check for Handoff: mouse on an unselected icon
        int hit = app_->HitTestItem(pt);
        if (hit >= 0 && !(*items_)[hit].selected)
        {
            RECT iconRect = app_->GetItemIconRect((*items_)[hit].bounds);
            RECT hf = { iconRect.left - 4, iconRect.top - 2,
                        iconRect.right + 4, iconRect.bottom + 4 };
            if (PtInRect(&hf, pt))
                region = HitRegion::Handoff;
        }
    }
    return region;
}

std::wstring DesktopGrid::GetDragHint(Slot* slot, HitRegion region,
    const std::vector<Item*>& sourceItems, Container* origin, int mods) const
{
    (void)slot;
    (void)sourceItems;

    if (!app_) return L"";

    bool ctrlDown = (mods & MK_CONTROL) != 0;
    bool altDown  = (mods & MK_ALT) != 0;
    bool shiftDown = (mods & MK_SHIFT) != 0;

    POINT dragPoint = app_->dragSession_.CurrentPoint();

    if (region == HitRegion::Handoff)
    {
        int hit = app_->HitTestItem(dragPoint);
        if (hit >= 0 && !(*items_)[hit].selected)
        {
            if (origin)
                return L"释放：交给「" + (*items_)[hit].name + L"」处理";
            else
                return L"释放：拖入「" + (*items_)[hit].name + L"」";
        }
    }

    // External drag — simple hint
    if (!origin)
    {
        if (altDown)   return L"释放：创建快捷方式";
        if (shiftDown) return L"释放：移动到桌面";
        if (ctrlDown)  return L"释放：复制到桌面";
        return L"释放：放置到桌面";
    }

    // Internal drag
    if (altDown)  return L"释放：创建快捷方式到此空位";
    if (ctrlDown) return L"释放：复制到此空位";

    GridCell bestCell = app_->FindBestDropCell(
        app_->CellFromPoint(app_->GetDragTargetPoint(dragPoint)));
    if (app_->BuildSelectedMove(bestCell).empty())
        return L"释放：当前位置已有图标";

    return L"释放：移动到此空位";
}

void DesktopGrid::DrawDropPreview(ID2D1DeviceContext* ctx, Slot* slot, HitRegion region)
{
    if (!app_ || !ctx) return;

    // Handoff now unified in RenderFrame — skip here
    if (region == HitRegion::Handoff) return;

    const bool hasItemDrag = app_->dragSession_.IsActive() && !app_->dragSession_.Items().empty();
    POINT dragPoint = app_->dragSession_.CurrentPoint();

    if (hasItemDrag)
    {
        int mods = DropActionToMods(app_->dragSession_.Action());
        DropPreviewList preview = app_->BuildDropPreviewList(app_->dragSession_.SourceList(),
            this, slot, region, mods, dragPoint);
        app_->DrawDesktopDropPreviewList(ctx, preview);
    }
    else
    {
        GridCell targetCell = app_->CellFromPoint(dragPoint);
        if (targetCell.pageId.empty()) return;
        DropPreviewList preview = app_->BuildExternalDesktopPreviewList(targetCell,
            static_cast<size_t>(std::max(1, app_->externalDropFileCount_)));
        app_->DrawDesktopDropPreviewList(ctx, preview);
    }
}

void DesktopGrid::DrawChrome(ID2D1DeviceContext* context, POINT mousePt)
{
    (void)context;
    (void)mousePt;
}

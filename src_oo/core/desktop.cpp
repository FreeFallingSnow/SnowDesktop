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
        if (app_ && app_->IsItemInAnyWidget(item)) continue;
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
    (void)targetSlot;

    // Handoff: delegate to shell via DropSelectedItemsOnTarget
    if (region == HitRegion::Handoff)
    {
        int hit = app_->HitTestItem(app_->dragCurrentPoint_);
        if (hit >= 0 && !(*items_)[hit].selected)
            app_->DropSelectedItemsOnTarget(hit);
        app_->SaveLayoutSlots();
        app_->ClearSelection();
        app_->ReloadItems();
        return;
    }

    DesktopApp::DesktopDropPayload payload = app_->CollectDesktopDropPayload(sourceItems);
    if (payload.hasWidgets) return;

    POINT adjusted = app_->GetDragTargetPoint(app_->dragCurrentPoint_);
    GridCell tc = app_->FindBestDropCell(app_->CellFromPoint(adjusted));

    if (!payload.hasDesktopIcons)
    {
        bool dropped = app_->DropFilePathsToDesktopCell(payload.filePaths, tc, mods, false);
        if (dropped && origin && (mods & (MK_CONTROL | MK_ALT)) == 0)
        {
            auto* originWidget = dynamic_cast<WidgetContainer*>(origin);
            DesktopWidget* data = originWidget ? originWidget->GetWidgetData() : nullptr;
            if (data && data->type == DesktopWidgetType::FolderMapping)
            {
                auto& widgets = app_->GetWidgets();
                for (size_t i = 0; i < widgets.size(); ++i)
                {
                    if (&widgets[i] == data)
                    {
                        app_->RefreshFolderMappingWidget(i);
                        break;
                    }
                }
            }
        }
        return;
    }

    if (mods & MK_ALT)
    {
        app_->DropFilePathsToDesktopCell(payload.filePaths, tc, MK_ALT, true);
    }
    else if (mods & MK_CONTROL)
    {
        app_->DropFilePathsToDesktopCell(payload.filePaths, tc, MK_CONTROL, true);
    }
    else
    {
        app_->RemoveDesktopKeysFromWidgets(payload.desktopKeys);
        app_->MoveSelectedItemsToCell(tc);
    }
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

    POINT dragPoint = origin ? app_->dragCurrentPoint_ : app_->externalDragPoint_;

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
    GridCell bestCell = app_->FindBestDropCell(
        app_->CellFromPoint(app_->GetDragTargetPoint(dragPoint)));
    if (app_->BuildSelectedMove(bestCell).empty())
        return L"释放：当前位置已有图标";

    if (altDown)  return L"释放：创建快捷方式到此空位";
    if (ctrlDown) return L"释放：复制到此空位";
    return L"释放：移动到此空位";
}

void DesktopGrid::DrawDropPreview(ID2D1DeviceContext* ctx, Slot* slot, HitRegion region)
{
    if (!app_ || !ctx) return;

    // Handoff now unified in RenderFrame — skip here
    if (region == HitRegion::Handoff) return;

    POINT dragPoint = app_->draggingItems_ ? app_->dragCurrentPoint_ : app_->externalDragPoint_;
    GridCell targetCell = app_->draggingItems_
        ? app_->FindBestDropCell(app_->CellFromPoint(dragPoint))
        : app_->CellFromPoint(dragPoint);

    if (targetCell.pageId.empty()) return;

    if (app_->draggingItems_)
    {
        std::vector<DesktopApp::PendingGridMove> moves = app_->BuildSelectedMove(targetCell);
        for (const auto& move : moves)
        {
            RECT targetRect = GetGridRect(app_->gridPages_, move.cell, (*items_)[move.index].gridSpan);
            app_->DrawD2DRoundedRectangle(ctx, targetRect, 6.0f,
                D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.12f),
                D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.50f), 2.0f);
        }
    }
    else
    {
        extern inline RECT GetGridRect(const std::vector<GridPage>&, const GridCell&, GridSpan);
        extern inline const GridPage* FindGridPage(const std::vector<GridPage>&, const std::wstring&);
        int previewCount = std::max(1, static_cast<int>(app_->externalDropFileCount_));
        const GridPage* targetPage = FindGridPage(app_->gridPages_, targetCell.pageId);
        int cols = targetPage ? targetPage->columns : 1;
        int row = targetCell.row;
        int col = targetCell.column;
        for (int i = 0; i < previewCount; ++i)
        {
            GridCell previewCell{ targetCell.pageId, col, row };
            RECT targetRect = GetGridRect(app_->gridPages_, previewCell, GridSpan{1, 1});
            app_->DrawD2DRoundedRectangle(ctx, targetRect, 6.0f,
                D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.12f),
                D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.50f), 2.0f);
            ++col;
            if (col >= cols) { col = 0; ++row; }
        }
    }
}

void DesktopGrid::DrawChrome(ID2D1DeviceContext* context, POINT mousePt)
{
    (void)context;
    (void)mousePt;
}

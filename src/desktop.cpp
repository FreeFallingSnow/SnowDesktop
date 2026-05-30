#include "desktop.h"
#include "slot.h"
#include "item.h"
#include "types.h"
#include "constants.h"
#include "utils.h"
#include <algorithm>
#ifdef SNOWDESKTOP_OO
#include "widget.h"
#include "app_oo.h"
#endif

DesktopGrid::DesktopGrid(std::vector<GridPage>* pages, std::vector<DesktopItem>* items, void* app)
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

std::wstring DesktopGrid::GetTitle() const { return L"Desktop"; }
RECT DesktopGrid::GetBounds() const { return bounds_; }

bool DesktopGrid::EnsureSlotCache()
{
    if (slotsBuilt_) return false;
    cachedSlots_.clear();
    if (!pages_) return true;
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
                cachedSlots_.push_back(std::make_unique<Slot>(this, cellBounds, cachedSlots_.size()));
            }
        }
    }
    slotsBuilt_ = true;
    return true;
}

std::vector<std::unique_ptr<Slot>> DesktopGrid::BuildSlots()
{
    slotsBuilt_ = false;
    EnsureSlotCache();
    // Return empty — the cached slots are used via GetSlots().BuildSlots() contract.
    // Callers use GetSlots() which returns cachedSlots_ directly.
    return {};
}

// O(1) hit test — uses page grid math for Empty/Sort, but delegates
// Handoff detection to the caller (who has access to GetItemIconRect).
// Returns the slot for the cell under the point.
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

    // Find the slot index
    size_t idx = 0;
    for (const auto& p : *pages_)
    {
        if (p.id == page->id) { idx += col + row * p.columns; break; }
        idx += p.columns * p.rows;
    }

    EnsureSlotCache();
    if (idx >= cachedSlots_.size()) return HitRegion::Empty;
    outSlot = cachedSlots_[idx].get();

    // Find occupied item
    DesktopItem* occupiedBy = nullptr;
    for (auto& item : *items_)
    {
        if (item.gridCell.pageId == page->id &&
            item.gridCell.column == col && item.gridCell.row == row &&
            item.bounds.left < item.bounds.right && item.bounds.top < item.bounds.bottom)
        {
            occupiedBy = &item;
            break;
        }
    }

    if (!occupiedBy) return HitRegion::Empty;

    // Handoff decision made by app via HitTestItem + GetItemIconRect
    // We just return SortBefore; the app overrides with Handoff if needed
    return HitRegion::SortBefore;
}

#ifdef SNOWDESKTOP_OO
#include "widget.h"
#include "app_oo.h"

void DesktopGrid::OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
    Slot* targetSlot, HitRegion region, int mods)
{
    auto* app = static_cast<SnowDesktopAppOO*>(app_);
    if (!app) return;

    // ── External files ──────────────────────────────────────────
    for (auto* src : sourceItems)
        if (dynamic_cast<ExternalFileItem*>(src)) return;

    // ── Widget drag ─────────────────────────────────────────────
    for (auto* src : sourceItems)
        if (dynamic_cast<Widget*>(src)) return;

    // ── DesktopIcon self-drag ───────────────────────────────────
    POINT adjusted = app->GetDragTargetPoint(app->dragCurrentPoint_);
    GridCell tc = app->FindBestDropCell(app->CellFromPoint(adjusted));

    if (mods & MK_ALT)
        app->CreateShortcutSelectedItemsToCell(tc);
    else if (mods & MK_CONTROL)
        app->CopySelectedItemsToCell(tc);
    else
        app->MoveSelectedItemsToCell(tc);
}
#else
void DesktopGrid::OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
    Slot* targetSlot, HitRegion region, int mods)
{
    (void)sourceItems; (void)origin; (void)targetSlot; (void)region; (void)mods;
}
#endif

void DesktopGrid::DrawChrome(ID2D1DeviceContext* context, POINT mousePt)
{
    (void)context;
    (void)mousePt;
}

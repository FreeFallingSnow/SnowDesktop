#include "widget.h"
#include "slot.h"
#include "item.h"
#include "types.h"
#include "app.h"
#include "drop_model.h"
#include <algorithm>
#include <shlobj.h>
#include <shlwapi.h>
#include <unordered_set>

// ── Legacy-style helpers ───────────────────────────────────────

static size_t GetCollectionInlineCapacity(const DesktopWidget& widget)
{
    if (widget.gridSpan.columns <= 1 && widget.gridSpan.rows <= 1) return 4;
    return static_cast<size_t>(std::max(1, widget.gridSpan.columns) * std::max(1, widget.gridSpan.rows) - 1);
}

static size_t GetCollectionAllButtonSlot(const DesktopWidget& widget)
{
    if (widget.gridSpan.columns <= 1 && widget.gridSpan.rows <= 1)
        return static_cast<size_t>(-1);
    return static_cast<size_t>(std::max(1, widget.gridSpan.columns) * std::max(1, widget.gridSpan.rows) - 1);
}

// Gets a slot rect aligned to the desktop page grid (matches legacy GetCollectionPreviewSlotRect)
static RECT GetCollectionSlotRect(const DesktopWidget& widget, size_t slot,
    RECT body, DesktopApp* app)
{
    if (IsRectEmptyRect(body)) return {};

    bool compact = widget.gridSpan.columns <= 1 && widget.gridSpan.rows <= 1;
    if (compact)
    {
        // 2×2 grid centered in the body
        InflateRect(&body, -6, -6);
        int columns = 2;
        int bodyW = std::max<int>(1, (int)(body.right - body.left));
        int bodyH = std::max<int>(1, (int)(body.bottom - body.top));
        int gridSize = std::min(bodyW, bodyH);
        const auto& pages = app->GetDesktopGrid()->GetPages();
        const GridPage* page = nullptr;
        for (const auto& p : pages)
            if (p.id == widget.gridCell.pageId) { page = &p; break; }
        int gapY = page ? page->gapY : 0;
        int gridTop = body.top + gapY / 2 - 10;
        int slotSz = std::max<int>(1, gridSize / 2);
        int col = (int)(slot % (size_t)columns);
        int row = (int)(slot / (size_t)columns);
        RECT rect = { body.left + col * slotSz, gridTop + row * slotSz,
                      col + 1 == columns ? body.right : body.left + (col + 1) * slotSz,
                      row + 1 == 2 ? gridTop + gridSize : gridTop + (row + 1) * slotSz };
        InflateRect(&rect, -1, -1);
        return rect;
    }

    // Non-compact: grid-aligned slots matching desktop cell size
    InflateRect(&body, -4, -4);
    const auto& pages = app->GetDesktopGrid()->GetPages();
    const GridPage* page = nullptr;
    for (const auto& p : pages)
        if (p.id == widget.gridCell.pageId) { page = &p; break; }
    int cellH = page ? page->cellHeight : 96;
    int gapY = page ? page->gapY : 0;
    int columns = std::max(1, widget.gridSpan.columns);
    int rows = std::max(1, widget.gridSpan.rows);
    int col = (int)(slot % (size_t)columns);
    int rowIdx = (int)(slot / (size_t)columns);
    if (rowIdx >= rows) return {};

    int width = std::max<int>(1, (int)(body.right - body.left) / columns);
    int startY = body.top + gapY / 2 - 8;
    int rowStep = cellH + gapY;
    return { body.left + col * width, startY + rowIdx * rowStep,
             col + 1 == columns ? body.right : body.left + (col + 1) * width,
             startY + rowIdx * rowStep + cellH };
}

void Collection::DrawThumbnail(ID2D1DeviceContext* context,
    const DesktopItem& item, RECT rect, bool selected) const
{
    if (!app_ || !context || IsRectEmptyRect(rect)) return;

    if (selected)
    {
        app_->DrawD2DRoundedRectangle(context, rect, 7.0f,
            D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.24f),
            D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.78f));
    }

    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int iconSize = std::max(16, std::min(width - 6, height - 4));
    const int iconX = rect.left + (width - iconSize) / 2;
    const int iconY = rect.top + (height - iconSize) / 2;

    if (item.iconBitmap)
    {
        ID2D1Bitmap1* bmp = app_->GetOrCreateD2DBitmap(item.iconBitmap);
        if (bmp)
        {
            D2D1_RECT_F dst = D2D1::RectF(static_cast<float>(iconX), static_cast<float>(iconY),
                static_cast<float>(iconX + iconSize), static_cast<float>(iconY + iconSize));
            context->DrawBitmap(bmp, dst, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
        }
    }
}

// Draw a single item at its slot position (matches legacy DrawD2DItemAt)
// Items are drawn at desktop icon size with text, same as desktop icons.
void Collection::DrawContent(ID2D1DeviceContext* context, RECT body)
{
    if (!data_ || !app_) return;
    if (data_->itemKeys.empty()) return;

    bool compact = data_->gridSpan.columns <= 1 && data_->gridSpan.rows <= 1;
    const auto& items = app_->GetDesktopItems();
    auto& slots = GetSlots();
    size_t inlineCapacity = std::min(GetCollectionInlineCapacity(*data_), data_->itemKeys.size());

    for (size_t i = 0; i < inlineCapacity && i < slots.size(); ++i)
    {
        if (i >= data_->itemKeys.size()) break;
        size_t itemIdx = app_->FindItemIndexByKey(data_->itemKeys[i]);
        if (itemIdx == static_cast<size_t>(-1)) continue;

        const DesktopItem& di = items[itemIdx];
        RECT slotRect = slots[i]->GetBounds();
        if (IsRectEmptyRect(slotRect)) continue;

        if (compact)
            DrawThumbnail(context, di, slotRect, di.selected);
        else
        {
            RECT bodyRect = GetBodyRect();
            bool hovered = PtInRect(&slotRect, app_->lastMousePoint_) != FALSE && !di.selected && PtInRect(&bodyRect, app_->lastMousePoint_);
            DesktopIcon icon(const_cast<DesktopItem*>(&di), const_cast<Collection*>(this), app_);
            icon.Draw(context, slotRect, di.selected ? 2 : (hovered ? 1 : 0));
        }
    }

    // "All" button: 2×2 mosaic of remaining items
    size_t allSlot = GetCollectionAllButtonSlot(*data_);
    if (allSlot != static_cast<size_t>(-1) && !compact)
    {
        RECT allRect = GetCollectionSlotRect(*data_, allSlot, body, app_);
        if (!IsRectEmptyRect(allRect))
        {
            bool hasRemainingIcon = false;
            for (size_t j = 0; j < 4; ++j)
            {
                size_t keyIdx = inlineCapacity + j;
                if (keyIdx < data_->itemKeys.size() &&
                    app_->FindItemIndexByKey(data_->itemKeys[keyIdx]) != static_cast<size_t>(-1))
                {
                    hasRemainingIcon = true;
                    break;
                }
            }
            if (!hasRemainingIcon && !PtInRect(&allRect, app_->lastMousePoint_))
                return;

            // Draw 2×2 thumbnail grid
            RECT inner = allRect;
            InflateRect(&inner, -8, -8);
            OffsetRect(&inner, 0, -4);
            int tileW = std::max<int>(1, (int)(inner.right - inner.left) / 2);
            int tileH = std::max<int>(1, (int)(inner.bottom - inner.top) / 2);

            for (int j = 0; j < 4; ++j)
            {
                int col = j % 2;
                int row = j / 2;
                RECT tile = { inner.left + col * tileW, inner.top + row * tileH,
                              col + 1 == 2 ? inner.right : inner.left + (col + 1) * tileW,
                              row + 1 == 2 ? inner.bottom : inner.top + (row + 1) * tileH };
                InflateRect(&tile, -2, -2);

                size_t keyIdx = inlineCapacity + (size_t)j;
                if (keyIdx < data_->itemKeys.size())
                {
                    size_t itemIdx = app_->FindItemIndexByKey(data_->itemKeys[keyIdx]);
                    if (itemIdx != static_cast<size_t>(-1))
                    {
                        const DesktopItem& di = items[itemIdx];
                        InflateRect(&tile, -4, -4);
                        DrawThumbnail(context, di, tile, di.selected);
                    }
                }
                else
                {
                    if (!hasRemainingIcon)
                    {
                        InflateRect(&tile, -2, -2);
                        app_->DrawD2DRoundedRectangle(context, tile, 3.0f,
                            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.24f),
                            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.32f));
                    }
                }
            }
        }
    }
}

std::vector<std::unique_ptr<Slot>> Collection::BuildSlots()
{
    slotItemCache_.clear();

    std::vector<std::unique_ptr<Slot>> slots;
    if (!data_ || !app_) return slots;

    size_t inlineCap = GetCollectionInlineCapacity(*data_);
    size_t visible = std::min(inlineCap, data_->itemKeys.size());
    RECT body = GetBodyRect();
    for (size_t idx = 0; idx < visible; ++idx)
    {
        RECT cell = GetCollectionSlotRect(*data_, idx, body, app_);
        if (IsRectEmptyRect(cell)) continue;
        auto slot = std::make_unique<Slot>(this, cell, idx);
        Item* item = GetSlotItem(idx);
        if (item) item->SetBounds(cell);
        slot->SetItem(item);
        slots.push_back(std::move(slot));
    }

    return slots;
}

size_t Collection::GetSlotCount() const
{
    if (!data_) return 0;
    if (data_->itemKeys.empty()) return 0;

    size_t inlineCap = GetCollectionInlineCapacity(*data_);
    size_t visible = std::min(inlineCap, data_->itemKeys.size());

    return visible;
}

Item* Collection::GetSlotItem(size_t idx) const
{
    if (!data_ || idx >= data_->itemKeys.size() || !app_) return nullptr;
    if (idx >= GetCollectionInlineCapacity(*data_)) return nullptr;
    size_t itemIdx = app_->FindItemIndexByKey(data_->itemKeys[idx]);
    if (itemIdx == static_cast<size_t>(-1)) return nullptr;
    auto icon = std::make_unique<DesktopIcon>(&app_->GetDesktopItems()[itemIdx],
        const_cast<Collection*>(this), app_);
    Item* result = icon.get();
    slotItemCache_.push_back(std::move(icon));
    return result;
}

Item* Collection::GetMemberItem(size_t idx) const
{
    if (!data_ || idx >= data_->itemKeys.size() || !app_) return nullptr;
    size_t itemIdx = app_->FindItemIndexByKey(data_->itemKeys[idx]);
    if (itemIdx == static_cast<size_t>(-1)) return nullptr;
    auto icon = std::make_unique<DesktopIcon>(&app_->GetDesktopItems()[itemIdx],
        const_cast<Collection*>(this), app_);
    Item* result = icon.get();
    dragSourceCache_.push_back(std::move(icon));
    return result;
}

std::vector<size_t> Collection::GetSelectedMemberIndices() const
{
    std::vector<size_t> result;
    if (!data_ || !app_) return result;
    for (size_t i = 0; i < data_->itemKeys.size(); ++i)
    {
        size_t itemIdx = app_->FindItemIndexByKey(data_->itemKeys[i]);
        if (itemIdx == static_cast<size_t>(-1)) continue;
        if (app_->GetDesktopItems()[itemIdx].selected)
            result.push_back(i);
    }
    return result;
}

void Collection::ReorderMembers(const std::vector<size_t>& indices, size_t insertBefore)
{
    if (!data_) return;
    std::vector<std::wstring> moving;
    for (auto it = indices.rbegin(); it != indices.rend(); ++it)
    {
        if (*it >= data_->itemKeys.size()) continue;
        moving.push_back(data_->itemKeys[*it]);
        data_->itemKeys.erase(data_->itemKeys.begin() + static_cast<std::ptrdiff_t>(*it));
    }
    size_t adjusted = insertBefore;
    for (auto idx : indices)
        if (idx < insertBefore) --adjusted;
    if (adjusted > data_->itemKeys.size()) adjusted = data_->itemKeys.size();
    for (auto it = moving.rbegin(); it != moving.rend(); ++it)
        data_->itemKeys.insert(data_->itemKeys.begin() + static_cast<std::ptrdiff_t>(adjusted++), *it);
    InvalidateSlots();
}

std::vector<Item*> Collection::GetSelectedItems() const
{
    dragSourceCache_.clear();
    std::vector<Item*> result;
    if (!data_ || !app_) return result;

    for (const auto& key : data_->itemKeys)
    {
        size_t idx = app_->FindItemIndexByKey(key);
        if (idx == static_cast<size_t>(-1)) continue;
        DesktopItem& di = app_->GetDesktopItems()[idx];
        if (!di.selected) continue;
        RECT bounds = app_->GetVisibleCollectionItemBounds(idx);
        if (IsRectEmptyRect(bounds)) continue;

        auto icon = std::make_unique<DesktopIcon>(&app_->GetDesktopItems()[idx], const_cast<Collection*>(this), app_);
        icon->SetBounds(bounds);
        result.push_back(icon.get());
        dragSourceCache_.push_back(std::move(icon));
    }
    return result;
}

RECT Collection::GetAllButtonRect() const
{
    if (!data_ || !app_) return {};
    RECT body = GetBodyRect();
    bool compact = data_->gridSpan.columns <= 1 && data_->gridSpan.rows <= 1;
    if (compact)
    {
        InflateRect(&body, -6, -6);
        return body;
    }
    size_t allSlot = GetCollectionAllButtonSlot(*data_);
    if (allSlot == static_cast<size_t>(-1)) return {};
    return GetCollectionSlotRect(*data_, allSlot, body, app_);
}

WidgetHit Collection::HitTestWidget(POINT pt) const
{
    WidgetHit base = WidgetContainer::HitTestWidget(pt);
    if (base == WidgetHit::None || base == WidgetHit::ResizeHandle) return base;
    if (!data_ || !app_ || data_->type != DesktopWidgetType::Collection) return base;

    RECT frame = GetFrameRect();
    if (!PtInRect(&frame, pt)) return WidgetHit::None;

    const bool compact = data_->gridSpan.columns <= 1 && data_->gridSpan.rows <= 1;
    if (compact)
        return base == WidgetHit::Content ? WidgetHit::CollectionOpenBtn : base;

    size_t allSlot = GetCollectionAllButtonSlot(*data_);
    if (allSlot != static_cast<size_t>(-1))
    {
        RECT allRect = GetCollectionSlotRect(*data_, allSlot, GetBodyRect(), app_);
        if (PtInRect(&allRect, pt)) return WidgetHit::CollectionOpenBtn;
    }
    return base;
}

void Collection::OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
    Slot* targetSlot, HitRegion region, int mods)
{
    if (!app_ || !data_) return;
    DragSourceList sourceList = app_->BuildDragSourceList(sourceItems, origin);
    DropPreviewList preview = app_->BuildDropPreviewList(sourceList, this, targetSlot, region, mods,
        app_->dragSession_.CurrentPoint());
    app_->ExecuteDropPipeline(sourceList, preview);
}

size_t Collection::GetDropInsertIndex(Slot* targetSlot, HitRegion region) const
{
    size_t insertAt = targetSlot ? targetSlot->GetIndex() : (data_ ? data_->itemKeys.size() : 0);
    if (targetSlot && region == HitRegion::SortAfter)
        ++insertAt;
    return data_ ? std::min(insertAt, data_->itemKeys.size()) : insertAt;
}

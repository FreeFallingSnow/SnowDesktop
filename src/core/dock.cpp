#include "dock.h"

#include "app.h"
#include "constants.h"
#include "slot.h"

#include <algorithm>
#include <cmath>
#include <shlwapi.h>

DockEntryItem::DockEntryItem(DesktopApp* app, Container* container, size_t entryIndex)
    : app_(app), container_(container), entryIndex_(entryIndex) {}

const DockEntry* DockEntryItem::Entry() const
{
    return app_ && entryIndex_ < app_->dockEntries_.size()
        ? &app_->dockEntries_[entryIndex_] : nullptr;
}

DockEntryType DockEntryItem::GetEntryType() const
{
    const DockEntry* entry = Entry();
    return entry ? entry->type : DockEntryType::DesktopItem;
}

std::wstring DockEntryItem::GetReference() const
{
    const DockEntry* entry = Entry();
    return entry ? entry->reference : L"";
}

std::wstring DockEntryItem::GetTitle() const
{
    const DockEntry* entry = Entry();
    if (!entry || !app_) return L"";
    if (entry->type == DockEntryType::DesktopItem)
    {
        size_t index = app_->FindItemIndexByKey(entry->reference);
        return index < app_->items_.size() ? app_->items_[index].name : L"";
    }
    auto it = std::find_if(app_->widgets_.begin(), app_->widgets_.end(),
        [&](const DesktopWidget& widget) { return widget.id == entry->reference; });
    return it != app_->widgets_.end() ? it->title : L"集合";
}

std::wstring DockEntryItem::GetPath() const
{
    const DockEntry* entry = Entry();
    if (!entry || !app_ || entry->type != DockEntryType::DesktopItem) return L"";
    size_t index = app_->FindItemIndexByKey(entry->reference);
    return index < app_->items_.size() ? app_->items_[index].parsingName : L"";
}

HBITMAP DockEntryItem::GetIconBitmap() const
{
    const DockEntry* entry = Entry();
    if (!entry || !app_ || entry->type != DockEntryType::DesktopItem) return nullptr;
    size_t index = app_->FindItemIndexByKey(entry->reference);
    return index < app_->items_.size() ? app_->items_[index].iconBitmap : nullptr;
}

RECT DockEntryItem::GetBounds() const { return bounds_; }
void DockEntryItem::SetBounds(RECT bounds) { bounds_ = bounds; }

bool DockEntryItem::IsSelected() const
{
    const DockEntry* entry = Entry();
    return entry && entry->selected;
}

void DockEntryItem::SetSelected(bool selected)
{
    if (!app_ || entryIndex_ >= app_->dockEntries_.size()) return;
    app_->dockEntries_[entryIndex_].selected = selected;
}

Container* DockEntryItem::GetContainer() const { return container_; }

void DockEntryItem::Draw(ID2D1DeviceContext* context, RECT rect, int state)
{
    const DockEntry* entry = Entry();
    if (entry && app_) app_->DrawDockEntry(context, *entry, rect, state);
}

ComPtr<IDataObject> DockEntryItem::CreateDataObject()
{
    const DockEntry* entry = Entry();
    if (!entry || !app_ || entry->type != DockEntryType::DesktopItem) return nullptr;
    size_t index = app_->FindItemIndexByKey(entry->reference);
    if (index >= app_->items_.size()) return nullptr;
    DesktopIcon icon(&app_->items_[index], container_, app_);
    return icon.CreateDataObject();
}

DockContainer::DockContainer(DesktopApp* app, std::vector<DockEntry>* entries, RECT area)
    : app_(app), entries_(entries), area_(area) {}

size_t DockContainer::Capacity() const
{
    const int width = std::max(0L, area_.right - area_.left - 24L);
    return static_cast<size_t>(std::max(0, width / kDockSlotWidth - 1));
}

bool DockContainer::HasCapacity(size_t additional) const
{
    return entries_ && entries_->size() + additional <= Capacity();
}

RECT DockContainer::GetBounds() const
{
    const size_t count = entries_ ? entries_->size() : 0;
    const int desiredWidth = static_cast<int>((count + 1) * kDockSlotWidth + 16);
    const int maxWidth = std::max(1, static_cast<int>(area_.right - area_.left) - 24);
    const int width = std::min(desiredWidth, maxWidth);
    const int reservedHeight = std::max(1, static_cast<int>(area_.bottom - area_.top));
    int gridBottomMargin = kGridMarginY;
    if (app_)
    {
        const GridPage* firstPage = app_->GetFirstPageGridPage();
        if (firstPage) gridBottomMargin = std::max(0, firstPage->marginY);
    }
    const int verticalSpan = reservedHeight + gridBottomMargin;
    const int height = std::min(kDockSlotHeight + 12, verticalSpan);
    const int balancedGap = std::max(0, (verticalSpan - height) / 2);
    const int left = area_.left + (area_.right - area_.left - width) / 2;
    const int top = area_.top - gridBottomMargin + balancedGap;
    return RECT{ left, top, left + width, top + height };
}

std::vector<std::unique_ptr<Slot>> DockContainer::BuildSlots()
{
    std::vector<std::unique_ptr<Slot>> slots;
    entryItems_.clear();
    RECT bounds = GetBounds();
    const size_t count = entries_ ? entries_->size() : 0;
    for (size_t i = 0; i <= count; ++i)
    {
        RECT cell{
            bounds.left + 8 + static_cast<LONG>(i * kDockSlotWidth),
            bounds.top + 6,
            bounds.left + 8 + static_cast<LONG>((i + 1) * kDockSlotWidth),
            bounds.bottom - 6
        };
        auto slot = std::make_unique<Slot>(this, cell, i);
        if (i < count)
        {
            auto item = std::make_unique<DockEntryItem>(app_, this, i);
            item->SetBounds(cell);
            slot->SetItem(item.get());
            entryItems_.push_back(std::move(item));
        }
        slots.push_back(std::move(slot));
    }
    return slots;
}

size_t DockContainer::InsertIndexFor(Slot* slot, HitRegion region) const
{
    const size_t count = entries_ ? entries_->size() : 0;
    if (!slot) return count;
    size_t index = std::min(slot->GetIndex(), count);
    if (region == HitRegion::SortAfter) ++index;
    return std::min(index, count);
}

size_t DockContainer::GetInsertIndexAtPoint(POINT pt) const
{
    const size_t count = entries_ ? entries_->size() : 0;
    const auto& slots = const_cast<DockContainer*>(this)->GetSlots();
    for (size_t i = 0; i < count && i < slots.size(); ++i)
    {
        RECT bounds = slots[i]->GetBounds();
        if (PtInRect(&bounds, pt))
            return pt.x < (bounds.left + bounds.right) / 2 ? i : i + 1;
    }
    return count;
}

void DockContainer::DrawInsertionPreview(
    ID2D1DeviceContext* context, size_t insertIndex) const
{
    if (!context) return;
    const size_t count = entries_ ? entries_->size() : 0;
    insertIndex = std::min(insertIndex, count);
    RECT bounds = GetBounds();
    const float x = static_cast<float>(bounds.left + 8 +
        static_cast<LONG>(insertIndex * kDockSlotWidth));
    ComPtr<ID2D1SolidColorBrush> brush;
    context->CreateSolidColorBrush(D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.95f), &brush);
    if (brush)
        context->FillRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(x - 2.0f, static_cast<float>(bounds.top + 10),
                x + 2.0f, static_cast<float>(bounds.bottom - 10)), 2.0f, 2.0f),
            brush.Get());
}

void DockContainer::OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
    Slot* targetSlot, HitRegion region, int mods)
{
    if (!app_) return;
    app_->CommitDockDrop(sourceItems, origin, InsertIndexFor(targetSlot, region), mods);
}

void DockContainer::DrawChrome(ID2D1DeviceContext* context, POINT mousePt)
{
    (void)mousePt;
    if (!context) return;
    RECT bounds = GetBounds();
    PersonalizationSettings p = app_ && app_->settingsWindow_
        ? app_->settingsWindow_->GetPersonalization()
        : PersonalizationSettings::DarkPreset();
    const D2D1_COLOR_F fill = D2D1::ColorF(
        p.widgetBgR, p.widgetBgG, p.widgetBgB, p.widgetAlpha);
    const D2D1_COLOR_F border = D2D1::ColorF(
        p.widgetBorderR, p.widgetBorderG, p.widgetBorderB, p.widgetBorderAlpha);
    if (app_)
        app_->DrawWidgetPanelBackground(context, bounds, p.cornerRadius, 1.0f,
            fill, border, false, 1.0f, &p);

}

void DockContainer::DrawContents(ID2D1DeviceContext* context)
{
    if (!context) return;
    const auto& slots = GetSlots();
    const size_t count = entries_ ? entries_->size() : 0;
    for (size_t i = 0; i < count && i < slots.size(); ++i)
    {
        Item* item = slots[i]->GetItem();
        if (!item) continue;
        RECT slotBounds = slots[i]->GetBounds();
        const bool hovered = PtInRect(&slotBounds, app_->lastMousePoint_) != FALSE;
        item->Draw(context, slotBounds, item->IsSelected() ? 2 : (hovered ? 1 : 0));
    }

    RECT search = GetSearchRect();
    const bool searchHovered = PtInRect(&search, app_->lastMousePoint_) != FALSE;
    const int backgroundSize = std::max(1, std::min(58,
        static_cast<int>(std::min(search.right - search.left, search.bottom - search.top)) - 10));
    RECT searchBackground{
        search.left + (search.right - search.left - backgroundSize) / 2,
        search.top + (search.bottom - search.top - backgroundSize) / 2,
        search.left + (search.right - search.left + backgroundSize) / 2,
        search.top + (search.bottom - search.top + backgroundSize) / 2
    };
    const float backgroundAlpha = std::clamp(app_->iconBeautifyBgOpacity_, 0.18f, 1.0f);
    app_->DrawD2DRoundedRectangle(context, searchBackground, 13.0f,
        D2D1::ColorF(app_->iconBeautifyBgStartR_, app_->iconBeautifyBgStartG_,
            app_->iconBeautifyBgStartB_, searchHovered
                ? std::min(1.0f, backgroundAlpha + 0.12f) : backgroundAlpha),
        searchHovered
            ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.82f)
            : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.24f),
        searchHovered ? 1.6f : 1.0f);

    const bool light = app_ && app_->quickNavLightTheme_;
    ComPtr<ID2D1SolidColorBrush> brush;
    context->CreateSolidColorBrush(light
        ? D2D1::ColorF(0.10f, 0.12f, 0.16f, 0.90f)
        : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.90f), &brush);
    if (!brush) return;
    const float cx = (searchBackground.left + searchBackground.right) / 2.0f - 4.0f;
    const float cy = (searchBackground.top + searchBackground.bottom) / 2.0f - 4.0f;
    context->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 12.0f, 12.0f), brush.Get(), 3.0f);
    context->DrawLine(D2D1::Point2F(cx + 8.0f, cy + 8.0f),
        D2D1::Point2F(cx + 18.0f, cy + 18.0f), brush.Get(), 3.0f);
}

std::vector<Item*> DockContainer::GetSelectedItems() const
{
    std::vector<Item*> selected;
    for (const auto& item : entryItems_)
        if (item && item->IsSelected()) selected.push_back(item.get());
    return selected;
}

HitRegion DockContainer::HitTestDrag(POINT pt, Slot*& outSlot)
{
    auto resetDwell = [&]() {
        if (!app_) return;
        app_->dockHandoffDwellIndex_ = static_cast<size_t>(-1);
        app_->dockHandoffDwellStartTick_ = 0;
        app_->dockHandoffDwellReady_ = false;
        if (app_->hwnd_) KillTimer(app_->hwnd_, kDockHandoffDwellTimerId);
    };
    outSlot = nullptr;
    RECT dockBounds = GetBounds();
    if (!PtInRect(&dockBounds, pt))
    {
        resetDwell();
        return HitRegion::None;
    }
    for (const auto& slot : GetSlots())
    {
        RECT bounds = slot->GetBounds();
        if (!PtInRect(&bounds, pt)) continue;
        outSlot = slot.get();
        if (slot->GetIndex() >= (entries_ ? entries_->size() : 0))
        {
            resetDwell();
            return HitRegion::Empty;
        }

        RECT handoffRect = bounds;
        InflateRect(&handoffRect, -16, -10);
        Item* targetItem = slot->GetItem();
        const bool canHandoff = targetItem && !targetItem->IsSelected() &&
            PtInRect(&handoffRect, pt);
        if (canHandoff && app_)
        {
            const size_t index = slot->GetIndex();
            const DWORD now = GetTickCount();
            if (app_->dockHandoffDwellIndex_ != index)
            {
                app_->dockHandoffDwellIndex_ = index;
                app_->dockHandoffDwellStartTick_ = now;
                app_->dockHandoffDwellReady_ = false;
                if (app_->hwnd_)
                    SetTimer(app_->hwnd_, kDockHandoffDwellTimerId,
                        kDockHandoffDwellIntervalMs, nullptr);
            }
            if (app_->dockHandoffDwellReady_ ||
                now - app_->dockHandoffDwellStartTick_ >= kDockHandoffDwellDelayMs)
            {
                app_->dockHandoffDwellReady_ = true;
                return HitRegion::Handoff;
            }
        }
        else
        {
            resetDwell();
        }
        return pt.x < (bounds.left + bounds.right) / 2
            ? HitRegion::SortBefore : HitRegion::SortAfter;
    }
    resetDwell();
    return HitRegion::Empty;
}

std::wstring DockContainer::GetDragHint(Slot* slot, HitRegion region,
    const std::vector<Item*>& sourceItems, Container* origin, int mods) const
{
    if (region == HitRegion::Handoff && slot && slot->GetItem())
        return L"释放：交给「" + slot->GetItem()->GetTitle() + L"」处理";
    if (origin != this && !HasCapacity(sourceItems.empty() ? 1 : sourceItems.size()))
        return L"Dock 已满";
    if (origin == this) return L"释放：调整 Dock 顺序";
    return (mods & MK_CONTROL)
        ? L"释放：建立 Dock 映射（保留原入口）"
        : L"释放：移动到 Dock（按住 Ctrl 可建立映射）";
}

void DockContainer::DrawDropPreview(ID2D1DeviceContext* ctx, Slot* slot, HitRegion region)
{
    if (!slot || !ctx) return;
    if (region == HitRegion::Handoff)
    {
        RECT bounds = slot->GetBounds();
        app_->DrawD2DRoundedRectangle(ctx, bounds, 12.0f,
            D2D1::ColorF(0.28f, 0.80f, 0.48f, 0.18f),
            D2D1::ColorF(0.28f, 0.90f, 0.52f, 0.88f), 2.0f);
        return;
    }
    slot->DrawDropIndicator(ctx, region);
}

RECT DockContainer::GetSearchRect() const
{
    const auto& slots = const_cast<DockContainer*>(this)->GetSlots();
    return slots.empty() ? RECT{} : slots.back()->GetBounds();
}

bool DockContainer::IsSearchPoint(POINT pt) const
{
    RECT rect = GetSearchRect();
    return PtInRect(&rect, pt) != FALSE;
}

DockEntryItem* DockContainer::EntryAtPoint(POINT pt) const
{
    const auto& slots = const_cast<DockContainer*>(this)->GetSlots();
    const size_t count = entries_ ? entries_->size() : 0;
    for (size_t i = 0; i < count && i < slots.size(); ++i)
    {
        RECT bounds = slots[i]->GetBounds();
        if (PtInRect(&bounds, pt)) return dynamic_cast<DockEntryItem*>(slots[i]->GetItem());
    }
    return nullptr;
}

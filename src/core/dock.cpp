#include "dock.h"

#include "app.h"
#include "constants.h"
#include "slot.h"

#include <algorithm>
#include <cmath>
#include <shlwapi.h>

DockFrequentItem::DockFrequentItem(
    DesktopApp* app, Container* container, size_t itemIndex)
    : app_(app), container_(container), itemIndex_(itemIndex) {}

std::wstring DockFrequentItem::GetTitle() const
{
    return app_ && itemIndex_ < app_->items_.size()
        ? app_->items_[itemIndex_].name : L"";
}

std::wstring DockFrequentItem::GetPath() const
{
    return app_ && itemIndex_ < app_->items_.size()
        ? app_->items_[itemIndex_].parsingName : L"";
}

HBITMAP DockFrequentItem::GetIconBitmap() const
{
    return app_ && itemIndex_ < app_->items_.size()
        ? app_->items_[itemIndex_].iconBitmap : nullptr;
}

RECT DockFrequentItem::GetBounds() const { return bounds_; }
void DockFrequentItem::SetBounds(RECT bounds) { bounds_ = bounds; }

bool DockFrequentItem::IsSelected() const
{
    return app_ && itemIndex_ < app_->items_.size() && app_->items_[itemIndex_].selected;
}

void DockFrequentItem::SetSelected(bool selected)
{
    if (app_ && itemIndex_ < app_->items_.size())
        app_->items_[itemIndex_].selected = selected;
}

Container* DockFrequentItem::GetContainer() const { return container_; }

void DockFrequentItem::Draw(ID2D1DeviceContext* context, RECT rect, int state)
{
    if (!app_ || itemIndex_ >= app_->items_.size()) return;
    DockEntry entry{ DockEntryType::DesktopItem,
        app_->items_[itemIndex_].layoutKey, true };
    app_->DrawDockEntry(context, entry, rect, state);
}

ComPtr<IDataObject> DockFrequentItem::CreateDataObject()
{
    if (!app_ || itemIndex_ >= app_->items_.size()) return nullptr;
    DesktopIcon icon(&app_->items_[itemIndex_], container_, app_);
    return icon.CreateDataObject();
}

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

bool DockContainer::IsVertical() const
{
    return app_ && (app_->dockSettings_.position == DockPosition::Left ||
        app_->dockSettings_.position == DockPosition::Right);
}

bool DockContainer::IsEdgeAttached() const
{
    return app_ && app_->dockSettings_.edgeAttached;
}

size_t DockContainer::SortableEntryCount() const
{
    const size_t count = entries_ ? entries_->size() : 0;
    return count > 0 && app_ && app_->IsRecycleBinDockEntry(entries_->back())
        ? count - 1
        : count;
}

int DockContainer::ItemPitch() const
{
    const int iconSize = app_ ? app_->GetDockItemIconSize() : kIconSize;
    return std::max(1, iconSize + kDockSpacing);
}

int DockContainer::EdgeMargin() const
{
    if (app_)
    {
        const GridPage* page = app_->GetFirstPageGridPage();
        if (page) return std::max(0, IsVertical() ? page->marginX : page->marginY);
    }
    return IsVertical() ? kGridMarginX : kGridMarginY;
}

BarStyle DockContainer::GetInsertionStyle() const
{
    return IsVertical() ? BarStyle::HBar : BarStyle::VBar;
}

size_t DockContainer::Capacity() const
{
    const LONG extent = IsVertical()
        ? area_.bottom - area_.top : area_.right - area_.left;
    const LONG available = std::max(0L, extent -
        (IsEdgeAttached() ? kDockSpacing : kDockSpacing * 3L) -
        kDockSeparatorGap * 2L);
    const LONG visibleSlots = available / std::max(1, ItemPitch());
    return static_cast<size_t>(std::max(0L, visibleSlots - 1L));
}

bool DockContainer::HasCapacity(size_t additional) const
{
    return entries_ && entries_->size() + additional <= Capacity();
}

RECT DockContainer::GetBounds() const
{
    const size_t count = entries_ ? entries_->size() : 0;
    const size_t requestedFrequentCount = app_
        ? app_->GetFrequentDockItemIndices().size() : 0;
    const size_t frequentCount = std::min(requestedFrequentCount,
        Capacity() > count ? Capacity() - count : 0);
    const size_t fixedCount = SortableEntryCount();
    const int separatorCount =
        (fixedCount > 0 && frequentCount > 0 ? 1 : 0) +
        (fixedCount + frequentCount > 0 ? 1 : 0);
    const bool vertical = IsVertical();
    const int iconSize = app_ ? app_->GetDockItemIconSize() : kIconSize;
    const int slotLength = ItemPitch();
    const int desiredLength = static_cast<int>(
        (count + frequentCount + 1) * slotLength) + kDockSpacing +
        separatorCount * kDockSeparatorGap;
    const int areaWidth = std::max(1, static_cast<int>(area_.right - area_.left));
    const int areaHeight = std::max(1, static_cast<int>(area_.bottom - area_.top));
    const int maxLength = std::max(1,
        (vertical ? areaHeight : areaWidth) - kDockSpacing * 2);
    const int length = std::min(desiredLength, maxLength);
    const int thickness = std::min(iconSize + kDockSpacing * 2,
        vertical ? areaWidth : areaHeight);
    if (IsEdgeAttached())
    {
        switch (app_->dockSettings_.position)
        {
        case DockPosition::Top:
            return RECT{ area_.left, area_.top, area_.right, area_.top + thickness };
        case DockPosition::Left:
            return RECT{ area_.left, area_.top, area_.left + thickness, area_.bottom };
        case DockPosition::Right:
            return RECT{ area_.right - thickness, area_.top, area_.right, area_.bottom };
        case DockPosition::Bottom:
        default:
            return RECT{ area_.left, area_.bottom - thickness, area_.right, area_.bottom };
        }
    }
    const int edgeDistance = std::max(kDockSpacing, EdgeMargin());
    const int innerGap = edgeDistance - EdgeMargin();
    if (vertical)
    {
        const int left = app_ && app_->dockSettings_.position == DockPosition::Left
            ? area_.left + edgeDistance
            : area_.left + innerGap;
        const int top = area_.top + (areaHeight - length) / 2;
        return RECT{ left, top, left + thickness, top + length };
    }
    const int left = area_.left + (areaWidth - length) / 2;
    const int top = app_ && app_->dockSettings_.position == DockPosition::Top
        ? area_.top + edgeDistance
        : area_.top + innerGap;
    return RECT{ left, top, left + length, top + thickness };
}

std::vector<std::unique_ptr<Slot>> DockContainer::BuildSlots()
{
    std::vector<std::unique_ptr<Slot>> slots;
    entryItems_.clear();
    frequentItems_.clear();
    RECT bounds = GetBounds();
    const size_t count = entries_ ? entries_->size() : 0;
    const size_t fixedCount = SortableEntryCount();
    const bool hasRecycleBin = fixedCount < count;
    std::vector<size_t> frequentIndices = app_
        ? app_->GetFrequentDockItemIndices() : std::vector<size_t>{};
    const size_t availableFrequent = Capacity() > count ? Capacity() - count : 0;
    if (frequentIndices.size() > availableFrequent)
        frequentIndices.resize(availableFrequent);
    const size_t frequentCount = frequentIndices.size();
    const int slotLength = ItemPitch();
    const int halfGap = kDockSpacing / 2;
    const int frequentOffset = fixedCount > 0 && frequentCount > 0
        ? kDockSeparatorGap : 0;
    const int searchOffset = frequentOffset +
        (fixedCount + frequentCount > 0 ? kDockSeparatorGap : 0);

    auto makeCell = [&](size_t visualIndex, int axisOffset,
        bool searchSlot, bool recycleBinSlot) {
        RECT cell{};
        if (IsVertical())
        {
            LONG top = bounds.top + halfGap +
                static_cast<LONG>(visualIndex * slotLength) + axisOffset;
            if (IsEdgeAttached() && (searchSlot || recycleBinSlot))
                top = bounds.bottom - halfGap - slotLength *
                    (searchSlot && hasRecycleBin ? 2 : 1);
            cell = RECT{ bounds.left,
                top,
                bounds.right,
                top + slotLength };
        }
        else
        {
            LONG left = bounds.left + halfGap +
                static_cast<LONG>(visualIndex * slotLength) + axisOffset;
            if (IsEdgeAttached() && (searchSlot || recycleBinSlot))
                left = bounds.right - halfGap - slotLength *
                    (searchSlot && hasRecycleBin ? 2 : 1);
            cell = RECT{ left,
                bounds.top,
                left + slotLength,
                bounds.bottom };
        }
        return cell;
    };

    slots.reserve(count + frequentCount + 1);
    for (size_t i = 0; i < count; ++i)
    {
        const bool recycleBinSlot = hasRecycleBin && i + 1 == count;
        const size_t visualIndex = recycleBinSlot
            ? fixedCount + frequentCount + 1
            : i;
        RECT cell = makeCell(visualIndex,
            recycleBinSlot ? searchOffset : 0, false, recycleBinSlot);
        auto slot = std::make_unique<Slot>(this, cell, i);
        auto item = std::make_unique<DockEntryItem>(app_, this, i);
        item->SetBounds(cell);
        slot->SetItem(item.get());
        entryItems_.push_back(std::move(item));
        slots.push_back(std::move(slot));
    }

    for (size_t i = 0; i < frequentCount; ++i)
    {
        const size_t slotIndex = count + i;
        RECT cell = makeCell(fixedCount + i, frequentOffset, false, false);
        auto slot = std::make_unique<Slot>(this, cell, slotIndex);
        auto item = std::make_unique<DockFrequentItem>(
            app_, this, frequentIndices[i]);
        item->SetBounds(cell);
        slot->SetItem(item.get());
        frequentItems_.push_back(std::move(item));
        slots.push_back(std::move(slot));
    }

    const size_t searchIndex = count + frequentCount;
    RECT searchCell = makeCell(
        fixedCount + frequentCount, searchOffset, true, false);
    slots.push_back(std::make_unique<Slot>(this, searchCell, searchIndex));
    return slots;
}

size_t DockContainer::InsertIndexFor(Slot* slot, HitRegion region) const
{
    const size_t count = SortableEntryCount();
    if (!slot) return count;
    size_t index = std::min(slot->GetIndex(), count);
    if (region == HitRegion::SortAfter) ++index;
    return std::min(index, count);
}

size_t DockContainer::GetInsertIndexAtPoint(POINT pt) const
{
    const size_t count = SortableEntryCount();
    const auto& slots = const_cast<DockContainer*>(this)->GetSlots();
    for (size_t i = 0; i < count && i < slots.size(); ++i)
    {
        RECT bounds = slots[i]->GetBounds();
        if (PtInRect(&bounds, pt))
            return IsVertical()
                ? (pt.y < (bounds.top + bounds.bottom) / 2 ? i : i + 1)
                : (pt.x < (bounds.left + bounds.right) / 2 ? i : i + 1);
    }
    return count;
}

void DockContainer::DrawInsertionPreview(
    ID2D1DeviceContext* context, size_t insertIndex) const
{
    if (!context) return;
    const size_t count = SortableEntryCount();
    insertIndex = std::min(insertIndex, count);
    RECT bounds = GetBounds();
    ComPtr<ID2D1SolidColorBrush> brush;
    context->CreateSolidColorBrush(D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.95f), &brush);
    if (!brush) return;
    if (IsVertical())
    {
        const float y = static_cast<float>(bounds.top + kDockSpacing / 2 +
            static_cast<LONG>(insertIndex * ItemPitch()));
        context->FillRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(static_cast<float>(bounds.left + 10), y - 2.0f,
                static_cast<float>(bounds.right - 10), y + 2.0f), 2.0f, 2.0f),
            brush.Get());
        return;
    }
    const float x = static_cast<float>(bounds.left + kDockSpacing / 2 +
        static_cast<LONG>(insertIndex * ItemPitch()));
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
    PersonalizationSettings p = app_ && !app_->dockSettings_.followPersonalization
        ? app_->dockSettings_.appearance
        : PersonalizationSettings::DarkPreset();
    if (app_ && app_->settingsWindow_)
        p = app_->settingsWindow_->GetDockAppearance();
    p.shadowAlpha = 0.0f;
    const float panelRadius = IsEdgeAttached() ? 0.0f : p.cornerRadius;
    const D2D1_COLOR_F fill = D2D1::ColorF(
        p.widgetBgR, p.widgetBgG, p.widgetBgB, p.widgetAlpha);
    const D2D1_COLOR_F border = D2D1::ColorF(
        p.widgetBorderR, p.widgetBorderG, p.widgetBorderB, p.widgetBorderAlpha);
    if (app_)
        app_->DrawWidgetPanelBackground(context, bounds, panelRadius, 1.0f,
            fill, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f), false, 1.0f, &p);

    if (border.a > 0.0f)
    {
        ComPtr<ID2D1SolidColorBrush> borderBrush;
        if (SUCCEEDED(context->CreateSolidColorBrush(border, &borderBrush)) && borderBrush)
        {
            if (IsEdgeAttached())
            {
                D2D1_POINT_2F start{};
                D2D1_POINT_2F end{};
                switch (app_->dockSettings_.position)
                {
                case DockPosition::Top:
                    start = D2D1::Point2F(static_cast<float>(bounds.left),
                        static_cast<float>(bounds.bottom) - 0.5f);
                    end = D2D1::Point2F(static_cast<float>(bounds.right), start.y);
                    break;
                case DockPosition::Left:
                    start = D2D1::Point2F(static_cast<float>(bounds.right) - 0.5f,
                        static_cast<float>(bounds.top));
                    end = D2D1::Point2F(start.x, static_cast<float>(bounds.bottom));
                    break;
                case DockPosition::Right:
                    start = D2D1::Point2F(static_cast<float>(bounds.left) + 0.5f,
                        static_cast<float>(bounds.top));
                    end = D2D1::Point2F(start.x, static_cast<float>(bounds.bottom));
                    break;
                case DockPosition::Bottom:
                default:
                    start = D2D1::Point2F(static_cast<float>(bounds.left),
                        static_cast<float>(bounds.top) + 0.5f);
                    end = D2D1::Point2F(static_cast<float>(bounds.right), start.y);
                    break;
                }
                context->DrawLine(start, end, borderBrush.Get(), 1.0f);
            }
            else
            {
                context->DrawRoundedRectangle(D2D1::RoundedRect(
                    D2D1::RectF(static_cast<float>(bounds.left), static_cast<float>(bounds.top),
                        static_cast<float>(bounds.right), static_cast<float>(bounds.bottom)),
                    panelRadius, panelRadius), borderBrush.Get(), 1.0f);
            }
        }
    }

}

void DockContainer::DrawContents(ID2D1DeviceContext* context)
{
    if (!context) return;
    const auto& slots = GetSlots();
    const size_t count = entries_ ? entries_->size() : 0;
    std::wstring hoveredTitle;
    RECT hoveredBounds{};
    for (size_t i = 0; i < count && i < slots.size(); ++i)
    {
        Item* item = slots[i]->GetItem();
        if (!item) continue;
        RECT slotBounds = slots[i]->GetBounds();
        const bool hovered = PtInRect(&slotBounds, app_->lastMousePoint_) != FALSE;
        item->Draw(context, slotBounds, item->IsSelected() ? 2 : (hovered ? 1 : 0));
        if (hovered && !app_->dragSession_.IsActive())
        {
            hoveredTitle = item->GetTitle();
            hoveredBounds = slotBounds;
        }
    }

    for (const auto& item : frequentItems_)
    {
        if (!item) continue;
        const RECT itemBounds = item->GetBounds();
        const bool hovered = PtInRect(&itemBounds, app_->lastMousePoint_) != FALSE;
        item->Draw(context, itemBounds, item->IsSelected() ? 2 : (hovered ? 1 : 0));
        if (hovered && !app_->dragSession_.IsActive())
        {
            hoveredTitle = item->GetTitle();
            hoveredBounds = itemBounds;
        }
    }

    const size_t fixedCount = SortableEntryCount();
    auto drawSeparatorBefore = [&](const RECT& followingBounds) {
        ComPtr<ID2D1SolidColorBrush> separatorBrush;
        context->CreateSolidColorBrush(
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f), &separatorBrush);
        if (!separatorBrush) return;
        if (IsVertical())
        {
            const float y = static_cast<float>(followingBounds.top) -
                static_cast<float>(kDockSeparatorGap) * 0.5f;
            const float centerX = (followingBounds.left + followingBounds.right) / 2.0f;
            context->DrawLine(D2D1::Point2F(centerX - 14.0f, y),
                D2D1::Point2F(centerX + 14.0f, y), separatorBrush.Get(), 1.0f);
        }
        else
        {
            const float x = static_cast<float>(followingBounds.left) -
                static_cast<float>(kDockSeparatorGap) * 0.5f;
            const float centerY = (followingBounds.top + followingBounds.bottom) / 2.0f;
            context->DrawLine(D2D1::Point2F(x, centerY - 14.0f),
                D2D1::Point2F(x, centerY + 14.0f), separatorBrush.Get(), 1.0f);
        }
    };

    if (fixedCount > 0 && !frequentItems_.empty())
    {
        drawSeparatorBefore(frequentItems_.front()->GetBounds());
    }

    RECT search = GetSearchRect();
    if (fixedCount > 0 || !frequentItems_.empty())
        drawSeparatorBefore(search);
    const bool searchHovered = PtInRect(&search, app_->lastMousePoint_) != FALSE;
    const int backgroundSize = app_->GetDockItemIconSize();
    const float searchScale = static_cast<float>(backgroundSize) / 52.0f;
    RECT searchBackground{
        search.left + (search.right - search.left - backgroundSize) / 2,
        search.top + (search.bottom - search.top - backgroundSize) / 2,
        search.left + (search.right - search.left + backgroundSize) / 2,
        search.top + (search.bottom - search.top + backgroundSize) / 2
    };
    const bool lightSurface = app_->DrawDockControlBackground(
        context, searchBackground, searchHovered ? 1 : 0);

    ComPtr<ID2D1SolidColorBrush> brush;
    context->CreateSolidColorBrush(searchHovered
        ? D2D1::ColorF(0.30f, 0.58f, 1.0f, 1.0f)
        : (lightSurface
            ? D2D1::ColorF(0.08f, 0.11f, 0.16f, 0.88f)
            : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.92f)), &brush);
    if (!brush) return;
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1StrokeStyle> roundedStroke;
    context->GetFactory(&factory);
    if (factory)
    {
        const D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_SOLID, 0.0f);
        factory->CreateStrokeStyle(properties, nullptr, 0, &roundedStroke);
    }
    const float centerX = (searchBackground.left + searchBackground.right) / 2.0f -
        2.7f * searchScale;
    const float centerY = (searchBackground.top + searchBackground.bottom) / 2.0f -
        2.7f * searchScale;
    const float searchStroke = std::max(1.5f, 2.35f * searchScale);
    context->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(centerX, centerY),
        9.2f * searchScale, 9.2f * searchScale),
        brush.Get(), searchStroke, roundedStroke.Get());
    context->DrawLine(D2D1::Point2F(centerX + 6.6f * searchScale,
            centerY + 6.6f * searchScale),
        D2D1::Point2F(centerX + 13.5f * searchScale,
            centerY + 13.5f * searchScale),
        brush.Get(), searchStroke, roundedStroke.Get());

    if (searchHovered && !app_->dragSession_.IsActive())
    {
        hoveredTitle = L"快捷搜索";
        hoveredBounds = search;
    }

    if (!hoveredTitle.empty() && app_->dwriteFactory_)
    {
        ComPtr<IDWriteTextFormat> tooltipFormat;
        app_->dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-CN", &tooltipFormat);
        if (tooltipFormat)
        {
            tooltipFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            tooltipFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            ComPtr<IDWriteTextLayout> layout;
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(app_->dwriteFactory_->CreateTextLayout(
                hoveredTitle.c_str(), static_cast<UINT32>(hoveredTitle.size()),
                tooltipFormat.Get(), 240.0f, 28.0f, &layout)) && layout)
                layout->GetMetrics(&metrics);

            const int tooltipWidth = std::clamp(
                static_cast<int>(std::ceil(metrics.widthIncludingTrailingWhitespace)) + 20,
                48, 260);
            constexpr int tooltipHeight = 30;
            constexpr int tooltipGap = 8;
            RECT tooltip{};
            switch (app_->dockSettings_.position)
            {
            case DockPosition::Top:
                tooltip.left = (hoveredBounds.left + hoveredBounds.right - tooltipWidth) / 2;
                tooltip.top = hoveredBounds.bottom + tooltipGap;
                break;
            case DockPosition::Left:
                tooltip.left = hoveredBounds.right + tooltipGap;
                tooltip.top = (hoveredBounds.top + hoveredBounds.bottom - tooltipHeight) / 2;
                break;
            case DockPosition::Right:
                tooltip.left = hoveredBounds.left - tooltipGap - tooltipWidth;
                tooltip.top = (hoveredBounds.top + hoveredBounds.bottom - tooltipHeight) / 2;
                break;
            case DockPosition::Bottom:
            default:
                tooltip.left = (hoveredBounds.left + hoveredBounds.right - tooltipWidth) / 2;
                tooltip.top = hoveredBounds.top - tooltipGap - tooltipHeight;
                break;
            }
            tooltip.right = tooltip.left + tooltipWidth;
            tooltip.bottom = tooltip.top + tooltipHeight;

            POINT dockCenter{
                (hoveredBounds.left + hoveredBounds.right) / 2,
                (hoveredBounds.top + hoveredBounds.bottom) / 2
            };
            for (const auto& page : app_->gridPages_)
            {
                if (!PtInRect(&page.bounds, dockCenter)) continue;
                const int minLeft = static_cast<int>(page.bounds.left + 6);
                const int maxLeft = static_cast<int>(std::max<LONG>(
                    page.bounds.left + 6, page.bounds.right - tooltipWidth - 6));
                const int minTop = static_cast<int>(page.bounds.top + 6);
                const int maxTop = static_cast<int>(std::max<LONG>(
                    page.bounds.top + 6, page.bounds.bottom - tooltipHeight - 6));
                tooltip.left = std::clamp(static_cast<int>(tooltip.left), minLeft, maxLeft);
                tooltip.top = std::clamp(static_cast<int>(tooltip.top), minTop, maxTop);
                tooltip.right = tooltip.left + tooltipWidth;
                tooltip.bottom = tooltip.top + tooltipHeight;
                break;
            }

            app_->DrawD2DRoundedRectangle(context, tooltip, 7.0f,
                D2D1::ColorF(0.06f, 0.07f, 0.09f, 0.94f),
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f));
            app_->DrawD2DTextEllipsis(context, hoveredTitle, tooltip,
                tooltipFormat.Get(), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f),
                DWRITE_TEXT_ALIGNMENT_CENTER,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
        }
    }
}

std::vector<Item*> DockContainer::GetSelectedItems() const
{
    std::vector<Item*> selected;
    for (const auto& item : entryItems_)
        if (item && item->IsSelected()) selected.push_back(item.get());
    for (const auto& item : frequentItems_)
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
        Item* targetItem = slot->GetItem();
        if (!targetItem || dynamic_cast<DockFrequentItem*>(targetItem))
        {
            resetDwell();
            return HitRegion::Empty;
        }

        RECT handoffRect = bounds;
        InflateRect(&handoffRect, -16, -10);
        if (auto* dockItem = dynamic_cast<DockEntryItem*>(targetItem);
            dockItem && app_ && dockItem->GetEntryIndex() < app_->dockEntries_.size() &&
            app_->IsRecycleBinDockEntry(app_->dockEntries_[dockItem->GetEntryIndex()]))
        {
            resetDwell();
            const auto& sourceItems = app_->dragSession_.Items();
            const bool isDragSource = std::find(sourceItems.begin(), sourceItems.end(),
                targetItem) != sourceItems.end();
            return isDragSource ? HitRegion::None : HitRegion::Handoff;
        }
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
        if (IsVertical())
            return pt.y < (bounds.top + bounds.bottom) / 2
                ? HitRegion::SortBefore : HitRegion::SortAfter;
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

DockFrequentItem* DockContainer::FrequentItemAtPoint(POINT pt) const
{
    const_cast<DockContainer*>(this)->GetSlots();
    for (const auto& item : frequentItems_)
    {
        if (!item) continue;
        RECT bounds = item->GetBounds();
        if (PtInRect(&bounds, pt)) return item.get();
    }
    return nullptr;
}

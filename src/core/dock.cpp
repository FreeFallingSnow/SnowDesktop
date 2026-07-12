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

bool DockContainer::IsVertical() const
{
    return app_ && (app_->dockSettings_.position == DockPosition::Left ||
        app_->dockSettings_.position == DockPosition::Right);
}

bool DockContainer::IsEdgeAttached() const
{
    return app_ && app_->dockSettings_.edgeAttached;
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
        (IsEdgeAttached() ? kDockSpacing : kDockSpacing * 3L));
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
    const bool vertical = IsVertical();
    const int iconSize = app_ ? app_->GetDockItemIconSize() : kIconSize;
    const int slotLength = ItemPitch();
    const int desiredLength = static_cast<int>((count + 1) * slotLength) + kDockSpacing;
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
    RECT bounds = GetBounds();
    const size_t count = entries_ ? entries_->size() : 0;
    const int slotLength = ItemPitch();
    const int halfGap = kDockSpacing / 2;
    for (size_t i = 0; i <= count; ++i)
    {
        RECT cell{};
        const bool searchSlot = i == count;
        if (IsVertical())
        {
            const LONG top = IsEdgeAttached() && searchSlot
                ? bounds.bottom - halfGap - slotLength
                : bounds.top + halfGap + static_cast<LONG>(i * slotLength);
            cell = RECT{ bounds.left,
                top,
                bounds.right,
                top + slotLength };
        }
        else
        {
            const LONG left = IsEdgeAttached() && searchSlot
                ? bounds.right - halfGap - slotLength
                : bounds.left + halfGap + static_cast<LONG>(i * slotLength);
            cell = RECT{ left,
                bounds.top,
                left + slotLength,
                bounds.bottom };
        }
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
    const size_t count = entries_ ? entries_->size() : 0;
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
    const float panelRadius = IsEdgeAttached() ? 0.0f : p.cornerRadius;
    const D2D1_COLOR_F fill = D2D1::ColorF(
        p.widgetBgR, p.widgetBgG, p.widgetBgB, p.widgetAlpha);
    const D2D1_COLOR_F border = D2D1::ColorF(
        p.widgetBorderR, p.widgetBorderG, p.widgetBorderB, p.widgetBorderAlpha);
    if (app_)
        app_->DrawWidgetPanelBackground(context, bounds, panelRadius, 1.0f,
            fill, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f), false, 1.0f, &p);

    if (p.gradientEndA > 0.001f)
    {
        D2D1_GRADIENT_STOP stopsData[] = {
            { 0.0f, D2D1::ColorF(fill.r, fill.g, fill.b, 0.0f) },
            { 1.0f, D2D1::ColorF(fill.r, fill.g, fill.b,
                std::clamp(p.gradientEndA, 0.0f, 1.0f)) },
        };
        ComPtr<ID2D1GradientStopCollection> stops;
        ComPtr<ID2D1LinearGradientBrush> gradient;
        if (SUCCEEDED(context->CreateGradientStopCollection(stopsData, 2,
            D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &stops)) && stops &&
            SUCCEEDED(context->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(0.0f, bounds.top + (bounds.bottom - bounds.top) * 0.35f),
                    D2D1::Point2F(0.0f, static_cast<float>(bounds.bottom))),
                stops.Get(), &gradient)) && gradient)
        {
            context->FillRoundedRectangle(D2D1::RoundedRect(
                D2D1::RectF(static_cast<float>(bounds.left), static_cast<float>(bounds.top),
                    static_cast<float>(bounds.right), static_cast<float>(bounds.bottom)),
                panelRadius, panelRadius), gradient.Get());
        }
    }

    if (border.a > 0.0f)
    {
        ComPtr<ID2D1SolidColorBrush> borderBrush;
        if (SUCCEEDED(context->CreateSolidColorBrush(border, &borderBrush)) && borderBrush)
            context->DrawRoundedRectangle(D2D1::RoundedRect(
                D2D1::RectF(static_cast<float>(bounds.left), static_cast<float>(bounds.top),
                    static_cast<float>(bounds.right), static_cast<float>(bounds.bottom)),
                panelRadius, panelRadius), borderBrush.Get(), 1.0f);
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

    RECT search = GetSearchRect();
    const bool searchHovered = PtInRect(&search, app_->lastMousePoint_) != FALSE;
    PersonalizationSettings dockAppearance = app_ && !app_->dockSettings_.followPersonalization
        ? app_->dockSettings_.appearance
        : PersonalizationSettings::DarkPreset();
    if (app_ && app_->settingsWindow_)
        dockAppearance = app_->settingsWindow_->GetDockAppearance();
    const int backgroundSize = app_->GetDockItemIconSize();
    const float searchScale = static_cast<float>(backgroundSize) / 52.0f;
    RECT searchBackground{
        search.left + (search.right - search.left - backgroundSize) / 2,
        search.top + (search.bottom - search.top - backgroundSize) / 2,
        search.left + (search.right - search.left + backgroundSize) / 2,
        search.top + (search.bottom - search.top + backgroundSize) / 2
    };
    const float luminance = dockAppearance.widgetBgR * 0.2126f +
        dockAppearance.widgetBgG * 0.7152f + dockAppearance.widgetBgB * 0.0722f;
    const bool lightSurface = luminance > 0.58f && dockAppearance.widgetAlpha > 0.10f;
    const D2D1_COLOR_F tileFill = searchHovered
        ? D2D1::ColorF(0.39f, 0.66f, 1.0f, lightSurface ? 0.20f : 0.25f)
        : (lightSurface
            ? D2D1::ColorF(0.08f, 0.11f, 0.16f, 0.075f)
            : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.11f));
    const D2D1_COLOR_F tileBorder = searchHovered
        ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.88f)
        : (lightSurface
            ? D2D1::ColorF(0.08f, 0.11f, 0.16f, 0.14f)
            : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f));
    app_->DrawD2DRoundedRectangle(context, searchBackground,
        std::max(6.0f, 15.0f * searchScale), tileFill, tileBorder,
        (searchHovered ? 1.6f : 1.0f) * std::max(0.75f, searchScale));

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

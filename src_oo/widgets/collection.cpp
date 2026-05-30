#include "widget.h"
#include "slot.h"
#include "types.h"
#include "app.h"
#include <algorithm>

static void EraseKeyFromWidget(DesktopWidget& w, const std::wstring& key)
{
    auto it = std::find(w.itemKeys.begin(), w.itemKeys.end(), key);
    if (it != w.itemKeys.end()) w.itemKeys.erase(it);
}

size_t Collection::GetSlotCount() const
{
    return data_ ? data_->itemKeys.size() : 0;
}

void Collection::OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
    Slot* targetSlot, HitRegion region, int mods)
{
    if (!app_ || !data_) return;
    size_t insertAt = targetSlot ? targetSlot->GetIndex() : data_->itemKeys.size();

    for (auto* src : sourceItems)
    {
        auto* icon = dynamic_cast<DesktopIcon*>(src);
        if (!icon || !icon->GetDesktopItem()) continue;
        std::wstring key = icon->GetDesktopItem()->layoutKey;
        for (auto& w : app_->GetWidgets())
            if (&w != data_) EraseKeyFromWidget(w, key);
        auto it = std::find(data_->itemKeys.begin(), data_->itemKeys.end(), key);
        if (it == data_->itemKeys.end())
        {
            if (insertAt > data_->itemKeys.size()) insertAt = data_->itemKeys.size();
            data_->itemKeys.insert(data_->itemKeys.begin() + static_cast<std::ptrdiff_t>(insertAt), key);
            ++insertAt;
        }
    }
    InvalidateSlots();
    app_->GetDesktopGrid()->InvalidateSlots();
    app_->RebuildContainersAndItems();
    app_->SaveLayoutSlots();
    app_->InvalidateDesktop();
    (void)origin; (void)region; (void)mods;
}

void Collection::DrawContent(ID2D1DeviceContext* context, RECT body)
{
    if (!data_ || !app_) return;
    auto& slots = GetSlots();
    const auto& items = app_->GetDesktopItems();
    for (size_t i = 0; i < slots.size(); ++i)
    {
        size_t idx = slots[i]->GetIndex();
        if (idx >= data_->itemKeys.size()) continue;
        size_t itemIdx = app_->FindItemIndexByKey(data_->itemKeys[idx]);
        if (itemIdx == static_cast<size_t>(-1)) continue;
        const DesktopItem& di = items[itemIdx];
        RECT cell = slots[i]->GetBounds();
        RECT iconRect = app_->GetItemIconRect(cell);
        if (di.iconBitmap)
        {
            ID2D1Bitmap1* bmp = app_->GetOrCreateD2DBitmap(di.iconBitmap);
            if (bmp)
            {
                D2D1_RECT_F d = D2D1::RectF((float)iconRect.left, (float)iconRect.top,
                    (float)iconRect.right, (float)iconRect.bottom);
                context->DrawBitmap(bmp, d, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
            }
        }
        if (!di.name.empty())
            app_->DrawItemText(context, cell, di.name, false, 1.0f);
    }
    (void)body;
}

WidgetHit Collection::HitTestWidget(POINT pt) const
{
    return WidgetContainer::HitTestWidget(pt);
}

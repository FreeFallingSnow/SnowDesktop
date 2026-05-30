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

Item* FileCategories::GetSlotItem(size_t idx) const { return nullptr; }

size_t FileCategories::GetSlotCount() const
{
    if (!data_) return 0;
    // TODO: filter by activeCategoryId
    return data_->itemKeys.size();
}

void FileCategories::OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
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

void FileCategories::DrawContent(ID2D1DeviceContext* context, RECT body)
{
    if (!data_ || !app_) return;
    auto& slots = GetSlots();
    if (slots.empty()) return;
    const auto& items = app_->GetDesktopItems();

    for (size_t i = 0; i < slots.size(); ++i)
    {
        size_t idx = slots[i]->GetIndex();
        if (idx >= data_->itemKeys.size()) continue;
        size_t itemIdx = app_->FindItemIndexByKey(data_->itemKeys[idx]);
        if (itemIdx == static_cast<size_t>(-1)) continue;
        const DesktopItem& di = items[itemIdx];
        RECT cell = slots[i]->GetBounds();

        // List row: 32px high, icon left, text right
        int iconSz = std::min<LONG>(28, cell.bottom - cell.top - 4);
        RECT iconR = { cell.left + 2, cell.top + (cell.bottom - cell.top - iconSz) / 2,
                       cell.left + 2 + iconSz, cell.top + (cell.bottom - cell.top + iconSz) / 2 };
        if (di.iconBitmap)
        {
            ID2D1Bitmap1* bmp = app_->GetOrCreateD2DBitmap(di.iconBitmap);
            if (bmp)
            {
                D2D1_RECT_F d = D2D1::RectF((float)iconR.left, (float)iconR.top,
                    (float)iconR.right, (float)iconR.bottom);
                context->DrawBitmap(bmp, d, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
            }
        }
        if (!di.name.empty())
        {
            RECT textR = { iconR.right + 4, cell.top + 2, cell.right - 4, cell.bottom - 2 };
            int tw = textR.right - textR.left;
            int th = textR.bottom - textR.top;
            if (tw > 0 && th > 0)
            {
                ComPtr<IDWriteTextFormat> fmt;
                ComPtr<ID2D1SolidColorBrush> br;
                // Reuse app's listItemTextFormat if accessible
                app_->DrawItemText(context, textR, di.name, false, 1.0f);
            }
        }
    }
    (void)body;
}

void FileCategories::DrawButtons(ID2D1DeviceContext* context, RECT handleRect, bool hovered)
{
    (void)context; (void)handleRect; (void)hovered;
}

WidgetHit FileCategories::HitTestWidget(POINT pt) const
{
    return WidgetContainer::HitTestWidget(pt);
}

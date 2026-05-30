#include "widget.h"
#include "slot.h"
#include "item.h"
#include "types.h"
#include "app.h"
#include <algorithm>

size_t FolderMapping::GetSlotCount() const
{
    return data_ ? data_->folderEntries.size() : 0;
}

int FolderMapping::GetItemHeight() const
{
    return (data_ && data_->listMode) ? 32 : 136;
}

int FolderMapping::GetItemWidth() const
{
    if (!data_ || !data_->listMode) return 92;
    RECT b = GetBounds();
    return std::max(92L, static_cast<LONG>(b.right - b.left) - 8);
}

bool FolderMapping::SingleColumn() const
{
    return data_ && data_->listMode;
}

void FolderMapping::OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
    Slot* targetSlot, HitRegion region, int mods)
{
    if (!app_ || !data_) return;

    if (origin == this)
    {
        // Same-source reorder
        size_t insertAt = targetSlot ? targetSlot->GetIndex() : data_->folderEntries.size();
        if (insertAt > data_->folderEntries.size()) insertAt = data_->folderEntries.size();

        // Collect selected entries to move
        std::vector<FolderEntry> selected;
        for (auto it = data_->folderEntries.begin(); it != data_->folderEntries.end();)
        {
            if (it->selected) { selected.push_back(std::move(*it)); it = data_->folderEntries.erase(it); }
            else ++it;
        }
        if (insertAt > data_->folderEntries.size()) insertAt = data_->folderEntries.size();
        for (auto& e : selected)
            data_->folderEntries.insert(data_->folderEntries.begin() + static_cast<std::ptrdiff_t>(insertAt++), std::move(e));
    }
    else
    {
        // Cross-source: copy physical files to mapped folder
        if (data_->sourceFolderPath.empty()) return;
        for (auto* src : sourceItems)
        {
            auto* icon = dynamic_cast<DesktopIcon*>(src);
            if (!icon) continue;
            std::wstring path = icon->GetPath();
            if (path.empty()) continue;
            size_t pos = path.find_last_of(L"\\/");
            std::wstring name = (pos != std::wstring::npos) ? path.substr(pos + 1) : path;
            std::wstring dest = data_->sourceFolderPath + L"\\" + name;
            bool move = (mods & MK_CONTROL) == 0;
            if (move) MoveFileExW(path.c_str(), dest.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH);
            else CopyFileW(path.c_str(), dest.c_str(), FALSE);
        }
        // Refresh entries
        app_->RefreshFolderMappingWidget(data_ - app_->GetWidgets().data());
    }

    InvalidateSlots();
    app_->SaveLayoutSlots();
    app_->InvalidateDesktop();
    (void)targetSlot; (void)region; (void)mods;
}

void FolderMapping::DrawContent(ID2D1DeviceContext* context, RECT body)
{
    if (!data_ || !app_) return;
    auto& slots = GetSlots();
    bool listMode = data_->listMode;

    for (size_t i = 0; i < slots.size() && i < data_->folderEntries.size(); ++i)
    {
        const FolderEntry& entry = data_->folderEntries[i];
        RECT cell = slots[i]->GetBounds();

        if (listMode)
        {
            int iconSz = std::min<LONG>(28, cell.bottom - cell.top - 4);
            RECT iconR = { cell.left + 4, cell.top + (cell.bottom - cell.top - iconSz) / 2,
                           cell.left + 4 + iconSz, cell.top + (cell.bottom - cell.top + iconSz) / 2 };
            if (entry.iconBitmap)
            {
                ID2D1Bitmap1* bmp = app_->GetOrCreateD2DBitmap(entry.iconBitmap);
                if (bmp)
                {
                    D2D1_RECT_F d = D2D1::RectF((float)iconR.left, (float)iconR.top,
                        (float)iconR.right, (float)iconR.bottom);
                    context->DrawBitmap(bmp, d, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
                }
            }
            RECT textR = { iconR.right + 4, cell.top + 2, cell.right - 4, cell.bottom - 2 };
            if (textR.right > textR.left && !entry.name.empty())
                app_->DrawItemText(context, textR, entry.name, false, 1.0f);
        }
        else
        {
            RECT iconRect = app_->GetItemIconRect(cell);
            if (entry.iconBitmap)
            {
                ID2D1Bitmap1* bmp = app_->GetOrCreateD2DBitmap(entry.iconBitmap);
                if (bmp)
                {
                    D2D1_RECT_F d = D2D1::RectF((float)iconRect.left, (float)iconRect.top,
                        (float)iconRect.right, (float)iconRect.bottom);
                    context->DrawBitmap(bmp, d, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
                }
            }
            if (!entry.name.empty())
                app_->DrawItemText(context, cell, entry.name, false, 1.0f);
        }
    }
    (void)body;
}

void FolderMapping::DrawButtons(ID2D1DeviceContext* context, RECT handleRect, bool hovered)
{
    if (!data_) return;
    bool listMode = data_->listMode;
    // List/icon toggle button — two small 24x24 squares on the right side
    int btnY = handleRect.top + 2;
    int btnH = std::min<LONG>(22, handleRect.bottom - handleRect.top - 4);
    int iconBtnX = handleRect.right - 50;
    int listBtnX = handleRect.right - 26;

    // Icon mode button
    {
        RECT iconBtn = { iconBtnX, btnY, iconBtnX + 22, btnY + btnH };
        D2D1::ColorF fill = listMode ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f)
                                     : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.30f);
        ComPtr<ID2D1SolidColorBrush> br;
        context->CreateSolidColorBrush(fill, &br);
        if (br) context->FillRectangle(
            D2D1::RectF((float)iconBtn.left, (float)iconBtn.top, (float)iconBtn.right, (float)iconBtn.bottom), br.Get());
        // small grid icon: 4 dots
        float cx = iconBtn.left + 11.0f, cy = iconBtn.top + 11.0f;
        context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.8f), &br);
        if (br)
        {
            for (int r = -1; r <= 1; r += 2)
            for (int c = -1; c <= 1; c += 2)
                context->FillRectangle(D2D1::RectF(cx + (float)c * 5.0f - 1.5f, cy + (float)r * 5.0f - 1.5f,
                                                    cx + (float)c * 5.0f + 1.5f, cy + (float)r * 5.0f + 1.5f), br.Get());
        }
    }

    // List mode button
    {
        RECT listBtn = { listBtnX, btnY, listBtnX + 22, btnY + btnH };
        D2D1::ColorF fill = listMode ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.30f)
                                     : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f);
        ComPtr<ID2D1SolidColorBrush> br;
        context->CreateSolidColorBrush(fill, &br);
        if (br) context->FillRectangle(
            D2D1::RectF((float)listBtn.left, (float)listBtn.top, (float)listBtn.right, (float)listBtn.bottom), br.Get());
        // horizontal lines
        context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.8f), &br);
        if (br)
        {
            for (int y = -1; y <= 1; ++y)
                context->FillRectangle(D2D1::RectF(listBtn.left + 5.0f, listBtn.top + 11.0f + (float)y * 6.0f - 0.5f,
                                                    listBtn.right - 5.0f, listBtn.top + 11.0f + (float)y * 6.0f + 0.5f), br.Get());
        }
    }
    (void)hovered;
}

WidgetHit FolderMapping::HitTestWidget(POINT pt) const
{
    WidgetHit base = WidgetContainer::HitTestWidget(pt);
    if (base != WidgetHit::MoveHandle || !data_) return base;

    // Check list toggle button area
    RECT handle = GetMoveHandleRect();
    int iconBtnX = handle.right - 50;
    int listBtnX = handle.right - 26;
    RECT iconBtn = { iconBtnX, handle.top + 2, iconBtnX + 22, handle.top + 24 };
    RECT listBtn = { listBtnX, handle.top + 2, listBtnX + 22, handle.top + 24 };
    if (PtInRect(&iconBtn, pt) || PtInRect(&listBtn, pt))
        return WidgetHit::ListToggleBtn;
    return base;
}

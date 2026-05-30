#include "widget.h"
#include "slot.h"
#include "item.h"
#include "types.h"
#include <algorithm>

std::vector<std::unique_ptr<Slot>> FolderMapping::BuildSlots()
{
    std::vector<std::unique_ptr<Slot>> slots;
    if (!data_ || data_->type != DesktopWidgetType::FolderMapping) return slots;

    const RECT& bounds = data_->bounds;
    const int cellH = data_->listMode ? 32 : kMinCellHeight;
    const int cellW = data_->listMode ? (bounds.right - bounds.left) : kCellWidth;
    const int cols = std::max(1, static_cast<int>((bounds.right - bounds.left) / cellW));

    for (size_t idx = 0; idx < data_->folderEntries.size(); ++idx)
    {
        int col = static_cast<int>(idx) % cols;
        int row = static_cast<int>(idx) / cols;
        RECT cell = {
            bounds.left + col * cellW,
            bounds.top + row * cellH,
            bounds.left + (col + 1) * cellW,
            bounds.top + (row + 1) * cellH
        };
        slots.push_back(std::make_unique<Slot>(this, cell, idx));
    }
    // Trailing empty slot for drop-at-end
    size_t idx = data_->folderEntries.size();
    int col = static_cast<int>(idx) % cols;
    int row = static_cast<int>(idx) / cols;
    RECT cell = {
        bounds.left + col * cellW,
        bounds.top + row * cellH,
        bounds.left + (col + 1) * cellW,
        bounds.top + (row + 1) * cellH
    };
    slots.push_back(std::make_unique<Slot>(this, cell, idx));
    return slots;
}

void FolderMapping::OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
    Slot* targetSlot, HitRegion region, int mods)
{
    if (!app_) return;
    if (origin == this)
    {
        // Same-source: in-memory reorder
        // TODO: app_->FinishWidgetMemberReorder(data_->id, ...);
    }
    else
    {
        // Cross-source: copy files to folder
        // TODO: app_->CopyFilesToFolder(data_->sourceFolderPath, sourceItems);
        // TODO: app_->RefreshFolderMapping(data_->id);
    }
    (void)sourceItems; (void)origin; (void)targetSlot; (void)region; (void)mods;
}

void FolderMapping::DrawChrome(ID2D1DeviceContext* context, POINT mousePt)
{
    // TODO: list/icon toggle, open folder, title bar, scrollbar, gradient bottom bar
    (void)context; (void)mousePt;
}

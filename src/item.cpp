#include "item.h"
#include "types.h"

// ── DesktopIcon ──────────────────────────────────────────────

DesktopIcon::DesktopIcon(DesktopItem* item, Container* container)
    : item_(item), container_(container) {}

std::wstring DesktopIcon::GetTitle() const { return item_ ? item_->name : L""; }
std::wstring DesktopIcon::GetPath() const { return item_ ? item_->parsingName : L""; }
HBITMAP DesktopIcon::GetIconBitmap() const { return item_ ? item_->iconBitmap : nullptr; }
RECT DesktopIcon::GetBounds() const { return item_ ? item_->bounds : RECT{}; }
void DesktopIcon::SetBounds(RECT bounds) { if (item_) item_->bounds = bounds; }
bool DesktopIcon::IsSelected() const { return item_ && item_->selected; }
void DesktopIcon::SetSelected(bool selected) { if (item_) item_->selected = selected; }
Container* DesktopIcon::GetContainer() const { return container_; }

void DesktopIcon::Draw(ID2D1DeviceContext* context, RECT rect, int state)
{
    // Rendering is handled by the existing DrawD2DItemGeneric in app.h.
    // The OO system dispatches state to the existing rendering function.
    // state: 0=normal, 1=hovered, 2=selected, 3=dragged
    (void)context;
    (void)rect;
    (void)state;
}

ComPtr<IDataObject> DesktopIcon::CreateDataObject()
{
    // OLE data object creation is handled by CreateSelectedDataObject in app.h
    return nullptr;
}

// ── FolderEntryIcon ─────────────────────────────────────────

FolderEntryIcon::FolderEntryIcon(FolderEntry* entry, Container* container)
    : entry_(entry), container_(container) {}

std::wstring FolderEntryIcon::GetTitle() const { return entry_ ? entry_->name : L""; }
std::wstring FolderEntryIcon::GetPath() const { return entry_ ? entry_->fullPath : L""; }
HBITMAP FolderEntryIcon::GetIconBitmap() const { return entry_ ? entry_->iconBitmap : nullptr; }
RECT FolderEntryIcon::GetBounds() const { return bounds_; }
void FolderEntryIcon::SetBounds(RECT bounds) { bounds_ = bounds; }
bool FolderEntryIcon::IsSelected() const { return entry_ && entry_->selected; }
void FolderEntryIcon::SetSelected(bool selected) { if (entry_) entry_->selected = selected; }
Container* FolderEntryIcon::GetContainer() const { return container_; }

void FolderEntryIcon::Draw(ID2D1DeviceContext* context, RECT rect, int state)
{
    (void)context;
    (void)rect;
    (void)state;
}

ComPtr<IDataObject> FolderEntryIcon::CreateDataObject()
{
    return nullptr;
}

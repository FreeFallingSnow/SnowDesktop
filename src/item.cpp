#include "item.h"
#include "types.h"
#ifdef SNOWDESKTOP_OO
#include "app_oo.h"
#endif

// ── DesktopIcon ──────────────────────────────────────────────

DesktopIcon::DesktopIcon(DesktopItem* item, Container* container, void* app)
    : item_(item), container_(container), app_(app) {}

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
#ifdef SNOWDESKTOP_OO
    auto* app = static_cast<SnowDesktopAppOO*>(app_);
    if (!app || !item_) return;
    if (item_->bounds.left >= item_->bounds.right || item_->bounds.top >= item_->bounds.bottom) return;

    const bool hovered = (state == 1);
    const bool selected = (state == 2 || state == 3);
    const bool dragged = (state == 3);
    const float opacity = dragged ? 0.6f : 1.0f;

    if (hovered && !selected)
    {
        app->DrawD2DRoundedRectangle(context, rect, 6.0f,
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f * opacity),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f * opacity));
    }

    RECT iconRect = app->GetItemIconRect(rect);

    if (selected)
    {
        app->DrawD2DFilledRectangle(context,
            app->GetItemSelectionRect(rect, true),
            D2D1::ColorF(0.55f, 0.55f, 0.55f, 0.34f * opacity),
            D2D1::ColorF(0.78f, 0.78f, 0.78f, 0.55f * opacity));
    }

    if (item_->iconBitmap)
    {
        ID2D1Bitmap1* bmp = app->GetOrCreateD2DBitmap(item_->iconBitmap);
        if (bmp)
        {
            D2D1_RECT_F dst = D2D1::RectF(
                static_cast<float>(iconRect.left), static_cast<float>(iconRect.top),
                static_cast<float>(iconRect.right), static_cast<float>(iconRect.bottom));
            context->DrawBitmap(bmp, dst, opacity, D2D1_INTERPOLATION_MODE_LINEAR);
        }
    }

    app->DrawItemText(context, rect, item_->name, selected, opacity);
#else
    (void)context; (void)rect; (void)state;
#endif
}

ComPtr<IDataObject> DesktopIcon::CreateDataObject()
{
    return nullptr;
}

// ── FolderEntryIcon ─────────────────────────────────────────

FolderEntryIcon::FolderEntryIcon(FolderEntry* entry, Container* container, void* app)
    : entry_(entry), container_(container), app_(app) {}

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
#ifdef SNOWDESKTOP_OO
    auto* app = static_cast<SnowDesktopAppOO*>(app_);
    if (!app || !entry_) return;
    if (rect.left >= rect.right || rect.top >= rect.bottom) return;

    const bool hovered = (state == 1);
    const bool selected = (state == 2 || state == 3);
    const bool dragged = (state == 3);
    const float opacity = dragged ? 0.6f : 1.0f;

    if (hovered && !selected)
    {
        app->DrawD2DRoundedRectangle(context, rect, 6.0f,
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f * opacity),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f * opacity));
    }

    RECT iconRect = app->GetItemIconRect(rect);

    if (selected)
    {
        app->DrawD2DFilledRectangle(context,
            app->GetItemSelectionRect(rect, true),
            D2D1::ColorF(0.55f, 0.55f, 0.55f, 0.34f * opacity),
            D2D1::ColorF(0.78f, 0.78f, 0.78f, 0.55f * opacity));
    }

    if (entry_->iconBitmap)
    {
        ID2D1Bitmap1* bmp = app->GetOrCreateD2DBitmap(entry_->iconBitmap);
        if (bmp)
        {
            D2D1_RECT_F dst = D2D1::RectF(
                static_cast<float>(iconRect.left), static_cast<float>(iconRect.top),
                static_cast<float>(iconRect.right), static_cast<float>(iconRect.bottom));
            context->DrawBitmap(bmp, dst, opacity, D2D1_INTERPOLATION_MODE_LINEAR);
        }
    }

    app->DrawItemText(context, rect, entry_->name, selected, opacity);
#else
    (void)context; (void)rect; (void)state;
#endif
}

ComPtr<IDataObject> FolderEntryIcon::CreateDataObject()
{
    return nullptr;
}

// ── ExternalFileItem ─────────────────────────────────────────

ExternalFileItem::ExternalFileItem(const std::wstring& filePath)
    : path_(filePath)
{
    size_t pos = path_.find_last_of(L"\\/");
    title_ = (pos != std::wstring::npos) ? path_.substr(pos + 1) : path_;
}

std::wstring ExternalFileItem::GetTitle() const { return title_; }
std::wstring ExternalFileItem::GetPath() const { return path_; }
HBITMAP ExternalFileItem::GetIconBitmap() const { return nullptr; }
RECT ExternalFileItem::GetBounds() const { return {}; }
void ExternalFileItem::SetBounds(RECT) {}
bool ExternalFileItem::IsSelected() const { return false; }
void ExternalFileItem::SetSelected(bool) {}
Container* ExternalFileItem::GetContainer() const { return nullptr; }
void ExternalFileItem::Draw(ID2D1DeviceContext*, RECT, int) {}
ComPtr<IDataObject> ExternalFileItem::CreateDataObject() { return nullptr; }

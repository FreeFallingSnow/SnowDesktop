#pragma once
// Item::Draw implementations — only included by app_oo.h (OO target).
// These use SnowDesktopAppOO's rendering helpers which are now public.
// Shared item.cpp keeps stub implementations for the old target.

inline void DesktopIcon_DrawImpl(DesktopIcon* self, SnowDesktopAppOO* app, ID2D1DeviceContext* context, RECT rect, int state)
{
    DesktopItem* item = self->GetDesktopItem();
    if (!item || !context || !app) return;
    if (item->bounds.left >= item->bounds.right || item->bounds.top >= item->bounds.bottom) return;

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

    if (item->iconBitmap)
    {
        ID2D1Bitmap1* bmp = app->GetOrCreateD2DBitmap(item->iconBitmap);
        if (bmp)
        {
            D2D1_RECT_F dst = D2D1::RectF(
                static_cast<float>(iconRect.left), static_cast<float>(iconRect.top),
                static_cast<float>(iconRect.right), static_cast<float>(iconRect.bottom));
            context->DrawBitmap(bmp, dst, opacity, D2D1_INTERPOLATION_MODE_LINEAR);
        }
    }

    app->DrawItemText(context, rect, item->name, selected, opacity);
}

inline void FolderEntryIcon_DrawImpl(FolderEntryIcon* self, SnowDesktopAppOO* app, ID2D1DeviceContext* context, RECT rect, int state)
{
    FolderEntry* entry = self->GetFolderEntry();
    if (!entry || !context || !app) return;
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

    if (entry->iconBitmap)
    {
        ID2D1Bitmap1* bmp = app->GetOrCreateD2DBitmap(entry->iconBitmap);
        if (bmp)
        {
            D2D1_RECT_F dst = D2D1::RectF(
                static_cast<float>(iconRect.left), static_cast<float>(iconRect.top),
                static_cast<float>(iconRect.right), static_cast<float>(iconRect.bottom));
            context->DrawBitmap(bmp, dst, opacity, D2D1_INTERPOLATION_MODE_LINEAR);
        }
    }

    app->DrawItemText(context, rect, entry->name, selected, opacity);
}

#include "app/app.h"

// Shared nonactivating popup and typography; drag feedback retains its light
// surface, modifier emphasis and visibility during capture and OLE.
void DesktopApp::InvalidateDragHintRaster()
{
    dragTooltip_.Invalidate();
}

void DesktopApp::HideDragHintWindow()
{
    dragTooltip_.Hide();
}

void DesktopApp::DestroyDragHintWindow()
{
    dragTooltip_.Close();
    hintHwnd_ = nullptr;
}

void DesktopApp::ShowDragHintWindow(POINT clientPoint, const std::wstring& text, bool modifierActive)
{
    if (!hwnd_ || !ClientToScreen(hwnd_, &clientPoint))
    {
        HideDragHintWindow();
        return;
    }
    ShowDragHintWindowScreen(clientPoint, text, modifierActive);
}

void DesktopApp::ShowDragHintWindowScreen(POINT screenPoint, const std::wstring& text, bool modifierActive)
{
    if (text.empty() || !hwnd_ || !dcompDevice_ || !dwriteFactory_)
    {
        HideDragHintWindow();
        return;
    }
    auto appearance = PersonalizationSettings::LightPreset();
    appearance.glassEnabled = appearance.acrylicEnabled = false;
    appearance.cornerRadius = 8.f;
    appearance.widgetAlpha = 1.f;
    appearance.widgetBgR = modifierActive ? 1.f : 252.f / 255.f;
    appearance.widgetBgG = modifierActive ? 244.f / 255.f : 253.f / 255.f;
    appearance.widgetBgB = modifierActive ? 194.f / 255.f : 1.f;
    appearance.widgetBorderR = modifierActive ? 224.f / 255.f : 205.f / 255.f;
    appearance.widgetBorderG = modifierActive ? 166.f / 255.f : 211.f / 255.f;
    appearance.widgetBorderB = modifierActive ? 35.f / 255.f : 220.f / 255.f;
    appearance.widgetBorderAlpha = 1.f;
    appearance.widgetBorderWidth = 1.f;
    const HWND owner = floatingDockHostActive_ && floatingDockHwnd_ && IsWindow(floatingDockHwnd_)
        ? floatingDockHwnd_ : hwnd_;
    dragTooltip_.SetDragFeedback(true);
    dragTooltip_.Configure(owner, dcompDevice_.Get(), dwriteFactory_.Get(), appearance);
    dragTooltip_.SetTarget("drag", text, {screenPoint.x, screenPoint.y, screenPoint.x + 1, screenPoint.y + 1},
        snowdesktop::NativeTooltipPlacement::Below, true);
    // Existing OLE hit testing and preview ordering still refer to this HWND.
    hintHwnd_ = dragTooltip_.Window();
}

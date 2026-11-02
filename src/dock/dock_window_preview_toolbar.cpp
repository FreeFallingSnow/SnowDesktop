#include "dock_window_preview.h"
#include <commctrl.h>
#include <algorithm>

namespace
{
constexpr int kToolbarHeight = 36;
std::array<RECT, snowdesktop::thumbnail_toolbar::kMaximumButtons> ButtonRects(
    const RECT& card, UINT dpi, const snowdesktop::thumbnail_toolbar::Snapshot& toolbar)
{
    std::array<RECT, snowdesktop::thumbnail_toolbar::kMaximumButtons> rects{};
    const auto count = snowdesktop::thumbnail_toolbar::VisibleCount(toolbar);
    if (!count) return rects;
    const int inset = std::max(1, MulDiv(5, static_cast<int>(dpi), 96));
    const int width = std::max(1, std::min(MulDiv(28, static_cast<int>(dpi), 96),
        static_cast<int>(card.right - card.left - inset * 2) / static_cast<int>(count)));
    const int height = MulDiv(28, static_cast<int>(dpi), 96);
    int left = card.left + (card.right - card.left - static_cast<int>(count) * width) / 2;
    const int top = card.bottom - MulDiv(kToolbarHeight, static_cast<int>(dpi), 96) +
        MulDiv(3, static_cast<int>(dpi), 96);
    for (DWORD index = 0; index < toolbar.count; ++index)
    {
        if (!snowdesktop::thumbnail_toolbar::Visible(toolbar.buttons[index].native)) continue;
        rects[index] = {left, top, left + width, std::min<LONG>(card.bottom, top + height)};
        left += width;
    }
    return rects;
}
void DrawButtonIcon(HDC dc, const RECT& bounds, UINT dpi, const snowdesktop::thumbnail_toolbar::Button& button)
{
    if (!button.width || !button.height || button.width > snowdesktop::thumbnail_toolbar::kIconExtent ||
        button.height > snowdesktop::thumbnail_toolbar::kIconExtent) return;
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(button.width);
    info.bmiHeader.biHeight = -static_cast<LONG>(button.height);
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP color = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HBITMAP mask = CreateBitmap(static_cast<int>(button.width), static_cast<int>(button.height), 1, 1, nullptr);
    if (color && mask && pixels)
    {
        auto* output = static_cast<DWORD*>(pixels);
        const auto count = static_cast<size_t>(button.width) * button.height;
        std::copy_n(button.pixels.begin(), count, output);
        if (button.native.flags & THBF_DISABLED)
            for (size_t index = 0; index < count; ++index)
            {
                const DWORD pixel = output[index];
                const DWORD grey = (((pixel >> 16) & 255) * 30 + ((pixel >> 8) & 255) * 59 + (pixel & 255) * 11) / 200;
                output[index] = ((pixel >> 25) << 24) | (grey << 16) | (grey << 8) | grey;
            }
        ICONINFO iconInfo{TRUE, 0, 0, mask, color};
        // An all-zero alpha image is a transparent placeholder. Creating an
        // HICON would let User32 treat it as a legacy opaque bitmap instead.
        const HICON icon = std::any_of(output, output + count, [](DWORD pixel) { return (pixel >> 24) != 0; })
            ? CreateIconIndirect(&iconInfo) : nullptr;
        if (icon)
        {
            const int size = std::max(1, std::min({MulDiv(18, static_cast<int>(dpi), 96),
                static_cast<int>(bounds.right - bounds.left), static_cast<int>(bounds.bottom - bounds.top)}));
            const int x = bounds.left + (bounds.right - bounds.left - size) / 2;
            const int y = bounds.top + (bounds.bottom - bounds.top - size) / 2;
            DrawIconEx(dc, x, y, icon, size, size, 0, nullptr, DI_NORMAL);
            DestroyIcon(icon);
        }
    }
    if (color) DeleteObject(color);
    if (mask) DeleteObject(mask);
}
}
void DockWindowPreview::StartToolbarTracking()
{
    namespace native = snowdesktop::thumbnail_toolbar;
    ClearToolbarTooltips();
    std::vector<native::Snapshot> next(items_.size());
    std::vector<HWND> targets;
    for (size_t index = 0; index < items_.size(); ++index)
    {
        targets.push_back(items_[index].window);
        for (const auto& toolbar : toolbars_)
            if (toolbar.target.window == reinterpret_cast<std::uintptr_t>(items_[index].window) &&
                toolbar.target == native::Capture(items_[index].window)) { next[index] = toolbar; break; }
    }
    toolbars_ = std::move(next);
    if (!toolbarClient_) toolbarClient_ = std::make_unique<native::Client>();
    toolbarClient_->SetTargets(targets);
    SetTimer(hwnd_, kToolbarTimerId, 100, nullptr);
}
int DockWindowPreview::ToolbarRegionAtPoint(POINT point) const
{
    const int index = CardIndexAtPoint(point);
    if (index < 0 || static_cast<size_t>(index) >= toolbars_.size() ||
        !snowdesktop::thumbnail_toolbar::VisibleCount(toolbars_[index])) return -1;
    return point.y >= cardRects_[index].bottom - MulDiv(kToolbarHeight, static_cast<int>(dpi_), 96) ? index : -1;
}
std::pair<int, int> DockWindowPreview::ToolbarButtonAtPoint(POINT point) const
{
    const int index = ToolbarRegionAtPoint(point);
    if (index < 0) return {-1, -1};
    const auto rects = ButtonRects(cardRects_[index], dpi_, toolbars_[index]);
    for (DWORD button = 0; button < toolbars_[index].count; ++button)
        if (PtInRect(&rects[button], point)) return {index, static_cast<int>(button)};
    return {index, -1};
}
void DockWindowPreview::RefreshToolbar()
{
    if (!toolbarClient_ || !IsVisible()) return;
    namespace native = snowdesktop::thumbnail_toolbar;
    auto update = toolbarClient_->Take();
    for (const auto& target : update.dismiss)
        if (IsShowingWindow(reinterpret_cast<HWND>(target.window)) && target == native::Capture(reinterpret_cast<HWND>(target.window)))
        { Hide(); return; }
    std::vector<native::Snapshot> next(items_.size());
    for (size_t index = 0; index < items_.size(); ++index)
        for (const auto& snapshot : update.snapshots)
            if (snapshot.target.window == reinterpret_cast<std::uintptr_t>(items_[index].window))
            { next[index] = snapshot; break; }
    if (next == toolbars_) return;
    bool reflow = next.size() != toolbars_.size();
    for (size_t index = 0; !reflow && index < next.size(); ++index)
        reflow = native::VisibleCount(next[index]) != native::VisibleCount(toolbars_[index]);
    ClearToolbarTooltips(); // Remove pointers before replacing their backing strings.
    toolbars_ = std::move(next);
    if (reflow)
    {
        const auto items = items_;
        Show(items, anchorScreen_, dockPosition_, lightTheme_, peekOwner_);
    }
    else { UpdateToolbarTooltips(); InvalidatePopups(); }
}
void DockWindowPreview::PaintToolbar(HDC dc, size_t index, const RECT& client)
{
    if (index >= toolbars_.size()) return;
    namespace native = snowdesktop::thumbnail_toolbar;
    const auto rects = ButtonRects(client, dpi_, toolbars_[index]);
    for (DWORD button = 0; button < toolbars_[index].count; ++button)
    {
        const auto& value = toolbars_[index].buttons[button];
        if (!native::Visible(value.native)) continue;
        const auto& rect = rects[button];
        if (hoveredToolbar_ == std::pair{static_cast<int>(index), static_cast<int>(button)} &&
            native::Interactive(value.native) && !(value.native.flags & THBF_NOBACKGROUND))
        {
            HBRUSH brush = CreateSolidBrush(lightTheme_ ? RGB(197, 216, 241) : RGB(77, 99, 128));
            HGDIOBJ oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, MulDiv(5, static_cast<int>(dpi_), 96), MulDiv(5, static_cast<int>(dpi_), 96));
            SelectObject(dc, oldPen); SelectObject(dc, oldBrush); DeleteObject(brush);
        }
        DrawButtonIcon(dc, rect, dpi_, value);
    }
}
void DockWindowPreview::ClearToolbarTooltips()
{
    for (HWND tooltip : toolbarTooltips_) if (tooltip) DestroyWindow(tooltip);
    toolbarTooltips_.clear();
}
void DockWindowPreview::UpdateToolbarTooltips()
{
    ClearToolbarTooltips();
    toolbarTooltips_.resize(windows_.size(), nullptr);
    for (size_t index = 0; index < windows_.size() && index < toolbars_.size(); ++index)
    {
        if (!snowdesktop::thumbnail_toolbar::VisibleCount(toolbars_[index])) continue;
        const HWND tooltip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            windows_[index], nullptr, instance_, nullptr);
        if (!tooltip) continue;
        toolbarTooltips_[index] = tooltip;
        SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, MulDiv(320, static_cast<int>(dpi_), 96));
        RECT client{}; GetClientRect(windows_[index], &client);
        const auto rects = ButtonRects(client, dpi_, toolbars_[index]);
        for (DWORD button = 0; button < toolbars_[index].count; ++button)
        {
            auto& value = toolbars_[index].buttons[button].native;
            if (!snowdesktop::thumbnail_toolbar::Visible(value) || !value.tip[0]) continue;
            TOOLINFOW tool{sizeof(tool)};
            tool.uFlags = TTF_SUBCLASS | TTF_TRANSPARENT; tool.hwnd = windows_[index];
            tool.uId = button + 1; tool.rect = rects[button]; tool.lpszText = value.tip;
            SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        }
    }
}

#include "dock_window_preview.h"
#include "dock_window_preview_layout.h"
#include <commctrl.h>
#include <algorithm>

namespace
{
std::array<RECT, snowdesktop::thumbnail_toolbar::kMaximumButtons> ButtonRects(
    const RECT& card, UINT dpi, const snowdesktop::thumbnail_toolbar::Snapshot& toolbar)
{
    std::array<RECT, snowdesktop::thumbnail_toolbar::kMaximumButtons> rects{};
    const auto count = snowdesktop::thumbnail_toolbar::VisibleCount(toolbar);
    if (!count) return rects;
    const int inset = std::max(1, MulDiv(5, static_cast<int>(dpi), 96));
    const int gap = MulDiv(dock_preview_toolbar::kButtonGap, static_cast<int>(dpi), 96);
    const int gaps = static_cast<int>(count - 1) * gap;
    const int width = std::max(1, std::min(MulDiv(dock_preview_toolbar::kButtonSize, static_cast<int>(dpi), 96),
        static_cast<int>(card.right - card.left - inset * 2 - gaps) / static_cast<int>(count)));
    const int height = MulDiv(dock_preview_toolbar::kButtonSize, static_cast<int>(dpi), 96);
    int left = card.left + (card.right - card.left - static_cast<int>(count) * width - gaps) / 2;
    const int top = card.bottom - MulDiv(dock_preview_toolbar::kBottomInset, static_cast<int>(dpi), 96) - height;
    for (DWORD index = 0; index < toolbar.count; ++index)
    {
        if (!snowdesktop::thumbnail_toolbar::Visible(toolbar.buttons[index].native)) continue;
        rects[index] = {left, top, left + width, std::min<LONG>(card.bottom, top + height)};
        left += width + gap;
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
        RECT ink{static_cast<LONG>(button.width), static_cast<LONG>(button.height), 0, 0};
        for (LONG y = 0; y < static_cast<LONG>(button.height); ++y)
            for (LONG x = 0; x < static_cast<LONG>(button.width); ++x)
                if (output[static_cast<size_t>(y) * button.width + x] >> 24)
                {
                    ink.left = std::min(ink.left, x); ink.right = std::max(ink.right, x + 1);
                    ink.top = std::min(ink.top, y); ink.bottom = std::max(ink.bottom, y + 1);
                }
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
            const int size = std::max(1, std::min({MulDiv(dock_preview_toolbar::kIconSize, static_cast<int>(dpi), 96),
                static_cast<int>(bounds.right - bounds.left), static_cast<int>(bounds.bottom - bounds.top)}));
            // Application artwork can have uneven transparent padding. Keep
            // its original scale, but center the visible ink rather than the
            // full image canvas; disabled rendering uses the same alignment.
            const int x = (bounds.left + bounds.right - MulDiv(ink.left + ink.right, size, static_cast<int>(button.width))) / 2;
            const int y = (bounds.top + bounds.bottom - MulDiv(ink.top + ink.bottom, size, static_cast<int>(button.height))) / 2;
            DrawIconEx(dc, x, y, icon, size, size, 0, nullptr, DI_NORMAL);
            DestroyIcon(icon);
        }
    }
    if (color) DeleteObject(color);
    if (mask) DeleteObject(mask);
}
}
void DockWindowPreview::PrepareToolbars(const std::vector<DockWindowPreviewItem>& items)
{
    if (!toolbarClient_) return;
    std::vector<HWND> targets;
    for (const auto& item : items) targets.push_back(item.window);
    // Switching groups can prefetch the new group while the old preview is
    // still visible. Keep its updates and commands alive until Show switches.
    if (IsVisible())
        for (const auto& item : items_)
            if (std::find(targets.begin(), targets.end(), item.window) == targets.end()) targets.push_back(item.window);
    toolbarClient_->SetTargets(targets);
}
void DockWindowPreview::StartToolbarTracking()
{
    namespace native = snowdesktop::thumbnail_toolbar;
    ClearToolbarTooltips();
    std::vector<native::Snapshot> next(items_.size());
    PrepareToolbars(items_);
    if (toolbarClient_) toolbarClient_->SetNotification(hwnd_, kToolbarUpdateMessage);
    const auto update = toolbarClient_ ? toolbarClient_->Take() : native::Update{};
    for (size_t index = 0; index < items_.size(); ++index)
    {
        const auto target = native::Capture(items_[index].window);
        for (const auto& toolbar : toolbars_)
            if (toolbar.target == target) { next[index] = toolbar; break; }
        // Consume the hover-prefetched snapshot before the first Layout,
        // including a fresh empty toolbar replacing a previously cached one.
        for (const auto& toolbar : update.snapshots)
            if (toolbar.target == target) { next[index] = toolbar; break; }
    }
    toolbars_ = std::move(next);
}
int DockWindowPreview::ToolbarRegionAtPoint(POINT point) const
{
    const int index = CardIndexAtPoint(point);
    if (index < 0 || static_cast<size_t>(index) >= toolbars_.size() ||
        !snowdesktop::thumbnail_toolbar::VisibleCount(toolbars_[index])) return -1;
    return point.y >= cardRects_[index].bottom - MulDiv(dock_preview_toolbar::kBarRegionHeight, static_cast<int>(dpi_), 96) ? index : -1;
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
    if (!native::VisibleCount(toolbars_[index])) return;
    const int inset = std::max(1, MulDiv(5, static_cast<int>(dpi_), 96));
    const RECT bar{client.left + inset,
        std::max(client.top, client.bottom - MulDiv(dock_preview_toolbar::kBarRegionHeight, static_cast<int>(dpi_), 96)),
        client.right - inset, client.bottom - inset};
    HBRUSH backgroundBrush = CreateSolidBrush(lightTheme_ ? RGB(240, 240, 242) : RGB(37, 39, 43));
    FillRect(dc, &bar, backgroundBrush);
    DeleteObject(backgroundBrush);
    const auto rects = ButtonRects(client, dpi_, toolbars_[index]);
    for (DWORD button = 0; button < toolbars_[index].count; ++button)
    {
        const auto& value = toolbars_[index].buttons[button];
        if (!native::Visible(value.native)) continue;
        const auto& rect = rects[button];
        if (!(value.native.flags & THBF_NOBACKGROUND))
        {
            const bool hovered = hoveredToolbar_ == std::pair{static_cast<int>(index), static_cast<int>(button)} &&
                native::Interactive(value.native);
            const bool pressed = hovered && pressedToolbar_ && pressedToolbar_->target == toolbars_[index].target &&
                pressedToolbar_->identity == toolbars_[index].identity && pressedToolbar_->button == value.native.id;
            const COLORREF background = lightTheme_
                ? (pressed ? RGB(208, 209, 211) : hovered ? RGB(246, 246, 246) : RGB(231, 232, 234))
                : (pressed ? RGB(57, 59, 63) : hovered ? RGB(83, 85, 89) : RGB(67, 69, 73));
            HBRUSH brush = CreateSolidBrush(background);
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

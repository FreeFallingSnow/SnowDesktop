#include "dock_window_preview.h"
#include "dock_window_preview_layout.h"
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <new>

namespace
{
constexpr UINT_PTR kToolbarOwnerSubclass = 0x53445442;
constexpr UINT_PTR kToolbarOverlaySubclass = 0x53445443;
LRESULT CALLBACK ToolbarOwnerProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
LRESULT CALLBACK ToolbarOverlayProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

// The owned, input-transparent surface is composed above the live DWM
// thumbnail. Its lifetime follows the card through the owner's subclass.
struct ToolbarLayer
{
    HWND owner = nullptr, window = nullptr;
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    DWORD* pixels = nullptr;
    SIZE size{};
    bool enabled = false, excludedFromPeek = false;
    explicit ToolbarLayer(HWND card) : owner(card) {}
    ~ToolbarLayer()
    {
        if (window) DestroyWindow(window);
        if (dc && previous) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
    bool Resize(SIZE requested)
    {
        if (requested.cx <= 0 || requested.cy <= 0) return false;
        if (bitmap && size.cx == requested.cx && size.cy == requested.cy) return true;
        if (!dc) dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = requested.cx; info.bmiHeader.biHeight = -requested.cy;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        const HBITMAP next = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!next || !bits) { if (next) DeleteObject(next); return false; }
        if (previous) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        bitmap = next; previous = SelectObject(dc, bitmap);
        if (!previous || previous == HGDI_ERROR)
        { DeleteObject(bitmap); bitmap = nullptr; previous = nullptr; pixels = nullptr; return false; }
        pixels = static_cast<DWORD*>(bits); size = requested;
        return true;
    }
    void Sync()
    {
        if (!window) return;
        RECT client{};
        if (!enabled || !IsWindowVisible(owner) || !GetClientRect(owner, &client) || client.right != size.cx)
        { ShowWindow(window, SW_HIDE); return; }
        POINT position{0, client.bottom - size.cy}; ClientToScreen(owner, &position);
        SetWindowPos(window, nullptr, position.x, position.y, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    bool Present()
    {
        if (!window)
        {
            window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                L"STATIC", L"SnowDesktop Thumbnail Controls", WS_POPUP, 0, 0, 1, 1,
                owner, nullptr, reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE)), nullptr);
            if (!window) return false;
            if (!SetWindowSubclass(window, ToolbarOverlayProc, kToolbarOverlaySubclass, reinterpret_cast<DWORD_PTR>(this)))
            { DestroyWindow(window); window = nullptr; return false; }
            const BOOL excluded = TRUE;
            excludedFromPeek = SUCCEEDED(DwmSetWindowAttribute(window, DWMWA_EXCLUDED_FROM_PEEK, &excluded, sizeof(excluded)));
        }
        RECT client{}; GetClientRect(owner, &client);
        POINT position{0, client.bottom - size.cy}; ClientToScreen(owner, &position);
        POINT origin{}; BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        GdiFlush();
        if (!UpdateLayeredWindow(window, nullptr, &position, &size, dc, &origin, 0, &blend, ULW_ALPHA))
        { enabled = false; ShowWindow(window, SW_HIDE); return false; }
        Sync();
        return true;
    }
};
LRESULT CALLBACK ToolbarOverlayProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data)
{
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_NCDESTROY)
    {
        auto* layer = reinterpret_cast<ToolbarLayer*>(data);
        if (layer->window == window) layer->window = nullptr;
        RemoveWindowSubclass(window, ToolbarOverlayProc, kToolbarOverlaySubclass);
    }
    return DefSubclassProc(window, message, wParam, lParam);
}
LRESULT CALLBACK ToolbarOwnerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data)
{
    if (message == WM_NCDESTROY)
    {
        RemoveWindowSubclass(window, ToolbarOwnerProc, kToolbarOwnerSubclass);
        delete reinterpret_cast<ToolbarLayer*>(data);
        return DefSubclassProc(window, message, wParam, lParam);
    }
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    DWORD_PTR current = 0;
    if ((message == WM_WINDOWPOSCHANGED || message == WM_SHOWWINDOW) &&
        GetWindowSubclass(window, ToolbarOwnerProc, kToolbarOwnerSubclass, &current) && current == data)
        reinterpret_cast<ToolbarLayer*>(data)->Sync();
    return result;
}
ToolbarLayer* FindToolbarLayer(HWND owner, bool create = false)
{
    DWORD_PTR data = 0;
    if (GetWindowSubclass(owner, ToolbarOwnerProc, kToolbarOwnerSubclass, &data))
        return reinterpret_cast<ToolbarLayer*>(data);
    if (!create) return nullptr;
    std::unique_ptr<ToolbarLayer> layer(new(std::nothrow) ToolbarLayer(owner));
    if (!layer || !SetWindowSubclass(owner, ToolbarOwnerProc, kToolbarOwnerSubclass, reinterpret_cast<DWORD_PTR>(layer.get()))) return nullptr;
    return layer.release();
}
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
void RenderToolbarImage(HDC dc, DWORD* pixels, SIZE size, UINT dpi,
    const snowdesktop::thumbnail_toolbar::Snapshot& toolbar, bool lightTheme, int hoveredButton, int pressedButton)
{
    namespace native = snowdesktop::thumbnail_toolbar;
    GdiFlush();
    std::fill_n(pixels, static_cast<size_t>(size.cx) * size.cy, 0u);
    const RECT client{0, 0, size.cx, size.cy};
    const auto rects = ButtonRects(client, dpi, toolbar);
    for (DWORD button = 0; button < toolbar.count; ++button)
    {
        const auto& value = toolbar.buttons[button];
        if (!native::Visible(value.native)) continue;
        const RECT& rect = rects[button];
        if (!(value.native.flags & THBF_NOBACKGROUND))
        {
            const bool hot = static_cast<int>(button) == hoveredButton && native::Interactive(value.native);
            const bool pressed = hot && static_cast<int>(button) == pressedButton;
            const unsigned opacity = (value.native.flags & THBF_DISABLED) ? 96u : pressed ? 194u : hot ? 164u : 128u;
            const unsigned color = lightTheme ? 255u : 24u;
            const float radius = static_cast<float>(std::max(1, MulDiv(4, static_cast<int>(dpi), 96)));
            for (LONG y = std::max(0L, rect.top); y < std::min(size.cy, rect.bottom); ++y)
                for (LONG x = std::max(0L, rect.left); x < std::min(size.cx, rect.right); ++x)
                {
                    const float px = static_cast<float>(x) + .5f, py = static_cast<float>(y) + .5f;
                    const float dx = std::max({static_cast<float>(rect.left) + radius - px, 0.f, px - static_cast<float>(rect.right) + radius});
                    const float dy = std::max({static_cast<float>(rect.top) + radius - py, 0.f, py - static_cast<float>(rect.bottom) + radius});
                    const float coverage = std::clamp(radius + .5f - std::hypot(dx, dy), 0.f, 1.f);
                    const DWORD alpha = static_cast<DWORD>(std::lround(static_cast<float>(opacity) * coverage));
                    const DWORD channel = color * alpha / 255;
                    pixels[static_cast<size_t>(y) * size.cx + x] = (alpha << 24) | (channel << 16) | (channel << 8) | channel;
                }
        }
        DrawButtonIcon(dc, rect, dpi, value);
    }
    GdiFlush();
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
    for (const HWND window : windows_)
        if (auto* layer = FindToolbarLayer(window)) { layer->enabled = false; layer->Sync(); }
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
void DockWindowPreview::PaintToolbar(HDC, size_t index, const RECT& client)
{
    if (index >= windows_.size()) return;
    namespace native = snowdesktop::thumbnail_toolbar;
    const bool visible = index < toolbars_.size() && native::VisibleCount(toolbars_[index]);
    auto* layer = FindToolbarLayer(windows_[index], visible);
    if (!layer) return;
    layer->enabled = visible;
    if (!visible) { layer->Sync(); return; }
    const SIZE size{client.right - client.left,
        std::min(client.bottom - client.top, static_cast<LONG>(MulDiv(dock_preview_toolbar::kBarRegionHeight, static_cast<int>(dpi_), 96)))};
    if (!layer->Resize(size))
    {
        layer->enabled = false; layer->Sync(); return;
    }
    const int hovered = hoveredToolbar_.first == static_cast<int>(index) ? hoveredToolbar_.second : -1;
    const bool pressed = pressedToolbar_ && pressedToolbar_->target == toolbars_[index].target &&
        pressedToolbar_->identity == toolbars_[index].identity && hovered >= 0 &&
        static_cast<DWORD>(hovered) < toolbars_[index].count && toolbars_[index].buttons[hovered].native.id == pressedToolbar_->button;
    RenderToolbarImage(layer->dc, layer->pixels, size, dpi_, toolbars_[index], lightTheme_, hovered, pressed ? hovered : -1);
    layer->Present();
    if (!layer->excludedFromPeek) peekSurfacesReady_ = false;
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

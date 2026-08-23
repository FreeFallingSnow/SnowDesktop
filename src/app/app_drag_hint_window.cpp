#include "app.h"
#include "../drag_hint_rules.h"

#include <algorithm>
#include <cmath>
#include <vector>

// Drag-hint popup window lifecycle and rendering.

namespace
{
UINT ResolveDragHintDpi(HMONITOR monitor, HWND fallbackWindow)
{
    UINT dpiX = USER_DEFAULT_SCREEN_DPI;
    UINT dpiY = USER_DEFAULT_SCREEN_DPI;
    if (monitor && SUCCEEDED(GetDpiForMonitor(
            monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) &&
        dpiY > 0)
    {
        return dpiY;
    }
    if (fallbackWindow && IsWindow(fallbackWindow))
    {
        const UINT windowDpi = GetDpiForWindow(fallbackWindow);
        if (windowDpi > 0)
            return windowDpi;
    }
    return USER_DEFAULT_SCREEN_DPI;
}

int ScaleDragHintMetric(int value, UINT dpi)
{
    return std::max(1, MulDiv(
        value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI));
}

POINT ResolveDragHintWindowPosition(
    POINT anchor,
    SIZE windowSize,
    RECT workArea,
    UINT dpi)
{
    const auto resolved =
        snowdesktop::drag_hint_rules::ResolveWindowPosition(
            {anchor.x, anchor.y},
            {windowSize.cx, windowSize.cy},
            {workArea.left, workArea.top,
                workArea.right, workArea.bottom},
            ScaleDragHintMetric(48, dpi),
            ScaleDragHintMetric(22, dpi),
            ScaleDragHintMetric(8, dpi));
    return {resolved.x, resolved.y};
}

bool IsValidPreviousGdiObject(HGDIOBJ object)
{
    return object && object != HGDI_ERROR;
}

float RoundedRectSignedDistance(
    float x, float y,
    float left, float top,
    float right, float bottom,
    float radius)
{
    const float centerX = (left + right) * 0.5f;
    const float centerY = (top + bottom) * 0.5f;
    const float halfWidth = (right - left) * 0.5f;
    const float halfHeight = (bottom - top) * 0.5f;
    const float resolvedRadius = std::clamp(
        radius, 0.0f, std::min(halfWidth, halfHeight));
    const float qx = std::fabs(x - centerX) -
        (halfWidth - resolvedRadius);
    const float qy = std::fabs(y - centerY) -
        (halfHeight - resolvedRadius);
    return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) +
        std::min(std::max(qx, qy), 0.0f) - resolvedRadius;
}

std::uint32_t PackPremultipliedArgb(
    float alpha, float red, float green, float blue)
{
    const auto channel = [](float value) {
        return static_cast<std::uint32_t>(std::clamp(
            std::lround(value), 0L, 255L));
    };
    const float normalizedAlpha = std::clamp(alpha, 0.0f, 255.0f);
    const float multiplier = normalizedAlpha / 255.0f;
    return (channel(normalizedAlpha) << 24) |
        (channel(red * multiplier) << 16) |
        (channel(green * multiplier) << 8) |
        channel(blue * multiplier);
}

std::uint32_t SourceOverPremultiplied(
    std::uint32_t source, std::uint32_t destination)
{
    const std::uint32_t sourceAlpha = source >> 24;
    const std::uint32_t inverseAlpha = 255u - sourceAlpha;
    const auto blendChannel = [inverseAlpha](
            std::uint32_t sourceChannel,
            std::uint32_t destinationChannel) {
        return std::min(255u, sourceChannel +
            (destinationChannel * inverseAlpha + 127u) / 255u);
    };
    return (blendChannel(sourceAlpha, destination >> 24) << 24) |
        (blendChannel((source >> 16) & 0xffu,
            (destination >> 16) & 0xffu) << 16) |
        (blendChannel((source >> 8) & 0xffu,
            (destination >> 8) & 0xffu) << 8) |
        blendChannel(source & 0xffu, destination & 0xffu);
}
}

void DesktopApp::InvalidateDragHintRaster()
{
    hintRasterValid_ = false;
    hintTextCache_.clear();
    hintRasterSize_ = {};
    hintRasterDpi_ = 0;
}

bool DesktopApp::EnsureDragHintWindow()
{
    if (hintHwnd_ && IsWindow(hintHwnd_))
        return true;
    hintHwnd_ = nullptr;
    InvalidateDragHintRaster();
    const HWND owner =
        floatingDockVisible_ && floatingDockHwnd_ &&
            IsWindow(floatingDockHwnd_)
        ? floatingDockHwnd_ : nullptr;
    hintHwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
            WS_EX_TRANSPARENT | WS_EX_TOPMOST,
        kHintWindowClassName, L"", WS_POPUP,
        0, 0, 1, 1, owner, nullptr, instance_, nullptr);
    return hintHwnd_ != nullptr;
}

void DesktopApp::SyncDragHintWindowOwner()
{
    if (!hintHwnd_ || !IsWindow(hintHwnd_))
        return;
    const HWND requestedOwner =
        floatingDockVisible_ && floatingDockHwnd_ &&
            IsWindow(floatingDockHwnd_)
        ? floatingDockHwnd_ : nullptr;
    const HWND currentOwner = reinterpret_cast<HWND>(
        GetWindowLongPtrW(hintHwnd_, GWLP_HWNDPARENT));
    if (currentOwner != requestedOwner)
    {
        SetWindowLongPtrW(
            hintHwnd_, GWLP_HWNDPARENT,
            reinterpret_cast<LONG_PTR>(requestedOwner));
    }
}

void DesktopApp::HideDragHintWindow()
{
    // Keep the last successfully submitted layered bitmap. OLE can produce
    // short Leave/Enter transitions; re-entry with the same key can show and
    // move the cached raster without rebuilding any GDI resources.
    if (hintHwnd_ && IsWindow(hintHwnd_) &&
        IsWindowVisible(hintHwnd_))
    {
        ShowWindow(hintHwnd_, SW_HIDE);
    }
}

void DesktopApp::DestroyDragHintWindow()
{
    if (hintHwnd_ && IsWindow(hintHwnd_))
        DestroyWindow(hintHwnd_);
    hintHwnd_ = nullptr;
    InvalidateDragHintRaster();
}

void DesktopApp::ShowDragHintWindow(
    POINT clientPoint,
    const std::wstring& text)
{
    if (text.empty())
    {
        HideDragHintWindow();
        return;
    }
    if (!hwnd_ || !IsWindow(hwnd_))
    {
        HideDragHintWindow();
        return;
    }
    POINT screenPoint = clientPoint;
    if (!ClientToScreen(hwnd_, &screenPoint))
    {
        HideDragHintWindow();
        return;
    }
    ShowDragHintWindowScreen(screenPoint, text);
}

void DesktopApp::ShowDragHintWindowScreen(
    POINT screenPoint,
    const std::wstring& text)
{
    if (text.empty() || !EnsureDragHintWindow())
    {
        HideDragHintWindow();
        return;
    }
    SyncDragHintWindowOwner();

    const HMONITOR monitor = MonitorFromPoint(
        screenPoint, MONITOR_DEFAULTTONEAREST);
    const UINT dpi = ResolveDragHintDpi(monitor, hwnd_);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo))
    {
        monitorInfo.rcWork = {
            GetSystemMetrics(SM_XVIRTUALSCREEN),
            GetSystemMetrics(SM_YVIRTUALSCREEN),
            GetSystemMetrics(SM_XVIRTUALSCREEN) +
                GetSystemMetrics(SM_CXVIRTUALSCREEN),
            GetSystemMetrics(SM_YVIRTUALSCREEN) +
                GetSystemMetrics(SM_CYVIRTUALSCREEN),
        };
    }

    if (snowdesktop::drag_hint_rules::ShouldReuseRaster(
            hintRasterValid_, text == hintTextCache_,
            hintRasterDpi_, dpi))
    {
        const POINT windowPos = ResolveDragHintWindowPosition(
            screenPoint, hintRasterSize_, monitorInfo.rcWork, dpi);
        // The floating Dock can re-enter the topmost band while presenting a
        // composition frame. Raise the hint on every cached move so it stays
        // above both the fixed and floating Dock surfaces.
        SetWindowPos(hintHwnd_, HWND_TOPMOST,
            windowPos.x, windowPos.y, 0, 0,
            SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                SWP_NOSENDCHANGING | SWP_SHOWWINDOW);
        return;
    }

    HDC screenDc = GetDC(nullptr);
    if (!screenDc)
    {
        InvalidateDragHintRaster();
        HideDragHintWindow();
        return;
    }
    HFONT font = CreateFontW(
        -ScaleDragHintMetric(13, dpi), 0, 0, 0,
        FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!font)
    {
        ReleaseDC(nullptr, screenDc);
        InvalidateDragHintRaster();
        HideDragHintWindow();
        return;
    }

    const HGDIOBJ oldScreenFont = SelectObject(screenDc, font);
    SIZE textSize{};
    const bool measured =
        IsValidPreviousGdiObject(oldScreenFont) &&
        GetTextExtentPoint32W(
            screenDc, text.c_str(),
            static_cast<int>(text.size()), &textSize) != FALSE;
    if (IsValidPreviousGdiObject(oldScreenFont))
        SelectObject(screenDc, oldScreenFont);
    if (!measured)
    {
        DeleteObject(font);
        ReleaseDC(nullptr, screenDc);
        InvalidateDragHintRaster();
        HideDragHintWindow();
        return;
    }

    const int panelWidth = std::clamp(
        static_cast<int>(textSize.cx) + ScaleDragHintMetric(24, dpi),
        ScaleDragHintMetric(130, dpi),
        ScaleDragHintMetric(520, dpi));
    const int panelHeight = std::clamp(
        static_cast<int>(textSize.cy) + ScaleDragHintMetric(14, dpi),
        ScaleDragHintMetric(32, dpi),
        ScaleDragHintMetric(46, dpi));
    const int shadowInset = ScaleDragHintMetric(5, dpi);
    const int width = panelWidth + shadowInset * 2;
    const int height = panelHeight + shadowInset * 2;
    SIZE windowSize{width, height};
    POINT windowPos = ResolveDragHintWindowPosition(
        screenPoint, windowSize, monitorInfo.rcWork, dpi);

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(
        screenDc, &bitmapInfo, DIB_RGB_COLORS,
        &bits, nullptr, 0);
    if (!bitmap || !bits)
    {
        if (bitmap)
            DeleteObject(bitmap);
        DeleteObject(font);
        ReleaseDC(nullptr, screenDc);
        InvalidateDragHintRaster();
        HideDragHintWindow();
        return;
    }
    HDC memoryDc = CreateCompatibleDC(screenDc);
    if (!memoryDc)
    {
        DeleteObject(bitmap);
        DeleteObject(font);
        ReleaseDC(nullptr, screenDc);
        InvalidateDragHintRaster();
        HideDragHintWindow();
        return;
    }
    const HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
    const HGDIOBJ oldMemoryFont = SelectObject(memoryDc, font);
    if (!IsValidPreviousGdiObject(oldBitmap) ||
        !IsValidPreviousGdiObject(oldMemoryFont))
    {
        if (IsValidPreviousGdiObject(oldMemoryFont))
            SelectObject(memoryDc, oldMemoryFont);
        if (IsValidPreviousGdiObject(oldBitmap))
            SelectObject(memoryDc, oldBitmap);
        DeleteDC(memoryDc);
        DeleteObject(bitmap);
        DeleteObject(font);
        ReleaseDC(nullptr, screenDc);
        InvalidateDragHintRaster();
        HideDragHintWindow();
        return;
    }

    auto* pixels = static_cast<std::uint32_t*>(bits);
    const float panelLeft = static_cast<float>(shadowInset);
    const float panelTop = static_cast<float>(shadowInset);
    const float panelRight = panelLeft + panelWidth;
    const float panelBottom = panelTop + panelHeight;
    const float cornerRadius = static_cast<float>(
        ScaleDragHintMetric(9, dpi));
    const float shadowRadius = static_cast<float>(shadowInset);
    const float shadowOffsetY = static_cast<float>(
        ScaleDragHintMetric(1, dpi));
    std::vector<std::uint8_t> alphaMask(
        static_cast<std::size_t>(width) * height);
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const float sampleX = static_cast<float>(x) + 0.5f;
            const float sampleY = static_cast<float>(y) + 0.5f;
            const float panelDistance = RoundedRectSignedDistance(
                sampleX, sampleY,
                panelLeft, panelTop, panelRight, panelBottom,
                cornerRadius);
            const float panelCoverage = std::clamp(
                0.5f - panelDistance, 0.0f, 1.0f);
            const float borderCoverage = panelCoverage * std::clamp(
                panelDistance + 1.5f, 0.0f, 1.0f);
            const float shadowDistance = RoundedRectSignedDistance(
                sampleX, sampleY - shadowOffsetY,
                panelLeft, panelTop, panelRight, panelBottom,
                cornerRadius);
            const float shadowFalloff = std::clamp(
                1.0f - std::max(shadowDistance, 0.0f) /
                    shadowRadius,
                0.0f, 1.0f);
            const float shadowAlpha = 34.0f * shadowFalloff *
                shadowFalloff * (1.0f - panelCoverage);
            std::uint32_t pixel = PackPremultipliedArgb(
                shadowAlpha, 45.0f, 55.0f, 70.0f);

            const float backgroundRed = 252.0f +
                (205.0f - 252.0f) * borderCoverage;
            const float backgroundGreen = 253.0f +
                (211.0f - 253.0f) * borderCoverage;
            const float backgroundBlue = 255.0f +
                (220.0f - 255.0f) * borderCoverage;
            pixel = SourceOverPremultiplied(
                PackPremultipliedArgb(
                    255.0f * panelCoverage,
                    backgroundRed, backgroundGreen, backgroundBlue),
                pixel);
            const std::size_t pixelIndex =
                static_cast<std::size_t>(y) * width + x;
            pixels[pixelIndex] = pixel;
            alphaMask[pixelIndex] = static_cast<std::uint8_t>(
                pixel >> 24);
        }
    }

    SetBkMode(memoryDc, TRANSPARENT);
    SetTextColor(memoryDc, RGB(25, 32, 42));
    const int horizontalPadding = ScaleDragHintMetric(10, dpi);
    RECT textRect{
        shadowInset + horizontalPadding, shadowInset,
        shadowInset + panelWidth - horizontalPadding,
        shadowInset + panelHeight};
    const int drawnHeight = DrawTextW(
        memoryDc, text.c_str(), -1, &textRect,
        DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    // GDI text drawing does not reliably preserve the alpha byte of a 32-bit
    // DIB. Restore the analytic mask so glyph pixels stay opaque without
    // turning the rounded corners and shadow margin into a solid rectangle.
    for (std::size_t i = 0; i < alphaMask.size(); ++i)
    {
        pixels[i] = (pixels[i] & 0x00ffffffu) |
            (static_cast<std::uint32_t>(alphaMask[i]) << 24);
    }

    POINT sourcePoint{0, 0};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    const bool updated = drawnHeight > 0 &&
        UpdateLayeredWindow(
            hintHwnd_, screenDc, &windowPos, &windowSize,
            memoryDc, &sourcePoint, 0, &blend, ULW_ALPHA) != FALSE;

    SelectObject(memoryDc, oldMemoryFont);
    SelectObject(memoryDc, oldBitmap);
    DeleteDC(memoryDc);
    DeleteObject(bitmap);
    DeleteObject(font);
    ReleaseDC(nullptr, screenDc);

    if (!updated)
    {
        InvalidateDragHintRaster();
        HideDragHintWindow();
        return;
    }

    // Publish only after the layered pixels have been accepted. A failed
    // render must never make stale pixels eligible for the move-only path.
    hintTextCache_ = text;
    hintRasterSize_ = windowSize;
    hintRasterDpi_ = dpi;
    hintRasterValid_ = true;
    SetWindowPos(hintHwnd_, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
            SWP_NOOWNERZORDER |
            SWP_NOSENDCHANGING | SWP_SHOWWINDOW);
}

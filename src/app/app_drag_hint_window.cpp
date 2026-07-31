#include "app.h"

// Drag-hint popup window lifecycle and rendering.

bool DesktopApp::EnsureDragHintWindow()
{
    if (hintHwnd_) return true;
    hintHwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kHintWindowClassName, L"", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, instance_, nullptr);
    return hintHwnd_ != nullptr;
}

/**
 * @brief 隐藏拖拽提示窗口
 */
void DesktopApp::HideDragHintWindow()
{
    if (hintHwnd_) { ShowWindow(hintHwnd_, SW_HIDE); hintTextCache_.clear(); }
}

/**
 * @brief 销毁拖拽提示窗口
 */
void DesktopApp::DestroyDragHintWindow()
{
    if (hintHwnd_) { DestroyWindow(hintHwnd_); hintHwnd_ = nullptr; }
}

/**
 * @brief 显示拖拽提示窗口（客户端坐标版本）
 * @param clientPoint 客户端坐标点
 * @param text 提示文本内容
 */
void DesktopApp::ShowDragHintWindow(POINT clientPoint, const std::wstring& text)
{
    if (text.empty())
    {
        HideDragHintWindow();
        return;
    }

    // Skip expensive GDI rebuild if text hasn't changed — just move the window
    if (text == hintTextCache_)
    {
        if (hintHwnd_ && IsWindowVisible(hintHwnd_))
        {
            POINT screenPoint = clientPoint;
            ClientToScreen(hwnd_, &screenPoint);
            SetWindowPos(hintHwnd_, HWND_TOPMOST,
                screenPoint.x + 48, screenPoint.y + 22,
                0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        return;
    }
    hintTextCache_ = text;

    if (!EnsureDragHintWindow())
    {
        HideDragHintWindow();
        return;
    }

    POINT screenPoint = clientPoint;
    ClientToScreen(hwnd_, &screenPoint);

    HDC screenDc = GetDC(nullptr);
    HFONT font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(screenDc, font);
    SIZE textSize{};
    GetTextExtentPoint32W(screenDc, text.c_str(), static_cast<int>(text.size()), &textSize);
    SelectObject(screenDc, oldFont);

    int width = std::clamp(static_cast<int>(textSize.cx + 24), 130, 520);
    int height = std::clamp(static_cast<int>(textSize.cy + 14), 32, 46);
    POINT windowPos{ screenPoint.x + 48, screenPoint.y + 22 };

    HMONITOR monitor = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor && GetMonitorInfoW(monitor, &monitorInfo))
    {
        windowPos.x = std::clamp<LONG>(windowPos.x, monitorInfo.rcWork.left + 8,
            monitorInfo.rcWork.right - static_cast<LONG>(width) - 8);
        windowPos.y = std::clamp<LONG>(windowPos.y, monitorInfo.rcWork.top + 8,
            monitorInfo.rcWork.bottom - static_cast<LONG>(height) - 8);
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits)
    {
        DeleteObject(font);
        ReleaseDC(nullptr, screenDc);
        HideDragHintWindow();
        return;
    }

    auto* pixels = static_cast<std::uint32_t*>(bits);
    auto argb = [](std::uint8_t a, std::uint8_t r, std::uint8_t g, std::uint8_t b) -> std::uint32_t {
        return (static_cast<std::uint32_t>(a) << 24) |
            (static_cast<std::uint32_t>(r) << 16) |
            (static_cast<std::uint32_t>(g) << 8) |
            static_cast<std::uint32_t>(b);
    };

    const std::uint32_t bg = argb(255, 255, 255, 255);
    const std::uint32_t bd = argb(255, 205, 211, 220);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            pixels[(y * width) + x] = (x == 0 || y == 0 || x == width - 1 || y == height - 1) ? bd : bg;

    HDC memoryDc = CreateCompatibleDC(screenDc);
    HGDIOBJ oldBmp = SelectObject(memoryDc, bitmap);
    HGDIOBJ oldMFont = SelectObject(memoryDc, font);
    SetBkMode(memoryDc, TRANSPARENT);
    SetTextColor(memoryDc, RGB(25, 32, 42));
    RECT textRect{ 10, 0, width - 10, height };
    DrawTextW(memoryDc, text.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    for (int i = 0; i < width * height; ++i)
    {
        std::uint32_t pixel = pixels[i];
        std::uint8_t r = static_cast<std::uint8_t>((pixel >> 16) & 0xff);
        std::uint8_t g = static_cast<std::uint8_t>((pixel >> 8) & 0xff);
        std::uint8_t b = static_cast<std::uint8_t>(pixel & 0xff);
        pixels[i] = argb(255, r, g, b);
    }

    POINT sourcePoint{ 0, 0 };
    SIZE windowSize{ width, height };
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(hintHwnd_, screenDc, &windowPos, &windowSize, memoryDc, &sourcePoint, 0, &blend, ULW_ALPHA);
    ShowWindow(hintHwnd_, SW_SHOWNOACTIVATE);

    SelectObject(memoryDc, oldMFont);
    SelectObject(memoryDc, oldBmp);
    DeleteDC(memoryDc);
    DeleteObject(bitmap);
    DeleteObject(font);
    ReleaseDC(nullptr, screenDc);
}

/**
 * @brief 显示拖拽提示窗口（屏幕坐标版本）
 * @param screenPoint 屏幕坐标点
 * @param text 提示文本内容
 */
void DesktopApp::ShowDragHintWindowScreen(POINT screenPoint, const std::wstring& text)
{
    if (text.empty() || !EnsureDragHintWindow())
    {
        HideDragHintWindow();
        return;
    }

    HDC screenDc = GetDC(nullptr);
    HFONT font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(screenDc, font);
    SIZE textSize{};
    GetTextExtentPoint32W(screenDc, text.c_str(), static_cast<int>(text.size()), &textSize);
    SelectObject(screenDc, oldFont);

    int width = std::clamp(static_cast<int>(textSize.cx + 24), 130, 520);
    int height = std::clamp(static_cast<int>(textSize.cy + 14), 32, 46);
    POINT windowPos{ screenPoint.x + 48, screenPoint.y + 22 };

    HMONITOR monitor = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor && GetMonitorInfoW(monitor, &monitorInfo))
    {
        windowPos.x = std::clamp<LONG>(windowPos.x, monitorInfo.rcWork.left + 8,
            monitorInfo.rcWork.right - static_cast<LONG>(width) - 8);
        windowPos.y = std::clamp<LONG>(windowPos.y, monitorInfo.rcWork.top + 8,
            monitorInfo.rcWork.bottom - static_cast<LONG>(height) - 8);
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits)
    {
        DeleteObject(font);
        ReleaseDC(nullptr, screenDc);
        HideDragHintWindow();
        return;
    }

    auto* pixels = static_cast<std::uint32_t*>(bits);
    auto argb = [](std::uint8_t a, std::uint8_t r, std::uint8_t g, std::uint8_t b) -> std::uint32_t {
        return (static_cast<std::uint32_t>(a) << 24) |
            (static_cast<std::uint32_t>(r) << 16) |
            (static_cast<std::uint32_t>(g) << 8) |
            static_cast<std::uint32_t>(b);
    };

    const std::uint32_t bg = argb(255, 255, 255, 255);
    const std::uint32_t bd = argb(255, 205, 211, 220);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            pixels[(y * width) + x] = (x == 0 || y == 0 || x == width - 1 || y == height - 1) ? bd : bg;

    HDC memoryDc = CreateCompatibleDC(screenDc);
    HGDIOBJ oldBmp = SelectObject(memoryDc, bitmap);
    HGDIOBJ oldMFont = SelectObject(memoryDc, font);
    SetBkMode(memoryDc, TRANSPARENT);
    SetTextColor(memoryDc, RGB(25, 32, 42));
    RECT textRect{ 10, 0, width - 10, height };
    DrawTextW(memoryDc, text.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    for (int i = 0; i < width * height; ++i)
    {
        std::uint32_t pixel = pixels[i];
        std::uint8_t r = static_cast<std::uint8_t>((pixel >> 16) & 0xff);
        std::uint8_t g = static_cast<std::uint8_t>((pixel >> 8) & 0xff);
        std::uint8_t b = static_cast<std::uint8_t>(pixel & 0xff);
        pixels[i] = argb(255, r, g, b);
    }

    POINT sourcePoint{ 0, 0 };
    SIZE windowSize{ width, height };
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(hintHwnd_, screenDc, &windowPos, &windowSize, memoryDc, &sourcePoint, 0, &blend, ULW_ALPHA);
    ShowWindow(hintHwnd_, SW_SHOWNOACTIVATE);

    SelectObject(memoryDc, oldMFont);
    SelectObject(memoryDc, oldBmp);
    DeleteDC(memoryDc);
    DeleteObject(bitmap);
    DeleteObject(font);
    ReleaseDC(nullptr, screenDc);
}

// ── Widget context menu ────────────────────────────────────

/**
 * @brief 显示 Lua 小部件的编辑器宿主页
 * @param widgetIndex 小部件索引
 */

#include "app.h"

// Owner-drawn menu icon creation and cleanup.

HBITMAP DesktopApp::CreateMenuIconBitmap(const wchar_t* text)
{
    const int cx = std::max(20, GetSystemMetrics(SM_CXMENUCHECK));
    const int cy = std::max(20, GetSystemMetrics(SM_CYMENUCHECK));
    if (cx <= 0 || cy <= 0 || !text || !*text) return nullptr;

    HDC screenDc = GetDC(nullptr);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = cx;
    bmi.bmiHeader.biHeight = -cy;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp)
    {
        ReleaseDC(nullptr, screenDc);
        return nullptr;
    }

    std::fill_n(static_cast<std::uint32_t*>(bits), cx * cy, 0u);

    HDC memDc = CreateCompatibleDC(screenDc);
    HGDIOBJ oldBmp = SelectObject(memDc, bmp);
    HGDIOBJ fallbackFont = GetStockObject(DEFAULT_GUI_FONT);
    HGDIOBJ oldFont = SelectObject(memDc, faMenuFont_ ? faMenuFont_ : fallbackFont);

    RECT rc{ 0, 0, cx, cy };
    SetBkMode(memDc, TRANSPARENT);
    SetTextColor(memDc, RGB(255, 255, 255));
    DrawTextW(memDc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(memDc, oldFont);
    SelectObject(memDc, oldBmp);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);

    auto* pixels = static_cast<std::uint32_t*>(bits);
    const size_t count = static_cast<size_t>(cx) * static_cast<size_t>(cy);
    for (size_t i = 0; i < count; ++i)
    {
        std::uint32_t p = pixels[i];
        if ((p & 0x00FFFFFF) == 0) continue;
        std::uint8_t lum = static_cast<std::uint8_t>(
            std::max({ (p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF }));
        pixels[i] = static_cast<std::uint32_t>(lum) << 24;
    }
    return bmp;
}

/**
 * @brief 为指定菜单项设置位图图标。
 *        通过 CreateMenuIconBitmap 创建图标位图后，使用 MIIM_BITMAP
 *        将位图关联到菜单项上。创建的位图由 menuIconPool_ 统一管理。
 * @param menu    目标菜单句柄。
 * @param command 菜单项的 ID（或子菜单句柄）。
 * @param text    用于生成图标的文本（图标字符）。
 */
void DesktopApp::SetMenuItemIcon(HMENU menu, UINT_PTR command, const wchar_t* text)
{
    HBITMAP icon = CreateMenuIconBitmap(text);
    if (!icon) return;

    MENUITEMINFOW mii{ sizeof(mii) };
    mii.fMask = MIIM_BITMAP;
    mii.hbmpItem = icon;

    bool applied = false;
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count && !applied; ++i)
    {
        MENUITEMINFOW probe{ sizeof(probe) };
        probe.fMask = MIIM_ID | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &probe)) continue;
        if (probe.wID == command || reinterpret_cast<UINT_PTR>(probe.hSubMenu) == command)
            applied = SetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &mii) != FALSE;
    }

    if (applied)
        menuIconPool_.push_back(icon);
    else
        DeleteObject(icon);
}

/**
 * @brief 清除所有缓存的菜单图标位图。
 *        遍历 menuIconPool_ 逐一 DeleteObject 释放 GDI 资源，然后清空容器。
 */
void DesktopApp::ClearMenuIcons()
{
    for (HBITMAP bmp : menuIconPool_)
        DeleteObject(bmp);
    menuIconPool_.clear();
}

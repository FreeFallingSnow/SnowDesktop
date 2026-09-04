#include "widget_preview_scene.h"
#include "demo_mode_rules.h"

#include <algorithm>
#include <cstdint>

namespace snowdesktop
{
namespace
{

HBITMAP CreateSymbolBitmap(
    const std::wstring& glyph, int requestedSize, bool lightTheme,
    std::uint32_t backgroundRgb)
{
    (void)lightTheme;
    const int size = std::clamp(requestedSize, 32, 256);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* rawPixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(
        nullptr, &info, DIB_RGB_COLORS, &rawPixels, nullptr, 0);
    HDC dc = bitmap ? CreateCompatibleDC(nullptr) : nullptr;
    if (!bitmap || !dc || !rawPixels)
    {
        if (dc) DeleteDC(dc);
        if (bitmap) DeleteObject(bitmap);
        return nullptr;
    }

    auto* pixels = static_cast<std::uint32_t*>(rawPixels);
    std::fill_n(pixels, static_cast<size_t>(size) * size, 0x00000000u);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    SetBkMode(dc, TRANSPARENT);
    const COLORREF background = RGB(
        (backgroundRgb >> 16U) & 0xFFU,
        (backgroundRgb >> 8U) & 0xFFU,
        backgroundRgb & 0xFFU);
    SetGraphicsMode(dc, GM_ADVANCED);

    const int margin = std::max(2, size / 12);
    HPEN pen = CreatePen(PS_SOLID, 1, background);
    HBRUSH brush = CreateSolidBrush(background);
    HGDIOBJ oldPen = pen ? SelectObject(dc, pen) : nullptr;
    HGDIOBJ oldBrush = brush ? SelectObject(dc, brush) : nullptr;
    RoundRect(dc, margin, margin, size - margin, size - margin,
        std::max(4, size / 4), std::max(4, size / 4));

    SetTextColor(dc, RGB(255, 255, 255));
    HFONT font = CreateFontW(-std::max(12, size * 12 / 24), 0, 0, 0,
        FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"FluentSystemIcons-Regular");
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    RECT textRect{ margin, margin, size - margin, size - margin };
    DrawTextW(dc, glyph.c_str(), static_cast<int>(glyph.size()),
        &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (oldFont) SelectObject(dc, oldFont);
    if (font) DeleteObject(font);
    GdiFlush();

    for (size_t i = 0; i < static_cast<size_t>(size) * size; ++i)
    {
        const std::uint32_t rgb = pixels[i] & 0x00ffffffu;
        pixels[i] = rgb == 0 ? 0 : rgb | 0xff000000u;
    }

    if (oldBrush) SelectObject(dc, oldBrush);
    if (oldPen) SelectObject(dc, oldPen);
    if (brush) DeleteObject(brush);
    if (pen) DeleteObject(pen);

    if (oldBitmap) SelectObject(dc, oldBitmap);
    DeleteDC(dc);
    return bitmap;
}

} // namespace

void WidgetPreviewScene::PreparePlaceholderModels(
    int bitmapSize, bool lightTheme)
{
    bitmapSize = std::clamp(bitmapSize, 32, 256);
    desktopItems_.clear();
    folderEntries_.clear();
    desktopItems_.reserve(items_.size());
    folderEntries_.reserve(items_.size());
    for (size_t index = 0; index < items_.size(); ++index)
    {
        const auto& sample = items_[index];
        DesktopItem desktop;
        desktop.name = sample.title;
        desktop.parsingName = sample.key;
        desktop.layoutKey = sample.key;
        const std::uint32_t backgroundRgb = sample.backgroundRgb != 0
            ? sample.backgroundRgb
            : demo_mode_rules::kVisualIdentities[index %
                demo_mode_rules::kVisualIdentities.size()].backgroundRgb;
        desktop.iconBitmap = CreateSymbolBitmap(
            sample.glyph, bitmapSize, lightTheme, backgroundRgb);
        desktop.iconBitmapSize = { bitmapSize, bitmapSize };
        desktop.selected = false;
        desktop.iconState = desktop.iconBitmap
            ? IconState::FullQuality : IconState::Loading;
        desktopItems_.push_back(std::move(desktop));

        FolderEntry folder;
        folder.name = sample.title;
        folder.fullPath = sample.key;
        folder.isDirectory = sample.directory;
        folder.iconBitmap = CreateSymbolBitmap(
            sample.glyph, bitmapSize, lightTheme, backgroundRgb);
        folder.iconBitmapSize = { bitmapSize, bitmapSize };
        folder.selected = false;
        folder.iconState = folder.iconBitmap
            ? IconState::FullQuality : IconState::Loading;
        folderEntries_.push_back(std::move(folder));
    }
}

} // namespace snowdesktop

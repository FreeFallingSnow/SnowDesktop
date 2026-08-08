#include "widget_preview_scene.h"

#include <algorithm>
#include <cstdint>

namespace snowdesktop
{
namespace
{

HBITMAP CreateLetterBitmap(
    const std::wstring& glyph, int requestedSize, bool lightTheme,
    size_t colorIndex)
{
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
    std::fill_n(pixels, static_cast<size_t>(size) * size, 0x00ffffffu);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    SetBkMode(dc, TRANSPARENT);
    static constexpr COLORREF colors[] = {
        RGB(92, 160, 255), RGB(76, 190, 132), RGB(255, 166, 74),
        RGB(164, 118, 255), RGB(245, 101, 112), RGB(59, 190, 205),
        RGB(238, 105, 178), RGB(104, 126, 232),
    };
    const COLORREF background = colors[
        colorIndex % (sizeof(colors) / sizeof(colors[0]))];
    SetTextColor(dc, lightTheme ? RGB(20, 27, 38) : RGB(24, 28, 36));
    SetGraphicsMode(dc, GM_ADVANCED);

    const int margin = std::max(2, size / 12);
    HPEN pen = CreatePen(PS_SOLID, 1, background);
    HBRUSH brush = CreateSolidBrush(background);
    HGDIOBJ oldPen = pen ? SelectObject(dc, pen) : nullptr;
    HGDIOBJ oldBrush = brush ? SelectObject(dc, brush) : nullptr;
    RoundRect(dc, margin, margin, size - margin, size - margin,
        std::max(4, size / 3), std::max(4, size / 3));
    if (oldBrush) SelectObject(dc, oldBrush);
    if (oldPen) SelectObject(dc, oldPen);
    if (brush) DeleteObject(brush);
    if (pen) DeleteObject(pen);

    HFONT font = CreateFontW(-std::max(12, size * 13 / 24), 0, 0, 0,
        FW_BLACK, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    RECT textRect{ margin, margin, size - margin, size - margin };
    DrawTextW(dc, glyph.c_str(), static_cast<int>(glyph.size()),
        &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (oldFont) SelectObject(dc, oldFont);
    if (font) DeleteObject(font);
    GdiFlush();

    for (size_t i = 0; i < static_cast<size_t>(size) * size; ++i)
    {
        const std::uint32_t source = pixels[i];
        const unsigned blue = source & 0xffu;
        const unsigned green = (source >> 8) & 0xffu;
        const unsigned red = (source >> 16) & 0xffu;
        const unsigned distance =
            (255u - red) + (255u - green) + (255u - blue);
        const unsigned alpha = std::min(255u, distance * 3u);
        pixels[i] = (blue * alpha / 255u) |
            ((green * alpha / 255u) << 8) |
            ((red * alpha / 255u) << 16) | (alpha << 24);
    }

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
        desktop.iconBitmap = CreateLetterBitmap(
            sample.glyph, bitmapSize, lightTheme, index);
        desktop.iconBitmapSize = { bitmapSize, bitmapSize };
        desktop.selected = false;
        desktop.iconState = desktop.iconBitmap
            ? IconState::FullQuality : IconState::Loading;
        desktopItems_.push_back(std::move(desktop));

        FolderEntry folder;
        folder.name = sample.title;
        folder.fullPath = sample.key;
        folder.isDirectory = sample.directory;
        folder.iconBitmap = CreateLetterBitmap(
            sample.glyph, bitmapSize, lightTheme, index);
        folder.iconBitmapSize = { bitmapSize, bitmapSize };
        folder.selected = false;
        folder.iconState = folder.iconBitmap
            ? IconState::FullQuality : IconState::Loading;
        folderEntries_.push_back(std::move(folder));
    }
}

} // namespace snowdesktop

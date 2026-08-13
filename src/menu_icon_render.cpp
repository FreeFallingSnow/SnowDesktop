#include "menu_icon_render.h"

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace snowdesktop::menu_icon
{
namespace
{

int Scale(int value, UINT dpi)
{
    return std::max(1, MulDiv(value, static_cast<int>(dpi),
        USER_DEFAULT_SCREEN_DPI));
}

void FillSolidRect(HDC dc, const RECT& bounds, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    if (!brush)
        return;
    FillRect(dc, &bounds, brush);
    DeleteObject(brush);
}

void FillRoundedRect(HDC dc, const RECT& bounds, int radius,
    COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    if (!brush)
        return;
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom,
        radius * 2, radius * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
}

const wchar_t* ResolveQuickGlyph(
    MenuQuickIcon icon, const wchar_t* fallback)
{
    switch (icon)
    {
    case MenuQuickIcon::Paste: return L"\uF2D5";
    case MenuQuickIcon::NewItem: return L"\uF10C";
    case MenuQuickIcon::Refresh: return L"\uF13D";
    case MenuQuickIcon::Cut: return L"\uF33A";
    case MenuQuickIcon::Copy: return L"\uF32B";
    case MenuQuickIcon::Rename: return L"\U000F0A39";
    case MenuQuickIcon::Delete: return L"\uF34C";
    case MenuQuickIcon::Edit: return L"\uF3DD";
    case MenuQuickIcon::Settings: return L"\uF6A9";
    case MenuQuickIcon::Open: return L"\uF582";
    case MenuQuickIcon::FontGlyph:
    default:
        return fallback ? fallback : L"";
    }
}

RECT RelativeClip(const RECT& bounds,
    int leftPercent, int topPercent,
    int rightPercent, int bottomPercent)
{
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    return {
        bounds.left + width * leftPercent / 100,
        bounds.top + height * topPercent / 100,
        bounds.left + width * rightPercent / 100,
        bounds.top + height * bottomPercent / 100,
    };
}

struct GlyphAlphaMask
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> alpha;
};

using GlyphMaskKey = std::tuple<
    LONG, LONG, LONG, int, int, std::wstring>;

using TextAlignmentKey = std::tuple<
    LONG, LONG, LONG, BYTE, BYTE, BYTE, int,
    std::wstring, std::wstring>;

int ComputeOpticalTextOffset(HDC dc, const std::wstring& text,
    int boundsHeight)
{
    if (!dc || text.empty() || boundsHeight <= 0)
        return 0;

    HFONT font = static_cast<HFONT>(GetCurrentObject(dc, OBJ_FONT));
    TEXTMETRICW textMetrics{};
    SIZE extent{};
    if (!font || !GetTextMetricsW(dc, &textMetrics) ||
        !GetTextExtentPoint32W(dc, text.c_str(),
            static_cast<int>(text.size()), &extent))
    {
        return 0;
    }

    // DT_VCENTER centers the selected font's line box.  With GDI font
    // linking, fallback glyph ink (notably CJK) can sit several physical
    // pixels below that centre at fractional DPI scales.  Render a small
    // monochrome probe so the visible ink, rather than the nominal line box,
    // determines the correction.
    constexpr int padding = 4;
    constexpr int maximumWidth = 4096;
    const int bitmapWidth = std::clamp(
        static_cast<int>(extent.cx) + padding * 2,
        padding * 2 + 1, maximumWidth);
    const int bitmapHeight = std::max(
        boundsHeight + padding * 2,
        static_cast<int>(textMetrics.tmHeight) + padding * 2);
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = bitmapWidth;
    bitmapInfo.bmiHeader.biHeight = -bitmapHeight;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* rawPixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bitmapInfo,
        DIB_RGB_COLORS, &rawPixels, nullptr, 0);
    HDC maskDc = bitmap ? CreateCompatibleDC(dc) : nullptr;
    if (!bitmap || !maskDc || !rawPixels)
    {
        if (maskDc)
            DeleteDC(maskDc);
        if (bitmap)
            DeleteObject(bitmap);
        return 0;
    }

    HGDIOBJ oldBitmap = SelectObject(maskDc, bitmap);
    HGDIOBJ oldFont = SelectObject(maskDc, font);
    std::fill_n(static_cast<std::uint32_t*>(rawPixels),
        static_cast<size_t>(bitmapWidth) * bitmapHeight, 0u);
    SetBkMode(maskDc, TRANSPARENT);
    SetTextColor(maskDc, RGB(255, 255, 255));
    RECT drawBounds{
        padding,
        padding,
        bitmapWidth - padding,
        padding + boundsHeight,
    };
    DrawTextW(maskDc, text.c_str(), static_cast<int>(text.size()),
        &drawBounds,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    const auto* pixels = static_cast<const std::uint32_t*>(rawPixels);
    int inkTop = bitmapHeight;
    int inkBottom = -1;
    for (int y = 0; y < bitmapHeight; ++y)
    {
        for (int x = 0; x < bitmapWidth; ++x)
        {
            if ((pixels[static_cast<size_t>(y) * bitmapWidth + x] &
                    0x00FFFFFFu) == 0)
            {
                continue;
            }
            inkTop = std::min(inkTop, y);
            inkBottom = std::max(inkBottom, y);
        }
    }

    if (oldFont && oldFont != HGDI_ERROR)
        SelectObject(maskDc, oldFont);
    if (oldBitmap && oldBitmap != HGDI_ERROR)
        SelectObject(maskDc, oldBitmap);
    DeleteDC(maskDc);
    DeleteObject(bitmap);
    if (inkBottom < inkTop)
        return 0;

    const int desiredCenterTwice = padding * 2 + boundsHeight - 1;
    const int inkCenterTwice = inkTop + inkBottom;
    const int deltaTwice = desiredCenterTwice - inkCenterTwice;
    const int roundedOffset = deltaTwice >= 0
        ? (deltaTwice + 1) / 2
        : (deltaTwice - 1) / 2;
    return std::clamp(roundedOffset,
        -boundsHeight / 4, boundsHeight / 4);
}

int OpticalTextOffset(HDC dc, std::wstring_view text, int boundsHeight)
{
    if (!dc || text.empty() || boundsHeight <= 0)
        return 0;
    HFONT font = static_cast<HFONT>(GetCurrentObject(dc, OBJ_FONT));
    LOGFONTW logFont{};
    if (!font || GetObjectW(font, sizeof(logFont), &logFont) == 0)
        return 0;

    constexpr size_t maximumSampleLength = 128;
    std::wstring sample(text.substr(0, maximumSampleLength));
    const TextAlignmentKey key{
        logFont.lfHeight,
        logFont.lfWidth,
        logFont.lfWeight,
        logFont.lfItalic,
        logFont.lfCharSet,
        logFont.lfQuality,
        boundsHeight,
        std::wstring(logFont.lfFaceName),
        sample,
    };
    static std::mutex cacheMutex;
    static std::map<TextAlignmentKey, int> cache;
    {
        std::lock_guard lock(cacheMutex);
        const auto found = cache.find(key);
        if (found != cache.end())
            return found->second;
    }

    const int offset = ComputeOpticalTextOffset(dc, sample, boundsHeight);
    std::lock_guard lock(cacheMutex);
    if (cache.size() >= 512)
        cache.clear();
    return cache.try_emplace(key, offset).first->second;
}

void OpticallyCenterTextBounds(HDC dc, std::wstring_view text, RECT& bounds)
{
    const int height = static_cast<int>(bounds.bottom - bounds.top);
    OffsetRect(&bounds, 0, OpticalTextOffset(dc, text, height));
}

GlyphAlphaMask BuildOpticallyWeightedMask(const LOGFONTW& sourceFont,
    const wchar_t* glyph, int width, int height)
{
    constexpr int oversample = 4;
    GlyphAlphaMask result;
    if (!glyph || !*glyph || width <= 0 || height <= 0)
        return result;

    const int highWidth = width * oversample;
    const int highHeight = height * oversample;
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = highWidth;
    bitmapInfo.bmiHeader.biHeight = -highHeight;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* rawPixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bitmapInfo,
        DIB_RGB_COLORS, &rawPixels, nullptr, 0);
    HDC maskDc = bitmap ? CreateCompatibleDC(nullptr) : nullptr;
    if (!bitmap || !maskDc || !rawPixels)
    {
        if (maskDc)
            DeleteDC(maskDc);
        if (bitmap)
            DeleteObject(bitmap);
        return result;
    }

    HGDIOBJ oldBitmap = SelectObject(maskDc, bitmap);
    std::fill_n(static_cast<std::uint32_t*>(rawPixels),
        static_cast<size_t>(highWidth) * highHeight, 0u);

    LOGFONTW highFont = sourceFont;
    highFont.lfHeight *= oversample;
    highFont.lfWidth *= oversample;
    highFont.lfWeight = FW_NORMAL;
    highFont.lfQuality = ANTIALIASED_QUALITY;
    HFONT font = CreateFontIndirectW(&highFont);
    if (!font)
    {
        if (oldBitmap)
            SelectObject(maskDc, oldBitmap);
        DeleteDC(maskDc);
        DeleteObject(bitmap);
        return result;
    }
    HGDIOBJ oldFont = SelectObject(maskDc, font);
    if (!oldFont || oldFont == HGDI_ERROR)
    {
        DeleteObject(font);
        if (oldBitmap)
            SelectObject(maskDc, oldBitmap);
        DeleteDC(maskDc);
        DeleteObject(bitmap);
        return result;
    }
    SetBkMode(maskDc, TRANSPARENT);
    SetTextColor(maskDc, RGB(255, 255, 255));
    RECT drawBounds{ 0, 0, highWidth, highHeight };
    DrawTextW(maskDc, glyph, -1, &drawBounds,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    const auto* pixels = static_cast<const std::uint32_t*>(rawPixels);
    std::vector<std::uint8_t> source(
        static_cast<size_t>(highWidth) * highHeight);
    for (size_t i = 0; i < source.size(); ++i)
        source[i] = static_cast<std::uint8_t>(pixels[i] & 0xFFu);

    // Expand the 4x mask by one sample before downsampling.  This adds only
    // 0.25 physical pixel of optical weight and preserves counters/corners.
    std::vector<std::uint8_t> expanded(source.size(), 0);
    for (int y = 0; y < highHeight; ++y)
    {
        for (int x = 0; x < highWidth; ++x)
        {
            std::uint8_t coverage = 0;
            for (int offsetY = -1; offsetY <= 1; ++offsetY)
            {
                const int sampleY = y + offsetY;
                if (sampleY < 0 || sampleY >= highHeight)
                    continue;
                for (int offsetX = -1; offsetX <= 1; ++offsetX)
                {
                    const int sampleX = x + offsetX;
                    if (sampleX < 0 || sampleX >= highWidth)
                        continue;
                    coverage = std::max(coverage, source[
                        static_cast<size_t>(sampleY) * highWidth +
                        sampleX]);
                }
            }
            expanded[static_cast<size_t>(y) * highWidth + x] = coverage;
        }
    }

    result.width = width;
    result.height = height;
    result.alpha.resize(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            unsigned sum = 0;
            for (int sampleY = 0; sampleY < oversample; ++sampleY)
            {
                for (int sampleX = 0; sampleX < oversample; ++sampleX)
                {
                    sum += expanded[
                        static_cast<size_t>(y * oversample + sampleY) *
                            highWidth +
                        x * oversample + sampleX];
                }
            }
            result.alpha[static_cast<size_t>(y) * width + x] =
                static_cast<std::uint8_t>((sum + 8) / 16);
        }
    }

    // Fluent glyph masters do not all share the same visible vertical
    // bearings.  Centre the final raster mask so every leading menu icon
    // aligns with the row independently of glyph choice and DPI.
    int inkTop = height;
    int inkBottom = -1;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            if (result.alpha[static_cast<size_t>(y) * width + x] == 0)
                continue;
            inkTop = std::min(inkTop, y);
            inkBottom = std::max(inkBottom, y);
        }
    }
    if (inkBottom >= inkTop)
    {
        const int deltaTwice = height - 1 - inkTop - inkBottom;
        const int verticalOffset = deltaTwice >= 0
            ? (deltaTwice + 1) / 2
            : (deltaTwice - 1) / 2;
        if (verticalOffset != 0)
        {
            std::vector<std::uint8_t> centered(result.alpha.size(), 0);
            for (int y = 0; y < height; ++y)
            {
                const int destinationY = y + verticalOffset;
                if (destinationY < 0 || destinationY >= height)
                    continue;
                std::copy_n(
                    result.alpha.begin() + static_cast<size_t>(y) * width,
                    width,
                    centered.begin() +
                        static_cast<size_t>(destinationY) * width);
            }
            result.alpha.swap(centered);
        }
    }

    if (oldFont)
        SelectObject(maskDc, oldFont);
    if (font)
        DeleteObject(font);
    if (oldBitmap)
        SelectObject(maskDc, oldBitmap);
    DeleteDC(maskDc);
    DeleteObject(bitmap);
    return result;
}

const GlyphAlphaMask* GetOpticallyWeightedMask(HDC dc,
    const wchar_t* glyph, int width, int height)
{
    if (!dc || !glyph || !*glyph)
        return nullptr;
    HFONT font = static_cast<HFONT>(GetCurrentObject(dc, OBJ_FONT));
    LOGFONTW logFont{};
    if (!font || GetObjectW(font, sizeof(logFont), &logFont) == 0 ||
        _wcsicmp(logFont.lfFaceName,
            L"FluentSystemIcons-Regular") != 0)
    {
        return nullptr;
    }

    const GlyphMaskKey key{
        logFont.lfHeight,
        logFont.lfWidth,
        logFont.lfWeight,
        width,
        height,
        std::wstring(glyph),
    };
    static std::mutex cacheMutex;
    static std::map<GlyphMaskKey, GlyphAlphaMask> cache;
    std::lock_guard lock(cacheMutex);
    auto [it, inserted] = cache.try_emplace(key);
    if (inserted)
    {
        it->second = BuildOpticallyWeightedMask(
            logFont, glyph, width, height);
    }
    return it->second.alpha.empty() ? nullptr : &it->second;
}

bool DrawOpticallyWeightedFluentGlyph(HDC dc, const wchar_t* glyph,
    const RECT& bounds, COLORREF color, const RECT* clip)
{
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    const GlyphAlphaMask* mask = GetOpticallyWeightedMask(
        dc, glyph, width, height);
    if (!mask)
        return false;

    using AlphaBlendFn = BOOL(WINAPI*)(HDC, int, int, int, int,
        HDC, int, int, int, int, BLENDFUNCTION);
    static const HMODULE alphaBlendModule =
        LoadLibraryW(L"msimg32.dll");
    static const auto alphaBlend = alphaBlendModule
        ? reinterpret_cast<AlphaBlendFn>(
            GetProcAddress(alphaBlendModule, "AlphaBlend"))
        : nullptr;
    if (!alphaBlend)
        return false;

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* rawPixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bitmapInfo,
        DIB_RGB_COLORS, &rawPixels, nullptr, 0);
    HDC sourceDc = bitmap ? CreateCompatibleDC(dc) : nullptr;
    if (!bitmap || !sourceDc || !rawPixels)
    {
        if (sourceDc)
            DeleteDC(sourceDc);
        if (bitmap)
            DeleteObject(bitmap);
        return false;
    }

    auto* pixels = static_cast<std::uint32_t*>(rawPixels);
    for (size_t i = 0; i < mask->alpha.size(); ++i)
    {
        const unsigned alpha = mask->alpha[i];
        const unsigned blue = GetBValue(color) * alpha / 255;
        const unsigned green = GetGValue(color) * alpha / 255;
        const unsigned red = GetRValue(color) * alpha / 255;
        pixels[i] = blue | (green << 8) | (red << 16) | (alpha << 24);
    }

    HGDIOBJ oldBitmap = SelectObject(sourceDc, bitmap);
    const int savedDc = SaveDC(dc);
    if (clip)
    {
        IntersectClipRect(dc, clip->left, clip->top,
            clip->right, clip->bottom);
    }
    const BLENDFUNCTION blend{
        AC_SRC_OVER, 0, 255, AC_SRC_ALPHA,
    };
    const BOOL drawn = alphaBlend(dc,
        bounds.left, bounds.top, width, height,
        sourceDc, 0, 0, width, height, blend);
    RestoreDC(dc, savedDc);
    if (oldBitmap)
        SelectObject(sourceDc, oldBitmap);
    DeleteDC(sourceDc);
    DeleteObject(bitmap);
    return drawn != FALSE;
}

void DrawGlyphLayer(HDC dc, const wchar_t* glyph,
    const RECT& bounds, COLORREF color, const RECT* clip)
{
    if (!glyph || !*glyph)
        return;
    if (DrawOpticallyWeightedFluentGlyph(
            dc, glyph, bounds, color, clip))
    {
        return;
    }

    RECT alignedBounds = bounds;
    OpticallyCenterTextBounds(dc, glyph, alignedBounds);
    const int verticalOffset = alignedBounds.top - bounds.top;
    RECT alignedClip{};
    const RECT* effectiveClip = nullptr;
    if (clip)
    {
        alignedClip = *clip;
        OffsetRect(&alignedClip, 0, verticalOffset);
        effectiveClip = &alignedClip;
    }
    const int savedDc = SaveDC(dc);
    if (effectiveClip)
    {
        IntersectClipRect(dc, effectiveClip->left, effectiveClip->top,
            effectiveClip->right, effectiveClip->bottom);
    }
    SetTextColor(dc, color);
    RECT drawBounds = alignedBounds;
    DrawTextW(dc, glyph, -1, &drawBounds,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RestoreDC(dc, savedDc);
}

void DrawQuickLayeredGlyph(HDC dc, MenuQuickIcon icon,
    const wchar_t* fallback, const RECT& bounds,
    COLORREF foreground, COLORREF accent, bool disabled)
{
    const wchar_t* glyph = ResolveQuickGlyph(icon, fallback);
    DrawGlyphLayer(dc, glyph, bounds, foreground, nullptr);
    if (disabled)
        return;

    RECT glyphBounds = bounds;
    SIZE glyphSize{};
    const int glyphLength = glyph
        ? static_cast<int>(std::wcslen(glyph)) : 0;
    if (glyphLength > 0 &&
        GetTextExtentPoint32W(dc, glyph, glyphLength, &glyphSize) &&
        glyphSize.cx > 0 && glyphSize.cy > 0)
    {
        const int centerX = (bounds.left + bounds.right) / 2;
        const int centerY = (bounds.top + bounds.bottom) / 2;
        glyphBounds = {
            centerX - glyphSize.cx / 2,
            centerY - glyphSize.cy / 2,
            centerX - glyphSize.cx / 2 + glyphSize.cx,
            centerY - glyphSize.cy / 2 + glyphSize.cy,
        };
    }

    const auto drawAccent = [&](int leftPercent, int topPercent,
                                int rightPercent, int bottomPercent) {
        const RECT clip = RelativeClip(glyphBounds,
            leftPercent, topPercent, rightPercent, bottomPercent);
        DrawGlyphLayer(dc, glyph, bounds, accent, &clip);
    };
    switch (icon)
    {
    case MenuQuickIcon::NewItem:
        // Fluent add_circle_20_regular matches the Windows treatment.  Keep
        // the ring neutral and color only the plus in the centre so it stays
        // crisp on both light and dark acrylic backgrounds.
        drawAccent(30, 30, 70, 70);
        return;
    case MenuQuickIcon::Cut:
        // Match the native Windows 11 treatment: both handles are blue while
        // the blades remain neutral.
        drawAccent(0, 52, 100, 100);
        return;
    case MenuQuickIcon::Copy:
        // The foreground sheet is blue; the rear sheet stays neutral.
        drawAccent(40, 15, 100, 85);
        return;
    case MenuQuickIcon::Rename:
        // The rename-A glyph matches Explorer.  Three tight clips color the
        // I-beam without spilling onto the adjacent neutral bracket.
        drawAccent(58, 0, 67, 100);
        drawAccent(50, 0, 75, 22);
        drawAccent(50, 78, 75, 100);
        return;
    case MenuQuickIcon::Open:
        // Match Explorer: the open window stays neutral while only the
        // north-east arrow uses the system accent color.
        drawAccent(48, 0, 100, 56);
        return;
    case MenuQuickIcon::Paste:
    case MenuQuickIcon::Refresh:
    case MenuQuickIcon::Delete:
    case MenuQuickIcon::Edit:
    case MenuQuickIcon::Settings:
    case MenuQuickIcon::FontGlyph:
    default:
        return;
    }
}

SIZE MeasureText(HDC dc, const wchar_t* text, int length)
{
    SIZE result{};
    if (text && length > 0)
        GetTextExtentPoint32W(dc, text, length, &result);
    return result;
}

void DrawCheckmark(HDC dc, const RECT& bounds, const Metrics& metrics,
    COLORREF color)
{
    const int centerX = bounds.left + metrics.leftPadding +
        metrics.iconColumnWidth / 2;
    const int centerY = (bounds.top + bounds.bottom) / 2;
    const int stroke = std::max(1, metrics.rowHeight / 17);
    HPEN pen = CreatePen(PS_SOLID, stroke, color);
    if (!pen)
        return;
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, centerX - metrics.iconColumnWidth / 4, centerY, nullptr);
    LineTo(dc, centerX - 1, centerY + metrics.iconColumnWidth / 5);
    LineTo(dc, centerX + metrics.iconColumnWidth / 3,
        centerY - metrics.iconColumnWidth / 4);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawSubmenuArrow(HDC dc, const RECT& bounds,
    const Metrics& metrics, COLORREF color)
{
    const int centerX = bounds.right - metrics.rightPadding -
        metrics.arrowColumnWidth / 2;
    const int centerY = (bounds.top + bounds.bottom) / 2;
    const int halfWidth = std::max(2, metrics.arrowColumnWidth / 8);
    const int halfHeight = std::max(3, metrics.arrowColumnWidth / 4);
    HPEN pen = CreatePen(PS_SOLID,
        std::max(1, metrics.rowHeight / 24), color);
    if (!pen)
        return;
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, centerX - halfWidth, centerY - halfHeight, nullptr);
    LineTo(dc, centerX + halfWidth, centerY);
    LineTo(dc, centerX - halfWidth, centerY + halfHeight);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

} // namespace

Palette ResolvePalette(bool lightTheme)
{
    if (lightTheme)
    {
        return {
            RGB(250, 250, 250),
            RGB(232, 232, 232),
            RGB(26, 26, 26),
            RGB(118, 118, 118),
            RGB(225, 225, 225),
            RGB(0, 120, 212),
        };
    }
    return {
        RGB(44, 44, 44),
        RGB(56, 56, 56),
        RGB(255, 255, 255),
        RGB(158, 158, 158),
        RGB(68, 68, 68),
        RGB(96, 205, 255),
    };
}

Metrics ResolveMetrics(UINT dpi)
{
    const UINT effectiveDpi = dpi > 0 ? dpi : USER_DEFAULT_SCREEN_DPI;
    return {
        Scale(32, effectiveDpi),
        Scale(8, effectiveDpi),
        Scale(192, effectiveDpi),
        Scale(4, effectiveDpi),
        Scale(2, effectiveDpi),
        Scale(4, effectiveDpi),
        Scale(10, effectiveDpi),
        Scale(22, effectiveDpi),
        Scale(7, effectiveDpi),
        Scale(9, effectiveDpi),
        Scale(16, effectiveDpi),
        Scale(52, effectiveDpi),
        Scale(46, effectiveDpi),
        Scale(64, effectiveDpi),
        Scale(20, effectiveDpi),
        Scale(1, effectiveDpi),
        Scale(14, effectiveDpi),
        Scale(12, effectiveDpi),
        Scale(18, effectiveDpi),
        Scale(18, effectiveDpi),
    };
}

SIZE MeasureItem(HDC dc, HFONT textFont, const ItemView& item,
    const Metrics& metrics)
{
    if (item.separator)
        return { static_cast<LONG>(metrics.minimumWidth),
            static_cast<LONG>(metrics.separatorHeight) };

    HGDIOBJ oldFont = nullptr;
    if (dc && textFont)
        oldFont = SelectObject(dc, textFont);

    const std::wstring_view label = item.label ? item.label : L"";
    const size_t tab = label.find(L'\t');
    const std::wstring_view primary = label.substr(0, tab);
    const std::wstring_view shortcut = tab == std::wstring_view::npos
        ? std::wstring_view{} : label.substr(tab + 1);
    const SIZE primarySize = MeasureText(dc, primary.data(),
        static_cast<int>(primary.size()));
    const SIZE shortcutSize = MeasureText(dc, shortcut.data(),
        static_cast<int>(shortcut.size()));
    if (oldFont)
        SelectObject(dc, oldFont);

    const int shortcutGap = shortcut.empty() ? 0 : metrics.textGap * 3;
    const int arrowWidth = item.hasSubmenu ? metrics.arrowColumnWidth : 0;
    const int contentWidth = metrics.leftPadding +
        metrics.iconColumnWidth + metrics.textGap + primarySize.cx +
        shortcutGap + shortcutSize.cx + arrowWidth + metrics.rightPadding;
    return {
        static_cast<LONG>(std::max(metrics.minimumWidth, contentWidth)),
        static_cast<LONG>(metrics.rowHeight),
    };
}

bool DrawItem(HDC dc, HFONT textFont, HFONT iconFont,
    const ItemView& item, const RECT& bounds, UINT itemState,
    const Palette& palette, const Metrics& metrics)
{
    if (!dc || bounds.right <= bounds.left || bounds.bottom <= bounds.top)
        return false;

    FillSolidRect(dc, bounds, palette.background);
    if (item.separator)
    {
        const int centerY = (bounds.top + bounds.bottom) / 2;
        RECT line{
            bounds.left + metrics.outerInset * 2,
            centerY,
            bounds.right - metrics.outerInset * 2,
            centerY + 1,
        };
        FillSolidRect(dc, line, palette.separator);
        return true;
    }

    const bool disabled =
        (itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
    const bool selected = (itemState & ODS_SELECTED) != 0;
    if (selected && !disabled)
    {
        RECT selection = bounds;
        selection.left += metrics.outerInset;
        selection.right -= metrics.outerInset;
        selection.top += metrics.selectionInsetY;
        selection.bottom -= metrics.selectionInsetY;
        FillRoundedRect(dc, selection, metrics.selectionRadius,
            palette.hoverBackground);
    }

    const COLORREF foreground = disabled
        ? palette.disabledText : palette.text;
    const int oldBackgroundMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldTextColor = SetTextColor(dc, foreground);

    if (item.checked || (itemState & ODS_CHECKED) != 0)
    {
        DrawCheckmark(dc, bounds, metrics, foreground);
    }
    else if (item.glyph && *item.glyph)
    {
        HGDIOBJ oldFont = SelectObject(dc,
            iconFont ? static_cast<HGDIOBJ>(iconFont)
                     : GetStockObject(DEFAULT_GUI_FONT));
        RECT iconBounds = bounds;
        iconBounds.left += metrics.leftPadding;
        iconBounds.right = iconBounds.left + metrics.iconColumnWidth;
        if (item.semanticIcon == MenuQuickIcon::FontGlyph)
        {
            DrawGlyphLayer(dc, item.glyph, iconBounds,
                foreground, nullptr);
        }
        else
        {
            const COLORREF accent = disabled
                ? palette.disabledText : palette.accent;
            DrawQuickLayeredGlyph(dc, item.semanticIcon,
                item.glyph, iconBounds, foreground, accent, disabled);
        }
        if (oldFont)
            SelectObject(dc, oldFont);
    }

    HGDIOBJ oldFont = SelectObject(dc,
        textFont ? static_cast<HGDIOBJ>(textFont)
                 : GetStockObject(DEFAULT_GUI_FONT));
    RECT textBounds = bounds;
    textBounds.left += metrics.leftPadding +
        metrics.iconColumnWidth + metrics.textGap;
    textBounds.right -= metrics.rightPadding +
        (item.hasSubmenu ? metrics.arrowColumnWidth : 0);

    std::wstring_view label = item.label ? item.label : L"";
    const size_t tab = label.find(L'\t');
    std::wstring primary(label.substr(0, tab));
    std::wstring shortcut = tab == std::wstring_view::npos
        ? std::wstring{} : std::wstring(label.substr(tab + 1));
    std::wstring alignmentSample = primary;
    if (!shortcut.empty())
    {
        alignmentSample += L' ';
        alignmentSample += shortcut;
    }
    OpticallyCenterTextBounds(dc, alignmentSample, textBounds);
    DrawTextW(dc, primary.c_str(), static_cast<int>(primary.size()),
        &textBounds, DT_LEFT | DT_VCENTER | DT_SINGLELINE |
            DT_END_ELLIPSIS | DT_HIDEPREFIX);
    if (!shortcut.empty())
    {
        DrawTextW(dc, shortcut.c_str(), static_cast<int>(shortcut.size()),
            &textBounds, DT_RIGHT | DT_VCENTER | DT_SINGLELINE |
                DT_END_ELLIPSIS | DT_HIDEPREFIX);
    }
    if (oldFont)
        SelectObject(dc, oldFont);

    if (item.hasSubmenu)
        DrawSubmenuArrow(dc, bounds, metrics, foreground);

    SetTextColor(dc, oldTextColor);
    SetBkMode(dc, oldBackgroundMode);
    return true;
}

bool DrawQuickAction(HDC dc, HFONT textFont, HFONT iconFont,
    MenuQuickIcon quickIcon, const ItemView& item, const RECT& bounds,
    UINT itemState, const Palette& palette, const Metrics& metrics)
{
    if (!dc || bounds.right <= bounds.left || bounds.bottom <= bounds.top)
        return false;

    FillSolidRect(dc, bounds, palette.background);
    const bool disabled =
        (itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
    const bool selected = (itemState & ODS_SELECTED) != 0;
    if (selected && !disabled)
    {
        RECT selection = bounds;
        selection.left += metrics.outerInset;
        selection.right -= metrics.outerInset;
        selection.top += metrics.selectionInsetY;
        selection.bottom -= metrics.selectionInsetY;
        FillRoundedRect(dc, selection, metrics.selectionRadius,
            palette.hoverBackground);
    }

    const COLORREF foreground = disabled
        ? palette.disabledText : palette.text;
    const int oldBackgroundMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldTextColor = SetTextColor(dc, foreground);
    HGDIOBJ oldFont = SelectObject(dc,
        iconFont ? static_cast<HGDIOBJ>(iconFont)
                 : GetStockObject(DEFAULT_GUI_FONT));
    RECT iconBounds = bounds;
    iconBounds.left += metrics.outerInset;
    iconBounds.right -= metrics.outerInset;
    iconBounds.top += metrics.outerInset;
    iconBounds.bottom = iconBounds.top + metrics.quickActionIconHeight;
    const COLORREF accent = disabled
        ? palette.disabledText : palette.accent;
    DrawQuickLayeredGlyph(dc, quickIcon, item.glyph, iconBounds,
        foreground, accent, disabled);
    if (oldFont)
        SelectObject(dc, oldFont);

    oldFont = SelectObject(dc,
        textFont ? static_cast<HGDIOBJ>(textFont)
                 : GetStockObject(DEFAULT_GUI_FONT));
    RECT labelBounds = bounds;
    labelBounds.left += metrics.outerInset;
    labelBounds.right -= metrics.outerInset;
    labelBounds.top = iconBounds.bottom + metrics.quickActionLabelGap;
    labelBounds.bottom -= metrics.outerInset;
    std::wstring_view label = item.label ? item.label : L"";
    const size_t tab = label.find(L'\t');
    std::wstring primary(label.substr(0, tab));
    OpticallyCenterTextBounds(dc, primary, labelBounds);
    DrawTextW(dc, primary.c_str(), static_cast<int>(primary.size()),
        &labelBounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE |
            DT_END_ELLIPSIS | DT_HIDEPREFIX);
    if (oldFont)
        SelectObject(dc, oldFont);
    SetTextColor(dc, oldTextColor);
    SetBkMode(dc, oldBackgroundMode);
    return true;
}

bool DrawInlineAction(HDC dc, HFONT textFont, HFONT iconFont,
    const ItemView& item, const RECT& bounds, UINT itemState,
    const Palette& palette, const Metrics& metrics)
{
    if (!dc || bounds.right <= bounds.left || bounds.bottom <= bounds.top)
        return false;

    FillSolidRect(dc, bounds, palette.background);
    const bool disabled =
        (itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
    const bool selected = (itemState & ODS_SELECTED) != 0 || item.checked;
    if (selected && !disabled)
    {
        RECT selection = bounds;
        selection.left += metrics.outerInset;
        selection.right -= metrics.outerInset;
        selection.top += metrics.selectionInsetY;
        selection.bottom -= metrics.selectionInsetY;
        FillRoundedRect(dc, selection, metrics.selectionRadius,
            palette.hoverBackground);
    }

    const COLORREF foreground = disabled
        ? palette.disabledText
        : (item.checked ? palette.accent : palette.text);
    const int oldMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldColor = SetTextColor(dc, foreground);
    const bool hasGlyph = item.glyph && *item.glyph;
    const bool hasLabel = item.label && *item.label;
    if (hasGlyph)
    {
        HGDIOBJ oldFont = SelectObject(dc,
            iconFont ? static_cast<HGDIOBJ>(iconFont)
                     : GetStockObject(DEFAULT_GUI_FONT));
        RECT glyphBounds = bounds;
        if (hasLabel)
        {
            glyphBounds.left += metrics.leftPadding;
            glyphBounds.right =
                glyphBounds.left + metrics.iconColumnWidth;
        }
        else
        {
            glyphBounds.left += metrics.outerInset * 2;
            glyphBounds.right -= metrics.outerInset * 2;
        }
        DrawGlyphLayer(dc, item.glyph, glyphBounds, foreground, nullptr);
        if (oldFont) SelectObject(dc, oldFont);
    }
    if (hasLabel)
    {
        HGDIOBJ oldFont = SelectObject(dc,
            textFont ? static_cast<HGDIOBJ>(textFont)
                     : GetStockObject(DEFAULT_GUI_FONT));
        RECT labelBounds = bounds;
        labelBounds.left += hasGlyph
            ? metrics.leftPadding + metrics.iconColumnWidth +
                metrics.textGap
            : metrics.outerInset * 2;
        labelBounds.right -= metrics.outerInset * 2;
        OpticallyCenterTextBounds(dc, item.label, labelBounds);
        DrawTextW(dc, item.label, -1, &labelBounds,
            (hasGlyph ? DT_LEFT : DT_CENTER) |
                DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                DT_NOPREFIX);
        if (oldFont) SelectObject(dc, oldFont);
    }
    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldMode);
    return true;
}

bool DrawTextInput(HDC dc, HFONT textFont, HFONT iconFont,
    const ItemView& item, const TextInputView& input,
    const RECT& bounds, const Palette& palette, const Metrics& metrics)
{
    if (!dc || bounds.right <= bounds.left || bounds.bottom <= bounds.top)
        return false;

    FillSolidRect(dc, bounds, palette.background);
    RECT field = bounds;
    field.left += metrics.outerInset;
    field.right -= metrics.outerInset;
    field.top += metrics.selectionInsetY;
    field.bottom -= metrics.selectionInsetY;

    HBRUSH brush = CreateSolidBrush(palette.hoverBackground);
    HPEN pen = CreatePen(PS_SOLID, 1,
        input.focused ? palette.accent : palette.separator);
    if (brush && pen)
    {
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        const int radius = metrics.selectionRadius * 2;
        RoundRect(dc, field.left, field.top, field.right, field.bottom,
            radius, radius);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
    }
    if (pen) DeleteObject(pen);
    if (brush) DeleteObject(brush);

    const int oldMode = SetBkMode(dc, TRANSPARENT);
    RECT glyphBounds = field;
    glyphBounds.left += metrics.leftPadding / 2;
    glyphBounds.right = glyphBounds.left + metrics.iconColumnWidth;
    HGDIOBJ oldFont = SelectObject(dc,
        iconFont ? static_cast<HGDIOBJ>(iconFont)
                 : GetStockObject(DEFAULT_GUI_FONT));
    DrawGlyphLayer(dc, item.glyph, glyphBounds, palette.disabledText,
        &field);
    if (oldFont) SelectObject(dc, oldFont);

    const std::wstring committed = input.text ? input.text : L"";
    const size_t cursor = std::min(input.cursor, committed.size());
    const size_t anchor = std::min(
        input.selectionAnchor, committed.size());
    const size_t selectionStart = std::min(cursor, anchor);
    const size_t selectionEnd = std::max(cursor, anchor);
    const std::wstring composition = input.compositionText
        ? input.compositionText : L"";
    std::wstring display = committed;
    size_t displayCursor = cursor;
    size_t compositionStart = 0;
    if (!composition.empty())
    {
        display = committed.substr(0, selectionStart);
        display += composition;
        display += committed.substr(selectionEnd);
        compositionStart = selectionStart;
        displayCursor = compositionStart + std::min(
            input.compositionCursor, composition.size());
    }
    const bool showingPlaceholder = display.empty() && !input.focused;
    const std::wstring visibleText = showingPlaceholder
        ? std::wstring(item.label ? item.label : L"") : display;
    RECT textBounds = field;
    textBounds.left = glyphBounds.right + metrics.textGap;
    textBounds.right -= metrics.rightPadding;
    const COLORREF oldColor = SetTextColor(dc,
        showingPlaceholder ? palette.disabledText : palette.text);
    oldFont = SelectObject(dc,
        textFont ? static_cast<HGDIOBJ>(textFont)
                 : GetStockObject(DEFAULT_GUI_FONT));

    const auto measurePrefix = [&](size_t length) {
        SIZE size{};
        const size_t safeLength = std::min(length, display.size());
        if (safeLength > 0)
        {
            GetTextExtentPoint32W(dc, display.data(),
                static_cast<int>(safeLength), &size);
        }
        return static_cast<int>(size.cx);
    };
    const int availableWidth = std::max<LONG>(
        1, textBounds.right - textBounds.left);
    const int caretAdvance = measurePrefix(displayCursor);
    const int horizontalOffset = std::max(
        0, caretAdvance - availableWidth + metrics.outerInset * 2);
    RECT drawBounds = textBounds;
    drawBounds.left -= horizontalOffset;
    drawBounds.right += horizontalOffset;
    OpticallyCenterTextBounds(dc, visibleText, drawBounds);
    const int savedDc = SaveDC(dc);
    IntersectClipRect(dc, textBounds.left, textBounds.top,
        textBounds.right, textBounds.bottom);

    if (input.focused && composition.empty() && cursor != anchor)
    {
        RECT selection = textBounds;
        selection.left += measurePrefix(selectionStart) - horizontalOffset;
        selection.right = textBounds.left +
            measurePrefix(selectionEnd) - horizontalOffset;
        selection.top += metrics.outerInset;
        selection.bottom -= metrics.outerInset;
        HBRUSH selectionBrush = CreateSolidBrush(palette.separator);
        if (selectionBrush)
        {
            FillRect(dc, &selection, selectionBrush);
            DeleteObject(selectionBrush);
        }
    }

    DrawTextW(dc, visibleText.c_str(), static_cast<int>(visibleText.size()),
        &drawBounds, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    if (input.focused && !composition.empty())
    {
        const int compositionLeft = textBounds.left +
            measurePrefix(compositionStart) - horizontalOffset;
        const int compositionRight = textBounds.left +
            measurePrefix(compositionStart + composition.size()) -
                horizontalOffset;
        HPEN compositionPen = CreatePen(PS_SOLID, 1, palette.text);
        if (compositionPen)
        {
            HGDIOBJ oldCompositionPen = SelectObject(dc, compositionPen);
            const int y = textBounds.bottom - metrics.outerInset;
            MoveToEx(dc, compositionLeft, y, nullptr);
            LineTo(dc, std::max(compositionLeft + 1, compositionRight), y);
            SelectObject(dc, oldCompositionPen);
            DeleteObject(compositionPen);
        }
    }

    if (input.focused && input.caretVisible)
    {
        const int caretX = std::clamp(
            static_cast<int>(textBounds.left) + caretAdvance -
                horizontalOffset,
            static_cast<int>(textBounds.left),
            std::max(static_cast<int>(textBounds.left),
                static_cast<int>(textBounds.right) - 1));
        HPEN caretPen = CreatePen(PS_SOLID, 1, palette.accent);
        if (caretPen)
        {
            HGDIOBJ oldCaretPen = SelectObject(dc, caretPen);
            MoveToEx(dc, caretX, field.top + metrics.outerInset, nullptr);
            LineTo(dc, caretX, field.bottom - metrics.outerInset);
            SelectObject(dc, oldCaretPen);
            DeleteObject(caretPen);
        }
    }
    RestoreDC(dc, savedDc);
    if (oldFont) SelectObject(dc, oldFont);
    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldMode);
    return true;
}

} // namespace snowdesktop::menu_icon

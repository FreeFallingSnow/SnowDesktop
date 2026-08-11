#include "icon_beautify.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <tuple>
#include <unordered_map>

// Shape constants and cubic paths are adapted from DeskMakeover's MIT-licensed
// dm-icon-core shape catalog (Copyright (c) 2026 Jinming Yang). The complete
// upstream notice and source URL are recorded in THIRD_PARTY_NOTICES.md.

namespace snowdesktop::icon_beautify
{
InteractionAction AdvanceHoverPreview(HoverPreviewState& state,
    int hoveredCandidate, int clickedCandidate, std::uint32_t now)
{
    if (clickedCandidate >= 0)
    {
        state = {};
        return InteractionAction::Commit;
    }
    if (hoveredCandidate != state.candidate)
    {
        const bool restore = state.previewApplied;
        state.candidate = hoveredCandidate;
        state.startedTick = now;
        state.previewApplied = false;
        return restore ? InteractionAction::Restore : InteractionAction::None;
    }
    if (hoveredCandidate >= 0 && !state.previewApplied &&
        now - state.startedTick >= 90)
    {
        state.previewApplied = true;
        return InteractionAction::Preview;
    }
    return InteractionAction::None;
}

InteractionAction AdvanceContinuousPreview(ContinuousPreviewState& state,
    bool changed, bool deactivatedAfterEdit, std::uint32_t now)
{
    if (deactivatedAfterEdit)
        return InteractionAction::Commit;
    if (changed && (state.lastPreviewTick == 0 ||
        now - state.lastPreviewTick >= 100))
    {
        state.lastPreviewTick = now;
        return InteractionAction::Preview;
    }
    return InteractionAction::None;
}

namespace
{
constexpr float kLegacyCornerRadiusRatio = 0.35f;
constexpr float kLegacyCornerExponent = 4.0f;

struct Point
{
    float x = 0.0f;
    float y = 0.0f;
};

struct Bounds
{
    bool valid = false;
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

std::uint8_t PixelA(std::uint32_t pixel)
{
    return static_cast<std::uint8_t>((pixel >> 24) & 0xff);
}

std::uint32_t PackBgra(int b, int g, int r, int a)
{
    return (static_cast<std::uint32_t>(std::clamp(a, 0, 255)) << 24) |
        (static_cast<std::uint32_t>(std::clamp(r, 0, 255)) << 16) |
        (static_cast<std::uint32_t>(std::clamp(g, 0, 255)) << 8) |
        static_cast<std::uint32_t>(std::clamp(b, 0, 255));
}

std::uint32_t PackPremultiplied(int r, int g, int b, int a)
{
    return PackBgra((b * a + 127) / 255, (g * a + 127) / 255,
        (r * a + 127) / 255, a);
}

std::uint32_t SourceOver(std::uint32_t src, std::uint32_t dst)
{
    const int sa = PixelA(src);
    if (sa == 0) return dst;
    if (sa == 255) return src;
    const int inv = 255 - sa;
    return PackBgra(
        static_cast<int>(src & 0xff) +
            (static_cast<int>(dst & 0xff) * inv + 127) / 255,
        static_cast<int>((src >> 8) & 0xff) +
            (static_cast<int>((dst >> 8) & 0xff) * inv + 127) / 255,
        static_cast<int>((src >> 16) & 0xff) +
            (static_cast<int>((dst >> 16) & 0xff) * inv + 127) / 255,
        sa + (PixelA(dst) * inv + 127) / 255);
}

std::uint32_t ScalePixel(std::uint32_t pixel, int scale)
{
    if (scale <= 0 || PixelA(pixel) == 0) return 0;
    if (scale >= 255) return pixel;
    return PackBgra(
        (static_cast<int>(pixel & 0xff) * scale + 127) / 255,
        (static_cast<int>((pixel >> 8) & 0xff) * scale + 127) / 255,
        (static_cast<int>((pixel >> 16) & 0xff) * scale + 127) / 255,
        (PixelA(pixel) * scale + 127) / 255);
}

void SampleCubic(std::vector<Point>& points, Point p0, Point c1,
    Point c2, Point p1, int steps = 24)
{
    for (int i = 1; i <= steps; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float u = 1.0f - t;
        points.push_back(Point{
            u * u * u * p0.x + 3.0f * u * u * t * c1.x +
                3.0f * u * t * t * c2.x + t * t * t * p1.x,
            u * u * u * p0.y + 3.0f * u * u * t * c1.y +
                3.0f * u * t * t * c2.y + t * t * t * p1.y,
        });
    }
}

std::vector<Point> AppleOutline()
{
    // The continuous-corner curve uses the public iOS control points also used
    // by DeskMakeover. Coordinates are normalized to the unit square.
    constexpr float r = 0.225f;
    auto tl = [](float x, float y) { return Point{ x * r, y * r }; };
    auto tr = [](float x, float y) { return Point{ 1.0f - x * r, y * r }; };
    auto br = [](float x, float y) { return Point{ 1.0f - x * r, 1.0f - y * r }; };
    auto bl = [](float x, float y) { return Point{ x * r, 1.0f - y * r }; };

    std::vector<Point> points;
    points.reserve(160);
    Point cur = tl(1.528665f, 0.0f);
    points.push_back(cur);
    cur = tr(1.528665f, 0.0f); points.push_back(cur);
    auto corner = [&](Point a1, Point a2, Point a3,
        Point b1, Point b2, Point b3,
        Point c1, Point c2, Point c3) {
        SampleCubic(points, cur, a1, a2, a3, 12); cur = a3;
        SampleCubic(points, cur, b1, b2, b3, 12); cur = b3;
        SampleCubic(points, cur, c1, c2, c3, 12); cur = c3;
    };
    corner(tr(1.08849296f, 0), tr(0.86840694f, 0), tr(0.63149379f, 0.07491139f),
        tr(0.37282383f, 0.16905956f), tr(0.16905956f, 0.37282383f), tr(0.07491139f, 0.63149379f),
        tr(0, 0.86840694f), tr(0, 1.08849296f), tr(0, 1.52866498f));
    cur = br(0, 1.528665f); points.push_back(cur);
    corner(br(0, 1.08849296f), br(0, 0.86840694f), br(0.07491139f, 0.63149379f),
        br(0.16905956f, 0.37282383f), br(0.37282383f, 0.16905956f), br(0.63149379f, 0.07491139f),
        br(0.86840694f, 0), br(1.08849296f, 0), br(1.52866498f, 0));
    cur = bl(1.528665f, 0); points.push_back(cur);
    corner(bl(1.08849296f, 0), bl(0.86840694f, 0), bl(0.63149379f, 0.07491139f),
        bl(0.37282383f, 0.16905956f), bl(0.16905956f, 0.37282383f), bl(0.07491139f, 0.63149379f),
        bl(0, 0.86840694f), bl(0, 1.08849296f), bl(0, 1.52866498f));
    cur = tl(0, 1.528665f); points.push_back(cur);
    corner(tl(0, 1.08849296f), tl(0, 0.86840694f), tl(0.07491139f, 0.63149379f),
        tl(0.16905956f, 0.37282383f), tl(0.37282383f, 0.16905956f), tl(0.63149379f, 0.07491139f),
        tl(0.86840694f, 0), tl(1.08849296f, 0), tl(1.52866498f, 0));
    return points;
}

std::vector<Point> FourCubicOutline(Point start,
    const std::array<std::array<Point, 3>, 4>& curves)
{
    std::vector<Point> points;
    points.push_back(start);
    Point cur = start;
    for (const auto& curve : curves)
    {
        SampleCubic(points, cur, curve[0], curve[1], curve[2]);
        cur = curve[2];
    }
    return points;
}

const std::vector<Point>& OutlineFor(IconBeautifyShape shape)
{
    static const std::vector<Point> apple = AppleOutline();
    static const std::vector<Point> samsung = FourCubicOutline(
        { 0.5f, 0.0f }, {{
            {{{0.1f,0.0f},{0.0f,0.1f},{0.0f,0.5f}}},
            {{{0.0f,0.9f},{0.1f,1.0f},{0.5f,1.0f}}},
            {{{0.9f,1.0f},{1.0f,0.9f},{1.0f,0.5f}}},
            {{{1.0f,0.1f},{0.9f,0.0f},{0.5f,0.0f}}},
        }});
    static const std::vector<Point> teardrop = FourCubicOutline(
        { 0.50f, 0.02f }, {{
            {{{0.72f,0.12f},{0.98f,0.34f},{0.98f,0.58f}}},
            {{{0.98f,0.84f},{0.78f,0.98f},{0.52f,0.98f}}},
            {{{0.22f,0.98f},{0.04f,0.80f},{0.04f,0.56f}}},
            {{{0.04f,0.34f},{0.25f,0.12f},{0.50f,0.02f}}},
        }});
    static const std::vector<Point> lemon = FourCubicOutline(
        { 0.12f, 0.12f }, {{
            {{{0.38f,-0.02f},{0.82f,0.05f},{0.95f,0.36f}}},
            {{{1.04f,0.58f},{0.91f,0.90f},{0.72f,0.96f}}},
            {{{0.42f,1.06f},{0.08f,0.88f},{0.03f,0.62f}}},
            {{{-0.02f,0.38f},{0.02f,0.22f},{0.12f,0.12f}}},
        }});
    static const std::vector<Point> flower = [] {
        std::vector<Point> p;
        Point cur{0.5f, 0.0f}; p.push_back(cur);
        const std::array<std::array<Point, 3>, 12> curves{{
            {{{.606f,0},{.699f,.053f},{.756f,.135f}}},
            {{{.7856f,.1781f},{.8229f,.2154f},{.866f,.245f}}},
            {{{.95f,.3027f},{1.0001f,.3981f},{1,.5f}}},
            {{{1,.606f},{.947f,.699f},{.865f,.756f}}},
            {{{.8219f,.7856f},{.7846f,.8229f},{.755f,.866f}}},
            {{{.6973f,.95f},{.6019f,1.0001f},{.5f,1}}},
            {{{.394f,1},{.301f,.947f},{.244f,.865f}}},
            {{{.2144f,.8219f},{.1771f,.7846f},{.134f,.755f}}},
            {{{.05f,.6973f},{-.0001f,.6019f},{0,.5f}}},
            {{{0,.394f},{.053f,.301f},{.135f,.244f}}},
            {{{.1781f,.2144f},{.2154f,.1771f},{.245f,.134f}}},
            {{{.3027f,.05f},{.3981f,-.0001f},{.5f,0}}},
        }};
        for (const auto& c : curves) { SampleCubic(p, cur, c[0], c[1], c[2]); cur = c[2]; }
        return p;
    }();
    static const std::vector<Point> pebble = FourCubicOutline(
        { 0.55f, 0.0f }, {{
            {{{0.25f,0.0f},{0.0f,0.25f},{0.0f,0.5f}}},
            {{{0.0f,0.78f},{0.28f,1.0f},{0.55f,1.0f}}},
            {{{0.85f,1.0f},{1.0f,0.85f},{1.0f,0.58f}}},
            {{{1.0f,0.30f},{0.86f,0.0f},{0.55f,0.0f}}},
        }});
    static const std::vector<Point> bookmark{
        {.12f,.04f},{.88f,.04f},{.94f,.10f},{.94f,.94f},
        {.50f,.72f},{.06f,.94f},{.06f,.10f}
    };
    static const std::vector<Point> diamond{
        {.50f,.02f},{.98f,.50f},{.50f,.98f},{.02f,.50f}
    };
    switch (shape)
    {
    case IconBeautifyShape::Apple: return apple;
    case IconBeautifyShape::Samsung: return samsung;
    case IconBeautifyShape::Teardrop: return teardrop;
    case IconBeautifyShape::Bookmark: return bookmark;
    case IconBeautifyShape::Lemon: return lemon;
    case IconBeautifyShape::Diamond: return diamond;
    case IconBeautifyShape::Flower: return flower;
    case IconBeautifyShape::Pebble: return pebble;
    default: return apple;
    }
}

bool PointInPolygon(const std::vector<Point>& points, float x, float y)
{
    bool inside = false;
    size_t j = points.size() - 1;
    for (size_t i = 0; i < points.size(); ++i)
    {
        const Point& a = points[i];
        const Point& b = points[j];
        if ((a.y > y) != (b.y > y) &&
            x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x)
            inside = !inside;
        j = i;
    }
    return inside;
}

bool Contains(IconBeautifyShape shape, float x, float y)
{
    if (x < 0.0f || y < 0.0f || x > 1.0f || y > 1.0f)
        return false;
    if (shape == IconBeautifyShape::Circle)
    {
        const float dx = (x - 0.5f) * 2.0f;
        const float dy = (y - 0.5f) * 2.0f;
        return dx * dx + dy * dy <= 1.0f;
    }
    if (shape == IconBeautifyShape::RoundedSquare)
    {
        constexpr float radius = 0.12f;
        const float dx = std::max(std::abs(x - 0.5f) - (0.5f - radius), 0.0f);
        const float dy = std::max(std::abs(y - 0.5f) - (0.5f - radius), 0.0f);
        return dx * dx + dy * dy <= radius * radius;
    }
    return PointInPolygon(OutlineFor(shape), x, y);
}

int LegacyMaskAlpha(int x, int y, int width, int height, float inset)
{
    const float innerWidth = static_cast<float>(width) - inset * 2.0f;
    const float innerHeight = static_cast<float>(height) - inset * 2.0f;
    if (innerWidth <= 0.0f || innerHeight <= 0.0f) return 0;
    const float radius = std::max(1.0f,
        std::max(6.0f, std::min(static_cast<float>(width), static_cast<float>(height)) *
            kLegacyCornerRadiusRatio) - inset);
    const float px = static_cast<float>(x) + 0.5f - inset;
    const float py = static_cast<float>(y) + 0.5f - inset;
    if (px < -0.5f || py < -0.5f || px > innerWidth + 0.5f || py > innerHeight + 0.5f)
        return 0;
    const float left = radius;
    const float top = radius;
    const float right = innerWidth - radius;
    const float bottom = innerHeight - radius;
    float dx = 0.0f;
    if (px < left) dx = left - px;
    else if (px > right) dx = px - right;
    float dy = 0.0f;
    if (py < top) dy = top - py;
    else if (py > bottom) dy = py - bottom;
    const float distance = std::pow(
        std::pow(dx, kLegacyCornerExponent) + std::pow(dy, kLegacyCornerExponent),
        1.0f / kLegacyCornerExponent);
    return static_cast<int>(std::round(
        std::clamp(radius + 0.5f - distance, 0.0f, 1.0f) * 255.0f));
}

struct MaskKey
{
    int shape = 0;
    int width = 0;
    int height = 0;
    int inset100 = 0;
    bool operator==(const MaskKey&) const = default;
};

struct MaskKeyHash
{
    size_t operator()(const MaskKey& key) const noexcept
    {
        size_t result = static_cast<size_t>(key.shape + 31);
        result = result * 1315423911u + static_cast<size_t>(key.width);
        result = result * 1315423911u + static_cast<size_t>(key.height);
        result = result * 1315423911u + static_cast<size_t>(key.inset100);
        return result;
    }
};

const std::vector<std::uint8_t>& CachedMask(IconBeautifyShape shape, int width, int height, float inset)
{
    static std::mutex mutex;
    static std::unordered_map<MaskKey, std::vector<std::uint8_t>, MaskKeyHash> cache;
    const MaskKey key{ static_cast<int>(shape), width, height,
        static_cast<int>(std::round(inset * 100.0f)) };
    std::scoped_lock lock(mutex);
    auto found = cache.find(key);
    if (found != cache.end()) return found->second;
    std::vector<std::uint8_t> mask(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            mask[static_cast<size_t>(y) * width + x] =
                ShapeMaskAlpha(shape, x, y, width, height, inset);
    return cache.emplace(key, std::move(mask)).first->second;
}

Bounds VisibleBounds(const std::vector<std::uint32_t>& pixels, int width, int height)
{
    Bounds result{ false, width, height, -1, -1 };
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            if (PixelA(pixels[static_cast<size_t>(y) * width + x]) > 16)
            {
                result.valid = true;
                result.left = std::min(result.left, x);
                result.top = std::min(result.top, y);
                result.right = std::max(result.right, x);
                result.bottom = std::max(result.bottom, y);
            }
    return result;
}

EdgeColor ColorFromFloats(float r, float g, float b)
{
    return EdgeColor{
        static_cast<int>(std::round(std::clamp(r, 0.0f, 1.0f) * 255.0f)),
        static_cast<int>(std::round(std::clamp(g, 0.0f, 1.0f) * 255.0f)),
        static_cast<int>(std::round(std::clamp(b, 0.0f, 1.0f) * 255.0f)) };
}

int Luma(const EdgeColor& color)
{
    return (color.r * 299 + color.g * 587 + color.b * 114) / 1000;
}

EdgeColor Mix(const EdgeColor& a, const EdgeColor& b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return EdgeColor{
        static_cast<int>(std::round(a.r + (b.r - a.r) * t)),
        static_cast<int>(std::round(a.g + (b.g - a.g) * t)),
        static_cast<int>(std::round(a.b + (b.b - a.b) * t)) };
}

EdgeColor AutoOutline(const EdgeColor& a, const EdgeColor& b)
{
    EdgeColor mid = Mix(a, b, 0.5f);
    const int delta = Luma(mid) >= 128 ? -34 : 34;
    mid.r = std::clamp(mid.r + delta, 0, 255);
    mid.g = std::clamp(mid.g + delta, 0, 255);
    mid.b = std::clamp(mid.b + delta, 0, 255);
    return mid;
}

std::uint32_t SampleBilinear(const std::vector<std::uint32_t>& pixels,
    int width, int height, float x, float y)
{
    x = std::clamp(x, 0.0f, static_cast<float>(std::max(0, width - 1)));
    y = std::clamp(y, 0.0f, static_cast<float>(std::max(0, height - 1)));
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int x1 = std::min(width - 1, x0 + 1);
    const int y1 = std::min(height - 1, y0 + 1);
    const float fx = x - x0;
    const float fy = y - y0;
    const std::uint32_t p00 = pixels[static_cast<size_t>(y0) * width + x0];
    const std::uint32_t p10 = pixels[static_cast<size_t>(y0) * width + x1];
    const std::uint32_t p01 = pixels[static_cast<size_t>(y1) * width + x0];
    const std::uint32_t p11 = pixels[static_cast<size_t>(y1) * width + x1];
    auto channel = [&](int shift) {
        const float top = static_cast<float>((p00 >> shift) & 0xff) +
            (static_cast<float>((p10 >> shift) & 0xff) - static_cast<float>((p00 >> shift) & 0xff)) * fx;
        const float bottom = static_cast<float>((p01 >> shift) & 0xff) +
            (static_cast<float>((p11 >> shift) & 0xff) - static_cast<float>((p01 >> shift) & 0xff)) * fx;
        return static_cast<int>(std::round(top + (bottom - top) * fy));
    };
    return PackBgra(channel(0), channel(8), channel(16), channel(24));
}

void FillPlate(std::vector<std::uint32_t>& output, int width, int height,
    const IconBeautifySettings& settings, bool legacyExact)
{
    const EdgeColor start = ColorFromFloats(settings.backgroundStartR,
        settings.backgroundStartG, settings.backgroundStartB);
    const EdgeColor end = settings.gradientEnabled
        ? ColorFromFloats(settings.backgroundEndR, settings.backgroundEndG, settings.backgroundEndB)
        : start;
    const EdgeColor automatic = AutoOutline(start, end);
    const auto& mask = CachedMask(settings.shape, width, height, 0.0f);
    const auto& inner = CachedMask(settings.shape, width, height,
        legacyExact ? 1.0f : std::max(0.0f, settings.outlineWidth));
    const int plateOpacity = static_cast<int>(std::round(settings.backgroundOpacity * 255.0f));
    for (int y = 0; y < height; ++y)
    {
        const float yt = height > 1 ? static_cast<float>(y) / (height - 1) : 0.0f;
        for (int x = 0; x < width; ++x)
        {
            const float xt = width > 1 ? static_cast<float>(x) / (width - 1) : 0.0f;
            float t = 0.0f;
            if (settings.gradientEnabled)
            {
                switch (settings.gradientDirection)
                {
                case 1: t = xt; break;
                case 2: t = (xt + yt) * 0.5f; break;
                case 3: t = (xt + 1.0f - yt) * 0.5f; break;
                default: t = yt; break;
                }
            }
            EdgeColor fill = Mix(start, end, t);
            const size_t index = static_cast<size_t>(y) * width + x;
            if (legacyExact && mask[index] > 0)
            {
                const float edge = static_cast<float>(std::clamp<int>(
                    mask[index] - inner[index], 0, 255)) / mask[index];
                fill = Mix(fill, automatic, edge);
            }
            const int alpha = (mask[index] * plateOpacity + 127) / 255;
            output[index] = PackPremultiplied(fill.r, fill.g, fill.b, alpha);
        }
    }
}

void ApplyFinish(std::vector<std::uint32_t>& output, int width, int height,
    const IconBeautifySettings& settings)
{
    if (settings.finish == IconBeautifyFinish::Flat) return;
    const auto& mask = CachedMask(settings.shape, width, height, 0.0f);
    const auto& inner = CachedMask(settings.shape, width, height,
        settings.finish == IconBeautifyFinish::Sticker ? 2.5f : 1.25f);
    for (int y = 0; y < height; ++y)
    {
        const float yt = (static_cast<float>(y) + 0.5f) / height;
        for (int x = 0; x < width; ++x)
        {
            const size_t index = static_cast<size_t>(y) * width + x;
            if (mask[index] == 0) continue;
            const float xt = (static_cast<float>(x) + 0.5f) / width;
            int whiteAlpha = 0;
            int darkAlpha = 0;
            switch (settings.finish)
            {
            case IconBeautifyFinish::Gloss:
            {
                const float sweep = std::clamp(1.0f - (yt * 1.6f + xt * 0.32f), 0.0f, 1.0f);
                whiteAlpha = static_cast<int>(std::round(sweep * sweep * 72.0f));
                break;
            }
            case IconBeautifyFinish::Glass:
                whiteAlpha = static_cast<int>(std::round(
                    std::clamp((0.52f - yt) / 0.52f, 0.0f, 1.0f) * 50.0f));
                darkAlpha = static_cast<int>(std::round(
                    std::clamp((yt - 0.48f) / 0.52f, 0.0f, 1.0f) * 24.0f));
                whiteAlpha += std::clamp<int>(mask[index] - inner[index], 0, 255) * 42 / 255;
                break;
            case IconBeautifyFinish::Sticker:
                whiteAlpha = std::clamp<int>(mask[index] - inner[index], 0, 255) * 228 / 255;
                break;
            default:
                break;
            }
            whiteAlpha = whiteAlpha * mask[index] / 255;
            darkAlpha = darkAlpha * mask[index] / 255;
            if (darkAlpha > 0)
                output[index] = SourceOver(PackPremultiplied(18, 24, 34, darkAlpha), output[index]);
            if (whiteAlpha > 0)
                output[index] = SourceOver(PackPremultiplied(255, 255, 255, whiteAlpha), output[index]);
        }
    }
}

void ApplyOutline(std::vector<std::uint32_t>& output, int width, int height,
    const IconBeautifySettings& settings, std::optional<EdgeColor> edgeFill)
{
    if (settings.outlineMode == IconBeautifyOutlineMode::None || settings.outlineWidth <= 0.0f)
        return;
    EdgeColor stroke{};
    if (settings.outlineMode == IconBeautifyOutlineMode::Custom)
        stroke = ColorFromFloats(settings.outlineR, settings.outlineG, settings.outlineB);
    else if (edgeFill)
        stroke = Luma(*edgeFill) >= 128 ? EdgeColor{160, 170, 188} : EdgeColor{218, 225, 238};
    else
        stroke = AutoOutline(
            ColorFromFloats(settings.backgroundStartR, settings.backgroundStartG, settings.backgroundStartB),
            ColorFromFloats(settings.backgroundEndR, settings.backgroundEndG, settings.backgroundEndB));
    const auto& mask = CachedMask(settings.shape, width, height, 0.0f);
    const auto& inner = CachedMask(settings.shape, width, height, settings.outlineWidth);
    const int opacity = static_cast<int>(std::round(settings.outlineOpacity * 255.0f));
    for (size_t i = 0; i < output.size(); ++i)
    {
        const int edgeAlpha = std::clamp<int>(mask[i] - inner[i], 0, 255);
        const int alpha = edgeAlpha * opacity / 255;
        if (alpha > 0)
            output[i] = SourceOver(PackPremultiplied(stroke.r, stroke.g, stroke.b, alpha), output[i]);
    }
}
}

IconBeautifySettings Normalize(IconBeautifySettings settings)
{
    settings.mode = std::clamp(settings.mode, 0, 1);
    settings.backgroundOpacity = std::clamp(settings.backgroundOpacity, 0.0f, 1.0f);
    settings.gradientDirection = std::clamp(settings.gradientDirection, 0, 3);
    settings.backgroundStartR = std::clamp(settings.backgroundStartR, 0.0f, 1.0f);
    settings.backgroundStartG = std::clamp(settings.backgroundStartG, 0.0f, 1.0f);
    settings.backgroundStartB = std::clamp(settings.backgroundStartB, 0.0f, 1.0f);
    settings.backgroundEndR = std::clamp(settings.backgroundEndR, 0.0f, 1.0f);
    settings.backgroundEndG = std::clamp(settings.backgroundEndG, 0.0f, 1.0f);
    settings.backgroundEndB = std::clamp(settings.backgroundEndB, 0.0f, 1.0f);
    settings.shape = static_cast<IconBeautifyShape>(std::clamp(static_cast<int>(settings.shape), 0, 10));
    settings.contentScale = std::clamp(settings.contentScale, 0.50f, 0.90f);
    settings.finish = static_cast<IconBeautifyFinish>(std::clamp(static_cast<int>(settings.finish), 0, 3));
    settings.outlineMode = static_cast<IconBeautifyOutlineMode>(
        std::clamp(static_cast<int>(settings.outlineMode), 0, 2));
    settings.outlineWidth = std::clamp(settings.outlineWidth, 0.0f, 4.0f);
    settings.outlineOpacity = std::clamp(settings.outlineOpacity, 0.0f, 1.0f);
    settings.outlineR = std::clamp(settings.outlineR, 0.0f, 1.0f);
    settings.outlineG = std::clamp(settings.outlineG, 0.0f, 1.0f);
    settings.outlineB = std::clamp(settings.outlineB, 0.0f, 1.0f);
    settings.shadowStrength = std::clamp(settings.shadowStrength, 0.0f, 1.0f);
    return settings;
}

bool Equal(const IconBeautifySettings& lhs, const IconBeautifySettings& rhs)
{
    const IconBeautifySettings a = Normalize(lhs);
    const IconBeautifySettings b = Normalize(rhs);
    auto eq = [](float x, float y) { return std::abs(x - y) <= 0.0005f; };
    return a.enabled == b.enabled && a.mode == b.mode &&
        eq(a.backgroundOpacity, b.backgroundOpacity) &&
        a.gradientEnabled == b.gradientEnabled &&
        a.gradientDirection == b.gradientDirection &&
        eq(a.backgroundStartR, b.backgroundStartR) &&
        eq(a.backgroundStartG, b.backgroundStartG) &&
        eq(a.backgroundStartB, b.backgroundStartB) &&
        eq(a.backgroundEndR, b.backgroundEndR) &&
        eq(a.backgroundEndG, b.backgroundEndG) &&
        eq(a.backgroundEndB, b.backgroundEndB) &&
        a.shape == b.shape && eq(a.contentScale, b.contentScale) &&
        a.finish == b.finish && a.outlineMode == b.outlineMode &&
        eq(a.outlineWidth, b.outlineWidth) &&
        eq(a.outlineOpacity, b.outlineOpacity) &&
        eq(a.outlineR, b.outlineR) && eq(a.outlineG, b.outlineG) &&
        eq(a.outlineB, b.outlineB) && eq(a.shadowStrength, b.shadowStrength);
}

bool UsesLegacyGeometryDefaults(const IconBeautifySettings& settings)
{
    const IconBeautifySettings s = Normalize(settings);
    return s.shape == IconBeautifyShape::LegacyRounded && s.finish == IconBeautifyFinish::Flat &&
        s.outlineMode == IconBeautifyOutlineMode::Automatic &&
        std::abs(s.outlineWidth - 1.0f) <= 0.0005f &&
        std::abs(s.outlineOpacity - 1.0f) <= 0.0005f &&
        std::abs(s.contentScale - 0.68f) <= 0.0005f &&
        std::abs(s.shadowStrength - 0.35f) <= 0.0005f;
}

std::uint8_t ShapeMaskAlpha(IconBeautifyShape shape, int x, int y,
    int width, int height, float inset)
{
    if (width <= 0 || height <= 0 || x < 0 || y < 0 || x >= width || y >= height)
        return 0;
    inset = std::max(0.0f, inset);
    if (shape == IconBeautifyShape::LegacyRounded)
        return static_cast<std::uint8_t>(LegacyMaskAlpha(x, y, width, height, inset));
    const float innerWidth = static_cast<float>(width) - inset * 2.0f;
    const float innerHeight = static_cast<float>(height) - inset * 2.0f;
    if (innerWidth <= 0.0f || innerHeight <= 0.0f) return 0;
    constexpr std::array<float, 4> offsets{0.125f, 0.375f, 0.625f, 0.875f};
    int hits = 0;
    for (float oy : offsets)
        for (float ox : offsets)
        {
            const float nx = (static_cast<float>(x) + ox - inset) / innerWidth;
            const float ny = (static_cast<float>(y) + oy - inset) / innerHeight;
            if (Contains(shape, nx, ny)) ++hits;
        }
    return static_cast<std::uint8_t>((hits * 255 + 8) / 16);
}

std::vector<std::uint32_t> Render(const std::vector<std::uint32_t>& source,
    int width, int height, const IconBeautifySettings& rawSettings,
    std::optional<EdgeColor> detectedEdgeFill)
{
    if (width <= 0 || height <= 0 ||
        source.size() != static_cast<size_t>(width) * height)
        return source;
    const IconBeautifySettings settings = Normalize(rawSettings);
    if (!settings.enabled)
        return source;
    const Bounds bounds = VisibleBounds(source, width, height);
    if (!bounds.valid) return source;
    const bool legacyExact = UsesLegacyGeometryDefaults(settings);
    const auto& mask = CachedMask(settings.shape, width, height, 0.0f);
    std::vector<std::uint32_t> output(source.size(), 0);

    if (detectedEdgeFill)
    {
        const EdgeColor edge = *detectedEdgeFill;
        const std::uint32_t background = PackPremultiplied(edge.r, edge.g, edge.b, 255);
        if (legacyExact)
        {
            for (size_t i = 0; i < output.size(); ++i)
                output[i] = ScalePixel(SourceOver(source[i], background), mask[i]);
        }
        else
        {
            for (size_t i = 0; i < output.size(); ++i)
                output[i] = ScalePixel(background, mask[i]);
            const int destW = std::max(1, static_cast<int>(std::round(
                width * settings.contentScale)));
            const int destH = std::max(1, static_cast<int>(std::round(
                height * settings.contentScale)));
            const int destLeft = (width - destW) / 2;
            const int destTop = (height - destH) / 2;
            for (int y = 0; y < destH; ++y)
                for (int x = 0; x < destW; ++x)
                {
                    const float sx = ((static_cast<float>(x) + 0.5f) /
                        destW) * width - 0.5f;
                    const float sy = ((static_cast<float>(y) + 0.5f) /
                        destH) * height - 0.5f;
                    const int outX = destLeft + x;
                    const int outY = destTop + y;
                    const size_t index = static_cast<size_t>(outY) * width + outX;
                    output[index] = ScalePixel(SourceOver(
                        SampleBilinear(source, width, height, sx, sy),
                        background), mask[index]);
                }
        }
        ApplyFinish(output, width, height, settings);
        if (legacyExact)
        {
            if (Luma(edge) >= 232)
            {
                IconBeautifySettings legacyOutline = settings;
                legacyOutline.outlineMode = IconBeautifyOutlineMode::Custom;
                legacyOutline.outlineR = 190.0f / 255.0f;
                legacyOutline.outlineG = 199.0f / 255.0f;
                legacyOutline.outlineB = 214.0f / 255.0f;
                legacyOutline.outlineOpacity = 150.0f / 255.0f;
                ApplyOutline(output, width, height, legacyOutline, edge);
            }
        }
        else
            ApplyOutline(output, width, height, settings, edge);
        return output;
    }

    FillPlate(output, width, height, settings, legacyExact);
    const int sourceW = std::max(1, bounds.right - bounds.left + 1);
    const int sourceH = std::max(1, bounds.bottom - bounds.top + 1);
    int maxW = 0;
    int maxH = 0;
    if (legacyExact)
    {
        const int padding = std::max(5,
            static_cast<int>(std::round(std::min(width, height) * 0.16f)));
        maxW = std::max(1, width - padding * 2);
        maxH = std::max(1, height - padding * 2);
    }
    else
    {
        maxW = std::max(1, static_cast<int>(std::round(width * settings.contentScale)));
        maxH = std::max(1, static_cast<int>(std::round(height * settings.contentScale)));
    }
    const float scale = std::min(static_cast<float>(maxW) / sourceW,
        static_cast<float>(maxH) / sourceH);
    const int destW = std::max(1, static_cast<int>(std::round(sourceW * scale)));
    const int destH = std::max(1, static_cast<int>(std::round(sourceH * scale)));
    const int destLeft = (width - destW) / 2;
    const int destTop = (height - destH) / 2;

    constexpr std::array<std::tuple<int, int, int>, 7> passes{{
        {0,1,120},{-1,1,63},{1,1,63},{0,2,51},{-1,0,40},{1,0,40},{0,-1,29}
    }};
    for (int y = 0; y < destH; ++y)
        for (int x = 0; x < destW; ++x)
        {
            const float sx = bounds.left +
                ((static_cast<float>(x) + 0.5f) / destW) * sourceW - 0.5f;
            const float sy = bounds.top +
                ((static_cast<float>(y) + 0.5f) / destH) * sourceH - 0.5f;
            const std::uint32_t sampled = SampleBilinear(source, width, height, sx, sy);
            if (PixelA(sampled) == 0) continue;
            const int ox = destLeft + x;
            const int oy = destTop + y;
            for (const auto& [dx, dy, baseOpacity] : passes)
            {
                const int px = ox + dx;
                const int py = oy + dy;
                if (px < 0 || py < 0 || px >= width || py >= height) continue;
                const size_t index = static_cast<size_t>(py) * width + px;
                const int opacity = static_cast<int>(std::round(baseOpacity * settings.shadowStrength));
                const int alpha = (PixelA(sampled) * mask[index] * opacity +
                    255 * 255 / 2) / (255 * 255);
                if (alpha > 0)
                    output[index] = SourceOver(PackPremultiplied(48, 58, 72, alpha), output[index]);
            }
        }

    for (int y = 0; y < destH; ++y)
        for (int x = 0; x < destW; ++x)
        {
            const float sx = bounds.left +
                ((static_cast<float>(x) + 0.5f) / destW) * sourceW - 0.5f;
            const float sy = bounds.top +
                ((static_cast<float>(y) + 0.5f) / destH) * sourceH - 0.5f;
            const int ox = destLeft + x;
            const int oy = destTop + y;
            const size_t index = static_cast<size_t>(oy) * width + ox;
            const std::uint32_t sampled = ScalePixel(
                SampleBilinear(source, width, height, sx, sy), mask[index]);
            output[index] = SourceOver(sampled, output[index]);
        }

    ApplyFinish(output, width, height, settings);
    if (!legacyExact)
        ApplyOutline(output, width, height, settings, std::nullopt);
    return output;
}
}

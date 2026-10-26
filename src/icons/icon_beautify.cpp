#include "icon_beautify.h"
#include "icon_beautify_preview.h"
#include "icon_render_rules.h"
#include "theme/flat_glass_rim.h"
#include "theme/appearance_edge_presets.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <mutex>
#include <tuple>
#include <unordered_map>

// Shape constants and cubic paths are adapted from DeskMakeover's MIT-licensed
// dm-icon-core shape catalog (Copyright (c) 2026 Jinming Yang). The complete
// upstream notice and source URL are recorded in THIRD_PARTY_NOTICES.md.

namespace snowdesktop::icon_beautify
{
namespace
{
std::uint32_t SampleBilinear(const std::vector<std::uint32_t>& pixels,
    int width, int height, float x, float y);
}

bool PrepareCanvas(std::vector<std::uint32_t>& pixels, int& width, int& height, bool enabled)
{
    if (width <= 0 || height <= 0 || pixels.size() != static_cast<size_t>(width) * height) return false;
    if (!enabled) return true;
    const int sourceEdge = std::max(width, height);
    const int edge = std::max(sourceEdge, icon_render_rules::kMinimumSourcePixels);
    if (width == edge && height == edge) return true;
    if (static_cast<std::uint64_t>(edge) * edge > 16ull * 1024 * 1024) return false;
    std::vector<std::uint32_t> square(static_cast<size_t>(edge) * edge, 0);
    const float scale = static_cast<float>(edge) / sourceEdge;
    const int fittedWidth = std::max(1, static_cast<int>(std::lround(width * scale)));
    const int fittedHeight = std::max(1, static_cast<int>(std::lround(height * scale)));
    const int left = (edge - fittedWidth) / 2, top = (edge - fittedHeight) / 2;
    for (int y = 0; y < fittedHeight; ++y)
    {
        auto* row = square.data() + static_cast<size_t>(y + top) * edge + left;
        if (edge == sourceEdge)
            std::copy_n(pixels.data() + static_cast<size_t>(y) * width, width, row);
        else
            for (int x = 0; x < fittedWidth; ++x)
                row[x] = SampleBilinear(pixels, width, height,
                    (static_cast<float>(x) + 0.5f) * width / fittedWidth - 0.5f,
                    (static_cast<float>(y) + 0.5f) * height / fittedHeight - 0.5f);
    }
    width = height = edge; pixels = std::move(square);
    return true;
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

using Point = ShapePoint;

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

std::vector<Point> ContinuousRoundedOutline()
{
    // The continuous-corner curve uses normalized control points from the
    // DeskMakeover shape catalog.
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

std::vector<Point> LegacyRoundedOutline()
{
    constexpr float radius = kLegacyCornerRadiusRatio;
    constexpr float coordinatePower = 2.0f / kLegacyCornerExponent;
    constexpr float pi = 3.14159265358979323846f;
    constexpr int cornerSegments = 12;
    std::vector<Point> points;
    points.reserve(cornerSegments * 4 + 4);
    auto cornerPoint = [=](float centerX, float centerY, float angle) {
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        return Point{
            centerX + radius * std::copysign(
                std::pow(std::abs(cosine), coordinatePower), cosine),
            centerY + radius * std::copysign(
                std::pow(std::abs(sine), coordinatePower), sine),
        };
    };
    auto addCorner = [&](float centerX, float centerY,
        float startAngle, float endAngle) {
        for (int index = 1; index <= cornerSegments; ++index)
        {
            const float t = static_cast<float>(index) /
                static_cast<float>(cornerSegments);
            points.push_back(cornerPoint(centerX, centerY,
                startAngle + (endAngle - startAngle) * t));
        }
    };

    points.push_back({ radius, 0.0f });
    points.push_back({ 1.0f - radius, 0.0f });
    addCorner(1.0f - radius, radius, -pi * 0.5f, 0.0f);
    points.push_back({ 1.0f, 1.0f - radius });
    addCorner(1.0f - radius, 1.0f - radius, 0.0f, pi * 0.5f);
    points.push_back({ radius, 1.0f });
    addCorner(radius, 1.0f - radius, pi * 0.5f, pi);
    points.push_back({ 0.0f, radius });
    addCorner(radius, radius, pi, pi * 1.5f);
    return points;
}

std::vector<Point> CircleOutline()
{
    constexpr float pi = 3.14159265358979323846f;
    constexpr int segments = 64;
    std::vector<Point> points;
    points.reserve(segments);
    for (int index = 0; index < segments; ++index)
    {
        const float angle = -pi * 0.5f + 2.0f * pi *
            static_cast<float>(index) / static_cast<float>(segments);
        points.push_back({
            0.5f + std::cos(angle) * 0.5f,
            0.5f + std::sin(angle) * 0.5f,
        });
    }
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
    static const std::vector<Point> legacyRounded = LegacyRoundedOutline();
    static const std::vector<Point> continuousRounded = ContinuousRoundedOutline();
    static const std::vector<Point> circle = CircleOutline();
    static const std::vector<Point> softRounded = FourCubicOutline(
        { 0.5f, 0.0f }, {{
            {{{0.1f,0.0f},{0.0f,0.1f},{0.0f,0.5f}}},
            {{{0.0f,0.9f},{0.1f,1.0f},{0.5f,1.0f}}},
            {{{0.9f,1.0f},{1.0f,0.9f},{1.0f,0.5f}}},
            {{{1.0f,0.1f},{0.9f,0.0f},{0.5f,0.0f}}},
        }});
    static const std::vector<Point> pebble = FourCubicOutline(
        { 0.55f, 0.0f }, {{
            {{{0.25f,0.0f},{0.0f,0.25f},{0.0f,0.5f}}},
            {{{0.0f,0.78f},{0.28f,1.0f},{0.55f,1.0f}}},
            {{{0.85f,1.0f},{1.0f,0.85f},{1.0f,0.58f}}},
            {{{1.0f,0.30f},{0.86f,0.0f},{0.55f,0.0f}}},
        }});
    switch (shape)
    {
    case IconBeautifyShape::LegacyRounded: return legacyRounded;
    case IconBeautifyShape::ContinuousRounded: return continuousRounded;
    case IconBeautifyShape::Circle: return circle;
    case IconBeautifyShape::SoftRounded: return softRounded;
    case IconBeautifyShape::Pebble: return pebble;
    default: return legacyRounded;
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

struct MaskCache
{
    std::mutex mutex;
    std::unordered_map<MaskKey, std::vector<std::uint8_t>, MaskKeyHash> entries;
};

MaskCache& ShapeMasks()
{
    static MaskCache cache;
    return cache;
}

const std::vector<std::uint8_t>& CachedMask(IconBeautifyShape shape, int width, int height, float inset)
{
    auto& state = ShapeMasks();
    auto& cache = state.entries;
    const MaskKey key{ static_cast<int>(shape), width, height,
        static_cast<int>(std::round(inset * 100.0f)) };
    std::scoped_lock lock(state.mutex);
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
    const IconBeautifySettings& settings, bool legacyExact, int rasterScale)
{
    const EdgeColor start = ColorFromFloats(settings.backgroundStartR,
        settings.backgroundStartG, settings.backgroundStartB);
    const EdgeColor end = settings.gradientEnabled
        ? ColorFromFloats(settings.backgroundEndR, settings.backgroundEndG, settings.backgroundEndB)
        : start;
    const EdgeColor automatic = AutoOutline(start, end);
    const auto& mask = CachedMask(settings.shape, width, height, 0.0f);
    const auto& inner = CachedMask(settings.shape, width, height,
        (legacyExact ? 1.0f : std::max(0.0f, settings.outlineWidth)) * rasterScale);
    const int plateOpacity = static_cast<int>(std::round(settings.backgroundOpacity * 255.0f));
    const auto& gradient = settings.backgroundGradient;
    const auto line = ResolvePanelGradientLine(gradient, std::max(1, width - 1), std::max(1, height - 1));
    const double dx = line.x2 - line.x1, dy = line.y2 - line.y1;
    const double lengthSquared = dx * dx + dy * dy;
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
            int opacity = plateOpacity;
            if (gradient.enabled)
            {
                const double position = std::clamp(((x - line.x1) * dx + (y - line.y1) * dy) / lengthSquared, 0., 1.);
                auto right = std::upper_bound(gradient.stops.begin(), gradient.stops.end(), position,
                    [](double p, const auto& stop) { return p < stop.position; });
                if (right == gradient.stops.end()) right = gradient.stops.end() - 1;
                if (right == gradient.stops.begin()) ++right;
                const auto& a = *(right - 1); const auto& b = *right;
                const double amount = std::clamp((position - a.position) / (b.position - a.position), 0., 1.);
                const double alpha = a.opacity * (1 - amount) + b.opacity * amount;
                const auto channel = [&](int shift) {
                    return alpha > .000001 ? static_cast<int>(std::lround(
                        (((a.color >> shift) & 255) * a.opacity * (1 - amount) +
                            ((b.color >> shift) & 255) * b.opacity * amount) / alpha)) : 0;
                };
                fill = {channel(16), channel(8), channel(0)};
                opacity = static_cast<int>(std::lround(alpha * plateOpacity));
            }
            const size_t index = static_cast<size_t>(y) * width + x;
            if (legacyExact && mask[index] > 0)
            {
                const float edge = static_cast<float>(std::clamp<int>(
                    mask[index] - inner[index], 0, 255)) / mask[index];
                fill = Mix(fill, automatic, edge);
            }
            const int alpha = (mask[index] * opacity + 127) / 255;
            output[index] = PackPremultiplied(fill.r, fill.g, fill.b, alpha);
        }
    }
}

std::uint32_t ApplyFilter(std::uint32_t pixel,
    const IconBeautifySettings& settings)
{
    if (!settings.filterEnabled || PixelA(pixel) == 0)
        return pixel;
    if (settings.filterStrength <= 0.0005f)
        return pixel;

    const int alpha = PixelA(pixel);
    float r = static_cast<float>((pixel >> 16) & 0xff) / alpha;
    float g = static_cast<float>((pixel >> 8) & 0xff) / alpha;
    float b = static_cast<float>(pixel & 0xff) / alpha;
    const float originalR = r;
    const float originalG = g;
    const float originalB = b;
    const float gray = std::clamp(
        r * 0.299f + g * 0.587f + b * 0.114f, 0.0f, 1.0f);
    r = settings.filterTintR * gray;
    g = settings.filterTintG * gray;
    b = settings.filterTintB * gray;
    if (settings.filterMode == 1)
    {
        // Set the tint's luminosity to the original pixel's, then compress into
        // gamut about that luminosity. Black and white remain black and white.
        const float tintLuma = settings.filterTintR * .299f + settings.filterTintG * .587f + settings.filterTintB * .114f;
        r = settings.filterTintR + gray - tintLuma;
        g = settings.filterTintG + gray - tintLuma;
        b = settings.filterTintB + gray - tintLuma;
        const float minimum = std::min({r, g, b});
        if (minimum < 0 && gray > minimum)
        {
            const float factor = gray / (gray - minimum);
            r = gray + (r - gray) * factor; g = gray + (g - gray) * factor; b = gray + (b - gray) * factor;
        }
        const float maximum = std::max({r, g, b});
        if (maximum > 1 && maximum > gray)
        {
            const float factor = (1 - gray) / (maximum - gray);
            r = gray + (r - gray) * factor; g = gray + (g - gray) * factor; b = gray + (b - gray) * factor;
        }
    }
    else if (settings.filterMode == 2)
    {
        // A raster app icon often includes an opaque plate. Gradient mapping
        // retains its mark and internal contrast where a flat alpha tint cannot.
        const auto tone = [gray](float tint) { return std::lerp(tint * .16f, .88f + tint * .12f, gray); };
        r = tone(settings.filterTintR); g = tone(settings.filterTintG); b = tone(settings.filterTintB);
    }
    // Preserve the legacy byte rounding for migrated settings. New treatments
    // keep their exact endpoint, avoiding one-byte variation at a color endpoint.
    if (settings.filterMode == 0 || settings.filterStrength < 1.f)
    {
        r = originalR + (r - originalR) * settings.filterStrength;
        g = originalG + (g - originalG) * settings.filterStrength;
        b = originalB + (b - originalB) * settings.filterStrength;
    }
    return PackPremultiplied(
        static_cast<int>(std::round(std::clamp(r, 0.0f, 1.0f) * 255.0f)),
        static_cast<int>(std::round(std::clamp(g, 0.0f, 1.0f) * 255.0f)),
        static_cast<int>(std::round(std::clamp(b, 0.0f, 1.0f) * 255.0f)),
        alpha);
}

void ApplyTexture(std::vector<std::uint32_t>& output, int width, int height,
    const IconBeautifySettings& settings, int rasterScale)
{
    if (settings.textureHighlightStrength <= 0.0005f &&
        settings.textureShadeStrength <= 0.0005f &&
        settings.textureEdgeHighlight <= 0.0005f)
        return;
    const auto& mask = CachedMask(settings.shape, width, height, 0.0f);
    const auto& inner = CachedMask(settings.shape, width, height, 1.5f * rasterScale);
    for (int y = 0; y < height; ++y)
    {
        const float yt = (static_cast<float>(y) + 0.5f) / height;
        for (int x = 0; x < width; ++x)
        {
            const size_t index = static_cast<size_t>(y) * width + x;
            if (mask[index] == 0) continue;
            const float xt = (static_cast<float>(x) + 0.5f) / width;
            const float highlightPosition = yt +
                (xt - 0.5f) * settings.textureHighlightAngle * 0.5f;
            const float highlight = std::clamp(
                (settings.textureHighlightSize - highlightPosition) /
                    settings.textureHighlightSize,
                0.0f, 1.0f);
            int whiteAlpha = static_cast<int>(std::round(
                highlight * highlight * settings.textureHighlightStrength * 255.0f));
            whiteAlpha += static_cast<int>(std::round(
                std::clamp<int>(mask[index] - inner[index], 0, 255) *
                settings.textureEdgeHighlight));
            whiteAlpha = std::clamp(whiteAlpha, 0, 255);
            const float shade = std::clamp((yt - 0.5f) * 2.0f, 0.0f, 1.0f);
            int darkAlpha = static_cast<int>(std::round(
                shade * settings.textureShadeStrength * 255.0f));
            whiteAlpha = whiteAlpha * mask[index] / 255;
            darkAlpha = darkAlpha * mask[index] / 255;
            if (darkAlpha > 0)
                output[index] = SourceOver(PackPremultiplied(18, 24, 34, darkAlpha), output[index]);
            if (whiteAlpha > 0)
                output[index] = SourceOver(PackPremultiplied(255, 255, 255, whiteAlpha), output[index]);
        }
    }
}

void ApplyEdgeReflection(std::vector<std::uint32_t>& output, int width, int height,
    const IconBeautifySettings& settings, int rasterScale = 1)
{
    if (!settings.edgeHighlightEnabled || settings.edgeHighlightStrength <= 0.0f) return;
    const auto& outline = OutlineFor(settings.shape);
    const auto& mask = CachedMask(settings.shape, width, height, 0.0f);
    const float wf = static_cast<float>(width), hf = static_cast<float>(height);
    const float shortSide = std::min(wf, hf);
    const float depth = settings.edgeHighlightWidth * shortSide / 52.0f;
    const flat_glass_rim::Evaluator material(settings.edgeLight);
    const float rimInset = depth * .5f;
    const float halo = rimInset + std::max(material.InnerSupport(depth, false), material.InnerSupport(depth, true));
    const auto& inner = CachedMask(settings.shape, width, height, halo);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
    {
        const size_t index = static_cast<size_t>(y) * width + x;
        if (mask[index] == 0 || inner[index] == 255) continue;
        float nearest = halo * halo;
        for (size_t i = 0; i < outline.size(); ++i)
        {
            const auto& a = outline[i]; const auto& b = outline[(i + 1) % outline.size()];
            const float ax = a.x * wf, ay = a.y * hf;
            const float dx = (b.x - a.x) * wf, dy = (b.y - a.y) * hf;
            const float length2 = dx * dx + dy * dy;
            if (length2 < 0.00001f) continue;
            const float t = std::clamp(((static_cast<float>(x) + 0.5f - ax) * dx + (static_cast<float>(y) + 0.5f - ay) * dy) / length2, 0.0f, 1.0f);
            const float ex = ax + t * dx - (static_cast<float>(x) + 0.5f), ey = ay + t * dy - (static_cast<float>(y) + 0.5f);
            const float distance2 = ex * ex + ey * ey;
            if (distance2 < nearest)
            {
                nearest = distance2;
            }
        }
        const float distance = std::sqrt(nearest) - rimInset;
        const float lighting = material.Lighting((static_cast<float>(x) + .5f) / wf,
            (static_cast<float>(y) + .5f) / hf);
        // Evaluate in native units so the evaluator's fixed outer support is
        // supersampled together with the normalized contour and depth.
        const float light = material.Intensity(distance / rasterScale, depth / rasterScale, lighting);
        const int alpha = static_cast<int>(std::lround(light * settings.edgeHighlightStrength * mask[index]));
        output[index] = SourceOver(PackPremultiplied(255, 255, 255, alpha), output[index]);
        if (material.settings.shadowStrength > 0.f)
        {
            const int shadowAlpha = static_cast<int>(std::lround(
                material.Occlusion(distance / rasterScale, depth / rasterScale, lighting) *
                settings.edgeHighlightStrength * material.settings.shadowStrength * mask[index]));
            output[index] = SourceOver(
                PackPremultiplied(4, 6, 9, shadowAlpha), output[index]);
        }
    }
}

void ApplyOutline(std::vector<std::uint32_t>& output, int width, int height,
    const IconBeautifySettings& settings, int rasterScale)
{
    if (!settings.outlineEnabled || settings.outlineWidth <= 0.0f)
        return;
    const EdgeColor stroke = ColorFromFloats(
        settings.outlineR, settings.outlineG, settings.outlineB);
    const auto& mask = CachedMask(settings.shape, width, height, 0.0f);
    const float outlineWidth = settings.glassEnabled
        ? settings.outlineWidth * std::min(width, height) / 52.0f : settings.outlineWidth * rasterScale;
    const auto& inner = CachedMask(settings.shape, width, height, outlineWidth);
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

const std::vector<ShapePoint>& ShapeOutline(IconBeautifyShape shape)
{
    return OutlineFor(shape);
}

IconBeautifySettings Normalize(IconBeautifySettings settings)
{
    const int preset = static_cast<int>(settings.preset);
    if (preset != static_cast<int>(IconBeautifyPreset::None) &&
        preset != static_cast<int>(IconBeautifyPreset::DefaultBeautify) &&
        preset != static_cast<int>(IconBeautifyPreset::Custom) &&
        preset != static_cast<int>(IconBeautifyPreset::FrostedGlass) &&
        preset != static_cast<int>(IconBeautifyPreset::FrostedGlassDark) &&
        preset != static_cast<int>(IconBeautifyPreset::FrostedGlassLight))
        settings.preset = IconBeautifyPreset::Custom;
    settings.glassBlurRadius = std::isfinite(settings.glassBlurRadius) ?
        std::clamp(settings.glassBlurRadius, 4.0f, 48.0f) : 16.0f;
    settings.edgeHighlightWidth = std::isfinite(settings.edgeHighlightWidth) ?
        std::clamp(settings.edgeHighlightWidth, 0.5f, 4.0f) : 1.0f;
    settings.edgeHighlightStrength = std::isfinite(settings.edgeHighlightStrength) ?
        std::clamp(settings.edgeHighlightStrength, 0.0f, 1.0f) : 0.40f;
    settings.edgeLight = NormalizeEdgeLight(settings.edgeLight);
    settings.mode = std::clamp(settings.mode, 0, 2);
    settings.backgroundSource = std::clamp(settings.backgroundSource, 0, 2);
    settings.filterMode = std::clamp(settings.filterMode, 0, 2);
    if (!ValidatePanelGradient(settings.backgroundGradient)) settings.backgroundGradient = {};
    settings.backgroundOpacity = std::clamp(settings.backgroundOpacity, 0.0f, 1.0f);
    settings.gradientDirection = std::clamp(settings.gradientDirection, 0, 3);
    settings.backgroundStartR = std::clamp(settings.backgroundStartR, 0.0f, 1.0f);
    settings.backgroundStartG = std::clamp(settings.backgroundStartG, 0.0f, 1.0f);
    settings.backgroundStartB = std::clamp(settings.backgroundStartB, 0.0f, 1.0f);
    settings.backgroundEndR = std::clamp(settings.backgroundEndR, 0.0f, 1.0f);
    settings.backgroundEndG = std::clamp(settings.backgroundEndG, 0.0f, 1.0f);
    settings.backgroundEndB = std::clamp(settings.backgroundEndB, 0.0f, 1.0f);
    switch (settings.shape)
    {
    case IconBeautifyShape::LegacyRounded:
    case IconBeautifyShape::ContinuousRounded:
    case IconBeautifyShape::Circle:
    case IconBeautifyShape::SoftRounded:
    case IconBeautifyShape::Pebble:
        break;
    default:
        settings.shape = IconBeautifyShape::LegacyRounded;
        break;
    }
    settings.contentScale = std::clamp(settings.contentScale, 0.50f, 0.90f);
    settings.textureHighlightStrength = std::clamp(
        settings.textureHighlightStrength, 0.0f, 1.0f);
    settings.textureHighlightSize = std::clamp(
        settings.textureHighlightSize, 0.10f, 1.0f);
    settings.textureHighlightAngle = std::clamp(
        settings.textureHighlightAngle, -1.0f, 1.0f);
    settings.textureShadeStrength = std::clamp(
        settings.textureShadeStrength, 0.0f, 1.0f);
    settings.textureEdgeHighlight = std::clamp(
        settings.textureEdgeHighlight, 0.0f, 1.0f);
    settings.filterStrength = std::clamp(settings.filterStrength, 0.0f, 1.0f);
    settings.filterTintR = std::clamp(settings.filterTintR, 0.0f, 1.0f);
    settings.filterTintG = std::clamp(settings.filterTintG, 0.0f, 1.0f);
    settings.filterTintB = std::clamp(settings.filterTintB, 0.0f, 1.0f);
    settings.outlineWidth = std::clamp(settings.outlineWidth, 0.0f, 4.0f);
    settings.outlineOpacity = std::clamp(settings.outlineOpacity, 0.0f, 1.0f);
    settings.outlineR = std::clamp(settings.outlineR, 0.0f, 1.0f);
    settings.outlineG = std::clamp(settings.outlineG, 0.0f, 1.0f);
    settings.outlineB = std::clamp(settings.outlineB, 0.0f, 1.0f);
    settings.shadowStrength = std::clamp(settings.shadowStrength, 0.0f, 1.0f);
    return settings;
}

PanelGradient BackgroundGradient(const IconBeautifySettings& settings)
{
    if (settings.backgroundGradient.enabled || !settings.gradientEnabled) return settings.backgroundGradient;
    PanelGradient gradient;
    gradient.enabled = settings.gradientEnabled;
    constexpr double angles[]{90, 0, 45, 315};
    gradient.angle = angles[std::clamp(settings.gradientDirection, 0, 3)];
    const auto rgb = [](float r, float g, float b) {
        const auto c = ColorFromFloats(r, g, b);
        return static_cast<unsigned>(c.r << 16 | c.g << 8 | c.b);
    };
    gradient.stops = {{0, rgb(settings.backgroundStartR, settings.backgroundStartG, settings.backgroundStartB), 1},
        {1, rgb(settings.backgroundEndR, settings.backgroundEndG, settings.backgroundEndB), 1}};
    return gradient;
}

IconBeautifySettings NormalizeGlobal(IconBeautifySettings settings)
{
    settings = Normalize(settings);
    if (settings.mode == 2) settings.mode = 0;
    return settings;
}

bool Equal(const IconBeautifySettings& lhs, const IconBeautifySettings& rhs)
{
    const IconBeautifySettings a = Normalize(lhs);
    const IconBeautifySettings b = Normalize(rhs);
    auto eq = [](float x, float y) { return std::abs(x - y) <= 0.0005f; };
    return a.enabled == b.enabled && a.preset == b.preset &&
        a.mode == b.mode &&
        a.backgroundSource == b.backgroundSource && a.backgroundGradient == b.backgroundGradient && a.filterMode == b.filterMode &&
        eq(a.backgroundOpacity, b.backgroundOpacity) &&
        a.glassEnabled == b.glassEnabled && eq(a.glassBlurRadius, b.glassBlurRadius) &&
        a.edgeHighlightEnabled == b.edgeHighlightEnabled &&
        eq(a.edgeHighlightWidth, b.edgeHighlightWidth) &&
        eq(a.edgeHighlightStrength, b.edgeHighlightStrength) && a.edgeLight == b.edgeLight &&
        a.gradientEnabled == b.gradientEnabled &&
        a.gradientDirection == b.gradientDirection &&
        eq(a.backgroundStartR, b.backgroundStartR) &&
        eq(a.backgroundStartG, b.backgroundStartG) &&
        eq(a.backgroundStartB, b.backgroundStartB) &&
        eq(a.backgroundEndR, b.backgroundEndR) &&
        eq(a.backgroundEndG, b.backgroundEndG) &&
        eq(a.backgroundEndB, b.backgroundEndB) &&
        a.shape == b.shape && eq(a.contentScale, b.contentScale) &&
        eq(a.textureHighlightStrength, b.textureHighlightStrength) &&
        eq(a.textureHighlightSize, b.textureHighlightSize) &&
        eq(a.textureHighlightAngle, b.textureHighlightAngle) &&
        eq(a.textureShadeStrength, b.textureShadeStrength) &&
        eq(a.textureEdgeHighlight, b.textureEdgeHighlight) &&
        a.filterEnabled == b.filterEnabled &&
        eq(a.filterStrength, b.filterStrength) &&
        eq(a.filterTintR, b.filterTintR) &&
        eq(a.filterTintG, b.filterTintG) && eq(a.filterTintB, b.filterTintB) &&
        a.outlineEnabled == b.outlineEnabled &&
        eq(a.outlineWidth, b.outlineWidth) &&
        eq(a.outlineOpacity, b.outlineOpacity) &&
        eq(a.outlineR, b.outlineR) && eq(a.outlineG, b.outlineG) &&
        eq(a.outlineB, b.outlineB) && eq(a.shadowStrength, b.shadowStrength);
}

bool UsesLegacyGeometryDefaults(const IconBeautifySettings& settings)
{
    const IconBeautifySettings s = Normalize(settings);
    return s.shape == IconBeautifyShape::LegacyRounded && !s.backgroundGradient.enabled && s.backgroundSource == 0 &&
        s.textureHighlightStrength <= 0.0005f &&
        s.textureShadeStrength <= 0.0005f &&
        s.textureEdgeHighlight <= 0.0005f && !s.filterEnabled &&
        !s.outlineEnabled && !s.glassEnabled && !s.edgeHighlightEnabled &&
        std::abs(s.outlineWidth - 1.0f) <= 0.0005f &&
        std::abs(s.outlineOpacity - 1.0f) <= 0.0005f &&
        std::abs(s.contentScale - 0.68f) <= 0.0005f &&
        std::abs(s.shadowStrength - 0.35f) <= 0.0005f;
}

IconBeautifySettings MakePreset(IconBeautifyPreset preset)
{
    IconBeautifySettings settings{};
    switch (preset)
    {
    case IconBeautifyPreset::None:
        settings.preset = IconBeautifyPreset::None;
        return settings;
    case IconBeautifyPreset::DefaultBeautify:
        settings.preset = IconBeautifyPreset::DefaultBeautify;
        settings.enabled = true;
        settings.shape = IconBeautifyShape::LegacyRounded;
        settings.outlineEnabled = false;
        return settings;
    case IconBeautifyPreset::FrostedGlass:
    case IconBeautifyPreset::FrostedGlassDark:
    case IconBeautifyPreset::FrostedGlassLight:
    {
        settings.preset = preset;
        settings.enabled = true;
        settings.shape = IconBeautifyShape::ContinuousRounded;
        settings.backgroundStartR = settings.backgroundStartG = settings.backgroundStartB = 1.0f;
        settings.backgroundEndR = settings.backgroundEndG = settings.backgroundEndB = 1.0f;
        settings.backgroundOpacity = 0.04f;
        settings.glassEnabled = true;
        settings.glassBlurRadius = 10.0f;
        settings.edgeHighlightEnabled = true;
        const auto edge = MaterialEdges(preset == IconBeautifyPreset::FrostedGlassDark ? MaterialEdgePreset::GlassDark :
            preset == IconBeautifyPreset::FrostedGlassLight ? MaterialEdgePreset::GlassLight : MaterialEdgePreset::GlassTransparent);
        settings.edgeHighlightWidth = edge.width;
        settings.edgeHighlightStrength = edge.opacity;
        settings.edgeLight = edge.light;
        // Presets tune the shared inward rim for normal icon sizes. Its center
        // stays inside the plate so both glow controls contribute visible pixels.
        settings.edgeHighlightWidth = 1.8f;
        settings.edgeHighlightStrength = preset == IconBeautifyPreset::FrostedGlassDark ? .24f :
            preset == IconBeautifyPreset::FrostedGlassLight ? .30f : .28f;
        settings.edgeLight.innerGlow = 1.6f;
        // Start fading earlier along the contour instead of keeping a long
        // bright plateau followed by a localized dip. The broader feather
        // carries the transition across the weak arcs at normal icon sizes.
        settings.edgeLight.spread = 50.f;
        settings.edgeLight.feather = 70.f;
        settings.edgeLight.ambient = .10f;
        settings.edgeLight.primary = .67f;
        settings.edgeLight.opposite = .63f;
        settings.edgeLight.glowStrength = .12f;
        if (preset != IconBeautifyPreset::FrostedGlass)
        {
            const bool dark = preset == IconBeautifyPreset::FrostedGlassDark;
            settings.backgroundStartR = settings.backgroundEndR = (dark ? 13.f : 235.f) / 255.f;
            settings.backgroundStartG = settings.backgroundEndG = (dark ? 18.f : 245.f) / 255.f;
            settings.backgroundStartB = settings.backgroundEndB = (dark ? 26.f : 255.f) / 255.f;
            settings.backgroundOpacity = dark ? .28f : .15f;
            settings.glassBlurRadius = dark ? 24.f : 22.f;
        }
        // The glass reflection supplies the contour. A second uniform stroke
        // would brighten the weak sectors and sharpen the outside edge.
        settings.outlineEnabled = false;
        settings.outlineWidth = 0.75f;
        settings.outlineOpacity = 0.05f;
        settings.outlineR = settings.outlineG = settings.outlineB = 1.0f;
        settings.shadowStrength = 0.15f;
        return settings;
    }
    case IconBeautifyPreset::Custom:
    default:
        settings.preset = IconBeautifyPreset::Custom;
        settings.enabled = true;
        return settings;
    }
}

IconBeautifySettings ResolvePersistedSettings(IconBeautifySettings settings,
    bool hasExplicitPreset)
{
    settings = Normalize(settings);
    if (!hasExplicitPreset)
    {
        settings.preset = IdentifyPreset(settings);
        return settings;
    }
    if (settings.preset == IconBeautifyPreset::Custom) return settings;
    auto current = MakePreset(settings.preset);
    current.mode = settings.mode;
    if (current.preset != IconBeautifyPreset::None)
        current.enabled = settings.enabled;
    return current;
}

void ApplyLegacyFinish(IconBeautifySettings& settings,
    IconBeautifyFinish finish)
{
    settings.textureHighlightStrength = 0.0f;
    settings.textureHighlightSize = 0.55f;
    settings.textureHighlightAngle = 0.0f;
    settings.textureShadeStrength = 0.0f;
    settings.textureEdgeHighlight = 0.0f;
    switch (finish)
    {
    case IconBeautifyFinish::Gloss:
        settings.textureHighlightStrength = 72.0f / 255.0f;
        settings.textureHighlightSize = 0.65f;
        settings.textureHighlightAngle = 0.40f;
        break;
    case IconBeautifyFinish::Glass:
        settings.textureHighlightStrength = 50.0f / 255.0f;
        settings.textureHighlightSize = 0.52f;
        settings.textureShadeStrength = 24.0f / 255.0f;
        settings.textureEdgeHighlight = 42.0f / 255.0f;
        break;
    case IconBeautifyFinish::Sticker:
        settings.textureEdgeHighlight = 228.0f / 255.0f;
        break;
    case IconBeautifyFinish::Flat:
    default:
        break;
    }
}

IconBeautifyPreset IdentifyPreset(const IconBeautifySettings& settings)
{
    const IconBeautifySettings normalized = Normalize(settings);
    if (!normalized.enabled)
        return IconBeautifyPreset::None;
    if (normalized.preset == IconBeautifyPreset::Custom)
        return IconBeautifyPreset::Custom;
    constexpr std::array<IconBeautifyPreset, 4> presets{
        IconBeautifyPreset::DefaultBeautify,
        IconBeautifyPreset::FrostedGlass,
        IconBeautifyPreset::FrostedGlassDark,
        IconBeautifyPreset::FrostedGlassLight,
    };
    for (IconBeautifyPreset preset : presets)
    {
        IconBeautifySettings candidate = MakePreset(preset);
        candidate.preset = normalized.preset;
        candidate.mode = normalized.mode;
        if (Equal(normalized, candidate))
            return preset;
    }
    return IconBeautifyPreset::Custom;
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

std::vector<std::uint32_t> RenderEdgeReflection(int width, int height,
    const IconBeautifySettings& settings)
{
    if (width <= 0 || height <= 0) return {};
    std::vector<std::uint32_t> pixels(static_cast<size_t>(width) * height, 0);
    ApplyEdgeReflection(pixels, width, height, Normalize(settings));
    return pixels;
}

static std::vector<std::uint32_t> RenderAtScale(const std::vector<std::uint32_t>& source,
    int width, int height, const IconBeautifySettings& rawSettings,
    std::optional<PlateFill> detectedEdgeFill, int rasterScale, SourceKind sourceKind)
{
    if (width <= 0 || height <= 0 ||
        source.size() != static_cast<size_t>(width) * height)
        return source;
    IconBeautifySettings settings = Normalize(rawSettings);
    if (!settings.enabled)
        return source;
    // Built-in Dock glyphs already reserve optical margins. Shrink-all applies
    // to application images; these controls receive only the selected plate.
    if (sourceKind == SourceKind::NativeControl && settings.mode == 1)
        settings.mode = 2;
    const Bounds bounds = VisibleBounds(source, width, height);
    if (!bounds.valid) return source;
    const Bounds contentBounds = settings.mode == 1 ? Bounds{true, 0, 0, width - 1, height - 1} : bounds;
    if (settings.mode != 0) detectedEdgeFill.reset();
    if (!detectedEdgeFill && settings.backgroundSource != 0)
    {
        auto seed = settings.backgroundSource == 1 ? DetectPlateFill(source, width, height) : std::nullopt;
        if (!seed)
        {
            // Score opaque color clusters rather than averaging unrelated hues.
            // Chroma weighting keeps a large white icon plate from hiding its mark.
            struct Bucket { double weight = 0, r = 0, g = 0, b = 0; };
            std::array<Bucket, 512> buckets{};
            for (auto pixel : source)
            {
                const int a = PixelA(pixel); if (a < 32) continue;
                const int r = std::min(255, static_cast<int>((pixel >> 16) & 255) * 255 / a);
                const int g = std::min(255, static_cast<int>((pixel >> 8) & 255) * 255 / a);
                const int b = std::min(255, static_cast<int>(pixel & 255) * 255 / a);
                const double chroma = (std::max({r, g, b}) - std::min({r, g, b})) / 255.;
                const double weight = a * (.025 + chroma * chroma);
                auto& bucket = buckets[(r >> 5) * 64 + (g >> 5) * 8 + (b >> 5)];
                bucket.weight += weight; bucket.r += r * weight; bucket.g += g * weight; bucket.b += b * weight;
            }
            const auto best = std::max_element(buckets.begin(), buckets.end(),
                [](const auto& a, const auto& b) { return a.weight < b.weight; });
            if (best->weight > 0) seed = EdgeColor{static_cast<int>(std::lround(best->r / best->weight)),
                static_cast<int>(std::lround(best->g / best->weight)), static_cast<int>(std::lround(best->b / best->weight))};
        }
        if (seed)
        {
            settings.backgroundStartR = static_cast<float>(seed->r) / 255.f;
            settings.backgroundStartG = static_cast<float>(seed->g) / 255.f;
            settings.backgroundStartB = static_cast<float>(seed->b) / 255.f;
            settings.backgroundEndR = settings.backgroundStartR; settings.backgroundEndG = settings.backgroundStartG; settings.backgroundEndB = settings.backgroundStartB;
            settings.backgroundGradient = BackgroundGradient(settings);
            for (auto& stop : settings.backgroundGradient.stops)
            {
                const auto color = Mix(*seed, {255, 255, 255}, static_cast<float>(.35 * (1 - stop.position)));
                stop.color = static_cast<unsigned>(color.r << 16 | color.g << 8 | color.b);
            }
        }
    }
    const bool legacyExact = UsesLegacyGeometryDefaults(settings);
    const auto& mask = CachedMask(settings.shape, width, height, 0.0f);
    std::vector<std::uint32_t> output(source.size(), 0);

    Bounds tileBounds = bounds;
    float tileInset = 0;
    if (detectedEdgeFill && detectedEdgeFill->cover)
    {
        // Ignore antialiased shadows when fitting a complete image tile.
        tileBounds = {false, width, height, -1, -1};
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
            if (PixelA(source[static_cast<size_t>(y) * width + x]) >= 160)
            {
                tileBounds.valid = true;
                tileBounds.left = std::min(tileBounds.left, x); tileBounds.right = std::max(tileBounds.right, x);
                tileBounds.top = std::min(tileBounds.top, y); tileBounds.bottom = std::max(tileBounds.bottom, y);
            }
        const float tw = static_cast<float>(tileBounds.right - tileBounds.left + 1);
        const float th = static_cast<float>(tileBounds.bottom - tileBounds.top + 1);
        bool covered = false;
        // Find the smallest crop whose real pixels cover the selected outline.
        // This also handles pixel-art corner gaps without shrinking the mark.
        for (int step = 0; step <= 12; ++step)
        {
            tileInset = std::max(detectedEdgeFill->inset, step * .01f);
            int tested = 0, opaque = 0;
            for (int gy = 0; gy < 48; ++gy) for (int gx = 0; gx < 48; ++gx)
            {
                const int x = std::min(width - 1, (2 * gx + 1) * width / 96);
                const int y = std::min(height - 1, (2 * gy + 1) * height / 96);
                if (mask[static_cast<size_t>(y) * width + x] < 192) continue;
                if (gx >= 8 && gx < 40 && gy >= 8 && gy < 40) continue;
                const float sx = tileBounds.left + (tileInset + (x + .5f) / width * (1 - 2 * tileInset)) * tw - .5f;
                const float sy = tileBounds.top + (tileInset + (y + .5f) / height * (1 - 2 * tileInset)) * th - .5f;
                ++tested; opaque += PixelA(SampleBilinear(source, width, height, sx, sy)) >= 240;
            }
            if (tested > 0 && opaque >= tested * .995) { covered = true; break; }
        }
        if (!covered) detectedEdgeFill.reset();
    }

    if (detectedEdgeFill || settings.mode == 2)
    {
        // Smart recognition fills a detected original plate. Explicit theme
        // backgrounds retain the source canvas without overriding theme colors.
        std::uint32_t solidBackground = 0;
        if (detectedEdgeFill && !detectedEdgeFill->gradient)
            solidBackground = ApplyFilter(PackPremultiplied(detectedEdgeFill->r,
                detectedEdgeFill->g, detectedEdgeFill->b, 255), settings);
        if (!detectedEdgeFill || detectedEdgeFill->cover)
            FillPlate(output, width, height, settings, legacyExact, rasterScale);
        for (size_t i = 0; i < output.size(); ++i)
        {
            std::uint32_t background = solidBackground;
            if (detectedEdgeFill && detectedEdgeFill->cover) background = output[i];
            if (detectedEdgeFill && detectedEdgeFill->gradient)
            {
                const auto& fill = *detectedEdgeFill;
                const float x = (static_cast<float>(i % width) + .5f) / width - .5f;
                const float y = (static_cast<float>(i / width) + .5f) / height - .5f;
                const auto channel = [&](int base, size_t c) {
                    return std::clamp(static_cast<int>(std::lround(base + fill.dx[c] * x + fill.dy[c] * y)), 0, 255);
                };
                background = ApplyFilter(PackPremultiplied(channel(fill.r, 0),
                    channel(fill.g, 1), channel(fill.b, 2), 255), settings);
            }
            std::uint32_t sampled = source[i];
            if (detectedEdgeFill && detectedEdgeFill->normalizeContour && PixelA(sampled) > 0)
            {
                const float rx = (static_cast<float>(i % width) - (bounds.left + bounds.right) * .5f) /
                    std::max(1, bounds.right - bounds.left + 1);
                const float ry = (static_cast<float>(i / width) - (bounds.top + bounds.bottom) * .5f) /
                    std::max(1, bounds.bottom - bounds.top + 1);
                if (rx * rx + ry * ry >= .42f * .42f)
                {
                    // Normalize only background-colored pixels in the outer
                    // band. Marks keep their pixels; the original disc's
                    // antialiasing and small rim highlights cannot form a seam.
                    int error = 0;
                    const auto filtered = ApplyFilter(sampled, settings);
                    for (const int shift : {0, 8, 16})
                    {
                        const int value = static_cast<int>((filtered >> shift) & 255) * 255 / PixelA(filtered);
                        const int delta = value - static_cast<int>((background >> shift) & 255);
                        error += delta * delta;
                    }
                    if (error <= 40 * 40) sampled = 0;
                }
            }
            if (detectedEdgeFill && detectedEdgeFill->cover)
            {
                const float sx = tileBounds.left + (tileInset + (static_cast<float>(i % width) + .5f) / width * (1 - 2 * tileInset)) *
                    (tileBounds.right - tileBounds.left + 1) - .5f;
                const float sy = tileBounds.top + (tileInset + (static_cast<float>(i / width) + .5f) / height * (1 - 2 * tileInset)) *
                    (tileBounds.bottom - tileBounds.top + 1) - .5f;
                sampled = SampleBilinear(source, width, height, sx, sy);
            }
            const auto foreground = ApplyFilter(sampled, settings);
            output[i] = detectedEdgeFill ? ScalePixel(SourceOver(foreground, background), mask[i]) :
                SourceOver(ScalePixel(foreground, mask[i]), output[i]);
        }
        ApplyTexture(output, width, height, settings, rasterScale);
        ApplyOutline(output, width, height, settings, rasterScale);
        ApplyEdgeReflection(output, width, height, settings, rasterScale);
        return output;
    }

    FillPlate(output, width, height, settings, legacyExact, rasterScale);
    const int sourceW = std::max(1, contentBounds.right - contentBounds.left + 1);
    const int sourceH = std::max(1, contentBounds.bottom - contentBounds.top + 1);
    int maxW = 0;
    int maxH = 0;
    if (legacyExact)
    {
        const int padding = std::max(5,
            static_cast<int>(std::round(static_cast<float>(std::min(width, height)) / rasterScale * 0.16f))) * rasterScale;
        maxW = std::max(1, width - padding * 2);
        maxH = std::max(1, height - padding * 2);
    }
    else
    {
        maxW = std::max(1, static_cast<int>(std::round(static_cast<float>(width) / rasterScale * settings.contentScale))) * rasterScale;
        maxH = std::max(1, static_cast<int>(std::round(static_cast<float>(height) / rasterScale * settings.contentScale))) * rasterScale;
    }
    const float fit = std::min(static_cast<float>(maxW) / sourceW,
        static_cast<float>(maxH) / sourceH);
    const bool nativeMargins = sourceKind == SourceKind::NativeControl;
    const float scale = nativeMargins ? std::min(1.0f, fit) : fit;
    int destW = std::max(1, static_cast<int>(std::round(sourceW * scale / rasterScale))) * rasterScale;
    int destH = std::max(1, static_cast<int>(std::round(sourceH * scale / rasterScale))) * rasterScale;
    if (nativeMargins) { destW = std::min(sourceW, destW); destH = std::min(sourceH, destH); }
    const int destLeft = (width - destW) / 2;
    const int destTop = (height - destH) / 2;

    constexpr std::array<std::tuple<int, int, int>, 7> passes{{
        {0,1,120},{-1,1,63},{1,1,63},{0,2,51},{-1,0,40},{1,0,40},{0,-1,29}
    }};
    for (int y = 0; y < destH; ++y)
        for (int x = 0; x < destW; ++x)
        {
            const float sx = contentBounds.left +
                ((static_cast<float>(x) + 0.5f) / destW) * sourceW - 0.5f;
            const float sy = contentBounds.top +
                ((static_cast<float>(y) + 0.5f) / destH) * sourceH - 0.5f;
            const std::uint32_t sampled = SampleBilinear(source, width, height, sx, sy);
            if (PixelA(sampled) == 0) continue;
            const int ox = destLeft + x;
            const int oy = destTop + y;
            for (const auto& [dx, dy, baseOpacity] : passes)
            {
                const int px = ox + dx * rasterScale;
                const int py = oy + dy * rasterScale;
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
            const float sx = contentBounds.left +
                ((static_cast<float>(x) + 0.5f) / destW) * sourceW - 0.5f;
            const float sy = contentBounds.top +
                ((static_cast<float>(y) + 0.5f) / destH) * sourceH - 0.5f;
            const int ox = destLeft + x;
            const int oy = destTop + y;
            const size_t index = static_cast<size_t>(oy) * width + ox;
            const std::uint32_t sampled = ScalePixel(ApplyFilter(
                SampleBilinear(source, width, height, sx, sy), settings),
                mask[index]);
            output[index] = SourceOver(sampled, output[index]);
        }

    ApplyTexture(output, width, height, settings, rasterScale);
    ApplyOutline(output, width, height, settings, rasterScale);
    ApplyEdgeReflection(output, width, height, settings, rasterScale);
    return output;
}

std::vector<std::uint32_t> Render(const std::vector<std::uint32_t>& source,
    int width, int height, const IconBeautifySettings& settings,
    std::optional<PlateFill> detectedEdgeFill, SourceKind sourceKind)
{
    return RenderAtScale(source, width, height, settings, detectedEdgeFill, 1, sourceKind);
}

MaskCacheStatistics TryGetMaskCacheStatistics()
{
    auto& cache = ShapeMasks();
    std::unique_lock lock(cache.mutex, std::try_to_lock);
    if (!lock.owns_lock()) return {};
    MaskCacheStatistics result;
    result.available = true;
    result.entries = cache.entries.size();
    for (const auto& [key, pixels] : cache.entries)
    { (void)key; result.payloadBytes += pixels.size(); result.capacityBytes += pixels.capacity(); }
    return result;
}

std::vector<std::uint32_t> RenderPreview(const std::vector<std::uint32_t>& source,
    int width, int height, const IconBeautifySettings& settings,
    std::optional<PlateFill> detectedEdgeFill, int rasterScale, SourceKind sourceKind)
{
    return RenderAtScale(source, width, height, settings, detectedEdgeFill,
        std::clamp(rasterScale, 1, 4), sourceKind);
}

#include "icon_backplate_analysis.inc"
}

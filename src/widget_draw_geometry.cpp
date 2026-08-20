#include "widget_draw_geometry.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace snowdesktop::widget_runtime
{
namespace
{
constexpr float MaximumCoordinate = 1000000.0f;
constexpr float MaximumDimension = 100000.0f;

bool Bounded(float value) noexcept
{
    return std::isfinite(value) && std::abs(value) <= MaximumCoordinate;
}

bool ValidRect(const DrawRect& rect) noexcept
{
    return Bounded(rect.x) && Bounded(rect.y) &&
        std::isfinite(rect.width) && std::isfinite(rect.height) &&
        rect.width > 0.0f && rect.height > 0.0f &&
        rect.width <= MaximumDimension && rect.height <= MaximumDimension &&
        Bounded(rect.x + rect.width) && Bounded(rect.y + rect.height);
}

float AlignOffset(DrawImageAlignment alignment, float available,
    float used) noexcept
{
    if (alignment == DrawImageAlignment::Center)
        return std::max(0.0f, (available - used) * 0.5f);
    if (alignment == DrawImageAlignment::End)
        return std::max(0.0f, available - used);
    return 0.0f;
}
}

DrawImagePlacement ResolveDrawImagePlacement(float sourceWidth,
    float sourceHeight, const DrawRect& destination, DrawImageFit fit,
    DrawImageAlignment alignment) noexcept
{
    DrawImagePlacement result;
    if (!std::isfinite(sourceWidth) || !std::isfinite(sourceHeight) ||
        sourceWidth <= 0.0f || sourceHeight <= 0.0f ||
        !ValidRect(destination))
        return result;
    result.source = { 0.0f, 0.0f, sourceWidth, sourceHeight };
    result.destination = destination;
    if (fit == DrawImageFit::Fill)
    {
        result.valid = true;
        return result;
    }

    if (fit == DrawImageFit::Contain)
    {
        const float scale = std::min(
            destination.width / sourceWidth,
            destination.height / sourceHeight);
        const float width = sourceWidth * scale;
        const float height = sourceHeight * scale;
        result.destination.x += AlignOffset(
            alignment, destination.width, width);
        result.destination.y += AlignOffset(
            alignment, destination.height, height);
        result.destination.width = width;
        result.destination.height = height;
    }
    else if (fit == DrawImageFit::Cover)
    {
        const float destinationAspect =
            destination.width / destination.height;
        const float sourceAspect = sourceWidth / sourceHeight;
        if (sourceAspect > destinationAspect)
        {
            const float width = sourceHeight * destinationAspect;
            result.source.x = AlignOffset(
                alignment, sourceWidth, width);
            result.source.width = width;
        }
        else
        {
            const float height = sourceWidth / destinationAspect;
            result.source.y = AlignOffset(
                alignment, sourceHeight, height);
            result.source.height = height;
        }
    }
    else
    {
        const float width = std::min(sourceWidth, destination.width);
        const float height = std::min(sourceHeight, destination.height);
        result.destination.x += AlignOffset(
            alignment, destination.width, width);
        result.destination.y += AlignOffset(
            alignment, destination.height, height);
        result.destination.width = width;
        result.destination.height = height;
        result.source.width = width;
        result.source.height = height;
    }
    result.valid = result.source.width > 0.0f &&
        result.source.height > 0.0f && result.destination.width > 0.0f &&
        result.destination.height > 0.0f;
    return result;
}

bool ValidateDrawPath(const std::vector<DrawPathCommand>& commands,
    std::string& error) noexcept
{
    error.clear();
    if (commands.empty() || commands.size() > 256)
    {
        error = "draw path must contain 1 to 256 commands";
        return false;
    }
    if (commands.front().type != DrawPathCommandType::Move)
    {
        error = "draw path must begin with move";
        return false;
    }
    bool figureOpen = false;
    bool geometrySeen = false;
    const auto validPoint = [](const DrawPoint& point) {
        return Bounded(point.x) && Bounded(point.y);
    };
    for (const auto& command : commands)
    {
        switch (command.type)
        {
        case DrawPathCommandType::Move:
            if (!validPoint(command.point))
            {
                error = "draw path coordinates must be finite and bounded";
                return false;
            }
            figureOpen = true;
            break;
        case DrawPathCommandType::Line:
            if (!figureOpen || !validPoint(command.point))
            {
                error = "draw path line requires an open figure and bounded coordinates";
                return false;
            }
            geometrySeen = true;
            break;
        case DrawPathCommandType::Cubic:
            if (!figureOpen || !validPoint(command.point) ||
                !validPoint(command.control1) ||
                !validPoint(command.control2))
            {
                error = "draw path cubic requires an open figure and bounded coordinates";
                return false;
            }
            geometrySeen = true;
            break;
        case DrawPathCommandType::Quadratic:
            if (!figureOpen || !validPoint(command.point) ||
                !validPoint(command.control1))
            {
                error = "draw path quadratic requires an open figure and bounded coordinates";
                return false;
            }
            geometrySeen = true;
            break;
        case DrawPathCommandType::Close:
            if (!figureOpen)
            {
                error = "draw path close requires an open figure";
                return false;
            }
            figureOpen = false;
            break;
        }
    }
    if (!geometrySeen)
    {
        error = "draw path requires at least one drawable segment";
        return false;
    }
    return true;
}

bool BuildDrawArc(float centerX, float centerY, float radius,
    float startDegrees, float sweepDegrees,
    std::vector<DrawArcPiece>& pieces, std::string& error) noexcept
{
    error.clear();
    pieces.clear();
    if (!Bounded(centerX) || !Bounded(centerY) ||
        !std::isfinite(radius) || radius <= 0.0f ||
        radius > MaximumDimension || !std::isfinite(startDegrees) ||
        !std::isfinite(sweepDegrees) || sweepDegrees == 0.0f ||
        std::abs(sweepDegrees) > 360.0f ||
        !Bounded(centerX - radius) || !Bounded(centerX + radius) ||
        !Bounded(centerY - radius) || !Bounded(centerY + radius))
    {
        error = "draw arc values must be finite, bounded, and use a non-zero sweep up to 360 degrees";
        return false;
    }
    const int count = std::max(1, static_cast<int>(std::ceil(
        std::abs(sweepDegrees) / 120.0f)));
    const float pieceSweep = sweepDegrees / static_cast<float>(count);
    const auto pointAt = [&](float degrees) {
        const float radians = degrees * std::numbers::pi_v<float> / 180.0f;
        return DrawPoint{ centerX + std::cos(radians) * radius,
            centerY + std::sin(radians) * radius };
    };
    pieces.reserve(static_cast<std::size_t>(count));
    float current = startDegrees;
    for (int index = 0; index < count; ++index)
    {
        pieces.push_back({ pointAt(current),
            pointAt(current + pieceSweep), radius, pieceSweep > 0.0f });
        current += pieceSweep;
    }
    return true;
}

bool BuildDrawSparkline(const std::vector<float>& values,
    const DrawRect& bounds, std::optional<float> minimum,
    std::optional<float> maximum, std::vector<DrawPoint>& points,
    std::string& error) noexcept
{
    error.clear();
    points.clear();
    if (values.empty() || values.size() > 512 || !ValidRect(bounds))
    {
        error = "draw sparkline requires 1 to 512 values and positive bounded dimensions";
        return false;
    }
    if (!std::all_of(values.begin(), values.end(), [](float value) {
            return std::isfinite(value) &&
                value >= -1.0e9f && value <= 1.0e9f;
        }))
    {
        error = "draw sparkline values must be finite and bounded";
        return false;
    }
    if (minimum.has_value() != maximum.has_value() ||
        (minimum && (!std::isfinite(*minimum) ||
            !std::isfinite(*maximum) || *minimum >= *maximum)))
    {
        error = "draw sparkline min and max must be paired and increasing";
        return false;
    }
    float low = 0.0f;
    float high = 1.0f;
    if (minimum)
    {
        low = *minimum;
        high = *maximum;
    }
    else
    {
        const auto range = std::minmax_element(values.begin(), values.end());
        low = *range.first;
        high = *range.second;
        if (low == high)
        {
            const float padding = std::max(1.0f, std::abs(low) * 0.1f);
            low -= padding;
            high += padding;
        }
    }
    points.reserve(values.size());
    const float xStep = values.size() > 1
        ? bounds.width / static_cast<float>(values.size() - 1) : 0.0f;
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        const float normalized = std::clamp(
            (values[index] - low) / (high - low), 0.0f, 1.0f);
        points.push_back({ bounds.x + xStep * static_cast<float>(index),
            bounds.y + bounds.height * (1.0f - normalized) });
    }
    return true;
}

bool BuildDrawShadowLayers(const DrawRect& bounds, float blur,
    float radius, float offsetX, float offsetY, float alpha,
    std::vector<DrawShadowLayer>& layers, std::string& error) noexcept
{
    error.clear();
    layers.clear();
    if (!ValidRect(bounds) || !std::isfinite(blur) || blur < 0.0f ||
        blur > 64.0f || !std::isfinite(radius) || radius < 0.0f ||
        radius > std::min(bounds.width, bounds.height) * 0.5f ||
        !Bounded(offsetX) ||
        !Bounded(offsetY) || !std::isfinite(alpha) ||
        alpha < 0.0f || alpha > 1.0f)
    {
        error = "draw shadow values must be finite and bounded";
        return false;
    }
    const int count = blur <= 0.0f ? 1 : std::clamp(
        static_cast<int>(std::ceil(blur * 0.5f)), 2, 16);
    const DrawRect outerBounds{
        bounds.x + offsetX - blur,
        bounds.y + offsetY - blur,
        bounds.width + blur * 2.0f,
        bounds.height + blur * 2.0f,
    };
    if (!ValidRect(outerBounds))
    {
        error = "draw shadow expanded bounds must remain finite and bounded";
        return false;
    }
    float weightSum = 0.0f;
    for (int index = 1; index <= count; ++index)
        weightSum += static_cast<float>(index * index);
    layers.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index)
    {
        const float outward = count == 1 ? 0.0f : blur *
            static_cast<float>(count - index) /
            static_cast<float>(count);
        const float weight = static_cast<float>((index + 1) * (index + 1));
        layers.push_back({
            { bounds.x + offsetX - outward,
                bounds.y + offsetY - outward,
                bounds.width + outward * 2.0f,
                bounds.height + outward * 2.0f },
            radius + outward,
            alpha * weight / weightSum,
        });
    }
    return true;
}

bool ShouldScrollDrawMarquee(float textWidth, float viewportWidth) noexcept
{
    return std::isfinite(textWidth) && std::isfinite(viewportWidth) &&
        textWidth > 0.0f && viewportWidth > 0.0f &&
        textWidth > viewportWidth + 0.5f;
}

float AdvanceDrawMarqueeOffset(float offset, float deltaMilliseconds,
    float speed, float cycle) noexcept
{
    if (!std::isfinite(offset) || !std::isfinite(deltaMilliseconds) ||
        !std::isfinite(speed) || !std::isfinite(cycle) ||
        deltaMilliseconds <= 0.0f || speed <= 0.0f || cycle <= 0.0f)
        return std::isfinite(offset) && offset >= 0.0f ? offset : 0.0f;
    const float boundedDelta = std::min(deltaMilliseconds, 100.0f);
    const float advanced = std::fmod(
        std::max(0.0f, offset) + speed * boundedDelta / 1000.0f,
        cycle);
    return advanced >= 0.0f ? advanced : advanced + cycle;
}
}

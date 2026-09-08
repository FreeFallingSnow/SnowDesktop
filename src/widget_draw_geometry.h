#pragma once

#include <optional>
#include <string>
#include <vector>

namespace snowdesktop::widget_runtime
{
struct DrawPoint
{
    float x = 0.0f;
    float y = 0.0f;

    bool operator==(const DrawPoint&) const = default;
};

struct DrawRect
{
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    bool operator==(const DrawRect&) const = default;
};

enum class DrawImageFit
{
    Fill,
    Contain,
    Cover,
    None,
};

enum class DrawImageAlignment
{
    Start,
    Center,
    End,
};

struct DrawImagePlacement
{
    DrawRect source;
    DrawRect destination;
    bool valid = false;
};

DrawImagePlacement ResolveDrawImagePlacement(float sourceWidth,
    float sourceHeight, const DrawRect& destination, DrawImageFit fit,
    DrawImageAlignment alignment) noexcept;

bool ValidateDrawImageTransform(float rotationDegrees, float originX,
    float originY, std::string& error) noexcept;

enum class DrawPathCommandType
{
    Move,
    Line,
    Cubic,
    Quadratic,
    Close,
};

struct DrawPathCommand
{
    DrawPathCommandType type = DrawPathCommandType::Move;
    DrawPoint point;
    DrawPoint control1;
    DrawPoint control2;
};

bool ValidateDrawPath(const std::vector<DrawPathCommand>& commands,
    std::string& error) noexcept;

struct DrawArcPiece
{
    DrawPoint start;
    DrawPoint end;
    float radius = 0.0f;
    bool clockwise = true;
};

bool BuildDrawArc(float centerX, float centerY, float radius,
    float startDegrees, float sweepDegrees,
    std::vector<DrawArcPiece>& pieces, std::string& error) noexcept;

bool BuildDrawSparkline(const std::vector<float>& values,
    const DrawRect& bounds, std::optional<float> minimum,
    std::optional<float> maximum, std::vector<DrawPoint>& points,
    std::string& error) noexcept;

struct DrawShadowLayer
{
    DrawRect bounds;
    float radius = 0.0f;
    float alpha = 0.0f;
};

bool BuildDrawShadowLayers(const DrawRect& bounds, float blur,
    float radius, float offsetX, float offsetY, float alpha,
    std::vector<DrawShadowLayer>& layers, std::string& error) noexcept;

bool ShouldScrollDrawMarquee(float textWidth, float viewportWidth) noexcept;

float AdvanceDrawMarqueeOffset(float offset, float deltaMilliseconds,
    float speed, float cycle) noexcept;
}

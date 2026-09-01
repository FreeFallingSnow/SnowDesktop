#include "widget_draw_geometry.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace
{
using namespace snowdesktop::widget_runtime;

void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

bool Near(float left, float right)
{
    return std::abs(left - right) < 0.01f;
}

void TestImagePlacement()
{
    const DrawRect bounds{ 10.0f, 20.0f, 100.0f, 100.0f };
    const auto contain = ResolveDrawImagePlacement(
        200.0f, 100.0f, bounds, DrawImageFit::Contain,
        DrawImageAlignment::Center);
    Check(contain.valid && Near(contain.destination.x, 10.0f) &&
            Near(contain.destination.y, 45.0f) &&
            Near(contain.destination.width, 100.0f) &&
            Near(contain.destination.height, 50.0f),
        "contain must preserve aspect ratio and center unused space");

    const auto cover = ResolveDrawImagePlacement(
        200.0f, 100.0f, bounds, DrawImageFit::Cover,
        DrawImageAlignment::End);
    Check(cover.valid && Near(cover.source.x, 100.0f) &&
            Near(cover.source.width, 100.0f) &&
            cover.destination == bounds,
        "cover must crop the source using the requested alignment");

    const auto none = ResolveDrawImagePlacement(
        40.0f, 30.0f, bounds, DrawImageFit::None,
        DrawImageAlignment::End);
    Check(none.valid && Near(none.destination.x, 70.0f) &&
            Near(none.destination.y, 90.0f) &&
            Near(none.destination.width, 40.0f) &&
            Near(none.destination.height, 30.0f),
        "none must retain native size and align within the destination");
}

void TestImageTransformValidation()
{
    std::string error;
    Check(ValidateDrawImageTransform(-16.0f, 0.9f, 0.08f, error),
        "bounded image rotation and normalized pivot must validate");
    Check(!ValidateDrawImageTransform(
            std::numeric_limits<float>::infinity(), 0.5f, 0.5f, error),
        "image rotation must reject non-finite angles");
    Check(!ValidateDrawImageTransform(361.0f, 0.5f, 0.5f, error),
        "image rotation must reject more than one turn");
    Check(!ValidateDrawImageTransform(0.0f, -0.01f, 0.5f, error) &&
            !ValidateDrawImageTransform(0.0f, 0.5f, 1.01f, error),
        "image rotation must reject pivots outside normalized bounds");
}

void TestPathValidation()
{
    std::string error;
    const std::vector<DrawPathCommand> valid = {
        { DrawPathCommandType::Move, { 0.0f, 0.0f } },
        { DrawPathCommandType::Line, { 20.0f, 0.0f } },
        { DrawPathCommandType::Quadratic, { 20.0f, 20.0f },
            { 30.0f, 10.0f } },
        { DrawPathCommandType::Close },
    };
    Check(ValidateDrawPath(valid, error),
        "a bounded path beginning with move must validate");
    auto invalid = valid;
    invalid.front().type = DrawPathCommandType::Line;
    Check(!ValidateDrawPath(invalid, error) &&
            error.find("begin with move") != std::string::npos,
        "paths must reject drawing before the first move");
    invalid = valid;
    invalid[1].point.x = std::numeric_limits<float>::infinity();
    Check(!ValidateDrawPath(invalid, error) &&
            error.find("bounded") != std::string::npos,
        "paths must reject non-finite coordinates");
}

void TestArcSegmentation()
{
    std::vector<DrawArcPiece> pieces;
    std::string error;
    Check(BuildDrawArc(50.0f, 40.0f, 20.0f, 0.0f, 360.0f,
            pieces, error) && pieces.size() == 3 &&
            Near(pieces.front().start.x, 70.0f) &&
            Near(pieces.back().end.x, 70.0f) &&
            pieces.front().clockwise,
        "a full clockwise arc must split into safe Direct2D pieces");
    Check(BuildDrawArc(0.0f, 0.0f, 10.0f, 90.0f, -180.0f,
            pieces, error) && pieces.size() == 2 &&
            !pieces.front().clockwise,
        "negative sweeps must preserve counter-clockwise direction");
    Check(!BuildDrawArc(0.0f, 0.0f, 10.0f, 0.0f, 361.0f,
            pieces, error),
        "arcs must reject sweeps larger than one turn");
    Check(!BuildDrawArc(999999.0f, 0.0f, 10.0f, 0.0f, 90.0f,
            pieces, error),
        "arcs must reject geometry extending beyond the coordinate budget");
}

void TestSparklineGeometry()
{
    std::vector<DrawPoint> points;
    std::string error;
    Check(BuildDrawSparkline({ 0.0f, 5.0f, 10.0f },
            { 0.0f, 10.0f, 100.0f, 40.0f }, 0.0f, 10.0f,
            points, error) && points.size() == 3 &&
            Near(points[0].x, 0.0f) && Near(points[0].y, 50.0f) &&
            Near(points[1].x, 50.0f) && Near(points[1].y, 30.0f) &&
            Near(points[2].x, 100.0f) && Near(points[2].y, 10.0f),
        "sparkline points must span the bounds and invert the value axis");
    Check(BuildDrawSparkline({ 3.0f, 3.0f },
            { 0.0f, 0.0f, 20.0f, 10.0f }, std::nullopt,
            std::nullopt, points, error) &&
            Near(points[0].y, points[1].y),
        "constant auto-ranges must remain finite and level");
    Check(!BuildDrawSparkline({ 1.0f, 2.0f },
            { 0.0f, 0.0f, 20.0f, 10.0f }, 2.0f, 1.0f,
            points, error),
        "sparkline explicit ranges must be increasing");
}

void TestShadowBudget()
{
    std::vector<DrawShadowLayer> layers;
    std::string error;
    Check(BuildDrawShadowLayers({ 10.0f, 10.0f, 80.0f, 40.0f },
            64.0f, 8.0f, 2.0f, 4.0f, 0.5f, layers, error) &&
            layers.size() == 16 &&
            layers.front().bounds.width > layers.back().bounds.width &&
            layers.front().alpha < layers.back().alpha,
        "shadow blur must use at most sixteen bounded falloff layers");
    Check(!BuildDrawShadowLayers({ 0.0f, 0.0f, 10.0f, 10.0f },
            65.0f, 0.0f, 0.0f, 0.0f, 1.0f, layers, error),
        "shadow blur must reject work beyond the public limit");
    Check(!BuildDrawShadowLayers({ 0.0f, 0.0f, 10.0f, 10.0f },
            8.0f, 6.0f, 0.0f, 0.0f, 1.0f, layers, error),
        "shadow radius must not exceed half the shortest side");
    Check(!BuildDrawShadowLayers(
            { 999990.0f, 0.0f, 10.0f, 10.0f }, 8.0f, 2.0f,
            0.0f, 0.0f, 1.0f, layers, error),
        "expanded shadows must remain inside the coordinate budget");
}

void TestMarqueeGeometry()
{
    Check(ShouldScrollDrawMarquee(120.0f, 100.0f) &&
            !ShouldScrollDrawMarquee(100.0f, 100.0f),
        "marquee text must animate only when it exceeds the viewport");
    Check(Near(AdvanceDrawMarqueeOffset(
            118.0f, 250.0f, 24.0f, 120.0f), 0.4f),
        "marquee catch-up must be capped to avoid a visible jump");
    Check(Near(AdvanceDrawMarqueeOffset(
            119.0f, 50.0f, 24.0f, 120.0f), 0.2f),
        "marquee offsets must wrap continuously within the cycle");
}
}

int main()
{
    TestImagePlacement();
    TestImageTransformValidation();
    TestPathValidation();
    TestArcSegmentation();
    TestSparklineGeometry();
    TestShadowBudget();
    TestMarqueeGeometry();
    std::cout << "widget draw geometry tests passed\n";
    return 0;
}

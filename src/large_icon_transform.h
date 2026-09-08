#pragma once
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace snowdesktop::large_icon_transform
{
struct Card
{
    D2D1_MATRIX_4X4_F matrix = D2D1::Matrix4x4F();
    float light = 1;
    bool active = false;
};
inline Card Resolve(float width, float height, float x, float y, float strength)
{
    Card result;
    if (!(width > 0 && height > 0) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(strength)) return result;
    x = std::clamp(x, -1.f, 1.f); y = std::clamp(y, -1.f, 1.f);
    strength = std::clamp(strength, 0.f, 2.f);
    result.active = (std::abs(x) + std::abs(y)) * strength > .00001f;
    if (!result.active) return result;
    // Both D2D and the native backdrop visual use this local row-vector matrix.
    result.matrix = D2D1::Matrix4x4F::Translation(-width / 2, -height / 2, 0) *
        D2D1::Matrix4x4F::RotationX(-y * 8 * strength) *
        D2D1::Matrix4x4F::RotationY(x * 8 * strength) *
        D2D1::Matrix4x4F::PerspectiveProjection(std::max(width, height) * 3) *
        D2D1::Matrix4x4F::Translation(width / 2, height / 2, 0);
    result.light = 1 + .08f * strength * (.4f * x - .6f * y);
    return result;
}
inline D2D1_POINT_2F Project(const D2D1_MATRIX_4X4_F& matrix, float x, float y)
{
    const float w = x * matrix._14 + y * matrix._24 + matrix._44;
    return {(x * matrix._11 + y * matrix._21 + matrix._41) / w,
        (x * matrix._12 + y * matrix._22 + matrix._42) / w};
}
inline RECT Bounds(RECT frame, const D2D1_MATRIX_4X4_F& matrix)
{
    const float w = static_cast<float>(frame.right - frame.left), h = static_cast<float>(frame.bottom - frame.top);
    const std::array corners{Project(matrix, 0, 0), Project(matrix, w, 0), Project(matrix, 0, h), Project(matrix, w, h)};
    float left = corners[0].x, top = corners[0].y, right = left, bottom = top;
    for (const auto& point : corners)
    { left = std::min(left, point.x); top = std::min(top, point.y); right = std::max(right, point.x); bottom = std::max(bottom, point.y); }
    return {frame.left + static_cast<LONG>(std::floor(left)), frame.top + static_cast<LONG>(std::floor(top)),
        frame.left + static_cast<LONG>(std::ceil(right)), frame.top + static_cast<LONG>(std::ceil(bottom))};
}
}

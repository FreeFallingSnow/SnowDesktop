#include "demo_cursor.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace snowdesktop::demo_cursor
{
namespace
{
struct Point { float x, y; };
constexpr std::array<Point, 7> Arrow{{{0, 0}, {0, 24}, {5.6f, 18.4f}, {10, 28}, {14.6f, 25.8f}, {10.3f, 16.5f}, {18.2f, 16.5f}}};
float Distance(float x, float y)
{
    bool inside = false;
    float distance = std::numeric_limits<float>::max();
    for (std::size_t i = 0, j = Arrow.size() - 1; i < Arrow.size(); j = i++)
    {
        const auto a = Arrow[i], b = Arrow[j];
        if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) inside = !inside;
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float t = std::clamp(((x - a.x) * dx + (y - a.y) * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
        distance = std::min(distance, std::hypot(x - a.x - t * dx, y - a.y - t * dy));
    }
    return inside ? -distance : distance;
}
void Blend(std::uint8_t* destination, float r, float g, float b, float alpha)
{
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    const float inverse = 1.0f - alpha;
    destination[0] = static_cast<std::uint8_t>(std::clamp(b * alpha * 255 + destination[0] * inverse, 0.0f, 255.0f));
    destination[1] = static_cast<std::uint8_t>(std::clamp(g * alpha * 255 + destination[1] * inverse, 0.0f, 255.0f));
    destination[2] = static_cast<std::uint8_t>(std::clamp(r * alpha * 255 + destination[2] * inverse, 0.0f, 255.0f));
    destination[3] = static_cast<std::uint8_t>(std::clamp(alpha * 255 + destination[3] * inverse, 0.0f, 255.0f));
}
}

void Draw(std::span<std::uint8_t> pixels, int width, int height, int stride, const Pose& pose)
{
    if (width <= 0 || height <= 0 || width > 32768 || height > 32768 || stride < width * 4 ||
        pixels.size() < static_cast<std::size_t>(stride) * height || !std::isfinite(pose.scale) ||
        !std::isfinite(pose.x) || !std::isfinite(pose.y) || pose.scale < .25f || pose.scale > 8.f) return;
    const auto disc = [&](float cx, float cy, float radius, float alpha, bool ring) {
        if (!std::isfinite(cx) || !std::isfinite(cy) || cx < -radius - 2 || cy < -radius - 2 ||
            cx > width + radius + 2 || cy > height + radius + 2) return;
        const int left = std::max(0, static_cast<int>(std::floor(cx - radius - 2)));
        const int right = std::min(width, static_cast<int>(std::ceil(cx + radius + 2)));
        const int top = std::max(0, static_cast<int>(std::floor(cy - radius - 2)));
        const int bottom = std::min(height, static_cast<int>(std::ceil(cy + radius + 2)));
        for (int y = top; y < bottom; ++y) for (int x = left; x < right; ++x)
        {
            const float distance = std::hypot(x + .5f - cx, y + .5f - cy);
            const float coverage = ring ? std::clamp(1.5f - std::abs(distance - radius), 0.f, 1.f) : std::clamp(radius + .5f - distance, 0.f, 1.f);
            Blend(pixels.data() + static_cast<std::size_t>(y) * stride + x * 4, .08f, .45f, .88f, alpha * coverage);
        }
    };
    for (std::size_t index = 0; index < pose.trail.size(); ++index)
        disc(static_cast<float>(pose.trail[index].x), static_cast<float>(pose.trail[index].y), 1.8f * pose.scale,
            .25f * static_cast<float>(index + 1) / static_cast<float>(pose.trail.size()), false);
    if (pose.pressed) disc(pose.x, pose.y, 8 * pose.scale, .22f, false);
    if (pose.clickAgeMilliseconds >= 0 && pose.clickAgeMilliseconds < 400)
    {
        const float t = pose.clickAgeMilliseconds / 400;
        disc(pose.x, pose.y, (6 + 14 * t) * pose.scale, .8f * (1 - t), true);
    }
    if (pose.x < -34 * pose.scale || pose.y < -34 * pose.scale ||
        pose.x > width + 4 * pose.scale || pose.y > height + 4 * pose.scale) return;
    const int left = std::max(0, static_cast<int>(std::floor(pose.x - 3 * pose.scale)));
    const int top = std::max(0, static_cast<int>(std::floor(pose.y - 3 * pose.scale)));
    const int right = std::min(width, static_cast<int>(std::ceil(pose.x + 23 * pose.scale)));
    const int bottom = std::min(height, static_cast<int>(std::ceil(pose.y + 33 * pose.scale)));
    for (int y = top; y < bottom; ++y) for (int x = left; x < right; ++x)
    {
        const float ux = (x + .5f - pose.x) / pose.scale, uy = (y + .5f - pose.y) / pose.scale;
        const float d = Distance(ux, uy);
        auto* destination = pixels.data() + static_cast<std::size_t>(y) * stride + x * 4;
        const float shadow = std::clamp(2.8f - Distance(ux - 1, uy - 1.4f), 0.f, 1.f) * .2f;
        Blend(destination, 0, 0, 0, shadow);
        const float outline = std::clamp(1.4f - d * pose.scale, 0.f, 1.f);
        Blend(destination, .05f, .06f, .08f, outline);
        const float fill = std::clamp(-d * pose.scale - .1f, 0.f, 1.f);
        const float shade = pose.pressed ? .88f : 1.f;
        Blend(destination, shade, shade, shade, fill);
    }
}

struct Overlay::Impl
{
    HWND window = nullptr;
    HDC memory = nullptr;
    HBITMAP bitmap = nullptr, previous = nullptr;
    void* bits = nullptr;
    int width = 0, height = 0;
    ~Impl()
    {
        if (window) DestroyWindow(window);
        if (memory && previous) SelectObject(memory, previous);
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
    }
};
Overlay::Overlay() : impl_(std::make_unique<Impl>()) {}
Overlay::~Overlay() = default;
void Overlay::Hide() noexcept { if (impl_->window) ShowWindow(impl_->window, SW_HIDE); }
bool Overlay::Show(const Pose& screenPose)
{
    if (!std::isfinite(screenPose.x) || !std::isfinite(screenPose.y) || !std::isfinite(screenPose.scale) ||
        std::abs(screenPose.x) > 1000000 || std::abs(screenPose.y) > 1000000 ||
        screenPose.scale < .25f || screenPose.scale > 8.f) return false;
    if (!impl_->window)
    {
        impl_->window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            L"STATIC", L"SnowDesktop Demo Cursor", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!impl_->window) return false;
    }
    if (!impl_->memory) impl_->memory = CreateCompatibleDC(nullptr);
    if (!impl_->memory) return false;
    const int padding = static_cast<int>(std::ceil(44 * std::clamp(screenPose.scale, .25f, 8.f)));
    int left = static_cast<int>(std::floor(screenPose.x)) - padding, top = static_cast<int>(std::floor(screenPose.y)) - padding;
    int right = left + padding * 2, bottom = top + padding * 2;
    for (const auto point : screenPose.trail)
    {
        if (std::abs(static_cast<double>(point.x)) > 1000000 || std::abs(static_cast<double>(point.y)) > 1000000) return false;
        left = std::min(left, static_cast<int>(point.x) - padding); top = std::min(top, static_cast<int>(point.y) - padding);
        right = std::max(right, static_cast<int>(point.x) + padding); bottom = std::max(bottom, static_cast<int>(point.y) + padding);
    }
    const int width = right - left, height = bottom - top;
    if (width <= 0 || height <= 0 || width > 2048 || height > 2048) return false;
    if (impl_->width != width || impl_->height != height)
    {
        if (impl_->previous) SelectObject(impl_->memory, impl_->previous);
        if (impl_->bitmap) DeleteObject(impl_->bitmap);
        impl_->bitmap = nullptr; impl_->bits = nullptr; impl_->width = impl_->height = 0;
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        impl_->bitmap = CreateDIBSection(impl_->memory, &info, DIB_RGB_COLORS, &impl_->bits, nullptr, 0);
        if (!impl_->bitmap) return false;
        impl_->previous = static_cast<HBITMAP>(SelectObject(impl_->memory, impl_->bitmap));
        impl_->width = width; impl_->height = height;
    }
    const auto bytes = static_cast<std::size_t>(width) * height * 4;
    auto pixels = std::span(static_cast<std::uint8_t*>(impl_->bits), bytes);
    std::fill(pixels.begin(), pixels.end(), std::uint8_t{});
    auto pose = screenPose; pose.x -= static_cast<float>(left); pose.y -= static_cast<float>(top);
    for (auto& point : pose.trail) { point.x -= left; point.y -= top; }
    Draw(pixels, width, height, width * 4, pose);
    POINT destination{left, top}, source{}; SIZE size{width, height};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    if (!UpdateLayeredWindow(impl_->window, nullptr, &destination, &size, impl_->memory, &source, 0, &blend, ULW_ALPHA)) return false;
    SetWindowPos(impl_->window, HWND_TOPMOST, left, top, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}
}

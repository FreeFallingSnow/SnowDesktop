#pragma once

#include <windows.h>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace snowdesktop::demo_cursor
{
struct Pose
{
    float x = 0, y = 0, scale = 1;
    bool pressed = false;
    float clickAgeMilliseconds = -1;
    std::vector<POINT> trail;
};
// BGRA premultiplied alpha, top-down. Draws over an existing image.
void Draw(std::span<std::uint8_t> pixels, int width, int height, int stride, const Pose& pose);

class Overlay final
{
public:
    Overlay();
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;
    bool Show(const Pose& screenPose);
    void Hide() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

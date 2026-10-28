#pragma once

#include "platform/control/control_protocol.h"
#include "ui/render/demo_cursor.h"
#include <windows.h>
#include <string>
#include <vector>

namespace snowdesktop::demo
{
inline constexpr unsigned SceneSchemaVersion = 1;
struct Item
{
    std::string id, title, glyph, category = "all", dateGroup;
    std::uint32_t color = 0x527aa0;
    bool directory = false;
};
struct Surface
{
    std::string id, kind, title, search, category = "all";
    RECT bounds{};
    std::vector<std::string> items;
    bool visible = true, listMode = false, showSearch = true, showCategories = true;
    bool scrollContainer = true;
};
struct Step
{
    int atMs = 0, durationMs = 0;
    std::string action, target, value, element;
    bool elementResolved = false;
    POINT point{};
    SIZE size{};
};
struct Scene
{
    std::string name, locale = "en-US", appearance = "glass-light", clock = "2026/10/29  09:41";
    int width = 1920, height = 1080, fps = 30, durationMs = 8000;
    std::vector<Item> items;
    std::vector<Surface> surfaces;
    std::vector<Step> timeline;
    control::Value source;
    unsigned FrameCount() const;
    std::uint64_t FrameMilliseconds(unsigned index) const;
};
struct Presentation
{
    Surface surface;
    bool visible = true;
    float scale = 1, opacity = 1;
};
struct Frame
{
    std::vector<Presentation> surfaces;
    demo_cursor::Pose cursor;
};
Scene ParseScene(const control::Value& value);
Frame Evaluate(const Scene& scene, std::uint64_t milliseconds);
Frame EvaluateLayout(const Scene& scene, std::uint64_t milliseconds);
}

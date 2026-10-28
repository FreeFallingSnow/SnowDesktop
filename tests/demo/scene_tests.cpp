#include "demo/scene.h"
#include <iostream>
#include <stdexcept>

namespace
{
using namespace snowdesktop;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
control::Value Input()
{
    control::Value value;
    Require(ParseJson(R"({"schemaVersion":1,"width":1280,"height":720,"fps":30,"durationMs":1001,
      "items":[{"id":"notes","title":"会议笔记","category":"documents"},{"id":"photos","title":"旅行照片","category":"images"}],
      "surfaces":[{"id":"work","kind":"files","x":40,"y":80,"width":400,"height":300}],
      "timeline":[
        {"atMs":0,"action":"cursor.move","durationMs":300,"x":200,"y":200},
        {"atMs":20,"action":"surface.move","target":"work","durationMs":200,"x":100,"y":100},
        {"atMs":260,"action":"surface.resize","target":"work","durationMs":150,"width":500,"height":300},
        {"atMs":300,"action":"cursor.down"},{"atMs":360,"action":"cursor.up"},
        {"atMs":400,"action":"surface.hide","target":"work"},
        {"atMs":600,"action":"surface.view","target":"work","value":"list"},
        {"atMs":650,"action":"surface.search","target":"work","value":"笔记"},
        {"atMs":680,"action":"surface.category","target":"work","value":"documents"},
        {"atMs":700,"action":"surface.show","target":"work"}]})", value), "parse scene fixture");
    return value;
}
template<class Modify> void Reject(Modify modify)
{
    auto value = Input(); modify(value);
    try { (void)demo::ParseScene(value); }
    catch (const control::Error&) { return; }
    throw std::runtime_error("invalid scene was accepted");
}
}
int main()
{
    try
    {
        auto scene = demo::ParseScene(Input());
        Require(scene.FrameCount() == 31 && scene.FrameMilliseconds(30) == 1000, "partial final frame is included without duration drift");
        const auto pressed = demo::Evaluate(scene, 300);
        Require(pressed.cursor.pressed && pressed.cursor.x == 200 && pressed.cursor.y == 200, "pointer endpoint and press have exact virtual time");
        const auto released = demo::Evaluate(scene, 420);
        Require(!released.cursor.pressed && released.cursor.clickAgeMilliseconds == 60 && !released.cursor.trail.empty(), "release ring and trail advance during an idle frame");
        Require(!demo::Evaluate(scene, 650).surfaces[0].visible && demo::Evaluate(scene, 950).surfaces[0].visible,
            "production popup motion closes and reopens on the virtual clock");
        const auto final = demo::Evaluate(scene, 1001);
        const auto& surface = final.surfaces[0].surface;
        Require(surface.bounds.left == 100 && surface.bounds.top == 100 && surface.bounds.right == 600 && surface.bounds.bottom == 400,
            "native surface transforms preserve size and final bounds");
        Require(surface.listMode && surface.search == "笔记" && surface.category == "documents", "view, search and category changes are observable scene state");
        for (unsigned index = scene.FrameCount(); index-- > 0;)
        {
            const auto time = scene.FrameMilliseconds(index);
            const auto first = demo::Evaluate(scene, time), again = demo::Evaluate(scene, time);
            Require(first.cursor.x == again.cursor.x && first.cursor.y == again.cursor.y &&
                first.surfaces[0].scale == again.surfaces[0].scale, "random access evaluation is repeatable without a wall clock");
        }
        Reject([](auto& value) { value.object.emplace("filePath", control::String("C:/personal/layout.json")); });
        Reject([](auto& value) { value.object["locale"] = control::String("system"); });
        Reject([](auto& value) { value.object["width"] = control::Number(1279); });
        Reject([](auto& value) { value.object["width"] = control::Number(3840); value.object["height"] = control::Number(2160); value.object["durationMs"] = control::Number(30000); value.object["fps"] = control::Number(60); });
        Reject([](auto& value) { value.object["surfaces"].array[0].object["items"] = control::Array(); value.object["surfaces"].array[0].object["items"].array.push_back(control::String("unknown")); });
        Reject([](auto& value) { value.object["timeline"].array[1].object["x"] = control::Number(1200); });
        Reject([](auto& value) { value.object["timeline"].array[1].object["target"] = control::String("unrelated"); });
        Reject([](auto& value) { value.object["timeline"].array[5].object["atMs"] = control::Number(10); });
        Reject([](auto& value) { value.object["timeline"].array.erase(value.object["timeline"].array.begin() + 4); });
        Reject([](auto& value) { value.object["surfaces"].array[0].object["kind"] = control::String("dock"); });
        std::cout << "fixed scene and virtual timeline checks passed\n"; return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

#include "platform/control/control_elements.h"
#include "ui/input/control_pointer.h"
#include "ui/render/demo_cursor.h"
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace
{
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Function> void Reject(Function function, const char* code)
{
    try { function(); }
    catch (const snowdesktop::control::Error& error) { Check(error.code == code, "wrong element rejection"); return; }
    throw std::runtime_error("unsafe element operation was accepted");
}
}

// Public IPC catalogs must not silently target a different item or expose secret
// values. These checks use independent rectangles/IDs and exercise production
// serialization/resolution; they do not stand in for desktop interaction tests.
void ApplicationControlElementChecks()
{
    using namespace snowdesktop::control;
    Check(ElementKey("a/b") == "a%2fb" && ElementKey("a%2fb") == "a%252fb", "IDs must be collision-free across path separators");
    ElementSnapshot snapshot;
    Element parent; parent.id = "group"; parent.bounds = parent.clip = {-1200, -300, -200, 600};
    Element button; button.id = "button"; button.parent = "group"; button.bounds = {-500, -20, -200, 80};
    button.clip = {-400, -100, -250, 100}; button.actions = {"click"}; button.password = true; button.value = "secret";
    snapshot.elements = {parent, button}; snapshot.Finish();
    Check(!ElementJson(button).Find("value"), "password values cannot be serialized");
    const auto center = ElementCenter(snapshot.Resolve("button", "click"));
    Check(center.x == -325 && center.y == 30, "target must use visible physical-screen intersection");
    Reject([&] { snapshot.Resolve("absent", "click"); }, "element_not_found");
    Reject([&] { snapshot.Resolve("button", "drag"); }, "unsupported_action");
    const auto revision = snapshot.revision;
    snapshot.elements[1].enabled = false; snapshot.Finish();
    Check(snapshot.revision != revision, "state changes must invalidate prepared actions");
    Reject([&] { snapshot.Resolve("button", "click"); }, "element_disabled");
    snapshot.elements[1].enabled = true; snapshot.elements[1].clip = {0, 0, 1, 1}; snapshot.Finish();
    Check(!ElementJson(snapshot.elements[1]).Find("visible")->boolean, "disjoint clipping means invisible");
    Reject([&] { snapshot.Resolve("button", "click"); }, "element_not_visible");
    snapshot.elements[1].id = "group";
    Reject([&] { snapshot.Finish(); }, "ambiguous_element");
    snapshot.elements[1] = button; snapshot.elements[0].parent = "button";
    Reject([&] { snapshot.Finish(); }, "invalid_element_tree");
    snapshot.elements[0].parent = "missing";
    Reject([&] { snapshot.Finish(); }, "invalid_element_tree");

    using namespace snowdesktop::controlled_pointer;
    LONG x = 0, y = 0;
    Check(AbsolutePoint({-1920, -200}, {-1920, -200, 1920, 1080}, x, y) && x == 0 && y == 0, "negative display origin maps to zero");
    Check(AbsolutePoint({1919, 1079}, {-1920, -200, 1920, 1080}, x, y) && x == 65535 && y == 65535, "last physical pixel maps to endpoint");
    Check(!AbsolutePoint({1920, 0}, {-1920, -200, 1920, 1080}, x, y), "exclusive right edge must be rejected");
    const POINT start{-500, 20}, end{900, 700};
    const auto first = Interpolate(start, end, 0), last = Interpolate(start, end, 1);
    Check(first.x == start.x && first.y == start.y && last.x == end.x && last.y == end.y, "curved pointer path retains exact endpoints");
    Player player; Plan invalid; invalid.sourceId = "button"; invalid.durationMilliseconds = 0;
    std::string error;
    Check(!player.Start(invalid, [](const std::string&) { return Target{}; }, {}, error) && error == "invalid_pointer_plan", "invalid plans cannot inject input");

    using namespace snowdesktop::demo_cursor;
    std::vector<std::uint8_t> pixels(128 * 96 * 4);
    Pose pose; pose.x = 40; pose.y = 20; pose.pressed = true; pose.clickAgeMilliseconds = 120;
    pose.trail = {{10, 10}, {15, 12}, {25, 16}};
    Draw(pixels, 128, 96, 128 * 4, pose);
    unsigned visible = 0;
    for (std::size_t index = 0; index < pixels.size(); index += 4)
    {
        const auto alpha = pixels[index + 3]; if (alpha) ++visible;
        Check(pixels[index] <= alpha && pixels[index + 1] <= alpha && pixels[index + 2] <= alpha, "layered cursor requires premultiplied alpha");
    }
    Check(visible > 200 && visible < 2500, "cursor, trail and click ring must produce a bounded visual");
    Check(pixels[3] == 0 && pixels.back() == 0, "cursor cannot paint outside its local visual bounds");
    const auto reference = pixels;
    pose.x = std::numeric_limits<float>::max(); pose.trail.clear();
    Draw(pixels, 128, 96, 128 * 4, pose);
    Check(pixels == reference, "offscreen cursor coordinates cannot overflow pixel bounds");
}

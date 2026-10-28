#include "scene.h"
#include "layout/popup_animation_rules.h"
#include "ui/input/control_pointer.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace snowdesktop::demo
{
namespace
{
using namespace control;
[[noreturn]] void Invalid(const char* message) { throw Error("invalid_scene", message, -32602); }
std::string StringField(const Value& object, const char* key, std::string fallback, std::size_t maximum, bool empty = false)
{
    const auto* value = object.Find(key);
    if (!value) return fallback;
    if (!value->IsString() || value->string.size() > maximum || (!empty && value->string.empty()) || value->string.find('\0') != std::string::npos)
        Invalid("Invalid bounded scene string");
    // Validate UTF-8 independently of a permissive renderer conversion.
    if (!value->string.empty() && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value->string.data(), static_cast<int>(value->string.size()), nullptr, 0))
        Invalid("Scene text must be valid UTF-8");
    return value->string;
}
std::string Identifier(const Value& object, const char* key)
{
    const auto id = StringField(object, key, {}, 64);
    if (id.empty() || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'; }))
        Invalid("Scene identities use ASCII letters, digits, dash or underscore");
    return id;
}
int NumberField(const Value& object, const char* key, int fallback, int minimum, int maximum)
{
    return object.Find(key) ? Integer(object, key, minimum, maximum) : fallback;
}
bool Flag(const Value& object, const char* key, bool fallback)
{
    const auto* value = object.Find(key); if (!value) return fallback;
    if (!value->IsBoolean()) Invalid("Scene flag must be boolean"); return value->boolean;
}
const std::vector<Value>& Entries(const Value& object, const char* key, std::size_t maximum, bool required)
{
    const auto* value = object.Find(key);
    static const std::vector<Value> empty;
    if (!value && !required) return empty;
    if (!value || !value->IsArray() || value->array.size() > maximum || (required && value->array.empty()))
        Invalid("Scene array is missing, empty or exceeds its limit");
    return value->array;
}
void Bounds(const Scene& scene, RECT rect)
{
    if (rect.left < 0 || rect.top < 0 || rect.right > scene.width || rect.bottom > scene.height ||
        rect.right - rect.left < 32 || rect.bottom - rect.top < 32) Invalid("Scene surface is outside the canvas or too small");
}
struct Motion
{
    RECT from{}, to{};
    std::uint64_t start = 0;
    int duration = 0;
    RECT At(std::uint64_t time) const
    {
        if (!duration) return to;
        const auto p = popup_animation_rules::EaseInOutSmooth(static_cast<float>(time >= start ? time - start : 0) / duration);
        const auto mix = [&](LONG a, LONG b) { return static_cast<LONG>(std::lround(a + (b - a) * p)); };
        return {mix(from.left, to.left), mix(from.top, to.top), mix(from.right, to.right), mix(from.bottom, to.bottom)};
    }
};
struct CursorState
{
    POINT point{};
    bool pressed = false;
    std::int64_t releasedMs = -1;
};
CursorState CursorAt(const Scene& scene, std::uint64_t time)
{
    POINT from{scene.width * 4 / 5, scene.height * 3 / 4}, to = from;
    std::uint64_t start = 0; int duration = 0; CursorState state;
    const auto point = [&](std::uint64_t at) {
        return duration ? controlled_pointer::Interpolate(from, to, static_cast<double>(at >= start ? at - start : 0) / duration) : to;
    };
    for (const auto& step : scene.timeline)
    {
        if (static_cast<std::uint64_t>(step.atMs) > time) break;
        if (step.action == "cursor.move")
        {
            if (!step.element.empty() && !step.elementResolved) throw std::logic_error("demo_element_target_unresolved");
            from = point(step.atMs); to = step.point; start = step.atMs; duration = step.durationMs;
        }
        else if (step.action == "cursor.down") state.pressed = true;
        else if (step.action == "cursor.up") { state.pressed = false; state.releasedMs = step.atMs; }
    }
    state.point = point(time); return state;
}
}
unsigned Scene::FrameCount() const { return static_cast<unsigned>((static_cast<std::uint64_t>(durationMs) * fps + 999) / 1000); }
std::uint64_t Scene::FrameMilliseconds(unsigned index) const
{
    if (index >= FrameCount()) throw std::out_of_range("scene_frame_index");
    return static_cast<std::uint64_t>(index) * 1000 / fps;
}
Scene ParseScene(const Value& value)
{
    Fields(value, {"schemaVersion", "name", "locale", "appearance", "clock", "width", "height", "fps", "durationMs", "items", "surfaces", "timeline"});
    if (Integer(value, "schemaVersion", 1, 1) != SceneSchemaVersion) Invalid("Unsupported scene schema");
    Scene scene;
    scene.name = StringField(value, "name", "Desktop demonstration", 120);
    scene.locale = StringField(value, "locale", scene.locale, 35);
    if (scene.locale == "system") Invalid("An offline scene requires a fixed locale");
    scene.appearance = StringField(value, "appearance", scene.appearance, 32);
    if (scene.appearance != "dark" && scene.appearance != "light" && scene.appearance != "glass-dark" &&
        scene.appearance != "glass-light" && scene.appearance != "glass-transparent") Invalid("Unknown scene appearance");
    scene.clock = StringField(value, "clock", scene.clock, 64);
    scene.width = NumberField(value, "width", scene.width, 640, 3840);
    scene.height = NumberField(value, "height", scene.height, 360, 2160);
    scene.fps = NumberField(value, "fps", scene.fps, 1, 60);
    if ((scene.width & 1) || (scene.height & 1)) Invalid("Video canvas dimensions must be even");
    scene.durationMs = NumberField(value, "durationMs", scene.durationMs, 100, 30000);
    if (static_cast<std::uint64_t>(scene.width) * scene.height * scene.FrameCount() > 4000000000ULL)
        Invalid("Scene exceeds the bounded rendering work budget");
    std::unordered_set<std::string> items, surfaces;
    for (const auto& entry : Entries(value, "items", 128, true))
    {
        Fields(entry, {"id", "title", "glyph", "category", "dateGroup", "color", "directory"});
        Item item; item.id = Identifier(entry, "id");
        if (!items.insert(item.id).second) Invalid("Duplicate scene item identity");
        item.title = StringField(entry, "title", item.id, 384); item.glyph = StringField(entry, "glyph", "\xEF\x83\xB6", 16);
        item.category = StringField(entry, "category", "all", 64); item.dateGroup = StringField(entry, "dateGroup", {}, 128, true);
        item.color = static_cast<std::uint32_t>(NumberField(entry, "color", 0x527aa0, 0, 0xffffff));
        item.directory = Flag(entry, "directory", false); scene.items.push_back(std::move(item));
    }
    for (const auto& entry : Entries(value, "surfaces", 12, true))
    {
        Fields(entry, {"id", "kind", "title", "x", "y", "width", "height", "items", "visible", "listMode", "showSearch", "showCategories", "scrollContainer"});
        Surface surface; surface.id = Identifier(entry, "id");
        if (!surfaces.insert(surface.id).second) Invalid("Duplicate scene surface identity");
        surface.kind = StringField(entry, "kind", "collection", 32);
        if (surface.kind != "collection" && surface.kind != "files" && surface.kind != "mapping" && surface.kind != "popup" &&
            surface.kind != "dock" && surface.kind != "status-bar" && surface.kind != "navigation") Invalid("Unsupported native scene surface");
        surface.title = StringField(entry, "title", surface.id, 256);
        surface.bounds.left = Integer(entry, "x", 0, scene.width - 32); surface.bounds.top = Integer(entry, "y", 0, scene.height - 32);
        surface.bounds.right = surface.bounds.left + Integer(entry, "width", 32, scene.width);
        surface.bounds.bottom = surface.bounds.top + Integer(entry, "height", 32, scene.height); Bounds(scene, surface.bounds);
        surface.visible = Flag(entry, "visible", true); surface.listMode = Flag(entry, "listMode", false);
        surface.showSearch = Flag(entry, "showSearch", true); surface.showCategories = Flag(entry, "showCategories", true);
        surface.scrollContainer = Flag(entry, "scrollContainer", true);
        if (entry.Find("scrollContainer") && surface.kind != "collection") Invalid("Only collection supports scrollContainer");
        std::unordered_set<std::string> used;
        for (const auto& id : Entries(entry, "items", 128, false))
        {
            if (!id.IsString() || !items.contains(id.string) || !used.insert(id.string).second) Invalid("Surface item references must exist and be unique");
            surface.items.push_back(id.string);
        }
        if (surface.items.empty()) for (const auto& item : scene.items) surface.items.push_back(item.id);
        scene.surfaces.push_back(std::move(surface));
    }
    int previous = -1; bool pressed = false;
    for (const auto& entry : Entries(value, "timeline", 256, false))
    {
        Step step; step.action = Text(entry, "action"); step.atMs = Integer(entry, "atMs", 0, scene.durationMs - 1);
        if (step.atMs < previous) Invalid("Timeline must be in time order"); previous = step.atMs;
        if (step.action == "cursor.move")
        {
            Fields(entry, {"action", "atMs", "durationMs", "x", "y", "target", "element"});
            step.durationMs = Integer(entry, "durationMs", 1, scene.durationMs - step.atMs);
            if (entry.Find("element") || entry.Find("target"))
            {
                if (entry.Find("x") || entry.Find("y")) Invalid("Element cursor targets cannot also specify coordinates");
                step.target = Identifier(entry, "target"); step.element = StringField(entry, "element", {}, 384);
                const auto surface = std::find_if(scene.surfaces.begin(), scene.surfaces.end(), [&](const Surface& candidate) { return candidate.id == step.target; });
                if (step.element.empty() || surface == scene.surfaces.end() ||
                    (surface->kind != "collection" && surface->kind != "files" && surface->kind != "mapping"))
                    Invalid("Element cursor targets require a supported native widget surface");
            }
            else step.point = {Integer(entry, "x", 0, scene.width - 1), Integer(entry, "y", 0, scene.height - 1)};
        }
        else if (step.action == "cursor.down" || step.action == "cursor.up")
        {
            Fields(entry, {"action", "atMs"});
            const bool down = step.action == "cursor.down";
            if (pressed == down) Invalid("Cursor presses and releases must alternate"); pressed = down;
        }
        else
        {
            step.target = Identifier(entry, "target");
            if (!surfaces.contains(step.target)) Invalid("Timeline target does not exist");
            if (step.action == "surface.show" || step.action == "surface.hide") Fields(entry, {"action", "atMs", "target"});
            else if (step.action == "surface.move")
            {
                Fields(entry, {"action", "atMs", "target", "durationMs", "x", "y"});
                step.durationMs = Integer(entry, "durationMs", 1, scene.durationMs - step.atMs);
                step.point = {Integer(entry, "x", 0, scene.width - 32), Integer(entry, "y", 0, scene.height - 32)};
            }
            else if (step.action == "surface.resize")
            {
                Fields(entry, {"action", "atMs", "target", "durationMs", "width", "height"});
                step.durationMs = Integer(entry, "durationMs", 1, scene.durationMs - step.atMs);
                step.size = {Integer(entry, "width", 32, scene.width), Integer(entry, "height", 32, scene.height)};
            }
            else if (step.action == "surface.view" || step.action == "surface.search" || step.action == "surface.category")
            {
                Fields(entry, {"action", "atMs", "target", "value"});
                step.value = StringField(entry, "value", {}, 384, step.action == "surface.search");
                if (!entry.Find("value")) Invalid("Surface value is required");
                if (step.action == "surface.view" && step.value != "grid" && step.value != "list") Invalid("Unknown native surface view");
                const auto& kind = std::find_if(scene.surfaces.begin(), scene.surfaces.end(), [&](const Surface& surface) { return surface.id == step.target; })->kind;
                if (kind == "dock" || kind == "status-bar" ||
                    (step.action == "surface.search" && kind != "files" && kind != "mapping" && kind != "navigation") ||
                    (step.action == "surface.category" && kind != "files" && kind != "mapping"))
                    Invalid("Surface does not implement this native operation");
                if (step.action == "surface.category" && step.value != "all" &&
                    std::none_of(scene.items.begin(), scene.items.end(), [&](const Item& item) { return item.category == step.value; }))
                    Invalid("Scene category does not exist");
            }
            else Invalid("Unsupported scene timeline action");
        }
        scene.timeline.push_back(std::move(step));
    }
    if (pressed) Invalid("Scene cannot leave a cursor button pressed");
    // Validate every motion endpoint and interruption, not only final layout.
    for (const auto& step : scene.timeline) { (void)EvaluateLayout(scene, step.atMs); (void)EvaluateLayout(scene, step.atMs + step.durationMs); }
    (void)EvaluateLayout(scene, scene.durationMs);
    scene.source = value; return scene;
}
Frame EvaluateLayout(const Scene& scene, std::uint64_t milliseconds)
{
    struct Live { Surface surface; Motion motion; popup_animation_rules::State animation; };
    std::vector<Live> surfaces;
    for (const auto& surface : scene.surfaces)
    {
        Live live; live.surface = surface; live.motion.from = live.motion.to = surface.bounds;
        live.animation.Configure(false, 1, true);
        if (surface.visible) live.animation.ShowImmediately(); surfaces.push_back(std::move(live));
    }
    for (const auto& step : scene.timeline)
    {
        if (static_cast<std::uint64_t>(step.atMs) > milliseconds) break;
        if (step.target.empty() || step.action == "cursor.move") continue;
        const auto found = std::find_if(surfaces.begin(), surfaces.end(), [&](const Live& live) { return live.surface.id == step.target; });
        if (found == surfaces.end()) Invalid("Unknown timeline surface");
        auto& live = *found; live.surface.bounds = live.motion.At(step.atMs); Bounds(scene, live.surface.bounds);
        if (step.action == "surface.show") live.animation.Open(step.atMs);
        else if (step.action == "surface.hide") live.animation.Close(step.atMs);
        else if (step.action == "surface.move" || step.action == "surface.resize")
        {
            auto next = live.surface.bounds;
            if (step.action == "surface.move") OffsetRect(&next, step.point.x - next.left, step.point.y - next.top);
            else { next.right = next.left + step.size.cx; next.bottom = next.top + step.size.cy; }
            Bounds(scene, next); live.motion = {live.surface.bounds, next, static_cast<std::uint64_t>(step.atMs), step.durationMs};
        }
        else if (step.action == "surface.view") live.surface.listMode = step.value == "list";
        else if (step.action == "surface.search") live.surface.search = step.value;
        else if (step.action == "surface.category") live.surface.category = step.value;
    }
    Frame frame;
    for (auto& live : surfaces)
    {
        live.surface.bounds = live.motion.At(milliseconds); Bounds(scene, live.surface.bounds);
        live.animation.Advance(milliseconds); const auto visual = live.animation.GetVisual();
        frame.surfaces.push_back({std::move(live.surface), visual.visible, visual.scale, visual.opacity});
    }
    return frame;
}
Frame Evaluate(const Scene& scene, std::uint64_t milliseconds)
{
    auto frame = EvaluateLayout(scene, milliseconds);
    const auto current = CursorAt(scene, milliseconds);
    frame.cursor.x = static_cast<float>(current.point.x); frame.cursor.y = static_cast<float>(current.point.y);
    frame.cursor.scale = std::clamp(scene.width / 1280.f, .75f, 2.5f); frame.cursor.pressed = current.pressed;
    if (current.releasedMs >= 0) frame.cursor.clickAgeMilliseconds = static_cast<float>(milliseconds - current.releasedMs);
    for (auto time = milliseconds > 180 ? milliseconds - 180 : 0; time < milliseconds; time += 12)
        frame.cursor.trail.push_back(CursorAt(scene, time).point);
    return frame;
}
}

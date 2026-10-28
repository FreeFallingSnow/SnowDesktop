#include "builtin_scenes.h"
#include "scene.h"

namespace snowdesktop::demo
{
control::Value Describe()
{
    using namespace control;
    Value scene;
    // l10n-allow: fixed bilingual sample data for isolated render exports.
    ParseJson(R"({"schemaVersion":1,"name":"Desktop overview","locale":"zh-CN","appearance":"glass-light","width":1920,"height":1080,"fps":30,"durationMs":8000,"clock":"2026/10/29  09:41",
      "items":[
        {"id":"notes","title":"项目笔记","glyph":"\uf379","color":5938338,"category":"documents","dateGroup":"Today"},
        {"id":"travel","title":"旅行计划","glyph":"\uf56c","color":10057790,"category":"documents","dateGroup":"Today"},
        {"id":"photos","title":"周末照片","glyph":"\uf489","color":10120041,"category":"images","dateGroup":"Yesterday"},
        {"id":"budget","title":"家庭预算","glyph":"\ue54c","color":6258030,"category":"documents","dateGroup":"Yesterday"},
        {"id":"design","title":"设计草图","glyph":"\uf489","color":9868649,"category":"images","dateGroup":"This week"},
        {"id":"books","title":"阅读清单","glyph":"\ue179","color":5928104,"category":"documents","dateGroup":"This week"},
        {"id":"music","title":"音乐","glyph":"\ue855","color":8022939,"category":"others"},
        {"id":"browser","title":"浏览器","glyph":"\ue6b2","color":5209980,"category":"others"},
        {"id":"archive","title":"资料归档","glyph":"\uf42e","color":7179383,"category":"folders","directory":true}],
      "surfaces":[
        {"id":"status","kind":"status-bar","x":0,"y":0,"width":1920,"height":40},
        {"id":"work","kind":"files","title":"工作空间","x":80,"y":160,"width":540,"height":540,"items":["notes","travel","budget","books"],"showSearch":true,"showCategories":true},
        {"id":"inspiration","kind":"mapping","title":"灵感资料","x":700,"y":160,"width":460,"height":400,"items":["photos","design","travel","books"],"showSearch":true,"showCategories":false},
        {"id":"favorites","kind":"collection","title":"常用","x":1260,"y":160,"width":400,"height":300,"scrollContainer":false},
        {"id":"dock","kind":"dock","x":580,"y":950,"width":760,"height":96},
        {"id":"library","kind":"popup","title":"常用","x":660,"y":310,"width":620,"height":500,"visible":false}],
      "timeline":[
        {"atMs":0,"action":"cursor.move","durationMs":800,"target":"work","element":"view"},
        {"atMs":1000,"action":"cursor.down"},{"atMs":1100,"action":"cursor.up"},
        {"atMs":1100,"action":"surface.view","target":"work","value":"list"},
        {"atMs":1600,"action":"cursor.move","durationMs":650,"target":"inspiration","element":"search"},
        {"atMs":2400,"action":"cursor.down"},{"atMs":2500,"action":"cursor.up"},
        {"atMs":2500,"action":"surface.search","target":"inspiration","value":"旅行"},
        {"atMs":3300,"action":"surface.search","target":"inspiration","value":""},
        {"atMs":3400,"action":"cursor.move","durationMs":650,"target":"favorites","element":"open"},
        {"atMs":4200,"action":"cursor.down"},{"atMs":4300,"action":"cursor.up"},
        {"atMs":4300,"action":"surface.show","target":"library"},
        {"atMs":4700,"action":"cursor.move","durationMs":600,"x":960,"y":480},
        {"atMs":6200,"action":"surface.hide","target":"library"},
        {"atMs":6400,"action":"cursor.move","durationMs":650,"x":960,"y":990}]})", scene);
    (void)ParseScene(scene);
    auto kinds = Array(), actions = Array();
    for (const auto* kind : {"collection", "files", "mapping", "popup", "dock", "status-bar", "navigation"}) kinds.array.push_back(String(kind));
    for (const auto* action : {"cursor.move", "cursor.down", "cursor.up", "surface.show", "surface.hide", "surface.move", "surface.resize", "surface.view", "surface.search", "surface.category"}) actions.array.push_back(String(action));
    return Object({{"sceneSchemaVersion", Number(SceneSchemaVersion)}, {"example", std::move(scene)},
        {"surfaceKinds", std::move(kinds)}, {"actions", std::move(actions)},
        {"maximumDurationMs", Number(30000)}, {"maximumSurfaces", Number(12)}, {"maximumItems", Number(128)},
        {"maximumSteps", Number(256)}, {"maximumPixelFrames", Number(4000000000.0)},
        {"elementTargets", Object({{"surfaceKinds", String("collection, files, mapping")},
            {"keys", String("view, search, open, date-headers, category/<id>, header/<column>, divider/<column>, collapse, move, resize")},
            {"resolution", String("native hit-tested visible control center at cursor arrival time; unavailable controls fail the export")}})},
        {"output", String("SDR BT.709 H264 MP4 plus native PNG posters and a manifest")}});
}
}

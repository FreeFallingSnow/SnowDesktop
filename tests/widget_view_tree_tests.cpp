#include "widget_view_lua.h"
#include "widget_view_tree.h"
#include "widget_resource_lua.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <lauxlib.h>
#include <lualib.h>
}

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

void TestLayoutAndRegions()
{
    ViewNode root;
    root.type = ViewNodeType::Column;
    root.key = "root";
    root.padding = 10.0f;
    root.gap = 5.0f;
    root.alignItems = ViewAlignment::Center;
    root.justifyContent = ViewJustification::Center;

    ViewNode title;
    title.type = ViewNodeType::Text;
    title.key = "title";
    title.text = "12:34";
    title.width = { ViewLengthKind::Fill, 0.0f };
    title.height = { ViewLengthKind::Fixed, 30.0f };
    title.textAlign = ViewTextAlignment::Center;
    title.fontSize = 24.0f;

    ViewNode button;
    button.type = ViewNodeType::Button;
    button.key = "refresh";
    button.text = "Refresh";
    button.width = { ViewLengthKind::Fixed, 100.0f };
    button.height = { ViewLengthKind::Fixed, 32.0f };
    button.style.cornerRadius = 8.0f;
    button.events["click"].id = "refresh";
    button.events["contextMenu"].id = "refresh.menu";

    root.children.push_back(title);
    root.children.push_back(button);
    std::string error;
    Check(ValidateAndLayoutViewTree(root, 240.0f, 120.0f, error),
        "a bounded column tree must validate and lay out");
    Check(Near(root.children[0].frame.x, 10.0f) &&
            Near(root.children[0].frame.width, 220.0f) &&
            Near(root.children[0].frame.y, 26.5f),
        "column layout must center the fixed block and stretch its title");
    Check(Near(root.children[1].frame.x, 70.0f) &&
            Near(root.children[1].frame.y, 61.5f),
        "column layout must center a fixed-width button after the gap");

    std::vector<InteractionRegion> regions;
    Check(CollectViewInteractionRegions(root, regions, error) &&
            regions.size() == 1 && regions[0].key == "refresh" &&
            regions[0].shape.type ==
                InteractionShapeType::RoundedRect &&
            regions[0].events.contains("click") &&
            regions[0].events.contains("contextMenu") &&
            regions[0].accessibilityRole == "button" &&
            regions[0].cursor == "hand",
        "button layout must produce one semantic element interaction region");
}

void TestValidationFailures()
{
    ViewNode root;
    root.type = ViewNodeType::Column;
    root.key = "same";
    ViewNode duplicate;
    duplicate.type = ViewNodeType::Text;
    duplicate.key = "same";
    duplicate.text = "duplicate";
    root.children.push_back(duplicate);
    std::string error;
    Check(!ValidateAndLayoutViewTree(root, 100.0f, 100.0f, error) &&
            error.find("duplicate") != std::string::npos,
        "duplicate stable keys must reject the whole view commit");

    ViewNode leaf;
    leaf.type = ViewNodeType::Text;
    leaf.key = "leaf";
    leaf.text = "text";
    ViewNode illegalChild;
    illegalChild.type = ViewNodeType::Spacer;
    illegalChild.key = "child";
    leaf.children.push_back(illegalChild);
    Check(!ValidateAndLayoutViewTree(leaf, 100.0f, 100.0f, error) &&
            error.find("cannot have children") != std::string::npos,
        "leaf nodes must reject children rather than silently ignoring them");

    ViewNode invalidProgress;
    invalidProgress.type = ViewNodeType::ProgressRing;
    invalidProgress.key = "progress";
    invalidProgress.value = 1.1f;
    Check(!ValidateAndLayoutViewTree(
            invalidProgress, 100.0f, 100.0f, error),
        "progress nodes must reject values outside the normalized range");

    ViewNode unlabeledIconButton;
    unlabeledIconButton.type = ViewNodeType::IconButton;
    unlabeledIconButton.key = "icon-action";
    unlabeledIconButton.text = ">";
    Check(!ValidateAndLayoutViewTree(
            unlabeledIconButton, 100.0f, 100.0f, error) &&
            error.find("accessibility.label") != std::string::npos,
        "iconButton must require an accessible label");

    ViewNode unlabeledSeries;
    unlabeledSeries.type = ViewNodeType::Waveform;
    unlabeledSeries.key = "waveform";
    unlabeledSeries.values = { -0.5f, 0.5f };
    Check(!ValidateAndLayoutViewTree(
            unlabeledSeries, 100.0f, 100.0f, error) &&
            error.find("accessibility.label") != std::string::npos,
        "data-series nodes must require an accessible label");

    ViewNode unlabeledMeter;
    unlabeledMeter.type = ViewNodeType::Meter;
    unlabeledMeter.key = "meter";
    unlabeledMeter.value = 0.5f;
    Check(!ValidateAndLayoutViewTree(
            unlabeledMeter, 100.0f, 100.0f, error) &&
            error.find("accessibility.label") != std::string::npos,
        "meter nodes must require an accessible label");
}

void RegisterViewLibrary(lua_State* state)
{
    lua_newtable(state);
    const struct Entry
    {
        const char* name;
        lua_CFunction callback;
    } entries[] = {
        { "box", LuaViewBox },
        { "row", LuaViewRow },
        { "column", LuaViewColumn },
        { "stack", LuaViewStack },
        { "text", LuaViewText },
        { "image", LuaViewImage },
        { "button", LuaViewButton },
        { "toggle", LuaViewToggle },
        { "checkbox", LuaViewCheckbox },
        { "icon", LuaViewIcon },
        { "iconButton", LuaViewIconButton },
        { "shape", LuaViewShape },
        { "badge", LuaViewBadge },
        { "divider", LuaViewDivider },
        { "progressBar", LuaViewProgressBar },
        { "progressRing", LuaViewProgressRing },
        { "meter", LuaViewMeter },
        { "sparkline", LuaViewSparkline },
        { "lineChart", LuaViewLineChart },
        { "barChart", LuaViewBarChart },
        { "waveform", LuaViewWaveform },
        { "spectrum", LuaViewSpectrum },
        { "spacer", LuaViewSpacer },
    };
    for (const auto& entry : entries)
    {
        lua_pushcfunction(state, entry.callback);
        lua_setfield(state, -2, entry.name);
    }
    lua_setglobal(state, "view");
}

void TestLuaParsing()
{
    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be available");
    luaL_openlibs(state);
    RegisterViewLibrary(state);
    const char* script = R"lua(
        local source = { key = "title", text = "Ready" }
        local title = view.text(source)
        assert(source.type == nil)
        return view.column({
            key = "root",
            padding = 8,
            gap = 4,
            alignItems = "center",
            justifyContent = "center",
            children = {
                title,
                view.button({
                    key = "open",
                    label = "Open",
                    width = 96,
                    height = 32,
                    style = {
                        background = 0x123456,
                        foreground = 0xFFFFFF,
                        cornerRadius = 6,
                    },
                    hoverStyle = { background = 0x345678 },
                    action = { id = "open", value = { source = "test" } },
                    events = {
                        contextMenu = {
                            id = "open.menu",
                            value = { item = 7 },
                        },
                    },
                    accessibility = {
                        label = "Open the selected item",
                    },
                }),
            },
        })
    )lua";
    Check(luaL_loadstring(state, script) == LUA_OK &&
            lua_pcall(state, 0, 1, 0) == LUA_OK,
        "view constructors must create a Lua scene without mutating inputs");
    ViewNode root;
    std::string error;
    Check(ParseLuaViewTree(state, -1, root, error) &&
            root.type == ViewNodeType::Column &&
            root.children.size() == 2 &&
            root.children[0].type == ViewNodeType::Text &&
            root.children[1].type == ViewNodeType::Button &&
            root.children[1].events.at("click").id == "open" &&
            root.children[1].events.at("contextMenu").id == "open.menu" &&
            root.children[1].style.background ==
                std::uint32_t{ 0x123456 } &&
            root.children[1].hoverStyle.background ==
                std::uint32_t{ 0x345678 } &&
            root.children[1].accessibilityLabel ==
                "Open the selected item",
        "Lua parsing must retain typed nodes, styles, actions, and semantics");
    Check(ValidateAndLayoutViewTree(root, 300.0f, 160.0f, error),
        "a parsed Lua view must pass the host layout contract");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.text({
            key = "invalid",
            text = "Typo",
            fontSze = 18,
        })
    )lua") == LUA_OK,
        "unknown-field view fixture must evaluate");
    root = {};
    error.clear();
    Check(!ParseLuaViewTree(state, -1, root, error) &&
            error.find("fontSze") != std::string::npos,
        "view parsing must reject unknown fields instead of hiding typos");
    lua_close(state);
}

void TestVisualNodeParsing()
{
    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be available");
    luaL_openlibs(state);
    RegisterViewLibrary(state);
    PushResourceHandle(state, LuaResourceType::Image, "logo");
    lua_setglobal(state, "logoResource");
    PushResourceHandle(state, LuaResourceType::Font, "display");
    lua_setglobal(state, "displayFont");
    Check(luaL_dostring(state, R"lua(
        return view.row({
            key = "visuals",
            gap = 4,
            children = {
                view.shape({
                    key = "dot",
                    shape = "circle",
                    width = 8,
                    height = 8,
                    style = { background = 0xFF0000, opacity = 0.5 },
                }),
                view.progressBar({
                    key = "bar",
                    width = 80,
                    value = 0.25,
                    thickness = 6,
                    trackOpacity = 0.2,
                    fillOpacity = 0.9,
                }),
                view.progressRing({
                    key = "ring",
                    width = 40,
                    height = 40,
                    value = 0.75,
                    thickness = 5,
                }),
                view.icon({
                    key = "glyph",
                    glyph = ">",
                    iconFont = "fluent",
                    fontSize = 18,
                }),
                view.iconButton({
                    key = "next",
                    glyph = ">",
                    iconFont = "fa",
                    width = 32,
                    height = 32,
                    action = { id = "next" },
                    accessibility = { label = "Next" },
                }),
                view.image({
                    key = "logo",
                    source = logoResource,
                    alt = "SnowDesktop",
                    width = 48,
                    height = 32,
                    fit = "cover",
                    alignment = "end",
                    interpolation = "nearest",
                }),
                view.text({
                    key = "brand",
                    text = "SnowDesktop",
                    font = displayFont,
                    fontSize = 16,
                }),
            },
        })
    )lua") == LUA_OK,
        "visual-node Lua fixture must evaluate");
    ViewNode root;
    std::string error;
    Check(ParseLuaViewTree(state, -1, root, error) &&
            root.children.size() == 7 &&
            root.children[0].type == ViewNodeType::Shape &&
            root.children[0].shapeKind == ViewShapeKind::Circle &&
            root.children[1].type == ViewNodeType::ProgressBar &&
            Near(root.children[1].value, 0.25f) &&
            Near(root.children[1].trackOpacity, 0.2f) &&
            root.children[2].type == ViewNodeType::ProgressRing &&
            root.children[3].iconFont == ViewIconFont::Fluent &&
            root.children[4].type == ViewNodeType::IconButton &&
            root.children[5].type == ViewNodeType::Image &&
            root.children[5].imageResourceName == "logo" &&
            root.children[5].alt == "SnowDesktop" &&
            root.children[5].imageFit == ViewImageFit::Cover &&
            root.children[5].imageAlignment == ViewImageAlignment::End &&
            root.children[5].imageInterpolation ==
                ViewImageInterpolation::Nearest &&
            root.children[6].fontResourceName == "display",
        "visual and package resource nodes must retain typed fields");
    Check(ValidateAndLayoutViewTree(root, 320.0f, 80.0f, error),
        "visual nodes must validate and lay out together");
    std::vector<InteractionRegion> regions;
    Check(CollectViewInteractionRegions(root, regions, error) &&
            regions.size() == 1 && regions[0].key == "next" &&
            regions[0].events.at("click").id == "next" &&
            regions[0].accessibilityRole == "button",
        "iconButton must produce an actionable semantic region");

    lua_pop(state, lua_gettop(state));
    PushResourceHandle(state, LuaResourceType::Font, "display");
    lua_setglobal(state, "wrongImageResource");
    Check(luaL_dostring(state, R"lua(
        return view.image({
            key = "invalid-image",
            source = wrongImageResource,
            alt = "Invalid",
        })
    )lua") == LUA_OK,
        "wrong-resource-type fixture must evaluate");
    ViewNode invalid;
    error.clear();
    Check(!ParseLuaViewTree(state, -1, invalid, error) &&
            error.find("wrong package resource type") != std::string::npos,
        "image nodes must reject font handles as their source");
    lua_close(state);
}

void TestDataSeriesParsingAndLimits()
{
    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be available");
    luaL_openlibs(state);
    RegisterViewLibrary(state);
    Check(luaL_dostring(state, R"lua(
        return view.column({
            key = "charts",
            children = {
                view.sparkline({
                    key = "cpu",
                    values = { 0.1, 0.4, 0.2, 0.8 },
                    thickness = 2,
                    accessibility = { label = "CPU history" },
                }),
                view.lineChart({
                    key = "network",
                    values = { 12, 30, 18 },
                    min = 0,
                    max = 40,
                    accessibility = { label = "Network traffic" },
                }),
                view.barChart({
                    key = "storage",
                    values = { -2, 4, 6 },
                    accessibility = { label = "Storage activity" },
                }),
                view.waveform({
                    key = "wave",
                    values = { -1, -0.25, 0.5, 1 },
                    accessibility = { label = "Audio waveform" },
                }),
                view.spectrum({
                    key = "spectrum",
                    values = { 0.1, 0.5, 1.0 },
                    accessibility = { label = "Audio spectrum" },
                }),
            },
        })
    )lua") == LUA_OK,
        "data-series Lua fixture must evaluate");
    ViewNode root;
    std::string error;
    Check(ParseLuaViewTree(state, -1, root, error) &&
            root.children.size() == 5 &&
            root.children[0].type == ViewNodeType::Sparkline &&
            root.children[0].values.size() == 4 &&
            Near(root.children[0].values[3], 0.8f) &&
            root.children[1].type == ViewNodeType::LineChart &&
            root.children[1].seriesMinimum &&
            root.children[1].seriesMaximum &&
            Near(*root.children[1].seriesMinimum, 0.0f) &&
            Near(*root.children[1].seriesMaximum, 40.0f) &&
            root.children[2].type == ViewNodeType::BarChart &&
            root.children[3].type == ViewNodeType::Waveform &&
            root.children[4].type == ViewNodeType::Spectrum,
        "data-series constructors must retain bounded typed samples");
    Check(ValidateAndLayoutViewTree(root, 320.0f, 240.0f, error),
        "five bounded data-series nodes must validate and lay out");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.sparkline({
            key = "bad-range",
            values = { 1, 2 },
            min = 0,
            accessibility = { label = "Bad range" },
        })
    )lua") == LUA_OK,
        "incomplete-range fixture must evaluate");
    ViewNode invalid;
    Check(!ParseLuaViewTree(state, -1, invalid, error) &&
            error.find("both min and max") != std::string::npos,
        "data-series nodes must reject one-sided explicit ranges");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        local values = {}
        for i = 1, 513 do values[i] = i end
        return view.waveform({
            key = "too-many",
            values = values,
            accessibility = { label = "Too many samples" },
        })
    )lua") == LUA_OK,
        "oversized-series fixture must evaluate");
    Check(!ParseLuaViewTree(state, -1, invalid, error) &&
            error.find("1 to 512") != std::string::npos,
        "one node must reject more than 512 samples");
    lua_close(state);

    ViewNode many;
    many.type = ViewNodeType::Column;
    many.key = "many";
    for (int index = 0; index < 9; ++index)
    {
        ViewNode series;
        series.type = ViewNodeType::Sparkline;
        series.key = "series-" + std::to_string(index);
        series.values.assign(512, static_cast<float>(index));
        series.accessibilityLabel = "Series";
        many.children.push_back(std::move(series));
    }
    Check(!ValidateAndLayoutViewTree(many, 320.0f, 240.0f, error) &&
            error.find("point limit") != std::string::npos,
        "one tree must reject more than 4096 total series samples");
}

void TestStatusVisualParsing()
{
    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be available");
    luaL_openlibs(state);
    RegisterViewLibrary(state);
    Check(luaL_dostring(state, R"lua(
        return view.row({
            key = "status",
            height = 48,
            gap = 8,
            children = {
                view.badge({
                    key = "online",
                    text = "Online",
                    width = 72,
                    accessibility = { label = "Connection online" },
                    style = { background = 0x167A45 },
                }),
                view.divider({
                    key = "separator",
                    orientation = "vertical",
                    thickness = 2,
                    style = { foreground = 0x808080 },
                }),
                view.meter({
                    key = "battery",
                    width = 120,
                    value = 0.65,
                    thickness = 8,
                    trackOpacity = 0.2,
                    accessibility = { label = "Battery 65 percent" },
                }),
            },
        })
    )lua") == LUA_OK,
        "status-visual Lua fixture must evaluate");
    ViewNode root;
    std::string error;
    Check(ParseLuaViewTree(state, -1, root, error) &&
            root.children.size() == 3 &&
            root.children[0].type == ViewNodeType::Badge &&
            root.children[0].text == "Online" &&
            Near(root.children[0].padding, 4.0f) &&
            root.children[1].type == ViewNodeType::Divider &&
            root.children[1].orientation == ViewOrientation::Vertical &&
            root.children[1].width.kind == ViewLengthKind::Auto &&
            root.children[1].height.kind == ViewLengthKind::Fill &&
            root.children[2].type == ViewNodeType::Meter &&
            Near(root.children[2].value, 0.65f),
        "badge, divider, and meter constructors must retain typed fields");
    Check(ValidateAndLayoutViewTree(root, 320.0f, 48.0f, error) &&
            Near(root.children[1].frame.width, 2.0f) &&
            Near(root.children[1].frame.height, 48.0f),
        "a vertical divider must use intrinsic thickness and fill height");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.divider({
            key = "bad-divider",
            orientation = "diagonal",
        })
    )lua") == LUA_OK,
        "invalid-divider fixture must evaluate");
    ViewNode invalid;
    Check(!ParseLuaViewTree(state, -1, invalid, error) &&
            error.find("horizontal or vertical") != std::string::npos,
        "divider orientation must reject unsupported values");
    lua_close(state);
}

void TestSelectionControlParsing()
{
    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be available");
    luaL_openlibs(state);
    RegisterViewLibrary(state);
    Check(luaL_dostring(state, R"lua(
        return view.column({
            key = "controls",
            children = {
                view.toggle({
                    key = "notifications",
                    label = "Notifications",
                    checked = false,
                    action = { id = "notifications.change" },
                    checkedStyle = { background = 0x4C9AFF },
                    hoverStyle = { opacity = 0.9 },
                }),
                view.checkbox({
                    key = "compact",
                    label = "Compact layout",
                    checked = true,
                    events = {
                        change = { id = "compact.change" },
                        contextMenu = { id = "compact.menu" },
                    },
                }),
            },
        })
    )lua") == LUA_OK,
        "selection-control Lua fixture must evaluate");
    ViewNode root;
    std::string error;
    Check(ParseLuaViewTree(state, -1, root, error) &&
            root.children.size() == 2 &&
            root.children[0].type == ViewNodeType::Toggle &&
            !root.children[0].checked &&
            root.children[0].events.at("change").id ==
                "notifications.change" &&
            root.children[0].checkedStyle.background ==
                std::uint32_t{ 0x4C9AFF } &&
            root.children[1].type == ViewNodeType::Checkbox &&
            root.children[1].checked &&
            root.children[1].events.at("contextMenu").id ==
                "compact.menu",
        "selection controls must retain controlled values and actions");
    Check(ValidateAndLayoutViewTree(root, 280.0f, 96.0f, error),
        "selection controls must validate and lay out");
    std::vector<InteractionRegion> regions;
    Check(CollectViewInteractionRegions(root, regions, error) &&
            regions.size() == 2 &&
            regions[0].controlKind == InteractionControlKind::Toggle &&
            !regions[0].checked &&
            regions[0].accessibilityRole == "switch" &&
            regions[0].cursor == "hand" &&
            regions[1].controlKind == InteractionControlKind::Checkbox &&
            regions[1].checked &&
            regions[1].accessibilityRole == "checkbox",
        "selection controls must produce semantic controlled regions");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.toggle({
            key = "missing-value",
            label = "Missing",
            action = { id = "missing.change" },
        })
    )lua") == LUA_OK,
        "missing-controlled-value fixture must evaluate");
    ViewNode invalid;
    Check(!ParseLuaViewTree(state, -1, invalid, error) &&
            error.find("require checked") != std::string::npos,
        "selection controls must require an explicit controlled value");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.checkbox({
            key = "wrong-event",
            label = "Wrong event",
            checked = false,
            events = { click = { id = "wrong.click" } },
        })
    )lua") == LUA_OK,
        "wrong-control-event fixture must evaluate");
    invalid = {};
    Check(ParseLuaViewTree(state, -1, invalid, error) &&
            !ValidateAndLayoutViewTree(invalid, 200.0f, 40.0f, error) &&
            error.find("require change") != std::string::npos,
        "selection controls must reject click in favor of change");
    lua_close(state);
}
}

int main()
{
    TestLayoutAndRegions();
    TestValidationFailures();
    TestLuaParsing();
    TestVisualNodeParsing();
    TestDataSeriesParsingAndLimits();
    TestStatusVisualParsing();
    TestSelectionControlParsing();
    std::cout << "Widget view tree tests passed\n";
    return 0;
}

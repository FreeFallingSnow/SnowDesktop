#include "widget_view_lua.h"
#include "widget_view_tree.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
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
        { "button", LuaViewButton },
        { "icon", LuaViewIcon },
        { "iconButton", LuaViewIconButton },
        { "shape", LuaViewShape },
        { "progressBar", LuaViewProgressBar },
        { "progressRing", LuaViewProgressRing },
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
            },
        })
    )lua") == LUA_OK,
        "visual-node Lua fixture must evaluate");
    ViewNode root;
    std::string error;
    Check(ParseLuaViewTree(state, -1, root, error) &&
            root.children.size() == 5 &&
            root.children[0].type == ViewNodeType::Shape &&
            root.children[0].shapeKind == ViewShapeKind::Circle &&
            root.children[1].type == ViewNodeType::ProgressBar &&
            Near(root.children[1].value, 0.25f) &&
            Near(root.children[1].trackOpacity, 0.2f) &&
            root.children[2].type == ViewNodeType::ProgressRing &&
            root.children[3].iconFont == ViewIconFont::Fluent &&
            root.children[4].type == ViewNodeType::IconButton,
        "shape, progress, and icon nodes must retain typed fields");
    Check(ValidateAndLayoutViewTree(root, 320.0f, 80.0f, error),
        "visual nodes must validate and lay out together");
    std::vector<InteractionRegion> regions;
    Check(CollectViewInteractionRegions(root, regions, error) &&
            regions.size() == 1 && regions[0].key == "next" &&
            regions[0].events.at("click").id == "next" &&
            regions[0].accessibilityRole == "button",
        "iconButton must produce an actionable semantic region");
    lua_close(state);
}
}

int main()
{
    TestLayoutAndRegions();
    TestValidationFailures();
    TestLuaParsing();
    TestVisualNodeParsing();
    std::cout << "Widget view tree tests passed\n";
    return 0;
}

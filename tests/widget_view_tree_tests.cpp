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
        { "grid", LuaViewGrid },
        { "flow", LuaViewFlow },
        { "stack", LuaViewStack },
        { "scroll", LuaViewScroll },
        { "list", LuaViewList },
        { "gridList", LuaViewGridList },
        { "virtualList", LuaViewVirtualList },
        { "virtualGrid", LuaViewVirtualGrid },
        { "listItem", LuaViewListItem },
        { "text", LuaViewText },
        { "styledText", LuaViewStyledText },
        { "textInput", LuaViewTextInput },
        { "textArea", LuaViewTextArea },
        { "searchBox", LuaViewSearchBox },
        { "numberInput", LuaViewNumberInput },
        { "select", LuaViewSelect },
        { "image", LuaViewImage },
        { "referenceIcon", LuaViewReferenceIcon },
        { "button", LuaViewButton },
        { "link", LuaViewLink },
        { "toggle", LuaViewToggle },
        { "checkbox", LuaViewCheckbox },
        { "radioGroup", LuaViewRadioGroup },
        { "slider", LuaViewSlider },
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
        { "monthCalendar", LuaViewMonthCalendar },
        { "slotSurface", LuaViewSlotSurface },
        { "slotItem", LuaViewSlotItem },
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
        local source = {
            key = "title", text = "Ready", textWrap = "wrap",
            maxLines = 2, overflowText = "clip", verticalAlign = "end",
            fontWeight = 600, fontStyle = "italic",
            lineHeight = 24, letterSpacing = 1.5,
            locale = "ar-SA", textDirection = "rtl",
            tooltip = "Current status",
        }
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
                    focusStyle = { borderColor = 0x72C7FF, borderWidth = 2 },
                    disabledStyle = { opacity = 0.35 },
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
            root.children[0].textWrap == ViewTextWrap::Wrap &&
            root.children[0].maximumLines == 2 &&
            root.children[0].overflowText == ViewTextOverflow::Clip &&
            root.children[0].verticalAlign == ViewAlignment::End &&
            root.children[0].fontWeight == 600 &&
            root.children[0].fontStyle == ViewFontStyle::Italic &&
            root.children[0].locale == "ar-SA" &&
            root.children[0].textDirection ==
                ViewTextDirection::RightToLeft &&
            root.children[0].lineHeight == 24.0f &&
            Near(root.children[0].letterSpacing, 1.5f) &&
            root.children[0].tooltip == "Current status" &&
            root.children[1].type == ViewNodeType::Button &&
            root.children[1].events.at("click").id == "open" &&
            root.children[1].events.at("contextMenu").id == "open.menu" &&
            root.children[1].style.background ==
                std::uint32_t{ 0x123456 } &&
            root.children[1].hoverStyle.background ==
                std::uint32_t{ 0x345678 } &&
            root.children[1].focusStyle.borderColor ==
                std::uint32_t{ 0x72C7FF } &&
            root.children[1].focusStyle.borderWidth == 2.0f &&
            root.children[1].disabledStyle.opacity == 0.35f &&
            root.children[1].accessibilityLabel ==
                "Open the selected item",
        "Lua parsing must retain typed nodes, styles, actions, and semantics");
    Check(ValidateAndLayoutViewTree(root, 300.0f, 160.0f, error),
        "a parsed Lua view must pass the host layout contract");
    std::vector<InteractionRegion> regions;
    Check(CollectViewInteractionRegions(root, regions, error) &&
            regions.size() == 2 && regions[0].key == "title" &&
            regions[0].tooltip == "Current status",
        "a tooltip-only node must receive a bounded host hover region");

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
    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.text({
            key = "too-many-lines", text = "Text", maxLines = 65,
        })
    )lua") == LUA_OK,
        "max-lines boundary fixture must evaluate");
    root = {};
    Check(ParseLuaViewTree(state, -1, root, error) &&
            !ValidateAndLayoutViewTree(root, 100.0f, 40.0f, error) &&
            error == "view maxLines must be between 0 and 64",
        "maxLines must reject values above the bounded public range");
    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.text({
            key = "bad-weight", text = "Text", fontWeight = 650,
        })
    )lua") == LUA_OK,
        "font-weight boundary fixture must evaluate");
    root = {};
    Check(ParseLuaViewTree(state, -1, root, error) &&
            !ValidateAndLayoutViewTree(root, 100.0f, 40.0f, error) &&
            error.find("fontWeight") != std::string::npos,
        "fontWeight must use bounded 100-step values");
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
                view.referenceIcon({
                    key = "application-icon",
                    reference = "app-ref-1",
                    alt = "Bound application",
                    width = 48,
                    height = 48,
                    fit = "contain",
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
            root.children.size() == 8 &&
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
            root.children[6].type == ViewNodeType::ReferenceIcon &&
            root.children[6].itemReference == "app-ref-1" &&
            root.children[6].alt == "Bound application" &&
            root.children[7].fontResourceName == "display",
        "visual, reference icon, and package resource nodes must retain typed fields");
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

    lua_settop(state, 0);
    Check(luaL_dostring(state, R"lua(
        return view.referenceIcon({
            key = "invalid-reference-icon",
            reference = string.rep("x", 129),
            alt = "Invalid",
        })
    )lua") == LUA_OK,
        "overlong reference icon fixture must evaluate");
    invalid = {};
    error.clear();
    Check(ParseLuaViewTree(state, -1, invalid, error) &&
            !ValidateAndLayoutViewTree(invalid, 64.0f, 64.0f, error) &&
            error.find("bounded opaque reference") != std::string::npos,
        "referenceIcon must reject references outside the opaque token bound");
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
            Near(root.children[0].padding.left, 4.0f) &&
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
            selected = true,
            selectedStyle = { background = 0x101820 },
            children = {
                view.toggle({
                    key = "notifications",
                    label = "Notifications",
                    checked = false,
                    tabIndex = 2,
                    action = { id = "notifications.change" },
                    events = {
                        keyDown = { id = "notifications.key-down" },
                        keyUp = { id = "notifications.key-up" },
                    },
                    checkedStyle = { background = 0x4C9AFF },
                    hoverStyle = { opacity = 0.9 },
                }),
                view.checkbox({
                    key = "compact",
                    label = "Compact layout",
                    checked = false,
                    indeterminate = true,
                    focusable = true,
                    tabIndex = 1,
                    events = {
                        change = { id = "compact.change" },
                        contextMenu = {
                            id = "compact.menu", scope = "component" },
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
            root.selected &&
            root.selectedStyle.background == std::uint32_t{ 0x101820 } &&
            root.children[0].type == ViewNodeType::Toggle &&
            !root.children[0].checked &&
            root.children[0].tabIndex == 2 &&
            root.children[0].events.at("change").id ==
                "notifications.change" &&
            root.children[0].events.at("keyDown").id ==
                "notifications.key-down" &&
            root.children[0].events.at("keyUp").id ==
                "notifications.key-up" &&
            root.children[0].checkedStyle.background ==
                std::uint32_t{ 0x4C9AFF } &&
            root.children[1].type == ViewNodeType::Checkbox &&
            !root.children[1].checked &&
            root.children[1].indeterminate &&
            root.children[1].focusable == true &&
            root.children[1].tabIndex == 1 &&
            root.children[1].events.at("contextMenu").id ==
                "compact.menu" &&
            root.children[1].events.at("contextMenu").contextMenuScope ==
                InteractionAction::ContextMenuScope::Component,
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
            regions[0].events.at("keyDown").id ==
                "notifications.key-down" &&
            regions[0].events.at("keyUp").id ==
                "notifications.key-up" &&
            regions[0].focusable == true && regions[0].tabIndex == 2 &&
            regions[1].controlKind == InteractionControlKind::Checkbox &&
            !regions[1].checked && regions[1].indeterminate &&
            regions[1].focusable == true && regions[1].tabIndex == 1 &&
            regions[1].accessibilityRole == "checkbox",
        "selection controls must produce semantic controlled regions");
    WidgetInteractionRegions focusRegions;
    focusRegions.BeginFrame();
    for (auto& region : regions)
        Check(focusRegions.Submit(std::move(region), error),
            "focus-order regions must submit");
    focusRegions.CommitFrame();
    const auto focusKeys = focusRegions.KeyboardFocusableKeys();
    Check(focusKeys.size() == 2 && focusKeys[0] == "compact" &&
            focusKeys[1] == "notifications",
        "declarative tabIndex must control stable keyboard traversal order");

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
            key = "invalid-mixed",
            label = "Invalid mixed",
            checked = true,
            indeterminate = true,
            action = { id = "invalid-mixed.change" },
        })
    )lua") == LUA_OK,
        "invalid mixed-checkbox fixture must evaluate");
    invalid = {};
    Check(ParseLuaViewTree(state, -1, invalid, error) &&
            !ValidateAndLayoutViewTree(invalid, 200.0f, 40.0f, error) &&
            error.find("checked=false") != std::string::npos,
        "an indeterminate checkbox must reject a simultaneously checked value");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.button({
            key = "invalid-focus-order",
            label = "Invalid focus order",
            focusable = false,
            tabIndex = 1,
            action = { id = "invalid-focus-order.open" },
        })
    )lua") == LUA_OK,
        "invalid focus-order fixture must evaluate");
    invalid = {};
    Check(ParseLuaViewTree(state, -1, invalid, error) &&
            !ValidateAndLayoutViewTree(invalid, 200.0f, 40.0f, error) &&
            error.find("requires a focusable node") != std::string::npos,
        "tabIndex must reject an explicitly non-focusable node");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.text({
            key = "invalid-key-target",
            text = "Cannot focus",
            events = { keyDown = { id = "invalid.key" } },
        })
    )lua") == LUA_OK,
        "invalid raw-key target fixture must evaluate");
    invalid = {};
    Check(ParseLuaViewTree(state, -1, invalid, error) &&
            !ValidateAndLayoutViewTree(invalid, 200.0f, 40.0f, error) &&
            error.find("events require a focusable node") !=
                std::string::npos,
        "raw key events must reject a node outside the focus order");

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

void TestActionControlParsing()
{
    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be available");
    luaL_openlibs(state);
    RegisterViewLibrary(state);
    Check(luaL_dostring(state, R"lua(
        return view.column({
            key = "actions",
            children = {
                view.link({
                    key = "details",
                    label = "Open details",
                    height = 24,
                    action = { id = "details.open" },
                    events = {
                        contextMenu = { id = "details.menu" },
                    },
                }),
                view.radioGroup({
                    key = "density",
                    height = 80,
                    orientation = "vertical",
                    selectedValue = "comfortable",
                    options = {
                        { key = "comfortable", value = "comfortable",
                            label = "Comfortable" },
                        { key = "compact", value = "compact",
                            label = "Compact", enabled = false },
                    },
                    checkedStyle = { foreground = 0x4C9AFF },
                    action = { id = "density.change" },
                    events = {
                        contextMenu = { id = "density.menu" },
                    },
                }),
                view.slider({
                    key = "volume",
                    height = 32,
                    value = 35,
                    min = 0,
                    max = 100,
                    step = 5,
                    action = { id = "volume.change" },
                    accessibility = { label = "Volume" },
                }),
            },
        })
    )lua") == LUA_OK,
        "action-control Lua fixture must evaluate");
    ViewNode root;
    std::string error;
    Check(ParseLuaViewTree(state, -1, root, error) &&
            root.children.size() == 3 &&
            root.children[0].type == ViewNodeType::Link &&
            root.children[0].events.at("click").id == "details.open" &&
            root.children[1].type == ViewNodeType::RadioGroup &&
            root.children[1].selectedValue == "comfortable" &&
            root.children[1].options.size() == 2 &&
            !root.children[1].options[1].enabled &&
            root.children[2].type == ViewNodeType::Slider &&
            Near(root.children[2].value, 35.0f) &&
            Near(root.children[2].minimum, 0.0f) &&
            Near(root.children[2].maximum, 100.0f) &&
            Near(root.children[2].step, 5.0f),
        "link, radioGroup, and slider must retain typed controlled fields");
    Check(ValidateAndLayoutViewTree(root, 300.0f, 136.0f, error),
        "the action-control fixture must validate and lay out");
    std::vector<InteractionRegion> regions;
    Check(CollectViewInteractionRegions(root, regions, error) &&
            regions.size() == 4 &&
            regions[0].key == "details" &&
            regions[0].accessibilityRole == "link" &&
            regions[1].key == "density/comfortable" &&
            regions[1].controlKind == InteractionControlKind::Radio &&
            regions[1].checked &&
            regions[1].events.contains("contextMenu") &&
            regions[2].key == "density/compact" &&
            !regions[2].enabled &&
            regions[3].controlKind == InteractionControlKind::Slider &&
            Near(regions[3].controlValue, 35.0f) &&
            regions[3].accessibilityRole == "slider",
        "action controls must produce link, per-option radio, and slider regions");
    const ViewRect secondOption = ViewRadioOptionFrame(
        root.children[1], 1);
    Check(secondOption.y > root.children[1].frame.y &&
            secondOption.height > 0.0f,
        "vertical radio options must receive separate stable geometry");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.radioGroup({
            key = "duplicate",
            selectedValue = "one",
            options = {
                { key = "one", value = "one", label = "One" },
                { key = "two", value = "one", label = "Also one" },
            },
            action = { id = "duplicate.change" },
        })
    )lua") == LUA_OK,
        "duplicate radio-value fixture must evaluate");
    ViewNode invalid;
    Check(ParseLuaViewTree(state, -1, invalid, error) &&
            !ValidateAndLayoutViewTree(invalid, 240.0f, 80.0f, error) &&
            error.find("unique") != std::string::npos,
        "radioGroup option values must be unique");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.slider({
            key = "bad-slider",
            value = 5,
            min = 10,
            max = 0,
            step = 1,
            action = { id = "bad.change" },
            accessibility = { label = "Bad slider" },
        })
    )lua") == LUA_OK,
        "invalid slider-range fixture must evaluate");
    invalid = {};
    Check(ParseLuaViewTree(state, -1, invalid, error) &&
            !ValidateAndLayoutViewTree(invalid, 240.0f, 32.0f, error) &&
            error.find("min < max") != std::string::npos,
        "slider ranges and values must be validated together");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.link({ key = "inactive", label = "Inactive" })
    )lua") == LUA_OK,
        "inactive-link fixture must evaluate");
    invalid = {};
    Check(ParseLuaViewTree(state, -1, invalid, error) &&
            !ValidateAndLayoutViewTree(invalid, 120.0f, 24.0f, error) &&
            error.find("click action") != std::string::npos,
        "link nodes must not silently accept missing actions");
    lua_close(state);
}

void TestUniformGridParsingAndLayout()
{
    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be available");
    luaL_openlibs(state);
    RegisterViewLibrary(state);
    Check(luaL_dostring(state, R"lua(
        local children = {}
        for index = 1, 6 do
            children[index] = view.shape({
                key = "cell-" .. index,
                shape = "roundedRectangle",
                height = 30,
            })
        end
        return view.grid({
            key = "grid",
            columns = 3,
            padding = 10,
            columnGap = 5,
            rowGap = 7,
            children = children,
        })
    )lua") == LUA_OK,
        "uniform-grid Lua fixture must evaluate");
    ViewNode root;
    std::string error;
    Check(ParseLuaViewTree(state, -1, root, error) &&
            root.type == ViewNodeType::Grid &&
            root.columns == 3 &&
            root.columnGap && Near(*root.columnGap, 5.0f) &&
            root.rowGap && Near(*root.rowGap, 7.0f) &&
            root.children.size() == 6,
        "uniform-grid parsing must retain columns and independent gaps");
    Check(ValidateAndLayoutViewTree(root, 300.0f, 110.0f, error) &&
            Near(root.children[0].frame.x, 10.0f) &&
            Near(root.children[0].frame.y, 10.0f) &&
            Near(root.children[0].frame.width, 90.0f) &&
            Near(root.children[0].frame.height, 30.0f) &&
            Near(root.children[1].frame.x, 105.0f) &&
            Near(root.children[2].frame.x, 200.0f) &&
            Near(root.children[3].frame.x, 10.0f) &&
            Near(root.children[3].frame.y, 47.0f),
        "uniform grid must lay out visible children row-major in equal columns");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.grid({
            key = "placed-grid",
            columns = 3,
            columnGap = 4,
            rowGap = 6,
            children = {
                view.shape({ key = "wide", width = "fill", height = 20,
                    gridColumn = 1, gridRow = 1, columnSpan = 2 }),
                view.shape({ key = "right", width = "fill", height = 30,
                    gridColumn = 3, gridRow = 1, rowSpan = 2 }),
                view.shape({ key = "auto", width = "fill", height = 16 }),
                view.shape({ key = "partial", width = "fill", height = 10,
                    gridColumn = 2 }),
            },
        })
    )lua") == LUA_OK,
        "explicit-grid-placement Lua fixture must evaluate");
    root = {};
    Check(ParseLuaViewTree(state, -1, root, error) &&
            root.children.size() == 4 &&
            root.children[0].gridColumn == 1 &&
            root.children[0].gridRow == 1 &&
            root.children[0].columnSpan == 2 &&
            root.children[1].rowSpan == 2,
        "grid placement parsing must retain 1-based coordinates and spans");
    Check(ValidateAndLayoutViewTree(root, 158.0f, 100.0f, error) &&
            root.children[0].resolvedGridColumn == 0 &&
            root.children[0].resolvedGridRow == 0 &&
            Near(root.children[0].frame.x, 0.0f) &&
            Near(root.children[0].frame.width, 104.0f) &&
            root.children[1].resolvedGridColumn == 2 &&
            root.children[1].resolvedGridRow == 0 &&
            Near(root.children[1].frame.x, 108.0f) &&
            Near(root.children[1].frame.width, 50.0f) &&
            root.children[2].resolvedGridColumn == 0 &&
            root.children[2].resolvedGridRow == 1 &&
            Near(root.children[2].frame.y, 28.0f) &&
            root.children[3].resolvedGridColumn == 1 &&
            root.children[3].resolvedGridRow == 1 &&
            Near(root.children[3].frame.x, 54.0f),
        "grid placement must span tracks and auto-place into the first free cell");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.grid({
            key = "track-grid",
            columns = { 80, "auto", { fr = 1 },
                { min = 40, max = { fr = 2 } } },
            rows = { 24, { fr = 1 } },
            columnGap = 10,
            rowGap = 4,
            children = {
                view.shape({ key = "fixed", width = "fill",
                    height = "fill", gridColumn = 1, gridRow = 1 }),
                view.shape({ key = "auto", width = 60,
                    height = "fill", gridColumn = 2, gridRow = 1 }),
                view.shape({ key = "fraction", width = "fill",
                    height = "fill", gridColumn = 3, gridRow = 2 }),
                view.shape({ key = "minmax", width = "fill",
                    height = "fill", gridColumn = 4, gridRow = 2 }),
            },
        })
    )lua") == LUA_OK,
        "explicit grid-track Lua fixture must evaluate");
    root = {};
    Check(ParseLuaViewTree(state, -1, root, error) &&
            root.columns == 4 && root.columnTracks.size() == 4 &&
            root.columnTracks[0].kind == ViewGridTrackKind::Fixed &&
            Near(root.columnTracks[0].value, 80.0f) &&
            root.columnTracks[1].kind == ViewGridTrackKind::Auto &&
            root.columnTracks[2].kind == ViewGridTrackKind::Fraction &&
            Near(root.columnTracks[2].value, 1.0f) &&
            root.columnTracks[3].kind == ViewGridTrackKind::MinMax &&
            Near(root.columnTracks[3].minimum, 40.0f) &&
            root.columnTracks[3].maximumKind ==
                ViewGridTrackKind::Fraction &&
            Near(root.columnTracks[3].maximumValue, 2.0f) &&
            root.rowTracks.size() == 2,
        "grid track parsing must retain fixed, auto, fractional, and minmax definitions");
    Check(ValidateAndLayoutViewTree(root, 400.0f, 120.0f, error) &&
            Near(root.children[0].frame.x, 0.0f) &&
            Near(root.children[0].frame.width, 80.0f) &&
            Near(root.children[0].frame.height, 24.0f) &&
            Near(root.children[1].frame.x, 90.0f) &&
            Near(root.children[1].frame.width, 60.0f) &&
            Near(root.children[2].frame.x, 160.0f) &&
            Near(root.children[2].frame.width, 68.6667f) &&
            Near(root.children[2].frame.y, 28.0f) &&
            Near(root.children[2].frame.height, 92.0f) &&
            Near(root.children[3].frame.x, 238.6667f) &&
            Near(root.children[3].frame.width, 161.3333f),
        "grid track layout must resolve intrinsic content before distributing fractional space");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.grid({ key = "invalid-track",
            columns = { { min = 60, max = 40 } } })
    )lua") == LUA_OK,
        "invalid minmax grid-track fixture must evaluate");
    root = {};
    Check(!ParseLuaViewTree(state, -1, root, error) &&
            error.find("at least min") != std::string::npos,
        "minmax tracks must reject a fixed maximum below the minimum");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.virtualGrid({ key = "virtual-tracks",
            columns = { { fr = 1 }, { fr = 1 } },
            itemCount = 0, itemExtent = 24, firstIndex = 0 })
    )lua") == LUA_OK,
        "virtual-grid track fixture must evaluate");
    root = {};
    Check(!ParseLuaViewTree(state, -1, root, error) &&
            error.find("integer") != std::string::npos,
        "virtualGrid must retain fixed equal-width columns for bounded virtualization");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.grid({
            key = "overlap",
            columns = 2,
            children = {
                view.shape({ key = "first", gridColumn = 1, gridRow = 1 }),
                view.shape({ key = "second", gridColumn = 1, gridRow = 1 }),
            },
        })
    )lua") == LUA_OK,
        "overlapping-grid-placement Lua fixture must evaluate");
    root = {};
    Check(ParseLuaViewTree(state, -1, root, error) &&
            !ValidateAndLayoutViewTree(root, 100.0f, 100.0f, error) &&
            error.find("overlaps an occupied cell") != std::string::npos,
        "explicit grid placement must reject occupied cells");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.row({
            key = "not-grid",
            children = {
                view.shape({ key = "placed", gridColumn = 1 }),
            },
        })
    )lua") == LUA_OK,
        "misplaced-grid-child-property Lua fixture must evaluate");
    root = {};
    Check(ParseLuaViewTree(state, -1, root, error) &&
            !ValidateAndLayoutViewTree(root, 100.0f, 100.0f, error) &&
            error.find("only valid for direct grid") != std::string::npos,
        "grid placement properties must reject non-grid parents");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.grid({ key = "invalid", columns = 0 })
    )lua") == LUA_OK,
        "zero-column grid fixture must evaluate");
    ViewNode invalid;
    Check(!ParseLuaViewTree(state, -1, invalid, error) &&
            error.find("positive integer") != std::string::npos,
        "grid columns must reject zero at the Lua boundary");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.row({ key = "invalid-gap", columnGap = 4 })
    )lua") == LUA_OK,
        "misapplied grid-field fixture must evaluate");
    invalid = {};
    Check(!ParseLuaViewTree(state, -1, invalid, error) &&
            error.find("only grid") != std::string::npos,
        "grid-only fields must be rejected on other containers");
    invalid = {};
    invalid.type = ViewNodeType::Grid;
    invalid.key = "too-many-columns";
    invalid.columns = 65;
    Check(!ValidateAndLayoutViewTree(invalid, 300.0f, 100.0f, error) &&
            error.find("1 and 64") != std::string::npos,
        "grid layout must enforce the documented column limit");
    lua_close(state);
}

void TestFlowParsingAndLayout()
{
    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be available");
    luaL_openlibs(state);
    RegisterViewLibrary(state);
    Check(luaL_dostring(state, R"lua(
        return view.flow({
            key = "flow",
            columnGap = 5,
            rowGap = 7,
            alignItems = "start",
            justifyContent = "spaceBetween",
            children = {
                view.shape({ key = "one", width = 60, height = 20 }),
                view.shape({ key = "hidden", width = 140, height = 20,
                    visible = false }),
                view.shape({ key = "two", width = 60, height = 20 }),
                view.shape({ key = "three", width = 60, height = 20 }),
            },
        })
    )lua") == LUA_OK,
        "flow Lua fixture must evaluate");
    ViewNode root;
    std::string error;
    Check(ParseLuaViewTree(state, -1, root, error) &&
            root.type == ViewNodeType::Flow &&
            root.columnGap && Near(*root.columnGap, 5.0f) &&
            root.rowGap && Near(*root.rowGap, 7.0f),
        "flow parsing must retain independent line gaps");
    Check(ValidateAndLayoutViewTree(root, 150.0f, 80.0f, error) &&
            Near(root.children[0].frame.x, 0.0f) &&
            Near(root.children[0].frame.y, 0.0f) &&
            Near(root.children[2].frame.x, 90.0f) &&
            Near(root.children[2].frame.y, 0.0f) &&
            Near(root.children[3].frame.x, 0.0f) &&
            Near(root.children[3].frame.y, 27.0f) &&
            Near(root.children[3].frame.width, 60.0f),
        "flow must skip hidden children, wrap by width, and justify each line");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.flow({ key = "invalid", columns = 2 })
    )lua") == LUA_OK,
        "flow-columns fixture must evaluate");
    ViewNode invalid;
    Check(!ParseLuaViewTree(state, -1, invalid, error) &&
            error.find("only grid") != std::string::npos,
        "flow must reject fixed grid column counts");
    lua_close(state);
}

void TestScrollableCollections()
{
    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be available");
    luaL_openlibs(state);
    RegisterViewLibrary(state);
    Check(luaL_dostring(state, R"lua(
        local items = {}
        for index = 1, 3 do
            items[index] = view.listItem({
                key = "item-" .. index,
                height = 40,
                action = { id = "item.open", value = { index = index } },
                events = {
                    contextMenu = {
                        id = "item.menu",
                        value = { index = index },
                    },
                },
                accessibility = { label = "Item " .. index },
                children = {
                    view.text({
                        key = "label-" .. index,
                        text = "Item " .. index,
                    }),
                },
            })
        end
        return view.scroll({
            key = "feed",
            height = 100,
            padding = 4,
            events = { scrollEnd = { id = "feed.end" } },
            children = {
                view.list({ key = "items", gap = 4, children = items }),
            },
        })
    )lua") == LUA_OK,
        "scrollable-list Lua fixture must evaluate");
    ViewNode root;
    std::string error;
    Check(ParseLuaViewTree(state, -1, root, error) &&
            root.type == ViewNodeType::Scroll &&
            root.orientation == ViewOrientation::Vertical &&
            root.children.size() == 1 &&
            root.children[0].type == ViewNodeType::List &&
            root.children[0].orientation == ViewOrientation::Vertical &&
            root.events.at("scrollEnd").id == "feed.end" &&
            root.children[0].children.size() == 3 &&
            root.children[0].children[0].type ==
                ViewNodeType::ListItem,
        "scroll, list, and listItem constructors must retain typed nodes");
    Check(ValidateAndLayoutViewTree(root, 200.0f, 100.0f, error),
        "a bounded scrollable list must validate and lay out");
    std::vector<ViewScrollViewport> viewports;
    Check(ApplyViewScrollOffsets(root,
            [](std::string_view key, float) {
                return key == "feed" ? 100.0f : 0.0f;
            }, viewports, error) && viewports.size() == 1 &&
            viewports[0].key == "feed" &&
            Near(viewports[0].viewportExtent, 92.0f) &&
            Near(viewports[0].contentExtent, 128.0f) &&
            Near(viewports[0].maximum, 36.0f) &&
            Near(viewports[0].offset, 36.0f) &&
            root.clipFrame && Near(root.clipFrame->y, 4.0f),
        "scroll state must clamp a host offset to measured content extent");
    std::vector<InteractionRegion> regions;
    Check(CollectViewInteractionRegions(root, regions, error) &&
            regions.size() == 4 && regions[0].key == "feed" &&
            regions[0].events.at("scrollEnd").id == "feed.end" &&
            regions[1].key == "item-1" &&
            regions[1].clip && Near(regions[1].clip->y, 4.0f) &&
            Near(regions[1].clip->height, 92.0f) &&
            regions[1].events.at("click").id == "item.open" &&
            regions[1].events.at("contextMenu").id == "item.menu" &&
            regions[1].accessibilityRole == "listitem" &&
            regions[1].accessibilityLabel == "Item 1" &&
            root.children[0].children[0].frame.y < root.clipFrame->y,
        "scroll-end and list-item actions must keep independent clipped regions");
    WidgetInteractionRegions stagedRegions;
    stagedRegions.BeginFrame();
    bool staged = true;
    for (auto& region : regions)
        staged = staged && stagedRegions.Submit(std::move(region), error);
    Check(staged,
        "scrollEnd regions must pass the host interaction transaction validator");
    stagedRegions.CommitFrame();

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.gridList({
            key = "tiles",
            columns = 2,
            columnGap = 6,
            rowGap = 8,
            children = {
                view.listItem({
                    key = "tile",
                    accessibility = { label = "Tile" },
                    children = {
                        view.text({ key = "tile-label", text = "Tile" }),
                    },
                }),
            },
        })
    )lua") == LUA_OK,
        "grid-list Lua fixture must evaluate");
    ViewNode grid;
    Check(ParseLuaViewTree(state, -1, grid, error) &&
            grid.type == ViewNodeType::GridList && grid.columns == 2 &&
            ValidateAndLayoutViewTree(grid, 200.0f, 80.0f, error),
        "gridList must reuse bounded row-major collection layout");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.list({
            key = "selectable",
            selectionMode = "multiple",
            selectedKeys = { "item-b" },
            events = { change = { id = "items.select" } },
            children = {
                view.listItem({
                    key = "item-a", height = 32,
                    accessibility = { label = "Item A" },
                    children = {
                        view.text({ key = "item-a-label", text = "A" }),
                    },
                }),
                view.listItem({
                    key = "item-b", height = 32,
                    events = { doubleClick = { id = "item.open" } },
                    accessibility = { label = "Item B" },
                    children = {
                        view.text({ key = "item-b-label", text = "B" }),
                    },
                }),
            },
        })
    )lua") == LUA_OK,
        "controlled collection selection fixture must evaluate");
    ViewNode selectable;
    Check(ParseLuaViewTree(state, -1, selectable, error) &&
            selectable.selectionMode == ViewSelectionMode::Multiple &&
            selectable.selectedKeys ==
                std::vector<std::string>{ "item-b" } &&
            ValidateAndLayoutViewTree(selectable, 200.0f, 80.0f, error) &&
            !selectable.children[0].selected &&
            selectable.children[1].selected,
        "collection selection must normalize controlled keys onto item state");
    regions.clear();
    Check(CollectViewInteractionRegions(selectable, regions, error) &&
            regions.size() == 2 && regions[0].key == "item-a" &&
            regions[0].controlKind ==
                InteractionControlKind::SelectionMultiple &&
            regions[0].currentSelectedKeys ==
                std::vector<std::string>{ "item-b" } &&
            regions[0].proposedSelectedKey == "item-a" &&
            regions[0].events.at("change").id == "items.select" &&
            regions[1].events.at("doubleClick").id == "item.open",
        "collection items must expose one controlled selection region each");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.list({
            key = "invalid-list",
            children = {
                view.text({ key = "not-item", text = "Invalid" }),
            },
        })
    )lua") == LUA_OK,
        "invalid collection fixture must evaluate");
    ViewNode invalid;
    Check(ParseLuaViewTree(state, -1, invalid, error) &&
            !ValidateAndLayoutViewTree(invalid, 200.0f, 80.0f, error) &&
            error.find("listItem") != std::string::npos,
        "collection containers must reject non-listItem direct children");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.list({
            key = "missing-label",
            children = {
                view.listItem({
                    key = "unlabelled-item",
                    children = {
                        view.text({ key = "content", text = "Content" }),
                    },
                }),
            },
        })
    )lua") == LUA_OK,
        "unlabelled list-item fixture must evaluate");
    ViewNode unlabelled;
    Check(ParseLuaViewTree(state, -1, unlabelled, error) &&
            !ValidateAndLayoutViewTree(unlabelled, 200.0f, 80.0f, error) &&
            error.find("accessibility.label") != std::string::npos,
        "list items must require stable accessibility labels");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.shape({ key = "invalid-scroll-event",
            events = { scrollEnd = { id = "invalid.end" } } })
    )lua") == LUA_OK,
        "misapplied scroll-end fixture must evaluate");
    ViewNode invalidScrollEvent;
    Check(ParseLuaViewTree(state, -1, invalidScrollEvent, error) &&
            !ValidateAndLayoutViewTree(
                invalidScrollEvent, 100.0f, 40.0f, error) &&
            error.find("reserved for scroll") != std::string::npos,
        "scrollEnd must remain scoped to host-managed scroll containers");
    lua_close(state);

    ViewNode horizontal;
    horizontal.type = ViewNodeType::Scroll;
    horizontal.key = "horizontal";
    horizontal.orientation = ViewOrientation::Horizontal;
    ViewNode row;
    row.type = ViewNodeType::Row;
    row.key = "wide-row";
    row.width = { ViewLengthKind::Fixed, 240.0f };
    row.height = { ViewLengthKind::Fill, 0.0f };
    horizontal.children.push_back(row);
    Check(ValidateAndLayoutViewTree(horizontal, 100.0f, 40.0f, error),
        "a horizontal scroll viewport must validate");
    viewports.clear();
    Check(ApplyViewScrollOffsets(horizontal,
            [](std::string_view, float) { return 60.0f; },
            viewports, error) && viewports.size() == 1 &&
            Near(viewports[0].maximum, 140.0f) &&
            Near(horizontal.children[0].frame.x, -60.0f),
        "horizontal scroll must translate content on the x axis");
}

void TestListOrientation()
{
    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be available");
    luaL_openlibs(state);
    RegisterViewLibrary(state);
    Check(luaL_dostring(state, R"lua(
        return view.stack({
            key = "host",
            children = {
                view.list({
                    key = "horizontal-items",
                    orientation = "horizontal",
                    width = "auto",
                    alignSelf = "start",
                    gap = 4,
                    children = {
                        view.listItem({
                            key = "first", width = 40, height = 30,
                            accessibility = { label = "First" },
                            children = {
                                view.text({ key = "first-label", text = "First" }),
                            },
                        }),
                        view.listItem({
                            key = "second", width = 60, height = 30,
                            accessibility = { label = "Second" },
                            children = {
                                view.text({ key = "second-label", text = "Second" }),
                            },
                        }),
                    },
                }),
            },
        })
    )lua") == LUA_OK,
        "horizontal list fixture must evaluate");
    ViewNode horizontal;
    std::string error;
    Check(ParseLuaViewTree(state, -1, horizontal, error) &&
            horizontal.children.size() == 1 &&
            horizontal.children[0].orientation ==
                ViewOrientation::Horizontal,
        "horizontal list orientation must parse");
    Check(ValidateAndLayoutViewTree(horizontal, 200.0f, 50.0f, error),
        "horizontal list orientation must validate and lay out");
    Check(Near(horizontal.children[0].frame.width, 104.0f) &&
            Near(horizontal.children[0].frame.height, 30.0f),
        "horizontal list orientation must drive intrinsic size");
    Check(Near(horizontal.children[0].children[0].frame.x, 0.0f) &&
            Near(horizontal.children[0].children[1].frame.x, 44.0f) &&
            Near(horizontal.children[0].children[1].frame.y,
                horizontal.children[0].children[0].frame.y),
        "horizontal list orientation must place items on the x axis");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.gridList({ key = "grid", columns = 1,
            orientation = "horizontal", children = {} })
    )lua") == LUA_OK,
        "unsupported collection orientation fixture must evaluate");
    ViewNode invalid;
    Check(!ParseLuaViewTree(state, -1, invalid, error) &&
            error.find("accept orientation") != std::string::npos,
        "grid and virtual collection orientation must stay rejected");
    lua_close(state);
}

void TestVirtualizedCollections()
{
    std::string error;
    ViewVirtualRange calculated;
    Check(ComputeViewVirtualRange(1000, 40.0f, 1, 4.0f,
            100.0f, 220.0f, 2, calculated, error) &&
            calculated.firstIndex == 4 &&
            calculated.lastIndex == 10 &&
            Near(calculated.offset, 220.0f) &&
            Near(calculated.contentExtent, 43996.0f) &&
            Near(calculated.maximum, 43896.0f),
        "virtual range calculation must return an overscanned 1-based window");
    Check(ComputeViewVirtualRange(0, 40.0f, 1, 0.0f,
            100.0f, 50.0f, 2, calculated, error) &&
            calculated.firstIndex == 0 && calculated.lastIndex == 0 &&
            Near(calculated.offset, 0.0f) &&
            Near(calculated.contentExtent, 100.0f),
        "empty virtual ranges must retain a bounded viewport without items");
    Check(!ComputeViewVirtualRange(1000000, 2.0f, 1, 0.0f,
            100.0f, 0.0f, 2, calculated, error) &&
            error.find("1000000") != std::string::npos,
        "virtual range calculation must reject oversized logical content");
    Check(!ComputeViewVirtualRange(1000, 1.0f, 1, 0.0f,
            200.0f, 0.0f, 0, calculated, error) &&
            error.find("128") != std::string::npos,
        "virtual range calculation must reject oversized visible windows");

    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be available");
    luaL_openlibs(state);
    RegisterViewLibrary(state);
    Check(luaL_dostring(state, R"lua(
        local items = {}
        for index = 4, 10 do
            items[#items + 1] = view.listItem({
                key = "virtual-item-" .. index,
                action = { id = "item.open", value = { index = index } },
                accessibility = { label = "Virtual item " .. index },
                children = {
                    view.text({
                        key = "virtual-label-" .. index,
                        text = "Virtual item " .. index,
                    }),
                },
            })
        end
        return view.virtualList({
            key = "virtual-feed",
            height = 100,
            itemCount = 1000,
            itemExtent = 40,
            firstIndex = 4,
            overscan = 2,
            rowGap = 4,
            children = items,
        })
    )lua") == LUA_OK,
        "virtual-list Lua fixture must evaluate");
    ViewNode list;
    Check(ParseLuaViewTree(state, -1, list, error) &&
            list.type == ViewNodeType::VirtualList &&
            list.orientation == ViewOrientation::Vertical &&
            list.itemCount == 1000 && list.itemExtent == 40.0f &&
            list.firstIndex == 4 && list.overscan == 2 &&
            list.children.size() == 7,
        "virtualList must retain fixed-extent window metadata");
    Check(ValidateAndLayoutViewTree(list, 200.0f, 100.0f, error),
        "a bounded virtual list window must validate and lay out");
    std::vector<ViewScrollViewport> viewports;
    Check(ApplyViewScrollOffsets(list,
            [](std::string_view key, float) {
                return key == "virtual-feed" ? 220.0f : 0.0f;
            }, viewports, error) && viewports.size() == 1 &&
            Near(viewports[0].contentExtent, 43996.0f) &&
            Near(viewports[0].offset, 220.0f) &&
            Near(list.children[2].frame.y, 0.0f),
        "virtualList must place its global item window and reuse host scrolling");
    std::vector<InteractionRegion> regions;
    Check(CollectViewInteractionRegions(list, regions, error) &&
            regions.size() == 3 &&
            regions.front().key == "virtual-item-6" &&
            regions.back().key == "virtual-item-8" &&
            regions.front().clip && Near(regions.front().clip->height, 100.0f),
        "only visible virtual list items must expose clipped interactions");

    ViewNode incomplete = list;
    incomplete.firstIndex = 1;
    incomplete.children.resize(1);
    Check(ValidateAndLayoutViewTree(incomplete, 200.0f, 100.0f, error) &&
            !ApplyViewScrollOffsets(incomplete,
                [](std::string_view, float) { return 220.0f; },
                viewports, error) &&
            error.find("cover") != std::string::npos,
        "the host must reject a virtual window that misses visible items");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        local items = {}
        for index = 5, 20 do
            items[#items + 1] = view.listItem({
                key = "tile-" .. index,
                accessibility = { label = "Tile " .. index },
                children = {
                    view.text({ key = "tile-label-" .. index,
                        text = "Tile " .. index }),
                },
            })
        end
        return view.virtualGrid({
            key = "virtual-tiles",
            height = 70,
            columns = 4,
            itemCount = 100,
            itemExtent = 30,
            firstIndex = 5,
            overscan = 1,
            columnGap = 6,
            rowGap = 5,
            children = items,
        })
    )lua") == LUA_OK,
        "virtual-grid Lua fixture must evaluate");
    ViewNode grid;
    Check(ParseLuaViewTree(state, -1, grid, error) &&
            grid.type == ViewNodeType::VirtualGrid &&
            grid.columns == 4 && grid.firstIndex == 5 &&
            ValidateAndLayoutViewTree(grid, 200.0f, 70.0f, error),
        "virtualGrid must parse and use global row-major item indices");
    viewports.clear();
    Check(ApplyViewScrollOffsets(grid,
            [](std::string_view, float) { return 70.0f; },
            viewports, error) && viewports.size() == 1 &&
            Near(viewports[0].contentExtent, 870.0f) &&
            Near(grid.children[0].frame.y, -35.0f) &&
            Near(grid.children[4].frame.y, 0.0f),
        "virtualGrid must place overscan rows around the visible grid rows");
    regions.clear();
    Check(CollectViewInteractionRegions(grid, regions, error) &&
            regions.size() == 8 && regions.front().key == "tile-9" &&
            regions.back().key == "tile-16",
        "virtualGrid must only materialize interaction regions for visible rows");
    lua_close(state);
}

void TestCollectionContentStates()
{
    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be available");
    luaL_openlibs(state);
    RegisterViewLibrary(state);
    Check(luaL_dostring(state, R"lua(
        return view.list({
            key = "feed",
            busy = true,
            loadingContent = view.column({
                key = "feed-loading",
                alignItems = "center",
                justifyContent = "center",
                children = {
                    view.text({ key = "feed-loading-label",
                        text = "Loading" }),
                },
            }),
            emptyContent = view.text({
                key = "feed-empty", text = "No items",
            }),
            children = {
                view.listItem({
                    key = "stale-item",
                    accessibility = { label = "Stale" },
                    children = {
                        view.text({ key = "stale-label", text = "Stale" }),
                    },
                }),
            },
        })
    )lua") == LUA_OK,
        "loading collection content fixture must evaluate");
    ViewNode loading;
    std::string error;
    Check(ParseLuaViewTree(state, -1, loading, error) && loading.busy &&
            loading.collectionContent == ViewCollectionContent::Loading &&
            loading.children.size() == 1 &&
            loading.children[0].key == "feed-loading" &&
            ValidateAndLayoutViewTree(loading, 200.0f, 80.0f, error) &&
            Near(loading.children[0].frame.width, 200.0f) &&
            Near(loading.children[0].frame.height, 80.0f),
        "busy collections must atomically replace item children with loadingContent");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.virtualList({
            key = "empty-virtual",
            height = 100,
            itemCount = 0,
            itemExtent = 44,
            firstIndex = 0,
            emptyContent = view.text({
                key = "empty-label", text = "Nothing here",
                textAlign = "center",
            }),
        })
    )lua") == LUA_OK,
        "empty virtual collection fixture must evaluate");
    ViewNode empty;
    Check(ParseLuaViewTree(state, -1, empty, error) &&
            empty.collectionContent == ViewCollectionContent::Empty &&
            empty.children.size() == 1 &&
            ValidateAndLayoutViewTree(empty, 200.0f, 100.0f, error),
        "itemCount zero must activate a validated virtual emptyContent node");
    std::vector<ViewScrollViewport> viewports;
    Check(ApplyViewScrollOffsets(empty,
            [](std::string_view, float) { return 90.0f; },
            viewports, error) && viewports.size() == 1 &&
            Near(viewports[0].maximum, 0.0f) &&
            Near(viewports[0].contentExtent, 100.0f) &&
            Near(empty.children[0].frame.y, 0.0f),
        "virtual emptyContent must fill the viewport without a stale scroll range");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.box({
            key = "invalid",
            emptyContent = view.text({ key = "empty", text = "Empty" }),
        })
    )lua") == LUA_OK,
        "invalid non-collection content fixture must evaluate");
    ViewNode invalid;
    Check(!ParseLuaViewTree(state, -1, invalid, error) &&
            error.find("emptyContent") != std::string::npos,
        "emptyContent must remain scoped to collection containers");
    lua_close(state);
}

void TestDeclarativeInputControls()
{
    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be available");
    luaL_openlibs(state);
    RegisterViewLibrary(state);
    Check(luaL_dostring(state, R"lua(
        return view.column({
            key = "form",
            gap = 4,
            children = {
                view.textInput({
                    key = "name", value = "Snow", placeholder = "Name",
                    selection = { start = 1, finish = 4 },
                    required = true,
                    maxBytes = 64, action = { id = "name.change" },
                    validationState = "error",
                    validationMessage = "Name is required",
                    validationStyle = {
                        borderColor = 0xAA0000, borderWidth = 2,
                    },
                    events = {
                        focus = { id = "name.focus" },
                        blur = { id = "name.blur" },
                        submit = { id = "name.submit" },
                        selectionChange = { id = "name.selection" },
                        contextMenu = { id = "name.menu" },
                    },
                    accessibility = { label = "Name" },
                }),
                view.textArea({
                    key = "notes", value = "one\ntwo", height = 80,
                    liveUpdate = false, readOnly = true,
                    focusable = false,
                    accessibility = { label = "Notes" },
                }),
                view.searchBox({
                    key = "search", value = "clock",
                    action = { id = "search.change" },
                    events = { submit = { id = "search.submit" } },
                    accessibility = { label = "Search" },
                }),
                view.numberInput({
                    key = "count", value = 5, min = 0, max = 10, step = 1,
                    selectAll = true, action = { id = "count.change" },
                    accessibility = { label = "Count" },
                }),
                view.select({
                    key = "theme", selectedValue = "dark", expanded = true,
                    placeholder = "Theme",
                    options = {
                        { key = "light", value = "light", label = "Light" },
                        { key = "dark", value = "dark", label = "Dark" },
                    },
                    action = { id = "theme.change" },
                    events = { click = { id = "theme.toggle" } },
                    accessibility = { label = "Theme" },
                }),
            },
        })
    )lua") == LUA_OK,
        "declarative input fixture must evaluate");
    ViewNode root;
    std::string error;
    Check(ParseLuaViewTree(state, -1, root, error) &&
            root.children.size() == 5 &&
            root.children[0].type == ViewNodeType::TextInput &&
            root.children[0].inputValue == "Snow" &&
            root.children[0].textSelection ==
                std::optional<ViewTextSelection>{{ 1, 4 }} &&
            root.children[0].required &&
            root.children[0].validationState ==
                ViewValidationState::Error &&
            root.children[0].validationMessage == "Name is required" &&
            root.children[0].validationStyle.borderColor == 0xAA0000u &&
            root.children[0].validationStyle.borderWidth == 2.0f &&
            root.children[1].type == ViewNodeType::TextArea &&
            !root.children[1].liveUpdate && root.children[1].readOnly &&
            root.children[1].focusable == false &&
            root.children[2].type == ViewNodeType::SearchBox &&
            root.children[3].type == ViewNodeType::NumberInput &&
            Near(root.children[3].value, 5.0f) &&
            root.children[4].type == ViewNodeType::Select &&
            root.children[4].expanded,
        "all five declarative input types must retain controlled fields");
    Check(ValidateAndLayoutViewTree(root, 260.0f, 320.0f, error),
        "declarative input fixture must validate and lay out");

    std::vector<ViewInputControl> controls;
    Check(CollectViewInputControls(root, controls, error) &&
            controls.size() == 4 &&
            controls[0].key == "name" &&
            controls[0].changeAction.id == "name.change" &&
            controls[0].selection ==
                std::optional<ViewTextSelection>{{ 1, 4 }} &&
            controls[0].selectionChangeAction.id == "name.selection" &&
            controls[0].focusAction.id == "name.focus" &&
            controls[1].type == ViewNodeType::TextArea &&
            controls[1].readOnly && !controls[1].focusable &&
            controls[1].changeAction.id.empty() &&
            controls[3].type == ViewNodeType::NumberInput &&
            controls[3].value == "5",
        "text-like inputs must produce typed host-control descriptors");

    std::vector<InteractionRegion> regions;
    Check(CollectViewInteractionRegions(root, regions, error) &&
            regions.size() == 7 && regions[0].key == "name" &&
            regions[0].cursor == "text" &&
            !regions[0].events.contains("change") &&
            !regions[0].events.contains("selectionChange") &&
            !regions[0].events.contains("focus") &&
            regions[4].key == "theme" &&
            regions[4].hasExpandedProposal &&
            regions[5].key == "theme/light" &&
            regions[6].key == "theme/dark",
        "inputs and expanded select options must expose non-overlapping host and pointer regions");

    WidgetInteractionRegions interaction;
    interaction.BeginFrame();
    for (auto& region : regions)
        Check(interaction.Submit(std::move(region), error),
            "declarative input regions must submit");
    interaction.CommitFrame();
    const auto toggle = interaction.ResolveAction(
        "theme", "click");
    const auto choice = interaction.ResolveAction(
        "theme/light", "click");
    Check(toggle && toggle->previousExpanded == true &&
            toggle->expanded == false && choice &&
            choice->previousSelection == "dark" &&
            choice->selection == "light",
        "select must propose controlled expansion and selection values");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.textInput({
            key = "bad", value = "x",
            action = { id = "bad.change" },
            events = { click = { id = "bad.click" } },
            accessibility = { label = "Bad" },
        })
    )lua") == LUA_OK,
        "invalid input event fixture must evaluate");
    ViewNode invalid;
    Check(ParseLuaViewTree(state, -1, invalid, error) &&
            !ValidateAndLayoutViewTree(invalid, 200.0f, 40.0f, error) &&
            error.find("reject click") != std::string::npos,
        "controlled text inputs must reject ambiguous click actions");
    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.numberInput({
            key = "bad-state", value = 1,
            validationState = "fatal",
            action = { id = "bad-state.change" },
            accessibility = { label = "Bad state" },
        })
    )lua") == LUA_OK,
        "invalid validation-state fixture must evaluate");
    Check(!ParseLuaViewTree(state, -1, invalid, error) &&
            error.find("validationState") != std::string::npos,
        "validationState must reject values outside the public enum");
    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.textInput({
            key = "bad-selection", value = "雪a",
            selection = { start = 1, finish = 4 },
            action = { id = "bad-selection.change" },
            events = {
                selectionChange = { id = "bad-selection.selection" },
            },
            accessibility = { label = "Bad selection" },
        })
    )lua") == LUA_OK,
        "invalid text-selection fixture must evaluate");
    Check(!ParseLuaViewTree(state, -1, invalid, error) &&
            error.find("UTF-8 byte boundaries") != std::string::npos,
        "text selection must reject offsets inside a UTF-8 code point");
    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.searchBox({
            key = "missing-selection-action", value = "clock",
            selection = { start = 0, finish = 5 },
            action = { id = "search.change" },
            accessibility = { label = "Search" },
        })
    )lua") == LUA_OK,
        "missing selectionChange fixture must evaluate");
    Check(ParseLuaViewTree(state, -1, invalid, error) &&
            !ValidateAndLayoutViewTree(invalid, 200.0f, 40.0f, error) &&
            error.find("requires selection and selectionChange") !=
                std::string::npos,
        "controlled text selection must require a proposal action");
    lua_close(state);
}

void TestStyledTextAndMonthCalendar()
{
    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be available");
    luaL_openlibs(state);
    RegisterViewLibrary(state);
    Check(luaL_dostring(state, R"lua(
        return view.column({
            key = "content",
            gap = 8,
            children = {
                view.styledText({
                    key = "summary",
                    height = 40,
                    spans = {
                        { text = "Build ", foreground = 0x94A3B8 },
                        { key = "result", text = "passed",
                            foreground = 0x4ADE80,
                            hoverForeground = 0x86EFAC,
                            pressedForeground = 0x22C55E,
                            bold = true, underline = true,
                            tooltip = "Open build result",
                            action = { id = "build.open" },
                            events = {
                                contextMenu = { id = "build.menu" },
                                pointerMove = { id = "build.move" },
                            },
                            accessibility = { label = "Build result" } },
                        { text = " · validation pending", italic = true,
                            fontSize = 13 },
                    },
                    accessibility = { label = "Build passed, validation pending" },
                }),
                view.monthCalendar({
                    key = "calendar",
                    year = 2026,
                    month = 8,
                    firstDayOfWeek = 2,
                    selectedDate = "2026-08-15",
                    todayDate = "2026-08-14",
                    eventDates = { "2026-08-15", "2026-08-21" },
                    weekdayLabels = { "Sun", "Mon", "Tue", "Wed",
                        "Thu", "Fri", "Sat" },
                    height = 224,
                    action = { id = "calendar.select" },
                    events = {
                        contextMenu = { id = "calendar.menu" },
                    },
                    selectedStyle = { background = 0x4C9AFF },
                    todayStyle = { borderColor = 0xFFFFFF, borderWidth = 2 },
                    adjacentStyle = { opacity = 0.35 },
                    eventStyle = { foreground = 0xFBBF24 },
                    accessibility = { label = "August 2026" },
                }),
            },
        })
    )lua") == LUA_OK,
        "styledText and monthCalendar Lua fixture must evaluate");
    ViewNode root;
    std::string error;
    Check(ParseLuaViewTree(state, -1, root, error) &&
            root.children.size() == 2 &&
            root.children[0].type == ViewNodeType::StyledText &&
            root.children[0].spans.size() == 3 &&
            root.children[0].text ==
                "Build passed · validation pending" &&
            root.children[0].spans[1].bold &&
            root.children[0].spans[1].underline &&
            root.children[0].spans[1].key == "result" &&
            root.children[0].spans[1].hoverForeground ==
                std::uint32_t{ 0x86EFAC } &&
            root.children[0].spans[1].pressedForeground ==
                std::uint32_t{ 0x22C55E } &&
            root.children[0].spans[1].events.contains("click") &&
            root.children[0].spans[1].events.contains("contextMenu") &&
            root.children[0].spans[1].events.contains("pointerMove") &&
            root.children[0].spans[1].accessibilityLabel == "Build result" &&
            root.children[0].textWrap == ViewTextWrap::Wrap &&
            root.children[0].overflowText == ViewTextOverflow::Clip &&
            root.children[0].verticalAlign == ViewAlignment::Center &&
            root.children[1].type == ViewNodeType::MonthCalendar &&
            root.children[1].calendarYear == 2026 &&
            root.children[1].calendarMonth == 8 &&
            root.children[1].firstDayOfWeek == 2 &&
            root.children[1].calendarEventDates.size() == 2 &&
            root.children[1].weekdayLabels[1] == "Mon",
        "styled text spans and calendar fields must remain strongly typed");
    Check(ValidateAndLayoutViewTree(root, 320.0f, 272.0f, error),
        "styledText and monthCalendar must validate and lay out together");
    ViewNode invalidSpan = root.children[0];
    invalidSpan.spans[1].key.clear();
    Check(!ValidateAndLayoutViewTree(invalidSpan, 320.0f, 40.0f, error) &&
            error.find("stable key") != std::string::npos,
        "interactive styledText spans must require a stable generated target key");

    std::array<ViewMonthCalendarCell, 42> cells;
    Check(BuildViewMonthCalendarCells(root.children[1], cells, error) &&
            cells[0].date == "2026-07-27" &&
            cells[19].date == "2026-08-15" &&
            cells[19].selected && cells[19].hasEvent,
        "monthCalendar must generate a deterministic six-week Monday-first grid");
    const ViewRect firstCell = ViewMonthCalendarCellFrame(
        root.children[1], 0);
    Check(firstCell.width > 0.0f && firstCell.height > 0.0f,
        "monthCalendar cells must receive positive bounded frames");

    std::vector<InteractionRegion> regions;
    Check(CollectViewInteractionRegions(root, regions, error) &&
            regions.size() == 43 &&
            regions[0].key == "calendar" &&
            regions[1].key == "calendar/2026-07-27" &&
            regions[20].key == "calendar/2026-08-15" &&
            regions[20].controlKind == InteractionControlKind::Radio &&
            regions[20].checked &&
            regions[20].accessibilityRole == "gridcell",
        "monthCalendar must expose one surface and 42 independently selectable date regions");
    WidgetInteractionRegions interaction;
    interaction.BeginFrame();
    for (auto& region : regions)
        Check(interaction.Submit(std::move(region), error),
            "monthCalendar regions must satisfy the interaction contract");
    interaction.CommitFrame();
    const auto nextDate = interaction.ResolveAction(
        "calendar/2026-08-21", "click");
    Check(nextDate && nextDate->eventName == "change" &&
            nextDate->previousSelection == "2026-08-15" &&
            nextDate->selection == "2026-08-21",
        "monthCalendar clicks must propose a controlled ISO date selection");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.monthCalendar({
            key = "bad-calendar",
            year = 2026,
            month = 2,
            selectedDate = "2026-02-30",
            weekdayLabels = { "S", "M", "T", "W", "T", "F", "S" },
            action = { id = "select" },
            accessibility = { label = "Invalid" },
        })
    )lua") == LUA_OK,
        "invalid calendar date fixture must evaluate");
    ViewNode invalid;
    Check(ParseLuaViewTree(state, -1, invalid, error) &&
            !ValidateAndLayoutViewTree(invalid, 280.0f, 224.0f, error) &&
            error.find("selectedDate") != std::string::npos,
        "monthCalendar must reject impossible controlled dates");
    lua_close(state);
}

void TestLogicalSlotSceneContract()
{
    LogicalSlotDeclarations declarations;
    declarations["primaryApp"] = {
        LogicalSlotKind::Binding, { "app.reference" },
        "reference", "allow", true, 1 };
    declarations["favorites"] = {
        LogicalSlotKind::Collection,
        { "desktop.item", "filesystem.reference" },
        "reference", {}, false, 4 };
    LogicalSlotModel model;
    std::string error;
    Check(model.Configure(declarations, error),
        "logical slot scene fixture must configure");

    ViewNode binding;
    binding.type = ViewNodeType::SlotSurface;
    binding.key = "primary-surface";
    binding.logicalSlotId = "primaryApp";
    binding.logicalSlotKind = LogicalSlotKind::Binding;
    ViewNode placeholder;
    placeholder.type = ViewNodeType::Text;
    placeholder.key = "primary-placeholder";
    placeholder.text = "Drop an application";
    binding.children.push_back(placeholder);
    Check(ValidateAndLayoutViewTree(binding, 160.0f, 120.0f, error) &&
            ValidateViewLogicalSlots(binding, model, error) &&
            binding.children[0].frame == binding.frame,
        "an empty binding may render one full-surface placeholder");

    LogicalSlotChange change;
    Check(model.Bind("primaryApp",
            { {}, {}, "app.reference", "Calendar", "startmenu",
                "application", "app:calendar", true },
            change, error),
        "binding fixture must accept its declared app reference");
    const auto* bound = model.Find("primaryApp");
    Check(bound && bound->items.size() == 1,
        "binding fixture must retain one host item");
    ViewNode slotItem;
    slotItem.type = ViewNodeType::SlotItem;
    slotItem.key = bound->items[0].id;
    slotItem.logicalSlotReference = bound->items[0].reference;
    slotItem.accessibilityLabel = bound->items[0].title;
    ViewNode label;
    label.type = ViewNodeType::Text;
    label.key = "primary-label";
    label.text = bound->items[0].title;
    slotItem.children.push_back(label);
    binding.children.assign(1, slotItem);
    binding.logicalSlotRevision = bound->revision;
    Check(ValidateAndLayoutViewTree(binding, 160.0f, 120.0f, error) &&
            ValidateViewLogicalSlots(binding, model, error),
        "a bound surface must report the exact opaque item and revision");
    binding.children[0].logicalSlotReference = "lsr-forged";
    Check(!ValidateViewLogicalSlots(binding, model, error) &&
            error.find("exact host item") != std::string::npos,
        "a forged binding reference must reject the whole scene commit");

    Check(model.Bind("favorites",
            { {}, {}, "desktop.item", "Notes", "desktop", "shortcut",
                "desktop:notes", true }, change, error) &&
        model.Bind("favorites",
            { {}, {}, "filesystem.reference", "Plan", "picker", "file",
                "file:plan", true }, change, error),
        "collection fixture must accept two declared reference kinds");
    const auto* favorites = model.Find("favorites");
    ViewNode collection;
    collection.type = ViewNodeType::SlotSurface;
    collection.key = "favorites-surface";
    collection.logicalSlotId = "favorites";
    collection.logicalSlotKind = LogicalSlotKind::Collection;
    collection.logicalSlotRevision = favorites->revision;
    for (std::size_t index = 0; index < favorites->items.size(); ++index)
    {
        ViewNode item;
        item.type = ViewNodeType::SlotItem;
        item.key = favorites->items[index].id;
        item.logicalSlotReference = favorites->items[index].reference;
        item.accessibilityLabel = favorites->items[index].title;
        ViewNode child;
        child.type = ViewNodeType::Text;
        child.key = "favorite-label-" + std::to_string(index);
        child.text = favorites->items[index].title;
        item.children.push_back(std::move(child));
        collection.children.push_back(std::move(item));
    }
    Check(ValidateAndLayoutViewTree(collection, 240.0f, 160.0f, error) &&
            ValidateViewLogicalSlots(collection, model, error),
        "a collection scene must report every host item in host order");
    std::swap(collection.children[0], collection.children[1]);
    Check(!ValidateViewLogicalSlots(collection, model, error) &&
            error.find("order") != std::string::npos,
        "stale collection order must reject the whole scene commit");

    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be available");
    luaL_openlibs(state);
    RegisterViewLibrary(state);
    Check(luaL_dostring(state, R"lua(
        return view.slotSurface({
            key = "primary",
            binding = "primaryApp",
            revision = 7,
            child = view.slotItem({
                key = "lsi-item",
                reference = "lsr-item",
                child = view.text({ key = "label", text = "Calendar" }),
                accessibility = { label = "Calendar" },
                events = { contextMenu = {
                    id = "item.menu", scope = "element"
                } },
            }),
        })
    )lua") == LUA_OK,
        "logical slot Lua scene fixture must evaluate");
    ViewNode parsed;
    Check(ParseLuaViewTree(state, -1, parsed, error) &&
            parsed.type == ViewNodeType::SlotSurface &&
            parsed.logicalSlotId == "primaryApp" &&
            parsed.logicalSlotRevision == 7 &&
            parsed.children.size() == 1 &&
            parsed.children[0].type == ViewNodeType::SlotItem &&
            parsed.children[0].logicalSlotReference == "lsr-item" &&
            parsed.children[0].children.size() == 1,
        "slotSurface and slotItem constructors must retain typed scene fields");
    lua_close(state);
}

void TestBoundedSizeConstraints()
{
    ViewNode root;
    root.type = ViewNodeType::Column;
    root.key = "constraints-root";

    ViewNode boundedText;
    boundedText.type = ViewNodeType::Text;
    boundedText.key = "bounded-text";
    boundedText.text = "Bounded";
    boundedText.width = { ViewLengthKind::Fixed, 40.0f };
    boundedText.height = { ViewLengthKind::Fixed, 80.0f };
    boundedText.minimumWidth = 120.0f;
    boundedText.maximumHeight = 32.0f;
    root.children.push_back(boundedText);

    ViewNode boundedFill;
    boundedFill.type = ViewNodeType::Spacer;
    boundedFill.key = "bounded-fill";
    boundedFill.width = { ViewLengthKind::Fill, 0.0f };
    boundedFill.height = { ViewLengthKind::Fill, 0.0f };
    boundedFill.maximumWidth = 90.0f;
    boundedFill.minimumHeight = 48.0f;
    root.children.push_back(boundedFill);

    std::string error;
    Check(ValidateAndLayoutViewTree(root, 300.0f, 200.0f, error) &&
            root.children[0].frame.width == 120.0f &&
            root.children[0].frame.height == 32.0f &&
            root.children[1].frame.width == 90.0f &&
            root.children[1].frame.height == 168.0f,
        "size constraints must participate in fixed and fill layout");

    ViewNode invalid = root;
    invalid.key = "invalid-constraints";
    invalid.minimumWidth = 200.0f;
    invalid.maximumWidth = 100.0f;
    Check(!ValidateAndLayoutViewTree(invalid, 300.0f, 200.0f, error) &&
            error.find("finite and bounded") != std::string::npos,
        "a minimum larger than its maximum must reject the tree");

    ViewNode aspectRoot;
    aspectRoot.type = ViewNodeType::Box;
    aspectRoot.key = "aspect-root";
    ViewNode aspectChild;
    aspectChild.type = ViewNodeType::Shape;
    aspectChild.key = "aspect-child";
    aspectChild.width = { ViewLengthKind::Fill, 0.0f };
    aspectChild.height = { ViewLengthKind::Auto, 0.0f };
    aspectChild.maximumWidth = 160.0f;
    aspectChild.aspectRatio = 2.0f;
    aspectRoot.children.push_back(aspectChild);
    Check(ValidateAndLayoutViewTree(aspectRoot, 300.0f, 200.0f, error) &&
            aspectRoot.children[0].frame.width == 160.0f &&
            aspectRoot.children[0].frame.height == 80.0f,
        "aspectRatio must derive the auto axis after size constraints");

    ViewNode conflictingRatio;
    conflictingRatio.type = ViewNodeType::Shape;
    conflictingRatio.key = "conflicting-ratio";
    conflictingRatio.minimumWidth = 200.0f;
    conflictingRatio.maximumHeight = 50.0f;
    conflictingRatio.aspectRatio = 2.0f;
    Check(!ValidateAndLayoutViewTree(
            conflictingRatio, 300.0f, 200.0f, error) &&
            error == "aspectRatio conflicts with size constraints",
        "mutually impossible ratio constraints must reject the tree");

    ViewNode mismatchedFixed;
    mismatchedFixed.type = ViewNodeType::Shape;
    mismatchedFixed.key = "mismatched-fixed";
    mismatchedFixed.width = { ViewLengthKind::Fixed, 100.0f };
    mismatchedFixed.height = { ViewLengthKind::Fixed, 100.0f };
    mismatchedFixed.aspectRatio = 2.0f;
    Check(!ValidateAndLayoutViewTree(
            mismatchedFixed, 300.0f, 200.0f, error) &&
            error == "fixed width and height must match aspectRatio",
        "mismatched double-fixed dimensions must reject the tree");

    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be available");
    luaL_openlibs(state);
    RegisterViewLibrary(state);
    Check(luaL_dostring(state, R"lua(
        return view.box({
            key = "lua-constraints",
            minWidth = 64,
            maxWidth = 256,
            minHeight = 48,
            maxHeight = 192,
            aspectRatio = 1.5,
        })
    )lua") == LUA_OK,
        "bounded size constraint Lua fixture must evaluate");
    ViewNode parsed;
    Check(ParseLuaViewTree(state, -1, parsed, error) &&
            parsed.minimumWidth == 64.0f &&
            parsed.maximumWidth == 256.0f &&
            parsed.minimumHeight == 48.0f &&
            parsed.maximumHeight == 192.0f &&
            parsed.aspectRatio == 1.5f,
        "Lua parsing must retain all bounded size constraints");
    lua_close(state);
}

void TestUniformMargins()
{
    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be available");
    luaL_openlibs(state);
    RegisterViewLibrary(state);
    Check(luaL_dostring(state, R"lua(
        return view.row({
            key = "margin-row",
            gap = 5,
            alignItems = "start",
            children = {
                view.shape({ key = "one", width = 40, height = 20,
                    margin = 5 }),
                view.shape({ key = "two", width = 40, height = 20,
                    margin = 10 }),
            },
        })
    )lua") == LUA_OK,
        "uniform-margin Lua fixture must evaluate");
    ViewNode row;
    std::string error;
    Check(ParseLuaViewTree(state, -1, row, error) &&
            row.children.size() == 2 &&
            Near(row.children[0].margin.left, 5.0f) &&
            Near(row.children[1].margin.left, 10.0f),
        "Lua parsing must retain bounded uniform margins");
    Check(ValidateAndLayoutViewTree(row, 200.0f, 60.0f, error) &&
            Near(row.children[0].frame.x, 5.0f) &&
            Near(row.children[0].frame.y, 5.0f) &&
            Near(row.children[0].frame.width, 40.0f) &&
            Near(row.children[1].frame.x, 65.0f) &&
            Near(row.children[1].frame.y, 10.0f),
        "linear layout must reserve margin outside child frames and gaps");

    lua_settop(state, 0);
    Check(luaL_dostring(state, R"lua(
        return view.row({
            key = "directional-insets",
            padding = { horizontal = 10, vertical = 4, left = 20 },
            alignItems = "start",
            children = {
                view.shape({ key = "directional-child", width = 40,
                    height = 20,
                    margin = { top = 1, right = 2, bottom = 3, left = 4 } }),
            },
        })
    )lua") == LUA_OK,
        "directional edge-inset Lua fixture must evaluate");
    ViewNode directional;
    Check(ParseLuaViewTree(state, -1, directional, error) &&
            directional.padding == ViewEdgeInsets{ 4, 10, 4, 20 } &&
            directional.children.size() == 1 &&
            directional.children[0].margin ==
                ViewEdgeInsets{ 1, 2, 3, 4 },
        "Lua parsing must expand axis insets and let explicit sides override them");
    Check(ValidateAndLayoutViewTree(
            directional, 100.0f, 50.0f, error) &&
            directional.children[0].frame == ViewRect{ 24, 5, 40, 20 },
        "directional padding and margins must affect their matching edges");

    ViewNode grid;
    grid.type = ViewNodeType::Grid;
    grid.key = "margin-grid";
    grid.columns = 1;
    ViewNode gridChild;
    gridChild.type = ViewNodeType::Shape;
    gridChild.key = "grid-child";
    gridChild.height = { ViewLengthKind::Fixed, 20.0f };
    gridChild.margin = 5.0f;
    grid.children.push_back(gridChild);
    Check(ValidateAndLayoutViewTree(grid, 100.0f, 50.0f, error) &&
            Near(grid.children[0].frame.x, 5.0f) &&
            Near(grid.children[0].frame.y, 5.0f) &&
            Near(grid.children[0].frame.width, 90.0f),
        "grid layout must reserve margin inside each cell");

    ViewNode flow;
    flow.type = ViewNodeType::Flow;
    flow.key = "margin-flow";
    flow.alignItems = ViewAlignment::Start;
    for (int index = 0; index < 2; ++index)
    {
        ViewNode child;
        child.type = ViewNodeType::Shape;
        child.key = "flow-" + std::to_string(index);
        child.width = { ViewLengthKind::Fixed, 40.0f };
        child.height = { ViewLengthKind::Fixed, 20.0f };
        child.margin = 5.0f;
        flow.children.push_back(std::move(child));
    }
    Check(ValidateAndLayoutViewTree(flow, 100.0f, 50.0f, error) &&
            Near(flow.children[0].frame.x, 5.0f) &&
            Near(flow.children[1].frame.x, 55.0f) &&
            Near(flow.children[1].frame.y, 5.0f),
        "flow wrapping must count margins in line width and child placement");

    ViewNode stack;
    stack.type = ViewNodeType::Stack;
    stack.key = "margin-stack";
    ViewNode stackChild;
    stackChild.type = ViewNodeType::Shape;
    stackChild.key = "stack-child";
    stackChild.width = { ViewLengthKind::Fill, 0.0f };
    stackChild.height = { ViewLengthKind::Fill, 0.0f };
    stackChild.margin = 10.0f;
    stack.children.push_back(stackChild);
    Check(ValidateAndLayoutViewTree(stack, 100.0f, 100.0f, error) &&
            stack.children[0].frame == ViewRect{ 10, 10, 80, 80 },
        "stack layout must inset fill children by their margins");

    ViewNode scroll;
    scroll.type = ViewNodeType::Scroll;
    scroll.key = "margin-scroll";
    scroll.orientation = ViewOrientation::Vertical;
    ViewNode scrollChild;
    scrollChild.type = ViewNodeType::Shape;
    scrollChild.key = "scroll-child";
    scrollChild.height = { ViewLengthKind::Fixed, 100.0f };
    scrollChild.margin = 5.0f;
    scroll.children.push_back(scrollChild);
    Check(ValidateAndLayoutViewTree(scroll, 100.0f, 50.0f, error),
        "scroll margin fixture must validate and lay out");
    std::vector<ViewScrollViewport> viewports;
    Check(ApplyViewScrollOffsets(scroll,
            [](std::string_view, float) { return 0.0f; },
            viewports, error) && viewports.size() == 1 &&
            Near(viewports[0].contentExtent, 110.0f),
        "scroll content extent must include leading and trailing margins");

    ViewNode invalid;
    invalid.type = ViewNodeType::Shape;
    invalid.key = "negative-margin";
    invalid.margin = -1.0f;
    Check(!ValidateAndLayoutViewTree(invalid, 40.0f, 40.0f, error) &&
            error ==
                "view node dimensions and typography must be finite and bounded",
        "negative margins must reject the complete view tree");
    lua_close(state);
}

void TestFlexSizing()
{
    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be available");
    luaL_openlibs(state);
    RegisterViewLibrary(state);
    Check(luaL_dostring(state, R"lua(
        return view.row({
            key = "grow-row",
            alignItems = "start",
            children = {
                view.shape({ key = "one", width = "auto", height = 20,
                    flexBasis = 100, flexGrow = 1, flexShrink = 0 }),
                view.shape({ key = "two", width = "auto", height = 20,
                    flexBasis = 50, flexGrow = 2 }),
            },
        })
    )lua") == LUA_OK,
        "flex sizing Lua fixture must evaluate");
    ViewNode grow;
    std::string error;
    Check(ParseLuaViewTree(state, -1, grow, error) &&
            grow.children.size() == 2 &&
            grow.children[0].flexBasis.kind == ViewLengthKind::Fixed &&
            Near(grow.children[0].flexBasis.value, 100.0f) &&
            Near(grow.children[0].flexGrow, 1.0f) &&
            Near(grow.children[0].flexShrink, 0.0f),
        "Lua parsing must retain basis, grow, and shrink factors");
    Check(ValidateAndLayoutViewTree(grow, 300.0f, 40.0f, error) &&
            Near(grow.children[0].frame.width, 150.0f) &&
            Near(grow.children[1].frame.x, 150.0f) &&
            Near(grow.children[1].frame.width, 150.0f),
        "positive free space must be distributed by flexGrow after flexBasis");

    ViewNode shrink;
    shrink.type = ViewNodeType::Row;
    shrink.key = "shrink-row";
    shrink.alignItems = ViewAlignment::Start;
    for (int index = 0; index < 2; ++index)
    {
        ViewNode child;
        child.type = ViewNodeType::Shape;
        child.key = "shrink-" + std::to_string(index);
        child.width = { ViewLengthKind::Auto, 0.0f };
        child.height = { ViewLengthKind::Fixed, 20.0f };
        child.flexBasis = { ViewLengthKind::Fixed, 100.0f };
        child.flexShrink = 1.0f;
        shrink.children.push_back(std::move(child));
    }
    shrink.children[0].minimumWidth = 80.0f;
    Check(ValidateAndLayoutViewTree(shrink, 120.0f, 40.0f, error) &&
            Near(shrink.children[0].frame.width, 80.0f) &&
            Near(shrink.children[1].frame.x, 80.0f) &&
            Near(shrink.children[1].frame.width, 40.0f),
        "flexShrink must redistribute overflow after one item reaches its minimum");

    shrink.children[0].minimumWidth.reset();
    shrink.children[0].flexShrink = 0.0f;
    Check(ValidateAndLayoutViewTree(shrink, 150.0f, 40.0f, error) &&
            Near(shrink.children[0].frame.width, 100.0f) &&
            Near(shrink.children[1].frame.width, 50.0f),
        "a zero flexShrink factor must preserve that item's basis");

    ViewNode fill;
    fill.type = ViewNodeType::Row;
    fill.key = "fill-row";
    fill.alignItems = ViewAlignment::Start;
    ViewNode filler;
    filler.type = ViewNodeType::Shape;
    filler.key = "filler";
    filler.height = { ViewLengthKind::Fixed, 20.0f };
    ViewNode fixed;
    fixed.type = ViewNodeType::Shape;
    fixed.key = "fixed";
    fixed.width = { ViewLengthKind::Fixed, 50.0f };
    fixed.height = { ViewLengthKind::Fixed, 20.0f };
    fill.children = { filler, fixed };
    Check(ValidateAndLayoutViewTree(fill, 200.0f, 40.0f, error) &&
            Near(fill.children[0].frame.width, 150.0f) &&
            Near(fill.children[1].frame.x, 150.0f),
        "fill must keep its existing implicit flexGrow factor");

    ViewNode invalid = fixed;
    invalid.key = "fill-basis";
    invalid.flexBasis = { ViewLengthKind::Fill, 0.0f };
    Check(!ValidateAndLayoutViewTree(invalid, 100.0f, 40.0f, error) &&
            error ==
                "view node dimensions and typography must be finite and bounded",
        "flexBasis must accept only auto or a bounded numeric value");
    lua_close(state);
}

void TestFlexLayout()
{
    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be available");
    luaL_openlibs(state);
    RegisterViewLibrary(state);
    Check(luaL_dostring(state, R"lua(
        return view.row({
            key = "direction",
            flexDirection = "columnReverse",
            alignItems = "start",
            gap = 5,
            children = {
                view.shape({ key = "first", width = 20, height = 20 }),
                view.shape({ key = "second", width = 30, height = 10 }),
            },
        })
    )lua") == LUA_OK,
        "flex-direction Lua fixture must evaluate");
    ViewNode direction;
    std::string error;
    Check(ParseLuaViewTree(state, -1, direction, error) &&
            direction.flexDirection == ViewFlexDirection::ColumnReverse &&
            direction.flexWrap == ViewFlexWrap::NoWrap &&
            direction.alignContent == ViewContentAlignment::Stretch,
        "row and column must parse typed flex-container properties");
    Check(ValidateAndLayoutViewTree(direction, 100.0f, 70.0f, error) &&
            Near(direction.children[0].frame.x, 0.0f) &&
            Near(direction.children[0].frame.y, 50.0f) &&
            Near(direction.children[1].frame.x, 0.0f) &&
            Near(direction.children[1].frame.y, 35.0f),
        "reverse flexDirection must override the main axis without changing source order");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.row({
            key = "wrapped",
            flexDirection = "rowReverse",
            flexWrap = "wrapReverse",
            alignItems = "start",
            alignContent = "spaceAround",
            justifyContent = "spaceEvenly",
            gap = 10,
            children = {
                view.shape({ key = "one", width = 60, height = 20 }),
                view.shape({ key = "two", width = 60, height = 20 }),
                view.shape({ key = "three", width = 60, height = 20 }),
            },
        })
    )lua") == LUA_OK,
        "wrapped-flex Lua fixture must evaluate");
    ViewNode wrapped;
    Check(ParseLuaViewTree(state, -1, wrapped, error) &&
            wrapped.flexDirection == ViewFlexDirection::RowReverse &&
            wrapped.flexWrap == ViewFlexWrap::WrapReverse &&
            wrapped.alignContent == ViewContentAlignment::SpaceAround &&
            wrapped.justifyContent == ViewJustification::SpaceEvenly,
        "reverse flex and distributed alignment enums must retain typed values");
    Check(ValidateAndLayoutViewTree(wrapped, 130.0f, 100.0f, error) &&
            Near(wrapped.children[0].frame.x, 70.0f) &&
            Near(wrapped.children[1].frame.x, 0.0f) &&
            Near(wrapped.children[0].frame.y, 67.5f) &&
            Near(wrapped.children[2].frame.x, 35.0f) &&
            Near(wrapped.children[2].frame.y, 12.5f),
        "main and wrap reversal must compose with around/evenly distribution");

    ViewNode around;
    around.type = ViewNodeType::Row;
    around.key = "around";
    around.alignItems = ViewAlignment::Start;
    around.justifyContent = ViewJustification::SpaceAround;
    for (int index = 0; index < 2; ++index)
    {
        ViewNode child;
        child.type = ViewNodeType::Shape;
        child.key = "around-" + std::to_string(index);
        child.width = { ViewLengthKind::Fixed, 20.0f };
        child.height = { ViewLengthKind::Fixed, 10.0f };
        around.children.push_back(std::move(child));
    }
    Check(ValidateAndLayoutViewTree(around, 100.0f, 20.0f, error) &&
            Near(around.children[0].frame.x, 15.0f) &&
            Near(around.children[1].frame.x, 65.0f),
        "spaceAround must reserve half a distributed share at both edges");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.box({ key = "invalid", flexDirection = "row" })
    )lua") == LUA_OK,
        "misapplied flex-container property fixture must evaluate");
    ViewNode invalid;
    Check(!ParseLuaViewTree(state, -1, invalid, error) &&
            error.find("flexDirection") != std::string::npos,
        "non-flex containers must reject flex-container properties");
    lua_close(state);
}

void TestOverflowShadowAndImageTint()
{
    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be available");
    luaL_openlibs(state);
    RegisterViewLibrary(state);
    PushResourceHandle(state, LuaResourceType::Image, "status-mask");
    lua_setglobal(state, "statusMask");
    Check(luaL_dostring(state, R"lua(
        return view.column({
            key = "visuals",
            overflow = "clip",
            shadow = {
                color = 0x102030,
                blur = 10,
                offsetX = 2,
                offsetY = 5,
                alpha = 0.4,
            },
            children = {
                view.image({
                    key = "mask",
                    source = statusMask,
                    alt = "Status",
                    tint = 0x44AAEE,
                }),
            },
        })
    )lua") == LUA_OK,
        "overflow, shadow, and image-tint fixture must evaluate");
    ViewNode root;
    std::string error;
    Check(ParseLuaViewTree(state, -1, root, error) &&
            root.overflow == ViewOverflow::Clip && root.clipChildren &&
            root.shadow && root.shadow->color == 0x102030 &&
            Near(root.shadow->blur, 10.0f) &&
            Near(root.shadow->offsetX, 2.0f) &&
            Near(root.shadow->offsetY, 5.0f) &&
            Near(root.shadow->alpha, 0.4f) &&
            root.children.size() == 1 &&
            root.children[0].imageTint == 0x44AAEEu,
        "visual properties must retain bounded typed values");
    Check(ValidateAndLayoutViewTree(root, 100.0f, 80.0f, error) &&
            root.clipFrame && Near(root.clipFrame->width, 100.0f) &&
            Near(root.clipFrame->height, 80.0f),
        "overflow=clip must produce the same content clip used by layout and hit testing");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.box({ key = "conflict", clip = true,
            overflow = "visible" })
    )lua") == LUA_OK,
        "conflicting clipping fixture must evaluate");
    ViewNode invalid;
    Check(!ParseLuaViewTree(state, -1, invalid, error) &&
            error.find("same clipping behavior") != std::string::npos,
        "clip compatibility syntax must not contradict overflow");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.text({ key = "leaf", text = "Leaf",
            overflow = "clip" })
    )lua") == LUA_OK,
        "leaf overflow fixture must evaluate");
    invalid = {};
    Check(ParseLuaViewTree(state, -1, invalid, error) &&
            !ValidateAndLayoutViewTree(invalid, 100.0f, 40.0f, error) &&
            error == "view clip is only valid for container nodes",
        "overflow clipping must reject leaf nodes just like clip=true");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.shape({ key = "bad-tint", tint = 0xFFFFFF })
    )lua") == LUA_OK,
        "misapplied image tint fixture must evaluate");
    invalid = {};
    Check(!ParseLuaViewTree(state, -1, invalid, error) &&
            error.find("tint") != std::string::npos,
        "tint must remain scoped to package image nodes");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.box({ key = "bad-shadow",
            shadow = { blur = 65 } })
    )lua") == LUA_OK,
        "out-of-range shadow fixture must evaluate");
    invalid = {};
    Check(ParseLuaViewTree(state, -1, invalid, error) &&
            !ValidateAndLayoutViewTree(invalid, 100.0f, 40.0f, error) &&
            error == "view shadow values must be finite and bounded",
        "shadow values must be rejected outside host rendering bounds");
    lua_close(state);
}

void TestStackPositioningAndClipping()
{
    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be available");
    luaL_openlibs(state);
    RegisterViewLibrary(state);
    Check(luaL_dostring(state, R"lua(
        return view.stack({
            key = "overlay",
            padding = 10,
            clip = true,
            children = {
                view.button({ key = "back", label = "Back",
                    width = 60, height = 40, zIndex = -1,
                    offset = { x = -5, y = 2 },
                    action = { id = "back.open" } }),
                view.button({ key = "front", label = "Front",
                    width = 60, height = 40, zIndex = 2,
                    offset = { x = 20, y = 15 },
                    action = { id = "front.open" } }),
            },
        })
    )lua") == LUA_OK,
        "stack positioning Lua fixture must evaluate");
    ViewNode root;
    std::string error;
    Check(ParseLuaViewTree(state, -1, root, error) &&
            root.clipChildren && root.children.size() == 2 &&
            Near(root.children[0].offsetX, -5.0f) &&
            Near(root.children[0].offsetY, 2.0f) &&
            root.children[0].zIndex == -1 &&
            root.children[1].zIndex == 2,
        "Lua parsing must retain bounded stack offsets, zIndex, and clipping");
    Check(ValidateAndLayoutViewTree(root, 100.0f, 80.0f, error) &&
            root.clipFrame == ViewRect{ 10, 10, 80, 60 } &&
            root.children[0].frame == ViewRect{ 5, 12, 60, 40 } &&
            root.children[1].frame == ViewRect{ 30, 25, 60, 40 },
        "stack offsets must translate visuals without consuming layout space");
    const auto paintOrder = ViewChildrenInPaintOrder(root);
    Check(paintOrder.size() == 2 && paintOrder[0]->key == "back" &&
            paintOrder[1]->key == "front",
        "stack paint order must be stable and sorted by zIndex");
    std::vector<InteractionRegion> regions;
    Check(CollectViewInteractionRegions(root, regions, error) &&
            regions.size() == 2 && regions[0].key == "back" &&
            regions[1].key == "front" && regions[0].clip &&
            Near(regions[0].clip->x, 10.0f) &&
            Near(regions[0].clip->y, 10.0f) &&
            Near(regions[0].clip->width, 80.0f) &&
            Near(regions[0].clip->height, 60.0f),
        "paint order and explicit clips must also govern hit regions");
    WidgetInteractionRegions interaction;
    interaction.BeginFrame();
    for (auto& region : regions)
        Check(interaction.Submit(std::move(region), error),
            "stack region must submit");
    interaction.CommitFrame();
    Check(interaction.TargetAt(40.0f, 30.0f) == "front" &&
            interaction.TargetAt(6.0f, 20.0f).empty(),
        "zIndex must choose the top visual while clip excludes overflow hits");

    ViewNode row;
    row.type = ViewNodeType::Row;
    row.key = "row";
    ViewNode positioned;
    positioned.type = ViewNodeType::Shape;
    positioned.key = "positioned";
    positioned.offsetX = 1.0f;
    row.children.push_back(positioned);
    Check(!ValidateAndLayoutViewTree(row, 100.0f, 40.0f, error) &&
            error ==
                "view offset and zIndex are only valid for direct stack children",
        "offset must not create arbitrary visual order outside stack");

    ViewNode clippedLeaf;
    clippedLeaf.type = ViewNodeType::Shape;
    clippedLeaf.key = "clipped-leaf";
    clippedLeaf.clipChildren = true;
    Check(!ValidateAndLayoutViewTree(
            clippedLeaf, 40.0f, 40.0f, error) &&
            error == "view clip is only valid for container nodes",
        "clip must reject nodes that cannot have descendants");
    lua_close(state);
}

void TestTextLocaleValidation()
{
    ViewNode valid;
    valid.type = ViewNodeType::Text;
    valid.key = "localized";
    valid.text = "مرحبا";
    valid.locale = "ar-Arab-SA";
    valid.textDirection = ViewTextDirection::Auto;
    std::string error;
    Check(ValidateAndLayoutViewTree(valid, 120.0f, 40.0f, error),
        "bounded BCP 47 locale tags must validate");

    ViewNode invalid = valid;
    invalid.key = "invalid-locale";
    invalid.locale = "ar_SA";
    Check(!ValidateAndLayoutViewTree(invalid, 120.0f, 40.0f, error) &&
            error ==
                "view locale must be an empty or bounded BCP 47 language tag",
        "locale tags with unsupported separators must reject the tree");

    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be available");
    luaL_openlibs(state);
    RegisterViewLibrary(state);
    Check(luaL_dostring(state, R"lua(
        return view.text({ key = "direction", text = "text",
            textDirection = "sideways" })
    )lua") == LUA_OK,
        "invalid direction Lua fixture must evaluate");
    ViewNode parsed;
    Check(!ParseLuaViewTree(state, -1, parsed, error) &&
            error == "view textDirection must be auto, ltr, or rtl",
        "unknown text directions must fail during atomic tree parsing");
    lua_close(state);
}

void TestBasicTransforms()
{
    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be available");
    luaL_openlibs(state);
    RegisterViewLibrary(state);
    Check(luaL_dostring(state, R"lua(
        return view.button({
            key = "moved", label = "Move", width = 20, height = 10,
            transform = {
                translateX = 4, translateY = -2, scale = 1.5,
                originX = 0, originY = 1,
            },
            action = { id = "move" },
        })
    )lua") == LUA_OK,
        "basic transform Lua fixture must evaluate");
    ViewNode parsed;
    std::string error;
    Check(ParseLuaViewTree(state, -1, parsed, error) && parsed.transform &&
            Near(parsed.transform->translateX, 4.0f) &&
            Near(parsed.transform->translateY, -2.0f) &&
            Near(parsed.transform->scale, 1.5f) &&
            Near(parsed.transform->originX, 0.0f) &&
            Near(parsed.transform->originY, 1.0f),
        "Lua parsing must retain bounded node transform fields");

    ViewNode invalid = parsed;
    invalid.transform->scale = 0.0f;
    Check(!ValidateAndLayoutViewTree(invalid, 100.0f, 40.0f, error) &&
            error.find("transform") != std::string::npos,
        "non-positive node scales must reject the whole view commit");
    invalid = parsed;
    invalid.transform->originX = 1.1f;
    Check(!ValidateAndLayoutViewTree(invalid, 100.0f, 40.0f, error) &&
            error.find("transform") != std::string::npos,
        "transform origins outside normalized bounds must reject the tree");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.text({ key = "invalid", text = "Invalid",
            transform = { rotate = 45 } })
    )lua") == LUA_OK,
        "unknown transform field fixture must evaluate");
    Check(!ParseLuaViewTree(state, -1, parsed, error) &&
            error.find("rotate") != std::string::npos,
        "unpublished transform fields must fail atomic tree parsing");
    lua_close(state);

    ViewNode root;
    root.type = ViewNodeType::Stack;
    root.key = "root";
    root.clipChildren = true;
    root.overflow = ViewOverflow::Clip;
    root.transform = ViewTransform{
        10.0f, 5.0f, 2.0f, 0.0f, 0.0f };

    ViewNode button;
    button.type = ViewNodeType::Button;
    button.key = "button";
    button.text = "Button";
    button.width = { ViewLengthKind::Fixed, 20.0f };
    button.height = { ViewLengthKind::Fixed, 10.0f };
    button.offsetX = 120.0f;
    button.events["click"].id = "button.click";
    button.transform = ViewTransform{
        -115.0f, 1.0f, 0.5f, 0.0f, 0.0f };

    ViewNode input;
    input.type = ViewNodeType::TextInput;
    input.key = "input";
    input.inputValue = "value";
    input.accessibilityLabel = "Input";
    input.width = { ViewLengthKind::Fixed, 40.0f };
    input.height = { ViewLengthKind::Fixed, 20.0f };
    input.events["change"].id = "input.change";
    input.transform = ViewTransform{
        5.0f, 1.0f, 0.5f, 0.0f, 0.0f };
    root.children = { button, input };

    Check(ValidateAndLayoutViewTree(root, 100.0f, 80.0f, error),
        "nested bounded transforms must validate after layout");
    std::vector<InteractionRegion> regions;
    Check(CollectViewInteractionRegions(root, regions, error) &&
            regions.size() == 2 &&
            Near(regions[0].shape.x, 20.0f) &&
            Near(regions[0].shape.y, 7.0f) &&
            Near(regions[0].shape.width, 20.0f) &&
            Near(regions[0].shape.height, 10.0f) &&
            regions[0].clip && Near(regions[0].clip->x, 10.0f) &&
            Near(regions[0].clip->y, 5.0f) &&
            Near(regions[0].clip->width, 200.0f) &&
            Near(regions[0].clip->height, 160.0f),
        "transforms must move an initially clipped hit shape and its ancestor clip together");

    std::vector<ViewInputControl> controls;
    Check(CollectViewInputControls(root, controls, error) &&
            controls.size() == 1 && controls[0].key == "input" &&
            Near(controls[0].frame.x, 20.0f) &&
            Near(controls[0].frame.y, 7.0f) &&
            Near(controls[0].frame.width, 40.0f) &&
            Near(controls[0].frame.height, 20.0f) &&
            Near(controls[0].fontSize, 15.0f) && controls[0].clip &&
            Near(controls[0].clip->width, 200.0f),
        "host input geometry and typography must follow cumulative transforms");
}

void TestVisibilityStates()
{
    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be available");
    luaL_openlibs(state);
    RegisterViewLibrary(state);
    Check(luaL_dostring(state, R"lua(
        return view.row({
            key = "visibility-row",
            children = {
                view.button({
                    key = "reserved", label = "Reserved", width = 40,
                    visibility = "hidden",
                    action = { id = "reserved.click" },
                }),
                view.button({
                    key = "active", label = "Active", width = 40,
                    action = { id = "active.click" },
                }),
            },
        })
    )lua") == LUA_OK,
        "hidden visibility fixture must evaluate");
    ViewNode hidden;
    std::string error;
    Check(ParseLuaViewTree(state, -1, hidden, error) &&
            hidden.children[0].visibility == ViewVisibility::Hidden &&
            hidden.children[0].visible &&
            ValidateAndLayoutViewTree(hidden, 100.0f, 40.0f, error) &&
            Near(hidden.children[0].frame.x, 0.0f) &&
            Near(hidden.children[1].frame.x, 40.0f),
        "hidden nodes must reserve normal layout space");
    std::vector<InteractionRegion> regions;
    Check(CollectViewInteractionRegions(hidden, regions, error) &&
            regions.size() == 1 && regions[0].key == "active" &&
            Near(regions[0].shape.x, 40.0f),
        "hidden subtrees must not retain pointer or keyboard targets");

    ViewNode collapsed = hidden;
    collapsed.children[0].visible = false;
    collapsed.children[0].visibility = ViewVisibility::Collapsed;
    Check(ValidateAndLayoutViewTree(collapsed, 100.0f, 40.0f, error) &&
            Near(collapsed.children[1].frame.x, 0.0f),
        "collapsed nodes must be removed from layout");

    lua_pop(state, 1);
    Check(luaL_dostring(state, R"lua(
        return view.text({ key = "conflict", text = "Conflict",
            visible = true, visibility = "hidden" })
    )lua") == LUA_OK,
        "visibility conflict fixture must evaluate");
    ViewNode invalid;
    Check(!ParseLuaViewTree(state, -1, invalid, error) &&
            error.find("conflicting") != std::string::npos,
        "legacy visible and explicit visibility must not contradict each other");
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
    TestActionControlParsing();
    TestUniformGridParsingAndLayout();
    TestFlowParsingAndLayout();
    TestScrollableCollections();
    TestListOrientation();
    TestVirtualizedCollections();
    TestCollectionContentStates();
    TestDeclarativeInputControls();
    TestStyledTextAndMonthCalendar();
    TestLogicalSlotSceneContract();
    TestBoundedSizeConstraints();
    TestUniformMargins();
    TestFlexSizing();
    TestFlexLayout();
    TestOverflowShadowAndImageTint();
    TestStackPositioningAndClipping();
    TestTextLocaleValidation();
    TestBasicTransforms();
    TestVisibilityStates();
    std::cout << "Widget view tree tests passed\n";
    return 0;
}

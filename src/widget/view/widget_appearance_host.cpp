#include "widget/runtime/widget_engine.h"
#include "common/utils.h"
#include "common/constants.h"
#include "theme/edge_light_codec.h"
#include "widget/view/widget_appearance_presets.h"
#include "widget/view/widget_outline.h"
#include "widgets/widget_chrome_rules.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

bool WidgetEngine::ReadBoolFlag(const std::wstring& packageId, const char* flag, bool defaultVal) const
{
    for (const auto& w : widgets_)
    {
        if (w.valid && Utf8ToWide(w.packageId) == packageId)
        {
            lua_State* state = w.state;
            if (!state) return defaultVal;
            snowdesktop::lua_runtime::StackGuard stackGuard(state);
            lua_rawgeti(state, LUA_REGISTRYINDEX, w.ref);
            if (!lua_istable(state, -1)) { lua_pop(state, 1); return defaultVal; }
            lua_getfield(state, -1, flag);
            bool result = lua_isnil(state, -1) ? defaultVal : (lua_toboolean(state, -1) != 0);
            lua_pop(state, 2);
            return result;
        }
    }
    return defaultVal;
}

snowdesktop::widget_runtime::WidgetOutlineGeometry WidgetEngine::ResolveWidgetOutline(
    const std::wstring& widgetId, ID2D1Factory* factory, D2D1_RECT_F frame)
{
    const int index = FindWidget(widgetId);
    if (index < 0) return {};
    return widgets_[index].outline.Resolve(factory, frame);
}

bool WidgetEngine::WidgetOutlineContains(
    const std::wstring& widgetId, ID2D1Factory* factory, const RECT& frame, POINT point)
{
    const auto outline = ResolveWidgetOutline(widgetId, factory, D2D1::RectF(
        static_cast<float>(frame.left), static_cast<float>(frame.top),
        static_cast<float>(frame.right), static_cast<float>(frame.bottom)));
    if (!outline.positioned) return true;
    BOOL inside = FALSE;
    return SUCCEEDED(outline.positioned->FillContainsPoint(D2D1::Point2F(
        static_cast<float>(point.x), static_cast<float>(point.y)), nullptr, &inside)) && inside;
}

bool WidgetEngine::ReadCustomAppearance(const std::wstring& widgetId,
    PersonalizationSettings& appearance) const
{
    const int index = FindWidget(widgetId);
    if (index < 0) return false;
    const auto selection = RuntimeGetStorageValue(widgetId, "__preset");
    const auto& widget = widgets_[index];
    const auto findPreset = [&](const auto& presets) -> const LuaWidgetManifest::SettingPreset* {
        const auto found = std::find_if(presets.begin(), presets.end(),
            [&](const auto& preset) { return preset.id == selection; });
        return found == presets.end() ? nullptr : &*found;
    };
    const auto* authored = findPreset(widget.manifest.presets);
    if (!authored) authored = findPreset(widget.scriptPresets);
    const int fallbackContentTheme = appearance.contentTheme;
    if (!ReadCustomColors(widgetId,
            appearance.widgetBgR, appearance.widgetBgG, appearance.widgetBgB, appearance.widgetAlpha,
            appearance.widgetBorderR, appearance.widgetBorderG, appearance.widgetBorderB, appearance.widgetBorderAlpha,
            appearance.widgetBorderWidth, appearance.widgetEdgeHighlightEnabled,
            appearance.widgetEdgeHighlightWidth, appearance.widgetEdgeHighlightStrength,
            appearance.gradientEndA, appearance.glassEnabled, appearance.acrylicEnabled,
            &appearance.panelGradient, &appearance.edgeLight, !authored)) return false;
    const auto contentTheme = RuntimeGetStorageValue(widgetId, "__contentTheme");
    if (contentTheme == "0" || contentTheme == "1") appearance.contentTheme = contentTheme == "1" ? 1 : 0;
    if (authored && !authored->values.contains("__contentTheme"))
        appearance.contentTheme = fallbackContentTheme;
    appearance = snowdesktop::widget_runtime::ResolveWidgetHostAppearancePreset(
        std::move(appearance), selection, authored ? &authored->values : nullptr);
    return true;
}

bool WidgetEngine::ReadCustomColors(const std::wstring& widgetId,
    float& bgR, float& bgG, float& bgB, float& alpha,
    float& borderR, float& borderG, float& borderB, float& borderAlpha,
    float& borderWidth, bool& edgeHighlightEnabled,
    float& edgeHighlightWidth, float& edgeHighlightStrength,
    float& gradientEndA,
    bool& glassEnabled, bool& acrylicEnabled,
    snowdesktop::PanelGradient* panelGradient,
    snowdesktop::EdgeLightSettings* edgeLight,
    bool includeStoredValues) const
{
    if (panelGradient) *panelGradient = {};
    if (edgeLight) *edgeLight = {};
    int idx = FindWidget(widgetId);
    if (idx < 0) return false;
    const auto& w = widgets_[idx];
    lua_State* state = w.state;
    if (!state) return false;
    snowdesktop::lua_runtime::StackGuard stackGuard(state);

    lua_rawgeti(state, LUA_REGISTRYINDEX, w.ref);
    if (!lua_istable(state, -1)) { lua_pop(state, 1); return false; }

    auto readHex = [&](const char* key, float& r, float& g, float& b,
                       int defaultValue) {
        lua_getfield(state, -1, key);
        int value = lua_isinteger(state, -1)
            ? static_cast<int>(lua_tointeger(state, -1))
            : defaultValue;
        lua_pop(state, 1);
        r = ((value >> 16) & 0xFF) / 255.0f;
        g = ((value >> 8) & 0xFF) / 255.0f;
        b = (value & 0xFF) / 255.0f;
    };
    auto readFloat = [&](const char* key, float& out, float defaultValue) {
        lua_getfield(state, -1, key);
        out = lua_isnumber(state, -1)
            ? static_cast<float>(lua_tonumber(state, -1))
            : defaultValue;
        lua_pop(state, 1);
    };
    auto readBool = [&](const char* key, bool& out, bool defaultValue) {
        lua_getfield(state, -1, key);
        out = lua_isnil(state, -1)
            ? defaultValue : (lua_toboolean(state, -1) != 0);
        lua_pop(state, 1);
    };

    readHex("bg", bgR, bgG, bgB, 0x151A21);
    readHex("border", borderR, borderG, borderB, 0xFFFFFF);
    readFloat("alpha", alpha, 0.36f);
    readFloat("borderAlpha", borderAlpha, alpha);
    readFloat("gradientEndA", gradientEndA, 0.0f);
    readBool("glassEnabled", glassEnabled, false);
    readBool("acrylicEnabled", acrylicEnabled, false);

    lua_getfield(state, -1, "borderStyle");
    const bool legacyBorderStyleDeclared = lua_isinteger(state, -1);
    const int legacyBorderStyle = legacyBorderStyleDeclared
        ? static_cast<int>(lua_tointeger(state, -1)) : 0;
    lua_pop(state, 1);
    lua_getfield(state, -1, "borderWidth");
    const bool borderWidthDeclared = lua_isnumber(state, -1);
    if (borderWidthDeclared)
        borderWidth = static_cast<float>(lua_tonumber(state, -1));
    lua_pop(state, 1);
    lua_getfield(state, -1, "edgeHighlightEnabled");
    const bool edgeHighlightEnabledDeclared = !lua_isnil(state, -1);
    if (edgeHighlightEnabledDeclared)
        edgeHighlightEnabled = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    lua_getfield(state, -1, "edgeHighlightWidth");
    const bool edgeHighlightWidthDeclared = lua_isnumber(state, -1);
    if (edgeHighlightWidthDeclared)
        edgeHighlightWidth = static_cast<float>(lua_tonumber(state, -1));
    lua_pop(state, 1);
    readFloat("edgeHighlightStrength", edgeHighlightStrength,
        kDefaultEdgeHighlightStrength);

    const auto readStorage = [&](const char* key) {
        return includeStoredValues ? RuntimeGetStorageValue(widgetId, key) : std::string{};
    };
    auto readStoredColor = [&](const char* key, float& r, float& g,
                               float& b) {
        const std::string value = readStorage(key);
        if (value.empty()) return;
        const int encoded = std::atoi(value.c_str());
        r = ((encoded >> 16) & 0xFF) / 255.0f;
        g = ((encoded >> 8) & 0xFF) / 255.0f;
        b = (encoded & 0xFF) / 255.0f;
    };
    auto readStoredFloat = [&](const char* key, float& out) {
        const std::string value = readStorage(key);
        if (!value.empty())
            out = static_cast<float>(std::atof(value.c_str()));
    };
    auto readStoredBool = [&](const char* key, bool& out) {
        const std::string value = readStorage(key);
        if (!value.empty())
            out = value == "1" || value == "true";
    };
    readStoredColor("bg", bgR, bgG, bgB);
    readStoredColor("border", borderR, borderG, borderB);
    readStoredFloat("alpha", alpha);
    readStoredFloat("borderAlpha", borderAlpha);
    readStoredFloat("gradientEndA", gradientEndA);
    readStoredBool("glassEnabled", glassEnabled);
    readStoredBool("acrylicEnabled", acrylicEnabled);
    if (panelGradient)
    {
        JsonValue value;
        if (ParseJson(readStorage("__panelGradient"), value))
            (void)snowdesktop::DecodePanelGradient(value, *panelGradient);
    }

    if (edgeLight)
    {
        JsonValue value;
        if (ParseJson(readStorage("__edgeLight"), value))
            (void)snowdesktop::DecodeEdgeLight(value, *edgeLight);
    }
    const std::string storedLegacyBorderStyle =
        readStorage("borderStyle");
    const std::string storedBorderWidth =
        readStorage("borderWidth");
    if (!storedBorderWidth.empty())
    {
        borderWidth = static_cast<float>(
            std::atof(storedBorderWidth.c_str()));
    }
    else if (!borderWidthDeclared)
        borderWidth = 1.0f;
    const std::string storedEdgeHighlightEnabled =
        readStorage("edgeHighlightEnabled");
    if (!storedEdgeHighlightEnabled.empty())
        edgeHighlightEnabled = storedEdgeHighlightEnabled == "1" ||
            storedEdgeHighlightEnabled == "true";
    else if (!edgeHighlightEnabledDeclared)
        edgeHighlightEnabled = !storedLegacyBorderStyle.empty()
            ? std::atoi(storedLegacyBorderStyle.c_str()) == 1
            : legacyBorderStyleDeclared
                ? legacyBorderStyle == 1 : glassEnabled;
    const std::string storedEdgeHighlightWidth =
        readStorage("edgeHighlightWidth");
    if (!storedEdgeHighlightWidth.empty())
        edgeHighlightWidth = static_cast<float>(
            std::atof(storedEdgeHighlightWidth.c_str()));
    else if (!edgeHighlightWidthDeclared)
        edgeHighlightWidth = ((!storedLegacyBorderStyle.empty() &&
                std::atoi(storedLegacyBorderStyle.c_str()) == 1) ||
            (storedLegacyBorderStyle.empty() && legacyBorderStyle == 1)) &&
            (!storedBorderWidth.empty() || borderWidthDeclared)
            ? borderWidth : kDefaultEdgeHighlightWidth;
    const std::string storedEdgeHighlightStrength =
        readStorage("edgeHighlightStrength");
    if (!storedEdgeHighlightStrength.empty())
        edgeHighlightStrength = static_cast<float>(
            std::atof(storedEdgeHighlightStrength.c_str()));
    else
        readStoredFloat("borderEffectStrength", edgeHighlightStrength);
    if (!std::isfinite(borderWidth))
        borderWidth = 1.0f;
    if (!std::isfinite(edgeHighlightWidth))
        edgeHighlightWidth = kDefaultEdgeHighlightWidth;
    if (!std::isfinite(edgeHighlightStrength))
        edgeHighlightStrength = kDefaultEdgeHighlightStrength;
    borderWidth = std::clamp(borderWidth,
        kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth);
    edgeHighlightWidth = std::clamp(edgeHighlightWidth,
        kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth);
    edgeHighlightStrength = std::clamp(
        edgeHighlightStrength, 0.0f, 1.0f);
    if (storedEdgeHighlightEnabled.empty() &&
        !edgeHighlightEnabledDeclared && glassEnabled)
        borderAlpha = 0.0f;

    lua_pop(state, 1);
    return true;
}

// ── Check if widget uses custom style ────────────────────────────
bool WidgetEngine::HasCustomStyle(const std::wstring& widgetId) const
{
    int idx = FindWidget(widgetId);
    return idx >= 0 && widgets_[idx].customStyle;
}

bool WidgetEngine::HasBackgroundLayer(const std::wstring& widgetId) const
{
    const int index = FindWidget(widgetId);
    return index >= 0 && widgets_[index].hasBackgroundLayer;
}

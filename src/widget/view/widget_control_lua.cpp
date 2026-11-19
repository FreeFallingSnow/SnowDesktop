#include "widget_control_lua.h"

#include "common/utils.h"
#include "widget/runtime/widget_engine.h"
#include "widget/runtime/widget_lua_binding_context.h"
#include "widget_draw_lua.h"
#include "widget/view/widget_interaction_lua_value.h"
#include "widget_text_input_rules.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string_view>
#include <utility>

extern "C" {
#include <lauxlib.h>
}

namespace snowdesktop::widget_runtime
{
static bool IsValidUtf8Local(const std::string& value)
{
    if (value.empty()) return false;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0) > 0;
}

static bool HostTextIsWhitespaceOnly(const std::wstring& text)
{
    return std::all_of(text.begin(), text.end(), [](wchar_t ch) {
        return ch == L' ' || ch == L'\t' || ch == L'\r' ||
            ch == L'\n' || ch == L'\v' || ch == L'\f';
    });
}

static snowdesktop::widget_runtime::InteractionAction ReadControlSubmitAction(
    lua_State* state, int descriptor, const char* api);

int lua_UiTextInput(lua_State* L)
{
    const char* id = luaL_checkstring(L, 1);
    const char* storageKey = luaL_checkstring(L, 2);
    float x = static_cast<float>(luaL_checknumber(L, 3));
    float y = static_cast<float>(luaL_checknumber(L, 4));
    float width = static_cast<float>(luaL_checknumber(L, 5));
    float height = static_cast<float>(luaL_checknumber(L, 6));
    const int options = lua_istable(L, 7) ? lua_absindex(L, 7) : 0;

    auto numberOption = [&](const char* key, double fallback) {
        if (!options) return fallback;
        lua_getfield(L, options, key);
        double result = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : fallback;
        lua_pop(L, 1);
        return result;
    };
    auto integerOption = [&](const char* key, int fallback) {
        return static_cast<int>(numberOption(key, fallback));
    };
    auto boolOption = [&](const char* key, bool fallback) {
        if (!options) return fallback;
        lua_getfield(L, options, key);
        bool result = lua_isnil(L, -1) ? fallback : lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return result;
    };
    auto stringOption = [&](const char* key, const char* fallback) {
        if (!options) return std::string(fallback);
        lua_getfield(L, options, key);
        std::string result = lua_isstring(L, -1) ? lua_tostring(L, -1) : fallback;
        lua_pop(L, 1);
        return result;
    };

    const std::string placeholder = stringOption("placeholder", "");
    const float fontSize = static_cast<float>(
        std::clamp(numberOption("fontSize", 15.0), 9.0, 96.0));
    const int textColor = integerOption("textColor", 0xFFFFFF);
    const int placeholderColor = integerOption("placeholderColor", 0x94A3B8);
    const int backgroundColor = integerOption("backgroundColor", 0xFFFFFF);
    const int borderColor = integerOption("borderColor", 0xFFFFFF);
    const int focusedBorderColor = integerOption("focusedBorderColor", 0x64A8FF);
    const float backgroundAlpha = std::clamp(
        static_cast<float>(numberOption("backgroundAlpha", 0.05)), 0.0f, 1.0f);
    const float focusedBackgroundAlpha = std::clamp(
        static_cast<float>(numberOption("focusedBackgroundAlpha", 0.12)), 0.0f, 1.0f);
    const float borderAlpha = std::clamp(
        static_cast<float>(numberOption("borderAlpha", 0.12)), 0.0f, 1.0f);
    const float focusedBorderAlpha = std::clamp(
        static_cast<float>(numberOption("focusedBorderAlpha", 0.70)), 0.0f, 1.0f);
    const float radius = std::max(0.0f, static_cast<float>(numberOption("radius", 6.0)));
    const float padding = std::max(0.0f, static_cast<float>(numberOption("padding", 10.0)));
    const float borderThickness = std::max(0.5f,
        static_cast<float>(numberOption("borderThickness", 1.0)));
    const bool selectAll = boolOption("selectAll", false);
    const bool liveUpdate = boolOption("liveUpdate", true);
    const int requestedMaxBytes = integerOption("maxBytes", 0);
    const std::size_t maximumUtf8Bytes = requestedMaxBytes > 0
        ? static_cast<std::size_t>(std::min(requestedMaxBytes, 64 * 1024))
        : 0;

    auto* s = GetLuaDrawContext(L);
    auto* host = BoundWidgetEngine(L);
    std::string value;
    if (host && storageKey && *storageKey)
        value = host->RuntimeGetStorageValue(BoundWidgetId(L), storageKey);

    bool focused = false;
    size_t cursor = 0;
    size_t selectionAnchor = 0;
    std::wstring focusedText;
    std::wstring compositionText;
    size_t compositionCursor = 0;
    if (host && id && *id)
    {
        focused = host->RuntimeGetFocusedHostInput(
            BoundWidgetId(L), id, focusedText, cursor,
            selectionAnchor, compositionText,
            compositionCursor);
        if (focused)
            value = WideToUtf8(focusedText);
    }
    const bool widgetSelected = host &&
        host->RuntimeIsWidgetSelected(BoundWidgetId(L));
    const bool drawFocusedBorder = focused && !widgetSelected;
    const HostInputDisplayText focusedDisplay =
        BuildHostInputDisplayText(focusedText, cursor,
            selectionAnchor, compositionText,
            compositionCursor);

    DrawHostRect(s, x, y, width, height, backgroundColor, radius,
        focused ? focusedBackgroundAlpha : backgroundAlpha);
    DrawHostStrokeRect(s, x, y, width, height,
        drawFocusedBorder ? focusedBorderColor : borderColor, radius,
        borderThickness,
        drawFocusedBorder ? focusedBorderAlpha : borderAlpha);
    const bool showingPlaceholder = value.empty() && !focused;
    const std::wstring displayText = focused
        ? focusedDisplay.text
        : Utf8ToWide(showingPlaceholder ? placeholder : value);
    const float innerWidth =
        std::max(1.0f, width - padding * 2.0f);
    ComPtr<IDWriteTextLayout> textLayout =
        CreateHostSingleLineTextLayout(s, displayText,
            fontSize, innerWidth, height);
    if (s && s->ctx)
    {
        const D2D1_RECT_F clip = D2D1::RectF(
            s->widgetRect.left + x + padding,
            s->widgetRect.top + y,
            s->widgetRect.left + x + width - padding,
            s->widgetRect.top + y + height);
        s->ctx->PushAxisAlignedClip(
            clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (focused && compositionText.empty() &&
            selectionAnchor != cursor &&
            textLayout)
        {
            DrawHostTextSelection(s, textLayout.Get(),
                std::min(selectionAnchor, cursor),
                std::max(selectionAnchor, cursor),
                x + padding, y, focusedBorderColor);
        }
        ID2D1SolidColorBrush* textBrush = GetCachedBrush(
            s, showingPlaceholder
                ? placeholderColor : textColor);
        if (textBrush && textLayout)
        {
            s->ctx->DrawTextLayout(
                D2D1::Point2F(
                    s->widgetRect.left + x + padding,
                    s->widgetRect.top + y),
                textLayout.Get(), textBrush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (focused && focusedDisplay.compositionLength > 0 &&
            textLayout)
        {
            DrawHostCompositionUnderline(s, textLayout.Get(),
                focusedDisplay.compositionStart,
                focusedDisplay.compositionLength,
                x + padding, y, textColor);
        }

        if (focused && textLayout)
        {
            const size_t safeCursor =
                std::min(focusedDisplay.cursor,
                    focusedDisplay.text.size());
            UINT32 hitPosition = 0;
            BOOL trailing = FALSE;
            if (!focusedDisplay.text.empty())
            {
                if (safeCursor >= focusedDisplay.text.size())
                {
                    hitPosition = static_cast<UINT32>(
                        focusedDisplay.text.size() - 1);
                    trailing = TRUE;
                }
                else
                    hitPosition =
                        static_cast<UINT32>(safeCursor);
            }
            float caretX = 0.0f;
            float caretY = 0.0f;
            DWRITE_HIT_TEST_METRICS caretMetrics{};
            if (SUCCEEDED(textLayout->HitTestTextPosition(
                hitPosition, trailing, &caretX, &caretY,
                &caretMetrics)))
            {
                DrawHostRect(s, x + padding + caretX,
                    y + caretY, 1.5f,
                    std::max(caretMetrics.height, fontSize),
                    textColor, 0.0f, 0.98f);
            }
        }
        s->ctx->PopAxisAlignedClip();
    }
    if (host && id && *id && storageKey && *storageKey)
    {
        LuaWidget::HostControl control;
        control.type = LuaWidget::HostControl::Type::Input;
        control.id = id;
        control.storageKey = storageKey;
        control.rect = { static_cast<LONG>(std::lround(x)), static_cast<LONG>(std::lround(y)),
            static_cast<LONG>(std::lround(x + width)), static_cast<LONG>(std::lround(y + height)) };
        control.selectAll = selectAll;
        control.liveUpdate = liveUpdate;
        control.placeholder = placeholder;
        control.fontSize = fontSize;
        control.padding = padding;
        control.maximumUtf8Bytes = maximumUtf8Bytes;
        control.submitAction = ReadControlSubmitAction(L, options, "control.textInput");
        std::string error;
        if (!host->RuntimeRegisterV2HostControl(
                BoundWidgetId(L), std::move(control), error))
        {
            return luaL_error(L, "control.textInput: %s",
                error.c_str());
        }
    }

    lua_pushlstring(L, value.data(), value.size());
    return 1;
}

int lua_UiTextArea(lua_State* L)
{
    const char* id = luaL_checkstring(L, 1);
    const char* storageKey = luaL_checkstring(L, 2);
    const float x = static_cast<float>(luaL_checknumber(L, 3));
    const float y = static_cast<float>(luaL_checknumber(L, 4));
    const float width = static_cast<float>(luaL_checknumber(L, 5));
    const float height = static_cast<float>(luaL_checknumber(L, 6));
    const int options = lua_istable(L, 7) ? lua_absindex(L, 7) : 0;

    auto numberOption = [&](const char* key, double fallback) {
        if (!options) return fallback;
        lua_getfield(L, options, key);
        const double result = lua_isnumber(L, -1)
            ? lua_tonumber(L, -1) : fallback;
        lua_pop(L, 1);
        return result;
    };
    auto integerOption = [&](const char* key, int fallback) {
        return static_cast<int>(numberOption(key, fallback));
    };
    auto boolOption = [&](const char* key, bool fallback) {
        if (!options) return fallback;
        lua_getfield(L, options, key);
        const bool result = lua_isnil(L, -1)
            ? fallback : lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return result;
    };
    auto stringOption = [&](const char* key, const char* fallback) {
        if (!options) return std::string(fallback);
        lua_getfield(L, options, key);
        const std::string result = lua_isstring(L, -1)
            ? lua_tostring(L, -1) : fallback;
        lua_pop(L, 1);
        return result;
    };

    const std::string placeholder = stringOption("placeholder", "");
    const float fontSize = static_cast<float>(
        std::clamp(numberOption("fontSize", 15.0), 9.0, 96.0));
    const int textColor = integerOption("textColor", 0xFFFFFF);
    const int placeholderColor =
        integerOption("placeholderColor", 0x94A3B8);
    const int backgroundColor =
        integerOption("backgroundColor", 0xFFFFFF);
    const int borderColor = integerOption("borderColor", 0xFFFFFF);
    const int focusedBorderColor =
        integerOption("focusedBorderColor", 0x64A8FF);
    const float backgroundAlpha = std::clamp(static_cast<float>(
        numberOption("backgroundAlpha", 0.05)), 0.0f, 1.0f);
    const float focusedBackgroundAlpha = std::clamp(static_cast<float>(
        numberOption("focusedBackgroundAlpha", 0.08)), 0.0f, 1.0f);
    const float borderAlpha = std::clamp(static_cast<float>(
        numberOption("borderAlpha", 0.0)), 0.0f, 1.0f);
    const float focusedBorderAlpha = std::clamp(static_cast<float>(
        numberOption("focusedBorderAlpha", 0.35)), 0.0f, 1.0f);
    const float radius = std::max(0.0f,
        static_cast<float>(numberOption("radius", 6.0)));
    const float padding = std::max(0.0f,
        static_cast<float>(numberOption("padding", 8.0)));
    const float borderThickness = std::max(0.5f,
        static_cast<float>(numberOption("borderThickness", 1.0)));
    const bool selectAll = boolOption("selectAll", false);
    const bool liveUpdate = boolOption("liveUpdate", true);
    const bool placeholderWhenWhitespace =
        boolOption("placeholderWhenWhitespace", false);
    const int requestedMaxBytes = integerOption("maxBytes", 0);
    const std::size_t maximumUtf8Bytes = requestedMaxBytes > 0
        ? static_cast<std::size_t>(std::min(requestedMaxBytes, 64 * 1024))
        : 0;

    auto* s = GetLuaDrawContext(L);
    auto* host = BoundWidgetEngine(L);
    std::string value;
    if (host && storageKey && *storageKey)
        value = host->RuntimeGetStorageValue(
            BoundWidgetId(L), storageKey);

    bool focused = false;
    size_t cursor = 0;
    size_t selectionAnchor = 0;
    std::wstring focusedText;
    std::wstring compositionText;
    size_t compositionCursor = 0;
    if (host && id && *id)
    {
        focused = host->RuntimeGetFocusedHostInput(
            BoundWidgetId(L), id, focusedText, cursor,
            selectionAnchor, compositionText,
            compositionCursor);
        if (focused)
            value = WideToUtf8(focusedText);
    }
    const bool widgetSelected = host &&
        host->RuntimeIsWidgetSelected(BoundWidgetId(L));
    const bool drawFocusedBorder = focused && !widgetSelected;
    const HostInputDisplayText focusedDisplay =
        BuildHostInputDisplayText(focusedText, cursor,
            selectionAnchor, compositionText,
            compositionCursor);

    DrawHostRect(s, x, y, width, height, backgroundColor, radius,
        focused ? focusedBackgroundAlpha : backgroundAlpha);
    DrawHostStrokeRect(s, x, y, width, height,
        drawFocusedBorder ? focusedBorderColor : borderColor, radius,
        borderThickness,
        drawFocusedBorder ? focusedBorderAlpha : borderAlpha);

    const std::wstring valueText = focused
        ? focusedDisplay.text : Utf8ToWide(value);
    const bool semanticallyEmpty = valueText.empty() ||
        (placeholderWhenWhitespace &&
            HostTextIsWhitespaceOnly(valueText));
    const bool showingPlaceholder = semanticallyEmpty && !focused;
    const std::wstring displayText = showingPlaceholder
        ? Utf8ToWide(placeholder) : valueText;
    const float scrollbarReserve = 8.0f;
    const float innerWidth = std::max(1.0f,
        width - padding * 2.0f - scrollbarReserve);
    ComPtr<IDWriteTextLayout> textLayout =
        CreateHostMultilineTextLayout(s, displayText, fontSize,
            innerWidth);
    DWRITE_TEXT_METRICS textMetrics{};
    if (textLayout)
        textLayout->GetMetrics(&textMetrics);
    const auto verticalExtents = snowdesktop::widget_runtime::
        ResolveHostInputVerticalExtents(
            height, textMetrics.height + padding * 2.0f);

    if (host && id && *id &&
        storageKey && *storageKey)
    {
        LuaWidget::HostControl control;
        control.type = LuaWidget::HostControl::Type::Input;
        control.id = id;
        control.storageKey = storageKey;
        control.rect = {
            static_cast<LONG>(std::lround(x)),
            static_cast<LONG>(std::lround(y)),
            static_cast<LONG>(std::lround(x + width)),
            static_cast<LONG>(std::lround(y + height))
        };
        control.selectAll = selectAll;
        control.liveUpdate = liveUpdate;
        control.multiline = true;
        control.placeholder = placeholder;
        control.fontSize = fontSize;
        control.padding = padding;
        control.contentHeight = verticalExtents.content;
        control.viewportHeight = verticalExtents.viewport;
        control.maximumUtf8Bytes = maximumUtf8Bytes;
        control.submitAction = ReadControlSubmitAction(L, options, "control.textArea");
        std::string error;
        if (!host->RuntimeRegisterV2HostControl(
                BoundWidgetId(L), std::move(control), error))
        {
            return luaL_error(L, "control.textArea: %s",
                error.c_str());
        }
    }

    int scrollOffset = host
        ? host->RuntimeGetScrollOffset(
            BoundWidgetId(L), id) : 0;
    ComPtr<IDWriteTextLayout> focusedLayout;
    DWRITE_HIT_TEST_METRICS caretMetrics{};
    float caretX = 0.0f;
    float caretY = 0.0f;
    bool hasCaret = false;
    if (focused)
    {
        focusedLayout = CreateHostMultilineTextLayout(s,
            focusedDisplay.text, fontSize, innerWidth);
        if (focusedLayout)
        {
            const size_t safeCursor =
                std::min(focusedDisplay.cursor,
                    focusedDisplay.text.size());
            UINT32 hitPosition = 0;
            BOOL trailing = FALSE;
            if (!focusedDisplay.text.empty())
            {
                if (safeCursor >= focusedDisplay.text.size() &&
                    (focusedDisplay.text.back() == L'\n' ||
                        focusedDisplay.text.back() == L'\r'))
                {
                    hitPosition = static_cast<UINT32>(
                        focusedDisplay.text.size());
                    trailing = FALSE;
                }
                else if (safeCursor >=
                    focusedDisplay.text.size())
                {
                    hitPosition = static_cast<UINT32>(
                        focusedDisplay.text.size() - 1);
                    trailing = TRUE;
                }
                else
                    hitPosition =
                        static_cast<UINT32>(safeCursor);
            }
            hasCaret = SUCCEEDED(
                focusedLayout->HitTestTextPosition(
                    hitPosition, trailing, &caretX, &caretY,
                    &caretMetrics));
            if (hasCaret && host &&
                host->RuntimeConsumeHostInputCaretVisibilityRequest(
                    BoundWidgetId(L), id))
            {
                const float caretTop = padding + caretY;
                const float caretBottom = caretTop +
                    std::max(caretMetrics.height, fontSize);
                const float visibleHeight =
                    std::max(1.0f, height - padding * 2.0f);
                int desiredOffset = scrollOffset;
                if (caretTop < scrollOffset)
                    desiredOffset =
                        static_cast<int>(std::floor(caretTop));
                else if (caretBottom >
                    scrollOffset + visibleHeight)
                    desiredOffset = static_cast<int>(std::ceil(
                        caretBottom - visibleHeight));
                host->RuntimeSetScrollOffset(
                    BoundWidgetId(L), id, desiredOffset);
                scrollOffset =
                    host->RuntimeGetScrollOffset(
                        BoundWidgetId(L), id);
            }
        }
    }

    if (s && s->ctx)
    {
        const D2D1_RECT_F clip = D2D1::RectF(
            s->widgetRect.left + x + padding,
            s->widgetRect.top + y + padding,
            s->widgetRect.left + x + width -
                padding - scrollbarReserve,
            s->widgetRect.top + y + height - padding);
        s->ctx->PushAxisAlignedClip(
            clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (focused && compositionText.empty() &&
            selectionAnchor != cursor &&
            focusedLayout)
        {
            DrawHostTextSelection(s, focusedLayout.Get(),
                std::min(selectionAnchor, cursor),
                std::max(selectionAnchor, cursor),
                x + padding, y + padding - scrollOffset,
                focusedBorderColor);
        }
        ID2D1SolidColorBrush* textBrush = GetCachedBrush(
            s, showingPlaceholder
                ? placeholderColor : textColor);
        IDWriteTextLayout* layoutToDraw = focused &&
            focusedLayout ? focusedLayout.Get() :
            textLayout.Get();
        if (textBrush && layoutToDraw)
        {
            s->ctx->DrawTextLayout(
                D2D1::Point2F(
                    s->widgetRect.left + x + padding,
                    s->widgetRect.top + y + padding -
                        scrollOffset),
                layoutToDraw, textBrush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (focused && focusedDisplay.compositionLength > 0 &&
            focusedLayout)
        {
            DrawHostCompositionUnderline(s,
                focusedLayout.Get(),
                focusedDisplay.compositionStart,
                focusedDisplay.compositionLength,
                x + padding, y + padding - scrollOffset,
                textColor);
        }
        if (focused && hasCaret)
        {
            const float cursorHeight = std::max(
                caretMetrics.height, fontSize);
            DrawHostRect(s,
                x + padding + caretX,
                y + padding + caretY - scrollOffset,
                1.5f, cursorHeight, textColor, 0.0f, 0.98f);
        }
        s->ctx->PopAxisAlignedClip();
    }

    lua_pushlstring(L, value.data(), value.size());
    return 1;
}

int lua_UiFocusInput(lua_State* L)
{
    const char* id = luaL_checkstring(L, 1);
    auto* host = BoundWidgetEngine(L);
    bool focused = host && id && *id &&
        host->RuntimeFocusHostInput(BoundWidgetId(L), id);
    lua_pushboolean(L, focused);
    return 1;
}

static void ValidateControlTableFields(lua_State* state, int table,
    std::span<const char* const> allowed, const char* api)
{
    table = lua_absindex(state, table);
    if (lua_getmetatable(state, table) != 0)
    {
        lua_pop(state, 1);
        luaL_error(state, "%s: descriptor tables cannot have metatables", api);
        return;
    }
    lua_pushnil(state);
    while (lua_next(state, table) != 0)
    {
        std::size_t length = 0;
        const char* key = lua_type(state, -2) == LUA_TSTRING
            ? lua_tolstring(state, -2, &length) : nullptr;
        const bool known = key && std::any_of(
            allowed.begin(), allowed.end(), [&](const char* candidate) {
                return std::string_view(key, length) == candidate;
            });
        if (!known)
        {
            const std::string field = key
                ? std::string(key, length) : "<non-string>";
            lua_pop(state, 2);
            luaL_error(state, "%s: unknown field '%s'", api,
                field.c_str());
            return;
        }
        lua_pop(state, 1);
    }
}

static std::string ReadControlIdentifier(lua_State* state, int table,
    const char* field, const char* api)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (lua_type(state, -1) != LUA_TSTRING)
    {
        luaL_error(state, "%s: %s must be a string", api, field);
        return {};
    }
    std::size_t length = 0;
    const char* value = lua_tolstring(state, -1, &length);
    const std::string result(value ? value : "", length);
    lua_pop(state, 1);
    if (result.empty() || result.size() > 128 ||
        result.find('\0') != std::string::npos ||
        !IsValidUtf8Local(result))
    {
        luaL_error(state,
            "%s: %s must contain 1 to 128 bytes of valid UTF-8",
            api, field);
        return {};
    }
    return result;
}

static snowdesktop::widget_runtime::InteractionAction ReadControlSubmitAction(
    lua_State* state, int descriptor, const char* api)
{
    snowdesktop::widget_runtime::InteractionAction action;
    if (!descriptor) return action;
    lua_getfield(state, lua_absindex(state, descriptor), "events");
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        return action;
    }
    luaL_checktype(state, -1, LUA_TTABLE);
    const int events = lua_absindex(state, -1);
    static constexpr const char* eventFields[] = { "submit" };
    ValidateControlTableFields(state, events, eventFields, api);
    lua_getfield(state, events, "submit");
    if (!lua_isnil(state, -1))
    {
        luaL_checktype(state, -1, LUA_TTABLE);
        const int binding = lua_absindex(state, -1);
        static constexpr const char* actionFields[] = { "id", "value" };
        ValidateControlTableFields(state, binding, actionFields, api);
        action.id = ReadControlIdentifier(state, binding, "id", api);
        lua_getfield(state, binding, "value");
        std::size_t nodes = 0;
        std::size_t bytes = 0;
        std::unordered_set<const void*> ancestors;
        std::string error;
        if (!ReadInteractionValue(state, -1, action.value, 0,
                nodes, bytes, ancestors, error))
            luaL_error(state, "%s: %s", api, error.c_str());
        lua_pop(state, 1);
    }
    lua_pop(state, 2);
    return action;
}

static double ReadControlNumber(lua_State* state, int table,
    const char* field, const char* api)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (lua_type(state, -1) != LUA_TNUMBER)
    {
        luaL_error(state, "%s: %s must be a number", api, field);
        return 0.0;
    }
    const double value = lua_tonumber(state, -1);
    lua_pop(state, 1);
    return value;
}

static void ValidateOptionalControlString(lua_State* state, int table,
    const char* field, std::size_t maximumBytes, const char* api)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        return;
    }
    if (lua_type(state, -1) != LUA_TSTRING)
    {
        luaL_error(state, "%s: %s must be a string", api, field);
        return;
    }
    std::size_t length = 0;
    const char* value = lua_tolstring(state, -1, &length);
    const std::string text(value ? value : "", length);
    lua_pop(state, 1);
    if (length > maximumBytes || text.find('\0') != std::string::npos ||
        (!text.empty() && !IsValidUtf8Local(text)))
    {
        luaL_error(state, "%s: %s must be at most %d bytes of valid UTF-8",
            api, field, static_cast<int>(maximumBytes));
    }
}

static void ValidateOptionalControlBoolean(lua_State* state, int table,
    const char* field, const char* api)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (!lua_isnil(state, -1) && lua_type(state, -1) != LUA_TBOOLEAN)
    {
        luaL_error(state, "%s: %s must be a boolean", api, field);
        return;
    }
    lua_pop(state, 1);
}

static void ValidateOptionalControlNumber(lua_State* state, int table,
    const char* field, double minimum, double maximum, const char* api,
    bool integer = false)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        return;
    }
    if (lua_type(state, -1) != LUA_TNUMBER ||
        (integer && !lua_isinteger(state, -1)))
    {
        luaL_error(state, "%s: %s must be %s", api, field,
            integer ? "an integer" : "a number");
        return;
    }
    const double value = lua_tonumber(state, -1);
    lua_pop(state, 1);
    if (!std::isfinite(value) || value < minimum || value > maximum)
    {
        luaL_error(state, "%s: %s is outside the supported range",
            api, field);
    }
}

static void ValidateOptionalControlFontSize(lua_State* state, int table,
    const char* api)
{
    lua_getfield(state, lua_absindex(state, table), "fontSize");
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        return;
    }
    if (lua_type(state, -1) != LUA_TNUMBER)
    {
        luaL_error(state, "%s: fontSize must be a number", api);
        return;
    }
    const double value = lua_tonumber(state, -1);
    lua_pop(state, 1);
    // Row, page, and user scaling can exceed the editor's rendering range.
    // The editor clamps valid sizes before converting them to float.
    if (!std::isfinite(value) || value <= 0.0)
        luaL_error(state, "%s: fontSize must be finite and positive", api);
}

static int LuaControlText(lua_State* state, bool multiline)
{
    const char* api = multiline
        ? "control.textArea" : "control.textInput";
    if (lua_gettop(state) != 1)
        return luaL_error(state, "%s: expected one descriptor", api);
    luaL_checktype(state, 1, LUA_TTABLE);
    const int descriptor = lua_absindex(state, 1);
    static constexpr const char* inputFields[] = {
        "key", "storageKey", "shape", "placeholder", "fontSize",
        "textColor", "placeholderColor", "backgroundColor",
        "borderColor", "focusedBorderColor", "backgroundAlpha",
        "focusedBackgroundAlpha", "borderAlpha", "focusedBorderAlpha",
        "radius", "padding", "borderThickness", "selectAll",
        "liveUpdate", "maxBytes", "events",
    };
    static constexpr const char* areaFields[] = {
        "key", "storageKey", "shape", "placeholder",
        "placeholderWhenWhitespace", "fontSize", "textColor",
        "placeholderColor", "backgroundColor", "borderColor",
        "focusedBorderColor", "backgroundAlpha",
        "focusedBackgroundAlpha", "borderAlpha", "focusedBorderAlpha",
        "radius", "padding", "borderThickness", "selectAll",
        "liveUpdate", "maxBytes", "events",
    };
    ValidateControlTableFields(state, descriptor,
        multiline ? std::span<const char* const>(areaFields)
                  : std::span<const char* const>(inputFields), api);
    const std::string key = ReadControlIdentifier(
        state, descriptor, "key", api);
    const std::string storageKey = ReadControlIdentifier(
        state, descriptor, "storageKey", api);
    (void)ReadControlSubmitAction(state, descriptor, api);

    lua_getfield(state, descriptor, "shape");
    if (lua_type(state, -1) != LUA_TTABLE)
        return luaL_error(state, "%s: shape must be a table", api);
    const int shape = lua_absindex(state, -1);
    static constexpr const char* shapeFields[] = {
        "type", "x", "y", "width", "height",
    };
    ValidateControlTableFields(state, shape, shapeFields, api);
    lua_getfield(state, shape, "type");
    if (lua_type(state, -1) != LUA_TSTRING ||
        std::string_view(lua_tostring(state, -1)) != "rect")
    {
        return luaL_error(state, "%s: shape type must be 'rect'", api);
    }
    lua_pop(state, 1);
    const double x = ReadControlNumber(state, shape, "x", api);
    const double y = ReadControlNumber(state, shape, "y", api);
    const double width = ReadControlNumber(state, shape, "width", api);
    const double height = ReadControlNumber(state, shape, "height", api);
    lua_pop(state, 1);
    if (!std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(width) || !std::isfinite(height) ||
        std::abs(x) > 1'000'000.0 || std::abs(y) > 1'000'000.0 ||
        width <= 0.0 || height <= 0.0 ||
        width > 1'000'000.0 || height > 1'000'000.0)
    {
        return luaL_error(state,
            "%s: shape geometry must be finite, positive, and bounded", api);
    }

    ValidateOptionalControlString(
        state, descriptor, "placeholder", 4096, api);
    ValidateOptionalControlFontSize(state, descriptor, api);
    for (const char* field : { "textColor", "placeholderColor",
        "backgroundColor", "borderColor", "focusedBorderColor" })
    {
        ValidateOptionalControlNumber(state, descriptor, field,
            0.0, 0xFFFFFF, api, true);
    }
    for (const char* field : { "backgroundAlpha",
        "focusedBackgroundAlpha", "borderAlpha",
        "focusedBorderAlpha" })
    {
        ValidateOptionalControlNumber(
            state, descriptor, field, 0.0, 1.0, api);
    }
    ValidateOptionalControlNumber(
        state, descriptor, "radius", 0.0, 4096.0, api);
    ValidateOptionalControlNumber(
        state, descriptor, "padding", 0.0, 4096.0, api);
    ValidateOptionalControlNumber(
        state, descriptor, "borderThickness", 0.5, 64.0, api);
    ValidateOptionalControlNumber(
        state, descriptor, "maxBytes", 1.0, 64 * 1024.0, api, true);
    ValidateOptionalControlBoolean(
        state, descriptor, "selectAll", api);
    ValidateOptionalControlBoolean(
        state, descriptor, "liveUpdate", api);
    if (multiline)
    {
        ValidateOptionalControlBoolean(state, descriptor,
            "placeholderWhenWhitespace", api);
    }

    lua_getfield(state, descriptor, "maxBytes");
    const bool hasMaxBytes = !lua_isnil(state, -1);
    lua_pop(state, 1);
    if (!hasMaxBytes)
    {
        lua_pushinteger(state, multiline ? 64 * 1024 : 4096);
        lua_setfield(state, descriptor, "maxBytes");
    }

    lua_pushlstring(state, key.data(), key.size());
    lua_pushlstring(state, storageKey.data(), storageKey.size());
    lua_pushnumber(state, x);
    lua_pushnumber(state, y);
    lua_pushnumber(state, width);
    lua_pushnumber(state, height);
    lua_pushvalue(state, descriptor);
    lua_remove(state, descriptor);
    return multiline
        ? lua_UiTextArea(state) : lua_UiTextInput(state);
}

int lua_ControlTextInput(lua_State* state)
{
    return LuaControlText(state, false);
}

int lua_ControlTextArea(lua_State* state)
{
    return LuaControlText(state, true);
}

int lua_ControlFocus(lua_State* state)
{
    if (lua_gettop(state) != 1)
        return luaL_error(state, "control.focus: expected one key");
    if (lua_type(state, 1) != LUA_TSTRING)
        return luaL_error(state, "control.focus: key must be a string");
    std::size_t length = 0;
    const char* raw = lua_tolstring(state, 1, &length);
    const std::string key(raw ? raw : "", length);
    if (key.empty() || key.size() > 128 ||
        key.find('\0') != std::string::npos ||
        !IsValidUtf8Local(key))
    {
        return luaL_error(state,
            "control.focus: key must contain 1 to 128 bytes of valid UTF-8");
    }
    auto* host = BoundWidgetEngine(state);
    std::string error = "hostUnavailable";
    const bool focused = host &&
        host->RuntimeFocusHostInputFromTrustedGesture(
            BoundWidgetId(state), key, error);
    lua_pushboolean(state, focused ? 1 : 0);
    if (focused)
        lua_pushnil(state);
    else
        lua_pushlstring(state, error.data(), error.size());
    return 2;
}

int lua_ControlBlur(lua_State* state)
{
    if (lua_gettop(state) != 1)
        return luaL_error(state, "control.blur: expected one key");
    if (lua_type(state, 1) != LUA_TSTRING)
        return luaL_error(state, "control.blur: key must be a string");
    std::size_t length = 0;
    const char* raw = lua_tolstring(state, 1, &length);
    const std::string key(raw ? raw : "", length);
    if (key.empty() || key.size() > 128 ||
        key.find('\0') != std::string::npos ||
        !IsValidUtf8Local(key))
    {
        return luaL_error(state,
            "control.blur: key must contain 1 to 128 bytes of valid UTF-8");
    }
    auto* host = BoundWidgetEngine(state);
    std::string error = "hostUnavailable";
    const bool blurred = host &&
        host->RuntimeBlurHostInputFromTrustedGesture(
            BoundWidgetId(state), key, error);
    lua_pushboolean(state, blurred ? 1 : 0);
    if (blurred)
        lua_pushnil(state);
    else
        lua_pushlstring(state, error.data(), error.size());
    return 2;
}

int lua_UiButton(lua_State* L)
{
    const char* id = luaL_checkstring(L, 1);
    const char* label = luaL_checkstring(L, 2);
    float x = static_cast<float>(luaL_checknumber(L, 3));
    float y = static_cast<float>(luaL_checknumber(L, 4));
    float width = static_cast<float>(luaL_checknumber(L, 5));
    float height = static_cast<float>(luaL_checknumber(L, 6));
    bool enabled = lua_isnoneornil(L, 7) || lua_toboolean(L, 7) != 0;
    auto* s = GetLuaDrawContext(L);
    auto* host = BoundWidgetEngine(L);
    DrawHostRect(s, x, y, width, height, enabled ? 0x3478D4 : 0x555B65, 6, enabled ? 0.95f : 0.55f);
    DrawHostText(s, Utf8ToWide(label), x, y, width, height, 13, 0xFFFFFF);
    if (enabled && host)
    {
        LuaWidget::HostControl control;
        control.type = LuaWidget::HostControl::Type::Button;
        control.id = id;
        control.rect = { static_cast<LONG>(x), static_cast<LONG>(y),
            static_cast<LONG>(x + width), static_cast<LONG>(y + height) };
        host->RuntimeRegisterHostControl(BoundWidgetId(L), std::move(control));
    }
    return 0;
}

int lua_UiToggle(lua_State* L)
{
    const char* id = luaL_checkstring(L, 1);
    const char* label = luaL_checkstring(L, 2);
    float x = static_cast<float>(luaL_checknumber(L, 3));
    float y = static_cast<float>(luaL_checknumber(L, 4));
    float width = static_cast<float>(luaL_checknumber(L, 5));
    float height = static_cast<float>(luaL_checknumber(L, 6));
    bool value = lua_toboolean(L, 7) != 0;
    auto* s = GetLuaDrawContext(L);
    auto* host = BoundWidgetEngine(L);
    DrawHostRect(s, x, y, width, height, 0x29323B, 6, 0.95f);
    DrawHostText(s, Utf8ToWide(label), x + 6, y, width - height - 8, height, 12, 0xFFFFFF);
    DrawHostRect(s, x + width - height + 3, y + 3, height - 6, height - 6,
        value ? 0x39B980 : 0x69717A, (height - 6) / 2, 1.0f);
    if (host)
    {
        LuaWidget::HostControl control;
        control.type = LuaWidget::HostControl::Type::Toggle;
        control.id = id;
        control.value = value;
        control.rect = { static_cast<LONG>(x), static_cast<LONG>(y),
            static_cast<LONG>(x + width), static_cast<LONG>(y + height) };
        host->RuntimeRegisterHostControl(BoundWidgetId(L), std::move(control));
    }
    return 0;
}

int lua_UiProgress(lua_State* L)
{
    float x = static_cast<float>(luaL_checknumber(L, 1));
    float y = static_cast<float>(luaL_checknumber(L, 2));
    float width = static_cast<float>(luaL_checknumber(L, 3));
    float height = static_cast<float>(luaL_checknumber(L, 4));
    float value = std::clamp(static_cast<float>(luaL_checknumber(L, 5)), 0.0f, 1.0f);
    int color = static_cast<int>(luaL_optinteger(L, 6, 0x4EA1FF));
    auto* s = GetLuaDrawContext(L);
    DrawHostRect(s, x, y, width, height, 0x26313A, height / 2, 1.0f);
    DrawHostRect(s, x, y, width * value, height, color, height / 2, 1.0f);
    return 0;
}

int lua_UiScrollArea(lua_State* L)
{
    const char* id = luaL_checkstring(L, 1);
    float x = static_cast<float>(luaL_checknumber(L, 2));
    float y = static_cast<float>(luaL_checknumber(L, 3));
    float width = static_cast<float>(luaL_checknumber(L, 4));
    float height = static_cast<float>(luaL_checknumber(L, 5));
    int contentHeight = static_cast<int>(luaL_checkinteger(L, 6));
    auto* host = BoundWidgetEngine(L);
    if (host)
    {
        LuaWidget::HostControl control;
        control.type = LuaWidget::HostControl::Type::Scroll;
        control.id = id;
        control.contentHeight = std::max(contentHeight, static_cast<int>(height));
        control.viewportHeight = static_cast<int>(height);
        control.rect = { static_cast<LONG>(x), static_cast<LONG>(y),
            static_cast<LONG>(x + width), static_cast<LONG>(y + height) };
        host->RuntimeRegisterHostControl(BoundWidgetId(L), std::move(control));
    }
    int offset = host
        ? host->RuntimeGetScrollOffset(BoundWidgetId(L), id) : 0;
    if (host)
    {
        host->RuntimeSetScrollOffset(BoundWidgetId(L), id, offset);
        offset = host->RuntimeGetScrollOffset(BoundWidgetId(L), id);
    }
    lua_pushinteger(L, offset);
    return 1;
}

int lua_UiVirtualList(lua_State* L)
{
    const char* id = luaL_checkstring(L, 1);
    float x = static_cast<float>(luaL_checknumber(L, 2));
    float y = static_cast<float>(luaL_checknumber(L, 3));
    float width = static_cast<float>(luaL_checknumber(L, 4));
    float height = static_cast<float>(luaL_checknumber(L, 5));
    int itemHeight = std::max(1, static_cast<int>(luaL_checkinteger(L, 6)));
    int count = std::max(0, static_cast<int>(luaL_checkinteger(L, 7)));
    const int viewportHeight = std::max(1, static_cast<int>(height));
    auto* host = BoundWidgetEngine(L);
    if (host)
    {
        LuaWidget::HostControl control;
        control.type = LuaWidget::HostControl::Type::Scroll;
        control.id = id;
        control.contentHeight = count * itemHeight;
        control.viewportHeight = viewportHeight;
        control.rect = { static_cast<LONG>(x), static_cast<LONG>(y),
            static_cast<LONG>(x + width), static_cast<LONG>(y + height) };
        host->RuntimeRegisterHostControl(BoundWidgetId(L), std::move(control));
    }
    int offset = host
        ? host->RuntimeGetScrollOffset(BoundWidgetId(L), id) : 0;
    if (host)
    {
        host->RuntimeSetScrollOffset(BoundWidgetId(L), id, offset);
        offset = host->RuntimeGetScrollOffset(BoundWidgetId(L), id);
    }
    int first = count == 0 ? 0 : offset / itemHeight + 1;
    int last = count == 0 ? 0 : std::min(count,
        (offset + viewportHeight + itemHeight - 1) / itemHeight);
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, first); lua_setfield(L, -2, "first");
    lua_pushinteger(L, last); lua_setfield(L, -2, "last");
    lua_pushinteger(L, offset); lua_setfield(L, -2, "offset");
    return 1;
}

int lua_UiSetScrollOffset(lua_State* L)
{
    const char* id = luaL_checkstring(L, 1);
    const int offset = std::max(
        0, static_cast<int>(luaL_checkinteger(L, 2)));
    auto* host = BoundWidgetEngine(L);
    if (host)
        host->RuntimeSetScrollOffset(
            BoundWidgetId(L), id ? id : "", offset);
    return 0;
}
}

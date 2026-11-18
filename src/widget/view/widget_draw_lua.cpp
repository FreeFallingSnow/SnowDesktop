#include "widget_draw_lua.h"

#include "widget_draw_geometry.h"
#include "widget_native_marquee.h"
#include "common/utf8.h"
#include "common/utils.h"
#include "widget/runtime/lua_runtime.h"

#include <d2d1_1.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include <lauxlib.h>
}

namespace snowdesktop::widget_runtime
{
using Microsoft::WRL::ComPtr;
using snowdesktop::lua_runtime::LuaTableIsContiguousArray;
using snowdesktop::lua_runtime::LuaTableUsesOnlyFields;

namespace
{
const char kLuaDrawContextKey = 0;

LuaDrawContext* GetLuaDrawContext(lua_State* state)
{
    lua_rawgetp(state, LUA_REGISTRYINDEX, &kLuaDrawContextKey);
    auto* context = static_cast<LuaDrawContext*>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    return context;
}

std::optional<std::wstring> ResolveDrawFont(lua_State* state, int index)
{
    const auto* context = GetLuaDrawContext(state);
    return context && context->services
        ? context->services->resolveFont(state, index) : std::nullopt;
}
}

void BindLuaDrawContext(lua_State* state, LuaDrawContext* context,
    const LuaDrawHostServices& services)
{
    if (context) context->services = &services;
    lua_pushlightuserdata(state, context);
    lua_rawsetp(state, LUA_REGISTRYINDEX, &kLuaDrawContextKey);
}

int lua_DrawText(lua_State* L)
{
    float x = static_cast<float>(luaL_checknumber(L, 1));
    float y = static_cast<float>(luaL_checknumber(L, 2));
    const char* text = luaL_checkstring(L, 3);
    float size = static_cast<float>(luaL_optnumber(L, 4, 14));
    int color = static_cast<int>(luaL_optinteger(L, 5, 0xFFFFFF));
    float maxWidth = static_cast<float>(luaL_optnumber(L, 6, 0));
    bool bold = lua_toboolean(L, 7) != 0;
    bool singleLine = lua_toboolean(L, 8) != 0;
    float requestedHeight = static_cast<float>(luaL_optnumber(L, 9, 0));
    float alpha = static_cast<float>(luaL_optnumber(L, 10, 1.0));

    std::optional<std::wstring> privateFontPath;
    if (!lua_isnoneornil(L, 11))
    {
        privateFontPath = ResolveDrawFont(L, 11);
        if (!privateFontPath)
            return luaL_error(L, "draw.text: invalid font resource handle");
    }

    auto* s = GetLuaDrawContext(L);
    if (!s || !s->ctx || !s->dwrite) return 0;

    IDWriteTextFormat* format = GetCachedTextFormat(s, size,
        bold ? DWRITE_FONT_WEIGHT_BOLD : s->itemFontWeight,
        false, maxWidth > 0 && singleLine
            ? DWRITE_WORD_WRAPPING_NO_WRAP
            : DWRITE_WORD_WRAPPING_WRAP,
        false, false, false,
        privateFontPath ? &*privateFontPath : nullptr);
    if (!format) return 0;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring wtext(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext.data(), wlen);

    ID2D1SolidColorBrush* brush = GetCachedBrush(s, color, alpha);
    if (!brush) return 0;

    float bx = x + s->widgetRect.left;
    float by = y + s->widgetRect.top;

    if (maxWidth > 0)
    {
        ComPtr<IDWriteTextLayout> layout;
        const float maxHeight = requestedHeight > 0
            ? requestedHeight
            : (singleLine ? std::max(size * 1.35f, size + 4.0f) : 5000.0f);
        s->dwrite->CreateTextLayout(wtext.c_str(), static_cast<UINT32>(wtext.size() - 1),
            format, maxWidth, maxHeight, &layout);
        if (layout && (singleLine || requestedHeight > 0))
        {
            ComPtr<IDWriteInlineObject> ellipsis;
            if (SUCCEEDED(s->dwrite->CreateEllipsisTrimmingSign(format, &ellipsis)) && ellipsis)
            {
                DWRITE_TRIMMING trimming{};
                trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
                layout->SetTrimming(&trimming, ellipsis.Get());
            }
        }
        if (layout)
            s->ctx->DrawTextLayout(D2D1::Point2F(bx, by), layout.Get(), brush);
    }
    else
    {
        D2D1_RECT_F rect = { bx, by, bx + 800, by + 200 };
        s->ctx->DrawTextW(wtext.c_str(), static_cast<UINT32>(wtext.size() - 1),
            format, &rect, brush);
    }
    return 0;
}

int lua_MeasureText(lua_State* L)
{
    const char* text = luaL_checkstring(L, 1);
    float size = static_cast<float>(luaL_optnumber(L, 2, 14));
    float maxWidth = static_cast<float>(luaL_optnumber(L, 3, 0));
    bool bold = lua_toboolean(L, 4) != 0;
    std::optional<std::wstring> privateFontPath;
    if (!lua_isnoneornil(L, 5))
    {
        privateFontPath = ResolveDrawFont(L, 5);
        if (!privateFontPath)
            return luaL_error(L,
                "draw.measureText: invalid font resource handle");
    }

    auto pushSize = [&](float width, float height,
        const std::optional<D2D1_RECT_F>& ink = std::nullopt) {
        lua_createtable(L, 0, ink ? 3 : 2);
        lua_pushnumber(L, width);
        lua_setfield(L, -2, "width");
        lua_pushnumber(L, height);
        lua_setfield(L, -2, "height");
        if (ink)
        {
            lua_createtable(L, 0, 4);
            lua_pushnumber(L, ink->left);
            lua_setfield(L, -2, "left");
            lua_pushnumber(L, ink->top);
            lua_setfield(L, -2, "top");
            lua_pushnumber(L, std::max(0.0f, ink->right - ink->left));
            lua_setfield(L, -2, "width");
            lua_pushnumber(L, std::max(0.0f, ink->bottom - ink->top));
            lua_setfield(L, -2, "height");
            lua_setfield(L, -2, "ink");
        }
        return 1;
    };

    auto* s = GetLuaDrawContext(L);
    if (!s || !s->dwrite)
        return pushSize(0.0f, 0.0f);

    IDWriteTextFormat* format = GetCachedTextFormat(s, size,
        bold ? DWRITE_FONT_WEIGHT_BOLD : s->itemFontWeight,
        false, DWRITE_WORD_WRAPPING_WRAP, false, false, false,
        privateFontPath ? &*privateFontPath : nullptr);
    if (!format)
        return pushSize(0.0f, 0.0f);

    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring wtext(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext.data(), wlen);

    ComPtr<IDWriteTextLayout> layout;
    const float layoutWidth = maxWidth > 0.0f ? maxWidth : 4096.0f;
    if (FAILED(s->dwrite->CreateTextLayout(wtext.c_str(), static_cast<UINT32>(wtext.size() - 1),
        format, layoutWidth, 4096.0f, &layout)) || !layout)
        return pushSize(0.0f, size);

    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    DWRITE_OVERHANG_METRICS overhang{};
    if (SUCCEEDED(layout->GetOverhangMetrics(&overhang)))
    {
        // Visible bounds are separate from the unchanged line metrics.
        // Negative overhang denotes unused space inside the whole layout box.
        D2D1_RECT_F ink{};
        if (metrics.width > 0.0f)
            ink = { -overhang.left, -overhang.top,
                layout->GetMaxWidth() + overhang.right,
                layout->GetMaxHeight() + overhang.bottom };
        return pushSize(metrics.widthIncludingTrailingWhitespace,
            metrics.height, ink);
    }
    return pushSize(metrics.widthIncludingTrailingWhitespace, metrics.height);
}

int lua_DrawRect(lua_State* L)
{
    float x = static_cast<float>(luaL_checknumber(L, 1));
    float y = static_cast<float>(luaL_checknumber(L, 2));
    float w = static_cast<float>(luaL_checknumber(L, 3));
    float h = static_cast<float>(luaL_checknumber(L, 4));
    int color = static_cast<int>(luaL_optinteger(L, 5, 0xFFFFFF));
    float radius = static_cast<float>(luaL_optnumber(L, 6, 0));
    float alpha = static_cast<float>(luaL_optnumber(L, 7, 1.0));

    auto* s = GetLuaDrawContext(L);
    if (!s || !s->ctx) return 0;

    ID2D1SolidColorBrush* brush = GetCachedBrush(s, color, alpha);
    if (!brush) return 0;

    D2D1_RECT_F rect = { x + s->widgetRect.left, y + s->widgetRect.top,
        x + s->widgetRect.left + w, y + s->widgetRect.top + h };
    if (radius > 0)
    {
        D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(rect, radius, radius);
        s->ctx->FillRoundedRectangle(rounded, brush);
    }
    else
    {
        s->ctx->FillRectangle(rect, brush);
    }
    return 0;
}

int lua_DrawPushClip(lua_State* L)
{
    float x = static_cast<float>(luaL_checknumber(L, 1));
    float y = static_cast<float>(luaL_checknumber(L, 2));
    float width = std::max(0.0f, static_cast<float>(luaL_checknumber(L, 3)));
    float height = std::max(0.0f, static_cast<float>(luaL_checknumber(L, 4)));
    auto* s = GetLuaDrawContext(L);
    if (!s || !s->ctx) return 0;
    D2D1_RECT_F clip = D2D1::RectF(
        s->widgetRect.left + x,
        s->widgetRect.top + y,
        s->widgetRect.left + x + width,
        s->widgetRect.top + y + height);
    if (!s->immediateClipRects.empty())
    {
        const auto& parent = s->immediateClipRects.back();
        clip.left = std::max(clip.left, parent.left);
        clip.top = std::max(clip.top, parent.top);
        clip.right = std::min(clip.right, parent.right);
        clip.bottom = std::min(clip.bottom, parent.bottom);
    }
    s->ctx->PushAxisAlignedClip(clip,
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    s->immediateClipRects.push_back(clip);
    ++s->widgetClipDepth;
    return 0;
}

int lua_DrawPopClip(lua_State* L)
{
    auto* s = GetLuaDrawContext(L);
    if (!s || !s->ctx || s->widgetClipDepth <= 0) return 0;
    s->ctx->PopAxisAlignedClip();
    if (!s->immediateClipRects.empty())
        s->immediateClipRects.pop_back();
    --s->widgetClipDepth;
    return 0;
}

int lua_DrawStrokeRect(lua_State* L)
{
    float x = static_cast<float>(luaL_checknumber(L, 1));
    float y = static_cast<float>(luaL_checknumber(L, 2));
    float w = static_cast<float>(luaL_checknumber(L, 3));
    float h = static_cast<float>(luaL_checknumber(L, 4));
    int color = static_cast<int>(luaL_optinteger(L, 5, 0xFFFFFF));
    float radius = static_cast<float>(luaL_optnumber(L, 6, 0));
    float thickness = static_cast<float>(luaL_optnumber(L, 7, 1.0));
    float alpha = static_cast<float>(luaL_optnumber(L, 8, 1.0));
    auto* s = GetLuaDrawContext(L);
    if (!s || !s->ctx) return 0;
    ID2D1SolidColorBrush* brush = GetCachedBrush(s, color, alpha);
    if (!brush) return 0;
    D2D1_RECT_F rect = { x + s->widgetRect.left, y + s->widgetRect.top,
        x + s->widgetRect.left + w, y + s->widgetRect.top + h };
    if (radius > 0)
        s->ctx->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush, thickness);
    else
        s->ctx->DrawRectangle(rect, brush, thickness);
    return 0;
}

static int LuaDrawColor(lua_State* state, int index,
    int fallback, const char* api)
{
    if (lua_isnoneornil(state, index)) return fallback;
    const lua_Integer value = luaL_checkinteger(state, index);
    if (value < 0 || value > 0xFFFFFF)
        luaL_error(state, "%s: colors must be between 0 and 0xFFFFFF", api);
    return static_cast<int>(value);
}

static float LuaDrawAlpha(lua_State* state, int index, const char* api)
{
    const float value = static_cast<float>(luaL_optnumber(state, index, 1.0));
    if (!std::isfinite(value) || value < 0.0f || value > 1.0f)
        luaL_error(state, "%s: alpha must be between 0 and 1", api);
    return value;
}

enum class DrawMarqueeError
{
    None,
    Options,
    Key,
    Text,
    Number,
    Geometry,
    Color,
    Alpha,
    Bold,
    Font,
    Host,
    Format,
    Layout,
    Measure,
    Submit,
};

static const char* DrawMarqueeErrorMessage(DrawMarqueeError error) noexcept
{
    switch (error)
    {
    case DrawMarqueeError::Options:
        return "options must be a closed plain table";
    case DrawMarqueeError::Key:
        return "key must be 1 to 128 bytes of valid UTF-8";
    case DrawMarqueeError::Text:
        return "text must be 1 to 4096 bytes of valid UTF-8";
    case DrawMarqueeError::Number:
        return "x, y, width, height, size, speed, gap, and alpha must be numbers";
    case DrawMarqueeError::Geometry:
        return "geometry, size, speed, and gap must be finite and bounded";
    case DrawMarqueeError::Color:
        return "color must be an integer between 0 and 0xFFFFFF";
    case DrawMarqueeError::Alpha:
        return "alpha must be between 0 and 1";
    case DrawMarqueeError::Bold:
        return "bold must be a boolean";
    case DrawMarqueeError::Font:
        return "font must be a valid package font resource handle";
    case DrawMarqueeError::Host:
        return "host context is unavailable";
    case DrawMarqueeError::Format:
        return "text format is unavailable";
    case DrawMarqueeError::Layout:
        return "text layout could not be created";
    case DrawMarqueeError::Measure:
        return "text layout could not be measured";
    case DrawMarqueeError::Submit:
        return "host rejected the marquee submission";
    case DrawMarqueeError::None:
        break;
    }
    return "unknown error";
}

static DrawMarqueeError TryDrawMarqueeText(
    lua_State* state, bool& scrolling)
{
    scrolling = false;
    if (lua_type(state, 1) != LUA_TTABLE)
        return DrawMarqueeError::Options;
    const int descriptor = lua_absindex(state, 1);
    std::string validationError;
    if (!LuaTableUsesOnlyFields(state, descriptor,
            { "key", "x", "y", "width", "height", "text", "size",
                "color", "bold", "speed", "gap", "alpha", "font" },
            "draw.marqueeText", validationError))
        return DrawMarqueeError::Options;

    const auto readString = [state, descriptor](
            const char* field, std::string& value) {
        lua_getfield(state, descriptor, field);
        if (lua_type(state, -1) != LUA_TSTRING)
        {
            lua_pop(state, 1);
            return false;
        }
        std::size_t length = 0;
        const char* raw = lua_tolstring(state, -1, &length);
        value.assign(raw ? raw : "", length);
        lua_pop(state, 1);
        return raw != nullptr;
    };
    std::string key;
    std::string text;
    if (!readString("key", key) || key.empty() || key.size() > 128 ||
        key.find('\0') != std::string::npos || !snowdesktop::utf8::IsValid(key))
        return DrawMarqueeError::Key;
    if (!readString("text", text) || text.empty() || text.size() > 4096 ||
        text.find('\0') != std::string::npos || !snowdesktop::utf8::IsValid(text))
        return DrawMarqueeError::Text;

    const auto readNumber = [state, descriptor](const char* field,
            float fallback, bool required, float& value) {
        lua_getfield(state, descriptor, field);
        if (lua_isnil(state, -1) && !required)
        {
            value = fallback;
            lua_pop(state, 1);
            return true;
        }
        if (lua_type(state, -1) != LUA_TNUMBER)
        {
            lua_pop(state, 1);
            return false;
        }
        value = static_cast<float>(lua_tonumber(state, -1));
        lua_pop(state, 1);
        return true;
    };
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float size = 14.0f;
    float speed = 24.0f;
    float gap = 24.0f;
    float alpha = 1.0f;
    if (!readNumber("x", 0.0f, true, x) ||
        !readNumber("y", 0.0f, true, y) ||
        !readNumber("width", 0.0f, true, width) ||
        !readNumber("height", 0.0f, true, height) ||
        !readNumber("size", 14.0f, false, size) ||
        !readNumber("speed", 24.0f, false, speed) ||
        !readNumber("gap", 24.0f, false, gap) ||
        !readNumber("alpha", 1.0f, false, alpha))
        return DrawMarqueeError::Number;
    if (!std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(width) || !std::isfinite(height) ||
        !std::isfinite(size) || !std::isfinite(speed) ||
        !std::isfinite(gap) || std::abs(x) > 1'000'000.0f ||
        std::abs(y) > 1'000'000.0f || width <= 0.0f ||
        height <= 0.0f || width > 100'000.0f ||
        height > 100'000.0f || std::abs(x + width) > 1'000'000.0f ||
        std::abs(y + height) > 1'000'000.0f || size < 1.0f ||
        size > 512.0f || speed < 1.0f || speed > 512.0f ||
        gap < 0.0f || gap > 4096.0f)
        return DrawMarqueeError::Geometry;
    if (!std::isfinite(alpha) || alpha < 0.0f || alpha > 1.0f)
        return DrawMarqueeError::Alpha;

    int color = 0xFFFFFF;
    lua_getfield(state, descriptor, "color");
    if (!lua_isnil(state, -1))
    {
        if (!lua_isinteger(state, -1) || lua_tointeger(state, -1) < 0 ||
            lua_tointeger(state, -1) > 0xFFFFFF)
        {
            lua_pop(state, 1);
            return DrawMarqueeError::Color;
        }
        color = static_cast<int>(lua_tointeger(state, -1));
    }
    lua_pop(state, 1);
    lua_getfield(state, descriptor, "bold");
    if (!lua_isnil(state, -1) && !lua_isboolean(state, -1))
    {
        lua_pop(state, 1);
        return DrawMarqueeError::Bold;
    }
    const bool bold = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);

    std::optional<std::wstring> privateFontPath;
    lua_getfield(state, descriptor, "font");
    if (!lua_isnil(state, -1))
    {
        privateFontPath = ResolveDrawFont(state, -1);
        if (!privateFontPath)
        {
            lua_pop(state, 1);
            return DrawMarqueeError::Font;
        }
    }
    lua_pop(state, 1);

    auto* d2d = GetLuaDrawContext(state);
    if (!d2d || !d2d->ctx || !d2d->dwrite || !d2d->services)
        return DrawMarqueeError::Host;
    IDWriteTextFormat* format = GetCachedTextFormat(d2d, size,
        bold ? DWRITE_FONT_WEIGHT_BOLD : d2d->itemFontWeight,
        false, DWRITE_WORD_WRAPPING_NO_WRAP, false, false, false,
        privateFontPath ? &*privateFontPath : nullptr);
    if (!format) return DrawMarqueeError::Format;

    const std::wstring wideText = Utf8ToWide(text);
    ComPtr<IDWriteTextLayout> layout;
    if (wideText.empty() || FAILED(d2d->dwrite->CreateTextLayout(
            wideText.data(), static_cast<UINT32>(wideText.size()), format,
            100'000.0f, std::max(height, size * 1.5f), &layout)) || !layout)
        return DrawMarqueeError::Layout;
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics)))
        return DrawMarqueeError::Measure;

    NativeMarqueeText marquee;
    marquee.key = std::move(key);
    marquee.viewport = D2D1::RectF(
        x + d2d->widgetRect.left,
        y + d2d->widgetRect.top,
        x + d2d->widgetRect.left + width,
        y + d2d->widgetRect.top + height);
    if (!d2d->immediateClipRects.empty())
    {
        const auto& clip = d2d->immediateClipRects.back();
        marquee.viewport.left = std::max(marquee.viewport.left, clip.left);
        marquee.viewport.top = std::max(marquee.viewport.top, clip.top);
        marquee.viewport.right = std::min(marquee.viewport.right, clip.right);
        marquee.viewport.bottom = std::min(
            marquee.viewport.bottom, clip.bottom);
    }
    marquee.layout = std::move(layout);
    marquee.originX = x + d2d->widgetRect.left;
    marquee.originY = y + d2d->widgetRect.top + std::max(
        0.0f, (height - metrics.height) * 0.5f);
    marquee.textWidth = metrics.widthIncludingTrailingWhitespace;
    marquee.textHeight = metrics.height;
    marquee.speed = speed;
    marquee.gap = gap;
    marquee.color = color;
    marquee.alpha = alpha;
    scrolling = snowdesktop::widget_runtime::ShouldScrollDrawMarquee(
        marquee.textWidth, width);
    marquee.scrolling = marquee.viewport.right > marquee.viewport.left &&
        marquee.viewport.bottom > marquee.viewport.top && scrolling;
    if (!d2d->services->submitMarquee(state, std::move(marquee)))
        return DrawMarqueeError::Submit;
    return DrawMarqueeError::None;
}

int lua_DrawMarqueeText(lua_State* state)
{
    if (auto* d2d = GetLuaDrawContext(state); d2d && d2d->backgroundLayerActive)
        return luaL_error(state,
            "draw.marqueeText: backgroundLayer does not support native marquee state");
    bool scrolling = false;
    const DrawMarqueeError error = TryDrawMarqueeText(state, scrolling);
    if (error != DrawMarqueeError::None)
        return luaL_error(state, "draw.marqueeText: %s",
            DrawMarqueeErrorMessage(error));
    lua_pushboolean(state, scrolling ? 1 : 0);
    return 1;
}

int lua_DrawArc(lua_State* state)
{
    const float centerX = static_cast<float>(luaL_checknumber(state, 1));
    const float centerY = static_cast<float>(luaL_checknumber(state, 2));
    const float radius = static_cast<float>(luaL_checknumber(state, 3));
    const float start = static_cast<float>(luaL_checknumber(state, 4));
    const float sweep = static_cast<float>(luaL_checknumber(state, 5));
    const float thickness = static_cast<float>(
        luaL_optnumber(state, 6, 1.0));
    const int color = LuaDrawColor(state, 7, 0xFFFFFF, "draw.arc");
    const float alpha = LuaDrawAlpha(state, 8, "draw.arc");
    if (!std::isfinite(thickness) || thickness <= 0.0f ||
        thickness > 4096.0f)
        return luaL_error(state,
            "draw.arc: thickness must be positive and bounded");
    std::vector<snowdesktop::widget_runtime::DrawArcPiece> pieces;
    std::string error;
    if (!snowdesktop::widget_runtime::BuildDrawArc(centerX, centerY,
            radius, start, sweep, pieces, error))
        return luaL_error(state, "draw.arc: %s", error.c_str());
    auto* d2d = GetLuaDrawContext(state);
    if (!d2d || !d2d->ctx) return 0;
    ComPtr<ID2D1Factory> factory;
    d2d->ctx->GetFactory(&factory);
    ComPtr<ID2D1PathGeometry> geometry;
    ComPtr<ID2D1GeometrySink> sink;
    if (!factory || FAILED(factory->CreatePathGeometry(&geometry)) ||
        !geometry || FAILED(geometry->Open(&sink)) || !sink)
        return 0;
    const auto absolute = [d2d](const auto& point) {
        return D2D1::Point2F(point.x + d2d->widgetRect.left,
            point.y + d2d->widgetRect.top);
    };
    sink->BeginFigure(absolute(pieces.front().start),
        D2D1_FIGURE_BEGIN_HOLLOW);
    for (const auto& piece : pieces)
        sink->AddArc(D2D1::ArcSegment(absolute(piece.end),
            D2D1::SizeF(piece.radius, piece.radius), 0.0f,
            piece.clockwise ? D2D1_SWEEP_DIRECTION_CLOCKWISE :
                D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
            D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return 0;
    if (ID2D1SolidColorBrush* brush = GetCachedBrush(d2d, color, alpha))
        d2d->ctx->DrawGeometry(geometry.Get(), brush, thickness);
    return 0;
}

int lua_DrawPath(lua_State* state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    std::string error;
    if (!LuaTableIsContiguousArray(state, 1, "draw.path commands", error))
        return luaL_error(state, "draw.path: %s", error.c_str());
    const std::size_t count = lua_rawlen(state, 1);
    if (count == 0 || count > 256)
        return luaL_error(state,
            "draw.path: commands must contain 1 to 256 items");
    using Command = snowdesktop::widget_runtime::DrawPathCommand;
    using Type = snowdesktop::widget_runtime::DrawPathCommandType;
    std::vector<Command> commands;
    commands.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        lua_rawgeti(state, 1, static_cast<lua_Integer>(index + 1));
        if (!lua_istable(state, -1))
        {
            lua_pop(state, 1);
            return luaL_error(state,
                "draw.path: every command must be a table");
        }
        lua_getfield(state, -1, "op");
        const char* operation = luaL_checkstring(state, -1);
        const std::string op = operation ? operation : "";
        lua_pop(state, 1);
        std::vector<std::string_view> allowed{ "op" };
        Command command;
        if (op == "move" || op == "line")
        {
            allowed = { "op", "x", "y" };
            command.type = op == "move" ? Type::Move : Type::Line;
            lua_getfield(state, -1, "x");
            command.point.x = static_cast<float>(
                luaL_checknumber(state, -1));
            lua_pop(state, 1);
            lua_getfield(state, -1, "y");
            command.point.y = static_cast<float>(
                luaL_checknumber(state, -1));
            lua_pop(state, 1);
        }
        else if (op == "cubic")
        {
            allowed = { "op", "x1", "y1", "x2", "y2", "x", "y" };
            command.type = Type::Cubic;
            const auto read = [&](const char* field) {
                lua_getfield(state, -1, field);
                const float value = static_cast<float>(
                    luaL_checknumber(state, -1));
                lua_pop(state, 1);
                return value;
            };
            command.control1 = { read("x1"), read("y1") };
            command.control2 = { read("x2"), read("y2") };
            command.point = { read("x"), read("y") };
        }
        else if (op == "quadratic")
        {
            allowed = { "op", "x1", "y1", "x", "y" };
            command.type = Type::Quadratic;
            const auto read = [&](const char* field) {
                lua_getfield(state, -1, field);
                const float value = static_cast<float>(
                    luaL_checknumber(state, -1));
                lua_pop(state, 1);
                return value;
            };
            command.control1 = { read("x1"), read("y1") };
            command.point = { read("x"), read("y") };
        }
        else if (op == "close")
            command.type = Type::Close;
        else
        {
            lua_pop(state, 1);
            return luaL_error(state,
                "draw.path: unsupported command '%s'", op.c_str());
        }
        if (!LuaTableUsesOnlyFields(state, -1,
                std::span<const std::string_view>(allowed),
                "draw.path command", error))
        {
            lua_pop(state, 1);
            return luaL_error(state, "draw.path: %s", error.c_str());
        }
        commands.push_back(command);
        lua_pop(state, 1);
    }
    if (!snowdesktop::widget_runtime::ValidateDrawPath(commands, error))
        return luaL_error(state, "draw.path: %s", error.c_str());

    std::optional<int> fillColor;
    std::optional<int> strokeColor;
    float thickness = 1.0f;
    float alpha = 1.0f;
    D2D1_FILL_MODE fillMode = D2D1_FILL_MODE_ALTERNATE;
    if (!lua_isnoneornil(state, 2))
    {
        luaL_checktype(state, 2, LUA_TTABLE);
        if (!LuaTableUsesOnlyFields(state, 2,
                { "fillColor", "strokeColor", "thickness", "alpha",
                    "fillRule" }, "draw.path options", error))
            return luaL_error(state, "draw.path: %s", error.c_str());
        lua_getfield(state, 2, "fillColor");
        if (!lua_isnil(state, -1))
            fillColor = LuaDrawColor(
                state, -1, 0xFFFFFF, "draw.path");
        lua_pop(state, 1);
        lua_getfield(state, 2, "strokeColor");
        if (!lua_isnil(state, -1))
            strokeColor = LuaDrawColor(
                state, -1, 0xFFFFFF, "draw.path");
        lua_pop(state, 1);
        lua_getfield(state, 2, "thickness");
        if (!lua_isnil(state, -1))
            thickness = static_cast<float>(luaL_checknumber(state, -1));
        lua_pop(state, 1);
        lua_getfield(state, 2, "alpha");
        if (!lua_isnil(state, -1))
            alpha = static_cast<float>(luaL_checknumber(state, -1));
        lua_pop(state, 1);
        lua_getfield(state, 2, "fillRule");
        if (!lua_isnil(state, -1))
        {
            const char* value = luaL_checkstring(state, -1);
            const std::string rule = value ? value : "";
            if (rule == "winding") fillMode = D2D1_FILL_MODE_WINDING;
            else if (rule != "alternate")
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    "draw.path: fillRule must be alternate or winding");
            }
        }
        lua_pop(state, 1);
    }
    if (!fillColor && !strokeColor) strokeColor = 0xFFFFFF;
    if (!std::isfinite(thickness) || thickness <= 0.0f ||
        thickness > 4096.0f || !std::isfinite(alpha) || alpha < 0.0f ||
        alpha > 1.0f)
        return luaL_error(state,
            "draw.path: thickness and alpha are out of range");

    auto* d2d = GetLuaDrawContext(state);
    if (!d2d || !d2d->ctx) return 0;
    ComPtr<ID2D1Factory> factory;
    d2d->ctx->GetFactory(&factory);
    ComPtr<ID2D1PathGeometry> geometry;
    ComPtr<ID2D1GeometrySink> sink;
    if (!factory || FAILED(factory->CreatePathGeometry(&geometry)) ||
        !geometry || FAILED(geometry->Open(&sink)) || !sink)
        return 0;
    sink->SetFillMode(fillMode);
    bool open = false;
    const auto point = [d2d](const auto& value) {
        return D2D1::Point2F(value.x + d2d->widgetRect.left,
            value.y + d2d->widgetRect.top);
    };
    for (const auto& command : commands)
    {
        if (command.type == Type::Move)
        {
            if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->BeginFigure(point(command.point), fillColor
                ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
            open = true;
        }
        else if (command.type == Type::Line)
            sink->AddLine(point(command.point));
        else if (command.type == Type::Cubic)
            sink->AddBezier(D2D1::BezierSegment(point(command.control1),
                point(command.control2), point(command.point)));
        else if (command.type == Type::Quadratic)
            sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(
                point(command.control1), point(command.point)));
        else if (open)
        {
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            open = false;
        }
    }
    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return 0;
    if (fillColor)
    {
        if (ID2D1SolidColorBrush* brush = GetCachedBrush(
                d2d, *fillColor, alpha))
            d2d->ctx->FillGeometry(geometry.Get(), brush);
    }
    if (strokeColor)
    {
        if (ID2D1SolidColorBrush* brush = GetCachedBrush(
                d2d, *strokeColor, alpha))
            d2d->ctx->DrawGeometry(geometry.Get(), brush, thickness);
    }
    return 0;
}

int lua_DrawGradientRect(lua_State* state)
{
    const float x = static_cast<float>(luaL_checknumber(state, 1));
    const float y = static_cast<float>(luaL_checknumber(state, 2));
    const float width = static_cast<float>(luaL_checknumber(state, 3));
    const float height = static_cast<float>(luaL_checknumber(state, 4));
    const int startColor = LuaDrawColor(
        state, 5, 0xFFFFFF, "draw.gradientRect");
    const int endColor = LuaDrawColor(
        state, 6, 0x000000, "draw.gradientRect");
    const char* directionRaw = luaL_optstring(state, 7, "vertical");
    const std::string direction = directionRaw ? directionRaw : "";
    const float radius = static_cast<float>(
        luaL_optnumber(state, 8, 0.0));
    const float alpha = LuaDrawAlpha(state, 9, "draw.gradientRect");
    if (!std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(width) || !std::isfinite(height) ||
        width <= 0.0f || height <= 0.0f || width > 100000.0f ||
        height > 100000.0f || std::abs(x) > 1000000.0f ||
        std::abs(y) > 1000000.0f ||
        std::abs(x + width) > 1000000.0f ||
        std::abs(y + height) > 1000000.0f ||
        !std::isfinite(radius) || radius < 0.0f ||
        radius > std::min(width, height) * 0.5f)
        return luaL_error(state,
            "draw.gradientRect: geometry must be positive and bounded");
    if (direction != "horizontal" && direction != "vertical" &&
        direction != "diagonalDown" && direction != "diagonalUp")
        return luaL_error(state,
            "draw.gradientRect: unsupported direction");
    auto* d2d = GetLuaDrawContext(state);
    if (!d2d || !d2d->ctx) return 0;
    const D2D1_RECT_F rect = D2D1::RectF(
        d2d->widgetRect.left + x, d2d->widgetRect.top + y,
        d2d->widgetRect.left + x + width,
        d2d->widgetRect.top + y + height);
    D2D1_POINT_2F startPoint = D2D1::Point2F(rect.left, rect.top);
    D2D1_POINT_2F endPoint = D2D1::Point2F(rect.left, rect.bottom);
    if (direction == "horizontal")
        endPoint = D2D1::Point2F(rect.right, rect.top);
    else if (direction == "diagonalDown")
        endPoint = D2D1::Point2F(rect.right, rect.bottom);
    else if (direction == "diagonalUp")
    {
        startPoint = D2D1::Point2F(rect.left, rect.bottom);
        endPoint = D2D1::Point2F(rect.right, rect.top);
    }
    const auto color = [alpha](int value) {
        return D2D1::ColorF(((value >> 16) & 0xFF) / 255.0f,
            ((value >> 8) & 0xFF) / 255.0f,
            (value & 0xFF) / 255.0f, alpha);
    };
    const D2D1_GRADIENT_STOP stops[] = {
        { 0.0f, color(startColor) }, { 1.0f, color(endColor) }
    };
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> brush;
    if (FAILED(d2d->ctx->CreateGradientStopCollection(
            stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
            &collection)) || !collection ||
        FAILED(d2d->ctx->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(startPoint, endPoint),
            collection.Get(), &brush)) || !brush)
        return 0;
    if (radius > 0.0f)
        d2d->ctx->FillRoundedRectangle(
            D2D1::RoundedRect(rect, radius, radius), brush.Get());
    else
        d2d->ctx->FillRectangle(rect, brush.Get());
    return 0;
}

int lua_DrawShadow(lua_State* state)
{
    snowdesktop::widget_runtime::DrawRect bounds{
        static_cast<float>(luaL_checknumber(state, 1)),
        static_cast<float>(luaL_checknumber(state, 2)),
        static_cast<float>(luaL_checknumber(state, 3)),
        static_cast<float>(luaL_checknumber(state, 4)),
    };
    const int color = LuaDrawColor(
        state, 5, 0x000000, "draw.shadow");
    const float blur = static_cast<float>(luaL_optnumber(state, 6, 12.0));
    const float radius = static_cast<float>(luaL_optnumber(state, 7, 0.0));
    const float offsetX = static_cast<float>(luaL_optnumber(state, 8, 0.0));
    const float offsetY = static_cast<float>(luaL_optnumber(state, 9, 4.0));
    const float alpha = LuaDrawAlpha(state, 10, "draw.shadow");
    std::vector<snowdesktop::widget_runtime::DrawShadowLayer> layers;
    std::string error;
    if (!snowdesktop::widget_runtime::BuildDrawShadowLayers(bounds, blur,
            radius, offsetX, offsetY, alpha, layers, error))
        return luaL_error(state, "draw.shadow: %s", error.c_str());
    auto* d2d = GetLuaDrawContext(state);
    if (!d2d || !d2d->ctx) return 0;
    for (const auto& layer : layers)
    {
        ID2D1SolidColorBrush* brush = GetCachedBrush(
            d2d, color, layer.alpha);
        if (!brush) continue;
        const D2D1_RECT_F rect = D2D1::RectF(
            d2d->widgetRect.left + layer.bounds.x,
            d2d->widgetRect.top + layer.bounds.y,
            d2d->widgetRect.left + layer.bounds.x + layer.bounds.width,
            d2d->widgetRect.top + layer.bounds.y + layer.bounds.height);
        if (layer.radius > 0.0f)
            d2d->ctx->FillRoundedRectangle(D2D1::RoundedRect(
                rect, layer.radius, layer.radius), brush);
        else
            d2d->ctx->FillRectangle(rect, brush);
    }
    return 0;
}

int lua_DrawSparkline(lua_State* state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    std::string error;
    if (!LuaTableIsContiguousArray(
            state, 1, "draw.sparkline values", error))
        return luaL_error(state, "draw.sparkline: %s", error.c_str());
    const std::size_t count = lua_rawlen(state, 1);
    if (count == 0 || count > 512)
        return luaL_error(state,
            "draw.sparkline: values must contain 1 to 512 items");
    std::vector<float> values;
    values.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        lua_rawgeti(state, 1, static_cast<lua_Integer>(index + 1));
        values.push_back(static_cast<float>(luaL_checknumber(state, -1)));
        lua_pop(state, 1);
    }
    snowdesktop::widget_runtime::DrawRect bounds{
        static_cast<float>(luaL_checknumber(state, 2)),
        static_cast<float>(luaL_checknumber(state, 3)),
        static_cast<float>(luaL_checknumber(state, 4)),
        static_cast<float>(luaL_checknumber(state, 5)),
    };
    const int color = LuaDrawColor(
        state, 6, 0xFFFFFF, "draw.sparkline");
    const float thickness = static_cast<float>(
        luaL_optnumber(state, 7, 1.0));
    std::optional<float> minimum;
    std::optional<float> maximum;
    if (!lua_isnoneornil(state, 8))
        minimum = static_cast<float>(luaL_checknumber(state, 8));
    if (!lua_isnoneornil(state, 9))
        maximum = static_cast<float>(luaL_checknumber(state, 9));
    const float alpha = LuaDrawAlpha(state, 10, "draw.sparkline");
    if (!std::isfinite(thickness) || thickness <= 0.0f ||
        thickness > 4096.0f)
        return luaL_error(state,
            "draw.sparkline: thickness must be positive and bounded");
    std::vector<snowdesktop::widget_runtime::DrawPoint> points;
    if (!snowdesktop::widget_runtime::BuildDrawSparkline(values, bounds,
            minimum, maximum, points, error))
        return luaL_error(state, "draw.sparkline: %s", error.c_str());
    auto* d2d = GetLuaDrawContext(state);
    if (!d2d || !d2d->ctx) return 0;
    ID2D1SolidColorBrush* brush = GetCachedBrush(d2d, color, alpha);
    if (!brush) return 0;
    const auto point = [d2d](const auto& value) {
        return D2D1::Point2F(value.x + d2d->widgetRect.left,
            value.y + d2d->widgetRect.top);
    };
    if (points.size() == 1)
        d2d->ctx->FillEllipse(D2D1::Ellipse(
            point(points.front()), thickness, thickness), brush);
    else
    {
        for (std::size_t index = 1; index < points.size(); ++index)
            d2d->ctx->DrawLine(point(points[index - 1]),
                point(points[index]), brush, thickness);
    }
    return 0;
}

int lua_DrawImage(lua_State* L)
{
    float x = static_cast<float>(luaL_checknumber(L, 2));
    float y = static_cast<float>(luaL_checknumber(L, 3));
    float w = static_cast<float>(luaL_checknumber(L, 4));
    float h = static_cast<float>(luaL_checknumber(L, 5));
    float alpha = static_cast<float>(luaL_optnumber(L, 6, 1.0));
    auto* s = GetLuaDrawContext(L);
    if (!s || !s->services) return 0;
    ID2D1Bitmap1* bmp = s->services->resolveImage(
        L, 1, "draw.image", w, h, DrawImageFit::Fill, nullptr, false);
    if (!s || !s->ctx || !bmp) return 0;
    D2D1_RECT_F dst = D2D1::RectF(x + s->widgetRect.left, y + s->widgetRect.top,
        x + s->widgetRect.left + w, y + s->widgetRect.top + h);
    const auto size = bmp->GetSize();
    const auto sampled = s->imageSamplingCache.Resolve(s->ctx, bmp, dst,
        D2D1::RectF(0, 0, size.width, size.height));
    s->ctx->DrawBitmap(sampled.bitmap.Get(), dst, alpha,
        sampled.interpolation, sampled.source);
    return 0;
}

int lua_DrawImageFit(lua_State* state)
{
    const float x = static_cast<float>(luaL_checknumber(state, 2));
    const float y = static_cast<float>(luaL_checknumber(state, 3));
    const float width = static_cast<float>(luaL_checknumber(state, 4));
    const float height = static_cast<float>(luaL_checknumber(state, 5));
    const char* fitRaw = luaL_optstring(state, 6, "contain");
    const char* alignmentRaw = luaL_optstring(state, 7, "center");
    const float alpha = LuaDrawAlpha(state, 8, "draw.imageFit");
    const char* interpolationRaw = luaL_optstring(state, 9, "linear");
    const float rotationDegrees = static_cast<float>(
        luaL_optnumber(state, 10, 0.0));
    const float originX = static_cast<float>(
        luaL_optnumber(state, 11, 0.5));
    const float originY = static_cast<float>(
        luaL_optnumber(state, 12, 0.5));
    const float cornerRadius = static_cast<float>(
        luaL_optnumber(state, 13, 0.0));
    if (!std::isfinite(cornerRadius) || cornerRadius < 0.0f ||
        cornerRadius > 100000.0f)
        luaL_error(state,
            "draw.imageFit: cornerRadius must be finite and between 0 and 100000");
    using Fit = snowdesktop::widget_runtime::DrawImageFit;
    using Alignment = snowdesktop::widget_runtime::DrawImageAlignment;
    Fit fit = Fit::Contain;
    const std::string fitName = fitRaw ? fitRaw : "";
    if (fitName == "fill") fit = Fit::Fill;
    else if (fitName == "cover") fit = Fit::Cover;
    else if (fitName == "none") fit = Fit::None;
    else if (fitName != "contain")
        return luaL_error(state, "draw.imageFit: unsupported fit");
    Alignment alignment = Alignment::Center;
    const std::string alignmentName = alignmentRaw ? alignmentRaw : "";
    if (alignmentName == "start") alignment = Alignment::Start;
    else if (alignmentName == "end") alignment = Alignment::End;
    else if (alignmentName != "center")
        return luaL_error(state, "draw.imageFit: unsupported alignment");
    const std::string interpolation = interpolationRaw
        ? interpolationRaw : "";
    if (interpolation != "linear" && interpolation != "nearest")
        return luaL_error(state,
            "draw.imageFit: interpolation must be linear or nearest");
    std::string transformError;
    if (!snowdesktop::widget_runtime::ValidateDrawImageTransform(
            rotationDegrees, originX, originY, transformError))
        return luaL_error(state, "draw.imageFit: %s",
            transformError.c_str());
    auto* d2d = GetLuaDrawContext(state);
    if (!d2d || !d2d->services) return 0;
    D2D1_SIZE_F originalSize{};
    ID2D1Bitmap1* bitmap = d2d->services->resolveImage(
        state, 1, "draw.imageFit", width, height, fit, &originalSize,
        interpolation == "nearest");
    if (!bitmap || !d2d->ctx) return 0;
    const D2D1_SIZE_F sourceSize = bitmap->GetSize();
    const auto placement = snowdesktop::widget_runtime::
        ResolveDrawImagePlacement(originalSize.width, originalSize.height,
            { x, y, width, height }, fit, alignment);
    if (!placement.valid)
        return luaL_error(state,
            "draw.imageFit: dimensions must be positive and bounded");
    const D2D1_RECT_F destination = D2D1::RectF(
        d2d->widgetRect.left + placement.destination.x,
        d2d->widgetRect.top + placement.destination.y,
        d2d->widgetRect.left + placement.destination.x +
            placement.destination.width,
        d2d->widgetRect.top + placement.destination.y +
            placement.destination.height);
    const D2D1_RECT_F source = D2D1::RectF(
        placement.source.x * sourceSize.width / originalSize.width,
        placement.source.y * sourceSize.height / originalSize.height,
        (placement.source.x + placement.source.width) * sourceSize.width / originalSize.width,
        (placement.source.y + placement.source.height) * sourceSize.height / originalSize.height);
    ComPtr<ID2D1RoundedRectangleGeometry> roundedClip;
    if (cornerRadius > 0.0f)
    {
        ComPtr<ID2D1Factory> factory;
        d2d->ctx->GetFactory(&factory);
        const float clipRadius = std::min(cornerRadius,
            std::min(placement.destination.width,
                placement.destination.height) * 0.5f);
        if (!factory || FAILED(factory->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(destination, clipRadius, clipRadius),
                &roundedClip)) || !roundedClip)
            return luaL_error(state,
                "draw.imageFit: cannot create the rounded clip");
    }
    if (roundedClip)
        d2d->ctx->PushLayer(D2D1::LayerParameters(
            destination, roundedClip.Get(),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), nullptr);
    D2D1_MATRIX_3X2_F previousTransform{};
    const bool rotated = std::abs(rotationDegrees) > 0.0001f;
    if (rotated)
    {
        d2d->ctx->GetTransform(&previousTransform);
        const D2D1_POINT_2F origin = D2D1::Point2F(
            destination.left +
                (destination.right - destination.left) * originX,
            destination.top +
                (destination.bottom - destination.top) * originY);
        d2d->ctx->SetTransform(
            D2D1::Matrix3x2F::Rotation(rotationDegrees, origin) *
            previousTransform);
    }
    const auto sampled = d2d->imageSamplingCache.Resolve(d2d->ctx, bitmap,
        destination, source, interpolation == "nearest");
    d2d->ctx->DrawBitmap(sampled.bitmap.Get(), destination, alpha,
        sampled.interpolation, sampled.source);
    if (rotated)
        d2d->ctx->SetTransform(previousTransform);
    if (roundedClip)
        d2d->ctx->PopLayer();
    return 0;
}

int lua_DrawLine(lua_State* L)
{
    float x1 = (float)luaL_checknumber(L, 1);
    float y1 = (float)luaL_checknumber(L, 2);
    float x2 = (float)luaL_checknumber(L, 3);
    float y2 = (float)luaL_checknumber(L, 4);
    float thick = (float)luaL_optnumber(L, 5, 1);
    int color = (int)luaL_optinteger(L, 6, 0xFFFFFF);
    float alpha = (float)luaL_optnumber(L, 7, 1.0);

    auto* s = GetLuaDrawContext(L);
    if (!s || !s->ctx) return 0;

    ID2D1SolidColorBrush* brush = GetCachedBrush(s, color, alpha);
    if (!brush) return 0;

    s->ctx->DrawLine(
        D2D1::Point2F(x1 + s->widgetRect.left, y1 + s->widgetRect.top),
        D2D1::Point2F(x2 + s->widgetRect.left, y2 + s->widgetRect.top),
        brush, thick);
    return 0;
}

int lua_DrawCircle(lua_State* L)
{
    float cx = (float)luaL_checknumber(L, 1);
    float cy = (float)luaL_checknumber(L, 2);
    float r = (float)luaL_checknumber(L, 3);
    int color = (int)luaL_optinteger(L, 4, 0xFFFFFF);
    float alpha = (float)luaL_optnumber(L, 5, 1.0);

    auto* s = GetLuaDrawContext(L);
    if (!s || !s->ctx) return 0;

    ID2D1SolidColorBrush* brush = GetCachedBrush(s, color, alpha);
    if (!brush) return 0;

    s->ctx->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(cx + s->widgetRect.left, cy + s->widgetRect.top), r, r),
        brush);
    return 0;
}

static int LuaDrawFontGlyph(lua_State* L, bool fluent)
{
    const char* glyph = luaL_checkstring(L, 1);
    float x = static_cast<float>(luaL_checknumber(L, 2));
    float y = static_cast<float>(luaL_checknumber(L, 3));
    float size = static_cast<float>(luaL_optnumber(L, 4, 20));
    int color = static_cast<int>(luaL_optinteger(L, 5, 0xFFFFFF));

    auto* s = GetLuaDrawContext(L);
    if (!s || !s->ctx || !s->dwrite) return 0;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, glyph, -1, nullptr, 0);
    if (wlen <= 1) return 0;
    std::wstring wtext(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, glyph, -1, wtext.data(), wlen);

    IDWriteTextFormat* format = GetCachedTextFormat(s, size,
        DWRITE_FONT_WEIGHT_NORMAL, true, DWRITE_WORD_WRAPPING_NO_WRAP,
        !fluent, false, fluent);
    if (!format) return 0;

    ID2D1SolidColorBrush* brush = GetCachedBrush(s, color);
    if (!brush) return 0;

    float bx = x + s->widgetRect.left;
    float by = y + s->widgetRect.top;

    // Use a layout box much larger than size so DirectWrite centering has room
    // to work even when the glyph's advance width equals the font size (1em).
    // The box is centered on the target rect's center.
    const float box = size * 4.0f;
    const float boxX = bx + size * 0.5f - box * 0.5f;
    const float boxY = by + size * 0.5f - box * 0.5f;

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(s->dwrite->CreateTextLayout(wtext.c_str(),
            static_cast<UINT32>(wtext.size() - 1), format, box, box, &layout)) || !layout)
    {
        D2D1_RECT_F rect = { bx, by, bx + size, by + size };
        s->ctx->DrawTextW(wtext.c_str(), static_cast<UINT32>(wtext.size() - 1),
            format, &rect, brush);
        return 0;
    }

    // GetOverhangMetrics returns how far the drawn pixels extend beyond each
    // edge of the layout box. Positive = overhang, negative = gap (inside).
    // Visual center offset from box center = (overhang.right - overhang.left) / 2.
    // Shift the drawing origin to align the visual center with the target center.
    DWRITE_OVERHANG_METRICS overhang{};
    float drawX = boxX;
    float drawY = boxY;
    if (SUCCEEDED(layout->GetOverhangMetrics(&overhang)))
    {
        drawX -= (overhang.right - overhang.left) * 0.5f;
        drawY -= (overhang.bottom - overhang.top) * 0.5f;
    }

    s->ctx->DrawTextLayout(D2D1::Point2F(drawX, drawY), layout.Get(), brush);
    return 0;
}

int lua_DrawFa(lua_State* L)
{
    return LuaDrawFontGlyph(L, false);
}

int lua_DrawFluent(lua_State* L)
{
    return LuaDrawFontGlyph(L, true);
}
}

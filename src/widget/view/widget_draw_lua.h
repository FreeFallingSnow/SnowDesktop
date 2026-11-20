#pragma once

#include "widget_draw_context.h"

#include <optional>
#include <string>
#include <vector>

struct lua_State;

namespace snowdesktop::widget_runtime
{
enum class DrawImageFit;
struct NativeMarqueeText;

struct LuaDrawImageResult
{
    ID2D1Bitmap1* bitmap = nullptr;
    const char* error = nullptr;
};

// The host owns resource lookup and marquee submission. These operations keep
// their original Lua instance context and return the same borrowed resources.
struct LuaDrawHostServices
{
    std::optional<std::wstring> (*resolveFont)(lua_State*, int);
    LuaDrawImageResult (*resolveImage)(lua_State*, int, float, float,
        DrawImageFit, D2D1_SIZE_F*, bool);
    bool (*submitMarquee)(lua_State*, NativeMarqueeText&&);
};

struct LuaDrawContext : WidgetDrawContext
{
    const LuaDrawHostServices* services = nullptr;
    bool backgroundLayerActive = false;
    std::vector<D2D1_RECT_F> immediateClipRects;
};

LuaDrawContext* GetLuaDrawContext(lua_State* state);
void BindLuaDrawContext(lua_State* state, LuaDrawContext* context,
    const LuaDrawHostServices& services);

int lua_DrawText(lua_State* state);
int lua_MeasureText(lua_State* state);
int lua_DrawRect(lua_State* state);
int lua_DrawPushClip(lua_State* state);
int lua_DrawPopClip(lua_State* state);
int lua_DrawStrokeRect(lua_State* state);
int lua_DrawMarqueeText(lua_State* state);
int lua_DrawArc(lua_State* state);
int lua_DrawPath(lua_State* state);
int lua_DrawGradientRect(lua_State* state);
int lua_DrawShadow(lua_State* state);
int lua_DrawSparkline(lua_State* state);
int lua_DrawImage(lua_State* state);
int lua_DrawImageFit(lua_State* state);
int lua_DrawLine(lua_State* state);
int lua_DrawCircle(lua_State* state);
int lua_DrawFa(lua_State* state);
int lua_DrawFluent(lua_State* state);
int lua_DrawGlyph(lua_State* state);
}

#include "widget_outline.h"

#include "widget_draw_lua.h"
#include "theme/flat_glass_rim.h"
#include "widget/preview/widget_preview_stage.h"
#include "widget/runtime/lua_runtime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <utility>

extern "C" {
#include <lua.h>
}

namespace snowdesktop::widget_runtime
{
using Microsoft::WRL::ComPtr;

namespace
{
constexpr float kOutlineTolerance = 0.0001f;

bool Normalized(DrawPoint point)
{
    return point.x >= 0.0f && point.x <= 1.0f &&
        point.y >= 0.0f && point.y <= 1.0f;
}

bool ValidateOutline(const std::vector<DrawPathCommand>& path, std::string& error)
{
    using Type = DrawPathCommandType;
    bool open = false;
    bool segment = false;
    for (const auto& cmd : path)
    {
        switch (cmd.type)
        {
        case Type::Move:
            if (open)
            {
                error = "each outline contour must end with close";
                return false;
            }
            open = true;
            segment = false;
            break;
        case Type::Line:
        case Type::Quadratic:
        case Type::Cubic:
            segment = true;
            break;
        case Type::Close:
            if (!segment)
            {
                error = "each outline contour needs a drawable segment";
                return false;
            }
            open = false;
            break;
        }
        if (!Normalized(cmd.point) || !Normalized(cmd.control1) ||
            !Normalized(cmd.control2))
        {
            error = "outline coordinates must be between 0 and 1";
            return false;
        }
    }
    if (!open) return true;
    error = "each outline contour must end with close";
    return false;
}

bool ReadOutlineCondition(lua_State* state, int index,
    WidgetOutlineCondition& condition, std::string& error)
{
    if (!lua_istable(state, index))
    {
        error = "outline when must be a table";
        return false;
    }
    if (!::snowdesktop::lua_runtime::LuaTableUsesOnlyFields(state, index,
            { "setting", "value" }, "outline when", error))
        return false;

    index = lua_absindex(state, index);
    const auto read = [&](const char* field, std::size_t limit, std::string& out) {
        lua_pushstring(state, field);
        lua_rawget(state, index);
        std::size_t length = 0;
        const char* value = lua_type(state, -1) == LUA_TSTRING
            ? lua_tolstring(state, -1, &length) : nullptr;
        const bool valid = value && length > 0 && length <= limit &&
            std::string_view(value, length).find('\0') == std::string_view::npos;
        if (valid) out.assign(value, length);
        lua_pop(state, 1);
        return valid;
    };
    if (read("setting", 128, condition.setting) && read("value", 1024, condition.value))
        return true;
    error = "outline when requires nonempty setting and value strings (up to 128 and 1024 bytes)";
    return false;
}

ComPtr<ID2D1PathGeometry> BuildOutline(
    ID2D1Factory* factory, const WidgetOutline& outline)
{
    const auto source = BuildDrawPathGeometry(factory, outline.commands,
        outline.fillMode, {}, true);
    if (!source) return nullptr;

    ComPtr<ID2D1PathGeometry> boundary;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(&boundary)) ||
        FAILED(boundary->Open(&sink)))
        return nullptr;

    // Draw the resolved fill boundary, including holes, with overlapping
    // contours contributing only their visible exterior edges.
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    if (FAILED(source->Outline(nullptr, kOutlineTolerance, sink.Get())) ||
        FAILED(sink->Close()))
        return nullptr;
    float area = 0.0f;
    if (FAILED(boundary->ComputeArea(nullptr, kOutlineTolerance, &area)) ||
        area <= 0.0f)
        return nullptr;
    return boundary;
}
}

bool ReadWidgetOutline(lua_State* state, int descriptor, WidgetOutline& outline,
    std::string& error)
{
    const ::snowdesktop::lua_runtime::StackGuard stack(state);
    descriptor = lua_absindex(state, descriptor);
    lua_pushliteral(state, "outline");
    lua_rawget(state, descriptor);
    if (lua_isnil(state, -1)) return true;
    if (!lua_istable(state, -1))
    {
        error = "outline must be a table";
        return false;
    }
    if (!::snowdesktop::lua_runtime::LuaTableUsesOnlyFields(state, -1,
            { "path", "fillRule", "fit", "when" }, "outline", error))
        return false;

    lua_pushliteral(state, "fillRule");
    lua_rawget(state, -2);
    if (!lua_isnil(state, -1))
    {
        std::size_t length = 0;
        const char* text = lua_type(state, -1) == LUA_TSTRING
            ? lua_tolstring(state, -1, &length) : nullptr;
        const std::string_view rule = text ? std::string_view(text, length) : std::string_view{};
        if (rule == "winding") outline.fillMode = D2D1_FILL_MODE_WINDING;
        else if (rule != "alternate")
        {
            error = "outline fillRule must be alternate or winding";
            return false;
        }
    }
    lua_pop(state, 1);
    lua_pushliteral(state, "fit");
    lua_rawget(state, -2);
    if (!lua_isnil(state, -1))
    {
        std::size_t length = 0;
        const char* text = lua_type(state, -1) == LUA_TSTRING
            ? lua_tolstring(state, -1, &length) : nullptr;
        const auto fit = text ? std::string_view(text, length) : std::string_view{};
        if (fit == "contain") outline.contain = true;
        else if (fit != "fill")
        {
            error = "outline fit must be fill or contain";
            return false;
        }
    }
    lua_pop(state, 1);
    lua_pushliteral(state, "when");
    lua_rawget(state, -2);
    if (!lua_isnil(state, -1))
    {
        WidgetOutlineCondition condition;
        if (!ReadOutlineCondition(state, -1, condition, error))
            return false;
        outline.when = std::move(condition);
    }
    lua_pop(state, 1);
    lua_pushliteral(state, "path");
    lua_rawget(state, -2);
    return ReadLuaDrawPath(state, -1, outline.commands, error) &&
        ValidateOutline(outline.commands, error);
}

WidgetOutlineGeometry WidgetOutline::Resolve(ID2D1Factory* factory, D2D1_RECT_F frame)
{
    if (!factory || commands.empty() ||
        frame.right <= frame.left || frame.bottom <= frame.top)
        return {};
    if (unit)
    {
        ComPtr<ID2D1Factory> owner;
        unit->GetFactory(&owner);
        if (owner.Get() != factory)
        {
            unit.Reset();
            local.Reset();
            positioned.Reset();
            unavailable = false;
        }
    }
    if (unavailable) return {};
    if (!unit)
    {
        unit = BuildOutline(factory, *this);
        if (!unit)
        {
            unavailable = true;
            return {};
        }
    }
    const float width = frame.right - frame.left;
    const float height = frame.bottom - frame.top;
    const float fittedWidth = contain ? std::min(width, height) : width;
    const float fittedHeight = contain ? fittedWidth : height;
    if (!local || bounds.right - bounds.left != width ||
        bounds.bottom - bounds.top != height)
    {
        local.Reset();
        positioned.Reset();
        const auto fit = D2D1::Matrix3x2F::Scale(fittedWidth, fittedHeight) *
            D2D1::Matrix3x2F::Translation((width - fittedWidth) * .5f,
                (height - fittedHeight) * .5f);
        if (FAILED(factory->CreateTransformedGeometry(unit.Get(), &fit, &local)))
            return {};
    }
    if (!positioned || bounds.left != frame.left || bounds.top != frame.top)
    {
        positioned.Reset();
        const auto translation = D2D1::Matrix3x2F::Translation(frame.left, frame.top);
        if (FAILED(factory->CreateTransformedGeometry(local.Get(), &translation, &positioned)))
            return {};
    }
    bounds = frame;
    return {local, positioned};
}

void DrawWidgetOutlineBorder(ID2D1DeviceContext* context, ID2D1Geometry* outline,
    D2D1_COLOR_F color, float width)
{
    ComPtr<ID2D1SolidColorBrush> brush;
    if (color.a > 0.0f &&
        SUCCEEDED(context->CreateSolidColorBrush(color, &brush)))
        context->DrawGeometry(outline, brush.Get(), width);
}

bool DrawWidgetOutlineHighlight(ID2D1DeviceContext* context,
    ID2D1Geometry* outline, D2D1_RECT_F frame, D2D1_COLOR_F fill, float width,
    float strength, const EdgeLightSettings& settings)
{
    if (strength <= 0.0005f || width <= 0.0f) return false;
    const flat_glass_rim::Evaluator light(settings);
    const auto reflected = widget_preview::ResolveEdgeHighlightReflection(fill, strength);
    const float centerX = (frame.left + frame.right) * .5f;
    const float centerY = (frame.top + frame.bottom) * .5f;
    const float dx = light.directionX * (frame.right - frame.left) * .5f;
    const float dy = light.directionY * (frame.bottom - frame.top) * .5f;
    const auto color = [&](float alpha) {
        auto result = reflected;
        result.a = std::clamp(alpha, 0.0f, 1.0f) * strength;
        return result;
    };
    const auto& material = light.settings;
    const std::array<D2D1_GRADIENT_STOP, 3> stops{{
        {0.0f, color(material.ambient + material.opposite)},
        {0.5f, color(material.ambient)},
        {1.0f, color(material.ambient + material.primary)},
    }};
    ComPtr<ID2D1GradientStopCollection> colors;
    ComPtr<ID2D1LinearGradientBrush> brush;
    if (FAILED(context->CreateGradientStopCollection(stops.data(),
            static_cast<UINT32>(stops.size()), &colors)) ||
        FAILED(context->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(centerX - dx, centerY - dy),
            D2D1::Point2F(centerX + dx, centerY + dy)), colors.Get(), &brush)))
        return false;
    if (material.glowStrength > 0.0f && material.innerGlow > 1.0f)
    {
        brush->SetOpacity(material.glowStrength * .5f);
        context->DrawGeometry(outline, brush.Get(), width * 2.0f * material.innerGlow);
        brush->SetOpacity(1.0f);
    }
    context->DrawGeometry(outline, brush.Get(), width * 2.0f);
    return true;
}
}

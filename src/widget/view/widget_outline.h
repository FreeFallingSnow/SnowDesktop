#pragma once

#include "widget_draw_geometry.h"

#include <d2d1_1.h>
#include <wrl/client.h>

#include <string>
#include <vector>

struct lua_State;

namespace snowdesktop
{
struct EdgeLightSettings;
}

namespace snowdesktop::widget_runtime
{
struct WidgetOutlineGeometry
{
    Microsoft::WRL::ComPtr<ID2D1Geometry> local;
    Microsoft::WRL::ComPtr<ID2D1Geometry> positioned;
};

struct WidgetOutline
{
    std::vector<DrawPathCommand> commands;
    D2D1_FILL_MODE fillMode = D2D1_FILL_MODE_ALTERNATE;
    bool contain = false;

    // One immutable filled outline, plus transforms for painting and the backdrop.
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> unit;
    Microsoft::WRL::ComPtr<ID2D1TransformedGeometry> local;
    Microsoft::WRL::ComPtr<ID2D1TransformedGeometry> positioned;
    D2D1_RECT_F bounds{};
    bool unavailable = false;

    WidgetOutlineGeometry Resolve(ID2D1Factory* factory, D2D1_RECT_F frame);
};

bool ReadWidgetOutline(lua_State* state, int descriptor, WidgetOutline& outline,
    std::string& error);

void DrawWidgetOutlineBorder(ID2D1DeviceContext* context, ID2D1Geometry* outline,
    D2D1_COLOR_F color, float width);

bool DrawWidgetOutlineHighlight(ID2D1DeviceContext* context,
    ID2D1Geometry* outline, D2D1_RECT_F frame, D2D1_COLOR_F fill, float width,
    float strength, const EdgeLightSettings& settings);
}

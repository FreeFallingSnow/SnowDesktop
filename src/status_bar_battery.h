#pragma once
// Microsoft Fluent System Icons: Battery Charge 20 Filled, MIT.
// Copyright (c) Microsoft Corporation. All rights reserved.
// Unmodified source geometry from 21d5d02f724be2aaf586564775fff73a18a76eb6:
// assets/Battery Charge/SVG/ic_fluent_battery_charge_20_filled.svg
// License: third_party/fluentui-system-icons/LICENSE. No Filled font is shipped.
#include <d2d1.h>
#include <wrl/client.h>
namespace snowdesktop
{
inline constexpr wchar_t ChargingBatteryPath[] = L"M6.2334 6.17578C5.81383 7.00817 6.41657 7.99587 7.35352 7.9961H8V9.66602C8.00001 11.0629 9.88418 11.5187 10.5176 10.2666L12.7656 5.82031C12.902 5.54975 12.9288 5.2637 12.873 5H16C17.6569 5 19 6.34315 19 8C19.5523 8 20 8.44772 20 9V11C20 11.5523 19.5523 12 19 12L18.9961 12.1543C18.9158 13.7394 17.6051 15 16 15H3C1.34315 15 7.02247e-06 13.6569 0 12V8C6.44266e-08 6.34315 1.34315 5 3 5H6.82812L6.2334 6.17578ZM9.37305 2.18164C9.52978 1.87172 9.99775 1.98294 9.99805 2.33008V5.00098H11.6445C11.8346 5.00098 11.9578 5.20151 11.8721 5.3711L9.62402 9.81641C9.46715 10.1262 9.00001 10.0143 9 9.66699V6.99707H7.35254C7.16285 6.99684 7.03973 6.79635 7.125 6.62696L9.37305 2.18164Z";
inline Microsoft::WRL::ComPtr<ID2D1PathGeometry> CreateChargingBatteryGeometry(ID2D1Factory* factory)
{
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if (!factory || FAILED(factory->CreatePathGeometry(&geometry)) || FAILED(geometry->Open(&sink))) return {};
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    sink->BeginFigure(D2D1::Point2F(6.2334f, 6.17578f), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(5.81383f, 7.00817f), D2D1::Point2F(6.41657f, 7.99587f), D2D1::Point2F(7.35352f, 7.9961f)));
    sink->AddLine(D2D1::Point2F(8.f, 7.9961f));
    sink->AddLine(D2D1::Point2F(8.f, 9.66602f));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(8.00001f, 11.0629f), D2D1::Point2F(9.88418f, 11.5187f), D2D1::Point2F(10.5176f, 10.2666f)));
    sink->AddLine(D2D1::Point2F(12.7656f, 5.82031f));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(12.902f, 5.54975f), D2D1::Point2F(12.9288f, 5.2637f), D2D1::Point2F(12.873f, 5.f)));
    sink->AddLine(D2D1::Point2F(16.f, 5.f));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(17.6569f, 5.f), D2D1::Point2F(19.f, 6.34315f), D2D1::Point2F(19.f, 8.f)));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(19.5523f, 8.f), D2D1::Point2F(20.f, 8.44772f), D2D1::Point2F(20.f, 9.f)));
    sink->AddLine(D2D1::Point2F(20.f, 11.f));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(20.f, 11.5523f), D2D1::Point2F(19.5523f, 12.f), D2D1::Point2F(19.f, 12.f)));
    sink->AddLine(D2D1::Point2F(18.9961f, 12.1543f));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(18.9158f, 13.7394f), D2D1::Point2F(17.6051f, 15.f), D2D1::Point2F(16.f, 15.f)));
    sink->AddLine(D2D1::Point2F(3.f, 15.f));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(1.34315f, 15.f), D2D1::Point2F(7.02247e-06f, 13.6569f), D2D1::Point2F(0.f, 12.f)));
    sink->AddLine(D2D1::Point2F(0.f, 8.f));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(6.44266e-08f, 6.34315f), D2D1::Point2F(1.34315f, 5.f), D2D1::Point2F(3.f, 5.f)));
    sink->AddLine(D2D1::Point2F(6.82812f, 5.f));
    sink->AddLine(D2D1::Point2F(6.2334f, 6.17578f));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->BeginFigure(D2D1::Point2F(9.37305f, 2.18164f), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(9.52978f, 1.87172f), D2D1::Point2F(9.99775f, 1.98294f), D2D1::Point2F(9.99805f, 2.33008f)));
    sink->AddLine(D2D1::Point2F(9.99805f, 5.00098f));
    sink->AddLine(D2D1::Point2F(11.6445f, 5.00098f));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(11.8346f, 5.00098f), D2D1::Point2F(11.9578f, 5.20151f), D2D1::Point2F(11.8721f, 5.3711f)));
    sink->AddLine(D2D1::Point2F(9.62402f, 9.81641f));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(9.46715f, 10.1262f), D2D1::Point2F(9.00001f, 10.0143f), D2D1::Point2F(9.f, 9.66699f)));
    sink->AddLine(D2D1::Point2F(9.f, 6.99707f));
    sink->AddLine(D2D1::Point2F(7.35254f, 6.99707f));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(7.16285f, 6.99684f), D2D1::Point2F(7.03973f, 6.79635f), D2D1::Point2F(7.125f, 6.62696f)));
    sink->AddLine(D2D1::Point2F(9.37305f, 2.18164f));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return {};
    return geometry;
}
}

#include "pch.h"
#include "system_panel_surface.h"
#include "ui/render/smooth_corner_geometry.h"
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <cmath>
namespace snowdesktop::winui
{
namespace x = winrt::Microsoft::UI::Xaml;
namespace c = x::Controls;
namespace m = x::Media;
namespace
{
winrt::Windows::UI::Color Color(float r, float g, float b, float alpha = 1)
{
    const auto byte = [](float value) { return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.f, 1.f) * 255)); };
    return {byte(alpha), byte(r), byte(g), byte(b)};
}

struct XamlCornerSink
{
    m::PathFigure figure;
    static winrt::Windows::Foundation::Point Point(D2D1_POINT_2F point) { return {point.x, point.y}; }
    void SetFillMode(D2D1_FILL_MODE) {}
    void BeginFigure(D2D1_POINT_2F point, D2D1_FIGURE_BEGIN) { figure.StartPoint(Point(point)); }
    void AddLine(D2D1_POINT_2F point) { m::LineSegment segment; segment.Point(Point(point)); figure.Segments().Append(segment); }
    void AddBezier(const D2D1_BEZIER_SEGMENT& value)
    {
        m::BezierSegment segment; segment.Point1(Point(value.point1)); segment.Point2(Point(value.point2));
        segment.Point3(Point(value.point3)); figure.Segments().Append(segment);
    }
    void AddArc(const D2D1_ARC_SEGMENT& value)
    {
        m::ArcSegment segment; segment.Point(Point(value.point)); segment.Size({value.size.width, value.size.height});
        segment.SweepDirection(m::SweepDirection::Clockwise); segment.IsLargeArc(false); figure.Segments().Append(segment);
    }
    void EndFigure(D2D1_FIGURE_END) { figure.IsClosed(true); }
};
m::PathGeometry SmoothPath(float width, float height, float radius, float inset = 0)
{
    XamlCornerSink sink;
    smooth_corners::Trace(&sink, D2D1::RectF(inset, inset, width-inset, height-inset), std::max(0.f, radius-inset));
    m::PathGeometry path; path.Figures().Append(sink.figure); return path;
}

void UseSmoothPanelFrame(const c::Border& frame, float radius)
{
    // Keep a real XAML path in the tree so live and RenderTargetBitmap previews
    // share the contour. Composition-only clips are absent from bitmap export.
    frame.Loaded([radius](const auto& sender, const auto&) {
        const auto panel = sender.template as<c::Border>();
        const auto key = winrt::box_value(L"SnowDesktopSmoothPanel");
        if (panel.Resources().HasKey(key)) return;
        panel.Resources().Insert(key, winrt::box_value(true));
        const auto original = panel.Child(); panel.Child(nullptr);
        const auto border = panel.BorderThickness(), padding = panel.Padding();
        c::Border content; content.Padding({padding.Left+border.Left, padding.Top+border.Top,
            padding.Right+border.Right, padding.Bottom+border.Bottom}); content.Child(original);
        winrt::Microsoft::UI::Xaml::Shapes::Path fill, stroke;
        fill.Fill(panel.Background()); stroke.Stroke(panel.BorderBrush()); stroke.StrokeThickness(border.Left);
        fill.IsHitTestVisible(false); stroke.IsHitTestVisible(false);
        c::Canvas canvas; canvas.Children().Append(fill); canvas.Children().Append(stroke);
        c::Grid grid; grid.Children().Append(canvas); grid.Children().Append(content);
        panel.Background(nullptr); panel.BorderBrush(nullptr); panel.BorderThickness({0}); panel.Padding({0});
        panel.CornerRadius({0}); panel.Child(grid);
        const auto update = [weak = winrt::make_weak(panel), fill, stroke, radius, border] {
            if (const auto current = weak.get())
            {
                const float width = static_cast<float>(current.ActualWidth()), height = static_cast<float>(current.ActualHeight());
                if (width <= 0 || height <= 0) return;
                fill.Data(SmoothPath(width, height, radius));
                stroke.Data(SmoothPath(width, height, radius, static_cast<float>(border.Left)*.5f));
            }
        };
        panel.SizeChanged([update](const auto&, const auto&) { update(); }); update();
    });
}
}
c::Border CreateSystemPanelFrame(const PersonalizationSettings& appearance)
{
        c::Border frame; frame.Padding({16, 16, 16, 16});
        const double radius = appearance.cornerRadius;
        frame.CornerRadius({radius, radius, radius, radius});
        frame.RequestedTheme(appearance.contentTheme == 1 ? x::ElementTheme::Light : x::ElementTheme::Dark);
        HIGHCONTRASTW hc{sizeof(hc)}; SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0);
        const bool highContrast = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
        if (highContrast)
        {
            const auto color = GetSysColor(COLOR_WINDOW);
            frame.Background(m::SolidColorBrush(Color(GetRValue(color) / 255.f, GetGValue(color) / 255.f, GetBValue(color) / 255.f)));
        }
        else if (appearance.panelGradient.enabled)
        {
            m::LinearGradientBrush brush;
            const auto line = ResolvePanelGradientLine(appearance.panelGradient, 480, 560);
            brush.StartPoint({static_cast<float>(line.x1 / 480), static_cast<float>(line.y1 / 560)});
            brush.EndPoint({static_cast<float>(line.x2 / 480), static_cast<float>(line.y2 / 560)});
            for (const auto& value : appearance.panelGradient.stops)
            {
                m::GradientStop stop; stop.Offset(value.position);
                stop.Color(Color(((value.color >> 16) & 255) / 255.f, ((value.color >> 8) & 255) / 255.f,
                    (value.color & 255) / 255.f, static_cast<float>(value.opacity) * appearance.widgetAlpha));
                brush.GradientStops().Append(stop);
            }
            frame.Background(brush);
        }
        else frame.Background(m::SolidColorBrush(Color(appearance.widgetBgR, appearance.widgetBgG, appearance.widgetBgB, appearance.widgetAlpha)));
        if (!highContrast)
        {
            frame.BorderBrush(m::SolidColorBrush(Color(appearance.widgetBorderR, appearance.widgetBorderG, appearance.widgetBorderB, appearance.widgetBorderAlpha)));
            const double width = appearance.widgetBorderWidth;
            frame.BorderThickness({width, width, width, width});
        }
    UseSmoothPanelFrame(frame, appearance.cornerRadius);
    return frame;
}
bool UpdateSystemPanelRegion(HWND window, int width, int height, double radius, int offsetY, std::span<const RECT> cards)
{
    const RECT whole{0, 0, width, height};
    if (cards.empty()) cards = std::span<const RECT>(&whole, 1);
    HRGN region = CreateRectRgn(0, 0, 0, 0);
    if (!region) return false;
    for (const auto& card : cards)
    {
        const int diameter = std::clamp(static_cast<int>(std::lround(radius * 2)), 0,
            static_cast<int>((std::min)(card.right - card.left, card.bottom - card.top)));
        const auto part = diameter > 0 ? CreateRoundRectRgn(card.left, card.top, card.right + 1, card.bottom + 1, diameter, diameter) :
            CreateRectRgn(card.left, card.top, card.right, card.bottom);
        if (!part) { DeleteObject(region); return false; }
        CombineRgn(region, region, part, RGN_OR); DeleteObject(part);
    }
    OffsetRgn(region, 0, offsetY);
    const auto clip = CreateRectRgn(0, 0, width, height);
    if (!clip) { DeleteObject(region); return false; }
    CombineRgn(region, region, clip, RGN_AND); DeleteObject(clip);
    if (SetWindowRgn(window, region, FALSE)) return true; // User32 owns the region.
    DeleteObject(region); return false;
}
}

#include "pch.h"
#include "system_panel_surface.h"
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
    return frame;
}
bool UpdateSystemPanelRegion(HWND window, int width, int height, double radius)
{
    const int diameter = std::clamp(static_cast<int>(std::lround(radius * 2)), 0, (std::min)(width, height));
    HRGN region = diameter > 0 ? CreateRoundRectRgn(0, 0, width + 1, height + 1, diameter, diameter) :
        CreateRectRgn(0, 0, width, height);
    if (!region) return false;
    if (SetWindowRgn(window, region, FALSE)) return true; // User32 owns the region.
    DeleteObject(region); return false;
}
}

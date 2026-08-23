#include "app.h"
#include "../widget_preview_stage.h"

// Native glass border rendering and diagnostics.

bool DesktopApp::DrawGlassBorder(ID2D1DeviceContext* ctx, RECT frame,
    float radius, D2D1_COLOR_F color, float strokeWidth)
{
    return snowdesktop::widget_preview::DrawGlassBorder(
        ctx, frame, radius, color, strokeWidth);
}

/** @brief 返回设置界面显示的原生毛玻璃合成状态。 */
std::wstring DesktopApp::GetGlassBackendStatusText() const
{
    if (desktopBackdropCompositor_.IsAvailable())
    {
        std::wstring status = _LW("glass.dwm_enabled");
        status += _LW("glass.glass_panel");
        status += std::to_wstring(desktopBackdropCompositor_.PanelCount());
        status += _LW("glass.syncing");
        return status;
    }

    std::wstring status = _LW("glass.dwm_unavailable");
    if (!desktopBackdropCompositor_.LastError().empty())
    {
        status += L"：";
        status += desktopBackdropCompositor_.LastError();
    }
    return status;
}

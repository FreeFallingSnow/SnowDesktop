#pragma once
#include "../personalization.h"
#include <winrt/Microsoft.UI.Xaml.Controls.h>

namespace snowdesktop::winui
{
// Shared by the live popup and its offline visual-tree renderer.
winrt::Microsoft::UI::Xaml::Controls::Border CreateSystemPanelFrame(const PersonalizationSettings& appearance);
bool UpdateSystemPanelRegion(HWND window, int width, int height, double radius, int offsetY = 0);
}

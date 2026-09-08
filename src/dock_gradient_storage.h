#pragma once
#include "dock_settings.h"
#include <array>
#include <ostream>

namespace snowdesktop
{
// Optional fields in SnowDesktop.dock.json. Each scenario retains its own
// gradient, including the disabled gradient when the user selects solid fill.
inline constexpr std::array<const char*, 4> kTaskbarGradientKeys{
    "taskbarPanelGradient", "systemTaskbarVisibleWindowPanelGradient",
    "systemTaskbarMaximizedWindowPanelGradient", "systemTaskbarShellUiPanelGradient"};

template<class Settings> auto TaskbarGradients(Settings& settings)
{
    return std::array{&settings.systemTaskbarAppearance.panelGradient,
        &settings.systemTaskbarVisibleWindow.appearance.panelGradient,
        &settings.systemTaskbarMaximizedWindow.appearance.panelGradient,
        &settings.systemTaskbarShellUi.appearance.panelGradient};
}

inline bool ReadTaskbarGradients(const JsonValue& document, DockSettings& settings)
{
    std::array<PanelGradient, 4> decoded;
    for (size_t i = 0; i < decoded.size(); ++i)
        if (const auto* value = document.Find(kTaskbarGradientKeys[i]))
            if (!DecodePanelGradient(*value, decoded[i])) return false;
    auto destinations = TaskbarGradients(settings);
    for (size_t i = 0; i < decoded.size(); ++i) *destinations[i] = std::move(decoded[i]);
    return true;
}

inline bool WriteTaskbarGradients(std::ostream& output, const DockSettings& settings)
{
    const auto gradients = TaskbarGradients(settings);
    for (const auto* gradient : gradients)
        if (!ValidatePanelGradient(*gradient)) return false;
    for (size_t i = 0; i < gradients.size(); ++i)
        output << "  \"" << kTaskbarGradientKeys[i] << "\": " << EncodePanelGradient(*gradients[i]) << ",\n";
    return static_cast<bool>(output);
}
}

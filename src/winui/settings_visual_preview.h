#pragma once
#include <optional>

namespace snowdesktop::winui
{
// Internal diagnostic command. Creates only an isolated XAML render island;
// no DesktopApp, controller/store, IPC, device service or mutation callbacks.
std::optional<int> TryRunSettingsVisualPreview();
}

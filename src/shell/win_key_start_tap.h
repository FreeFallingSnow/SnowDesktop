#pragma once

#include <windows.h>

namespace snowdesktop
{
inline constexpr wchar_t kWinKeyStartTapMessage[] = L"SnowDesktop.WinKeyStart";

// Installs a low-level hook while enabled. A lone Windows-key tap is reported
// to the target window; Windows-key shortcuts continue through the system.
void SyncWinKeyStartTap(HWND target, bool enabled);
void StopWinKeyStartTap();
[[nodiscard]] bool ConsumeWinKeyStartTap() noexcept;
}

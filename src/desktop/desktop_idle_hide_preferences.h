#pragma once

namespace snowdesktop::desktop_idle_hide
{
inline constexpr int kDefaultDelaySeconds = 5;
inline constexpr int kMaxDelaySeconds = 3600;

int NormalizeDelay(int seconds) noexcept;
}

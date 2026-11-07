#include "desktop_idle_hide_preferences.h"

#include <algorithm>

namespace snowdesktop::desktop_idle_hide
{
int NormalizeDelay(int seconds) noexcept
{
    return std::clamp(seconds, 1, kMaxDelaySeconds);
}
}

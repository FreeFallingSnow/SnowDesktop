#pragma once

namespace snowdesktop
{
inline constexpr int kDefaultTooltipFontSize = 12;
inline constexpr int kMinimumTooltipFontSize = 10;
inline constexpr int kMaximumTooltipFontSize = 24;
int NormalizeTooltipFontSize(int value);
}

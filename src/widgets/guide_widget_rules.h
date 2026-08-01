#pragma once

namespace snowdesktop::guide_widget_rules
{

/**
 * @brief Guide 只在页面没有其他可见内容时充当空页占位。
 */
inline bool ShouldRemove(
    bool hasVisibleDesktopItem,
    bool hasStandaloneNonGuideWidget) noexcept
{
    return hasVisibleDesktopItem || hasStandaloneNonGuideWidget;
}

} // namespace snowdesktop::guide_widget_rules

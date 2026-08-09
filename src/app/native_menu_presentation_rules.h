#pragma once

namespace snowdesktop::native_menu_presentation_rules
{

/**
 * Native Shell menus run their own modal message loop. Presentation may be
 * flushed after an owner message unless a composition surface is still inside
 * BeginDraw/EndDraw.
 */
constexpr bool ShouldFlushAfterOwnerMessage(
    bool nativeMenuActive,
    bool desktopPaintInProgress,
    bool quickNavigationPaintInProgress,
    bool floatingDockPaintInProgress) noexcept
{
    return nativeMenuActive &&
        !desktopPaintInProgress &&
        !quickNavigationPaintInProgress &&
        !floatingDockPaintInProgress;
}

} // namespace snowdesktop::native_menu_presentation_rules

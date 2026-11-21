#pragma once

#include <windows.h>

namespace snowdesktop::floating_dock_rules
{

bool IsWindowAbove(HWND upper, HWND lower);

template <typename IsApplicationWindow, typename IsDockWindow>
HWND LowestDesktopLayerApplication(
    HWND referenceWindow, HWND desktopWindow, IsApplicationWindow&& isApplicationWindow,
    IsDockWindow&& isDockWindow, bool* scanComplete = nullptr)
{
    if (scanComplete) *scanComplete = true;
    // Show Desktop can leave application windows below Explorer. They
    // cannot anchor a Dock that must remain above the desktop.
    HWND current = desktopWindow ? GetWindow(desktopWindow, GW_HWNDPREV) :
        GetWindow(referenceWindow, GW_HWNDLAST);
    for (unsigned visited = 0; current && visited < 4096; ++visited)
    {
        if (current != referenceWindow && !isDockWindow(current) &&
            (GetWindowLongPtrW(current, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0 &&
            isApplicationWindow(current))
        {
            return current;
        }
        current = GetWindow(current, GW_HWNDPREV);
    }
    if (scanComplete) *scanComplete = current == nullptr;
    return nullptr;
}

// A dismissed Dock must be below applications, not merely non-topmost.
// Explorer's immediate predecessor can be a Shell/WinUI tool window whose
// placement is not a usable application-layer anchor. Prefer the lowest real
// application; use the desktop predecessor only when no application is present.
template <typename IsApplicationWindow, typename IsDockWindow>
HWND DesktopLayerInsertAfter(
    HWND contentWindow, HWND desktopWindow,
    IsApplicationWindow&& isApplicationWindow, IsDockWindow&& isDockWindow)
{
    // GW_HWNDLAST on a topmost Dock searches the topmost group. Explorer's
    // normal top-level window is the reference for ordinary application order.
    const HWND referenceWindow = desktopWindow && IsWindow(desktopWindow)
        ? desktopWindow : contentWindow;
    if (HWND application = LowestDesktopLayerApplication(
            referenceWindow, desktopWindow, isApplicationWindow, isDockWindow))
        return application;

    HWND anchor = desktopWindow && IsWindow(desktopWindow)
        ? GetWindow(desktopWindow, GW_HWNDPREV) : nullptr;
    for (unsigned visited = 0; anchor && isDockWindow(anchor) && visited < 4096; ++visited)
        anchor = GetWindow(anchor, GW_HWNDPREV);
    return anchor && IsWindow(anchor) && anchor != contentWindow &&
        !isDockWindow(anchor) &&
        (GetWindowLongPtrW(anchor, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0
        ? anchor : HWND_NOTOPMOST;
}

template <typename IsApplicationWindow, typename IsDockWindow>
bool IsDesktopLayerRestored(
    HWND contentWindow, HWND desktopWindow,
    IsApplicationWindow&& isApplicationWindow, IsDockWindow&& isDockWindow)
{
    if (!contentWindow || !IsWindow(contentWindow) ||
        (GetWindowLongPtrW(contentWindow, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0 ||
        !desktopWindow || !IsWindow(desktopWindow) ||
        !IsWindowAbove(contentWindow, desktopWindow))
        return false;
    bool scanComplete = false;
    const HWND application = LowestDesktopLayerApplication(
        contentWindow, desktopWindow, isApplicationWindow, isDockWindow, &scanComplete);
    return scanComplete && (!application || IsWindowAbove(application, contentWindow));
}

template <typename IsApplicationWindow, typename IsDockWindow, typename PlacePair>
bool RestoreDesktopLayer(
    HWND contentWindow, HWND desktopWindow,
    IsApplicationWindow&& isApplicationWindow, IsDockWindow&& isDockWindow,
    PlacePair&& placePair, HWND foregroundFallback = nullptr)
{
    if (!contentWindow || !IsWindow(contentWindow))
        return false;
    desktopWindow = desktopWindow && IsWindow(desktopWindow)
        ? GetAncestor(desktopWindow, GA_ROOT) : nullptr;
    // A target can disappear/change bands while User32 positions the pair.
    // Resolve again instead of accepting a band-only fallback as completion.
    for (unsigned attempt = 0; attempt < 2; ++attempt)
    {
        const HWND anchor = DesktopLayerInsertAfter(
            contentWindow, desktopWindow, isApplicationWindow, isDockWindow);
        if (placePair(anchor) && IsDesktopLayerRestored(
                contentWindow, desktopWindow, isApplicationWindow, isDockWindow))
            return true;
    }
    // A rejected/stale lowest anchor must not leave a dismissed Dock over the
    // application the user just activated. This is a partial placement only:
    // retain recovery until the complete desktop-order predicate also passes.
    if (foregroundFallback && IsWindow(foregroundFallback) &&
        foregroundFallback != contentWindow && !isDockWindow(foregroundFallback) &&
        (GetWindowLongPtrW(foregroundFallback, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0 &&
        (!desktopWindow || IsWindowAbove(foregroundFallback, desktopWindow)) &&
        isApplicationWindow(foregroundFallback) && placePair(foregroundFallback))
        return IsDesktopLayerRestored(
            contentWindow, desktopWindow, isApplicationWindow, isDockWindow);
    return false;
}

} // namespace snowdesktop::floating_dock_rules

#include "quick_navigation_animation_rules.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
void Check(bool condition, const char* message)
{
    if (condition)
        return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

bool NearlyEqual(float left, float right, float tolerance = 0.0001f)
{
    return std::fabs(left - right) <= tolerance;
}
}

int main()
{
    using namespace snowdesktop::quick_navigation_animation_rules;

    State state;
    Check(state.IsHidden(), "quick navigation starts hidden");

    state.Open(1000);
    Check(state.IsAnimating(), "open starts an animation");
    Check(state.IsInteractive(), "opening is immediately interactive");
    Check(state.GetVisual().visible,
        "opening window exists immediately so its search field can focus");
    state.Advance(1000 + kOpenDurationMs / 2);
    const Visual halfOpen = state.GetVisual();
    Check(halfOpen.visible, "half-open panel remains visible");
    Check(NearlyEqual(halfOpen.progress, 0.5f),
        "opening progress is time based");
    Check(NearlyEqual(halfOpen.opacity, 0.5f),
        "half-open opacity follows eased progress");
    Check(halfOpen.scale > kMinimumScale &&
          halfOpen.scale < 1.0f,
        "opening grows from the Dock icon scale");
    Check(kMinimumScale <= 0.08f,
        "hidden panel contracts to approximately the Dock search icon");
    Check(NearlyEqual(
            ScaleCoordinate(42.0f, 42.0f, halfOpen.scale),
            42.0f),
        "the Dock search icon anchor remains stationary");
    Check(ScaleCoordinate(
            442.0f, 42.0f, kMinimumScale) < 80.0f,
        "the panel edge contracts toward the Dock search icon");
    Check(ShouldRefreshCloseAnchor(
            AnchorMode::Pointer),
        "shortcut close follows the current pointer");
    Check(!ShouldRefreshCloseAnchor(
            AnchorMode::DockSearch),
        "Dock close keeps the search icon anchor");
    Check(NearlyEqual(
            ScaleCoordinate(
                640.0f, 1200.0f, 1.0f),
            640.0f),
        "changing the close pointer at full scale cannot move or flash the panel");

    const Visual beforeClose = state.GetVisual();
    state.Close(1000 + kOpenDurationMs / 2);
    Check(state.IsClosing(), "close can interrupt opening");
    Check(NearlyEqual(beforeClose.opacity, state.GetVisual().opacity),
        "interrupting open keeps the current visual frame");
    state.Advance(1000 + kOpenDurationMs / 2 + 20);
    const Visual closing = state.GetVisual();
    Check(closing.progress < beforeClose.progress,
        "interrupted close moves toward hidden");

    state.Open(1000 + kOpenDurationMs / 2 + 20);
    Check(state.IsInteractive(), "open can interrupt closing");
    Check(NearlyEqual(closing.scale, state.GetVisual().scale),
        "reopening keeps scale continuous");
    state.Advance(2000);
    Check(!state.IsAnimating(), "reopened animation completes");
    Check(NearlyEqual(state.GetVisual().opacity, 1.0f),
        "completed open is fully opaque");

    state.Close(3000);
    state.Advance(3000 + kCloseDurationMs);
    Check(state.IsHidden(), "close completes at hidden");
    Check(!state.IsAnimating(), "hidden animation stops");
    Check(kOpenDurationMs <= 140 && kCloseDurationMs <= 110,
        "quick navigation animation stays responsive");

    state.ShowImmediately();
    Check(NearlyEqual(state.GetVisual().scale, 1.0f),
        "disabled animations show at full scale");
    state.ResetHidden();
    Check(state.IsHidden(), "reset returns to hidden");

    std::cout << "quick navigation animation rules tests passed\n";
    return 0;
}

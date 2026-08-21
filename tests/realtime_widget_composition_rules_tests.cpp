#include "realtime_widget_composition_rules.h"

#include <cstdlib>
#include <iostream>

namespace rules = snowdesktop::realtime_widget_composition_rules;

namespace
{
void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}
}

int main()
{
    rules::SceneState stable;
    stable.compositionActive = true;
    stable.desktopSurfaceVisible = true;
    Check(rules::ShouldUseIndependentSurface(stable),
        "a visible stable realtime widget should use its child surface");

    auto inactive = stable;
    inactive.compositionActive = false;
    Check(!rules::ShouldUseIndependentSurface(inactive),
        "an inactive realtime surface must use the root surface");

    auto hidden = stable;
    hidden.desktopSurfaceVisible = false;
    Check(!rules::ShouldUseIndependentSurface(hidden),
        "a hidden widget must not present its child surface");

    auto CheckInteractionFallback = [&](auto member, const char* message) {
        auto state = stable;
        state.*member = true;
        Check(!rules::ShouldUseIndependentSurface(state), message);
    };
    CheckInteractionFallback(&rules::SceneState::dragActive,
        "internal drag must fall back to the root surface");
    CheckInteractionFallback(&rules::SceneState::externalDragActive,
        "external drag must fall back to the root surface");
    CheckInteractionFallback(&rules::SceneState::widgetPreviewActive,
        "move or resize preview must fall back to the root surface");
    CheckInteractionFallback(&rules::SceneState::desktopMarqueeActive,
        "desktop marquee must fall back to the root surface");
    CheckInteractionFallback(&rules::SceneState::popupActive,
        "popup overlap must fall back to the root surface");
    CheckInteractionFallback(&rules::SceneState::widgetPanelActive,
        "widget panel overlap must fall back to the root surface");

    std::cout << "realtime widget composition rules tests passed\n";
    return 0;
}

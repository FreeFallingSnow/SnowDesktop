#pragma once
#include "personalization_types.h"

namespace snowdesktop
{
struct SurfaceTheme
{
    // -2 preserves the old conditional override; -1 follows the global preset;
    // 0..3 are the existing four presets; 4 selects the independent appearance.
    // Status bars additionally opt into 5/6 for dark/light glass and 7/8 for
    // transparent backgrounds with dark/light text. Popup callers
    // retain their original four-preset codec and resolver.
    int mode = -1;
    bool customized = false;
    PersonalizationSettings appearance;
    friend bool operator==(const SurfaceTheme&, const SurfaceTheme&) = default;
};

}

#pragma once
#include "icon_beautify.h"

namespace snowdesktop::icon_beautify
{
// Supersample a preview using native pixel units for fixed-width material
// effects. Source dimensions include the scale; desktop Render stays at 1x.
std::vector<std::uint32_t> RenderPreview(
    const std::vector<std::uint32_t>& source, int width, int height,
    const IconBeautifySettings& settings, std::optional<PlateFill> detectedEdgeFill,
    int rasterScale, SourceKind sourceKind = SourceKind::Image);
}

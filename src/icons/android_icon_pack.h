#pragma once
#include "icon_pack_archive.h"
namespace snowdesktop::android_icon_pack
{
// Resolves compiled Android resources, then converts mapped drawable/adaptive
// icons into standalone SVG or raster assets. Monochrome is a separate variant.
bool Convert(icon_pack::Archive& archive, std::string& error);
}

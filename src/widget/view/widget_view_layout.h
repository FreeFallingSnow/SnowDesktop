#pragma once

#include <string>

namespace snowdesktop::widget_runtime
{
struct ViewNode;
struct ViewRect;

namespace detail
{
bool ResolveGridPlacements(ViewNode& root, std::string& error);
void LayoutViewTree(ViewNode& root, const ViewRect& frame);
}
}

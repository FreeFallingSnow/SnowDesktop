#pragma once

#include <string>

namespace snowdesktop::widget_runtime
{
struct ViewNode;

namespace detail
{
bool ValidateViewTreeStructure(const ViewNode& root, std::string& error);
bool ValidateAccessibilityMetadata(const ViewNode& root, std::string& error);
}
}

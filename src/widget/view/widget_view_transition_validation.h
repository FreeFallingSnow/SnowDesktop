#pragma once

#include <string>

namespace snowdesktop::widget_runtime
{
struct ViewNode;

bool ValidateViewNodeTransitions(const ViewNode& node, std::string& error);
}

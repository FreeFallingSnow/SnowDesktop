#pragma once

#include <string>

namespace snowdesktop::widget_runtime
{
// Serializes the public API v2 declarative-view contract for authoring tools.
// The JSON schema is versioned independently from the component API version.
std::string SerializeViewContractJson();
}

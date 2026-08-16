#pragma once

#include <string>

namespace snowdesktop::widget_api
{
// Serializes every host-provided Lua library function exposed to component
// sandboxes. The schema is versioned independently from apiVersion.
std::string SerializePublicApiContractJson();
}

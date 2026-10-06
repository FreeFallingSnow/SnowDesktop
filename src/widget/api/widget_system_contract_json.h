#pragma once

#include <string>

namespace snowdesktop::widget_api
{
// Serializes the API v2 system function, data-topic, and task catalogs for
// offline authoring tools. The export schema is versioned independently.
std::string SerializeSystemCapabilityContractJson();
}

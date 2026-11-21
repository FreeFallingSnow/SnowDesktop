#pragma once

#include <iosfwd>
#include <string_view>

namespace snowdesktop::widget_contract_json
{
void WriteJsonString(std::ostream& output, std::string_view value);
}

#pragma once

#include <string_view>

namespace snowdesktop::utf8
{
// Checks Unicode scalar encoding. U+0000 is a valid scalar.
bool IsValid(std::string_view value) noexcept;
}

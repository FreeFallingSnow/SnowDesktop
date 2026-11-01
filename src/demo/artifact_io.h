#pragma once
#include "platform/control/control_protocol.h"
#include <filesystem>

namespace snowdesktop::demo
{
std::string Sha256(const std::filesystem::path& file);
control::Value ReadJson(const std::filesystem::path& file, std::size_t maximum = 1024 * 1024);
void WriteJson(const std::filesystem::path& file, const control::Value& value);
std::string Utf8(std::wstring_view value);
std::wstring Wide(std::string_view value);
}

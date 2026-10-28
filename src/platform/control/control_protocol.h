#pragma once

#include "common/json_value.h"
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>

namespace snowdesktop::control
{
using Value = JsonValue;
inline constexpr std::size_t MaximumFrameBytes = 1024 * 1024;
inline constexpr unsigned ProtocolVersion = 1;

Value Object(std::initializer_list<std::pair<std::string, Value>> fields = {});
Value Array();
Value String(std::string value);
Value Number(double value);
Value Boolean(bool value);
std::string Serialize(const Value& value);

class Error : public std::runtime_error
{
public:
    Error(std::string code, std::string message, int rpcCode = -32000);
    std::string code;
    int rpcCode;
};

// No notifications or batches: every accepted operation has a correlated reply.
using Handler = std::function<Value(std::string_view, const Value&)>;
std::string Dispatch(std::string_view request, const Handler& handler) noexcept;
Value Describe();
void Fields(const Value& params, std::initializer_list<std::string_view> allowed);
std::string Text(const Value& params, std::string_view key, bool required = true);
int Integer(const Value& params, std::string_view key, int minimum, int maximum);
}

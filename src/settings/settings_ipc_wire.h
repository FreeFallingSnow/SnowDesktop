#pragma once

#include <cstddef>
#include <span>
#include <stdexcept>
#include <vector>

namespace snowdesktop::settings_ipc
{
inline constexpr std::size_t MaximumFrameBytes = 16 * 1024 * 1024;
using Bytes = std::vector<std::byte>;

class ProtocolError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};
} // namespace snowdesktop::settings_ipc

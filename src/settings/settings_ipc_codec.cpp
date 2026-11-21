#include "settings_ipc_codec.h"

#include <cstring>

namespace snowdesktop::settings_ipc
{
void Writer::Append(const void* data, std::size_t size)
{
    if (size > MaximumFrameBytes - bytes_.size())
        throw ProtocolError("settings IPC frame exceeds limit");
    const auto first = static_cast<const std::byte*>(data);
    bytes_.insert(bytes_.end(), first, first + size);
}

Reader::Reader(std::span<const std::byte> bytes) : bytes_(bytes)
{
    if (bytes.size() > MaximumFrameBytes) throw ProtocolError("oversized frame");
}

void Reader::Finish() const
{
    if (position_ != bytes_.size()) throw ProtocolError("trailing IPC data");
}

void Reader::Extract(void* target, std::size_t size)
{
    if (size > bytes_.size() - position_) throw ProtocolError("truncated IPC data");
    if (size) std::memcpy(target, bytes_.data() + position_, size);
    position_ += size;
}

void Reader::Allocate(std::size_t size)
{
    if (size > MaximumFrameBytes * 4 - allocation_)
        throw ProtocolError("settings IPC allocation limit");
    allocation_ += size;
}
} // namespace snowdesktop::settings_ipc

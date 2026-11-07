// SPDX-License-Identifier: MIT
#pragma once

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace snowdesktop::steam_widget_multiplayer
{
inline constexpr std::size_t kPayloadLimit = 1024;
inline constexpr std::size_t kChannelLimit = 192;
inline constexpr std::uint32_t kRoomProtocolVersion = 2;
inline constexpr std::size_t kFriendLimit = 24;
inline constexpr unsigned kMessagesPerSecond = 8;

enum class Kind : std::uint32_t
{
    Create = 1, Join, Friends, Invite, Send, Leave, OpenInvitation,
    Ready, Result, Friend, Invitation, Room, Message, Disconnected,
};

// Private protocol between matching x64 executables. The pipe endpoints are
// inherited explicitly and each writer writes exactly one bounded frame.
struct Frame
{
    std::uint64_t request = 0;
    std::uint64_t value = 0;
    Kind kind{};
    std::uint32_t size = 0;
    std::uint32_t flags = 0;
    std::uint32_t version = kRoomProtocolVersion;
    std::uint32_t channelSize = 0;
    std::array<char, kPayloadLimit> bytes{};
    std::array<char, kChannelLimit> channel{};

    bool SetText(std::string_view text) noexcept
    {
        if (text.size() > bytes.size()) return false;
        size = static_cast<std::uint32_t>(text.size());
        if (size) std::memcpy(bytes.data(), text.data(), size);
        return true;
    }

    [[nodiscard]] std::string_view Text() const noexcept
    {
        return {bytes.data(), size};
    }

    bool SetChannel(std::string_view name) noexcept
    {
        if (name.empty() || name.size() > channel.size() ||
            name.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ|:._+-") != std::string_view::npos)
            return false;
        channelSize = static_cast<std::uint32_t>(name.size());
        std::memcpy(channel.data(), name.data(), channelSize);
        return true;
    }

    [[nodiscard]] std::string_view Channel() const noexcept
    {
        return {channel.data(), channelSize};
    }
};

static_assert(std::is_trivially_copyable_v<Frame>);

enum class ReadResult { Empty, Received, Closed };

inline ReadResult ReadFrame(HANDLE pipe, Frame& frame)
{
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
        return ReadResult::Closed;
    if (available < sizeof(Frame)) return ReadResult::Empty;
    DWORD read = 0;
    if (!ReadFile(pipe, &frame, sizeof(Frame), &read, nullptr) ||
        read != sizeof(Frame) || frame.version != kRoomProtocolVersion ||
        frame.size > frame.bytes.size() || frame.channelSize > frame.channel.size()) return ReadResult::Closed;
    return ReadResult::Received;
}

inline bool WriteFrame(HANDLE pipe, const Frame& frame)
{
    DWORD written = 0;
    return WriteFile(pipe, &frame, sizeof(Frame), &written, nullptr) &&
        written == sizeof(Frame);
}

inline bool IsUtf8Text(std::string_view value)
{
    return !value.empty() && value.size() <= kPayloadLimit &&
        value.find('\0') == std::string_view::npos &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), nullptr, 0) > 0;
}
}

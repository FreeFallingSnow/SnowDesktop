#pragma once

#include <windows.h>
#include <cstdint>
#include <iterator>
#include <string>

namespace snowdesktop::shell_start_pin
{
// Private host/Explorer transport. It accepts only Start pinning, never an
// arbitrary Shell verb, executable or command line.
inline constexpr wchar_t kMessage[] = L"SnowDesktop.Shell.StartPin.v1";
inline constexpr wchar_t kMappingPrefix[] = L"Local\\SnowDesktop.Shell.StartPin.v1.";
inline constexpr std::uint32_t kMagic = 0x5344504e;
enum class Action : std::uint32_t { Pin, Unpin };
enum Status : LONG { Pending, Running, Completed, Cancelled };

struct Request
{
    std::uint32_t magic = kMagic;
    std::uint32_t version = 1;
    std::uint32_t size = sizeof(Request);
    DWORD processId = 0;
    std::uint64_t token = 0;
    Action action = Action::Pin;
    LONG status = Pending;
    HRESULT result = E_PENDING;
    HWND owner = nullptr;
    POINT point{};
    wchar_t path[32768]{};
};

inline std::wstring MappingName(DWORD processId, std::uint64_t token)
{
    return std::wstring(kMappingPrefix) + std::to_wstring(processId) +
        L"." + std::to_wstring(token);
}

inline bool ValidRequest(const Request& request, DWORD processId,
    std::uint64_t token)
{
    if (request.magic != kMagic || request.version != 1 ||
        request.size != sizeof(Request) || !processId || !token ||
        request.processId != processId || request.token != token ||
        (request.action != Action::Pin && request.action != Action::Unpin) ||
        request.status != Pending || !request.path[0])
        return false;
    const auto end = std::char_traits<wchar_t>::find(
        request.path, std::size(request.path), L'\0');
    if (!end) return false;
    const auto length = end - request.path;
    // A fully qualified filesystem identity is required; do not resolve a
    // relative path inside Explorer or substitute a shortcut's launch target.
    return (length >= 3 &&
            ((request.path[0] >= L'A' && request.path[0] <= L'Z') ||
             (request.path[0] >= L'a' && request.path[0] <= L'z')) &&
            request.path[1] == L':' && request.path[2] == L'\\') ||
        (length > 2 && request.path[0] == L'\\' && request.path[1] == L'\\');
}
} // namespace snowdesktop::shell_start_pin

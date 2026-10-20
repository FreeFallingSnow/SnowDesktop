#pragma once

#include <windows.h>

#include <vector>

namespace snowdesktop
{

// Keep corrupt registry values bounded, and never advertise more writable
// bytes than the vector actually owns. A concurrent registry update may also
// change the value's type or size between the two queries.
template<class Query>
std::vector<DWORD> ReadCrashRestartHistory(Query&& query)
{
    constexpr DWORD maxBytes = 64 * sizeof(DWORD);
    DWORD type = 0;
    DWORD bytes = 0;
    if (query(&type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_BINARY || bytes == 0 || bytes > maxBytes ||
        bytes % sizeof(DWORD) != 0)
        return {};
    std::vector<DWORD> ticks(bytes / sizeof(DWORD));
    DWORD received = static_cast<DWORD>(ticks.size() * sizeof(DWORD));
    type = 0;
    if (query(&type, reinterpret_cast<BYTE*>(ticks.data()), &received) !=
            ERROR_SUCCESS || type != REG_BINARY || received > bytes ||
        received % sizeof(DWORD) != 0)
        return {};
    ticks.resize(received / sizeof(DWORD));
    return ticks;
}

inline std::vector<DWORD> ReadCrashRestartHistory(HKEY key)
{
    return ReadCrashRestartHistory([key](DWORD* type, BYTE* data, DWORD* bytes) {
        return RegQueryValueExW(key, L"CrashTicks", nullptr, type, data, bytes);
    });
}

} // namespace snowdesktop

#include <windows.h>

#include <array>
#include <iostream>
#include <string>

int wmain()
{
    std::array<wchar_t, 32768> modulePath{};
    const DWORD length = GetModuleFileNameW(
        nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    const std::wstring executable(modulePath.data(), length);
    if (executable.find(L"offline") != std::wstring::npos)
    {
        std::cerr << "{\"ok\":false,\"error\":{"
                     "\"code\":\"steam_not_logged_on\","
                     "\"message\":\"offline fixture\"}}\n";
        return 4;
    }
    const bool owned = executable.find(L"not-owned") == std::wstring::npos;
    std::cout
        << "{\"ok\":true,\"protocolVersion\":1,"
           "\"expectedAppId\":"
        << SNOWDESKTOP_STEAM_APP_ID
        << ",\"appId\":" << SNOWDESKTOP_STEAM_APP_ID
        << ",\"loggedOn\":true,\"owned\":"
        << (owned ? "true" : "false") << ','
        << "\"steamId\":\"76561198000000001\"}\n";
    return 0;
}

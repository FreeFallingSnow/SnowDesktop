#include <iostream>

int wmain()
{
    std::cout
        << "{\"ok\":true,\"protocolVersion\":1,"
           "\"expectedAppId\":"
        << SNOWDESKTOP_STEAM_APP_ID
        << ",\"appId\":" << SNOWDESKTOP_STEAM_APP_ID
        << ",\"loggedOn\":true,\"owned\":true,"
           "\"steamId\":\"76561198000000001\"}\n";
    return 0;
}

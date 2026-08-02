// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

#if SNOWDESKTOP_HAS_STEAMWORKS
#include <steam/steam_api.h>
#endif

namespace
{
constexpr int kSteamworksUnavailable = 3;
constexpr int kSteamInitializationFailed = 4;
constexpr int kInvalidArguments = 64;

void PrintUsage()
{
    std::cout
        << "SnowDesktop Steam Bridge " << SNOWDESKTOP_VERSION << '\n'
        << "MIT-licensed SnowDesktop/Steam process boundary\n\n"
        << "Usage:\n"
        << "  SnowDesktopSteamBridge.exe --version\n"
        << "  SnowDesktopSteamBridge.exe status\n"
        << "  SnowDesktopSteamBridge.exe workshop list-subscribed\n";
}

#if SNOWDESKTOP_HAS_STEAMWORKS
void PrintJsonString(std::string_view value)
{
    std::cout << '"';
    for (const char character : value)
    {
        switch (character)
        {
        case '\\': std::cout << "\\\\"; break;
        case '"': std::cout << "\\\""; break;
        case '\n': std::cout << "\\n"; break;
        case '\r': std::cout << "\\r"; break;
        case '\t': std::cout << "\\t"; break;
        default: std::cout << character; break;
        }
    }
    std::cout << '"';
}

class SteamApiSession final
{
public:
    SteamApiSession() : initialized_(SteamAPI_Init()) {}

    ~SteamApiSession()
    {
        if (initialized_)
        {
            SteamAPI_Shutdown();
        }
    }

    SteamApiSession(const SteamApiSession&) = delete;
    SteamApiSession& operator=(const SteamApiSession&) = delete;

    [[nodiscard]] bool IsInitialized() const noexcept
    {
        return initialized_;
    }

private:
    bool initialized_ = false;
};

int PrintInitializationFailure()
{
    std::cerr
        << "SteamAPI_Init failed. Launch the bridge through Steam, keep the "
           "Steam client running under the same Windows user, and verify that "
           "the account owns the configured App ID.\n";
    return kSteamInitializationFailed;
}

int PrintStatus()
{
    SteamApiSession steam;
    if (!steam.IsInitialized())
    {
        return PrintInitializationFailure();
    }

    SteamAPI_RunCallbacks();
    ISteamUtils* utils = SteamUtils();
    ISteamUser* user = SteamUser();
    if (utils == nullptr || user == nullptr)
    {
        std::cerr << "Steam interfaces were unavailable after initialization.\n";
        return kSteamInitializationFailed;
    }

    std::cout
        << "{\"steamworksCompiled\":true,\"initialized\":true,\"appId\":"
        << utils->GetAppID()
        << ",\"loggedOn\":" << (user->BLoggedOn() ? "true" : "false")
        << ",\"steamId\":\"" << user->GetSteamID().ConvertToUint64()
        << "\"}\n";
    return 0;
}

int ListSubscribedWorkshopItems()
{
    SteamApiSession steam;
    if (!steam.IsInitialized())
    {
        return PrintInitializationFailure();
    }

    ISteamUGC* ugc = SteamUGC();
    if (ugc == nullptr)
    {
        std::cerr << "ISteamUGC was unavailable after initialization.\n";
        return kSteamInitializationFailed;
    }

    const std::uint32_t count = ugc->GetNumSubscribedItems();
    std::vector<PublishedFileId_t> itemIds(count);
    const std::uint32_t returned = itemIds.empty()
        ? 0
        : ugc->GetSubscribedItems(itemIds.data(), count);

    std::cout << "{\"items\":[";
    for (std::uint32_t index = 0; index < returned; ++index)
    {
        if (index != 0)
        {
            std::cout << ',';
        }

        const PublishedFileId_t itemId = itemIds[index];
        const std::uint32_t state = ugc->GetItemState(itemId);
        std::uint64_t sizeOnDisk = 0;
        std::uint32_t timestamp = 0;
        std::array<char, 32768> installFolder{};
        const bool installed = ugc->GetItemInstallInfo(
            itemId,
            &sizeOnDisk,
            installFolder.data(),
            static_cast<std::uint32_t>(installFolder.size()),
            &timestamp);

        std::cout << "{\"publishedFileId\":\""
                  << static_cast<std::uint64_t>(itemId)
                  << "\",\"state\":" << state
                  << ",\"installed\":" << (installed ? "true" : "false")
                  << ",\"sizeOnDisk\":" << sizeOnDisk
                  << ",\"installFolder\":";
        PrintJsonString(installed ? installFolder.data() : "");
        std::cout << ",\"timestamp\":" << timestamp << '}';
    }
    std::cout << "]}\n";
    return 0;
}
#else
int PrintStatus()
{
    std::cout
        << "{\"steamworksCompiled\":false,\"initialized\":false,"
           "\"reason\":\"external Steamworks SDK was not configured\"}\n";
    return kSteamworksUnavailable;
}

int ListSubscribedWorkshopItems()
{
    std::cerr
        << "Workshop access is unavailable in this SDK-free build. Configure "
           "SNOWDESKTOP_STEAMWORKS_SDK_ROOT with an external Steamworks SDK "
           "and rebuild this target.\n";
    return kSteamworksUnavailable;
}
#endif
}

int main(int argc, char* argv[])
{
    if (argc == 1)
    {
        PrintUsage();
        return 0;
    }

    const std::string_view command = argv[1];
    if (command == "--help" || command == "-h")
    {
        PrintUsage();
        return 0;
    }
    if (command == "--version")
    {
        std::cout << SNOWDESKTOP_VERSION << '\n';
        return 0;
    }
    if (command == "status" && argc == 2)
    {
        return PrintStatus();
    }
    if (command == "workshop" && argc == 3 &&
        std::string_view(argv[2]) == "list-subscribed")
    {
        return ListSubscribedWorkshopItems();
    }

    std::cerr << "Unknown or incomplete command.\n\n";
    PrintUsage();
    return kInvalidArguments;
}

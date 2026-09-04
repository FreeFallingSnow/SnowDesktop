#include "steam_entitlement.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::string Read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}
}

int wmain(int argc, wchar_t** argv)
{
    Check(argc == 2, "the Bridge fixture path is provided");
    if (argc != 2) return EXIT_FAILURE;

    using namespace snowdesktop::steam_entitlement;
    const auto owned = ParseBridgeResponse(
        "noise\n{\"ok\":true,\"appId\":5080330,"
        "\"loggedOn\":true,\"owned\":true,"
        "\"steamId\":\"76561198000000001\"}\n", 0);
    Check(owned.outcome == BridgeOutcome::Owned &&
            owned.steamId == 76561198000000001ull,
        "a valid current-account ownership result is accepted");
    const auto notOwned = ParseBridgeResponse(
        "{\"ok\":true,\"appId\":5080330,"
        "\"loggedOn\":true,\"owned\":false,"
        "\"steamId\":\"76561198000000001\"}\n", 0);
    Check(notOwned.outcome == BridgeOutcome::NotOwned,
        "an authoritative owned=false result is preserved");
    const auto offline = ParseBridgeResponse(
        "{\"ok\":false,\"error\":{"
        "\"code\":\"steam_not_logged_on\","
        "\"message\":\"offline\"}}\n", 4);
    Check(offline.outcome == BridgeOutcome::SteamUnavailable,
        "an offline Steam error is distinct from non-ownership");
    Check(ParseBridgeResponse("not json", 0).outcome ==
            BridgeOutcome::Failed,
        "malformed Bridge output never unlocks features");

    const std::filesystem::path fixture =
        std::filesystem::absolute(argv[1]);
    const std::filesystem::path root = fixture.parent_path() /
        (L"steam-entitlement-test-" +
            std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    Check(!error, "the isolated entitlement test directory is created");
    const std::filesystem::path cache = root / L"entitlement.bin";

    {
        Service service(fixture, fixture, cache);
        Check(service.Current().state == State::Unregistered,
            "a valid Bridge without a cache begins unregistered");
        Check(service.StartRegistration({}),
            "an unregistered service starts one Bridge check");
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(5);
        while (service.Current().state == State::Checking &&
            std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        Check(service.IsRegistered(),
            "a successful ownership check registers advanced features");
        Check(!service.StartRegistration({}),
            "a registered service does not repeat the startup check");
    }

    const std::string protectedBytes = Read(cache);
    Check(!protectedBytes.empty(),
        "registration writes a protected cache");
    Check(protectedBytes.find("76561198000000001") == std::string::npos &&
            protectedBytes.find("owned") == std::string::npos,
        "the cache does not store ownership or Steam ID as plaintext");
    {
        Service restored(fixture, fixture, cache);
        Check(restored.IsRegistered(),
            "the current Windows user can restore an unexpired DPAPI cache");
    }
    {
        Service unavailable(root / L"missing.exe", fixture, cache);
        Check(unavailable.Current().state == State::BridgeUnavailable &&
                !unavailable.StartRegistration({}),
            "a missing Bridge cannot register advanced features");
    }

    std::filesystem::remove_all(root, error);
    if (failures != 0)
    {
        std::cerr << failures << " Steam entitlement check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Steam entitlement checks passed\n";
    return EXIT_SUCCESS;
}

#include "steam_entitlement.h"

#include <windows.h>

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
    const auto offlineResponse = ParseBridgeResponse(
        "{\"ok\":false,\"error\":{"
        "\"code\":\"steam_not_logged_on\","
        "\"message\":\"offline\"}}\n", 4);
    Check(offlineResponse.outcome == BridgeOutcome::SteamUnavailable,
        "an offline Steam error is distinct from non-ownership");
    Check(ParseBridgeResponse("not json", 0).outcome ==
            BridgeOutcome::Failed,
        "malformed Bridge output never unlocks features");

    const auto compatibleConfiguration = ParseSteamBridgeConfiguration(
        "noise\n{\"ok\":true,\"protocolVersion\":1,"
        "\"version\":\"" SNOWDESKTOP_VERSION
        "\",\"expectedAppId\":5080330,"
        "\"steamworksCompiled\":true}\n", 0);
    Check(IsSteamBridgeConfigurationCompatible(
            compatibleConfiguration, SNOWDESKTOP_VERSION),
        "the host accepts an exact Bridge version and protocol match");
    Check(!IsSteamBridgeConfigurationCompatible(
            compatibleConfiguration, "0.0.0.0"),
        "a stale Bridge version is rejected");
    Check(!IsSteamBridgeConfigurationCompatible(
            ParseSteamBridgeConfiguration(
                "{\"ok\":true,\"protocolVersion\":2,"
                "\"version\":\"" SNOWDESKTOP_VERSION "\","
                "\"expectedAppId\":5080330,"
                "\"steamworksCompiled\":true}", 0),
            SNOWDESKTOP_VERSION),
        "an unsupported Bridge protocol is rejected");
    Check(!IsSteamBridgeConfigurationCompatible(
            ParseSteamBridgeConfiguration(
                "{\"ok\":true,\"protocolVersion\":1,"
                "\"version\":\"" SNOWDESKTOP_VERSION "\","
                "\"expectedAppId\":5080330,"
                "\"steamworksCompiled\":false}", 0),
            SNOWDESKTOP_VERSION),
        "an SDK-free Bridge is not exposed as Workshop-capable");

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
    std::int64_t registeredUntil = 0;

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
        Check(service.StartRegistration({}, true),
            "startup can explicitly revalidate an existing registration");
        const auto renewalDeadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(5);
        while (service.Current().state == State::Checking &&
            std::chrono::steady_clock::now() < renewalDeadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const Snapshot renewed = service.Current();
        registeredUntil = renewed.validUntil;
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        Check(renewed.registered && registeredUntil > now,
            "successful startup revalidation renews registration");
    }

    const std::string protectedBytes = Read(cache);
    Check(!protectedBytes.empty(),
        "registration writes a protected cache");
    Check(protectedBytes.find("76561198000000001") == std::string::npos &&
            protectedBytes.find("owned") == std::string::npos,
        "the cache does not store ownership or Steam ID as plaintext");
    {
        Service restored(fixture, fixture, cache);
        Check(restored.IsRegistered() &&
                restored.Current().validUntil == registeredUntil,
            "the current Windows user can restore an unexpired DPAPI cache");
    }
    const auto offlineFixture = root / L"offline-bridge.exe";
    const auto notOwnedFixture = root / L"not-owned-bridge.exe";
    const auto staleFixture = root / L"stale-bridge.exe";
    const auto sdkFreeFixture = root / L"sdk-free-bridge.exe";
    std::filesystem::copy_file(fixture, offlineFixture,
        std::filesystem::copy_options::overwrite_existing, error);
    Check(!error, "the offline Bridge fixture is prepared");
    error.clear();
    std::filesystem::copy_file(fixture, notOwnedFixture,
        std::filesystem::copy_options::overwrite_existing, error);
    Check(!error, "the non-owner Bridge fixture is prepared");
    error.clear();
    std::filesystem::copy_file(fixture, staleFixture,
        std::filesystem::copy_options::overwrite_existing, error);
    Check(!error, "the stale Bridge fixture is prepared");
    error.clear();
    std::filesystem::copy_file(fixture, sdkFreeFixture,
        std::filesystem::copy_options::overwrite_existing, error);
    Check(!error, "the SDK-free Bridge fixture is prepared");
    {
        Service offline(offlineFixture, offlineFixture, cache);
        Check(offline.IsRegistered() &&
                offline.StartRegistration({}, true),
            "an unexpired registration begins startup revalidation");
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(5);
        while (offline.Current().state == State::Checking &&
            std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const Snapshot snapshot = offline.Current();
        Check(snapshot.registered &&
                snapshot.validUntil == registeredUntil &&
                snapshot.state == State::RegistrationFailed &&
                snapshot.failure == Failure::SteamUnavailable &&
                std::filesystem::is_regular_file(cache),
            "temporary Steam failure keeps but does not renew the offline lease");
    }
    {
        Service refunded(notOwnedFixture, notOwnedFixture, cache);
        Check(refunded.IsRegistered() &&
                refunded.StartRegistration({}, true),
            "a cached registration can be checked for refunded ownership");
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(5);
        while (refunded.Current().state == State::Checking &&
            std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const Snapshot snapshot = refunded.Current();
        Check(!snapshot.registered &&
                snapshot.validUntil == 0 &&
                snapshot.failure == Failure::NotOwned &&
                !std::filesystem::exists(cache),
            "an authoritative owned=false response immediately revokes the cache");
    }
    {
        Service unavailable(root / L"missing.exe", fixture, cache);
        Check(unavailable.Current().state == State::BridgeUnavailable &&
                !unavailable.StartRegistration({}),
            "a missing Bridge cannot register advanced features");
    }
    // A debug reset must survive service recreation, cancel renewal, and
    // preserve the existing lease if its protected record cannot be removed.
    {
        std::ofstream(cache, std::ios::binary) << protectedBytes;
        Service service(fixture, fixture, cache);
        const Snapshot before = service.Current();
        HANDLE locked = CreateFileW(cache.c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
            nullptr);
        Check(locked != INVALID_HANDLE_VALUE, "the cache is locked against deletion");
        if (locked != INVALID_HANDLE_VALUE)
        {
            Check(!service.ResetRegistration() && service.IsRegistered() &&
                    service.Current().validUntil == before.validUntil &&
                    service.Current().failure == Failure::StorageError &&
                    Read(cache) == protectedBytes,
                "a failed reset reports a storage error without discarding the saved lease");
            CloseHandle(locked);
        }
        Check(service.ResetRegistration(), "a local unlock record can be cleared");
        const Snapshot reset = service.Current();
        Check(!reset.registered && reset.validUntil == 0 &&
                reset.state == State::Unregistered &&
                reset.failure == Failure::None &&
                reset.revision > before.revision &&
                !std::filesystem::exists(cache),
            "reset clears both persisted and in-memory entitlement and advances its revision");
        {
            Service restored(fixture, fixture, cache);
            Check(!restored.IsRegistered(),
                "a new service does not restore a cleared entitlement");
        }
        Check(service.ResetRegistration(), "reset is safe with no cache");
        Check(service.StartRegistration({}),
            "reset leaves the service available for another ownership check");
        Check(service.ResetRegistration() && !service.IsRegistered() &&
                !std::filesystem::exists(cache),
            "reset cancels a pending check before it can recreate the unlock record");
        Check(service.StartRegistration({}),
            "a canceled check does not permanently stop registration");
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(5);
        while (service.Current().state == State::Checking &&
            std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        Check(service.IsRegistered() && std::filesystem::is_regular_file(cache),
            "ownership verification can unlock features again after reset");
    }
    {
        Service unavailable(root / L"missing.exe", fixture, cache);
        Check(unavailable.ResetRegistration() &&
                unavailable.Current().state == State::BridgeUnavailable &&
                !unavailable.IsRegistered() && !std::filesystem::exists(cache),
            "reset can clear an old cache even when the Bridge is unavailable");
    }
    {
        Service stale(staleFixture, staleFixture, cache);
        Check(stale.Current().state == State::BridgeUnavailable &&
                !stale.Current().bridgeAvailable &&
                !stale.StartRegistration({}),
            "a stale Bridge cannot register or expose Steam features");
    }
    {
        Service sdkFree(sdkFreeFixture, sdkFreeFixture, cache);
        Check(sdkFree.Current().state == State::BridgeUnavailable &&
                !sdkFree.Current().bridgeAvailable &&
                !sdkFree.StartRegistration({}),
            "an SDK-free Bridge cannot register or expose Steam features");
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

#include "platform/auto_start_launch.h"

#include <cstddef>
#include <cwchar>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace launch = snowdesktop::auto_start_launch;

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// Identity discovery, privilege state and Shell execution are replaced. Production
// quoting, normalization, event creation, signalling and waiting run intact.
LONG identityResult = ERROR_SUCCESS;
std::wstring applicationId;
struct Capture
{
    std::wstring target;
    std::wstring parameters;
    std::wstring directory;
    unsigned calls = 0;
    unsigned waits = 0;
    bool acknowledge = true;
} captured;

LONG WINAPI QueryApplicationId(UINT32* length, PWSTR value)
{
    SetLastError(ERROR_FILE_NOT_FOUND); // API return codes must not use this.
    if (identityResult != ERROR_SUCCESS) return identityResult;
    const auto required = static_cast<UINT32>(applicationId.size() + 1);
    if (*length < required)
    {
        *length = required;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    std::wmemcpy(value, applicationId.c_str(), required);
    *length = required;
    return ERROR_SUCCESS;
}

void Execute(const std::wstring& target, const std::wstring& parameters,
    const std::wstring& directory)
{
    ++captured.calls;
    captured.target = target;
    captured.parameters = parameters;
    captured.directory = directory;
    if (captured.acknowledge)
    {
        struct OrdinaryChild
        {
            bool IsElevated() { return false; }
            bool IsExplorerElevated() { throw launch::Failure{ERROR_INVALID_FUNCTION, L"Ordinary child Explorer lookup"}; }
            bool ConfirmContinue(const launch::Failure&) { return false; }
            DWORD Relaunch(std::wstring_view) { throw std::runtime_error("ordinary child must continue directly"); }
        } child;
        const auto result = launch::Normalize(L"fixture.exe " + parameters, child);
        Require(!result.handled && result.error == ERROR_SUCCESS,
            "the ordinary child acknowledges and continues through production startup normalization");
    }
}

DWORD WINAPI WaitForAck(HANDLE event, DWORD timeout)
{
    ++captured.waits;
    return WaitForSingleObject(event, timeout);
}

launch::NativePlatform Platform()
{
    return launch::NativePlatform(&Execute, 0, &CreateEventW, &WaitForAck, &QueryApplicationId);
}

std::wstring CurrentDirectory()
{
    std::wstring value(32768, L'\0');
    const DWORD length = GetCurrentDirectoryW(static_cast<DWORD>(value.size()), value.data());
    Require(length != 0 && length < value.size(), "read fixture working directory");
    value.resize(length);
    return value;
}

std::wstring CurrentExecutable()
{
    std::wstring value(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    Require(length != 0 && length < value.size(), "read fixture executable");
    value.resize(length);
    return value;
}

void PackagedLaunch()
{
    identityResult = ERROR_SUCCESS;
    const std::vector<std::wstring> inputs{L"", L"a path with spaces", L"C:\\tail\\", L"a\"quote"};
    std::wstring command = L"fixture.exe --snowdesktop-autostart-owner=packaged";
    for (const auto& input : inputs) command += L" " + launch::QuoteArgument(input);
    for (const auto* identity : {L"64E2EDAE.SnowDesktop_fxb2gaxa7nhr2!SnowDesktop",
        L"SnowDesktop.Dev_testpublisher!DifferentApplication"})
    {
        captured = {};
        applicationId = identity;
        Require(Platform().Relaunch(command) == 0, "packaged relaunch completes");
        Require(captured.calls == 1 && captured.target == L"shell:AppsFolder\\" + applicationId,
            "packaged relaunch must activate its current AUMID instead of the EXE path");
        Require(captured.directory == CurrentDirectory(), "preserve working directory");
        const launch::Arguments arguments(L"fixture.exe " + captured.parameters);
        Require(arguments.count == static_cast<int>(inputs.size()) + 4, "retain all startup arguments and receipt");
        Require(std::wstring_view(arguments.value[1]) == L"--snowdesktop-autostart-owner=packaged",
            "the generic privilege handoff preserves the caller's startup intent verbatim");
        for (std::size_t index = 0; index < inputs.size(); ++index)
            Require(arguments.value[index + 2] == inputs[index], "startup argument round trip");
        Require(std::wstring_view(arguments.value[arguments.count - 2]) == launch::kExplorerRelaunchArgument,
            "retain the loop-prevention marker");
        Require(launch::IsExplorerAckArgument(arguments.value[arguments.count - 1]) && captured.waits == 1,
            "observe the real child acknowledgement event");
    }
    std::cout << "PASS packaged activation, arguments and acknowledgement\n";
}

void StartupPrivilegeRouting()
{
    struct StartupPlatform
    {
        bool elevated = false;
        bool explorerElevated = false;
        bool explorerAvailable = true;
        unsigned elevationChecks = 0;
        unsigned explorerChecks = 0;
        unsigned confirmations = 0;
        bool IsElevated() { ++elevationChecks; return elevated; }
        bool IsExplorerElevated()
        {
            ++explorerChecks;
            if (!explorerAvailable) throw launch::Failure{ERROR_TIMEOUT, L"Fixture Explorer unavailable"};
            return explorerElevated;
        }
        bool ConfirmContinue(const launch::Failure&) { ++confirmations; return false; }
        DWORD Relaunch(std::wstring_view command) { return Platform().Relaunch(command); }
    };
    const std::vector<std::wstring> commands{
        L"fixture.exe",
        L"fixture.exe --snowdesktop-autostart-owner=portable",
        L"fixture.exe --snowdesktop-autostart-owner=packaged",
        L"fixture.exe --snowdesktop-autostart-owner=steam",
        L"fixture.exe --snowdesktop-autostart",
        L"fixture.exe --snowdesktop-explorer-relaunch"};
    identityResult = ERROR_SUCCESS;
    applicationId = L"SnowDesktop.Dev_testpublisher!SnowDesktop";
    for (const auto& command : commands)
    {
        for (const bool explorerAvailable : {true, false})
        {
            captured = {};
            StartupPlatform platform;
            platform.explorerAvailable = explorerAvailable;
            const auto result = launch::Normalize(command, platform);
            Require(!result.handled && result.error == ERROR_SUCCESS && captured.calls == 0 &&
                captured.waits == 0 && platform.confirmations == 0 && platform.explorerChecks == 0 &&
                platform.elevationChecks == 1,
                "non-elevated startup must continue without Explorer lookup, relaunch or confirmation");
        }
    }
    std::cout << "PASS ordinary manual/autostart launches continue directly, even without Explorer\n";

    // The same permission check controls interactive and all three autostart
    // owners. Their startup intent cannot substitute for an elevated token.
    for (std::size_t index = 0; index < 5; ++index)
    {
        for (const bool explorerElevated : {false, true})
        {
            captured = {};
            StartupPlatform platform;
            platform.elevated = true;
            platform.explorerElevated = explorerElevated;
            const auto result = launch::Normalize(commands[index], platform);
            Require(result.error == ERROR_SUCCESS && result.handled == !explorerElevated &&
                captured.calls == (explorerElevated ? 0u : 1u) && platform.explorerChecks == 1 &&
                platform.elevationChecks == 1 && platform.confirmations == 0,
                "only elevated launches use an ordinary Explorer for handoff");
            if (!explorerElevated)
                Require(captured.target == L"shell:AppsFolder\\" + applicationId && captured.waits == 1,
                    "an elevated packaged launch retains identity activation and acknowledgement");
        }
    }
    std::cout << "PASS elevated launches share the same Explorer handoff policy\n";

    captured = {};
    StartupPlatform repeated;
    repeated.elevated = true;
    const auto denied = launch::Normalize(commands.back(), repeated);
    Require(denied.handled && denied.error == ERROR_CANCELLED && captured.calls == 0 &&
        repeated.confirmations == 1, "a still-elevated handed-off child must not loop");

    StartupPlatform helper;
    helper.elevated = true;
    const auto auxiliary = launch::Normalize(commands[2], helper, false);
    Require(!auxiliary.handled && auxiliary.error == ERROR_SUCCESS && helper.elevationChecks == 0 &&
        helper.explorerChecks == 0 && captured.calls == 0, "inherited-handle helpers bypass desktop handoff");

    StartupPlatform accepted;
    accepted.elevated = true;
    const auto consented = launch::Normalize(commands[2], accepted, true, true);
    Require(!consented.handled && consented.error == ERROR_SUCCESS && consented.currentPrivilegesAccepted &&
        accepted.elevationChecks == 0 && accepted.explorerChecks == 0 && captured.calls == 0,
        "explicit startup consent must remain effective without another handoff");
    std::cout << "PASS repeated-child, helper and explicit-consent guards\n";
}

void OrdinaryLaunch()
{
    for (const LONG result : {APPMODEL_ERROR_NO_APPLICATION, APPMODEL_ERROR_NO_PACKAGE})
    {
        captured = {};
        identityResult = result;
        Require(Platform().Relaunch(L"fixture.exe --snowdesktop-autostart-owner=steam") == 0,
            "ordinary EXE relaunch completes");
        Require(captured.calls == 1 && captured.target == CurrentExecutable() && captured.waits == 1,
            "ordinary relaunch preserves its actual EXE and receipt");
    }
    std::cout << "PASS ordinary EXE fallback\n";
}

void IdentityFailure()
{
    for (const LONG result : {ERROR_ACCESS_DENIED, ERROR_INSUFFICIENT_BUFFER})
    {
        captured = {};
        identityResult = result;
        try
        {
            Platform().Relaunch(L"fixture.exe");
            throw std::runtime_error("identity discovery failure must be reported");
        }
        catch (const launch::Failure& failure)
        {
            Require(failure.code == static_cast<DWORD>(result), "preserve the identity API error code");
            Require(captured.calls == 0 && captured.waits == 0, "identity errors must not launch an EXE fallback");
        }
    }
    identityResult = ERROR_SUCCESS;
    applicationId.clear();
    captured = {};
    try
    {
        Platform().Relaunch(L"fixture.exe");
        throw std::runtime_error("an empty application identity must not open AppsFolder");
    }
    catch (const launch::Failure& failure)
    {
        Require(failure.code == ERROR_INVALID_DATA && captured.calls == 0, "reject an empty application identity");
    }
    std::cout << "PASS identity error propagation\n";
}

void MissingAcknowledgement()
{
    captured = {};
    captured.acknowledge = false;
    identityResult = ERROR_SUCCESS;
    applicationId = L"SnowDesktop.Dev_testpublisher!SnowDesktop";
    try
    {
        Platform().Relaunch(L"fixture.exe");
        throw std::runtime_error("a submitted launch without a child receipt must time out");
    }
    catch (const launch::Failure& failure)
    {
        Require(failure.code == ERROR_TIMEOUT && captured.calls == 1 && captured.waits == 1,
            "preserve missing-child detection without duplicate launch");
    }
    std::cout << "PASS missing acknowledgement remains a failure\n";
}
}

int main()
{
    try
    {
        PackagedLaunch();
        StartupPrivilegeRouting();
        OrdinaryLaunch();
        IdentityFailure();
        MissingAcknowledgement();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    catch (const launch::Failure& failure)
    {
        std::wcerr << L"FAIL " << failure.operation << L" (" << failure.code << L")\n";
        return 1;
    }
}

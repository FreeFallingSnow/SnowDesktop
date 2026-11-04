#include "platform/auto_start_launch.h"

#include <cwchar>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
namespace launch = snowdesktop::auto_start_launch;

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// Only identity discovery and Shell execution are replaced. Production
// quoting, owner conversion, event creation, signalling and waiting run intact.
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
        launch::AcknowledgeExplorerLaunch(L"fixture.exe " + parameters);
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
        Require(std::wstring_view(arguments.value[1]) == snowdesktop::kAutoStartBackgroundArgument,
            "autostart ownership becomes background intent");
        for (std::size_t index = 0; index < inputs.size(); ++index)
            Require(arguments.value[index + 2] == inputs[index], "startup argument round trip");
        Require(std::wstring_view(arguments.value[arguments.count - 2]) == launch::kExplorerRelaunchArgument,
            "retain the loop-prevention marker");
        Require(launch::IsExplorerAckArgument(arguments.value[arguments.count - 1]) && captured.waits == 1,
            "observe the real child acknowledgement event");
    }
    std::cout << "PASS packaged activation, arguments and acknowledgement\n";
}

void AutoStartRouting()
{
    struct StartupPlatform
    {
        unsigned confirmations = 0;
        bool IsElevated() { return false; }
        bool IsExplorerElevated() { return false; }
        bool ConfirmContinue(const launch::Failure&) { ++confirmations; return false; }
        DWORD Relaunch(std::wstring_view command) { return Platform().Relaunch(command); }
    } platform;
    captured = {};
    identityResult = ERROR_SUCCESS;
    applicationId = L"SnowDesktop.Dev_testpublisher!SnowDesktop";
    const auto manual = launch::Normalize(L"fixture.exe", platform);
    Require(!manual.handled && manual.error == ERROR_SUCCESS && captured.calls == 0,
        "an ordinary manual launch starts directly");
    const auto startup = launch::Normalize(L"fixture.exe --snowdesktop-autostart-owner=packaged", platform);
    Require(startup.handled && startup.error == ERROR_SUCCESS && captured.calls == 1 &&
        captured.target == L"shell:AppsFolder\\" + applicationId && platform.confirmations == 0,
        "a Limited scheduled autostart must hand off through its packaged identity");
    std::cout << "PASS ordinary scheduled startup routing\n";
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
        AutoStartRouting();
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

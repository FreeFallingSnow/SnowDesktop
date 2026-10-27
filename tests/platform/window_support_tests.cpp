#include "platform/window_support/window_capabilities.h"
#include "platform/window_support/window_pointer_events.h"
#include "platform/window_support/window_operation_failures.h"
#include "platform/window_support/window_support_security.h"
#include "platform/window_support/window_support_authorization.h"
#include "platform/window_support/window_support_host_identity.h"
#include "platform/window_support/window_support_installation.h"
#include "../test_temporary_directory.h"
#include <sddl.h>

#include <cstdio>
#include <initializer_list>
#include <fstream>
#include <iterator>

using namespace snowdesktop::window_support;

int main(int argc, char** argv)
{
    unsigned failed = 0;
    const auto check = [&](bool result, const char* message) {
        if (!result) { std::fprintf(stderr, "FAIL: %s\n", message); ++failed; }
    };
    {
        snowdesktop::test::TemporaryDirectory temporary;
        const auto live = temporary.path / L"authorized";
        const auto staged = temporary.path / L"staged";
        std::filesystem::create_directory(live);
        std::filesystem::create_directory(staged);
        check(HelperImplementationId().size() == 64 && HelperImplementationChanged(live),
            "a legacy installation without an implementation identity offers an explicit update");
        { std::ofstream marker(live / L"implementation-id.bin", std::ios::binary); marker << HelperImplementationId(); }
        check(!HelperImplementationChanged(live), "matching implementation stays current regardless of application version metadata");
        { std::ofstream marker(live / L"implementation-id.bin", std::ios::binary); marker << "old implementation"; }
        check(HelperImplementationChanged(live), "a changed helper implementation offers an update");
        const auto write = [](const auto& path, const char* text) { std::ofstream output(path, std::ios::binary); output << text; };
        const auto read = [](const auto& path) { std::ifstream input(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(input), {}); };
        write(staged / L"new.bin", "new protected bundle");
        check(argc == 2, "installation regression receives its isolated Hook DLL fixture");
        if (argc == 2)
        {
            const auto mappedImage = live / L"hook.dll";
            std::filesystem::copy_file(std::filesystem::path(argv[1]), mappedImage);
            const HMODULE module = LoadLibraryW(mappedImage.c_str());
            check(module != nullptr, "old target-side Hook remains mapped during the update");
            if (module)
            {
                const BOOL deleted = DeleteFileW(mappedImage.c_str());
                const DWORD error = GetLastError();
                check(!deleted && error == ERROR_ACCESS_DENIED, "old in-place replacement reproduces user error 5 with a loaded DLL");
                std::filesystem::path retired;
                check(PublishHelperBundle(staged, live, retired) == ERROR_SUCCESS &&
                    read(live / L"new.bin") == "new protected bundle" && std::filesystem::is_regular_file(retired / L"hook.dll"),
                    "production bundle publication succeeds while the old Hook remains loaded and reusable");
                FreeLibrary(module);
            }
        }
        const auto failing = temporary.path / L"failing";
        const auto occupied = temporary.path / L"not-a-directory";
        std::filesystem::create_directory(failing);
        write(failing / L"new.bin", "pending"); write(occupied, "keep existing file");
        std::filesystem::path retired;
        check(PublishHelperBundle(failing, occupied, retired) == ERROR_ACCESS_DENIED &&
            read(occupied) == "keep existing file" && read(failing / L"new.bin") == "pending",
            "invalid destination leaves both existing data and the prepared update unchanged");
        const auto rollback = temporary.path / L"rollback", blocked = temporary.path / L"blocked-stage";
        std::filesystem::create_directory(rollback); std::filesystem::create_directory(blocked);
        write(rollback / L"old.bin", "working old helper"); write(blocked / L"new.bin", "pending new helper");
        const HANDLE lock = CreateFileW(blocked.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        check(lock != INVALID_HANDLE_VALUE, "a directory lease can reject publication after the old bundle was retired");
        if (lock != INVALID_HANDLE_VALUE)
        {
            check(PublishHelperBundle(blocked, rollback, retired) != ERROR_SUCCESS &&
                read(rollback / L"old.bin") == "working old helper" && read(blocked / L"new.bin") == "pending new helper",
                "rejected publication restores the old fixed path and retains the prepared update");
            CloseHandle(lock);
        }
    }
    const auto firstRuntime = HostPathIdentity(L"D:\\Steam\\SnowDesktop\\.snowdesktop\\runtime\\build-one\\SnowDesktop.exe");
    check(!firstRuntime.empty() && firstRuntime == HostPathIdentity(L"D:\\Steam\\SnowDesktop\\.snowdesktop\\runtime\\build-two\\SnowDesktop.exe"),
        "the same managed installation retains authorization across runtime version directories");
    check(HostIdentityMatches(L"D:\\Steam\\SnowDesktop\\.snowdesktop\\runtime\\deleted-build\\SnowDesktop.exe", firstRuntime) &&
        HostIdentityMatches(firstRuntime, firstRuntime),
        "both legacy protected paths and new identity records survive removal of the old managed version");
    check(!HostIdentityMatches(L"D:\\Other\\SnowDesktop\\.snowdesktop\\runtime\\deleted-build\\SnowDesktop.exe", firstRuntime) &&
        !HostIdentityMatches(L"D:\\Steam\\SnowDesktop\\.snowdesktop\\runtime\\..\\SnowDesktop.exe", firstRuntime),
        "legacy migration never broadens a grant to another installation or a traversing path");
    check(HostIdentityMatches(L"C:\\Program Files\\WindowsApps\\SnowDesktop_1.0.1.0_x64__8wekyb3d8bbwe\\SnowDesktop.exe",
        L"SnowDesktopHost:2:package:SnowDesktop_8wekyb3d8bbwe") &&
        !HostIdentityMatches(L"C:\\Program Files\\WindowsApps\\SnowDesktop_1.0.1.0_x64__8wekyb3d8bbwe\\SnowDesktop.exe",
        L"SnowDesktopHost:2:package:Other_8wekyb3d8bbwe"),
        "legacy packaged paths match the same package family across versions and reject another package");
    check(firstRuntime != HostPathIdentity(L"D:\\Other\\SnowDesktop\\.snowdesktop\\runtime\\build-two\\SnowDesktop.exe") &&
        firstRuntime != HostPathIdentity(L"D:\\Steam\\SnowDesktop\\.snowdesktop\\dev\\build-two\\SnowDesktop.exe") &&
        firstRuntime != HostPathIdentity(L"D:\\Steam\\SnowDesktop\\.snowdesktop\\runtime\\build-two\\nested\\SnowDesktop.exe"),
        "another installation, development profile or nested executable cannot reuse the managed grant");
    check(HostPathIdentity(L"D:\\Portable\\one\\SnowDesktop.exe") != HostPathIdentity(L"D:\\Portable\\two\\SnowDesktop.exe"),
        "portable folders keep their exact authorized location");
    check(HostPathIdentity(L"relative\\SnowDesktop.exe").empty() &&
        HostPathIdentity(L"D:\\Steam\\SnowDesktop\\.snowdesktop\\runtime\\..\\SnowDesktop.exe").empty() &&
        HostPathIdentity(L"D:\\Steam\\SnowDesktop\\.snowdesktop\\runtime\\build-one\\other.exe").empty(),
        "relative paths, traversal and unrelated program names are rejected");
    check(!AuthorizedHostMatches(firstRuntime, GetCurrentProcess()) &&
        !AuthorizedHostMatches(L"SnowDesktopHost:2:path:D:\\unrelated\\SnowDesktop.exe", GetCurrentProcess()),
        "the live process check rejects a renamed test executable despite valid-looking recorded paths");
    for (const auto state : {AuthorizationState::NotRegistered, AuthorizationState::Registered, AuthorizationState::Invalid})
        check(!AuthorizationPending(state), "completed and failed authorizations allow another explicit action");
    check(AuthorizationPending(AuthorizationState::AwaitingConsent) && AuthorizationPending(AuthorizationState::Configuring),
        "consent and installation both prevent overlapping authorization requests");
    wchar_t userName[256]{};
    DWORD userNameLength = static_cast<DWORD>(std::size(userName));
    check(GetUserNameW(userName, &userNameLength) && AuthorizationTaskUserMatches(userName),
        "Task Scheduler's normalized account name resolves to the current SID");
    check(AuthorizationTaskUserMatches(CurrentUserSid()) && !AuthorizationTaskUserMatches(L"S-1-5-18") &&
        !AuthorizationTaskUserMatches(L""), "only the actual current user can own the authorization task");
    bool launched = false;
    check(ConfigureAuthorization(true, nullptr, [&] { launched = true; }) == ERROR_CANCELLED && !launched,
        "a missing initiating settings window cancels before UAC and before installation progress");
    const HANDLE completion = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    check(completion != nullptr, "authorization wait fixture is created");
    if (completion)
    {
        const auto before = GetTickCount64();
        check(WaitForAuthorizationCompletion(completion, 20) == ERROR_TIMEOUT && GetTickCount64() - before < 2000,
            "an unresponsive setup has a bounded wait and reports timeout");
        SetEvent(completion);
        check(WaitForAuthorizationCompletion(completion, 100) == ERROR_SUCCESS,
            "a completed setup leaves the waiting state");
        CloseHandle(completion);
    }
    constexpr Capability operations[] = {Capability::PointerObservation,
        Capability::WindowPin, Capability::WindowControl, Capability::MinimizeAnimation};
    for (const auto operation : operations)
    {
        for (CapabilityMask local = 0; local < 16; ++local)
            for (CapabilityMask remote = 0; remote < 16; ++remote)
                for (const auto state : {ProviderState::Absent, ProviderState::Starting,
                         ProviderState::Ready, ProviderState::Failed})
                {
                    const CapabilitySnapshot snapshot{local, remote, state, 19};
                    const auto route = ResolveCapability(snapshot, operation);
                    const bool hasLocal = Contains(local, operation);
                    const bool hasRemote = state == ProviderState::Ready && Contains(remote, operation);
                    check(route.Available() == (hasLocal || hasRemote), "only established capability enables an operation");
                    check(route.provider == (hasLocal ? Provider::InProcess : hasRemote ? Provider::Helper : Provider::Unavailable),
                        "provider precedence never depends on a permission label");
                    check(route.generation == 19, "route carries its observation generation");
                    check(ResolveCapability(snapshot, operation, {false, true, true}).NeedsFallback(),
                        "stale target cannot be authorized by a ready provider");
                    check(ResolveCapability(snapshot, operation, {true, false, true}).Available() == hasRemote,
                        "target-specific local unavailability permits only an established remote operation");
                    check(ResolveCapability(snapshot, operation, {true, true, false}).Available() == hasLocal,
                        "a ready helper cannot override its target access restriction");
                }
    }
    const CapabilitySnapshot partial{Mask(Capability::WindowControl), Mask(Capability::WindowPin), ProviderState::Ready, 20};
    check(ResolveCapability(partial, Capability::WindowPin).provider == Provider::Helper,
        "pin can use helper while native control remains local");
    check(ResolveCapability(partial, Capability::PointerObservation).NeedsFallback(),
        "pin support does not suppress an unavailable pointer fallback");
    check(ResolveCapability(partial, Capability::None).NeedsFallback(), "empty capability is never usable");
    WindowOperationFailures failures;
    const protocol::WindowIdentity first{17, 18, 19}, reused{17, 20, 21};
    failures.Record(first, Capability::WindowPin, Provider::InProcess, ERROR_ACCESS_DENIED, 100);
    check(failures.Blocked(first, Capability::WindowPin, Provider::InProcess, 101), "an actual failed operation excludes its provider");
    check(!failures.Blocked(first, Capability::PointerObservation, Provider::InProcess, 101), "pin failure never disables mouse observation");
    check(!failures.Blocked(first, Capability::WindowPin, Provider::Helper, 101), "another provider can satisfy the failed operation");
    check(!failures.Blocked(reused, Capability::WindowPin, Provider::InProcess, 101), "reused HWNDs do not inherit an earlier process failure");
    check(!failures.Blocked(first, Capability::WindowPin, Provider::InProcess, 5100), "a transient failure permits a bounded later retry");
    failures.Record(reused, Capability::WindowPin, Provider::InProcess, ERROR_TIMEOUT, 100);
    check(!failures.Blocked(reused, Capability::WindowPin, Provider::InProcess, 101),
        "an asynchronous confirmation timeout cannot disable later native retry");
    const auto descriptor = AuthorizationSecurityDescriptor();
    check(TrustedAuthorizationSecurity(descriptor.c_str()), "configured task is trusted");
    const auto unsafeOwner = L"O:" + CurrentUserSid() + L"G:BAD:P(A;;GA;;;BA)(A;;GRGX;;;" + CurrentUserSid() + L")";
    check(!TrustedAuthorizationSecurity(unsafeOwner.c_str()), "user-owned task cannot retain one-time elevation authority");
    const auto unsafeWrite = L"O:BAG:BAD:P(A;;GA;;;BA)(A;;GW;;;" + CurrentUserSid() + L")";
    check(!TrustedAuthorizationSecurity(unsafeWrite.c_str()) && !TrustedAuthorizationSecurity(L"invalid"),
        "writable or malformed registered authority is rejected before use");
    PSECURITY_DESCRIPTOR security = nullptr;
    const bool parsed = ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor.c_str(), SDDL_REVISION_1, &security, nullptr) != FALSE;
    check(parsed, "task security descriptor is accepted by Windows");
    if (parsed)
    {
        PSID owner = nullptr;
        BOOL defaulted = FALSE;
        check(GetSecurityDescriptorOwner(security, &owner, &defaulted) && IsWellKnownSid(owner, WinBuiltinAdministratorsSid),
            "ordinary split-token user is not the privileged task owner");
        PACL dacl = nullptr;
        BOOL present = FALSE;
        check(GetSecurityDescriptorDacl(security, &present, &dacl, &defaulted) && present && dacl,
            "privileged task has an explicit access list");
        PSID user = nullptr;
        const auto userSid = CurrentUserSid();
        const bool sidValid = ConvertStringSidToSidW(userSid.c_str(), &user) != FALSE;
        bool foundUser = false;
        if (dacl && sidValid) for (DWORD i = 0; i < dacl->AceCount; ++i)
        {
            void* raw = nullptr;
            if (!GetAce(dacl, i, &raw)) continue;
            const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
            if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE || !EqualSid(user, const_cast<DWORD*>(&ace->SidStart))) continue;
            foundUser = true;
            check(ace->Mask == (GENERIC_READ | GENERIC_EXECUTE), "ordinary user may read/run but cannot rewrite the task or its DACL");
        }
        check(foundUser, "authorized current user has the demand-run grant");
        if (user) LocalFree(user);
        LocalFree(security);
    }
    RecentPointerEvents recent;
    protocol::PointerEvent down{1, {42, -10}, WM_LBUTTONDOWN, 1000, 0, 0, 1};
    auto up = down;
    up.sequence = 2; up.message = WM_LBUTTONUP; up.time = 1001; up.buttons = 0;
    check(recent.Accept(down, 1010) && recent.Accept(up, 1010), "button transitions remain separate events");
    down.sequence = 89;
    check(!recent.Accept(down, 1011) && !recent.Accept(up, 1011), "interleaved native/helper duplicates are delivered once");
    down.time = 700;
    check(!recent.Accept(down, 1011), "delayed events cannot dismiss newly opened content");
    down.time = 0xfffffff0;
    check(recent.Accept(down, 16), "event age handles DWORD tick wrap");
    down.time = 1100; down.point.x = 43;
    check(recent.Accept(down, 1101), "a later distinct press remains actionable");
    down.time = 1200; down.flags = LLMHF_INJECTED;
    check(recent.Accept(down, 1201), "injected observation is delivered once");
    down.flags |= LLMHF_LOWER_IL_INJECTED;
    check(!recent.Accept(down, 1201), "observer integrity metadata does not duplicate the same input");
    HWND fixture = CreateWindowExW(0, L"STATIC", L"Window support identity fixture", WS_POPUP,
        0, 0, 20, 20, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(fixture != nullptr, "native target fixture is created");
    if (fixture)
    {
        auto identity = protocol::Capture(fixture);
        check(protocol::Current(identity), "live HWND is bound to its process and thread");
        ++identity.process;
        check(!protocol::Current(identity), "a copied HWND with another process is rejected");
        identity = protocol::Capture(fixture);
        DestroyWindow(fixture);
        check(!protocol::Current(identity), "destroyed targets invalidate deferred actions");
    }
    std::printf("window support capability matrix: %s\n", failed ? "FAILED" : "passed");
    return failed ? 1 : 0;
}

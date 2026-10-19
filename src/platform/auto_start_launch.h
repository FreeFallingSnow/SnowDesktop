#pragma once

#include <windows.h>
#include <shellapi.h>

#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snowdesktop::auto_start_launch
{
struct Failure
{
    DWORD code;
    const wchar_t* operation;
};

inline void Require(bool success, const wchar_t* operation)
{
    if (!success)
    {
        const DWORD code = GetLastError();
        throw Failure{code ? code : ERROR_GEN_FAILURE, operation};
    }
}

class Handle final
{
public:
    HANDLE value = nullptr;
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { if (value) CloseHandle(value); }
};

struct Identity
{
    std::vector<BYTE> user;
    DWORD session = 0;
    DWORD integrity = 0;
    bool elevated = false;
};

inline std::vector<BYTE> TokenBuffer(HANDLE token, TOKEN_INFORMATION_CLASS kind)
{
    DWORD size = 0;
    GetTokenInformation(token, kind, nullptr, 0, &size);
    Require(size != 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER,
        L"GetTokenInformation(size)");
    std::vector<BYTE> buffer(size);
    Require(GetTokenInformation(token, kind, buffer.data(), size, &size) != FALSE,
        L"GetTokenInformation");
    return buffer;
}

inline Identity ReadIdentity(HANDLE token)
{
    Identity identity;
    const auto user = TokenBuffer(token, TokenUser);
    const PSID sid = reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid;
    Require(IsValidSid(sid) != FALSE, L"IsValidSid(user)");
    identity.user.resize(GetLengthSid(sid));
    Require(CopySid(static_cast<DWORD>(identity.user.size()), identity.user.data(), sid) != FALSE,
        L"CopySid(user)");
    const auto integrity = TokenBuffer(token, TokenIntegrityLevel);
    const PSID label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(integrity.data())->Label.Sid;
    Require(IsValidSid(label) != FALSE && *GetSidSubAuthorityCount(label) != 0,
        L"IsValidSid(integrity)");
    identity.integrity = *GetSidSubAuthority(label,
        static_cast<DWORD>(*GetSidSubAuthorityCount(label) - 1));
    DWORD size = 0;
    Require(GetTokenInformation(token, TokenSessionId, &identity.session,
        sizeof(identity.session), &size) != FALSE, L"GetTokenInformation(session)");
    TOKEN_ELEVATION elevation{};
    Require(GetTokenInformation(token, TokenElevation, &elevation,
        sizeof(elevation), &size) != FALSE, L"GetTokenInformation(elevation)");
    identity.elevated = elevation.TokenIsElevated != 0;
    return identity;
}

inline bool SameDesktopUser(const Identity& left, const Identity& right)
{
    return !left.user.empty() && left.user == right.user && left.session == right.session;
}

inline bool IsAutoStartCommand(std::wstring_view command)
{
    const std::wstring terminated(command);
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(terminated.c_str(), &count);
    Require(arguments != nullptr, L"CommandLineToArgvW(autostart)");
    struct Arguments final
    {
        LPWSTR* value;
        ~Arguments() { LocalFree(value); }
    } cleanup{arguments};
    for (int index = 1; index < count; ++index)
    {
        const std::wstring_view argument(arguments[index]);
        if (argument == L"--snowdesktop-autostart-owner=portable" ||
            argument == L"--snowdesktop-autostart-owner=packaged" ||
            argument == L"--snowdesktop-autostart-owner=steam")
            return true;
    }
    return false;
}

struct Result
{
    bool handled = false;
    DWORD error = ERROR_SUCCESS;
    DWORD childProcessId = 0;
    const wchar_t* operation = L"AutoStart desktop-user handoff";
};

// Tests observe the real desktop-parent creation or inject a creation failure.
using ProcessCreator = decltype(&CreateProcessW);
using ShellWindowProvider = decltype(&GetShellWindow);

class NativePlatform final
{
public:
    explicit NativePlatform(ProcessCreator creator = &CreateProcessW,
        ShellWindowProvider shellWindow = &GetShellWindow)
        : creator_(creator), shellWindow_(shellWindow) {}

    Identity CurrentIdentity()
    {
        Handle token;
        Require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value) != FALSE,
            L"OpenProcessToken(autostart)");
        return ReadIdentity(token.value);
    }

    Identity DesktopIdentity()
    {
        // Logon tasks may run before Explorer creates its desktop. Wait only
        // for that shell, never start a second desktop or fall back to elevation.
        const ULONGLONG deadline = GetTickCount64() + 30000;
        HWND shell = shellWindow_();
        while (!shell && GetTickCount64() < deadline)
        {
            Sleep(100);
            shell = shellWindow_();
        }
        if (!shell) throw Failure{ERROR_TIMEOUT, L"GetShellWindow(autostart)"};
        DWORD processId = 0;
        Require(GetWindowThreadProcessId(shell, &processId) != 0,
            L"GetWindowThreadProcessId(shell)");
        shellProcess_.value = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        Require(shellProcess_.value != nullptr, L"OpenProcess(shell)");
        Require(OpenProcessToken(shellProcess_.value, TOKEN_QUERY,
            &shellToken_.value) != FALSE, L"OpenProcessToken(shell)");
        return ReadIdentity(shellToken_.value);
    }

    DWORD Relaunch(const Identity& desktop, std::wstring_view command)
    {
        // CreateProcessW's Unicode command limit includes the terminating null.
        if (command.size() >= 32767)
            throw Failure{ERROR_FILENAME_EXCED_RANGE, L"AutoStart command length"};
        std::wstring executable(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
            static_cast<DWORD>(executable.size()));
        Require(length != 0, L"GetModuleFileNameW(autostart)");
        if (length >= executable.size())
            throw Failure{ERROR_INSUFFICIENT_BUFFER, L"GetModuleFileNameW(autostart)"};
        executable.resize(length);
        const DWORD directorySize = GetCurrentDirectoryW(0, nullptr);
        Require(directorySize != 0, L"GetCurrentDirectoryW(size)");
        std::wstring directory(directorySize, L'\0');
        const DWORD copied = GetCurrentDirectoryW(directorySize, directory.data());
        Require(copied != 0 && copied < directorySize, L"GetCurrentDirectoryW");
        directory.resize(copied);
        std::wstring mutableCommand(command);
        LPWCH environment = GetEnvironmentStringsW();
        Require(environment != nullptr, L"GetEnvironmentStringsW(autostart)");
        struct Environment final
        {
            LPWCH value;
            ~Environment() { FreeEnvironmentStringsW(value); }
        } environmentCleanup{environment};
        struct Child final
        {
            PROCESS_INFORMATION info{};
            bool resumed = false;
            ~Child()
            {
                // Only this newly created, never-resumed process may be retired.
                if (info.hProcess && !resumed)
                    TerminateProcess(info.hProcess, ERROR_ACCESS_DENIED);
                if (info.hThread) CloseHandle(info.hThread);
                if (info.hProcess) CloseHandle(info.hProcess);
            }
        } child;
        CreateWithDesktopParent(executable.c_str(), mutableCommand.data(),
            environment, directory.c_str(), &child.info);
        Handle childToken;
        Require(OpenProcessToken(child.info.hProcess, TOKEN_QUERY | WRITE_DAC | WRITE_OWNER,
            &childToken.value) != FALSE,
            L"OpenProcessToken(autostart child)");
        const Identity actual = ReadIdentity(childToken.value);
        if (!SameDesktopUser(actual, desktop) || actual.elevated ||
            actual.integrity != desktop.integrity)
            throw Failure{ERROR_ACCESS_DENIED, L"AutoStart child desktop-user verification"};
        PrepareChildTokenObject(childToken.value, desktop);
        Require(ResumeThread(child.info.hThread) != static_cast<DWORD>(-1),
            L"ResumeThread(autostart child)");
        child.resumed = true;
        return child.info.dwProcessId;
    }

private:
    void PrepareChildTokenObject(HANDLE child, const Identity& desktop)
    {
        // Parent selection supplies the desktop's security context, but the new
        // token object's descriptor can still come from the elevated creator.
        // Repair only this new token so the child can open its own token. Never
        // mutate the desktop process's existing token or change token privileges.
        const auto sourceStats = TokenBuffer(shellToken_.value, TokenStatistics);
        const auto childStats = TokenBuffer(child, TokenStatistics);
        const auto sourceId = reinterpret_cast<const TOKEN_STATISTICS*>(sourceStats.data())->TokenId;
        const auto childId = reinterpret_cast<const TOKEN_STATISTICS*>(childStats.data())->TokenId;
        if (sourceId.LowPart == childId.LowPart && sourceId.HighPart == childId.HighPart)
            throw Failure{ERROR_INVALID_DATA, L"AutoStart child token is not independent"};
        const auto defaults = TokenBuffer(shellToken_.value, TokenDefaultDacl);
        PACL dacl = reinterpret_cast<const TOKEN_DEFAULT_DACL*>(defaults.data())->DefaultDacl;
        if (!dacl || !IsValidAcl(dacl))
            throw Failure{ERROR_INVALID_DATA, L"AutoStart desktop default DACL"};
        SID_IDENTIFIER_AUTHORITY authority = SECURITY_MANDATORY_LABEL_AUTHORITY;
        BYTE sid[SECURITY_MAX_SID_SIZE]{};
        Require(InitializeSid(sid, &authority, 1) != FALSE, L"InitializeSid(autostart label)");
        *GetSidSubAuthority(sid, 0) = desktop.integrity;
        std::vector<BYTE> storage(sizeof(ACL) + sizeof(SYSTEM_MANDATORY_LABEL_ACE) + GetLengthSid(sid));
        auto* sacl = reinterpret_cast<PACL>(storage.data());
        Require(InitializeAcl(sacl, static_cast<DWORD>(storage.size()), ACL_REVISION) != FALSE,
            L"InitializeAcl(autostart label)");
        Require(AddMandatoryAce(sacl, ACL_REVISION, 0, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP, sid) != FALSE,
            L"AddMandatoryAce(autostart label)");
        SECURITY_DESCRIPTOR descriptor{};
        Require(InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) != FALSE,
            L"InitializeSecurityDescriptor(autostart token)");
        Require(SetSecurityDescriptorDacl(&descriptor, TRUE, dacl, FALSE) != FALSE,
            L"SetSecurityDescriptorDacl(autostart token)");
        Require(SetSecurityDescriptorSacl(&descriptor, TRUE, sacl, FALSE) != FALSE,
            L"SetSecurityDescriptorSacl(autostart token)");
        Require(SetKernelObjectSecurity(child, DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION,
            &descriptor) != FALSE, L"SetKernelObjectSecurity(autostart child token)");
    }

    void CreateWithDesktopParent(LPCWSTR executable, LPWSTR command,
        LPVOID environment, LPCWSTR directory, LPPROCESS_INFORMATION child)
    {
        Handle parent;
        parent.value = OpenProcess(PROCESS_CREATE_PROCESS, FALSE, GetProcessId(shellProcess_.value));
        Require(parent.value != nullptr, L"OpenProcess(autostart desktop parent)");
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        Require(size != 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER,
            L"InitializeProcThreadAttributeList(autostart size)");
        std::vector<BYTE> storage(size);
        auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        Require(InitializeProcThreadAttributeList(list, 1, 0, &size) != FALSE,
            L"InitializeProcThreadAttributeList(autostart)");
        struct Attributes final
        {
            LPPROC_THREAD_ATTRIBUTE_LIST value;
            ~Attributes() { DeleteProcThreadAttributeList(value); }
        } attributes{list};
        Require(UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS,
            &parent.value, sizeof(parent.value), nullptr, nullptr) != FALSE,
            L"UpdateProcThreadAttribute(autostart desktop parent)");
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = list;
        Require(creator_(executable, command, nullptr, nullptr, FALSE,
            CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT |
                EXTENDED_STARTUPINFO_PRESENT,
            environment, directory, &startup.StartupInfo, child) != FALSE,
            L"CreateProcessW(autostart desktop parent)");
    }

    ProcessCreator creator_;
    ShellWindowProvider shellWindow_;
    Handle shellProcess_;
    Handle shellToken_;
};

template<typename Platform>
Result Normalize(std::wstring_view command, Platform& platform) noexcept
{
    try
    {
        // Explicit administrator launches and privileged helper commands retain
        // their existing behavior. Only the existing login-owner tokens opt in.
        if (!IsAutoStartCommand(command)) return {};
        const Identity current = platform.CurrentIdentity();
        if (!current.elevated && current.integrity < SECURITY_MANDATORY_HIGH_RID)
            return {};
        const Identity desktop = platform.DesktopIdentity();
        if (!SameDesktopUser(current, desktop))
            throw Failure{ERROR_ACCESS_DENIED, L"AutoStart shell user/session mismatch"};
        if (desktop.elevated || desktop.integrity >= SECURITY_MANDATORY_HIGH_RID)
            return {};
        if (desktop.integrity < SECURITY_MANDATORY_MEDIUM_RID)
            throw Failure{ERROR_ACCESS_DENIED, L"AutoStart shell integrity"};
        return {true, ERROR_SUCCESS, platform.Relaunch(desktop, command)};
    }
    catch (const Failure& failure)
    {
        return {true, failure.code, 0, failure.operation};
    }
    catch (const std::bad_alloc&)
    {
        return {true, ERROR_OUTOFMEMORY, 0, L"AutoStart handoff allocation"};
    }
    catch (...)
    {
        return {true, ERROR_GEN_FAILURE, 0, L"AutoStart handoff exception"};
    }
}

inline Result Normalize() noexcept
{
    NativePlatform platform;
    return Normalize(GetCommandLineW(), platform);
}
}

#include "window_support_security.h"
#include "window_support_protocol.h"

#include <sddl.h>
#include <shlobj.h>
#include <vector>

namespace snowdesktop::window_support
{
namespace
{
struct Handle { HANDLE value = nullptr; ~Handle() { if (value) CloseHandle(value); } };
}
bool ReadTokenIdentity(HANDLE process, TokenIdentity& identity) noexcept
{
    try
    {
        Handle token;
        if (!OpenProcessToken(process, TOKEN_QUERY, &token.value)) return false;
        DWORD size = 0;
        GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
        if (!size) return false;
        std::vector<BYTE> user(size);
        if (!GetTokenInformation(token.value, TokenUser, user.data(), size, &size)) return false;
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) return false;
        try { identity.sid = sid; } catch (...) { LocalFree(sid); throw; }
        LocalFree(sid);
        alignas(TOKEN_MANDATORY_LABEL) BYTE label[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE]{};
        if (!GetTokenInformation(token.value, TokenIntegrityLevel, label, sizeof(label), &size)) return false;
        const auto value = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(label);
        if (!IsValidSid(value->Label.Sid)) return false;
        const BYTE count = *GetSidSubAuthorityCount(value->Label.Sid);
        if (!count) return false;
        identity.integrity = *GetSidSubAuthority(value->Label.Sid, count - 1u);
        TOKEN_ELEVATION elevation{};
        DWORD uiAccess = 0;
        if (!GetTokenInformation(token.value, TokenElevation, &elevation, sizeof(elevation), &size) ||
            !GetTokenInformation(token.value, TokenUIAccess, &uiAccess, sizeof(uiAccess), &size) ||
            !GetTokenInformation(token.value, TokenSessionId, &identity.session, sizeof(identity.session), &size)) return false;
        identity.elevated = elevation.TokenIsElevated != 0;
        identity.uiAccess = uiAccess != 0;
        return true;
    }
    catch (...) { return false; }
}
std::wstring ProcessImage(HANDLE process)
{
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) return {};
    path.resize(size);
    return path;
}
std::wstring CurrentUserSid()
{
    TokenIdentity identity;
    return ReadTokenIdentity(GetCurrentProcess(), identity) ? identity.sid : std::wstring{};
}
std::filesystem::path HelperDirectory()
{
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &path))) return {};
    std::filesystem::path base;
    try { base = path; } catch (...) { CoTaskMemFree(path); throw; }
    CoTaskMemFree(path);
    const auto sid = CurrentUserSid();
    return sid.empty() ? std::filesystem::path{} : base / L"SnowDesktop" / L"WindowSupport" / sid;
}
std::filesystem::path HelperImage()
{ const auto directory = HelperDirectory(); return directory.empty() ? directory : directory / protocol::HelperFilename; }
std::wstring HelperPipeName()
{
    TokenIdentity identity;
    if (!ReadTokenIdentity(GetCurrentProcess(), identity)) return {};
    return L"\\\\.\\pipe\\SnowDesktop.WindowSupport.v1." + identity.sid + L"." + std::to_wstring(identity.session);
}
std::wstring HelperTaskName() { return CurrentUserSid(); }
bool SameInteractiveUser(HANDLE process) noexcept
{
    TokenIdentity own, other;
    return ReadTokenIdentity(GetCurrentProcess(), own) && ReadTokenIdentity(process, other) &&
        own.sid == other.sid && own.session == other.session;
}
bool TargetInCurrentSession(HWND window) noexcept
{
    if (!window || !IsWindow(window) || GetAncestor(window, GA_ROOT) != window) return false;
    DWORD process = 0, session = 0, ownSession = 0;
    return GetWindowThreadProcessId(window, &process) && process &&
        ProcessIdToSessionId(process, &session) && ProcessIdToSessionId(GetCurrentProcessId(), &ownSession) &&
        session == ownSession;
}
bool NativeTargetAccessible(HWND window) noexcept
{
    if (!TargetInCurrentSession(window)) return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == GetCurrentProcessId()) return true;
    Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId)};
    TokenIdentity own, target;
    return process.value && ReadTokenIdentity(GetCurrentProcess(), own) &&
        ReadTokenIdentity(process.value, target) &&
        (own.uiAccess || own.integrity >= target.integrity);
}
bool IsProtectedHelper(HANDLE process) noexcept
{
    try
    {
        TokenIdentity identity;
        if (!SameInteractiveUser(process) || !ReadTokenIdentity(process, identity) ||
            !identity.elevated || identity.integrity < SECURITY_MANDATORY_HIGH_RID) return false;
        const auto image = HelperImage();
        const auto attributes = GetFileAttributesW(image.c_str());
        return !image.empty() && attributes != INVALID_FILE_ATTRIBUTES &&
            !(attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            _wcsicmp(ProcessImage(process).c_str(), image.c_str()) == 0;
    }
    catch (...) { return false; }
}
}

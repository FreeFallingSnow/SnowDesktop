#include "window_support_security.h"
#include "window_support_protocol.h"
#include "window_support_host_identity.h"

#include <sddl.h>
#include <shlobj.h>
#include <vector>
#include <mutex>
#include <algorithm>

namespace snowdesktop::window_support
{
namespace
{
struct Handle { HANDLE value = nullptr; ~Handle() { if (value) CloseHandle(value); } };
std::mutex scopeMutex;
std::wstring selectedScope;
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
std::wstring HelperAuthorizationScope()
{
    std::lock_guard lock(scopeMutex);
    if (selectedScope.empty()) selectedScope = HostAuthorizationKey(HostProcessIdentity(GetCurrentProcess()));
    return selectedScope;
}
bool SelectHelperAuthorizationScope(std::wstring_view key)
{
    if (key.size() != 64 || !std::all_of(key.begin(), key.end(), [](wchar_t ch) {
        return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f'); })) return false;
    std::lock_guard lock(scopeMutex);
    if (!selectedScope.empty() && selectedScope != key) return false;
    selectedScope = key;
    return true;
}
std::filesystem::path LegacyHelperDirectory()
{
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &path))) return {};
    std::filesystem::path base;
    try { base = path; } catch (...) { CoTaskMemFree(path); throw; }
    CoTaskMemFree(path);
    const auto sid = CurrentUserSid();
    return sid.empty() ? std::filesystem::path{} : base / L"SnowDesktop" / L"WindowSupport" / sid;
}
std::filesystem::path HelperDirectory()
{
    const auto root = LegacyHelperDirectory();
    const auto scope = HelperAuthorizationScope();
    return root.empty() || scope.empty() ? std::filesystem::path{} : root / scope;
}
std::filesystem::path HelperImage()
{ const auto directory = HelperDirectory(); return directory.empty() ? directory : directory / protocol::HelperFilename; }
std::wstring HelperPipeName()
{
    TokenIdentity identity;
    if (!ReadTokenIdentity(GetCurrentProcess(), identity)) return {};
    const auto scope = HelperAuthorizationScope();
    if (scope.empty()) return {};
    return L"\\\\.\\pipe\\SnowDesktop.WindowSupport.v1." + identity.sid + L"." + std::to_wstring(identity.session) + L"." + scope;
}
std::wstring HelperTaskName()
{
    const auto sid = CurrentUserSid(), scope = HelperAuthorizationScope();
    return sid.empty() || scope.empty() ? std::wstring{} : sid + L"." + scope;
}
HANDLE CreateHelperConnectionPipe(SECURITY_ATTRIBUTES* security)
{
    const auto name = HelperPipeName();
    if (name.empty()) { SetLastError(ERROR_INVALID_NAME); return INVALID_HANDLE_VALUE; }
    // Separate instances let another live host use the same installation.
    return CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, sizeof(protocol::Response), sizeof(protocol::Request), 0, security);
}
std::wstring AuthorizationSecurityDescriptor(bool sharedFolder)
{
    const auto sid = CurrentUserSid();
    if (sid.empty()) return {};
    // The split-token user must not regain WRITE_DAC through ownership.
    return L"O:BAG:BAD:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGX;;;" +
        (sharedFolder ? std::wstring(L"BU") : sid) + L")";
}
bool TrustedAuthorizationSecurity(const wchar_t* text) noexcept
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!text || !ConvertStringSecurityDescriptorToSecurityDescriptorW(text, SDDL_REVISION_1, &descriptor, nullptr)) return false;
    PSID owner = nullptr;
    BOOL defaulted = FALSE, present = FALSE;
    PACL dacl = nullptr;
    bool trusted = GetSecurityDescriptorOwner(descriptor, &owner, &defaulted) &&
        (IsWellKnownSid(owner, WinBuiltinAdministratorsSid) || IsWellKnownSid(owner, WinLocalSystemSid)) &&
        GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) && present && dacl;
    for (DWORD i = 0; trusted && i < dacl->AceCount; ++i)
    {
        void* raw = nullptr;
        if (!GetAce(dacl, i, &raw)) { trusted = false; break; }
        const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
        if (ace->Header.AceType == ACCESS_DENIED_ACE_TYPE) continue;
        if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE) { trusted = false; break; }
        auto* sid = const_cast<DWORD*>(&ace->SidStart);
        constexpr DWORD writeRights = GENERIC_ALL | GENERIC_WRITE | WRITE_DAC | WRITE_OWNER | DELETE |
            FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES;
        if (!IsWellKnownSid(sid, WinBuiltinAdministratorsSid) && !IsWellKnownSid(sid, WinLocalSystemSid) &&
            (ace->Mask & writeRights)) trusted = false;
    }
    LocalFree(descriptor);
    return trusted;
}
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

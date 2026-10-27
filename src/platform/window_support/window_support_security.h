#pragma once

#include <windows.h>
#include <filesystem>
#include <string>

namespace snowdesktop::window_support
{
struct TokenIdentity
{
    std::wstring sid;
    DWORD integrity = 0, session = 0;
    bool elevated = false, uiAccess = false;
};
bool ReadTokenIdentity(HANDLE process, TokenIdentity& identity) noexcept;
std::wstring ProcessImage(HANDLE process);
std::wstring CurrentUserSid();
std::filesystem::path HelperDirectory();
std::filesystem::path HelperImage();
std::wstring HelperPipeName();
std::wstring HelperTaskName();
std::wstring AuthorizationSecurityDescriptor(bool sharedFolder = false);
bool TrustedAuthorizationSecurity(const wchar_t* descriptor) noexcept;
bool SameInteractiveUser(HANDLE process) noexcept;
bool TargetInCurrentSession(HWND window) noexcept;
bool NativeTargetAccessible(HWND window) noexcept;
bool IsProtectedHelper(HANDLE process) noexcept;
}

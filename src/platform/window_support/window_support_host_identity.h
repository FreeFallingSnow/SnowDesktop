#pragma once

#include <windows.h>
#include <filesystem>
#include <string>
#include <string_view>

namespace snowdesktop::window_support
{
// Only Steam's explicit managed runtime slot is version-independent. Portable
// hosts retain their exact executable location; arbitrary sibling paths fail.
std::wstring HostPathIdentity(const std::filesystem::path& image);
std::wstring HostProcessIdentity(HANDLE process);
std::wstring HostAuthorizationKey(std::wstring_view identity);
bool HostIdentityMatches(std::wstring_view recorded, std::wstring_view current);
bool AuthorizedHostMatches(std::wstring_view recorded, HANDLE process);
}

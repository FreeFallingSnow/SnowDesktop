#include "window_support_host_identity.h"
#include "window_support_security.h"

#include <appmodel.h>
#include <algorithm>

namespace snowdesktop::window_support
{
namespace
{
constexpr std::wstring_view pathPrefix = L"SnowDesktopHost:2:path:";
constexpr std::wstring_view steamPrefix = L"SnowDesktopHost:2:steam:";
constexpr std::wstring_view packagePrefix = L"SnowDesktopHost:2:package:";
bool Equal(std::wstring_view first, std::wstring_view second)
{
    return first.size() == second.size() && CompareStringOrdinal(first.data(), static_cast<int>(first.size()),
        second.data(), static_cast<int>(second.size()), TRUE) == CSTR_EQUAL;
}
bool RuntimeIdentifier(std::wstring_view text)
{
    return !text.empty() && text.size() <= 96 && text != L"." && text != L".." &&
        std::all_of(text.begin(), text.end(), [](wchar_t ch) {
            return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'z') ||
                (ch >= L'A' && ch <= L'Z') || ch == L'-' || ch == L'_' || ch == L'.';
        });
}
std::wstring PackageIdentity(HANDLE process)
{
    UINT32 length = 0;
    if (GetPackageFamilyName(process, &length, nullptr) != ERROR_INSUFFICIENT_BUFFER || !length) return {};
    std::wstring family(length, L'\0');
    if (GetPackageFamilyName(process, &length, family.data()) != ERROR_SUCCESS || !length) return {};
    family.resize(length - 1u);
    return std::wstring(packagePrefix) + family;
}
std::wstring LegacyPackageIdentity(const std::filesystem::path& image)
{
    for (auto parent = image.parent_path(); !parent.empty(); parent = parent.parent_path())
    {
        const auto name = parent.filename().wstring();
        UINT32 length = 0;
        if (PackageFamilyNameFromFullName(name.c_str(), &length, nullptr) == ERROR_INSUFFICIENT_BUFFER && length)
        {
            std::wstring family(length, L'\0');
            if (PackageFamilyNameFromFullName(name.c_str(), &length, family.data()) == ERROR_SUCCESS)
            {
                family.resize(length - 1u);
                return std::wstring(packagePrefix) + family;
            }
        }
        if (parent == parent.parent_path()) break;
    }
    return {};
}
}
std::wstring HostPathIdentity(const std::filesystem::path& image)
{
    if (!image.is_absolute() || !Equal(image.filename().wstring(), L"SnowDesktop.exe")) return {};
    for (const auto& part : image)
        if (part == L".." || part == L".") return {};
    const auto version = image.parent_path();
    const auto runtime = version.parent_path();
    const auto state = runtime.parent_path();
    if (Equal(runtime.filename().wstring(), L"runtime") && Equal(state.filename().wstring(), L".snowdesktop") &&
        RuntimeIdentifier(version.filename().wstring()))
        return std::wstring(steamPrefix) + runtime.wstring();
    return std::wstring(pathPrefix) + image.wstring();
}
std::wstring HostProcessIdentity(HANDLE process)
{
    const auto image = std::filesystem::path(ProcessImage(process));
    const auto path = HostPathIdentity(image);
    if (path.empty()) return {};
    const auto package = PackageIdentity(process);
    return package.empty() ? path : package;
}
bool HostIdentityMatches(std::wstring_view recorded, std::wstring_view current)
{
    if (recorded.empty() || recorded.find(L'\0') != std::wstring_view::npos) return false;
    if (current.empty()) return false;
    if (recorded.starts_with(L"SnowDesktopHost:2:")) return Equal(recorded, current);
    // Existing protected records contain a literal executable path. Resolve
    // their stable anchor without reading a deleted old version directory.
    const auto legacy = std::filesystem::path(recorded);
    const auto oldPath = HostPathIdentity(legacy);
    if (oldPath.empty()) return false;
    if (Equal(oldPath, current)) return true;
    const auto package = LegacyPackageIdentity(legacy);
    return !package.empty() && Equal(package, current);
}
bool AuthorizedHostMatches(std::wstring_view recorded, HANDLE process)
{ return HostIdentityMatches(recorded, HostProcessIdentity(process)); }
}

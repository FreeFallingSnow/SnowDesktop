#include "shell_extension_catalogue.h"
#include "shell_extension_display_name.h"
#include "shell_extension_application.h"
#include "shell_catalogue_workers.h"
#include <atomic>
#include <future>
#include <thread>
#include <stdexcept>
#include "shell_extension_diagnostics.h"
#include "diagnostics/performance_trace.h"
#include "ui/menu/menu_label.h"
#include <algorithm>
#include <cwctype>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <mutex>
#include <shlwapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <winver.h>
#include <msxml6.h>
#include <appmodel.h>
#include <wrl/client.h>

namespace snowdesktop::shell_extensions
{
namespace
{
using Microsoft::WRL::ComPtr;
std::wstring Lower(std::wstring value) { for (auto &c : value) c = towlower(c); return value; }
std::string Utf8(const std::wstring &s)
{
    const auto n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string value(n, 0);
    if (n) WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), value.data(), n, nullptr, nullptr);
    return value;
}
struct Key
{
    HKEY value = nullptr;
    bool owned = false;
    Key(HKEY root, const std::wstring &path, REGSAM access = KEY_READ)
    {
        RegOpenKeyExW(root, path.c_str(), 0, access, &value);
        // Opening an empty path on a predefined key returns the borrowed root.
        owned = value && value != root;
    }
    ~Key() { if (owned) RegCloseKey(value); }
    Key(const Key &) = delete;
};
std::vector<std::wstring> Children(HKEY root, const std::wstring &path,
                                   CatalogueForegroundDemand *control = nullptr)
{
    if (control) control->WaitUntilAllowed();
    Key key(root, path);
    std::vector<std::wstring> result;
    if (!key.value) return result;
    for (DWORD i = 0; i < 65536; ++i)
    {
        if (control && i % 64 == 0) control->WaitUntilAllowed();
        wchar_t name[1024]{}; DWORD count = 1024;
        if (RegEnumKeyExW(key.value, i, name, &count, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        result.emplace_back(name, count);
    }
    return result;
}
std::vector<std::wstring> Values(HKEY root, const std::wstring &path)
{
    Key key(root, path); std::vector<std::wstring> result;
    if (!key.value) return result;
    for (DWORD i = 0; i < 65536; ++i)
    {
        wchar_t name[1024]{}; DWORD length = 1024;
        if (RegEnumValueW(key.value, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        result.emplace_back(name, length);
    }
    return result;
}
std::wstring Read(HKEY root, const std::wstring &path, const wchar_t *name = nullptr)
{
    if (!root) return {};
    // RegGetValue terminates successful string reads. Preserve the empty-data
    // case without clearing a 64 KiB buffer for every catalogue value.
    wchar_t text[32768]; text[0] = 0; DWORD bytes = sizeof(text);
    if (RegGetValueW(root, path.empty() ? nullptr : path.c_str(), name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, text, &bytes)) return {};
    return text;
}
bool Has(HKEY root, const std::wstring &path, const wchar_t *name)
{
    DWORD bytes = 0;
    return root && RegGetValueW(root, path.empty() ? nullptr : path.c_str(), name, RRF_RT_ANY, nullptr, nullptr, &bytes) == ERROR_SUCCESS;
}
bool Blocked(const std::wstring &clsid, HKEY user = HKEY_CURRENT_USER, HKEY machine = HKEY_LOCAL_MACHINE)
{
    if (clsid.empty()) return false;
    bool enforceApproved = false, approved = false;
    for (auto hive : {user, machine})
    {
        if (Has(hive, L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Blocked", clsid.c_str())) return true;
        DWORD enforce = 0, bytes = sizeof(enforce);
        if (RegGetValueW(hive, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"EnforceShellExtensionSecurity", RRF_RT_REG_DWORD, nullptr, &enforce, &bytes) == ERROR_SUCCESS)
            enforceApproved |= enforce != 0;
        approved |= Has(hive, L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved", clsid.c_str());
    }
    return enforceApproved && !approved;
}
std::wstring Localized(std::wstring text)
{
    wchar_t buffer[4096]{};
    if (!text.empty() && text.front() == L'@' && SUCCEEDED(SHLoadIndirectString(text.c_str(), buffer, 4096, nullptr))) return buffer;
    return text;
}
std::uint64_t Hash(std::span<const std::byte> bytes)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto byte : bytes) { hash ^= std::to_integer<unsigned char>(byte); hash *= 1099511628211ull; }
    return hash;
}
void LoadIcon(Entry &entry, std::wstring location)
{
    if (location.empty()) return;
    wchar_t path[32768]{};
    wcsncpy_s(path, location.c_str(), _TRUNCATE);
    const auto index = PathParseIconLocationW(path);
    if (PathIsNetworkPathW(path) || GetDriveTypeW(std::filesystem::path(path).root_path().c_str()) == DRIVE_REMOTE) return;
    struct IconRow { std::uint64_t stamp; Entry image; };
    static std::mutex iconMutex;
    static std::map<std::wstring, IconRow> icons;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) return;
    const auto stamp = (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    {
        std::lock_guard lock(iconMutex);
        if (const auto found = icons.find(location); found != icons.end() && found->second.stamp == stamp)
        { entry.width = found->second.image.width; entry.height = found->second.image.height; entry.pixels = found->second.image.pixels; return; }
    }
    HICON icon = nullptr;
    if (FAILED(SHDefExtractIconW(path, index, 0, nullptr, &icon, MAKELONG(20, 20))) || !icon) return;
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = 20;
    info.bmiHeader.biHeight = -20; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void *pixels = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (dc && bitmap && pixels)
    {
        const auto old = SelectObject(dc, bitmap);
        memset(pixels, 0, 1600);
        if (DrawIconEx(dc, 0, 0, icon, 20, 20, 0, nullptr, DI_NORMAL))
        {
            entry.width = entry.height = 20;
            entry.pixels.assign(static_cast<unsigned char *>(pixels), static_cast<unsigned char *>(pixels) + 1600);
        }
        SelectObject(dc, old);
    }
    if (bitmap) DeleteObject(bitmap);
    if (dc) DeleteDC(dc);
    DestroyIcon(icon);
    if (!entry.pixels.empty())
    {
        std::lock_guard lock(iconMutex);
        if (icons.size() >= 512) icons.erase(icons.begin());
        Entry image; image.width = entry.width; image.height = entry.height; image.pixels = entry.pixels;
        icons[location] = {stamp, std::move(image)};
    }
}
template<class T> void Unique(std::vector<T> &items) { std::sort(items.begin(), items.end()); items.erase(std::unique(items.begin(), items.end()), items.end()); }
std::wstring LocalModule(std::wstring path)
{
    if (path.empty() || path.size() >= 32768) return {};
    wchar_t expanded[32768]{};
    const auto size = ExpandEnvironmentStringsW(path.c_str(), expanded, static_cast<DWORD>(std::size(expanded)));
    if (!size || size > std::size(expanded)) return {};
    PathUnquoteSpacesW(expanded);
    path = expanded;
    if (!std::filesystem::path(path).is_absolute())
    {
        if (path.find_first_of(L"\\/") != std::wstring::npos) return {};
        // A bare registered executable can use App Paths or the system folder.
        // Do not resolve relative to the host's current working directory.
        std::wstring registered;
        for (const auto hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE})
            if (registered.empty()) registered = Read(hive, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + path);
        if (!registered.empty())
        {
            wchar_t value[32768]{}; wcsncpy_s(value, registered.c_str(), _TRUNCATE); PathUnquoteSpacesW(value);
            path = value;
        }
        else
        {
            wchar_t system[32768]{};
            if (!GetSystemDirectoryW(system, static_cast<UINT>(std::size(system)))) return {};
            path = (std::filesystem::path(system) / path).wstring();
        }
    }
    const std::filesystem::path file(path);
    if (!file.is_absolute() || PathIsNetworkPathW(path.c_str()) || GetDriveTypeW(file.root_path().c_str()) == DRIVE_REMOTE) return {};
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return {};
    wchar_t longPath[32768]{};
    if (GetLongPathNameW(path.c_str(), longPath, 32768)) return std::filesystem::path(longPath).lexically_normal().wstring();
    return file.lexically_normal().wstring();
}
std::wstring CommandModule(const std::wstring &command)
{
    if (command.empty()) return {};
    int count = 0;
    auto args = CommandLineToArgvW(command.c_str(), &count);
    if (!args) return {};
    auto module = count ? LocalModule(args[0]) : std::wstring{};
    // Recover existing unquoted executable paths written by some installers.
    // This only reads module metadata; it never runs the registered command.
    if (module.empty() && command.front() != L'"')
    {
        const auto lower = Lower(command);
        for (size_t at = lower.find(L".exe"); at != std::wstring::npos; at = lower.find(L".exe", at + 4))
            if (at + 4 == command.size() || iswspace(command[at + 4]))
                if (auto candidate = LocalModule(command.substr(0, at + 4)); !candidate.empty()) { module = std::move(candidate); break; }
    }
    const auto name = Lower(std::filesystem::path(module).filename().wstring());
    if (name == L"rundll32.exe")
    {
        std::wstring library = count > 1 ? args[1] : L"";
        if (const auto comma = library.find(L','); comma != std::wstring::npos) library.resize(comma);
        module = LocalModule(std::move(library));
    }
    else if (name == L"cmd.exe" || name == L"powershell.exe" || name == L"pwsh.exe" ||
        name == L"wscript.exe" || name == L"cscript.exe" || name == L"mshta.exe" || name == L"dllhost.exe")
        module.clear(); // A command interpreter does not identify its script's application.
    LocalFree(args);
    return module;
}
Application ModuleApplication(const std::wstring &module)
{
    if (module.empty()) return {};
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(module.c_str(), GetFileExInfoStandard, &data)) return {};
    const auto stamp = (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    const auto language = GetThreadUILanguage(), fallbackLanguage = GetUserDefaultUILanguage();
    const DWORD languages = MAKELONG(language, fallbackLanguage);
    struct Cached { std::uint64_t stamp; DWORD languages; Application value; };
    static std::mutex mutex;
    static std::map<std::wstring, Cached> cache;
    const auto key = Lower(module);
    {
        std::lock_guard lock(mutex);
        if (const auto found = cache.find(key); found != cache.end() && found->second.stamp == stamp && found->second.languages == languages) return found->second.value;
    }
    Application result{"file:" + Utf8(key), std::filesystem::path(module).stem().wstring()};
    DWORD unused = 0;
    const DWORD size = GetFileVersionInfoSizeW(module.c_str(), &unused);
    if (size && size <= 4 * 1024 * 1024)
    {
        std::vector<unsigned char> bytes(size);
        if (GetFileVersionInfoW(module.c_str(), 0, size, bytes.data()))
        {
            struct Translation { WORD language, codepage; };
            Translation *translations = nullptr; UINT length = 0;
            if (VerQueryValueW(bytes.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void **>(&translations), &length))
            {
                std::vector<Translation> preferred(translations, translations + std::min<size_t>(256, length / sizeof(Translation)));
                auto rank = [&](WORD value) { return value == language ? 0 : value == fallbackLanguage ? 1 :
                    PRIMARYLANGID(value) == PRIMARYLANGID(language) ? 2 : value == 0 ? 3 : 4; };
                std::stable_sort(preferred.begin(), preferred.end(), [&](const auto& a, const auto& b) { return rank(a.language) < rank(b.language); });
                for (const auto& translation : preferred)
                {
                    auto text = [&](const wchar_t *field) {
                        wchar_t query[128]{};
                        swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\%s", translation.language, translation.codepage, field);
                        wchar_t *value = nullptr; UINT characters = 0;
                        if (!VerQueryValueW(bytes.data(), query, reinterpret_cast<void **>(&value), &characters) || !value || characters <= 1 || characters > 512) return std::wstring{};
                        // Some vendors overstate wValueLength and leave template
                        // bytes after the terminator. Never turn that tail into a
                        // display name or application identity.
                        std::wstring label(value, std::find(value, value + characters, L'\0'));
                        const auto first = label.find_first_not_of(L" \t\r\n");
                        if (first == std::wstring::npos) return std::wstring{};
                        label = label.substr(first, label.find_last_not_of(L" \t\r\n") - first + 1);
                        // Unedited version-resource templates are not application names.
                        if (Lower(label).starts_with(L"todo:") || (label.front() == L'<' && label.back() == L'>')) return std::wstring{};
                        return label;
                    };
                    const auto product = text(L"ProductName"), company = text(L"CompanyName");
                    const auto description = text(L"FileDescription");
                    const auto name = UsableShellDisplayName(product) ? product : description;
                    if (!UsableShellDisplayName(name)) continue;
                    result.name = name;
                    // Product+company unifies one application's EXE and Shell
                    // DLL. It is display metadata, never command authority.
                    if (UsableShellDisplayName(product) && !company.empty()) result.id = "product:" + Utf8(Lower(company + L"\n" + product));
                    break;
                }
            }
        }
    }
    std::lock_guard lock(mutex);
    if (cache.size() >= 1024) cache.clear();
    cache[key] = {stamp, languages, result};
    return result;
}

std::wstring ReadApplicationDirectory(std::wstring directory)
{
    wchar_t expanded[32768]{};
    if (directory.empty() || !ExpandEnvironmentStringsW(directory.c_str(), expanded, 32768)) return {};
    PathUnquoteSpacesW(expanded); directory = expanded;
    const std::filesystem::path path(directory);
    if (!path.is_absolute() || path == path.root_path() || PathIsNetworkPathW(directory.c_str()) ||
        GetDriveTypeW(path.root_path().c_str()) == DRIVE_REMOTE) return {};
    const auto attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return {};
    wchar_t longPath[32768]{};
    if (GetLongPathNameW(directory.c_str(), longPath, 32768)) directory = longPath;
    directory = ApplicationPathKey(std::filesystem::path(directory).lexically_normal().wstring());
    static const auto generic = [] {
        std::vector<std::wstring> result;
        for (const auto* folder : {&FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86, &FOLDERID_ProgramData,
            &FOLDERID_LocalAppData, &FOLDERID_RoamingAppData, &FOLDERID_Profile, &FOLDERID_Windows,
            &FOLDERID_ProgramFilesCommon, &FOLDERID_ProgramFilesCommonX86, &FOLDERID_Desktop,
            &FOLDERID_Documents, &FOLDERID_Downloads, &FOLDERID_Public})
        {
            PWSTR value = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(*folder, KF_FLAG_DONT_VERIFY, nullptr, &value)))
            { result.push_back(ApplicationPathKey(value)); CoTaskMemFree(value); }
        }
        wchar_t temporary[32768]{};
        if (const auto length = GetTempPathW(32768, temporary); length && length < 32768)
            result.push_back(ApplicationPathKey(temporary));
        return result;
    }();
    if (std::find(generic.begin(), generic.end(), directory) != generic.end()) return {};
    wchar_t windows[32768]{};
    if (GetWindowsDirectoryW(windows, 32768) && ApplicationPathContains(ApplicationPathKey(windows), directory)) return {};
    return directory;
}
using ApplicationDirectoryCache = std::map<std::wstring, std::wstring>;
std::wstring ApplicationDirectory(std::wstring directory, ApplicationDirectoryCache& cache)
{
    const auto key = ApplicationPathKey(directory);
    if (const auto old = cache.find(key); old != cache.end()) return old->second;
    auto result = ReadApplicationDirectory(std::move(directory));
    cache.emplace(key, result);
    return result;
}
std::wstring ApplicationExecutableRoot(const std::wstring& module, ApplicationDirectoryCache& cache)
{
    auto directory = std::filesystem::path(module).parent_path();
    for (unsigned i = 0; i < 4; ++i)
    {
        const auto name = ApplicationPathKey(directory.filename().wstring());
        const bool version = !name.empty() && iswdigit(name.front()) &&
            std::all_of(name.begin(), name.end(), [](wchar_t c) { return iswdigit(c) || c == L'.'; });
        if (!version && name != L"bin" && name != L"bin32" && name != L"bin64" && name != L"win32" &&
            name != L"win64" && name != L"x86" && name != L"x64" && name != L"amd64" &&
            name != L"support files" && name != L"office6" && name != L"office16" &&
            name != L"app" && name != L"application") break;
        directory = directory.parent_path();
    }
    return ApplicationDirectory(directory.wstring(), cache);
}
struct ApplicationMetadataIndex
{
    ApplicationProfileIndex profiles;
    std::mutex mutex;
    std::map<std::wstring, ClassifiedApplication> modules;
    std::map<std::wstring, std::wstring> boundaries;
    std::map<std::string, std::wstring> icons;
    std::map<std::wstring, std::string> canonicalProducts;
    std::map<std::wstring, std::wstring> preferredIcons;
    // These memos live for exactly one inventory scan. Explicit refreshes
    // revalidate directories, installation records and icon files from scratch.
    ApplicationDirectoryCache directories;
    std::map<std::wstring, std::optional<InstalledApplicationRecord>> moduleProfiles;
    std::map<std::string, std::wstring> validatedIcons;
    explicit ApplicationMetadataIndex(HKEY classes, CatalogueForegroundDemand* foreground)
    {
        if (classes != HKEY_CLASSES_ROOT) return; // Private registry fixtures stay isolated.
        for (const auto hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE})
        {
            for (const auto* base : {L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"})
                for (const auto& key : Children(hive, base, foreground))
                {
                    const auto path = std::wstring(base) + L"\\" + key;
                    const auto name = Localized(Read(hive, path, L"DisplayName"));
                    if (!UsableShellDisplayName(name)) continue;
                    const auto icon = Read(hive, path, L"DisplayIcon");
                    auto root = ApplicationDirectory(Read(hive, path, L"InstallLocation"), directories);
                    wchar_t iconFile[32768]{}; wcsncpy_s(iconFile, icon.c_str(), _TRUNCATE); PathParseIconLocationW(iconFile);
                    const auto module = LocalModule(iconFile);
                    const auto iconRoot = ApplicationExecutableRoot(module, directories);
                    const auto version = std::filesystem::path(root).filename().wstring();
                    if (!root.empty() && !iconRoot.empty() && ApplicationPathContains(iconRoot, root) &&
                        !version.empty() && std::all_of(version.begin(), version.end(), [](wchar_t c) { return iswdigit(c) || c == L'.'; }))
                        root = iconRoot;
                    if (!root.empty()) profiles.Add({root, name, icon, 30});
                    if (!module.empty()) profiles.Add({iconRoot, name, icon, 20});
                    const auto uninstall = CommandModule(Read(hive, path, L"UninstallString"));
                    if (!uninstall.empty())
                    {
                        auto folder = std::filesystem::path(uninstall).parent_path();
                        const auto leaf = ApplicationPathKey(folder.filename().wstring());
                        if (leaf == L"uninstall" || leaf == L"uninstaller" || leaf == L"setup") folder = folder.parent_path();
                        const auto uninstallVersion = folder.filename().wstring();
                        if (!iconRoot.empty() && ApplicationPathContains(iconRoot, ApplicationPathKey(folder.wstring())) &&
                            !uninstallVersion.empty() && std::all_of(uninstallVersion.begin(), uninstallVersion.end(), [](wchar_t c) { return iswdigit(c) || c == L'.'; }))
                            folder = iconRoot;
                        profiles.Add({ApplicationDirectory(folder.wstring(), directories), name, icon, 20});
                    }
                }
            const std::wstring base = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths";
            for (const auto& key : Children(hive, base, foreground))
            {
                const auto module = CommandModule(Read(hive, base + L"\\" + key));
                if (module.empty()) continue;
                const auto application = ModuleApplication(module);
                profiles.Add({ApplicationExecutableRoot(module, directories), application.name, module, 10});
            }
        }
    }
    std::wstring Boundary(std::filesystem::path directory)
    {
        const auto key = ApplicationPathKey(directory.wstring());
        if (const auto old = boundaries.find(key); old != boundaries.end()) return old->second;
        std::vector<std::wstring> visited;
        std::wstring result;
        for (unsigned depth = 0; depth < 10 && !directory.empty() && directory != directory.root_path(); ++depth, directory = directory.parent_path())
        {
            const auto current = ApplicationPathKey(directory.wstring());
            if (const auto old = boundaries.find(current); old != boundaries.end()) { result = old->second; break; }
            if (ApplicationDirectory(directory.wstring(), directories).empty()) break;
            visited.push_back(current);
            const auto leaf = ApplicationPathKey(directory.filename().wstring());
            if (leaf == L"app" || leaf == L"application" || leaf == L"bin" || leaf == L"bin64" || leaf == L"bin32") continue;
            for (const auto* marker : {L"Uninstall.exe", L"unins000.exe", L"uninstall(x64).exe", L"uninstall(x86).exe"})
            {
                const auto file = directory / marker;
                const auto attributes = GetFileAttributesW(file.c_str());
                if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) { result = current; break; }
            }
            if (!result.empty()) break;
        }
        for (const auto& path : visited) boundaries[path] = result;
        return result;
    }
    std::optional<InstalledApplicationRecord> Profile(const std::wstring& module)
    {
        const auto key = ApplicationPathKey(module);
        if (const auto old = moduleProfiles.find(key); old != moduleProfiles.end()) return old->second;
        auto profile = profiles.Find(module);
        const auto boundary = !profile || profile->strength <= 10 ? Boundary(std::filesystem::path(module).parent_path()) : std::wstring{};
        if (!boundary.empty() && (!profile || (profile->strength <= 10 && ApplicationPathContains(boundary, profile->root))))
        {
            const auto actualName = [](const std::filesystem::path& path) {
                WIN32_FIND_DATAW data{};
                const auto search = FindFirstFileW(path.c_str(), &data);
                if (search == INVALID_HANDLE_VALUE) return path.filename().wstring();
                FindClose(search); return std::wstring(data.cFileName);
            };
            auto name = actualName(boundary);
            if (name.size() > 1 && towlower(name.front()) == L'v' && iswdigit(name[1]))
                name = actualName(std::filesystem::path(boundary).parent_path()) + L" " + name;
            profile = InstalledApplicationRecord{boundary, name, boundary + L"\\Uninstall.exe", 15};
        }
        // App Paths describes an executable, not necessarily an installation.
        if (profile && profile->strength <= 10) profile.reset();
        moduleProfiles.emplace(key, profile);
        return profile;
    }
    void Prepare(const std::map<std::string, std::wstring>& registrations, CatalogueForegroundDemand* foreground)
    {
        std::map<std::wstring, Application> registered;
        for (const auto& [id, module] : registrations)
        {
            if (foreground) foreground->WaitUntilAllowed();
            if (module.empty() || registered.contains(module)) continue;
            const auto application = ModuleApplication(module);
            registered.emplace(module, application);
            const auto root = ApplicationExecutableRoot(module, directories);
            if (ApplicationPathKey(std::filesystem::path(module).extension().wstring()) == L".exe" &&
                application.id.starts_with("product:") &&
                ApplicationPathKey(std::filesystem::path(root).filename().wstring()) == ApplicationPathKey(application.name))
                profiles.Add({root, application.name, module, 15});
        }
        std::map<std::wstring, std::set<std::string>> products;
        for (const auto& [module, application] : registered)
        {
            if (foreground) foreground->WaitUntilAllowed();
            const auto profile = Profile(module);
            if (!profile) continue;
            const auto name = ApplicationPathKey(ApplicationDisplayName(application.name));
            const auto installed = ApplicationPathKey(ApplicationDisplayName(profile->name));
            if (application.id.starts_with("product:") && name == installed) products[profile->root].insert(application.id);
            wchar_t iconFile[32768]{}; wcsncpy_s(iconFile, profile->icon.c_str(), _TRUNCATE); PathParseIconLocationW(iconFile);
            if (!name.empty() && installed.starts_with(name) && ApplicationPathKey(std::filesystem::path(module).extension().wstring()) == L".exe" &&
                !ApplicationPathContains(profile->root, ApplicationPathKey(iconFile)) && ApplicationExecutableRoot(module, directories) == profile->root)
                preferredIcons[profile->root] = module;
        }
        for (const auto& [root, values] : products) if (values.size() == 1) canonicalProducts[root] = *values.begin();
    }
    ClassifiedApplication Resolve(const std::wstring& module, const Application& fallback)
    {
        std::lock_guard lock(mutex);
        const auto key = ApplicationPathKey(module);
        if (const auto old = modules.find(key); old != modules.end()) return old->second;
        const auto profile = Profile(module);
        ClassifiedApplication result{fallback, {}};
        if (profile)
        {
            const auto canonical = canonicalProducts.find(profile->root);
            result.application = {canonical == canonicalProducts.end() ? "install:" + Utf8(profile->root) : canonical->second, profile->name};
            const auto preferred = preferredIcons.find(profile->root);
            result.icon = preferred == preferredIcons.end() ? profile->icon : preferred->second;
        }
        else if (const auto weak = profiles.Find(module); weak && weak->name == fallback.name)
            result.icon = profiles.SharedIcon(module);
        if (!result.icon.empty()) icons[result.application.id] = result.icon;
        modules.emplace(key, result); return result;
    }
    std::wstring Icon(const std::string& id)
    {
        std::lock_guard lock(mutex);
        if (const auto old = validatedIcons.find(id); old != validatedIcons.end()) return old->second;
        const auto found = icons.find(id);
        if (found == icons.end()) return {};
        wchar_t path[32768]{}; wcsncpy_s(path, found->second.c_str(), _TRUNCATE); PathParseIconLocationW(path);
        auto result = GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES ? std::wstring{} : found->second;
        validatedIcons.emplace(id, result);
        return result;
    }
};
// Index small ASCII value lists only for this verb. Missing metadata fields do
// not need individual registry queries. Unicode, large or unsuccessful lists
// retain direct reads; policy presence checks always use the live registry.
struct KeyValues
{
    HKEY key; bool indexed = false; std::vector<std::wstring> names;
    explicit KeyValues(HKEY source) : key(source)
    {
        DWORD count = 0, maximumName = 0;
        if (!key || RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            &count, &maximumName, nullptr, nullptr, nullptr) != ERROR_SUCCESS || count > 32 || maximumName >= 256) return;
        for (DWORD index = 0; index < count; ++index)
        {
            wchar_t name[256]; DWORD length = 256;
            if (RegEnumValueW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) return;
            std::wstring normal(name, length);
            for (auto &letter : normal)
            {
                if (letter > 127) return;
                if (letter >= L'A' && letter <= L'Z') letter += L'a' - L'A';
            }
            names.push_back(std::move(normal));
        }
        indexed = true;
    }
    std::wstring String(const wchar_t *name = nullptr) const
    {
        if (indexed)
        {
            std::wstring normal = name ? name : L"";
            for (auto &letter : normal) if (letter >= L'A' && letter <= L'Z') letter += L'a' - L'A';
            if (std::find(names.begin(), names.end(), normal) == names.end()) return {};
        }
        return Read(key, L"", name);
    }
};
// Cache parsed XML only, never registration or policy. Every scan re-enumerates
// packages and verifies file ID, size, write/change times before reusing results.
// Missing, malformed or unidentifiable files are not negatively cached.
struct ManifestVerb { std::wstring clsid, id, type, displayName, identity, module; };
struct ManifestStamp
{
    ULONGLONG volume;
    std::array<BYTE, 16> fileId;
    LONGLONG size, written, changed;
    bool operator==(const ManifestStamp &) const = default;
};
struct ManifestFile
{
    HANDLE handle;
    explicit ManifestFile(const std::wstring &path) : handle(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {}
    ~ManifestFile() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    std::optional<ManifestStamp> Stamp() const
    {
        FILE_ID_INFO id{}; FILE_BASIC_INFO basic{}; FILE_STANDARD_INFO standard{};
        if (handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) ||
            !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) ||
            !GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard))) return {};
        ManifestStamp stamp{id.VolumeSerialNumber, {}, standard.EndOfFile.QuadPart,
            basic.LastWriteTime.QuadPart, basic.ChangeTime.QuadPart};
        std::copy(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), stamp.fileId.begin());
        return stamp;
    }
};
std::wstring ManifestAttribute(IXMLDOMNode *node, const wchar_t *name)
{
    ComPtr<IXMLDOMNamedNodeMap> attributes; ComPtr<IXMLDOMNode> value;
    BSTR key = SysAllocString(name), text = nullptr;
    if (node && SUCCEEDED(node->get_attributes(&attributes)) && attributes) attributes->getNamedItem(key, &value);
    if (value) value->get_text(&text);
    std::wstring result = text ? text : L"";
    SysFreeString(key); SysFreeString(text); return result;
}
ComPtr<IXMLDOMNode> ManifestNode(IXMLDOMNode* node, const wchar_t* expression)
{
    ComPtr<IXMLDOMNode> result;
    BSTR query = SysAllocString(expression);
    if (node) node->selectSingleNode(query, &result);
    SysFreeString(query); return result;
}
std::wstring ManifestText(IXMLDOMNode* node)
{
    BSTR text = nullptr;
    if (node) node->get_text(&text);
    std::wstring result = text ? text : L"";
    SysFreeString(text); return result;
}
std::vector<ManifestVerb> ReadManifestVerbs(const std::wstring &path)
{
    struct Cached { ManifestStamp stamp; std::vector<ManifestVerb> verbs; };
    static std::mutex cacheMutex;
    static std::map<std::wstring, Cached> cache;
    ManifestFile file(path);
    const auto stamp = file.Stamp();
    if (stamp)
    {
        std::lock_guard lock(cacheMutex);
        const auto found = cache.find(path);
        if (found != cache.end() && found->second.stamp == *stamp) return found->second.verbs;
    }
    ComPtr<IXMLDOMDocument2> document;
    if (FAILED(CoCreateInstance(__uuidof(DOMDocument60), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&document)))) return {};
    document->put_async(VARIANT_FALSE); document->put_validateOnParse(VARIANT_FALSE); document->put_resolveExternals(VARIANT_FALSE);
    VARIANT filename{}; VariantInit(&filename); filename.vt = VT_BSTR;
    filename.bstrVal = SysAllocString(path.c_str());
    VARIANT_BOOL loaded = VARIANT_FALSE; document->load(filename, &loaded); VariantClear(&filename);
    if (!loaded) return {};
    BSTR query = SysAllocString(L"//*[local-name()='Extension' and @Category='windows.fileExplorerContextMenus']//*[local-name()='Verb']");
    ComPtr<IXMLDOMNodeList> nodes; document->selectNodes(query, &nodes); SysFreeString(query);
    if (!nodes) return {};
    long count = 0; nodes->get_length(&count);
    std::vector<ManifestVerb> result;
    for (long i = 0; i < count; ++i)
    {
        ComPtr<IXMLDOMNode> verb, parent; nodes->get_item(i, &verb); verb->get_parentNode(&parent);
        ManifestVerb item;
        item.clsid = ManifestAttribute(verb.Get(), L"Clsid"); item.id = ManifestAttribute(verb.Get(), L"Id");
        item.type = ManifestAttribute(parent.Get(), L"Type");
        item.displayName = ManifestText(ManifestNode(document.Get(), L"/*[local-name()='Package']/*[local-name()='Properties']/*[local-name()='DisplayName']").Get());
        item.identity = ManifestAttribute(ManifestNode(document.Get(), L"/*[local-name()='Package']/*[local-name()='Identity']").Get(), L"Name");
        auto application = ManifestNode(verb.Get(), L"ancestor::*[local-name()='Application'][1]");
        if (!application) application = ManifestNode(document.Get(), L"//*[local-name()='Application'][1]");
        item.module = ManifestAttribute(application.Get(), L"Executable");
        // COM class paths identify the actual provider when a sparse package's
        // application executable is only a launcher. Read metadata, never load it.
        ComPtr<IXMLDOMNodeList> classes;
        BSTR classQuery = SysAllocString(L"//*[local-name()='Class' and @Id]");
        document->selectNodes(classQuery, &classes); SysFreeString(classQuery);
        long classCount = 0; if (classes) classes->get_length(&classCount);
        auto normalized = [](std::wstring value) { std::erase(value, L'{'); std::erase(value, L'}'); return Lower(std::move(value)); };
        for (long c = 0; c < classCount; ++c)
        {
            ComPtr<IXMLDOMNode> type, server; classes->get_item(c, &type);
            if (normalized(ManifestAttribute(type.Get(), L"Id")) != normalized(item.clsid)) continue;
            auto modulePath = ManifestAttribute(type.Get(), L"Path");
            type->get_parentNode(&server);
            if (modulePath.empty()) modulePath = ManifestAttribute(server.Get(), L"Path");
            if (modulePath.empty()) modulePath = ManifestAttribute(server.Get(), L"Executable");
            if (!modulePath.empty()) { item.module = std::move(modulePath); break; }
        }
        result.push_back(std::move(item));
    }
    // Recheck the open object and the current path. A replacement during XML
    // loading must not associate a new manifest with the old file identity.
    ManifestFile current(path);
    if (stamp && file.Stamp() == stamp && current.Stamp() == stamp)
    {
        std::lock_guard lock(cacheMutex);
        if (cache.size() >= 1024) cache.clear();
        cache.insert_or_assign(path, Cached{*stamp, result});
    }
    return result;
}

std::wstring PackageDisplayName(const ManifestVerb& verb, const std::wstring& package, const std::wstring& directory)
{
    if (!Lower(verb.displayName).starts_with(L"ms-resource:"))
    {
        auto value = Localized(verb.displayName);
        return UsableShellDisplayName(value) ? value : std::wstring{};
    }
    std::vector<std::wstring> uris{verb.displayName};
    auto resource = verb.displayName.substr(12);
    if (!verb.identity.empty())
    {
        if (resource.starts_with(L"///")) uris.push_back(L"ms-resource://" + verb.identity + resource.substr(2));
        else if (!resource.starts_with(L"//"))
        {
            if (!resource.starts_with(L"/")) uris.push_back(L"ms-resource://" + verb.identity + L"/resources/" + resource);
            uris.push_back(L"ms-resource://" + verb.identity + (resource.starts_with(L"/") ? L"" : L"/") + resource);
        }
    }
    for (const auto& uri : uris)
        for (const auto& source : {package, directory + L"\\resources.pri"})
        {
            auto value = Localized(L"@{" + source + L"?" + uri + L"}");
            if (UsableShellDisplayName(value)) return value;
        }
    return {};
}

struct Scanner
{
    HKEY classes;
    CatalogueForegroundDemand *control = nullptr;
    bool capture = false;
    using CapturedRegistration = std::tuple<Registration, std::wstring, std::wstring>;
    std::vector<CapturedRegistration> captured;
    Catalogue result;
    std::vector<Registration> folderRows;
    std::array<std::vector<Registration>, 2> backgroundRows;
    std::map<std::string, size_t> ids;
    // One inventory contains many registrations of the same provider. Reuse
    // their metadata only within this scan; the next scan rechecks files,
    // registration values and policy instead of retaining negative results.
    struct HandlerMetadata { std::wstring label, module, icon; Application application; };
    std::map<std::wstring, HandlerMetadata> handlers;
    std::map<std::wstring, Application> commands, applications;
    std::map<std::string, std::wstring> registrationModules;
    std::map<std::wstring, std::wstring> labels;
    std::map<std::wstring, bool> policies;
    std::map<std::wstring, Entry> images;
    std::map<std::wstring, std::uint64_t> assets;
    std::map<std::wstring, std::vector<std::wstring>> associations;
    std::uint64_t AssetRevision(const std::wstring &location)
    {
        if (location.empty()) return 0;
        const auto [it, inserted] = assets.try_emplace(location);
        if (!inserted) return it->second;
        wchar_t path[32768]{};
        wcsncpy_s(path, location.c_str(), _TRUNCATE);
        PathParseIconLocationW(path);
        auto module = LocalModule(path[0] == L'@' ? path + 1 : path);
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!module.empty() && GetFileAttributesExW(module.c_str(), GetFileExInfoStandard, &data))
            it->second = Hash(settings_ipc::Pack(module, data.dwFileAttributes,
                data.ftLastWriteTime.dwHighDateTime, data.ftLastWriteTime.dwLowDateTime,
                data.nFileSizeHigh, data.nFileSizeLow));
        return it->second;
    }
    const std::vector<std::wstring> &Associations(const std::wstring &type)
    {
        const auto [it, inserted] = associations.try_emplace(type);
        if (inserted)
        {
            it->second.push_back(Read(classes, type));
            if (classes == HKEY_CLASSES_ROOT)
                it->second.push_back(Read(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + type + L"\\UserChoice", L"ProgId"));
        }
        return it->second;
    }
    std::wstring Label(const std::wstring &text)
    {
        if (text.empty() || text.front() != L'@') return text;
        const auto [it, inserted] = labels.try_emplace(text);
        if (inserted) it->second = Localized(text);
        return it->second;
    }
    bool IsBlocked(const std::wstring &clsid)
    {
        if (clsid.empty()) return false;
        const auto [it, inserted] = policies.try_emplace(Lower(clsid));
        if (inserted) it->second = Blocked(clsid);
        return it->second;
    }
    Application ApplicationForModule(const std::wstring &module)
    {
        if (module.empty()) return {};
        const auto [it, inserted] = applications.try_emplace(module);
        if (inserted)
        {
            it->second = ModuleApplication(module);
            const auto application = L"Applications\\" + std::filesystem::path(module).filename().wstring();
            auto friendly = Label(Read(classes, application, L"FriendlyAppName"));
            if (!UsableShellDisplayName(friendly)) friendly = Label(Read(classes, application + L"\\Application", L"ApplicationName"));
            if (it->second.id.starts_with("file:") && UsableShellDisplayName(friendly)) it->second.name = std::move(friendly);
        }
        return it->second;
    }
    Application CommandApplication(const std::wstring &command)
    {
        if (command.empty()) return {};
        const auto [it, inserted] = commands.try_emplace(command);
        if (inserted) it->second = ApplicationForModule(CommandModule(command));
        return it->second;
    }
    const HandlerMetadata &Handler(const std::wstring &clsid)
    {
        const auto [it, inserted] = handlers.try_emplace(Lower(clsid));
        if (inserted && !clsid.empty())
        {
            auto &info = it->second;
            const auto root = L"CLSID\\" + clsid;
            info.module = Read(classes, root + L"\\InprocServer32");
            const auto module = LocalModule(info.module);
            info.application = module.empty() ? CommandApplication(Read(classes, root + L"\\LocalServer32")) : ApplicationForModule(module);
            info.label = Label(Read(classes, root, L"LocalizedString"));
            if (!UsableShellDisplayName(info.label)) info.label = Label(Read(classes, root));
            info.icon = Read(classes, root + L"\\DefaultIcon");
        }
        return it->second;
    }
    Application HandlerApplication(const std::wstring &clsid)
    {
        return Handler(clsid).application;
    }
    void Image(Entry &entry, const std::wstring &icon)
    {
        if (icon.empty()) return;
        const auto [image, inserted] = images.try_emplace(icon);
        if (inserted) LoadIcon(image->second, icon);
        entry.width = image->second.width; entry.height = image->second.height;
        entry.pixels = image->second.pixels;
    }
    std::uint64_t FolderRevision()
    {
        return ScopedRevision(folderRows);
    }
    std::uint64_t BackgroundRevision(Context scope)
    {
        return ScopedRevision(backgroundRows[scope == Context::Desktop ? 1 : 0]);
    }
    static std::uint64_t ScopedRevision(std::vector<Registration> &rows)
    {
        std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
            return std::tie(a.id, a.sources, a.contexts) < std::tie(b.id, b.sources, b.contexts);
        });
        return Hash(settings_ipc::Pack(rows));
    }
    void Add(Registration row, const std::wstring &icon, const std::wstring &module = {})
    {
        if (control) control->WaitUntilAllowed();
        // Workers keep individual sources; the caller applies the original
        // scan order and global identity/row limit before loading images.
        if (capture) { captured.emplace_back(std::move(row), icon, module); return; }
        row.menuRevision = Hash(settings_ipc::Pack(row.menuRevision ? row.menuRevision : row.revision,
            icon, AssetRevision(icon)));
        if (row.types.empty()) row.types.push_back(L"*");
        if (row.id.empty() || result.rows.size() >= 16384) return;
        if (!module.empty()) registrationModules.try_emplace(row.id, LocalModule(module));
        row.display.provider = row.id;
        if (row.contexts & ContextBit(Context::Folder))
        {
            auto source = row; Image(source.display, icon);
            folderRows.push_back(std::move(source));
        }
        for (size_t i = 0; i < backgroundRows.size(); ++i)
            if (row.contexts & ContextBit(i ? Context::Desktop : Context::FolderBackground))
            {
                auto source = row; Image(source.display, icon);
                backgroundRows[i].push_back(std::move(source));
            }
        if (const auto it = ids.find(row.id); it != ids.end())
        {
            auto &existing = result.rows[it->second];
            existing.contexts |= row.contexts;
            existing.sources.insert(existing.sources.end(), row.sources.begin(), row.sources.end());
            existing.types.insert(existing.types.end(), row.types.begin(), row.types.end());
            existing.verbs.insert(existing.verbs.end(), row.verbs.begin(), row.verbs.end());
            existing.systemEnabled |= row.systemEnabled;
            return;
        }
        Image(row.display, icon);
        ids[row.id] = result.rows.size();
        result.rows.push_back(std::move(row));
    }
    void FinishRows()
    {
        for (auto &row : result.rows)
        {
            if (control) control->WaitUntilAllowed();
            Unique(row.sources); Unique(row.types); Unique(row.verbs);
            std::vector<std::wstring> associationValues;
            for (const auto &type : row.types)
                if (!type.empty() && type.front() == L'.')
                {
                    const auto &values = Associations(type);
                    associationValues.insert(associationValues.end(), values.begin(), values.end());
                }
            row.revision = Hash(settings_ipc::Pack(row.revision, associationValues));
            row.menuRevision = Hash(settings_ipc::Pack(row.menuRevision, associationValues,
                row.sources, row.types, row.verbs, row.contexts, row.systemEnabled,
                row.display.label, row.commandIdentity, row.application));
            row.revision = Hash(settings_ipc::Pack(row.revision, row.sources, row.types, row.verbs, row.contexts, row.systemEnabled, row.display.label, row.display.pixels, row.commandIdentity, row.application));
        }
        std::sort(result.rows.begin(), result.rows.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
    }
    void ClassifyApplications()
    {
        // This postpass belongs only to the software inventory. Scope proofs
        // and menu revisions retain the original registration metadata, so
        // presentation grouping cannot invalidate or authorize native menus.
        MenuTiming timing("catalogue.classification");
        ApplicationMetadataIndex metadata(classes, control);
        timing.Record("profiles");
        metadata.Prepare(registrationModules, control);
        timing.Record("prepared");
        for (auto& row : result.rows)
        {
            if (control) control->WaitUntilAllowed();
            const auto found = registrationModules.find(row.id);
            if (found == registrationModules.end() || found->second.empty()) continue;
            const auto application = metadata.Resolve(found->second, row.application).application;
            if (row.application != application)
            {
                row.application = application;
                row.revision = Hash(settings_ipc::Pack(row.revision, row.application));
            }
        }
        timing.Record("names", static_cast<unsigned>(metadata.modules.size()));
        // Resolve every member first so one shared product icon also reaches
        // registrations that sort before the representative executable.
        std::map<std::wstring, Entry> applicationImages;
        for (auto& row : result.rows)
        {
            if (control) control->WaitUntilAllowed();
            const auto icon = metadata.Icon(row.application.id);
            if (!icon.empty())
            {
                const auto [stored, inserted] = applicationImages.try_emplace(icon);
                if (inserted) Image(stored->second, icon);
                const auto& image = stored->second;
                if (!image.pixels.empty() && (row.display.width != image.width || row.display.height != image.height || row.display.pixels != image.pixels))
                {
                    row.display.width = image.width; row.display.height = image.height; row.display.pixels = image.pixels;
                    row.revision = Hash(settings_ipc::Pack(row.revision, row.display.width, row.display.height, row.display.pixels));
                }
            }
        }
        timing.Record("icons", static_cast<unsigned>(applicationImages.size()));
    }
    void Verb(const std::wstring &path, unsigned contexts, const std::vector<std::wstring> &types)
    {
        const auto verb = path.substr(path.find_last_of(L'\\') + 1);
        if (control) control->WaitUntilAllowed();
        Key key(classes, path, KEY_QUERY_VALUE);
        if (!key.value) return;
        Key commandKey(classes, path + L"\\command", KEY_QUERY_VALUE);
        KeyValues values(key.value);
        const auto handler = values.String(L"ExplorerCommandHandler");
        Registration row;
        row.kind = RegistrationKind::Verb;
        row.id = "reg:" + Utf8(Lower(path));
        row.contexts = contexts; row.types = types; row.sources = {path};
        row.verbs = {Utf8(Lower(verb))};
        if (!handler.empty()) row.verbs.push_back(Utf8(Lower(handler)));
        row.systemEnabled = !Has(key.value, L"", L"LegacyDisable") && !Has(key.value, L"", L"ProgrammaticAccessOnly") && !IsBlocked(handler);
        auto label = values.String(L"MUIVerb");
        if (label.empty()) label = values.String();
        if (label.empty()) label = verb;
        row.display.label = DecodeMenuLabel(Label(label)).text;
        const auto command = Read(commandKey.value, L"");
        const auto delegate = Read(commandKey.value, L"", L"DelegateExecute");
        const auto subCommands = values.String(L"SubCommands");
        row.application = !handler.empty() ? HandlerApplication(handler) :
            !delegate.empty() ? HandlerApplication(delegate) : CommandApplication(command);
        if (!row.application.id.empty())
        {
            const auto shell = path.rfind(L"\\shell\\");
            auto friendly = shell == std::wstring::npos ? std::wstring{} :
                Label(Read(classes, path.substr(0, shell) + L"\\Application", L"ApplicationName"));
            if (row.application.id.starts_with("file:") && UsableShellDisplayName(friendly)) row.application.name = std::move(friendly);
        }
        // Never infer equivalence from the caption, verb alone, or a bare
        // SubCommands list. Keep arguments case-sensitive and compare the
        // complete payload rather than a collision-prone display hash.
        if (!command.empty() || !handler.empty() || !delegate.empty())
        {
            const auto identity = settings_ipc::Pack(Lower(verb), command, Lower(handler), Lower(delegate),
                subCommands, values.String(L"ExtendedSubCommandsKey"));
            row.commandIdentity.assign(reinterpret_cast<const char *>(identity.data()), identity.size());
        }
        auto icon = values.String(L"Icon");
        if (icon.empty() && !handler.empty()) icon = Handler(handler).module;
        if (icon.empty())
        {
            if (!command.empty()) { wchar_t exe[32768]{}; wcsncpy_s(exe, command.c_str(), _TRUNCATE); PathRemoveArgsW(exe); PathUnquoteSpacesW(exe); icon = exe; }
        }
        // Include values that affect applicability, not volatile registry write times.
        row.revision = Hash(settings_ipc::Pack(command, handler, values.String(L"AppliesTo"), values.String(L"MultiSelectModel"), Has(key.value, L"", L"Extended"), subCommands));
        const auto module = !handler.empty() ? Handler(handler).module :
            !delegate.empty() ? Handler(delegate).module : CommandModule(command);
        row.menuRevision = Hash(settings_ipc::Pack(row.revision, AssetRevision(module)));
        Add(std::move(row), icon, module);
    }
    void HandlerRegistration(const std::wstring &path, unsigned contexts, const std::vector<std::wstring> &types)
    {
        const auto name = path.substr(path.find_last_of(L'\\') + 1);
        if (control) control->WaitUntilAllowed();
        Key key(classes, path, KEY_QUERY_VALUE);
        if (!key.value) return;
        auto clsid = Read(key.value, L"");
        if (clsid.empty() && !name.empty() && name.front() == L'{') clsid = name;
        GUID guid{};
        if (FAILED(CLSIDFromString(clsid.c_str(), &guid))) return;
        Registration row;
        row.kind = RegistrationKind::Handler; row.id = "clsid:" + Utf8(Lower(clsid));
        row.contexts = contexts; row.types = types; row.sources = {path}; row.verbs = {Utf8(Lower(clsid))};
        row.systemEnabled = !IsBlocked(clsid);
        const auto &info = Handler(clsid);
        row.display.label = Label(info.label.empty() ? name : info.label);
        row.application = info.application;
        row.revision = Hash(settings_ipc::Pack(info.module));
        row.menuRevision = Hash(settings_ipc::Pack(row.revision, AssetRevision(info.module)));
        Add(std::move(row), info.icon.empty() ? info.module : info.icon, info.module);
    }
    void Root(const std::wstring &root, unsigned contexts, const std::vector<std::wstring> &types = {})
    {
        if (control) control->WaitUntilAllowed();
        for (const auto &verb : Children(classes, root + L"\\shell", control))
            Verb(root + L"\\shell\\" + verb, contexts, types);
        for (const auto &name : Children(classes, root + L"\\shellex\\ContextMenuHandlers", control))
            HandlerRegistration(root + L"\\shellex\\ContextMenuHandlers\\" + name, contexts, types);
    }
    void Packages()
    {
        const std::wstring root = L"Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages";
        for (const auto &package : Children(classes, root, control))
        {
            if (control) control->WaitUntilAllowed();
            auto directory = Read(classes, root + L"\\" + package, L"PackageRootFolder");
            const auto contentDirectory = directory;
            if (classes == HKEY_CLASSES_ROOT)
            {
                // For sparse packages the compatibility registry value names
                // external program content, not the installed manifest folder.
                UINT32 length = 0;
                if (GetPackagePathByFullName(package.c_str(), &length, nullptr) == ERROR_INSUFFICIENT_BUFFER &&
                    length > 1 && length <= 32768)
                {
                    std::wstring installed(length, L'\0');
                    if (GetPackagePathByFullName(package.c_str(), &length, installed.data()) == ERROR_SUCCESS)
                        directory = installed.c_str();
                }
            }
            if (directory.empty()) continue;
            for (const auto &verb : ReadManifestVerbs(directory + L"\\AppxManifest.xml"))
            {
                auto clsid = verb.clsid;
                const auto &id = verb.id;
                const auto &type = verb.type;
                if (clsid.size() == 36) clsid = L"{" + clsid + L"}";
                if (clsid.empty() || type.empty()) continue;
                const auto first = package.find(L'_'), last = package.rfind(L'_');
                const auto family = first != std::wstring::npos && last > first ? package.substr(0, first) + package.substr(last) : package;
                Registration row; row.kind = RegistrationKind::Packaged;
                row.id = "package:" + Utf8(Lower(family + L":" + clsid));
                std::wstring module;
                if (!verb.module.empty())
                    for (const auto& content : {contentDirectory, directory})
                        if (module.empty() && !content.empty()) module = LocalModule((std::filesystem::path(content) / verb.module).wstring());
                row.application = ApplicationForModule(module);
                const auto name = PackageDisplayName(verb, package, directory);
                if (row.application.id.empty()) row.application.id = "package:" + Utf8(Lower(family));
                if (!name.empty() && !row.application.id.starts_with("install:")) row.application.name = name;
                if (!UsableShellDisplayName(row.application.name)) row.application.name = family.substr(0, family.find(L'_'));
                row.sources = {L"package:" + package + L":" + id}; row.verbs = {Utf8(Lower(clsid))};
                row.contexts = type == L"Directory" ? ContextBit(Context::Folder) : type == L"Directory\\Background" ? ContextBit(Context::FolderBackground) | ContextBit(Context::Desktop) : ContextBit(Context::File);
                if (type != L"*" && type != L"Directory" && type != L"Directory\\Background") row.types = {Lower(type)};
                row.systemEnabled = !IsBlocked(clsid); row.display.label = row.application.name;
                row.revision = Hash(settings_ipc::Pack(package, id, clsid));
                Add(std::move(row), module, module);
            }
        }
    }
};
struct FileRoot
{
    std::wstring path;
    std::vector<std::wstring> types;
};
std::map<std::wstring, std::vector<std::wstring>> ReadTypeAssociations(
    HKEY classes, const std::vector<std::wstring> &roots,
    std::map<std::wstring, std::vector<std::wstring>> &sourceValues,
    CatalogueForegroundDemand *foreground)
{
    std::map<std::wstring, std::vector<std::wstring>> types;
    std::optional<Key> choices;
    if (classes == HKEY_CLASSES_ROOT)
        choices.emplace(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts", KEY_QUERY_VALUE);
    const auto readAssociation = [classes, &choices, foreground](const std::wstring &name) {
        if (foreground) foreground->WaitUntilAllowed();
        Key extension(classes, name, KEY_QUERY_VALUE);
        const auto queryRoot = extension.value ? extension.value : classes;
        const auto queryPath = extension.value ? std::wstring{} : name;
        std::map<std::wstring, std::vector<std::wstring>> result;
        result[Lower(name)].push_back(Lower(name));
        const auto prog = Read(queryRoot, queryPath); if (!prog.empty()) result[Lower(prog)].push_back(Lower(name));
        // Preserve the original values used by row fingerprints in this scan.
        // A new Scanner reads fresh values; this does not cache associations across scans.
        std::vector<std::wstring> values{prog};
        const auto perceived = Read(queryRoot, queryPath, L"PerceivedType");
        if (!perceived.empty()) result[L"systemfileassociations\\" + Lower(perceived)].push_back(Lower(name));
        result[L"systemfileassociations\\" + Lower(name)].push_back(Lower(name));
        for (const auto &progId : Values(queryRoot, queryPath.empty() ? L"OpenWithProgids" : name + L"\\OpenWithProgids")) result[Lower(progId)].push_back(Lower(name));
        if (classes == HKEY_CLASSES_ROOT)
        {
            const auto choice = choices && choices->value
                ? Read(choices->value, name + L"\\UserChoice", L"ProgId")
                : Read(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + name + L"\\UserChoice", L"ProgId");
            if (!choice.empty()) result[Lower(choice)].push_back(Lower(name));
            values.push_back(choice);
        }
        return std::pair{std::move(result), std::move(values)};
    };
    std::vector<std::wstring> fileTypes;
    for (const auto &name : roots)
        if (!name.empty() && name.front() == L'.') fileTypes.push_back(name);
    using Associated = std::pair<std::map<std::wstring, std::vector<std::wstring>>, std::vector<std::wstring>>;
    std::vector<Associated> associated(fileTypes.size());
    // Each extension reads independent registration values. Keep results apart
    // and merge in root order so parallel completion cannot alter associations.
    if (fileTypes.size() < 64)
        for (size_t i = 0; i < fileTypes.size(); ++i) associated[i] = readAssociation(fileTypes[i]);
    else
    {
        std::atomic_size_t next = 0;
        const bool completed = detail::RunCatalogueWorkers(
            std::clamp(std::thread::hardware_concurrency() / 2u, 2u, 8u), foreground, [&] {
                snowdesktop::performance::Scope scope("shell.catalogue", "associations.worker");
                while (true)
                {
                    const auto index = next.fetch_add(1, std::memory_order_relaxed);
                    if (index >= fileTypes.size()) break;
                    associated[index] = readAssociation(fileTypes[index]);
                }
            });
        if (!completed)
        {
            // The helper drains writers before a clean serial replacement.
            for (size_t i = 0; i < fileTypes.size(); ++i) associated[i] = readAssociation(fileTypes[i]);
        }
    }
    for (size_t i = 0; i < associated.size(); ++i)
    {
        for (auto &[name, values] : associated[i].first)
            types[name].insert(types[name].end(), values.begin(), values.end());
        sourceValues[Lower(fileTypes[i])] = std::move(associated[i].second);
    }
    return types;
}
void ReadFileRoots(Scanner &scanner, const std::vector<FileRoot> &roots,
                   CatalogueForegroundDemand *foreground)
{
    snowdesktop::performance::Scope scope("shell.catalogue", "file.roots");
    const auto files = ContextBit(Context::File);
    auto serial = [&] { for (const auto &root : roots) scanner.Root(root.path, files, root.types); };
    // Small/private inventories avoid thread setup. One to eight workers bound
    // additional CPU work while native menu helpers remain active.
    if (roots.size() < 64) { serial(); return; }
    std::vector<std::vector<Scanner::CapturedRegistration>> results(roots.size());
    std::atomic_size_t next = 0;
    const unsigned workerCount = std::clamp(std::thread::hardware_concurrency() / 2u, 2u, 8u);
    const bool completed = detail::RunCatalogueWorkers(workerCount, foreground, [&] {
        snowdesktop::performance::Scope workerScope("shell.catalogue", "file.worker");
        const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(initialized)) throw std::runtime_error("metadata COM initialization");
        struct Cleanup { ~Cleanup() { CoUninitialize(); } } cleanup;
        Scanner local{scanner.classes}; local.control = foreground; local.capture = true;
        while (true)
        {
            const auto index = next.fetch_add(1, std::memory_order_relaxed);
            if (index >= roots.size()) break;
            local.Root(roots[index].path, files, roots[index].types);
            results[index] = std::move(local.captured); local.captured.clear();
        }
    });
    if (!completed)
    {
        // No worker row has entered the inventory, so fallback starts cleanly.
        results.clear(); serial(); return;
    }
    for (auto &result : results)
        for (auto& [row, icon, module] : result) scanner.Add(std::move(row), icon, module);
}
struct ExactSource
{
    std::wstring path, root;
    RegistrationKind kind;
    unsigned contexts;
    std::vector<std::wstring> types;
};
unsigned RootContexts(const std::wstring &root)
{
    if (root == L"allfilesystemobjects") return ContextBit(Context::File) | ContextBit(Context::Folder);
    if (root == L"directory" || root == L"folder" || root == L"drive") return ContextBit(Context::Folder);
    if (root == L"directory\\background") return ContextBit(Context::FolderBackground) | ContextBit(Context::Desktop);
    if (root == L"desktopbackground") return ContextBit(Context::Desktop);
    return ContextBit(Context::File);
}
bool CommonRoot(const std::wstring &root)
{
    return root == L"*" || root == L"allfilesystemobjects" || root == L"directory" ||
        root == L"folder" || root == L"drive" || root == L"directory\\background" || root == L"desktopbackground";
}
std::vector<ExactSource> ExactSources(const std::vector<Registration> &seeds)
{
    std::vector<ExactSource> result;
    for (const auto &seed : seeds)
    {
        if (seed.kind != RegistrationKind::Verb && seed.kind != RegistrationKind::Handler) continue;
        auto paths = seed.sources;
        if (paths.empty() && seed.kind == RegistrationKind::Verb && seed.id.starts_with("reg:"))
        {
            const auto text = std::string_view(seed.id).substr(4);
            if (text.empty() || text.size() > 32767) continue;
            const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (!length) continue;
            std::wstring path(length, L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), path.data(), length);
            paths.push_back(std::move(path));
        }
        for (const auto &path : paths)
        {
            if (path.empty() || path.size() > 32767 || path.find(L'\0') != std::wstring::npos) continue;
            const auto normal = Lower(path);
            const std::wstring marker = seed.kind == RegistrationKind::Verb ? L"\\shell\\" : L"\\shellex\\contextmenuhandlers\\";
            const auto at = normal.rfind(marker);
            if (at == std::wstring::npos || at == 0 || at + marker.size() >= normal.size() ||
                normal.find(L'\\', at + marker.size()) != std::wstring::npos) continue;
            if (seed.kind == RegistrationKind::Verb && !seed.id.empty() && seed.id != "reg:" + Utf8(normal)) continue;
            const auto root = normal.substr(0, at);
            const unsigned contexts = RootContexts(root) & (seed.contexts ? seed.contexts : 15u);
            if (!contexts) continue;
            result.push_back({path, root, seed.kind, contexts, seed.types});
        }
    }
    // Match the full reader's stable common-root priority for merged handlers.
    // Other paths remain limited proof; discovering a newly registered alias
    // of the same CLSID requires management inventory, never a hidden scan here.
    const auto priority = [](const auto &root) {
        const std::array<std::wstring_view, 7> roots{L"*", L"allfilesystemobjects", L"directory", L"folder", L"drive", L"directory\\background", L"desktopbackground"};
        return static_cast<size_t>(std::find(roots.begin(), roots.end(), std::wstring_view(root)) - roots.begin());
    };
    std::stable_sort(result.begin(), result.end(), [&](const auto &a, const auto &b) { return priority(a.root) < priority(b.root); });
    return result;
}
}
static Catalogue ReadCatalogueImpl(HKEY classes, bool packages, CatalogueForegroundDemand *foreground)
{
    snowdesktop::performance::Scope scope("shell.catalogue", "scan");
    MenuTiming timing("catalogue");
    if (foreground) foreground->WaitUntilAllowed();
    Scanner scanner{classes}; scanner.control = foreground;
    const unsigned files = ContextBit(Context::File), folders = ContextBit(Context::Folder);
    scanner.Root(L"*", files); scanner.Root(L"AllFilesystemObjects", files | folders);
    scanner.Root(L"Directory", folders); scanner.Root(L"Folder", folders); scanner.Root(L"Drive", folders);
    scanner.Root(L"Directory\\Background", ContextBit(Context::FolderBackground) | ContextBit(Context::Desktop));
    scanner.Root(L"DesktopBackground", ContextBit(Context::Desktop));
    const auto roots = Children(classes, L"", foreground);
    auto types = ReadTypeAssociations(classes, roots, scanner.associations, foreground);
    const std::set<std::wstring> special{L"*", L"allfilesystemobjects", L"directory", L"folder", L"drive", L"desktopbackground", L"clsid", L"interface", L"typelib"};
    std::vector<FileRoot> fileRoots;
    for (const auto &name : roots)
        if (!special.contains(Lower(name)))
        {
            auto &extensions = types[Lower(name)];
            if (extensions.empty()) extensions.push_back(L"progid:" + Lower(name));
            fileRoots.push_back({name, extensions});
        }
    for (const auto &name : Children(classes, L"SystemFileAssociations", foreground))
        fileRoots.push_back({L"SystemFileAssociations\\" + name, types[L"systemfileassociations\\" + Lower(name)]});
    ReadFileRoots(scanner, fileRoots, foreground);
    if (packages) scanner.Packages();
    scanner.FinishRows();
    scanner.ClassifyApplications();
    scanner.result.revision = Hash(settings_ipc::Pack(scanner.result.rows));
    scanner.result.folderRevision = scanner.FolderRevision();
    scanner.result.backgroundRevisions = {scanner.BackgroundRevision(Context::FolderBackground), scanner.BackgroundRevision(Context::Desktop)};
    timing.Record("complete", static_cast<unsigned>(scanner.result.rows.size()));
    return std::move(scanner.result);
}
Catalogue ReadCatalogue(HKEY classes, bool packages)
{
    return ReadCatalogueImpl(classes, packages, nullptr);
}
std::vector<Registration> ReadPackagedMenuCommands(HKEY classes)
{
    Scanner scanner{classes};
    scanner.Packages();
    scanner.FinishRows();
    return std::move(scanner.result.rows);
}
void LoadMenuCommandIcon(Entry& entry, const std::wstring& location) { LoadIcon(entry, location); }
Catalogue ReadCatalogueWithForegroundDemand(HKEY classes, bool packages,
                                            CatalogueForegroundDemand &foreground)
{
    return ReadCatalogueImpl(classes, packages, &foreground);
}
Catalogue ReadCatalogueSources(const std::vector<Registration> &sources,
                               CatalogueForegroundDemand *foreground, HKEY classes)
{
    snowdesktop::performance::Scope scope("shell.catalogue", "selected.sources");
    MenuTiming timing("selected.catalogue");
    if (foreground) foreground->WaitUntilAllowed();
    Scanner scanner{classes}; scanner.control = foreground;
    const auto exact = ExactSources(sources);
    std::vector<std::wstring> extensions;
    for (const auto &source : exact)
    {
        for (const auto &type : source.types)
            if (!type.empty() && type.front() == L'.') extensions.push_back(Lower(type));
        if (source.root.front() == L'.' && source.root.find(L'\\') == std::wstring::npos)
            extensions.push_back(source.root);
        constexpr std::wstring_view associationRoot = L"systemfileassociations\\";
        if (source.root.starts_with(associationRoot) && source.root.size() > associationRoot.size() &&
            source.root[associationRoot.size()] == L'.')
            extensions.push_back(source.root.substr(associationRoot.size()));
    }
    Unique(extensions);
    // These are exact seed extensions, not all classes. ReadTypeAssociations
    // queries only their values/OpenWithProgids and live UserChoice keys.
    const auto associated = ReadTypeAssociations(classes, extensions, scanner.associations, foreground);
    for (const auto &source : exact)
    {
        auto types = source.types;
        if (!CommonRoot(source.root))
        {
            types.erase(std::remove_if(types.begin(), types.end(), [](const auto &type) {
                return !type.empty() && (type.front() == L'.' || type.starts_with(L"progid:"));
            }), types.end());
            if (const auto found = associated.find(source.root); found != associated.end())
                types.insert(types.end(), found->second.begin(), found->second.end());
            if (types.empty()) types.push_back(L"progid:" + source.root);
        }
        if (source.kind == RegistrationKind::Verb) scanner.Verb(source.path, source.contexts, types);
        else scanner.HandlerRegistration(source.path, source.contexts, types);
    }
    scanner.FinishRows();
    std::vector<std::uint64_t> commands;
    for (const auto &row : scanner.result.rows) commands.push_back(row.menuRevision);
    scanner.result.revision = Hash(settings_ipc::Pack(std::string("selected-registration-sources-v1"), scanner.result.rows, commands));
    // Add() also collects scope rows for the full scanner, but a limited read
    // must never advertise those as complete folder/background proofs.
    timing.Record("complete", static_cast<unsigned>(scanner.result.rows.size()));
    return std::move(scanner.result);
}
std::uint64_t ReadFolderCatalogueRevision(HKEY classes, bool packages, CatalogueForegroundDemand *foreground)
{
    MenuTiming timing("folder.catalogue");
    if (foreground) foreground->WaitUntilAllowed();
    Scanner scanner{classes}; scanner.control = foreground;
    const auto folders = ContextBit(Context::Folder);
    scanner.Root(L"AllFilesystemObjects", ContextBit(Context::File) | folders);
    scanner.Root(L"Directory", folders); scanner.Root(L"Folder", folders); scanner.Root(L"Drive", folders);
    if (packages) scanner.Packages();
    if (foreground) foreground->WaitUntilAllowed();
    const auto revision = scanner.FolderRevision();
    if (foreground) foreground->WaitUntilAllowed();
    timing.Record("complete", static_cast<unsigned>(scanner.folderRows.size()));
    return revision;
}
std::uint64_t ReadBackgroundCatalogueRevision(Context scope, HKEY classes, bool packages,
                                             CatalogueForegroundDemand *foreground)
{
    if (scope != Context::FolderBackground && scope != Context::Desktop) return 0;
    MenuTiming timing(scope == Context::Desktop ? "desktop.catalogue" : "background.catalogue");
    if (foreground) foreground->WaitUntilAllowed();
    Scanner scanner{classes}; scanner.control = foreground;
    scanner.Root(L"Directory\\Background", ContextBit(Context::FolderBackground) | ContextBit(Context::Desktop));
    if (scope == Context::Desktop) scanner.Root(L"DesktopBackground", ContextBit(Context::Desktop));
    if (packages) scanner.Packages();
    if (foreground) foreground->WaitUntilAllowed();
    const auto revision = scanner.BackgroundRevision(scope);
    if (foreground) foreground->WaitUntilAllowed();
    timing.Record("complete", static_cast<unsigned>(scanner.backgroundRows[scope == Context::Desktop ? 1 : 0].size()));
    return revision;
}
bool HandlerEnabled(const std::wstring &clsid, HKEY user, HKEY machine)
{
    return !Blocked(clsid, user, machine);
}
bool Applies(const Registration &row, const Request &request)
{
    if (!(row.contexts & ContextBit(ResolveContext(request)))) return false;
    if (row.types.empty() || std::find(row.types.begin(), row.types.end(), L"*") != row.types.end() || request.paths.empty()) return true;
    return std::all_of(request.paths.begin(), request.paths.end(), [&](const auto &p) {
        return std::find(row.types.begin(), row.types.end(), Lower(std::filesystem::path(p).extension().wstring())) != row.types.end();
    });
}
void Associate(Catalogue &catalogue, const Request &request, Reply &reply)
{
    if (!reply.ok) return;
    auto selection = request; selection.context = ResolveContext(request);
    for (auto &entry : reply.entries)
    {
        if (entry.separator) continue;
        Registration *match = nullptr;
        auto key = entry.key; std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
        for (auto &row : catalogue.rows)
            if (row.systemEnabled && Applies(row, selection) && !key.empty() && std::find(row.verbs.begin(), row.verbs.end(), key) != row.verbs.end())
            {
                if (match) { match = nullptr; break; }
                match = &row;
            }
        if (!match) continue; // Labels and submenu contents never establish ownership.
        match->linked = true;
        if (match->display.pixels.empty() && !entry.pixels.empty())
        {
            match->display.width = entry.width; match->display.height = entry.height;
            match->display.pixels = entry.pixels;
        }
        const Association association{entry.provider, match->id, selection.context};
        if (std::none_of(catalogue.associations.begin(), catalogue.associations.end(), [&](const auto &a) { return a.provider == association.provider && a.registration == association.registration && a.context == association.context; })) catalogue.associations.push_back(association);
        entry.registration = match->id;
    }
}
void MigrateAssociations(Preferences &prefs, const Catalogue &catalogue)
{
    if (prefs.showAll) return; // Bypassing display rules must not manufacture opt-ins.
    for (const auto &a : catalogue.associations)
    {
        // A dynamic command opt-in is not consent to enable every other
        // command generated by its handler. Only static verb aliases migrate.
        if (a.registration.starts_with("clsid:") || std::any_of(catalogue.rows.begin(), catalogue.rows.end(), [&](const auto& row) {
            return row.id == a.registration && row.kind == RegistrationKind::Handler;
        })) continue;
        if (std::any_of(catalogue.associations.begin(), catalogue.associations.end(), [&](const auto &b) { return b.provider == a.provider && b.context == a.context && b.registration != a.registration; })) continue;
        const bool configured = std::any_of(prefs.rules.begin(), prefs.rules.end(), [&](const auto &r) { return r.id == a.registration && r.category == CategoryOf(a.context); }) ||
            std::any_of(prefs.overrides.begin(), prefs.overrides.end(), [&](const auto &r) { return r.id == a.registration && r.context == a.context; });
        if (!configured && !IsHidden(prefs, a.provider, a.context)) SetOverride(prefs, a.registration, a.context, Visibility::Show);
    }
}
}

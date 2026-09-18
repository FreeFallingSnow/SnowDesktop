#include "debug_profile.h"
#include "atomic_file.h"
#include "json_value.h"

#include <windows.h>
#include <algorithm>
#include <fstream>
#include <sstream>

namespace snowdesktop::debug_profile
{
namespace
{

bool SafeAncestors(std::filesystem::path path, std::string& error)
{
    for (; !path.empty(); )
    {
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        {
            error = "A managed data path contains a reparse point.";
            return false;
        }
        const auto parent = path.parent_path();
        if (parent == path) break;
        path = parent;
    }
    return true;
}

std::filesystem::path Canonical(const std::filesystem::path& path)
{
    std::error_code ec;
    auto result = std::filesystem::weakly_canonical(path, ec);
    return ec ? std::filesystem::path{} : result;
}

bool Contains(const std::filesystem::path& root, const std::filesystem::path& path)
{
    auto a = root.begin();
    auto b = path.begin();
    for (; a != root.end(); ++a, ++b)
        if (b == path.end() || _wcsicmp(a->c_str(), b->c_str()) != 0) return false;
    return true;
}

std::string Quote(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    std::string result = "\"";
    for (const auto value : utf8)
    {
        const auto c = static_cast<unsigned char>(value);
        if (c == '"' || c == '\\') result += '\\';
        if (c < 32)
        {
            constexpr char hex[] = "0123456789abcdef";
            result += "\\u00";
            result += hex[c >> 4]; result += hex[c & 15];
        }
        else result += static_cast<char>(c);
    }
    return result + '"';
}

bool WritableDirectory(const std::filesystem::path& directory, std::string& error)
{
    // CREATE_NEW never overwrites user content, including on failed validation.
    const auto probe = directory / (L".snowdesktop-probe-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE | DELETE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        error = "The directory is not writable (" + std::to_string(GetLastError()) + ").";
        return false;
    }
    CloseHandle(file);
    std::error_code ec;
    std::filesystem::directory_iterator iterator(directory, ec);
    if (ec) error = ec.message();
    return !ec;
}
}

Paths ResolvePaths(const std::filesystem::path& normalData)
{
    Paths paths;
    paths.normalData = normalData;
    paths.root = normalData.parent_path() / (normalData.filename().wstring() + L".debug");
    paths.data = paths.root / L"data";
    paths.defaultDesktop = paths.root / L"Desktop";
    paths.control = normalData / L"SnowDesktop.debug-profile.json";
    return paths;
}

bool Read(const Paths& paths, Configuration& config, std::string& error)
{
    config = {};
    config.desktop = paths.defaultDesktop;
    std::error_code ec;
    if (!std::filesystem::exists(paths.control, ec))
    {
        if (ec) error = ec.message();
        return !ec;
    }
    std::string contents;
    JsonValue root;
    if (!atomic_file::ReadAll(paths.control, contents, &error) || !ParseJson(contents, root, &error))
        return false;
    const auto enabled = root.Find("enabled");
    const auto reset = root.Find("pendingReset");
    const auto changed = root.Find("pendingDesktopChange");
    const auto desktop = root.Find("desktop");
    if (!enabled || !enabled->IsBoolean() || !reset || !reset->IsBoolean() ||
        !changed || !changed->IsBoolean() || !desktop || !desktop->IsString() ||
        desktop->string.empty() || desktop->string.find('\0') != std::string::npos)
    {
        error = "Invalid debug profile configuration.";
        return false;
    }
    config.enabled = enabled->boolean;
    config.pendingReset = reset->boolean;
    config.pendingDesktopChange = changed->boolean;
    try { config.desktop = std::filesystem::u8path(desktop->string); }
    catch (const std::exception&) { error = "Invalid desktop path encoding."; return false; }
    return true;
}

bool Write(const Paths& paths, const Configuration& config, std::string& error)
{
    if (!SafeAncestors(paths.control, error)) return false;
    std::error_code ec;
    std::filesystem::create_directories(paths.normalData, ec);
    if (ec) { error = ec.message(); return false; }
    const std::string contents = std::string("{\n  \"enabled\": ") + (config.enabled ? "true" : "false") +
        ",\n  \"pendingReset\": " + (config.pendingReset ? "true" : "false") +
        ",\n  \"pendingDesktopChange\": " + (config.pendingDesktopChange ? "true" : "false") +
        ",\n  \"desktop\": " + Quote(config.desktop) + "\n}\n";
    return atomic_file::WriteAll(paths.control, contents, {}, &error);
}

bool PathsOverlap(const std::filesystem::path& left, const std::filesystem::path& right)
{
    const auto a = Canonical(left), b = Canonical(right);
    return a.empty() || b.empty() || Contains(a, b) || Contains(b, a);
}

bool ValidateDesktop(const Paths& paths, const std::filesystem::path& desktop,
    const std::vector<std::filesystem::path>& systemDesktops, std::string& error)
{
    std::error_code ec;
    if (!desktop.is_absolute() || desktop.native().size() >= MAX_PATH ||
        !std::filesystem::is_directory(desktop, ec))
    { error = "Select an existing absolute directory shorter than MAX_PATH."; return false; }
    std::vector<std::filesystem::path> forbidden = systemDesktops;
    forbidden.insert(forbidden.end(), {paths.normalData, paths.data,
        paths.root / L"FullBackups", paths.root / L"TempState", paths.root / L"PrivateState"});
    for (const auto& path : forbidden)
        if (!path.empty() && PathsOverlap(desktop, path))
        { error = "The simulated desktop overlaps a real desktop or managed application data."; return false; }
    return WritableDirectory(desktop, error);
}

bool Prepare(const Paths& paths, const Configuration& config,
    const std::vector<std::filesystem::path>& systemDesktops, std::string& error)
{
    if (!SafeAncestors(paths.data, error) || !SafeAncestors(paths.root / L"FullBackups", error) ||
        !SafeAncestors(paths.root / L"TempState", error) ||
        !SafeAncestors(paths.root / L"PrivateState", error)) return false;
    std::error_code ec;
    if (config.desktop == paths.defaultDesktop)
    {
        if (!SafeAncestors(config.desktop, error)) return false;
        std::filesystem::create_directories(config.desktop, ec);
        if (ec) { error = ec.message(); return false; }
    }
    if (!ValidateDesktop(paths, config.desktop, systemDesktops, error)) return false;
    std::filesystem::create_directories(paths.data, ec);
    if (ec) { error = ec.message(); return false; }
    return WritableDirectory(paths.data, error);
}

bool Clear(const Paths& paths, std::string& error)
{
    // Only these application-owned children may be removed. Preflight the entire
    // set before deleting anything; recursive iterators never follow symlinks.
    const std::vector<std::filesystem::path> targets = {
        paths.data, paths.root / L"FullBackups", paths.root / L"TempState", paths.root / L"PrivateState"};
    for (const auto& target : targets)
    {
        if (!SafeAncestors(target, error)) return false;
        std::error_code ec;
        if (!std::filesystem::exists(target, ec))
        { if (ec) { error = ec.message(); return false; } continue; }
        for (std::filesystem::recursive_directory_iterator it(target, ec), end;
            !ec && it != end; it.increment(ec))
        {
            if (!SafeAncestors(it->path(), error)) return false;
        }
        if (ec) { error = ec.message(); return false; }
    }
    for (const auto& target : targets)
    {
        std::error_code ec;
        std::filesystem::remove_all(target, ec);
        if (ec) { error = ec.message(); return false; }
    }
    return true;
}

bool Initialize(const std::filesystem::path& normalData,
    const std::vector<std::filesystem::path>& systemDesktops, std::string& error)
{
    Session next;
    next.paths = ResolvePaths(normalData);
    if (!Read(next.paths, next.configuration, error)) return false;
    if (next.configuration.enabled && !Prepare(next.paths, next.configuration, systemDesktops, error))
        return false;
    if (next.configuration.pendingReset)
    {
        if (!Clear(next.paths, error)) return false;
        next.configuration.pendingReset = false;
        next.configuration.pendingDesktopChange = false;
        if (!Write(next.paths, next.configuration, error)) return false;
        if (next.configuration.enabled && !Prepare(next.paths, next.configuration, systemDesktops, error))
            return false;
    }
    next.initialized = true;
    runtimeSession = std::move(next);
    return true;
}

bool AcknowledgeDesktopChange(std::string& error)
{
    auto& session = runtimeSession;
    if (!Enabled() || !session.configuration.pendingDesktopChange) return true;
    Configuration saved;
    if (!Read(session.paths, saved, error)) return false;
    saved.pendingDesktopChange = false;
    if (!Write(session.paths, saved, error)) return false;
    session.configuration.pendingDesktopChange = false;
    return true;
}
}

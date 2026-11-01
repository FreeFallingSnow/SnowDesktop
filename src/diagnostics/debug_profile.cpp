#include "debug_profile.h"
#include "data/atomic_file.h"
#include "common/json_value.h"

#include <windows.h>
#include <algorithm>
#include <stdexcept>
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

Paths ResolvePaths(const std::filesystem::path& normalData, std::wstring_view environmentId)
{
    if (!ValidEnvironmentId(environmentId)) throw std::invalid_argument("Invalid debug environment ID.");
    Paths paths;
    paths.normalData = normalData;
    paths.root = normalData.parent_path() / (normalData.filename().wstring() + L".debug");
    paths.environmentsRoot = paths.root / L"environments";
    paths.environmentId = environmentId;
    if (environmentId != L"default") paths.root = paths.environmentsRoot / environmentId;
    paths.data = paths.root / L"data";
    paths.defaultDesktop = paths.root / L"Desktop";
    paths.control = normalData / L"SnowDesktop.debug-profile.json";
    return paths;
}

namespace
{
Environment SelectedState(const Configuration& config)
{
    Environment entry;
    entry.id = config.selectedEnvironment;
    entry.desktop = config.desktop;
    entry.pendingReset = config.pendingReset;
    entry.pendingDesktopChange = config.pendingDesktopChange;
    return entry;
}

bool StoreSelection(Configuration& config, std::string& error)
{
    if (config.environments.empty())
    {
        if (config.selectedEnvironment != L"default")
        { error = "Unknown selected debug environment."; return false; }
        config.environments.push_back(SelectedState(config));
    }
    const auto found = std::find_if(config.environments.begin(), config.environments.end(),
        [&](const auto& entry) { return entry.id == config.selectedEnvironment; });
    if (found == config.environments.end())
    { error = "Unknown selected debug environment."; return false; }
    found->desktop = config.desktop;
    found->pendingReset = config.pendingReset;
    found->pendingDesktopChange = config.pendingDesktopChange;
    return true;
}

bool ValidateCatalog(const Configuration& config, std::string& error)
{
    if (config.environments.empty() || config.environments.size() > MaximumEnvironments)
    { error = "Invalid number of debug environments."; return false; }
    bool hasDefault = false, hasSelected = false;
    for (std::size_t i = 0; i < config.environments.size(); ++i)
    {
        const auto& entry = config.environments[i];
        if (!ValidEnvironmentId(entry.id) || !entry.desktop.is_absolute() ||
            entry.desktop.native().find(L'\0') != std::wstring::npos ||
            ((!entry.name.empty() || entry.id != L"default") &&
                (!ValidEnvironmentName(entry.name) || TrimEnvironmentName(entry.name) != entry.name)))
        { error = "Invalid debug environment record."; return false; }
        hasDefault = hasDefault || entry.id == L"default";
        hasSelected = hasSelected || entry.id == config.selectedEnvironment;
        for (std::size_t j = 0; j < i; ++j)
            if (entry.id == config.environments[j].id || (!entry.name.empty() &&
                _wcsicmp(entry.name.c_str(), config.environments[j].name.c_str()) == 0))
            { error = "Duplicate debug environment ID or name."; return false; }
    }
    if (!hasDefault || !hasSelected)
    { error = "The default or selected debug environment is missing."; return false; }
    return true;
}

std::wstring WideString(const std::string& text)
{
    return std::filesystem::path(std::u8string(text.begin(), text.end())).wstring();
}

void LoadSelection(Configuration& config, const Environment& entry)
{
    config.selectedEnvironment = entry.id;
    config.desktop = entry.desktop;
    config.pendingReset = entry.pendingReset;
    config.pendingDesktopChange = entry.pendingDesktopChange;
}
}

namespace
{
bool ReadConfiguration(const Paths& paths, Configuration& config, std::string& error)
{
    config = {};
    config.desktop = paths.defaultDesktop;
    std::error_code ec;
    if (!std::filesystem::exists(paths.control, ec))
    {
        if (ec) error = ec.message();
        if (!ec) config.environments.push_back(SelectedState(config));
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
    try { config.desktop = std::filesystem::path(std::u8string(desktop->string.begin(), desktop->string.end())); }
    catch (const std::exception&) { error = "Invalid desktop path encoding."; return false; }
    const auto environments = root.Find("environments");
    if (!environments)
    {
        // The original single environment keeps its exact data and desktop paths.
        config.environments.push_back(SelectedState(config));
        return true;
    }
    const auto version = root.Find("schemaVersion");
    const auto selected = root.Find("selectedEnvironment");
    if (!version || !version->IsNumber() || version->number != 2 ||
        !selected || !selected->IsString() || !environments->IsArray())
    { error = "Invalid debug environment catalog."; return false; }
    try
    {
        config.selectedEnvironment = WideString(selected->string);
        for (const auto& value : environments->array)
        {
            const auto id = value.Find("id"), name = value.Find("name"), folder = value.Find("desktop");
            const auto pending = value.Find("pendingReset"), moved = value.Find("pendingDesktopChange");
            if (!id || !id->IsString() || !name || !name->IsString() || !folder || !folder->IsString() ||
                !pending || !pending->IsBoolean() || !moved || !moved->IsBoolean())
            { error = "Invalid debug environment record."; return false; }
            config.environments.push_back({WideString(id->string), WideString(name->string),
                pending->boolean, moved->boolean,
                std::filesystem::path(std::u8string(folder->string.begin(), folder->string.end()))});
        }
    }
    catch (const std::exception&) { error = "Invalid debug environment encoding."; return false; }
    if (!ValidateCatalog(config, error)) return false;
    const auto found = std::find_if(config.environments.begin(), config.environments.end(),
        [&](const auto& entry) { return entry.id == config.selectedEnvironment; });
    LoadSelection(config, *found);
    return true;
}
}

bool Read(const Paths& paths, Configuration& config, std::string& error)
{
    Configuration parsed;
    if (!ReadConfiguration(paths, parsed, error))
    {
        // Startup recovery must not retain a partially parsed invalid catalog.
        config = {};
        config.desktop = ResolvePaths(paths.normalData).defaultDesktop;
        return false;
    }
    config = std::move(parsed);
    return true;
}

bool Write(const Paths& paths, const Configuration& config, std::string& error)
{
    if (!SafeAncestors(paths.control, error)) return false;
    auto saved = config;
    if (!StoreSelection(saved, error) || !ValidateCatalog(saved, error)) return false;
    std::error_code ec;
    std::filesystem::create_directories(paths.normalData, ec);
    if (ec) { error = ec.message(); return false; }
    std::string contents = std::string("{\n  \"schemaVersion\": 2,\n  \"enabled\": ") + (config.enabled ? "true" : "false") +
        ",\n  \"pendingReset\": " + (config.pendingReset ? "true" : "false") +
        ",\n  \"pendingDesktopChange\": " + (config.pendingDesktopChange ? "true" : "false") +
        ",\n  \"desktop\": " + Quote(config.desktop) +
        ",\n  \"selectedEnvironment\": " + Quote(std::filesystem::path(saved.selectedEnvironment)) +
        ",\n  \"environments\": [";
    for (std::size_t i = 0; i < saved.environments.size(); ++i)
    {
        const auto& entry = saved.environments[i];
        if (i) contents += ',';
        contents += "\n    {\"id\": " + Quote(std::filesystem::path(entry.id)) +
            ", \"name\": " + Quote(std::filesystem::path(entry.name)) +
            ", \"pendingReset\": " + (entry.pendingReset ? "true" : "false") +
            ", \"pendingDesktopChange\": " + (entry.pendingDesktopChange ? "true" : "false") +
            ", \"desktop\": " + Quote(entry.desktop) + '}';
    }
    contents += "\n  ]\n}\n";
    return atomic_file::WriteAll(paths.control, contents, {}, &error);
}

bool SelectEnvironment(Configuration& config, std::wstring_view id, std::string& error)
{
    auto next = config;
    if (!StoreSelection(next, error)) return false;
    const auto found = std::find_if(next.environments.begin(), next.environments.end(),
        [&](const auto& entry) { return entry.id == id; });
    if (found == next.environments.end()) { error = "Unknown debug environment."; return false; }
    LoadSelection(next, *found);
    config = std::move(next);
    return true;
}

bool RenameEnvironment(Configuration& config, std::wstring_view name, std::string& error)
{
    auto next = config;
    const auto trimmed = TrimEnvironmentName(name);
    if (!StoreSelection(next, error)) return false;
    if (!ValidEnvironmentName(trimmed))
    { error = "Use a nonempty environment name of at most 60 characters without control characters."; return false; }
    for (auto& entry : next.environments)
        if (entry.id != next.selectedEnvironment && _wcsicmp(entry.name.c_str(), trimmed.c_str()) == 0)
        { error = "An environment with that name already exists."; return false; }
    for (auto& entry : next.environments)
        if (entry.id == next.selectedEnvironment) entry.name = trimmed;
    config = std::move(next);
    return true;
}

bool CreateEnvironment(const Paths& paths, Configuration& config, std::wstring_view name, std::string& error)
{
    auto next = config;
    if (!StoreSelection(next, error)) return false;
    if (next.environments.size() >= MaximumEnvironments)
    { error = "At most 32 debug environments can be created."; return false; }
    // CREATE_NEW-style identity: names never become directory segments.
    const std::wstring prefix = L"env-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    std::wstring id = prefix;
    unsigned suffix = 0;
    for (;;)
    {
        std::error_code ec;
        const bool exists = std::filesystem::exists(ResolvePaths(paths.normalData, id).root, ec);
        if (ec) { error = ec.message(); return false; }
        if (!exists && std::none_of(next.environments.begin(), next.environments.end(),
            [&](const auto& entry) { return entry.id == id; })) break;
        id = prefix + L"-" + std::to_wstring(++suffix);
    }
    const auto target = ResolvePaths(paths.normalData, id);
    next.environments.push_back({id, {}, false, false, target.defaultDesktop});
    LoadSelection(next, next.environments.back());
    if (!RenameEnvironment(next, name, error)) return false;
    config = std::move(next);
    return true;
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
    // Only this environment's own default desktop is allowed inside the
    // named-environment tree; no environment can enumerate another's data.
    if (Canonical(desktop) != Canonical(paths.defaultDesktop))
        forbidden.push_back(paths.environmentsRoot);
    const auto original = ResolvePaths(paths.normalData);
    if (paths.environmentId != L"default")
        forbidden.insert(forbidden.end(), {original.data, original.root / L"FullBackups",
            original.root / L"TempState", original.root / L"PrivateState", original.defaultDesktop});
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
    const auto expected = ResolvePaths(paths.normalData, paths.environmentId);
    if (!paths.normalData.is_absolute() || paths.root != expected.root || paths.data != expected.data ||
        paths.defaultDesktop != expected.defaultDesktop)
    { error = "The debug environment reset target does not match its managed root."; return false; }
    // Only these application-owned children may be removed. Preflight the entire
    // set before deleting anything; recursive iterators never follow symlinks.
    const std::vector<std::filesystem::path> targets = {
        paths.data, paths.root / L"FullBackups", paths.root / L"TempState", paths.root / L"PrivateState"};
    std::vector<std::filesystem::path> removals;
    const auto fail = [&](const std::filesystem::path& path, const std::error_code& ec) {
        const auto utf8 = path.u8string();
        error = ec.message() + " [" + std::string(utf8.begin(), utf8.end()) + "]";
        return false;
    };
    for (const auto& target : targets)
    {
        if (!SafeAncestors(target, error)) return false;
        std::error_code ec;
        if (!std::filesystem::exists(target, ec))
        { if (ec) return fail(target, ec); continue; }
        for (std::filesystem::recursive_directory_iterator it(target, ec), end;
            !ec && it != end; it.increment(ec))
        {
            if (!SafeAncestors(it->path(), error)) return false;
        }
        if (ec) return fail(target, ec);
        if (target == paths.data)
        {
            // Explorer and other processes can retain these DLLs after the
            // debug host exits. Their cache/leases outlive a profile reset;
            // neither deleting nor renaming a mapped runtime is safe.
            bool retainRuntime = false;
            std::vector<std::filesystem::path> children;
            for (std::filesystem::directory_iterator it(target, ec), end;
                !ec && it != end; it.increment(ec))
            {
                if (_wcsicmp(it->path().filename().c_str(), L"ShellHook") == 0 && it->is_directory(ec))
                    retainRuntime = true;
                else
                    children.push_back(it->path());
            }
            if (ec) return fail(target, ec);
            if (retainRuntime)
            {
                removals.insert(removals.end(), children.begin(), children.end());
                continue;
            }
        }
        removals.push_back(target);
    }
    for (const auto& target : removals)
    {
        std::error_code ec;
        std::filesystem::remove_all(target, ec);
        if (ec) return fail(target, ec);
    }
    return true;
}

bool Initialize(const std::filesystem::path& normalData,
    const std::vector<std::filesystem::path>& systemDesktops, std::string& error)
{
    Session next;
    next.paths = ResolvePaths(normalData);
    if (!Read(next.paths, next.configuration, error)) return false;
    next.paths = ResolvePaths(normalData, next.configuration.selectedEnvironment);
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
    if (saved.selectedEnvironment != session.configuration.selectedEnvironment) return true;
    saved.pendingDesktopChange = false;
    if (!Write(session.paths, saved, error)) return false;
    session.configuration.pendingDesktopChange = false;
    return true;
}
}

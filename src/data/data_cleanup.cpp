#include "data_cleanup.h"
#include "atomic_file.h"
#include "common/json_value.h"
#include "drag_drop/drop_staging.h"

#include <shlobj.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cwchar>
#include <limits>
#include <iterator>
#include <utility>

namespace snowdesktop::data_cleanup
{
struct Worker::State
{
    std::mutex mutex;
    std::condition_variable condition;
    Job pending;
    std::stop_source active;
    bool stopped = false;
};
Worker::Worker() : state_(std::make_shared<State>())
{
    std::thread([state = state_] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        for (;;)
        {
            Job job; std::stop_token stop;
            {
                std::unique_lock lock(state->mutex);
                state->condition.wait(lock, [&] { return state->stopped || state->pending; });
                if (state->stopped) break;
                job = std::exchange(state->pending, {});
                state->active = std::stop_source{}; stop = state->active.get_token();
            }
            try { job(stop); } catch (...) { /* A later snapshot retries collection. */ }
        }
        if (SUCCEEDED(com)) CoUninitialize();
    }).detach();
}
void Worker::Submit(Job job)
{
    std::lock_guard lock(state_->mutex);
    if (state_->stopped) return;
    state_->active.request_stop(); state_->pending = std::move(job); state_->condition.notify_one();
}
void Worker::Cancel()
{
    std::lock_guard lock(state_->mutex);
    state_->active.request_stop(); state_->pending = {};
}
void Worker::Stop()
{
    std::lock_guard lock(state_->mutex);
    state_->stopped = true; state_->active.request_stop(); state_->pending = {}; state_->condition.notify_all();
}
namespace
{
bool SafeDirectory(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}
bool SafeSubdirectory(const std::filesystem::path& data, const std::filesystem::path& directory)
{
    auto cursor = data;
    for (const auto& component : directory.lexically_relative(data))
    {
        if (component == L"..") return false;
        cursor /= component;
        if (!SafeDirectory(cursor)) return false;
    }
    return true;
}
std::vector<std::filesystem::path> Entries(const std::filesystem::path& directory)
{
    if (!SafeDirectory(directory)) return {};
    std::error_code error;
    std::vector<std::filesystem::path> paths;
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
        paths.push_back(it->path());
    return error ? std::vector<std::filesystem::path>{} : paths;
}
bool Digits(std::wstring_view value)
{
    return !value.empty() && value.find_first_not_of(L"0123456789") == value.npos;
}
bool ProcessExited(std::wstring_view digits)
{
    if (!Digits(digits) || digits.size() > 10) return false;
    const auto pid = std::wcstoull(std::wstring(digits).c_str(), nullptr, 10);
    if (!pid || pid > (std::numeric_limits<DWORD>::max)() || pid == GetCurrentProcessId()) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
    const bool exited = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    CloseHandle(process); return exited;
}
bool OwnerExited(std::wstring_view name, std::wstring_view prefix)
{
    if (!name.starts_with(prefix)) return false;
    name.remove_prefix(prefix.size());
    const auto separator = name.find(L'-');
    return separator != name.npos && ProcessExited(name.substr(0, separator));
}
bool Hex(std::wstring_view value, std::size_t length)
{
    return value.size() == length && value.find_first_not_of(L"0123456789abcdefABCDEF") == value.npos;
}
bool IsUuid(std::wstring_view name)
{
    return name.size() == 36 && name[8] == L'-' && name[13] == L'-' && name[18] == L'-' && name[23] == L'-' &&
        Hex(name.substr(0, 8), 8) && Hex(name.substr(9, 4), 4) && Hex(name.substr(14, 4), 4) &&
        Hex(name.substr(19, 4), 4) && Hex(name.substr(24), 12);
}
bool SafeFile(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE));
}
bool GeneratedAutomationFile(std::wstring_view name, bool capture)
{
    constexpr std::array<std::wstring_view, 5> captureFiles{
        L"capture.json", L"capture.png", L"capture.mp4", L"capture.partial.mp4", L"frames.csv"};
    constexpr std::array<std::wstring_view, 7> demoFiles{
        L"scene.json", L"scene-result.json", L"host-result.json", L"progress.json", L"poster.png", L"video.mp4", L"video.partial.mp4"};
    if (capture) return std::ranges::find(captureFiles, name) != captureFiles.end();
    return std::ranges::find(demoFiles, name) != demoFiles.end() ||
        (name.starts_with(L"frame-") && name.ends_with(L".png") && name.size() > 10 &&
         Digits(name.substr(6, name.size() - 10)));
}
std::size_t CollectAutomation(const std::filesystem::path& data, std::stop_token stop)
{
    using namespace std::chrono;
    using FileClock = std::filesystem::file_time_type::clock;
    const auto now = FileClock::now();
    const auto root = data / L"Automation";
    std::size_t removed = 0;
    for (const bool capture : {true, false})
    {
        const auto category = root / (capture ? L"Captures" : L"Demos");
        if (!SafeSubdirectory(data, category)) continue;
        for (const auto& run : Entries(category))
        {
            if (stop.stop_requested()) return removed;
            if (!IsUuid(run.filename().native()) || !SafeDirectory(run)) continue;
            std::error_code error;
            auto latest = std::filesystem::last_write_time(run, error);
            if (error || now - latest < days(7)) continue;
            std::vector<std::filesystem::path> files;
            bool recognized = true, finishedOutput = false;
            for (std::filesystem::directory_iterator entry(run, error), end; !error && entry != end; entry.increment(error))
            {
                const auto path = entry->path();
                const auto name = path.filename().native();
                if (!GeneratedAutomationFile(name, capture) || !SafeFile(path))
                {
                    recognized = false;
                    break;
                }
                const auto stamp = entry->last_write_time(error);
                if (error) break;
                latest = std::max(latest, stamp);
                finishedOutput |= name == (capture ? L"capture.png" : L"video.mp4") ||
                    (capture && name == L"capture.mp4");
                files.push_back(path);
            }
            const auto retention = finishedOutput ? days(30) : days(7);
            if (error || !recognized || now - latest < retention || !SafeSubdirectory(data, run)) continue;

            // Delete only the verified flat set. New or unknown files prevent
            // removing the now nonempty directory, and junctions are never traversed.
            bool complete = true;
            for (const auto& file : files)
            {
                if (stop.stop_requested()) return removed;
                if (!SafeSubdirectory(data, run) || !SafeFile(file) || !DeleteFileW(file.c_str()))
                {
                    complete = false;
                    break;
                }
            }
            if (complete && SafeSubdirectory(data, run) && RemoveDirectoryW(run.c_str())) ++removed;
        }
    }
    return removed;
}
std::size_t CollectLegacyIconImports(const std::filesystem::path& data, std::stop_token stop)
{
    using namespace std::chrono;
    const auto library = data / L"icon-library";
    const auto packs = library / L"packs";
    if (!SafeSubdirectory(data, library)) return 0;
    const auto now = std::filesystem::file_time_type::clock::now();
    std::size_t removed = 0;
    const auto collect = [&](const std::filesystem::path& directory, bool archives) {
        if (!SafeSubdirectory(data, directory)) return;
        for (const auto& path : Entries(directory))
        {
            if (stop.stop_requested()) return;
            const auto extension = path.extension().native();
            const auto stem = path.stem().native();
            const std::wstring_view name(stem);
            const bool owned = archives
                ? (extension == L".zip" || extension == L".apk") && Hex(name, 64)
                : (name.starts_with(L".import-") && Hex(name.substr(8), 64)) ||
                    (name.starts_with(L".pack-group-") && Hex(name.substr(12), 32));
            if (!owned || !SafeFile(path)) continue;
            std::error_code error;
            const auto stamp = std::filesystem::last_write_time(path, error);
            if (!error && now - stamp >= days(1) && SafeSubdirectory(data, directory) &&
                SafeFile(path) && DeleteFileW(path.c_str())) ++removed;
        }
    };
    collect(packs, true);
    if (!stop.stop_requested() && SafeSubdirectory(data, packs)) RemoveDirectoryW(packs.c_str());
    collect(library, false);
    return removed;
}
std::size_t CollectSymbolPartials(const std::filesystem::path& data, std::stop_token stop)
{
    const auto root = data / L"ShellHookSymbols";
    if (!SafeSubdirectory(data, root)) return 0;
    std::size_t removed = 0;
    for (const auto& directory : Entries(root))
    {
        const auto key = directory.filename().native();
        if (key.size() < 33 || key.size() > 40 || !Hex(key, key.size()) || !SafeSubdirectory(data, directory)) continue;
        for (const auto& file : Entries(directory))
        {
            if (stop.stop_requested()) return removed;
            const auto name = file.filename().native();
            constexpr std::wstring_view marker = L".pdb.partial-";
            const auto start = name.find(marker);
            if (start == name.npos || !ProcessExited(std::wstring_view(name).substr(start + marker.size()))) continue;
            if (SafeSubdirectory(data, directory) && SafeFile(file) && DeleteFileW(file.c_str())) ++removed;
        }
    }
    return removed;
}
void ReadJsonPaths(const JsonValue& value, std::vector<std::filesystem::path>& paths)
{
    const auto append = [&](const std::string& text) {
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (!count) return;
        std::wstring wide(count, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), count);
        const std::filesystem::path path(wide);
        if (path.is_absolute()) paths.push_back(path.lexically_normal());
    };
    if (value.IsString()) append(value.string);
    for (const auto& child : value.array) ReadJsonPaths(child, paths);
    for (const auto& [key, child] : value.object) { append(key); ReadJsonPaths(child, paths); }
}
}
std::size_t Collect(const std::filesystem::path& data,
    const std::vector<std::filesystem::path>& referencedPaths, std::stop_token stop)
{
    if (!SafeDirectory(data)) return 0;
    std::size_t removed = CollectAutomation(data, stop);
    removed += CollectLegacyIconImports(data, stop);
    removed += CollectSymbolPartials(data, stop);
    if (stop.stop_requested()) return removed;
    auto protectedPaths = referencedPaths;
    bool canCollectLegacy = true;
    // Saved component values may still point to legacy backing files. Preserve
    // them until the owning component has migrated or removed that reference.
    for (const auto* name : {L"SnowDesktop.layout.json", L"SnowDesktop.storage.json", L"SnowDesktop.dock.json"})
    {
        const auto path = data / name;
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) canCollectLegacy = false;
            continue;
        }
        if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        { canCollectLegacy = false; continue; }
        std::string text; JsonValue document;
        if (!atomic_file::ReadAll(path, text) || !ParseJson(text, document)) { canCollectLegacy = false; continue; }
        ReadJsonPaths(document, protectedPaths);
    }
    auto observed = referencedPaths;
    for (const auto& directory : referencedPaths)
        if (SafeDirectory(directory))
        {
            const auto children = Entries(directory);
            observed.insert(observed.end(), children.begin(), children.end());
        }
    for (const auto& path : observed)
    {
        if (stop.stop_requested()) return removed;
        if (_wcsicmp(path.extension().c_str(), L".lnk") != 0) continue;
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) canCollectLegacy = false;
            continue;
        }
        if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE))
        { canCollectLegacy = false; continue; }
        Microsoft::WRL::ComPtr<IShellLinkW> link;
        Microsoft::WRL::ComPtr<IPersistFile> file;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
            FAILED(link.As(&file)) || FAILED(file->Load(path.c_str(), STGM_READ))) { canCollectLegacy = false; continue; }
        wchar_t target[32768]{};
        if (FAILED(link->GetPath(target, static_cast<int>(std::size(target)), nullptr, SLGP_RAWPATH))) canCollectLegacy = false;
        else if (*target) protectedPaths.emplace_back(target);
    }
    if (canCollectLegacy)
    {
        const auto legacy = data / L"DropContent";
        for (const auto& path : SafeSubdirectory(data, legacy) ? Entries(legacy) : std::vector<std::filesystem::path>{})
        {
            if (stop.stop_requested()) return removed;
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) continue;
            const bool referenced = std::any_of(protectedPaths.begin(), protectedPaths.end(), [&](const auto& live) {
                return _wcsicmp(live.lexically_normal().c_str(), path.lexically_normal().c_str()) == 0;
            });
            if (!referenced && DeleteFileW(path.c_str())) ++removed;
        }
        if (SafeSubdirectory(data, legacy))
        { std::error_code error; std::filesystem::remove(legacy, error); } // Empty only.
    }
    // Process-owned publish/package/initialization scratch trees. Names without
    // a recognizable owner remain untouched, as do live or inaccessible owners.
    for (const auto& [directory, prefix] : std::vector<std::pair<std::filesystem::path, std::wstring>>{
            {data / L"ThemeWorkshop" / L"staging", L"upload-"},
            {data / L"SteamWorkshopManager" / L"staging" / L"uploads", L"upload-"},
            {data / L"SteamWorkshopManager" / L"staging" / L"packages", L"package-"},
            {data / L"initialization-experiments", L"session-"}})
        for (const auto& path : SafeSubdirectory(data, directory) ? Entries(directory) : std::vector<std::filesystem::path>{})
        {
            if (stop.stop_requested()) return removed;
            if (OwnerExited(path.filename().wstring(), prefix) && drop_staging::RemoveTree(path)) ++removed;
        }
    // Apply the same retention to self-written dumps and WER dumps. Keep the
    // newest dump even if it alone exceeds the budget; never touch crash.log.
    struct Dump { std::filesystem::path path; std::filesystem::file_time_type modified; std::uint64_t bytes; };
    std::vector<Dump> dumps;
    for (const auto& directory : {data / L"crashdumps", data / L"crashdumps" / L"wer"})
        for (const auto& path : SafeSubdirectory(data, directory) ? Entries(directory) : std::vector<std::filesystem::path>{})
        {
            const auto name = path.filename().wstring();
            if (_wcsicmp(path.extension().c_str(), L".dmp") != 0 ||
                (!name.starts_with(L"SnowDesktop_") && !name.starts_with(L"SnowDesktop.exe."))) continue;
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) continue;
            std::error_code error;
            const auto bytes = std::filesystem::file_size(path, error);
            if (error) continue;
            const auto modified = std::filesystem::last_write_time(path, error);
            if (!error) dumps.push_back({path, modified, bytes});
        }
    std::sort(dumps.begin(), dumps.end(), [](const auto& a, const auto& b) { return a.modified > b.modified; });
    std::uint64_t bytes = 0; std::size_t kept = 0;
    for (const auto& dump : dumps)
    {
        if (stop.stop_requested()) return removed;
        if (dump.bytes && (kept == 0 || (kept < 5 && bytes + dump.bytes <= 128ull * 1024 * 1024)))
        { bytes += dump.bytes; ++kept; continue; }
        if (DeleteFileW(dump.path.c_str())) ++removed;
    }
    return removed;
}
}

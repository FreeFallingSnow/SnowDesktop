#include "shell_extension_menu_cache.h"
#include "shell_extension_diagnostics.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <shlobj.h>
#include <shlwapi.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

namespace snowdesktop::shell_extensions
{
namespace
{
constexpr std::uint32_t Schema = 2;
constexpr std::uint64_t MaximumSnapshotBytes = 2 * 1024 * 1024;
std::uint64_t Hash(std::span<const std::byte> bytes)
{
    std::uint64_t value = 14695981039346656037ull;
    for (auto byte : bytes)
    {
        value ^= std::to_integer<unsigned char>(byte);
        value *= 1099511628211ull;
    }
    return value;
}
std::uint64_t FileStamp(const std::filesystem::path &path)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        return 0;
    return (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
           data.ftLastWriteTime.dwLowDateTime;
}
std::optional<settings_ipc::Bytes> Read(const std::filesystem::path &path, std::uint64_t limit)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > limit)
        return {};
    std::ifstream file(path, std::ios::binary);
    settings_ipc::Bytes bytes(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size)))
        return {};
    return bytes;
}
bool Write(const std::filesystem::path &path, const settings_ipc::Bytes &bytes)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return false;
    auto temporary = path;
    temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
                 std::to_wstring(GetCurrentThreadId()) + L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file.write(reinterpret_cast<const char *>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size())))
        {
            file.close();
            std::filesystem::remove(temporary, error);
            return false;
        }
        file.close();
        if (!file)
        {
            std::filesystem::remove(temporary, error);
            return false;
        }
    }
    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        return true;
    std::filesystem::remove(temporary, error);
    return false;
}
bool Sanitize(std::vector<Entry> &entries, size_t &count, unsigned depth = 0)
{
    if (depth > 8)
        return false;
    for (auto &entry : entries)
    {
        if (++count > 2048 || entry.label.size() > 2048 || entry.provider.size() > 8192 ||
            entry.key.size() > 8192 || entry.width < 0 || entry.height < 0 || entry.width > 128 ||
            entry.height > 128 || entry.pixels.size() != static_cast<size_t>(entry.width * entry.height * 4))
            return false;
        entry.token = 0;
        if (!Sanitize(entry.children, count, depth + 1))
            return false;
    }
    return true;
}
std::filesystem::path DefaultDirectory()
{
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
        return {};
    const std::filesystem::path base(local);
    CoTaskMemFree(local);
    wchar_t executable[32768]{};
    const auto length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (!length || length >= std::size(executable))
        return {};
    // Different portable/install locations and test executables cannot borrow
    // one another's snapshots; the settings child uses the same executable.
    return base / L"SnowDesktop" / L"Cache" / L"ShellMenus-v2" /
           std::to_wstring(Hash(settings_ipc::Pack(std::wstring(executable, length))));
}
using DiskRow = std::tuple<std::uint32_t, settings_ipc::Bytes, std::wstring, std::uint64_t, Request, Reply>;

std::wstring CacheFile(const settings_ipc::Bytes &bytes)
{
    unsigned char digest[32]{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<std::byte *>(bytes.data())), static_cast<ULONG>(bytes.size()), digest, 32) < 0) return {};
    std::wstring text; constexpr wchar_t hex[] = L"0123456789abcdef";
    for (const auto c : digest) { text += hex[c >> 4]; text += hex[c & 15]; }
    return text + L".bin";
}
struct PendingQueries
{
    struct Job
    {
        std::unique_ptr<Session> session;
        CommandReference reference;
        POINT position{};
        std::uint64_t generation = 0;
        MenuSnapshotCache *cache = nullptr;
        MenuSnapshotCache::Ticket ticket;
    };
    std::vector<Job> jobs;
    UINT_PTR timer = 0;
    bool ticking = false;
    ~PendingQueries()
    {
        if (timer)
            KillTimer(nullptr, timer);
    }
    static PendingQueries &Current()
    {
        thread_local PendingQueries state;
        return state;
    }
    static void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD)
    {
        auto &state = Current();
        if (state.ticking)
            return; // Invoke acknowledgement may pump this STA.
        state.ticking = true;
        for (size_t i = 0; i < state.jobs.size();)
        {
            try
            {
                auto &job = state.jobs[i];
                auto reply = job.session->Poll();
                if (!reply)
                {
                    ++i;
                    continue;
                }
                if (job.generation == MenuCacheGeneration())
                {
                    if (job.cache)
                        job.cache->Store(job.ticket, *reply);
                    const auto token = ResolveCommand(*reply, job.reference);
                    if (token)
                        job.session->Invoke(token, job.position);
                }
            }
            catch (...)
            { /* A failed query/invocation cannot escape a timer callback. */
            }
            state.jobs.erase(state.jobs.begin() + i);
        }
        state.ticking = false;
        if (state.jobs.empty() && state.timer)
        {
            KillTimer(nullptr, state.timer);
            state.timer = 0;
        }
    }
    bool Add(Job job)
    {
        if (jobs.size() >= 8)
            return false;
        if (!timer)
            timer = SetTimer(nullptr, 0, 30, Tick);
        if (!timer)
            return false;
        jobs.push_back(std::move(job));
        return true;
    }
};
} // namespace

MenuSnapshotCache::MenuSnapshotCache(std::filesystem::path directory) : directory_(std::move(directory))
{
}
MenuSnapshotCache &SharedMenuCache()
{
    thread_local MenuSnapshotCache cache(DefaultDirectory());
    return cache;
}
std::uint64_t MenuSnapshotCache::Now()
{
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    return ((static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime) / 10000;
}
std::wstring MenuSnapshotCache::Epoch() const
{
    if (directory_.empty())
        return L"unavailable";
    try
    {
        if (const auto data = Read(directory_ / L"epoch.bin", 256))
            return settings_ipc::Unpack<std::wstring>(*data);
    }
    catch (...)
    {
        return L"invalid";
    }
    return {};
}
void MenuSnapshotCache::Invalidate()
{
    MenuTrace("cache", "invalidate");
    rows_.clear();
    if (directory_.empty())
        return;
    GUID guid{};
    wchar_t text[40]{};
    if (SUCCEEDED(CoCreateGuid(&guid)))
    {
        StringFromGUID2(guid, text, 40);
        Write(directory_ / L"epoch.bin", settings_ipc::Pack(std::wstring(text)));
    }
}
MenuSnapshotCache::Ticket MenuSnapshotCache::Capture(const Request &source) const
{
    try
    {
        if (directory_.empty())
            return {};
        auto request = source;
        if (request.paths.empty() && request.context == Context::Desktop)
        {
            PWSTR path = nullptr;
            if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &path)))
                return {};
            request.paths = {path};
            request.background = true;
            CoTaskMemFree(path);
        }
        const bool sample = request.paths.empty();
        if (sample && (!request.catalogueOnly || request.context == Context::Automatic))
            return {};
        if (request.paths.size() > 256)
            return {};
        if (!sample)
            request.catalogueOnly = false; // Shared exact-object key.
        std::vector<std::tuple<DWORD, DWORD, DWORD, DWORD, DWORD>> stamps;
        for (auto &path : request.paths)
        {
            const std::filesystem::path file(path);
            // A cache lookup must not synchronously touch remote Shell targets.
            if (!file.is_absolute() || PathIsNetworkPathW(path.c_str()) ||
                GetDriveTypeW(file.root_path().c_str()) == DRIVE_REMOTE)
                return {};
            path = file.lexically_normal().wstring();
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
                return {};
            // Directory timestamps change whenever a child is created/removed.
            // Keep its display snapshot stable; invocation still resolves the
            // command from a fresh query of this exact directory and selection.
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                stamps.emplace_back(data.dwFileAttributes, 0, 0, 0, 0);
            else
                stamps.emplace_back(data.dwFileAttributes, data.nFileSizeHigh, data.nFileSizeLow,
                                    data.ftLastWriteTime.dwHighDateTime, data.ftLastWriteTime.dwLowDateTime);
        }
        request.context = ResolveContext(request);
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
        Ticket ticket;
        ticket.request = source;
        ticket.identity =
            settings_ipc::Pack(request, stamps, FileStamp(executable), GetUserDefaultUILanguage());
        ticket.epoch = Epoch();
        ticket.file = CacheFile(ticket.identity);
        if (request.context == Context::Desktop) ticket.file = L"d-" + ticket.file;
        if (ticket.file.empty()) return {};
        return ticket;
    }
    catch (...)
    {
        return {};
    }
}
std::optional<Reply> MenuSnapshotCache::Find(const Ticket &ticket, std::uint64_t now)
{
    try
    {
        if (!ticket || ticket.epoch != Epoch())
            return {};
        const auto path = directory_ / ticket.file;
        const auto stamp = FileStamp(path);
        if (!stamp)
            return {};
        auto row = rows_.find(ticket.identity);
        if (row == rows_.end() || row->second.fileStamp != stamp || row->second.identity != ticket.identity ||
            row->second.epoch != ticket.epoch)
        {
            const auto data = Read(path, MaximumSnapshotBytes);
            if (!data)
                return {};
            auto [schema, identity, epoch, written, request, reply] = settings_ipc::Unpack<DiskRow>(*data);
            size_t count = 0;
            if (schema != Schema || identity != ticket.identity || epoch != ticket.epoch || !reply.ok ||
                !Sanitize(reply.entries, count))
                return {};
            row = rows_
                      .insert_or_assign(ticket.identity, MemoryRow{identity, std::move(epoch), written,
                                                               stamp, ++clock_, data->size(), request.context == Context::Desktop, std::move(reply)})
                      .first;
        }
        if (now < row->second.written || now - row->second.written >= LifetimeMs)
            return {};
        row->second.used = ++clock_;
        std::error_code touched;
        std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), touched);
        row->second.fileStamp = FileStamp(path);
        TrimMemory();
        MenuTrace("cache", "hit", 0, static_cast<unsigned>(row->second.reply.entries.size()));
        return row->second.reply;
    }
    catch (...)
    {
        return {};
    }
}
bool MenuSnapshotCache::Store(const Ticket &ticket, const Reply &reply, std::uint64_t now)
{
    try
    {
        if (!ticket || !reply.ok || (ticket.sequence && issued_[ticket.identity] != ticket.sequence) || ticket.epoch != Epoch() ||
            Capture(ticket.request).identity != ticket.identity)
            return false;
        auto snapshot = reply;
        snapshot.error.clear();
        size_t count = 0;
        if (!Sanitize(snapshot.entries, count))
            return false;
        const auto bytes = settings_ipc::Pack(DiskRow{Schema, ticket.identity, ticket.epoch, now, ticket.request, snapshot});
        if (bytes.size() > MaximumSnapshotBytes)
            return false;
        const auto path = directory_ / ticket.file;
        if (!Write(path, bytes))
            return false;
        rows_.insert_or_assign(
            ticket.identity, MemoryRow{ticket.identity, ticket.epoch, now, FileStamp(path), ++clock_, bytes.size(), ticket.request.context == Context::Desktop, std::move(snapshot)});
        TrimMemory();
        TrimDisk();
        return true;
    }
    catch (...)
    {
        return false;
    }
}
MenuSnapshotCache::Ticket MenuSnapshotCache::Begin(Ticket ticket)
{
    if (ticket) ticket.sequence = ++issued_[ticket.identity];
    return ticket;
}
void MenuSnapshotCache::Erase(const Request &request)
{
    auto ticket = Capture(request);
    if (!ticket) return;
    rows_.erase(ticket.identity);
    ++issued_[ticket.identity];
    std::error_code ignored;
    std::filesystem::remove(directory_ / ticket.file, ignored);
}
void MenuSnapshotCache::TrimMemory()
{
    std::uint64_t bytes = 0; for (const auto &[id, row] : rows_) bytes += row.bytes;
    while (rows_.size() > MemoryEntries || bytes > MemoryBytes)
    {
        const auto victim = std::min_element(rows_.begin(), rows_.end(), [](const auto &a, const auto &b) { return std::tie(a.second.desktop, a.second.used) < std::tie(b.second.desktop, b.second.used); });
        if (victim == rows_.end()) break;
        bytes -= victim->second.bytes; rows_.erase(victim);
    }
}
void MenuSnapshotCache::TrimDisk()
{
    struct File { std::filesystem::path path; std::uint64_t bytes, stamp; bool desktop; };
    std::vector<File> files; std::uint64_t total = 0;
    std::error_code error;
    for (const auto &file : std::filesystem::directory_iterator(directory_, error))
    {
        if (file.path().extension() != L".bin" || file.path().filename() == L"epoch.bin" || file.path().filename() == L"catalogue.bin") continue;
        const auto bytes = file.file_size(error); if (error) { error.clear(); continue; }
        const bool desktop = file.path().filename().wstring().starts_with(L"d-");
        files.push_back({file.path(), bytes, FileStamp(file.path()), desktop}); total += bytes;
    }
    std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) { return std::tie(a.desktop, a.stamp) < std::tie(b.desktop, b.stamp); });
    size_t count = files.size();
    for (const auto &file : files)
    {
        if (count <= DiskEntries && total <= DiskBytes) break;
        if (std::filesystem::remove(file.path, error)) { --count; total -= file.bytes; }
    }
}
std::vector<std::pair<Request, Reply>> MenuSnapshotCache::Warm()
{
    std::vector<std::pair<Request, Reply>> result;
    std::vector<std::filesystem::path> paths;
    std::error_code error;
    for (const auto &file : std::filesystem::directory_iterator(directory_, error))
        if ((file.path().filename().wstring().size() == 68 || file.path().filename().wstring().size() == 70) && file.path().extension() == L".bin") paths.push_back(file.path());
    std::sort(paths.begin(), paths.end(), [](const auto &a, const auto &b) { return FileStamp(a) > FileStamp(b); });
    for (const auto &path : paths)
    {
        if (result.size() >= MemoryEntries) break;
        try
        {
            const auto bytes = Read(path, MaximumSnapshotBytes); if (!bytes) continue;
            const auto disk = settings_ipc::Unpack<DiskRow>(*bytes);
            const auto &request = std::get<4>(disk);
            const auto ticket = Capture(request);
            if (auto reply = Find(ticket)) result.emplace_back(request, std::move(*reply));
        }
        catch (...) {}
    }
    return result;
}
CommandReference AppendReference(CommandReference path, const Entry &entry)
{
    path.emplace_back(entry.provider, entry.key, entry.label, entry.native);
    return path;
}
UINT ResolveCommand(const Reply &reply, const CommandReference &reference)
{
    if (!reply.ok || reference.empty() || reference.size() > 9)
        return 0;
    const auto *entries = &reply.entries;
    const Entry *found = nullptr;
    for (const auto &part : reference)
    {
        found = nullptr;
        for (const auto &entry : *entries)
            if (!entry.separator && std::tie(entry.provider, entry.key, entry.label, entry.native) == part)
            {
                if (found)
                    return 0; // Ambiguity must never select by position.
                found = &entry;
            }
        if (!found || !found->enabled)
            return 0;
        entries = &found->children;
    }
    return found && found->children.empty() ? found->token : 0;
}
bool InvokeWhenReady(std::unique_ptr<Session> session, CommandReference reference, POINT position,
                     std::uint64_t generation)
{
    if (!session || reference.empty())
        return false;
    return PendingQueries::Current().Add({std::move(session), std::move(reference), position, generation});
}
void FinishQueryInBackground(std::unique_ptr<Session> session, MenuSnapshotCache::Ticket ticket,
                             std::uint64_t generation)
{
    try
    {
        if (session)
            PendingQueries::Current().Add(
                {std::move(session), {}, {}, generation, &SharedMenuCache(), std::move(ticket)});
    }
    catch (...)
    { /* This is also called during popup destruction. */
    }
}
} // namespace snowdesktop::shell_extensions

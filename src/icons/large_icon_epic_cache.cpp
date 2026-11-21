#include "large_icon_epic.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <unordered_set>

namespace snowdesktop::large_icon_epic
{
namespace
{
// Chromium's Windows block-file cache. Read only indexed HTTP image bodies.
// https://chromium.googlesource.com/chromium/src/+/HEAD/net/disk_cache/blockfile/disk_format.h
constexpr std::size_t IndexHeaderSize = 368;
constexpr std::size_t BlockHeaderSize = 8192;
constexpr std::size_t EntrySize = 256;
constexpr std::size_t KeyOffset = 96;
constexpr std::size_t MaxEntries = 32768;
constexpr std::size_t MaxCachedBody = 4 * 1024 * 1024;
constexpr std::size_t MaxCandidates = 6;

std::string ReadShared(const std::filesystem::path& path, std::size_t limit, std::size_t offset = 0,
    bool exact = false)
{
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {};
    const std::unique_ptr<void, decltype(&CloseHandle)> file(handle, CloseHandle);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < static_cast<LONGLONG>(offset)) return {};
    const auto available = static_cast<std::uint64_t>(size.QuadPart) - offset;
    if (exact ? available < limit : available > limit) return {};
    const auto count = static_cast<DWORD>(exact ? limit : available);
    if (!count) return {};
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (offset && !SetFilePointerEx(file.get(), position, nullptr, FILE_BEGIN)) return {};
    std::string bytes(count, '\0');
    DWORD read = 0;
    if (!ReadFile(file.get(), bytes.data(), count, &read, nullptr) || read != count) return {};
    return bytes;
}

std::uint32_t Word(std::string_view bytes, std::size_t offset)
{
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

std::string ReadStream(const std::filesystem::path& cache, std::uint32_t address, std::size_t length)
{
    if (!(address & 0x80000000) || !length || length > MaxCachedBody) return {};
    const auto kind = (address >> 28) & 7;
    if (kind == 0)
    {
        wchar_t name[16]{};
        swprintf_s(name, L"f_%06x", address & 0x0fffffff);
        return ReadShared(cache / name, length, 0, true);
    }
    if (kind < 2 || kind > 4) return {};
    const std::size_t blockSize = std::size_t{256} << ((kind - 2) * 2);
    const auto blocks = 1 + ((address >> 24) & 3);
    if (length > blockSize * blocks) return {};
    const auto offset = BlockHeaderSize + (address & 0xffff) * blockSize;
    return ReadShared(cache / (L"data_" + std::to_wstring((address >> 16) & 0xff)), length, offset, true);
}

std::string_view WithoutQuery(std::string_view url)
{
    return url.substr(0, url.find('?'));
}

bool IsImageBody(std::string_view bytes)
{
    return bytes.starts_with("\xff\xd8\xff") || bytes.starts_with("\x89PNG\r\n\x1a\n") ||
        (bytes.size() >= 12 && (bytes.substr(4, 8) == "ftypavif" ||
            (bytes.starts_with("RIFF") && bytes.substr(8, 4) == "WEBP")));
}

void ReadCache(const std::filesystem::path& cache, std::string_view url, std::stop_token stop,
    std::vector<std::string>& candidates)
{
    const auto index = ReadShared(cache / L"index", 4 * 1024 * 1024);
    if (index.size() < IndexHeaderSize || Word(index, 0) != 0xc103cac3) return;
    const auto version = Word(index, 4);
    if (version != 0x20001 && version != 0x30000) return;
    const auto tableLength = Word(index, 28);
    if (!tableLength || tableLength > (index.size() - IndexHeaderSize) / sizeof(std::uint32_t)) return;
    const auto entries = ReadShared(cache / L"data_1", BlockHeaderSize + EntrySize * 65536);
    if (entries.size() < BlockHeaderSize || Word(entries, 8) != EntrySize) return;

    std::unordered_set<std::uint32_t> visited;
    const auto baseUrl = WithoutQuery(url);
    for (std::size_t i = 0; i < tableLength && !stop.stop_requested() && visited.size() < MaxEntries; ++i)
    {
        auto address = Word(index, IndexHeaderSize + i * sizeof(std::uint32_t));
        while (address && !stop.stop_requested() && visited.size() < MaxEntries && visited.insert(address).second)
        {
            if ((address & 0xfc000000) != 0xa0000000) break;
            const auto offset = BlockHeaderSize + (address & 0xffff) * EntrySize;
            const auto length = EntrySize * (1 + ((address >> 24) & 3));
            std::string additionalEntry;
            std::string_view entry;
            if (((address >> 16) & 0xff) == 1)
            {
                if (offset + length > entries.size()) break;
                entry = std::string_view(entries).substr(offset, length);
            }
            else
            {
                additionalEntry = ReadStream(cache, address, length);
                if (additionalEntry.empty()) break;
                entry = additionalEntry;
            }
            address = Word(entry, 4);
            if (Word(entry, 20) != 0) continue;
            const auto keyLength = Word(entry, 32);
            const auto longKey = Word(entry, 36);
            if (!keyLength || keyLength > 4096 || (!longKey && keyLength > entry.size() - KeyOffset)) continue;
            const auto externalKey = longKey ? ReadStream(cache, longKey, keyLength) : std::string{};
            auto key = longKey ? std::string_view(externalKey) : entry.substr(KeyOffset, keyLength);
            const auto separator = key.find_last_of(' ');
            if (separator != key.npos) key.remove_prefix(separator + 1);
            if (WithoutQuery(key) != baseUrl) continue;

            const auto size = Word(entry, 44);
            if (candidates.size() == MaxCandidates && size <= candidates.back().size()) continue;
            auto body = ReadStream(cache, Word(entry, 60), size);
            if (!IsImageBody(body) || std::ranges::find(candidates, body) != candidates.end()) continue;
            const auto position = std::ranges::lower_bound(candidates, body.size(), std::greater{}, &std::string::size);
            candidates.insert(position, std::move(body));
            if (candidates.size() > MaxCandidates) candidates.pop_back();
        }
    }
}
}

std::vector<std::string> CachedCovers(std::string_view url, std::stop_token stop)
{
    if (!IsCoverUrl(url) || stop.stop_requested()) return {};
    PWSTR directory = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &directory))) return {};
    const auto saved = std::filesystem::path(directory) / L"EpicGamesLauncher" / L"Saved";
    CoTaskMemFree(directory);
    std::vector<std::filesystem::path> caches;
    std::error_code ec;
    for (auto entry = std::filesystem::directory_iterator(saved, ec);
        !ec && entry != std::filesystem::directory_iterator{} && !stop.stop_requested(); entry.increment(ec))
    {
        const auto name = entry->path().filename().wstring();
        if (name == L"webcache" || (name.starts_with(L"webcache_") && name.size() > 9 &&
                std::ranges::all_of(std::wstring_view(name).substr(9), [](wchar_t c) { return c >= L'0' && c <= L'9'; })))
            caches.push_back(entry->path() / L"Cache");
    }
    std::ranges::sort(caches, std::greater{});
    std::vector<std::string> candidates;
    for (const auto& cache : caches)
    {
        if (stop.stop_requested()) return {};
        ReadCache(cache, url, stop, candidates);
    }
    return candidates;
}
} // namespace snowdesktop::large_icon_epic

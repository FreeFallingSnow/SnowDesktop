#include "hook_entry_deployment.h"
#include "injectable_runtime_cache.h"
#include <fstream>
#include <vector>
#include <mutex>
#include <cstring>
#include <limits>

namespace snowdesktop::hook_entry
{
namespace
{
template<typename T>
bool ReadAt(const std::vector<BYTE>& bytes, std::size_t offset, T& result)
{
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) return false;
    std::memcpy(&result, bytes.data() + offset, sizeof(T));
    return true;
}
bool Reparse(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}
struct Handle
{
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
};
struct Publication
{
    std::mutex mutex;
    HANDLE mapping = nullptr;
    Catalog* catalog = nullptr;
    ~Publication()
    {
        if (catalog) UnmapViewOfFile(catalog);
        if (mapping) CloseHandle(mapping);
    }
    bool Publish(Payload payload, const std::filesystem::path& image)
    {
        const auto index = static_cast<std::size_t>(payload);
        if (index >= kPayloadCount || !image.is_absolute() || image.native().size() >= kPathCapacity)
        { SetLastError(ERROR_INVALID_PARAMETER); return false; }
        if (!catalog)
        {
            mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                0, sizeof(Catalog), CatalogName(GetCurrentProcessId()).c_str());
            if (!mapping) return false;
            if (GetLastError() == ERROR_ALREADY_EXISTS)
            { CloseHandle(mapping); mapping = nullptr; SetLastError(ERROR_ALREADY_EXISTS); return false; }
            catalog = static_cast<Catalog*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Catalog)));
            if (!catalog) { CloseHandle(mapping); mapping = nullptr; return false; }
            new(catalog) Catalog;
            catalog->owner = GetCurrentProcessId();
            catalog->ownerCreation = ProcessCreation(GetCurrentProcess());
            if (!catalog->ownerCreation) return false;
        }
        const auto path = image.wstring();
        if (std::wstring(catalog->paths[index].data()) == path && catalog->generations[index]) return true;
        InterlockedIncrement(&catalog->sequence);
        std::copy(path.begin(), path.end(), catalog->paths[index].begin());
        catalog->paths[index][path.size()] = L'\0';
        ++catalog->generations[index];
        MemoryBarrier(); InterlockedIncrement(&catalog->sequence);
        return true;
    }
};
Publication& Publisher()
{
    static Publication publication;
    return publication;
}
}

std::filesystem::path CurrentImplementation(Payload payload)
{
    auto& publication = Publisher();
    std::lock_guard lock(publication.mutex);
    Snapshot snapshot;
    return publication.catalog && Read(*publication.catalog, GetCurrentProcessId(), payload, snapshot)
        ? std::filesystem::path(snapshot.path) : std::filesystem::path{};
}

bool HasEntryAbi(const std::filesystem::path& image, WORD expectedMachine)
{
    std::ifstream input(image, std::ios::binary | std::ios::ate);
    if (!input) return false;
    const auto size = input.tellg();
    if (size < 0 || size > 4 * 1024 * 1024) return false;
    std::vector<BYTE> bytes(static_cast<std::size_t>(size));
    input.seekg(0); input.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!input) return false;
    IMAGE_DOS_HEADER dos{}; DWORD signature = 0; IMAGE_FILE_HEADER file{};
    if (!ReadAt(bytes, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        !ReadAt(bytes, static_cast<std::size_t>(dos.e_lfanew), signature) || signature != IMAGE_NT_SIGNATURE ||
        !ReadAt(bytes, static_cast<std::size_t>(dos.e_lfanew) + sizeof(DWORD), file) || file.Machine != expectedMachine ||
        file.NumberOfSections > 64) return false;
    const auto optional = static_cast<std::size_t>(dos.e_lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    WORD magic = 0; IMAGE_DATA_DIRECTORY exports{};
    if (!ReadAt(bytes, optional, magic)) return false;
    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
        IMAGE_OPTIONAL_HEADER64 header{};
        if (!ReadAt(bytes, optional, header)) return false;
        exports = header.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    }
    else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
    {
        IMAGE_OPTIONAL_HEADER32 header{};
        if (!ReadAt(bytes, optional, header)) return false;
        exports = header.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    }
    else return false;
    const auto sections = optional + file.SizeOfOptionalHeader;
    const auto offsetOf = [&](DWORD rva) -> std::size_t {
        for (unsigned i = 0; i < file.NumberOfSections; ++i)
        {
            IMAGE_SECTION_HEADER section{};
            if (!ReadAt(bytes, sections + i * sizeof(section), section)) break;
            if (rva >= section.VirtualAddress && rva - section.VirtualAddress < section.SizeOfRawData)
                return static_cast<std::size_t>(section.PointerToRawData) + rva - section.VirtualAddress;
        }
        return bytes.size();
    };
    IMAGE_EXPORT_DIRECTORY directory{};
    if (!exports.VirtualAddress || !ReadAt(bytes, offsetOf(exports.VirtualAddress), directory) || directory.NumberOfNames > 256) return false;
    for (DWORD i = 0; i < directory.NumberOfNames; ++i)
    {
        DWORD name = 0; WORD ordinal = 0; DWORD function = 0;
        if (!ReadAt(bytes, offsetOf(directory.AddressOfNames) + i * sizeof(DWORD), name) ||
            !ReadAt(bytes, offsetOf(directory.AddressOfNameOrdinals) + i * sizeof(WORD), ordinal) ||
            ordinal >= directory.NumberOfFunctions ||
            !ReadAt(bytes, offsetOf(directory.AddressOfFunctions) + ordinal * sizeof(DWORD), function)) return false;
        const auto nameOffset = offsetOf(name);
        constexpr char abiName[] = "SnowDesktopHookEntryAbi";
        if (nameOffset >= bytes.size() || sizeof(abiName) > bytes.size() - nameOffset) continue;
        const auto text = reinterpret_cast<const char*>(bytes.data() + nameOffset);
        if (std::memcmp(text, abiName, sizeof(abiName)) != 0) continue;
        DWORD abi = 0;
        return ReadAt(bytes, offsetOf(function), abi) && abi == kAbi;
    }
    return false;
}

std::wstring Prepare(const std::filesystem::path& cacheRoot,
    const std::filesystem::path& entrySource, Payload payload,
    const std::filesystem::path& implementation)
{
    auto& publication = Publisher();
    std::lock_guard lock(publication.mutex);
    const WORD machine = payload == Payload::Dock32 ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_AMD64;
    if (!HasEntryAbi(entrySource, machine) || !std::filesystem::is_regular_file(implementation))
    { SetLastError(ERROR_BAD_EXE_FORMAT); return {}; }
    const auto directory = cacheRoot / L"entry-v1";
    const auto target = directory / entrySource.filename();
    if (Reparse(cacheRoot) || Reparse(directory) || Reparse(target))
    { SetLastError(ERROR_ACCESS_DENIED); return {}; }
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) { SetLastError(static_cast<DWORD>(error.value())); return {}; }
    Handle lease;
    const auto deadline = GetTickCount64() + 2000;
    do
    {
        lease.value = CreateFileW((directory / L".publish.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (lease.value != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_SHARING_VIOLATION) return {};
        Sleep(10);
    } while (GetTickCount64() < deadline);
    if (lease.value == INVALID_HANDLE_VALUE) { SetLastError(ERROR_BUSY); return {}; }
    const bool compatible = HasEntryAbi(target, machine);
    if (!compatible || deployment::InjectableRuntimeCache::Digest(target) != deployment::InjectableRuntimeCache::Digest(entrySource))
    {
        if (!CopyFileW(entrySource.c_str(), target.c_str(), FALSE) && !compatible) return {};
    }
    if (!HasEntryAbi(target, machine)) { SetLastError(ERROR_BAD_EXE_FORMAT); return {}; }
    if (!publication.Publish(payload, implementation)) return {};
    SetLastError(ERROR_SUCCESS);
    return target.wstring();
}
}

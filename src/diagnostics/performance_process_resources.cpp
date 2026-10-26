#include "performance_process_resources.h"
#include "performance_trace.h"

#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <limits>
#include <climits>
#include <unordered_map>

namespace snowdesktop::performance::process_resources
{
namespace
{
using Clock = std::chrono::steady_clock;
struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::uint64_t Time(FILETIME value) noexcept
{ return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime; }
bool ExtendedCountersSupported() noexcept
{
    // Conservative support boundary: older systems still get EX counters and
    // the independently queried resident-page classification.
    static const bool supported = [] {
        using Function = LONG(WINAPI*)(OSVERSIONINFOW*);
        Function function = nullptr;
        const auto address = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
        static_assert(sizeof(function) == sizeof(address));
        std::memcpy(&function, &address, sizeof(function));
        OSVERSIONINFOW version{}; version.dwOSVersionInfoSize = sizeof(version);
        return function && function(&version) >= 0 && version.dwBuildNumber >= 22631;
    }();
    return supported;
}
void Gauge(std::string_view module, std::string_view name, double value,
    std::wstring_view owner = {})
{ Value(module, name, owner, value); }
void RecordCounters(const Counters& counters, std::string_view module, std::wstring_view owner)
{
    const auto emit = [&](std::string_view name, auto value) {
        Gauge(module, name, static_cast<double>(value), owner);
    };
    emit("memory_sample_available", counters.memoryAvailable);
    emit("extended_memory_sample_available", counters.extendedAvailable);
    if (counters.memoryAvailable)
    {
        emit("private_commit_bytes", counters.privateCommit);
        emit("working_set_bytes", counters.workingSet);
        emit("page_fault_count", counters.pageFaults);
    }
    if (counters.extendedAvailable)
    {
        emit("private_working_set_bytes", counters.privateWorkingSet);
        emit("shared_commit_bytes", counters.sharedCommit);
    }
    if (counters.timesAvailable) emit("cpu_total_ms", static_cast<double>(counters.cpu100ns) / 10000.0);
    if (counters.handlesAvailable) emit("handle_count", counters.handles);
    emit("gdi_sample_available", counters.gdiAvailable);
    emit("user_sample_available", counters.userAvailable);
    if (counters.gdiAvailable) emit("gdi_object_count", counters.gdi);
    if (counters.userAvailable) emit("user_object_count", counters.user);
}
int TypeIndex(DWORD type) noexcept
{ return type == MEM_PRIVATE ? 0 : type == MEM_IMAGE ? 1 : type == MEM_MAPPED ? 2 : -1; }
void RecordGroup(std::string_view prefix, const MemoryGroup& group, bool includeReserved = true)
{
    const std::string name(prefix);
    const auto emit = [&](const char* suffix, std::uint64_t value) {
        Gauge("process.address_space.memory", name + suffix, static_cast<double>(value));
    };
    emit("_committed_region_bytes", group.committedRegionBytes);
    if (includeReserved) emit("_reserved_address_bytes", group.reservedAddressBytes);
    emit("_resident_bytes", group.residentBytes);
    emit("_private_resident_bytes", group.privateResidentBytes);
    emit("_shareable_resident_bytes", group.shareableResidentBytes);
}
bool EqualPath(std::wstring_view left, std::wstring_view right) noexcept
{
    if (left.starts_with(L"\\\\?\\")) left.remove_prefix(4);
    if (right.starts_with(L"\\\\?\\")) right.remove_prefix(4);
    return left.size() == right.size() && left.size() <= INT_MAX &&
        CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
            right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}
ProgramKind KnownProgram(std::wstring_view name) noexcept
{
    if (EqualPath(name, L"SnowDesktopSteamBridge.exe")) return ProgramKind::SteamBridge;
    if (EqualPath(name, L"SnowDesktopWorkshopManager.exe")) return ProgramKind::WorkshopManager;
    if (EqualPath(name, L"SnowDesktopLauncher.exe")) return ProgramKind::Launcher;
    if (EqualPath(name, L"snowwidget.exe")) return ProgramKind::WidgetTool;
    return ProgramKind::Excluded;
}
}

ProgramKind ClassifyFamilyProgram(std::wstring_view hostPath, std::wstring_view candidatePath) noexcept
{
    if (EqualPath(hostPath, candidatePath)) return ProgramKind::SameExecutable;
    const auto hostSeparator = hostPath.find_last_of(L"\\/");
    const auto childSeparator = candidatePath.find_last_of(L"\\/");
    if (hostSeparator == hostPath.npos || childSeparator == candidatePath.npos ||
        !EqualPath(hostPath.substr(0, hostSeparator), candidatePath.substr(0, childSeparator))) return ProgramKind::Excluded;
    return KnownProgram(candidatePath.substr(childSeparator + 1));
}

Counters ReadCounters(HANDLE process) noexcept
{
    Counters result;
    struct Extended { PROCESS_MEMORY_COUNTERS_EX base; SIZE_T privateWorkingSet; ULONG64 sharedCommit; } memory{};
    const bool extended = ExtendedCountersSupported();
    const DWORD bytes = extended ? static_cast<DWORD>(sizeof(memory)) : static_cast<DWORD>(sizeof(memory.base));
    memory.base.cb = bytes;
    result.memoryAvailable = K32GetProcessMemoryInfo(process,
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), bytes) != FALSE;
    if (!result.memoryAvailable && extended)
    {
        memory.base.cb = sizeof(memory.base);
        result.memoryAvailable = K32GetProcessMemoryInfo(process,
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory.base), sizeof(memory.base)) != FALSE;
    }
    else result.extendedAvailable = result.memoryAvailable && extended;
    if (result.memoryAvailable)
    {
        result.privateCommit = memory.base.PrivateUsage;
        result.workingSet = memory.base.WorkingSetSize;
        result.pageFaults = memory.base.PageFaultCount;
    }
    if (result.extendedAvailable)
    { result.privateWorkingSet = memory.privateWorkingSet; result.sharedCommit = memory.sharedCommit; }
    FILETIME created{}, exited{}, kernel{}, user{};
    result.timesAvailable = GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE;
    if (result.timesAvailable) { result.created = Time(created); result.cpu100ns = Time(kernel) + Time(user); }
    result.handlesAvailable = GetProcessHandleCount(process, &result.handles) != FALSE;
    SetLastError(ERROR_SUCCESS); result.gdi = GetGuiResources(process, GR_GDIOBJECTS);
    result.gdiAvailable = result.gdi != 0 || GetLastError() == ERROR_SUCCESS;
    SetLastError(ERROR_SUCCESS); result.user = GetGuiResources(process, GR_USEROBJECTS);
    result.userAvailable = result.user != 0 || GetLastError() == ERROR_SUCCESS;
    return result;
}
void RecordHostCounters()
{ if (Enabled()) RecordCounters(ReadCounters(GetCurrentProcess()), "process", {}); }

ImageGroup ClassifyImage(std::wstring_view path, bool mainImage) noexcept
{
    if (mainImage) return ImageGroup::Main;
    const auto separator = path.find_last_of(L"\\/");
    const auto base = path.substr(separator == path.npos ? 0 : separator + 1);
    // Fixed basename families classify image mappings only, not allocations
    // made by these DLLs. No paths or arbitrary module names become tracks.
    const auto is = [&](std::wstring_view name) { return EqualPath(base, name); };
    const auto starts = [&](std::wstring_view prefix) {
        return base.size() >= prefix.size() && EqualPath(base.substr(0, prefix.size()), prefix);
    };
    if (is(L"d2d1.dll") || is(L"dxgi.dll") || is(L"dcomp.dll") ||
        starts(L"d3d") || starts(L"nv") || starts(L"igd") || starts(L"amd")) return ImageGroup::Graphics;
    if (starts(L"dwrite") || is(L"usp10.dll") || is(L"fontsub.dll")) return ImageGroup::Text;
    if (is(L"shell32.dll") || is(L"shcore.dll") || is(L"shlwapi.dll") || is(L"windows.storage.dll") ||
        starts(L"windows.ui.") || starts(L"microsoft.ui.") || starts(L"windowsudk.shell")) return ImageGroup::ShellUi;
    if (is(L"ntdll.dll") || is(L"kernel32.dll") || is(L"kernelbase.dll") || is(L"combase.dll") ||
        is(L"ole32.dll") || is(L"oleaut32.dll") || is(L"ucrtbase.dll") || starts(L"msvcp") || starts(L"vcruntime")) return ImageGroup::Runtime;
    return ImageGroup::Other;
}

AddressSpace ReadAddressSpace(HANDLE process, const ScanLimits& limits)
{
    AddressSpace result;
    struct Workspace
    {
        std::array<PSAPI_WORKING_SET_EX_INFORMATION, 4096> pages{};
        std::array<unsigned char, 4096> types{}, images{};
    } workspace;
    result.workspaceBytes = sizeof(workspace);
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    const auto deadline = Clock::now() + limits.time;
    std::unordered_map<std::uintptr_t, ImageGroup> images;
    std::size_t pending = 0;
    const auto flush = [&] {
        if (!pending) return true;
        if (!K32QueryWorkingSetEx(process, workspace.pages.data(),
            static_cast<DWORD>(pending * sizeof(workspace.pages[0]))))
        { result.error = GetLastError(); return false; }
        result.queriedPages += pending;
        for (std::size_t index = 0; index < pending; ++index)
        {
            const auto attributes = workspace.pages[index].VirtualAttributes;
            if (!attributes.Valid) continue;
            const auto add = [&](MemoryGroup& group) {
                group.residentBytes += system.dwPageSize;
                if (attributes.Shared) group.shareableResidentBytes += system.dwPageSize;
                else group.privateResidentBytes += system.dwPageSize;
            };
            add(result.types[workspace.types[index]]);
            if (workspace.types[index] == 1) add(result.images[workspace.images[index]]);
        }
        pending = 0;
        return true;
    };
    std::uintptr_t cursor = 0;
    const auto maximum = reinterpret_cast<std::uintptr_t>(system.lpMaximumApplicationAddress);
    while (cursor <= maximum)
    {
        if (result.regions >= limits.regions || Clock::now() >= deadline)
        { result.error = ERROR_TIMEOUT; return result; }
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQueryEx(process, reinterpret_cast<void*>(cursor), &memory, sizeof(memory)))
        { result.error = GetLastError(); return result; }
        ++result.regions;
        const auto start = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (!memory.RegionSize || memory.RegionSize > std::numeric_limits<std::uintptr_t>::max() - start)
        { result.error = ERROR_ARITHMETIC_OVERFLOW; return result; }
        const auto end = start + memory.RegionSize;
        const int type = TypeIndex(memory.Type);
        if (type >= 0)
        {
            auto& group = result.types[static_cast<std::size_t>(type)];
            if (memory.State == MEM_RESERVE) group.reservedAddressBytes += memory.RegionSize;
            if (memory.State == MEM_COMMIT)
            {
                ImageGroup image = ImageGroup::Other;
                if (type == 1)
                {
                    const auto allocation = reinterpret_cast<std::uintptr_t>(memory.AllocationBase);
                    const auto found = images.find(allocation);
                    if (found != images.end()) image = found->second;
                    else
                    {
                        std::array<wchar_t, 2048> path{};
                        const DWORD size = K32GetMappedFileNameW(process, memory.AllocationBase,
                            path.data(), static_cast<DWORD>(path.size()));
                        if (!size || size == path.size()) ++result.unnamedImages;
                        else image = ClassifyImage(std::wstring_view(path.data(), size),
                            memory.AllocationBase == GetModuleHandleW(nullptr));
                        images.emplace(allocation, image);
                    }
                    result.images[static_cast<std::size_t>(image)].committedRegionBytes += memory.RegionSize;
                }
                group.committedRegionBytes += memory.RegionSize;
                for (auto page = start; page < end; page += system.dwPageSize)
                {
                    if (result.queriedPages + pending >= limits.pages)
                    { result.error = ERROR_BUFFER_OVERFLOW; return result; }
                    workspace.pages[pending].VirtualAddress = reinterpret_cast<void*>(page);
                    workspace.types[pending] = static_cast<unsigned char>(type);
                    workspace.images[pending] = static_cast<unsigned char>(image);
                    if (++pending == workspace.pages.size())
                    {
                        if (!flush()) return result;
                        if (Clock::now() >= deadline) { result.error = ERROR_TIMEOUT; return result; }
                    }
                }
            }
        }
        if (end <= cursor) { result.error = ERROR_ARITHMETIC_OVERFLOW; return result; }
        cursor = end;
    }
    result.complete = flush();
    result.imageAllocations = images.size();
    return result;
}

Family ReadFamily()
{
    Family result;
    FILETIME snapshotTime{}; GetSystemTimePreciseAsFileTime(&snapshotTime);
    const auto latestCreation = Time(snapshotTime);
    Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (snapshot.value == INVALID_HANDLE_VALUE) return result;
    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length == executable.size()) return result;
    const std::wstring_view path(executable.data(), length);
    const auto basename = path.substr(path.find_last_of(L"\\/") + 1);
    std::vector<PROCESSENTRY32W> entries;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.value, &entry)) return result;
    do { if (entries.size() >= 65536) return result; entries.push_back(entry); }
    while (Process32NextW(snapshot.value, &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES) return result;
    const auto root = GetCurrentProcessId();
    const auto rootEntry = std::find_if(entries.begin(), entries.end(), [&](const auto& item) { return item.th32ProcessID == root; });
    auto counters = ReadCounters(GetCurrentProcess());
    if (rootEntry == entries.end() || !counters.timesAvailable || !counters.memoryAvailable) return result;
    result.members.push_back({root, rootEntry->th32ParentProcessID, rootEntry->cntThreads, counters});
    std::unordered_map<DWORD, std::uint64_t> trusted{{root, counters.created}};
    for (std::size_t member = 0; member < result.members.size(); ++member)
    {
        const auto parent = result.members[member];
        for (const auto& candidate : entries)
        {
            if (candidate.th32ParentProcessID != parent.pid || trusted.contains(candidate.th32ProcessID) ||
                (!EqualPath(candidate.szExeFile, basename) && KnownProgram(candidate.szExeFile) == ProgramKind::Excluded)) continue;
            if (result.members.size() >= 64) { ++result.unavailable; return result; }
            Handle process{OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, candidate.th32ProcessID)};
            if (!process.value) { ++result.unavailable; continue; }
            std::array<wchar_t, 32768> childPath{}; DWORD size = static_cast<DWORD>(childPath.size());
            if (!QueryFullProcessImageNameW(process.value, 0, childPath.data(), &size)) { ++result.unavailable; continue; }
            const auto program = ClassifyFamilyProgram(path, std::wstring_view(childPath.data(), size));
            if (program == ProgramKind::Excluded) continue;
            const auto child = ReadCounters(process.value);
            DWORD exitCode = 0;
            // Reject a PID reused after the process-list snapshot: the opened
            // handle must describe a process already alive at snapshot start.
            if (!child.timesAvailable || !child.memoryAvailable || child.created < parent.counters.created ||
                child.created > latestCreation ||
                !GetExitCodeProcess(process.value, &exitCode) || exitCode != STILL_ACTIVE)
            { ++result.unavailable; continue; }
            trusted.emplace(candidate.th32ProcessID, child.created);
            result.members.push_back({candidate.th32ProcessID, parent.pid, candidate.cntThreads, child, program});
        }
    }
    result.complete = result.unavailable == 0;
    return result;
}

void Sampler::Sample(bool includeDescendants, bool force)
{
    if (!Enabled()) return;
    const auto now = Clock::now();
    if (!force && now < next_) return;
    next_ = now + std::chrono::seconds(10);
    {
        Scope scope("profiler", "sample.address_space");
        const auto memory = ReadAddressSpace(GetCurrentProcess());
        const auto emit = [&](std::string_view name, auto value) {
            Gauge("process.address_space.memory", name, static_cast<double>(value));
        };
        emit("sample_available", memory.complete);
        emit("sample_error", memory.error);
        emit("scanned_regions", memory.regions);
        emit("queried_pages", memory.queriedPages);
        emit("sampler_workspace_bytes_estimate", memory.workspaceBytes);
        if (memory.complete)
        {
            constexpr std::array names{"private", "image", "mapped"};
            constexpr std::array imageNames{"main_image", "graphics_image", "text_image", "shell_ui_image", "runtime_image", "other_image"};
            for (std::size_t index = 0; index < names.size(); ++index) RecordGroup(names[index], memory.types[index]);
            for (std::size_t index = 0; index < imageNames.size(); ++index) RecordGroup(imageNames[index], memory.images[index], false);
            emit("image_allocations", memory.imageAllocations);
            emit("unnamed_image_allocations", memory.unnamedImages);
        }
    }
    if (!includeDescendants) return;
    Scope scope("profiler", "sample.process_family");
    const auto family = ReadFamily();
    Gauge("process.family.memory", "sample_complete", family.complete);
    Gauge("process.family.memory", "unavailable_candidates", static_cast<double>(family.unavailable));
    Gauge("process.family.memory", "observed_processes", static_cast<double>(family.members.size()));
    if (family.members.empty()) return;
    std::uint64_t commit = 0, workingSet = 0, privateWorkingSet = 0, threads = 0, handles = 0;
    bool havePrivateWorkingSet = true, haveHandles = true;
    std::vector<std::wstring> owners;
    for (const auto& member : family.members)
    {
        commit += member.counters.privateCommit; workingSet += member.counters.workingSet;
        privateWorkingSet += member.counters.privateWorkingSet; threads += member.threads;
        handles += member.counters.handles;
        havePrivateWorkingSet = havePrivateWorkingSet && member.counters.extendedAvailable;
        haveHandles = haveHandles && member.counters.handlesAvailable;
        if (member.pid == GetCurrentProcessId())
        { Gauge("process", "thread_count", member.threads); continue; }
        auto owner = std::to_wstring(member.pid) + L":" + std::to_wstring(member.counters.created);
        Gauge("process.aux.memory", "present", 1, owner);
        Gauge("process.aux.memory", "program_kind", static_cast<int>(member.program), owner);
        Gauge("process.aux.memory", "thread_count", member.threads, owner);
        RecordCounters(member.counters, "process.aux.memory", owner);
        owners.push_back(std::move(owner));
    }
    if (family.complete)
        for (const auto& previous : previousOwners_)
            if (std::find(owners.begin(), owners.end(), previous) == owners.end())
                Gauge("process.aux.memory", "present", 0, previous);
    if (family.complete) previousOwners_ = std::move(owners);
    else for (auto& owner : owners)
        if (std::find(previousOwners_.begin(), previousOwners_.end(), owner) == previousOwners_.end())
            previousOwners_.push_back(std::move(owner));
    Gauge("process.family.memory", "observed_private_commit_sum_bytes", static_cast<double>(commit));
    Gauge("process.family.memory", "observed_working_set_sum_bytes", static_cast<double>(workingSet));
    Gauge("process.family.memory", "observed_thread_count", static_cast<double>(threads));
    Gauge("process.family.memory", "private_working_set_available", havePrivateWorkingSet);
    if (havePrivateWorkingSet) Gauge("process.family.memory", "observed_private_working_set_sum_bytes", static_cast<double>(privateWorkingSet));
    if (haveHandles) Gauge("process.family.memory", "observed_handle_count", static_cast<double>(handles));
}
}

#include "diagnostics/performance_process_resources.h"
#include "diagnostics/performance_capture.h"
#include "diagnostics/performance_trace.h"
#include "app/render/app_resource_statistics.h"
#include "common/json_value.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

void AssertNativeTraceSql(const std::filesystem::path&, std::string_view);

namespace
{
namespace stats = snowdesktop::performance::process_resources;
namespace perf = snowdesktop::performance;
void Check(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }
struct Child
{
    PROCESS_INFORMATION process{};
    explicit Child(const std::filesystem::path& path)
    {
        auto command = L"\"" + path.wstring() + L"\" --resource-statistics-child";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        Check(CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE,
            "launch owned process-resource fixture");
    }
    ~Child()
    {
        if (WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0)
        { TerminateProcess(process.hProcess, 0); WaitForSingleObject(process.hProcess, 5000); }
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
    }
};
struct Allocation
{
    void* memory = VirtualAlloc(nullptr, 2 * 1024 * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ~Allocation() { if (memory) VirtualFree(memory, 0, MEM_RELEASE); }
};
perf::CaptureSnapshot Wait(std::string_view session, bool terminal)
{
    const auto deadline = GetTickCount64() + 15000;
    while (GetTickCount64() < deadline)
    {
        const auto value = perf::Snapshot(session);
        if (value.phase == perf::CapturePhase::Failed) throw std::runtime_error(value.error);
        if (value.phase == (terminal ? perf::CapturePhase::Completed : perf::CapturePhase::Recording)) return value;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("resource-statistics capture deadline");
}
}

int RunProcessResourceChild()
{ Sleep(15000); return 0; }

void RunProcessResourceStatisticsTests()
{
    struct Cleanup { ~Cleanup() { perf::Shutdown(); } } cleanup;
    Check(!perf::Enabled(), "whole-process statistics remain opt-in");
    stats::Sampler disabled;
    disabled.Sample(true, true);
    Check(!stats::ReadCounters(nullptr).memoryAvailable, "invalid process does not become zero memory");
    Check(stats::ClassifyFamilyProgram(L"D:\\App\\SnowDesktop.exe", L"d:\\APP\\SnowDesktopWorkshopManager.exe") ==
            stats::ProgramKind::WorkshopManager &&
        stats::ClassifyFamilyProgram(L"D:\\App\\SnowDesktop.exe", L"D:\\App\\SnowDesktopSteamBridge.exe") ==
            stats::ProgramKind::SteamBridge &&
        stats::ClassifyFamilyProgram(L"D:\\App\\SnowDesktop.exe", L"D:\\Other\\SnowDesktopSteamBridge.exe") ==
            stats::ProgramKind::Excluded &&
        stats::ClassifyFamilyProgram(L"D:\\App\\SnowDesktop.exe", L"D:\\App\\foreign.exe") == stats::ProgramKind::Excluded,
        "known companion programs require the exact installation directory; foreign names remain excluded");
    const auto current = stats::ReadCounters(GetCurrentProcess());
    Check(current.memoryAvailable && current.workingSet && current.timesAvailable && current.created,
        "production process counters observe live process");
    if (current.extendedAvailable)
        Check(current.privateWorkingSet <= current.workingSet, "private working set is a subset of working set");
    Check(stats::ClassifyImage(L"C:\\Windows\\DWRITE.DLL", false) == stats::ImageGroup::Text &&
        stats::ClassifyImage(L"C:\\Driver\\nvwgf2umx.dll", false) == stats::ImageGroup::Graphics &&
        stats::ClassifyImage(L"x.dll", true) == stats::ImageGroup::Main &&
        stats::ClassifyImage(L"unrecognized.dll", false) == stats::ImageGroup::Other,
        "image grouping is finite, case-insensitive and preserves unknowns");
    Allocation allocation;
    Check(allocation.memory != nullptr, "reserve controlled private resource pages");
    auto* bytes = static_cast<volatile unsigned char*>(allocation.memory);
    for (std::size_t offset = 0; offset < 2 * 1024 * 1024; offset += 4096) bytes[offset] = 1;
    stats::ScanLimits limits; limits.time = std::chrono::seconds(3);
    const auto address = stats::ReadAddressSpace(GetCurrentProcess(), limits);
    Check(address.complete && address.types[0].committedRegionBytes >= 2 * 1024 * 1024 &&
        address.types[0].privateResidentBytes >= 2 * 1024 * 1024,
        "real allocated/touched private pages are observed without trimming or reading pixels");
    for (const auto& group : address.types)
        Check(group.residentBytes == group.privateResidentBytes + group.shareableResidentBytes,
            "resident pages partition into private and shareable rather than double counting");
    std::uint64_t imageBytes = 0;
    for (const auto& group : address.images) imageBytes += group.residentBytes;
    Check(imageBytes == address.types[1].residentBytes && address.images[0].residentBytes > 0,
        "image groups partition the actual image working set");
    limits.regions = 0;
    Check(!stats::ReadAddressSpace(GetCurrentProcess(), limits).complete,
        "a bounded incomplete address scan is never a complete zero sample");
    limits.regions = 65536; limits.pages = 0;
    Check(!stats::ReadAddressSpace(GetCurrentProcess(), limits).complete,
        "page-query budget exhaustion remains unavailable");

    std::array<wchar_t, 32768> executable{};
    Check(GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())) != 0,
        "locate resource fixture executable");
    const auto root = std::filesystem::temp_directory_path() /
        (L"SnowDesktopResourceStats-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root / L"other-installation");
    const auto copy = root / L"other-installation" / std::filesystem::path(executable.data()).filename();
    std::filesystem::copy_file(executable.data(), copy);
    Child owned(executable.data()), foreign(copy);
    const auto family = stats::ReadFamily();
    Check(family.complete && std::any_of(family.members.begin(), family.members.end(), [&](const auto& member) {
        return member.pid == owned.process.dwProcessId && member.counters.created >= current.created;
    }), "same-executable owned descendant carries a validated live identity");
    Check(std::none_of(family.members.begin(), family.members.end(), [&](const auto& member) {
        return member.pid == foreign.process.dwProcessId;
    }), "same-name process in another installation is excluded from software totals");

    perf::CaptureOptions options;
    options.session = options.requestId = "whole-process-statistics";
    options.output = root / L"capture" / L"trace.pftrace";
    options.seconds = 2; options.preset = perf::CapturePreset::Resource; options.scopeCpu = true;
    std::string error;
    Check(perf::Start(options, error), "start real SDK capture for process resource gauges");
    Wait(options.session, false);
    std::vector<int> storage{1, 2}; storage.reserve(8);
    snowdesktop::app_resource_statistics::Payload payload;
    payload.VectorStorage(storage); payload.Text(std::wstring(L"A中")); payload.Text(std::string("xyz"));
    payload.Record("stat_fixture");
    Check(storage == std::vector<int>({1, 2}) && storage.capacity() == 8,
        "resource observation preserves model contents and reserved storage");
    const auto completed = Wait(options.session, true);
    Check(!perf::Enabled(), "process collectors stop with their owning capture");
    std::ifstream input(completed.manifest);
    const std::string json((std::istreambuf_iterator<char>(input)), {});
    JsonValue manifest;
    Check(ParseJson(json, manifest, &error) && manifest.Find("processResourceStatistics") &&
        !manifest.Find("processResourceStatistics")->Find("heapAttribution")->boolean,
        "manifest states OS-only descendant and unattributed-heap coverage");
    AssertNativeTraceSql(completed.trace,
        "SELECT ((SELECT COUNT(*) FROM counter c JOIN track t ON t.id=c.track_id WHERE t.name='process|private_commit_bytes|0000000000000000')>0"
        " AND (SELECT COUNT(*) FROM counter c JOIN track t ON t.id=c.track_id WHERE t.name='process.address_space.memory|sample_available|0000000000000000')>0"
        " AND (SELECT MAX(c.value) FROM counter c JOIN track t ON t.id=c.track_id WHERE t.name='process.family.memory|observed_processes|0000000000000000')>=2"
        " AND (SELECT COUNT(DISTINCT t.name) FROM counter c JOIN track t ON t.id=c.track_id WHERE t.name GLOB 'process.aux.memory|present|*' AND c.value=1)>0"
        " AND (SELECT MAX(c.value) FROM counter c JOIN track t ON t.id=c.track_id WHERE t.name='host.model.memory|stat_fixture_selected_text_payload_bytes|0000000000000000')=7"
        " AND (SELECT MAX(c.value) FROM counter c JOIN track t ON t.id=c.track_id WHERE t.name='host.model.memory|stat_fixture_vector_storage_bytes|0000000000000000')=32"
        " AND (SELECT COUNT(*) FROM slice WHERE name='sample.address_space' AND thread_dur IS NOT NULL)>0) AS passed;");
    std::cout << "Whole-process resource fixtures: " << root.string() << '\n';
}

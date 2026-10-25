#include "performance_capture.h"
#include "performance_trace.h"
#include "performance_trace_control.h"
#include "performance_control_pipe.h"
#include "perfetto_session.h"
#include "perfetto_categories.h"
#include "performance_artifacts.h"
#include "performance_analysis.h"
#include "performance_compositor_clock.h"
#include "performance_environment.h"
#include "common/json_value.h"

#include <bcrypt.h>
#include <psapi.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iomanip>
#include <locale>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#ifndef SNOWDESKTOP_VERSION
#define SNOWDESKTOP_VERSION "test"
#endif

namespace snowdesktop::performance
{
namespace
{
using Clock = std::chrono::steady_clock;
std::uint64_t FileTimeValue(FILETIME value) noexcept
{ return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime; }
std::string Utf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const auto length = static_cast<int>(value.size());
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("invalidUtf16");
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, result.data(), size, nullptr, nullptr);
    return result;
}
std::wstring Wide(std::string_view value)
{
    if (value.empty()) return {};
    const auto length = static_cast<int>(value.size());
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, nullptr, 0);
    if (size <= 0) throw std::runtime_error("invalidUtf8");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, result.data(), size);
    return result;
}
std::string Quote(std::string_view value)
{
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char ch : value)
    {
        if (ch == '\\' || ch == '"') { out.push_back('\\'); out.push_back(static_cast<char>(ch)); }
        else if (ch < 32) { out += "\\u00"; out.push_back(hex[ch >> 4]); out.push_back(hex[ch & 15]); }
        else out.push_back(static_cast<char>(ch));
    }
    out.push_back('"');
    return out;
}
bool ValidId(std::string_view value) noexcept
{ return !value.empty() && value.size() <= 64 && value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") == value.npos; }
const char* PhaseName(CapturePhase phase) noexcept
{
    switch (phase)
    {
    case CapturePhase::Idle: return "idle"; case CapturePhase::Starting: return "starting";
    case CapturePhase::Recording: return "recording"; case CapturePhase::Stopping: return "stopping";
    case CapturePhase::Finalizing: return "finalizing"; case CapturePhase::Completed: return "completed";
    case CapturePhase::Failed: return "failed";
    }
    return "failed";
}
bool Running(CapturePhase phase) noexcept
{ return phase == CapturePhase::Starting || phase == CapturePhase::Recording || phase == CapturePhase::Stopping || phase == CapturePhase::Finalizing; }
const char* PresetName(CapturePreset preset) noexcept
{
    switch (preset) { case CapturePreset::Resource: return "idle"; case CapturePreset::Interaction: return "interaction"; case CapturePreset::Widget: return "widget-detail"; }
    return "invalid";
}
std::vector<std::string> Categories(CapturePreset preset)
{
    std::vector<std::string> result{ "snow.resource", "snow.lua.callback", "snow.task", "snow.frame", "snow.profiler" };
    if (preset != CapturePreset::Resource)
        for (const auto* item : { "snow.input", "snow.dock", "snow.popup", "snow.layout", "snow.render", "snow.icon" }) result.emplace_back(item);
    if (preset == CapturePreset::Widget)
        for (const auto* item : { "snow.lua.detail", "snow.cache.detail", "snow.task.detail" }) result.emplace_back(item);
    return result;
}
std::string ConfigHash(const CaptureOptions& options)
{
    std::string canonical;
    const auto add = [&](std::string_view part) { canonical += std::to_string(part.size()); canonical += ':'; canonical += part; };
    add("snowdesktop.capture.config.v2"); add(PresetName(options.preset)); add(std::to_string(options.seconds));
    add(std::to_string(options.bufferKiB)); add(std::to_string(options.maximumFileBytes)); add(options.scopeCpu ? "cpu" : "wall");
    add(std::to_string(options.selectedInstances.size()));
    for (const auto& owner : options.selectedInstances) add(Utf8(owner));
    std::array<unsigned char, 32> digest{};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("configFingerprintFailed");
    const auto status = BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(canonical.data()),
        static_cast<ULONG>(canonical.size()), digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("configFingerprintFailed");
    std::ostringstream out;
    for (const auto byte : digest) out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    return out.str();
}
bool SameRequest(const CaptureOptions& a, const CaptureOptions& b)
{
    return a.session == b.session && a.requestId == b.requestId && a.output == b.output &&
        a.seconds == b.seconds && a.bufferKiB == b.bufferKiB && a.maximumFileBytes == b.maximumFileBytes &&
        a.preset == b.preset && a.scopeCpu == b.scopeCpu && a.selectedInstances == b.selectedInstances;
}
struct File
{
    HANDLE value = INVALID_HANDLE_VALUE;
    ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::string HashFile(const std::filesystem::path& path)
{
    File file{ CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr) };
    if (file.value == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    const auto finish = [&] { if (hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0); };
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) { finish(); return {}; }
    std::array<unsigned char, 65536> bytes{};
    DWORD read = 0;
    bool success = true;
    while (true)
    {
        if (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) { success = false; break; }
        if (!read) break;
        if (BCryptHashData(hash, bytes.data(), read, 0) < 0) { success = false; break; }
    }
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) success = false;
    finish();
    if (!success) return {};
    std::ostringstream out;
    for (const auto byte : digest) out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    return out.str();
}
std::string ExecutableHash()
{
    wchar_t path[32768]{};
    const auto length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return length && length < std::size(path) ? HashFile(path) : std::string{};
}
bool SafeDirectory(const std::filesystem::path& path, bool allowMissing)
{
    if (!IsLocalCapturePath(path)) return false;
    auto current = path;
    while (!current.empty())
    {
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const auto error = GetLastError();
            if (!allowMissing || (error != ERROR_PATH_NOT_FOUND && error != ERROR_FILE_NOT_FOUND)) return false;
        }
        else if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return true;
}
std::uint64_t StorageRemaining(const std::filesystem::path& root)
{
    constexpr std::uint64_t quota = 1024ULL * 1024 * 1024;
    std::uint64_t bytes = 0;
    unsigned entries = 0, completed = 0;
    for (const auto& item : std::filesystem::recursive_directory_iterator(root))
    {
        if (++entries > 10000) throw std::runtime_error("captureStorageEntryLimit");
        const auto attributes = GetFileAttributesW(item.path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("captureStoragePathRejected");
        if (!item.is_regular_file()) continue;
        const auto size = item.file_size();
        if (size > quota - bytes) throw std::runtime_error("captureStorageQuota");
        bytes += size;
        if (item.path().filename() == L"session.json")
        {
            if (size > 65536) throw std::runtime_error("captureManifestLimit");
            std::ifstream input(item.path(), std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(input)), {});
            JsonValue manifest; std::string error;
            if (!ParseJson(text, manifest, &error)) throw std::runtime_error("captureStorageManifestInvalid");
            const auto* complete = manifest.Find("completion");
            if (complete && complete->IsBoolean() && complete->boolean) ++completed;
        }
    }
    if (completed >= 20 || bytes + 65536 >= quota) throw std::runtime_error("captureStorageQuota");
    return quota - bytes - 65536; // Reserve bounded manifest/metadata space.
}
struct Record
{
    CaptureOptions options;
    std::filesystem::path managedRoot;
    std::function<void(bool)> gpuDemand;
    CaptureSnapshot snapshot;
    Clock::time_point started{};
    std::string executableSha256, traceSha256;
    std::string environment = "null";
    std::vector<std::uint64_t> selectedOwners;
    bool finalWidgetSample = false;
    std::uint64_t firstTraceNs = 0, lastTraceNs = 0;
    TraceQuality quality;
};
struct State
{
    std::mutex mutex, startMutex;
    std::condition_variable wake;
    std::thread worker;
    std::atomic<bool> stopRequested{ false };
    std::atomic<bool> analysisCancel{ false };
    std::atomic<std::uint64_t> widgetSamples{0};
    std::atomic<HWND> window{ nullptr };
    std::shared_ptr<Record> current, pending, pendingAnalysis;
    std::deque<std::shared_ptr<Record>> history;
    std::filesystem::path root;
    std::function<void(bool)> gpuDemand;
    std::uint64_t generation = 0;
    bool workerExit = false, analysisActive = false;
    ~State() { { std::lock_guard lock(mutex); workerExit = true; stopRequested.store(true); analysisCancel.store(true); } wake.notify_all(); if (worker.joinable()) worker.join(); }
};
State state;
UINT SampleMessage() noexcept
{ static const auto message = RegisterWindowMessageW(L"FreeFallingSnow.SnowDesktop.Performance.Sample.v3"); return message; }
UINT ControlMessage() noexcept
{ static const auto message = RegisterWindowMessageW(ControlMessageName); return message; }
UINT StateMessage() noexcept
{ static const auto message = RegisterWindowMessageW(L"FreeFallingSnow.SnowDesktop.Performance.State.v3"); return message; }
void NotifyState() noexcept
{ const auto window = state.window.load(); if (window && StateMessage()) PostMessageW(window, StateMessage(), 0, 0); }
void SetPhase(const std::shared_ptr<Record>& record, CapturePhase phase)
{ { std::lock_guard lock(state.mutex); record->snapshot.phase = phase; } NotifyState(); }

void SampleProcess()
{
    Scope scope("profiler", "sample.process");
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        Value("process", "cpu_total_ms", {},
            static_cast<double>(FileTimeValue(kernel) + FileTimeValue(user)) / 10000.0);
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
    {
        Value("process", "private_commit_bytes", {}, static_cast<double>(memory.PrivateUsage));
        Value("process", "working_set_bytes", {}, static_cast<double>(memory.WorkingSetSize));
        Value("process", "page_fault_count", {}, memory.PageFaultCount);
    }
    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles))
        Value("process", "handle_count", {}, handles);
}



void SampleIo()
{
    IO_COUNTERS counters{};
    if (!GetProcessIoCounters(GetCurrentProcess(), &counters)) return;
    Value("process", "io_read_bytes", {}, static_cast<double>(counters.ReadTransferCount));
    Value("process", "io_write_bytes", {}, static_cast<double>(counters.WriteTransferCount));
    Value("process", "io_other_bytes", {}, static_cast<double>(counters.OtherTransferCount));
    Value("process", "io_read_operations", {}, static_cast<double>(counters.ReadOperationCount));
    Value("process", "io_write_operations", {}, static_cast<double>(counters.WriteOperationCount));
}
bool RequestWidgetSample(std::uint64_t generation)
{
    const auto window = state.window.load();
    return window && SampleMessage() && PostMessageW(window, SampleMessage(), static_cast<WPARAM>(generation), 0);
}
std::string Manifest(const Record& record, bool complete)
{
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    const auto allProcessors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    std::ostringstream out; out.imbue(std::locale::classic()); out << std::setprecision(12);
    out << "{\"schemaVersion\":2,\"probeSchemaVersion\":2,\"format\":\"perfetto\",\"sdkVersion\":\"v58.2\",\"sessionId\":" << Quote(record.snapshot.session)
        << ",\"generation\":" << record.snapshot.generation << ",\"requestId\":" << Quote(record.snapshot.requestId)
        << ",\"state\":" << Quote(complete ? "completed" : "failed") << ",\"completion\":" << (complete ? "true" : "false")
        << ",\"errorCode\":" << Quote(record.snapshot.error) << ",\"hostVersion\":" << Quote(SNOWDESKTOP_VERSION)
        << ",\"managedStorage\":" << (record.managedRoot.empty() ? "false" : "true")
        << ",\"finalWidgetSampleConfirmed\":" << (record.finalWidgetSample ? "true" : "false")
        << ",\"processId\":" << GetCurrentProcessId() << ",\"processRole\":\"host\",\"sourceCommit\":null,\"executableSha256\":"
        << (record.executableSha256.empty() ? "null" : Quote(record.executableSha256))
        << ",\"traceSha256\":" << (record.traceSha256.empty() ? "null" : Quote(record.traceSha256))
        << ",\"logicalProcessors\":" << (allProcessors ? allProcessors : system.dwNumberOfProcessors) << ",\"preset\":" << Quote(PresetName(record.options.preset))
        << ",\"configHash\":" << Quote(record.snapshot.configHash) << ",\"scopeCpuEnabled\":" << (record.options.scopeCpu ? "true" : "false")
        << ",\"environment\":" << record.environment << ",\"selectedInstances\":[";
    for (std::size_t i = 0; i < record.selectedOwners.size(); ++i)
    {
        if (i) out << ',';
        std::ostringstream owner; owner << std::hex << std::setw(16) << std::setfill('0') << record.selectedOwners[i];
        out << "{\"ordinal\":" << i + 1 << ",\"owner\":" << Quote(owner.str()) << '}';
    }
    out << ']'
        << ",\"threadCpuSource\":" << (record.options.scopeCpu ? "\"perfetto.thread_time\"" : "null")
        << ",\"requestedSeconds\":" << record.options.seconds << ",\"bufferKiB\":" << record.options.bufferKiB
        << ",\"firstTraceNs\":" << Quote(std::to_string(record.firstTraceNs)) << ",\"lastTraceNs\":" << Quote(std::to_string(record.lastTraceNs))
        << ",\"durationMs\":" << record.snapshot.elapsedMs << ",\"traceFile\":" << Quote(Utf8(record.snapshot.trace.filename().wstring()))
        << ",\"coverage\":\"pending-analysis\",\"participants\":[{\"role\":\"host\",\"pid\":" << GetCurrentProcessId()
        << ",\"included\":true},{\"role\":\"settings\",\"included\":false}],\"probeQuality\":{\"droppedOwners\":" << record.quality.droppedOwners
        << ",\"droppedCounters\":" << record.quality.droppedCounters << ",\"droppedLinks\":" << record.quality.droppedLinks
        << ",\"droppedTracks\":" << record.quality.droppedTracks
        << ",\"probeErrors\":" << record.quality.probeErrors << "}}";
    return out.str();
}
void Worker(std::shared_ptr<Record> record) noexcept
{
    std::string error;
    bool complete = false;
    bool gpuDemandStarted = false;
    File manifest, storageLease;
    std::filesystem::path partialManifest;
    try
    {
        if (record->snapshot.trace.empty())
        {
            auto root = record->managedRoot;
            if (root.empty())
            {
                PWSTR local = nullptr;
                if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &local)))
                    throw std::runtime_error("captureUserDirectoryUnavailable");
                try { root = std::filesystem::path(local) / L"SnowDesktop/diagnostics/performance"; }
                catch (...) { CoTaskMemFree(local); throw; }
                CoTaskMemFree(local);
                record->managedRoot = root;
            }
            const auto trace = root / Wide(record->options.session) / L"trace.pftrace";
            std::lock_guard lock(state.mutex);
            record->snapshot.trace = trace;
            record->snapshot.manifest = trace.parent_path() / L"session.json";
        }
        partialManifest = std::filesystem::path(record->snapshot.manifest.wstring() + L".partial");
        const auto directory = record->snapshot.trace.parent_path();
        if (!SafeDirectory(directory, true)) throw std::runtime_error("capturePathRejected");
        std::filesystem::create_directories(directory);
        if (!SafeDirectory(directory, false)) throw std::runtime_error("capturePathRejected");
        std::uint64_t fileLimit = record->options.maximumFileBytes;
        const auto& managedRoot = record->managedRoot;
        if (!managedRoot.empty() && directory.parent_path() == managedRoot)
        {
            const auto leasePath = managedRoot / L".capture-storage.lock";
            const auto leaseAttributes = GetFileAttributesW(leasePath.c_str());
            if (leaseAttributes != INVALID_FILE_ATTRIBUTES && (leaseAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                throw std::runtime_error("captureStoragePathRejected");
            storageLease.value = CreateFileW(leasePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (storageLease.value == INVALID_HANDLE_VALUE) throw std::runtime_error("captureStorageBusy");
            LARGE_INTEGER leaseSize{};
            if (!GetFileSizeEx(storageLease.value, &leaseSize) || leaseSize.QuadPart != 0) throw std::runtime_error("captureStorageLockInvalid");
            fileLimit = std::min(fileLimit, StorageRemaining(managedRoot));
        }
        if (std::filesystem::exists(record->snapshot.manifest)) throw std::runtime_error("manifestAlreadyExists");
        manifest.value = CreateFileW(partialManifest.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (manifest.value == INVALID_HANDLE_VALUE) throw std::runtime_error("cannotReserveManifest");
        record->executableSha256 = ExecutableHash();
        record->environment = CaptureEnvironmentJson();
        PerfettoSession session;
        PerfettoSessionOptions native;
        native.output = record->snapshot.trace;
        native.bufferKiB = record->options.bufferKiB;
        native.maximumFileBytes = fileLimit;
        native.threadCpu = record->options.scopeCpu;
        native.categories = Categories(record->options.preset);
        if (!session.Start(native, error)) throw std::runtime_error(error);
        std::uint64_t salt = 0;
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&salt), sizeof(salt), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            throw std::runtime_error("captureIdentityRandomFailed");
        ActivateTrace(record->snapshot.generation, salt, record->options.selectedInstances);
        for (const auto& instance : record->options.selectedInstances) record->selectedOwners.push_back(OwnerId(instance));
        record->firstTraceNs = native_trace::TrackEvent::GetTraceTimeNs();
        {
            std::lock_guard lock(state.mutex);
            record->started = Clock::now();
            record->snapshot.phase = CapturePhase::Recording;
        }
        NotifyState();
        { Scope marker("profiler", "capture.start"); }
        if (record->gpuDemand) { gpuDemandStarted = true; record->gpuDemand(true); }
        SampleProcess(); SampleIo(); RequestWidgetSample(record->snapshot.generation);
        const auto deadline = record->started + std::chrono::seconds(record->options.seconds);
        {
            std::unique_ptr<CompositorClock> compositor;
            if (record->options.preset != CapturePreset::Resource) compositor = std::make_unique<CompositorClock>();
            while (!state.stopRequested.load() && Clock::now() < deadline)
            {
                std::unique_lock lock(state.mutex);
                state.wake.wait_until(lock, std::min(deadline, Clock::now() + std::chrono::seconds(1)), [] { return state.stopRequested.load(); });
                lock.unlock();
                SampleProcess(); SampleIo();
                if (compositor) compositor->Sample();
                RequestWidgetSample(record->snapshot.generation);
            }
        }
        const auto sampleBefore = state.widgetSamples.load();
        if (RequestWidgetSample(record->snapshot.generation))
        {
            std::unique_lock lock(state.mutex);
            state.wake.wait_for(lock, std::chrono::milliseconds(250), [&] { return state.workerExit || state.widgetSamples.load() > sampleBefore; });
            record->finalWidgetSample = state.widgetSamples.load() > sampleBefore;
        }
        { Scope marker("profiler", "capture.stop"); }
        record->lastTraceNs = native_trace::TrackEvent::GetTraceTimeNs();
        record->quality = DeactivateTrace();
        {
            std::lock_guard lock(state.mutex);
            record->snapshot.elapsedMs = static_cast<double>(record->lastTraceNs - record->firstTraceNs) / 1e6;
        }
        SetPhase(record, CapturePhase::Finalizing);
        if (gpuDemandStarted) { record->gpuDemand(false); gpuDemandStarted = false; }
        complete = session.Stop(error);
        if (complete)
        {
            record->traceSha256 = HashFile(record->snapshot.trace);
            if (record->traceSha256.empty()) { complete = false; error = "traceFingerprintFailed"; }
        }
    }
    catch (const std::exception& failure)
    {
        const std::string_view detail = failure.what();
        error = detail.size() <= 80 && detail.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") == detail.npos
            ? std::string(detail) : "captureWorkerFailed";
    }
    catch (...) { error = "captureWorkerFailed"; }
    if (gpuDemandStarted) { try { record->gpuDemand(false); } catch (...) { error = "captureGpuCleanupFailed"; } }
    if (Enabled()) record->quality = DeactivateTrace();
    {
        std::lock_guard lock(state.mutex);
        record->snapshot.error = error;
    }
    try
    {
        if (manifest.value != INVALID_HANDLE_VALUE)
        {
            const auto json = Manifest(*record, complete);
            DWORD written = 0;
            const bool saved = json.size() <= MAXDWORD && WriteFile(manifest.value, json.data(), static_cast<DWORD>(json.size()), &written, nullptr) &&
                written == json.size() && FlushFileBuffers(manifest.value);
            CloseHandle(manifest.value); manifest.value = INVALID_HANDLE_VALUE;
            if (!saved || !MoveFileExW(partialManifest.c_str(), record->snapshot.manifest.c_str(), MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("manifestPublishFailed");
        }
    }
    catch (...)
    {
        std::lock_guard lock(state.mutex);
        complete = false;
        record->snapshot.error = "manifestPublishFailed";
    }
    // Terminal status is published only after SDK export and the immutable
    // manifest are finished. The final record is cached for idempotent callers.
    if (storageLease.value != INVALID_HANDLE_VALUE) { CloseHandle(storageLease.value); storageLease.value = INVALID_HANDLE_VALUE; }
    {
        std::lock_guard lock(state.mutex);
        record->snapshot.phase = complete ? CapturePhase::Completed : CapturePhase::Failed;
        state.history.push_back(record);
        if (state.history.size() > 20) state.history.pop_front();
    }
    NotifyState();
}
void WorkerLoop() noexcept
{
    for (;;)
    {
        std::shared_ptr<Record> record;
        bool analysis = false;
        {
            std::unique_lock lock(state.mutex);
            state.wake.wait(lock, [] { return state.workerExit || state.pending || state.pendingAnalysis; });
            if (!state.pending && !state.pendingAnalysis && state.workerExit) return;
            if (state.pending) record = std::move(state.pending);
            else { record = std::move(state.pendingAnalysis); analysis = true; record->snapshot.analysis = "running"; }
        }
        if (!analysis) Worker(std::move(record));
        else
        {
            NotifyState();
            const auto result = AnalyzeCapture(record->snapshot.manifest, &state.analysisCancel);
            {
                std::lock_guard lock(state.mutex);
                record->snapshot.analysis = result.error.empty() ? "completed" : "failed";
                record->snapshot.report = result.report;
                record->snapshot.analysisError = result.error;
                record->snapshot.quality = result.quality;
                state.analysisActive = false;
            }
            NotifyState();
        }
    }
}
}

bool Start(const CaptureOptions& requested, std::string& error)
{
    error.clear();
    std::lock_guard startLock(state.startMutex);
    auto options = requested;
    if (options.requestId.empty()) options.requestId = options.session;
    if (!ValidId(options.session) || !ValidId(options.requestId) || options.seconds < 1 || options.seconds > 600 ||
        options.bufferKiB < 64 || options.bufferKiB > 65536 || options.maximumFileBytes < 1024 ||
        options.maximumFileBytes > 256ULL * 1024 * 1024 || options.selectedInstances.size() > 8 ||
        (options.preset != CapturePreset::Resource && options.preset != CapturePreset::Interaction && options.preset != CapturePreset::Widget) ||
        (options.preset == CapturePreset::Widget && options.selectedInstances.empty()) ||
        std::any_of(options.selectedInstances.begin(), options.selectedInstances.end(), [](const auto& id) { return id.empty() || id.size() > 256; }))
    { error = "invalidCaptureOptions"; return false; }
    if (!options.output.empty() && (!IsLocalCapturePath(options.output) || options.output.extension() != L".pftrace"))
    { error = "invalidOutputPath"; return false; }
    try
    {
        std::lock_guard lock(state.mutex);
        if (state.current && state.current->snapshot.requestId == options.requestId)
        {
            if (SameRequest(options, state.current->options)) return true;
            error = "requestIdConflict"; return false;
        }
        for (const auto& old : state.history)
            if (old->snapshot.requestId == options.requestId)
            {
                if (SameRequest(options, old->options)) return true;
                error = "requestIdConflict"; return false;
            }
        if (state.analysisActive) { error = "analysisBusy"; return false; }
        if (state.current && Running(state.current->snapshot.phase)) { error = "captureBusy"; return false; }
        if (std::any_of(state.history.begin(), state.history.end(), [&](const auto& old) { return old->snapshot.session == options.session; }) ||
            (state.current && state.current->snapshot.session == options.session))
        { error = "sessionAlreadyExists"; return false; }
        auto record = std::make_shared<Record>();
        record->options = options;
        record->gpuDemand = state.gpuDemand;
        if (options.output.empty()) record->managedRoot = state.root;
        record->snapshot.session = options.session;
        record->snapshot.requestId = options.requestId;
        record->snapshot.configHash = ConfigHash(options);
        record->snapshot.trace = options.output;
        if (!options.output.empty()) record->snapshot.manifest = options.output.parent_path() / L"session.json";
        record->snapshot.requestedSeconds = options.seconds;
        record->snapshot.preset = static_cast<unsigned>(options.preset); record->snapshot.bufferKiB = options.bufferKiB;
        record->snapshot.selectedInstances = options.selectedInstances;
        record->snapshot.scopeCpu = options.scopeCpu;
        record->snapshot.phase = CapturePhase::Starting;
        if (!state.worker.joinable())
        {
            state.workerExit = false;
            state.worker = std::thread(WorkerLoop);
        }
        record->snapshot.generation = ++state.generation;
        state.current = record;
        state.pending = record;
        state.stopRequested.store(false);
        state.wake.notify_all();
        NotifyState();
    }
    catch (...)
    {
        error = "captureWorkerStartFailed"; return false;
    }
    return true;
}
bool RequestStop(std::string_view session, std::uint64_t generation) noexcept
{
    std::lock_guard lock(state.mutex);
    if (!state.current || state.current->snapshot.session != session ||
        (generation && generation != state.current->snapshot.generation))
    {
        return std::any_of(state.history.begin(), state.history.end(), [&](const auto& old) {
            return old->snapshot.session == session && (!generation || old->snapshot.generation == generation) && !Running(old->snapshot.phase);
        });
    }
    if (state.current->snapshot.phase == CapturePhase::Completed || state.current->snapshot.phase == CapturePhase::Failed) return true;
    state.stopRequested.store(true);
    if (state.current->snapshot.phase == CapturePhase::Recording) state.current->snapshot.phase = CapturePhase::Stopping;
    state.wake.notify_all();
    NotifyState();
    return true;
}
bool RequestAnalysis(std::string_view session, std::uint64_t generation, std::string& error)
{
    error.clear();
    std::lock_guard lock(state.mutex);
    auto record = state.current;
    if (!record || record->snapshot.session != session || record->snapshot.generation != generation)
    { error = "sessionMismatch"; return false; }
    if (record->snapshot.phase != CapturePhase::Completed || state.analysisActive)
    { error = "captureBusy"; return false; }
    if (record->snapshot.analysis == "completed") return true;
    record->snapshot.analysis = "queued"; record->snapshot.analysisError.clear();
    state.analysisCancel.store(false);
    state.pendingAnalysis = record; state.analysisActive = true;
    state.wake.notify_all(); NotifyState(); return true;
}
CaptureSnapshot Snapshot(std::string_view session)
{
    std::lock_guard lock(state.mutex);
    if (state.current && (session.empty() || state.current->snapshot.session == session))
    {
        auto result = state.current->snapshot;
        if ((result.phase == CapturePhase::Recording || result.phase == CapturePhase::Stopping) && state.current->started != Clock::time_point{})
            result.elapsedMs = std::chrono::duration<double, std::milli>(Clock::now() - state.current->started).count();
        return result;
    }
    for (const auto& old : state.history) if (old->snapshot.session == session) return old->snapshot;
    CaptureSnapshot result;
    result.found = session.empty(); result.session = session;
    if (!result.found) { result.phase = CapturePhase::Failed; result.error = "unknownSession"; }
    return result;
}
std::string SnapshotJson(const CaptureSnapshot& snapshot, std::string_view replyRequestId)
{
    std::ostringstream out; out.imbue(std::locale::classic());
    out << "{\"protocolVersion\":3,\"requestId\":" << Quote(replyRequestId.empty() ? snapshot.requestId : replyRequestId)
        << ",\"captureRequestId\":" << Quote(snapshot.requestId) << ",\"sessionId\":" << Quote(snapshot.session)
        << ",\"generation\":" << snapshot.generation << ",\"state\":" << Quote(PhaseName(snapshot.phase))
        << ",\"processId\":" << GetCurrentProcessId() << ",\"elapsedMs\":" << snapshot.elapsedMs
        << ",\"requestedSeconds\":" << snapshot.requestedSeconds << ",\"configHash\":" << Quote(snapshot.configHash)
        << ",\"analysis\":" << Quote(snapshot.analysis) << ",\"analysisError\":" << Quote(snapshot.analysisError)
        << ",\"quality\":" << Quote(snapshot.quality) << ",\"preset\":" << Quote(PresetName(static_cast<CapturePreset>(snapshot.preset)))
        << ",\"bufferKiB\":" << snapshot.bufferKiB << ",\"scopeCpuEnabled\":" << (snapshot.scopeCpu ? "true" : "false")
        << ",\"errorCode\":" << Quote(snapshot.error) << ",\"retryable\":" << (snapshot.error == "captureBusy" ? "true" : "false")
        << ",\"paths\":{\"trace\":" << Quote(Utf8(snapshot.trace.wstring())) << ",\"manifest\":" << Quote(Utf8(snapshot.manifest.wstring()))
        << ",\"report\":" << Quote(Utf8(snapshot.report.wstring()))
        << "},\"capabilities\":{\"backend\":\"perfetto\",\"sdkVersion\":\"v58.2\",\"hostOnly\":true,\"protocol\":3},"
        << "\"participants\":[{\"role\":\"host\",\"pid\":" << GetCurrentProcessId() << ",\"included\":true}]}";
    return out.str();
}
std::string ProcessControlRequest(std::string_view request) noexcept
{
    CaptureSnapshot failure; failure.phase = CapturePhase::Failed;
    try
    {
        JsonValue value; std::string parseError;
        if (request.size() > 16384 || !ParseJson(request, value, &parseError) || !value.IsObject()) throw std::runtime_error("invalidControlRequest");
        constexpr std::array<std::string_view, 10> fields{
            "protocolVersion", "requestId", "sessionId", "command", "generation", "preset", "seconds", "bufferKiB", "threadCpu", "widgetIds" };
        for (const auto& [field, ignored] : value.object)
        {
            (void)ignored;
            if (std::find(fields.begin(), fields.end(), field) == fields.end()) throw std::runtime_error("unsupportedControlField");
        }
        const auto string = [&](std::string_view name, bool required = false) {
            const auto* item = value.Find(name);
            if (!item && !required) return std::string{};
            if (!item || !item->IsString() || item->string.size() > 256) throw std::runtime_error("invalidControlField");
            return item->string;
        };
        const auto number = [&](std::string_view name, std::uint64_t fallback, std::uint64_t maximum) {
            const auto* item = value.Find(name);
            if (!item) return fallback;
            if (!item->IsNumber() || !std::isfinite(item->number) || item->number < 0 || item->number > static_cast<double>(maximum) || std::floor(item->number) != item->number)
                throw std::runtime_error("invalidControlNumber");
            return static_cast<std::uint64_t>(item->number);
        };
        if (number("protocolVersion", 0, 3) != 3) throw std::runtime_error("unsupportedProtocol");
        failure.requestId = string("requestId", true);
        if (!ValidId(failure.requestId)) throw std::runtime_error("invalidRequestId");
        failure.session = string("sessionId");
        if (!failure.session.empty() && !ValidId(failure.session)) throw std::runtime_error("invalidSessionId");
        const auto command = string("command", true);
        if (command == "status") return SnapshotJson(Snapshot(failure.session), failure.requestId);
        if (command == "stop")
        {
            const auto generation = number("generation", 0, 9007199254740991ULL);
            if (!generation || !RequestStop(failure.session, generation)) throw std::runtime_error("sessionMismatch");
            return SnapshotJson(Snapshot(failure.session), failure.requestId);
        }
        if (command != "start") throw std::runtime_error("unsupportedCommand");
        CaptureOptions options;
        options.session = failure.session; options.requestId = failure.requestId;
        options.seconds = static_cast<unsigned>(number("seconds", 120, 600));
        options.bufferKiB = static_cast<unsigned>(number("bufferKiB", 32768, 65536));
        const auto preset = string("preset", true);
        if (preset == "idle") options.preset = CapturePreset::Resource;
        else if (preset == "interaction") options.preset = CapturePreset::Interaction;
        else if (preset == "widget-detail") options.preset = CapturePreset::Widget;
        else throw std::runtime_error("invalidPreset");
        if (const auto* cpu = value.Find("threadCpu"))
        { if (!cpu->IsBoolean()) throw std::runtime_error("invalidThreadCpu"); options.scopeCpu = cpu->boolean; }
        if (const auto* widgets = value.Find("widgetIds"))
        {
            if (!widgets->IsArray() || widgets->array.size() > 8) throw std::runtime_error("invalidWidgetFilter");
            for (const auto& item : widgets->array)
            { if (!item.IsString() || item.string.empty() || item.string.size() > 256) throw std::runtime_error("invalidWidgetFilter"); options.selectedInstances.push_back(Wide(item.string)); }
        }
        std::string error;
        if (!Start(options, error)) throw std::runtime_error(error);
        return SnapshotJson(Snapshot(options.session), failure.requestId);
    }
    catch (const std::exception& error) { failure.error = error.what(); }
    catch (...) { failure.error = "controlRequestFailed"; }
    try { return SnapshotJson(failure); } catch (...) { return "{\"protocolVersion\":3,\"state\":\"failed\",\"errorCode\":\"controlAllocationFailed\"}"; }
}
void ConfigureControl(HWND window, const std::filesystem::path& root)
{
    std::lock_guard lock(state.mutex);
    state.root = root;
    state.window.store(window);
}
void ConfigureGpuDemand(std::function<void(bool)> demand)
{ std::lock_guard lock(state.mutex); state.gpuDemand = std::move(demand); }
void Shutdown() noexcept
{
    state.window.store(nullptr);
    StopControlPipe();
    std::lock_guard startLock(state.startMutex);
    { std::lock_guard lock(state.mutex); state.workerExit = true; state.stopRequested.store(true); state.analysisCancel.store(true); }
    state.wake.notify_all();
    if (state.worker.joinable()) state.worker.join();
    {
        std::lock_guard lock(state.mutex);
        state.gpuDemand = {};
        if (state.current) state.current->gpuDemand = {};
        for (const auto& record : state.history) record->gpuDemand = {};
    }
}
LRESULT Status() noexcept
{
    std::lock_guard lock(state.mutex);
    if (!state.current) return ProtocolIdle;
    switch (state.current->snapshot.phase)
    {
    case CapturePhase::Starting: return ProtocolStarting;
    case CapturePhase::Recording: return ProtocolRecording;
    case CapturePhase::Stopping: case CapturePhase::Finalizing: return ProtocolFinishing;
    case CapturePhase::Failed: return ProtocolFailed;
    default: return ProtocolIdle;
    }
}
bool IsStateChangeMessage(UINT message) noexcept { return message && message == StateMessage(); }
bool IsControlMessage(UINT message, WPARAM wParam, LPARAM lParam) noexcept
{
    if ((message == ControlMessage() && message) || (message == SampleMessage() && message)) return true;
    if (message == WM_TIMER && wParam == SampleTimer) return true;
    if (message != WM_COPYDATA || !lParam) return false;
    return reinterpret_cast<const COPYDATASTRUCT*>(lParam)->dwData == CopyDataTag;
}
LRESULT HandleControlMessage(HWND window, UINT message, WPARAM wParam, LPARAM,
    void (*sampleWidgets)(void*), void* owner) noexcept
{
    try
    {
        if (message == ControlMessage()) return EnsureControlPipe() ? Status() : ProtocolFailed;
        if (message == WM_TIMER) { KillTimer(window, SampleTimer); return 0; }
        if (message == SampleMessage())
        {
            const auto snapshot = Snapshot();
            if (Enabled() && snapshot.generation == static_cast<std::uint64_t>(wParam) && sampleWidgets)
            {
                sampleWidgets(owner);
                state.widgetSamples.fetch_add(1); state.wake.notify_all();
            }
            return 1;
        }
        // Old WM_COPYDATA starts cannot authenticate the caller and cannot
        // preserve the new native format contract. No SDK work runs here.
        return 0;
    }
    catch (...) { return 0; }
}
}

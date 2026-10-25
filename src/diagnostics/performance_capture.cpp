#include "performance_capture.h"
#include "performance_trace.h"
#include "performance_trace_control.h"
#include "performance_control_pipe.h"
#include "perfetto_session.h"
#include "perfetto_categories.h"
#include "common/json_value.h"

#include <bcrypt.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
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
    const auto text = std::string(PresetName(options.preset)) + "|" + std::to_string(options.seconds) + "|" +
        std::to_string(options.bufferKiB) + "|" + std::to_string(options.maximumFileBytes) + "|" + (options.scopeCpu ? "cpu" : "wall");
    std::uint64_t value = 1469598103934665603ULL;
    const auto add = [&](std::string_view part) { for (const unsigned char ch : part) { value ^= ch; value *= 1099511628211ULL; } };
    add(text);
    for (const auto& owner : options.selectedInstances) { add("|"); add(Utf8(owner)); }
    std::ostringstream out; out << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
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
    CaptureSnapshot snapshot;
    Clock::time_point started{};
    std::string executableSha256, traceSha256;
    std::uint64_t firstTraceNs = 0, lastTraceNs = 0;
    TraceQuality quality;
};
struct State
{
    std::mutex mutex, startMutex;
    std::condition_variable wake;
    std::thread worker;
    std::atomic<bool> stopRequested{ false };
    std::atomic<HWND> window{ nullptr };
    std::shared_ptr<Record> current;
    std::deque<CaptureSnapshot> history;
    std::filesystem::path root;
    std::uint64_t generation = 0;
    ~State() { stopRequested.store(true); wake.notify_all(); if (worker.joinable()) worker.join(); }
};
State state;
UINT SampleMessage() noexcept
{ static const auto message = RegisterWindowMessageW(L"FreeFallingSnow.SnowDesktop.Performance.Sample.v3"); return message; }
UINT ControlMessage() noexcept
{ static const auto message = RegisterWindowMessageW(ControlMessageName); return message; }
void SetPhase(const std::shared_ptr<Record>& record, CapturePhase phase)
{ std::lock_guard lock(state.mutex); record->snapshot.phase = phase; }

class GpuSampler
{
public:
    GpuSampler()
    {
        Scope setup("profiler", "gpu.initialize");
        if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) return;
        Add(L"\\GPU Engine(*)\\Utilization Percentage", engine_);
        Add(L"\\GPU Process Memory(*)\\Dedicated Usage", dedicated_);
        Add(L"\\GPU Process Memory(*)\\Shared Usage", shared_);
        baseline_ = PdhCollectQueryData(query_) == ERROR_SUCCESS;
    }
    ~GpuSampler() { if (query_) PdhCloseQuery(query_); }
    void Sample()
    {
        if (!query_ || PdhCollectQueryData(query_) != ERROR_SUCCESS)
        {
            Value("process", "gpu_query_available", {}, 0);
            return;
        }
        if (!baseline_) { baseline_ = true; return; }
        const auto engineCount = Emit(engine_, "gpu_engine_percent");
        const auto dedicatedCount = Emit(dedicated_, "gpu_dedicated_bytes");
        const auto sharedCount = Emit(shared_, "gpu_shared_bytes");
        // No matching PID instance is unavailable, not a fabricated zero.
        Value("process", "gpu_engine_instances", {}, engineCount);
        Value("process", "gpu_dedicated_instances", {}, dedicatedCount);
        Value("process", "gpu_shared_instances", {}, sharedCount);
    }
private:
    void Add(const wchar_t* path, PDH_HCOUNTER& counter)
    {
        if (PdhAddEnglishCounterW(query_, path, 0, &counter) != ERROR_SUCCESS)
            counter = nullptr;
    }
    unsigned Emit(PDH_HCOUNTER counter, const char* name)
    {
        if (!counter) return 0;
        DWORD bytes = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE,
                &bytes, &count, nullptr) != PDH_MORE_DATA ||
            bytes > 16 * 1024 * 1024) return 0;
        std::vector<std::uint64_t> storage((bytes + 7) / 8);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(storage.data());
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE,
                &bytes, &count, items) != ERROR_SUCCESS) return 0;
        const std::wstring prefix = L"pid_" +
            std::to_wstring(GetCurrentProcessId()) + L"_";
        unsigned matches = 0;
        for (DWORD i = 0; i < count; ++i)
        {
            const auto& item = items[i];
            if (!item.szName || !std::wstring_view(item.szName).starts_with(prefix) ||
                (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
                 item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)) continue;
            Value("process", name, item.szName, item.FmtValue.doubleValue);
            ++matches;
        }
        return matches;
    }
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER engine_ = nullptr, dedicated_ = nullptr, shared_ = nullptr;
    bool baseline_ = false;
};

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
void RequestWidgetSample(std::uint64_t generation)
{
    const auto window = state.window.load();
    if (window && SampleMessage()) PostMessageW(window, SampleMessage(), static_cast<WPARAM>(generation), 0);
}
std::string Manifest(const Record& record, bool complete)
{
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    std::ostringstream out; out.imbue(std::locale::classic()); out << std::setprecision(12);
    out << "{\"schemaVersion\":2,\"format\":\"perfetto\",\"sdkVersion\":\"v58.2\",\"sessionId\":" << Quote(record.snapshot.session)
        << ",\"generation\":" << record.snapshot.generation << ",\"requestId\":" << Quote(record.snapshot.requestId)
        << ",\"state\":" << Quote(complete ? "completed" : "failed") << ",\"completion\":" << (complete ? "true" : "false")
        << ",\"errorCode\":" << Quote(record.snapshot.error) << ",\"hostVersion\":" << Quote(SNOWDESKTOP_VERSION)
        << ",\"processId\":" << GetCurrentProcessId() << ",\"processRole\":\"host\",\"sourceCommit\":null,\"executableSha256\":"
        << (record.executableSha256.empty() ? "null" : Quote(record.executableSha256))
        << ",\"traceSha256\":" << (record.traceSha256.empty() ? "null" : Quote(record.traceSha256))
        << ",\"logicalProcessors\":" << system.dwNumberOfProcessors << ",\"preset\":" << Quote(PresetName(record.options.preset))
        << ",\"configHash\":" << Quote(record.snapshot.configHash) << ",\"scopeCpuEnabled\":" << (record.options.scopeCpu ? "true" : "false")
        << ",\"threadCpuSource\":" << (record.options.scopeCpu ? "\"perfetto.thread_time\"" : "null")
        << ",\"requestedSeconds\":" << record.options.seconds << ",\"bufferKiB\":" << record.options.bufferKiB
        << ",\"firstTraceNs\":" << Quote(std::to_string(record.firstTraceNs)) << ",\"lastTraceNs\":" << Quote(std::to_string(record.lastTraceNs))
        << ",\"durationMs\":" << record.snapshot.elapsedMs << ",\"traceFile\":" << Quote(Utf8(record.snapshot.trace.filename().wstring()))
        << ",\"coverage\":\"pending-analysis\",\"participants\":[{\"role\":\"host\",\"pid\":" << GetCurrentProcessId()
        << ",\"included\":true},{\"role\":\"settings\",\"included\":false}],\"probeQuality\":{\"droppedOwners\":" << record.quality.droppedOwners
        << ",\"droppedCounters\":" << record.quality.droppedCounters << ",\"droppedLinks\":" << record.quality.droppedLinks
        << ",\"probeErrors\":" << record.quality.probeErrors << "}}";
    return out.str();
}
void Worker(std::shared_ptr<Record> record) noexcept
{
    std::string error;
    bool complete = false;
    File manifest, storageLease;
    const auto partialManifest = std::filesystem::path(record->snapshot.manifest.wstring() + L".partial");
    try
    {
        const auto directory = record->snapshot.trace.parent_path();
        if (!SafeDirectory(directory, true)) throw std::runtime_error("capturePathRejected");
        std::filesystem::create_directories(directory);
        if (!SafeDirectory(directory, false)) throw std::runtime_error("capturePathRejected");
        std::uint64_t fileLimit = record->options.maximumFileBytes;
        std::filesystem::path managedRoot;
        { std::lock_guard lock(state.mutex); managedRoot = state.root; }
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
        record->firstTraceNs = native_trace::TrackEvent::GetTraceTimeNs();
        {
            std::lock_guard lock(state.mutex);
            record->started = Clock::now();
            record->snapshot.phase = CapturePhase::Recording;
        }
        { Scope marker("profiler", "capture.start"); }
        SampleProcess(); SampleIo(); RequestWidgetSample(record->snapshot.generation);
        const auto deadline = record->started + std::chrono::seconds(record->options.seconds);
        {
            GpuSampler gpu;
            while (!state.stopRequested.load() && Clock::now() < deadline)
            {
                std::unique_lock lock(state.mutex);
                state.wake.wait_until(lock, std::min(deadline, Clock::now() + std::chrono::seconds(1)), [] { return state.stopRequested.load(); });
                lock.unlock();
                SampleProcess(); SampleIo();
                { Scope scope("profiler", "sample.gpu"); gpu.Sample(); }
                RequestWidgetSample(record->snapshot.generation);
            }
        }
        { Scope marker("profiler", "capture.stop"); }
        record->lastTraceNs = native_trace::TrackEvent::GetTraceTimeNs();
        record->quality = DeactivateTrace();
        {
            std::lock_guard lock(state.mutex);
            record->snapshot.elapsedMs = static_cast<double>(record->lastTraceNs - record->firstTraceNs) / 1e6;
        }
        SetPhase(record, CapturePhase::Finalizing);
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
    std::lock_guard lock(state.mutex);
    record->snapshot.phase = complete ? CapturePhase::Completed : CapturePhase::Failed;
    state.history.push_back(record->snapshot);
    if (state.history.size() > 20) state.history.pop_front();
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
    const auto config = ConfigHash(options);
    {
        std::lock_guard lock(state.mutex);
        if (state.current && state.current->snapshot.requestId == options.requestId)
        {
            if (state.current->snapshot.session == options.session && state.current->snapshot.configHash == config &&
                (options.output.empty() || options.output == state.current->options.output)) return true;
            error = "requestIdConflict"; return false;
        }
        for (const auto& old : state.history)
            if (old.requestId == options.requestId)
            {
                if (old.session == options.session && old.configHash == config && (options.output.empty() || options.output == old.trace)) return true;
                error = "requestIdConflict"; return false;
            }
        if (state.current && Running(state.current->snapshot.phase)) { error = "captureBusy"; return false; }
        if (std::any_of(state.history.begin(), state.history.end(), [&](const auto& old) { return old.session == options.session || old.requestId == options.requestId; }))
        { error = "sessionAlreadyExists"; return false; }
        if (options.output.empty()) options.output = state.root / Wide(options.session) / L"trace.pftrace";
    }
    if (!options.output.is_absolute() || options.output != options.output.lexically_normal() || options.output.extension() != L".pftrace")
    { error = "invalidOutputPath"; return false; }
    if (state.worker.joinable()) state.worker.join();
    auto record = std::make_shared<Record>();
    record->options = options;
    record->snapshot.session = options.session;
    record->snapshot.requestId = options.requestId;
    record->snapshot.configHash = config;
    record->snapshot.trace = options.output;
    record->snapshot.manifest = options.output.parent_path() / L"session.json";
    record->snapshot.requestedSeconds = options.seconds;
    record->snapshot.scopeCpu = options.scopeCpu;
    record->snapshot.phase = CapturePhase::Starting;
    {
        std::lock_guard lock(state.mutex);
        record->snapshot.generation = ++state.generation;
        state.current = record;
        state.stopRequested.store(false);
    }
    try { state.worker = std::thread(Worker, record); }
    catch (...)
    {
        std::lock_guard lock(state.mutex);
        record->snapshot.phase = CapturePhase::Failed; record->snapshot.error = "captureWorkerStartFailed";
        error = record->snapshot.error; return false;
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
            return old.session == session && (!generation || old.generation == generation) && !Running(old.phase);
        });
    }
    if (state.current->snapshot.phase == CapturePhase::Completed || state.current->snapshot.phase == CapturePhase::Failed) return true;
    state.stopRequested.store(true);
    if (state.current->snapshot.phase == CapturePhase::Recording) state.current->snapshot.phase = CapturePhase::Stopping;
    state.wake.notify_all();
    return true;
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
    for (const auto& old : state.history) if (old.session == session) return old;
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
        << ",\"errorCode\":" << Quote(snapshot.error) << ",\"retryable\":" << (snapshot.error == "captureBusy" ? "true" : "false")
        << ",\"paths\":{\"trace\":" << Quote(Utf8(snapshot.trace.wstring())) << ",\"manifest\":" << Quote(Utf8(snapshot.manifest.wstring()))
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
void Shutdown() noexcept
{
    state.window.store(nullptr);
    StopControlPipe();
    state.stopRequested.store(true); state.wake.notify_all();
    std::lock_guard startLock(state.startMutex);
    if (state.worker.joinable()) state.worker.join();
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
            if (Enabled() && snapshot.generation == static_cast<std::uint64_t>(wParam) && sampleWidgets) sampleWidgets(owner);
            return 1;
        }
        // Old WM_COPYDATA starts cannot authenticate the caller and cannot
        // preserve the new native format contract. No SDK work runs here.
        return 0;
    }
    catch (...) { return 0; }
}
}

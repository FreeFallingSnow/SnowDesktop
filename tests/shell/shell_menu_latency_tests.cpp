#include "shell/shell_extension_service.h"
#include "shell/shell_extension_menu_presentation.h"
#include "shell/shell_extension_management.h"
#include "shell_menu_samples.h"
#include "common/json_string.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <objbase.h>
#include <stdexcept>
#include <map>
#include <set>
#include <VersionHelpers.h>
#include <psapi.h>
#include <tlhelp32.h>

namespace
{
namespace ext = snowdesktop::shell_extensions;
namespace menu = snowdesktop::modern_menu;
using Clock = std::chrono::steady_clock;
void Require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
struct Workspace
{
    std::filesystem::path path;
    Workspace()
    {
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("create menu latency workspace identity");
        wchar_t name[40]{}; StringFromGUID2(guid, name, 40);
        path = std::filesystem::temp_directory_path() / (L"SnowDesktop-MenuLatency-" + std::wstring(name));
        std::filesystem::create_directories(path);
    }
    ~Workspace() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    ext::Request File(std::wstring name, std::wstring_view type = L".txt") const
    {
        const auto target = path / (std::move(name) + std::wstring(type));
        const auto bytes = ext::DiscoverySample(type);
        Require(!bytes.empty(), "latency probe uses a valid supported file sample");
        std::ofstream output(target, std::ios::binary);
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        Require(bool(output), "write private menu latency sample");
        ext::Request request; request.paths = {target.wstring()}; return request;
    }
};
void Pump()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    { TranslateMessage(&message); DispatchMessageW(&message); }
    MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
}
template<class Predicate> void Until(Predicate ready, const char *failure, DWORD timeout = 5000)
{
    const auto deadline = GetTickCount64() + timeout;
    while (!ready() && GetTickCount64() < deadline) Pump();
    Require(ready(), failure);
}
struct TraceCapture
{
    std::wstring previous;
    std::filesystem::path file;
    explicit TraceCapture(const std::filesystem::path &prefix, bool keepExisting = false)
    {
        wchar_t buffer[32768]{};
        const auto length = GetEnvironmentVariableW(L"SNOWDESKTOP_MENU_TRACE", buffer, 32768);
        Require(length < 32768, "diagnostic path fits the environment limit");
        previous.assign(buffer, length);
        const auto selected = keepExisting && !previous.empty() ? previous : prefix.wstring();
        Require(SetEnvironmentVariableW(L"SNOWDESKTOP_MENU_TRACE", selected.c_str()) != FALSE, "enable private menu trace");
        file = selected + L"." + std::to_wstring(GetCurrentProcessId()) + L".log";
    }
    ~TraceCapture() { SetEnvironmentVariableW(L"SNOWDESKTOP_MENU_TRACE", previous.empty() ? nullptr : previous.c_str()); }
    unsigned Count(std::string_view event) const
    {
        std::ifstream input(file, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(input)), {});
        unsigned count = 0;
        for (size_t at = 0; (at = text.find(event, at)) != std::string::npos; at += event.size()) ++count;
        return count;
    }
};
}

void TestShellMenuPreparedService()
{
    Workspace workspace;
    TraceCapture trace(workspace.path / L"trace");
    const auto selected = workspace.File(L"prepared-retention");
    const auto initial = workspace.File(L"initial");
    const auto disabled = workspace.File(L"disabled");
    ext::MenuService service(workspace.path / L"cache");
    ext::Preferences prefs; ext::SetCommon(prefs, "verb:latency-test", ext::Category::Objects, true);
    service.Configure(prefs);
    service.Query(initial);
    Until([&] { const auto view = service.View(initial); return view.snapshot && !view.pending; },
          "an explicit initial file popup supplies the preparation demand");
    Until([&] { return trace.Count("phase=preparation reason=ready") == 1; },
          "after explicit file demand the service prepares its next worker while idle", 10000);
    service.Query(selected);
    Until([&] { const auto view = service.View(selected); return view.snapshot && !view.pending; },
          "the production service queries the exact selected file");
    Require(service.View(selected).snapshot->entries.front().key == "cold" &&
            trace.Count("phase=helper_start reason=reused") > 0,
            "the service reuses its ready process without a TXT or target menu prequery");
    Require(trace.Count("phase=preparation reason=ready") == 1,
            "a reusable healthy worker does not create a replacement standby for each query");
    const auto released = trace.Count("phase=preparation reason=released");
    service.Configure({}); service.Prewarm(disabled);
    Until([&] { return trace.Count("phase=preparation reason=released") > released; },
          "turning file extensions off releases the unused standby process");
    auto barrier = disabled; barrier.paths = {workspace.path.wstring()};
    service.Query(barrier, ext::QueryPriority::Inspect, true);
    Until([&] { return service.View(barrier).snapshot.has_value(); }, "disabled selection prewarm barrier completes");
    Require(!service.View(disabled).snapshot,
            "turning extensions off suppresses new file prewarm while explicit inspection remains available");
}

void BenchmarkShellMenuFirstScreen()
{
    Workspace workspace;
    TraceCapture trace(workspace.path / L"trace", true);
    SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU", L"1");
    struct RealMode { ~RealMode() { SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU", nullptr); } } realMode;
    ext::Preferences prefs;
    wchar_t settingsPath[32768]{};
    if (GetEnvironmentVariableW(L"SNOWDESKTOP_MENU_BENCHMARK_PREFERENCES", settingsPath, 32768))
    {
        std::ifstream input(settingsPath, std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(input)), {});
        JsonValue json;
        Require(ParseJson(content, json), "read the copied benchmark visibility settings");
        prefs = ext::ReadPreferences(json.Find("shellExtensions"));
    }
    else
    {
        ext::SetCommon(prefs, "verb:sevenzip", ext::Category::Objects, true);
        ext::SetCommon(prefs, "menu:winrar:14695981039346656037", ext::Category::Objects, true);
    }
    Require(ext::HasOptIns(prefs, ext::Context::File), "the latency benchmark needs enabled file extensions");
    const auto cache = workspace.path / L"cache";
    wchar_t metadataPath[32768]{};
    if (GetEnvironmentVariableW(L"SNOWDESKTOP_MENU_BENCHMARK_METADATA", metadataPath, 32768))
    {
        std::filesystem::create_directories(cache);
        for (const auto name : {L"catalogue.bin", L"observed.dat"})
            std::filesystem::copy_file(std::filesystem::path(metadataPath) / name, cache / name);
    }
    ext::MenuService service(cache); service.Configure(prefs);
    auto open = [&](const ext::Request &request, const char *scenario, double preparationMs) {
        const auto started = Clock::now();
        ext::Presentation popup(request, prefs, L"", L"", service);
        std::vector<menu::Item> items; menu::Options options;
        popup.Attach(items, options, 0);
        const auto elapsed = [&] { return std::chrono::duration<double, std::milli>(Clock::now() - started).count(); };
        double firstMs = items.empty() ? -1 : elapsed();
        auto nextPoll = Clock::now();
        const auto deadline = GetTickCount64() + 20000;
        while (options.pollItems && (!options.pollItemsFinished || !options.pollItemsFinished()) && GetTickCount64() < deadline)
        {
            if (Clock::now() >= nextPoll)
            {
                if (auto update = options.pollItems(items, true)) items = std::move(*update);
                nextPoll = Clock::now() + std::chrono::milliseconds(50);
            }
            if (firstMs < 0 && !items.empty()) firstMs = elapsed();
            Pump();
        }
        const bool complete = !options.pollItems || (options.pollItemsFinished && options.pollItemsFinished());
        const double completeMs = elapsed();
        std::cout << "first_screen scenario=" << scenario << " first_ms=" << firstMs << " complete_ms=" << completeMs
                  << " preparation_ms=" << preparationMs << " entries=" << items.size() << " complete=" << complete
                  << " within_200ms=" << (complete && !items.empty() && completeMs < 200) << std::endl;
        Require(complete && !items.empty(), "real popup enumeration must complete with enabled visible commands");
    };
    const auto cold = workspace.File(L"cold");
    open(cold, "cold-immediate", 0);
    open(cold, "same-file", 0);
    open(workspace.File(L"new-same-type"), "new-file-immediate", 0);
    unsigned index = 0;
    constexpr std::pair<const wchar_t *, const char *> samples[] = {
        {L".txt", "pooled-txt"}, {L".png", "pooled-png"},
        {L".zip", "pooled-zip"}, {L".pdf", "pooled-pdf"}};
    for (const auto &[type, scenario] : samples)
    {
        const auto request = workspace.File(L"selected-" + std::to_wstring(index++), type);
        open(request, scenario, 0);
        open(request, "same-file-repeat", 0);
    }
}

// Explicit, read-only diagnostic over a caller-supplied sample manifest. The
// production Session, scheduler and popup projection stay real; no command is
// invoked and all cache output belongs to this private workspace.
void BenchmarkDesktopMenus()
{
    Require(IsWindows10OrGreater(), "real menu benchmarks use the host's Windows compatibility context");
    Workspace workspace;
    SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU", L"1");
    struct RealMode { ~RealMode() { SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU", nullptr); } } realMode;
    const auto read = [](const wchar_t* variable) {
        wchar_t path[32768]{};
        Require(GetEnvironmentVariableW(variable, path, 32768) > 0, "diagnostic input path is required");
        std::ifstream input(path, std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(input)), {});
        JsonValue value; Require(ParseJson(content, value), "read diagnostic JSON input"); return value;
    };
    const auto utf8 = [](std::wstring_view value) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        std::string text(size, 0);
        if (size) WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), text.data(), size, nullptr, nullptr);
        return text;
    };
    const auto quote = [](std::string_view text) { return "\"" + snowdesktop::json::EscapeString(text) + "\""; };
    const auto manifest = read(L"SNOWDESKTOP_MENU_BENCHMARK_MANIFEST");
    Require(manifest.IsArray() && !manifest.array.empty(), "sample manifest is a nonempty array");
    const auto settings = read(L"SNOWDESKTOP_MENU_BENCHMARK_PREFERENCES");
    const auto prefs = ext::ReadPreferences(settings.Find("shellExtensions"));
    struct Sample { std::string name; ext::Request request; std::set<std::string> required; };
    std::vector<Sample> samples;
    for (const auto& value : manifest.array)
    {
        const auto* name = value.Find("name"); const auto* path = value.Find("path");
        Require(name && name->IsString() && path && path->IsString(), "sample has a name and path");
        Sample sample; sample.name = name->string;
        sample.request.paths = {std::filesystem::path(std::u8string(path->string.begin(), path->string.end())).wstring()};
        Require(GetFileAttributesW(sample.request.paths.front().c_str()) != INVALID_FILE_ATTRIBUTES, "actual sample exists");
        if (const auto* background = value.Find("background")) sample.request.background = background->boolean;
        if (const auto* desktop = value.Find("desktop"); desktop && desktop->boolean) sample.request.context = ext::Context::Desktop;
        else if (sample.request.background) sample.request.context = ext::Context::FolderBackground;
        if (const auto* required = value.Find("required"))
            for (const auto& label : required->array) sample.required.insert(label.string);
        samples.push_back(std::move(sample));
    }
    std::map<std::pair<std::string, std::string>, std::multiset<std::string>> baseline;
    unsigned failures = 0;
    const auto record = [&](const Sample& sample, const std::string& mode, const char* phase, unsigned iteration,
        double ms, bool ok, DWORD worker, const std::string& error, const std::vector<ext::Entry>& entries) {
        // Preserve multiplicity: identical labels from aggregate and direct
        // providers must not silently disappear from stability comparisons.
        std::multiset<std::string> identity;
        std::set<std::string> labels;
        std::string encoded = "[";
        std::function<void(const std::vector<ext::Entry>&, std::string)> collect = [&](const auto& items, std::string parent) {
            for (const auto& entry : items) if (!entry.separator)
            {
                const auto label = utf8(entry.label);
                const auto key = parent + "/" + label;
                identity.insert(key); labels.insert(label); labels.insert(entry.provider);
                if (encoded.size() > 1) encoded += ',';
                encoded += "{\"path\":" + quote(key) + ",\"provider\":" + quote(entry.provider) +
                    ",\"registration\":" + quote(entry.registration) + ",\"key\":" + quote(entry.key) +
                    ",\"native\":" + (entry.native ? "true" : "false") + "}";
                collect(entry.children, key);
            }
        };
        collect(entries, ""); encoded += ']';
        const auto id = std::make_pair(sample.name, mode);
        const auto previous = baseline.find(id);
        const bool stable = previous == baseline.end() || previous->second == identity;
        if (ok && previous == baseline.end()) baseline.emplace(id, identity);
        bool complete = ok;
        if (mode != "native") for (const auto& label : sample.required) complete &= labels.contains(label);
        failures += !complete || !stable;
        std::cout << "{\"sample\":" << quote(sample.name) << ",\"mode\":" << quote(mode)
            << ",\"phase\":" << quote(phase) << ",\"iteration\":" << iteration << ",\"ms\":" << ms
            << ",\"ok\":" << (ok ? "true" : "false") << ",\"complete\":" << (complete ? "true" : "false")
            << ",\"stable\":" << (stable ? "true" : "false") << ",\"worker\":" << worker
            << ",\"error\":" << quote(error) << ",\"entries\":" << encoded << "}" << std::endl;
    };
    wchar_t modeText[32]{};
    GetEnvironmentVariableW(L"SNOWDESKTOP_MENU_BENCHMARK_MODE", modeText, 32);
    const std::wstring mode = *modeText ? modeText : L"raw";
    Require(mode == L"raw" || mode == L"production" || mode == L"native", "select a supported benchmark mode");
    std::unique_ptr<ext::MenuService> service;
    if (mode == L"production")
    {
        service = std::make_unique<ext::MenuService>(workspace.path / L"cache");
        service->Configure(prefs);
    }
    const auto run = [&](const Sample& sample, const char* phase, unsigned iteration) {
        const auto started = Clock::now();
        const auto elapsed = [&] { return std::chrono::duration<double, std::milli>(Clock::now() - started).count(); };
        const auto deadline = GetTickCount64() + 20000;
        if (mode == L"native")
        {
            auto request = sample.request; request.catalogueOnly = true;
            ext::NativeMenuSession session(request, {17, 17}, nullptr);
            std::optional<ext::NativeMenuResult> result;
            while (!result && GetTickCount64() < deadline) { result = session.Poll(); if (!result) Pump(); }
            ext::Entry roots; roots.label = result ? std::wstring(result->verb.begin(), result->verb.end()) : L"timeout";
            record(sample, "native", phase, iteration, elapsed(), result && result->ok, 0,
                result ? result->error : "benchmark timeout", {roots});
        }
        else if (mode == L"raw")
        {
            ext::Session session(sample.request, 8000, true);
            const DWORD pid = session.ProcessId(); std::optional<ext::Reply> result;
            while (!result && GetTickCount64() < deadline) { result = session.Poll(); if (!result) Pump(); }
            record(sample, "raw", phase, iteration, elapsed(), result && result->ok, pid,
                result ? result->error : "benchmark timeout", result ? result->entries : std::vector<ext::Entry>{});
        }
        else
        {
            ext::Presentation popup(sample.request, prefs, L"", L"", *service);
            std::vector<menu::Item> items; menu::Options options; popup.Attach(items, options, 0);
            while (options.pollItems && (!options.pollItemsFinished || !options.pollItemsFinished()) && GetTickCount64() < deadline)
            {
                if (auto update = options.pollItems(items, true)) items = std::move(*update);
                Pump();
            }
            const bool done = !options.pollItems || (options.pollItemsFinished && options.pollItemsFinished());
            const auto state = service->MenuDisplay(sample.request, prefs, true);
            std::function<std::vector<ext::Entry>(const std::vector<menu::Item>&, const std::vector<ext::Entry>&)> convert = [&](const auto& list, const auto& identities) {
                std::vector<ext::Entry> output;
                std::map<std::wstring, unsigned> observed;
                for (const auto& item : list)
                {
                    ext::Entry entry; entry.label = item.label; entry.separator = item.separator;
                    unsigned remaining = ++observed[item.label];
                    const auto match = std::find_if(identities.begin(), identities.end(), [&](const auto& identity) {
                        return identity.label == item.label && --remaining == 0;
                    });
                    if (match != identities.end())
                    { entry.provider = match->provider; entry.registration = match->registration; entry.key = match->key; }
                    entry.children = convert(item.children, match == identities.end() ? std::vector<ext::Entry>{} : match->children);
                    output.push_back(std::move(entry));
                }
                return output;
            };
            record(sample, "production", phase, iteration, elapsed(), done && state.error.empty(), 0, state.error,
                convert(items, state.snapshot ? state.snapshot->entries : std::vector<ext::Entry>{}));
            if (state.snapshot) record(sample, "attributed", phase, iteration, elapsed(), state.snapshot->ok,
                0, state.error, state.snapshot->entries);
        }
    };
    // Grouped repeats expose cold-to-warm changes; rotation exercises reuse of
    // one worker across distinct object types and all mapping directories.
    for (const auto& sample : samples)
        for (unsigned i = 0; i < 3; ++i) run(sample, i ? "repeat" : "first", i);
    for (unsigned i = 0; i < 3; ++i)
        for (const auto& sample : samples) run(sample, "alternating", i);
    if (service) service->Shutdown();
    ext::Session::ReleaseIdleWorker(); ext::Session::ReleasePreparedMenuWorker();
    std::cout << "desktop_menu_benchmark failures=" << failures << std::endl;
    Require(!failures, "all actual menu reads must complete with required commands and stable item sets");
}

// Opt-in service-path observation. Real registry readers, target validation,
// scheduler and helpers run against private caches; user commands are never run.
void BenchmarkMenuBackgroundAndManagement()
{
    Require(IsWindows10OrGreater(), "service benchmarks use the host's Windows compatibility context");
    Workspace workspace;
    TraceCapture trace(workspace.path / L"service-trace", true);
    SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU", L"1");
    struct RealMode { ~RealMode() { SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU", nullptr); } } realMode;
    const auto readInput = [](const wchar_t* variable) {
        wchar_t path[32768]{};
        const DWORD length = GetEnvironmentVariableW(variable, path, 32768);
        Require(length && length < 32768, "service benchmark input is an absolute JSON path");
        std::ifstream input(path, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(input)), {});
        JsonValue value; Require(ParseJson(bytes, value), "read private service benchmark input"); return value;
    };
    const auto settings = readInput(L"SNOWDESKTOP_MENU_BENCHMARK_PREFERENCES");
    const auto prefs = ext::ReadPreferences(settings.Find("shellExtensions"));
    const auto manifest = readInput(L"SNOWDESKTOP_MENU_BENCHMARK_MANIFEST");
    Require(manifest.IsArray(), "service samples are an array");
    struct Sample { std::string name; ext::Request request; std::set<std::string> required; };
    std::vector<Sample> samples;
    for (const auto& value : manifest.array)
    {
        const auto* name = value.Find("name"); const auto* path = value.Find("path");
        Require(name && name->IsString() && path && path->IsString(), "service sample has a name and path");
        Sample sample; sample.name = name->string;
        sample.request.paths = {std::filesystem::path(std::u8string(path->string.begin(), path->string.end())).wstring()};
        Require(GetFileAttributesW(sample.request.paths.front().c_str()) != INVALID_FILE_ATTRIBUTES, "service sample exists");
        if (const auto* background = value.Find("background")) sample.request.background = background->boolean;
        if (const auto* desktop = value.Find("desktop"); desktop && desktop->boolean) sample.request.context = ext::Context::Desktop;
        else if (sample.request.background) sample.request.context = ext::Context::FolderBackground;
        if (const auto* required = value.Find("required")) for (const auto& id : required->array) sample.required.insert(id.string);
        samples.push_back(std::move(sample));
    }
    Require(!samples.empty(), "service benchmark has actual samples");
    const auto selected = std::find_if(samples.begin(), samples.end(), [](const auto& sample) { return sample.name == "project-background"; });
    const auto& selection = selected == samples.end() ? samples.front() : *selected;
    const auto quote = [](std::string_view value) { return '"' + snowdesktop::json::EscapeString(value) + '"'; };
    const auto utf8 = [](std::wstring_view value) {
        const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        std::string bytes(length, '\0');
        if (length) WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), bytes.data(), length, nullptr, nullptr);
        return bytes;
    };
    const auto processCpu = [] {
        FILETIME created{}, exited{}, kernel{}, user{};
        Require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE, "read observer CPU");
        const auto ticks = [](FILETIME time) { return (std::uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
        return double(ticks(kernel) + ticks(user)) / 10000.0;
    };
    const auto childCount = [] {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        Require(snapshot != INVALID_HANDLE_VALUE, "inspect only this observer's child processes");
        PROCESSENTRY32W process{sizeof(process)}; unsigned count = 0;
        if (Process32FirstW(snapshot, &process)) do {
            if (process.th32ParentProcessID == GetCurrentProcessId()) ++count;
        } while (Process32NextW(snapshot, &process));
        CloseHandle(snapshot); return count;
    };
    struct Counters { unsigned queries = 0, scans = 0, sources = 0, registry = 0; };
    const auto traceCounters = [&] {
        Counters result;
        // TraceCapture keeps the caller's stable prefix, outside the cache.
        const auto prefix = trace.file.filename().wstring();
        const auto processSuffix = prefix.rfind(L'.', prefix.size() - 5);
        Require(processSuffix != std::wstring::npos, "trace filename has a process suffix");
        const auto stem = prefix.substr(0, processSuffix + 1);
        for (const auto& file : std::filesystem::directory_iterator(trace.file.parent_path()))
        {
            if (!file.is_regular_file() || !file.path().filename().wstring().starts_with(stem) || file.path().extension() != L".log") continue;
            std::ifstream input(file.path(), std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(input)), {});
            const auto count = [&](std::string_view token) {
                unsigned n = 0;
                for (size_t at = 0; (at = text.find(token, at)) != std::string::npos; at += token.size()) ++n;
                return n;
            };
            result.queries += count("phase=helper.stage reason=query context menu");
            result.scans += count("phase=catalogue reason=scan.start");
            result.sources += count("phase=catalogue.sources reason=completed");
            result.registry += count("phase=catalogue reason=stale.registry");
        }
        return result;
    };
    struct Observation
    {
        Clock::time_point started = Clock::now();
        double cpu = 0, firstCall = 0, sumCall = 0, maxCall = 0;
        unsigned calls = 0;
        Counters before;
    };
    const auto begin = [&] { Observation value; value.cpu = processCpu(); value.before = traceCounters(); return value; };
    const auto call = [](Observation& observation, auto&& action) {
        const auto started = Clock::now(); auto value = action();
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        if (!observation.calls) observation.firstCall = ms;
        ++observation.calls; observation.sumCall += ms; observation.maxCall = std::max(observation.maxCall, ms);
        return value;
    };
    const auto report = [&](std::string_view scenario, const Observation& observation, bool complete, size_t rows,
        size_t entries, size_t sources, bool sourcesOnly, bool required, std::string_view error,
        std::string_view stage = "result", bool fullSnapshot = false) {
        const auto counters = traceCounters();
        PROCESS_MEMORY_COUNTERS memory{sizeof(memory)};
        Require(K32GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)) != FALSE, "read observer working set");
        std::cout << "{\"scenario\":" << quote(scenario) << ",\"stage\":" << quote(stage)
            << ",\"elapsed_ms\":" << std::chrono::duration<double, std::milli>(Clock::now() - observation.started).count()
            << ",\"complete\":" << (complete ? "true" : "false") << ",\"rows\":" << rows << ",\"entries\":" << entries
            << ",\"sources\":" << sources << ",\"sources_only\":" << (sourcesOnly ? "true" : "false")
            << ",\"full_snapshot_ready\":" << (fullSnapshot ? "true" : "false")
            << ",\"required_ok\":" << (required ? "true" : "false") << ",\"error\":" << quote(error)
            << ",\"calls\":" << observation.calls << ",\"first_call_ms\":" << observation.firstCall
            << ",\"mean_call_ms\":" << (observation.calls ? observation.sumCall / observation.calls : 0)
            << ",\"max_call_ms\":" << observation.maxCall << ",\"driver_cpu_ms\":" << processCpu() - observation.cpu
            << ",\"driver_working_set\":" << memory.WorkingSetSize << ",\"helper_children\":" << childCount()
            << ",\"query_starts\":" << counters.queries - observation.before.queries
            << ",\"catalogue_scans\":" << counters.scans - observation.before.scans
            << ",\"source_scans\":" << counters.sources - observation.before.sources
            << ",\"registry_events\":" << counters.registry - observation.before.registry << "}" << std::endl;
    };
    const auto pumpFor = [](DWORD milliseconds) { const auto deadline = GetTickCount64() + milliseconds; while (GetTickCount64() < deadline) Pump(); };
    unsigned failures = 0;
    const auto ids = [](const ext::Catalogue& catalogue) {
        std::multiset<std::string> result;
        for (const auto& row : catalogue.rows) result.insert(row.id);
        return result;
    };
    const auto required = [&](const ext::MenuView& view, bool rootsOnly) {
        if (!view.snapshot || !view.snapshot->ok) return false;
        std::set<std::string> available;
        std::function<void(const std::vector<ext::Entry>&)> collect = [&](const auto& list) {
            for (const auto& entry : list) { available.insert(entry.provider); available.insert(utf8(entry.label)); collect(entry.children); }
        };
        collect(view.snapshot->entries);
        return std::all_of(selection.required.begin(), selection.required.end(), [&](const auto& id) {
            return (rootsOnly && !id.starts_with("verb:") && !id.starts_with("package:") && !id.starts_with("reg:")) || available.contains(id);
        }) && (!rootsOnly || std::all_of(view.snapshot->entries.begin(), view.snapshot->entries.end(),
            [](const auto& entry) { return entry.children.empty() && !entry.token; }));
    };
    const auto background = [&](ext::MenuService& service, std::string_view prefix) {
        for (const auto& sample : samples)
        {
            if (!service.MenuEnabled(sample.request, prefs)) continue;
            auto observation = begin();
            service.Prewarm(sample.request);
            ext::MenuView view; bool done = false; size_t readySources = 0, entries = 0;
            std::string error;
            const auto deadline = GetTickCount64() + 15000;
            do {
                view = call(observation, [&] { return service.MenuDisplay(sample.request, prefs); });
                done = !view.pending && (view.snapshot.has_value() || view.sourcesOnly);
                readySources = 0; entries = view.snapshot ? view.snapshot->entries.size() : 0; error = view.error;
                for (const auto& source : view.sources)
                {
                    const auto sourceView = service.View(source);
                    if (sourceView.pending || (!sourceView.snapshot && sourceView.error.empty())) done = false;
                    else ++readySources;
                    if (sourceView.snapshot) entries += sourceView.snapshot->entries.size();
                    if (!sourceView.error.empty()) error = sourceView.error;
                }
                if (!done) pumpFor(50);
            } while (!done && GetTickCount64() < deadline);
            // sources_only with zero sources is an intentional deferred aggregate,
            // not evidence that a complete visible menu has been prefetched.
            report(std::string(prefix) + ":" + sample.name, observation, done, readySources, entries,
                view.sources.size(), view.sourcesOnly, true, error, "result", view.snapshot.has_value());
            failures += !done || !error.empty();
        }
    };
    ext::MenuService service(workspace.path / L"cache"); service.Configure(prefs);
    background(service, "background-empty-inventory");
    // This is the default settings view: installed-source metadata only. Keep
    // its timing and completeness separate from actual command observations.
    auto installed = selection.request; installed.catalogueOnly = true;
    for (const auto scenario : {"extensions-cold", "extensions-refresh", "extensions-reopen"})
    {
        const bool force = std::string_view(scenario) == "extensions-refresh";
        auto observation = begin();
        auto view = call(observation, [&] { return service.Inspect(installed, force); });
        const auto deadline = GetTickCount64() + 30000;
        while (view.scanning && GetTickCount64() < deadline)
        {
            pumpFor(300);
            view = call(observation, [&] { return service.Inspect(installed); });
        }
        const bool correct = !view.scanning && !view.registrations.rows.empty() &&
            traceCounters().queries == observation.before.queries;
        report(scenario, observation, !view.scanning, view.registrations.rows.size(), 0, 0, false, correct, {}, "result", false);
        failures += !correct;
        if (std::string_view(scenario) == "extensions-cold")
        {
            std::cout << "{\"scenario\":\"extensions-cold\",\"stage\":\"software-groups\",\"groups\":[";
            bool first = true;
            for (const auto category : {ext::Category::Objects, ext::Category::Background})
                for (const auto& row : ext::ExtensionManagementRows(view.registrations, category))
                {
                    if (!first) std::cout << ','; first = false;
                    std::cout << "{\"id\":" << quote(row.id) << ",\"name\":" << quote(utf8(row.display.label))
                        << ",\"category\":" << static_cast<unsigned>(category) << ",\"members\":" << row.members.size()
                        << ",\"source_ids\":[";
                    bool firstSource = true;
                    for (const auto& id : row.members)
                    {
                        if (!firstSource) std::cout << ',';
                        firstSource = false; std::cout << quote(id.id);
                    }
                    std::cout << "]}";
                }
            std::cout << "]}" << std::endl;
        }
        if (std::string_view(scenario) != "extensions-cold") service.EndInspection();
    }
    const auto inspect = [&](std::string_view scenario, bool force) {
        auto metadata = selection.request; metadata.rootMetadataOnly = true;
        auto observation = begin();
        auto view = call(observation, [&] { return service.Inspect(selection.request, force); });
        bool selectedSeen = false, selectedCorrect = false;
        const auto observeSelection = [&] {
            if (selectedSeen) return;
            const auto target = service.View(metadata);
            if (target.pending || (!target.snapshot && target.error.empty())) return;
            selectedSeen = true;
            selectedCorrect = required(target, true);
            report(std::string(scenario) + ":selected-result", observation, true, view.catalogue.rows.size(),
                target.snapshot ? target.snapshot->entries.size() : 0, 0, false, selectedCorrect,
                target.error, "result", false);
        };
        observeSelection();
        auto nextProgress = GetTickCount64() + 5000;
        const auto deadline = GetTickCount64() + 180000;
        while ((view.scanning || view.menu.pending) && GetTickCount64() < deadline)
        {
            pumpFor(300); // Same observation cadence as the settings presenter.
            view = call(observation, [&] { return service.Inspect(selection.request, false); });
            observeSelection();
            if (GetTickCount64() >= nextProgress)
            {
                report(scenario, observation, false, view.catalogue.rows.size(), 0, 0, false, false, view.menu.error, "progress");
                nextProgress = GetTickCount64() + 5000;
            }
        }
        const bool complete = !view.scanning && !view.menu.pending;
        const auto target = service.View(metadata);
        // Full discovery can exceed the 64-entry selection cache. Settings
        // retains observed root metadata after LRU eviction; verify this exact
        // selection at its first completed reply rather than pinning its row
        // or requiring that transient snapshot to survive every type sample.
        const bool correct = complete && selectedSeen && selectedCorrect;
        report(scenario, observation, complete, view.catalogue.rows.size(), target.snapshot ? target.snapshot->entries.size() : 0,
            0, false, correct, view.menu.error, "result", false);
        std::cout << "{\"scenario\":" << quote(scenario) << ",\"stage\":\"identities\",\"ids\":[";
        bool first = true;
        for (const auto& id : ids(view.catalogue)) { if (!first) std::cout << ','; first = false; std::cout << quote(id); }
        std::cout << "]}" << std::endl;
        failures += !complete || view.catalogue.rows.empty() || !correct || !view.menu.error.empty();
        return view;
    };
    const auto cold = inspect("settings-cold", false);
    if (!cold.scanning && !cold.menu.pending)
    {
        auto observation = begin(); const auto baseline = ids(cold.catalogue); bool stable = true;
        ext::CatalogueView view;
        for (unsigned i = 0; i < 30; ++i)
        {
            view = call(observation, [&] { return service.Inspect(selection.request, false); });
            stable &= ids(view.catalogue) == baseline;
            pumpFor(100);
        }
        report("settings-warm-poll", observation, !view.scanning && !view.menu.pending, view.catalogue.rows.size(), 0, 0, false, stable, view.menu.error);
        failures += !stable;
        inspect("settings-explicit-refresh", true);
        service.EndInspection();
        pumpFor(500);
        inspect("settings-reopen", false);
    }
    else
        std::cout << "{\"scenario\":\"settings-refresh-and-reopen\",\"stage\":\"skipped\",\"reason\":\"cold discovery exceeded the 180s observation budget; the original 30s failure remains recorded\"}" << std::endl;
    service.EndInspection(); pumpFor(500);
    background(service, "background-known-inventory");
    {
        auto observation = begin(); service.QueryFullMenu(selection.request);
        ext::MenuView target;
        const auto deadline = GetTickCount64() + 20000;
        do { target = service.View(selection.request); if (!target.pending && target.snapshot) break; pumpFor(20); }
        while (GetTickCount64() < deadline);
        const bool correct = target.snapshot && !target.pending && required(target, false);
        report("actual-menu-full", observation, !target.pending, 0, target.snapshot ? target.snapshot->entries.size() : 0,
            0, false, correct, target.error, "result", correct);
        failures += !correct;
    }
    // Start a fresh explicit request, then close settings while work is pending.
    auto cancellation = begin(); service.Inspect(selection.request, true); pumpFor(250);
    service.Configure({});
    const auto closeStarted = Clock::now(); service.EndInspection();
    cancellation.firstCall = std::chrono::duration<double, std::milli>(Clock::now() - closeStarted).count();
    cancellation.calls = 1; cancellation.sumCall = cancellation.maxCall = cancellation.firstCall;
    pumpFor(3000);
    report("settings-close-cancel", cancellation, true, 0, 0, 0, false, true, "");
    auto disabled = begin(); service.Prewarm(selection.request); pumpFor(1500);
    const auto disabledCounters = traceCounters();
    const bool noWork = disabledCounters.queries == disabled.before.queries && disabledCounters.scans == disabled.before.scans;
    report("background-disabled", disabled, noWork, 0, 0, 0, false, noWork, ""); failures += !noWork;
    auto shutdown = begin(); service.Shutdown();
    const auto exitDeadline = GetTickCount64() + 5000;
    while (childCount() && GetTickCount64() < exitDeadline) pumpFor(50);
    const bool drained = childCount() == 0;
    report("service-shutdown", shutdown, drained, 0, 0, 0, false, drained, ""); failures += !drained;
    std::cout << "{\"scenario\":\"summary\",\"failures\":" << failures << "}" << std::endl;
    Require(!failures, "service observations meet completion, required-command, stability and cleanup checks");
}

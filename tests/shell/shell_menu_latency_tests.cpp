#include "shell/shell_extension_service.h"
#include "shell/shell_extension_menu_presentation.h"
#include "shell/shell_extension_discovery.h"
#include "common/json_string.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <objbase.h>
#include <stdexcept>
#include <map>
#include <set>

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
    std::map<std::pair<std::string, std::string>, std::set<std::string>> baseline;
    unsigned failures = 0;
    const auto record = [&](const Sample& sample, const std::string& mode, const char* phase, unsigned iteration,
        double ms, bool ok, DWORD worker, const std::string& error, const std::vector<ext::Entry>& entries) {
        std::set<std::string> identity, labels;
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

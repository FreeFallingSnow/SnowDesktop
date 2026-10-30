#include "shell/shell_extension_service.h"
#include "shell/shell_extension_menu_presentation.h"
#include "shell/shell_extension_discovery.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <objbase.h>
#include <stdexcept>

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
    Until([&] { return trace.Count("phase=preparation reason=ready") == 2; },
          "one completed real query allows one replacement standby worker", 10000);
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
    auto prepared = trace.Count("phase=preparation reason=ready");
    constexpr std::pair<const wchar_t *, const char *> samples[] = {
        {L".txt", "prepared-txt"}, {L".png", "prepared-png"},
        {L".zip", "prepared-zip"}, {L".pdf", "prepared-pdf"}};
    for (const auto &[type, scenario] : samples)
    {
        const auto request = workspace.File(L"selected-" + std::to_wstring(index++), type);
        const auto started = Clock::now();
        Until([&] { return trace.Count("phase=preparation reason=ready") > prepared; },
              "real single-use standby preparation finishes", 20000);
        prepared = trace.Count("phase=preparation reason=ready");
        const double preparationMs = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        open(request, scenario, preparationMs);
        open(request, "same-file-repeat", 0);
    }
}

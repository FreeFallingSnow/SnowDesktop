#include "shell/shell_extension_service.h"
#include "shell/shell_extension_menu_presentation.h"
#include "shell/shell_extension_discovery.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <objbase.h>
#include <stdexcept>
#include <thread>

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
ext::Reply Result()
{
    ext::Entry entry; entry.provider = "verb:latency-test"; entry.key = "latency-test";
    entry.label = L"Exact selected file action"; entry.token = 17;
    return {true, {entry}, {}};
}
ext::Preferences TestPreferences()
{
    ext::Preferences prefs;
    ext::SetCommon(prefs, "verb:latency-test", ext::Category::Objects, true);
    return prefs;
}
}

void TestShellMenuSelectionLatency()
{
    Workspace workspace;
    const auto selected = workspace.File(L"selected");
    const auto disabled = workspace.File(L"disabled");
    std::atomic<unsigned> queries = 0;
    ext::MenuService service(workspace.path / L"cache", [&](const ext::Request &) {
        ++queries;
        const auto readyAt = Clock::now() + std::chrono::milliseconds(40);
        return ext::QueryWork{[readyAt]() -> std::optional<ext::Reply> {
            return Clock::now() >= readyAt ? std::optional(Result()) : std::nullopt;
        }, {}};
    }, [] { return ext::Catalogue{}; });
    const auto prefs = TestPreferences(); service.Configure(prefs);
    service.Prewarm(selected);
    Until([&] { const auto view = service.MenuDisplay(selected, prefs); return view.snapshot && !view.pending; },
          "a stable local file must produce its exact selection snapshot before a popup opens");
    Require(queries == 1, "selection prewarm queries the selected file once");
    ext::Presentation popup(selected, prefs, L"", L"", service);
    std::vector<menu::Item> items; menu::Options options;
    popup.Attach(items, options, 0);
    Require(items.size() == 1 && items.front().label == Result().entries.front().label && !options.pollItems,
            "the popup immediately consumes the prewarmed exact selection without another helper");
    Require(queries == 1, "opening an already prepared selection does not restart its query");
    service.Configure({}); service.Prewarm(disabled);
    auto barrier = disabled; barrier.paths = {workspace.path.wstring()};
    service.Query(barrier, ext::QueryPriority::Inspect, true);
    Until([&] { return service.View(barrier).snapshot.has_value(); }, "disabled selection prewarm barrier completes");
    Require(queries == 2 && !service.View(disabled).snapshot,
            "turning extensions off suppresses new file prewarm while explicit inspection remains available");
}

void BenchmarkShellMenuFirstScreen()
{
    Workspace workspace;
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
    ext::MenuService service(workspace.path / L"cache"); service.Configure(prefs);
    auto open = [&](const ext::Request &request, const char *scenario, double preparationMs) {
        const auto started = Clock::now();
        ext::Presentation popup(request, prefs, L"", L"", service);
        std::vector<menu::Item> items; menu::Options options;
        popup.Attach(items, options, 0);
        const auto elapsed = [&] { return std::chrono::duration<double, std::milli>(Clock::now() - started).count(); };
        double firstMs = items.empty() ? -1 : elapsed();
        const auto deadline = GetTickCount64() + 20000;
        while (options.pollItems && (!options.pollItemsFinished || !options.pollItemsFinished()) && GetTickCount64() < deadline)
        {
            if (auto update = options.pollItems(items, true)) items = std::move(*update);
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
    for (const auto type : {L".txt", L".png", L".zip", L".pdf"})
    {
        const auto request = workspace.File(L"selected-" + std::to_wstring(index++), type);
        const auto started = Clock::now(); service.Prewarm(request);
        Until([&] { const auto view = service.MenuDisplay(request, prefs); return view.snapshot && !view.pending; },
              "real stable selection finishes its exact prewarm", 20000);
        const double preparationMs = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        open(request, "selected-file", preparationMs);
        open(request, "selected-repeat", 0);
    }
}

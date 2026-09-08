#include "performance_capture.h"
#include "performance_trace.h"
#include "json_value.h"
#include "lua_runtime.h"

extern "C" {
#include <lauxlib.h>
}

#include <chrono>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace
{
namespace perf = snowdesktop::performance;
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
double Number(const JsonValue& value, std::string_view key)
{
    const auto* item = value.Find(key);
    Check(item && item->IsNumber(), "expected numeric report field");
    return item->number;
}
std::string String(const JsonValue& value, std::string_view key)
{
    const auto* item = value.Find(key);
    Check(item && item->IsString(), "expected string report field");
    return item->string;
}
JsonValue Read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    JsonValue value;
    std::string error;
    Check(ParseJson(text, value, &error), "completed report must parse as JSON");
    return value;
}
const JsonValue& Find(const JsonValue& report, std::string_view collection,
    std::string_view module, std::string_view phase)
{
    const auto* values = report.Find(collection);
    Check(values && values->IsArray(), "report collection must exist");
    for (const auto& value : values->array)
        if (String(value, "module") == module && String(value, "phase") == phase)
            return value;
    throw std::runtime_error("expected instrumented report row");
}

LRESULT Request(HWND window, std::wstring payload)
{
    COPYDATASTRUCT data{};
    data.dwData = perf::CopyDataTag;
    data.cbData = static_cast<DWORD>((payload.size() + 1) * sizeof(wchar_t));
    data.lpData = payload.data();
    return perf::HandleControlMessage(window, WM_COPYDATA, 0,
        reinterpret_cast<LPARAM>(&data), nullptr, nullptr);
}
void ExerciseCoupledScopes()
{
    perf::Scope settings("settings", "apply");
    {
        perf::Scope event("widget.event", "timer", L"widget-a", 42);
        perf::Value("widget.memory", "lua_bytes", L"widget-a", 256);
        perf::DrawLink("desktop", L"widget-a", false);
        auto* lua = luaL_newstate();
        Check(lua != nullptr, "allocate test Lua state");
        LuaRuntimeQuota quota;
        lua_pushlightuserdata(lua, &quota);
        lua_setfield(lua, LUA_REGISTRYINDEX, "__quota_ptr");
        Check(luaL_loadstring(lua, "local n=0; for i=1,1000 do n=n+i end; return n") == LUA_OK,
            "load fixture Lua");
        Check(snowdesktop::lua_runtime::ProtectedCall(lua, 0, 1) == LUA_OK,
            "profiling must preserve Lua execution and quota hooks");
        Check(lua_tointeger(lua, -1) == 500500, "profiling must preserve Lua results");
        lua_close(lua);
        { perf::Scope shared("shared.system", "sample"); }
    }
    {
        perf::Scope event("widget.event", "data", L"widget-b", 43);
        perf::DrawLink("desktop", L"widget-b", false);
    }
    {
        perf::Scope batch("composition.shared", "flush");
        { perf::Scope draw("widget.composition", "draw-a", L"widget-a");
          perf::DrawLink("desktop", L"widget-a", true); }
        { perf::Scope draw("widget.composition", "draw-b", L"widget-b");
          perf::DrawLink("desktop", L"widget-b", true); }
    }
    std::thread worker([] { perf::Scope task("task.worker", "independent", L"widget-b", 43); });
    worker.join();
}

LRESULT CALLBACK FixtureWindow(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (perf::IsControlMessage(message, wp, lp))
        return perf::HandleControlMessage(window, message, wp, lp, nullptr, nullptr);
    if (message == WM_TIMER && wp == 1)
    {
        ExerciseCoupledScopes();
        return 0;
    }
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { perf::Shutdown(); PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wp, lp);
}
}

// An invisible control-only fixture for exercising the actual PowerShell
// protocol. It never starts or operates SnowDesktop's desktop host.
int RunPerformanceControlFixture()
{
    WNDCLASSW type{};
    type.lpfnWndProc = FixtureWindow;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"SnowDesktopControlWindow";
    if (!RegisterClassW(&type)) return 2;
    HWND window = CreateWindowW(type.lpszClassName, L"SnowDesktopControl",
        WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, type.hInstance, nullptr);
    if (!window) return 3;
    SetTimer(window, 1, 100, nullptr);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

void RunPerformanceCaptureTests()
{
    const auto root = std::filesystem::temp_directory_path() /
        (L"SnowDesktopPerfTests-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directory(root);
    std::string error;
    Check(!perf::Enabled() && perf::Status() == perf::ProtocolIdle,
        "profiling must be disabled by default");
    { perf::Scope disabled("off", "no-recording"); }
    perf::Value("off", "no-recording", {}, 1);
    Check(std::filesystem::is_empty(root), "disabled probes must create no files");

    perf::CaptureOptions options{ "coupled-test", root / L"coupled.json", 60, 2048 };
    Check(perf::Start(options, error), "start explicit capture");
    Check(!perf::Start(options, error), "concurrent session must be rejected");
    Check(!perf::RequestStop("someone-else"), "a stale session must not stop another capture");
    ExerciseCoupledScopes();
    Check(perf::RequestStop(options.session), "owner can request stop");
    perf::Shutdown();
    Check(!perf::Enabled() && perf::Status() == perf::ProtocolIdle,
        "stop must disable hooks and publish a completed report");
    const auto report = Read(options.output);
    Check(Number(report, "schemaVersion") == 1, "report schema must be explicit");
    const auto& parent = Find(report, "events", "widget.event", "timer");
    const auto& lua = Find(report, "events", "lua", "protectedCall");
    Check(Number(lua, "parent") == Number(parent, "id"), "Lua must link to its owning callback");
    Check(String(lua, "owner") == "widget-a" && Number(lua, "correlation") == 42,
        "Lua must inherit instance and task context");
    Check(Number(parent, "selfWallMs") <= Number(parent, "wallMs") &&
        Number(parent, "wallMs") - Number(parent, "selfWallMs") >= Number(lua, "wallMs") - 0.001,
        "nested elapsed time must not be double-counted as self time");
    Check(String(Find(report, "events", "shared.system", "sample"), "owner").empty(),
        "shared work must not be assigned to the caller widget");
    Check(Number(Find(report, "events", "task.worker", "independent"), "parent") == 0,
        "unrelated worker threads must not inherit synchronous parents");
    const auto& origin = Find(report, "events", "widget.draw.source", "desktop");
    Check(Number(origin, "value") == Number(parent, "id"),
        "deferred draw must preserve its originating callback");
    Check(Number(origin, "parent") == Number(Find(report, "events", "widget.composition", "draw-a"), "id"),
        "causal link must point to the draw that consumes it");
    Check(!perf::Start(options, error), "existing reports must never be overwritten");

    options = { "bounded-test", root / L"bounded.json", 60, 1 };
    Check(perf::Start(options, error), "start bounded capture");
    for (int i = 0; i < 20; ++i) perf::Value("bounded", "counter", L"owner", i);
    auto spanning = std::make_unique<perf::Scope>("old-session", "unfinished");
    perf::Shutdown();
    const auto bounded = Read(options.output);
    Check(bounded.Find("events")->array.size() == 1 && Number(bounded, "droppedEvents") >= 19,
        "timeline must be bounded and truncation explicit");
    Check(Number(Find(bounded, "groups", "bounded", "counter"), "count") == 20,
        "aggregates must continue after timeline fills");
    options = { "next-test", root / L"next.json", 1, 2048 };
    Check(perf::Start(options, error), "restart capture");
    { perf::Scope next("new-session", "scope"); }
    spanning.reset();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (perf::Status() == perf::ProtocolRecording || perf::Status() == perf::ProtocolFinishing)
    {
        Check(std::chrono::steady_clock::now() < deadline, "capture must stop by itself");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    perf::Shutdown();
    const auto next = Read(options.output);
    Check(Number(Find(next, "events", "new-session", "scope"), "parent") == 0,
        "new session must not inherit stale scopes");
    for (const auto& event : next.Find("events")->array)
        Check(String(event, "module") != "old-session", "old scopes must not leak into a new session");
    Check(String(next, "stopReason") == "duration", "automatic stop must be distinguished from requested stop");

    options = { "summary-test", root / L"summary.json", 60, 1,
        perf::CaptureMode::Summary, false };
    Check(perf::Start(options, error), "start bounded summary capture");
    ExerciseCoupledScopes();
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 4; ++thread)
        threads.emplace_back([] {
            for (int i = 0; i < 200; ++i)
            {
                perf::Scope outer("summary", "parallel", L"shared-owner");
                perf::Scope inner("lua", "nested");
            }
        });
    for (auto& thread : threads) thread.join();
    auto summarySpanning = std::make_unique<perf::Scope>("summary-old", "spanning");
    perf::Shutdown();
    const auto summary = Read(options.output);
    Check(String(summary, "captureMode") == "summary" &&
            String(summary, "timelinePolicy") == "none" &&
            summary.Find("events")->array.empty() && Number(summary, "droppedEvents") == 0,
        "summary must intentionally omit timeline without reporting truncation");
    const auto& merged = Find(summary, "groups", "summary", "parallel");
    const auto& summaryLua = Find(summary, "groups", "lua", "nested");
    Check(Number(merged, "count") == 800 && Number(summaryLua, "count") == 800 &&
            String(summaryLua, "owner") == "shared-owner" &&
            Number(merged, "cpuSamples") == 0 &&
            Number(merged, "wallMs") - Number(merged, "selfWallMs") >=
                Number(summaryLua, "wallMs") - 0.001,
        "thread-local aggregates must merge counts and preserve owner and nested self time");
    Check(Number(Find(summary, "groups", "widget.memory", "lua_bytes"), "last") == 256,
        "summary must retain resource gauges");

    options = { "summary-stop", root / L"summary-stop.json", 60, 1,
        perf::CaptureMode::Summary, false };
    Check(perf::Start(options, error), "start summary stop race");
    std::atomic<bool> entered{ false }, release{ false };
    std::thread crossing([&] {
        { perf::Scope completed("summary-stop", "completed", L"worker"); }
        perf::Scope crossingScope("summary-stop", "crossing", L"worker");
        entered.store(true);
        while (!release.load()) std::this_thread::yield();
    });
    while (!entered.load()) std::this_thread::yield();
    perf::Shutdown();
    release.store(true);
    crossing.join();
    const auto stoppedSummary = Read(options.output);
    Check(Number(Find(stoppedSummary, "groups", "summary-stop", "completed"), "count") == 1,
        "a worker buffer must survive stop while an old scope still owns it");
    for (const auto& group : stoppedSummary.Find("groups")->array)
        Check(String(group, "phase") != "crossing", "unfinished summary scopes must be omitted");

    options = { "trace-no-cpu", root / L"trace-no-cpu.json", 60, 2048,
        perf::CaptureMode::Trace, false };
    Check(perf::Start(options, error), "switch summary to trace without thread CPU");
    ExerciseCoupledScopes();
    summarySpanning.reset();
    perf::Shutdown();
    const auto trace = Read(options.output);
    Check(Number(Find(trace, "groups", "lua", "protectedCall"), "cpuSamples") == 0 &&
            !Find(trace, "events", "lua", "protectedCall").Find("cpuAvailable")->boolean,
        "disabled scope CPU must be unavailable while trace retains causality");
    Check(Number(Find(trace, "events", "settings", "apply"), "parent") == 0,
        "a trace session must not inherit an old summary scope");

    options = { "summary-invalid", root / L"invalid-summary.json", 1, 10,
        perf::CaptureMode::Summary, true };
    Check(!perf::Start(options, error), "summary cannot silently enable expensive scope CPU");

    options = { "summary-bounds", root / L"summary-bounds.json", 60, 1,
        perf::CaptureMode::Summary, false };
    Check(perf::Start(options, error), "start bounded summary groups");
    for (int i = 0; i < 4200; ++i)
    { perf::Scope scope("bounded-summary", std::to_string(i), L"owner"); }
    perf::Shutdown();
    const auto summaryBounds = Read(options.output);
    Check(summaryBounds.Find("groups")->array.size() <= 4096 &&
            Number(summaryBounds, "droppedGroups") > 0,
        "summary labels from dynamic callers must have a bounded memory budget");

    Check(Request(nullptr, L"2\nstart\ninvalid\n1\nignored.json") == 0,
        "incomplete v2 control payload must be rejected");
    Check(Request(nullptr, L"1\nstart\ninvalid\n601\nignored.json") == 0,
        "unbounded capture duration must be rejected");
    COPYDATASTRUCT malformed{ perf::CopyDataTag, 3, const_cast<char*>("bad") };
    Check(perf::HandleControlMessage(nullptr, WM_COPYDATA, 0,
        reinterpret_cast<LPARAM>(&malformed), nullptr, nullptr) == 0,
        "malformed IPC payload must be rejected");
    // Remove only this test's individually named files and empty directory.
    for (const auto& entry : std::filesystem::directory_iterator(root))
        std::filesystem::remove(entry.path());
    std::filesystem::remove(root);
}

// Explicit benchmark, not a timing-sensitive pass/fail test. Export happens
// after the measured loop. Preserve each report for counts/bounds auditing.
int RunPerformanceOverheadBenchmark(const char* directory)
{
    const auto root = std::filesystem::absolute(directory);
    if (std::filesystem::exists(root)) return 2;
    std::filesystem::create_directories(root);
    constexpr int iterations = 10000;
    std::uint64_t result = 0;
    for (int round = 0; round < 3; ++round)
    {
        for (int step = 0; step < 4; ++step)
        {
            // Rotate order so a consistently earlier warmup is not favored.
            const int mode = (step + round) % 4;
            const char* name = mode == 0 ? "off" : mode == 1 ? "summary" :
                mode == 2 ? "trace" : "trace-cpu";
            std::string error;
            const std::string session = std::string(name) + "-" + std::to_string(round);
            perf::CaptureOptions options{ session, root / (session + ".json"), 60, 65536,
                mode == 1 ? perf::CaptureMode::Summary : perf::CaptureMode::Trace, mode == 3 };
            if (mode && !perf::Start(options, error)) return 3;
            // Let the one-time sampler initialization finish before timing.
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const auto begin = std::chrono::steady_clock::now();
            for (int i = 0; i < iterations; ++i)
            {
                perf::Scope event("widget.event", "frame", L"benchmark-widget-123456");
                { perf::Scope lua("lua", "protectedCall"); result = result * 33 + i; }
                { perf::Scope invalidate("widget.invalidate", "desktop", L"benchmark-widget-123456");
                  perf::DrawLink("desktop", L"benchmark-widget-123456", false); }
                { perf::Scope draw("widget.composition", "draw", L"benchmark-widget-123456");
                  perf::DrawLink("desktop", L"benchmark-widget-123456", true); }
            }
            const double elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - begin).count();
            if (mode) perf::Shutdown();
            std::cout << "{\"mode\":\"" << name << "\",\"round\":" << round
                << ",\"iterations\":" << iterations << ",\"scopeCount\":" << iterations * 4
                << ",\"elapsedMs\":" << elapsed << ",\"result\":" << result << "}\n";
        }
    }
    return 0;
}

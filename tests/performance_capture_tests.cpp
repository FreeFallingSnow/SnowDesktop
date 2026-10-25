#include "diagnostics/performance_capture.h"
#include "diagnostics/performance_trace.h"
#include "common/json_value.h"
#include "widget/runtime/lua_runtime.h"

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
#include <vector>

namespace
{
namespace perf = snowdesktop::performance;
std::filesystem::path fixtureRoot;
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
    if (message == RegisterWindowMessageW(perf::ControlMessageName))
        perf::ConfigureControl(window, fixtureRoot);
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
    fixtureRoot = std::filesystem::temp_directory_path() / (L"SnowDesktopControlFixture-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
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

namespace
{
perf::CaptureSnapshot WaitFor(std::string_view session, bool terminal)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < end)
    {
        auto snapshot = perf::Snapshot(session);
        if (snapshot.phase == perf::CapturePhase::Failed) return snapshot;
        if (terminal ? snapshot.phase == perf::CapturePhase::Completed : snapshot.phase == perf::CapturePhase::Recording) return snapshot;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("capture state deadline expired");
}
perf::CaptureOptions Options(const std::filesystem::path& root, std::string id)
{
    perf::CaptureOptions result;
    result.session = id; result.requestId = id;
    result.output = root / id / "trace.pftrace";
    result.seconds = 60; result.preset = perf::CapturePreset::Interaction;
    return result;
}
void ExerciseCli(const std::filesystem::path& root)
{
    fixtureRoot = root / L"cli-managed";
    const auto scripts = std::filesystem::path(SNOWDESKTOP_PERFORMANCE_SOURCE_ROOT) / L"scripts";
    WNDCLASSW type{};
    type.lpfnWndProc = FixtureWindow; type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"SnowDesktopControlWindow";
    Check(RegisterClassW(&type) != 0, "register isolated control-only fixture");
    const auto window = CreateWindowW(type.lpszClassName, L"SnowDesktopControl", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, type.hInstance, nullptr);
    Check(window != nullptr, "create isolated CLI control endpoint");
    SetTimer(window, 1, 100, nullptr);
    auto executable = std::filesystem::path(L"C:/Program Files/PowerShell/7/pwsh.exe");
    if (!std::filesystem::exists(executable))
    {
        wchar_t system[32768]{}; GetSystemDirectoryW(system, static_cast<UINT>(std::size(system)));
        executable = std::filesystem::path(system) / L"WindowsPowerShell/v1.0/powershell.exe";
    }
    const auto output = root / L"cli-export";
    std::wstring command = L"\"" + executable.wstring() + L"\" -NoProfile -ExecutionPolicy Bypass -File \"" +
        (scripts / L"profile.ps1").wstring() + L"\" capture -Seconds 1 -Preset interaction -Offline -Session cli-fixture -TargetProcessId " +
        std::to_wstring(GetCurrentProcessId()) + L" -OutputDirectory \"" + output.wstring() + L"\"";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child) != FALSE, "launch actual CLI against isolated host");
    const auto deadline = GetTickCount64() + 45000;
    while (WaitForSingleObject(child.hProcess, 0) != WAIT_OBJECT_0 && GetTickCount64() < deadline)
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        MsgWaitForMultipleObjects(1, &child.hProcess, FALSE, 20, QS_ALLINPUT);
    }
    const auto wait = WaitForSingleObject(child.hProcess, 0);
    if (wait != WAIT_OBJECT_0) { TerminateProcess(child.hProcess, 124); WaitForSingleObject(child.hProcess, 5000); }
    DWORD exitCode = 1; GetExitCodeProcess(child.hProcess, &exitCode);
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
    KillTimer(window, 1); DestroyWindow(window);
    MSG ignored{}; PeekMessageW(&ignored, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE);
    Check(wait == WAIT_OBJECT_0 && exitCode == 0, "real CLI, authenticated pipe, duration, export and fixed SQL must complete");
    bool summaryFound = false;
    for (const auto& entry : std::filesystem::directory_iterator(output))
        if (entry.is_directory() && entry.path().filename().wstring().starts_with(L"analysis-"))
        {
            const auto summary = Read(entry.path()/L"summary.json");
            Check(summary.Find("quality")->Find("valid")->boolean, "complete native known-answer CLI evidence is valid");
            const auto* scopes = summary.Find("scopes");
            Check(scopes && scopes->IsArray() && !scopes->array.empty(), "SQL report contains real Lua/callback activity");
            summaryFound = true;
        }
    Check(summaryFound, "CLI must publish machine-readable summary with queries and CSV");
}
}
void AssertNativeTraceSql(const std::filesystem::path&, std::string_view);
void RunPerformanceCaptureTests()
{
    const auto root = std::filesystem::temp_directory_path() /
        (L"SnowDesktopPerfTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directory(root);
    std::string error;
    Check(!perf::Enabled() && perf::Status() == perf::ProtocolIdle, "profiling defaults off");
    { perf::Scope disabled("off", "no-recording"); }
    perf::Value("off", "no-recording", {}, 1);
    Check(std::filesystem::is_empty(root), "disabled probes create no files");
    auto options = Options(root, "coupled-test"); options.scopeCpu = true;
    Check(perf::Start(options, error), "accept capture on worker");
    Check(perf::Start(options, error), "same request id returns same accepted operation");
    auto busy = Options(root, "concurrent");
    Check(!perf::Start(busy, error) && error == "captureBusy", "independent concurrent capture rejected");
    const auto recording = WaitFor(options.session, false);
    Check(recording.phase == perf::CapturePhase::Recording, "SDK must acknowledge before recording");
    Check(!perf::RequestStop("someone-else") && !perf::RequestStop(options.session, recording.generation + 1), "stale identity cannot stop capture");
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    ExerciseCoupledScopes();
    Check(perf::RequestStop(options.session, recording.generation), "matching generation can stop");
    const auto completed = WaitFor(options.session, true);
    Check(completed.phase == perf::CapturePhase::Completed && !perf::Enabled(), "terminal only after native export and manifest");
    const auto manifest = Read(completed.manifest);
    Check(Number(manifest, "schemaVersion") == 2 && String(manifest, "format") == "perfetto", "explicit native format schema");
    Check(manifest.Find("completion")->boolean && String(manifest, "traceSha256").size() == 64, "durable evidence fingerprint");
    AssertNativeTraceSql(completed.trace,
        "SELECT ((SELECT COUNT(*) FROM slice c JOIN slice p ON c.parent_id=p.id WHERE c.name='protectedCall' AND p.name='timer')=1"
        " AND (SELECT COUNT(*) FROM slice WHERE name='sample' AND EXTRACT_ARG(arg_set_id,'debug.owner')=0)=1"
        " AND (SELECT COUNT(*) FROM slice WHERE name='independent' AND parent_id IS NULL)=1"
        " AND (SELECT COUNT(*) FROM flow)=2"
        " AND (SELECT COUNT(DISTINCT EXTRACT_ARG(arg_set_id,'debug.owner')) FROM slice WHERE name IN ('timer','protectedCall'))=1) AS passed;");
    Check(perf::Start(options, error), "completed request replay remains idempotent");
    auto conflict = options; ++conflict.seconds;
    Check(!perf::Start(conflict, error) && error == "requestIdConflict", "idempotency cannot change configuration");
    auto next = Options(root, "next-test"); next.seconds = 1;
    Check(perf::Start(next, error), "restart capture");
    Check(WaitFor(next.session, false).phase == perf::CapturePhase::Recording, "new session recording");
    Check(perf::RequestStop(options.session, recording.generation), "old stop returns old terminal result");
    Check(perf::Snapshot(next.session).phase == perf::CapturePhase::Recording, "old stop cannot stop new capture");
    { perf::Scope event("widget.event", "next"); }
    Check(WaitFor(next.session, true).phase == perf::CapturePhase::Completed, "duration stops without a UI timer");
    AssertNativeTraceSql(next.output, "SELECT ((SELECT COUNT(*) FROM slice WHERE name='next')=1 AND (SELECT COUNT(*) FROM slice WHERE thread_dur IS NOT NULL)=0) AS passed;");
    auto failed = Options(root, "export-limit"); failed.maximumFileBytes = 1024;
    Check(perf::Start(failed, error), "accept size-bound session");
    Check(WaitFor(failed.session, false).phase == perf::CapturePhase::Recording, "size-bound session recording");
    for (int i = 0; i < 2000; ++i) { perf::Scope event("widget.event", "overflow"); }
    perf::RequestStop(failed.session);
    Check(WaitFor(failed.session, true).phase == perf::CapturePhase::Failed && !std::filesystem::exists(failed.output), "export cap never publishes truncated evidence as success");
    Check(std::filesystem::exists(failed.output.wstring() + L".partial") && !Read(failed.output.parent_path()/L"session.json").Find("completion")->boolean, "failure evidence retained");
    auto invalid = Options(root, "invalid"); invalid.output = root / "bad.json";
    Check(!perf::Start(invalid, error), "legacy output extension rejected");
    const auto protocol = perf::ProcessControlRequest(R"({"protocolVersion":3,"requestId":"test-status","command":"status","sessionId":"coupled-test"})");
    JsonValue reply; Check(ParseJson(protocol, reply, &error), "control returns JSON");
    Check(String(reply, "requestId") == "test-status" && String(reply, "state") == "completed", "status queries retain per-session result and echo request identity");
    const auto rejected = perf::ProcessControlRequest(R"({"protocolVersion":3,"requestId":"bad-output","command":"start","output":"C:/arbitrary"})");
    Check(rejected.find("unsupportedControlField") != rejected.npos, "external clients cannot select arbitrary server output paths");
    COPYDATASTRUCT malformed{ perf::CopyDataTag, 3, const_cast<char*>("bad") };
    Check(perf::HandleControlMessage(nullptr, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&malformed), nullptr, nullptr) == 0, "deprecated unauthenticated writes rejected");
    perf::Shutdown();
    ExerciseCli(root);
    std::cout << "Capture fixtures retained for offline analysis: " << root.string() << '\n';
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
            const char* name = mode == 0 ? "off" : mode == 1 ? "idle" :
                mode == 2 ? "interaction" : "interaction-cpu";
            std::string error;
            const std::string session = std::string(name) + "-" + std::to_string(round);
            auto options = Options(root, session);
            options.preset = mode == 1 ? perf::CapturePreset::Resource : perf::CapturePreset::Interaction;
            options.scopeCpu = mode == 3;
            if (mode && !perf::Start(options, error)) return 3;
            if (mode && WaitFor(session, false).phase != perf::CapturePhase::Recording) return 4;
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

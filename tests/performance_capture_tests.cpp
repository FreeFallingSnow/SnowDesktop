#include "diagnostics/performance_capture.h"
#include "diagnostics/performance_trace.h"
#include "common/json_value.h"
#include "widget/runtime/lua_runtime.h"
#include "diagnostics/perfetto_session.h"
#include <psapi.h>
#include <tlhelp32.h>

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
struct CaptureCleanup { ~CaptureCleanup() { perf::Shutdown(); } };
struct IsolatedShellModulePath
{
    std::wstring original;
    IsolatedShellModulePath()
    {
        const auto size = GetEnvironmentVariableW(L"PSModulePath", nullptr, 0);
        if (size) { original.resize(size); original.resize(GetEnvironmentVariableW(L"PSModulePath", original.data(), size)); }
        if (!SetEnvironmentVariableW(L"PSModulePath", L"C:\\SnowDesktopFixture\\PowerShell7Only"))
            throw std::runtime_error("set isolated module-path fixture");
    }
    ~IsolatedShellModulePath() { SetEnvironmentVariableW(L"PSModulePath", original.empty() ? nullptr : original.c_str()); }
};
void Check(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << std::endl; throw std::runtime_error(message); }
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
void ExercisePerformanceWorkloads(const std::filesystem::path&);
void RunPerformanceCaptureTests()
{
    CaptureCleanup cleanup;
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
    for (const auto* name : {L"trace:stream.pftrace", L"CON.pftrace", L"trace .pftrace:stream", L"LPT1.pftrace"})
    {
        invalid.output = root / name;
        Check(!perf::Start(invalid, error) && error == "invalidOutputPath", "capture output rejects alternate streams and device aliases");
    }
    auto filtered = Options(root, "request-identity"); filtered.selectedInstances = {L"a", L"b"}; filtered.seconds = 1;
    Check(perf::Start(filtered, error), "accept bounded filtered capture");
    auto ambiguous = filtered; ambiguous.selectedInstances = {L"a|b"};
    Check(!perf::Start(ambiguous, error) && error == "requestIdConflict", "delimiter ambiguity must not replay different selected instances");
    Check(WaitFor(filtered.session, true).phase == perf::CapturePhase::Completed, "persistent worker finishes queued capture");
    Check(perf::Start(options, error), "historical replay compares original parameters after later captures");
    auto workload = Options(root, "real-workload"); workload.scopeCpu = true;
    Check(perf::Start(workload, error), "accept real worker/scheduler workload capture");
    Check(WaitFor(workload.session, false).phase == perf::CapturePhase::Recording, "workload starts after SDK ack");
    ExercisePerformanceWorkloads(root);
    perf::RequestStop(workload.session);
    Check(WaitFor(workload.session, true).phase == perf::CapturePhase::Completed, "real workload evidence publishes");
    AssertNativeTraceSql(workload.output,
        "SELECT ((SELECT COUNT(*) FROM slice WHERE name='task.queued')=1"
        " AND (SELECT COUNT(*) FROM slice WHERE name='filesystem' AND EXTRACT_ARG(arg_set_id,'debug.module')='task.worker')=1"
        " AND (SELECT COUNT(*) FROM slice WHERE name='task.completed')=1"
        " AND (SELECT COUNT(*) FROM slice WHERE name='animation.lifetime' AND dur>0)=2"
        " AND (SELECT COUNT(*) FROM slice WHERE name='animation.update')=8"
        " AND (SELECT MAX(EXTRACT_ARG(arg_set_id,'debug.late_ms')) FROM slice WHERE name='animation.update')>5"
        " AND (SELECT COUNT(*) FROM slice WHERE name='animation.presentation')=4"
        " AND (SELECT COUNT(*) FROM slice WHERE name='animation.commit')=4) AS passed;");
    {
        IsolatedShellModulePath modulePath;
        Check(perf::RequestAnalysis(workload.session, perf::Snapshot(workload.session).generation, error), "host controller queues fixed offline analyzer");
        const auto analysisDeadline = GetTickCount64() + 60000;
        perf::CaptureSnapshot analyzed;
        do
        {
            analyzed = perf::Snapshot(workload.session);
            if (analyzed.analysis == "completed" || analyzed.analysis == "failed") break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (GetTickCount64() < analysisDeadline);
        Check(analyzed.analysis == "completed" && analyzed.quality == "valid" && !analyzed.report.empty(), "actual worker process publishes report and independent quality");
        wchar_t inheritedModulePath[256]{};
        Check(GetEnvironmentVariableW(L"PSModulePath", inheritedModulePath, 256) > 0 &&
            std::wstring_view(inheritedModulePath) == L"C:\\SnowDesktopFixture\\PowerShell7Only",
            "analysis launch must not mutate its parent module environment");
        const auto analyzedSummary = Read(analyzed.report.parent_path() / L"summary.json");
        const auto* instanceRows = analyzedSummary.Find("widgets");
        bool nestedDeduplicated = false;
        if (instanceRows && instanceRows->IsArray()) for (const auto& row : instanceRows->array)
            if (Number(row, "recordedCalls") == 2 && Number(row, "rootScopes") == 1) nestedDeduplicated = true;
        Check(nestedDeduplicated, "production SQL excludes nested same-instance CPU roots rather than summing both");
    }
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
    CaptureCleanup cleanup;
    const auto root = std::filesystem::absolute(directory);
    if (std::filesystem::exists(root)) return 2;
    std::filesystem::create_directories(root);
    constexpr int iterations = 10000;
    const auto cpuTime = [] {
        FILETIME created{}, exited{}, kernel{}, user{};
        Check(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE, "benchmark process CPU available");
        const auto value = [](FILETIME time) { return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
        return static_cast<double>(value(kernel) + value(user)) / 10000.0;
    };
    const auto memory = [] {
        PROCESS_MEMORY_COUNTERS_EX value{}; value.cb = sizeof(value);
        Check(K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&value), sizeof(value)) != FALSE,
            "benchmark process memory available"); return value;
    };
    const auto threads = [] {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        Check(snapshot != INVALID_HANDLE_VALUE, "benchmark thread inventory available");
        THREADENTRY32 entry{}; entry.dwSize = sizeof(entry); unsigned count = 0;
        if (Thread32First(snapshot, &entry)) do { if (entry.th32OwnerProcessID == GetCurrentProcessId()) ++count; } while (Thread32Next(snapshot, &entry));
        CloseHandle(snapshot); return count;
    };
    const auto run = [&](int mode, int round, const char* name) {
        LuaRuntimeQuota quota;
        auto* lua = lua_newstate(snowdesktop::lua_runtime::QuotaAllocator, &quota);
        Check(lua != nullptr, "benchmark real quota Lua state");
        lua_pushlightuserdata(lua, &quota); lua_setfield(lua, LUA_REGISTRYINDEX, "__quota_ptr");
        Check(luaL_loadstring(lua, "return function(seed) local t={} local n=seed for i=1,2000 do n=n+i end for i=1,64 do t[i]=n+i end return t[64] end") == LUA_OK &&
            lua_pcall(lua, 0, 1, 0) == LUA_OK, "benchmark loads real allocating Lua workload");
        const auto function = luaL_ref(lua, LUA_REGISTRYINDEX);
        for (int warm = 0; warm < 100; ++warm)
        { lua_rawgeti(lua, LUA_REGISTRYINDEX, function); lua_pushinteger(lua, warm); Check(snowdesktop::lua_runtime::ProtectedCall(lua, 1, 1)==LUA_OK, "warm Lua"); lua_pop(lua, 1); }
        const std::string session = std::string(name) + "-" + std::to_string(round + 1);
        auto options = Options(root, session);
        options.preset = mode == 1 ? perf::CapturePreset::Resource : mode == 4 ? perf::CapturePreset::Widget : perf::CapturePreset::Interaction;
        options.scopeCpu = mode == 3;
        if (mode == 4) options.selectedInstances = {L"benchmark-instance"};
        std::string error;
        const auto setup = std::chrono::steady_clock::now();
        if (mode)
        {
            Check(perf::Start(options, error), "benchmark capture accepted");
            Check(WaitFor(session, false).phase == perf::CapturePhase::Recording, "benchmark SDK ack");
            snowdesktop::lua_runtime::SampleAllocationTraffic(quota, L"benchmark-instance");
        }
        const auto setupMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-setup).count();
        const auto before = memory(); const auto cpuBefore = cpuTime();
        const auto begin = std::chrono::steady_clock::now();
        std::uint64_t result = 0;
        for (int i = 0; i < iterations; ++i)
        {
            perf::Scope event("widget.event", "timer", L"benchmark-instance");
            lua_rawgeti(lua, LUA_REGISTRYINDEX, function); lua_pushinteger(lua, i);
            Check(snowdesktop::lua_runtime::ProtectedCall(lua, 1, 1) == LUA_OK, "benchmark preserves real Lua execution");
            result += static_cast<std::uint64_t>(lua_tointeger(lua, -1)); lua_pop(lua, 1);
            perf::Value("widget.view.layout_cache", i % 2 ? "hit" : "miss", L"benchmark-instance", 1);
            { perf::Scope invalidate("widget.invalidate", "desktop", L"benchmark-instance"); perf::DrawLink("desktop", L"benchmark-instance", false); }
            { perf::Scope draw("widget.composition", "draw", L"benchmark-instance"); perf::DrawLink("desktop", L"benchmark-instance", true); }
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-begin).count();
        const auto cpuMs = cpuTime()-cpuBefore; const auto after = memory();
        Check(result == static_cast<std::uint64_t>(iterations) * (2001000 + 64) +
            static_cast<std::uint64_t>(iterations) * (iterations - 1) / 2, "identical known-answer work in all capture modes");
        std::cout << "{\"mode\":\"" << name << "\",\"round\":" << round << ",\"phase\":\"work\",\"iterations\":" << iterations
            << ",\"elapsedMs\":" << elapsed << ",\"cpuMs\":" << cpuMs << ",\"cpuMsPerSecond\":" << cpuMs * 1000 / elapsed
            << ",\"setupMs\":" << setupMs << ",\"privateBytesBefore\":" << before.PrivateUsage << ",\"privateBytesAfter\":" << after.PrivateUsage
            << ",\"sdkInitialized\":" << (perf::PerfettoSession::Initialized() ? "true" : "false") << ",\"result\":" << result << "}\n";
        if (mode)
        {
            snowdesktop::lua_runtime::SampleAllocationTraffic(quota, L"benchmark-instance");
            Check(perf::RequestStop(session), "benchmark normal stop accepted");
            Check(WaitFor(session, true).phase == perf::CapturePhase::Completed, "benchmark normal stop completes without worker shutdown");
        }
        lua_close(lua);
        const auto quietMemory = memory(); const auto quietThreads = threads(); const auto quietCpu = cpuTime();
        const auto quietStart = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(1250));
        const auto quietMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-quietStart).count();
        const auto quietCpuMs = cpuTime()-quietCpu;
        std::cout << "{\"mode\":\"" << name << "\",\"round\":" << round << ",\"phase\":\"quiet-after-stop\",\"elapsedMs\":" << quietMs
            << ",\"cpuMs\":" << quietCpuMs << ",\"cpuMsPerSecond\":" << quietCpuMs * 1000 / quietMs << ",\"privateBytes\":" << quietMemory.PrivateUsage
            << ",\"threads\":" << quietThreads << ",\"captureEnabled\":" << (perf::Enabled() ? "true" : "false") << "}\n";
    };
    Check(!perf::PerfettoSession::Initialized(), "cold off benchmark starts before SDK initialization");
    run(0, -1, "off-cold");
    for (int round = 0; round < 5; ++round)
        for (int step = 0; step < 5; ++step)
        {
            const int mode = (step + round + 1) % 5;
            const char* names[]{"off-warm", "idle", "interaction", "interaction-cpu", "widget-detail"};
            run(mode, round, names[mode]);
        }
    perf::Shutdown();
    return 0;
}

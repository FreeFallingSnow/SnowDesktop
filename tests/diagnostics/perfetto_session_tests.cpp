#include "diagnostics/perfetto_categories.h"
#include "diagnostics/perfetto_session.h"

#include <windows.h>

#include <chrono>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace
{
namespace perf = snowdesktop::performance;
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
std::wstring Environment(const wchar_t* name)
{
    const auto size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    std::wstring result(size, L'\0');
    const auto length = GetEnvironmentVariableW(name, result.data(), size);
    Check(length > 0 && length < size, "read test environment");
    result.resize(length);
    return result;
}
std::wstring Quote(std::wstring_view value)
{
    // No shell is involved. Escape argv according to Windows CRT rules.
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto character : value)
    {
        if (character == L'\\') { ++slashes; continue; }
        result.append(slashes * (character == L'\"' ? 2 : 1), L'\\');
        if (character == L'\"') result.push_back(L'\\');
        result.push_back(character);
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

void AssertSql(const std::filesystem::path& trace, std::string_view sql)
{
    auto processor = std::filesystem::path(Environment(L"SNOWDESKTOP_TRACE_PROCESSOR"));
    if (processor.empty())
        processor = std::filesystem::path(Environment(L"USERPROFILE")) /
            L".local/share/perfetto/prebuilts/trace_processor_shell-adfa6bad3d72be3b.exe";
    Check(std::filesystem::is_regular_file(processor),
        "Perfetto v58.2 analyzer missing: install official Skill or set SNOWDESKTOP_TRACE_PROCESSOR");
    const auto query = std::filesystem::path(trace.wstring() + L".sql");
    const auto csv = std::filesystem::path(trace.wstring() + L".csv");
    { std::ofstream output(query); output << sql; }
    SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
    const auto output = CreateFileW(csv.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(output != INVALID_HANDLE_VALUE, "reserve analyzer output");
    const auto errors = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(errors != INVALID_HANDLE_VALUE, "open analyzer stderr");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output;
    startup.hStdError = errors;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    auto command = Quote(processor.wstring()) + L" query -f " + Quote(query.wstring()) +
        L" " + Quote(trace.wstring());
    const bool started = CreateProcessW(processor.c_str(), command.data(), nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE;
    CloseHandle(output);
    CloseHandle(errors);
    Check(started, "start real native trace processor");
    const auto waited = WaitForSingleObject(process.hProcess, 30000);
    if (waited != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 124);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Check(waited == WAIT_OBJECT_0 && exitCode == 0, "native trace parsing/query must complete successfully");
    std::ifstream input(csv);
    std::string header, answer;
    std::getline(input, header);
    std::getline(input, answer);
    if (!answer.empty() && answer.back() == '\r') answer.pop_back();
    if (answer != "1") std::cerr << "Perfetto assertion query: " << sql << "\nresult: " << answer << '\n';
    Check(answer == "1", "official analyzer must confirm the independent trace expectation");
}

void BusyFor(std::chrono::milliseconds duration)
{
    const auto end = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < end) YieldProcessor();
}
void EmitFixture()
{
    TRACE_EVENT("snow.lua.callback", "fixture.parent", "owner", "instance-a");
    BusyFor(std::chrono::milliseconds(10));
    {
        TRACE_EVENT("snow.lua.callback", "fixture.child", "owner", "instance-b");
        BusyFor(std::chrono::milliseconds(20));
    }
    // Deliberately excluded: an enabled-only list without disabled '*' would
    // accidentally record this and give the resource preset misleading costs.
    TRACE_EVENT_INSTANT("snow.render", "fixture.filtered");
    TRACE_COUNTER("snow.resource", "fixture.memory", 128);
    TRACE_COUNTER("snow.resource", "fixture.memory", 256);
}
}

void RunPerfettoSessionTests()
{
    Check(!perf::PerfettoSession::Initialized(), "SDK must not initialize on process startup");
    const auto root = std::filesystem::temp_directory_path() /
        (L"SnowDesktopNativePerfetto-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directory(root);
    perf::PerfettoSession session, competitor;
    perf::PerfettoSessionOptions options;
    options.output = root / L"native.pftrace";
    options.categories = { "snow.lua.callback", "snow.resource" };
    options.threadCpu = true;
    std::string error;
    auto invalid = options;
    invalid.categories = { "*" };
    Check(!session.Start(invalid, error) && !perf::PerfettoSession::Initialized(),
        "invalid configuration must not initialize SDK or reserve output");
    Check(std::filesystem::is_empty(root), "rejected capture must leave no files");
    Check(session.Start(options, error), "start Windows in-process SDK capture");
    Check(session.Active(), "recording only follows SDK acknowledgement");
    Check(!competitor.Start(options, error) && error == "captureBusy", "SDK session owner must be unique");
    Check(!std::filesystem::exists(options.output), "final file cannot appear while recording");
    // Windows SDK calibrates thread-cycle time after its initial time sample.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    std::atomic<bool> spanningEntered{ false }, releaseSpanning{ false };
    std::jthread spanning([&](std::stop_token stop) {
        TRACE_EVENT("snow.lua.callback", "fixture.spanning");
        spanningEntered.store(true);
        while (!releaseSpanning.load() && !stop.stop_requested())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    const auto enterDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!spanningEntered.load() && std::chrono::steady_clock::now() < enterDeadline)
        std::this_thread::yield();
    Check(spanningEntered.load(), "enter scope crossing the native stop boundary");
    EmitFixture();
    Check(session.Stop(error), "stop and publish native trace");
    Check(!session.Active() && std::filesystem::file_size(options.output) > 0,
        "complete native trace must be durable before success");
    Check(!std::filesystem::exists(options.output.wstring() + L".partial"),
        "successful publication must consume its partial name");
    AssertSql(options.output,
        "SELECT ((SELECT COUNT(*) FROM slice WHERE name='fixture.parent' AND dur>0 AND thread_dur>0)=1"
        " AND (SELECT COUNT(*) FROM slice WHERE name='fixture.child' AND dur>0 AND thread_dur>0)=1"
        " AND (SELECT COUNT(*) FROM slice WHERE name='fixture.filtered')=0"
        " AND (SELECT COUNT(*) FROM counter c JOIN track t ON c.track_id=t.id"
        " WHERE t.name='fixture.memory')=2"
        " AND (SELECT COUNT(*) FROM slice WHERE name='fixture.spanning' AND dur>=0)=0"
        " AND (SELECT COUNT(*) FROM slice c JOIN slice p ON c.parent_id=p.id"
        " WHERE c.name='fixture.child' AND p.name='fixture.parent')=1) AS passed;");
    Check(!session.Start(options, error), "completed evidence must never be overwritten");
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        options.output = root / (L"restart-" + std::to_wstring(attempt) + L".pftrace");
        options.threadCpu = false;
        Check(session.Start(options, error), "restart native capture");
        if (attempt == 0) { releaseSpanning.store(true); spanning.join(); }
        EmitFixture();
        Check(session.Stop(error), "stop restarted capture");
    }
    AssertSql(options.output,
        "SELECT ((SELECT COUNT(*) FROM slice WHERE name='fixture.parent')=1"
        " AND (SELECT COUNT(*) FROM slice WHERE thread_dur IS NOT NULL)=0) AS passed;");
    options.output = root / L"limited.pftrace";
    options.maximumFileBytes = 1024;
    Check(session.Start(options, error), "start bounded export");
    EmitFixture();
    for (int event = 0; event < 2000; ++event)
        TRACE_EVENT_INSTANT("snow.lua.callback", "fixture.export.limit", "index", event);
    Check(!session.Stop(error) && !std::filesystem::exists(options.output),
        "size-limit failure must not publish a truncated trace as success");
    Check(std::filesystem::exists(options.output.wstring() + L".partial"),
        "failed partial must remain available as evidence");
    std::cout << "Native Perfetto fixture passed: " << root.string() << '\n';
    // Only this fixture's individually named generated files are removed.
    for (const auto& item : std::filesystem::directory_iterator(root))
        std::filesystem::remove(item.path());
    std::filesystem::remove(root);
}

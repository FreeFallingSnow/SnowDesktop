#pragma once
#include "diagnostics/performance_trace.h"
#include <iterator>

namespace native_widget_drag_hover_cases
{
inline void TraceBegin(snowdesktop::performance::ScopeToken&,
    std::string_view module, std::string_view phase, std::wstring_view owner, std::uint64_t) noexcept
{
    auto* observation = NativeDragIntegrationAccess::widgetDragObservation;
    if (!observation) return;
    if (module == "widget.composition" && phase == "draw") observation->drawn.emplace_back(owner);
    if (module == "composition.shared" && phase == "commit") ++observation->commits;
}
inline void TraceEnd(snowdesktop::performance::ScopeToken&) noexcept {}
inline void TraceValue(std::string_view, std::string_view, std::wstring_view, double, std::uint64_t) noexcept {}
inline void TraceLink(std::string_view, std::wstring_view, bool) noexcept {}

inline void Run()
{
    constexpr auto legacyVariable = L"SNOWDESKTOP_TEST_R01_LEGACY_INVALIDATION";
    wchar_t legacyValue[2]{};
    const bool restoreLegacyInvalidation = GetEnvironmentVariableW(legacyVariable, legacyValue, 2) == 1 && legacyValue[0] == L'1';
    const snowdesktop::performance::Hooks hooks{TraceBegin, TraceEnd, TraceValue, TraceLink};
    const auto* previousHooks = snowdesktop::performance::captureHooks.exchange(&hooks);
    struct RestoreHooks { const snowdesktop::performance::Hooks* previous; ~RestoreHooks() { snowdesktop::performance::captureHooks = previous; } } restore{previousHooks};
    native_drag_commit_cases::Profile profile;
    auto& app = NativeDragIntegrationAccess::Scene(false);
    profile.apps.push_back(&app);
    NativeDragIntegrationAccess::RunWidgetDragHover(app, profile.root, restoreLegacyInvalidation);
    if (!restoreLegacyInvalidation)
    {
        // Execute the fault in the same selected CTest and binary lease. A
        // separate process preserves the failing exit code and raw evidence.
        wchar_t executable[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
        native_drag_commit_cases::Require(length > 0 && length < std::size(executable), "resolve negative-control executable");
        const auto log = profile.root / L"widget-drag-legacy.log";
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        struct CloseOutput
        {
            HANDLE value;
            ~CloseOutput() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
        } output{CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
        native_drag_commit_cases::Require(output.value != INVALID_HANDLE_VALUE, "create isolated negative-control log");
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        startup.hStdOutput = startup.hStdError = output.value; startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION process{};
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --widget-surface-handoff";
        wchar_t previousValue[32768]{};
        const DWORD previousLength = GetEnvironmentVariableW(legacyVariable, previousValue, static_cast<DWORD>(std::size(previousValue)));
        native_drag_commit_cases::Require(previousLength < std::size(previousValue), "capture process-local negative-control flag");
        struct RestoreEnvironment
        {
            const wchar_t* name; const wchar_t* value;
            ~RestoreEnvironment() { SetEnvironmentVariableW(name, value); }
        } restoreEnvironment{legacyVariable, previousLength ? previousValue : nullptr};
        native_drag_commit_cases::Require(SetEnvironmentVariableW(legacyVariable, L"1") != FALSE,
            "set only this test process's original-invalidation control flag");
        const BOOL created = CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
        CloseHandle(output.value); output.value = INVALID_HANDLE_VALUE;
        native_drag_commit_cases::Require(created != FALSE, "start the isolated original-invalidation control");
        const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
        if (wait != WAIT_OBJECT_0)
        {
            TerminateProcess(process.hProcess, 99); // Only this explicitly created fixture process.
            WaitForSingleObject(process.hProcess, 5000);
        }
        DWORD exitCode = 0; const BOOL readExit = GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        native_drag_commit_cases::Require(wait == WAIT_OBJECT_0 && readExit && exitCode == 1,
            "original full-invalidation control must finish with the expected failing exit code");
        const auto evidence = native_drag_commit_cases::Read(log);
        native_drag_commit_cases::Require(evidence.find("Negative control: execute original widget-crossing full root invalidation") != std::string::npos &&
            evidence.find("ordinary widget crossing must not invalidate the complete desktop") != std::string::npos,
            "negative control must fail at the target business assertion, not fixture setup");
        std::cout << "Original-invalidation negative control exit=1 (expected):\n" << evidence;
    }
}
}

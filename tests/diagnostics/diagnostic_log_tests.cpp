#include "diagnostics/diagnostic_log.h"
#include "diagnostics/input_diagnostics.h"
#include "app/lifecycle/startup_diagnostics.h"
#include "app/dock/dock_input_diagnostics.h"
#include "icons/icon_load_diagnostics.h"
#include "steam/steam_workshop_diagnostics.h"

#include <windows.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
std::filesystem::path directory;
int failures = 0;
void Check(bool condition, const char* message)
{
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
std::wstring ReadLog()
{
    std::ifstream input(directory / L"SnowDesktop.log", std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
    std::wstring result(bytes.size() / sizeof(wchar_t), L'\0');
    if (!result.empty()) std::memcpy(result.data(), bytes.data(), result.size() * sizeof(wchar_t));
    return result;
}
void PumpFor(DWORD milliseconds)
{
    const auto until = GetTickCount64() + milliseconds;
    do
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            snowdesktop::InputDiagnostics::Acknowledge(message.hwnd, message.message, message.wParam, message.lParam);
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(2);
    } while (GetTickCount64() < until);
}
}

std::wstring GetDataFilePath(const wchar_t* filename)
{
    return (directory / filename).wstring();
}

int main()
{
    using namespace snowdesktop::diagnostics;
    directory = std::filesystem::temp_directory_path() /
        (L"SnowDesktop-log-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(directory);
    const auto path = directory / L"SnowDesktop.log";
    SetLogModules(0);
    for (const auto& descriptor : LogModules)
    {
        WriteModuleDiagnosticLogEntry(descriptor.module, L"disabled-info");
        WriteModuleDiagnosticLogEntry(descriptor.module, L"disabled-debug", DiagnosticLogLevel::Debug);
    }
    Check(!std::filesystem::exists(path), "all detailed modules default off and create no log file");
    constexpr auto missingWorkshopCache = "Steam Workshop cache is unavailable";
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Steam, L"workshop-cache-absent",
        snowdesktop::widget::SteamWorkshopQueryFailureLogLevel(missingWorkshopCache));
    Check(!std::filesystem::exists(path),
        "a missing never-subscribed Workshop cache does not create an error log by default");
    SetLogModules(ModuleBit(DiagnosticLogModule::Steam));
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Steam, L"workshop-cache-absent",
        snowdesktop::widget::SteamWorkshopQueryFailureLogLevel(missingWorkshopCache));
    Check(ReadLog().find(L"[INFO] workshop-cache-absent") != std::wstring::npos,
        "Steam diagnostics retain missing-cache context as optional Info");
    SetLogModules(0);
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Steam, L"workshop-cache-unreadable",
        snowdesktop::widget::SteamWorkshopQueryFailureLogLevel("cannot access Steam Workshop library: access denied"));
    Check(ReadLog().find(L"[ERROR] workshop-cache-unreadable") != std::wstring::npos,
        "real Workshop cache access failures remain unconditional errors");
    // Subsequent no-file assertions start with the same isolated clean sink.
    std::filesystem::remove(path);
    int callbacks = 0;
    {
        snowdesktop::startup_diagnostics::Scope startup(L"disabled-root", true);
        const int value = snowdesktop::startup_diagnostics::Call(L"disabled-child", [&] { ++callbacks; return 37; });
        Check(value == 37 && callbacks == 1, "disabled startup probes preserve operations and return values");
    }
    Check(!std::filesystem::exists(path), "disabled nested startup probes do not emit records");
    // Exercise the production observer and real sink, not Dock dismissal.
    // It must preserve decisions, retain skipped gates and bound log growth.
    namespace dockInput = snowdesktop::dock_input_diagnostics;
    {
        dockInput::Scope trace(true, true);
        Check(trace.Observe(dockInput::Gate::Press, true) &&
                !trace.Observe(dockInput::Gate::Menu, false),
            "disabled Dock observations preserve condition return values");
    }
    Check(!std::filesystem::exists(path), "Dock input diagnostics default off");
    SetLogModules(ModuleBit(DiagnosticLogModule::Dock));
    {
        dockInput::Scope trace(true, true);
        trace.sample.sampled = true;
        trace.sample.left = -32768;
        trace.sample.buttons = 1;
        Check(!(trace.Observe(dockInput::Gate::Press, false) &&
                trace.Observe(dockInput::Gate::Menu, (++callbacks, true))),
            "an observed false press keeps the original short circuit");
    }
    Check(callbacks == 1, "a skipped Dock gate does not invoke its operand");
    auto dockLog = ReadLog();
    Check(dockLog.find(L"DockInputTrace pid=") != std::wstring::npos &&
            dockLog.find(L"sampled=1 raw=-32768,0,0 buttons=1") != std::wstring::npos &&
            dockLog.find(L"press=0") != std::wstring::npos && dockLog.find(L"menu=-1") != std::wstring::npos,
        "Dock trace retains raw state and distinguishes false from unevaluated gates on early exit");
    {
        dockInput::Scope hidden(false, false);
    }
    std::filesystem::remove(path);
    for (unsigned i = 0; i < 700; ++i)
    {
        dockInput::Scope trace(true, true);
        trace.Observe(dockInput::Gate::Press, (i & 1u) != 0);
    }
    dockLog = ReadLog();
    unsigned dockRecords = 0;
    for (std::size_t offset = 0; (offset = dockLog.find(L"DockInputTrace pid=", offset)) != std::wstring::npos; ++offset)
        ++dockRecords;
    Check(dockRecords == 512 && dockLog.find(L"DockInputTrace limit reached") != std::wstring::npos,
        "a continuous Dock session stops after 512 input records");
    const auto boundedSize = std::filesystem::file_size(path);
    {
        dockInput::Scope trace(true, true);
        trace.Observe(dockInput::Gate::Menu, true);
    }
    Check(std::filesystem::file_size(path) == boundedSize, "an exhausted Dock session cannot keep growing its log");
    SetLogModules(0);
    {
        dockInput::Scope trace(true, true);
    }
    Check(std::filesystem::file_size(path) == boundedSize, "disabling Dock input logs takes effect immediately");
    std::filesystem::remove(path);
    // Replay the observed startup sample at the logger used by QueueIconTask:
    // 625 ms queued, 0 ms bitmap read, successful bitmap. Only the output path
    // is isolated; formatting, severity, filtering and file I/O are production.
    const snowdesktop::icon_diagnostics::ShellIconLoadTiming queuedIcon{
        1, 625, 0, 0, 0, 625, true, 503, L"C:\\Captured\\example.lnk"};
    snowdesktop::icon_diagnostics::LogShellIconLoadTiming(queuedIcon);
    Check(!std::filesystem::exists(path), "successful queued icons do not create logs while all modules are off");
    SetLogModules(ModuleBit(DiagnosticLogModule::Shell));
    snowdesktop::icon_diagnostics::LogShellIconLoadTiming(queuedIcon);
    Check(!std::filesystem::exists(path), "the Shell switch does not enable Icons timing records");
    SetLogModules(ModuleBit(DiagnosticLogModule::Icons));
    snowdesktop::icon_diagnostics::LogShellIconLoadTiming(queuedIcon);
    const auto iconLog = ReadLog();
    Check(iconLog.find(L"[INFO] Shell icon slow: phase=1 queueMs=625") != std::wstring::npos &&
        iconLog.find(L"bitmapMs=0") != std::wstring::npos && iconLog.find(L"bitmap=1") != std::wstring::npos &&
        iconLog.find(L"[WARN]") == std::wstring::npos,
        "enabled Icons logs the captured successful queue delay as Info");
    SetLogModules(0);
    const auto afterIconEnabled = std::filesystem::file_size(path);
    snowdesktop::icon_diagnostics::LogShellIconLoadTiming(queuedIcon);
    Check(std::filesystem::file_size(path) == afterIconEnabled, "disabling Icons stops the next real icon timing record");
    auto failedIcon = queuedIcon;
    failedIcon.hasBitmap = false;
    snowdesktop::icon_diagnostics::LogShellIconLoadTiming(failedIcon);
    Check(ReadLog().find(L"[WARN] Shell icon slow:") != std::wstring::npos &&
        ReadLog().find(L"bitmap=0") != std::wstring::npos,
        "unsuccessful slow icon loads retain warnings while all modules are off");
    for (const auto& descriptor : LogModules)
    {
        SetLogModules(ModuleBit(descriptor.module));
        for (const auto& candidate : LogModules)
            Check(IsLogEnabled(candidate.module) == (candidate.module == descriptor.module), "module switches are independent");
        WriteModuleDiagnosticLogEntry(descriptor.module, L"selected-module-info");
        WriteModuleDiagnosticLogEntry(descriptor.module, L"selected-module-debug", DiagnosticLogLevel::Debug);
    }
    auto text = ReadLog();
    Check(text.find(L"selected-module-info") != std::wstring::npos && text.find(L"[DEBUG]") != std::wstring::npos,
        "enabled info/debug reach the real UTF-16 file sink");
    SetLogModules(0);
    const auto before = std::filesystem::file_size(path);
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Input, L"after-disable");
    Check(std::filesystem::file_size(path) == before, "disabling takes effect for the next record");
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Input, L"essential-warning", DiagnosticLogLevel::Warning);
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Steam, L"essential-error", DiagnosticLogLevel::Error);
    text = ReadLog();
    Check(text.find(L"[WARN] essential-warning") != std::wstring::npos && text.find(L"[ERROR] essential-error") != std::wstring::npos &&
        text.find(L"disabled-info") == std::wstring::npos, "critical severity survives every module being off");
    SetLogModules(~std::uint32_t{0});
    Check(!IsLogEnabled(DiagnosticLogModule::Count), "unknown persisted bits cannot enable a module");
    {
        snowdesktop::startup_diagnostics::Scope startup(L"enabled-root", true);
        snowdesktop::startup_diagnostics::Scope child(L"enabled-child");
    }
    Check(ReadLog().find(L"step=enabled-child") != std::wstring::npos, "startup logging enables nested evidence");

    // Isolated message-only window: no mouse injection, hooks, focus or desktop host.
    const HWND window = CreateWindowExW(0, L"STATIC", L"log-observer-test", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "isolated observer window exists");
    if (window)
    {
        SetLogModules(0);
        {
            snowdesktop::InputDiagnostics observer(window);
            PumpFor(2100);
            Check(ReadLog().find(L"InputState ") == std::wstring::npos, "disabled production observer emits no input snapshots");
            SetLogModules(ModuleBit(DiagnosticLogModule::Input));
            PumpFor(2100);
            Check(ReadLog().find(L"InputState ") != std::wstring::npos, "enabling during the session starts input observation");
            SetLogModules(0);
            const auto disabledSize = std::filesystem::file_size(path);
            PumpFor(2100);
            Check(std::filesystem::file_size(path) == disabledSize, "disabling during the session stops input snapshots");
        }
        DestroyWindow(window);
    }
    const std::wstring large(540000, L'x');
    WriteDiagnosticLogEntry(large.c_str());
    WriteDiagnosticLogEntry(L"after-rotation", DiagnosticLogLevel::Error);
    Check(std::filesystem::exists(path.wstring() + L".1") && ReadLog().find(L"after-rotation") != std::wstring::npos,
        "the real file sink rotates and retains the subsequent critical entry");
    SetLogModules(0);
    std::filesystem::remove_all(directory);
    return failures ? 1 : 0;
}

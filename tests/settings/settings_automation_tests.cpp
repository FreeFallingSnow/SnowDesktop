#include "settings/settings_automation.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
using snowdesktop::settings_automation::Service;
using snowdesktop::control::Element;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
LRESULT CALLBACK FixtureProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wParam, lParam);
}
int Fixture()
{
    WNDCLASSW type{}; type.lpfnWndProc = FixtureProc; type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"SnowSettingsAutomationFixture";
    if (!RegisterClassW(&type)) return 2;
    const auto root = CreateWindowExW(WS_EX_NOACTIVATE, type.lpszClassName, L"Settings automation fixture",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 640, 360, nullptr, nullptr, type.hInstance, nullptr);
    if (!root) return 3;
    CreateWindowW(L"STATIC", L"Fixture search", WS_CHILD | WS_VISIBLE, 20, 20, 180, 24, root, nullptr, type.hInstance, nullptr);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"Original search", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        20, 48, 260, 30, root, reinterpret_cast<HMENU>(101), type.hInstance, nullptr);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"private sentinel", WS_CHILD | WS_VISIBLE | ES_PASSWORD,
        20, 92, 260, 30, root, reinterpret_cast<HMENU>(102), type.hInstance, nullptr);
    CreateWindowW(L"BUTTON", L"Disabled sentinel", WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_PUSHBUTTON,
        20, 140, 220, 32, root, reinterpret_cast<HMENU>(103), type.hInstance, nullptr);
    // Another top-level window in the same child is outside the allowed root.
    const auto outside = CreateWindowExW(WS_EX_NOACTIVATE, type.lpszClassName, L"Outside sentinel",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 720, 40, 260, 180, nullptr, nullptr, type.hInstance, nullptr);
    const auto line = std::to_string(reinterpret_cast<std::uintptr_t>(root)) + "\n";
    DWORD written = 0; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    if (IsWindow(outside)) DestroyWindow(outside);
    return 0;
}
struct Child
{
    HDESK desktop = nullptr;
    PROCESS_INFORMATION process{};
    HANDLE output = nullptr;
    HWND window = nullptr;
    ~Child()
    {
        if (IsWindow(window)) PostMessageW(window, WM_CLOSE, 0, 0);
        if (process.hProcess)
        {
            if (WaitForSingleObject(process.hProcess, 5000) == WAIT_TIMEOUT)
                TerminateProcess(process.hProcess, 9); // This test's private fixture only.
            CloseHandle(process.hProcess); CloseHandle(process.hThread);
        }
        if (output) CloseHandle(output);
        if (desktop) CloseDesktop(desktop);
    }
    void Start()
    {
        // The driver process itself starts on the private desktop, so its MTA
        // threads and its target child share the same desktop/window station.
        wchar_t name[256]{}; DWORD needed = 0;
        Require(GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, name, sizeof(name), &needed) != FALSE,
            "identify driver's private desktop");
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE}; HANDLE write = nullptr;
        Require(CreatePipe(&output, &write, &security, 0) != FALSE, "create fixture announcement pipe");
        SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0);
        wchar_t exe[32768]{}; Require(GetModuleFileNameW(nullptr, exe, 32768) != 0, "resolve fixture executable");
        std::wstring command = L"\"" + std::wstring(exe) + L"\" --settings-fixture";
        STARTUPINFOW startup{sizeof(startup)}; startup.lpDesktop = name;
        startup.dwFlags = STARTF_USESTDHANDLES; startup.hStdOutput = write; startup.hStdError = write;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        const bool started = CreateProcessW(exe, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process) != FALSE;
        CloseHandle(write); Require(started, "start fixture child");
        const auto deadline = GetTickCount64() + 5000;
        std::string text;
        while (GetTickCount64() < deadline && text.find('\n') == std::string::npos)
        {
            DWORD available = 0;
            if (!PeekNamedPipe(output, nullptr, 0, nullptr, &available, nullptr)) break;
            if (available)
            {
                char buffer[128]{}; DWORD count = 0;
                Require(ReadFile(output, buffer, std::min<DWORD>(available, 127), &count, nullptr) != FALSE, "read fixture HWND");
                text.append(buffer, count);
            }
            else std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Require(text.find('\n') != std::string::npos, "fixture HWND arrived within deadline");
        window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(std::stoull(text)));
    }
};
int IsolatedDriver()
{
    Child driver;
    const auto name = L"SnowSettingsAutomation-" + std::to_wstring(GetCurrentProcessId());
    driver.desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    Require(driver.desktop != nullptr, "create private driver desktop");
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE}; HANDLE outputWrite = nullptr;
    Require(CreatePipe(&driver.output, &outputWrite, &security, 0) != FALSE, "create driver output pipe");
    SetHandleInformation(driver.output, HANDLE_FLAG_INHERIT, 0);
    wchar_t exe[32768]{}; Require(GetModuleFileNameW(nullptr, exe, 32768) != 0, "resolve driver executable");
    std::wstring command = L"\"" + std::wstring(exe) + L"\" --settings-driver";
    STARTUPINFOW startup{sizeof(startup)}; startup.lpDesktop = const_cast<wchar_t*>(name.c_str());
    startup.dwFlags = STARTF_USESTDHANDLES; startup.hStdOutput = outputWrite; startup.hStdError = outputWrite;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    const bool started = CreateProcessW(exe, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &driver.process) != FALSE;
    CloseHandle(outputWrite); Require(started, "start private desktop driver");
    const auto deadline = GetTickCount64() + 45000;
    for (;;)
    {
        DWORD available = 0;
        if (PeekNamedPipe(driver.output, nullptr, 0, nullptr, &available, nullptr) && available)
        {
            char buffer[1024]{}; DWORD count = 0;
            Require(ReadFile(driver.output, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr) != FALSE, "read driver output");
            std::cout.write(buffer, count);
        }
        else if (WaitForSingleObject(driver.process.hProcess, 20) == WAIT_OBJECT_0) break;
        Require(GetTickCount64() < deadline, "private UIA driver completed before deadline");
    }
    DWORD exit = 1; Require(GetExitCodeProcess(driver.process.hProcess, &exit) != FALSE, "read driver exit status");
    return static_cast<int>(exit);
}
auto Wait(Service& service)
{
    const auto deadline = GetTickCount64() + 15000;
    auto state = service.Current();
    while (state.status == "running" && GetTickCount64() < deadline)
    { std::this_thread::sleep_for(std::chrono::milliseconds(10)); state = service.Current(); }
    if (state.status != "completed") std::cerr << "UIA status=" << state.status << " error=" << state.error << '\n';
    Require(state.status == "completed", "UIA task completed before deadline"); return state;
}
}
int wmain(int argc, wchar_t** argv)
{
    if (argc == 2 && std::wstring_view(argv[1]) == L"--settings-fixture") return Fixture();
    try
    {
        if (argc == 1) return IsolatedDriver();
        Child child; child.Start(); Service service; std::string error;
        Require(!service.Refresh(nullptr, error) && error == "settings_not_ready", "missing settings root rejected");
        Require(service.Refresh(child.window, error), "start asynchronous fixture query");
        const auto initial = Wait(service);
        auto nodes = service.Elements(child.window);
        Require(nodes.size() >= 5 && !initial.truncated, "complete bounded fixture catalog");
        snowdesktop::control::ElementSnapshot snapshot; snapshot.elements = nodes; snapshot.Finish();
        Require(std::none_of(nodes.begin(), nodes.end(), [](const Element& node) { return node.name == "Outside sentinel"; }),
            "UIA never traverses a sibling top-level window");
        const auto find = [&](const auto& predicate) {
            const auto it = std::find_if(nodes.begin(), nodes.end(), predicate);
            Require(it != nodes.end(), "fixture control exported"); return *it;
        };
        const auto password = find([](const Element& node) { return node.password; });
        Require(password.value.empty() && !snowdesktop::control::Supports(password, "setValue") &&
            !snowdesktop::control::ElementJson(password).Find("value"), "password text redacted and no value mutation offered");
        const auto edit = find([](const Element& node) { return node.role == "edit" && !node.password; });
        Require(edit.value == "Original search" && snowdesktop::control::Supports(edit, "setValue"), "editor value and pattern exported");
        Require(service.Perform(child.window, edit, "setValue", L"固定演示搜索", 0, error), "start normal value pattern"); Wait(service);
        Require(service.Elements(child.window).empty(), "semantic mutation invalidates cached catalog");
        Require(service.Refresh(child.window, error), "refresh after edit"); Wait(service); nodes = service.Elements(child.window);
        Require(find([](const Element& node) { return node.role == "edit" && !node.password; }).value == "固定演示搜索",
            "actual native editor received Chinese text through UIA");
        const auto disabled = find([](const Element& node) { return node.name == "Disabled sentinel"; });
        Require(!disabled.enabled, "disabled state preserved");
        Require(service.Perform(child.window, disabled, "invoke", {}, 0, error), "queue freshly checked disabled action");
        const auto deadline = GetTickCount64() + 15000;
        while (service.Current().status == "running" && GetTickCount64() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        Require(service.Current().status == "failed" && service.Current().error == "element_disabled", "disabled action refused by fresh provider state");
        auto forged = edit; forged.id = "settings/unrelated/target";
        Require(!service.Perform(child.window, forged, "setValue", L"must not apply", 0, error) && error == "settings_target_changed",
            "foreign target identity cannot route to the owned root");
        Require(service.Refresh(child.window, error), "refresh before concurrent change"); Wait(service); nodes = service.Elements(child.window);
        const auto beforeChange = find([](const Element& node) { return node.role == "edit" && !node.password; });
        DWORD_PTR ignored = 0;
        Require(SendMessageTimeoutW(GetDlgItem(child.window, 101), WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"Concurrent human input"),
            SMTO_ABORTIFHUNG, 2000, &ignored) != 0, "simulate independent editor change");
        Require(service.Perform(child.window, beforeChange, "setValue", L"must not overwrite", 0, error), "queue action against stale provider content");
        const auto staleDeadline = GetTickCount64() + 15000;
        while (service.Current().status == "running" && GetTickCount64() < staleDeadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        Require(service.Current().status == "failed" && service.Current().error == "stale_settings_element", "fresh provider content check protects independent changes");
        Require(service.Refresh(child.window, error), "query after rejected stale action"); Wait(service); nodes = service.Elements(child.window);
        Require(find([](const Element& node) { return node.role == "edit" && !node.password; }).value == "Concurrent human input",
            "rejected action preserved actual editor content");
        service.Stop();
        PostMessageW(child.window, WM_CLOSE, 0, 0);
        Require(WaitForSingleObject(child.process.hProcess, 5000) == WAIT_OBJECT_0, "fixture exited normally");
        Require(service.Elements(child.window).empty() && !service.Refresh(child.window, error), "retired process invalidates all bindings");
        std::cout << "settings automation integration passed\n"; return 0;
    }
    catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}

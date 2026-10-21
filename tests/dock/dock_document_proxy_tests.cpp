#include "dock/dock_window_rules.h"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <string>
#include <thread>

namespace
{
using namespace snowdesktop::dock_window_rules;
constexpr DWORD ProxyStyle = WS_POPUP | WS_CAPTION;
constexpr DWORD ProxyExtendedStyle =
    WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_WINDOWEDGE;

struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

LRESULT CALLBACK FixtureProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE)
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(
            reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
    if (message == WM_GETTEXTLENGTH)
        if (auto* count = reinterpret_cast<std::atomic<int>*>(
                GetWindowLongPtrW(window, GWLP_USERDATA)))
            ++*count;
    return DefWindowProcW(window, message, wParam, lParam);
}

bool LegacyProbe(HWND window)
{
    // Restore the exact faulty title-query path, including its eager arguments.
    return IsTaskbarDocumentProxyCandidateEligible(
        GetAncestor(window, GA_ROOT) == window, IsWindowVisible(window) != FALSE,
        GetWindow(window, GW_OWNER) != nullptr, GetWindowTextLengthW(window) > 0,
        GetWindowLongPtrW(window, GWL_STYLE), GetWindowLongPtrW(window, GWL_EXSTYLE),
        IsTaskWindowExplicitlyExcluded(window));
}

int ExternalFixture(HANDLE pipe, HANDLE release)
{
    HWND titled = CreateWindowExW(ProxyExtendedStyle, L"STATIC", L"Document A",
        ProxyStyle, 0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND untitled = CreateWindowExW(ProxyExtendedStyle, L"STATIC", L"",
        ProxyStyle, 0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const HWND windows[]{titled, untitled};
    DWORD written = 0;
    const bool sent = WriteFile(pipe, windows, sizeof(windows), &written, nullptr) &&
        written == sizeof(windows);
    CloseHandle(pipe);
    // Deliberately do not pump messages: foreign cached captions must still work.
    WaitForSingleObject(release, 10000);
    if (titled) DestroyWindow(titled);
    if (untitled) DestroyWindow(untitled);
    return sent && titled && untitled ? 0 : 2;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 4 && std::string(argv[1]) == "--external")
        return ExternalFixture(reinterpret_cast<HANDLE>(std::stoull(argv[2])),
            reinterpret_cast<HANDLE>(std::stoull(argv[3])));
    const bool negative = argc == 2 && std::string(argv[1]) == "--legacy";
    int failures = 0;
    const auto check = [&](bool passed, const char* message) {
        if (!passed) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    check(!IsTaskbarDocumentProxyCandidate(nullptr), "null window is excluded");

    const std::wstring className = L"SnowDesktop non-pumping fixture " +
        std::to_wstring(GetCurrentProcessId());
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = FixtureProcedure;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = className.c_str();
    if (!RegisterClassW(&windowClass)) return 2;
    Handle release{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!release.value) return 2;
    std::atomic<int> titleMessages = 0;
    std::promise<HWND> created;
    auto ready = created.get_future();
    std::thread worker([&] {
        HWND window = CreateWindowExW(ProxyExtendedStyle, className.c_str(),
            L"Media worker internal surface", ProxyStyle, 0, 0, 100, 100,
            nullptr, nullptr, GetModuleHandleW(nullptr), &titleMessages);
        created.set_value(window);
        // A bounded gate lets the restored old behavior fail without hanging CTest.
        WaitForSingleObject(release.value, 2000);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&message);
        if (window) DestroyWindow(window);
    });
    const HWND internal = ready.get();
    check(internal != nullptr, "own worker creates a hidden titled proxy-shaped window");
    const auto start = std::chrono::steady_clock::now();
    const bool candidate = negative ? LegacyProbe(internal) :
        IsTaskbarDocumentProxyCandidate(internal);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    check(!candidate, "own hidden worker window must be excluded from proxy discovery");
    check(elapsed < std::chrono::seconds(1), "proxy discovery must not wait for the worker's message pump");
    check(titleMessages == 0, "proxy discovery must not send WM_GETTEXTLENGTH to our worker");
    SetEvent(release.value);
    worker.join();
    UnregisterClassW(className.c_str(), GetModuleHandleW(nullptr));

    // Verify the same production probe still accepts titled proxies owned by
    // another process, even while that process is not dispatching messages.
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    Handle readPipe, writePipe;
    Handle externalRelease{CreateEventW(&attributes, TRUE, FALSE, nullptr)};
    if (!externalRelease.value || !CreatePipe(&readPipe.value, &writePipe.value, &attributes, 0))
        return 2;
    SetHandleInformation(readPipe.value, HANDLE_FLAG_INHERIT, 0);
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --external " +
        std::to_wstring(reinterpret_cast<ULONG_PTR>(writePipe.value)) + L" " +
        std::to_wstring(reinterpret_cast<ULONG_PTR>(externalRelease.value));
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return 2;
    Handle child{process.hProcess}, childThread{process.hThread};
    DWORD available = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline &&
        PeekNamedPipe(readPipe.value, nullptr, 0, nullptr, &available, nullptr) &&
        available < sizeof(HWND) * 2 && WaitForSingleObject(child.value, 10) == WAIT_TIMEOUT) {}
    HWND foreign[2]{};
    DWORD read = 0;
    const bool received = available >= sizeof(foreign) &&
        ReadFile(readPipe.value, foreign, sizeof(foreign), &read, nullptr) && read == sizeof(foreign);
    check(received && foreign[0] && foreign[1], "foreign non-pumping fixtures are ready");
    if (received)
    {
        check(IsTaskbarDocumentProxyCandidate(foreign[0]), "foreign titled document proxy remains eligible");
        check(!IsTaskbarDocumentProxyCandidate(foreign[1]), "foreign untitled helper remains excluded");
    }
    SetEvent(externalRelease.value);
    check(WaitForSingleObject(child.value, 5000) == WAIT_OBJECT_0, "foreign fixture exits cleanly");
    std::cout << "Dock document proxy regression: " << failures << " failures\n";
    return failures ? 1 : 0;
}

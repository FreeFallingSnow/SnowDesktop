#include "steam/steam_presence.h"
#include "presence_lifetime.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace
{
bool AwaitProcess(snowdesktop::steam_presence::Service& service)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!service.ProcessId() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return service.ProcessId() != 0;
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc > 1 && std::wstring_view(argv[1]) == L"presence")
        return snowdesktop::steam_bridge::RunPresenceLifetime(argc, argv, [] {});
    const auto self = std::filesystem::absolute(argv[0]);
    if (argc == 3 && std::wstring_view(argv[1]) == L"--orphan-parent")
    {
        snowdesktop::steam_presence::Service service(self);
        service.SetEnabled(true);
        if (!AwaitProcess(service)) return 2;
        { std::ofstream out{std::filesystem::path(argv[2])}; out << service.ProcessId(); }
        // Deliberately bypass destructors to exercise the real parent handle.
        ExitProcess(0);
    }
    int failures = 0;
    const auto check = [&](bool value, const char* message) {
        if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    snowdesktop::steam_presence::Service service(self);
    check(service.ProcessId() == 0, "disabled service does not create a child");
    service.SetEnabled(true);
    check(AwaitProcess(service), "enable launches a real child with inherited handles");
    const auto first = service.ProcessId();
    HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, first);
    check(child && WaitForSingleObject(child, 800) == WAIT_TIMEOUT, "presence stays alive without repeated launches");
    service.SetEnabled(true);
    check(service.ProcessId() == first, "applying enabled settings retains the same process");
    service.SetEnabled(false);
    check(!service.ProcessId() && child && WaitForSingleObject(child, 1000) == WAIT_OBJECT_0,
        "disable signals the exact session and waits for its exit");
    if (child) CloseHandle(child);
    service.SetEnabled(false);
    service.SetEnabled(true);
    check(AwaitProcess(service) && service.ProcessId() != first, "a later enable gets a fresh owned session");
    service.SetEnabled(false);
    const auto orphanFile = std::filesystem::temp_directory_path() /
        (L"snow-presence-" + std::to_wstring(GetCurrentProcessId()) + L".txt");
    std::wstring command = L"\"" + self.wstring() + L"\" --orphan-parent \"" + orphanFile.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION parent{};
    check(CreateProcessW(self.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, self.parent_path().c_str(), &startup, &parent) != FALSE, "launch isolated crash-parent fixture");
    if (parent.hProcess)
    {
        const bool exited = WaitForSingleObject(parent.hProcess, 7000) == WAIT_OBJECT_0;
        DWORD code = 1;
        GetExitCodeProcess(parent.hProcess, &code);
        check(exited && code == 0, "fixture exits without service cleanup");
        DWORD orphanId = 0;
        std::ifstream input(orphanFile); input >> orphanId;
        HANDLE orphan = OpenProcess(SYNCHRONIZE, FALSE, orphanId);
        check(orphanId != 0 && (!orphan || WaitForSingleObject(orphan, 5000) == WAIT_OBJECT_0),
            "session exits after its real parent dies, without PID polling or user processes");
        if (orphan) CloseHandle(orphan);
        CloseHandle(parent.hThread);
        CloseHandle(parent.hProcess);
    }
    std::error_code ec;
    std::filesystem::remove(orphanFile, ec);
    if (!failures) std::cout << "Steam presence lifetime checks passed\n";
    return failures ? 1 : 0;
}

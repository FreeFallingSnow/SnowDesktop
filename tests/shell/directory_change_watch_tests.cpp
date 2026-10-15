#include "shell/directory_change_watch.h"

#include <filesystem>
#include <iostream>
#include <vector>

namespace
{
int failures = 0;
void Expect(bool condition, const char* message)
{
    if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
}

int main()
{
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const auto root = std::filesystem::path(temp) /
        (L"SnowDesktopWatch-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root / L"first");
    std::filesystem::create_directories(root / L"second");
    using snowdesktop::shell::DirectoryChangeWatch;
    std::vector<std::unique_ptr<DirectoryChangeWatch>> watches;
    const std::vector<std::filesystem::path> paths{ root / L"first", root / L"second" };
    for (const auto& path : paths)
        watches.push_back(DirectoryChangeWatch::Create(path.wstring()));
    Expect(watches[0] && watches[1], "two independent native directory watches start");
    if (watches[0] && watches[1])
    {
        // Relocate the owning container while both requests are pending.
        auto relocated = std::move(watches);
        const HANDLE events[]{ relocated[0]->Event(), relocated[1]->Event() };
        for (const auto& path : paths)
        {
            HANDLE file = CreateFileW((path / L"notification.txt").c_str(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            Expect(file != INVALID_HANDLE_VALUE, "temporary change file is created");
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        }
        Expect(WaitForMultipleObjects(2, events, TRUE, 3000) == WAIT_OBJECT_0,
            "first completion arrives for both directories");
        for (const auto& watch : relocated)
        {
            DWORD bytes = 0;
            Expect(watch->Complete(bytes) && bytes >= sizeof(FILE_NOTIFY_INFORMATION),
                "completion queries the exact live OVERLAPPED used to submit the request");
            Expect(watch->Arm(), "completed directory request can be rearmed");
        }
        for (const auto& path : paths)
            Expect(DeleteFileW((path / L"notification.txt").c_str()) != FALSE,
                "second independent native notification is generated");
        Expect(WaitForMultipleObjects(2, events, TRUE, 3000) == WAIT_OBJECT_0,
            "rearmed completion arrives for both directories");
        for (const auto& watch : relocated)
        {
            DWORD bytes = 0;
            Expect(watch->Complete(bytes) && bytes >= sizeof(FILE_NOTIFY_INFORMATION),
                "rearmed request uses the same live operation storage");
            Expect(watch->Arm(), "a request remains pending at shutdown");
        }
        relocated.clear(); // Destructors cancel and drain; no file event is needed.
    }
    DWORD before = 0, after = 0;
    GetProcessHandleCount(GetCurrentProcess(), &before);
    for (int iteration = 0; iteration < 24; ++iteration)
    {
        auto watch = DirectoryChangeWatch::Create(paths[0].wstring());
        Expect(watch != nullptr, "immediate-stop watch starts");
    }
    GetProcessHandleCount(GetCurrentProcess(), &after);
    Expect(before == after, "immediate cancellation drains I/O and releases directory/event handles");
    std::filesystem::remove_all(root);
    if (failures) return 1;
    std::cout << "directory change watch tests passed\n";
    return 0;
}

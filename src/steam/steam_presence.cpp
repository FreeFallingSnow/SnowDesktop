#include "steam_presence.h"
#include "steam_child_environment.h"

#include <cstdint>
#include <vector>

namespace snowdesktop::steam_presence
{
namespace
{
struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
};
}
Service::Service(std::filesystem::path bridge) : bridge_(std::move(bridge)) {}
Service::~Service() { SetEnabled(false); }

void Service::SetEnabled(bool enabled)
{
    if (enabled == worker_.joinable()) return;
    if (!enabled)
    {
        worker_.request_stop();
        SetEvent(stopEvent_);
        worker_.join();
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
        return;
    }
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    stopEvent_ = CreateEventW(&security, TRUE, FALSE, nullptr);
    if (!stopEvent_) return;
    worker_ = std::jthread([this](std::stop_token stop) { Run(stop); });
}

void Service::Run(std::stop_token stop)
{
    Handle parent{OpenProcess(SYNCHRONIZE, TRUE, GetCurrentProcessId())};
    if (!parent.value) return;
    while (!stop.stop_requested())
    {
        auto environment = BuildSnowDesktopSteamChildEnvironment();
        std::wstring command = L"\"" + bridge_.wstring() + L"\" presence --parent-handle " +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(parent.value)) + L" --stop-handle " +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(stopEvent_));
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::vector<BYTE> storage(bytes);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        bool started = false;
        PROCESS_INFORMATION process{};
        if (!environment.empty() && InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes))
        {
            HANDLE inherited[]{parent.value, stopEvent_};
            if (UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                    inherited, sizeof(inherited), nullptr, nullptr))
                started = CreateProcessW(bridge_.c_str(), command.data(), nullptr, nullptr, TRUE,
                    CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                    environment.data(), bridge_.parent_path().c_str(), &startup.StartupInfo, &process) != FALSE;
            DeleteProcThreadAttributeList(startup.lpAttributeList);
        }
        if (started)
        {
            Handle child{process.hProcess};
            CloseHandle(process.hThread);
            processId_ = process.dwProcessId;
            HANDLE waits[]{stopEvent_, child.value};
            WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (WaitForSingleObject(child.value, 2000) == WAIT_TIMEOUT)
            {
                // Only our exact child is terminated if SDK shutdown stalls.
                TerminateProcess(child.value, ERROR_CANCELLED);
                WaitForSingleObject(child.value, 2000);
            }
            processId_ = 0;
        }
        // Recovery is opt-in and bounded; a missing/broken bridge cannot spin.
        if (WaitForSingleObject(stopEvent_, 30000) != WAIT_TIMEOUT) break;
    }
}
}

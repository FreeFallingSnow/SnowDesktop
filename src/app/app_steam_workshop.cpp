#include "app.h"

#include "steam_app_identity.h"
#include "steam_workshop_cache.h"

namespace
{
struct WorkshopWatchEntry
{
    HANDLE directory = nullptr;
    HANDLE event = nullptr;
    OVERLAPPED overlapped{};
    std::vector<BYTE> buffer;
    bool pending = false;
};

bool IsRelevantWorkshopPath(std::wstring relativePath)
{
    std::replace(relativePath.begin(), relativePath.end(), L'/', L'\\');
    const std::wstring manifest = L"appworkshop_" +
        std::to_wstring(snowdesktop::kSnowDesktopSteamAppId) + L".acf";
    if (_wcsicmp(relativePath.c_str(), manifest.c_str()) == 0)
        return true;

    const std::wstring content = L"content\\" +
        std::to_wstring(snowdesktop::kSnowDesktopSteamAppId);
    if (relativePath.size() < content.size() ||
        _wcsnicmp(relativePath.c_str(), content.c_str(), content.size()) != 0)
        return false;
    return relativePath.size() == content.size() ||
        relativePath[content.size()] == L'\\';
}

bool ContainsRelevantWorkshopChange(const BYTE* buffer, DWORD bytes)
{
    // A zero-byte completion means the kernel notification buffer overflowed;
    // reconcile conservatively because at least one change was lost.
    if (bytes == 0) return true;
    DWORD offset = 0;
    while (offset < bytes)
    {
        const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(
            buffer + offset);
        const std::wstring relativePath(info->FileName,
            info->FileNameLength / sizeof(wchar_t));
        if (IsRelevantWorkshopPath(relativePath)) return true;
        if (info->NextEntryOffset == 0) break;
        offset += info->NextEntryOffset;
    }
    return false;
}

bool ArmWorkshopWatch(WorkshopWatchEntry& watch)
{
    if (watch.pending) return false;
    ResetEvent(watch.event);
    watch.overlapped = {};
    watch.overlapped.hEvent = watch.event;
    watch.pending = ReadDirectoryChangesW(
        watch.directory, watch.buffer.data(),
        static_cast<DWORD>(watch.buffer.size()), TRUE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
        nullptr, &watch.overlapped, nullptr) != FALSE;
    return watch.pending;
}

void CloseWorkshopWatch(WorkshopWatchEntry& watch)
{
    if (watch.directory && watch.directory != INVALID_HANDLE_VALUE)
    {
        if (watch.pending)
        {
            CancelIoEx(watch.directory, &watch.overlapped);
            DWORD ignored = 0;
            // OVERLAPPED and its buffer must remain alive until cancellation
            // has completed. ERROR_OPERATION_ABORTED is the expected result.
            GetOverlappedResult(
                watch.directory, &watch.overlapped, &ignored, TRUE);
            watch.pending = false;
        }
        CloseHandle(watch.directory);
    }
    if (watch.event) CloseHandle(watch.event);
    watch = {};
}
}

void DesktopApp::StartSteamWorkshopWatcher()
{
    if (steamWorkshopWatcherActive_.load()) return;
    if (!hwnd_ || !IsWindow(hwnd_)) return;
    StopSteamWorkshopWatcher();
    steamWorkshopWatcherActive_ = true;

    // Always perform one initial reconciliation. After that, filesystem
    // notifications drive updates and the timer is only a low-frequency
    // recovery path.
    PollSteamWorkshopSubscriptions(true);

    steamWorkshopWatcherStopEvent_ =
        CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!steamWorkshopWatcherStopEvent_)
    {
        steamWorkshopWatcherActive_ = false;
        return;
    }
    steamWorkshopWatcherThread_ = CreateThread(nullptr, 0,
        &DesktopApp::SteamWorkshopWatcherThreadProc, this, 0, nullptr);
    if (!steamWorkshopWatcherThread_)
    {
        CloseHandle(steamWorkshopWatcherStopEvent_);
        steamWorkshopWatcherStopEvent_ = nullptr;
        steamWorkshopWatcherActive_ = false;
    }
}

void DesktopApp::StopSteamWorkshopWatcher()
{
    steamWorkshopWatcherActive_ = false;
    HANDLE thread = steamWorkshopWatcherThread_;
    HANDLE stopEvent = steamWorkshopWatcherStopEvent_;
    steamWorkshopWatcherThread_ = nullptr;
    steamWorkshopWatcherStopEvent_ = nullptr;
    if (stopEvent) SetEvent(stopEvent);
    bool stopped = thread == nullptr;
    if (thread)
    {
        if (WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0)
        {
            CloseHandle(thread);
            stopped = true;
        }
        // On timeout, leave the thread handle to the OS. The worker copied all
        // state it needs before entering its wait loop and never dereferences
        // DesktopApp again, so shutdown cannot create a use-after-free.
    }
    if (stopEvent && stopped) CloseHandle(stopEvent);
}

DWORD WINAPI DesktopApp::SteamWorkshopWatcherThreadProc(LPVOID parameter)
{
    auto* self = static_cast<DesktopApp*>(parameter);
    const HANDLE stopEvent = self->steamWorkshopWatcherStopEvent_;
    const HWND notifyWindow = self->hwnd_;
    if (!stopEvent || !notifyWindow) return 0;

    for (;;)
    {
        if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;

        std::string discoveryError;
        const auto libraries = snowdesktop::widget::DiscoverSteamLibraryRoots(
            snowdesktop::kSnowDesktopSteamAppId, discoveryError);
        // ReadDirectoryChangesW retains the OVERLAPPED and buffer addresses.
        // Heap-own each entry so vector growth never moves armed storage.
        std::vector<std::unique_ptr<WorkshopWatchEntry>> watches;
        watches.reserve(libraries.size());
        for (const auto& library : libraries)
        {
            if (watches.size() >= MAXIMUM_WAIT_OBJECTS - 1) break;
            const auto workshop = library / L"steamapps" / L"workshop";
            auto watch = std::make_unique<WorkshopWatchEntry>();
            watch->directory = CreateFileW(workshop.c_str(),
                FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
            if (!watch->directory ||
                watch->directory == INVALID_HANDLE_VALUE)
                continue;
            watch->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            watch->buffer.resize(64 * 1024);
            if (!watch->event || !ArmWorkshopWatch(*watch))
            {
                CloseWorkshopWatch(*watch);
                continue;
            }
            watches.push_back(std::move(watch));
        }

        bool rebuildWatches = watches.empty();
        std::vector<HANDLE> waitHandles;
        waitHandles.reserve(watches.size() + 1);
        while (!rebuildWatches)
        {
            waitHandles.clear();
            waitHandles.push_back(stopEvent);
            for (const auto& watch : watches)
                waitHandles.push_back(watch->event);
            const DWORD waitResult = WaitForMultipleObjects(
                static_cast<DWORD>(waitHandles.size()), waitHandles.data(),
                FALSE, INFINITE);
            if (waitResult == WAIT_OBJECT_0) break;
            if (waitResult == WAIT_FAILED)
            {
                rebuildWatches = true;
                break;
            }
            const DWORD index = waitResult - WAIT_OBJECT_0;
            if (index == 0 || index > watches.size())
            {
                rebuildWatches = true;
                break;
            }
            auto& watch = *watches[index - 1];
            DWORD bytes = 0;
            const bool completed = GetOverlappedResult(
                watch.directory, &watch.overlapped, &bytes, FALSE) != FALSE;
            watch.pending = false;
            const DWORD completionError = completed
                ? ERROR_SUCCESS : GetLastError();
            const bool relevant = completed
                ? ContainsRelevantWorkshopChange(watch.buffer.data(), bytes)
                : completionError == ERROR_NOTIFY_ENUM_DIR;
            if (!ArmWorkshopWatch(watch))
            {
                rebuildWatches = true;
                break;
            }
            if (relevant && IsWindow(notifyWindow))
                PostMessageW(notifyWindow,
                    kSteamWorkshopSubscriptionChangedMessage, 0, 0);
        }
        for (auto& watch : watches) CloseWorkshopWatch(*watch);
        if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
        // Steam may be starting, changing accounts, or bringing a library
        // back online. Retry discovery without leaving active state stuck.
        if (WaitForSingleObject(stopEvent, 5000) == WAIT_OBJECT_0) break;
    }
    return 0;
}

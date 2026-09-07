#include "shell_launch_worker.h"
#include "shell_launch_process.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <shlwapi.h>
#include <wrl/client.h>

namespace
{

int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

struct BlockingExecutorState
{
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::wstring> paths;
    std::thread::id executionThread;
    bool releaseFirst = false;
    int finished = 0;
};

bool WaitForPathCount(
    const std::shared_ptr<BlockingExecutorState>& state,
    size_t expected)
{
    std::unique_lock<std::mutex> lock(state->mutex);
    return state->cv.wait_for(
        lock, std::chrono::seconds(5), [&] {
            return state->paths.size() >= expected;
        });
}

void TestLaunchesAreCopiedAndRunOffTheCallerThread()
{
    auto state = std::make_shared<BlockingExecutorState>();
    snowdesktop::ShellLaunchWorker worker(
        [state](HWND, const std::wstring& path,
            PCIDLIST_ABSOLUTE, int) {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->executionThread = std::this_thread::get_id();
            state->paths.push_back(path);
            state->cv.notify_all();
            if (state->paths.size() == 1)
            {
                state->cv.wait(lock, [&] {
                    return state->releaseFirst;
                });
            }
            ++state->finished;
            state->cv.notify_all();
            return true;
        });

    const std::thread::id callerThread = std::this_thread::get_id();
    std::wstring firstPath = L"first.lnk";
    Check(
        worker.Enqueue(nullptr, firstPath),
        "the first launch request must be accepted");
    Check(
        WaitForPathCount(state, 1),
        "the first launch request must reach the worker");

    firstPath.assign(L"mutated-after-enqueue.lnk");
    Check(
        worker.Enqueue(nullptr, L"second.txt"),
        "a producer must remain responsive while the worker is blocked");

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->releaseFirst = true;
    }
    state->cv.notify_all();
    Check(
        WaitForPathCount(state, 2),
        "the queued launch must run after the blocked launch completes");

    {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->cv.wait_for(
            lock, std::chrono::seconds(5), [&] {
                return state->finished == 2;
            });
        Check(
            state->paths.size() == 2 &&
                state->paths[0] == L"first.lnk" &&
                state->paths[1] == L"second.txt",
            "the worker must preserve copied paths and FIFO ordering");
        Check(
            state->executionThread != callerThread,
            "Shell execution must not run on the enqueueing UI thread");
    }
    worker.Stop();
}

void TestStopDoesNotJoinABlockedShellHandler()
{
    auto state = std::make_shared<BlockingExecutorState>();
    snowdesktop::ShellLaunchWorker worker(
        [state](HWND, const std::wstring& path,
            PCIDLIST_ABSOLUTE, int) {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->paths.push_back(path);
            state->cv.notify_all();
            state->cv.wait(lock, [&] {
                return state->releaseFirst;
            });
            ++state->finished;
            state->cv.notify_all();
            return true;
        });

    Check(
        worker.Enqueue(nullptr, L"blocked.lnk"),
        "the blocking launch request must be accepted");
    Check(
        WaitForPathCount(state, 1),
        "the blocking launch request must start");

    const auto start = std::chrono::steady_clock::now();
    worker.Stop();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    Check(
        elapsed < std::chrono::milliseconds(250),
        "shutdown must not join a Shell handler blocked in third-party code");
    Check(
        !worker.Enqueue(nullptr, L"after-stop.txt"),
        "a stopped worker must reject new launch requests");

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->releaseFirst = true;
    }
    state->cv.notify_all();
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        Check(
            state->cv.wait_for(
                lock, std::chrono::seconds(5), [&] {
                    return state->finished == 1;
                }),
            "a detached in-flight launch must retain safe worker state");
    }
}

void TestInvalidRequestsAreRejected()
{
    snowdesktop::ShellLaunchWorker worker(
        [](HWND, const std::wstring&, PCIDLIST_ABSOLUTE, int) {
            return true;
        });
    Check(
        !worker.Enqueue(nullptr, L""),
        "an empty launch path must be rejected");
    Check(
        !worker.EnqueueShellItem(nullptr, L"shortcut.lnk", nullptr),
        "a Shell item launch without a PIDL must be rejected");
    worker.Stop();
}

void TestShellItemPidlIsCopiedBeforeExecution()
{
    struct State
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool firstStarted = false;
        bool releaseFirst = false;
        bool shellItemFinished = false;
        bool receivedDifferentPidl = false;
        unsigned char copiedPayload = 0;
    };

    auto state = std::make_shared<State>();
    PCIDLIST_ABSOLUTE originalPidl = nullptr;
    snowdesktop::ShellLaunchWorker worker(
        [state, &originalPidl](HWND, const std::wstring& path,
            PCIDLIST_ABSOLUTE absolutePidl, int) {
            std::unique_lock<std::mutex> lock(state->mutex);
            if (path == L"blocker.txt")
            {
                state->firstStarted = true;
                state->cv.notify_all();
                state->cv.wait(lock, [&] {
                    return state->releaseFirst;
                });
                return true;
            }

            state->receivedDifferentPidl =
                absolutePidl && absolutePidl != originalPidl;
            if (absolutePidl)
            {
                state->copiedPayload =
                    reinterpret_cast<const unsigned char*>(
                        absolutePidl)[2];
            }
            state->shellItemFinished = true;
            state->cv.notify_all();
            return true;
        });

    Check(
        worker.Enqueue(nullptr, L"blocker.txt"),
        "the blocking launch must be queued before the PIDL copy test");
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        Check(
            state->cv.wait_for(
                lock, std::chrono::seconds(5), [&] {
                    return state->firstStarted;
                }),
            "the blocking launch must start before queuing the Shell item");
    }

    alignas(ITEMIDLIST) std::array<unsigned char, 8> pidlBytes{
        6, 0, 0x2A, 0x11, 0x22, 0x33, 0, 0
    };
    originalPidl = reinterpret_cast<PCIDLIST_ABSOLUTE>(
        pidlBytes.data());
    Check(
        worker.EnqueueShellItem(
            nullptr, L"shortcut.lnk", originalPidl),
        "a Shell item PIDL must be accepted and copied");
    pidlBytes[2] = 0x7E;

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->releaseFirst = true;
    }
    state->cv.notify_all();
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        Check(
            state->cv.wait_for(
                lock, std::chrono::seconds(5), [&] {
                    return state->shellItemFinished;
                }),
            "the copied Shell item PIDL must reach the worker");
        Check(
            state->receivedDifferentPidl,
            "the worker must not borrow the caller's PIDL allocation");
        Check(
            state->copiedPayload == 0x2A,
            "the worker must preserve PIDL bytes present at enqueue time");
    }
    worker.Stop();
}

void TestAdministratorShortcutMetadataIsDetected()
{
    const HRESULT comResult = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED);
    Check(
        SUCCEEDED(comResult),
        "the administrator shortcut test must initialize COM");
    if (FAILED(comResult))
        return;

    wchar_t modulePath[32768]{};
    wchar_t tempPath[MAX_PATH]{};
    GUID identifier{};
    wchar_t identifierText[64]{};
    const bool pathsReady = GetModuleFileNameW(
            nullptr, modulePath,
            static_cast<DWORD>(std::size(modulePath))) > 0 &&
        GetTempPathW(
            static_cast<DWORD>(std::size(tempPath)), tempPath) > 0 &&
        SUCCEEDED(CoCreateGuid(&identifier)) &&
        StringFromGUID2(
            identifier, identifierText,
            static_cast<int>(std::size(identifierText))) > 0;
    Check(pathsReady,
        "the administrator shortcut test paths must be available");

    std::wstring linkPath;
    std::wstring manifestLinkPath;
    Microsoft::WRL::ComPtr<IShellLinkW> shellLink;
    Microsoft::WRL::ComPtr<IPersistFile> persistFile;
    Microsoft::WRL::ComPtr<IShellLinkDataList> dataList;
    constexpr CLSID shellLinkClsid{
        0x00021401, 0x0000, 0x0000,
        { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 }
    };
    if (pathsReady)
    {
        linkPath = std::wstring(tempPath) +
            L"SnowDesktopAdministratorShortcut-" +
            identifierText + L".lnk";
        const bool ordinaryLinkCreated = SUCCEEDED(CoCreateInstance(
                shellLinkClsid, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(shellLink.GetAddressOf()))) &&
            shellLink &&
            SUCCEEDED(shellLink->SetPath(modulePath)) &&
            SUCCEEDED(shellLink.As(&persistFile)) && persistFile &&
            SUCCEEDED(persistFile->Save(linkPath.c_str(), TRUE));
        Check(ordinaryLinkCreated,
            "an ordinary shortcut fixture must be created");
        if (ordinaryLinkCreated)
        {
            Check(
                !snowdesktop::ShellLaunchWorker::
                    ShortcutRequestsAdministrator(linkPath),
                "ordinary shortcuts must keep normal Open behavior");
        }

        DWORD flags = 0;
        const bool runAsFlagSaved = ordinaryLinkCreated &&
            SUCCEEDED(shellLink.As(&dataList)) && dataList &&
            SUCCEEDED(dataList->GetFlags(&flags)) &&
            SUCCEEDED(dataList->SetFlags(flags | SLDF_RUNAS_USER)) &&
            SUCCEEDED(persistFile->Save(linkPath.c_str(), TRUE));
        Check(runAsFlagSaved,
            "the run-as-user flag must be saved to the shortcut fixture");
        if (runAsFlagSaved)
        {
            Check(
                snowdesktop::ShellLaunchWorker::
                    ShortcutRequestsAdministrator(linkPath),
                "the SLDF_RUNAS_USER flag must select administrator launch");
        }

        wchar_t windowsDirectory[MAX_PATH]{};
        const UINT windowsDirectoryLength = GetWindowsDirectoryW(
            windowsDirectory,
            static_cast<UINT>(std::size(windowsDirectory)));
        manifestLinkPath = std::wstring(tempPath) +
            L"SnowDesktopManifestAdministratorShortcut-" +
            identifierText + L".lnk";
        const std::wstring regeditPath =
            windowsDirectoryLength > 0 &&
                windowsDirectoryLength < std::size(windowsDirectory)
            ? std::wstring(windowsDirectory) + L"\\regedit.exe"
            : std::wstring();

        Microsoft::WRL::ComPtr<IShellLinkW> manifestShellLink;
        Microsoft::WRL::ComPtr<IPersistFile> manifestPersistFile;
        const bool manifestLinkCreated = !regeditPath.empty() &&
            SUCCEEDED(CoCreateInstance(
                shellLinkClsid, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(manifestShellLink.GetAddressOf()))) &&
            manifestShellLink &&
            SUCCEEDED(manifestShellLink->SetPath(regeditPath.c_str())) &&
            SUCCEEDED(manifestShellLink.As(&manifestPersistFile)) &&
            manifestPersistFile &&
            SUCCEEDED(manifestPersistFile->Save(
                manifestLinkPath.c_str(), TRUE));
        Check(manifestLinkCreated,
            "an executable-manifest shortcut fixture must be created");
        if (manifestLinkCreated)
        {
            Check(
                snowdesktop::ShellLaunchWorker::
                    ShortcutRequestsAdministrator(manifestLinkPath),
                "a highestAvailable target manifest must select administrator launch");
        }
    }

    Check(
        !snowdesktop::ShellLaunchWorker::
            ShortcutRequestsAdministrator(L"C:\\Temp\\ordinary.txt"),
        "non-shortcut paths must not select administrator launch");

    dataList.Reset();
    persistFile.Reset();
    shellLink.Reset();
    if (!linkPath.empty())
        DeleteFileW(linkPath.c_str());
    if (!manifestLinkPath.empty())
        DeleteFileW(manifestLinkPath.c_str());
    CoUninitialize();
}

void TestIsolatedOpenLaunchesShortcut()
{
    const HRESULT comResult = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED);
    Check(
        SUCCEEDED(comResult),
        "the shortcut integration test must initialize COM");
    if (FAILED(comResult))
        return;

    wchar_t modulePath[32768]{};
    const DWORD moduleLength = GetModuleFileNameW(
        nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
    wchar_t tempPath[MAX_PATH]{};
    GUID identifier{};
    wchar_t identifierText[64]{};
    const bool pathsReady = moduleLength > 0 &&
        moduleLength < std::size(modulePath) &&
        GetTempPathW(static_cast<DWORD>(std::size(tempPath)), tempPath) > 0 &&
        SUCCEEDED(CoCreateGuid(&identifier)) &&
        StringFromGUID2(
            identifier, identifierText,
            static_cast<int>(std::size(identifierText))) > 0;
    Check(pathsReady, "the shortcut integration test paths must be available");

    std::wstring linkPath;
    std::wstring eventName;
    HANDLE launchedEvent = nullptr;
    PIDLIST_ABSOLUTE absolutePidl = nullptr;
    if (pathsReady)
    {
        linkPath = std::wstring(tempPath) +
            L"SnowDesktopShellLaunchWorker-" + identifierText + L".lnk";
        eventName = std::wstring(
            L"Local\\SnowDesktopShellLaunchWorker-") + identifierText;
        launchedEvent = CreateEventW(
            nullptr, TRUE, FALSE, eventName.c_str());
        Check(
            launchedEvent != nullptr,
            "the shortcut integration test event must be created");
    }

    Microsoft::WRL::ComPtr<IShellLinkW> shellLink;
    Microsoft::WRL::ComPtr<IPersistFile> persistFile;
    const CLSID shellLinkClsid{
        0x00021401, 0x0000, 0x0000,
        { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 }
    };
    if (launchedEvent)
    {
        const std::wstring arguments =
            L"--shell-launch-child " + eventName;
        const HRESULT createResult = CoCreateInstance(
            shellLinkClsid,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(shellLink.GetAddressOf()));
        const bool linkCreated = SUCCEEDED(createResult) && shellLink &&
            SUCCEEDED(shellLink->SetPath(modulePath)) &&
            SUCCEEDED(shellLink->SetArguments(arguments.c_str())) &&
            SUCCEEDED(shellLink.As(&persistFile)) && persistFile &&
            SUCCEEDED(persistFile->Save(linkPath.c_str(), TRUE));
        Check(
            linkCreated,
            "the integration test must create a real .lnk shortcut");
        if (linkCreated)
        {
            Check(
                SUCCEEDED(SHParseDisplayName(
                    linkPath.c_str(), nullptr,
                    &absolutePidl, 0, nullptr)) && absolutePidl,
                "the integration test shortcut must have an absolute PIDL");
        }
    }

    if (absolutePidl)
    {
        Check(
            snowdesktop::ShellLaunchWorker::Execute(
                nullptr, linkPath, absolutePidl),
            "the Shell context-menu Open command must accept the shortcut");
        Check(
            WaitForSingleObject(launchedEvent, 10000) == WAIT_OBJECT_0,
            "the shortcut must launch through its Shell context menu");

        ResetEvent(launchedEvent);
        Check(
            snowdesktop::ShellLaunchWorker::ExecuteInteractive(
                nullptr, linkPath, absolutePidl),
            "interactive Shell context-menu Open must accept the shortcut");
        Check(
            WaitForSingleObject(launchedEvent, 10000) == WAIT_OBJECT_0,
            "the shortcut must launch through the isolated interactive Open request");
    }

    if (absolutePidl)
        CoTaskMemFree(absolutePidl);
    shellLink.Reset();
    persistFile.Reset();
    if (!linkPath.empty())
        DeleteFileW(linkPath.c_str());
    if (launchedEvent)
        CloseHandle(launchedEvent);
    CoUninitialize();
}

std::wstring UniqueEventName(const wchar_t* prefix)
{
    GUID id{};
    wchar_t text[64]{};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, text, 64)) return {};
    return std::wstring(L"Local\\SnowDesktopShellProcess-") + prefix + text;
}

void TestRequestPayloadPreservesPathsAndRejectsInvalidPidls()
{
    namespace process = snowdesktop::shell_launch_process;
    process::Request request;
    request.path = L"C:\\用户目录\\开题答辩\\带 空格 & 引号\".lnk";
    request.owner = reinterpret_cast<HWND>(std::uintptr_t{0x1234});
    request.showCommand = SW_SHOWMAXIMIZED;
    request.action = process::Action::OpenWithShortcutPolicy;
    request.absolutePidl = {6, 0, 0x2A, 0x11, 0x22, 0x33, 0, 0};
    const auto encoded = process::Encode(request);
    const auto decoded = process::Decode(encoded);
    Check(decoded && decoded->path == request.path &&
            decoded->absolutePidl == request.absolutePidl &&
            decoded->owner == request.owner &&
            decoded->showCommand == request.showCommand && decoded->action == request.action,
        "the helper transport must preserve Unicode, shell metacharacters and complete PIDL bytes");
    for (std::size_t size = 0; size < encoded.size(); ++size)
    {
        if (process::Decode(std::span(encoded.data(), size)))
        {
            Check(false, "a truncated helper payload must never reach Shell code");
            break;
        }
    }
    auto malformed = encoded;
    malformed[12] = 0xFF;
    malformed[13] = 0xFF;
    malformed[14] = 0xFF;
    malformed[15] = 0xFF;
    Check(!process::Decode(malformed), "oversized path lengths must be rejected before allocation");
    malformed = encoded;
    malformed[4] = 2;
    Check(!process::Decode(malformed), "unknown helper protocol versions must be rejected");
    malformed = encoded;
    malformed.back() = 1;
    Check(!process::Decode(malformed), "a PIDL without a complete terminal item must be rejected");
    request.path.push_back(L'\0');
    Check(process::Encode(request).empty(), "embedded NUL must not silently truncate an open target");
    request.path = L"valid";
    request.absolutePidl = {1, 0, 0, 0};
    Check(process::Encode(request).empty(), "PIDL items shorter than their size field must be rejected");
}

bool ExecuteHelperFixture(const snowdesktop::shell_launch_process::Request& request)
{
    constexpr std::wstring_view blockPrefix = L"test:block:";
    constexpr std::wstring_view signalPrefix = L"test:signal:";
    const std::wstring_view path(request.path);
    const bool block = path.starts_with(blockPrefix);
    if (!block && !path.starts_with(signalPrefix))
        return snowdesktop::shell_launch_process::ExecuteRequest(request);
    const std::wstring eventName(path.substr(block ? blockPrefix.size() : signalPrefix.size()));
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str());
    if (!event) return false;
    const BOOL signaled = SetEvent(event);
    CloseHandle(event);
    if (block)
    {
        // Deliberately never finish this one request. Only the supervised
        // helper is blocked; no third-party registration or live desktop UI.
        HANDLE neverSignaled = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!neverSignaled) return false;
        WaitForSingleObject(neverSignaled, INFINITE);
        CloseHandle(neverSignaled);
    }
    return signaled != FALSE;
}

void TestBlockedHelperDoesNotSerializeLaterOpensAndIsReaped()
{
    namespace process = snowdesktop::shell_launch_process;
    const auto startedName = UniqueEventName(L"blocked-");
    const auto nextName = UniqueEventName(L"next-");
    HANDLE started = CreateEventW(nullptr, TRUE, FALSE, startedName.c_str());
    HANDLE next = CreateEventW(nullptr, TRUE, FALSE, nextName.c_str());
    Check(started && next && !startedName.empty() && !nextName.empty(),
        "the helper isolation events must be created");
    if (!started || !next) return;
    process::Request request;
    request.path = L"test:block:" + startedName;
    const auto blocked = process::Start(request, 8000);
    Check(static_cast<bool>(blocked), "the blocked helper must be dispatched");
    HANDLE child = blocked ? OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, blocked.id) : nullptr;
    Check(child != nullptr, "the exact blocked helper process must be observable");
    if (child)
    {
        Check(WaitForSingleObject(started, 5000) == WAIT_OBJECT_0,
            "the helper must start executing the blocking fixture");
        request.path = L"test:signal:" + nextName;
        const auto following = process::Start(request);
        Check(following && following.id != blocked.id,
            "a later open must use an independent helper process");
        Check(WaitForSingleObject(next, 5000) == WAIT_OBJECT_0,
            "a later open must complete while the previous Shell handler is blocked");
        Check(WaitForSingleObject(child, 0) == WAIT_TIMEOUT,
            "the successor must run before the blocked request reaches its deadline");
        Check(WaitForSingleObject(child, 10000) == WAIT_OBJECT_0,
            "a blocked helper must be reaped within its bounded deadline");
        DWORD result = 0;
        Check(GetExitCodeProcess(child, &result) && result == ERROR_TIMEOUT,
            "the deadline must terminate only the stuck helper with a timeout result");
        CloseHandle(child);
    }
    CloseHandle(started);
    CloseHandle(next);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (const auto result = snowdesktop::shell_launch_process::TryRunCommand(ExecuteHelperFixture))
        return *result;
    if (argc == 3 &&
        wcscmp(argv[1], L"--shell-launch-child") == 0)
    {
        HANDLE launchedEvent = OpenEventW(
            EVENT_MODIFY_STATE, FALSE, argv[2]);
        if (!launchedEvent)
            return 2;
        const BOOL signaled = SetEvent(launchedEvent);
        CloseHandle(launchedEvent);
        return signaled ? 0 : 3;
    }

    TestLaunchesAreCopiedAndRunOffTheCallerThread();
    TestStopDoesNotJoinABlockedShellHandler();
    TestInvalidRequestsAreRejected();
    TestShellItemPidlIsCopiedBeforeExecution();
    TestAdministratorShortcutMetadataIsDetected();
    TestRequestPayloadPreservesPathsAndRejectsInvalidPidls();
    TestBlockedHelperDoesNotSerializeLaterOpensAndIsReaped();
    TestIsolatedOpenLaunchesShortcut();
    if (failures != 0)
    {
        std::cerr << failures
                  << " Shell launch worker test(s) failed\n";
        return 1;
    }
    std::cout << "All Shell launch worker tests passed\n";
    return 0;
}

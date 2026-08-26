#include "shell_launch_worker.h"

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

void TestShellContextMenuOpenLaunchesShortcut()
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

} // namespace

int wmain(int argc, wchar_t** argv)
{
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
    TestShellContextMenuOpenLaunchesShortcut();
    if (failures != 0)
    {
        std::cerr << failures
                  << " Shell launch worker test(s) failed\n";
        return 1;
    }
    std::cout << "All Shell launch worker tests passed\n";
    return 0;
}

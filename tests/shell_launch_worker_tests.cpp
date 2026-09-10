#include "shell_launch_worker.h"
#include "shell_launch_process.h"
#include "shell_open_command.h"

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
#include <exdisp.h>
#include <shellapi.h>
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

// Models the shell extension boundary that previously loaded an unrelated
// handler during a double-click. The failure is deterministic, not a timeout.
class DefaultOpenMenu final : public IContextMenu
{
public:
    bool rejectQuery = false;
    bool rejectInvoke = false;
    bool namedOpen = true;
    int invokes = 0;
    bool selectedCommand = false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** value) override
    { *value = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU menu, UINT index,
        UINT first, UINT, UINT flags) override
    {
        if (rejectQuery || !(flags & CMF_DEFAULTONLY)) return E_FAIL;
        InsertMenuW(menu, index, MF_BYPOSITION | MF_STRING, first + 2, L"Default");
        SetMenuDefaultItem(menu, first + 2, FALSE);
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 3);
    }
    HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR command, UINT flags,
        UINT*, LPSTR name, UINT count) override
    {
        if (namedOpen && command == 2 && flags == GCS_VERBW)
            return wcscpy_s(reinterpret_cast<wchar_t*>(name), count, L"open") == 0 ? S_OK : E_FAIL;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE InvokeCommand(LPCMINVOKECOMMANDINFO info) override
    {
        ++invokes;
        selectedCommand = IS_INTRESOURCE(info->lpVerb) &&
            LOWORD(reinterpret_cast<ULONG_PTR>(info->lpVerb)) == 2 &&
            (info->fMask & CMIC_MASK_NOASYNC) != 0 && info->nShow == SW_SHOWNORMAL;
        return rejectInvoke ? E_FAIL : S_OK;
    }
};

void TestDefaultOpenDoesNotPrepareUnrelatedMenus()
{
    DefaultOpenMenu menu;
    Check(snowdesktop::shell_open_command::Invoke(&menu, nullptr, SW_SHOWNORMAL) &&
        menu.invokes == 1 && menu.selectedCommand,
        "default Open must activate once without requesting unrelated extension menus");
    menu.namedOpen = false;
    Check(snowdesktop::shell_open_command::Invoke(&menu, nullptr, SW_SHOWNORMAL) &&
        menu.invokes == 2 && menu.selectedCommand,
        "a Shell default command without a canonical name must remain usable");
    menu.rejectQuery = true;
    Check(!snowdesktop::shell_open_command::Invoke(&menu, nullptr, SW_SHOWNORMAL) && menu.invokes == 2,
        "failed menu preparation must not invoke an unprepared command");
    menu.rejectQuery = false;
    menu.rejectInvoke = true;
    Check(!snowdesktop::shell_open_command::Invoke(&menu, nullptr, SW_SHOWNORMAL) && menu.invokes == 3,
        "a failed Shell command must be reported without another invocation");
}

bool SameFolder(const std::wstring& first, const std::wstring& second)
{
    const auto identity = [](const std::wstring& path, BY_HANDLE_FILE_INFORMATION& info) {
        const HANDLE file = CreateFileW(path.c_str(), 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        const bool ok = GetFileInformationByHandle(file, &info) != FALSE;
        CloseHandle(file);
        return ok;
    };
    BY_HANDLE_FILE_INFORMATION a{}, b{};
    return identity(first, a) && identity(second, b) &&
        a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
        a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
}

void TestIsolatedFolderActivation()
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Check(SUCCEEDED(com), "folder activation must initialize COM");
    if (FAILED(com)) return;
    {
        Microsoft::WRL::ComPtr<IShellWindows> windows;
        Check(SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&windows))), "folder activation must observe real Explorer navigation");
        // Separate fresh targets prevent an already-open directory from passing.
        // Cover desktop/Dock, path-only callers, ordinary folders and Shell
        // objects with no parsing name. Only this test's windows are closed.
        for (int kind = 0; windows && kind < 4; ++kind)
        {
            GUID id{}; wchar_t idText[64]{}, temp[MAX_PATH]{};
            const bool pathsReady = SUCCEEDED(CoCreateGuid(&id)) &&
                StringFromGUID2(id, idText, 64) > 0 &&
                GetTempPathW(MAX_PATH, temp) > 0 && temp[0];
            Check(pathsReady, "folder activation must obtain isolated fixture paths");
            if (!pathsReady) continue;
            const std::wstring folder = std::wstring(temp) + L"SnowDesktop folder 文件夹-" + idText;
            const std::wstring shortcut = folder + L".lnk";
            bool ready = CreateDirectoryW(folder.c_str(), nullptr) != FALSE;
            Microsoft::WRL::ComPtr<IShellLinkW> link;
            Microsoft::WRL::ComPtr<IPersistFile> persist;
            ready = ready && SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&link))) && SUCCEEDED(link->SetPath(folder.c_str())) &&
                SUCCEEDED(link.As(&persist)) && SUCCEEDED(persist->Save(shortcut.c_str(), TRUE));
            Check(ready, "folder shortcut fixture must be created");
            snowdesktop::shell_launch_process::Request request;
            request.path = kind == 2 ? folder : shortcut;
            request.action = snowdesktop::shell_launch_process::Action::OpenWithShortcutPolicy;
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (kind == 0 || kind == 3)
            {
                ready = ready && SUCCEEDED(SHParseDisplayName(shortcut.c_str(), nullptr, &pidl, 0, nullptr)) && pidl;
                if (pidl)
                {
                    const auto bytes = reinterpret_cast<const unsigned char*>(pidl);
                    request.absolutePidl.assign(bytes, bytes + ILGetSize(pidl));
                }
            }
            if (kind == 3) request.path.clear();
            const auto started = ready ? snowdesktop::shell_launch_process::Start(request, 10000) :
                snowdesktop::shell_launch_process::StartedProcess{};
            Check(static_cast<bool>(started), "folder activation must dispatch the real helper");
            bool observed = false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (started && !observed && std::chrono::steady_clock::now() < deadline)
            {
                long count = 0; windows->get_Count(&count);
                for (long i = 0; i < count; ++i)
                {
                    VARIANT index{}; index.vt = VT_I4; index.lVal = i;
                    Microsoft::WRL::ComPtr<IDispatch> dispatch;
                    Microsoft::WRL::ComPtr<IWebBrowser2> browser;
                    if (FAILED(windows->Item(index, &dispatch)) || !dispatch || FAILED(dispatch.As(&browser))) continue;
                    BSTR url = nullptr; browser->get_LocationURL(&url);
                    wchar_t path[32768]{}; DWORD size = 32768;
                    if (url && SUCCEEDED(PathCreateFromUrlW(url, path, &size, 0)) && SameFolder(folder, path))
                    { observed = true; browser->Quit(); }
                    SysFreeString(url);
                }
                if (!observed)
                {
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                    { TranslateMessage(&message); DispatchMessageW(&message); }
                    MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
                }
            }
            Check(observed, kind == 0 ? "PIDL folder shortcut must actually navigate Explorer" :
                kind == 1 ? "path-only folder shortcut must actually navigate Explorer" :
                kind == 2 ? "ordinary folder must actually navigate Explorer" :
                "PIDL-only Shell item must actually navigate Explorer");
            CoTaskMemFree(pidl); persist.Reset(); link.Reset();
            DeleteFileW(shortcut.c_str()); RemoveDirectoryW(folder.c_str());
        }
    }
    CoUninitialize();
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

        ResetEvent(launchedEvent);
        Check(snowdesktop::ShellLaunchWorker::ExecuteInteractive(nullptr, linkPath, nullptr),
            "path-only application shortcuts must reach the isolated Open request");
        Check(WaitForSingleObject(launchedEvent, 10000) == WAIT_OBJECT_0,
            "path-only application shortcut must preserve its target and arguments");
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
    auto pidlOnly = request;
    pidlOnly.path.clear();
    const auto pidlOnlyDecoded = process::Decode(process::Encode(pidlOnly));
    Check(pidlOnlyDecoded && pidlOnlyDecoded->path.empty() &&
        pidlOnlyDecoded->absolutePidl == request.absolutePidl,
        "Shell objects without a parsing name must retain their complete PIDL");
    pidlOnly.absolutePidl.clear();
    Check(process::Encode(pidlOnly).empty(), "requests with neither a path nor a PIDL must be rejected");
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
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    bool privateHandles = arguments && argumentCount == 4;
    for (int i = 2; privateHandles && i < argumentCount; ++i)
    {
        const auto handle = reinterpret_cast<HANDLE>(
            static_cast<std::uintptr_t>(_wcstoui64(arguments[i], nullptr, 10)));
        DWORD flags = 0;
        privateHandles = GetHandleInformation(handle, &flags) &&
            (flags & HANDLE_FLAG_INHERIT) == 0;
    }
    if (arguments) LocalFree(arguments);
    if (!privateHandles) return false;
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
        wchar_t executable[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
        if (!length || length >= 32768) return false;
        std::wstring command = L"\"" + std::wstring(executable) +
            L"\" --shell-open-survivor " + eventName;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION opened{};
        if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, nullptr, &startup, &opened)) return false;
        CloseHandle(opened.hThread);
        CloseHandle(opened.hProcess);
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
    HANDLE survivor = CreateEventW(nullptr, TRUE, FALSE, (startedName + L"-survivor").c_str());
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, (startedName + L"-release").c_str());
    HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, (startedName + L"-finished").c_str());
    Check(started && next && survivor && release && finished &&
            !startedName.empty() && !nextName.empty(),
        "the helper isolation events must be created");
    if (!started || !next || !survivor || !release || !finished)
    {
        for (HANDLE event : {started, next, survivor, release, finished})
            if (event) CloseHandle(event);
        return;
    }
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
            "the helper must start with its private handles made non-inheritable");
        Check(WaitForSingleObject(survivor, 5000) == WAIT_OBJECT_0,
            "the helper must start the independent target process before blocking");
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
        SetEvent(release);
        Check(WaitForSingleObject(finished, 5000) == WAIT_OBJECT_0,
            "a program opened by the helper must survive timeout cleanup and continue running");
        CloseHandle(child);
    }
    SetEvent(release);
    CloseHandle(started);
    CloseHandle(next);
    CloseHandle(survivor);
    CloseHandle(release);
    CloseHandle(finished);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (const auto result = snowdesktop::shell_launch_process::TryRunCommand(ExecuteHelperFixture))
        return *result;
    if (argc == 2 && wcscmp(argv[1], L"--default-open-contract") == 0)
    {
        TestDefaultOpenDoesNotPrepareUnrelatedMenus();
        return failures ? 1 : 0;
    }
    if (argc == 3 && wcscmp(argv[1], L"--shell-open-survivor") == 0)
    {
        const std::wstring name(argv[2]);
        HANDLE started = OpenEventW(EVENT_MODIFY_STATE, FALSE, (name + L"-survivor").c_str());
        HANDLE release = OpenEventW(SYNCHRONIZE, FALSE, (name + L"-release").c_str());
        HANDLE finished = OpenEventW(EVENT_MODIFY_STATE, FALSE, (name + L"-finished").c_str());
        const bool ready = started && release && finished;
        if (ready) SetEvent(started);
        const bool survived = ready && WaitForSingleObject(release, 20000) == WAIT_OBJECT_0;
        if (survived) SetEvent(finished);
        for (HANDLE event : {started, release, finished})
            if (event) CloseHandle(event);
        return survived ? 0 : 4;
    }
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
    TestDefaultOpenDoesNotPrepareUnrelatedMenus();
    TestIsolatedFolderActivation();
    if (failures != 0)
    {
        std::cerr << failures
                  << " Shell launch worker test(s) failed\n";
        return 1;
    }
    std::cout << "All Shell launch worker tests passed\n";
    return 0;
}

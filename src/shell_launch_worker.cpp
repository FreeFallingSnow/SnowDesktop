#include "shell_launch_worker.h"
#include "shell_context_menu_invoke.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <cstring>
#include <iterator>
#include <utility>

namespace snowdesktop
{
namespace
{

using Microsoft::WRL::ComPtr;

bool SafeInvokeContextMenu(
    IContextMenu* contextMenu,
    LPCMINVOKECOMMANDINFO invoke)
{
    __try
    {
        return contextMenu &&
            SUCCEEDED(contextMenu->InvokeCommand(invoke));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool InvokeShellItemOpen(
    HWND owner,
    PCIDLIST_ABSOLUTE absolutePidl,
    int showCommand)
{
    if (!absolutePidl)
        return false;

    ComPtr<IShellFolder> parentFolder;
    PCUITEMID_CHILD child = nullptr;
    if (FAILED(SHBindToParent(
            absolutePidl,
            IID_PPV_ARGS(parentFolder.GetAddressOf()),
            &child)) ||
        !parentFolder || !child)
    {
        return false;
    }

    const HWND validOwner = owner && IsWindow(owner) ? owner : nullptr;
    ComPtr<IContextMenu> contextMenu;
    if (FAILED(parentFolder->GetUIObjectOf(
            validOwner,
            1,
            &child,
            IID_IContextMenu,
            nullptr,
            reinterpret_cast<void**>(
                contextMenu.GetAddressOf()))) ||
        !contextMenu)
    {
        return false;
    }

    HMENU menu = CreatePopupMenu();
    if (!menu)
        return false;

    constexpr UINT firstCommand = 1;
    constexpr UINT lastCommand = 0x7FFF;
    const HRESULT queryResult = contextMenu->QueryContextMenu(
        menu,
        0,
        firstCommand,
        lastCommand,
        CMF_NORMAL);

    UINT_PTR openOffset = static_cast<UINT_PTR>(-1);
    if (SUCCEEDED(queryResult))
    {
        const UINT commandCount = HRESULT_CODE(queryResult);
        for (UINT offset = 0; offset < commandCount; ++offset)
        {
            wchar_t verb[64]{};
            if (SUCCEEDED(contextMenu->GetCommandString(
                    offset,
                    GCS_VERBW,
                    nullptr,
                    reinterpret_cast<LPSTR>(verb),
                    static_cast<UINT>(std::size(verb)))) &&
                _wcsicmp(verb, L"open") == 0)
            {
                openOffset = offset;
                break;
            }
        }

        if (openOffset == static_cast<UINT_PTR>(-1))
        {
            const UINT defaultCommand = GetMenuDefaultItem(
                menu, FALSE, GMDI_USEDISABLED);
            if (defaultCommand >= firstCommand &&
                defaultCommand <= lastCommand)
            {
                openOffset = defaultCommand - firstCommand;
            }
        }
    }

    CMINVOKECOMMANDINFOEX invoke{};
    invoke.cbSize = sizeof(invoke);
    invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_FLAG_LOG_USAGE;
    invoke.hwnd = validOwner;
    if (openOffset != static_cast<UINT_PTR>(-1))
    {
        invoke.lpVerb = MAKEINTRESOURCEA(openOffset);
        invoke.lpVerbW = MAKEINTRESOURCEW(openOffset);
    }
    else
    {
        invoke.lpVerb = "open";
        invoke.lpVerbW = L"open";
    }
    const std::wstring invocationDirectory =
        DesktopShellInvocationDirectory();
    std::string invocationDirectoryA;
    SetShellInvocationDirectory(
        invoke, invocationDirectory, invocationDirectoryA);
    invoke.nShow = showCommand;

    const bool opened = SafeInvokeContextMenu(
        contextMenu.Get(),
        reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
    DestroyMenu(menu);
    return opened;
}

bool ExecuteShellOpen(
    HWND owner,
    const std::wstring& path,
    PCIDLIST_ABSOLUTE absolutePidl,
    int showCommand,
    ULONG launchMask)
{
    if (path.empty())
        return false;

    if (absolutePidl &&
        InvokeShellItemOpen(owner, absolutePidl, showCommand))
    {
        return true;
    }

    if (path.size() >= 6 &&
        _wcsnicmp(path.c_str(), L"shell:", 6) == 0)
    {
        PIDLIST_ABSOLUTE rawPidl = nullptr;
        const HRESULT parseResult = SHParseDisplayName(
            path.c_str(), nullptr, &rawPidl, 0, nullptr);
        if (SUCCEEDED(parseResult) && rawPidl)
        {
            SHELLEXECUTEINFOW namespaceExecuteInfo{};
            namespaceExecuteInfo.cbSize = sizeof(namespaceExecuteInfo);
            namespaceExecuteInfo.fMask = launchMask | SEE_MASK_IDLIST;
            namespaceExecuteInfo.hwnd =
                owner && IsWindow(owner) ? owner : nullptr;
            namespaceExecuteInfo.lpIDList = rawPidl;
            namespaceExecuteInfo.nShow = showCommand;
            const bool opened =
                ShellExecuteExW(&namespaceExecuteInfo) != FALSE;
            CoTaskMemFree(rawPidl);
            if (opened)
                return true;
        }
        else if (rawPidl)
        {
            CoTaskMemFree(rawPidl);
        }
    }

    SHELLEXECUTEINFOW executeInfo{};
    executeInfo.cbSize = sizeof(executeInfo);
    executeInfo.fMask = launchMask;
    executeInfo.hwnd = owner && IsWindow(owner) ? owner : nullptr;
    executeInfo.lpVerb = L"open";
    executeInfo.lpFile = path.c_str();
    executeInfo.nShow = showCommand;
    return ShellExecuteExW(&executeInfo) != FALSE;
}

} // namespace

ShellLaunchWorker::ShellLaunchWorker()
    : ShellLaunchWorker(&ShellLaunchWorker::Execute)
{
}

ShellLaunchWorker::ShellLaunchWorker(Executor executor)
    : state_(std::make_shared<State>(std::move(executor)))
{
}

ShellLaunchWorker::~ShellLaunchWorker()
{
    Stop();
}

bool ShellLaunchWorker::Enqueue(
    HWND owner,
    std::wstring path,
    int showCommand)
{
    if (path.empty())
        return false;

    Task task;
    task.owner = owner;
    task.path = std::move(path);
    task.showCommand = showCommand;
    return EnqueueTask(std::move(task));
}

bool ShellLaunchWorker::EnqueueShellItem(
    HWND owner,
    std::wstring path,
    PCIDLIST_ABSOLUTE absolutePidl,
    int showCommand)
{
    if (path.empty() || !absolutePidl)
        return false;

    const UINT pidlSize = ILGetSize(absolutePidl);
    if (pidlSize == 0)
        return false;

    Task task;
    task.owner = owner;
    task.path = std::move(path);
    task.showCommand = showCommand;
    try
    {
        task.absolutePidl.resize(pidlSize);
    }
    catch (...)
    {
        return false;
    }
    std::memcpy(
        task.absolutePidl.data(), absolutePidl, pidlSize);
    return EnqueueTask(std::move(task));
}

bool ShellLaunchWorker::EnqueueTask(Task task)
{
    if (!state_ || !state_->executor)
        return false;

    auto state = state_;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->stopping)
            return false;
        try
        {
            state->tasks.push_back(std::move(task));
            if (!thread_.joinable())
                thread_ = std::thread(&ShellLaunchWorker::Run, state);
        }
        catch (...)
        {
            if (!state->tasks.empty())
                state->tasks.pop_back();
            return false;
        }
    }
    state->cv.notify_one();
    return true;
}

void ShellLaunchWorker::Stop()
{
    if (!state_)
        return;

    auto state = state_;
    bool joinWorker = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->stopping)
        {
            state->stopping = true;
            state->tasks.clear();
        }
        joinWorker = !state->executing;
    }
    state->cv.notify_all();

    if (!thread_.joinable())
        return;
    if (joinWorker)
        thread_.join();
    else
        thread_.detach();
}

bool ShellLaunchWorker::Execute(
    HWND owner,
    const std::wstring& path,
    PCIDLIST_ABSOLUTE absolutePidl,
    int showCommand)
{
    constexpr ULONG launchMask =
        SEE_MASK_NOASYNC | SEE_MASK_FLAG_LOG_USAGE;
    return ExecuteShellOpen(
        owner, path, absolutePidl, showCommand, launchMask);
}

bool ShellLaunchWorker::ExecuteInteractive(
    HWND owner,
    const std::wstring& path,
    PCIDLIST_ABSOLUTE absolutePidl,
    int showCommand)
{
    constexpr ULONG launchMask =
        SEE_MASK_ASYNCOK | SEE_MASK_FLAG_LOG_USAGE;
    return ExecuteShellOpen(
        owner, path, absolutePidl, showCommand, launchMask);
}

void ShellLaunchWorker::Run(const std::shared_ptr<State>& state)
{
    const HRESULT comResult = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult))
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->tasks.clear();
        state->stopping = true;
        return;
    }

    for (;;)
    {
        Task task;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->cv.wait(lock, [&] {
                return state->stopping || !state->tasks.empty();
            });
            if (state->stopping && state->tasks.empty())
                break;
            task = std::move(state->tasks.front());
            state->tasks.pop_front();
            state->executing = true;
        }

        try
        {
            const auto absolutePidl = task.absolutePidl.empty()
                ? nullptr
                : reinterpret_cast<PCIDLIST_ABSOLUTE>(
                    task.absolutePidl.data());
            state->executor(
                task.owner,
                task.path,
                absolutePidl,
                task.showCommand);
        }
        catch (...)
        {
            // A failing injected/custom executor must not terminate the
            // worker or discard subsequent user launch requests.
        }

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->executing = false;
        }
    }

    CoUninitialize();
}

} // namespace snowdesktop

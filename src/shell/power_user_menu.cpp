#include "power_user_menu.h"

#include "common/l10n.h"
#include "system/controls/system_control_windows.h"

#include <powrprof.h>
#include <shellapi.h>
#include <shldisp.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <chrono>
#include <type_traits>
#include <memory>
#include <string>

namespace snowdesktop::power_user_menu
{
namespace
{
enum Command : UINT
{
    InstalledApps = 47000, MobilityCenter, PowerOptions, EventViewer, System,
    DeviceManager, NetworkConnections, DiskManagement, ComputerManagement,
    Terminal, TerminalAdmin, TaskManager, Settings, FileExplorer, Search, Run,
    SignOut, Sleep, Hibernate, ShutDown, Restart, Desktop
};
struct MenuDeleter { void operator()(HMENU menu) const noexcept { DestroyMenu(menu); } };
using Menu = std::unique_ptr<std::remove_pointer_t<HMENU>, MenuDeleter>;
struct Entry { UINT command; const char* label; };
constexpr Entry management[]{
    {InstalledApps, L10N_KEY("winx.installedApps")},
    {MobilityCenter, L10N_KEY("winx.mobilityCenter")},
    {PowerOptions, L10N_KEY("winx.powerOptions")},
    {EventViewer, L10N_KEY("winx.eventViewer")},
    {System, L10N_KEY("winx.system")},
    {DeviceManager, L10N_KEY("winx.deviceManager")},
    {NetworkConnections, L10N_KEY("winx.networkConnections")},
    {DiskManagement, L10N_KEY("winx.diskManagement")},
    {ComputerManagement, L10N_KEY("winx.computerManagement")},
    {Terminal, L10N_KEY("winx.terminal")},
    {TerminalAdmin, L10N_KEY("winx.terminalAdmin")}
};
constexpr Entry common[]{
    {TaskManager, L10N_KEY("statusBar.taskManager")},
    {Settings, L10N_KEY("winx.settings")},
    {FileExplorer, L10N_KEY("winx.fileExplorer")},
    {Search, L10N_KEY("winx.search")},
    {Run, L10N_KEY("winx.run")}
};

std::wstring SystemFile(const wchar_t* name)
{
    wchar_t folder[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(folder, MAX_PATH);
    return length && length < MAX_PATH ? std::wstring(folder) + L"\\" + name : std::wstring{};
}

DWORD Launch(HWND owner, const std::wstring& file, const wchar_t* parameters = nullptr,
    const wchar_t* verb = L"open")
{
    if (file.empty()) return ERROR_FILE_NOT_FOUND;
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_FLAG_NO_UI;
    info.hwnd = owner;
    info.lpVerb = verb;
    info.lpFile = file.c_str();
    info.lpParameters = parameters;
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info)) return ERROR_SUCCESS;
    const DWORD error = GetLastError();
    return error ? error : ERROR_GEN_FAILURE;
}

DWORD LaunchTerminal(HWND owner, bool administrator)
{
    const wchar_t* verb = administrator ? L"runas" : L"open";
    PWSTR local = nullptr;
    DWORD result = ERROR_FILE_NOT_FOUND;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
    {
        const std::wstring terminal = std::wstring(local) + L"\\Microsoft\\WindowsApps\\wt.exe";
        CoTaskMemFree(local);
        if (GetFileAttributesW(terminal.c_str()) != INVALID_FILE_ATTRIBUTES)
            result = Launch(owner, terminal, L"-w new", verb);
    }
    // Older Windows installations may have no Terminal execution alias.
    // Do not launch a second program after UAC cancellation or access denial.
    if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND)
        result = Launch(owner, SystemFile(L"WindowsPowerShell\\v1.0\\powershell.exe"), nullptr, verb);
    return result;
}

DWORD ShellCommand(UINT command)
{
    Microsoft::WRL::ComPtr<IShellDispatch4> shell;
    HRESULT result = CoCreateInstance(CLSID_Shell, nullptr,
        CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&shell));
    if (SUCCEEDED(result))
        result = command == Run ? shell->FileRun() : shell->ToggleDesktop();
    return SUCCEEDED(result) ? ERROR_SUCCESS : static_cast<DWORD>(result);
}

DWORD PowerCommand(UINT command)
{
    if (command == SignOut)
        return ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_FLAG_PLANNED)
            ? ERROR_SUCCESS : GetLastError();
    system_control::Request request;
    request.name = command == Sleep ? "system.power.sleep" :
        command == Hibernate ? "host.power.hibernate" :
        command == ShutDown ? "system.power.shutdown" : "system.power.restart";
    // A deliberate selection in this native host menu is the confirmation.
    // Lua and other external requests retain their separate confirmation gate.
    request.hostConfirmed = true;
    const system_control::Cancellation cancel{{}, std::chrono::steady_clock::now() + std::chrono::seconds(30)};
    const auto result = system_control::windows::CreatePowerBackend()->Execute(request, cancel);
    return result.ok ? ERROR_SUCCESS : result.platformCode ? result.platformCode : ERROR_GEN_FAILURE;
}
}

HMENU Create(HMENU dockMenu)
{
    if (!dockMenu) return nullptr;
    Menu root(CreatePopupMenu()), power(CreatePopupMenu());
    if (!root || !power) return nullptr;
    const auto append = [&](const Entry& entry) {
        return AppendMenuW(root.get(), MF_STRING, entry.command, _LW(entry.label)) != FALSE;
    };
    for (const auto& entry : management)
        if (!append(entry)) return nullptr;
    if (!AppendMenuW(root.get(), MF_SEPARATOR, 0, nullptr)) return nullptr;
    for (const auto& entry : common)
        if (!append(entry)) return nullptr;
    if (!AppendMenuW(root.get(), MF_SEPARATOR, 0, nullptr) ||
        !AppendMenuW(power.get(), MF_STRING, SignOut, _LW("winx.signOut"))) return nullptr;
    if (IsPwrSuspendAllowed() &&
        !AppendMenuW(power.get(), MF_STRING, Sleep, _LW("controlCenter.sleep"))) return nullptr;
    if (IsPwrHibernateAllowed() &&
        !AppendMenuW(power.get(), MF_STRING, Hibernate, _LW("controlCenter.hibernate"))) return nullptr;
    if (!AppendMenuW(power.get(), MF_STRING, ShutDown, _LW("controlCenter.shutdown")) ||
        !AppendMenuW(power.get(), MF_STRING, Restart, _LW("controlCenter.restart")) ||
        !AppendMenuW(root.get(), MF_POPUP, reinterpret_cast<UINT_PTR>(power.get()),
            _LW("winx.shutdownOrSignOut"))) return nullptr;
    (void)power.release();
    if (!AppendMenuW(root.get(), MF_STRING, Desktop, _LW("winx.desktop")) ||
        !AppendMenuW(root.get(), MF_SEPARATOR, 0, nullptr) ||
        !AppendMenuW(root.get(), MF_POPUP, reinterpret_cast<UINT_PTR>(dockMenu),
            _LW("winx.dockSettings"))) return nullptr;
    return root.release();
}

bool Execute(UINT command, HWND owner)
{
    if (command < InstalledApps || command > Desktop) return false;
    DWORD error = ERROR_NOT_SUPPORTED;
    switch (command)
    {
    case InstalledApps: error = Launch(owner, L"ms-settings:appsfeatures"); break;
    case MobilityCenter: error = Launch(owner, SystemFile(L"mblctr.exe")); break;
    case PowerOptions: error = Launch(owner, L"ms-settings:powersleep"); break;
    case EventViewer: error = Launch(owner, SystemFile(L"eventvwr.msc")); break;
    case System: error = Launch(owner, L"ms-settings:about"); break;
    case DeviceManager: error = Launch(owner, SystemFile(L"devmgmt.msc")); break;
    case NetworkConnections: error = Launch(owner, L"ms-settings:network"); break;
    case DiskManagement: error = Launch(owner, SystemFile(L"diskmgmt.msc")); break;
    case ComputerManagement: error = Launch(owner, SystemFile(L"compmgmt.msc")); break;
    case Terminal: case TerminalAdmin: error = LaunchTerminal(owner, command == TerminalAdmin); break;
    case TaskManager: error = Launch(owner, SystemFile(L"taskmgr.exe")); break;
    case Settings: error = Launch(owner, L"ms-settings:"); break;
    case FileExplorer:
    {
        wchar_t folder[MAX_PATH]{};
        const UINT length = GetWindowsDirectoryW(folder, MAX_PATH);
        error = length && length < MAX_PATH
            ? Launch(owner, std::wstring(folder) + L"\\explorer.exe") : ERROR_PATH_NOT_FOUND;
        break;
    }
    // Registered by Windows Search; this opens its system panel, not a file search.
    case Search: error = Launch(owner, L"ms-search:"); break;
    case Run: case Desktop: error = ShellCommand(command); break;
    case SignOut: case Sleep: case Hibernate: case ShutDown: case Restart: error = PowerCommand(command); break;
    }
    if (error != ERROR_SUCCESS && error != ERROR_CANCELLED) MessageBeep(MB_ICONWARNING);
    return true;
}
}

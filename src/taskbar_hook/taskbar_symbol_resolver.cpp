#include "taskbar_symbol_resolver.h"
#include <dbghelp.h>
#include <shellapi.h>
#include <winhttp.h>
#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <string>
#include <vector>

namespace snowdesktop::taskbar_hook
{
namespace
{
struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Internet
{
    HINTERNET value = nullptr;
    ~Internet() { if (value) WinHttpCloseHandle(value); }
};
struct View
{
    void* value = nullptr;
    ~View() { if (value) UnmapViewOfFile(value); }
};
constexpr const char* kNames[] = {
    // Research reference: Windhawk taskbar-auto-hide-keyboard-only.wh.cpp
    // https://github.com/ramensoftware/windhawk-mods/blob/main/mods/taskbar-auto-hide-keyboard-only.wh.cpp
    // This independent resolver uses Microsoft PDB decorated names to check
    // the full ABI. It does not import Windhawk's keyboard-only reveal policy.
    "?Unhide@TrayUI@@UEAAXW4TrayUnhideFlags@TrayCommon@@W4UnhideRequest@3@@Z",
    "?_Unhide@CSecondaryTray@@AEAAXW4TrayUnhideFlags@TrayCommon@@W4UnhideRequest@3@@Z",
    "?SetFocusOnTaskbar@TaskbarHost@@QEAAXXZ",
    "?SetFocusWithCommand@TaskbarHost@@IEAA_NW4TaskbarCommand@Shell@UI@WindowsUdk@winrt@@_N@Z",
    "?HandleTaskbarHotkey@TrayUI@@UEAAX_K@Z",
    "?_OnFocusMsg@TrayUI@@QEAAXI_K_J@Z",
    "?WndProc@TrayUI@@UEAA_JPEAUHWND__@@I_K_JPEA_N@Z",
    "?v_WndProc@CSecondaryTray@@EEAA_JPEAUHWND__@@I_K_J@Z",
};
static_assert(std::size(kNames) == static_cast<std::size_t>(AutoHideSymbol::Count));

std::wstring SymbolKey(const AutoHideImageIdentity& image)
{
    const auto& g = image.pdb;
    wchar_t text[64]{};
    swprintf_s(text, L"%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%lX",
        g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7], image.age);
    return text;
}

DWORD DownloadPdb(const std::filesystem::path& destination, const std::wstring& key)
{
    Internet session{WinHttpOpen(L"SnowDesktop taskbar symbols", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.value) return GetLastError();
    if (!WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 5000)) return GetLastError();
    Internet connection{WinHttpConnect(session.value, L"msdl.microsoft.com", INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (!connection.value) return GetLastError();
    const std::wstring url = L"/download/symbols/Taskbar.pdb/" + key + L"/Taskbar.pdb";
    Internet request{WinHttpOpenRequest(connection.value, L"GET", url.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!request.value) return GetLastError();
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)))
        return GetLastError();
    if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.value, nullptr)) return GetLastError();
    DWORD status = 0, length = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX)) return GetLastError();
    if (status != 200) return status == 404 ? ERROR_FILE_NOT_FOUND : ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
    const auto partial = destination.wstring() + L".partial-" + std::to_wstring(GetCurrentProcessId());
    struct RemovePartial
    {
        std::filesystem::path path;
        ~RemovePartial() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } remove{partial};
    std::ofstream file(partial, std::ios::binary | std::ios::trunc);
    if (!file) return ERROR_ACCESS_DENIED;
    std::array<char, 65536> buffer{};
    DWORD total = 0;
    const auto started = GetTickCount64();
    for (;;)
    {
        DWORD read = 0;
        if (!WinHttpReadData(request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) return GetLastError();
        if (!read) break;
        total += read;
        if (total > 32 * 1024 * 1024) return ERROR_FILE_TOO_LARGE;
        if (GetTickCount64() - started > 45000) return ERROR_TIMEOUT;
        file.write(buffer.data(), read);
        if (!file) return ERROR_WRITE_FAULT;
    }
    file.close();
    if (!total || !file) return ERROR_INVALID_DATA;
    if (!MoveFileExW(partial.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return GetLastError();
    return ERROR_SUCCESS;
}

DWORD ReadSymbols(const std::filesystem::path& dll, const std::filesystem::path& directory,
    const AutoHideImageView& image, std::span<const BYTE> bytes, AutoHideResolution& resolution)
{
    auto& adapter = resolution.adapter;
    const HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_EXACT_SYMBOLS | SYMOPT_NO_PROMPTS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_IGNORE_NT_SYMPATH);
    if (!SymInitializeW(process, directory.c_str(), FALSE)) return GetLastError();
    struct Cleanup { HANDLE process; ~Cleanup() { SymCleanup(process); } } cleanup{process};
    constexpr DWORD64 kBase = 0x1000000;
    if (!SymLoadModuleExW(process, nullptr, dll.c_str(), L"SnowTaskbar", kBase, adapter.image.imageSize, nullptr, 0))
        return GetLastError();
    IMAGEHLP_MODULEW64 info{};
    info.SizeOfStruct = sizeof(info);
    if (!SymGetModuleInfoW64(process, kBase, &info) || info.SymType != SymPdb || info.PdbUnmatched ||
        info.PdbAge != adapter.image.age || std::memcmp(&info.PdbSig70, &adapter.image.pdb, sizeof(GUID)))
        return ERROR_REVISION_MISMATCH;
    // Resolve independently: a missing auto-hide method must not disable an
    // otherwise compatible tray snapshot, or vice versa.
    const auto resolve = [&](const char* decorated) -> std::optional<DWORD> {
        alignas(SYMBOL_INFO) std::array<BYTE, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> storage{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage.data());
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = MAX_SYM_NAME;
        const std::string name = std::string("SnowTaskbar!") + decorated;
        if (!SymFromName(process, name.c_str(), symbol) || std::strcmp(symbol->Name, decorated) ||
            symbol->ModBase != kBase || symbol->Address < kBase ||
            symbol->Address - kBase >= adapter.image.imageSize) return {};
        return static_cast<DWORD>(symbol->Address - kBase);
    };
    static constexpr const char* vectorPrefix =
        "@?$produce@U?$convertible_observable_vector@UNotificationAreaIcon@Shell@UI@WindowsUdk@winrt@@V?$vector@UNotificationAreaIcon@Shell@UI@WindowsUdk@winrt@@V?$allocator@UNotificationAreaIcon@Shell@UI@WindowsUdk@winrt@@@std@@@std@@Usingle_threaded_collection_base@impl@5@@impl@winrt@@U?$IVector@UNotificationAreaIcon@Shell@UI@WindowsUdk@winrt@@@Collections@Foundation@Windows@3@@impl@winrt@@";
    const std::array<std::string, static_cast<std::size_t>(ModernTraySymbol::Count)> modernNames{
        "?HandleCopyData@TrayUI@@UEAA_N_KPEAUtagCOPYDATASTRUCT@@PEA_J@Z",
        "?ShellNotifyIcon@NotificationAreaIconManager2@@QEAA_NQEAU_TRAYNOTIFYDATAW@@@Z",
        "?ShellNotifyIconGetRect@NotificationAreaIconManager2@@QEAA_JQEAU_TRAYNOTIFYINFO@@@Z",
        "?Identity@NotificationAreaIcon2@implementation@Shell@UI@WindowsUdk@winrt@@QEBA?AUNotificationAreaIconIdentity@@XZ",
        "?SendMessageToOwnerWindow@NotificationAreaIcon2@implementation@Shell@UI@WindowsUdk@winrt@@AEAAXIAEBUPoint@Foundation@Windows@6@@Z",
        "?SetIcon@NotificationAreaIcon2@implementation@Shell@UI@WindowsUdk@winrt@@QEAAXPEAUHICON__@@@Z",
        "?SetTooltip@NotificationAreaIcon2@implementation@Shell@UI@WindowsUdk@winrt@@QEAAXPEBG@Z",
        "?c_str@hstring@winrt@@QEBAPEBGXZ",
        "?VisiblePromotedIcons@NotificationAreaIconManager2@@QEAA?AU?$IObservableVector@UNotificationAreaIcon@Shell@UI@WindowsUdk@winrt@@@Collections@Foundation@Windows@winrt@@XZ",
        "?VisibleOverflowIcons@NotificationAreaIconManager2@@QEAA?AU?$IObservableVector@UNotificationAreaIcon@Shell@UI@WindowsUdk@winrt@@@Collections@Foundation@Windows@winrt@@XZ",
        std::string("?get_Size") + vectorPrefix + "UEAAHPEAI@Z",
        std::string("?GetAt") + vectorPrefix + "UEAAHIPEAPEAX@Z"
    };
    auto& modern = resolution.modernTray; modern = {}; modern.image = adapter.image;
    const auto root = resolve("?g_trayUI@@3V?$ComPtr@UITrayUI@@@WRL@Microsoft@@A");
    const auto itemVtable = resolve("??_7NotificationAreaIcon2@implementation@Shell@UI@WindowsUdk@winrt@@6B@");
    bool compatible = root && itemVtable;
    if (compatible) { modern.root = *root; modern.itemVtable = *itemVtable; }
    for (std::size_t i = 0; compatible && i < modernNames.size(); ++i)
    {
        const auto address = resolve(modernNames[i].c_str());
        if (!address) { compatible = false; break; }
        auto function = image.Function(*address);
        if (i == static_cast<std::size_t>(ModernTraySymbol::StringBuffer) && !function)
        {
            AutoHideFunction leaf{*address, *address + 21};
            if (image.Executable(leaf.begin, leaf.end) && image.Read(leaf.begin, leaf.entry)) function = leaf;
        }
        if (!function) { compatible = false; break; }
        modern.functions[i] = *function;
    }
    const auto layout = compatible ? DecodeModernTrayLayout(bytes, modern) : std::nullopt;
    resolution.modernTrayError = layout ? ERROR_SUCCESS : ERROR_NOT_SUPPORTED;
    if (layout) modern.layout = *layout; else modern = {};
    for (std::size_t i = 0; i < std::size(kNames); ++i)
    {
        alignas(SYMBOL_INFO) std::array<BYTE, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> storage{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage.data());
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        const std::string name = std::string("SnowTaskbar!") + kNames[i];
        if (!SymFromName(process, name.c_str(), symbol) || std::strcmp(symbol->Name, kNames[i]) ||
            symbol->ModBase != kBase || symbol->Address < kBase ||
            symbol->Address - kBase >= adapter.image.imageSize) return ERROR_PROC_NOT_FOUND;
        const auto function = image.Function(static_cast<DWORD>(symbol->Address - kBase));
        if (!function) return ERROR_INVALID_ADDRESS;
        adapter.functions[i] = *function;
    }
    return image.Validate(adapter) ? ERROR_SUCCESS : ERROR_INVALID_DATA;
}
}

AutoHideResolution ResolveTaskbarSymbols(const std::filesystem::path& cacheRoot)
{
    AutoHideResolution result;
    try
    {
        wchar_t system[MAX_PATH]{};
        const UINT length = GetSystemDirectoryW(system, MAX_PATH);
        if (!length || length >= MAX_PATH) { result.error = ERROR_BAD_PATHNAME; return result; }
        const auto dll = std::filesystem::path(system) / L"Taskbar.dll";
        Handle file{CreateFileW(dll.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE) { result.error = GetLastError(); return result; }
        Handle mapping{CreateFileMappingW(file.value, nullptr, PAGE_READONLY | SEC_IMAGE_NO_EXECUTE, 0, 0, nullptr)};
        if (!mapping.value) { result.error = GetLastError(); return result; }
        View mapped{MapViewOfFile(mapping.value, FILE_MAP_READ, 0, 0, 0)};
        if (!mapped.value) { result.error = GetLastError(); return result; }
        const auto* bytes = static_cast<const BYTE*>(mapped.value);
        AutoHideImageView header({bytes, 4096});
        IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
        if (!header.Read(0, dos) || dos.e_lfanew <= 0 || dos.e_lfanew > 4096 ||
            !header.Read(static_cast<std::size_t>(dos.e_lfanew), nt) || nt.OptionalHeader.SizeOfImage > 64 * 1024 * 1024)
        { result.error = ERROR_BAD_EXE_FORMAT; return result; }
        AutoHideImageView image({bytes, nt.OptionalHeader.SizeOfImage});
        const auto identity = image.Identity();
        if (!identity) { result.error = ERROR_BAD_EXE_FORMAT; return result; }
        result.adapter.image = *identity;
        const auto key = SymbolKey(*identity);
        const auto directory = cacheRoot / key;
        const auto pdb = directory / L"Taskbar.pdb";
        std::filesystem::create_directories(directory);
        const bool cached = std::filesystem::is_regular_file(pdb);
        if (cached)
        {
            result.error = ReadSymbols(dll, directory, image, {bytes, nt.OptionalHeader.SizeOfImage}, result);
            if (result.error != ERROR_REVISION_MISMATCH) return result; // ABI changes are not a network failure.
        }
        result.error = DownloadPdb(pdb, key);
        if (result.error == ERROR_SUCCESS) result.error = ReadSymbols(dll, directory, image, {bytes, nt.OptionalHeader.SizeOfImage}, result);
    }
    catch (...) { result.error = ERROR_INVALID_DATA; }
    return result;
}

std::optional<int> TryRunTaskbarSymbolHelper()
{
    int count = 0;
    auto** args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return {};
    struct FreeArgs { wchar_t** args; ~FreeArgs() { LocalFree(args); } } free{args};
    if (count < 2 || std::wcscmp(args[1], kSymbolHelperCommand)) return {};
    if (count != 4 || !args[2][0] || !args[3][0]) return ERROR_INVALID_PARAMETER;
    wchar_t* end = nullptr;
    const auto rawHandle = std::wcstoull(args[2], &end, 10);
    if (!rawHandle || !end || *end) return ERROR_INVALID_HANDLE;
    const HANDLE handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(rawHandle));
    View view{MapViewOfFile(handle, FILE_MAP_WRITE, 0, 0, sizeof(AutoHideResolution))};
    if (!view.value) return static_cast<int>(GetLastError());
    auto* output = static_cast<AutoHideResolution*>(view.value);
    if (output->magic != AutoHideResolution{}.magic || output->version != AutoHideResolution{}.version) return ERROR_INVALID_DATA;
    *output = ResolveTaskbarSymbols(args[3]);
    return static_cast<int>(output->error);
}

AutoHideResolution RunTaskbarSymbolHelper(const std::filesystem::path& executable,
    const std::filesystem::path& cacheRoot, HANDLE cancel, DWORD timeoutMs)
{
    AutoHideResolution result;
    if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0)
    { result.error = ERROR_CANCELLED; return result; }
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle mapping{CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE,
        0, sizeof(AutoHideResolution), nullptr)};
    if (!mapping.value) { result.error = GetLastError(); return result; }
    View view{MapViewOfFile(mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(result))};
    if (!view.value) { result.error = GetLastError(); return result; }
    *static_cast<AutoHideResolution*>(view.value) = result;
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    { result.error = GetLastError(); return result; }
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<BYTE> attributes(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) { result.error = GetLastError(); return result; }
    struct DeleteList { LPPROC_THREAD_ATTRIBUTE_LIST list; ~DeleteList() { DeleteProcThreadAttributeList(list); } } deleteList{list};
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        &mapping.value, sizeof(mapping.value), nullptr, nullptr)) { result.error = GetLastError(); return result; }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.lpAttributeList = list;
    // Windows filenames cannot contain a quote. Trailing separators are removed
    // before quoting so CommandLineToArgvW cannot consume the closing quote.
    const auto directory = cacheRoot.lexically_normal().wstring();
    if (directory.empty() || directory.find(L'"') != std::wstring::npos ||
        executable.wstring().find(L'"') != std::wstring::npos)
    { result.error = ERROR_INVALID_PARAMETER; return result; }
    std::wstring command = L"\"" + executable.wstring() + L"\" " + kSymbolHelperCommand + L" " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(mapping.value)) + L" \"" +
        (directory.ends_with(L'\\') ? directory.substr(0, directory.size() - 1) : directory) + L"\"";
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
        &startup.StartupInfo, &child)) { result.error = GetLastError(); return result; }
    Handle process{child.hProcess}, thread{child.hThread};
    if (!AssignProcessToJobObject(job.value, process.value))
    {
        result.error = GetLastError();
        TerminateProcess(process.value, result.error);
        WaitForSingleObject(process.value, 1000);
        return result;
    }
    if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) { result.error = GetLastError(); return result; }
    HANDLE handles[] = {process.value, cancel};
    const DWORD wait = WaitForMultipleObjects(cancel ? 2 : 1, handles, FALSE, timeoutMs);
    if (wait != WAIT_OBJECT_0)
    {
        result.error = wait == WAIT_OBJECT_0 + 1 ? ERROR_CANCELLED : wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
        TerminateJobObject(job.value, result.error);
        WaitForSingleObject(process.value, 1000);
        return result;
    }
    DWORD exitCode = ERROR_GEN_FAILURE;
    GetExitCodeProcess(process.value, &exitCode);
    result = *static_cast<const AutoHideResolution*>(view.value);
    if (result.magic != AutoHideResolution{}.magic || result.version != AutoHideResolution{}.version || exitCode != result.error)
        result = AutoHideResolution{.error = ERROR_INVALID_DATA};
    return result;
}
}

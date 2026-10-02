// Real production Service + Explorer collector. Only deployment-path lookup and
// diagnostic output are replaced; the fixture is a separate hidden process.
// This opt-in test does not operate SnowDesktop or third-party application UI.
#include "tray_service.h"
#include "diagnostic_log.h"
#include "tray_modern_bootstrap.h"
#include "taskbar_hook/taskbar_symbol_resolver.h"
#include <windowsx.h>
#include <filesystem>
#include <iostream>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <tlhelp32.h>
using namespace snowdesktop::tray;
static std::wstring hookPath;
namespace snowdesktop::deployment { std::wstring GetTaskbarHookPath() { return hookPath; } }
std::wstring GetDataDirectoryPath() { return std::filesystem::path(hookPath).parent_path().wstring(); }
void WriteDiagnosticLogEntry(const wchar_t* text, DiagnosticLogLevel) { std::wcout << text << std::endl; }
constexpr UINT kCallback = WM_APP + 100, kCommand = WM_APP + 101;
constexpr UINT kColdIconId = 78;
constexpr UINT kClassicIconId = 80, kClassicCallback = kCallback + 10;
constexpr DWORD kClassicVersions[]{0, NOTIFYICON_VERSION, NOTIFYICON_VERSION_4};
constexpr GUID kIconGuid{0xe1e77079,0x1b5b,0x40f7,{0xaa,0x72,0x82,0xb4,0x19,0x6c,0x62,0x05}};
struct State
{
    DWORD owner = 0;
    volatile LONG ready = 0, errors = 0, count = 0;
    volatile LONG coldAddFailures = 0, coldVersionRequests = 0;
    bool classic = false;
    volatile LONG classicCount[3]{};
    Callback classicCallbacks[3][8]{};
    HWND window = nullptr;
    Callback callbacks[64]{};
};
static State* state = nullptr;
static UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
static NOTIFYICONDATAW iconData{};
static NOTIFYICONDATAW coldIconData{};
static NOTIFYICONDATAW classicIconData[3]{};
static bool classicPresent[3]{};
static bool present = false;
static bool coldPresent = false;
void Check(bool value, const char* what)
{ if (!value) throw std::runtime_error(what); std::cout << "PASS " << what << std::endl; }
void Notify(DWORD operation)
{ if (!Shell_NotifyIconW(operation, &iconData)) InterlockedIncrement(&state->errors); }
void Add()
{
    iconData.uFlags = NIF_GUID | NIF_ICON | NIF_MESSAGE | NIF_TIP;
    iconData.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(iconData.szTip, L"SnowDesktop isolated tray fixture");
    // A re-registration broadcast does not remove Explorer's existing icon.
    // Fall back to modify when ADD reports an already registered fixture.
    if (!Shell_NotifyIconW(NIM_ADD, &iconData)) Notify(NIM_MODIFY);
    iconData.uVersion = NOTIFYICON_VERSION_4; Notify(NIM_SETVERSION); present = true;
}
void AddCold()
{
    // Qt-like caller policy, with no Qt dependency: SETVERSION follows only a
    // successful ADD. The live collector must recover this pre-existing icon
    // without reading a version from ADD's otherwise irrelevant union field.
    coldIconData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    coldIconData.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_ADD, &coldIconData))
    { InterlockedIncrement(&state->coldAddFailures); return; }
    coldPresent = true;
    InterlockedIncrement(&state->coldVersionRequests);
    if (!Shell_NotifyIconW(NIM_SETVERSION, &coldIconData)) InterlockedIncrement(&state->errors);
}
LRESULT CALLBACK ClientProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == taskbarCreated)
    { if (present) Add(); if (coldPresent) AddCold(); return 0; }
    if (message == kCallback)
    {
        const auto count = Read(state->count);
        if (count < 64) { state->callbacks[count] = {wp, lp}; MemoryBarrier(); InterlockedIncrement(&state->count); }
        return 0;
    }
    if (message >= kClassicCallback && message < kClassicCallback + 3)
    {
        const auto index = message - kClassicCallback;
        const auto count = Read(state->classicCount[index]);
        if (count < 8)
        {
            state->classicCallbacks[index][count] = {wp, lp};
            MemoryBarrier(); InterlockedIncrement(&state->classicCount[index]);
        }
        return 0;
    }
    if (message == kCommand)
    {
        if (wp == 1)
        {
            iconData.uFlags = NIF_GUID | NIF_TIP | NIF_ICON;
            wcscpy_s(iconData.szTip, L"SnowDesktop updated tray fixture");
            iconData.hIcon = LoadIcon(nullptr, IDI_WARNING); Notify(NIM_MODIFY);
        }
        else if (wp == 2)
        { iconData.uFlags = NIF_GUID | NIF_STATE; iconData.dwStateMask = NIS_HIDDEN; iconData.dwState = NIS_HIDDEN; Notify(NIM_MODIFY); }
        else if (wp == 3) { Notify(NIM_DELETE); present = false; }
        else if (wp == 4) { Add(); iconData.uVersion = 0; Notify(NIM_SETVERSION); }
        else if (wp == 5) PostQuitMessage(0);
        else if (wp == 6)
        { iconData.uFlags = NIF_GUID | NIF_STATE; iconData.dwStateMask = NIS_HIDDEN; iconData.dwState = 0; Notify(NIM_MODIFY); }
        else if (wp == 7) AddCold();
        return 1;
    }
    if (message == WM_TIMER)
    {
        HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, state->owner);
        if (!parent || WaitForSingleObject(parent, 0) == WAIT_OBJECT_0) PostQuitMessage(0);
        if (parent) CloseHandle(parent);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
int Client(const wchar_t* mapping)
{
    HANDLE shared = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mapping); if (!shared) return 2;
    state = static_cast<State*>(MapViewOfFile(shared, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(State))); if (!state) return 2;
    WNDCLASSW wc{}; wc.lpfnWndProc = ClientProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"SnowDesktop.Tray.Fixture";
    RegisterClassW(&wc);
    state->window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"Tray fixture", WS_POPUP, 0,0,1,1,nullptr,nullptr,wc.hInstance,nullptr);
    iconData.cbSize = sizeof(iconData); iconData.hWnd = state->window; iconData.uID = 77; iconData.guidItem = kIconGuid; iconData.uCallbackMessage = kCallback;
    coldIconData.cbSize = sizeof(coldIconData); coldIconData.hWnd = state->window;
    coldIconData.uID = kColdIconId; coldIconData.uCallbackMessage = kCallback + 2;
    coldIconData.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(coldIconData.szTip, L"SnowDesktop isolated cold registration fixture");
    Add(); AddCold();
    if (state->classic)
        for (unsigned i = 0; i < std::size(classicIconData); ++i)
        {
            // Like Win10 system icons, these exist before collector attachment
            // and deliberately ignore TaskbarCreated. Only native bootstrap
            // can recover their protocol; ADD's timeout field is left zero.
            auto& data = classicIconData[i];
            data.cbSize = sizeof(data); data.hWnd = state->window; data.uID = kClassicIconId + i;
            data.uFlags = NIF_GUID | NIF_ICON | NIF_MESSAGE | NIF_TIP;
            data.guidItem = kIconGuid; data.guidItem.Data1 += i + 1;
            data.uCallbackMessage = kClassicCallback + i; data.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
            wcscpy_s(data.szTip, L"SnowDesktop isolated classic bootstrap fixture");
            classicPresent[i] = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
            if (!classicPresent[i]) InterlockedIncrement(&state->errors);
            data.uVersion = kClassicVersions[i];
            if (!Shell_NotifyIconW(NIM_SETVERSION, &data)) InterlockedIncrement(&state->errors);
        }
    SetTimer(state->window, 1, 1000, nullptr); InterlockedExchange(&state->ready, 1);
    MSG message{}; while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    if (present) Notify(NIM_DELETE);
    if (coldPresent && !Shell_NotifyIconW(NIM_DELETE, &coldIconData)) InterlockedIncrement(&state->errors);
    for (unsigned i = 0; i < std::size(classicIconData); ++i)
        if (classicPresent[i] && !Shell_NotifyIconW(NIM_DELETE, &classicIconData[i])) InterlockedIncrement(&state->errors);
    DestroyWindow(state->window); UnmapViewOfFile(state); CloseHandle(shared); return 0;
}
template<class Predicate> bool Await(Predicate predicate, unsigned milliseconds = 6000)
{
    const auto end = GetTickCount64() + milliseconds;
    do { if (predicate()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); } while (GetTickCount64() < end);
    return predicate();
}
struct Fixture
{
    HANDLE mapping = nullptr; State* value = nullptr; PROCESS_INFORMATION process{};
    ~Fixture()
    {
        if (value && value->window) PostMessageW(value->window, kCommand, 5, 0);
        if (process.hProcess) { WaitForSingleObject(process.hProcess, 3000); CloseHandle(process.hProcess); }
        if (process.hThread) CloseHandle(process.hThread);
        if (value) UnmapViewOfFile(value);
        if (mapping) CloseHandle(mapping);
    }
    void Start(bool classic = false)
    {
        const auto name = L"Local\\SnowDesktop.TrayFixture." + std::to_wstring(GetCurrentProcessId());
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(State),name.c_str());
        Check(mapping != nullptr, "fixture mapping");
        value = static_cast<State*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(State)));
        Check(value != nullptr, "fixture memory"); new(value) State; value->owner = GetCurrentProcessId(); value->classic = classic;
        wchar_t exe[32768]{}; GetModuleFileNameW(nullptr,exe,32768);
        auto command = L"\"" + std::wstring(exe) + L"\" --tray-fixture-client \"" + name + L"\"";
        STARTUPINFOW startup{}; startup.cb=sizeof(startup); startup.dwFlags=STARTF_USESHOWWINDOW; startup.wShowWindow=SW_HIDE;
        Check(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE,"fixture child starts hidden");
        Check(Await([&]{return Read(value->ready)!=0;}),"client registered before collector starts");
    }
    void Command(WPARAM command)
    { DWORD_PTR ignored=0; Check(SendMessageTimeoutW(value->window,kCommand,command,0,SMTO_ABORTIFHUNG,1000,&ignored)!=0,"fixture command delivered"); }
};
int TryRunTrayLiveTests()
{
    int argc = 0;
    auto** raw = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!raw) return 2;
    std::vector<std::wstring> arguments(raw, raw + argc); LocalFree(raw);
    std::vector<const wchar_t*> argv;
    for (const auto& argument : arguments) argv.push_back(argument.c_str());
    if (argc == 3 && wcscmp(argv[1], L"--tray-modern-snapshot") == 0)
    {
        // Read-only diagnostic: no collector injection, broadcast or callbacks.
        const auto resolved = snowdesktop::taskbar_hook::ResolveTaskbarSymbols(argv[2]);
        if (resolved.modernTrayError != ERROR_SUCCESS)
        { std::cerr << "modern snapshot unavailable: " << resolved.modernTrayError << '\n'; return 77; }
        DWORD explorer = 0; GetWindowThreadProcessId(FindWindowW(L"Shell_TrayWnd", nullptr), &explorer);
        for (unsigned attempt = 0; attempt < 3; ++attempt)
        {
            const auto events = BootstrapModernTray(explorer, 1, resolved.modernTray);
            if (events.empty()) continue;
            std::cout << "modern snapshot icons=" << events.size() << " explorer=" << explorer << '\n';
            for (const auto& event : events)
                std::cout << "pid=" << event.identity.process << " id=" << event.identity.id << " callback=" << event.callback
                    << " version=" << event.version << " hidden=" << ((event.state & NIS_HIDDEN) != 0)
                    << " image=" << event.width << 'x' << event.height << '\n';
            return 0;
        }
        std::cerr << "FAIL: no stable native tray snapshot\n"; return 1;
    }
    if (argc==3 && wcscmp(argv[1],L"--tray-fixture-client")==0) return Client(argv[2]);
    const bool classic = argc == 3 && wcscmp(argv[1], L"--tray-classic") == 0;
    if (!classic && (argc != 3 || wcscmp(argv[1], L"--tray-live") != 0)) return -1;
    if (!GetShellWindow()) { std::cerr << "SKIP: an interactive Explorer session is required\n"; return 77; }
    HANDLE processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry); bool occupied = false;
    if (processes != INVALID_HANDLE_VALUE)
    {
        if (Process32FirstW(processes, &entry)) do {
            if (_wcsicmp(entry.szExeFile, L"SnowDesktop.exe") == 0) occupied = true;
        } while (Process32NextW(processes, &entry));
        CloseHandle(processes);
    }
    else { std::cerr << "SKIP: cannot check desktop host ownership\n"; return 77; }
    if (occupied) { std::cerr << "SKIP: close SnowDesktop before this isolated tray diagnostic\n"; return 77; }
    const auto directory = std::filesystem::temp_directory_path() /
        (L"SnowDesktop.TrayFixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::error_code error; std::filesystem::create_directory(directory, error);
    if (error) { std::cerr << "FAIL: cannot prepare isolated Hook directory\n"; return 2; }
    const auto hook = directory / L"SnowDesktopTaskbarHook.dll";
    std::filesystem::copy_file(std::filesystem::absolute(argv[2]), hook, error);
    if (error) { std::filesystem::remove(directory); std::cerr << "FAIL: cannot copy test Hook\n"; return 2; }
    // Explorer pins hook code for callback safety. Keep this exact temporary
    // file until Explorer exits rather than locking any build output.
    hookPath = hook.wstring();
    // Reuse only cached PDBs, copied to this fixture's isolated directory.
    // Avoid a fresh network dependency when the matching symbols are present.
    wchar_t executable[32768]{};
    if (GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable))))
    {
        const auto cached = std::filesystem::path(executable).parent_path() / L"data" / L"ShellHookSymbols";
        if (std::filesystem::is_directory(cached, error))
            std::filesystem::copy(cached, directory / L"ShellHookSymbols",
                std::filesystem::copy_options::recursive | std::filesystem::copy_options::skip_existing, error);
    }
    std::wcout << L"Isolated Hook (released when Explorer exits): " << hookPath << std::endl;
    try
    {
        Fixture fixture; fixture.Start(true);
        if (classic)
        {
            Check(Read(fixture.value->errors) == 0, "classic fixtures register before the collector exists");
            Service service;
            for (unsigned i = 0; i < std::size(kClassicVersions); ++i)
            {
                std::optional<Icon> collected;
                Check(Await([&] {
                    for (const auto& icon : service.Current().icons)
                        if (icon.identity.window == reinterpret_cast<std::uint64_t>(fixture.value->window) &&
                            icon.identity.process == fixture.process.dwProcessId && icon.identity.id == kClassicIconId + i)
                            collected = icon;
                    return collected && collected->version == kClassicVersions[i] && !collected->pixels.empty();
                }), "classic bootstrap preserves the declared protocol without any re-registration");
                if (HasGuid(collected->identity.guid))
                {
                    auto expectedGuid = kIconGuid; expectedGuid.Data1 += i + 1;
                    Check(collected->identity.guid == expectedGuid, "modern bootstrap retains the actual GUID");
                }
                service.SetGeometry(collected->key, {-160, 40, -128, 72});
                for (auto action : {Activation::RightDown, Activation::RightUp, Activation::ContextKeyboard})
                    Check(service.Activate(collected->key, action, {-144, 56}), "cold icon context action accepted");
                Check(Await([&] { return Read(fixture.value->classicCount[i]) >= 4; }), "cold icon receives all context callbacks");
                Check(Read(fixture.value->classicCount[i]) == 4, "cold icon receives no duplicate context callbacks");
                const UINT legacy[]{WM_RBUTTONDOWN, WM_RBUTTONUP, WM_RBUTTONDOWN, WM_RBUTTONUP};
                const UINT modern[]{WM_RBUTTONDOWN, WM_RBUTTONUP, WM_CONTEXTMENU, WM_CONTEXTMENU};
                for (unsigned j = 0; j < 4; ++j)
                {
                    const auto callback = fixture.value->classicCallbacks[i][j];
                    if (kClassicVersions[i] == NOTIFYICON_VERSION_4)
                        Check(LOWORD(callback.lp) == modern[j] && HIWORD(callback.lp) == kClassicIconId + i &&
                            GET_X_LPARAM(callback.wp) == -144 && GET_Y_LPARAM(callback.wp) == 56,
                            "cold v4 callback retains event, identity and signed screen anchor");
                    else
                        Check(callback.wp == kClassicIconId + i && callback.lp ==
                            (kClassicVersions[i] ? modern[j] : legacy[j]),
                            "cold legacy or v3 callback keeps the original protocol and full icon ID");
                }
            }
            Check(Read(fixture.value->errors) == 0, "Shell accepted classic fixture notifications");
            return 0;
        }
        Check(Read(fixture.value->coldVersionRequests) == 1 && Read(fixture.value->coldAddFailures) == 0,
            "cold legacy-key fixture registered version 4 before the collector existed");
        fixture.Command(7);
        Check(Read(fixture.value->coldVersionRequests) == 1 && Read(fixture.value->coldAddFailures) == 1,
            "without recovery a real duplicate ADD fails and the caller skips SETVERSION");
        NOTIFYICONIDENTIFIER coldIdentifier{}; coldIdentifier.cbSize = sizeof(coldIdentifier);
        coldIdentifier.hWnd = fixture.value->window; coldIdentifier.uID = kColdIconId;
        RECT coldRect{};
        const bool nativeGeometryAvailable = Await([&] {
            return Shell_NotifyIconGetRect(&coldIdentifier, &coldRect) == S_OK && !IsRectEmpty(&coldRect);
        }, 2000);
        if (!nativeGeometryAvailable)
        {
            std::cerr << "SKIP: Explorer does not expose S_OK nonempty geometry for the isolated cold icon; recovery must fail closed\n";
            return 77;
        }
        const auto find=[&](const Snapshot& snapshot)->std::optional<Icon> {
            for(const auto& icon:snapshot.icons) if(icon.identity.guid==kIconGuid) return icon;
            return {};
        };
        const auto findCold = [&](const Snapshot& snapshot) -> std::optional<Icon> {
            for (const auto& icon : snapshot.icons)
                if (!HasGuid(icon.identity.guid) && icon.identity.window == reinterpret_cast<std::uint64_t>(fixture.value->window) &&
                    icon.identity.process == fixture.process.dwProcessId && icon.identity.id == kColdIconId) return icon;
            return {};
        };
        {
            Service service;
            for (unsigned i = 0; i < std::size(kClassicVersions); ++i)
            {
                Check(Await([&] {
                    for (const auto& icon : service.Current().icons)
                        if (icon.identity.window == reinterpret_cast<std::uint64_t>(fixture.value->window) &&
                            icon.identity.process == fixture.process.dwProcessId && icon.identity.id == kClassicIconId + i)
                            return icon.version == kClassicVersions[i] && icon.callback == kClassicCallback + i &&
                                !icon.pixels.empty() && !(icon.state & NIS_HIDDEN);
                    return false;
                }, 65000), "startup supplementation finds icons ignoring TaskbarCreated with their original callback protocols");
            }
            Check(Await([&] {
                const auto icon = findCold(service.Current());
                return icon && icon->version == 4 && !icon->pixels.empty() && Read(fixture.value->coldVersionRequests) == 2;
            }), "bounded duplicate ADD acknowledgement lets the original caller restore version 4 after late collector attachment");
            Check(Read(fixture.value->coldAddFailures) == 1,
                "re-registration succeeds without a second failed ADD or an unconditional caller-side version fallback");
            fixture.Command(7);
            Check(Read(fixture.value->coldVersionRequests) == 2 && Read(fixture.value->coldAddFailures) == 2,
                "the same recovery session cannot acknowledge the cold icon twice");
            Check(Await([&]{auto icon=find(service.Current());return icon && icon->version==4 && !icon->pixels.empty();}),"initial TaskbarCreated re-registration yields pixels and version 4");
            const auto original=find(service.Current()).value();
            fixture.Command(1);
            Check(Await([&]{auto icon=find(service.Current());return icon && icon->tip==L"SnowDesktop updated tray fixture" && icon->pixels!=original.pixels;}),"modify updates tooltip and dynamic pixels");
            fixture.Command(2);
            Check(Await([&]{auto icon=find(service.Current());return icon && (icon->state & NIS_HIDDEN);}),"hidden state arrives without dropping icon");
            Check(!service.Activate(original.key,Activation::Keyboard,{136,56}),"hidden icon rejects stale activation");
            fixture.Command(6);
            Check(Await([&]{auto icon=find(service.Current());return icon && !(icon->state & NIS_HIDDEN);}),"visible state restores the existing icon");
            const RECT rect{120,40,152,72}; service.SetGeometry(original.key,rect);
            NOTIFYICONIDENTIFIER identifier{};identifier.cbSize=sizeof(identifier);identifier.hWnd=fixture.value->window;identifier.uID=77;identifier.guidItem=kIconGuid;
            RECT actual{}; HRESULT result=Shell_NotifyIconGetRect(&identifier,&actual);
            std::cout<<"geometry hr="<<std::hex<<result<<std::dec<<" actual="<<actual.left<<","<<actual.top<<","<<actual.right<<","<<actual.bottom<<std::endl;
            Check(SUCCEEDED(result) && EqualRect(&rect,&actual),"real Shell geometry lookup returns mirrored icon rectangle");
            const POINT anchor{136,56};
            for(auto action:{Activation::LeftDown,Activation::LeftUp,Activation::DoubleClick,Activation::RightUp,Activation::Keyboard})
                Check(service.Activate(original.key,action,anchor),"activation delivered to fixture only");
            Check(Await([&]{return Read(fixture.value->count)>=7;}),"version 4 click callbacks arrive");
            const UINT expected[]{WM_LBUTTONDOWN,WM_LBUTTONUP,NIN_SELECT,WM_LBUTTONDBLCLK,WM_RBUTTONUP,WM_CONTEXTMENU,NIN_KEYSELECT};
            for(unsigned i=0;i<std::size(expected);++i)
            {
                const auto callback=fixture.value->callbacks[i];
                Check(LOWORD(callback.lp)==expected[i] && HIWORD(callback.lp)==77 && GET_X_LPARAM(callback.wp)==136 && GET_Y_LPARAM(callback.wp)==56,"callback packs version 4 event, identity and actual screen coordinates");
            }
            fixture.Command(3); Check(Await([&]{return !find(service.Current());}),"delete removes the icon");
            Check(!service.Activate(original.key,Activation::Keyboard,anchor),"deleted icon cannot invoke an old window");
            fixture.Command(4); Check(Await([&]{auto icon=find(service.Current());return icon && icon->version==0;}),"legacy icon can register after deletion");
            InterlockedExchange(&fixture.value->count,0);
            Check(service.Activate(original.key,Activation::RightUp,anchor),"legacy right click delivered");
            Check(Await([&]{return Read(fixture.value->count)==1;}),"legacy callback received");
            Check(fixture.value->callbacks[0].wp==77 && fixture.value->callbacks[0].lp==WM_RBUTTONUP,"legacy callback retains original parameters");
            Check(Read(fixture.value->errors)==0,"Shell accepted all fixture notifications");
        }
        Check(Await([&]{HANDLE h=OpenFileMappingW(FILE_MAP_READ,FALSE,ObjectName(GetCurrentProcessId(),L"State").c_str());if(h)CloseHandle(h);return !h;}),"collector releases the connection after service destruction");
        {
            Service service;
            Check(Await([&]{return find(service.Current()).has_value();}),"collector reconnects in the same host process");
            unsigned count=0;for(const auto& icon:service.Current().icons)count+=icon.identity.guid==kIconGuid;
            Check(count==1,"reconnect does not duplicate the icon");
        }
        return 0;
    }
    catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<std::endl;return 1;}
}

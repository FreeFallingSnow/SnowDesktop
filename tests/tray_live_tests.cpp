// Real production Service + Explorer collector. Only deployment-path lookup and
// diagnostic output are replaced; the fixture is a separate hidden process.
// This opt-in test does not operate SnowDesktop or third-party application UI.
#include "tray_service.h"
#include "diagnostic_log.h"
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
void WriteDiagnosticLogEntry(const wchar_t* text, DiagnosticLogLevel) { std::wcout << text << std::endl; }
constexpr UINT kCallback = WM_APP + 100, kCommand = WM_APP + 101;
constexpr GUID kIconGuid{0xe1e77079,0x1b5b,0x40f7,{0xaa,0x72,0x82,0xb4,0x19,0x6c,0x62,0x05}};
struct State
{
    DWORD owner = 0;
    volatile LONG ready = 0, errors = 0, count = 0;
    HWND window = nullptr;
    Callback callbacks[64]{};
};
static State* state = nullptr;
static UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
static NOTIFYICONDATAW iconData{};
static bool present = false;
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
LRESULT CALLBACK ClientProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == taskbarCreated && present) { Add(); return 0; }
    if (message == kCallback)
    {
        const auto count = Read(state->count);
        if (count < 64) { state->callbacks[count] = {wp, lp}; MemoryBarrier(); InterlockedIncrement(&state->count); }
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
    Add(); SetTimer(state->window, 1, 1000, nullptr); InterlockedExchange(&state->ready, 1);
    MSG message{}; while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    if (present) Notify(NIM_DELETE);
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
    void Start()
    {
        const auto name = L"Local\\SnowDesktop.TrayFixture." + std::to_wstring(GetCurrentProcessId());
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(State),name.c_str());
        Check(mapping != nullptr, "fixture mapping");
        value = static_cast<State*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(State)));
        Check(value != nullptr, "fixture memory"); new(value) State; value->owner = GetCurrentProcessId();
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
    if (argc==3 && wcscmp(argv[1],L"--tray-fixture-client")==0) return Client(argv[2]);
    if (argc != 3 || wcscmp(argv[1], L"--tray-live") != 0) return -1;
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
    std::wcout << L"Isolated Hook (released when Explorer exits): " << hookPath << std::endl;
    try
    {
        Fixture fixture; fixture.Start();
        const auto find=[&](const Snapshot& snapshot)->std::optional<Icon> {
            for(const auto& icon:snapshot.icons) if(icon.identity.guid==kIconGuid) return icon;
            return {};
        };
        {
            Service service;
            Check(Await([&]{auto icon=find(service.Current());return icon && icon->version==4 && !icon->pixels.empty();}),"initial TaskbarCreated re-registration yields pixels and version 4");
            const auto original=find(service.Current()).value();
            fixture.Command(1);
            Check(Await([&]{auto icon=find(service.Current());return icon && icon->tip==L"SnowDesktop updated tray fixture" && icon->pixels!=original.pixels;}),"modify updates tooltip and dynamic pixels");
            fixture.Command(2);
            Check(Await([&]{auto icon=find(service.Current());return icon && (icon->state & NIS_HIDDEN);}),"hidden state arrives without dropping icon");
            const RECT rect{120,40,152,72}; service.SetGeometry(original.key,rect);
            NOTIFYICONIDENTIFIER identifier{};identifier.cbSize=sizeof(identifier);identifier.hWnd=fixture.value->window;identifier.uID=77;identifier.guidItem=kIconGuid;
            RECT actual{}; HRESULT result=Shell_NotifyIconGetRect(&identifier,&actual);
            std::cout<<"geometry hr="<<std::hex<<result<<std::dec<<" actual="<<actual.left<<","<<actual.top<<","<<actual.right<<","<<actual.bottom<<std::endl;
            Check(SUCCEEDED(result) && EqualRect(&rect,&actual),"real Shell geometry lookup returns mirrored icon rectangle");
            const POINT anchor{136,56};
            for(auto action:{Activation::LeftDown,Activation::LeftUp,Activation::DoubleClick,Activation::RightUp,Activation::Keyboard})
                Check(service.Activate(original.key,action,anchor),"activation delivered to fixture only");
            Check(Await([&]{return Read(fixture.value->count)>=6;}),"version 4 click callbacks arrive");
            const UINT expected[]{WM_LBUTTONDOWN,WM_LBUTTONUP,NIN_SELECT,WM_LBUTTONDBLCLK,WM_CONTEXTMENU,NIN_KEYSELECT};
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

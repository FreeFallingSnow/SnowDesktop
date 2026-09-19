#include "shell_context_menu_invoke.h"
#include "shell_extension_menu.h"
#include "shell_new_item_capture.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <stdexcept>
#include <wrl/client.h>

namespace
{

void Expect(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

struct TemporaryDirectory
{
    std::filesystem::path path;
    TemporaryDirectory()
    {
        GUID id{};
        Expect(SUCCEEDED(CoCreateGuid(&id)), "unique temporary directory ID");
        wchar_t text[40]{};
        StringFromGUID2(id, text, 40);
        path = std::filesystem::temp_directory_path() / (std::wstring(L"SnowDesktop-New-") + text);
        Expect(std::filesystem::create_directory(path), "create isolated test directory");
    }
    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void TestRealNewFolderCapture()
{
    TemporaryDirectory directory;
    auto capture = std::make_shared<snowdesktop::ShellNewItemCapture>(
        directory.path.wstring(), L"desktop-files", 7);
    {
        Microsoft::WRL::ComPtr<IContextMenu> context;
        Expect(SUCCEEDED(CoCreateInstance(CLSID_NewMenu, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(context.GetAddressOf()))), "real New handler");
        Microsoft::WRL::ComPtr<IShellExtInit> initializer;
        Expect(SUCCEEDED(context.As(&initializer)), "New handler initializer");
        PIDLIST_ABSOLUTE folder = nullptr;
        Expect(SUCCEEDED(SHParseDisplayName(directory.path.c_str(), nullptr, &folder, 0, nullptr)),
            "isolated directory PIDL");
        const auto initialized = initializer->Initialize(folder, nullptr, 0);
        CoTaskMemFree(folder);
        Expect(SUCCEEDED(initialized), "initialize isolated New menu");
        Expect(SUCCEEDED(snowdesktop::AttachShellNewItemCapture(context.Get(), capture)),
            "attach production New-item capture site");
        HMENU root = CreatePopupMenu();
        const auto queried = context->QueryContextMenu(root, 0, 1, 0x7FFF, CMF_NORMAL);
        HMENU submenu = GetSubMenu(root, 0);
        Microsoft::WRL::ComPtr<IContextMenu2> messages;
        if (SUCCEEDED(context.As(&messages)))
            messages->HandleMenuMsg(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(submenu), 0);
        const UINT command = snowdesktop::FindNewFolderCommand(context.Get(), submenu);
        Expect(SUCCEEDED(queried) && command != 0, "real NewFolder command resolves");
        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE;
        invoke.hwnd = CreateWindowExW(0, L"STATIC", L"New item test owner", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        invoke.lpVerb = MAKEINTRESOURCEA(command - 1);
        invoke.lpVerbW = MAKEINTRESOURCEW(command - 1);
        const std::wstring invocationDirectory = directory.path.wstring();
        std::string ansiDirectory;
        snowdesktop::SetShellInvocationDirectory(invoke, invocationDirectory, ansiDirectory);
        invoke.nShow = SW_SHOWNORMAL;
        const HRESULT invoked = context->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
        DestroyWindow(invoke.hwnd);
        DestroyMenu(root);
        if (FAILED(invoked)) std::cerr << "NewFolder HRESULT: " << std::hex << invoked << std::dec << '\n';
        Expect(SUCCEEDED(invoked),
            "real Windows NewFolder command succeeds");
    }
    std::vector<std::wstring> captured;
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (captured.empty() && GetTickCount64() < deadline)
    {
        capture->Consume([&](const auto& id, const auto& path, size_t index) {
            Expect(id == L"desktop-files" && index == 7, "creation keeps captured owner and insertion index");
            captured.push_back(path);
            return true;
        });
        if (!captured.empty()) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE,
            static_cast<DWORD>(deadline - std::min(deadline, GetTickCount64())), QS_ALLINPUT);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    Expect(captured.size() == 1 && std::filesystem::is_directory(captured.front()) &&
        std::filesystem::equivalent(std::filesystem::path(captured.front()).parent_path(), directory.path),
        "the real Shell callback reports exactly the created folder inside the isolated directory");
    Expect(std::distance(std::filesystem::directory_iterator(directory.path),
        std::filesystem::directory_iterator{}) == 1, "NewFolder creates exactly one output");
    Expect(capture->Consume([](const auto&, const auto&, size_t) { return false; }),
        "released New handler retires its completed capture");
}

void RunTests()
{
    const std::wstring currentDirectory =
        std::filesystem::current_path().wstring();
    Expect(snowdesktop::ShellInvocationDirectoryForItem(
            currentDirectory) == currentDirectory,
        "a directory item invokes Shell commands in that directory");

    const std::wstring filePath =
        (std::filesystem::current_path() /
            L"snowdesktop-shell-invoke-probe.txt").wstring();
    Expect(snowdesktop::ShellInvocationDirectoryForItem(filePath) ==
            currentDirectory,
        "a file item invokes Shell commands in its parent directory");

    const std::wstring desktopDirectory =
        snowdesktop::DesktopShellInvocationDirectory();
    Expect(!desktopDirectory.empty() &&
            std::filesystem::is_directory(desktopDirectory),
        "the physical desktop directory is available for background verbs");

    CMINVOKECOMMANDINFOEX invoke{};
    invoke.cbSize = sizeof(invoke);
    invoke.fMask = CMIC_MASK_UNICODE;
    std::string ansiDirectory;
    snowdesktop::SetShellInvocationDirectory(
        invoke, currentDirectory, ansiDirectory);
    Expect(invoke.lpDirectoryW == currentDirectory.c_str(),
        "the Unicode Shell invocation directory uses stable caller storage");
    Expect(invoke.lpDirectory != nullptr &&
            !ansiDirectory.empty(),
        "legacy Shell handlers also receive an ANSI invocation directory");

    // Query the real Windows New handler without displaying a menu or creating
    // files. This catches unsupported canonical verbs and lazy submenu loading.
    {
        Microsoft::WRL::ComPtr<IContextMenu> context;
        Expect(SUCCEEDED(CoCreateInstance(CLSID_NewMenu, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(context.GetAddressOf()))),
            "Windows New-menu handler is available");
        Microsoft::WRL::ComPtr<IShellExtInit> initializer;
        Expect(SUCCEEDED(context.As(&initializer)), "New handler accepts a folder");
        PIDLIST_ABSOLUTE folder = nullptr;
        Expect(SUCCEEDED(SHParseDisplayName(currentDirectory.c_str(), nullptr,
                &folder, 0, nullptr)), "test folder resolves to a PIDL");
        const HRESULT initialized = initializer->Initialize(folder, nullptr, 0);
        CoTaskMemFree(folder);
        Expect(SUCCEEDED(initialized), "New handler initializes for the target folder");
        HMENU root = CreatePopupMenu();
        Expect(SUCCEEDED(context->QueryContextMenu(root, 0, 1, 0x7FFF, CMF_NORMAL)),
            "New handler builds its commands");
        HMENU submenu = GetSubMenu(root, 0);
        Microsoft::WRL::ComPtr<IContextMenu2> messages;
        if (SUCCEEDED(context.As(&messages)))
            messages->HandleMenuMsg(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(submenu), 0);
        const UINT command = snowdesktop::FindNewFolderCommand(context.Get(), submenu);
        Expect(command != 0, "Ctrl+Shift+N resolves the real NewFolder command");
        EnableMenuItem(submenu, command, MF_BYCOMMAND | MF_GRAYED);
        Expect(snowdesktop::FindNewFolderCommand(context.Get(), submenu) == 0,
            "a disabled NewFolder command cannot be executed");
        DestroyMenu(root);
    }
    TestRealNewFolderCapture();
}
} // namespace

// Real inherited pipes and process supervision; only the third-party query is
// substituted, so a hung extension cannot be mistaken for a passing UI mock.
void TestExtensionSessions()
{
    namespace ext = snowdesktop::shell_extensions;
    ext::Entry first; first.provider="handler:sample"; first.key="compress"; first.label=L"压缩"; first.token=31;
    ext::Entry second=first;second.key="extract";second.token=32;second.enabled=false;
    ext::Entry group;group.provider=first.provider;group.label=L"Sample";group.children={first,second};
    ext::Preferences prefs{true,{{first.provider,"","Sample",ext::Placement::Submenu},
        {first.provider,"compress","压缩",ext::Placement::Root}, {first.provider,"extract","解压",ext::Placement::Hidden}}};
    const auto pinned=ext::SelectEntries(prefs,{group},ext::Placement::Root);
    Expect(pinned.size()==1&&pinned.front().key=="compress"&&pinned.front().token==31,
        "pinning a stable command promotes that command and preserves its invocation token");
    Expect(ext::SelectEntries(prefs,{group},ext::Placement::Submenu).empty(),
        "explicit per-command choices override a whole-group selection without exposing excluded commands");
    auto renamed=first;renamed.label=L"Renamed after language change";
    Expect(ext::ResolvePlacement(prefs,renamed)==ext::Placement::Root,"display labels never identify persisted commands");
    renamed.provider="handler:different";
    Expect(ext::ResolvePlacement(prefs,renamed)==ext::Placement::Hidden,"matching verbs from a different provider stay hidden");
    prefs.enabled=false;Expect(ext::SelectEntries(prefs,{group},ext::Placement::Root).empty(),"off switch suppresses all extensions");
    auto wait=[](ext::Session& session) {
        const auto end=GetTickCount64()+4000;std::optional<ext::Reply> reply;
        while(GetTickCount64()<end&&!reply)
        {
            MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
            reply=session.Poll();if(!reply)MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        }
        Expect(reply.has_value(),"isolated query has a bounded completion");return *reply;
    };
    ext::Request request;request.paths={L"synthetic-success"};
    {ext::Session session(request,2500);auto reply=wait(session);
        Expect(reply.ok&&reply.entries.size()==1&&reply.entries[0].label==L"压缩"&&reply.entries[0].checked&&!reply.entries[0].enabled,
            "isolated query transports Unicode labels and actual menu states");}
    request.paths={L"synthetic-hang"};const auto start=GetTickCount64();
    {ext::Session session(request,250);auto reply=wait(session);Expect(!reply.ok&&GetTickCount64()-start<3000,"hung query is terminated without blocking the parent");}
    request.paths={L"synthetic-success"};
    {ext::Session session(request,2500);Expect(wait(session).ok,"a timed-out extension does not poison the next menu session");}
    for(int i=0;i<3;++i){ext::Session cancelled(request);}
}

int wmain()
{
    if (const auto helper = snowdesktop::shell_extensions::TryRunHelper([](const auto& request) {
        if (!request.paths.empty() && request.paths.front() == L"synthetic-hang") Sleep(INFINITE);
        snowdesktop::shell_extensions::Reply reply; reply.ok = true;
        snowdesktop::shell_extensions::Entry entry; entry.label=L"压缩";entry.checked=true;entry.enabled=false;
        reply.entries.push_back(entry);return reply;
    })) return *helper;

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 1;
    try { RunTests(); TestExtensionSessions(); }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        CoUninitialize();
        return 1;
    }
    CoUninitialize();
    std::cout << "shell context-menu invocation tests passed\n";
    return 0;
}

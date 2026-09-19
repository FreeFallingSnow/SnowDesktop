#include "shell_context_menu_invoke.h"
#include "shell_extension_menu.h"
#include "shell_extension_catalogue_cache.h"
#include "menu_label.h"
#include "shell_new_item_capture.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
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

// Own uniquely named HKCU file-type keys only; never modify real handlers.
struct TemporaryVerb
{
    std::wstring extension, progId;
    HKEY verb = nullptr;
    std::vector<std::wstring> owned;
    void Clear() noexcept
    {
        if (verb) { RegCloseKey(verb); verb=nullptr; }
        for (const auto &path : owned) RegDeleteTreeW(HKEY_CURRENT_USER,path.c_str());
        owned.clear();
    }
    static void Value(HKEY key, const wchar_t *name, const std::wstring &text)
    {
        Expect(RegSetValueExW(key,name,0,REG_SZ,reinterpret_cast<const BYTE*>(text.c_str()),
            static_cast<DWORD>((text.size()+1)*sizeof(wchar_t)))==ERROR_SUCCESS,"write owned test metadata");
    }
    TemporaryVerb()
    {
        GUID guid{}; Expect(SUCCEEDED(CoCreateGuid(&guid)),"unique association ID");
        wchar_t id[40]{}; StringFromGUID2(guid,id,40);
        extension=std::wstring(L".snowmenu-")+id; progId=std::wstring(L"SnowDesktop.MenuTest.")+id;
        try
        {
            for (const auto &name : {extension,progId})
            {
                const auto path=L"Software\\Classes\\"+name;
                HKEY key=nullptr; DWORD disposition=0;
                Expect(RegCreateKeyExW(HKEY_CURRENT_USER,path.c_str(),0,nullptr,0,KEY_READ|KEY_WRITE,nullptr,
                    &key,&disposition)==ERROR_SUCCESS,"create private association");
                RegCloseKey(key);
                Expect(disposition==REG_CREATED_NEW_KEY,"never replace an existing association");
                owned.push_back(path);
            }
            HKEY key=nullptr;
            Expect(RegOpenKeyExW(HKEY_CURRENT_USER,owned[0].c_str(),0,KEY_SET_VALUE,&key)==ERROR_SUCCESS,"open owned extension");
            const auto status=RegSetValueExW(key,nullptr,0,REG_SZ,reinterpret_cast<const BYTE*>(progId.c_str()),
                static_cast<DWORD>((progId.size()+1)*sizeof(wchar_t)));
            RegCloseKey(key); Expect(status==ERROR_SUCCESS,"associate test file");
            const auto path=owned[1]+L"\\shell\\SnowDesktopProbe";
            Expect(RegCreateKeyExW(HKEY_CURRENT_USER,path.c_str(),0,nullptr,0,KEY_READ|KEY_WRITE,nullptr,
                &verb,nullptr)==ERROR_SUCCESS,"create private verb");
            Value(verb,nullptr,L"SnowDesktop isolated policy probe");
            HKEY command=nullptr;
            Expect(RegCreateKeyExW(verb,L"command",0,nullptr,0,KEY_SET_VALUE,nullptr,&command,nullptr)==ERROR_SUCCESS,
                "create private command metadata");
            constexpr wchar_t executable[]=L"notepad.exe \"%1\""; // Never invoked.
            const auto written=RegSetValueExW(command,nullptr,0,REG_SZ,reinterpret_cast<const BYTE*>(executable),sizeof(executable));
            RegCloseKey(command); Expect(written==ERROR_SUCCESS,"write private command metadata");
        }
        catch (...) { Clear(); throw; }
    }
    ~TemporaryVerb() { Clear(); }
    void Disable() { Value(verb,L"LegacyDisable",L""); }
};
template<class Wait>
void TestSystemPolicy(Wait wait, const std::filesystem::path &directory)
{
    namespace ext=snowdesktop::shell_extensions;
    TemporaryVerb registration;
    const auto file=directory/(L"sample"+registration.extension);
    { std::ofstream output(file); output<<"isolated Shell policy sample"; }
    ext::Request request; request.paths={file.wstring()};
    const auto probe=[](const auto &list) {
        return std::find_if(list.begin(),list.end(),[](const auto &e){return e.label==L"SnowDesktop isolated policy probe";});
    };
    {
        ext::Session visible(request); const auto reply=wait(visible);
        Expect(reply.ok&&probe(reply.entries)!=reply.entries.end(),
            "system-enabled private verb appears through the actual Shell aggregate");
    }
    registration.Disable();
    {
        ext::Session disabled(request); const auto reply=wait(disabled);
        const auto shown=ext::VisibleEntries({},reply.entries,request);
        Expect(reply.ok&&probe(shown)==shown.end(),
            "following the Shell never restores a system-disabled command");
    }
}
// Real inherited pipes and process supervision; only the third-party query is
// substituted, so a hung extension cannot be mistaken for a passing UI mock.
void TestExtensionSessions()
{
    namespace ext = snowdesktop::shell_extensions;
    ext::InvalidateMenuCache();
    ext::Entry archive;
    archive.provider = "verb:sevenzip";
    archive.key = "SevenZip";
    archive.label = L"7-Zip";
    ext::Entry command; command.key="compress"; command.label=L"压缩"; command.token=31;
    command.checked=true; command.enabled=false;
    archive.children={command};
    ext::Entry other; other.provider="verb:editor"; other.label=L"Editor"; other.token=32;
    ext::Preferences prefs;
    ext::Request target; target.context=ext::Context::File;
    const auto direct=ext::VisibleEntries(prefs,{archive,other},target);
    Expect(direct.size()==2&&direct[0].label==L"7-Zip"&&direct[0].children.size()==1&&
        direct[0].children[0].token==31&&direct[0].children[0].checked&&!direct[0].children[0].enabled,
        "default system menus retain original roots, submenus and states without synthetic wrappers");
    for (auto context : {ext::Context::File, ext::Context::Folder, ext::Context::FolderBackground, ext::Context::Desktop})
    {
        prefs.hidden.clear(); ext::SetHidden(prefs, archive.provider, context, true);
        for (auto current : {ext::Context::File, ext::Context::Folder, ext::Context::FolderBackground, ext::Context::Desktop})
        {
            target.context=current;
            const auto shown=ext::VisibleEntries(prefs,{archive,other},target);
            Expect(shown.size()==(current==context?1u:2u), "secondary hiding is independent across all four contexts");
        }
        ext::SetHidden(prefs,archive.provider,context,false);
        Expect(ext::VisibleEntries(prefs,{archive},target).size()==1, "restoring visibility removes only the local exclusion");
    }
    Expect(ext::VisibleEntries(prefs,{},target).empty(), "local preferences never resurrect an item absent from the Shell");
    ext::SetHidden(prefs,archive.provider,ext::Context::File,true);
    archive.label=L"Localized title"; target.context=ext::Context::File;
    Expect(ext::VisibleEntries(prefs,{archive},target).empty(), "canonical identities survive display-name changes");
    auto wait=[](ext::Session& session) {
        const auto end=GetTickCount64()+10000;std::optional<ext::Reply> reply;
        while(GetTickCount64()<end&&!reply)
        {
            MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
            reply=session.Poll();if(!reply)MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        }
        Expect(reply.has_value(),"isolated query has a bounded completion");return *reply;
    };
    ext::Request request;request.paths={L"synthetic-success"};
    DWORD firstProcess = 0;
    {ext::Session session(request,2500);auto reply=wait(session); firstProcess = session.ProcessId();
        Expect(reply.ok&&reply.entries.size()==1&&reply.entries[0].label==L"压缩"&&reply.entries[0].checked&&!reply.entries[0].enabled,
            "isolated query transports Unicode labels and actual menu states");}
    request.paths={L"synthetic-second"};
    {ext::Session session(request,2500);auto reply=wait(session);
        Expect(session.ProcessId()==firstProcess && reply.ok && reply.entries[0].key=="second",
            "a warm worker is reused but queries the new selection instead of reusing old commands");}
    {
        ext::Session first(request,2500); Expect(wait(first).ok,"first concurrent query");
        ext::Session second(request,2500); Expect(wait(second).ok,"second concurrent query");
        Expect(first.ProcessId()!=second.ProcessId(),"active menu sessions never share a worker or command map");
    }
    ext::InvalidateMenuCache();
    {ext::Session session(request,2500);auto reply=wait(session);
        Expect(session.ProcessId()!=firstProcess && reply.ok,"explicit refresh discards the previous cached worker");}
    {
        ext::Request sample; sample.catalogueOnly=true; sample.context=ext::Context::File;
        std::wstring samplePath;
        DWORD sampleWorker=0;
        {
            ext::Session session(sample,2500); auto reply=wait(session);
            Expect(reply.ok,"owned catalogue sample query completes");
            samplePath=reply.entries[0].label; sampleWorker=session.ProcessId();
            Expect(GetFileAttributesW(samplePath.c_str())!=INVALID_FILE_ATTRIBUTES,"sample lives through its menu session");
        }
        // Reacquire immediately to exercise a release acknowledgement arriving
        // after the next request; it must not remove the current sample.
        ext::Session next(sample,2500); auto reply=wait(next);
        Expect(next.ProcessId()==sampleWorker && reply.ok &&
            GetFileAttributesW(samplePath.c_str())==INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(reply.entries[0].label.c_str())!=INVALID_FILE_ATTRIBUTES,
            "release acknowledgements remove retired samples while preserving the next session's object");
    }
    request.paths={L"synthetic-hang"};const auto start=GetTickCount64();
    {ext::Session session(request,250);auto reply=wait(session);Expect(!reply.ok&&GetTickCount64()-start<3000,"hung query is terminated without blocking the parent");}
    request.paths={L"synthetic-success"};
    {ext::Session session(request,2500);Expect(wait(session).ok,"a timed-out extension does not poison the next menu session");}
    for(int i=0;i<3;++i){ext::Session cancelled(request);}
    // Exercise the production aggregate against the installed system menus,
    // on an isolated directory and without invoking a file operation.
    TemporaryDirectory directory;
    request.paths={directory.path.wstring()};request.background=false;
    struct RealQueryMode
    {
        RealQueryMode(){ext::InvalidateMenuCache(); SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU",L"1");}
        ~RealQueryMode(){ext::InvalidateMenuCache(); SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU",nullptr);}
    } realMode;
    TestSystemPolicy(wait, directory.path);
    // Exercise the same four default queries as the settings tabs, including
    // real installed handler images. Report every scope before failing so one
    // incompatible DLL cannot hide the remaining scope results.
    bool scopesPassed = true;
    for (const auto context : {ext::Context::File, ext::Context::Folder,
                              ext::Context::FolderBackground, ext::Context::Desktop})
    {
        ext::Request sample; sample.catalogueOnly = true; sample.context = context;
        ext::Session actual(sample);
        const auto reply = wait(actual);
        std::cout << "System scope " << static_cast<int>(context) << ": "
                  << (reply.ok ? "queried" : "FAILED") << ", entries=" << reply.entries.size()
                  << ", " << reply.error << std::endl;
        scopesPassed &= reply.ok;
        if (!reply.ok || (context != ext::Context::File && context != ext::Context::Folder))
            continue;
        const auto archiveEntry = std::find_if(reply.entries.begin(), reply.entries.end(), [](const auto &entry) {
            return entry.label == L"7-Zip";
        });
        // Registration alone does not imply system visibility: 7-Zip may be
        // disabled. The isolated verb above independently checks visibility.
        if (archiveEntry != reply.entries.end())
        {
            std::cout << "7-Zip menu image: " << archiveEntry->width << "x" << archiveEntry->height
                      << ", " << archiveEntry->pixels.size() << " bytes" << std::endl;
            scopesPassed &= !archiveEntry->children.empty() && archiveEntry->width > 0 && archiveEntry->height > 0 &&
                            !archiveEntry->pixels.empty();
        }
        else std::cout << "7-Zip is not present in this system menu; icon check not applicable" << std::endl;
    }
    Expect(scopesPassed, "all four real settings scopes query successfully and preserve installed 7-Zip icons");

}

void TestCatalogueCache()
{
    namespace ext = snowdesktop::shell_extensions;
    const auto escaped=snowdesktop::DecodeMenuLabel(L"研发 && 工具(&T)\tCtrl+T");
    Expect(escaped.text==L"研发 & 工具(T)\tCtrl+T" && escaped.accessKey==L't',
        "native text preserves escaped ampersands and records the marked access key");
    Expect(snowdesktop::DecodeMenuLabel(L"file(A) && B\tCtrl+&C").accessKey==0,
        "literal names and shortcut-column text never invent access keys");
    ext::Entry transport; transport.accessKey=L'a';
    const auto packed=snowdesktop::settings_ipc::Pack(transport);
    Expect(snowdesktop::settings_ipc::Unpack<ext::Entry>(packed).accessKey==L'a',
        "native access keys survive the real Shell IPC codec");
    ext::CatalogueCache cache;
    ext::Request file; file.catalogueOnly=true; file.context=ext::Context::File;
    ext::Request folder=file; folder.context=ext::Context::Folder;
    ext::Reply reply; reply.ok=true;
    ext::Entry entry; entry.label=L"7-Zip"; entry.token=12;
    ext::Entry child; child.token=13; entry.children={child}; reply.entries={entry};
    cache.Store(file,reply,1,100);
    const auto hit=cache.Find(file,1,101);
    Expect(hit && hit->entries[0].label==L"7-Zip" && !hit->entries[0].token && !hit->entries[0].children[0].token,
        "settings cache preserves display data but strips session command tokens recursively");
    Expect(!cache.Find(folder,1,101),"settings contexts have independent cached lists");
    Expect(!cache.Find(file,1,100+ext::CatalogueCache::LifetimeMs),"expired catalogues query the current system");
    cache.Store(file,reply,1,200);
    Expect(!cache.Find(file,2,201),"registry invalidation rejects stale catalogue data");
    cache.Store(file,reply,2,300); cache.Clear();
    Expect(!cache.Find(file,2,301),"manual Refresh bypasses all settings snapshots");
    file.paths={L"specific-object"}; cache.Store(file,reply,2,400);
    Expect(!cache.Find(file,2,401),"specific inspected objects are never substituted by cached generic samples");
}

// Opt-in measurements use the real Session path and private files only. They
// do not invoke extensions or add machine-dependent latency assertions.
void BenchmarkMenus()
{
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory directory;
    const auto file = directory.path / L"sample.txt";
    { std::ofstream output(file); output << "menu timing sample"; }
    SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU", L"1");
    ext::InvalidateMenuCache();
    for (const bool folder : {false, true})
    {
        ext::Request request;
        request.paths = {(folder ? directory.path : file).wstring()};
        for (int iteration = 0; iteration < 4; ++iteration)
        {
            const auto start = GetTickCount64();
            ext::Session session(request);
            const auto launched = GetTickCount64();
            std::optional<ext::Reply> reply;
            while (!reply && GetTickCount64() - start < 10000)
            {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                { TranslateMessage(&message); DispatchMessageW(&message); }
                reply = session.Poll();
                if (!reply) MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
            const auto elapsed = GetTickCount64() - start;
            std::cout << "menu_benchmark scope=" << (folder ? "folder" : "file")
                      << " iteration=" << iteration << " start_ms=" << launched - start
                      << " ready_ms=" << elapsed << " ok=" << (reply && reply->ok)
                      << " worker=" << session.ProcessId()
                      << " entries=" << (reply ? reply->entries.size() : 0) << std::endl;
            Expect(reply && reply->ok, "benchmark real menu query succeeds");
        }
    }
    SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU", nullptr);
    ext::InvalidateMenuCache();
}

int wmain(int argc, wchar_t **argv)
{
    snowdesktop::shell_extensions::QueryExecutor query;
    wchar_t realMode[4]{};
    if(!GetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU",realMode,4)) query=[](const auto& request) {
        if (!request.paths.empty() && request.paths.front() == L"synthetic-hang") Sleep(INFINITE);
        snowdesktop::shell_extensions::Reply reply; reply.ok = true;
        snowdesktop::shell_extensions::Entry entry; entry.label=L"压缩";entry.checked=true;entry.enabled=false;
        entry.key=request.paths.front()==L"synthetic-second" ? "second" : "first";
        if (request.catalogueOnly) entry.label=request.paths.front();
        reply.entries.push_back(entry);return reply;
    };
    if (const auto helper = snowdesktop::shell_extensions::TryRunHelper(std::move(query))) return *helper;

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 1;
    try
    {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--benchmark-shell-menu") BenchmarkMenus();
        else { RunTests(); TestCatalogueCache(); TestExtensionSessions(); }
    }
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

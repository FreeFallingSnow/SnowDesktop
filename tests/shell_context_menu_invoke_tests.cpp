#include "shell_context_menu_invoke.h"
#include "shell_extension_menu.h"
#include "shell_extension_catalogue.h"
#include "shell_extension_menu_items.h"
#include "shell_extension_menu_presentation.h"
#include "menu_label.h"
#include "shell_new_item_capture.h"

#include <cstdlib>
#include <chrono>
#include <atomic>
#include <mutex>
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
void TestDeferredPopups()
{
    // Actual HMENU dummy rows reproduce the Shell placeholder shape, including
    // a command ID which the old reader incorrectly treated as executable.
    using snowdesktop::shell_extensions::RequiresNativePopup;
    HMENU menu = CreatePopupMenu();
    Expect(menu != nullptr, "create isolated native submenu");
    struct Cleanup
    {
        HMENU menu;
        ~Cleanup()
        {
            DestroyMenu(menu);
        }
    } cleanup{menu};
    AppendMenuW(menu, MF_STRING, 41, L"");
    Expect(RequiresNativePopup(menu),
           "an unnamed dummy command defers the parent popup, not an ellipsis child");
    ModifyMenuW(menu, 0, MF_BYPOSITION | MF_STRING, 41, L"...");
    Expect(RequiresNativePopup(menu), "a lazy ellipsis row opens the native parent directly");
    ModifyMenuW(menu, 0, MF_BYPOSITION | MF_STRING, 41, L"Open PowerShell here");
    Expect(!RequiresNativePopup(menu), "materialized submenu commands retain custom rendering");
    AppendMenuW(menu, MF_OWNERDRAW, 42, nullptr);
    Expect(RequiresNativePopup(menu), "unlabelled owner-drawn children keep their native parent renderer");
}

void TestRegistryCatalogue()
{
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory temp;
    const auto path = L"Software\\SnowDesktopCatalogueTests\\" + temp.path.filename().wstring();
    struct RegistryFixture
    {
        HKEY key = nullptr; std::wstring path;
        ~RegistryFixture() { if (key) RegCloseKey(key); RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()); }
    } registry{nullptr, path};
    Expect(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &registry.key, nullptr) == ERROR_SUCCESS, "private registry fixture");
    auto put = [&](const wchar_t *key, const wchar_t *name, const wchar_t *value) {
        HKEY created = nullptr;
        Expect(RegCreateKeyExW(registry.key, key, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &created, nullptr) == ERROR_SUCCESS, "create fixture registration");
        const auto status = RegSetValueExW(created, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value), static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
        RegCloseKey(created); Expect(status == ERROR_SUCCESS, "write fixture registration");
    };
    put(L".snowtest", nullptr, L"SnowTest.Document");
    put(L"SnowTest.Document\\shell\\inspect", L"MUIVerb", L"Same name");
    put(L"SnowTest.Document\\shell\\inspect\\command", nullptr, L"unused.exe %1");
    put(L"*\\shell\\other", L"MUIVerb", L"Same name");
    put(L"*\\shell\\hidden", L"LegacyDisable", L"");
    put(L"*\\shellex\\ContextMenuHandlers\\one", nullptr, L"{B92A9760-188A-44ED-88A5-F9E3D30E33AF}");
    put(L"Directory\\shellex\\ContextMenuHandlers\\two", nullptr, L"{B92A9760-188A-44ED-88A5-F9E3D30E33AF}");
    auto catalogue = ext::ReadCatalogue(registry.key, false);
    Expect(catalogue.rows.size() == 4, "registry catalogue finds type-specific verbs and merges a handler by CLSID");
    auto find = [&](const std::string &id) -> ext::Registration& {
        auto row = std::find_if(catalogue.rows.begin(), catalogue.rows.end(), [&](const auto &r) { return r.id == id; });
        Expect(row != catalogue.rows.end(), "expected registration exists"); return *row;
    };
    auto &handler = find("clsid:{b92a9760-188a-44ed-88a5-f9e3d30e33af}");
    Expect(handler.sources.size() == 2 && handler.contexts == 3 && !handler.linked, "one handler preserves both scopes without claiming a runtime association");
    Expect(!find("reg:*\\shell\\hidden").systemEnabled, "system-disabled registration is excluded from available settings");
    ext::Request request; request.paths = {L"C:\\fixture.snowtest"}; request.context = ext::Context::File;
    ext::Reply reply; reply.ok = true;
    ext::Entry actual; actual.key = "inspect"; actual.provider = "verb:inspect"; actual.label = L"Same name";
    reply.entries = {actual}; ext::Associate(catalogue, request, reply);
    Expect(reply.entries[0].registration == "reg:snowtest.document\\shell\\inspect", "unique canonical verb associates the actual type-specific command");
    reply.entries[0].key.clear(); reply.entries[0].registration.clear(); ext::Associate(catalogue, request, reply);
    Expect(reply.entries[0].registration.empty(), "a matching display name never establishes ownership");
    JsonValue legacy;
    Expect(ParseJson(R"({"shown":[{"id":"verb:inspect","context":0},{"id":"both","context":0},{"id":"both","context":1}]})", legacy), "legacy fixture parses");
    auto prefs = ext::ReadPreferences(&legacy);
    Expect(prefs.rulesVersion == 2 && !ext::IsHidden(prefs, "verb:inspect", ext::Context::File) && ext::IsHidden(prefs, "verb:inspect", ext::Context::Folder), "migration preserves different old scopes without broadening visibility");
    Expect(ext::CommonShown(prefs, "both", ext::Category::Objects), "matching old states merge into common rule");
    ext::MigrateAssociations(prefs, catalogue);
    Expect(!ext::IsHidden(prefs, "reg:snowtest.document\\shell\\inspect", ext::Context::File) && ext::IsHidden(prefs, "reg:snowtest.document\\shell\\inspect", ext::Context::Folder), "identity migration preserves the exact old enabled scope");
    ext::SetOverride(prefs, "reg:snowtest.document\\shell\\inspect", ext::Context::File, ext::Visibility::Inherit);
    ext::MigrateAssociations(prefs, catalogue);
    Expect(ext::OverrideOf(prefs, "reg:snowtest.document\\shell\\inspect", ext::Context::File) == ext::Visibility::Inherit && ext::IsHidden(prefs, "reg:snowtest.document\\shell\\inspect", ext::Context::File), "restored inheritance does not resurrect a retained legacy opt-in");
    ext::SetCommon(prefs, "both", ext::Category::Objects, true);
    ext::SetOverride(prefs, "both", ext::Context::Folder, ext::Visibility::Hide);
    Expect(ext::IsHidden(prefs, "both", ext::Context::Folder), "location hiding takes precedence over common visibility");
    ext::SetOverride(prefs, "both", ext::Context::Folder, ext::Visibility::Inherit);
    Expect(!ext::IsHidden(prefs, "both", ext::Context::Folder), "restore inheritance removes the exception");
    JsonValue saved; Expect(ParseJson(ext::WritePreferences(prefs), saved) && ext::ReadPreferences(&saved) == prefs, "rules and retained legacy records round trip");
}

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
    Expect(ext::VisibleEntries(prefs, {archive, other}, target).empty(),
           "fresh settings hide all extension items");
    for (auto context : {ext::Context::File, ext::Context::Folder, ext::Context::FolderBackground, ext::Context::Desktop})
    {
        prefs.shown.clear();
        ext::SetHidden(prefs, archive.provider, context, false);
        for (auto current : {ext::Context::File, ext::Context::Folder, ext::Context::FolderBackground, ext::Context::Desktop})
        {
            target.context=current;
            const auto shown=ext::VisibleEntries(prefs,{archive,other},target);
            Expect(shown.size() == (current == context ? 1u : 0u),
                   "explicit visibility is independent across all four contexts");
        }
        target.context = context;
        const auto direct = ext::VisibleEntries(prefs, {archive, other}, target);
        Expect(direct.size() == 1 && direct[0].label == L"7-Zip" && direct[0].children.size() == 1 &&
                   direct[0].children[0].token == 31 && direct[0].children[0].checked &&
                   !direct[0].children[0].enabled,
               "enabled system items retain roots, submenus and disabled states without wrappers");
        archive.label = L"Localized title";
        Expect(ext::VisibleEntries(prefs, {archive}, target).size() == 1,
               "canonical visibility survives a display-name change");
        archive.label = L"7-Zip";
        ext::SetHidden(prefs, archive.provider, context, true);
        Expect(ext::VisibleEntries(prefs, {archive}, target).empty(), "turning an item off hides it again");
    }
    ext::SetHidden(prefs, archive.provider, target.context, false);
    Expect(ext::VisibleEntries(prefs, {}, target).empty(),
           "an opt-in never resurrects an item absent from the Shell");
    auto wait=[](ext::Session& session) {
        const auto end=GetTickCount64()+10000;std::optional<ext::Reply> reply;
        while(GetTickCount64()<end&&!reply)
        {
            MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
            reply=session.Poll();if(!reply)MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        }
        Expect(reply.has_value(),"isolated query has a bounded completion");return *reply;
    };
    {
        // Exercise the real process launcher while the caller owns a redirected
        // stdout pipe. Third-party query code must be unable to retain/write it.
        struct OutputPipe
        {
            HANDLE original = GetStdHandle(STD_OUTPUT_HANDLE), read = nullptr, write = nullptr;
            ~OutputPipe()
            {
                SetStdHandle(STD_OUTPUT_HANDLE, original);
                if (read)
                    CloseHandle(read);
                if (write)
                    CloseHandle(write);
            }
        } output;
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        Expect(CreatePipe(&output.read, &output.write, &security, 0) != FALSE, "private stdout probe pipe");
        Expect(SetStdHandle(STD_OUTPUT_HANDLE, output.write) != FALSE,
               "redirect only the probe caller's stdout");
        ext::Request probe;
        probe.paths = {L"synthetic-stdio"};
        ext::Session session(probe, 2500);
        SetStdHandle(STD_OUTPUT_HANDLE, output.original);
        const auto reply = wait(session);
        DWORD available = 0;
        Expect(PeekNamedPipe(output.read, nullptr, 0, nullptr, &available, nullptr) != FALSE &&
                   available == 0 && reply.ok && reply.entries.front().key == "isolated-stdio",
               "Shell helpers cannot inherit or write the caller's redirected output stream");
    }
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
        for (const auto &entry : reply.entries)
            if (entry.label.find(L"PowerShell") != std::wstring::npos)
            {
                std::cout << "PowerShell cascade: native=" << entry.native
                          << ", children=" << entry.children.size() << std::endl;
                scopesPassed &=
                    std::none_of(entry.children.begin(), entry.children.end(), [](const auto &child) {
                        return child.label.empty() || child.label == L"…" || child.label == L"...";
                    });
            }
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

    // Windows' packaged Terminal registration was missing only from the first
    // aggregate in each helper. Compare the same unchanged desktop across a
    // cold and warm request; never invoke Terminal or depend on its local title.
    for (int coldStart = 0; coldStart < 2; ++coldStart)
    {
        ext::InvalidateMenuCache();
        ext::Request desktop;
        desktop.catalogueOnly = true;
        desktop.context = ext::Context::Desktop;
        ext::Reply first, second;
        {
            ext::Session cold(desktop);
            first = wait(cold);
        }
        {
            ext::Session warm(desktop);
            second = wait(warm);
        }
        Expect(first.ok && second.ok, "cold and warm desktop queries complete");
        const auto terminal = [](const auto &entries) {
            return std::any_of(entries.begin(), entries.end(), [](const auto &entry) {
                return entry.provider == "verb:{9f156763-7844-4dc4-b2b1-901f640f5155}";
            });
        };
        std::cout << "Cold desktop Terminal: first=" << terminal(first.entries)
                  << ", warm=" << terminal(second.entries) << std::endl;
        Expect(!terminal(second.entries) || terminal(first.entries),
               "Terminal available in the system must already appear in the first cold desktop query");
    }
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
    TemporaryDirectory directory;
    ext::MenuSnapshotCache writer(directory.path / L"cache");
    ext::Request file; file.catalogueOnly=true; file.context=ext::Context::File;
    const auto path = directory.path / L"sample.txt";
    { std::ofstream output(path); output << "sample"; }
    file.paths = {path.wstring()};
    ext::Reply reply; reply.ok=true;
    ext::Entry entry; entry.label=L"7-Zip"; entry.provider="archive"; entry.token=12;
    entry.width=1;entry.height=1;entry.pixels={12,24,36,255};
    ext::Entry child; child.token=13;child.key="compress";child.label=L"压缩";
    entry.children={child}; reply.entries={entry};
    const auto ticket=writer.Capture(file);
    Expect(writer.Store(ticket,reply,100),"write display snapshot to private disk cache");
    ext::MenuSnapshotCache reader(directory.path / L"cache");
    file.catalogueOnly=false; // The host reads a settings-written exact-object snapshot.
    const auto hostTicket=reader.Capture(file);
    const auto hit=reader.Find(hostTicket,101);
    // A separate helper process reads the settings-written file directly. Only
    // its query boundary is replaced, not serialization or the disk reader.
    ext::Request crossProcess;crossProcess.paths={L"read-disk-cache",writer.Directory().wstring(),path.wstring()};
    {
        ext::Session helper(crossProcess);
        std::optional<ext::Reply> remote;
        const auto deadline=GetTickCount64()+10000;
        while(!remote&&GetTickCount64()<deadline)
        {
            MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
            remote=helper.Poll();
            if(!remote) MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        }
        Expect(remote&&remote->ok&&remote->entries.size()==1&&remote->entries[0].pixels==entry.pixels&&
            !remote->entries[0].token,"another process reads the same disk snapshot and icons without native tokens");
    }
    Expect(hit && hit->entries[0].label==L"7-Zip" && !hit->entries[0].token && !hit->entries[0].children[0].token &&
        hit->entries[0].pixels==entry.pixels,"disk reload shares text, hierarchy and icons but never Shell tokens");
    const auto reference=ext::AppendReference(ext::AppendReference({},entry),child);
    reply.entries[0].children[0].token=72;
    Expect(ext::ResolveCommand(reply,reference)==72,"cached selection resolves the fresh command token, not the saved token");
    reply.entries[0].children[0].enabled=false;
    Expect(!ext::ResolveCommand(reply,reference),"disabled fresh commands cannot execute through a cache hit");
    reply.entries[0].children[0].enabled=true;reply.entries[0].enabled=false;
    Expect(!ext::ResolveCommand(reply,reference),"disabled ancestor blocks cached child invocation");
    reply.entries[0].enabled=true;reply.entries.push_back(reply.entries[0]);
    Expect(!ext::ResolveCommand(reply,reference),"ambiguous menu identities cannot select a command by position");
    reply.entries.resize(1);reply.entries[0].children[0].key="different";
    Expect(!ext::ResolveCommand(reply,reference),"a reused position with another verb cannot execute the old selection");
    const auto other = directory.path / L"other.txt";
    { std::ofstream output(other); output << "sample"; }
    auto different=file;different.paths={other.wstring()};
    Expect(!reader.Find(reader.Capture(different),101),"same-extension files never share contextual command trees");
    different=file;different.extended=true;
    Expect(!reader.Find(reader.Capture(different),101),"Shift extended menus have their own snapshots");
    auto sample=file;sample.paths.clear();sample.catalogueOnly=true;
    Expect(!reader.Find(reader.Capture(sample),101),"settings samples do not replace exact-object menus");
    Expect(writer.Store(writer.Capture(sample),reply,100),"sample catalogue uses the same bounded disk store");
    sample.context=ext::Context::Folder;
    Expect(!reader.Find(reader.Capture(sample),101),"settings scopes have independent sample catalogues");
    Expect(!reader.Find(hostTicket,100+ext::MenuSnapshotCache::LifetimeMs),"expired disk snapshots are not presented");
    Expect(!reader.Find(hostTicket,99),"clock rollback rejects snapshots from the future");
    writer.Invalidate();
    Expect(!reader.Find(hostTicket,101)&&!reader.Find(reader.Capture(file),101),"another process invalidates disk and memory views");
    Expect(!writer.Store(ticket,reply,102),"an old in-flight query cannot restore invalidated cache data");
    const auto changedTicket=writer.Capture(file);
    { std::ofstream output(path,std::ios::app); output << "changed"; }
    Expect(!writer.Store(changedTicket,reply,102),"target changes during a query invalidate its pending snapshot");
    const auto current=writer.Capture(file);
    Expect(writer.Store(current,reply,103),"write current target snapshot");
    auto older = writer.Begin(current), newer = writer.Begin(current);
    newer.dependency = 37;
    Expect(writer.Store(newer, reply, 103) && !writer.Store(older, reply, 103), "older completion cannot overwrite a newer request sequence");
    auto differentDependency = current; differentDependency.dependency = 38;
    Expect(!reader.Find(differentDependency, 104), "disk snapshots from another registration dependency are misses");
    { std::ofstream output(directory.path / L"cache" / current.file,std::ios::binary);output<<"truncated"; }
    ext::MenuSnapshotCache afterRestart(directory.path / L"cache");
    Expect(!afterRestart.Find(current,104),"corrupt disk cache falls back to a live query");

    ext::Request desktop;
    desktop.paths = {directory.path.wstring()};
    desktop.background = true;
    desktop.context = ext::Context::Desktop;
    const auto desktopTicket = writer.Capture(desktop);
    Expect(writer.Store(desktopTicket, reply, 200), "seed a desktop display snapshot");
    {
        std::ofstream childFile(directory.path / L"new-child.txt");
        childFile << "desktop content changed";
    }
    // Force a different directory write time so the regression cannot depend
    // on filesystem timestamp granularity or a fixed sleep.
    const auto oldTime = std::filesystem::last_write_time(directory.path);
    std::filesystem::last_write_time(directory.path, oldTime + std::chrono::seconds(2));
    Expect(reader.Find(reader.Capture(desktop), 201).has_value(),
           "adding desktop files does not discard its display snapshot");
    for (int i = 0; i < 270; ++i)
    {
        const auto sibling = directory.path / (L"cache-collision-" + std::to_wstring(i) + L".txt");
        {
            std::ofstream output(sibling);
            output << i;
        }
        ext::Request object;
        object.paths = {sibling.wstring()};
        Expect(writer.Store(writer.Capture(object), reply, 200), "fill unrelated selection snapshots");
    }
    auto extendedDesktop = desktop;
    extendedDesktop.extended = true;
    auto extendedReply = reply;
    extendedReply.entries[0].label = L"Shift desktop";
    Expect(writer.Store(writer.Capture(extendedDesktop), extendedReply, 200),
           "seed extended desktop snapshot");
    const auto retained = reader.Find(reader.Capture(desktop), 201);
    Expect(retained && retained->entries[0].label == reply.entries[0].label,
           "unrelated selections and Shift menus cannot evict the ordinary desktop snapshot");
    size_t snapshots = 0;
    for (const auto &cacheFile : std::filesystem::directory_iterator(writer.Directory()))
        if (cacheFile.path().extension() == L".bin" && cacheFile.path().filename() != L"epoch.bin") ++snapshots;
    Expect(snapshots <= ext::MenuSnapshotCache::DiskEntries, "full-key disk cache evicts past its 256-entry budget");
}

template<class Condition>
void PumpUntil(Condition condition, const char *message)
{
    const auto deadline=GetTickCount64()+10000;
    while (!condition() && GetTickCount64()<deadline)
    {
        MSG msg{};
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg);DispatchMessageW(&msg); }
        if (!condition()) MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
    }
    Expect(condition(),message);
}
void TestSnapshotPresentation()
{
    namespace ext = snowdesktop::shell_extensions;
    namespace menu = snowdesktop::modern_menu;
    TemporaryDirectory directory;
    const auto path = directory.path / L"presentation.txt";
    { std::ofstream file(path); file << "private target"; }
    ext::Request request; request.paths = {path.wstring()};
    ext::MenuService service(directory.path / L"cache");
    ext::Preferences prefs;
    menu::Item more; more.label = L"More"; more.command = 7;
    {
        ext::Presentation hidden(request, prefs, L"", L"", service);
        std::vector<menu::Item> items = {more}; menu::Options options;
        hidden.Attach(items, options, 7);
        Expect(items.size() == 1 && !options.pollItems && !service.View(request).pending, "default-hidden popup starts no Shell query");
    }
    ext::SetHidden(prefs, "verb:first", ext::Context::File, false);
    {
        ext::Presentation cold(request, prefs, L"", L"", service);
        std::vector<menu::Item> items = {more}; menu::Options options;
        cold.Attach(items, options, 7);
        Expect(items.size() == 1 && options.pollItems, "cold popup starts with base commands and an asynchronous completion hook, without a placeholder");
        PumpUntil([&] { return service.View(request).snapshot.has_value(); }, "first uncached popup completes its real query");
        Expect(!options.pollItems(items, false), "cold completion waits while the pointer or cascade prevents safe insertion");
        const auto loaded = options.pollItems(items, true);
        Expect(loaded && loaded->size() == 2 && loaded->front().label == L"压缩" && loaded->back().command == 7,
               "first uncached popup inserts enabled commands above More when the renderer permits it");
    }
    PumpUntil([&] { return service.View(request).snapshot.has_value(); }, "closing a popup leaves the host query alive to warm the next popup");
    const auto first = service.View(request);
    {
        ext::Presentation warm(request, prefs, L"", L"", service);
        std::vector<menu::Item> items = {more}; menu::Options options;
        warm.Attach(items, options, 7);
        Expect(items.size() == 2 && items.front().label == L"压缩" && items.back().command == 7 && !options.pollItems,
               "warm popup reads immutable memory snapshot above the bottom footer");
        service.Query(request, ext::QueryPriority::Menu, true);
        PumpUntil([&] { return service.View(request).revision > first.revision; }, "background refresh finishes");
        Expect(items.size() == 2 && !options.pollItems, "completed refresh cannot alter the open menu or its hit regions");
    }
    const auto otherPath = directory.path / L"quick-close.txt";
    { std::ofstream file(otherPath); file << "private target"; }
    auto other = request; other.paths = {otherPath.wstring()};
    { ext::Presentation closed(other, prefs, L"", L"", service); }
    PumpUntil([&] { return service.View(other).snapshot.has_value(); }, "closing an uncached popup keeps its shared query alive");
}
void TestPendingCachedClick()
{
    namespace ext = snowdesktop::shell_extensions;
    namespace menu = snowdesktop::modern_menu;
    TemporaryDirectory directory;
    const auto path = directory.path / L"retry.txt", output = directory.path / L"invoked.txt";
    { std::ofstream file(path); file << "private target"; }
    SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_MENU_INVOKE", output.c_str());
    ext::Request request; request.paths = {path.wstring()};
    ext::Reply cached; cached.ok = true;
    ext::Entry entry; entry.label = L"Cached command"; entry.key = "cached"; entry.provider = "verb:cached"; entry.token = 999;
    cached.entries = {entry};
    const auto cachePath = directory.path / L"cache";
    ext::MenuSnapshotCache cache(cachePath);
    Expect(cache.Store(cache.Capture(request), cached), "seed a private disk snapshot");
    {
        ext::MenuService service(cachePath);
        PumpUntil([&] { return service.View(request).snapshot.has_value(); }, "startup restores the exact selection from disk on the service worker");
        ext::Preferences prefs; ext::SetHidden(prefs, "verb:cached", ext::Context::File, false);
        ext::Presentation presentation(request, prefs, L"", L"", service);
        std::vector<menu::Item> items; menu::Options options; presentation.Attach(items, options, 0);
        Expect(items.size() == 1, "disk-restored snapshot is immediately available to popup");
        PumpUntil([&] { return !service.View(request).error.empty(); }, "controlled failed refresh completes");
        Expect(service.View(request).snapshot.has_value() && !options.pollItems, "failed query preserves both snapshot and frozen popup");
        Expect(presentation.Invoke(items.front().command, {0, 0}), "explicit cached click bypasses automatic backoff once");
        PumpUntil([&] { return std::filesystem::exists(output); }, "click reaches the supervised child's real invocation transport");
        UINT token = 0; { std::ifstream file(output); file >> token; }
        Expect(token == 72, "execute only the fresh session's token, never cached token 999");
    }
    SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_MENU_INVOKE", nullptr);
}
void TestQueryScheduler()
{
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory directory;
    struct Gate { std::atomic<bool> ready = false; ext::Reply reply; };
    std::mutex mutex;
    std::vector<std::shared_ptr<Gate>> gates;
    std::atomic<int> active = 0, maximum = 0, starts = 0, invokes = 0;
    std::atomic<bool> failStart = false;
    auto factory = [&](const ext::Request &) -> ext::QueryWork {
        ++starts;
        if (failStart) throw std::runtime_error("controlled startup failure");
        auto gate = std::make_shared<Gate>();
        auto lease = std::shared_ptr<int>(new int, [&](int *p) { --active; delete p; });
        const auto count = ++active; maximum.store(std::max(maximum.load(), count));
        gate->reply.ok = true; ext::Entry entry; entry.provider = "verb:test"; entry.key = "test"; entry.label = L"Test"; entry.token = 42;
        gate->reply.entries = {entry};
        { std::lock_guard lock(mutex); gates.push_back(gate); }
        return {[gate, lease]() -> std::optional<ext::Reply> { if (!gate->ready) return {}; return gate->reply; }, [&](UINT token, POINT) { if (token == 42) ++invokes; }};
    };
    ext::MenuService service(directory.path / L"cache", factory, [] { ext::Catalogue c; c.revision = 1; return c; });
    const auto path = directory.path / L"a.txt"; { std::ofstream f(path); f << "a"; }
    ext::Request request; request.paths = {path.wstring()};
    service.Query(request); service.Query(request, ext::QueryPriority::Inspect);
    PumpUntil([&] { return starts == 1; }, "one real selection creates one shared task");
    std::shared_ptr<Gate> first;
    { std::lock_guard lock(mutex); first = gates.front(); }
    first->ready = true;
    PumpUntil([&] { return service.View(request).snapshot.has_value(); }, "publish complete successful query");
    const auto snapshot = service.View(request).snapshot;
    Expect(snapshot->entries.front().token == 0, "published snapshots never contain executable tokens");
    failStart = true;
    service.Query(request, ext::QueryPriority::Menu, true);
    PumpUntil([&] { return !service.View(request).error.empty(); }, "controlled launch failure reaches the scheduler");
    Expect(service.View(request).snapshot.has_value(), "launch failure does not clear a valid memory snapshot");
    const int failedStarts = starts;
    for (int i = 0; i < 20; ++i) service.Query(request);
    Expect(starts == failedStarts && !service.View(request).pending, "automatic failures back off without spawning repeated helpers");
    failStart = false;
    service.Query(request, ext::QueryPriority::Menu, true);
    PumpUntil([&] { return starts > failedStarts; }, "explicit refresh can bypass backoff");
    std::shared_ptr<Gate> stale;
    { std::lock_guard lock(mutex); stale = gates.back(); }
    service.Invalidate(request);
    stale->reply.entries.front().label = L"stale"; stale->ready = true;
    PumpUntil([&] { return starts > failedStarts + 1; }, "invalidated completion schedules a newer dependency query");
    Expect(!service.View(request).snapshot, "late result from an old dependency cannot restore invalidated menu");
    std::shared_ptr<Gate> newest;
    { std::lock_guard lock(mutex); newest = gates.back(); }
    newest->reply.entries.clear(); newest->ready = true;
    PumpUntil([&] { return service.View(request).snapshot.has_value(); }, "a successful empty result is publishable");
    Expect(service.View(request).snapshot->entries.empty(), "valid empty menu replaces an older nonempty menu");
    // Hold two distinct requests and verify the third remains queued.
    std::vector<ext::Request> requests;
    for (int i = 0; i < 3; ++i)
    {
        auto r = request; r.paths = {(directory.path / (std::to_wstring(i) + L".txt")).wstring()};
        { std::ofstream f(r.paths.front()); f << i; } requests.push_back(r); service.Query(r);
    }
    PumpUntil([&] { std::lock_guard lock(mutex); return std::count_if(gates.begin(), gates.end(), [](const auto &g) { return !g->ready; }) == 2; }, "two queries run concurrently");
    {
        std::lock_guard lock(mutex);
        for (auto &gate : gates) gate->ready = true;
    }
    PumpUntil([&] { return service.View(requests[0]).snapshot && service.View(requests[1]).snapshot; }, "completed workers drain the queue");
    PumpUntil([&] { std::lock_guard lock(mutex); for (auto &gate : gates) gate->ready = true; return service.View(requests[2]).snapshot.has_value(); }, "queued third query completes");
    std::atomic<bool> rejected = false;
    ext::Entry clicked; clicked.provider = "verb:test"; clicked.key = "test"; clicked.label = L"Test";
    service.Execute(requests[2], ext::AppendReference({}, clicked), {}, [&](bool ok) { rejected = !ok; });
    PumpUntil([&] { std::lock_guard lock(mutex); return !gates.back()->ready; }, "explicit click starts a fresh command query");
    service.Configure({}); // User hides extensions while the execution query is pending.
    { std::lock_guard lock(mutex); gates.back()->ready = true; }
    PumpUntil([&] { return rejected.load(); }, "latest visibility rules reject a pending click after disable");
    Expect(invokes == 0, "disabled pending command is never invoked");
    service.Shutdown();
    Expect(maximum <= 2, "scheduler never runs more than two query workers");
}

void TestSelectionScopes()
{
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory temp;
    const auto a = temp.path / L"a.txt", b = temp.path / L"b.txt", c = temp.path / L"c.png";
    const auto d = temp.path / L"folder1", e = temp.path / L"folder2";
    for (const auto &path : {a, b, c}) { std::ofstream out(path); out << "fixture"; }
    std::filesystem::create_directory(d); std::filesystem::create_directory(e);
    const std::vector<std::vector<std::wstring>> selections = {
        {a.wstring()}, {a.wstring(), b.wstring()}, {a.wstring(), c.wstring()},
        {d.wstring()}, {d.wstring(), e.wstring()}, {a.wstring(), d.wstring()}};
    const unsigned expectedContexts[] = {1, 1, 1, 2, 2, 3};
    std::vector<ext::Request> requests;
    for (bool shift : {false, true}) for (const auto &paths : selections)
    { ext::Request request; request.paths = paths; request.extended = shift; requests.push_back(request); }
    std::atomic<int> starts = 0;
    ext::MenuService service(temp.path / L"cache", [&](const ext::Request &request) {
        ++starts;
        const auto found = std::find_if(requests.begin(), requests.end(), [&](const auto &r) { return r.paths == request.paths && r.extended == request.extended; });
        Expect(found != requests.end(), "query boundary receives the complete selection and Shift state");
        ext::Reply reply; reply.ok = true; ext::Entry entry; entry.provider = "provider"; entry.key = "action";
        entry.label = std::to_wstring(std::distance(requests.begin(), found)); entry.token = 17; reply.entries = {entry};
        return ext::QueryWork{[reply] { return reply; }, [](UINT, POINT) {}};
    });
    for (const auto &request : requests) service.Query(request);
    PumpUntil([&] { return std::all_of(requests.begin(), requests.end(), [&](const auto &r) { return service.View(r).snapshot.has_value(); }); }, "all single and mixed selections publish independently");
    ext::Preferences preferences; ext::SetCommon(preferences, "provider", ext::Category::Objects, true);
    ext::SetOverride(preferences, "provider", ext::Context::Folder, ext::Visibility::Hide);
    for (size_t i = 0; i < requests.size(); ++i)
    {
        const auto view = service.View(requests[i]);
        Expect(view.contexts == expectedContexts[i % 6] && view.snapshot->entries.front().label == std::to_wstring(i), "different selections and Shift never borrow a snapshot");
        Expect(ext::VisibleSnapshot(preferences, *view.snapshot, view.contexts).empty() == (i % 6 >= 3), "folder hiding wins for folder-only and mixed file-folder selections");
        service.Query(requests[i], ext::QueryPriority::Inspect);
    }
    Expect(starts == 12, "fresh settings and popup access reuse all twelve exact snapshots");
}

void TestCatalogueDependencies()
{
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory temp;
    const auto path = temp.path / L"a.txt"; { std::ofstream out(path); out << "fixture"; }
    ext::Request request; request.paths = {path.wstring()};
    std::atomic<int> scanNumber = 0, mode = 0;
    auto reader = [&] {
        ext::Catalogue catalogue; catalogue.revision = 10 + mode.load();
        ext::Registration text; text.id = "text"; text.contexts = 1; text.types = {L".txt"}; text.revision = mode == 2 ? 2 : 1;
        ext::Registration image; image.id = "image"; image.contexts = 1; image.types = {L".png"}; image.revision = mode >= 1 ? 2 : 1;
        catalogue.rows = {text, image}; ++scanNumber; return catalogue;
    };
    ext::MenuService service(temp.path / L"cache", [](const auto &) {
        ext::Reply reply; reply.ok = true; return ext::QueryWork{[reply] { return reply; }, [](UINT, POINT) {}};
    }, reader);
    service.Inspect({}, true);
    PumpUntil([&] { return scanNumber >= 1 && !service.Inspect().scanning; }, "initial dependency catalogue is ready");
    service.Query(request);
    PumpUntil([&] { return service.View(request).snapshot.has_value(); }, "seed an exact text selection");
    const auto revision = service.View(request).revision;
    mode = 1; service.Inspect({}, true);
    PumpUntil([&] { return scanNumber >= 2 && !service.Inspect().scanning; }, "unrelated registration scan completes");
    Expect(service.View(request).snapshot && service.View(request).revision == revision, "an image-only registry change retains the text snapshot");
    mode = 2; service.Inspect({}, true);
    PumpUntil([&] { return scanNumber >= 3 && !service.Inspect().scanning; }, "relevant registration scan completes");
    Expect(!service.View(request).snapshot, "a relevant registration change invalidates its dependent snapshot");
}

void TestUsefulManagementItems()
{
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory temp;
    std::atomic<bool> release = false;
    std::atomic<int> queries = 0;
    auto registry = [] {
        ext::Catalogue catalogue; catalogue.revision = 1;
        for (int i = 0; i < 3000; ++i)
        {
            ext::Registration row; row.id = "reg:unused-" + std::to_string(i); row.contexts = 15;
            row.display.label = L"Duplicate registry label"; catalogue.rows.push_back(row);
        }
        return catalogue;
    };
    ext::MenuService service(temp.path / L"cache", [&](const auto &) {
        ++queries;
        return ext::QueryWork{[&]() -> std::optional<ext::Reply> {
            if (!release) return {};
            ext::Reply reply; reply.ok = true;
            ext::Entry first; first.provider = "menu:archive:one"; first.label = L"Archive";
            first.width = first.height = 1; first.pixels = {20, 40, 60, 255};
            ext::Entry child; child.label = L"Compress"; child.key = "compress"; child.token = 17;
            first.children = {child};
            auto second = first; second.provider = "menu:archive:two";
            auto disabled = first; disabled.provider = "disabled"; disabled.enabled = false;
            auto placeholder = first; placeholder.provider = "placeholder"; placeholder.label = L"…";
            reply.entries = {first, second, disabled, placeholder};
            return reply;
        }, [](UINT, POINT) {}};
    }, registry);
    const auto cold = service.Inspect();
    Expect(cold.catalogue.rows.empty() && cold.scanning, "uncached settings return immediately and start background discovery");
    PumpUntil([&] { return queries >= 2; }, "uncached settings actively query real baseline objects without requiring opt-ins");
    const auto pending = service.Inspect();
    Expect(pending.catalogue.rows.empty(), "three thousand unassociated registry records never become disabled settings controls");
    release = true;
    PumpUntil([&] { const auto view = service.Inspect(); return !view.scanning; }, "settings dynamically receive useful Shell roots on first access");
    const auto loaded = service.Inspect();
    Expect(loaded.catalogue.rows.size() == 2, "only the two actionable observed identities reach settings");
    Expect(queries == 4, "default discovery is bounded to four real object/context queries");
    Expect(loaded.catalogue.rows[0].id != loaded.catalogue.rows[1].id, "same-name actual providers are not merged by label");
    for (const auto &row : loaded.catalogue.rows)
    {
        Expect(row.linked && row.systemEnabled && row.contexts == 15, "same identity merges observed scopes into an actionable switch");
        Expect(row.display.children.empty() && !row.display.token && row.display.pixels.size() == 4, "settings carry only root labels and icons, not every dynamic child or token");
        ext::Preferences prefs; ext::SetCommon(prefs, row.id, ext::Category::Objects, true);
        ext::Reply actual; actual.ok = true; actual.entries = {row.display};
        Expect(ext::VisibleSnapshot(prefs, actual, 3).size() == 1, "a displayed unassociated provider switch controls the production visibility path");
    }
    Expect(snowdesktop::settings_ipc::Pack(loaded).size() < 8192, "settings IPC payload does not scale with the raw registry inventory");
    std::cout << "Management projection: registry=3000, rows=" << loaded.catalogue.rows.size()
              << ", bytes=" << snowdesktop::settings_ipc::Pack(loaded).size() << '\n';

    service.Inspect({}, true);
    PumpUntil([&] { return !service.Inspect().scanning; }, "explicit refresh completes baseline discovery");
    Expect(queries == 8, "explicit settings refresh queries all four locations despite the fresh-cache throttle");

    ext::Request previous; previous.paths = {(temp.path / L"previous.txt").wstring()};
    service.Manage(previous);
    ext::Request desktop; desktop.context = ext::Context::Desktop; desktop.background = true;
    const auto switching = service.Inspect(desktop);
    Expect(switching.selection.context == ext::Context::Desktop,
           "desktop inspection never routes settings back to the previous file while resolving its path");
    PumpUntil([&] {
        const auto selected = service.Inspect(desktop).selection;
        return selected.context == ext::Context::Desktop && !selected.paths.empty();
    }, "desktop inspection resolves its real path in the background");
}

void BenchmarkManagement()
{
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory temp;
    SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU", L"1");
    ext::MenuService service(temp.path / L"cache");
    const auto started = std::chrono::steady_clock::now();
    const auto cold = service.Inspect();
    std::cout << "management first_ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()
              << " rows=" << cold.catalogue.rows.size() << std::endl;
    const auto deadline = GetTickCount64() + 30000;
    ext::CatalogueView ready;
    do
    {
        ready = service.Inspect();
        if (!ready.scanning) break;
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
    } while (GetTickCount64() < deadline);
    Expect(!ready.scanning && !ready.catalogue.rows.empty(), "real settings discovery produces actionable rows");
    std::cout << "management complete_ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()
              << " rows=" << ready.catalogue.rows.size() << " bytes=" << snowdesktop::settings_ipc::Pack(ready).size() << std::endl;
    for (int i = 0; i < 30; ++i)
    {
        const auto start = std::chrono::steady_clock::now();
        const auto view = service.Inspect();
        std::cout << "management warm_ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
                  << " rows=" << view.catalogue.rows.size() << std::endl;
    }
    SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU", nullptr);
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
        for (int iteration = 0; iteration < 35; ++iteration)
        {
            if (iteration < 5) ext::Session::ReleaseIdleWorker();
            const auto start = GetTickCount64();
            const auto diskStart=std::chrono::steady_clock::now();
            ext::MenuSnapshotCache disk(ext::SharedMenuCache().Directory());
            const auto ticket=disk.Capture(request);
            const auto snapshot=disk.Find(ticket);
            const auto diskMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-diskStart).count();
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
                      << " mode=" << (iteration < 5 ? "cold" : "warm")
                      << " iteration=" << iteration << " snapshot=" << bool(snapshot) << " disk_ms=" << diskMs
                      << " start_ms=" << launched - start
                      << " ready_ms=" << elapsed << " ok=" << (reply && reply->ok)
                      << " worker=" << session.ProcessId()
                      << " entries=" << (reply ? reply->entries.size() : 0) << std::endl;
            Expect(reply && reply->ok, "benchmark real menu query succeeds");
            Expect(disk.Store(ticket,*reply),"benchmark stores the fresh display snapshot");
        }
    }
    for (const bool folder : {false, true})
    {
        ext::Request request; request.paths = {(folder ? directory.path : file).wstring()};
        std::unique_ptr<ext::MenuService> service;
        for (int iteration = 0; iteration < 35; ++iteration)
        {
            if (iteration < 5)
                service = std::make_unique<ext::MenuService>(directory.path / (L"service-" + std::to_wstring(folder) + L"-" + std::to_wstring(iteration)));
            const auto start = std::chrono::steady_clock::now();
            const auto before = service->View(request);
            const auto firstMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            service->Query(request, ext::QueryPriority::Menu, true);
            PumpUntil([&] { const auto view = service->View(request); return !view.pending && view.snapshot && view.revision > before.revision; }, "shared service benchmark query completes");
            const auto queryMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            const auto hot = std::chrono::steady_clock::now();
            const auto snapshot = service->View(request);
            const auto hitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - hot).count();
            ext::Preferences visible;
            for (const auto &entry : snapshot.snapshot->entries)
                if (!entry.separator) ext::SetHidden(visible, entry.provider, folder ? ext::Context::Folder : ext::Context::File, false);
            const auto prepare = std::chrono::steady_clock::now();
            ext::Presentation popup(request, visible, L"", L"", *service);
            snowdesktop::modern_menu::Item more; more.command = 7; more.label = L"More";
            std::vector<snowdesktop::modern_menu::Item> items{more}; snowdesktop::modern_menu::Options options;
            popup.Attach(items, options, 7);
            const auto prepareMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepare).count();
            std::cout << "service_benchmark scope=" << (folder ? "folder" : "file")
                      << " mode=" << (iteration < 5 ? "cold" : "warm") << " iteration=" << iteration
                      << " first_ms=" << firstMs << " memory_ms=" << hitMs << " prepare_ms=" << prepareMs << " query_ms=" << queryMs
                      << " entries=" << snapshot.snapshot->entries.size() << std::endl;
        }
    }
    const auto catalogueStart = std::chrono::steady_clock::now();
    const auto catalogue = ext::ReadCatalogue();
    std::cout << "catalogue_benchmark rows=" << catalogue.rows.size()
              << " enabled=" << std::count_if(catalogue.rows.begin(), catalogue.rows.end(), [](const auto &r) { return r.systemEnabled; })
              << " ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - catalogueStart).count() << std::endl;
    // Measure the real scheduler/command resolver with a harmless boundary
    // replacement. No installed third-party action is executed by this probe.
    ext::Reply harmless; harmless.ok = true;
    ext::Entry command; command.provider = "benchmark"; command.key = "noop"; command.label = L"No-op"; command.token = 42;
    harmless.entries = {command};
    ext::MenuService clickService(directory.path / L"click-benchmark", [harmless](const auto &) {
        return ext::QueryWork{[harmless] { return harmless; }, [](UINT, POINT) {}};
    });
    ext::Request clickRequest; clickRequest.paths = {file.wstring()};
    for (int i = 0; i < 30; ++i)
    {
        std::atomic<bool> completed = false, succeeded = false;
        const auto start = std::chrono::steady_clock::now();
        clickService.Execute(clickRequest, ext::AppendReference({}, command), {}, [&](bool ok) { succeeded = ok; completed = true; });
        PumpUntil([&] { return completed.load(); }, "harmless click dispatch completes");
        Expect(succeeded, "harmless click resolves the current command");
        std::cout << "click_benchmark boundary=controlled iteration=" << i << " ms="
                  << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() << std::endl;
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
        if(request.paths.size()==3&&request.paths[0]==L"read-disk-cache")
        {
            snowdesktop::shell_extensions::MenuSnapshotCache cache(request.paths[1]);
            snowdesktop::shell_extensions::Request file;file.paths={request.paths[2]};
            return cache.Find(cache.Capture(file),101).value_or(snowdesktop::shell_extensions::Reply{});
        }
        snowdesktop::shell_extensions::Reply reply; reply.ok = true;
        snowdesktop::shell_extensions::Entry entry; entry.label=L"压缩";entry.checked=true;entry.enabled=false;
        entry.key=request.paths.front()==L"synthetic-second" ? "second" : "first";
        if (request.paths.front() == L"synthetic-stdio")
        {
            DWORD written = 0;
            const bool isolated = GetFileType(GetStdHandle(STD_INPUT_HANDLE)) == FILE_TYPE_CHAR &&
                                  GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_CHAR &&
                                  GetFileType(GetStdHandle(STD_ERROR_HANDLE)) == FILE_TYPE_CHAR;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "x", 1, &written, nullptr);
            entry.key = isolated && written == 1 ? "isolated-stdio" : "inherited-stdio";
        }
        if (request.catalogueOnly) entry.label=request.paths.front();
        entry.provider = "verb:" + entry.key;
        reply.entries.push_back(entry);
        if(std::filesystem::path(request.paths.front()).filename()==L"presentation.txt")
        {
            entry.label = L"保持选中";
            entry.key = "stable";
            entry.provider = "verb:stable";
            reply.entries.push_back(entry);
        }
        return reply;
    };
    wchar_t invocationPath[32768]{};
    const bool record=GetEnvironmentVariableW(L"SNOWDESKTOP_TEST_MENU_INVOKE",invocationPath,32768)!=0;
    snowdesktop::shell_extensions::InvokeExecutor invoke;
    if(record)
    {
        query = [](const auto &request) {
            const auto path = std::filesystem::path(request.paths.front());
            const auto failureMarker = path.parent_path() / L"retry-failed-once";
            // Failed helpers are deliberately discarded. Keep the one-shot
            // fault in this test's private directory so a new helper can retry.
            if (path.filename() == L"retry.txt" && !std::filesystem::exists(failureMarker))
            {
                std::ofstream marker(failureMarker);
                marker << "failed once";
                return snowdesktop::shell_extensions::Reply{false, {}, "transient query failure"};
            }
            snowdesktop::shell_extensions::Reply reply;
            reply.ok = true;
            snowdesktop::shell_extensions::Entry entry;
            entry.label = L"Cached command";
            entry.key = "cached";
            entry.provider = "verb:cached";
            entry.token = 72;
            reply.entries={entry};
            return reply;
        };
        invoke=[path=std::filesystem::path(invocationPath)](UINT token,POINT){std::ofstream file(path);file<<token;};
    }
    if (const auto helper = snowdesktop::shell_extensions::TryRunHelper(std::move(query),std::move(invoke))) return *helper;

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 1;
    try
    {
        TemporaryDirectory cacheDirectory;
        snowdesktop::shell_extensions::SharedMenuCache()=snowdesktop::shell_extensions::MenuSnapshotCache(cacheDirectory.path/L"shared");
        if (argc == 2 && std::wstring_view(argv[1]) == L"--benchmark-menu-settings") BenchmarkManagement();
        else if (argc == 2 && std::wstring_view(argv[1]) == L"--benchmark-shell-menu") BenchmarkMenus();
        else
        {
            RunTests();
            TestDeferredPopups();
            TestCatalogueCache();
            TestRegistryCatalogue();
            TestExtensionSessions();
            TestSnapshotPresentation();
            TestPendingCachedClick();
            TestQueryScheduler();
            TestSelectionScopes();
            TestCatalogueDependencies();
            TestUsefulManagementItems();
        }
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

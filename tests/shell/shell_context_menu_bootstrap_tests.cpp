#include "shell/shell_context_menu_site.h"
#include "shell/shell_extension_menu.h"
#include "shell/shell_extension_discovery.h"
#include <shlguid.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <memory>
#include <stdexcept>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <chrono>

namespace
{
using Microsoft::WRL::ComPtr;
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
bool SingleChildPidl(PCUITEMID_CHILD child)
{
    return child && child->mkid.cb && ILNext(child)->mkid.cb == 0;
}
void BindProbeSelection(PCIDLIST_ABSOLUTE pidl, const std::filesystem::path& expected,
                        ComPtr<IShellFolder>& parent, PCUITEMID_CHILD& child)
{
    Require(SUCCEEDED(SHBindToParent(pidl, IID_PPV_ARGS(&parent), &child)) && SingleChildPidl(child),
        "GetUIObjectOf receives exactly one child relative to its actual parent");
    ComPtr<IShellItem> selected;
    Require(SUCCEEDED(SHCreateItemWithParent(nullptr, parent.Get(), child, IID_PPV_ARGS(&selected))), "recreate the bound sample item");
    PWSTR path = nullptr;
    Require(SUCCEEDED(selected->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path, "bound sample has a filesystem identity");
    std::error_code error;
    const bool same = std::filesystem::equivalent(expected, std::filesystem::path(path), error);
    CoTaskMemFree(path);
    Require(!error && same, "the bound menu selection is the exact sample, not an ancestor folder or drive");
}

// The provider deliberately rejects repeat binds and repeat queries. A partial
// first-use menu remains the live menu, including its original command offsets.
struct State
{
    unsigned bindings = 0, queries = 0, alive = 0, invocations = 0;
    UINT first = 7, last = 103, flags = CMF_ITEMMENU | CMF_CANRENAME | CMF_EXTENDEDVERBS;
    int failure = 0;
    bool background = false, namespaceOnly = false;
    HWND owner = nullptr;
};
class Menu final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IContextMenu, IObjectWithSite>
{
public:
    explicit Menu(std::shared_ptr<State> state) : state_(std::move(state)) { ++state_->alive; }
    ~Menu() { --state_->alive; }
    IFACEMETHODIMP QueryContextMenu(HMENU menu, UINT index, UINT first, UINT last, UINT flags) override
    {
        if (++state_->queries != 1 || first != state_->first || last != state_->last ||
            flags != state_->flags || index != 0) return E_UNEXPECTED;
        if (state_->failure == 2) return E_ACCESSDENIED;
        if (!state_->namespaceOnly)
        {
            ComPtr<IServiceProvider> services;
            ComPtr<IFolderView2> view;
            ComPtr<IShellItemArray> selection;
            DWORD count = 0;
            if (!site_ || FAILED(site_.As(&services)) ||
                FAILED(services->QueryService(SID_SFolderView, IID_PPV_ARGS(&view)))) return E_UNEXPECTED;
            if (state_->background)
            {
                if (view->GetSelection(FALSE, &selection) != HRESULT_FROM_WIN32(ERROR_NOT_FOUND) || selection ||
                    FAILED(view->GetSelection(TRUE, &selection)) || FAILED(selection->GetCount(&count)) || count != 1)
                    return E_UNEXPECTED;
            }
            else
            {
                if (FAILED(view->GetSelection(FALSE, &selection)) || FAILED(selection->GetCount(&count)) || count != 1)
                    return E_UNEXPECTED;
                ComPtr<IDataObject> data;
                if (FAILED(view->Items(SVGIO_SELECTION, IID_PPV_ARGS(&data)))) return E_UNEXPECTED;
            }
            ComPtr<IShellItemArray> parent;
            if (FAILED(view->GetFolder(IID_PPV_ARGS(&parent))) || FAILED(parent->GetCount(&count)) || count != 1)
                return E_UNEXPECTED;
        }
        else if (site_) return E_UNEXPECTED;
        return InsertMenuW(menu, 0, MF_BYPOSITION | MF_STRING, first + 11, L"First-use command") ?
            MAKE_HRESULT(SEVERITY_SUCCESS, 0, 12) : E_FAIL;
    }
    IFACEMETHODIMP InvokeCommand(LPCMINVOKECOMMANDINFO info) override
    {
        if (state_->queries != 1 || reinterpret_cast<ULONG_PTR>(info->lpVerb) != 11) return E_UNEXPECTED;
        ++state_->invocations; return S_OK;
    }
    IFACEMETHODIMP GetCommandString(UINT_PTR, UINT, UINT*, LPSTR, UINT) override { return E_NOTIMPL; }
    IFACEMETHODIMP SetSite(IUnknown* site) override { site_ = site; return S_OK; }
    IFACEMETHODIMP GetSite(REFIID iid, void** output) override { return site_ ? site_->QueryInterface(iid, output) : E_FAIL; }
private:
    std::shared_ptr<State> state_;
    ComPtr<IUnknown> site_;
};
void RunSingleQueryCase(IShellFolder* folder, PCUITEMID_CHILD child, const std::wstring& sample,
    bool background, bool namespaceOnly, int failure)
{
    auto state = std::make_shared<State>();
    state->background = background; state->namespaceOnly = namespaceOnly; state->failure = failure;
    state->flags = background ? UINT(CMF_EXPLORE | CMF_EXTENDEDVERBS) : UINT(CMF_ITEMMENU | CMF_CANRENAME | CMF_EXTENDEDVERBS);
    state->owner = CreateWindowExW(0, L"STATIC", L"Private single-query owner", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const HMENU popup = CreatePopupMenu();
    Require(state->owner && popup, "single-query owner and menu");
    {
        snowdesktop::ShellContextMenuSite site;
        ComPtr<IContextMenu> actual;
        const auto bind = [&](HWND owner, IContextMenu** result) -> HRESULT {
            if (++state->bindings != 1 || owner != state->owner) return E_UNEXPECTED;
            if (failure == 1) return E_ACCESSDENIED;
            return Microsoft::WRL::Make<Menu>(state).CopyTo(result);
        };
        PCUITEMID_CHILD ownedChild = child ? ILCloneFull(child) : nullptr;
        const HRESULT hr = namespaceOnly
            ? site.BuildNamespaceMenu(state->owner, popup, state->first, state->last, state->flags, bind, actual.GetAddressOf())
            : background
                ? site.BuildMenu(folder, state->owner, popup, state->first, state->last, state->flags, bind, actual.GetAddressOf())
                : site.BuildSelectionMenu(folder, state->owner, popup, state->first, state->last, state->flags,
                    bind, 1, &ownedChild, actual.GetAddressOf());
        ILFree(const_cast<PITEMID_CHILD>(ownedChild));
        Require(state->bindings == 1 && state->queries == (failure == 1 ? 0u : 1u),
            "cold, warm and failed menus never bind/query a second aggregate");
        Require(!site.HostWindow(), "single-query sites never create an enumerating Windows folder view");
        if (failure)
            Require(hr == E_ACCESSDENIED && !actual, "failed binding/query stops without a retry");
        else
        {
            Require(SUCCEEDED(hr) && actual && GetMenuItemCount(popup) == 1 &&
                GetMenuItemID(popup, 0) == state->first + 11, "the first partial menu is retained with its original IDs");
            CMINVOKECOMMANDINFO info{sizeof(info)}; info.lpVerb = MAKEINTRESOURCEA(11);
            Require(SUCCEEDED(actual->InvokeCommand(&info)) && state->invocations == 1,
                "single queried menu invokes the same command exactly once");
            if (!background && !namespaceOnly)
            {
                ComPtr<IObjectWithSite> attached; actual.As(&attached);
                ComPtr<IServiceProvider> services;
                Require(SUCCEEDED(attached->GetSite(IID_PPV_ARGS(&services))), "menu retains its site");
                ComPtr<IFolderView2> view;
                Require(SUCCEEDED(services->QueryService(SID_SFolderView, IID_PPV_ARGS(&view))), "retained selection view");
                ComPtr<IShellItem> item;
                Require(SUCCEEDED(view->GetItem(0, IID_PPV_ARGS(&item))), "selection outlives caller PIDLs");
                PWSTR name = nullptr;
                Require(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)), "retained file identity");
                const bool same = std::filesystem::equivalent(sample, name);
                CoTaskMemFree(name);
                Require(same, "the menu retains the exact original selection");
                int index = -1;
                Require(view->GetSelectedItem(0, &index) == S_OK && index == 0 &&
                    view->GetSelectedItem(1, &index) == S_FALSE && index == -1, "selected-item bounds remain valid");
                ComPtr<IEnumIDList> enumeration;
                Require(SUCCEEDED(view->Items(SVGIO_SELECTION, IID_PPV_ARGS(&enumeration))), "selection PIDL enumeration");
                PITEMID_CHILD enumerated = nullptr;
                Require(enumeration->Next(1, &enumerated, nullptr) == S_OK && ILIsEqual(enumerated, child), "exact relative child PIDL");
                ILFree(enumerated);
                Require(enumeration->Next(1, &enumerated, nullptr) == S_FALSE, "enumeration stops at the selection boundary");
            }
        }
    }
    Require(state->alive == 0 && IsWindow(state->owner), "menu/site release does not destroy its caller window");
    DestroyMenu(popup); DestroyWindow(state->owner);
}
} // namespace

void TestShellContextMenuBootstrap()
{
    snowdesktop::ShellMenuInitializationFile file;
    Require(file.handle != INVALID_HANDLE_VALUE, "private selection sample");
    PIDLIST_ABSOLUTE raw = nullptr;
    Require(SUCCEEDED(SHParseDisplayName(file.path.c_str(), nullptr, &raw, 0, nullptr)), "sample PIDL");
    ComPtr<IShellFolder> parent; PCUITEMID_CHILD child = nullptr;
    Require(!SingleChildPidl(reinterpret_cast<PCUITEMID_CHILD>(raw)), "absolute PIDLs cannot substitute for relative children");
    BindProbeSelection(raw, file.path, parent, child);
    for (int failure = 0; failure <= 2; ++failure)
    {
        RunSingleQueryCase(parent.Get(), child, file.path, false, false, failure);
        RunSingleQueryCase(parent.Get(), nullptr, file.path, true, false, failure);
        RunSingleQueryCase(parent.Get(), nullptr, file.path, false, true, failure);
    }
    const auto snapshot = snowdesktop::shell_extensions::NativeMenuSelection::Capture(parent.Get(), 1, &child);
    const auto roundtrip = snowdesktop::settings_ipc::Unpack<snowdesktop::shell_extensions::NativeMenuSelection>(
        snowdesktop::settings_ipc::Pack(snapshot));
    Require(snapshot.parent == roundtrip.parent && snapshot.children == roundtrip.children,
        "real parent and relative children survive the native process boundary");
    const auto linkPath = std::filesystem::path(file.path + L".lnk");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); } } cleanup{linkPath};
    ComPtr<IShellLinkW> link; ComPtr<IPersistFile> persisted;
    Require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
        SUCCEEDED(link->SetPath(file.path.c_str())) && SUCCEEDED(link.As(&persisted)) &&
        SUCCEEDED(persisted->Save(linkPath.c_str(), TRUE)), "valid private shortcut");
    for (const auto& target : {linkPath, std::filesystem::path(file.path).parent_path()})
    {
        PIDLIST_ABSOLUTE selected = nullptr;
        Require(SUCCEEDED(SHParseDisplayName(target.c_str(), nullptr, &selected, 0, nullptr)), "alternate selection PIDL");
        ComPtr<IShellFolder> selectedParent; PCUITEMID_CHILD selectedChild = nullptr;
        BindProbeSelection(selected, target, selectedParent, selectedChild);
        RunSingleQueryCase(selectedParent.Get(), selectedChild, target.wstring(), false, false, 0);
        CoTaskMemFree(selected);
    }
    CoTaskMemFree(raw);
}

// Read-only comparison of the production single-query builders. Legacy mode
// names select cascade flags only; there are no file primers or retry passes. No desktop host, TrackPopupMenu or command invocation is involved.
void BenchmarkNativeMenuPreparation()
{
    wchar_t modeText[32]{}, extensionText[16]{};
    GetEnvironmentVariableW(L"SNOWDESKTOP_NATIVE_MENU_MODE", modeText, 32);
    GetEnvironmentVariableW(L"SNOWDESKTOP_NATIVE_MENU_EXTENSION", extensionText, 16);
    const std::wstring mode = *modeText ? modeText : L"optimized";
    const std::wstring extension = *extensionText ? extensionText : L".txt";
    Require(mode == L"baseline" || mode == L"no-primer" || mode == L"no-sync" || mode == L"optimized" || mode == L"selection", "known native benchmark mode");
    const auto bytes = snowdesktop::shell_extensions::DiscoverySample(extension);
    Require(!bytes.empty(), "native benchmark uses a valid supported private file");
    GUID guid{}; Require(SUCCEEDED(CoCreateGuid(&guid)), "native benchmark identity");
    wchar_t identifier[40]{}; StringFromGUID2(guid, identifier, 40);
    const auto directory = std::filesystem::temp_directory_path() / (std::wstring(L"SnowDesktop-NativeTiming-") + identifier);
    Require(std::filesystem::create_directory(directory), "own native benchmark directory");
    struct Workspace { std::filesystem::path path; ~Workspace() { std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{directory};
    const auto file = directory / (L"Sample" + extension);
    { std::ofstream output(file, std::ios::binary); output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); }
    PIDLIST_ABSOLUTE raw = nullptr;
    Require(SUCCEEDED(SHParseDisplayName(file.c_str(), nullptr, &raw, 0, nullptr)), "native benchmark selection PIDL");
    struct Pidl { PIDLIST_ABSOLUTE value; ~Pidl() { CoTaskMemFree(value); } } pidl{raw};
    struct Owner { HWND value; ~Owner() { if (value) DestroyWindow(value); } } owner{
        CreateWindowExW(0, L"STATIC", L"Private native timing owner", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)};
    Require(owner.value != nullptr, "native benchmark hidden owner");
    ComPtr<IShellFolder> folder; PCUITEMID_CHILD child = nullptr;
    BindProbeSelection(raw, file, folder, child);
    const auto bind = [&](HWND window, IContextMenu** menu) {
        return folder->GetUIObjectOf(window, 1, &child, IID_IContextMenu, nullptr, reinterpret_cast<void**>(menu));
    };
    const bool synchronous = mode == L"baseline" || mode == L"no-primer";
    const UINT flags = CMF_NORMAL | CMF_CANRENAME | CMF_EXTENDEDVERBS | (synchronous ? CMF_SYNCCASCADEMENU : 0);
    const auto utf8 = [](std::wstring_view value) {
        const auto size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        std::string result(size, 0);
        if (size) WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
        return result;
    };
    for (unsigned iteration = 0; iteration < 2; ++iteration)
    {
    const auto started = std::chrono::steady_clock::now();
    constexpr bool filePrimer = false;
    struct Popup { HMENU value = CreatePopupMenu(); ~Popup() { if (value) DestroyMenu(value); } } popup;
    Require(popup.value != nullptr, "native benchmark popup");
    snowdesktop::ShellContextMenuSite site;
    ComPtr<IContextMenu> context;
    const HRESULT result = mode == L"selection"
        ? site.BuildSelectionMenu(folder.Get(), owner.value, popup.value, 1, 0x7fff, flags, bind, 1, &child, context.GetAddressOf())
        : site.BuildMenu(folder.Get(), owner.value, popup.value, 1, 0x7fff, flags, bind, context.GetAddressOf());
    Require(SUCCEEDED(result) && context && GetMenuItemCount(popup.value) > 0, "native benchmark builds the actual menu");
    const auto prepared = std::chrono::steady_clock::now();
    ComPtr<IContextMenu3> messages3;
    ComPtr<IContextMenu2> messages2;
    LRESULT messageResult = 0;
    if (SUCCEEDED(context.As(&messages3)))
        messages3->HandleMenuMsg2(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(popup.value), 0, &messageResult);
    else if (SUCCEEDED(context.As(&messages2)))
        messages2->HandleMenuMsg(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(popup.value), 0);
    const auto ready = std::chrono::steady_clock::now();
    const auto milliseconds = std::chrono::duration<double, std::milli>(ready - started).count();
    std::cout << "native_menu mode=" << utf8(mode) << " extension=" << utf8(extension) << " iteration=" << iteration << " ready_ms=" << milliseconds
        << " prepare_ms=" << std::chrono::duration<double, std::milli>(prepared - started).count()
        << " root_init_ms=" << std::chrono::duration<double, std::milli>(ready - prepared).count()
        << " roots=" << GetMenuItemCount(popup.value) << " file_primer=" << filePrimer << " sync_cascades=" << synchronous << std::endl;
    for (int index = 0; index < GetMenuItemCount(popup.value); ++index)
    {
        wchar_t label[2048]{};
        MENUITEMINFOW item{sizeof(item)}; item.fMask = MIIM_STRING | MIIM_FTYPE | MIIM_SUBMENU;
        item.dwTypeData = label; item.cch = 2048;
        if (GetMenuItemInfoW(popup.value, index, TRUE, &item))
        {
            std::cout << "native_root iteration=" << iteration << " index=" << index << " separator=" << ((item.fType & MFT_SEPARATOR) != 0)
                << " submenu=" << bool(item.hSubMenu) << " label=" << utf8(label) << std::endl;
            if (item.hSubMenu)
            {
                if (messages3) messages3->HandleMenuMsg2(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(item.hSubMenu), MAKELPARAM(index, FALSE), &messageResult);
                else if (messages2) messages2->HandleMenuMsg(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(item.hSubMenu), MAKELPARAM(index, FALSE));
                std::cout << "native_children iteration=" << iteration << " parent=" << utf8(label) << " count=" << GetMenuItemCount(item.hSubMenu) << std::endl;
                for (int j = 0; j < GetMenuItemCount(item.hSubMenu); ++j)
                {
                    wchar_t text[2048]{}; GetMenuStringW(item.hSubMenu, static_cast<UINT>(j), text, 2048, MF_BYPOSITION);
                    std::cout << "native_child iteration=" << iteration << " parent=" << utf8(label) << " index=" << j << " label=" << utf8(text) << std::endl;
                }
            }
        }
    }
    }
}

// Explicit read-only diagnostic modes, never part of the automatic test suite.
// Run each in a separately supervised process to compare actual installed
// providers without launching/automating SnowDesktop or invoking any command.
void ProbeNativeShellMenuBootstrap(bool legacy)
{
    struct Probe
    {
        std::wstring directory;
        PIDLIST_ABSOLUTE pidl = nullptr;
        HWND owner = nullptr;
        HMENU menu = nullptr;
        ~Probe()
        {
            if (menu) DestroyMenu(menu);
            if (owner) DestroyWindow(owner);
            if (pidl) CoTaskMemFree(pidl);
            if (!directory.empty()) RemoveDirectoryW(directory.c_str());
        }
    } probe;
    GUID id{}; Require(SUCCEEDED(CoCreateGuid(&id)), "native probe identity");
    wchar_t text[40]{}; StringFromGUID2(id, text, 40);
    probe.directory = (std::filesystem::temp_directory_path() /
        (std::wstring(L"SnowDesktop-NativeMenuProbe-") + text)).wstring();
    Require(CreateDirectoryW(probe.directory.c_str(), nullptr) != FALSE, "private native probe folder");
    Require(SUCCEEDED(SHParseDisplayName(probe.directory.c_str(), nullptr, &probe.pidl, 0, nullptr)), "native probe PIDL");
    probe.owner = CreateWindowExW(0, L"STATIC", L"Private native menu probe", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    probe.menu = CreatePopupMenu(); Require(probe.owner && probe.menu, "native probe owner and menu");
    ComPtr<IShellFolder> desktop;
    PCUITEMID_CHILD child = nullptr;
    BindProbeSelection(probe.pidl, probe.directory, desktop, child);
    const auto bind = [&](HWND owner, IContextMenu** target) {
        return desktop->GetUIObjectOf(owner, 1, &child, IID_IContextMenu, nullptr, reinterpret_cast<void**>(target));
    };
    constexpr UINT flags = CMF_NORMAL | CMF_CANRENAME | CMF_SYNCCASCADEMENU;
    snowdesktop::ShellContextMenuSite site;
    ComPtr<IContextMenu> menu;
    const auto started = GetTickCount64();
    std::wcout << L"Native probe pid=" << GetCurrentProcessId() << L" legacy=" << legacy
        << L" folder=" << probe.directory << std::endl;
    HRESULT hr;
    if (legacy)
    {
        site.Initialize(desktop.Get(), probe.owner);
        hr = bind(site.HostWindow() ? site.HostWindow() : probe.owner, menu.GetAddressOf());
        if (SUCCEEDED(hr) && menu)
        {
            site.Attach(menu.Get());
            std::wcout << L"Legacy query begins after creating the view" << std::endl;
            hr = menu->QueryContextMenu(probe.menu, 0, 1, 0x7fff, flags);
        }
    }
    else
        hr = site.BuildMenu(desktop.Get(), probe.owner, probe.menu, 1, 0x7fff, flags, bind, menu.GetAddressOf());
    std::wcout << L"Native query hr=0x" << std::hex << static_cast<unsigned long>(hr) << std::dec
        << L" entries=" << GetMenuItemCount(probe.menu) << L" elapsedMs=" << GetTickCount64() - started << std::endl;
    Require(SUCCEEDED(hr) && menu && GetMenuItemCount(probe.menu) > 0, "actual native menu generation succeeds");
}

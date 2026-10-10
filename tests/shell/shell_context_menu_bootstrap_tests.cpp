#include "shell/shell_context_menu_site.h"
#include <shlguid.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <memory>
#include <stdexcept>
#include <filesystem>
#include <iostream>

namespace
{
using Microsoft::WRL::ComPtr;
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// Substitute only the third-party COM provider and Shell view. The production
// BuildMenu, HWND creation, file bootstrap, lifetime and rebind all run.
struct State
{
    bool loaded = false, viewExists = false;
    unsigned bindings = 0, coldLoads = 0, viewCreates = 0, warmAlive = 0, invocations = 0;
    UINT expectedFlags = 0, first = 7, last = 103;
    int failure = 0;
    HWND owner = nullptr;
    HMENU initialPopup = nullptr;
};

class View final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IShellView>
{
public:
    explicit View(std::shared_ptr<State> state) : state_(std::move(state)) {}
    IFACEMETHODIMP GetWindow(HWND* output) override { if (!output) return E_POINTER; *output = window_; return S_OK; }
    IFACEMETHODIMP ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
    IFACEMETHODIMP TranslateAccelerator(MSG*) override { return S_FALSE; }
    IFACEMETHODIMP EnableModeless(BOOL) override { return S_OK; }
    IFACEMETHODIMP UIActivate(UINT) override { return S_OK; }
    IFACEMETHODIMP Refresh() override { return S_OK; }
    IFACEMETHODIMP CreateViewWindow(IShellView*, LPCFOLDERSETTINGS, IShellBrowser* browser,
        RECT*, HWND* window) override
    {
        if (!window) return E_POINTER;
        HWND parent = nullptr;
        if (FAILED(browser->GetWindow(&parent))) return E_FAIL;
        window_ = CreateWindowExW(0, L"STATIC", L"Private menu view", WS_CHILD,
            0, 0, 1, 1, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        *window = window_;
        state_->viewExists = window_ != nullptr;
        return window_ ? S_OK : E_FAIL;
    }
    IFACEMETHODIMP DestroyViewWindow() override
    {
        if (window_) DestroyWindow(window_);
        window_ = nullptr; state_->viewExists = false; return S_OK;
    }
    IFACEMETHODIMP GetCurrentInfo(LPFOLDERSETTINGS) override { return E_NOTIMPL; }
    IFACEMETHODIMP AddPropertySheetPages(DWORD, LPFNSVADDPROPSHEETPAGE, LPARAM) override { return E_NOTIMPL; }
    IFACEMETHODIMP SaveViewState() override { return S_OK; }
    IFACEMETHODIMP SelectItem(PCUITEMID_CHILD, SVSIF) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetItemObject(UINT, REFIID, void**) override { return E_NOTIMPL; }
private:
    std::shared_ptr<State> state_;
    HWND window_ = nullptr;
};

class Folder final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IShellFolder>
{
public:
    explicit Folder(std::shared_ptr<State> state) : state_(std::move(state)) {}
    IFACEMETHODIMP ParseDisplayName(HWND, IBindCtx*, LPWSTR, ULONG*, PIDLIST_RELATIVE*, ULONG*) override { return E_NOTIMPL; }
    IFACEMETHODIMP EnumObjects(HWND, SHCONTF, IEnumIDList**) override { return E_NOTIMPL; }
    IFACEMETHODIMP BindToObject(PCUIDLIST_RELATIVE, IBindCtx*, REFIID, void**) override { return E_NOTIMPL; }
    IFACEMETHODIMP BindToStorage(PCUIDLIST_RELATIVE, IBindCtx*, REFIID, void**) override { return E_NOTIMPL; }
    IFACEMETHODIMP CompareIDs(LPARAM, PCUIDLIST_RELATIVE, PCUIDLIST_RELATIVE) override { return E_NOTIMPL; }
    IFACEMETHODIMP CreateViewObject(HWND, REFIID iid, void** result) override
    {
        ++state_->viewCreates;
        if (state_->failure == 5) return E_NOTIMPL;
        if (!state_->loaded || state_->warmAlive != 1 || !IsMenu(state_->initialPopup)) return E_UNEXPECTED;
        auto view = Microsoft::WRL::Make<View>(state_);
        return view->QueryInterface(iid, result);
    }
    IFACEMETHODIMP GetAttributesOf(UINT, PCUITEMID_CHILD_ARRAY, SFGAOF*) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetUIObjectOf(HWND, UINT, PCUITEMID_CHILD_ARRAY, REFIID, UINT*, void**) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetDisplayNameOf(PCUITEMID_CHILD, SHGDNF, STRRET*) override { return E_NOTIMPL; }
    IFACEMETHODIMP SetNameOf(HWND, PCUITEMID_CHILD, LPCWSTR, SHGDNF, PITEMID_CHILD*) override { return E_NOTIMPL; }
private:
    std::shared_ptr<State> state_;
};

class Menu final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IContextMenu, IObjectWithSite>
{
public:
    Menu(std::shared_ptr<State> state, unsigned binding) : state_(std::move(state)), binding_(binding)
    {
        if (binding_ == 1) ++state_->warmAlive;
    }
    ~Menu() { if (binding_ == 1) --state_->warmAlive; }
    IFACEMETHODIMP QueryContextMenu(HMENU menu, UINT index, UINT first, UINT last, UINT flags) override
    {
        if (queried_) return E_UNEXPECTED; // A provider need not support re-querying one mutable aggregate.
        queried_ = true;
        if (binding_ == 1) state_->initialPopup = menu;
        if (first != state_->first || last != state_->last || index != 0) return E_INVALIDARG;
        if (!state_->loaded)
        {
            // The observed DllMain creates a window before waiting for Shell
            // initialization. Report the bad ordering without hanging the test.
            if (state_->viewExists) return E_UNEXPECTED;
            if (flags & CMF_SYNCCASCADEMENU) return E_UNEXPECTED;
            if (state_->failure == 2) return E_ACCESSDENIED;
            HWND window = CreateWindowExW(0, L"STATIC", L"Provider initialization", WS_POPUP,
                0, 0, 1, 1, state_->owner, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!window) return E_FAIL;
            DestroyWindow(window);
            ++state_->coldLoads; state_->loaded = true;
        }
        if (binding_ == 2)
        {
            if (flags != state_->expectedFlags || state_->warmAlive != 1 || !IsMenu(state_->initialPopup)) return E_UNEXPECTED;
            if (state_->failure != 5)
            {
                ComPtr<IServiceProvider> services;
                ComPtr<IShellView> view;
                ComPtr<IShellBrowser> browser;
                if (!site_ || FAILED(site_.As(&services)) ||
                    FAILED(services->QueryService(SID_SFolderView, IID_PPV_ARGS(&view))) ||
                    FAILED(services->QueryService(SID_SShellBrowser, IID_PPV_ARGS(&browser)))) return E_UNEXPECTED;
            }
            if (state_->failure == 4) return E_ACCESSDENIED;
        }
        return InsertMenuW(menu, 0, MF_BYPOSITION | MF_STRING, first + (binding_ == 1 ? 3 : 11),
            binding_ == 1 ? L"Unpublished command" : L"Actual command") ? MAKE_HRESULT(SEVERITY_SUCCESS, 0, 12) : E_FAIL;
    }
    IFACEMETHODIMP InvokeCommand(LPCMINVOKECOMMANDINFO info) override
    {
        if (binding_ != 2 || reinterpret_cast<ULONG_PTR>(info->lpVerb) != 11) return E_UNEXPECTED;
        ++state_->invocations; return S_OK;
    }
    IFACEMETHODIMP GetCommandString(UINT_PTR, UINT, UINT*, LPSTR, UINT) override { return E_NOTIMPL; }
    IFACEMETHODIMP SetSite(IUnknown* site) override { site_ = site; return S_OK; }
    IFACEMETHODIMP GetSite(REFIID iid, void** output) override { return site_ ? site_->QueryInterface(iid, output) : E_FAIL; }
private:
    std::shared_ptr<State> state_;
    unsigned binding_;
    bool queried_ = false;
    ComPtr<IUnknown> site_;
};

void RunCase(UINT flags, int failure)
{
    auto state = std::make_shared<State>(); state->expectedFlags = flags; state->failure = failure;
    state->owner = CreateWindowExW(0, L"STATIC", L"Private bootstrap owner", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(state->owner != nullptr, "private menu owner");
    const HMENU popup = CreatePopupMenu();
    Require(popup != nullptr, "private actual menu");
    {
        auto folder = Microsoft::WRL::Make<Folder>(state);
        snowdesktop::ShellContextMenuSite site;
        ComPtr<IContextMenu> actual;
        const auto factory = [&](HWND owner, IContextMenu** target) -> HRESULT {
            ++state->bindings;
            if ((state->bindings == 1 && failure == 1) || (state->bindings == 2 && failure == 3)) return E_ACCESSDENIED;
            if (state->bindings == 1 && owner != state->owner) return E_UNEXPECTED;
            if (state->bindings == 2 && owner != (failure == 5 ? state->owner : site.HostWindow())) return E_UNEXPECTED;
            auto menu = Microsoft::WRL::Make<Menu>(state, state->bindings);
            return menu.CopyTo(target);
        };
        const HRESULT hr = site.BuildMenu(folder.Get(), state->owner, popup, state->first, state->last,
            flags, factory, actual.GetAddressOf());
        if (failure >= 1 && failure <= 4)
        {
            Require(hr == E_ACCESSDENIED && !actual, "failed initialization never exposes a partial aggregate");
            Require(state->bindings == (failure <= 2 ? 1u : 2u), "initial failure stops before the next binding");
            Require(state->viewCreates == (failure <= 2 ? 0u : 1u), "failed cold load cannot create a view");
        }
        else
        {
            Require(SUCCEEDED(hr) && actual && state->bindings == 2 && state->coldLoads == 1,
                "cold providers load before view creation and final menu uses a fresh aggregate");
            Require(GetMenuItemCount(popup) == 1 && GetMenuItemID(popup, 0) == state->first + 11,
                "only final menu IDs can be displayed and invoked");
            CMINVOKECOMMANDINFO info{sizeof(info)}; info.lpVerb = MAKEINTRESOURCEA(11);
            Require(SUCCEEDED(actual->InvokeCommand(&info)) && state->invocations == 1,
                "the final provider executes once with its own offset");
            Require(state->warmAlive == 1 && IsMenu(state->initialPopup),
                "cold providers and their unpublished menus remain retained throughout tracking and invocation");
        }
    }
    Require(!state->viewExists && state->warmAlive == 0 && !IsMenu(state->initialPopup),
        "all bootstrap providers, unpublished menus and hidden views retire with the site");
    DestroyMenu(popup); DestroyWindow(state->owner);
}
} // namespace

void TestShellContextMenuBootstrap()
{
    // Negative control of the prior production ordering, using the same COM
    // provider: a first load after view creation fails for the loader reason.
    auto state = std::make_shared<State>(); state->viewExists = true;
    auto provider = Microsoft::WRL::Make<Menu>(state, 1);
    const HMENU scratch = CreatePopupMenu();
    Require(provider->QueryContextMenu(scratch, 0, state->first, state->last, CMF_NORMAL) == E_UNEXPECTED,
        "LOADER_REENTRY_NEGATIVE_CONTROL: view-before-first-load must fail");
    DestroyMenu(scratch);
    for (const UINT flags : {UINT(CMF_NORMAL | CMF_CANRENAME | CMF_SYNCCASCADEMENU),
             UINT(CMF_NORMAL | CMF_EXPLORE | CMF_CANRENAME | CMF_SYNCCASCADEMENU),
             UINT(CMF_NORMAL | CMF_SYNCCASCADEMENU)})
        for (int failure = 0; failure <= 5; ++failure) RunCase(flags, failure);

    std::wstring sample;
    {
        snowdesktop::ShellMenuInitializationFile file;
        Require(file.handle != INVALID_HANDLE_VALUE, "private association bootstrap sample is created");
        sample = file.path;
        Require(GetFileAttributesW(sample.c_str()) != INVALID_FILE_ATTRIBUTES, "bootstrap sample remains while in use");
    }
    Require(GetFileAttributesW(sample.c_str()) == INVALID_FILE_ATTRIBUTES, "bootstrap sample is deleted on retirement");
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
    Require(SUCCEEDED(SHGetDesktopFolder(&desktop)), "native probe desktop binding");
    PCUITEMID_CHILD child = reinterpret_cast<PCUITEMID_CHILD>(probe.pidl);
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

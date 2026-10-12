#include "shell_context_menu_site.h"

#include <shlguid.h>
#include <filesystem>
#include <wrl/client.h>
#include <wrl/implements.h>

namespace snowdesktop
{
namespace
{

using Microsoft::WRL::ComPtr;

class BrowserSite final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    IShellBrowser, IServiceProvider>
{
public:
    explicit BrowserSite(HWND window) : window_(window) {}

    void SetView(IShellView* view) { view_ = view; }
    void SetWindow(HWND window) { window_ = window; }

    IFACEMETHODIMP GetWindow(HWND* window) override
    {
        if (!window)
            return E_POINTER;
        *window = window_;
        return window_ ? S_OK : E_FAIL;
    }

    IFACEMETHODIMP ContextSensitiveHelp(BOOL) override
        { return E_NOTIMPL; }
    IFACEMETHODIMP InsertMenusSB(HMENU, LPOLEMENUGROUPWIDTHS) override
        { return E_NOTIMPL; }
    IFACEMETHODIMP SetMenuSB(HMENU, HOLEMENU, HWND) override
        { return S_OK; }
    IFACEMETHODIMP RemoveMenusSB(HMENU) override { return S_OK; }
    IFACEMETHODIMP SetStatusTextSB(LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP EnableModelessSB(BOOL) override { return S_OK; }
    IFACEMETHODIMP TranslateAcceleratorSB(MSG*, WORD) override
        { return S_FALSE; }
    IFACEMETHODIMP BrowseObject(PCUIDLIST_RELATIVE, UINT) override
        { return E_NOTIMPL; }

    IFACEMETHODIMP GetViewStateStream(DWORD, IStream** stream) override
    {
        if (stream)
            *stream = nullptr;
        return E_NOTIMPL;
    }

    IFACEMETHODIMP GetControlWindow(UINT, HWND* window) override
    {
        if (window)
            *window = nullptr;
        return E_NOTIMPL;
    }

    IFACEMETHODIMP SendControlMsg(
        UINT, UINT, WPARAM, LPARAM, LRESULT* result) override
    {
        if (result)
            *result = 0;
        return E_NOTIMPL;
    }

    IFACEMETHODIMP QueryActiveShellView(IShellView** view) override
    {
        if (!view)
            return E_POINTER;
        *view = view_.Get();
        if (!*view)
            return E_FAIL;
        (*view)->AddRef();
        return S_OK;
    }

    IFACEMETHODIMP OnViewWindowActive(IShellView*) override
        { return S_OK; }
    IFACEMETHODIMP SetToolbarItems(LPTBBUTTONSB, UINT, UINT) override
        { return E_NOTIMPL; }

    IFACEMETHODIMP QueryService(
        REFGUID service, REFIID interfaceId, void** object) override
    {
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (IsEqualGUID(service, SID_SFolderView) && view_)
            return view_->QueryInterface(interfaceId, object);
        if (IsEqualGUID(service, SID_SShellBrowser) ||
            IsEqualGUID(service, SID_STopLevelBrowser) ||
            IsEqualGUID(service, SID_SInPlaceBrowser))
        {
            return QueryInterface(interfaceId, object);
        }
        return E_NOINTERFACE;
    }

private:
    HWND window_ = nullptr;
    ComPtr<IShellView> view_;
};

} // namespace

ShellMenuInitializationFile::ShellMenuInitializationFile()
{
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) return;
    wchar_t guid[40]{};
    StringFromGUID2(id, guid, 40);
    std::error_code error;
    const auto directory = std::filesystem::temp_directory_path(error);
    if (error) return;
    path = (directory / (std::wstring(L"SnowDesktop-MenuInit-") + guid + L".txt")).wstring();
    handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
}

ShellMenuInitializationFile::~ShellMenuInitializationFile()
{
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
}

struct ShellFileMenuPrimer::Impl
{
    ShellMenuInitializationFile file;
    ComPtr<IContextMenu> context;
    HMENU menu = CreatePopupMenu();
    ~Impl()
    {
        context.Reset();
        if (menu) DestroyMenu(menu);
    }
};

ShellFileMenuPrimer::ShellFileMenuPrimer() : impl_(std::make_unique<Impl>()) {}
ShellFileMenuPrimer::~ShellFileMenuPrimer() = default;

bool ShellFileMenuPrimer::Initialize()
{
    if (impl_->file.handle == INVALID_HANDLE_VALUE || !impl_->menu) return false;
    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(impl_->file.path.c_str(), nullptr, IID_PPV_ARGS(&item))) || !item ||
        FAILED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&impl_->context))) || !impl_->context) return false;
    return SUCCEEDED(impl_->context->QueryContextMenu(impl_->menu, 0, 1, 0x7fff, CMF_NORMAL | CMF_ITEMMENU));
}

struct ShellContextMenuSite::Impl
{
    ~Impl() { Reset(); }

    void Reset()
    {
        if (attachedMenu)
            attachedMenu->SetSite(nullptr);
        attachedMenu.Reset();
        if (browser)
            browser->SetWindow(hostWindow);
        if (view && viewWindow)
            view->DestroyViewWindow();
        viewWindow = nullptr;
        if (browser)
            browser->SetView(nullptr);
        browser.Reset();
        view.Reset();
        if (hostWindow)
            DestroyWindow(hostWindow);
        hostWindow = nullptr;
        initialMenu.Reset();
        if (initialPopup) DestroyMenu(initialPopup);
        initialPopup = nullptr;
        filePrimer.reset();
    }

    HWND hostWindow = nullptr;
    HWND viewWindow = nullptr;
    ComPtr<IShellView> view;
    ComPtr<BrowserSite> browser;
    ComPtr<IObjectWithSite> attachedMenu;
    ComPtr<IContextMenu> initialMenu;
    HMENU initialPopup = nullptr;
    std::unique_ptr<ShellFileMenuPrimer> filePrimer;
};

ShellContextMenuSite::ShellContextMenuSite()
    : impl_(std::make_unique<Impl>())
{
}

ShellContextMenuSite::~ShellContextMenuSite() = default;

bool ShellContextMenuSite::Initialize(IShellFolder* folder, HWND owner)
{
    impl_->Reset();
    if (!folder || !owner || !IsWindow(owner))
        return false;

    impl_->hostWindow = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"STATIC", L"SnowDesktop Shell Menu Site",
        WS_POPUP, 0, 0, 1, 1, owner, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!impl_->hostWindow)
        return false;

    if (FAILED(folder->CreateViewObject(
            impl_->hostWindow, IID_PPV_ARGS(&impl_->view))) ||
        !impl_->view)
    {
        impl_->Reset();
        return false;
    }

    impl_->browser = Microsoft::WRL::Make<BrowserSite>(
        impl_->hostWindow);
    if (!impl_->browser)
    {
        impl_->Reset();
        return false;
    }
    impl_->browser->SetView(impl_->view.Get());

    // This hidden view supplies folder services and a real view window.
    // It does not display file details or icons; menu icons are read separately.
    FOLDERSETTINGS settings{
        FVM_LIST,
        static_cast<FOLDERFLAGS>(
            FWF_NOCLIENTEDGE | FWF_NOBROWSERVIEWSTATE | FWF_NOICONS),
    };
    RECT bounds{ 0, 0, 1, 1 };
    if (FAILED(impl_->view->CreateViewWindow(
            nullptr, &settings, impl_->browser.Get(),
            &bounds, &impl_->viewWindow)) ||
        !impl_->viewWindow)
    {
        impl_->Reset();
        return false;
    }
    ShowWindow(impl_->viewWindow, SW_HIDE);
    return true;
}

bool ShellContextMenuSite::Attach(IContextMenu* contextMenu)
{
    if (!contextMenu || !impl_->browser || !impl_->viewWindow)
        return false;
    impl_->attachedMenu.Reset();
    if (FAILED(contextMenu->QueryInterface(
            IID_PPV_ARGS(&impl_->attachedMenu))) ||
        !impl_->attachedMenu)
    {
        return false;
    }
    const HRESULT result = impl_->attachedMenu->SetSite(
        static_cast<IServiceProvider*>(impl_->browser.Get()));
    if (FAILED(result))
    {
        impl_->attachedMenu.Reset();
        return false;
    }
    return true;
}

HRESULT ShellContextMenuSite::BuildMenu(IShellFolder* folder, HWND owner, HMENU menu,
    UINT firstCommand, UINT lastCommand, UINT flags,
    const MenuFactory& bind, IContextMenu** contextMenu)
{
    if (!contextMenu) return E_POINTER;
    *contextMenu = nullptr;
    impl_->Reset();
    if (!folder || !IsWindow(owner) || !menu || !bind || firstCommand > lastCommand)
        return E_INVALIDARG;

    // Match the ordinary helper's folder/ShellLink bootstrap: initialize file
    // associations before folder-only providers create windows from DllMain.
    auto filePrimer = std::make_unique<ShellFileMenuPrimer>();
    filePrimer->Initialize();
    ComPtr<IContextMenu> initial;
    HRESULT hr = bind(owner, initial.GetAddressOf());
    if (FAILED(hr) || !initial) return FAILED(hr) ? hr : E_FAIL;
    const HMENU scratch = CreatePopupMenu();
    if (!scratch) return E_OUTOFMEMORY;
    // No Shell view and no synchronous cascade expansion during initial loads.
    hr = initial->QueryContextMenu(scratch, 0, firstCommand, lastCommand,
        flags & ~CMF_SYNCCASCADEMENU);
    if (FAILED(hr)) { initial.Reset(); DestroyMenu(scratch); return hr; }

    Initialize(folder, owner);
    impl_->initialMenu = std::move(initial);
    // Provider windows/timers can still refer to the unpublished HMENU during
    // view initialization and final binding. Retain it with its COM aggregate.
    impl_->initialPopup = scratch;
    impl_->filePrimer = std::move(filePrimer);
    ComPtr<IContextMenu> actual;
    hr = bind(HostWindow() ? HostWindow() : owner, actual.GetAddressOf());
    if (FAILED(hr) || !actual) return FAILED(hr) ? hr : E_FAIL;
    Attach(actual.Get());
    // The unpublished aggregate's offsets/state must never be reused by a
    // real command. The fresh aggregate receives the original site and flags.
    hr = actual->QueryContextMenu(menu, 0, firstCommand, lastCommand, flags);
    if (FAILED(hr)) return hr;
    actual.CopyTo(contextMenu);
    return hr;
}

HRESULT ShellContextMenuSite::BuildNamespaceMenu(HWND owner, HMENU menu,
    UINT firstCommand, UINT lastCommand, UINT flags,
    const MenuFactory& bind, IContextMenu** contextMenu)
{
    if (!contextMenu) return E_POINTER;
    *contextMenu = nullptr;
    impl_->Reset();
    if (!IsWindow(owner) || !menu || !bind || firstCommand > lastCommand)
        return E_INVALIDARG;
    ComPtr<IContextMenu> actual;
    HRESULT hr = bind(owner, actual.GetAddressOf());
    if (FAILED(hr) || !actual) return FAILED(hr) ? hr : E_FAIL;
    hr = actual->QueryContextMenu(menu, 0, firstCommand, lastCommand, flags);
    if (FAILED(hr)) return hr;
    actual.CopyTo(contextMenu);
    return hr;
}

HWND ShellContextMenuSite::HostWindow() const
{
    return impl_->hostWindow;
}

void ShellContextMenuSite::SetInvocationOwner(HWND owner)
{
    if (impl_->browser && owner && IsWindow(owner))
        impl_->browser->SetWindow(owner);
}

} // namespace snowdesktop

#include "shell_context_menu_site.h"

#include <shlguid.h>
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

struct ShellContextMenuSite::Impl
{
    ~Impl() { Reset(); }

    void Reset()
    {
        if (attachedMenu)
            attachedMenu->SetSite(nullptr);
        attachedMenu.Reset();
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
    }

    HWND hostWindow = nullptr;
    HWND viewWindow = nullptr;
    ComPtr<IShellView> view;
    ComPtr<BrowserSite> browser;
    ComPtr<IObjectWithSite> attachedMenu;
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

    FOLDERSETTINGS settings{
        FVM_DETAILS,
        static_cast<FOLDERFLAGS>(
            FWF_NOCLIENTEDGE | FWF_NOBROWSERVIEWSTATE),
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

HWND ShellContextMenuSite::HostWindow() const
{
    return impl_->hostWindow;
}

} // namespace snowdesktop

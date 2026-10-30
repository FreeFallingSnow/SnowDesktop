#include "shell_context_menu_site.h"
#include "shell_extension_diagnostics.h"

#include <shlguid.h>
#include <vector>
#include <shlwapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>

namespace snowdesktop
{
namespace
{

using Microsoft::WRL::ComPtr;

class SelectionEnumerator final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IEnumIDList>
{
public:
    SelectionEnumerator(IFolderView* view, UINT count, UINT position = 0)
        : view_(view), count_(count), position_(position) {}
    IFACEMETHODIMP Next(ULONG count, PITEMID_CHILD* children, ULONG* fetched) override
    {
        if (fetched) *fetched = 0;
        if (!children || (!fetched && count != 1)) return E_POINTER;
        ULONG copied = 0;
        while (copied < count && position_ < count_)
        {
            const HRESULT hr = view_->Item(static_cast<int>(position_), &children[copied]);
            if (FAILED(hr)) { if (fetched) *fetched = copied; return hr; }
            ++copied; ++position_;
        }
        if (fetched) *fetched = copied;
        return copied == count ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Skip(ULONG count) override
    {
        const UINT remaining = count_ - position_;
        position_ += count < remaining ? count : remaining;
        return count <= remaining ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Reset() override { position_ = 0; return S_OK; }
    IFACEMETHODIMP Clone(IEnumIDList** enumerator) override
        { return Microsoft::WRL::Make<SelectionEnumerator>(view_.Get(), count_, position_).CopyTo(enumerator); }
private:
    ComPtr<IFolderView> view_;
    UINT count_, position_;
};

// Menu providers need the selected items, not an asynchronously enumerated
// duplicate folder window. Keep this immutable snapshot alive with the menu.
class SelectionView final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    IShellView, Microsoft::WRL::ChainInterfaces<IFolderView2, IFolderView>>
{
public:
    SelectionView(IShellFolder* folder, HWND owner) : folder_(folder), owner_(owner) {}
    ~SelectionView() { for (auto child : children_) ILFree(child); }
    HRESULT Initialize(UINT count, PCUITEMID_CHILD_ARRAY children)
    {
        if (!count) return S_OK;
        for (UINT i = 0; i < count; ++i)
        {
            if (!children[i]) return E_INVALIDARG;
            auto copy = ILCloneFull(children[i]);
            if (!copy) return E_OUTOFMEMORY;
            children_.push_back(copy);
        }
        return SHCreateShellItemArray(nullptr, folder_.Get(), count, children, &selection_);
    }
    IFACEMETHODIMP GetWindow(HWND* window) override
        { if (!window) return E_POINTER; *window = owner_; return S_OK; }
    IFACEMETHODIMP ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
    IFACEMETHODIMP TranslateAccelerator(MSG*) override { return S_FALSE; }
    IFACEMETHODIMP EnableModeless(BOOL) override { return S_OK; }
    IFACEMETHODIMP UIActivate(UINT) override { return S_OK; }
    IFACEMETHODIMP Refresh() override { return E_NOTIMPL; }
    IFACEMETHODIMP CreateViewWindow(IShellView*, LPCFOLDERSETTINGS, IShellBrowser*, RECT*, HWND*) override
        { return E_NOTIMPL; }
    IFACEMETHODIMP DestroyViewWindow() override { return S_OK; }
    IFACEMETHODIMP GetCurrentInfo(LPFOLDERSETTINGS settings) override
    {
        if (!settings) return E_POINTER;
        *settings = {FVM_LIST, FWF_NOICONS}; return S_OK;
    }
    IFACEMETHODIMP AddPropertySheetPages(DWORD, LPFNSVADDPROPSHEETPAGE, LPARAM) override { return E_NOTIMPL; }
    IFACEMETHODIMP SaveViewState() override { return S_OK; }
    IFACEMETHODIMP SelectItem(PCUITEMID_CHILD, SVSIF) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetItemObject(UINT flags, REFIID iid, void** object) override
        { return Items(flags, iid, object); }
    IFACEMETHODIMP GetCurrentViewMode(UINT* mode) override
        { if (!mode) return E_POINTER; *mode = FVM_LIST; return S_OK; }
    IFACEMETHODIMP SetCurrentViewMode(UINT) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetFolder(REFIID iid, void** object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (IsEqualIID(iid, IID_IShellItemArray) || IsEqualIID(iid, IID_IShellItem))
        {
            PIDLIST_ABSOLUTE parent = nullptr;
            HRESULT hr = SHGetIDListFromObject(folder_.Get(), &parent);
            if (FAILED(hr) || !parent) return FAILED(hr) ? hr : E_FAIL;
            if (IsEqualIID(iid, IID_IShellItemArray))
            {
                PCIDLIST_ABSOLUTE ids[] = {parent};
                hr = SHCreateShellItemArrayFromIDLists(1, ids, reinterpret_cast<IShellItemArray**>(object));
            }
            else hr = SHCreateItemFromIDList(parent, iid, object);
            CoTaskMemFree(parent);
            return hr;
        }
        return folder_->QueryInterface(iid, object);
    }
    IFACEMETHODIMP Item(int index, PITEMID_CHILD* child) override
    {
        if (!child) return E_POINTER;
        *child = nullptr;
        if (index < 0 || static_cast<size_t>(index) >= children_.size()) return E_INVALIDARG;
        *child = ILCloneFull(children_[index]); return *child ? S_OK : E_OUTOFMEMORY;
    }
    IFACEMETHODIMP ItemCount(UINT flags, int* count) override
    {
        if (!count) return E_POINTER;
        *count = 0;
        if ((flags & SVGIO_TYPE_MASK) != SVGIO_SELECTION && (flags & SVGIO_TYPE_MASK) != SVGIO_ALLVIEW)
            return E_INVALIDARG;
        *count = static_cast<int>(children_.size()); return S_OK;
    }
    IFACEMETHODIMP Items(UINT flags, REFIID iid, void** object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if ((flags & SVGIO_TYPE_MASK) != SVGIO_SELECTION && (flags & SVGIO_TYPE_MASK) != SVGIO_ALLVIEW)
            return E_INVALIDARG;
        if (children_.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        if (IsEqualIID(iid, IID_IDataObject))
            return folder_->GetUIObjectOf(owner_, static_cast<UINT>(children_.size()),
                const_cast<PCUITEMID_CHILD_ARRAY>(children_.data()), iid, nullptr, object);
        if (IsEqualIID(iid, IID_IEnumIDList))
            return Microsoft::WRL::Make<SelectionEnumerator>(static_cast<IFolderView*>(this),
                static_cast<UINT>(children_.size())).CopyTo(iid, object);
        return selection_->QueryInterface(iid, object);
    }
    IFACEMETHODIMP GetSelectionMarkedItem(int* index) override { return GetFocusedItem(index); }
    IFACEMETHODIMP GetFocusedItem(int* index) override
        { if (!index) return E_POINTER; *index = children_.empty() ? -1 : 0; return children_.empty() ? S_FALSE : S_OK; }
    IFACEMETHODIMP GetItemPosition(PCUITEMID_CHILD, POINT*) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetSpacing(POINT*) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetDefaultSpacing(POINT*) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetAutoArrange() override { return S_FALSE; }
    IFACEMETHODIMP SelectItem(int, DWORD) override { return E_NOTIMPL; }
    IFACEMETHODIMP SelectAndPositionItems(UINT, PCUITEMID_CHILD_ARRAY, POINT*, DWORD) override { return E_NOTIMPL; }
    IFACEMETHODIMP SetGroupBy(REFPROPERTYKEY, BOOL) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetGroupBy(PROPERTYKEY*, BOOL*) override { return E_NOTIMPL; }
    IFACEMETHODIMP SetViewProperty(PCUITEMID_CHILD, REFPROPERTYKEY, REFPROPVARIANT) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetViewProperty(PCUITEMID_CHILD, REFPROPERTYKEY, PROPVARIANT*) override { return E_NOTIMPL; }
    IFACEMETHODIMP SetTileViewProperties(PCUITEMID_CHILD, LPCWSTR) override { return E_NOTIMPL; }
    IFACEMETHODIMP SetExtendedTileViewProperties(PCUITEMID_CHILD, LPCWSTR) override { return E_NOTIMPL; }
    IFACEMETHODIMP SetText(FVTEXTTYPE, LPCWSTR) override { return E_NOTIMPL; }
    IFACEMETHODIMP SetCurrentFolderFlags(DWORD, DWORD) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetCurrentFolderFlags(DWORD* flags) override
        { if (!flags) return E_POINTER; *flags = FWF_NOICONS; return S_OK; }
    IFACEMETHODIMP GetSortColumnCount(int* count) override
        { if (!count) return E_POINTER; *count = 0; return S_OK; }
    IFACEMETHODIMP SetSortColumns(const SORTCOLUMN*, int) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetSortColumns(SORTCOLUMN*, int) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetItem(int index, REFIID iid, void** object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (index < 0 || !selection_) return E_INVALIDARG;
        ComPtr<IShellItem> item;
        const HRESULT hr = selection_->GetItemAt(static_cast<DWORD>(index), &item);
        return SUCCEEDED(hr) ? item->QueryInterface(iid, object) : hr;
    }
    IFACEMETHODIMP GetVisibleItem(int, BOOL, int*) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetSelectedItem(int start, int* index) override
    {
        if (!index) return E_POINTER;
        *index = -1;
        if (start < 0) return E_INVALIDARG;
        if (start >= static_cast<int>(children_.size())) return S_FALSE;
        *index = start; return S_OK;
    }
    IFACEMETHODIMP GetSelection(BOOL noneImpliesFolder, IShellItemArray** items) override
    {
        if (!items) return E_POINTER;
        *items = nullptr;
        if (selection_) return selection_.CopyTo(items);
        return noneImpliesFolder ? GetFolder(IID_PPV_ARGS(items)) : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    IFACEMETHODIMP GetSelectionState(PCUITEMID_CHILD child, DWORD* flags) override
    {
        if (!flags) return E_POINTER;
        *flags = 0;
        if (!child) return E_INVALIDARG;
        for (size_t i = 0; i < children_.size(); ++i)
            if (ILIsEqual(child, children_[i]))
                { *flags = SVSI_SELECT | (i == 0 ? SVSI_FOCUSED : 0); return S_OK; }
        return S_FALSE;
    }
    IFACEMETHODIMP InvokeVerbOnSelection(LPCSTR) override { return E_NOTIMPL; }
    IFACEMETHODIMP SetViewModeAndIconSize(FOLDERVIEWMODE, int) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetViewModeAndIconSize(FOLDERVIEWMODE* mode, int* size) override
        { if (!mode || !size) return E_POINTER; *mode = FVM_LIST; *size = 16; return S_OK; }
    IFACEMETHODIMP SetGroupSubsetCount(UINT) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetGroupSubsetCount(UINT*) override { return E_NOTIMPL; }
    IFACEMETHODIMP SetRedraw(BOOL) override { return S_OK; }
    IFACEMETHODIMP IsMoveInSameFolder() override { return E_NOTIMPL; }
    IFACEMETHODIMP DoRename() override { return E_NOTIMPL; }
private:
    ComPtr<IShellFolder> folder_;
    HWND owner_ = nullptr;
    std::vector<PITEMID_CHILD> children_;
    ComPtr<IShellItemArray> selection_;
};

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

    RECT ownerBounds{};
    GetWindowRect(owner, &ownerBounds);
    impl_->hostWindow = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"STATIC", L"SnowDesktop Shell Menu Site",
        WS_POPUP, ownerBounds.left, ownerBounds.top, 1, 1, owner, nullptr,
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
    // Backgrounds use the same lightweight site as selected items. Avoid
    // creating an enumerating Shell view or querying a disposable TXT menu.
    return BuildSelectionMenu(folder, owner, menu, firstCommand, lastCommand,
        flags, bind, 0, nullptr, contextMenu);
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

HRESULT ShellContextMenuSite::BuildSelectionMenu(IShellFolder* folder, HWND owner, HMENU menu,
    UINT firstCommand, UINT lastCommand, UINT flags, const MenuFactory& bind,
    UINT count, PCUITEMID_CHILD_ARRAY children, IContextMenu** contextMenu)
{
    shell_extensions::MenuTiming total("native_selection.total");
    if (!contextMenu) return E_POINTER;
    *contextMenu = nullptr;
    impl_->Reset();
    if (!folder || !IsWindow(owner) || !menu || !bind || (count && !children) || firstCommand > lastCommand)
        return E_INVALIDARG;
    const auto attach = [&](IContextMenu* actual) -> HRESULT {
        auto view = Microsoft::WRL::Make<SelectionView>(folder, owner);
        if (!view) return E_OUTOFMEMORY;
        const HRESULT initialized = view->Initialize(count, children);
        if (FAILED(initialized)) return initialized;
        impl_->view = view;
        impl_->viewWindow = owner; // SelectionView never owns/destroys this window.
        impl_->browser = Microsoft::WRL::Make<BrowserSite>(owner);
        if (!impl_->browser) return E_OUTOFMEMORY;
        impl_->browser->SetView(impl_->view.Get());
        Attach(actual);
        return S_OK;
    };
    ComPtr<IContextMenu> actual;
    shell_extensions::MenuTiming binding("native_selection.bind");
    HRESULT hr = bind(owner, actual.GetAddressOf());
    binding.Record(SUCCEEDED(hr) ? "complete" : "failed");
    if (FAILED(hr) || !actual) return FAILED(hr) ? hr : E_FAIL;
    // Background commands resolve their location from the site during QCM.
    // Item commands already receive their selection. Even constructing their
    // Shell item array before cold handler loading installs Shell window hooks;
    // a vendor's DllMain can then re-enter Shell initialization under loader lock.
    // Defer the entire item site, rather than only SetSite, until QCM returns.
    if (!count && FAILED(hr = attach(actual.Get()))) return hr;
    shell_extensions::MenuTiming query("native_selection.query");
    hr = actual->QueryContextMenu(menu, 0, firstCommand, lastCommand, flags);
    query.Record(SUCCEEDED(hr) ? "complete" : "failed");
    if (FAILED(hr)) return hr;
    if (count && FAILED(hr = attach(actual.Get()))) return hr;
    actual.CopyTo(contextMenu);
    total.Record("complete", count);
    return hr;
}

HWND ShellContextMenuSite::HostWindow() const
{
    return impl_->hostWindow;
}

bool ShellContextMenuSite::HasView() const { return impl_->browser && impl_->view; }

void ShellContextMenuSite::SetInvocationOwner(HWND owner)
{
    if (impl_->browser && owner && IsWindow(owner))
        impl_->browser->SetWindow(owner);
}

} // namespace snowdesktop

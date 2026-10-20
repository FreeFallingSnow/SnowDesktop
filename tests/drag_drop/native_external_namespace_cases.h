#pragma once

namespace native_external_namespace_cases
{
using namespace native_ole_file_cases;

// The external producer exposes an actual Shell CIDA for a known namespace
// item. Other Shell representations are withheld to define the pure-CIDA
// input; the optional DIB is a standard preview representation, not a file.
class Source final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDataObject, IDataObjectAsyncCapability>
{
public:
    ComPtr<IDataObject> shell;
    ComPtr<native_owned_content_cases::Source> preview;
    bool asynchronous = false, wrongApartment = false;
    DWORD owner = GetCurrentThreadId();
    int reads = 0, starts = 0, ends = 0;
    static CLIPFORMAT Cida() { return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_SHELLIDLIST)); }
    void CheckThread() { wrongApartment |= owner != GetCurrentThreadId(); }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override
    {
        CheckThread(); ++reads;
        if (!format || !medium) return E_POINTER;
        *medium = {};
        if (format->cfFormat == Cida()) return shell->GetData(format, medium);
        if (format->cfFormat == CF_DIB && preview) return preview->GetData(format, medium);
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override
    {
        CheckThread();
        if (!format) return E_POINTER;
        if (format->cfFormat == Cida()) return shell->QueryGetData(format);
        if (format->cfFormat == CF_DIB && preview) return preview->QueryGetData(format);
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override
    { CheckThread(); ++reads; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* output) override
    { if (output) output->ptd = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM* medium, BOOL release) override
    { CheckThread(); if (release && medium) ReleaseStgMedium(medium); return S_OK; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** output) override
    {
        if (direction != DATADIR_GET) return E_NOTIMPL;
        std::vector<FORMATETC> formats{{Cida(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}};
        if (preview) formats.push_back({CF_DIB, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
        return SHCreateStdEnumFmtEtc(static_cast<UINT>(formats.size()), formats.data(), output);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL enabled) override { asynchronous = enabled != FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* enabled) override
    { if (!enabled) return E_POINTER; *enabled = asynchronous; return S_OK; }
    HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx*) override { CheckThread(); ++starts; return S_OK; }
    HRESULT STDMETHODCALLTYPE InOperation(BOOL* active) override
    { if (!active) return E_POINTER; *active = starts > ends; return S_OK; }
    HRESULT STDMETHODCALLTYPE EndOperation(HRESULT, IBindCtx*, DWORD) override
    { CheckThread(); ++ends; return S_OK; }
};

inline ComPtr<Source> Make(bool asynchronous, bool image)
{
    const std::wstring parsing = std::wstring(L"::") + kDesktopIconClsidRecycleBin;
    PIDLIST_ABSOLUTE absolute = nullptr;
    Require(SUCCEEDED(SHParseDisplayName(parsing.c_str(), nullptr, &absolute, 0, nullptr)) && absolute,
        "D15 external fixture resolves the known Recycle Bin namespace without enumerating the desktop");
    Pidl item(absolute);
    std::array<wchar_t, MAX_PATH> physical{};
    Require(!SHGetPathFromIDListW(item.get(), physical.data()),
        "D15 external fixture is an actual PIDL namespace without a physical file path");
    Pidl parent(ILClone(item.get()));
    Require(parent.get() && ILRemoveLastID(parent.get()), "D15 namespace CIDA retains its real parent PIDL");
    PCUITEMID_CHILD child = ILFindLastID(item.get());
    auto source = Microsoft::WRL::Make<Source>();
    source->asynchronous = asynchronous;
    Require(SUCCEEDED(SHCreateDataObject(parent.get(), 1, &child, nullptr, IID_PPV_ARGS(&source->shell))),
        "D15 external fixture uses the real Shell namespace CIDA provider");
    if (image) { source->preview = Microsoft::WRL::Make<native_owned_content_cases::Source>(); source->preview->Image(); }
    FORMATETC cida{Source::Cida(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    FORMATETC hdrop{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    Require(source->QueryGetData(&cida) == S_OK && source->QueryGetData(&hdrop) != S_OK && source->reads == 0,
        "D15 fixture advertises actual CIDA and no file batch without rendering source data");
    return source;
}

inline void Run()
{
    native_drag_commit_cases::Profile profile;
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
    fs::create_directories(desktop);
    std::size_t scenes = 0;
    for (const bool grouped : {false, true})
    for (const bool image : {false, true})
    for (const bool asynchronous : {false, true})
    {
        native_drag_commit_cases::context = "D15 external Shell namespace grouped=" + std::to_string(grouped) +
            " image=" + std::to_string(image) + " async=" + std::to_string(asynchronous);
        const auto directory = profile.root / (L"external-namespace-" + std::to_wstring(scenes++));
        const auto mapped = directory / L"mapped", member = mapped / L"Existing.txt", ordinary = directory / L"Ordinary.txt";
        fs::create_directories(mapped);
        { std::ofstream stream(member, std::ios::binary); stream << "unchanged-mapped-member"; }
        { std::ofstream stream(ordinary, std::ios::binary); stream << "unchanged-ordinary-file"; }
        auto& app = profile.Model(false, directory / L"layout");
        Access::PopulateModel(app, {ordinary}, 0);
        Access::SeedExternalStorage(app, grouped ? DesktopWidgetType::FileGroup : DesktopWidgetType::FolderMapping,
            DesktopWidgetType::FolderMapping, false, member);
        Access::PrepareReload(app); Access::BindAsync(app);
        Require(Access::Save(app), "save the actual mapped destination before namespace ingress");
        const auto saved = Read(Access::LayoutPath(app));
        const auto widgets = Access::Widgets(app);
        const auto staged = native_owned_content_cases::Staged();
        auto source = Make(asynchronous, image);
        int menus = 0;
        Access::RightMenuBridge bridge(app, [&](HMENU, POINT, HWND) { ++menus; return 2u; });
        ComPtr<OleDragDropAdapter> adapter;
        adapter.Attach(new OleDragDropAdapter(&bridge));
        const POINT point = Access::NamespaceMappingPoint(app, grouped);
        constexpr DWORD allowed = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
        constexpr DWORD keys = MK_RBUTTON | MK_CONTROL;
        DWORD effect = allowed;
        Require(SUCCEEDED(adapter->DragEnter(source.Get(), keys, Access::ExternalPoint(point), &effect)),
            "actual OLE adapter dispatches the external namespace Enter");
        native_drag_commit_cases::context += " enter-effect=" + std::to_string(effect);
        Require(effect == DROPEFFECT_NONE,
            "D15: a pure Shell namespace over a physical mapping must reject on Enter before any release");
        effect = allowed;
        Require(SUCCEEDED(adapter->DragOver(keys, Access::ExternalPoint(point), &effect)) && effect == DROPEFFECT_NONE,
            "D15: repeated Over cannot re-admit a namespace or its preview image as a file");
        Require(source->reads == 0 && source->starts == 0 && source->ends == 0 && !source->wrongApartment,
            "D15: rejected namespace hover cannot render source formats or acquire an async lease");
        effect = allowed;
        Require(SUCCEEDED(adapter->Drop(source.Get(), MK_CONTROL, Access::ExternalPoint(point), &effect)) && effect == DROPEFFECT_NONE && menus == 0,
            "D15: even a forced right-button release cannot open an actionable namespace menu or commit a fallback");
        Require(source->reads == 0 && source->starts == 0 && source->ends == 0 && !source->wrongApartment &&
            Access::NoPendingFileDrop(app) && Access::FolderDropReadIdle(app) &&
            Outputs(mapped, {member}).empty() && Outputs(desktop, {}).empty() &&
            native_owned_content_cases::Staged() == staged && Read(member) == "unchanged-mapped-member" &&
            Read(ordinary) == "unchanged-ordinary-file" && Read(Access::LayoutPath(app)) == saved,
            "D15: rejected CIDA and bitmap preview leave source, mapping, desktop, staging and saved layout untouched");
        Require(Access::Widgets(app).size() == widgets.size(), "D15 namespace refusal cannot add or remove components");
        for (const auto& widget : widgets)
            Require(native_widget_pair_cases::Configuration(native_widget_pair_cases::Widget(app, widget.id)) ==
                native_widget_pair_cases::Configuration(widget),
                "D15 namespace refusal retains every group, child, setting and member identity");
        Require(Access::NoIngressPopup(app), "D15 external namespace test never constructs presentation windows");
        Access::StopModel(app);
    }
    std::cout << scenes << " actual Shell CIDA namespace sync/async, bitmap preview and mapped destination refusal scenes passed\n";
}
}

#pragma once

#include <shldisp.h>
#include <wrl/implements.h>
#include <array>

namespace ole_file_source_fixture
{
// Only the external Explorer provider is supplied. The wrapped Shell object
// remains on its original STA; production must marshal calls back to it.
class Source final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDataObject, IDataObjectAsyncCapability>
{
public:
    Microsoft::WRL::ComPtr<IDataObject> shell;
    DWORD owner = GetCurrentThreadId();
    bool asynchronous = true;
    bool wrongApartment = false, fileReadBeforeStart = false;
    int fileReads = 0, starts = 0, ends = 0;
    int dataReads = 0;
    HRESULT outcome = E_PENDING;
    DWORD endEffect = 0xffffffff;
    // A virtual multi-parent selection supplies a standard CF_HDROP batch.
    // Ordinary same-parent cases retain the real Shell provider unchanged.
    std::vector<std::filesystem::path> multiParentPaths;

    void CheckThread() { wrongApartment |= GetCurrentThreadId() != owner; }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override
    {
        CheckThread();
        ++dataReads;
        if (format && format->cfFormat == CF_HDROP)
        { ++fileReads; fileReadBeforeStart |= asynchronous && starts == 0; }
        if (format && format->cfFormat == CF_HDROP && !multiParentPaths.empty())
        {
            if (!medium || !(format->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
            std::wstring names;
            for (const auto& path : multiParentPaths) { names += path.wstring(); names.push_back(L'\0'); }
            names.push_back(L'\0');
            const auto bytes = sizeof(DROPFILES) + names.size() * sizeof(wchar_t);
            HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
            if (!memory) return E_OUTOFMEMORY;
            auto* drop = static_cast<DROPFILES*>(GlobalLock(memory));
            if (!drop) { GlobalFree(memory); return E_OUTOFMEMORY; }
            drop->pFiles = sizeof(DROPFILES); drop->fWide = TRUE;
            memcpy(reinterpret_cast<std::byte*>(drop) + sizeof(DROPFILES), names.data(), names.size() * sizeof(wchar_t));
            GlobalUnlock(memory);
            *medium = {}; medium->tymed = TYMED_HGLOBAL; medium->hGlobal = memory;
            return S_OK;
        }
        return shell->GetData(format, medium);
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC* f, STGMEDIUM* m) override
    { CheckThread(); ++dataReads; return shell->GetDataHere(f, m); }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* f) override
    { CheckThread(); return shell->QueryGetData(f); }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC* f, FORMATETC* out) override
    { CheckThread(); return shell->GetCanonicalFormatEtc(f, out); }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* f, STGMEDIUM* m, BOOL release) override
    { CheckThread(); return shell->SetData(f, m, release); }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override
    { CheckThread(); return shell->EnumFormatEtc(direction, out); }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL enabled) override { asynchronous = enabled != FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* enabled) override
    { if (!enabled) return E_POINTER; *enabled = asynchronous; return S_OK; }
    HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx*) override { CheckThread(); ++starts; return S_OK; }
    HRESULT STDMETHODCALLTYPE InOperation(BOOL* active) override
    { if (!active) return E_POINTER; *active = starts > ends; return S_OK; }
    HRESULT STDMETHODCALLTYPE EndOperation(HRESULT result, IBindCtx*, DWORD effect) override
    { CheckThread(); ++ends; outcome = result; endEffect = effect; return S_OK; }
};

inline Microsoft::WRL::ComPtr<Source> Make(const std::vector<std::filesystem::path>& files, bool asynchronous)
{
    auto source = Microsoft::WRL::Make<Source>();
    source->asynchronous = asynchronous;
    auto nativeParent = files.front().parent_path();
    nativeParent.make_preferred();
    Pidl parent(ILCreateFromPathW(nativeParent.c_str()));
    if (!parent.get()) throw std::runtime_error("native OLE fixture source parent PIDL is missing");
    std::vector<Pidl> absolute;
    std::vector<PCUITEMID_CHILD> children;
    for (const auto& file : files)
    {
        auto native = file;
        native.make_preferred();
        absolute.emplace_back(ILCreateFromPathW(native.c_str()));
        if (!absolute.back().get()) throw std::runtime_error("native OLE fixture selected file PIDL is missing");
        children.push_back(ILFindLastID(absolute.back().get()));
    }
    if (FAILED(SHCreateDataObject(parent.get(), static_cast<UINT>(children.size()), children.data(),
            nullptr, IID_PPV_ARGS(&source->shell))))
        throw std::runtime_error("create real Shell data object for native OLE fixture");
    return source;
}
}

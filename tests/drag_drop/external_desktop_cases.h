#pragma once

#include "drag_drop/delayed_desktop_file_drop.h"
#include "drag_drop/file_backed_drop_request.h"
#include <shlobj.h>
#include <shldisp.h>
#include <wrl/implements.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace external_desktop_cases
{
// The oracle is the explicit operation named by the gesture, constrained by
// the source's OLE effects. Columns are NONE,COPY,MOVE,COPY|MOVE,LINK,... .
// No expected value is obtained from production key conversion or routing.
struct Gesture
{
    const char* name;
    DWORD keys;
    std::array<DWORD, 8> effects;
};
inline constexpr Gesture gestures[]{
    {"Shift/move", MK_SHIFT, {0, 0, 2, 2, 0, 0, 2, 2}},
    {"Ctrl/copy", MK_CONTROL, {0, 1, 0, 1, 0, 1, 0, 1}},
    {"Alt/shortcut", MK_ALT, {0, 0, 0, 0, 4, 4, 4, 4}},
    {"Ctrl+Shift/shortcut", MK_CONTROL | MK_SHIFT, {0, 0, 0, 0, 4, 4, 4, 4}},
};

inline void Require(bool passed, const char* message)
{
    if (!passed) throw std::runtime_error(message);
}

// Only the Explorer provider is substituted. Paths/PIDL extraction, routing,
// file requests, Shell execution, persisted links and output records are real.
// This fixture does not replace the DesktopApp window/message loop or prove
// its UI completion/placement wiring; that remaining boundary is documented.
class DelayedSource final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    IDataObject, IDataObjectAsyncCapability>
{
public:
    Microsoft::WRL::ComPtr<IDataObject> shell;
    bool offer = true;
    int reads = 0, metadataReads = 0;
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* f, STGMEDIUM* m) override
    {
        if (f && f->cfFormat == RegisterClipboardFormatW(CFSTR_SHELLIDLIST)) ++metadataReads;
        else ++reads;
        return shell->GetData(f, m);
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC* f, STGMEDIUM* m) override
    { return shell->GetDataHere(f, m); }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* f) override
    { return offer ? shell->QueryGetData(f) : DV_E_FORMATETC; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC* f, FORMATETC* out) override
    { return shell->GetCanonicalFormatEtc(f, out); }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* f, STGMEDIUM* m, BOOL release) override
    { return shell->SetData(f, m, release); }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override
    { return shell->EnumFormatEtc(direction, out); }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override
    { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* enabled) override
    { if (!enabled) return E_POINTER; *enabled = TRUE; return S_OK; }
    HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE InOperation(BOOL* active) override
    { if (!active) return E_POINTER; *active = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE EndOperation(HRESULT, IBindCtx*, DWORD) override { return S_OK; }
};

inline void Run(const std::filesystem::path& root)
{
    using namespace snowdesktop;
    size_t serial = 0;
    for (const auto& gesture : gestures)
    for (const DWORD allowed : {7u, 0u, 1u, 2u, 3u, 4u, 5u, 6u})
    for (const size_t count : {size_t{1}, size_t{2}})
    {
        const auto directory = root / (L"dispatch-" + std::to_wstring(serial++));
        const auto from = directory / L"source";
        const auto to = directory / L"desktop";
        std::filesystem::create_directories(from);
        std::filesystem::create_directories(to);
        const std::vector<std::filesystem::path> paths{from / L"document.txt", from / L"folder"};
        std::ofstream(paths[0]) << "document content";
        if (count == 2)
        {
            std::filesystem::create_directory(paths[1]);
            std::ofstream(paths[1] / L"nested.txt") << "directory content";
        }
        PIDLIST_ABSOLUTE parent = ILCreateFromPathW(from.c_str());
        Require(parent != nullptr, "create real source directory PIDL");
        std::vector<PIDLIST_ABSOLUTE> absolute;
        std::vector<PCUITEMID_CHILD> children;
        for (size_t index = 0; index < count; ++index)
        {
            auto pidl = ILCreateFromPathW(paths[index].c_str());
            Require(pidl != nullptr, "create real source item PIDL");
            absolute.push_back(pidl);
            children.push_back(ILFindLastID(pidl));
        }
        auto source = Microsoft::WRL::Make<DelayedSource>();
        const auto created = SHCreateDataObject(parent, static_cast<UINT>(count), children.data(),
            nullptr, IID_PPV_ARGS(&source->shell));
        for (auto pidl : absolute) CoTaskMemFree(pidl);
        CoTaskMemFree(parent);
        Require(SUCCEEDED(created), "create real CF_HDROP provider");
        const DWORD expected = gesture.effects[allowed];
        int submissions = 0;
        auto result = std::make_shared<ShellFileOperationResult>();
        const auto handled = delayed_desktop_file_drop::Dispatch(source.Get(), true, true,
            gesture.keys, allowed, [&](DWORD selected) {
                ++submissions;
                Require(source->reads == 0, "delayed-file dispatch never renders source paths while probing");
                const auto files = external_drop_content::ReadFilePaths(source.Get());
                const auto request = file_backed_drop::BuildRequest(files, to.wstring(), selected, result);
                Require(request && ShellFileOperationWorker::Execute(*request), "dispatched operation executes real Shell request");
                return selected;
            });
        Require(source->metadataReads > 0, "probe reads Shell selection metadata without rendering file contents");
        Require(handled.has_value(), "delayed file desktop drop is handled by its production dispatch route");
        // Evaluate the user's original Shift+Explorer failure through real
        // source/destination state before checking the reported effect.
        for (size_t index = 0; index < count; ++index)
            Require(std::filesystem::exists(paths[index]) == (expected != DROPEFFECT_MOVE),
                "selected desktop move removes the original; copy/link/rejection preserve it");
        Require(*handled == expected, "desktop dispatch preserves the explicitly selected effect");
        Require(submissions == (expected ? 1 : 0), "accepted batch submits once; rejected effects submit nothing");
        Require(result->outputs.size() == (expected ? count : 0), "outputs contain exactly the committed selection");
        if (!expected)
            Require(std::filesystem::is_empty(to) && source->reads == 0,
                "forbidden effects neither read content nor create destination files");
        for (size_t index = 0; index < count; ++index)
        {
            Require(std::filesystem::exists(paths[index]) == (expected != DROPEFFECT_MOVE),
                "only a selected, offered move removes original files and directories");
            if (!expected) continue;
            const auto found = std::find_if(result->outputs.begin(), result->outputs.end(), [&](const auto& output) {
                return _wcsicmp(output.source.c_str(), paths[index].c_str()) == 0;
            });
            Require(found != result->outputs.end(), "each selected source has its own actual output record");
            const auto& output = *found;
            Require(std::filesystem::equivalent(std::filesystem::path(output.destination).parent_path(), to),
                "all actual outputs remain in this operation's destination");
            if (expected == DROPEFFECT_LINK)
            {
                Microsoft::WRL::ComPtr<IShellLinkW> link;
                Microsoft::WRL::ComPtr<IPersistFile> persist;
                Require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(&link))) && SUCCEEDED(link.As(&persist)) &&
                    SUCCEEDED(persist->Load(output.destination.c_str(), STGM_READ)), "created shortcut is readable");
                std::array<wchar_t, 32768> target{};
                Require(SUCCEEDED(link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, SLGP_RAWPATH)) &&
                    std::filesystem::equivalent(target.data(), output.source), "shortcut points to its selected original");
            }
            else
            {
                const auto payload = index == 0 ? std::filesystem::path(output.destination) :
                    std::filesystem::path(output.destination) / L"nested.txt";
                std::ifstream stream(payload);
                const std::string bytes{std::istreambuf_iterator<char>(stream), {}};
                Require(bytes == (index == 0 ? "document content" : "directory content"),
                    "copy/move preserves actual document and nested directory bytes");
            }
        }
        const auto noSubmit = [](DWORD) -> DWORD { throw std::runtime_error("unrelated route was submitted"); };
        Require(!delayed_desktop_file_drop::Dispatch(source.Get(), false, true, gesture.keys, allowed, noSubmit),
            "non-desktop targets retain their existing routes");
        Require(!delayed_desktop_file_drop::Dispatch(source.Get(), true, false, gesture.keys, allowed, noSubmit),
            "synchronous sources retain their existing routes");
        source->offer = false;
        Require(!delayed_desktop_file_drop::Dispatch(source.Get(), true, true, gesture.keys, allowed, noSubmit),
            "non-file payloads retain their existing routes");
    }
}
}

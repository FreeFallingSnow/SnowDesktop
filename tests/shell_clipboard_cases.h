#pragma once

#include <shlobj.h>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace shell_clipboard_cases
{
inline void Check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

// The provider boundary deliberately has no CF_HDROP. Namespace mode delegates
// PIDLs to a real Shell object; virtual mode offers descriptors and stream bytes.
// Neither mode replaces the production marshal, worker, or destination Shell.
class DataObject final : public IDataObject
{
public:
    explicit DataObject(Microsoft::WRL::ComPtr<IDataObject> shell) : shell_(std::move(shell)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override
    {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDataObject) return E_NOINTERFACE;
        *value = static_cast<IDataObject*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG count = --references_;
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override
    {
        if (!format) return E_POINTER;
        if (format->cfFormat == CF_HDROP) return DV_E_FORMATETC;
        if (shell_) return shell_->QueryGetData(format);
        if (format->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
        if (format->cfFormat == descriptor_ && format->lindex == -1 &&
            (format->tymed & TYMED_HGLOBAL)) return S_OK;
        if (format->cfFormat == contents_ && format->lindex == 0 &&
            (format->tymed & TYMED_ISTREAM)) return S_OK;
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override
    {
        if (!medium) return E_POINTER;
        *medium = {};
        const auto offered = QueryGetData(format);
        if (FAILED(offered)) return offered;
        if (shell_) return shell_->GetData(format, medium);
        if (format->cfFormat == descriptor_)
        {
            FILEGROUPDESCRIPTORW group{};
            group.cItems = 1;
            group.fgd[0].dwFlags = FD_FILESIZE | FD_ATTRIBUTES;
            group.fgd[0].dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
            group.fgd[0].nFileSizeLow = sizeof(payload) - 1;
            wcscpy_s(group.fgd[0].cFileName, L"virtual-phone.txt");
            HGLOBAL storage = GlobalAlloc(GMEM_MOVEABLE, sizeof(group));
            if (!storage) return E_OUTOFMEMORY;
            void* bytes = GlobalLock(storage);
            if (!bytes) { GlobalFree(storage); return E_OUTOFMEMORY; }
            std::memcpy(bytes, &group, sizeof(group));
            GlobalUnlock(storage);
            medium->tymed = TYMED_HGLOBAL;
            medium->hGlobal = storage;
            return S_OK;
        }
        Microsoft::WRL::ComPtr<IStream> stream;
        HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
        if (FAILED(hr)) return hr;
        hr = stream->Write(payload, sizeof(payload) - 1, nullptr);
        if (FAILED(hr)) return hr;
        LARGE_INTEGER start{};
        hr = stream->Seek(start, STREAM_SEEK_SET, nullptr);
        if (FAILED(hr)) return hr;
        medium->tymed = TYMED_ISTREAM;
        medium->pstm = stream.Detach();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* output) override
    { if (output) output->ptd = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* f, STGMEDIUM* m, BOOL release) override
    { return shell_ ? shell_->SetData(f, m, release) : E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (direction != DATADIR_GET) return E_NOTIMPL;
        FORMATETC formats[] = {
            {shell_ ? static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"Shell IDList Array")) : descriptor_,
             nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
            {contents_, nullptr, DVASPECT_CONTENT, 0, TYMED_ISTREAM}};
        return SHCreateStdEnumFmtEtc(shell_ ? 1 : 2, formats, result);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override
    { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
    static constexpr char payload[] = "virtual phone clipboard payload";
private:
    std::atomic<ULONG> references_{1};
    Microsoft::WRL::ComPtr<IDataObject> shell_;
    CLIPFORMAT descriptor_ = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW"));
    CLIPFORMAT contents_ = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents"));
};

inline void Run(const std::filesystem::path& root)
{
    using namespace snowdesktop;
    using Microsoft::WRL::ComPtr;
    const auto fixture = root / L"clipboard";
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{fixture};
    const auto source = fixture / L"source";
    std::filesystem::create_directories(source / L"folder");
    { std::ofstream(source / L"namespace.txt") << "namespace payload"; }
    { std::ofstream(source / L"folder" / L"child.txt") << "nested payload"; }
    const auto pathTarget = fixture / L"path-target";
    std::filesystem::create_directory(pathTarget);
    for (size_t paste = 1; paste <= 2; ++paste)
    {
        ShellDropRequest request;
        request.sources = {(source / L"namespace.txt").wstring(), (source / L"folder").wstring()};
        request.targetParsingName = pathTarget.wstring();
        request.allowedEffects = DROPEFFECT_COPY;
        request.keyState = ClipboardShellDropKeyState(DROPEFFECT_COPY);
        request.clipboardPaste = true;
        HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Check(finished != nullptr, "create local clipboard completion event");
        struct Event { HANDLE handle; ~Event() { CloseHandle(handle); } } event{finished};
        ShellFileOperationWorker worker;
        std::atomic<bool> succeeded{false};
        Check(worker.Enqueue(std::move(request), [&](bool ok) {
            // Inspect at the actual worker completion, before another paste or
            // any UI refresh can make a prematurely reported copy look correct.
            size_t files = 0, folders = 0;
            for (const auto& entry : std::filesystem::directory_iterator(pathTarget))
            {
                const auto file = entry.is_directory() ? entry.path() / L"child.txt" : entry.path();
                std::ifstream input(file, std::ios::binary);
                const std::string bytes(std::istreambuf_iterator<char>(input), {});
                if (entry.is_directory() && bytes == "nested payload") ++folders;
                if (!entry.is_directory() && bytes == "namespace payload") ++files;
            }
            succeeded = ok && files == paste && folders == paste &&
                std::filesystem::exists(source / L"namespace.txt") &&
                std::filesystem::exists(source / L"folder" / L"child.txt");
            SetEvent(finished);
        }), "queue local clipboard paste on the production worker");
        DWORD index = 0;
        const HRESULT waited = CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES,
            15000, 1, &finished, &index);
        Check(SUCCEEDED(waited) && succeeded,
            "first paste finishes before completion; repeated paste preserves both batches without a conflict prompt");
        worker.Stop();
    }
    for (const bool virtualFiles : {false, true})
    {
        const auto target = fixture / (virtualFiles ? L"virtual-target" : L"namespace-target");
        std::filesystem::create_directory(target);
        ComPtr<IDataObject> shell;
        if (!virtualFiles)
        {
            PIDLIST_ABSOLUTE parent = nullptr, first = nullptr, second = nullptr;
            const HRESULT parentHr = SHParseDisplayName(source.c_str(), nullptr, &parent, 0, nullptr);
            const HRESULT firstHr = SHParseDisplayName((source / L"namespace.txt").c_str(), nullptr, &first, 0, nullptr);
            const HRESULT secondHr = SHParseDisplayName((source / L"folder").c_str(), nullptr, &second, 0, nullptr);
            HRESULT createHr = E_FAIL;
            if (SUCCEEDED(parentHr) && SUCCEEDED(firstHr) && SUCCEEDED(secondHr))
            {
                PCUITEMID_CHILD children[] = {ILFindLastID(first), ILFindLastID(second)};
                createHr = SHCreateDataObject(parent, 2, children, nullptr, IID_PPV_ARGS(&shell));
            }
            CoTaskMemFree(parent); CoTaskMemFree(first); CoTaskMemFree(second);
            Check(SUCCEEDED(createHr), "create real namespace clipboard fixture");
        }
        ComPtr<IDataObject> data;
        data.Attach(new DataObject(std::move(shell)));
        FORMATETC paths{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        Check(data->QueryGetData(&paths) == DV_E_FORMATETC,
            "Shell handoff fixture must not supply ordinary file paths");
        ShellDropRequest request;
        Check(SUCCEEDED(CoMarshalInterThreadInterfaceInStream(IID_IDataObject, data.Get(),
            &request.marshaledDataObject)), "retain original clipboard object across apartments");
        request.targetParsingName = target.wstring();
        request.keyState = ClipboardShellDropKeyState(DROPEFFECT_COPY);
        request.allowedEffects = DROPEFFECT_COPY;
        HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Check(finished != nullptr, "create clipboard completion event");
        struct Event { HANDLE handle; ~Event() { CloseHandle(handle); } } event{finished};
        ShellFileOperationWorker worker;
        std::atomic<bool> succeeded{false};
        Check(worker.Enqueue(std::move(request), [&](bool ok) { succeeded = ok; SetEvent(finished); }),
            "queue original clipboard data on the production Shell worker");
        DWORD index = 0;
        const HRESULT waited = CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES,
            15000, 1, &finished, &index);
        Check(SUCCEEDED(waited) && succeeded, "Shell copies non-path clipboard content within deadline");
        worker.Stop();
        const auto read = [](const auto& path) { std::ifstream in(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(in), {}); };
        if (virtualFiles)
            Check(read(target / L"virtual-phone.txt") == DataObject::payload &&
                std::distance(std::filesystem::directory_iterator(target), std::filesystem::directory_iterator{}) == 1,
                "virtual FileContents are copied exactly once with intact bytes");
        else
            Check(read(target / L"namespace.txt") == "namespace payload" &&
                read(target / L"folder" / L"child.txt") == "nested payload" &&
                read(source / L"namespace.txt") == "namespace payload" &&
                read(source / L"folder" / L"child.txt") == "nested payload" &&
                std::distance(std::filesystem::directory_iterator(target), std::filesystem::directory_iterator{}) == 2,
                "namespace file/folder copy preserves hierarchy, bytes, counts and sources");
    }
}
}

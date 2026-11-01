#pragma once

#include "drag_drop/external_transfer_observation.h"

#include <shldisp.h>
#include <shlobj.h>
#include <shellapi.h>
#include <wrl/implements.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ole_file_source_fixture
{
enum class FileDropCapture
{
    captured,
    provider_failed,
    missing_or_wrong_medium,
    invalid_hglobal,
    invalid_dropfiles,
    path_query_failed,
};
struct FileDropRead
{
    HRESULT result = E_PENDING;
    FileDropCapture capture = FileDropCapture::provider_failed;
    DWORD threadId = 0;
    int startsAtEntry = 0;
    int endsAtEntry = 0;
    std::vector<std::filesystem::path> paths;
};
enum class FileDropCallPhase { entered, returned, unwound };
struct FileDropCallEvent
{
    std::uint64_t call = 0, parent = 0;
    std::size_t depth = 0;
    DWORD thread = 0;
    FileDropCallPhase phase = FileDropCallPhase::entered;
    HRESULT result = E_PENDING; // meaningful only for returned
};
struct FileDropCallState
{
    std::size_t count = 0, depth = 0;
    DWORD owner = 0;
    bool failed = false;
};
struct FormatProbe
{
    bool hasFormat = false;
    CLIPFORMAT format = 0;
    DWORD aspect = 0;
    LONG index = 0;
    DWORD tymed = 0;
    bool targetDevicePresent = false;
    HRESULT result = E_PENDING;
    DWORD threadId = 0;
};
// The fixture wraps a Shell IDataObject created from local filesystem items;
// it does not establish the identity of an external source application. The
// wrapped object remains on its original STA, so production must marshal
// calls back to it.
class Source final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDataObject, IDataObjectAsyncCapability>
{
public:
    struct LifetimeToken final {};
    // Identity is separate from COM ownership. A retained token does not keep
    // this Source alive; compare it with a currently owned Source token, not
    // with an address alone or an assumption that weak expiration is timely.
    std::weak_ptr<const LifetimeToken> LifetimeIdentity() const noexcept { return lifetimeIdentity_; }
    // Exact ordered paths passed to PIDL construction by Make. This immutable
    // input record does not query IDataObject or prove what the host read.
    const std::vector<std::filesystem::path>& ConstructionPaths() const noexcept { return constructionPaths_; }
    bool HasConstructionShell() const noexcept { return shell_.Get() != nullptr; }
    // Select a finite file-only provider before any use. This Source remains
    // the IDataObject seen by the host and delegates allowed requests to its
    // real Shell object; it does not choose a drop effect or destination.
    bool EnableRestrictedFileDrop() noexcept
    {
        if (restrictedFileDrop_ || GetCurrentThreadId() != owner || !shell_ || !asynchronous ||
            constructionPaths_.size() != 1 || !multiParentPaths.empty() || wrongApartment ||
            fileReads || dataReads || identityReads || starts || ends || !fileDropReads.empty() ||
            !formatProbes.empty() || fileDropObservationFailed || formatProbeObservationFailed) return false;
        restrictedFileDrop_ = true; return true;
    }
    bool RestrictedFileDrop() const noexcept { return restrictedFileDrop_ && GetCurrentThreadId() == owner; }
    // The sole setup probe is retained as prefix 0; later host probes are
    // never cleared or represented as this preflight call.
    bool ProbeRestrictedFileDrop() noexcept
    {
        if (!RestrictedFileDrop() || restrictedPreflightAttempted_ || !formatProbes.empty() || fileReads || dataReads ||
            starts || ends || wrongApartment || formatProbeObservationFailed) return false;
        restrictedPreflightAttempted_ = true;
        FORMATETC request{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        try
        {
            // A real Shell COM call can reenter fixture teardown. Keep this
            // exact source alive until the caller revalidates its owner.
            Microsoft::WRL::ComPtr<IDataObject> held(static_cast<IDataObject*>(this));
            const HRESULT result = QueryGetData(&request);
            restrictedPreflightSucceeded_ = result == S_OK && formatProbes.size() == 1 && RestrictedPreflightIntact();
            return restrictedPreflightSucceeded_;
        }
        catch (...) { return false; }
    }
    bool RestrictedPreflightIntact() const noexcept
    {
        if (!RestrictedFileDrop() || !restrictedPreflightAttempted_ || formatProbeObservationFailed || formatProbes.empty())
            return false;
        const auto& probe = formatProbes.front();
        return probe.threadId == owner && probe.hasFormat && probe.format == CF_HDROP && probe.aspect == DVASPECT_CONTENT &&
            probe.index == -1 && probe.tymed == TYMED_HGLOBAL && !probe.targetDevicePresent && probe.result == S_OK;
    }
    bool RestrictedPreflightSucceeded() const noexcept { return restrictedPreflightSucceeded_ && RestrictedPreflightIntact(); }
    // These observations never query the provider. A failed/overflowed log or
    // a call from outside the owner STA cannot prove a closed absence window.
    bool ReadFileDropCallState(FileDropCallState& output) const noexcept
    {
        const DWORD saved = GetLastError();
        const bool ownerThread = GetCurrentThreadId() == owner;
        if (ownerThread) output = {fileDropCallCount_, fileDropCallDepth_, owner, fileDropCallFailed_.load()};
        SetLastError(saved);
        return ownerThread;
    }
    bool ReadFileDropCallEvent(std::size_t index, FileDropCallEvent& output) const noexcept
    {
        const DWORD saved = GetLastError();
        const bool ok = GetCurrentThreadId() == owner && index < fileDropCallCount_;
        if (ok) output = fileDropCalls_[index];
        SetLastError(saved);
        return ok;
    }
    DWORD owner = GetCurrentThreadId();
    bool asynchronous = true;
    bool wrongApartment = false, fileReadBeforeStart = false;
    int fileReads = 0, starts = 0, ends = 0;
    int dataReads = 0;
    int identityReads = 0;
    HRESULT outcome = E_PENDING;
    DWORD endEffect = 0xffffffff;
    // Ordered CF_HDROP GetData calls and paths read from the returned medium.
    // If recording or parsing fails, fileDropObservationFailed stays true;
    // it is independent of the provider's HRESULT and source lease outcome.
    std::vector<FileDropRead> fileDropReads;
    bool fileDropObservationFailed = false;
    // Query calls are recorded in order. Restricted mode has one explicit
    // setup probe at index 0; later entries may come from production.
    // Absence from this list proves nothing about unqueried formats.
    std::vector<FormatProbe> formatProbes;
    bool formatProbeObservationFailed = false;
    // Exact requests made to the restricted provider, separate from Query.
    std::vector<FormatProbe> restrictedGets;
    bool restrictedGetObservationFailed = false;
    // A virtual multi-parent selection supplies a standard CF_HDROP batch.
    // Ordinary same-parent cases retain the real Shell provider unchanged.
    std::vector<std::filesystem::path> multiParentPaths;

    void CheckThread() { wrongApartment |= GetCurrentThreadId() != owner; }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override
    {
        CheckThread();
        if (restrictedFileDrop_)
        {
            const bool fileDrop = format && format->cfFormat == CF_HDROP;
            FileDropCallScope observedCall(*this, fileDrop);
            const DWORD thread = GetCurrentThreadId(); const int startsAtEntry = starts, endsAtEntry = ends;
            ++dataReads;
            if (fileDrop) { ++fileReads; fileReadBeforeStart |= asynchronous && starts == 0; }
            HRESULT result = !format || !medium ? E_POINTER : RestrictedRequest(format);
            if (result == S_OK)
            {
                FORMATETC allowed = *format; allowed.tymed = TYMED_HGLOBAL;
                result = shell_->GetData(&allowed, medium);
            }
            if (fileDrop) ObserveFileDrop(result, medium, thread, startsAtEntry, endsAtEntry);
            RecordRestrictedGet(format, result, thread);
            return observedCall.Return(result);
        }
        // CIDA is selection metadata, not delayed file content. Keep the
        // content-read assertions strict while observing identity probes.
        const bool identity = format && format->cfFormat ==
            RegisterClipboardFormatW(CFSTR_SHELLIDLIST);
        const bool fileDrop = format && format->cfFormat == CF_HDROP;
        FileDropCallScope observedCall(*this, fileDrop);
        const DWORD readThread = fileDrop ? GetCurrentThreadId() : 0;
        const int startsAtEntry = starts;
        const int endsAtEntry = ends;
        if (identity) ++identityReads;
        else ++dataReads;
        if (identity && !multiParentPaths.empty()) return DV_E_FORMATETC;
        if (fileDrop)
        { ++fileReads; fileReadBeforeStart |= asynchronous && starts == 0; }
        HRESULT result = E_UNEXPECTED;
        if (fileDrop && !multiParentPaths.empty())
        {
            if (!medium || !(format->tymed & TYMED_HGLOBAL)) result = DV_E_TYMED;
            else
            {
                std::wstring names;
                for (const auto& path : multiParentPaths) { names += path.wstring(); names.push_back(L'\0'); }
                names.push_back(L'\0');
                const auto bytes = sizeof(DROPFILES) + names.size() * sizeof(wchar_t);
                HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
                if (!memory) result = E_OUTOFMEMORY;
                else if (auto* drop = static_cast<DROPFILES*>(GlobalLock(memory)))
                {
                    drop->pFiles = sizeof(DROPFILES); drop->fWide = TRUE;
                    memcpy(reinterpret_cast<std::byte*>(drop) + sizeof(DROPFILES), names.data(), names.size() * sizeof(wchar_t));
                    GlobalUnlock(memory);
                    *medium = {}; medium->tymed = TYMED_HGLOBAL; medium->hGlobal = memory;
                    result = S_OK;
                }
                else { GlobalFree(memory); result = E_OUTOFMEMORY; }
            }
        }
        else result = shell_->GetData(format, medium);
        if (fileDrop) ObserveFileDrop(result, medium, readThread, startsAtEntry, endsAtEntry);
        return observedCall.Return(result);
    }
    // Observe only the medium actually returned by the production GetData
    // call. No GetData request is made by the observer. Locking is read-only,
    // and every observation failure leaves result and STGMEDIUM untouched.
    void ObserveFileDrop(HRESULT result, const STGMEDIUM* medium, DWORD readThread,
        int startsAtEntry, int endsAtEntry) noexcept
    {
        try
        {
            FileDropRead read;
            read.result = result;
            read.threadId = readThread;
            read.startsAtEntry = startsAtEntry;
            read.endsAtEntry = endsAtEntry;
            if (FAILED(result)) { fileDropReads.push_back(std::move(read)); return; }
            if (!medium || medium->tymed != TYMED_HGLOBAL || !medium->hGlobal)
                read.capture = FileDropCapture::missing_or_wrong_medium;
            else
            {
                const auto size = GlobalSize(medium->hGlobal);
                const auto* bytes = static_cast<const std::byte*>(GlobalLock(medium->hGlobal));
                if (!bytes) read.capture = FileDropCapture::invalid_hglobal;
                else
                {
                    struct Unlock { HGLOBAL handle; ~Unlock() { GlobalUnlock(handle); } } unlock{medium->hGlobal};
                    DROPFILES header{};
                    if (size >= sizeof(header)) std::memcpy(&header, bytes, sizeof(header));
                    const auto width = header.fWide ? sizeof(wchar_t) : std::size_t{1};
                    bool terminated = false, previousZero = false, sawContent = false;
                    std::size_t entries = 0;
                    if (size >= sizeof(header) && header.pFiles >= sizeof(header) && header.pFiles < size)
                    {
                        for (std::size_t at = header.pFiles; at <= size && size - at >= width; at += width)
                        {
                            wchar_t code = 0;
                            std::memcpy(&code, bytes + at, width);
                            const bool zero = code == 0;
                            sawContent |= !zero;
                            if (zero && previousZero) { terminated = true; break; }
                            if (zero && !previousZero) ++entries;
                            previousZero = zero;
                        }
                    }
                    if (!terminated) read.capture = FileDropCapture::invalid_dropfiles;
                    else
                    {
                        const auto drop = reinterpret_cast<HDROP>(medium->hGlobal);
                        const UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
                        // The empty double-NUL list has no entries.
                        if (!sawContent && entries == 1 && count == 0) entries = 0;
                        if (count != entries) read.capture = FileDropCapture::path_query_failed;
                        else
                        {
                            read.capture = FileDropCapture::captured;
                            for (UINT i = 0; i < count; ++i)
                            {
                                const UINT length = DragQueryFileW(drop, i, nullptr, 0);
                                if (!length || length == (std::numeric_limits<UINT>::max)())
                                { read.capture = FileDropCapture::path_query_failed; break; }
                                std::vector<wchar_t> path(static_cast<std::size_t>(length) + 1);
                                if (DragQueryFileW(drop, i, path.data(), length + 1) != length)
                                { read.capture = FileDropCapture::path_query_failed; break; }
                                read.paths.emplace_back(path.data());
                            }
                        }
                    }
                }
            }
            if (read.capture != FileDropCapture::captured) fileDropObservationFailed = true;
            fileDropReads.push_back(std::move(read));
        }
        catch (...) { fileDropObservationFailed = true; }
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC* f, STGMEDIUM* m) override
    { CheckThread(); ++dataReads; return restrictedFileDrop_ ? E_NOTIMPL : shell_->GetDataHere(f, m); }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* f) override
    {
        CheckThread();
        FormatProbe probe;
        probe.threadId = GetCurrentThreadId();
        if (f)
        {
            probe.hasFormat = true;
            probe.format = f->cfFormat;
            probe.aspect = f->dwAspect;
            probe.index = f->lindex;
            probe.tymed = f->tymed;
            probe.targetDevicePresent = f->ptd != nullptr;
        }
        HRESULT result = restrictedFileDrop_ ? (!f ? E_POINTER : RestrictedRequest(f)) : E_PENDING;
        if (!restrictedFileDrop_) result = shell_->QueryGetData(f);
        else if (result == S_OK) { FORMATETC allowed = *f; allowed.tymed = TYMED_HGLOBAL; result = shell_->QueryGetData(&allowed); }
        probe.result = result;
        try { formatProbes.push_back(probe); }
        catch (...) { formatProbeObservationFailed = true; }
        return result;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC* f, FORMATETC* out) override
    { CheckThread(); return restrictedFileDrop_ ? E_NOTIMPL : shell_->GetCanonicalFormatEtc(f, out); }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* f, STGMEDIUM* m, BOOL release) override
    { CheckThread(); return restrictedFileDrop_ ? E_NOTIMPL : shell_->SetData(f, m, release); }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override
    {
        CheckThread();
        if (!restrictedFileDrop_) return shell_->EnumFormatEtc(direction, out);
        if (!out) return E_POINTER;
        *out = nullptr;
        if (direction != DATADIR_GET) return E_NOTIMPL;
        FORMATETC allowed{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        return SHCreateStdEnumFmtEtc(1, &allowed, out);
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
    HRESULT STDMETHODCALLTYPE EndOperation(HRESULT result, IBindCtx*, DWORD effect) override
    { CheckThread(); ++ends; outcome = result; endEffect = effect; return S_OK; }
private:
    static HRESULT RestrictedRequest(const FORMATETC* format) noexcept
    {
        if (!format) return E_POINTER;
        if (format->cfFormat != CF_HDROP || format->ptd || format->dwAspect != DVASPECT_CONTENT || format->lindex != -1)
            return DV_E_FORMATETC;
        // FORMATETC::tymed is an acceptable-medium mask; only HGLOBAL is
        // delegated and returned even if the caller includes other bits.
        return (format->tymed & TYMED_HGLOBAL) ? S_OK : DV_E_TYMED;
    }
    void RecordRestrictedGet(const FORMATETC* format, HRESULT result, DWORD thread) noexcept
    {
        const DWORD saved = GetLastError();
        try
        {
            if (restrictedGets.size() >= snowdesktop::external_transfer_observation::EventLimit())
                restrictedGetObservationFailed = true;
            else
            {
                FormatProbe item; item.threadId = thread; item.result = result;
                if (format) { item.hasFormat = true; item.format = format->cfFormat; item.aspect = format->dwAspect;
                    item.index = format->lindex; item.tymed = format->tymed; item.targetDevicePresent = format->ptd != nullptr; }
                restrictedGets.push_back(item);
            }
        }
        catch (...) { restrictedGetObservationFailed = true; }
        SetLastError(saved);
    }
    bool restrictedFileDrop_ = false;
    bool restrictedPreflightAttempted_ = false, restrictedPreflightSucceeded_ = false;
    friend Microsoft::WRL::ComPtr<Source> Make(const std::vector<std::filesystem::path>&, bool);
    Microsoft::WRL::ComPtr<IDataObject> shell_;
    std::vector<std::filesystem::path> constructionPaths_;
    static constexpr std::size_t kFileDropCallCapacity = 1024;
    void RecordFileDropCall(const FileDropCallEvent& event) noexcept
    {
        if (fileDropCallCount_ == kFileDropCallCapacity)
        { fileDropCallFailed_.store(true); return; }
        fileDropCalls_[fileDropCallCount_++] = event;
    }
    class FileDropCallScope final
    {
    public:
        FileDropCallScope(Source& source, bool fileDrop) noexcept : source_(source)
        {
            if (!fileDrop) return;
            const DWORD saved = GetLastError();
            const DWORD thread = GetCurrentThreadId();
            if (thread != source_.owner || source_.fileDropNextCall_ == (std::numeric_limits<std::uint64_t>::max)() ||
                source_.fileDropCallDepth_ == (std::numeric_limits<std::size_t>::max)())
            {
                source_.fileDropCallFailed_.store(true);
                SetLastError(saved);
                return;
            }
            event_.call = ++source_.fileDropNextCall_;
            event_.parent = source_.fileDropCurrentCall_;
            event_.depth = source_.fileDropCallDepth_++;
            event_.thread = thread;
            source_.fileDropCurrentCall_ = event_.call;
            source_.RecordFileDropCall(event_);
            snowdesktop::external_transfer_observation::RecordAny(0,
                snowdesktop::external_transfer_observation::Phase::fixture_filedrop_getdata_enter,
                reinterpret_cast<std::uintptr_t>(&source_), 0, event_.call);
            active_ = true;
            SetLastError(saved);
        }
        ~FileDropCallScope() noexcept
        {
            if (!active_) return;
            const DWORD saved = GetLastError();
            if (!returned_)
            {
                event_.phase = FileDropCallPhase::unwound; source_.RecordFileDropCall(event_);
                snowdesktop::external_transfer_observation::RecordAny(0,
                    snowdesktop::external_transfer_observation::Phase::fixture_filedrop_getdata_unwound,
                    reinterpret_cast<std::uintptr_t>(&source_), 0, event_.call);
            }
            if (!source_.fileDropCallDepth_ || source_.fileDropCurrentCall_ != event_.call)
                source_.fileDropCallFailed_.store(true);
            else { --source_.fileDropCallDepth_; source_.fileDropCurrentCall_ = event_.parent; }
            SetLastError(saved);
        }
        HRESULT Return(HRESULT value) noexcept
        {
            if (!active_) return value;
            const DWORD saved = GetLastError();
            event_.phase = FileDropCallPhase::returned; event_.result = value;
            source_.RecordFileDropCall(event_);
            snowdesktop::external_transfer_observation::RecordAny(0,
                snowdesktop::external_transfer_observation::Phase::fixture_filedrop_getdata_return,
                reinterpret_cast<std::uintptr_t>(&source_), static_cast<std::int64_t>(value), event_.call);
            returned_ = true;
            SetLastError(saved);
            return value;
        }
    private:
        Source& source_;
        FileDropCallEvent event_;
        bool active_ = false, returned_ = false;
    };
    std::array<FileDropCallEvent, kFileDropCallCapacity> fileDropCalls_{};
    std::atomic<bool> fileDropCallFailed_{false};
    std::size_t fileDropCallCount_ = 0, fileDropCallDepth_ = 0;
    std::uint64_t fileDropNextCall_ = 0, fileDropCurrentCall_ = 0;
    std::shared_ptr<const LifetimeToken> lifetimeIdentity_ = std::make_shared<LifetimeToken>();
};

inline Microsoft::WRL::ComPtr<Source> Make(const std::vector<std::filesystem::path>& files, bool asynchronous)
{
    if (files.empty()) throw std::invalid_argument("native OLE file fixture requires at least one filesystem item");
    auto source = Microsoft::WRL::Make<Source>();
    source->asynchronous = asynchronous;
    auto nativeParent = files.front().parent_path();
    nativeParent.make_preferred();
    Pidl parent(ILCreateFromPathW(nativeParent.c_str()));
    if (!parent.get()) throw std::runtime_error("native OLE fixture source parent PIDL is missing");
    std::vector<Pidl> absolute;
    std::vector<PCUITEMID_CHILD> children;
    std::vector<std::filesystem::path> constructedPaths;
    constructedPaths.reserve(files.size());
    for (const auto& file : files)
    {
        auto native = file;
        native.make_preferred();
        constructedPaths.push_back(native);
        absolute.emplace_back(ILCreateFromPathW(native.c_str()));
        if (!absolute.back().get()) throw std::runtime_error("native OLE fixture selected file PIDL is missing");
        children.push_back(ILFindLastID(absolute.back().get()));
    }
    if (FAILED(SHCreateDataObject(parent.get(), static_cast<UINT>(children.size()), children.data(),
            nullptr, IID_PPV_ARGS(&source->shell_))))
        throw std::runtime_error("create real Shell data object for native OLE fixture");
    source->constructionPaths_ = std::move(constructedPaths);
    return source;
}
}

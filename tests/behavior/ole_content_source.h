#pragma once

#include <windows.h>
#include <objidl.h>
#include <shlobj.h>
#include <shlobj_core.h>
#include <wincodec.h>
#include <wrl/implements.h>
#include <wrl/client.h>
#include "app/drag_drop/ole_drag_drop_adapter.h"
#include "drag_drop/external_transfer_observation.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Standard OLE input only. No format-selection policy, file write, target
// ownership, layout update or production completion is implemented here.
namespace ole_content_source_fixture
{
enum class VirtualContent { available, missing, read_failure };
struct VirtualFile
{
    std::wstring name;
    std::vector<std::byte> bytes;
    VirtualContent content = VirtualContent::available;
};
struct FormatFault
{
    CLIPFORMAT format = 0;
    LONG index = -1;
    HRESULT result = E_FAIL;
};
struct Inputs
{
    // Each engaged optional advertises the corresponding independent format.
    // DIB bytes are caller-provided CF_DIB data, including the DIB header.
    std::optional<std::vector<std::byte>> dib;
    std::optional<std::wstring> text;
    std::optional<std::wstring> url;
    std::optional<std::vector<VirtualFile>> virtualFiles;
    // QueryGetData still advertises these entries; GetData returns the
    // requested failure, modeling an offered but unreadable representation.
    std::vector<FormatFault> getDataFaults;
};
struct Call
{
    bool query = false;
    CLIPFORMAT format = 0;
    DWORD aspect = 0;
    LONG index = 0;
    DWORD requestedTymed = 0;
    bool targetDevicePresent = false;
    HRESULT result = E_PENDING;
    DWORD thread = 0;
    DWORD returnedTymed = 0;
    // Captures the requested-length bytes written to the returned HGLOBAL,
    // during its construction lock. Allocator padding is not observed.
    bool mediumObserved = false;
    std::vector<std::byte> hglobalBytes;
};
struct Entry
{
    CLIPFORMAT format = 0;
    LONG index = -1;
    DWORD tymed = TYMED_HGLOBAL;
    std::vector<std::byte> bytes;
    bool readFailure = false;
    HRESULT getDataFailure = S_OK;
};
struct StreamRead
{
    ULONG requested = 0, returned = 0;
    HRESULT result = E_PENDING;
    DWORD thread = 0;
    std::uint64_t readId = 0;
    std::vector<std::byte> bytes;
};
struct StreamObservation
{
    LONG index = -1;
    // The actual provider call that created this wrapper; zero when the
    // shared diagnostic session was disabled. Clones share this observation.
    std::uint64_t providerCallId = 0;
    std::mutex mutex;
    std::vector<StreamRead> reads;
    std::atomic_bool observationFailed{false};
};

inline std::vector<std::byte> WideBytes(const std::wstring& value)
{
    if (value.size() >= (std::numeric_limits<std::size_t>::max)() / sizeof(wchar_t))
        throw std::length_error("OLE text too large");
    std::vector<std::byte> bytes((value.size() + 1) * sizeof(wchar_t));
    std::memcpy(bytes.data(), value.c_str(), bytes.size());
    return bytes;
}
}

// Independent read-only checks for a final file selected by the host fixture
// after real production completion. These helpers neither select a product
// format nor establish target identity, message dispatch, placement or save.
namespace ole_content_output_observation
{
struct Evidence
{
    bool observed = false;
    bool matches = false;
    std::string error;
    std::optional<std::uintmax_t> actualSize;
    bool actualBytesComplete = false;
    std::vector<std::byte> actualBytes;
};

inline Evidence ExactBytes(const std::filesystem::path& path, const std::vector<std::byte>& expected)
{
    Evidence evidence;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) { evidence.error = "output_file_size_unavailable"; return evidence; }
    evidence.actualSize = size;
    // A one-time size observation is not a stable result. Preserve it even if
    // the complete byte read is unavailable, but do not call it a mismatch.
    const bool sizeMismatch = size != expected.size();
    constexpr std::uintmax_t mismatchBudget = 16u * 1024u * 1024u;
    if (sizeMismatch && size > (std::max)(mismatchBudget, static_cast<std::uintmax_t>(expected.size())))
    { evidence.error = "output_read_budget_unavailable"; return evidence; }
    if (size > static_cast<std::uintmax_t>((std::numeric_limits<std::streamsize>::max)()))
    { evidence.error = "output_size_unrepresentable"; return evidence; }
    if (size > static_cast<std::uintmax_t>((std::numeric_limits<std::size_t>::max)()))
    { evidence.error = "output_size_unrepresentable"; return evidence; }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { evidence.error = "output_open_failed"; return evidence; }
    std::vector<std::byte> actual;
    try { actual.resize(static_cast<std::size_t>(size)); }
    catch (const std::bad_alloc&) { evidence.error = "output_read_allocation_unavailable"; return evidence; }
    if (!actual.empty()) stream.read(reinterpret_cast<char*>(actual.data()), static_cast<std::streamsize>(actual.size()));
    if (!stream || stream.peek() != std::char_traits<char>::eof() || stream.bad())
    { evidence.error = "output_read_incomplete_or_changed"; return evidence; }
    const auto afterSize = std::filesystem::file_size(path, ec);
    if (ec || afterSize != size)
    { evidence.error = "output_read_incomplete_or_changed"; return evidence; }
    // This is a complete read bracketed by size observations, not an atomic
    // file snapshot or proof that the host/producer has stopped writing.
    evidence.actualBytes = std::move(actual);
    evidence.actualBytesComplete = true;
    evidence.observed = true;
    evidence.matches = evidence.actualBytes == expected;
    if (!evidence.matches) evidence.error = sizeMismatch ? "output_size_mismatch" : "output_bytes_mismatch";
    return evidence;
}

inline Evidence PngRgba(const std::filesystem::path& path, UINT expectedWidth, UINT expectedHeight,
    const std::vector<std::uint8_t>& expectedRgba)
{
    Evidence evidence;
    if (!expectedWidth || !expectedHeight || expectedWidth > (std::numeric_limits<UINT>::max)() / 4 ||
        expectedHeight > (std::numeric_limits<UINT>::max)() / (expectedWidth * 4))
    { evidence.error = "png_expected_shape_invalid"; return evidence; }
    const auto bytes = static_cast<std::uint64_t>(expectedWidth) * expectedHeight * 4;
    if (bytes != expectedRgba.size()) { evidence.error = "png_expected_shape_invalid"; return evidence; }
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder);
    GUID container{};
    if (SUCCEEDED(hr)) hr = decoder->GetContainerFormat(&container);
    if (FAILED(hr)) { evidence.error = "png_open_or_decode_failed"; return evidence; }
    if (!IsEqualGUID(container, GUID_ContainerFormatPng))
    { evidence.observed = true; evidence.error = "output_is_not_png"; return evidence; }
    UINT count = 0;
    if (FAILED(decoder->GetFrameCount(&count))) { evidence.error = "png_frame_count_failed"; return evidence; }
    if (count != 1) { evidence.observed = true; evidence.error = "png_frame_count_mismatch"; return evidence; }
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    UINT width = 0, height = 0;
    if (SUCCEEDED(hr)) hr = frame->GetSize(&width, &height);
    if (FAILED(hr)) { evidence.error = "png_dimensions_unavailable"; return evidence; }
    if (width != expectedWidth || height != expectedHeight)
    { evidence.observed = true; evidence.error = "png_dimensions_mismatch"; return evidence; }
    Microsoft::WRL::ComPtr<IWICFormatConverter> converted;
    hr = factory->CreateFormatConverter(&converted);
    if (SUCCEEDED(hr)) hr = converted->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    std::vector<std::uint8_t> actual(expectedRgba.size());
    if (SUCCEEDED(hr)) hr = converted->CopyPixels(nullptr, width * 4, static_cast<UINT>(actual.size()), actual.data());
    if (FAILED(hr)) { evidence.error = "png_pixel_decode_failed"; return evidence; }
    evidence.observed = true;
    evidence.matches = actual == expectedRgba;
    if (!evidence.matches) evidence.error = "png_pixels_mismatch";
    return evidence;
}

inline Evidence InternetShortcut(const std::filesystem::path& path, const std::wstring& expectedUrl)
{
    Evidence evidence;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec)
    { evidence.error = "shortcut_file_unavailable"; return evidence; }
    if (expectedUrl.empty() || expectedUrl.size() > 32768)
    { evidence.error = "shortcut_expected_url_invalid"; return evidence; }
    std::vector<wchar_t> buffer(32770);
    const DWORD length = GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"",
        buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
    if (!length || length >= buffer.size() - 1)
    { evidence.error = "shortcut_url_unavailable_or_truncated"; return evidence; }
    evidence.observed = true;
    evidence.matches = std::wstring_view(buffer.data(), length) == expectedUrl;
    if (!evidence.matches) evidence.error = "shortcut_url_mismatch";
    return evidence;
}
}

namespace ole_content_source_fixture
{
// A valid 1x1 32-bit uncompressed DIB. The caller supplies the pixel; an
// independent output observer must check what production actually persisted.
inline std::vector<std::byte> DibPixel(std::uint8_t blue, std::uint8_t green,
    std::uint8_t red, std::uint8_t alpha = 255)
{
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header); header.biWidth = 1; header.biHeight = 1;
    header.biPlanes = 1; header.biBitCount = 32; header.biCompression = BI_RGB;
    header.biSizeImage = 4;
    std::vector<std::byte> bytes(sizeof(header) + 4);
    std::memcpy(bytes.data(), &header, sizeof(header));
    bytes[sizeof(header)] = static_cast<std::byte>(blue);
    bytes[sizeof(header) + 1] = static_cast<std::byte>(green);
    bytes[sizeof(header) + 2] = static_cast<std::byte>(red);
    bytes[sizeof(header) + 3] = static_cast<std::byte>(alpha);
    return bytes;
}

class ObservedStream final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IStream>
{
public:
    Microsoft::WRL::ComPtr<IStream> inner;
    std::shared_ptr<StreamObservation> observation;
    bool readFailure = false;
    HRESULT STDMETHODCALLTYPE Read(void* data, ULONG size, ULONG* read) override
    {
        namespace diagnostic = snowdesktop::external_transfer_observation;
        const DWORD beforeError = GetLastError();
        const bool trace = observation && diagnostic::Enabled();
        const std::uint64_t readId = trace ? diagnostic::NewTask() : 0;
        const auto marker = [&](diagnostic::Phase phase, std::int64_t value) noexcept {
            if (!trace) return;
            const DWORD saved = GetLastError();
            diagnostic::AdapterScopeIdentity scope{};
            snowdesktop::ole_adapter_observation::Event event;
            if (snowdesktop::ole_adapter_observation::Current(event))
                scope = {event.session, event.call, event.parent, event.adapter, event.thread,
                    event.depth, static_cast<unsigned>(event.kind), true};
            diagnostic::RecordAny(observation->providerCallId, phase,
                reinterpret_cast<std::uintptr_t>(observation.get()), value, readId, scope);
            SetLastError(saved);
        };
        marker(diagnostic::Phase::provider_stream_read_enter, static_cast<std::int64_t>(size));
        SetLastError(beforeError);
        ULONG actual = 0;
        const HRESULT result = readFailure ? STG_E_READFAULT : inner->Read(data, size, &actual);
        const DWORD afterError = GetLastError();
        if (read) *read = actual;
        if (observation)
        {
            try
            {
                std::lock_guard lock(observation->mutex);
                StreamRead event; event.requested = size; event.returned = actual;
                event.result = result; event.thread = GetCurrentThreadId(); event.readId = readId;
                if (actual && data)
                {
                    const auto* first = static_cast<const std::byte*>(data);
                    event.bytes.assign(first, first + actual);
                }
                observation->reads.push_back(std::move(event));
            }
            catch (...) { observation->observationFailed.store(true); }
        }
        marker(diagnostic::Phase::provider_stream_read_return, static_cast<std::int64_t>(result));
        SetLastError(afterError);
        return result;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* p, ULONG n, ULONG* written) override { return inner->Write(p, n, written); }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER offset, DWORD origin, ULARGE_INTEGER* position) override { return inner->Seek(offset, origin, position); }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER size) override { return inner->SetSize(size); }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* stream, ULARGE_INTEGER n, ULARGE_INTEGER* read, ULARGE_INTEGER* written) override
    {
        // Delegating preserves stream semantics but bypasses Read observation.
        // An oracle must reject the resulting incomplete evidence.
        if (observation) observation->observationFailed.store(true);
        if (readFailure)
        {
            if (read) read->QuadPart = 0;
            if (written) written->QuadPart = 0;
            return STG_E_READFAULT;
        }
        return inner->CopyTo(stream, n, read, written);
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD flags) override { return inner->Commit(flags); }
    HRESULT STDMETHODCALLTYPE Revert() override { return inner->Revert(); }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER offset, ULARGE_INTEGER n, DWORD type) override { return inner->LockRegion(offset, n, type); }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER offset, ULARGE_INTEGER n, DWORD type) override { return inner->UnlockRegion(offset, n, type); }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD flags) override { return inner->Stat(stat, flags); }
    HRESULT STDMETHODCALLTYPE Clone(IStream** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        Microsoft::WRL::ComPtr<IStream> copy;
        const HRESULT result = inner->Clone(&copy);
        if (FAILED(result)) return result;
        auto wrapper = Microsoft::WRL::Make<ObservedStream>();
        if (!wrapper) return E_OUTOFMEMORY;
        wrapper->inner = std::move(copy); wrapper->observation = observation; wrapper->readFailure = readFailure;
        *out = wrapper.Detach();
        return S_OK;
    }
};

class Source final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDataObject>
{
public:
    std::vector<Entry> entries;
    std::vector<Call> calls;
    std::vector<std::shared_ptr<StreamObservation>> streamObservations;
    std::mutex observationMutex;
    DWORD ownerThread = GetCurrentThreadId();
    std::atomic_bool wrongApartment{false}, observationFailed{false};

    HRESULT Find(const FORMATETC* format, const Entry** out) const noexcept
    {
        if (!format || !out) return E_POINTER;
        *out = nullptr;
        if (format->ptd) return DV_E_DVTARGETDEVICE;
        if (format->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
        bool hasFormat = false, hasIndex = false;
        for (const auto& entry : entries)
        {
            if (entry.format != format->cfFormat) continue;
            hasFormat = true;
            if (entry.index != format->lindex) continue;
            hasIndex = true;
            if (!(entry.tymed & format->tymed)) continue;
            *out = &entry;
            return S_OK;
        }
        return !hasFormat ? DV_E_FORMATETC : !hasIndex ? DV_E_LINDEX : DV_E_TYMED;
    }
    void Observe(bool query, const FORMATETC* format, HRESULT result, const STGMEDIUM* medium = nullptr,
        std::vector<std::byte>* hglobalSnapshot = nullptr, bool captured = false) noexcept
    {
        const DWORD savedError = GetLastError();
        if (ownerThread != GetCurrentThreadId()) wrongApartment.store(true);
        try
        {
            Call call; call.query = query; call.result = result; call.thread = GetCurrentThreadId();
            if (format)
            {
                call.format = format->cfFormat; call.aspect = format->dwAspect; call.index = format->lindex;
                call.requestedTymed = format->tymed; call.targetDevicePresent = format->ptd != nullptr;
            }
            if (!query && SUCCEEDED(result))
            {
                if (!medium) observationFailed.store(true);
                else
                {
                    call.returnedTymed = medium->tymed;
                    if (medium->tymed == TYMED_HGLOBAL && medium->hGlobal)
                    {
                        if (captured && hglobalSnapshot)
                        { call.hglobalBytes = std::move(*hglobalSnapshot); call.mediumObserved = true; }
                        else observationFailed.store(true);
                    }
                    else if (medium->tymed == TYMED_ISTREAM && medium->pstm)
                        call.mediumObserved = true; // Reads have their own stream observation.
                    else observationFailed.store(true);
                }
            }
            std::lock_guard lock(observationMutex);
            calls.push_back(std::move(call));
        }
        catch (...) { observationFailed.store(true); }
        SetLastError(savedError);
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override
    {
        const Entry* entry = nullptr;
        const HRESULT result = Find(format, &entry);
        Observe(true, format, result);
        return result;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override
    {
        namespace diagnostic = snowdesktop::external_transfer_observation;
        const bool trace = diagnostic::Enabled();
        std::uint64_t call = 0;
        const auto observeCall = [&](diagnostic::Phase phase, HRESULT hr) noexcept {
            if (!trace) return;
            const DWORD saved = GetLastError();
            diagnostic::AdapterScopeIdentity scope{};
            snowdesktop::ole_adapter_observation::Event event;
            if (snowdesktop::ole_adapter_observation::Current(event))
                scope = {event.session, event.call, event.parent, event.adapter, event.thread,
                    event.depth, static_cast<unsigned>(event.kind), true};
            diagnostic::RecordAny(0, phase, reinterpret_cast<std::uintptr_t>(static_cast<IDataObject*>(this)),
                static_cast<std::int64_t>(hr), call, scope);
            SetLastError(saved);
        };
        if (trace)
        {
            const DWORD saved = GetLastError();
            call = diagnostic::NewTask();
            SetLastError(saved);
            observeCall(diagnostic::Phase::content_provider_getdata_enter, E_PENDING);
        }
        std::vector<std::byte> hglobalSnapshot;
        bool captured = false;
        const Entry* entry = nullptr;
        HRESULT result = medium ? Find(format, &entry) : E_POINTER;
        if (SUCCEEDED(result) && FAILED(entry->getDataFailure)) result = entry->getDataFailure;
        if (SUCCEEDED(result))
        {
            *medium = {};
            HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, entry->bytes.size());
            if (!block) result = E_OUTOFMEMORY;
            else
            {
                if (!entry->bytes.empty())
                {
                    void* destination = GlobalLock(block);
                    if (!destination) result = E_OUTOFMEMORY;
                    else
                    {
                        std::memcpy(destination, entry->bytes.data(), entry->bytes.size());
                        if (entry->tymed == TYMED_HGLOBAL)
                        {
                            // Observe under the provider's existing lock. No
                            // additional lock or ownership change reaches COM.
                            const DWORD savedError = GetLastError();
                            try
                            {
                                constexpr std::size_t maximumObservedBytes = 64ull * 1024ull * 1024ull;
                                if (entry->bytes.size() <= maximumObservedBytes)
                                {
                                    const auto* first = static_cast<const std::byte*>(destination);
                                    hglobalSnapshot.assign(first, first + entry->bytes.size());
                                    captured = true;
                                }
                                else observationFailed.store(true);
                            }
                            catch (...) { observationFailed.store(true); }
                            SetLastError(savedError);
                        }
                        const BOOL stillLocked = GlobalUnlock(block);
                        const DWORD unlockError = GetLastError();
                        // The newly allocated block had exactly this one
                        // construction lock. An unexpected remaining lock or
                        // unlock failure makes observation unavailable, but
                        // does not alter the provider's HRESULT or medium.
                        if (stillLocked || unlockError != ERROR_SUCCESS) observationFailed.store(true);
                        SetLastError(unlockError);
                    }
                }
                else if (entry->tymed == TYMED_HGLOBAL) captured = true;
                if (SUCCEEDED(result) && entry->tymed == TYMED_HGLOBAL)
                { medium->tymed = TYMED_HGLOBAL; medium->hGlobal = block; block = nullptr; }
                else if (SUCCEEDED(result) && entry->tymed == TYMED_ISTREAM)
                {
                    Microsoft::WRL::ComPtr<IStream> stream;
                    result = CreateStreamOnHGlobal(block, TRUE, &stream);
                    if (SUCCEEDED(result))
                    {
                        block = nullptr; // stream owns it, including failure paths below
                        ULARGE_INTEGER exact{}; exact.QuadPart = entry->bytes.size();
                        result = stream->SetSize(exact);
                        if (SUCCEEDED(result))
                        {
                            try
                            {
                                auto observation = std::make_shared<StreamObservation>();
                                observation->index = entry->index;
                                observation->providerCallId = call;
                                auto wrapped = Microsoft::WRL::Make<ObservedStream>();
                                if (!wrapped) result = E_OUTOFMEMORY;
                                else
                                {
                                    wrapped->inner = stream; wrapped->observation = observation; wrapped->readFailure = entry->readFailure;
                                    {
                                        std::lock_guard lock(observationMutex);
                                        streamObservations.push_back(std::move(observation));
                                    }
                                    stream = wrapped;
                                }
                            }
                            catch (...) { result = E_OUTOFMEMORY; }
                        }
                        if (SUCCEEDED(result)) { medium->tymed = TYMED_ISTREAM; medium->pstm = stream.Detach(); }
                    }
                }
                if (block) GlobalFree(block);
            }
        }
        Observe(false, format, result, SUCCEEDED(result) ? medium : nullptr, &hglobalSnapshot, captured);
        observeCall(diagnostic::Phase::content_provider_getdata_return, result);
        return result;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override
    { if (!out) return E_POINTER; out->ptd = nullptr; return DATA_S_SAMEFORMATETC; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (direction != DATADIR_GET) return E_NOTIMPL;
        try
        {
            std::vector<FORMATETC> formats;
            for (const auto& entry : entries)
                formats.push_back(FORMATETC{entry.format, nullptr, DVASPECT_CONTENT, entry.index, entry.tymed});
            if (formats.size() > (std::numeric_limits<UINT>::max)()) return E_OUTOFMEMORY;
            return SHCreateStdEnumFmtEtc(static_cast<UINT>(formats.size()), formats.data(), out);
        }
        catch (...) { return E_OUTOFMEMORY; }
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
};

inline Microsoft::WRL::ComPtr<Source> Make(const Inputs& inputs)
{
    auto source = Microsoft::WRL::Make<Source>();
    if (!source) throw std::bad_alloc();
    if (inputs.dib) source->entries.push_back({CF_DIB, -1, TYMED_HGLOBAL, *inputs.dib, false});
    if (inputs.text) source->entries.push_back({CF_UNICODETEXT, -1, TYMED_HGLOBAL, WideBytes(*inputs.text), false});
    if (inputs.url)
    {
        const auto format = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"UniformResourceLocatorW"));
        if (!format) throw std::runtime_error("register Unicode URL clipboard format");
        source->entries.push_back({format, -1, TYMED_HGLOBAL, WideBytes(*inputs.url), false});
    }
    if (inputs.virtualFiles)
    {
        const auto descriptorFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW"));
        const auto contentsFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents"));
        if (!descriptorFormat || !contentsFormat) throw std::runtime_error("register virtual file clipboard formats");
        const auto count = inputs.virtualFiles->size();
        if (count > (std::numeric_limits<UINT>::max)() || count > static_cast<std::size_t>((std::numeric_limits<LONG>::max)()) ||
            count > ((std::numeric_limits<std::size_t>::max)() - offsetof(FILEGROUPDESCRIPTORW, fgd)) / sizeof(FILEDESCRIPTORW))
            throw std::length_error("virtual descriptor batch too large");
        std::vector<std::byte> descriptors(offsetof(FILEGROUPDESCRIPTORW, fgd) + count * sizeof(FILEDESCRIPTORW));
        const auto cItems = static_cast<UINT>(count);
        std::memcpy(descriptors.data(), &cItems, sizeof(cItems));
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto& file = (*inputs.virtualFiles)[i];
            if (file.name.empty() || file.name.size() >= MAX_PATH || file.name.find(L'\0') != std::wstring::npos)
                throw std::invalid_argument("virtual file name is not representable by FILEDESCRIPTORW");
            FILEDESCRIPTORW descriptor{};
            descriptor.dwFlags = FD_ATTRIBUTES | FD_FILESIZE;
            descriptor.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
            const auto size = static_cast<std::uint64_t>(file.bytes.size());
            descriptor.nFileSizeHigh = static_cast<DWORD>(size >> 32);
            descriptor.nFileSizeLow = static_cast<DWORD>(size);
            std::memcpy(descriptor.cFileName, file.name.c_str(), (file.name.size() + 1) * sizeof(wchar_t));
            std::memcpy(descriptors.data() + offsetof(FILEGROUPDESCRIPTORW, fgd) + i * sizeof(descriptor), &descriptor, sizeof(descriptor));
            if (file.content != VirtualContent::missing)
                source->entries.push_back({contentsFormat, static_cast<LONG>(i), TYMED_ISTREAM, file.bytes,
                    file.content == VirtualContent::read_failure});
        }
        source->entries.push_back({descriptorFormat, -1, TYMED_HGLOBAL, std::move(descriptors), false});
    }
    for (const auto& fault : inputs.getDataFaults)
    {
        if (SUCCEEDED(fault.result)) throw std::invalid_argument("OLE GetData fault must be a failing HRESULT");
        const auto found = std::find_if(source->entries.begin(), source->entries.end(), [&](const auto& entry) {
            return entry.format == fault.format && entry.index == fault.index;
        });
        if (found == source->entries.end() || FAILED(found->getDataFailure))
            throw std::invalid_argument("OLE GetData fault must identify one offered format and index");
        found->getDataFailure = fault.result;
    }
    return source;
}
}

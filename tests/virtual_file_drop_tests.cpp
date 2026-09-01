#include "virtual_file_drop.h"

#include <windows.h>
#include <shlobj.h>
#include <shldisp.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

template <typename Group, typename Descriptor>
std::vector<std::byte> DescriptorBytes(
    const std::vector<Descriptor>& descriptors)
{
    const size_t headerSize = offsetof(Group, fgd);
    std::vector<std::byte> bytes(
        headerSize + descriptors.size() * sizeof(Descriptor));
    const UINT count = static_cast<UINT>(descriptors.size());
    std::memcpy(bytes.data(), &count, sizeof(count));
    for (size_t index = 0; index < descriptors.size(); ++index)
    {
        std::memcpy(bytes.data() + headerSize +
                index * sizeof(Descriptor),
            &descriptors[index], sizeof(Descriptor));
    }
    return bytes;
}

FILEDESCRIPTORW WideDescriptor(
    const wchar_t* filename, bool directory = false)
{
    FILEDESCRIPTORW descriptor{};
    if (filename)
        wcsncpy_s(descriptor.cFileName, filename, _TRUNCATE);
    if (directory)
    {
        descriptor.dwFlags |= FD_ATTRIBUTES;
        descriptor.dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    }
    return descriptor;
}

FILEDESCRIPTORA AnsiDescriptor(
    const char* filename, bool directory = false)
{
    FILEDESCRIPTORA descriptor{};
    if (filename)
        strncpy_s(descriptor.cFileName, filename, _TRUNCATE);
    if (directory)
    {
        descriptor.dwFlags |= FD_ATTRIBUTES;
        descriptor.dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    }
    return descriptor;
}

class MockDataObject final :
    public IDataObject,
    public IDataObjectAsyncCapability
{
public:
    std::optional<std::vector<std::byte>> wide;
    std::optional<std::vector<std::byte>> ansi;
    bool exposeAsyncCapability = false;
    bool asyncMode = false;
    bool offerFileDrop = false;
    int wideRequests = 0;
    int ansiRequests = 0;
    int getDataCalls = 0;
    int queryGetDataCalls = 0;
    int getAsyncModeCalls = 0;
    int startOperationCalls = 0;

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid, void** object) override
    {
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDataObject)
            *object = static_cast<IDataObject*>(this);
        else if (iid == __uuidof(IDataObjectAsyncCapability) &&
            exposeAsyncCapability)
            *object = static_cast<IDataObjectAsyncCapability*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++references_;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        return --references_;
    }

    HRESULT STDMETHODCALLTYPE GetData(
        FORMATETC* format, STGMEDIUM* medium) override
    {
        ++getDataCalls;
        if (!format || !medium)
            return E_POINTER;
        *medium = {};

        const CLIPFORMAT wideFormat = static_cast<CLIPFORMAT>(
            RegisterClipboardFormatW(L"FileGroupDescriptorW"));
        const CLIPFORMAT ansiFormat = static_cast<CLIPFORMAT>(
            RegisterClipboardFormatW(L"FileGroupDescriptor"));
        const std::optional<std::vector<std::byte>>* payload = nullptr;
        if (format->cfFormat == wideFormat)
        {
            ++wideRequests;
            payload = &wide;
        }
        else if (format->cfFormat == ansiFormat)
        {
            ++ansiRequests;
            payload = &ansi;
        }
        else
            return DV_E_FORMATETC;

        if (!*payload ||
            (format->tymed & TYMED_HGLOBAL) == 0)
            return DV_E_FORMATETC;
        HGLOBAL memory = GlobalAlloc(
            GMEM_MOVEABLE, (*payload)->size());
        if (!memory)
            return E_OUTOFMEMORY;
        void* destination = GlobalLock(memory);
        if (!destination)
        {
            GlobalFree(memory);
            return E_OUTOFMEMORY;
        }
        std::memcpy(destination, (*payload)->data(),
            (*payload)->size());
        GlobalUnlock(memory);

        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = memory;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(
        FORMATETC*, STGMEDIUM*) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override
    {
        ++queryGetDataCalls;
        if (!format)
            return E_POINTER;
        return offerFileDrop &&
            format->cfFormat == CF_HDROP &&
            format->dwAspect == DVASPECT_CONTENT &&
            format->lindex == -1 &&
            (format->tymed & TYMED_HGLOBAL) != 0
            ? S_OK : DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(
        FORMATETC*, FORMATETC* output) override
    {
        if (output)
            output->ptd = nullptr;
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE SetData(
        FORMATETC*, STGMEDIUM*, BOOL) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(
        DWORD, IEnumFORMATETC**) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE DAdvise(
        FORMATETC*, DWORD, IAdviseSink*, DWORD*) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumDAdvise(
        IEnumSTATDATA**) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL enabled) override
    {
        asyncMode = enabled != FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* enabled) override
    {
        ++getAsyncModeCalls;
        if (!enabled)
            return E_POINTER;
        *enabled = asyncMode ? TRUE : FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE StartOperation(
        IBindCtx*) override
    {
        ++startOperationCalls;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE InOperation(BOOL* active) override
    {
        if (!active)
            return E_POINTER;
        *active = FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EndOperation(
        HRESULT, IBindCtx*, DWORD) override
    {
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{ 1 };
};

void TestWideDescriptorsPreserveIndicesAndFilterDirectories()
{
    MockDataObject dataObject;
    dataObject.wide = DescriptorBytes<
        FILEGROUPDESCRIPTORW, FILEDESCRIPTORW>({
        WideDescriptor(L"first.png"),
        WideDescriptor(L"album", true),
        WideDescriptor(L"report.pdf") });

    const auto descriptors =
        snowdesktop::virtual_file_drop::ReadDescriptors(&dataObject);
    Check(descriptors.size() == 2 &&
            descriptors[0].suggestedFileName == L"first.png" &&
            descriptors[0].descriptorIndex == 0 &&
            descriptors[1].suggestedFileName == L"report.pdf" &&
            descriptors[1].descriptorIndex == 2,
        "Unicode descriptor enumeration preserves FileContents indices while filtering directories");
    Check(dataObject.wideRequests == 1 &&
            dataObject.ansiRequests == 0,
        "a present Unicode descriptor group is authoritative");
}

void TestAnsiFallback()
{
    MockDataObject dataObject;
    dataObject.ansi = DescriptorBytes<
        FILEGROUPDESCRIPTORA, FILEDESCRIPTORA>({
        AnsiDescriptor("folder", true),
        AnsiDescriptor("fallback.jpg") });

    const auto descriptors =
        snowdesktop::virtual_file_drop::ReadDescriptors(&dataObject);
    Check(descriptors.size() == 1 &&
            descriptors[0].suggestedFileName == L"fallback.jpg" &&
            descriptors[0].descriptorIndex == 1,
        "ANSI descriptors are converted only when Unicode descriptors are unavailable");
    Check(dataObject.wideRequests == 1 &&
            dataObject.ansiRequests == 1,
        "ANSI fallback is queried after the Unicode format is unavailable");
}

void TestTruncatedAndMalformedGroupsAreRejected()
{
    MockDataObject shortHeader;
    shortHeader.wide = std::vector<std::byte>(
        sizeof(UINT) - 1);
    Check(snowdesktop::virtual_file_drop::
            ReadDescriptors(&shortHeader).empty(),
        "a descriptor group shorter than its count header is rejected");

    MockDataObject truncated;
    truncated.wide = DescriptorBytes<
        FILEGROUPDESCRIPTORW, FILEDESCRIPTORW>({
        WideDescriptor(L"only-one.png") });
    const UINT declaredCount = 2;
    std::memcpy(truncated.wide->data(),
        &declaredCount, sizeof(declaredCount));
    Check(snowdesktop::virtual_file_drop::
            ReadDescriptors(&truncated).empty(),
        "a group whose descriptor count exceeds its HGLOBAL size is rejected");

    MockDataObject unreasonableCount;
    unreasonableCount.wide = std::vector<std::byte>(sizeof(UINT));
    const UINT maximumCount = UINT_MAX;
    std::memcpy(unreasonableCount.wide->data(),
        &maximumCount, sizeof(maximumCount));
    Check(snowdesktop::virtual_file_drop::
            ReadDescriptors(&unreasonableCount).empty(),
        "an unreasonable descriptor count is rejected before allocation");

    FILEDESCRIPTORW unterminated{};
    std::fill(std::begin(unterminated.cFileName),
        std::end(unterminated.cFileName), L'x');
    MockDataObject malformedName;
    malformedName.wide = DescriptorBytes<
        FILEGROUPDESCRIPTORW, FILEDESCRIPTORW>({ unterminated });
    Check(snowdesktop::virtual_file_drop::
            ReadDescriptors(&malformedName).empty(),
        "a descriptor filename without an in-bounds terminator is rejected");
}

void TestMalformedWideGroupDoesNotFallBackToAnsi()
{
    MockDataObject dataObject;
    dataObject.wide = std::vector<std::byte>(sizeof(UINT) - 1);
    dataObject.ansi = DescriptorBytes<
        FILEGROUPDESCRIPTORA, FILEDESCRIPTORA>({
        AnsiDescriptor("must-not-bypass.txt") });

    Check(snowdesktop::virtual_file_drop::
            ReadDescriptors(&dataObject).empty(),
        "a malformed present Unicode group is rejected");
    Check(dataObject.wideRequests == 1 &&
            dataObject.ansiRequests == 0,
        "malformed Unicode data cannot bypass validation through ANSI fallback");
}

void TestAsyncFileDropProbeDoesNotMaterializeData()
{
    MockDataObject dataObject;
    dataObject.offerFileDrop = true;
    dataObject.exposeAsyncCapability = true;
    dataObject.asyncMode = true;

    Check(snowdesktop::virtual_file_drop::
            OffersAsyncFileDrop(&dataObject),
        "delayed CF_HDROP with enabled async capability is recognized");
    Check(dataObject.queryGetDataCalls == 1 &&
            dataObject.getAsyncModeCalls == 1 &&
            dataObject.getDataCalls == 0 &&
            dataObject.startOperationCalls == 0,
        "the delayed-file probe queries capability without reading or starting materialization");
}

void TestAsyncFileDropRequiresBothSignals()
{
    MockDataObject noFileDrop;
    noFileDrop.exposeAsyncCapability = true;
    noFileDrop.asyncMode = true;
    Check(!snowdesktop::virtual_file_drop::
            OffersAsyncFileDrop(&noFileDrop) &&
            noFileDrop.getAsyncModeCalls == 0,
        "async capability alone does not advertise a delayed file drop");

    MockDataObject noCapability;
    noCapability.offerFileDrop = true;
    Check(!snowdesktop::virtual_file_drop::
            OffersAsyncFileDrop(&noCapability),
        "CF_HDROP alone does not advertise async materialization");

    MockDataObject synchronousMode;
    synchronousMode.offerFileDrop = true;
    synchronousMode.exposeAsyncCapability = true;
    Check(!snowdesktop::virtual_file_drop::
            OffersAsyncFileDrop(&synchronousMode),
        "a disabled async mode is not treated as delayed CF_HDROP");
}
} // namespace

int main()
{
    TestWideDescriptorsPreserveIndicesAndFilterDirectories();
    TestAnsiFallback();
    TestTruncatedAndMalformedGroupsAreRejected();
    TestMalformedWideGroupDoesNotFallBackToAnsi();
    TestAsyncFileDropProbeDoesNotMaterializeData();
    TestAsyncFileDropRequiresBothSignals();

    if (failures != 0)
    {
        std::cerr << failures <<
            " virtual file drop test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Virtual file drop tests passed\n";
    return EXIT_SUCCESS;
}

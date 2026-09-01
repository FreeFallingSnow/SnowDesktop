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
#include <limits>
#include <optional>
#include <string>
#include <string_view>
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
    const wchar_t* filename, bool directory = false,
    std::optional<std::uint64_t> fileSize = std::nullopt)
{
    FILEDESCRIPTORW descriptor{};
    if (filename)
        wcsncpy_s(descriptor.cFileName, filename, _TRUNCATE);
    if (directory)
    {
        descriptor.dwFlags |= FD_ATTRIBUTES;
        descriptor.dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    }
    if (fileSize)
    {
        descriptor.dwFlags |= FD_FILESIZE;
        descriptor.nFileSizeHigh = static_cast<DWORD>(*fileSize >> 32);
        descriptor.nFileSizeLow = static_cast<DWORD>(*fileSize);
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
    enum class ContentsMedium
    {
        None,
        Stream,
        Global,
    };

    std::optional<std::vector<std::byte>> wide;
    std::optional<std::vector<std::byte>> ansi;
    ContentsMedium contentsMedium = ContentsMedium::None;
    std::vector<std::byte> contents;
    bool exposeAsyncCapability = false;
    bool asyncMode = false;
    bool offerFileDrop = false;
    int wideRequests = 0;
    int ansiRequests = 0;
    int getDataCalls = 0;
    int queryGetDataCalls = 0;
    int getAsyncModeCalls = 0;
    int startOperationCalls = 0;
    int fileContentsRequests = 0;
    LONG lastFileContentsIndex = -1;
    DWORD lastFileContentsTymed = TYMED_NULL;

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
        const CLIPFORMAT contentsFormat = static_cast<CLIPFORMAT>(
            RegisterClipboardFormatW(L"FileContents"));
        if (format->cfFormat == contentsFormat)
        {
            ++fileContentsRequests;
            lastFileContentsIndex = format->lindex;
            lastFileContentsTymed = format->tymed;
            if (contentsMedium == ContentsMedium::None)
                return DV_E_FORMATETC;

            HGLOBAL memory = GlobalAlloc(
                GMEM_MOVEABLE, contents.empty() ? 1 : contents.size());
            if (!memory)
                return E_OUTOFMEMORY;
            void* destination = GlobalLock(memory);
            if (!destination)
            {
                GlobalFree(memory);
                return E_OUTOFMEMORY;
            }
            if (!contents.empty())
                std::memcpy(destination, contents.data(), contents.size());
            GlobalUnlock(memory);

            if (contentsMedium == ContentsMedium::Global)
            {
                if ((format->tymed & TYMED_HGLOBAL) == 0)
                {
                    GlobalFree(memory);
                    return DV_E_TYMED;
                }
                medium->tymed = TYMED_HGLOBAL;
                medium->hGlobal = memory;
                return S_OK;
            }

            if ((format->tymed & TYMED_ISTREAM) == 0)
            {
                GlobalFree(memory);
                return DV_E_TYMED;
            }
            IStream* stream = nullptr;
            const HRESULT streamResult = CreateStreamOnHGlobal(
                memory, TRUE, &stream);
            if (FAILED(streamResult))
            {
                GlobalFree(memory);
                return streamResult;
            }
            ULARGE_INTEGER streamSize{};
            streamSize.QuadPart = contents.size();
            if (FAILED(stream->SetSize(streamSize)))
            {
                stream->Release();
                return E_FAIL;
            }
            LARGE_INTEGER beginning{};
            if (FAILED(stream->Seek(beginning, STREAM_SEEK_SET, nullptr)))
            {
                stream->Release();
                return E_FAIL;
            }
            medium->tymed = TYMED_ISTREAM;
            medium->pstm = stream;
            return S_OK;
        }

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

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        wchar_t temporaryRoot[MAX_PATH]{};
        if (GetTempPathW(MAX_PATH, temporaryRoot) == 0)
            return;
        wchar_t temporaryFile[MAX_PATH]{};
        if (GetTempFileNameW(temporaryRoot, L"svd", 0,
                temporaryFile) == 0)
            return;
        DeleteFileW(temporaryFile);
        if (CreateDirectoryW(temporaryFile, nullptr))
            path_ = temporaryFile;
    }

    ~TemporaryDirectory()
    {
        if (path_.empty())
            return;
        WIN32_FIND_DATAW entry{};
        const std::wstring search = path_ + L"\\*";
        HANDLE find = FindFirstFileW(search.c_str(), &entry);
        if (find != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (wcscmp(entry.cFileName, L".") == 0 ||
                    wcscmp(entry.cFileName, L"..") == 0)
                    continue;
                const std::wstring child = path_ + L"\\" +
                    entry.cFileName;
                DeleteFileW(child.c_str());
            } while (FindNextFileW(find, &entry));
            FindClose(find);
        }
        RemoveDirectoryW(path_.c_str());
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    const std::wstring& Path() const noexcept
    {
        return path_;
    }

private:
    std::wstring path_;
};

std::vector<std::byte> Bytes(std::string_view value)
{
    std::vector<std::byte> result(value.size());
    if (!value.empty())
        std::memcpy(result.data(), value.data(), value.size());
    return result;
}

bool WriteTestFile(const std::wstring& path,
    const std::vector<std::byte>& bytes)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool result = bytes.size() <=
            std::numeric_limits<DWORD>::max() &&
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
            &written, nullptr) &&
        written == bytes.size();
    CloseHandle(file);
    return result;
}

std::vector<std::byte> ReadTestFile(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) >
            std::numeric_limits<DWORD>::max())
    {
        CloseHandle(file);
        return {};
    }
    std::vector<std::byte> result(
        static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool success = result.empty() ||
        (ReadFile(file, result.data(), static_cast<DWORD>(result.size()),
            &read, nullptr) && read == result.size());
    CloseHandle(file);
    return success ? result : std::vector<std::byte>{};
}

std::wstring LeafName(std::wstring_view path)
{
    const size_t separator = path.find_last_of(L"/\\");
    return std::wstring(path.substr(separator == std::wstring_view::npos
        ? 0 : separator + 1));
}

size_t CountFiles(const std::wstring& directory)
{
    size_t count = 0;
    WIN32_FIND_DATAW entry{};
    const std::wstring search = directory + L"\\*";
    HANDLE find = FindFirstFileW(search.c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE)
        return 0;
    do
    {
        if (wcscmp(entry.cFileName, L".") != 0 &&
            wcscmp(entry.cFileName, L"..") != 0)
            ++count;
    } while (FindNextFileW(find, &entry));
    FindClose(find);
    return count;
}

void TestWideDescriptorsPreserveIndicesAndFilterDirectories()
{
    MockDataObject dataObject;
    dataObject.wide = DescriptorBytes<
        FILEGROUPDESCRIPTORW, FILEDESCRIPTORW>({
        WideDescriptor(L"first.png", false, 123),
        WideDescriptor(L"album", true),
        WideDescriptor(L"report.pdf") });

    const auto descriptors =
        snowdesktop::virtual_file_drop::ReadDescriptors(&dataObject);
    Check(descriptors.size() == 2 &&
            descriptors[0].suggestedFileName == L"first.png" &&
            descriptors[0].descriptorIndex == 0 &&
            descriptors[0].advertisedFileSize == 123 &&
            descriptors[1].suggestedFileName == L"report.pdf" &&
            descriptors[1].descriptorIndex == 2 &&
            !descriptors[1].advertisedFileSize,
        "Unicode descriptor enumeration preserves FileContents indices and advertised sizes while filtering directories");
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

void TestStreamContentsUseDescriptorIndexAndSafeLeafName()
{
    TemporaryDirectory directory;
    Check(!directory.Path().empty(),
        "a temporary directory is available for stream materialization");
    if (directory.Path().empty())
        return;

    const std::vector<std::byte> payload = Bytes("stream image bytes");
    MockDataObject dataObject;
    dataObject.wide = DescriptorBytes<
        FILEGROUPDESCRIPTORW, FILEDESCRIPTORW>({
        WideDescriptor(L"folder", true),
        WideDescriptor(L"..\\..\\CON.png", false, payload.size()) });
    dataObject.contentsMedium = MockDataObject::ContentsMedium::Stream;
    dataObject.contents = payload;

    const auto descriptors =
        snowdesktop::virtual_file_drop::ReadDescriptors(&dataObject);
    Check(descriptors.size() == 1 &&
            descriptors[0].descriptorIndex == 1,
        "the materialized descriptor retains its original FileContents index");
    if (descriptors.size() != 1)
        return;

    const auto materialized =
        snowdesktop::virtual_file_drop::MaterializeFileContents(
            &dataObject, descriptors[0], directory.Path(), 1024);
    Check(materialized.has_value(),
        "TYMED_ISTREAM FileContents are materialized");
    if (!materialized)
        return;
    Check(LeafName(materialized->path) == L"_CON.png",
        "path components and reserved device names are reduced to a safe leaf name");
    Check(materialized->sizeBytes == payload.size() &&
            ReadTestFile(materialized->path) == payload,
        "stream bytes and the reported size match the source content");
    Check(dataObject.lastFileContentsIndex == 1 &&
            dataObject.fileContentsRequests == 1 &&
            dataObject.lastFileContentsTymed == TYMED_ISTREAM,
        "FileContents first requests only the bounded streaming medium with the descriptor index");
}

void TestGlobalContentsNeverOverwriteAnExistingFile()
{
    TemporaryDirectory directory;
    Check(!directory.Path().empty(),
        "a temporary directory is available for HGLOBAL materialization");
    if (directory.Path().empty())
        return;

    const std::vector<std::byte> original = Bytes("keep me");
    const std::vector<std::byte> replacement = Bytes("new bytes");
    const std::wstring existingPath = directory.Path() + L"\\photo.png";
    Check(WriteTestFile(existingPath, original),
        "the collision fixture is created");

    MockDataObject dataObject;
    dataObject.contentsMedium = MockDataObject::ContentsMedium::Global;
    dataObject.contents = replacement;
    snowdesktop::virtual_file_drop::VirtualFileDescriptor descriptor{
        L"photo.png", 4, replacement.size() };

    const auto materialized =
        snowdesktop::virtual_file_drop::MaterializeFileContents(
            &dataObject, descriptor, directory.Path(), 1024);
    Check(materialized.has_value(),
        "TYMED_HGLOBAL FileContents are materialized beside a collision");
    if (!materialized)
        return;
    Check(ReadTestFile(existingPath) == original,
        "an existing destination file is never overwritten");
    Check(LeafName(materialized->path) == L"photo (1).png" &&
            ReadTestFile(materialized->path) == replacement,
        "a collision receives a unique name with intact content");
    Check(dataObject.fileContentsRequests == 2 &&
            dataObject.lastFileContentsTymed == TYMED_HGLOBAL,
        "HGLOBAL is requested separately only after the streaming medium is unavailable");
}

void TestOversizedStreamRemovesPartialOutput()
{
    TemporaryDirectory directory;
    Check(!directory.Path().empty(),
        "a temporary directory is available for size-limit cleanup");
    if (directory.Path().empty())
        return;

    MockDataObject dataObject;
    dataObject.contentsMedium = MockDataObject::ContentsMedium::Stream;
    dataObject.contents.assign(100 * 1024, std::byte{ 0x5a });
    snowdesktop::virtual_file_drop::VirtualFileDescriptor descriptor{
        L"oversized.bin", 0, std::nullopt };

    const auto materialized =
        snowdesktop::virtual_file_drop::MaterializeFileContents(
            &dataObject, descriptor, directory.Path(), 70 * 1024);
    Check(!materialized,
        "a stream that exceeds the byte limit is rejected");
    Check(CountFiles(directory.Path()) == 0,
        "a partially written oversized stream leaves no output file");

    dataObject.contentsMedium = MockDataObject::ContentsMedium::Global;
    dataObject.contents = Bytes("too large");
    descriptor.advertisedFileSize = dataObject.contents.size();
    const auto globalMaterialized =
        snowdesktop::virtual_file_drop::MaterializeFileContents(
            &dataObject, descriptor, directory.Path(), 1);
    Check(!globalMaterialized && CountFiles(directory.Path()) == 0,
        "an oversized HGLOBAL is rejected without leaving an output file");
}

void TestSanitizedNameExposesTheEffectiveSecuritySuffix()
{
    using snowdesktop::virtual_file_drop::SanitizeSuggestedFileName;
    Check(SanitizeSuggestedFileName(L"..\\payload.exe.") ==
            L"payload.exe" &&
            SanitizeSuggestedFileName(L"payload.exe ") ==
                L"payload.exe" &&
            SanitizeSuggestedFileName(L"payload.ex\u202ee") ==
                L"payload.exe",
        "callers can inspect the effective suffix after path, trailing-character, and bidi sanitization");
}

void TestAdvertisedOversizeIsRejectedBeforeReadingContents()
{
    TemporaryDirectory directory;
    Check(!directory.Path().empty(),
        "a temporary directory is available for advertised-size rejection");
    if (directory.Path().empty())
        return;

    MockDataObject dataObject;
    dataObject.contentsMedium = MockDataObject::ContentsMedium::Stream;
    dataObject.contents = Bytes("not requested");
    snowdesktop::virtual_file_drop::VirtualFileDescriptor descriptor{
        L"huge.bin", 0, 4097 };

    Check(!snowdesktop::virtual_file_drop::MaterializeFileContents(
            &dataObject, descriptor, directory.Path(), 4096),
        "an advertised file larger than the limit is rejected");
    Check(dataObject.fileContentsRequests == 0 &&
            CountFiles(directory.Path()) == 0,
        "an advertised oversize is rejected before content retrieval or file creation");
}

void TestGlobalFallbackRequiresAnAdvertisedBound()
{
    TemporaryDirectory directory;
    Check(!directory.Path().empty(),
        "a temporary directory is available for HGLOBAL bound checks");
    if (directory.Path().empty())
        return;

    MockDataObject dataObject;
    dataObject.contentsMedium = MockDataObject::ContentsMedium::Global;
    dataObject.contents = Bytes("unbounded global bytes");
    snowdesktop::virtual_file_drop::VirtualFileDescriptor descriptor{
        L"unknown-size.bin", 0, std::nullopt };

    Check(!snowdesktop::virtual_file_drop::MaterializeFileContents(
            &dataObject, descriptor, directory.Path(), 4096),
        "HGLOBAL fallback without an advertised byte bound is rejected");
    Check(dataObject.fileContentsRequests == 1 &&
            dataObject.lastFileContentsTymed == TYMED_ISTREAM &&
            CountFiles(directory.Path()) == 0,
        "an unbounded source is probed only for streaming content and leaves no file");
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
    TestStreamContentsUseDescriptorIndexAndSafeLeafName();
    TestGlobalContentsNeverOverwriteAnExistingFile();
    TestOversizedStreamRemovesPartialOutput();
    TestSanitizedNameExposesTheEffectiveSecuritySuffix();
    TestAdvertisedOversizeIsRejectedBeforeReadingContents();
    TestGlobalFallbackRequiresAnAdvertisedBound();
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

#include "virtual_file_drop.h"

#include <windows.h>
#include <shlobj.h>
#include <shldisp.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace
{
constexpr UINT kMaximumDescriptorCount = 4096;
constexpr size_t kMaximumSafeFileNameLength = 180;
constexpr unsigned int kMaximumUniqueNameAttempts = 10000;

class StgMediumScope
{
public:
    explicit StgMediumScope(STGMEDIUM& medium) noexcept
        : medium_(medium)
    {
    }

    ~StgMediumScope()
    {
        ReleaseStgMedium(&medium_);
    }

    StgMediumScope(const StgMediumScope&) = delete;
    StgMediumScope& operator=(const StgMediumScope&) = delete;

private:
    STGMEDIUM& medium_;
};

class GlobalLockScope
{
public:
    explicit GlobalLockScope(HGLOBAL memory) noexcept
        : memory_(memory), data_(GlobalLock(memory))
    {
    }

    ~GlobalLockScope()
    {
        if (data_)
            GlobalUnlock(memory_);
    }

    GlobalLockScope(const GlobalLockScope&) = delete;
    GlobalLockScope& operator=(const GlobalLockScope&) = delete;

    const void* Data() const noexcept
    {
        return data_;
    }

private:
    HGLOBAL memory_ = nullptr;
    void* data_ = nullptr;
};

class CreatedFileScope
{
public:
    CreatedFileScope(HANDLE handle, std::wstring path) noexcept
        : handle_(handle), path_(std::move(path))
    {
    }

    ~CreatedFileScope()
    {
        if (!committed_ && handle_ != INVALID_HANDLE_VALUE)
        {
            BY_HANDLE_FILE_INFORMATION originalIdentity{};
            const bool identityAvailable =
                GetFileInformationByHandle(
                    handle_, &originalIdentity) != FALSE;
            FILE_DISPOSITION_INFO disposition{ TRUE };
            if (!SetFileInformationByHandle(handle_,
                    FileDispositionInfo, &disposition,
                    sizeof(disposition)))
            {
                Close();
                HANDLE cleanup = CreateFileW(path_.c_str(),
                    DELETE | FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE |
                        FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL |
                        FILE_FLAG_OPEN_REPARSE_POINT,
                    nullptr);
                if (cleanup != INVALID_HANDLE_VALUE)
                {
                    BY_HANDLE_FILE_INFORMATION cleanupIdentity{};
                    const bool sameFile = identityAvailable &&
                        GetFileInformationByHandle(
                            cleanup, &cleanupIdentity) &&
                        originalIdentity.dwVolumeSerialNumber ==
                            cleanupIdentity.dwVolumeSerialNumber &&
                        originalIdentity.nFileIndexHigh ==
                            cleanupIdentity.nFileIndexHigh &&
                        originalIdentity.nFileIndexLow ==
                            cleanupIdentity.nFileIndexLow;
                    if (sameFile)
                    {
                        SetFileInformationByHandle(cleanup,
                            FileDispositionInfo, &disposition,
                            sizeof(disposition));
                    }
                    CloseHandle(cleanup);
                }
                return;
            }
        }
        Close();
    }

    CreatedFileScope(const CreatedFileScope&) = delete;
    CreatedFileScope& operator=(const CreatedFileScope&) = delete;

    HANDLE Handle() const noexcept
    {
        return handle_;
    }

    void Commit() noexcept
    {
        Close();
        committed_ = true;
    }

private:
    void Close() noexcept
    {
        if (handle_ == INVALID_HANDLE_VALUE)
            return;
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::wstring path_;
    bool committed_ = false;
};

bool IsReservedWindowsBaseName(std::wstring_view fileName)
{
    const size_t dot = fileName.find(L'.');
    std::wstring base(fileName.substr(0, dot));
    for (wchar_t& character : base)
    {
        if (character >= L'A' && character <= L'Z')
            character = static_cast<wchar_t>(character - L'A' + L'a');
    }
    while (!base.empty() &&
        (base.back() == L' ' || base.back() == L'.'))
        base.pop_back();

    if (base == L"con" || base == L"prn" || base == L"aux" ||
        base == L"nul" || base == L"clock$" ||
        base == L"conin$" || base == L"conout$")
        return true;
    if (base.size() != 4 ||
        (base.substr(0, 3) != L"com" &&
            base.substr(0, 3) != L"lpt"))
        return false;
    return (base[3] >= L'1' && base[3] <= L'9') ||
        base[3] == L'\u00b9' || base[3] == L'\u00b2' ||
        base[3] == L'\u00b3';
}

std::wstring FileExtension(std::wstring_view fileName)
{
    const size_t dot = fileName.find_last_of(L'.');
    if (dot == std::wstring_view::npos || dot == 0)
        return {};
    return std::wstring(fileName.substr(dot));
}

std::wstring SafeFileName(std::wstring value)
{
    const size_t slash = value.find_last_of(L"/\\");
    if (slash != std::wstring::npos)
        value.erase(0, slash + 1);
    std::erase_if(value, [](wchar_t character) {
        return character == L'\u061c' ||
            character == L'\u200e' || character == L'\u200f' ||
            (character >= L'\u202a' && character <= L'\u202e') ||
            (character >= L'\u2066' && character <= L'\u2069');
    });
    for (wchar_t& character : value)
    {
        if (character < L' ' || character == L'\\' ||
            character == L'/' || character == L':' ||
            character == L'*' || character == L'?' ||
            character == L'"' || character == L'<' ||
            character == L'>' || character == L'|')
            character = L'_';
    }
    while (!value.empty() &&
        (value.back() == L' ' || value.back() == L'.'))
        value.pop_back();
    if (value.empty() || value == L"." || value == L"..")
        value = L"virtual-file";
    if (IsReservedWindowsBaseName(value))
        value.insert(value.begin(), L'_');

    if (value.size() > kMaximumSafeFileNameLength)
    {
        const std::wstring extension = FileExtension(value);
        const size_t extensionLength = std::min<size_t>(
            extension.size(), 20);
        value.resize(kMaximumSafeFileNameLength - extensionLength);
        if (!value.empty() &&
            value.back() >= 0xd800 && value.back() <= 0xdbff)
            value.pop_back();
        value.append(extension.substr(extension.size() - extensionLength));
    }
    return value;
}

std::wstring JoinPath(std::wstring_view directory,
    std::wstring_view fileName)
{
    std::wstring result(directory);
    if (!result.empty() && result.back() != L'\\' &&
        result.back() != L'/')
        result.push_back(L'\\');
    result.append(fileName);
    return result;
}

std::optional<std::pair<HANDLE, std::wstring>> CreateUniqueFile(
    std::wstring_view destinationDirectory,
    std::wstring_view safeFileName)
{
    const std::wstring originalExtension = FileExtension(safeFileName);
    std::wstring extension = originalExtension;
    if (extension.size() > 20)
        extension.erase(0, extension.size() - 20);
    std::wstring stem(safeFileName.substr(
        0, safeFileName.size() - originalExtension.size()));

    for (unsigned int attempt = 0;
         attempt < kMaximumUniqueNameAttempts; ++attempt)
    {
        std::wstring suffix;
        if (attempt != 0)
            suffix = L" (" + std::to_wstring(attempt) + L")";
        const size_t maximumStemLength =
            kMaximumSafeFileNameLength -
            std::min(kMaximumSafeFileNameLength,
                suffix.size() + extension.size());
        std::wstring candidateStem = stem.substr(0, maximumStemLength);
        if (!candidateStem.empty() &&
            candidateStem.back() >= 0xd800 &&
            candidateStem.back() <= 0xdbff)
            candidateStem.pop_back();
        const std::wstring candidateName = candidateStem + suffix +
            extension;
        const std::wstring candidatePath = JoinPath(
            destinationDirectory, candidateName);
        HANDLE file = CreateFileW(candidatePath.c_str(),
            GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file != INVALID_HANDLE_VALUE)
            return std::pair<HANDLE, std::wstring>(
                file, candidatePath);
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS &&
            error != ERROR_ALREADY_EXISTS)
            return std::nullopt;
    }
    return std::nullopt;
}

bool WriteBytes(HANDLE file, const void* data, size_t size)
{
    const auto* bytes = static_cast<const std::byte*>(data);
    while (size != 0)
    {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(
            size, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(file, bytes, chunk, &written, nullptr) ||
            written == 0 || written > chunk)
            return false;
        bytes += written;
        size -= written;
    }
    return true;
}

bool WriteStream(HANDLE file, IStream* stream,
    std::uint64_t maximumBytes, std::uint64_t& writtenBytes)
{
    if (!stream)
        return false;

    std::array<std::byte, 64 * 1024> buffer{};
    writtenBytes = 0;
    while (true)
    {
        ULONG requested = static_cast<ULONG>(buffer.size());
        const std::uint64_t remaining = maximumBytes - writtenBytes;
        if (remaining < buffer.size())
            requested = static_cast<ULONG>(remaining + 1);

        ULONG read = 0;
        const HRESULT result = stream->Read(
            buffer.data(), requested, &read);
        if (FAILED(result) || read > requested ||
            read > maximumBytes - writtenBytes)
            return false;
        if (read != 0 && !WriteBytes(file, buffer.data(), read))
            return false;
        writtenBytes += read;
        if (read == 0 || result == S_FALSE)
            return true;
    }
}

bool WriteGlobal(HANDLE file, HGLOBAL memory,
    std::uint64_t maximumBytes, std::uint64_t& writtenBytes)
{
    if (!memory)
        return false;
    SetLastError(ERROR_SUCCESS);
    const SIZE_T size = GlobalSize(memory);
    if (size == 0 && GetLastError() != ERROR_SUCCESS)
        return false;
    if (size > maximumBytes)
        return false;
    writtenBytes = static_cast<std::uint64_t>(size);
    if (size == 0)
        return true;
    GlobalLockScope lock(memory);
    return lock.Data() && WriteBytes(file, lock.Data(), size);
}

std::optional<std::wstring> ReadWideFilename(
    const FILEDESCRIPTORW& descriptor)
{
    const auto end = std::end(descriptor.cFileName);
    const auto terminator = std::find(
        std::begin(descriptor.cFileName), end, L'\0');
    if (terminator == end || terminator == std::begin(descriptor.cFileName))
        return std::nullopt;
    return std::wstring(
        std::begin(descriptor.cFileName), terminator);
}

std::optional<std::wstring> ReadAnsiFilename(
    const FILEDESCRIPTORA& descriptor)
{
    const auto end = std::end(descriptor.cFileName);
    const auto terminator = std::find(
        std::begin(descriptor.cFileName), end, '\0');
    if (terminator == end || terminator == std::begin(descriptor.cFileName))
        return std::nullopt;

    const int sourceLength = static_cast<int>(
        terminator - std::begin(descriptor.cFileName));
    const int required = MultiByteToWideChar(
        CP_ACP, 0, descriptor.cFileName, sourceLength,
        nullptr, 0);
    if (required <= 0)
        return std::nullopt;
    std::wstring result(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_ACP, 0, descriptor.cFileName, sourceLength,
            result.data(), required) != required)
        return std::nullopt;
    return result;
}

template <typename Group, typename Descriptor, typename ReadFilename>
std::vector<snowdesktop::virtual_file_drop::VirtualFileDescriptor>
ParseDescriptorGroup(HGLOBAL memory, ReadFilename readFilename)
{
    static_assert(std::is_trivially_copyable_v<Descriptor>);
    std::vector<snowdesktop::virtual_file_drop::VirtualFileDescriptor>
        result;
    if (!memory)
        return result;

    constexpr size_t headerSize = offsetof(Group, fgd);
    const SIZE_T memorySize = GlobalSize(memory);
    if (memorySize < headerSize)
        return result;

    GlobalLockScope lock(memory);
    if (!lock.Data())
        return result;
    const auto* bytes = static_cast<const std::byte*>(lock.Data());

    UINT count = 0;
    std::memcpy(&count, bytes, sizeof(count));
    if (count > kMaximumDescriptorCount ||
        count > (memorySize - headerSize) / sizeof(Descriptor))
        return result;

    result.reserve(count);
    for (UINT index = 0; index < count; ++index)
    {
        Descriptor descriptor{};
        std::memcpy(&descriptor,
            bytes + headerSize + static_cast<size_t>(index) *
                sizeof(Descriptor),
            sizeof(descriptor));

        const std::optional<std::wstring> filename =
            readFilename(descriptor);
        if (!filename)
        {
            result.clear();
            return result;
        }
        if ((descriptor.dwFlags & FD_ATTRIBUTES) != 0 &&
            (descriptor.dwFileAttributes &
                FILE_ATTRIBUTE_DIRECTORY) != 0)
            continue;

        std::optional<std::uint64_t> advertisedFileSize;
        if ((descriptor.dwFlags & FD_FILESIZE) != 0)
        {
            advertisedFileSize =
                (static_cast<std::uint64_t>(descriptor.nFileSizeHigh)
                    << 32) |
                static_cast<std::uint64_t>(descriptor.nFileSizeLow);
        }
        result.push_back({ *filename,
            static_cast<std::uint32_t>(index),
            advertisedFileSize });
    }
    return result;
}

template <typename Group, typename Descriptor, typename ReadFilename>
std::optional<std::vector<
    snowdesktop::virtual_file_drop::VirtualFileDescriptor>>
TryReadFormat(IDataObject* dataObject, CLIPFORMAT format,
    ReadFilename readFilename)
{
    FORMATETC formatEtc{};
    formatEtc.cfFormat = format;
    formatEtc.dwAspect = DVASPECT_CONTENT;
    formatEtc.lindex = -1;
    formatEtc.tymed = TYMED_HGLOBAL;

    STGMEDIUM medium{};
    if (FAILED(dataObject->GetData(&formatEtc, &medium)))
        return std::nullopt;
    StgMediumScope release(medium);
    if (medium.tymed != TYMED_HGLOBAL || !medium.hGlobal)
        return std::vector<
            snowdesktop::virtual_file_drop::VirtualFileDescriptor>{};

    return ParseDescriptorGroup<Group, Descriptor>(
        medium.hGlobal, readFilename);
}
} // namespace

std::vector<snowdesktop::virtual_file_drop::VirtualFileDescriptor>
snowdesktop::virtual_file_drop::ReadDescriptors(IDataObject* dataObject)
{
    if (!dataObject)
        return {};

    const CLIPFORMAT wideFormat = static_cast<CLIPFORMAT>(
        RegisterClipboardFormatW(L"FileGroupDescriptorW"));
    if (wideFormat != 0)
    {
        auto wide = TryReadFormat<FILEGROUPDESCRIPTORW, FILEDESCRIPTORW>(
            dataObject, wideFormat, ReadWideFilename);
        if (wide)
            return std::move(*wide);
    }

    const CLIPFORMAT ansiFormat = static_cast<CLIPFORMAT>(
        RegisterClipboardFormatW(L"FileGroupDescriptor"));
    if (ansiFormat == 0)
        return {};
    auto ansi = TryReadFormat<FILEGROUPDESCRIPTORA, FILEDESCRIPTORA>(
        dataObject, ansiFormat, ReadAnsiFilename);
    return ansi ? std::move(*ansi) :
        std::vector<VirtualFileDescriptor>{};
}

std::wstring snowdesktop::virtual_file_drop::
SanitizeSuggestedFileName(std::wstring_view suggestedFileName)
{
    return SafeFileName(std::wstring(suggestedFileName));
}

std::optional<snowdesktop::virtual_file_drop::MaterializedVirtualFile>
snowdesktop::virtual_file_drop::MaterializeFileContents(
    IDataObject* dataObject,
    const VirtualFileDescriptor& descriptor,
    std::wstring_view destinationDirectory,
    std::uint64_t maximumBytes)
{
    if (!dataObject || destinationDirectory.empty() ||
        descriptor.descriptorIndex >
            static_cast<std::uint32_t>(
                std::numeric_limits<LONG>::max()) ||
        (descriptor.advertisedFileSize &&
            *descriptor.advertisedFileSize > maximumBytes))
        return std::nullopt;

    const std::wstring directory(destinationDirectory);
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return std::nullopt;

    const CLIPFORMAT contentsFormat = static_cast<CLIPFORMAT>(
        RegisterClipboardFormatW(L"FileContents"));
    if (contentsFormat == 0)
        return std::nullopt;

    STGMEDIUM medium{};
    FORMATETC streamFormat{};
    streamFormat.cfFormat = contentsFormat;
    streamFormat.dwAspect = DVASPECT_CONTENT;
    streamFormat.lindex = static_cast<LONG>(
        descriptor.descriptorIndex);
    streamFormat.tymed = TYMED_ISTREAM;
    DWORD expectedMedium = TYMED_ISTREAM;
    if (FAILED(dataObject->GetData(&streamFormat, &medium)))
    {
        // Some Chromium-based desktop clients expose FileContents only as
        // HGLOBAL and omit the optional FD_FILESIZE descriptor flag. Request
        // that standard medium after streaming fails; WriteGlobal validates
        // the actual GlobalSize against maximumBytes before copying it.
        FORMATETC globalFormat = streamFormat;
        globalFormat.tymed = TYMED_HGLOBAL;
        if (FAILED(dataObject->GetData(&globalFormat, &medium)))
            return std::nullopt;
        expectedMedium = TYMED_HGLOBAL;
    }
    StgMediumScope release(medium);

    if (medium.tymed != expectedMedium ||
        (medium.tymed == TYMED_ISTREAM && !medium.pstm) ||
        (medium.tymed == TYMED_HGLOBAL && !medium.hGlobal))
        return std::nullopt;

    const std::wstring safeFileName = SanitizeSuggestedFileName(
        descriptor.suggestedFileName);
    auto created = CreateUniqueFile(directory, safeFileName);
    if (!created)
        return std::nullopt;
    const std::wstring outputPath = created->second;
    CreatedFileScope output(created->first, outputPath);

    std::uint64_t writtenBytes = 0;
    bool success = false;
    if (medium.tymed == TYMED_ISTREAM)
        success = WriteStream(output.Handle(), medium.pstm,
            maximumBytes, writtenBytes);
    else if (medium.tymed == TYMED_HGLOBAL)
        success = WriteGlobal(output.Handle(), medium.hGlobal,
            maximumBytes, writtenBytes);
    if (!success)
        return std::nullopt;

    output.Commit();
    return MaterializedVirtualFile{ outputPath, writtenBytes };
}

bool snowdesktop::virtual_file_drop::UsesAsyncMode(
    IDataObject* dataObject)
{
    if (!dataObject)
        return false;

    IDataObjectAsyncCapability* capability = nullptr;
    if (FAILED(dataObject->QueryInterface(
            __uuidof(IDataObjectAsyncCapability),
            reinterpret_cast<void**>(&capability))) ||
        !capability)
        return false;

    BOOL asyncMode = FALSE;
    const HRESULT result = capability->GetAsyncMode(&asyncMode);
    capability->Release();
    return result == S_OK && asyncMode == TRUE;
}

bool snowdesktop::virtual_file_drop::OffersAsyncFileDrop(
    IDataObject* dataObject)
{
    if (!dataObject)
        return false;

    FORMATETC fileDrop{};
    fileDrop.cfFormat = CF_HDROP;
    fileDrop.dwAspect = DVASPECT_CONTENT;
    fileDrop.lindex = -1;
    fileDrop.tymed = TYMED_HGLOBAL;
    return dataObject->QueryGetData(&fileDrop) == S_OK &&
        UsesAsyncMode(dataObject);
}

#include "virtual_file_drop.h"

#include <windows.h>
#include <shlobj.h>
#include <shldisp.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <optional>
#include <type_traits>

namespace
{
constexpr UINT kMaximumDescriptorCount = 4096;

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

        result.push_back({ *filename,
            static_cast<std::uint32_t>(index) });
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
    if (dataObject->QueryGetData(&fileDrop) != S_OK)
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

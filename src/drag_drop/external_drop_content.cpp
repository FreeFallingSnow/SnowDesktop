#include "external_drop_content.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shldisp.h>
#include <cstddef>
#include <cstring>
#include <iterator>

namespace snowdesktop::external_drop_content
{
namespace
{
Paths ReadSelectionPaths(IDataObject* source)
{
    // Request only CIDA metadata. The Shell convenience array converter may
    // fall back to CF_HDROP, which can render a delayed provider before Drop.
    const auto id = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_SHELLIDLIST));
    FORMATETC format{id, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    if (!id || source->QueryGetData(&format) != S_OK) return {};
    STGMEDIUM medium{};
    if (FAILED(source->GetData(&format, &medium))) return {};
    struct ReleaseMedium
    {
        STGMEDIUM& value;
        ~ReleaseMedium() { ReleaseStgMedium(&value); }
    } release{medium};
    if (medium.tymed != TYMED_HGLOBAL || !medium.hGlobal) return {};
    const SIZE_T size = GlobalSize(medium.hGlobal);
    const auto* bytes = static_cast<const std::byte*>(GlobalLock(medium.hGlobal));
    if (!bytes) return {};
    struct Unlock
    {
        HGLOBAL value;
        ~Unlock() { GlobalUnlock(value); }
    } unlock{medium.hGlobal};
    if (size < 2 * sizeof(UINT)) return {};
    UINT count = 0;
    std::memcpy(&count, bytes, sizeof(count));
    if (!count || count > 128) return {};
    const SIZE_T tableBytes = (static_cast<SIZE_T>(count) + 2) * sizeof(UINT);
    if (tableBytes > size) return {};
    const auto pidl = [&](UINT index) -> PCUIDLIST_RELATIVE {
        UINT offset = 0;
        std::memcpy(&offset, bytes + (static_cast<SIZE_T>(index) + 1) * sizeof(UINT), sizeof(offset));
        if (offset < tableBytes || offset >= size) return nullptr;
        for (SIZE_T at = offset; at <= size && size - at >= sizeof(USHORT);)
        {
            USHORT length = 0;
            std::memcpy(&length, bytes + at, sizeof(length));
            if (!length) return reinterpret_cast<PCUIDLIST_RELATIVE>(bytes + offset);
            if (length < sizeof(USHORT) || length > size - at) return nullptr;
            at += length;
        }
        return nullptr;
    };
    const auto parent = reinterpret_cast<PCIDLIST_ABSOLUTE>(pidl(0));
    if (!parent) return {};
    Paths paths;
    for (UINT index = 1; index <= count; ++index)
    {
        const auto child = pidl(index);
        if (!child) return {};
        PIDLIST_ABSOLUTE absolute = ILCombine(parent, child);
        if (!absolute) return {};
        wchar_t path[32768]{};
        const BOOL filesystem = SHGetPathFromIDListEx(absolute, path,
            static_cast<DWORD>(std::size(path)), GPFIDL_DEFAULT);
        ILFree(absolute);
        if (!filesystem || !path[0]) return {};
        paths.emplace_back(path);
    }
    return paths;
}
}

ClipboardFileSource ProbeClipboardFileSource(IDataObject* source)
{
    if (!source) return ClipboardFileSource::None;
    const auto offers = [source](CLIPFORMAT id) {
        FORMATETC format{id, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        return id != 0 && source->QueryGetData(&format) == S_OK;
    };
    if (offers(CF_HDROP)) return ClipboardFileSource::FilePaths;
    for (const auto* name : {L"Shell IDList Array", L"FileGroupDescriptorW",
                            L"FileGroupDescriptor"})
        if (offers(static_cast<CLIPFORMAT>(RegisterClipboardFormatW(name))))
            return ClipboardFileSource::ShellObjects;
    return ClipboardFileSource::None;
}

std::optional<ShellFileSource> ReadShellFileSource(IDataObject* source)
{
    if (!source) return std::nullopt;
    const auto offers = [source](CLIPFORMAT id) {
        FORMATETC format{id, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        return id && source->QueryGetData(&format) == S_OK;
    };
    if (offers(CF_HDROP) ||
        !offers(static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_SHELLIDLIST))) ||
        (!offers(static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW))) &&
         !offers(static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptor")))))
        return std::nullopt;
    Microsoft::WRL::ComPtr<IShellItemArray> array;
    if (FAILED(SHCreateShellItemArrayFromDataObject(source, IID_PPV_ARGS(&array))))
        return std::nullopt;
    DWORD count = 0;
    if (FAILED(array->GetCount(&count)) || !count || count > 4096)
        return std::nullopt;
    ShellFileSource result;
    for (DWORD index = 0; index < count; ++index)
    {
        Microsoft::WRL::ComPtr<IShellItem> item;
        SFGAOF attributes = 0;
        if (FAILED(array->GetItemAt(index, &item)) ||
            FAILED(item->GetAttributes(SFGAO_CANCOPY | SFGAO_CANMOVE | SFGAO_STREAM |
                SFGAO_FOLDER | SFGAO_STORAGE, &attributes)) ||
            !(attributes & SFGAO_CANCOPY) ||
            (!(attributes & SFGAO_STREAM) &&
             (attributes & (SFGAO_FOLDER | SFGAO_STORAGE)) != (SFGAO_FOLDER | SFGAO_STORAGE)))
            return std::nullopt;
        PWSTR identity = nullptr, name = nullptr;
        const HRESULT identityStatus = item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &identity);
        const HRESULT nameStatus = item->GetDisplayName(SIGDN_NORMALDISPLAY, &name);
        if (FAILED(identityStatus) || FAILED(nameStatus) || !identity || !name)
        {
            CoTaskMemFree(identity); CoTaskMemFree(name);
            return std::nullopt;
        }
        result.identities.emplace_back(identity);
        result.names.emplace_back(name);
        CoTaskMemFree(identity); CoTaskMemFree(name);
        result.canMove &= (attributes & SFGAO_CANMOVE) != 0;
        result.items.push_back(std::move(item));
    }
    return result;
}

FileSource ProbeFileSource(IDataObject* source)
{
    FileSource result;
    if (!source) return result;
    IDataObjectAsyncCapability* capability = nullptr;
    if (SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(&capability))) && capability)
    {
        BOOL enabled = FALSE;
        result.asynchronous = capability->GetAsyncMode(&enabled) == S_OK && enabled;
        capability->Release();
    }
    if (result.asynchronous)
    {
        FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        result.available = source->QueryGetData(&format) == S_OK;
        if (result.available) result.previewPaths = ReadSelectionPaths(source);
    }
    else
    {
        result.paths = ReadFilePaths(source);
        result.available = !result.paths.empty() && result.paths.size() <= 128;
    }
    return result;
}

Paths ReadFilePaths(IDataObject* source)
{
    if (!source) return {};
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    Paths paths;
    if (FAILED(source->GetData(&format, &medium))) return paths;
    if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal)
    {
        const auto drop = static_cast<HDROP>(medium.hGlobal);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT index = 0; index < count; ++index)
        {
            const UINT length = DragQueryFileW(drop, index, nullptr, 0);
            if (length == 0 || length > 32767) continue;
            std::wstring path(length + 1, L'\0');
            if (DragQueryFileW(drop, index, path.data(), length + 1) == length)
            {
                path.resize(length);
                paths.push_back(std::move(path));
            }
        }
    }
    ReleaseStgMedium(&medium);
    return paths;
}

Content Read(bool /*asynchronousSource*/, bool allowContent, const Readers& readers)
{
    // Async sources reach this function on the worker after StartOperation.
    // Capability affects scheduling/lifetime, never whether files are read.
    Content result;
    if (readers.files) result.paths = readers.files();
    if (!result.paths.empty() || !allowContent) return result;
    if (readers.localFileUrls) result.paths = readers.localFileUrls();
    if (!result.paths.empty())
    {
        result.copyOnly = true;
        return result;
    }
    result.owned = true;
    // A preview bitmap or URL must not replace a declared file batch. An
    // incomplete batch fails as a whole instead of silently dropping entries.
    if (readers.virtualFileCount != 0)
    {
        if (readers.virtualFiles) result.paths = readers.virtualFiles();
        return result;
    }
    if (readers.image) result.paths = readers.image();
    if (!result.paths.empty()) return result;
    if (readers.dataUrl) result.paths = readers.dataUrl();
    if (!result.paths.empty()) return result;
    if (readers.virtualFiles) result.paths = readers.virtualFiles();
    if (!result.paths.empty()) return result;
    const Paths urls = readers.urls ? readers.urls() : Paths{};
    if (!urls.empty())
    {
        if (!readers.download)
        {
            result.pendingUrls = urls;
            return result;
        }
        result.paths = readers.download(urls);
        if (!result.paths.empty()) return result;
    }
    if (readers.shortcut) result.paths = readers.shortcut();
    if (!result.paths.empty()) return result;
    if (readers.text) result.paths = readers.text();
    return result;
}
}

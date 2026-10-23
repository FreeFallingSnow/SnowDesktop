#include "external_drop_content.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shldisp.h>

namespace snowdesktop::external_drop_content
{
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

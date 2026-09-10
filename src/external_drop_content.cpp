#include "external_drop_content.h"

#include <shellapi.h>

namespace snowdesktop::external_drop_content
{
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

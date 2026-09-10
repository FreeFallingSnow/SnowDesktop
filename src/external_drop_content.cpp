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
            wchar_t path[MAX_PATH]{};
            if (DragQueryFileW(drop, index, path, MAX_PATH) > 0)
                paths.emplace_back(path);
        }
    }
    ReleaseStgMedium(&medium);
    return paths;
}

Content Read(bool asynchronousSource, bool allowContent, const Readers& readers)
{
    // Preserve the current host behavior while exposing its execution boundary
    // to regression tests. Async component ingress currently has no read path.
    if (asynchronousSource) return {};
    Content result;
    if (readers.files) result.paths = readers.files();
    if (!result.paths.empty() || !allowContent) return result;
    result.owned = true;
    if (readers.image) result.paths = readers.image();
    if (!result.paths.empty()) return result;
    if (readers.dataUrl) result.paths = readers.dataUrl();
    if (!result.paths.empty()) return result;
    if (readers.shortcut) result.paths = readers.shortcut();
    if (!result.paths.empty()) return result;
    if (readers.text) result.paths = readers.text();
    return result;
}
}

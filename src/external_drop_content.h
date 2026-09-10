#pragma once

#include <windows.h>
#include <oleidl.h>
#include <functional>
#include <string>
#include <vector>

namespace snowdesktop::external_drop_content
{
using Paths = std::vector<std::wstring>;

struct Content
{
    Paths paths;
    Paths pendingUrls;
    bool owned = false;
    bool copyOnly = false;
};

// Decoder/IO boundaries; selection and fallback live in Read, which is also
// used by the host. No UI objects may be captured when dispatched to a worker.
struct Readers
{
    std::function<Paths()> files;
    std::function<Paths()> localFileUrls;
    std::function<Paths()> image;
    std::function<Paths()> dataUrl;
    std::function<Paths()> virtualFiles;
    size_t virtualFileCount = 0;
    std::function<Paths()> urls;
    std::function<Paths(const Paths&)> download;
    std::function<Paths()> shortcut;
    std::function<Paths()> text;
};

Paths ReadFilePaths(IDataObject* source);
Content Read(bool asynchronousSource, bool allowContent, const Readers& readers);
}

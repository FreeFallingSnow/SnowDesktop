#pragma once

#include <windows.h>
#include <oleidl.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <functional>
#include <optional>
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
enum class ClipboardFileSource { None, FilePaths, ShellObjects };
// Menu admission only: never render files/descriptors or start source IO.
// Shell keeps ownership of namespace IDs and virtual-file folder hierarchies.
ClipboardFileSource ProbeClipboardFileSource(IDataObject* source);
// Resolved only after Drop: real Shell files retain their provider instead of
// being read through the bounded browser/private-stream content decoder.
struct ShellFileSource
{
    std::vector<Microsoft::WRL::ComPtr<IShellItem>> items;
    Paths identities;
    Paths names;
    bool canMove = true;
};
std::optional<ShellFileSource> ReadShellFileSource(IDataObject* source);
struct FileSource
{
    bool asynchronous = false;
    bool available = false;
    Paths paths;
};
// Delayed CF_HDROP is advertised at hover time without rendering its data.
// ReadFilePaths runs later on the source's marshaled apartment after Drop.
FileSource ProbeFileSource(IDataObject* source);
Content Read(bool asynchronousSource, bool allowContent, const Readers& readers);
}

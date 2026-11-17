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

struct ApplicationSource
{
    std::wstring id;
    std::wstring name;
    std::vector<BYTE> targetId;
};
// Read Shell selection metadata without rendering a delayed file payload.
std::optional<ApplicationSource> ReadApplicationSource(IDataObject* source);
// Some Start menu sources advertise an AppID in CF_HDROP. Resolve only a
// single bare ID that matches an installed AppsFolder item.
std::optional<ApplicationSource> ResolveApplicationSource(const Paths& paths);

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
    // Optional Shell selection identities, never materialized file content.
    // Available for Explorer's delayed CF_HDROP without starting its operation.
    Paths previewPaths;
};
// Delayed CF_HDROP is advertised at hover time without rendering its data.
// ReadFilePaths runs later on the source's marshaled apartment after Drop.
FileSource ProbeFileSource(IDataObject* source);
Content Read(bool asynchronousSource, bool allowContent, const Readers& readers);
}

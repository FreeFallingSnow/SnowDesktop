#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct IDataObject;

namespace snowdesktop::virtual_file_drop
{

/**
 * A non-directory entry advertised through a Shell virtual-file descriptor.
 * descriptorIndex is the FORMATETC::lindex used to request FileContents.
 */
struct VirtualFileDescriptor
{
    std::wstring suggestedFileName;
    std::uint32_t descriptorIndex = 0;
    std::optional<std::uint64_t> advertisedFileSize;
};

inline constexpr std::uint64_t kDefaultMaximumMaterializedFileBytes =
    512ull * 1024ull * 1024ull;

struct MaterializedVirtualFile
{
    std::wstring path;
    std::uint64_t sizeBytes = 0;
};

/**
 * Read non-directory FileGroupDescriptor entries from an OLE data object.
 *
 * The Unicode format is authoritative when it is available. The ANSI format
 * is queried only when the Unicode format is unavailable. Malformed media are
 * rejected without reading FileContents.
 */
std::vector<VirtualFileDescriptor> ReadDescriptors(
    IDataObject* dataObject);

/**
 * Return the exact Windows-safe leaf name used by materialization.
 * Callers applying extension or file-type policy must inspect this value,
 * because trimming and bidi-control removal can change the effective suffix.
 */
std::wstring SanitizeSuggestedFileName(
    std::wstring_view suggestedFileName);

/**
 * Save one FileContents entry into an existing, non-reparse destination
 * directory.
 *
 * The descriptor index is used as FORMATETC::lindex. Only byte-oriented
 * TYMED_ISTREAM and TYMED_HGLOBAL media are accepted; structured-storage
 * media are rejected because they cannot be copied safely as an ordinary
 * file without a separate compound-storage serialization contract. Streaming
 * is preferred; HGLOBAL is attempted only when advertisedFileSize is present
 * and within the requested bound.
 *
 * The advertised name is reduced to one Windows-safe leaf name. Existing
 * files are never replaced, the byte limit is enforced while streaming, and
 * a partially written file is removed on failure. This is a synchronous COM
 * call and must run in an apartment where dataObject is valid; callers moving
 * it to a worker must marshal the interface rather than capture a raw pointer.
 */
std::optional<MaterializedVirtualFile> MaterializeFileContents(
    IDataObject* dataObject,
    const VirtualFileDescriptor& descriptor,
    std::wstring_view destinationDirectory,
    std::uint64_t maximumBytes =
        kDefaultMaximumMaterializedFileBytes);

/**
 * Check whether a data object offers delayed CF_HDROP materialization through
 * IDataObjectAsyncCapability. This probe never reads data or starts an async
 * operation.
 */
bool OffersAsyncFileDrop(IDataObject* dataObject);

} // namespace snowdesktop::virtual_file_drop

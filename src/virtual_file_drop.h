#pragma once

#include <cstdint>
#include <string>
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
 * Check whether a data object offers delayed CF_HDROP materialization through
 * IDataObjectAsyncCapability. This probe never reads data or starts an async
 * operation.
 */
bool OffersAsyncFileDrop(IDataObject* dataObject);

} // namespace snowdesktop::virtual_file_drop

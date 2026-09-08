#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

struct IDataObject;

namespace snowdesktop::drop_image_data
{
enum class SourceFormat : std::uint8_t
{
    None,
    DibV5,
    Dib,
    Png,
    Bitmap,
};

enum class SaveStatus : std::uint8_t
{
    Saved,
    FormatUnavailable,
    DataInvalid,
    OutputFailed,
};

struct Limits
{
    std::size_t maximumInputBytes = 64 * 1024 * 1024;
    std::uint32_t maximumDimension = 16384;
    std::uint64_t maximumPixelCount = 64ull * 1024 * 1024;
    std::size_t maximumPngBytes = 256 * 1024 * 1024;
    bool allowStreamInput = true;
};

struct SaveResult
{
    SaveStatus status = SaveStatus::FormatUnavailable;
    SourceFormat source = SourceFormat::None;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    explicit operator bool() const noexcept
    {
        return status == SaveStatus::Saved;
    }
};

// This is a read-only capability probe. Extraction still revalidates every
// medium because IDataObject implementations may render formats lazily.
bool OffersImageData(IDataObject* dataObject) noexcept;

// Restricts the probe to HGLOBAL/GDI representations that do not require
// consuming a producer-backed IStream on the OLE/UI thread.
bool OffersImmediateImageData(IDataObject* dataObject) noexcept;

// Tries standard OLE image representations in fidelity order:
// CF_DIBV5, the registered "PNG" / "image/png" formats, CF_DIB, then
// CF_BITMAP. The final path is created with CREATE_NEW only after a bounded
// PNG has been encoded, so an existing file is never overwritten and a
// failed encode leaves no partial destination behind.
SaveResult SaveAsPng(IDataObject* dataObject,
    const std::filesystem::path& destination,
    const Limits& limits = {}) noexcept;
}

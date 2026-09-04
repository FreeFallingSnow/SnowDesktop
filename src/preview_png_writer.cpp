#include "preview_png_writer.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <limits>

namespace snowdesktop::preview_png
{
namespace
{
using Microsoft::WRL::ComPtr;

std::string HresultText(HRESULT result)
{
    char buffer[16]{};
    sprintf_s(buffer, "0x%08lx", static_cast<unsigned long>(result));
    return buffer;
}
} // namespace

bool Save(const std::filesystem::path& output,
    int width, int height, std::span<const std::uint32_t> pixels,
    std::string& error)
{
    if (width <= 0 || height <= 0 ||
        pixels.size() != static_cast<std::size_t>(width) * height)
    {
        error = "preview pixels do not match the requested PNG size";
        return false;
    }
    const std::filesystem::path parent = output.parent_path().empty()
        ? std::filesystem::current_path() : output.parent_path();
    std::error_code filesystemError;
    if (!std::filesystem::is_directory(parent, filesystemError))
    {
        error = "output parent directory does not exist";
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(result))
    {
        error = "cannot create the WIC imaging factory: " +
            HresultText(result);
        return false;
    }

    const std::filesystem::path temporary = parent /
        (L".snowdesktop-preview-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()) + L".tmp");
    ComPtr<IWICStream> stream;
    result = factory->CreateStream(&stream);
    if (SUCCEEDED(result))
        result = stream->InitializeFromFilename(
            temporary.c_str(), GENERIC_WRITE);
    ComPtr<IWICBitmapEncoder> encoder;
    if (SUCCEEDED(result))
        result = factory->CreateEncoder(
            GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(result))
        result = encoder->Initialize(
            stream.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> frame;
    if (SUCCEEDED(result))
        result = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(result)) result = frame->Initialize(nullptr);
    if (SUCCEEDED(result))
        result = frame->SetSize(
            static_cast<UINT>(width), static_cast<UINT>(height));
    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppPBGRA;
    if (SUCCEEDED(result)) result = frame->SetPixelFormat(&pixelFormat);
    ComPtr<IWICBitmap> bitmap;
    const UINT stride = static_cast<UINT>(width * sizeof(std::uint32_t));
    const std::uint64_t byteCount = static_cast<std::uint64_t>(stride) *
        static_cast<unsigned>(height);
    if (byteCount > (std::numeric_limits<UINT>::max)())
        result = E_INVALIDARG;
    if (SUCCEEDED(result))
    {
        result = factory->CreateBitmapFromMemory(
            static_cast<UINT>(width), static_cast<UINT>(height),
            GUID_WICPixelFormat32bppPBGRA, stride,
            static_cast<UINT>(byteCount),
            reinterpret_cast<BYTE*>(const_cast<std::uint32_t*>(
                pixels.data())), &bitmap);
    }
    if (SUCCEEDED(result)) result = frame->WriteSource(bitmap.Get(), nullptr);
    if (SUCCEEDED(result)) result = frame->Commit();
    if (SUCCEEDED(result)) result = encoder->Commit();
    frame.Reset();
    encoder.Reset();
    stream.Reset();

    if (FAILED(result))
    {
        std::filesystem::remove(temporary, filesystemError);
        error = "cannot encode the preview PNG: " + HresultText(result);
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), output.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        const DWORD moveError = GetLastError();
        std::filesystem::remove(temporary, filesystemError);
        error = "cannot commit the preview PNG: Windows error " +
            std::to_string(moveError);
        return false;
    }
    return true;
}

} // namespace snowdesktop::preview_png

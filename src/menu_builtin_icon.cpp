#include "menu_builtin_icon.h"
#include "resource.h"

#include <array>
#include <wincodec.h>
#include <wrl/client.h>

namespace snowdesktop::menu_icon
{
namespace
{
using Microsoft::WRL::ComPtr;

struct ComScope
{
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};

constexpr std::array<std::array<int, 2>, 17> kResources{{
    { 0, 0 },
    { IDR_MENU_SORT_NATIVE_LIGHT, IDR_MENU_SORT_NATIVE_DARK },
    { IDR_MENU_DISPLAY_NATIVE_LIGHT, IDR_MENU_DISPLAY_NATIVE_DARK },
    { IDR_MENU_WIDGETS_NATIVE_LIGHT, IDR_MENU_WIDGETS_NATIVE_DARK },
    { IDR_MENU_PIN_NATIVE_LIGHT, IDR_MENU_PIN_NATIVE_DARK },
    { IDR_MENU_ADD_PAGE_NATIVE_LIGHT, IDR_MENU_ADD_PAGE_NATIVE_DARK },
    { IDR_MENU_SETTINGS_NATIVE_LIGHT, IDR_MENU_SETTINGS_NATIVE_DARK },
    { IDR_MENU_PASTE_NATIVE_LIGHT, IDR_MENU_PASTE_NATIVE_DARK },
    { IDR_MENU_NEW_ITEM_NATIVE_LIGHT, IDR_MENU_NEW_ITEM_NATIVE_DARK },
    { IDR_MENU_REFRESH_NATIVE_LIGHT, IDR_MENU_REFRESH_NATIVE_DARK },
    { IDR_MENU_COLLECTION, IDR_MENU_COLLECTION },
    { IDR_MENU_COLLECTION_GROUP, IDR_MENU_COLLECTION_GROUP },
    { IDR_MENU_FILE_GROUP, IDR_MENU_FILE_GROUP },
    { IDR_MENU_DESKTOP_FILES, IDR_MENU_DESKTOP_FILES },
    { IDR_MENU_FOLDER_MAPPING, IDR_MENU_FOLDER_MAPPING },
    { IDR_MENU_SEARCH, IDR_MENU_SEARCH },
    { IDR_MENU_WORKSHOP, IDR_MENU_WORKSHOP },
}};
}

HBITMAP CreateBuiltinIconBitmap(BuiltinIcon icon, bool lightTheme, int pixelSize)
{
    const auto index = static_cast<size_t>(icon);
    if (index == 0 || index >= kResources.size() ||
        pixelSize < 1 || pixelSize > 512)
        return nullptr;
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module,
        MAKEINTRESOURCEW(kResources[index][lightTheme ? 0 : 1]), RT_RCDATA);
    if (!resource) return nullptr;
    HGLOBAL loaded = LoadResource(module, resource);
    auto* bytes = loaded ? static_cast<BYTE*>(LockResource(loaded)) : nullptr;
    const DWORD size = SizeofResource(module, resource);
    if (!bytes || !size) return nullptr;

    // Also works for callers whose thread already uses the MTA.
    ComScope com;
    if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE) return nullptr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(bytes, size)) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
            WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), pixelSize, pixelSize,
            WICBitmapInterpolationModeFant)) ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return nullptr;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = pixelSize;
    info.bmiHeader.biHeight = -pixelSize;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info,
        DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap) return nullptr;
    if (!pixels || FAILED(converter->CopyPixels(nullptr, pixelSize * 4,
            pixelSize * pixelSize * 4, static_cast<BYTE*>(pixels))))
    {
        DeleteObject(bitmap);
        return nullptr;
    }
    return bitmap;
}
} // namespace snowdesktop::menu_icon

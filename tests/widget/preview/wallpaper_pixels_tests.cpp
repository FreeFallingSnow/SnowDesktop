#include "widget/preview/widget_wallpaper_pixels.h"

#include <cstdlib>
#include <iostream>
#include <new>
#include <utility>

namespace
{
thread_local bool denyLargeAllocation = false;
thread_local unsigned largeAllocationAttempts = 0;
}

// Exercise the real vector allocation boundary without making the regression
// test consume gigabytes when linked with the old decoder behavior.
void* operator new(std::size_t bytes)
{
    if (denyLargeAllocation && bytes >= 1024u * 1024u)
    {
        ++largeAllocationAttempts;
        throw std::bad_alloc();
    }
    if (void* value = std::malloc(bytes ? bytes : 1)) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace
{
class CodecSource final : public IWICBitmapSource
{
public:
    UINT width = 2, height = 1;
    HRESULT sizeResult = S_OK, copyResult = S_OK;
    unsigned copies = 0;
    UINT observedStride = 0, observedBytes = 0;
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** value) override
    {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(IWICBitmapSource)) return E_NOINTERFACE;
        *value = static_cast<IWICBitmapSource*>(this); AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSize(UINT* x, UINT* y) override
    { *x = width; *y = height; return sizeResult; }
    HRESULT STDMETHODCALLTYPE GetPixelFormat(WICPixelFormatGUID* value) override
    { *value = GUID_WICPixelFormat32bppBGRA; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetResolution(double* x, double* y) override
    { *x = 96; *y = 96; return S_OK; }
    HRESULT STDMETHODCALLTYPE CopyPalette(IWICPalette*) override { return WINCODEC_ERR_PALETTEUNAVAILABLE; }
    HRESULT STDMETHODCALLTYPE CopyPixels(const WICRect*, UINT stride, UINT bytes, BYTE* buffer) override
    {
        ++copies; observedStride = stride; observedBytes = bytes;
        if (FAILED(copyResult)) return copyResult;
        if (width != 2 || height != 1 || bytes != 8 || stride != 8) return E_INVALIDARG;
        auto* pixels = reinterpret_cast<std::uint32_t*>(buffer);
        pixels[0] = 0x00010203u;
        pixels[1] = 0x80c86432u;
        return S_OK;
    }
};
}

int RunWallpaperPixelsTests()
{
    namespace preview = snowdesktop::widget_preview;
    int failures = 0;
    const auto check = [&](bool passed, const char* message) {
        if (!passed) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
    };
    CodecSource source;
    const auto normal = preview::DecodeWallpaperPixels(&source);
    check(normal.width == 2 && normal.height == 1 && normal.pixels.size() == 2 &&
        source.observedStride == 8 && source.observedBytes == 8 &&
        normal.pixels[0] == 0xff20272du && normal.pixels[1] == 0xff744630u,
        "codec BGRA dimensions, stride and straight-alpha backdrop composition survive decoding");
    source.copyResult = E_FAIL;
    check(preview::DecodeWallpaperPixels(&source).pixels.empty(), "failed codec copies publish no partial wallpaper");
    source.sizeResult = E_FAIL;
    check(preview::DecodeWallpaperPixels(&source).pixels.empty(), "failed codec metadata publishes no wallpaper");
    source.sizeResult = S_OK;
    source.copyResult = S_OK;
    for (const auto dimensions : {std::pair{40000u, 40000u}, std::pair{0xffffffffu, 2u},
            std::pair{8192u, 8193u}, std::pair{0u, 1u}})
    {
        source.width = dimensions.first; source.height = dimensions.second; source.copies = 0;
        largeAllocationAttempts = 0; denyLargeAllocation = true;
        preview::Wallpaper decoded;
        bool escaped = false;
        try { decoded = preview::DecodeWallpaperPixels(&source); }
        catch (const std::bad_alloc&) { escaped = true; }
        denyLargeAllocation = false;
        check(!escaped && decoded.pixels.empty() && !source.copies && !largeAllocationAttempts,
            "oversized or invalid codec dimensions are rejected before allocation and pixel copy");
    }
    source.width = 8192; source.height = 8192;
    largeAllocationAttempts = 0; denyLargeAllocation = true;
    preview::Wallpaper allocationFailure;
    bool escaped = false;
    try { allocationFailure = preview::DecodeWallpaperPixels(&source); }
    catch (const std::bad_alloc&) { escaped = true; }
    denyLargeAllocation = false;
    check(!escaped && allocationFailure.pixels.empty() && largeAllocationAttempts == 1,
        "a valid 8K boundary reaches allocation and an allocator failure becomes the existing fallback");
    return failures;
}

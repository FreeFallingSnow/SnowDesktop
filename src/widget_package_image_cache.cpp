#include "widget_package_image_cache.h"

#include <algorithm>
#include <limits>
#include <wincodec.h>
#include <wrl/client.h>

namespace snowdesktop::widget_runtime
{
using Microsoft::WRL::ComPtr;

WidgetPackageImageCache::WidgetPackageImageCache(
    std::size_t maximumSingleBytes, std::size_t maximumTotalBytes)
    : maximumSingleBytes_(maximumSingleBytes),
      maximumTotalBytes_(maximumTotalBytes)
{
}

const PackageImageSource* WidgetPackageImageCache::Fail(
    const std::wstring& path)
{
    if (!path.empty()) failures_.insert(path);
    return nullptr;
}

const PackageImageSource* WidgetPackageImageCache::Load(
    const std::wstring& path)
{
    if (const auto found = sources_.find(path); found != sources_.end())
        return &found->second;
    if (path.empty() || failures_.contains(path)) return nullptr;

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) || !factory)
        return Fail(path);
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr,
            GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) || !decoder)
        return Fail(path);
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)) || !frame) return Fail(path);
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) || !converter ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeMedianCut)))
        return Fail(path);

    UINT width = 0;
    UINT height = 0;
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 ||
        height == 0 || width > (std::numeric_limits<UINT>::max)() / 4)
        return Fail(path);
    const std::uint64_t stride = static_cast<std::uint64_t>(width) * 4;
    const std::uint64_t decodedBytes = stride * height;
    if (maximumSingleBytes_ == 0 || maximumTotalBytes_ == 0 ||
        decodedBytes == 0 || decodedBytes > maximumSingleBytes_ ||
        decodedBytes > (std::numeric_limits<UINT>::max)() ||
        bytes_ > maximumTotalBytes_ ||
        decodedBytes > maximumTotalBytes_ - bytes_)
        return Fail(path);

    PackageImageSource source;
    source.width = width;
    source.height = height;
    source.stride = static_cast<std::uint32_t>(stride);
    source.pixels.resize(static_cast<std::size_t>(decodedBytes));
    if (FAILED(converter->CopyPixels(nullptr, source.stride,
            static_cast<UINT>(source.pixels.size()), source.pixels.data())))
        return Fail(path);

    bytes_ += source.pixels.size();
    auto [inserted, added] = sources_.emplace(path, std::move(source));
    return added ? &inserted->second : nullptr;
}

const PackageImageSource* WidgetPackageImageCache::Find(
    const std::wstring& path) const noexcept
{
    const auto found = sources_.find(path);
    return found == sources_.end() ? nullptr : &found->second;
}

bool WidgetPackageImageCache::Failed(
    const std::wstring& path) const noexcept
{
    return failures_.contains(path);
}

std::size_t WidgetPackageImageCache::Size() const noexcept
{
    return sources_.size();
}

std::size_t WidgetPackageImageCache::Bytes() const noexcept
{
    return bytes_;
}

void WidgetPackageImageCache::Clear() noexcept
{
    sources_.clear();
    failures_.clear();
    bytes_ = 0;
}
}

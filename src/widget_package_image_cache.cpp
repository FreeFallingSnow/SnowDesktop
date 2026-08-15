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
    const std::string& contentKey)
{
    if (!contentKey.empty()) failures_.insert(contentKey);
    return nullptr;
}

const PackageImageSource* WidgetPackageImageCache::Load(
    const std::string& contentKey, const std::wstring& path)
{
    if (const auto found = sources_.find(contentKey);
        found != sources_.end())
        return &found->second;
    if (contentKey.empty() || path.empty() ||
        failures_.contains(contentKey))
        return nullptr;

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) || !factory)
        return Fail(contentKey);
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr,
            GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) || !decoder)
        return Fail(contentKey);
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)) || !frame)
        return Fail(contentKey);
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) || !converter ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeMedianCut)))
        return Fail(contentKey);

    UINT width = 0;
    UINT height = 0;
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 ||
        height == 0 || width > (std::numeric_limits<UINT>::max)() / 4)
        return Fail(contentKey);
    const std::uint64_t stride = static_cast<std::uint64_t>(width) * 4;
    const std::uint64_t decodedBytes = stride * height;
    if (maximumSingleBytes_ == 0 || maximumTotalBytes_ == 0 ||
        decodedBytes == 0 || decodedBytes > maximumSingleBytes_ ||
        decodedBytes > (std::numeric_limits<UINT>::max)() ||
        bytes_ > maximumTotalBytes_ ||
        decodedBytes > maximumTotalBytes_ - bytes_)
        return Fail(contentKey);

    PackageImageSource source;
    source.width = width;
    source.height = height;
    source.stride = static_cast<std::uint32_t>(stride);
    source.pixels.resize(static_cast<std::size_t>(decodedBytes));
    if (FAILED(converter->CopyPixels(nullptr, source.stride,
            static_cast<UINT>(source.pixels.size()), source.pixels.data())))
        return Fail(contentKey);

    bytes_ += source.pixels.size();
    auto [inserted, added] = sources_.emplace(
        contentKey, std::move(source));
    return added ? &inserted->second : nullptr;
}

const PackageImageSource* WidgetPackageImageCache::Find(
    const std::string& contentKey) const noexcept
{
    const auto found = sources_.find(contentKey);
    return found == sources_.end() ? nullptr : &found->second;
}

bool WidgetPackageImageCache::Failed(
    const std::string& contentKey) const noexcept
{
    return failures_.contains(contentKey);
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

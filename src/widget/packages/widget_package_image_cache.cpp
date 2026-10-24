#include "widget_package_image_cache.h"

#include "common/background_work.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <wincodec.h>
#include <wrl/client.h>

namespace snowdesktop::widget_runtime
{
using Microsoft::WRL::ComPtr;

namespace
{
std::optional<PackageImageSource> DecodePackageImage(
    IWICImagingFactory* factory, IWICBitmapDecoder* decoder,
    std::size_t maximumSingleBytes, std::size_t availableBytes,
    PackageImageAcquireError& error)
{
    error = PackageImageAcquireError::DecodeFailed;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)) || !frame)
        return std::nullopt;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) || !converter ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeMedianCut)))
        return std::nullopt;

    UINT width = 0;
    UINT height = 0;
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 ||
        height == 0 || width > (std::numeric_limits<UINT>::max)() / 4)
        return std::nullopt;
    const std::uint64_t stride = static_cast<std::uint64_t>(width) * 4;
    const std::uint64_t decodedBytes = stride * height;
    if (maximumSingleBytes == 0 || decodedBytes == 0 ||
        decodedBytes > maximumSingleBytes || decodedBytes > availableBytes ||
        decodedBytes > (std::numeric_limits<UINT>::max)())
    {
        error = PackageImageAcquireError::QuotaExceeded;
        return std::nullopt;
    }

    PackageImageSource source;
    source.width = width;
    source.height = height;
    source.stride = static_cast<std::uint32_t>(stride);
    source.pixels.resize(static_cast<std::size_t>(decodedBytes));
    if (FAILED(converter->CopyPixels(nullptr, source.stride,
            static_cast<UINT>(source.pixels.size()), source.pixels.data())))
        return std::nullopt;
    error = PackageImageAcquireError::None;
    return source;
}

std::vector<std::uint8_t> ReadPrewarmBytes(
    const std::wstring& path, std::size_t& remaining)
{
    constexpr std::size_t MaximumEncodedBytes = 16ull * 1024ull * 1024ull;
    // A hint must not prevent package edits/replacement. Hash and decode the
    // same private bytes so concurrent file changes cannot label stale pixels.
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    const std::unique_ptr<void, decltype(&CloseHandle)> fileOwner(file, CloseHandle);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        static_cast<std::uint64_t>(size.QuadPart) >
            std::min(MaximumEncodedBytes, remaining))
        return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
            &read, nullptr) || read != bytes.size()) return {};
    remaining -= bytes.size();
    return bytes;
}
}

WidgetPackageImageCache::WidgetPackageImageCache() = default;

WidgetPackageImageCache::WidgetPackageImageCache(
    std::size_t maximumSingleBytes, std::size_t maximumTotalBytes)
    : maximumSingleBytes_(maximumSingleBytes),
      maximumTotalBytes_(maximumTotalBytes)
{
}

WidgetPackageImageCache::~WidgetPackageImageCache()
{
    CancelPrewarm();
}

void WidgetPackageImageCache::CancelPrewarm() noexcept
{
    if (prewarmCancelled_) prewarmCancelled_->store(true);
    prewarmWork_.reset();
    prewarmCancelled_.reset();
    prewarmPending_ = false;
    prepared_.clear();
    preparedBytes_ = 0;
}

void WidgetPackageImageCache::Prewarm(std::vector<std::wstring> files,
    std::vector<std::wstring> images, ContentDigest digest) try
{
    CancelPrewarm();
    constexpr std::size_t MaximumFiles = 128;
    if (files.size() > MaximumFiles) files.resize(MaximumFiles);
    if (images.size() > MaximumFiles - files.size())
        images.resize(MaximumFiles - files.size());
    if (files.empty() && images.empty()) return;
    const std::size_t singleLimit = std::min(maximumSingleBytes_,
        std::size_t{4ull * 1024ull * 1024ull});
    const std::size_t preparedLimit = std::min(maximumTotalBytes_,
        std::size_t{16ull * 1024ull * 1024ull});
    prewarmCancelled_ = std::make_shared<std::atomic_bool>(false);
    prewarmWork_ = std::make_unique<snowdesktop::BackgroundWork>(1, 1);
    prewarmPending_ = true;
    const bool submitted = prewarmWork_->Submit(L"package-cold-start",
        [files = std::move(files), images = std::move(images),
            digest = std::move(digest), cancelled = prewarmCancelled_,
            singleLimit, preparedLimit] {
            std::unordered_map<std::string, PackageImageSource> prepared;
            std::size_t remaining = 32ull * 1024ull * 1024ull;
            for (const auto& path : files)
            {
                if (cancelled->load() || remaining == 0) break;
                (void)ReadPrewarmBytes(path, remaining);
            }
            std::size_t decodedBytes = 0;
            for (const auto& path : images)
            {
                if (cancelled->load() || remaining == 0 ||
                    decodedBytes >= preparedLimit || !digest) break;
                auto encoded = ReadPrewarmBytes(path, remaining);
                if (encoded.empty()) continue;
                const auto key = digest(encoded);
                if (key.empty() || prepared.contains(key)) continue;
                ComPtr<IWICImagingFactory> factory;
                ComPtr<IWICStream> stream;
                ComPtr<IWICBitmapDecoder> decoder;
                if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
                    !factory || FAILED(factory->CreateStream(&stream)) ||
                    !stream || FAILED(stream->InitializeFromMemory(
                        encoded.data(), static_cast<DWORD>(encoded.size()))) ||
                    FAILED(factory->CreateDecoderFromStream(stream.Get(),
                        nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
                    !decoder) continue;
                PackageImageAcquireError error;
                auto source = DecodePackageImage(factory.Get(), decoder.Get(),
                    singleLimit, preparedLimit - decodedBytes, error);
                if (!source) continue;
                decodedBytes += source->pixels.size();
                prepared.emplace(key, std::move(*source));
            }
            return prepared;
        },
        [this](auto prepared) {
            prepared_ = std::move(prepared);
            preparedBytes_ = 0;
            for (const auto& [key, source] : prepared_)
            {
                (void)key;
                preparedBytes_ += source.pixels.size();
            }
            prewarmPending_ = false;
        }, nullptr, 0);
    if (!submitted) CancelPrewarm();
}
catch (...)
{
    CancelPrewarm();
}

bool WidgetPackageImageCache::PrewarmPending()
{
    if (prewarmWork_)
        prewarmWork_->Drain(std::chrono::milliseconds(0));
    if (!prewarmPending_)
    {
        prewarmWork_.reset();
        prewarmCancelled_.reset();
    }
    return prewarmPending_;
}

std::size_t WidgetPackageImageCache::PreparedBytes() const noexcept
{
    return preparedBytes_;
}

const PackageImageSource* WidgetPackageImageCache::Fail(
    const std::string& contentKey, PackageImageAcquireError* error)
{
    if (!contentKey.empty() && !failures_.contains(contentKey))
    {
        failureOrder_.push_back(contentKey);
        try { failures_.insert(contentKey); }
        catch (...)
        {
            failureOrder_.pop_back();
            throw;
        }
        if (failureOrder_.size() > MaximumFailureEntries)
        {
            failures_.erase(failureOrder_.front());
            failureOrder_.pop_front();
        }
    }
    if (error) *error = PackageImageAcquireError::DecodeFailed;
    return nullptr;
}

const PackageImageSource* WidgetPackageImageCache::Acquire(
    const std::string& contentKey, const std::wstring& path,
    PackageImageAcquireError* error)
{
    if (error) *error = PackageImageAcquireError::None;
    if (const auto found = sources_.find(contentKey);
        found != sources_.end())
    {
        ++found->second.references;
        return &found->second.source;
    }
    if (contentKey.empty() || path.empty())
    {
        if (error) *error = PackageImageAcquireError::InvalidInput;
        return nullptr;
    }
    if (failures_.contains(contentKey))
    {
        if (error) *error = PackageImageAcquireError::DecodeFailed;
        return nullptr;
    }

    (void)PrewarmPending();
    const std::size_t availableBytes = bytes_ > maximumTotalBytes_
        ? 0 : maximumTotalBytes_ - bytes_;
    std::optional<PackageImageSource> source;
    if (const auto warmed = prepared_.find(contentKey); warmed != prepared_.end())
    {
        const auto size = warmed->second.pixels.size();
        if (size > maximumSingleBytes_ || size > availableBytes)
        {
            if (error) *error = PackageImageAcquireError::QuotaExceeded;
            return nullptr;
        }
        source = std::move(warmed->second);
        preparedBytes_ -= size;
        prepared_.erase(warmed);
    }
    else
    {
        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) || !factory)
            return Fail(contentKey, error);
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr,
                GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) || !decoder)
            return Fail(contentKey, error);
        PackageImageAcquireError decodeError;
        source = DecodePackageImage(factory.Get(), decoder.Get(),
            maximumSingleBytes_, availableBytes, decodeError);
        if (!source)
        {
            if (decodeError == PackageImageAcquireError::DecodeFailed)
                return Fail(contentKey, error);
            if (error) *error = decodeError;
            return nullptr;
        }
    }

    bytes_ += source->pixels.size();
    auto [inserted, added] = sources_.emplace(
        contentKey, Entry{ std::move(*source), 1 });
    if (added) return &inserted->second.source;
    if (error) *error = PackageImageAcquireError::DecodeFailed;
    return nullptr;
}

bool WidgetPackageImageCache::Release(
    const std::string& contentKey) noexcept
{
    const auto found = sources_.find(contentKey);
    if (found == sources_.end() || found->second.references == 0)
        return false;
    --found->second.references;
    if (found->second.references != 0) return false;
    const std::size_t released = found->second.source.pixels.size();
    bytes_ = released > bytes_ ? 0 : bytes_ - released;
    sources_.erase(found);
    return true;
}

const PackageImageSource* WidgetPackageImageCache::Find(
    const std::string& contentKey) const noexcept
{
    const auto found = sources_.find(contentKey);
    return found == sources_.end() ? nullptr : &found->second.source;
}

bool WidgetPackageImageCache::Failed(
    const std::string& contentKey) const noexcept
{
    return failures_.contains(contentKey);
}

std::size_t WidgetPackageImageCache::ReferenceCount(
    const std::string& contentKey) const noexcept
{
    const auto found = sources_.find(contentKey);
    return found == sources_.end() ? 0 : found->second.references;
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
    CancelPrewarm();
    sources_.clear();
    failures_.clear();
    failureOrder_.clear();
    bytes_ = 0;
}
}

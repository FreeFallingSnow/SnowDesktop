#include "widget_package_image_cache.h"

#include "common/background_work.h"
#include <algorithm>
#include <cmath>
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
    PackageImageAcquireError& error, std::uint32_t edge = 0)
{
    error = PackageImageAcquireError::DecodeFailed;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)) || !frame)
        return std::nullopt;
    UINT width = 0, height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || !width || !height)
        return std::nullopt;
    // Scale premultiplied pixels so transparent colored edges do not bleed.
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) || !converter ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeMedianCut)))
        return std::nullopt;
    ComPtr<IWICBitmapSource> input = converter;
    ComPtr<IWICBitmapScaler> scaler;
    const auto longest = std::max(width, height);
    if (edge && edge < longest)
    {
        width = static_cast<UINT>(std::max<std::uint64_t>(1,
            (std::uint64_t{width} * edge + longest / 2) / longest));
        height = static_cast<UINT>(std::max<std::uint64_t>(1,
            (std::uint64_t{height} * edge + longest / 2) / longest));
        if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(converter.Get(), width, height,
                WICBitmapInterpolationModeFant)))
            return std::nullopt;
        input = scaler;
    }
    if (FAILED(input->GetSize(&width, &height)) || width == 0 ||
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
    if (FAILED(input->CopyPixels(nullptr, source.stride,
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

struct SnapshotDecoder
{
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
};

std::optional<SnapshotDecoder> OpenSnapshot(
    const std::vector<std::uint8_t>& encoded)
{
    if (encoded.empty() || encoded.size() > MAXDWORD) return std::nullopt;
    SnapshotDecoder value;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&value.factory))) ||
        FAILED(value.factory->CreateStream(&value.stream)) ||
        FAILED(value.stream->InitializeFromMemory(
            const_cast<BYTE*>(encoded.data()),
            static_cast<DWORD>(encoded.size()))) ||
        FAILED(value.factory->CreateDecoderFromStream(value.stream.Get(),
            nullptr, WICDecodeMetadataCacheOnLoad, &value.decoder)))
        return std::nullopt;
    return value;
}

PackageImageSize ScaledSize(PackageImageSize original, std::uint32_t edge)
{
    const auto longest = std::max(original.width, original.height);
    if (!edge || edge >= longest) return original;
    return {
        static_cast<std::uint32_t>(std::max<std::uint64_t>(1,
            (std::uint64_t{original.width} * edge + longest / 2) / longest)),
        static_cast<std::uint32_t>(std::max<std::uint64_t>(1,
            (std::uint64_t{original.height} * edge + longest / 2) / longest)) };
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
    Clear();
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
            std::unordered_map<std::string, PreparedImage> prepared;
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
                prepared.emplace(key, PreparedImage{std::move(*source),
                    std::make_shared<const std::vector<std::uint8_t>>(
                        std::move(encoded))});
            }
            return prepared;
        },
        [this](auto prepared) {
            prepared_ = std::move(prepared);
            preparedBytes_ = 0;
            for (const auto& [key, source] : prepared_)
            {
                (void)key;
                preparedBytes_ += source.source.pixels.size();
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
    PackageImageAcquireError* error, bool lazy, ContentDigest digest)
{
    if (error) *error = PackageImageAcquireError::None;
    if (const auto found = sources_.find(contentKey);
        found != sources_.end())
    {
        if (!lazy && !Validate(contentKey, error)) return nullptr;
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
    const std::size_t availableBytes = reservedBytes_ > maximumTotalBytes_
        ? 0 : maximumTotalBytes_ - reservedBytes_;
    std::optional<PackageImageSource> source;
    std::shared_ptr<const std::vector<std::uint8_t>> encoded;
    if (const auto warmed = prepared_.find(contentKey); warmed != prepared_.end())
    {
        const auto size = warmed->second.source.pixels.size();
        if (size > maximumSingleBytes_ || size > availableBytes)
        {
            if (error) *error = PackageImageAcquireError::QuotaExceeded;
            return nullptr;
        }
        source = std::move(warmed->second.source);
        encoded = std::move(warmed->second.encoded);
        preparedBytes_ -= size;
        prepared_.erase(warmed);
    }
    else
    {
        std::size_t remaining = 16ull * 1024ull * 1024ull;
        encoded = std::make_shared<const std::vector<std::uint8_t>>(
            ReadPrewarmBytes(path, remaining));
        auto opened = OpenSnapshot(*encoded);
        if (!opened)
            return Fail(contentKey, error);
        ComPtr<IWICBitmapFrameDecode> frame;
        UINT width = 0, height = 0;
        if (FAILED(opened->decoder->GetFrame(0, &frame)) ||
            FAILED(frame->GetSize(&width, &height)) || !width || !height ||
            width > MAXUINT / 4)
            return Fail(contentKey, error);
        const auto fullBytes = std::uint64_t{width} * height * 4;
        if (!maximumSingleBytes_ || fullBytes > maximumSingleBytes_ ||
            fullBytes > availableBytes || fullBytes > MAXUINT)
        {
            if (error) *error = PackageImageAcquireError::QuotaExceeded;
            return nullptr;
        }
        if (lazy)
            source = PackageImageSource{{}, width, height, width * 4};
        else
        {
            PackageImageAcquireError decodeError;
            source = DecodePackageImage(opened->factory.Get(),
                opened->decoder.Get(), maximumSingleBytes_,
                availableBytes, decodeError);
            if (!source)
            {
                if (decodeError == PackageImageAcquireError::DecodeFailed)
                    return Fail(contentKey, error);
                if (error) *error = decodeError;
                return nullptr;
            }
        }
    }

    if (digest && digest(*encoded) != contentKey)
    {
        if (error) *error = PackageImageAcquireError::InvalidInput;
        return nullptr;
    }
    constexpr std::size_t maximumEncodedBytes = 128ull * 1024ull * 1024ull;
    if (encoded->size() > maximumEncodedBytes - encodedBytes_ ||
        source->pixels.size() > maximumTotalBytes_ - bytes_ -
            std::min(pendingBytes_, maximumTotalBytes_ - bytes_))
    {
        if (error) *error = PackageImageAcquireError::QuotaExceeded;
        return nullptr;
    }
    Entry entry;
    entry.original = {source->width, source->height};
    entry.reservedBytes = std::size_t{source->width} * source->height * 4;
    entry.references = 1;
    entry.identity = ++nextIdentity_;
    entry.encoded = std::move(encoded);
    entry.ready = !lazy;
    entry.fullyValidated = !lazy;
    if (lazy) std::vector<std::uint8_t>{}.swap(source->pixels);
    entry.revision = source->pixels.empty() ? 0 : 1;
    entry.source = std::move(*source);
    auto [inserted, added] = sources_.emplace(
        contentKey, std::move(entry));
    if (added)
    {
        bytes_ += inserted->second.source.pixels.size();
        reservedBytes_ += inserted->second.reservedBytes;
        encodedBytes_ += inserted->second.encoded->size();
        return &inserted->second.source;
    }
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
    reservedBytes_ -= found->second.reservedBytes;
    encodedBytes_ -= found->second.encoded->size();
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

std::size_t WidgetPackageImageCache::EncodedBytes() const noexcept
{
    return encodedBytes_;
}

PackageImageSize WidgetPackageImageCache::OriginalSize(
    const std::string& key) const noexcept
{
    const auto found = sources_.find(key);
    return found == sources_.end() ? PackageImageSize{} : found->second.original;
}

std::uint64_t WidgetPackageImageCache::Revision(
    const std::string& key) const noexcept
{
    const auto found = sources_.find(key);
    return found == sources_.end() ? 0 : found->second.revision;
}

bool WidgetPackageImageCache::Ready(const std::string& key) const noexcept
{
    const auto found = sources_.find(key);
    return found != sources_.end() && found->second.ready;
}

bool WidgetPackageImageCache::DecodeFailed(const std::string& key) const noexcept
{
    const auto found = sources_.find(key);
    return found != sources_.end() && found->second.decodeFailed;
}

bool WidgetPackageImageCache::Validate(const std::string& key,
    PackageImageAcquireError* error)
{
    if (error) *error = PackageImageAcquireError::None;
    const auto found = sources_.find(key);
    if (found == sources_.end()) return false;
    auto& entry = found->second;
    if (entry.fullyValidated && !entry.source.pixels.empty()) return true;
    auto opened = OpenSnapshot(*entry.encoded);
    PackageImageAcquireError decodeError = PackageImageAcquireError::DecodeFailed;
    auto decoded = opened ? DecodePackageImage(opened->factory.Get(),
        opened->decoder.Get(), maximumSingleBytes_,
        maximumTotalBytes_ - (bytes_ - entry.source.pixels.size()),
        decodeError) : std::nullopt;
    if (!decoded)
    {
        if (error) *error = decodeError;
        return false;
    }
    bytes_ -= entry.source.pixels.size();
    bytes_ += decoded->pixels.size();
    entry.source = std::move(*decoded);
    entry.ready = true;
    entry.fullyValidated = true;
    entry.decodeFailed = false;
    ++entry.revision;
    changed_.push_back(key);
    return true;
}

std::uint32_t WidgetPackageImageCache::ResolutionBucket(
    PackageImageSize original, double rasterScale) noexcept
{
    const auto longest = std::max(original.width, original.height);
    if (!longest || !std::isfinite(rasterScale) || rasterScale <= 0) return 0;
    const double requested = std::ceil(longest * std::min(1.0, rasterScale));
    for (const std::uint32_t bucket : {256u, 512u, 768u, 1024u, 1536u, 2048u, 4096u})
        if (requested <= bucket) return std::min(longest, bucket);
    return longest;
}

void WidgetPackageImageCache::BeginFrame(const std::string& owner)
{
    for (auto& [key, entry] : sources_)
    {
        (void)key;
        entry.demands.erase(owner);
    }
}

void WidgetPackageImageCache::ForgetOwner(const std::string& prefix)
{
    for (auto& [key, entry] : sources_)
    {
        (void)key;
        std::erase_if(entry.demands, [&](const auto& value) {
            return value.first.starts_with(prefix);
        });
    }
}

void WidgetPackageImageCache::UpdateDemand(Entry& entry, Clock::time_point now)
{
    std::uint32_t wanted = 0;
    for (const auto& [owner, edge] : entry.demands)
    {
        (void)owner;
        wanted = std::max(wanted, edge);
    }
    if (wanted != entry.wantedEdge)
    {
        entry.wantedEdge = wanted;
        entry.changedAt = now;
    }
}

const PackageImageSource* WidgetPackageImageCache::Request(
    const std::string& key, const std::string& owner, double rasterScale,
    Clock::time_point now, bool synchronous)
{
    const auto found = sources_.find(key);
    if (found == sources_.end() || owner.empty()) return nullptr;
    auto& entry = found->second;
    const auto edge = ResolutionBucket(entry.original, rasterScale);
    if (edge)
    {
        auto& demand = entry.demands[owner];
        demand = std::max(demand, edge);
        UpdateDemand(entry, now);
        Schedule(key, entry, now, synchronous);
    }
    return entry.source.pixels.empty() ? nullptr : &entry.source;
}

void WidgetPackageImageCache::Schedule(const std::string& key, Entry& entry,
    Clock::time_point now, bool synchronous)
{
    if (entry.pending || entry.decodeFailed) return;
    constexpr auto delay = std::chrono::seconds(2);
    if (!entry.wantedEdge)
    {
        if (!entry.source.pixels.empty() && now - entry.changedAt >= delay)
        {
            bytes_ -= entry.source.pixels.size();
            std::vector<std::uint8_t>{}.swap(entry.source.pixels);
            ++entry.revision;
            changed_.push_back(key);
        }
        return;
    }
    const auto edge = entry.wantedEdge;
    const auto currentEdge = entry.source.pixels.empty() ? 0 :
        std::max(entry.source.width, entry.source.height);
    if (currentEdge == edge ||
        (currentEdge > edge && now - entry.changedAt < delay))
        return;
    const auto size = ScaledSize(entry.original, edge);
    const std::size_t expected = std::size_t{size.width} * size.height * 4;
    // Include the old live pixels and all queued replacement payloads. A full
    // quota defers a replacement rather than dropping a still usable image.
    // One replacement at a time bounds the temporary old/new overlap to one
    // image, and lets a full cache shrink without first discarding its pixels.
    if (pendingBytes_ || expected > maximumTotalBytes_ -
            (bytes_ - entry.source.pixels.size()))
        return;
    const auto encoded = entry.encoded;
    const auto identity = entry.identity;
    const auto maximum = maximumSingleBytes_;
    auto work = [encoded, edge, maximum, expected] {
        auto opened = OpenSnapshot(*encoded);
        PackageImageAcquireError error;
        return opened ? DecodePackageImage(opened->factory.Get(),
            opened->decoder.Get(), maximum, expected, error, edge) :
            std::optional<PackageImageSource>{};
    };
    auto apply = [this, key, identity, edge, expected](auto decoded) {
        pendingBytes_ -= std::min(pendingBytes_, expected);
        const auto found = sources_.find(key);
        if (found == sources_.end() || found->second.identity != identity) return;
        auto& current = found->second;
        current.pending = false;
        // A resized, hidden, released or replaced consumer cannot publish an
        // obsolete resolution after the worker finishes.
        if (current.wantedEdge != edge) return;
        if (!decoded)
        {
            current.decodeFailed = true;
            changed_.push_back(key);
            return;
        }
        bytes_ -= current.source.pixels.size();
        bytes_ += decoded->pixels.size();
        current.source = std::move(*decoded);
        current.ready = true;
        if (edge == std::max(current.original.width, current.original.height))
            current.fullyValidated = true;
        ++current.revision;
        changed_.push_back(key);
    };
    if (synchronous)
    {
        pendingBytes_ += expected;
        apply(work());
        return;
    }
    if (!decodeWork_)
    {
        decodeWork_ = std::make_unique<snowdesktop::BackgroundWork>(1, 64);
        decodeWork_->SetReadyCallback(onReady_);
    }
    const std::wstring job = std::wstring(key.begin(), key.end()) +
        L":" + std::to_wstring(identity);
    if (decodeWork_->Submit(job, std::move(work), std::move(apply), nullptr, 0))
    {
        pendingBytes_ += expected;
        entry.pending = true;
    }
}

void WidgetPackageImageCache::Pump(Clock::time_point now)
{
    // Resolve the current consumer set before accepting completed work.
    for (auto& [key, entry] : sources_)
    {
        (void)key;
        UpdateDemand(entry, now);
    }
    if (decodeWork_) decodeWork_->Drain(std::chrono::milliseconds(1));
    for (auto& [key, entry] : sources_)
        Schedule(key, entry, now, false);
}

void WidgetPackageImageCache::SetReadyCallback(std::function<void()> callback)
{
    onReady_ = std::move(callback);
    if (decodeWork_) decodeWork_->SetReadyCallback(onReady_);
}

std::vector<std::string> WidgetPackageImageCache::TakeChanged()
{
    std::sort(changed_.begin(), changed_.end());
    changed_.erase(std::unique(changed_.begin(), changed_.end()), changed_.end());
    return std::exchange(changed_, {});
}

void WidgetPackageImageCache::Clear() noexcept
{
    CancelPrewarm();
    decodeWork_.reset();
    sources_.clear();
    failures_.clear();
    failureOrder_.clear();
    bytes_ = 0;
    reservedBytes_ = 0;
    encodedBytes_ = 0;
    pendingBytes_ = 0;
    changed_.clear();
}
}

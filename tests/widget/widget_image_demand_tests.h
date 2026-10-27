#pragma once

#include <chrono>
#include <memory>
#include <vector>

namespace
{
void WriteDemandBitmap(const fs::path& path, int width, int height,
    std::uint32_t color)
{
    const std::uint32_t bytes = static_cast<std::uint32_t>(width * height * 4);
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + bytes;
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = height;
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = bytes;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height, color);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
    stream.write(reinterpret_cast<const char*>(&info), sizeof(info));
    stream.write(reinterpret_cast<const char*>(pixels.data()), bytes);
    Check(stream.good(), "resolution fixture must be written");
}

template<class Predicate>
void AwaitImage(WidgetPackageImageCache& cache,
    WidgetPackageImageCache::Clock::time_point now, Predicate predicate)
{
    const auto deadline = GetTickCount64() + 10000;
    while (!predicate() && GetTickCount64() < deadline)
    {
        cache.Pump(now);
        Sleep(1);
    }
    Check(predicate(), "bounded background decode must reach its expected state");
}

void RunImageDemandCases(const fs::path& root)
{
    using Cache = WidgetPackageImageCache;
    using namespace std::chrono_literals;
    const auto now = Cache::Clock::now();
    const auto file = root / L"demand.bmp";
    WriteDemandBitmap(file, 1024, 512, 0xff336699u);
    Cache cache;
    const auto* registered = cache.Acquire("shared", file.wstring(), nullptr, true);
    Check(registered && registered->pixels.empty() && cache.Bytes() == 0 &&
        !cache.Ready("shared") && cache.EncodedBytes() > 0,
        "lazy registration retains a snapshot without expanding its pixels");
    Check(cache.Acquire("shared", L"unused-path", nullptr, true) == registered &&
        cache.ReferenceCount("shared") == 2,
        "two instances share the same immutable encoded resource");
    fs::remove(file);
    cache.BeginFrame("a|desktop");
    Check(!cache.Request("shared", "a|desktop", 0.4, now),
        "a first live request queues decode without synchronously returning pixels");
    AwaitImage(cache, now, [&] { return cache.Ready("shared"); });
    Check(cache.Find("shared")->width == 512 && cache.Find("shared")->height == 256 &&
        cache.Bytes() == 512u * 256u * 4u && cache.OriginalSize("shared").width == 1024,
        "the requested bucket preserves aspect ratio and original intrinsic size");
    Check(cache.Find("shared")->pixels[0] == 0x99 &&
        cache.Find("shared")->pixels[1] == 0x66,
        "background decode survives deletion of the original file and keeps its pixels");

    cache.BeginFrame("b|panel");
    Check(cache.Request("shared", "b|panel", 0.9, now)->width == 512,
        "an upgrade keeps the previous usable pixels while preparing the larger bucket");
    AwaitImage(cache, now, [&] { return cache.Find("shared")->width == 1024; });
    const auto fullRevision = cache.Revision("shared");
    Check(cache.Validate("shared") && cache.Revision("shared") == fullRevision,
        "a full-resolution lazy decode already satisfies strict validation without decoding twice");
    cache.BeginFrame("a|desktop");
    cache.Request("shared", "a|desktop", 0.2, now);
    cache.Pump(now + 3s);
    Check(cache.Find("shared")->width == 1024,
        "a small instance cannot shrink a resource still needed by a larger panel");
    cache.ForgetOwner("b|panel");
    cache.Pump(now + 3s);
    cache.Pump(now + 4s);
    Check(cache.Find("shared")->width == 1024,
        "closing the largest consumer waits for the downgrade stability interval");
    AwaitImage(cache, now + 6s, [&] { return cache.Find("shared")->width == 256; });
    const auto revision = cache.Revision("shared");
    cache.BeginFrame("a|desktop");
    cache.Request("shared", "a|desktop", 0.24, now + 6s);
    cache.Pump(now + 9s);
    Check(cache.Revision("shared") == revision,
        "resizing inside one resolution bucket does not decode or rebuild it");

    cache.ForgetOwner("a|");
    cache.Pump(now + 9s);
    cache.Pump(now + 12s);
    Check(cache.Bytes() == 0 && cache.Ready("shared") && cache.EncodedBytes() > 0,
        "hidden surfaces release pixels after the grace period while retaining resource readiness");
    cache.BeginFrame("a|desktop");
    cache.Request("shared", "a|desktop", 0.5, now + 12s);
    AwaitImage(cache, now + 12s, [&] { return cache.Bytes() != 0; });
    Check(!cache.Release("shared") && cache.Release("shared") &&
        cache.Bytes() == 0 && cache.EncodedBytes() == 0,
        "the last handle releases both expanded pixels and the compressed snapshot");

    WriteDemandBitmap(file, 1024, 512, 0xffaabbccu);
    Cache strictAfterLazy(1024u * 512u * 4u, 1024u * 512u * 4u);
    Check(strictAfterLazy.Acquire("strict-after-lazy", file.wstring(), nullptr, true) != nullptr,
        "register the mixed lazy/strict resource");
    strictAfterLazy.BeginFrame("lazy|desktop");
    const auto* partial = strictAfterLazy.Request("strict-after-lazy", "lazy|desktop", 0.2, now, true);
    Check(partial && partial->width == 256 &&
        strictAfterLazy.Ready("strict-after-lazy"), "a scaled lazy resource can already be ready");
    Check(strictAfterLazy.Acquire("strict-after-lazy", file.wstring()) &&
        strictAfterLazy.Find("strict-after-lazy")->width == 1024 &&
        strictAfterLazy.Bytes() == 1024u * 512u * 4u,
        "a strict caller validates full resolution after lazy scaling without double-counting quota");
    Cache full(1024u * 512u * 4u, 1024u * 512u * 4u);
    Check(full.Acquire("full", file.wstring()) != nullptr,
        "the fixture fills its entire decoded quota");
    full.BeginFrame("full|desktop");
    full.Request("full", "full|desktop", 0.2, now);
    AwaitImage(full, now + 3s, [&] { return full.Find("full")->width == 256; });
    Check(full.Bytes() == 256u * 128u * 4u,
        "a full quota can shrink without discarding the previous image before replacement");

    Cache stale;
    Check(stale.Acquire("stale", file.wstring(), nullptr, true) != nullptr,
        "create the controlled stale-result resource");
    struct ReadyEvent
    {
        HANDLE value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~ReadyEvent() { if (value) CloseHandle(value); }
    };
    const auto ready = std::make_shared<ReadyEvent>();
    Check(ready->value != nullptr, "create a private decode completion gate");
    stale.SetReadyCallback([ready] { SetEvent(ready->value); });
    stale.BeginFrame("obsolete|desktop");
    stale.Request("stale", "obsolete|desktop", 0.2, now);
    Check(WaitForSingleObject(ready->value, 10000) == WAIT_OBJECT_0,
        "the worker publishes before waking its owner");
    stale.ForgetOwner("obsolete|");
    stale.Pump(now);
    Check(stale.Bytes() == 0 && !stale.Ready("stale"),
        "a hidden consumer cannot publish an obsolete queued resolution");
    stale.BeginFrame("current|desktop");
    ResetEvent(ready->value);
    stale.Request("stale", "current|desktop", 0.5, now);
    Check(WaitForSingleObject(ready->value, 10000) == WAIT_OBJECT_0,
        "the next completion is queued before clearing its generation");
    stale.Clear();
    stale.Pump(now);
    Check(stale.Size() == 0 && stale.Bytes() == 0 && stale.EncodedBytes() == 0,
        "clearing in-flight work cannot repopulate the next cache generation");

    WriteBitmap(file, 0xff112233u);
    Cache quota(4, 4);
    PackageImageAcquireError error;
    Check(quota.Acquire("one", file.wstring(), &error, true) &&
        !quota.Acquire("two", file.wstring(), &error, true) &&
        error == PackageImageAcquireError::QuotaExceeded && quota.Bytes() == 0,
        "lazy handles preserve the original per-resource and total pixel reservation quotas");
    Check(Cache::ResolutionBucket({1024,512}, 0.3) == 512 &&
        Cache::ResolutionBucket({1024,512}, 0.6) == 768 &&
        Cache::ResolutionBucket({1024,512}, 2.0) == 1024 &&
        Cache::ResolutionBucket({128,64}, 0.1) == 128,
        "resolution buckets round up and never upscale beyond the original source");

    Cache validation;
    Check(validation.Acquire("validation", file.wstring(), nullptr, true) &&
        !validation.Ready("validation"), "a lazy handle begins without decoded pixels");
    fs::remove(file);
    Check(validation.Acquire("validation", file.wstring(), &error) &&
        validation.Ready("validation") && validation.Bytes() == 4 &&
        validation.ReferenceCount("validation") == 2,
        "a later strict handle synchronously validates the same immutable snapshot");

    WriteBitmap(file, 0xff123456u);
    Cache identity;
    Check(!identity.Acquire("mismatched", file.wstring(), &error, true,
            PrewarmTestDigest) && error == PackageImageAcquireError::InvalidInput &&
        identity.Size() == 0 && identity.EncodedBytes() == 0,
        "a file changed between digest and snapshot acquisition cannot be labelled with an old content key");
}
}

#pragma once

#include <chrono>
#include <future>
#include <iterator>

namespace
{
std::string PrewarmTestDigest(std::span<const std::uint8_t> encoded)
{
    // The cache treats keys as opaque. Retaining all bytes makes this fixture's
    // identity exact, independently of production's SHA256 implementation.
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

std::string PrewarmTestFileKey(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

void AwaitPrewarm(WidgetPackageImageCache& cache)
{
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(10);
    while (cache.PrewarmPending() && std::chrono::steady_clock::now() < deadline)
        Sleep(1);
    Check(!cache.PrewarmPending(), "CPU preparation must complete within the fixture deadline");
}

void RunColdStartPrewarmCases(const fs::path& root)
{
    const auto image = root / L"cold-prewarm.bmp";
    const auto other = root / L"cold-other.bmp";
    const auto invalid = root / L"cold-invalid.bmp";
    WriteBitmap(image, 0xff336699u);
    WriteBitmap(other, 0xff112233u);
    { std::ofstream output(invalid, std::ios::binary); output << "not an image"; }
    const auto originalKey = PrewarmTestFileKey(image);
    const auto invalidKey = PrewarmTestFileKey(invalid);
    WidgetPackageImageCache baseline;
    const auto* original = baseline.Acquire(originalKey, image.wstring());
    Check(original != nullptr, "the original synchronous WIC path must decode the fixture");

    WidgetPackageImageCache warmed;
    warmed.Prewarm({invalid.wstring()}, {image.wstring(), invalid.wstring()},
        PrewarmTestDigest);
    AwaitPrewarm(warmed);
    Check(warmed.PreparedBytes() == 4 && warmed.Size() == 0 &&
            warmed.Bytes() == 0 && !warmed.Failed(invalidKey),
        "speculation must not create handles, consume live quota, or cache failures");
    // Deny all further file opens: only an already decoded CPU result can
    // satisfy this cache request. resource.image still hashes its live file
    // before it calls the cache; this fixture tests the internal CPU boundary.
    HANDLE exclusive = CreateFileW(image.c_str(), GENERIC_READ, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(exclusive != INVALID_HANDLE_VALUE, "the prepared image must release all file handles");
    WidgetPackageImageCache uncached;
    Check(!uncached.Acquire(originalKey, image.wstring()),
        "the exclusive handle must block the ordinary filename decoder");
    const auto* prepared = warmed.Acquire(originalKey, image.wstring());
    CloseHandle(exclusive);
    Check(prepared && prepared->width == original->width &&
            prepared->height == original->height && prepared->stride == original->stride &&
            prepared->pixels == original->pixels && warmed.PreparedBytes() == 0 &&
            warmed.Bytes() == 4 && warmed.ReferenceCount(originalKey) == 1,
        "consuming predecoded pixels must match synchronous WIC output and handle accounting");
    PackageImageAcquireError error;
    Check(!warmed.Acquire(invalidKey, invalid.wstring(), &error) &&
            error == PackageImageAcquireError::DecodeFailed && warmed.Failed(invalidKey),
        "failed speculation must retain the original synchronous failure and negative cache");

    WidgetPackageImageCache changed;
    changed.Prewarm({}, {image.wstring()}, PrewarmTestDigest);
    AwaitPrewarm(changed);
    WriteBitmap(image, 0xffaabbccu);
    const auto changedKey = PrewarmTestFileKey(image);
    const auto* changedSource = changed.Acquire(changedKey, image.wstring());
    const auto* changedBaseline = baseline.Acquire(changedKey, image.wstring());
    Check(changedKey != originalKey && changedSource && changedBaseline &&
            changedSource->pixels == changedBaseline->pixels &&
            changedSource->pixels != original->pixels,
        "same-path content changes must bypass stale speculation and decode current pixels");

    WidgetPackageImageCache quota(4, 4);
    quota.Prewarm({}, {image.wstring()}, PrewarmTestDigest);
    AwaitPrewarm(quota);
    Check(quota.PreparedBytes() == 4 && quota.Acquire("live", other.wstring()),
        "speculative pixels must not displace an ordinary live image");
    Check(!quota.Acquire(changedKey, image.wstring(), &error) &&
            error == PackageImageAcquireError::QuotaExceeded &&
            !quota.Failed(changedKey) && quota.PreparedBytes() == 4,
        "a warmed image must still enforce the original live byte quota");
    Check(quota.Release("live") && quota.Acquire(changedKey, image.wstring()) &&
            quota.PreparedBytes() == 0 && quota.Bytes() == 4,
        "a warmed quota rejection must remain retryable after the last live handle releases");

    auto entered = std::make_shared<std::promise<void>>();
    auto enteredFuture = entered->get_future();
    auto release = std::make_shared<std::promise<void>>();
    auto released = release->get_future().share();
    WidgetPackageImageCache pending;
    pending.Prewarm({}, {image.wstring()},
        [entered, released](std::span<const std::uint8_t> bytes) {
            entered->set_value();
            released.wait();
            return PrewarmTestDigest(bytes);
        });
    Check(enteredFuture.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
        "the isolated worker must reach the controlled preparation boundary");
    WriteBitmap(image, 0xff8899aau);
    const auto pendingKey = PrewarmTestFileKey(image);
    const auto* pendingBaseline = baseline.Acquire(pendingKey, image.wstring());
    const auto* fallback = pending.Acquire(pendingKey, image.wstring());
    Check(fallback && pendingBaseline && fallback->pixels == pendingBaseline->pixels &&
            pending.PrewarmPending(),
        "loading must synchronously produce current pixels while preparation remains blocked");
    pending.Clear();
    Check(!pending.PrewarmPending() && pending.Bytes() == 0 && pending.PreparedBytes() == 0,
        "clear must cancel delivery without waiting for a blocked worker");
    release->set_value();
    pending.Prewarm({}, {other.wstring()}, PrewarmTestDigest);
    AwaitPrewarm(pending);
    const auto otherKey = PrewarmTestFileKey(other);
    Check(pending.PreparedBytes() == 4 && pending.Acquire(otherKey, other.wstring()) &&
            pending.PreparedBytes() == 0 && pending.Size() == 1,
        "a cancelled generation must not publish into a newly prepared cache");
    pending.Clear();
}
}

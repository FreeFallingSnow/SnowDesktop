#pragma once
#include "icons/icon_library.h"

namespace native_icon_cold_pixels_cases
{
using namespace native_drag_commit_cases;

// The real QueueIconTask worker, OnIconLoaded acceptance and D2D cache are kept.
// Only the Shell input is a private ICO; rendering uses an offscreen WARP device.
inline void Run(DesktopApp& app, ID2D1DeviceContext* context, const fs::path& root)
{
    using namespace snowdesktop;
    auto style = icon_beautify::MakePreset(IconBeautifyPreset::FrostedGlass);
    style.mode = 1;
    icon_image::Image image;
    image.width = 64; image.height = 64;
    image.pixels.assign(64 * 64, 0);
    for (int y = 12; y < 52; ++y) for (int x = 18; x < 46; ++x)
        image.pixels[static_cast<size_t>(y) * 64 + x] = 0x80402010;
    std::string png, error;
    Require(icon_image::EncodePng(image, png, error), "encode private ICO frame");
    std::string ico;
    const auto le = [&](std::uint32_t value, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i) ico.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
    };
    le(0, 2); le(1, 2); le(1, 2);
    le(64, 1); le(64, 1); le(0, 2); le(1, 2); le(32, 2);
    le(static_cast<std::uint32_t>(png.size()), 4); le(22, 4); ico += png;
    const auto path = root / L"cold-pixel-worker.ico";
    { std::ofstream output(path, std::ios::binary); output.write(ico.data(), static_cast<std::streamsize>(ico.size())); }
    Access::ResetIconPixelProbe(app, context, style, path);
    {
        const auto customPath = root / L"cold-pixel-custom.png";
        { std::ofstream output(customPath, std::ios::binary); output.write(png.data(), static_cast<std::streamsize>(png.size())); }
        IconLibrary library(GetDataSubdirectoryPath(L"icon-library"));
        IconLibraryRequest request; request.action = "import"; request.paths = {customPath.wstring()};
        const auto imported = library.Invoke(request);
        Require(imported.succeeded && imported.imported.size() == 1, "import a private semi-transparent custom icon");
        const auto asset = imported.imported.front();
        Require(!Access::RequestCustomIconPixelProbe(app, context, asset, path.wstring()),
            "an uncached custom image retains its original asynchronous request behavior");
        const auto readyBy = GetTickCount64() + 10000;
        while (!Access::PreparedIconPixelCount(app) && GetTickCount64() < readyBy)
        { Access::DrainCustomIconPixelProbe(app); Sleep(1); }
        Require(Access::PreparedIconPixelCount(app) == 1 && Access::StyledIconPixelCount(app) == 0,
            "the actual custom decoder publishes a managed-alpha CPU snapshot without an early GPU upload");
        const auto customWarm = Access::ReadIconPixelBitmap(app, context,
            Access::RequestCustomIconPixelProbe(app, context, asset, path.wstring()));
        Require(Access::PreparedIconPixelCount(app) == 0 && Access::StyledIconPixelCount(app) == 1,
            "the custom first draw consumes its managed-alpha snapshot");
        Access::ClearIconPixelCache(app);
        const auto customCold = Access::ReadIconPixelBitmap(app, context,
            Access::RequestCustomIconPixelProbe(app, context, asset, path.wstring()));
        Require(customWarm.buffer.pixels == customCold.buffer.pixels && customWarm.glass == customCold.glass &&
                customWarm.buffer.width == customCold.buffer.width && customWarm.buffer.height == customCold.buffer.height,
            "custom worker and managed-alpha fallback preserve identical pixels, size and glass metadata");
    }
    Access::ResetIconPixelProbe(app, context, style, path);
    Access::QueueIconPixelProbe(app, path);
    const auto deadline = GetTickCount64() + 10000;
    while (!Access::IconPixelSource(app) && GetTickCount64() < deadline)
    { Access::DrainIconPixelProbe(app); Sleep(1); }
    Require(Access::IconPixelSource(app) && Access::StyledIconPixelCount(app) == 0 && Access::PreparedIconPixelCount(app) == 1,
        "actual Shell worker delivery prepares CPU pixels without uploading an unshown icon");
    Access::StopIconPixelProbe(app);
    const auto worker = Access::ReadIconPixelProbe(app, context, Access::IconPixelSource(app), style);
    Require(Access::StyledIconPixelCount(app) == 1 && Access::PreparedIconPixelCount(app) == 0,
        "first drawing consumes the worker-prepared snapshot and uploads exactly one GPU entry");
    Access::ClearIconPixelCache(app);
    const auto fallback = Access::ReadIconPixelProbe(app, context, Access::IconPixelSource(app), style);
    Require(worker.buffer.pixels == fallback.buffer.pixels && worker.glass == fallback.glass &&
        worker.buffer.width == fallback.buffer.width && worker.buffer.height == fallback.buffer.height,
        "worker-prepared upload and device-cache cold fallback return identical complete pixels and glass metadata");

    image.width = 96; image.height = 40; image.pixels.assign(96 * 40, 0x80402010);
    const auto prepare = [&](HBITMAP bitmap, bool managed = false, bool native = false) {
        std::shared_ptr<const icon_styled_pixels::Prepared> result;
        std::exception_ptr failure;
        std::thread workerThread([&] {
            try
            {
                auto pixels = std::make_shared<icon_styled_pixels::Prepared>();
                if (icon_styled_pixels::Prepare(bitmap, style, managed, native, *pixels)) result = std::move(pixels);
            }
            catch (...) { failure = std::current_exception(); }
        });
        workerThread.join();
        if (failure) std::rethrow_exception(failure);
        Require(result != nullptr, "prepare final source pixels on a non-UI thread");
        return result;
    };
    for (const bool folder : {false, true})
    {
        Access::ResetIconPixelProbe(app, context, style, path);
        const auto bitmap = icon_image::CreateBitmap(image);
        Require(bitmap != nullptr, "create premultiplied rectangular source");
        const auto expected = Access::ReadIconPixelProbe(app, context, bitmap, style);
        Access::ClearIconPixelCache(app);
        Require(Access::DeliverIconPixelProbe(app, bitmap, prepare(bitmap), IconLoadPhase::Phase2, false, folder) &&
                Access::PreparedIconPixelCount(app) == 1 && Access::StyledIconPixelCount(app) == 0 && Access::IconPixelSource(app, folder) == bitmap,
            "actual desktop/folder acceptance installs and prewarms the same source");
        const auto warmed = Access::ReadIconPixelProbe(app, context, bitmap, style);
        Require(warmed.buffer.width == 96 && warmed.buffer.height == 96 && warmed.buffer.pixels == expected.buffer.pixels &&
                warmed.glass == expected.glass && warmed.glass,
            "rectangular worker preparation preserves square canvas, premultiplied output and glass metadata");
    }
    Access::ResetIconPixelProbe(app, context, style, path);
    {
        // Distinguish supplied CPU pixels from silent recomputation at the
        // upload boundary; the real-worker equivalence check remains above.
        const auto source = icon_image::CreateBitmap(image);
        auto marker = std::make_shared<icon_styled_pixels::Prepared>(*prepare(source));
        marker->buffer.pixels.front() = 0xff12ab34;
        Require(Access::DeliverIconPixelProbe(app, source, marker), "accept a private prepared-payload boundary probe");
        const auto delivered = Access::ReadIconPixelProbe(app, context, source, style);
        Require(delivered.buffer.pixels.front() == 0xff12ab34,
            "the cold upload consumes the immutable supplied CPU payload instead of recomputing it on the UI");
        const auto absent = icon_image::CreateBitmap(image);
        Require(Access::DeliverIconPixelProbe(app, absent, {}), "accept the same source with no optional prewarming");
        const auto cold = Access::ReadIconPixelProbe(app, context, absent, style);
        Require(cold.buffer.pixels.front() != 0xff12ab34,
            "the original cold fallback is an independent negative control for prepared-payload routing");
    }
    Access::ResetIconPixelProbe(app, context, style, path);
    auto bitmap = icon_image::CreateBitmap(image);
    auto changed = style; changed.contentScale += 0.0001f;
    Access::SetIconPixelStyle(app, changed);
    Require(Access::DeliverIconPixelProbe(app, bitmap, prepare(bitmap)) && Access::PreparedIconPixelCount(app) == 0 && Access::StyledIconPixelCount(app) == 0,
        "even a sub-tolerance style change rejects an older worker snapshot");
    const auto changedOutput = Access::ReadIconPixelProbe(app, context, bitmap, changed);
    Require(changedOutput.style.contentScale == changed.contentScale, "style miss retains the current synchronous result");
    Access::SetIconPixelStyle(app, style);
    for (const auto flags : {std::pair{true, false}, std::pair{false, true}})
    {
        bitmap = icon_image::CreateBitmap(image);
        Require(Access::DeliverIconPixelProbe(app, bitmap, prepare(bitmap, flags.first, flags.second)) &&
                Access::PreparedIconPixelCount(app) == 0 && Access::StyledIconPixelCount(app) == 0,
            "managed-alpha or native-control pixels never enter a Shell-source cache entry");
    }
    bitmap = Access::IconPixelSource(app);
    const auto retained = Access::ReadIconPixelProbe(app, context, bitmap, style);
    for (const bool stale : {false, true})
    {
        const auto rejected = icon_image::CreateBitmap(image);
        const auto accepted = Access::DeliverIconPixelProbe(app, rejected, prepare(rejected),
            stale ? IconLoadPhase::Phase2 : IconLoadPhase::Phase1, stale);
        BITMAP released{};
        Require(accepted == !stale && Access::IconPixelSource(app) == bitmap &&
                Access::StyledIconPixelCount(app) == 1 && !GetObjectW(rejected, sizeof(released), &released),
            "refresh-first or obsolete results are released without changing the retained bitmap or warmed cache");
    }
    Access::ClearIconPixelCache(app);
    auto unrelated = icon_image::CreateBitmap(image);
    const auto wrongSource = prepare(unrelated);
    auto replacement = icon_image::CreateBitmap(image);
    Require(Access::DeliverIconPixelProbe(app, replacement, wrongSource) && Access::PreparedIconPixelCount(app) == 0 && Access::StyledIconPixelCount(app) == 0,
        "prepared pixels from another live HBITMAP cannot be reused after replacement");
    DeleteObject(unrelated);
    replacement = icon_image::CreateBitmap(image);
    auto prepared = prepare(replacement);
    Access::SetIconPixelContext(app, nullptr);
    Require(Access::DeliverIconPixelProbe(app, replacement, prepared) && Access::StyledIconPixelCount(app) == 0 && Access::PreparedIconPixelCount(app) == 1,
        "delivery without a device retains CPU pixels and the model image without publishing a GPU entry");
    const auto rebuilt = Access::ReadIconPixelProbe(app, context, replacement, style);
    Require(rebuilt.buffer.pixels == retained.buffer.pixels && rebuilt.glass == retained.glass,
        "a newly available device uploads the accepted CPU snapshot with unchanged pixels");
    replacement = icon_image::CreateBitmap(image);
    Require(Access::DeliverIconPixelProbe(app, replacement, prepare(replacement)) &&
            Access::PreparedIconPixelCount(app) == 1, "seed an unconsumed CPU snapshot before device invalidation");
    Access::ClearIconPixelCache(app);
    Require(Access::PreparedIconPixelCount(app) == 0 && Access::StyledIconPixelCount(app) == 0,
        "device/style cache invalidation clears both CPU and GPU entries");
    const auto coldAgain = Access::ReadIconPixelProbe(app, context, replacement, style);
    Require(coldAgain.buffer.pixels == rebuilt.buffer.pixels, "cleared CPU snapshots retain the exact synchronous fallback");
    auto next = icon_image::CreateBitmap(image);
    Require(Access::DeliverIconPixelProbe(app, next, prepare(next)) && Access::PreparedIconPixelCount(app) == 1,
        "a fresh accepted source can be prepared after device invalidation");
    auto last = icon_image::CreateBitmap(image);
    Require(Access::DeliverIconPixelProbe(app, last, prepare(last)) && Access::PreparedIconPixelCount(app) == 1,
        "replacing an unshown source erases its borrowed-handle CPU snapshot");
    {
        // Budget-only fixture: the cache never reads or owns its borrowed GDI handle.
        auto oversized = std::make_shared<icon_styled_pixels::Prepared>();
        oversized->source = last;
        oversized->buffer.width = 2049; oversized->buffer.height = 2049;
        oversized->buffer.pixels.resize(2049u * 2049u);
        icon_styled_pixels::Cache bounded;
        bounded.Put(oversized);
        BITMAP live{};
        Require(bounded.Size() == 0 && GetObjectW(last, sizeof(live), &live),
            "a snapshot above 16 MiB remains a cold miss without changing source lifetime");
    }
    Access::ResetIconPixelProbe(app, nullptr, style, path);
    icon_styled_pixels::Prepared invalid;
    Require(!icon_styled_pixels::Prepare(nullptr, style, false, false, invalid),
        "failed prewarming remains an explicit miss");
}
}

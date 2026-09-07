#include "large_icon_assets.h"
#include "http_runtime.h"
#include "preview_png_writer.h"
#include "atomic_file.h"
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>

namespace
{
std::atomic<int> requests = 0;
int failures = 0;
void Check(bool condition, const char* message)
{
    if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
struct Queue
{
    std::mutex mutex;
    std::condition_variable changed;
    bool ready = false;
    snowdesktop::LargeIconAssets assets;
    explicit Queue(const std::filesystem::path& directory) : assets(directory, [this] {
        std::lock_guard lock(mutex); ready = true; changed.notify_one();
    }, directory.parent_path() / L"steam") {}
    ~Queue() { assets.Stop(); }
    std::vector<snowdesktop::LargeIconAssetResult> Wait(size_t count)
    {
        std::vector<snowdesktop::LargeIconAssetResult> results;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (results.size() < count)
        {
            auto next = assets.TakeCompleted();
            for (auto& value : next) results.push_back(std::move(value));
            if (results.size() >= count) break;
            std::unique_lock lock(mutex);
            if (!changed.wait_until(lock, deadline, [&] { return ready; })) break;
            ready = false;
        }
        Check(results.size() == count, "asset jobs complete without blocking on another request");
        return results;
    }
};
}

// No test contacts Steam. The existing HTTP tests own transport/redirect
// behavior; this fake counts attempts and deterministically returns 404.
namespace snowdesktop::http_security
{
bool IsAllowedUrlForDomains(const std::wstring&, const std::vector<std::string>&, bool, bool) { return true; }
}
namespace snowdesktop::http_stream
{
Result StreamHttpGet(const Options& options, std::stop_token token, const HeadCallback& head, const ChunkSink&)
{
    ++requests;
    Result result; result.head.status = 404; result.head.finalUrl = options.url;
    result.cancelled = token.stop_requested(); result.responseAccepted = head(result.head);
    return result;
}
}

int RunLargeIconAssetTests()
{
    namespace fs = std::filesystem;
    using namespace snowdesktop;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto root = fs::temp_directory_path() / (L"SnowDesktop-large-icons-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto directory = root / L"managed";
    fs::create_directories(directory);
    std::string error;
    const auto input = root / L"full-source.png";
    std::vector<std::uint32_t> pixels(640 * 320, 0xff22aa77);
    Check(preview_png::Save(input, 640, 320, pixels, error), "create a real WIC-decoded source fixture");
    std::string sourceBytes; atomic_file::ReadAll(input, sourceBytes);
    {
        Queue queue(directory);
        LargeIconAssetRequest request;
        request.itemKey = L"import"; request.generation = 1; request.pixels = 64; request.importPath = input;
        queue.assets.Request(request);
        auto result = queue.Wait(1);
        if (result.empty()) return failures;
        Check(result[0].asset && result[0].asset->width == 64 && result[0].asset->height == 32,
            "display decodes at the requested size with aspect ratio intact");
        if (result[0].asset)
        {
            const auto reference = result[0].asset->reference;
            std::string retainedBytes; atomic_file::ReadAll(directory / reference, retainedBytes);
            Check(retainedBytes == sourceBytes, "import preserves full source bytes for later resizing and backup");
            Check(!result[0].asset->previewReference.empty() && fs::exists(directory / result[0].asset->previewReference),
                "settings receive a separate PNG preview");
            request.importPath.clear(); request.content = 1; request.reference = reference; request.generation = 2; request.pixels = 512;
            queue.assets.Request(request);
            auto larger = queue.Wait(1);
            Check(!larger.empty() && larger[0].asset && larger[0].asset->width == 512,
                "resizing re-decodes the original instead of enlarging the old 64-pixel cache");
            request.itemKey = L"same-source"; request.generation = 3;
            queue.assets.Request(request);
            auto shared = queue.Wait(1);
            Check(!larger.empty() && !shared.empty() && shared[0].asset == larger[0].asset,
                "identical source and display size share decoded bitmap memory");

            LargeIconAssetRequest steam;
            steam.itemKey = L"steam"; steam.content = 2; steam.appId = 4294967294u; steam.generation = 4;
            steam.localOnly = true; steam.lastGood = reference;
            const int before = requests;
            queue.assets.Request(steam);
            auto offline = queue.Wait(1);
            Check(requests == before && !offline.empty() && offline[0].asset && offline[0].asset->reference == reference,
                "local-only mode performs no HTTP requests and preserves the last usable cover");
            steam.localOnly = false; steam.generation = 5;
            queue.assets.Request(steam); queue.Wait(1);
            Check(requests > before, "a missing online cover attempts metadata lookup");
            const int failed = requests;
            steam.generation = 6;
            queue.assets.Request(steam); queue.Wait(1);
            Check(requests == failed, "a cached 404 suppresses repeated metadata requests");
            steam.refresh = true; steam.generation = 7;
            queue.assets.Request(steam); queue.Wait(1);
            Check(requests > failed, "manual refresh bypasses the negative cache");
        }
        const auto corrupt = root / L"broken.png";
        atomic_file::WriteAll(corrupt, "not a valid image");
        request.itemKey = L"broken"; request.generation = 8; request.importPath = corrupt; request.refresh = true;
        queue.assets.Request(request);
        auto invalid = queue.Wait(1);
        Check(!invalid.empty() && !invalid[0].asset && !invalid[0].error.empty(), "corrupt imports fail without publishing a resource");
        const auto huge = root / L"too-large.png";
        sourceBytes.resize(16 * 1024 * 1024 + 1, 'x'); atomic_file::WriteAll(huge, sourceBytes);
        request.itemKey = L"huge"; request.importPath = huge; request.generation = 9;
        queue.assets.Request(request);
        auto oversized = queue.Wait(1);
        Check(!oversized.empty() && !oversized[0].asset, "source files over 16 MiB are rejected before decoding");
        const auto gif = root / L"unsupported.gif";
        const unsigned char gifBytes[] = {71,73,70,56,57,97,1,0,1,0,128,0,0,0,0,0,255,255,255,33,249,4,1,0,0,0,0,44,0,0,0,0,1,0,1,0,0,2,2,68,1,0,59};
        atomic_file::WriteAll(gif, std::string_view(reinterpret_cast<const char*>(gifBytes), sizeof(gifBytes)));
        request.itemKey = L"gif"; request.importPath = gif; request.generation = 10;
        queue.assets.Request(request);
        auto unsupported = queue.Wait(1);
        Check(!unsupported.empty() && !unsupported[0].asset, "GIF content is rejected even if WIC can decode it");
        const auto library = root / L"steam" / L"appcache" / L"librarycache";
        fs::create_directories(library / L"99992" / L"hash");
        fs::copy_file(input, library / L"99991_library_600x900.jpg");
        fs::copy_file(input, library / L"99992" / L"hash" / L"library_600x900_2x.jpg");
        const int beforeLocal = requests;
        for (std::uint32_t id : {99991u, 99992u})
        {
            LargeIconAssetRequest local;
            local.itemKey = std::to_wstring(id); local.generation = id; local.content = 2;
            local.appId = id; local.portrait = true; local.localOnly = true;
            queue.assets.Request(local);
            auto cover = queue.Wait(1);
            Check(!cover.empty() && cover[0].asset && cover[0].asset->source == "steam-local",
                "both legacy flat and AppID/hash local Steam cache layouts are decoded");
        }
        Check(requests == beforeLocal, "local cache discovery does not request Steam metadata");
        const auto giant = root / L"too-many-pixels.png";
        pixels.assign(4001 * 4000, 0xff22aa77);
        Check(preview_png::Save(giant, 4001, 4000, pixels, error), "create compressed source exceeding decoded pixel limit");
        pixels.clear(); pixels.shrink_to_fit();
        request.itemKey = L"pixel-limit"; request.generation = 11; request.importPath = giant;
        queue.assets.Request(request);
        auto giantResult = queue.Wait(1);
        Check(!giantResult.empty() && !giantResult[0].asset, "sources over sixteen million pixels are rejected before pixel conversion");
    }
    if (root.parent_path() == fs::temp_directory_path() && root.filename().wstring().starts_with(L"SnowDesktop-large-icons-"))
        fs::remove_all(root);
    if (SUCCEEDED(initialized)) CoUninitialize();
    return failures;
}

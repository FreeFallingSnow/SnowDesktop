#include "large_icon_assets.h"
#include "large_icon_steam.h"
#include "http_runtime.h"
#include "preview_png_writer.h"
#include "atomic_file.h"
#include <objbase.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>

namespace
{
std::atomic<int> requests = 0;
std::mutex networkMutex;
std::condition_variable_any networkChanged;
bool networkBlocked = false;
int activeRequests = 0, peakRequests = 0;
std::unordered_map<std::wstring, std::pair<int, std::string>> responses;
std::vector<std::wstring> requestUrls;
int failures = 0;
void Check(bool condition, const char* message)
{
    if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
bool EncodeFixture(const std::filesystem::path& path, REFGUID container, int width, int height)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(container, nullptr, &encoder)) || FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize(width, height))) return false;
    auto format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&format))) return false;
    std::vector<std::uint32_t> pixels(static_cast<size_t>(width) * height, 0xff22aa77);
    ComPtr<IWICBitmap> source;
    return SUCCEEDED(factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA, width * 4,
        static_cast<UINT>(pixels.size() * 4), reinterpret_cast<BYTE*>(pixels.data()), &source)) &&
        SUCCEEDED(frame->WriteSource(source.Get(), nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}
void AppendLe(std::string& bytes, std::uint32_t value, int length)
{ for (int i = 0; i < length; ++i) bytes.push_back(static_cast<char>((value >> (i * 8)) & 255)); }
struct Queue
{
    std::mutex mutex;
    std::condition_variable changed;
    bool ready = false;
    snowdesktop::LargeIconAssets assets;
    explicit Queue(const std::filesystem::path& directory, snowdesktop::LargeIconAssetLimits limits = {}) : assets(directory, [this] {
        std::lock_guard lock(mutex); ready = true; changed.notify_one();
    }, directory.parent_path() / L"steam", limits) {}
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
// behavior; controlled responses exercise metadata, language fallback and WIC.
namespace snowdesktop::http_security
{
bool IsAllowedUrlForDomains(const std::wstring&, const std::vector<std::string>&, bool, bool) { return true; }
}
namespace snowdesktop::http_stream
{
Result StreamHttpGet(const Options& options, std::stop_token token, const HeadCallback& head, const ChunkSink& sink)
{
    ++requests;
    std::pair<int, std::string> response{404, {}};
    {
        std::unique_lock lock(networkMutex);
        requestUrls.push_back(options.url);
        peakRequests = std::max(peakRequests, ++activeRequests);
        networkChanged.notify_all();
        networkChanged.wait(lock, token, [] { return !networkBlocked; });
        --activeRequests;
        if (const auto found = responses.find(options.url); found != responses.end()) response = found->second;
    }
    Result result; result.head.status = response.first; result.head.finalUrl = options.url;
    result.cancelled = token.stop_requested(); result.responseAccepted = head(result.head);
    if (!result.cancelled && result.responseAccepted)
    {
        if (response.second.size() > options.maximumResponseBytes) result.error = "response too large";
        else if (!sink(std::as_bytes(std::span(response.second.data(), response.second.size())))) result.error = "sink failed";
        else result.bytesReceived = response.second.size();
    }
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
            Check(result[0].asset->hasEdgeColor && result[0].asset->edgeColor == 0x22aa77 && result[0].asset->accent != 0,
                "reliable edge RGB is separate from the bounded theme palette and keeps its exact source color");
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
        for (const auto container : {GUID_ContainerFormatBmp, GUID_ContainerFormatJpeg})
        {
            const auto path = root / (container == GUID_ContainerFormatBmp ? L"actual.bmp" : L"actual.jpg");
            Check(EncodeFixture(path, container, 180, 90), "encode actual BMP/JPEG bytes through the installed WIC codec");
            LargeIconAssetRequest format;
            format.itemKey = path.wstring(); format.importPath = path; format.pixels = 64; format.generation = 1;
            queue.assets.Request(format);
            auto imported = queue.Wait(1);
            Check(!imported.empty() && imported[0].asset && imported[0].asset->width == 64 && imported[0].asset->height == 32,
                "BMP and JPEG imports decode with the same aspect-preserving rules");
            if (!imported.empty() && imported[0].asset)
            {
                std::string originalBytes, copiedBytes;
                atomic_file::ReadAll(path, originalBytes); atomic_file::ReadAll(directory / imported[0].asset->reference, copiedBytes);
                Check(originalBytes == copiedBytes, "BMP/JPEG import retains original encoded bytes for resizing and backup");
            }
        }
        const auto smallIcon = root / L"ico-small.png", largeIcon = root / L"ico-large.png";
        Check(preview_png::Save(smallIcon, 16, 16, std::vector<std::uint32_t>(16 * 16, 0xffff0000), error) &&
            preview_png::Save(largeIcon, 128, 128, std::vector<std::uint32_t>(128 * 128, 0xff00ff00), error), "encode two real ICO frames");
        std::string smallPng, largePng, ico;
        atomic_file::ReadAll(smallIcon, smallPng); atomic_file::ReadAll(largeIcon, largePng);
        AppendLe(ico, 0, 2); AppendLe(ico, 1, 2); AppendLe(ico, 2, 2);
        for (int i = 0; i < 2; ++i)
        {
            AppendLe(ico, i ? 128 : 16, 1); AppendLe(ico, i ? 128 : 16, 1); AppendLe(ico, 0, 2);
            AppendLe(ico, 1, 2); AppendLe(ico, 32, 2);
            AppendLe(ico, static_cast<std::uint32_t>(i ? largePng.size() : smallPng.size()), 4);
            AppendLe(ico, 38 + static_cast<std::uint32_t>(i ? smallPng.size() : 0), 4);
        }
        ico += smallPng; ico += largePng;
        const auto icoPath = root / L"multi-frame.ico"; atomic_file::WriteAll(icoPath, ico);
        LargeIconAssetRequest iconRequest;
        iconRequest.itemKey = L"ico-largest"; iconRequest.importPath = icoPath; iconRequest.pixels = 256; iconRequest.generation = 1;
        queue.assets.Request(iconRequest);
        auto multiFrame = queue.Wait(1);
        Check(!multiFrame.empty() && multiFrame[0].asset && multiFrame[0].asset->width == 128 && multiFrame[0].asset->height == 128,
            "multi-frame ICO import chooses the largest valid frame instead of enlarging its first tiny frame");
        const auto shortcut = root / L"steam-game.url";
        atomic_file::WriteAll(shortcut, "[InternetShortcut]\r\nURL=steam://rungameid/949230\r\nIconIndex=0\r\nIconFile=multi-frame.ico\r\n");
        LargeIconAssetRequest steamIcon;
        steamIcon.itemKey = L"steam-foreground"; steamIcon.parsingName = shortcut.wstring(); steamIcon.pixels = 256; steamIcon.generation = 1;
        const auto beforeIcon = requests.load();
        queue.assets.Request(steamIcon);
        auto foreground = queue.Wait(1);
        Check(!foreground.empty() && foreground[0].asset && foreground[0].asset->width == 128 &&
            foreground[0].asset->source == "original" && foreground[0].asset->edgeColor == 0x00ff00 && requests == beforeIcon,
            "Steam foreground reads the largest declared URL icon locally instead of the generic Shell document");
        atomic_file::WriteAll(shortcut, "[InternetShortcut]\r\nURL=https://www.bilibili.com/\r\nIconFile=multi-frame.ico\r\n");
        steamIcon.refresh = true; ++steamIcon.generation;
        queue.assets.Request(steamIcon);
        auto website = queue.Wait(1);
        Check(!website.empty() && website[0].asset && website[0].asset->width == 128 && website[0].asset->edgeColor == 0x00ff00,
            "website large icons load a downloaded ICO through the same explicit-icon path");
        Microsoft::WRL::ComPtr<IShellLinkW> browserLink;
        Microsoft::WRL::ComPtr<IPersistFile> browserFile;
        const auto browserShortcut = root / L"browser.lnk";
        Check(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&browserLink))) &&
            SUCCEEDED(browserLink->SetPath(L"C:\\Browser\\chrome.exe")) && SUCCEEDED(browserLink->SetArguments(L"https://www.bilibili.com/")) &&
            SUCCEEDED(browserLink->SetIconLocation(icoPath.c_str(), 0)) && SUCCEEDED(browserLink.As(&browserFile)) &&
            SUCCEEDED(browserFile->Save(browserShortcut.c_str(), TRUE)), "create an explicit-icon browser shortcut");
        browserFile.Reset(); browserLink.Reset();
        steamIcon.parsingName = browserShortcut.wstring(); ++steamIcon.generation;
        queue.assets.Request(steamIcon);
        auto browserAsset = queue.Wait(1);
        Check(!browserAsset.empty() && browserAsset[0].asset && browserAsset[0].asset->width == 128 && browserAsset[0].asset->edgeColor == 0x00ff00,
            "browser .lnk large icons use their declared ICO instead of the Shell's generic document");
        for (unsigned alpha : {0u, 16u})
        {
            const auto transparentPath = root / (L"transparent-" + std::to_wstring(alpha) + L".png");
            const unsigned transparentPixel = (alpha << 24) | (alpha << 8);
            Check(preview_png::Save(transparentPath, 64, 64, std::vector<std::uint32_t>(64 * 64, transparentPixel), error), "encode transparent color samples");
            LargeIconAssetRequest noColor;
            noColor.itemKey = transparentPath.wstring(); noColor.importPath = transparentPath; noColor.generation = 1;
            queue.assets.Request(noColor);
            auto transparent = queue.Wait(1);
            Check(!transparent.empty() && transparent[0].asset && transparent[0].asset->accent == 0 && !transparent[0].asset->hasEdgeColor,
                "fully transparent and negligible-alpha pixels leave color selection to the current host theme");
        }
        const auto tooManyPixels = root / L"over-pixel-limit.png";
        Check(preview_png::Save(tooManyPixels, 4001, 4000, std::vector<std::uint32_t>(4001ull * 4000, 0xff55aa88), error), "encode a valid image just above sixteen million pixels");
        Check(fs::file_size(tooManyPixels) < 16 * 1024 * 1024, "pixel-limit fixture remains below the encoded byte limit");
        LargeIconAssetRequest pixelLimit;
        pixelLimit.itemKey = L"pixel-limit"; pixelLimit.importPath = tooManyPixels; pixelLimit.generation = 1;
        queue.assets.Request(pixelLimit);
        const auto rejectedPixels = queue.Wait(1);
        Check(!rejectedPixels.empty() && !rejectedPixels[0].asset, "source dimensions are rejected even when the PNG compresses below the file-size limit");
        const auto library = root / L"steam" / L"appcache" / L"librarycache";
        fs::create_directories(library / L"99992" / L"hash");
        fs::create_directories(library / L"99993" / L"current-hash");
        fs::copy_file(input, library / L"99991_library_600x900.jpg");
        fs::copy_file(input, library / L"99992" / L"hash" / L"library_600x900_2x.jpg");
        fs::copy_file(input, library / L"99993" / L"current-hash" / L"library_capsule_2x.jpg");
        Check(preview_png::Save(library / L"99993" / L"current-hash" / L"library_capsule.jpg", 16, 8,
            std::vector<std::uint32_t>(16 * 8, 0xff221177), error), "create a lower-resolution current Steam cache candidate");
        const int beforeLocal = requests;
        for (std::uint32_t id : {99991u, 99992u, 99993u})
        {
            LargeIconAssetRequest local;
            local.itemKey = std::to_wstring(id); local.generation = id; local.content = 2;
            local.appId = id; local.portrait = true; local.localOnly = true;
            queue.assets.Request(local);
            auto cover = queue.Wait(1);
            Check(!cover.empty() && cover[0].asset && cover[0].asset->source == "steam-local",
                "legacy flat, AppID/hash and current library_capsule local caches are decoded");
            if (id == 99993u)
                Check(!cover.empty() && cover[0].asset && cover[0].asset->width == 256,
                    "a current 2x library capsule takes priority over a newer low-resolution candidate");
        }
        Check(requests == beforeLocal, "local cache discovery does not request Steam metadata");

        const auto wide = [](const std::string& text) { return std::wstring(text.begin(), text.end()); };
        std::string imageBytes; atomic_file::ReadAll(input, imageBytes);
        const std::string metadata = R"({"response":{"store_items":[{"appid":90001,"assets":{"asset_url_format":"steam/apps/90001/${FILENAME}?t=100","header":"header.jpg","header_2x":"hash-wide/header_2x.jpg","library_capsule_2x":"hash-tall/library_capsule_2x.jpg"}}]}})";
        const std::wstring horizontalUrl = L"https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/90001/hash-wide/header_2x.jpg?t=100";
        const std::wstring portraitUrl = L"https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/90001/hash-tall/library_capsule_2x.jpg?t=100";
        {
            std::lock_guard lock(networkMutex);
            responses[wide(large_icon_steam::MetadataUrl(90001, "english"))] = {200, metadata};
            responses[horizontalUrl] = {200, imageBytes};
            responses[portraitUrl] = {200, imageBytes};
        }
        for (bool portrait : {false, true})
        {
            LargeIconAssetRequest online;
            online.itemKey = portrait ? L"live-path-tall" : L"live-path-wide";
            online.content = 2; online.appId = 90001; online.language = "schinese";
            online.generation = portrait ? 301 : 300; online.portrait = portrait;
            const int before = requests;
            queue.assets.Request(online);
            auto downloaded = queue.Wait(1);
            Check(!downloaded.empty() && downloaded[0].asset && downloaded[0].asset->source == "steam-online" &&
                downloaded[0].asset->width == 256 && requests == before + 3,
                "localized metadata failure falls back to English, resolves a hashed 2x asset, downloads and decodes it");
            if (!downloaded.empty() && downloaded[0].asset)
            {
                std::string saved; atomic_file::ReadAll(directory / downloaded[0].asset->reference, saved);
                Check(saved == imageBytes, "online artwork preserves validated source bytes for larger future displays");
            }
            {
                std::lock_guard lock(networkMutex);
                Check(!requestUrls.empty() && requestUrls.back() == (portrait ? portraitUrl : horizontalUrl),
                    "online acquisition uses the actual hashed high-resolution artwork path");
            }
            online.localOnly = true; ++online.generation;
            const int beforeOffline = requests;
            queue.assets.Request(online);
            auto cached = queue.Wait(1);
            Check(!cached.empty() && cached[0].asset && requests == beforeOffline,
                "a downloaded cover remains available after changing to local-only policy");
        }

    // Reuse the validated oversized fixture for the refresh request as well.
    request.itemKey = L"pixel-limit"; request.generation = 11; request.importPath = tooManyPixels;
        queue.assets.Request(request);
        auto giantResult = queue.Wait(1);
        Check(!giantResult.empty() && !giantResult[0].asset, "sources over sixteen million pixels are rejected before pixel conversion");

        // Hold the two workers at a deterministic boundary. This proves the
        // concurrency cap and coalescing without sleep-based timing guesses.
        { std::lock_guard lock(networkMutex); networkBlocked = true; peakRequests = 0; }
        const int beforeBatch = requests;
        for (int i = 0; i < 4; ++i)
        {
            LargeIconAssetRequest cover;
            cover.itemKey = L"concurrent-" + std::to_wstring(i); cover.content = 2;
            cover.appId = 99994 + i; cover.generation = 100 + i;
            queue.assets.Request(cover);
            if (i == 0)
            {
                cover.itemKey = L"shared-request"; cover.generation = 104;
                queue.assets.Request(cover);
            }
        }
        {
            std::unique_lock lock(networkMutex);
            Check(networkChanged.wait_for(lock, std::chrono::seconds(10), [] { return activeRequests == 2; }),
                "two concurrent resource workers reach the network boundary");
            Check(peakRequests == 2, "cover acquisition never exceeds two simultaneous tasks");
            networkBlocked = false;
        }
        networkChanged.notify_all();
        auto batch = queue.Wait(5);
        Check(requests == beforeBatch + 4, "the same AppID, artwork type and language share one metadata request");

        fs::copy_file(input, directory / L"fallback-a.png");
        fs::copy_file(input, directory / L"fallback-b.png");
        { std::lock_guard lock(networkMutex); networkBlocked = true; }
        LargeIconAssetRequest ownFallback;
        ownFallback.itemKey = L"fallback-a"; ownFallback.content = 2; ownFallback.appId = 95001;
        ownFallback.generation = 501; ownFallback.lastGood = "fallback-a.png";
        const int beforeFallback = requests;
        queue.assets.Request(ownFallback);
        {
            std::unique_lock lock(networkMutex);
            Check(networkChanged.wait_for(lock, std::chrono::seconds(10), [] { return activeRequests == 1; }),
                "shared artwork acquisition starts before the second fallback listener joins");
        }
        ownFallback.itemKey = L"fallback-b"; ownFallback.generation = 502; ownFallback.lastGood = "fallback-b.png";
        queue.assets.Request(ownFallback);
        { std::lock_guard lock(networkMutex); networkBlocked = false; }
        networkChanged.notify_all();
        auto ownCovers = queue.Wait(2);
        for (const auto& cover : ownCovers)
            Check(cover.asset && cover.asset->reference == cover.request.lastGood && !cover.error.empty(),
                "coalesced failures preserve each listener's own last-good image and retry state");
        Check(requests == beforeFallback + 1, "per-instance fallback does not duplicate the shared metadata request");

        // A cache hit completes synchronously; cancelling before drain must
        // suppress that already-queued completion as well as pending work.
        LargeIconAssetRequest cancelled;
        cancelled.itemKey = L"cancelled-cache-hit"; cancelled.content = 1; cancelled.pixels = 256;
        cancelled.reference = "fallback-a.png"; cancelled.generation = 503;
        queue.assets.Request(cancelled);
        queue.assets.Cancel(cancelled.itemKey);
        Check(queue.assets.TakeCompleted().empty(), "cancelled queued cache results cannot reach the host");

        { std::lock_guard lock(networkMutex); networkBlocked = true; }
        LargeIconAssetRequest stale;
        stale.itemKey = L"superseded"; stale.content = 2; stale.appId = 99999; stale.generation = 201;
        queue.assets.Request(stale);
        {
            std::unique_lock lock(networkMutex);
            Check(networkChanged.wait_for(lock, std::chrono::seconds(10), [] { return activeRequests == 1; }),
                "old cover work starts before the source is changed");
        }
        stale.content = 1; stale.reference = result[0].asset ? result[0].asset->reference : "missing.png"; stale.generation = 202;
        queue.assets.Request(stale);
        auto latest = queue.Wait(1);
        Check(!latest.empty() && latest[0].request.generation == 202 && latest[0].asset,
            "a superseded cover callback cannot replace the newer user image request");
        { std::lock_guard lock(networkMutex); networkBlocked = false; }
        networkChanged.notify_all();
    }

    const auto square = root / L"square.png";
    Check(preview_png::Save(square, 64, 64, std::vector<std::uint32_t>(64 * 64, 0xff2277aa), error), "create a fixed-size memory-budget fixture");
    {
        Queue budget(root / L"memory-budget", {2 * 64 * 64 * 4, 512ull * 1024 * 1024});
        LargeIconAssetRequest image;
        image.importPath = square; image.pixels = 64;
        image.itemKey = L"pinned-1"; image.generation = 1; budget.assets.Request(image);
        auto first = budget.Wait(1);
        image.itemKey = L"pinned-2"; image.generation = 2; budget.assets.Request(image);
        auto second = budget.Wait(1);
        image.itemKey = L"over-budget"; image.generation = 3; budget.assets.Request(image);
        auto third = budget.Wait(1);
        Check(!first.empty() && first[0].asset && !second.empty() && second[0].asset &&
            !third.empty() && !third[0].asset && !third[0].error.empty(),
            "live decoded images count against the budget and cannot be evicted to admit another image");
        first.clear(); second.clear();
        image.itemKey = L"after-release"; image.generation = 4; budget.assets.Request(image);
        auto recovered = budget.Wait(1);
        Check(!recovered.empty() && recovered[0].asset,
            "releasing live image references permits LRU eviction and subsequent decoding");
    }
    {
        const auto disk = root / L"disk-budget";
        fs::create_directories(disk);
        for (const auto* name : {L"steam-96001-landscape-english.png", L"steam-96002-landscape-english.png", L"steam-96003-landscape-english.png",
            L"import-user.png", L"unrelated.txt", L"steam-personal.png", L"steam-个人.png"})
            atomic_file::WriteAll(disk / name, std::string(3000, 'x'));
        Queue budget(disk, {128ull * 1024 * 1024, 4096});
        budget.assets.RetainReferences({"steam-96003-landscape-english.png"});
        LargeIconAssetRequest image;
        image.itemKey = L"collect"; image.generation = 1; image.importPath = square; image.pixels = 64;
        budget.assets.Request(image);
        {
            std::unique_lock lock(budget.mutex);
            Check(budget.changed.wait_for(lock, std::chrono::seconds(10), [&] { return budget.ready; }),
                "the cache collection worker completes before filesystem assertions");
        }
        auto collected = budget.Wait(1);
        Check(!fs::exists(disk / L"steam-96001-landscape-english.png") && !fs::exists(disk / L"steam-96002-landscape-english.png") &&
            fs::exists(disk / L"steam-96003-landscape-english.png") && fs::exists(disk / L"import-user.png") && fs::exists(disk / L"unrelated.txt") &&
            fs::exists(disk / L"steam-personal.png") && fs::exists(disk / L"steam-个人.png") &&
            !collected.empty() && collected[0].asset && fs::exists(disk / collected[0].asset->previewReference),
            "automatic disk eviction preserves retained sources, active previews, user imports and unrelated files");
    }
    if (root.parent_path() == fs::temp_directory_path() && root.filename().wstring().starts_with(L"SnowDesktop-large-icons-"))
        fs::remove_all(root);
    if (SUCCEEDED(initialized)) CoUninitialize();
    return failures;
}

// The real Shell extractor is an integration boundary; it runs separately
// from deterministic WIC/metadata tests without opening a desktop window.
int RunLargeIconShellAssetTests()
{
    namespace fs = std::filesystem;
    using namespace snowdesktop;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto root = fs::temp_directory_path() / (L"SnowDesktop-large-icons-shell-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(root);
    {
        Queue queue(root / L"managed");
        const auto shellFile = root / L"shell-original.txt";
        // Exercise the production async loader and real Shell provider. Merely
        // obtaining a bitmap also passes for the associated application's icon.
        for (const auto& extension : {L".png", L".jpg"})
        {
            const auto image = root / (std::wstring(L"thumbnail") + extension);
            Check(EncodeFixture(image, std::wstring_view(extension) == L".png" ?
                GUID_ContainerFormatPng : GUID_ContainerFormatJpeg, 160, 80), "encode thumbnail source fixture");
            for (const int size : {64, 256})
            {
                LargeIconAssetRequest preview;
                preview.itemKey = image.wstring(); preview.parsingName = image.wstring();
                preview.pixels = size; preview.generation = size;
                const auto checkPreview = [](const auto& results) {
                    Check(results.size() == 1 && results[0].asset && results[0].error.empty(),
                        "image thumbnail completes through the production asset loader");
                    if (results.empty() || !results[0].asset) return;
                    const auto& asset = *results[0].asset;
                    Check(asset.width == 2 * asset.height,
                        "original image uses the landscape thumbnail instead of a square application icon");
                    HDC dc = CreateCompatibleDC(nullptr);
                    HGDIOBJ previous = dc ? SelectObject(dc, asset.bitmap) : nullptr;
                    const COLORREF color = dc ? GetPixel(dc, asset.width / 2, asset.height / 2) : CLR_INVALID;
                    if (previous) SelectObject(dc, previous);
                    if (dc) DeleteDC(dc);
                    Check(color != CLR_INVALID && std::abs(int(GetRValue(color)) - 34) <= 3 &&
                        std::abs(int(GetGValue(color)) - 170) <= 3 && std::abs(int(GetBValue(color)) - 119) <= 3,
                        "thumbnail pixels preserve the source image content");
                };
                queue.assets.Request(preview);
                checkPreview(queue.Wait(1));
                Queue reopened(root / L"managed");
                reopened.assets.Request(preview);
                checkPreview(reopened.Wait(1));
            }
        }
        atomic_file::WriteAll(shellFile, "Shell icon source");
        LargeIconAssetRequest raw;
        raw.itemKey = L"raw-source-identity"; raw.parsingName = shellFile.wstring(); raw.generation = 401;
        raw.sourceStamp = 1; raw.sourceIconIndex = 10;
        queue.assets.Request(raw);
        auto firstRaw = queue.Wait(1);
        raw.sourceIconIndex = 11; ++raw.generation;
        queue.assets.Request(raw);
        auto changedAssociation = queue.Wait(1);
        raw.sourceStamp = 2; ++raw.generation;
        queue.assets.Request(raw);
        auto changedFile = queue.Wait(1);
        Check(!firstRaw.empty() && firstRaw[0].asset && !changedAssociation.empty() && changedAssociation[0].asset &&
            !changedFile.empty() && changedFile[0].asset &&
            firstRaw[0].asset->reference != changedAssociation[0].asset->reference &&
            changedAssociation[0].asset->reference != changedFile[0].asset->reference,
            "both Shell association changes and file changes invalidate the original-icon cache");
        raw.itemKey = L"missing-raw"; raw.parsingName = (root / L"missing-file.txt").wstring(); ++raw.generation;
        queue.assets.Request(raw);
        auto missingRaw = queue.Wait(1);
        Check(!missingRaw.empty() && !missingRaw[0].asset && !missingRaw[0].error.empty(),
            "failed original extraction reports a retryable resource failure instead of settling silently");

        const auto otherShellFile = root / L"other-shell-original.txt";
        atomic_file::WriteAll(otherShellFile, "Independent Shell identity");
        { std::lock_guard lock(networkMutex); networkBlocked = true; }
        LargeIconAssetRequest shared;
        shared.itemKey = L"shell-fallback-a"; shared.parsingName = shellFile.wstring();
        shared.content = 2; shared.appId = 95002; shared.generation = 601;
        queue.assets.Request(shared);
        {
            std::unique_lock lock(networkMutex);
            Check(networkChanged.wait_for(lock, std::chrono::seconds(10), [] { return activeRequests == 1; }),
                "a shared missing game reaches the network before its other Shell listener joins");
        }
        shared.itemKey = L"shell-fallback-b"; shared.parsingName = otherShellFile.wstring(); shared.generation = 602;
        queue.assets.Request(shared);
        { std::lock_guard lock(networkMutex); networkBlocked = false; }
        networkChanged.notify_all();
        auto separateOriginals = queue.Wait(2);
        Check(separateOriginals.size() == 2 && separateOriginals[0].asset && separateOriginals[1].asset &&
            separateOriginals[0].asset->source == "original" && separateOriginals[1].asset->source == "original" &&
            separateOriginals[0].asset->reference != separateOriginals[1].asset->reference,
            "shared missing Steam artwork falls back to distinct original Shell identities per desktop item");
    }
    if (root.parent_path() == fs::temp_directory_path() && root.filename().wstring().starts_with(L"SnowDesktop-large-icons-shell-"))
        fs::remove_all(root);
    if (SUCCEEDED(initialized)) CoUninitialize();
    return failures;
}

// Opt-in, read-only source evidence; never starts or operates the desktop host.
int RunLargeIconSourceProbe(const char* shortcutPath, const char* outputDirectory)
{
    namespace fs = std::filesystem;
    using namespace snowdesktop;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto root = fs::absolute(outputDirectory);
    fs::create_directories(root);
    const auto source = fs::absolute(shortcutPath);
    bool succeeded = false;
    {
        Queue queue(root / L"managed");
        LargeIconAssetRequest request;
        request.itemKey = L"source-probe"; request.parsingName = source.wstring();
        request.generation = 1; request.pixels = 256; request.refresh = true;
        queue.assets.Request(request);
        auto result = queue.Wait(1);
        if (!result.empty() && result[0].asset)
        {
            const auto& asset = result[0].asset;
            fs::copy_file(root / L"managed" / fs::path(asset->previewReference), root / L"large-icon.png", fs::copy_options::overwrite_existing);
            std::cout << "Source loaded: " << asset->source << ", " << asset->width << "x" << asset->height << '\n';
            succeeded = true;
        }
        // Capture the old ImageFactory fallback for a direct comparison.
        Microsoft::WRL::ComPtr<IShellItemImageFactory> shell;
        HBITMAP bitmap = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(source.c_str(), nullptr, IID_PPV_ARGS(&shell))) &&
            SUCCEEDED(shell->GetImage({256, 256}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bitmap)) && bitmap)
        {
            Microsoft::WRL::ComPtr<IWICImagingFactory> imaging;
            Microsoft::WRL::ComPtr<IWICBitmap> image;
            Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
            UINT w = 0, h = 0;
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&imaging))) &&
                SUCCEEDED(imaging->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUseAlpha, &image)) &&
                SUCCEEDED(image->GetSize(&w, &h)) && SUCCEEDED(imaging->CreateFormatConverter(&converter)) &&
                SUCCEEDED(converter->Initialize(image.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
            {
                std::vector<std::uint32_t> pixels(static_cast<size_t>(w) * h);
                std::string error;
                if (SUCCEEDED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size() * 4), reinterpret_cast<BYTE*>(pixels.data()))))
                    preview_png::Save(root / L"shell-fallback.png", w, h, pixels, error);
            }
            DeleteObject(bitmap);
        }
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return succeeded ? 0 : 1;
}

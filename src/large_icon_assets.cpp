#include "large_icon_assets.h"
#include "large_icon_config.h"
#include "large_icon_steam.h"
#include "http_runtime.h"
#include "preview_png_writer.h"
#include "icon_beautify.h"
#include "atomic_file.h"

#include <shobjidl.h>
#include <shlobj.h>
#include <cwctype>
#include <cstring>
#include <wincodec.h>
#include <wrl/client.h>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace snowdesktop
{
using Microsoft::WRL::ComPtr;
namespace
{
constexpr std::uint64_t maxFile = 16ull * 1024 * 1024;
constexpr std::uint64_t maxPixels = 16000000;
constexpr std::uint64_t maxMemory = 128ull * 1024 * 1024;
std::wstring Wide(std::string_view value) { return {value.begin(), value.end()}; }
std::string Hash(std::wstring_view value)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const wchar_t ch : value) { hash ^= static_cast<std::uint16_t>(ch); hash *= 1099511628211ull; }
    std::ostringstream out; out << std::hex << hash; return out.str();
}
std::int64_t Now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::filesystem::path SteamDirectory()
{
    wchar_t buffer[32768]{};
    DWORD bytes = sizeof(buffer);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buffer, &bytes) == ERROR_SUCCESS)
        return buffer;
    bytes = sizeof(buffer);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Valve\\Steam", L"InstallPath", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6432KEY, nullptr, buffer, &bytes) == ERROR_SUCCESS)
        return buffer;
    return {};
}
std::vector<std::filesystem::path> LocalCovers(const std::filesystem::path& configuredSteamDirectory,
    std::uint32_t appId, bool portrait, std::stop_token stop)
{
    std::vector<std::filesystem::path> result;
    const auto steam = configuredSteamDirectory.empty() ? SteamDirectory() : configuredSteamDirectory;
    if (steam.empty()) return result;
    const auto root = steam / L"appcache" / L"librarycache";
    const std::wstring id = std::to_wstring(appId);
    std::error_code ec;
    const auto match = [&](const std::filesystem::path& path) {
        auto filename = path.filename().wstring();
        std::transform(filename.begin(), filename.end(), filename.begin(), towlower);
        return portrait ? filename.find(L"library_600x900") != filename.npos :
            (filename.find(L"library_header") != filename.npos || filename.find(L"header") != filename.npos || filename.find(L"capsule_616x353") != filename.npos);
    };
    // Legacy flat names and current AppID/hash subdirectories. Do not walk
    // other games or userdata/account-specific custom artwork directories.
    for (std::filesystem::directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
         it != end && !ec && !stop.stop_requested(); it.increment(ec))
        if (it->is_regular_file(ec) && it->path().filename().wstring().starts_with(id + L"_") && match(it->path()))
            result.push_back(it->path());
    ec.clear();
    size_t visited = 0;
    for (std::filesystem::recursive_directory_iterator it(root / id, std::filesystem::directory_options::skip_permission_denied, ec), end;
         it != end && !ec && !stop.stop_requested() && visited++ < 2048; it.increment(ec))
    {
        if (it.depth() > 3 || it->is_symlink(ec)) { it.disable_recursion_pending(); continue; }
        if (it->is_regular_file(ec) && match(it->path())) result.push_back(it->path());
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        std::error_code ea, eb;
        return std::filesystem::last_write_time(a, ea) > std::filesystem::last_write_time(b, eb);
    });
    return result;
}

std::uint32_t Accent(const std::vector<std::uint32_t>& pixels, int width, int height)
{
    if (const auto edge = icon_beautify::DetectEdgeFill(pixels, width, height))
        return (std::clamp(edge->r, 40, 215) << 16) | (std::clamp(edge->g, 40, 215) << 8) | std::clamp(edge->b, 40, 215);
    std::array<std::uint64_t, 512> weights{};
    std::array<std::array<std::uint64_t, 3>, 512> sums{};
    for (const auto pixel : pixels)
    {
        const auto a = pixel >> 24;
        if (a < 32) continue;
        const unsigned r = std::min(255u, ((pixel >> 16) & 255) * 255 / a);
        const unsigned g = std::min(255u, ((pixel >> 8) & 255) * 255 / a);
        const unsigned b = std::min(255u, (pixel & 255) * 255 / a);
        const auto index = ((r >> 5) << 6) | ((g >> 5) << 3) | (b >> 5);
        weights[index] += a; sums[index][0] += r * a; sums[index][1] += g * a; sums[index][2] += b * a;
    }
    const size_t i = std::max_element(weights.begin(), weights.end()) - weights.begin();
    if (weights[i] == 0) return 0x505866;
    // Mixing each channel toward neutral bounds saturation as well as luma.
    auto channel = [&](int n) { return std::clamp<int>(static_cast<int>(sums[i][n] / weights[i]) * 3 / 4 + 32, 40, 215); };
    return (channel(0) << 16) | (channel(1) << 8) | channel(2);
}

std::shared_ptr<LargeIconAsset> Decode(const std::filesystem::path& path, int target, const std::filesystem::path& output,
    std::string reference, std::string source)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > maxFile) return {};
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return {};
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))) return {};
    GUID container{};
    if (FAILED(decoder->GetContainerFormat(&container)) ||
        (container != GUID_ContainerFormatPng && container != GUID_ContainerFormatJpeg &&
         container != GUID_ContainerFormatBmp && container != GUID_ContainerFormatIco)) return {};
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return {};
    if (container == GUID_ContainerFormatIco)
    {
        UINT count = 0, bestEdge = 0;
        decoder->GetFrameCount(&count);
        for (UINT i = 0; i < std::min(count, 256u); ++i)
        {
            ComPtr<IWICBitmapFrameDecode> candidate;
            UINT w = 0, h = 0;
            if (SUCCEEDED(decoder->GetFrame(i, &candidate)) && SUCCEEDED(candidate->GetSize(&w, &h)) &&
                std::uint64_t(w) * h <= maxPixels && std::max(w, h) > bestEdge)
            { frame = candidate; bestEdge = std::max(w, h); }
        }
    }
    UINT width = 0, height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0 || std::uint64_t(width) * height > maxPixels) return {};
    const double ratio = std::min(1., static_cast<double>(std::clamp(target, 64, 2048)) / std::max(width, height));
    width = std::max(1u, static_cast<UINT>(width * ratio)); height = std::max(1u, static_cast<UINT>(height * ratio));
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeFant))) return {};
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) || FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return {};
    std::vector<std::uint32_t> pixels(static_cast<size_t>(width) * height);
    if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size() * 4), reinterpret_cast<BYTE*>(pixels.data())))) return {};
    if (!output.empty() && path != output)
    {
        // Keep the validated source bytes. A later, larger display must decode
        // from this original rather than repeatedly enlarge a thumbnail.
        std::ifstream input(path, std::ios::binary);
        std::string original(static_cast<size_t>(maxFile) + 1, '\0');
        input.read(original.data(), static_cast<std::streamsize>(original.size()));
        original.resize(static_cast<size_t>(input.gcount()));
        if (input.bad() || original.empty() || original.size() > maxFile ||
            !atomic_file::WriteAll(output, original)) return {};
    }
    const auto managedPath = output.empty() ? path : output;
    const auto stamp = std::filesystem::last_write_time(managedPath, ec).time_since_epoch().count();
    const std::string preview = "preview-" + Hash(Wide(reference) + std::to_wstring(stamp)) + "-" + std::to_string(target) + ".png";
    std::string previewError;
    if (!preview_png::Save(managedPath.parent_path() / Wide(preview), width, height, pixels, previewError)) return {};
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -static_cast<LONG>(height); info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    auto asset = std::make_shared<LargeIconAsset>();
    asset->bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!asset->bitmap || !bits) return {};
    memcpy(bits, pixels.data(), pixels.size() * 4);
    asset->width = width; asset->height = height;
    asset->reference = std::move(reference); asset->source = std::move(source);
    asset->previewReference = preview;
    asset->accent = Accent(pixels, width, height);
    return asset;
}

std::shared_ptr<LargeIconAsset> RawIcon(const LargeIconAssetRequest& request, const std::filesystem::path& output, const std::string& reference)
{
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(request.parsingName.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return {};
    HBITMAP bitmap = nullptr;
    const int target = std::clamp(request.pixels, 64, 256);
    if (FAILED(factory->GetImage({target, target}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bitmap)) || !bitmap) return {};
    BITMAP object{}; GetObjectW(bitmap, sizeof(object), &object);
    const int width = object.bmWidth, height = std::abs(object.bmHeight);
    if (width <= 0 || height <= 0 || std::uint64_t(width) * height > maxPixels) { DeleteObject(bitmap); return {}; }
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    std::vector<std::uint32_t> pixels(static_cast<size_t>(width) * height);
    HDC dc = GetDC(nullptr);
    const bool read = dc && GetDIBits(dc, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS);
    if (dc) ReleaseDC(nullptr, dc);
    DeleteObject(bitmap);
    if (!read) return {};
    if (std::none_of(pixels.begin(), pixels.end(), [](auto p) { return (p >> 24) != 0; }))
        for (auto& p : pixels) p |= 0xff000000;
    std::string error;
    if (!preview_png::Save(output, width, height, pixels, error)) return {};
    return Decode(output, target, {}, reference, "original");
}

int Download(std::string_view url, std::stop_token stop, std::vector<std::byte>& bytes, std::uint64_t limit)
{
    http_stream::Options options; options.url = Wide(url); options.maximumResponseBytes = limit;
    options.totalTimeoutMs = 20000; options.timeoutMs = 8000;
    const auto result = http_stream::StreamHttpGet(options, stop,
        [](const auto& head) { return head.status == 200 && head.finalUrl.starts_with(L"https://") &&
            http_security::IsAllowedUrlForDomains(head.finalUrl, {"api.steampowered.com", "shared.akamai.steamstatic.com", "shared.steamstatic.com"}); },
        [&](auto chunk) { bytes.insert(bytes.end(), chunk.begin(), chunk.end()); return true; });
    return result.responseAccepted && result.error.empty() && !result.cancelled ? 200 : (result.head.status == 200 ? 0 : result.head.status);
}
}

struct LargeIconAssets::Impl
{
    struct Work { LargeIconAssetRequest request; std::vector<LargeIconAssetRequest> listeners; std::stop_source cancellation; };
    struct Cached { std::shared_ptr<LargeIconAsset> asset; std::uint64_t touched = 0; };
    std::filesystem::path directory;
    std::filesystem::path steamDirectory;
    std::function<void()> ready;
    std::mutex mutex;
    std::condition_variable_any condition;
    std::deque<std::string> queue;
    std::unordered_map<std::string, Work> pending;
    std::unordered_map<std::wstring, std::uint64_t> currentRequests;
    std::unordered_map<std::string, Cached> cache;
    std::vector<std::weak_ptr<LargeIconAsset>> allocations;
    std::unordered_set<std::string> retained;
    std::vector<LargeIconAssetResult> completed;
    std::array<std::jthread, 2> workers;
    std::array<std::mutex, 64> sourceMutexes;
    std::uint64_t clock = 0;
    bool stopped = false;
    static std::wstring ListenerKey(const LargeIconAssetRequest& r)
    { return r.itemKey + L"\n" + std::to_wstring(r.variant); }

    void CollectDisk()
    {
        // Called under mutex after a job completes. Only service-owned cache
        // names are candidates; user imports and all live references are kept.
        auto pinned = retained;
        for (const auto& [_, c] : cache)
            if (c.asset.use_count() > 1)
            { pinned.insert(c.asset->reference); pinned.insert(c.asset->previewReference); }
        for (const auto& [_, work] : pending) pinned.insert(Reference(work.request));
        struct File { std::filesystem::path path; std::uint64_t bytes; std::filesystem::file_time_type touched; };
        std::vector<File> candidates;
        std::uint64_t total = 0;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(directory, ec), end; it != end && !ec; it.increment(ec))
        {
            if (!it->is_regular_file(ec)) continue;
            const auto name = it->path().filename().string();
            if (!(name.starts_with("steam-") || name.starts_with("raw-") || name.starts_with("preview-")) ||
                it->path().extension() != L".png") continue;
            const auto bytes = it->file_size(ec);
            if (ec) break;
            total += bytes;
            if (!pinned.contains(name)) candidates.push_back({it->path(), bytes, it->last_write_time(ec)});
        }
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.touched < b.touched; });
        constexpr std::uint64_t quota = 512ull * 1024 * 1024;
        for (const auto& file : candidates)
        {
            if (total <= quota) break;
            if (std::filesystem::remove(file.path, ec)) total -= file.bytes;
        }
    }

    std::string Reference(const LargeIconAssetRequest& r) const
    {
        if (!r.importPath.empty()) return r.reference;
        if (r.content == 1 && IsManagedLargeIconImage(r.reference) && !r.reference.empty()) return r.reference;
        if (r.content == 2 && r.appId) return "steam-" + std::to_string(r.appId) + (r.portrait ? "-portrait-" : "-landscape-") + r.language + ".png";
        std::error_code ec;
        const auto time = std::filesystem::last_write_time(r.parsingName, ec);
        return "raw-" + Hash(r.parsingName + (ec ? L"" : std::to_wstring(time.time_since_epoch().count()))) + "-" + std::to_string(r.pixels) + ".png";
    }
    std::shared_ptr<LargeIconAsset> Load(const LargeIconAssetRequest& r, std::stop_token stop, std::string& error)
    {
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec) { error = "largeIcon.saveFailed"; return {}; }
        const auto reference = Reference(r);
        std::lock_guard sourceLock(sourceMutexes[std::hash<std::string>{}(reference) % sourceMutexes.size()]);
        const auto output = directory / Wide(reference);
        if (!r.refresh)
            if (auto cached = Decode(output, r.pixels, {}, reference, "cache")) return cached;
        if (!r.importPath.empty())
        {
            auto result = Decode(r.importPath, r.pixels, output, reference, "local");
            if (!result) error = "largeIcon.invalid";
            return result;
        }
        if (r.content == 0) return RawIcon(r, output, reference);
        if (r.content == 1) { error = "largeIcon.unavailable"; return {}; }
        if (r.appId)
        {
            for (const auto& local : LocalCovers(steamDirectory, r.appId, r.portrait, stop))
                if (auto asset = Decode(local, r.pixels, output, reference, "steam-local")) return asset;
            const auto failure = directory / Wide(reference + ".failure");
            std::int64_t retryAt = 0;
            std::ifstream record(failure); record >> retryAt; record.close();
            if (!r.localOnly && !stop.stop_requested() && (r.refresh || Now() >= retryAt))
            {
                int status = 0;
                for (const auto& language : {r.language, std::string("english")})
                {
                    std::vector<std::byte> bytes;
                    status = Download(large_icon_steam::MetadataUrl(r.appId, language), stop, bytes, 1024 * 1024);
                    JsonValue metadata;
                    std::string url;
                    if (status == 200 && ParseJson(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), metadata))
                        url = large_icon_steam::AssetUrl(metadata, r.appId, r.portrait);
                    if (!url.empty())
                    {
                        bytes.clear(); status = Download(url, stop, bytes, maxFile);
                        if (status == 200)
                        {
                            const auto temporary = directory / Wide(reference + "-" + std::to_string(r.generation) + ".download");
                            if (!atomic_file::WriteAll(temporary, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())))
                            { error = "largeIcon.saveFailed"; return {}; }
                            auto asset = Decode(temporary, r.pixels, output, reference, "steam-online");
                            std::filesystem::remove(temporary, ec);
                            if (asset) { std::filesystem::remove(failure, ec); return asset; }
                        }
                    }
                    if (stop.stop_requested() || r.language == "english") break;
                }
                std::ofstream failed(failure, std::ios::trunc); failed << Now() + (status == 404 ? 86400 : 120);
            }
        }
        // Refresh failures never remove the last useful cover, including a
        // previously chosen orientation. Offline loads require no entitlement.
        error = "largeIcon.unavailable";
        if (auto old = Decode(output, r.pixels, {}, reference, "cache")) return old;
        if (IsManagedLargeIconImage(r.lastGood) && !r.lastGood.empty())
            if (auto old = Decode(directory / Wide(r.lastGood), r.pixels, {}, r.lastGood, "cache")) return old;
        error = "largeIcon.unavailable";
        return {};
    }
    void Run(std::stop_token stop)
    {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (!stop.stop_requested())
        {
            std::string key; LargeIconAssetRequest request; std::stop_token requestStop;
            {
                std::unique_lock lock(mutex);
                if (!condition.wait(lock, stop, [&] { return stopped || !queue.empty(); }) || stopped) break;
                key = std::move(queue.front()); queue.pop_front(); request = pending.at(key).request;
                requestStop = pending.at(key).cancellation.get_token();
            }
            std::string error;
            std::shared_ptr<LargeIconAsset> asset;
            try { if (!requestStop.stop_requested()) asset = Load(request, requestStop, error); }
            catch (...) { error = "largeIcon.unavailable"; }
            {
                std::lock_guard lock(mutex);
                if (stopped) break;
                // A user can switch back to a source while its previous job is
                // being cancelled. Restart that key for current listeners;
                // never attach them permanently to an already-stopped token.
                if (requestStop.stop_requested())
                {
                    auto& work = pending.at(key);
                    std::erase_if(work.listeners, [&](const auto& listener) {
                        const auto current = currentRequests.find(ListenerKey(listener));
                        return current == currentRequests.end() || current->second != listener.generation;
                    });
                    if (!work.listeners.empty())
                    {
                        work.request = work.listeners.back();
                        work.cancellation = std::stop_source{};
                        queue.push_back(key); condition.notify_one();
                        continue;
                    }
                }
                if (asset)
                {
                    auto bytes = [&] {
                        std::erase_if(allocations, [](const auto& allocation) { return allocation.expired(); });
                        std::uint64_t size = 0;
                        for (const auto& allocation : allocations) if (const auto live = allocation.lock())
                            size += std::uint64_t(live->width) * live->height * 4;
                        return size;
                    };
                    const auto incoming = std::uint64_t(asset->width) * asset->height * 4;
                    while (bytes() + incoming > maxMemory)
                    {
                        auto oldest = cache.end();
                        for (auto it = cache.begin(); it != cache.end(); ++it)
                            if (it->second.asset.use_count() == 1 && (oldest == cache.end() || it->second.touched < oldest->second.touched)) oldest = it;
                        if (oldest == cache.end()) break;
                        cache.erase(oldest);
                    }
                    if (bytes() + incoming <= maxMemory)
                    { allocations.push_back(asset); if (error.empty()) cache[key] = {asset, ++clock}; }
                    else { asset.reset(); error = "largeIcon.unavailable"; }
                }
                for (auto& listener : pending.at(key).listeners) completed.push_back({std::move(listener), asset, error});
                pending.erase(key);
                CollectDisk();
            }
            if (ready) ready();
        }
        if (SUCCEEDED(initialized)) CoUninitialize();
    }
};

LargeIconAssets::LargeIconAssets(std::filesystem::path directory, std::function<void()> ready,
    std::filesystem::path steamDirectory) : impl_(std::make_unique<Impl>())
{
    impl_->directory = std::move(directory); impl_->ready = std::move(ready);
    impl_->steamDirectory = std::move(steamDirectory);
    for (auto& worker : impl_->workers) worker = std::jthread([this](auto stop) { impl_->Run(stop); });
}
LargeIconAssets::~LargeIconAssets() { Stop(); }
void LargeIconAssets::Stop()
{
    {
        std::lock_guard lock(impl_->mutex); impl_->stopped = true;
        for (auto& [_, work] : impl_->pending) work.cancellation.request_stop();
    }
    for (auto& worker : impl_->workers) worker.request_stop();
    impl_->condition.notify_all();
    for (auto& worker : impl_->workers) if (worker.joinable()) worker.join();
}
void LargeIconAssets::Request(LargeIconAssetRequest request)
{
    if (!request.importPath.empty()) request.reference = "import-" + Hash(request.importPath.wstring() +
        std::to_wstring(Now()) + std::to_wstring(request.generation)) + ".png";
    bool notify = false;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopped) return;
        impl_->currentRequests[Impl::ListenerKey(request)] = request.generation;
        const std::string key = impl_->Reference(request) + ":" + std::to_string(request.pixels) + (request.localOnly ? ":local" : ":online");
        auto cached = impl_->cache.find(key);
        if (!request.refresh && cached != impl_->cache.end())
        {
            cached->second.touched = ++impl_->clock;
            impl_->completed.push_back({std::move(request), cached->second.asset, {}}); notify = true;
        }
        else
        {
            const auto [it, inserted] = impl_->pending.try_emplace(key, Impl::Work{request, {}, {}});
            it->second.listeners.push_back(std::move(request));
            if (inserted) { impl_->queue.push_back(key); impl_->condition.notify_one(); }
        }
        for (auto& [_, work] : impl_->pending)
            if (std::none_of(work.listeners.begin(), work.listeners.end(), [&](const auto& listener) {
                const auto current = impl_->currentRequests.find(Impl::ListenerKey(listener));
                return current != impl_->currentRequests.end() && current->second == listener.generation;
            })) work.cancellation.request_stop();
    }
    if (notify && impl_->ready) impl_->ready();
}
std::vector<LargeIconAssetResult> LargeIconAssets::TakeCompleted()
{
    std::lock_guard lock(impl_->mutex);
    return std::exchange(impl_->completed, {});
}
void LargeIconAssets::Cancel(const std::wstring& itemKey)
{
    std::lock_guard lock(impl_->mutex);
    for (int variant = 0; variant <= 2; ++variant) impl_->currentRequests.erase(itemKey + L"\n" + std::to_wstring(variant));
    for (auto& [_, work] : impl_->pending)
        if (std::none_of(work.listeners.begin(), work.listeners.end(), [&](const auto& listener) {
            const auto it = impl_->currentRequests.find(Impl::ListenerKey(listener));
            return it != impl_->currentRequests.end() && it->second == listener.generation;
        })) work.cancellation.request_stop();
}
void LargeIconAssets::RetainReferences(std::vector<std::string> references)
{
    std::lock_guard lock(impl_->mutex);
    impl_->retained = {references.begin(), references.end()};
}
}

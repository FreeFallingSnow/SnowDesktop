// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <winhttp.h>
#include <wincodec.h>

#include "preview_cache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <functional>
#include <system_error>
#include <utility>
#include <vector>

namespace snowdesktop::steam_bridge
{
namespace
{
constexpr std::uint64_t kMaximumDownloadBytes = 4ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaximumCacheBytes = 64ull * 1024ull * 1024ull;

std::filesystem::path DefaultRoot()
{
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,
            KF_FLAG_DEFAULT, nullptr, &value)))
        return {};
    const auto result = std::filesystem::path(value) / L"SnowDesktop" /
        L"SteamWorkshopManager" / L"preview-cache";
    CoTaskMemFree(value);
    return result;
}

std::wstring Utf8ToWide(std::string_view value)
{
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), length) != length)
        return {};
    return result;
}

bool IsReparsePoint(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

std::uint64_t UrlHash(std::string_view value)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char byte : value)
    {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

struct WinHttpHandle
{
    HINTERNET value = nullptr;
    ~WinHttpHandle() { if (value) WinHttpCloseHandle(value); }
};

bool DownloadHttps(std::string_view rawUrl, const std::filesystem::path& path,
    std::string& error)
{
    const std::wstring url = Utf8ToWide(rawUrl);
    if (url.empty())
    {
        error = "preview URL is not valid UTF-8";
        return false;
    }
    URL_COMPONENTSW parts{ sizeof(parts) };
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0,
            &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
    {
        error = "only HTTPS Workshop previews are allowed";
        return false;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring resource(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength)
        resource.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    WinHttpHandle session{ WinHttpOpen(L"SnowDesktopWorkshopManager/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0) };
    if (!session.value)
    {
        error = "cannot initialize WinHTTP";
        return false;
    }
    WinHttpSetTimeouts(session.value, 5000, 5000, 10000, 10000);
    WinHttpHandle connection{ WinHttpConnect(session.value, host.c_str(),
        parts.nPort, 0) };
    if (!connection.value)
    {
        error = "cannot connect to the Workshop preview host";
        return false;
    }
    WinHttpHandle request{ WinHttpOpenRequest(connection.value, L"GET",
        resource.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) };
    if (!request.value ||
        !WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr))
    {
        error = "Workshop preview request failed";
        return false;
    }
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.value,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
            WINHTTP_NO_HEADER_INDEX) || status != 200)
    {
        error = "Workshop preview server returned HTTP " +
            std::to_string(status);
        return false;
    }
    std::vector<std::byte> bytes;
    while (true)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.value, &available))
        {
            error = "cannot read Workshop preview response";
            return false;
        }
        if (available == 0) break;
        if (bytes.size() + available > kMaximumDownloadBytes)
        {
            error = "Workshop preview exceeds the 4 MiB cache limit";
            return false;
        }
        const std::size_t offset = bytes.size();
        bytes.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.value, bytes.data() + offset,
                available, &read))
        {
            error = "cannot read Workshop preview data";
            return false;
        }
        bytes.resize(offset + read);
    }
    if (bytes.empty())
    {
        error = "Workshop preview response is empty";
        return false;
    }
    const auto temporary = path.wstring() + L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output || !output.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size())) || !output.flush())
        {
            error = "cannot write the preview cache";
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temporary.c_str());
        error = "cannot atomically update the preview cache";
        return false;
    }
    return true;
}

void TrimCache(const std::filesystem::path& root,
    const std::filesystem::path& keep)
{
    struct File { std::filesystem::path path; std::uint64_t size;
        std::filesystem::file_time_type time; };
    std::vector<File> files;
    std::uint64_t total = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator iterator(root,
             std::filesystem::directory_options::skip_permission_denied, ec),
         end; iterator != end; iterator.increment(ec))
    {
        if (ec || !iterator->is_regular_file(ec) || ec ||
            IsReparsePoint(iterator->path()) ||
            iterator->path().extension() != L".preview")
            continue;
        const auto size = iterator->file_size(ec);
        if (ec) continue;
        files.push_back({ iterator->path(), size,
            iterator->last_write_time(ec) });
        total += size;
    }
    std::sort(files.begin(), files.end(), [](const File& left,
        const File& right) { return left.time < right.time; });
    for (const auto& file : files)
    {
        if (total <= kMaximumCacheBytes) break;
        if (file.path == keep) continue;
        if (std::filesystem::remove(file.path, ec)) total -= file.size;
        ec.clear();
    }
}

bool LoadTexture(ID3D11Device* device, const std::filesystem::path& path,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& view,
    int& width, int& height, std::string& error)
{
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
    {
        error = "WIC is unavailable";
        return false;
    }
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr,
            GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
    {
        error = "cached preview is not a supported image";
        return false;
    }
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    UINT imageWidth = 0;
    UINT imageHeight = 0;
    if (FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(frame->GetSize(&imageWidth, &imageHeight)) || imageWidth == 0 ||
        imageHeight == 0 || imageWidth > 8192 || imageHeight > 8192 ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom)))
    {
        error = "cannot decode the cached preview";
        return false;
    }
    const UINT stride = imageWidth * 4;
    std::vector<std::byte> pixels(static_cast<std::size_t>(stride) * imageHeight);
    if (FAILED(converter->CopyPixels(nullptr, stride,
            static_cast<UINT>(pixels.size()),
            reinterpret_cast<BYTE*>(pixels.data()))))
    {
        error = "cannot read preview pixels";
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = imageWidth;
    description.Height = imageHeight;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{ pixels.data(), stride, 0 };
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&description, &data, &texture)) ||
        FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &view)))
    {
        error = "cannot create the D3D11 preview texture";
        return false;
    }
    width = static_cast<int>(imageWidth);
    height = static_cast<int>(imageHeight);
    return true;
}
}

struct PreviewCache::Entry
{
    enum class State { Loading, Ready, Loaded, Failed };
    std::string url;
    std::filesystem::path path;
    std::atomic<State> state = State::Loading;
    std::string error;
    int width = 0;
    int height = 0;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    std::jthread worker;
};

PreviewCache::PreviewCache(std::filesystem::path root)
    : root_(root.empty() ? DefaultRoot() : std::move(root))
{
}

PreviewCache::~PreviewCache() = default;

void PreviewCache::Request(std::uint64_t itemId, std::string url)
{
    if (itemId == 0 || url.empty()) return;
    std::lock_guard lock(mutex_);
    if (const auto found = entries_.find(itemId); found != entries_.end() &&
        found->second->url == url)
        return;
    auto entry = std::make_unique<Entry>();
    entry->url = std::move(url);
    entry->path = root_ / (std::to_wstring(itemId) + L"-" +
        std::to_wstring(UrlHash(entry->url)) + L".preview");
    Entry* raw = entry.get();
    entries_[itemId] = std::move(entry);
    raw->worker = std::jthread([this, raw](std::stop_token)
    {
        std::error_code ec;
        std::filesystem::create_directories(root_, ec);
        if (ec || IsReparsePoint(root_))
        {
            raw->error = "preview cache directory is unavailable";
            raw->state.store(Entry::State::Failed);
            return;
        }
        if (!std::filesystem::is_regular_file(raw->path, ec) || ec)
        {
            ec.clear();
            if (!DownloadHttps(raw->url, raw->path, raw->error))
            {
                raw->state.store(Entry::State::Failed);
                return;
            }
            TrimCache(root_, raw->path);
        }
        raw->state.store(Entry::State::Ready);
    });
}

void PreviewCache::RequestLocal(std::uint64_t key,
    const std::filesystem::path& path)
{
    if (key == 0 || path.empty()) return;
    const std::string identity = "local:" + std::to_string(
        std::hash<std::wstring>{}(path.wstring()));
    std::lock_guard lock(mutex_);
    if (const auto found = entries_.find(key); found != entries_.end() &&
        found->second->url == identity)
        return;
    auto entry = std::make_unique<Entry>();
    entry->url = identity;
    entry->path = path;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec ||
        IsReparsePoint(path))
    {
        entry->error = "local primary preview is missing or unsafe";
        entry->state.store(Entry::State::Failed);
    }
    else entry->state.store(Entry::State::Ready);
    entries_[key] = std::move(entry);
}

void PreviewCache::Pump(ID3D11Device* device)
{
    if (!device) return;
    std::lock_guard lock(mutex_);
    for (auto& [id, entry] : entries_)
    {
        (void)id;
        if (entry->state.load() != Entry::State::Ready) continue;
        if (LoadTexture(device, entry->path, entry->view, entry->width,
                entry->height, entry->error))
            entry->state.store(Entry::State::Loaded);
        else entry->state.store(Entry::State::Failed);
    }
}

PreviewTexture PreviewCache::Get(std::uint64_t itemId) const
{
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(itemId);
    if (found == entries_.end()) return {};
    const auto state = found->second->state.load();
    PreviewTexture result;
    result.view = found->second->view.Get();
    result.width = found->second->width;
    result.height = found->second->height;
    result.loading = state == Entry::State::Loading ||
        state == Entry::State::Ready;
    result.error = found->second->error;
    return result;
}
}

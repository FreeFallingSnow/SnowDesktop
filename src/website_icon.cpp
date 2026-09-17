#include "website_icon.h"
#include "atomic_file.h"
#include "http_runtime.h"
#include "shortcut_application_rules.h"

#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <map>

namespace snowdesktop::website_icon
{
namespace
{
using Microsoft::WRL::ComPtr;
namespace rules = shortcut_application_rules;
constexpr size_t maxShortcutBytes = 1024 * 1024;
constexpr size_t maxImageBytes = 4 * 1024 * 1024;

struct Handle
{
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct TemporaryFile
{
    std::filesystem::path path;
    ~TemporaryFile() { if (!path.empty()) DeleteFileW(path.c_str()); }
};

std::wstring UniqueName()
{
    GUID id{};
    wchar_t value[40]{};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, value, 40)) return {};
    return value;
}

std::wstring Wide(std::string_view text)
{
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!count) return {};
    std::wstring result(count, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), count);
    return result;
}

std::wstring HttpUrl(std::wstring_view value)
{
    const std::wstring url(rules::Trim(value));
    return url.size() <= 8192 && http_security::IsAllowedHttpOrHttpsUrl(url) ? url : L"";
}

bool LoadLink(const std::filesystem::path& path, ComPtr<IShellLinkW>& link, ComPtr<IPersistFile>& file)
{
    return SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&link))) && SUCCEEDED(link.As(&file)) &&
        SUCCEEDED(file->Load(path.c_str(), STGM_READ));
}

bool ReadContents(const std::filesystem::path& path, std::string& contents,
    BY_HANDLE_FILE_INFORMATION& identity)
{
    Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.value, &identity) ||
        (identity.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        identity.nFileSizeHigh || !identity.nFileSizeLow || identity.nFileSizeLow > maxShortcutBytes) return false;
    contents.resize(identity.nFileSizeLow);
    DWORD count = 0;
    return ReadFile(file.value, contents.data(), identity.nFileSizeLow, &count, nullptr) && count == contents.size();
}

bool SameFile(const Shortcut& expected)
{
    std::string contents;
    BY_HANDLE_FILE_INFORMATION identity{};
    return ReadContents(expected.path, contents, identity) &&
        identity.dwVolumeSerialNumber == expected.identity.dwVolumeSerialNumber &&
        identity.nFileIndexHigh == expected.identity.nFileIndexHigh &&
        identity.nFileIndexLow == expected.identity.nFileIndexLow && contents == expected.contents &&
        CompareFileTime(&identity.ftLastWriteTime, &expected.identity.ftLastWriteTime) == 0 &&
        !(identity.dwFileAttributes & FILE_ATTRIBUTE_READONLY);
}

bool Space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f'; }
std::string Lower(std::string_view text)
{
    std::string result(text);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return result;
}
bool Token(std::string_view text, std::string_view token)
{
    size_t p = 0;
    while (p < text.size())
    {
        while (p < text.size() && Space(text[p])) ++p;
        const size_t start = p;
        while (p < text.size() && !Space(text[p])) ++p;
        if (Lower(text.substr(start, p - start)) == token) return true;
    }
    return false;
}

std::wstring AttributeUrl(std::string_view value)
{
    std::wstring result = Wide(value);
    for (size_t p = 0; (p = result.find(L'&', p)) != result.npos; ++p)
    {
        const size_t end = result.find(L';', p + 1);
        if (end == result.npos || end - p > 12) continue;
        const auto entity = result.substr(p + 1, end - p - 1);
        wchar_t decoded = 0;
        if (entity == L"amp") decoded = L'&';
        else if (entity == L"quot") decoded = L'"';
        else if (entity == L"apos") decoded = L'\'';
        else if (entity == L"lt") decoded = L'<';
        else if (entity == L"gt") decoded = L'>';
        else if (entity.starts_with(L"#"))
        {
            const bool hex = entity.size() > 2 && (entity[1] == L'x' || entity[1] == L'X');
            const auto number = entity.substr(hex ? 2 : 1);
            wchar_t* tail = nullptr;
            const auto parsed = wcstoul(number.c_str(), &tail, hex ? 16 : 10);
            if (!number.empty() && tail && !*tail && parsed > 0 && parsed < 0xd800) decoded = static_cast<wchar_t>(parsed);
        }
        if (decoded) result.replace(p, end - p + 1, 1, decoded);
    }
    return result;
}

std::wstring Combine(const std::wstring& base, const std::wstring& relative)
{
    if (relative.empty()) return {};
    wchar_t value[8193]{};
    DWORD size = static_cast<DWORD>(std::size(value));
    if (FAILED(UrlCombineW(base.c_str(), relative.c_str(), value, &size, 0))) return {};
    std::wstring result(value);
    if (const auto fragment = result.find(L'#'); fragment != result.npos) result.resize(fragment);
    return HttpUrl(result);
}

void AppendLe(std::string& bytes, std::uint32_t value, int count)
{
    for (int i = 0; i < count; ++i) bytes.push_back(static_cast<char>(value >> (i * 8)));
}
}

std::wstring ReadUrl(const std::filesystem::path& path)
{
    const auto extension = path.extension().wstring();
    if (!rules::EqualsIgnoreCase(extension, L".url") && !rules::EqualsIgnoreCase(extension, L".lnk")) return {};
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return {};
    wchar_t target[32768]{};
    if (rules::EqualsIgnoreCase(extension, L".url"))
    {
        GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"", target,
            static_cast<DWORD>(std::size(target)), path.c_str());
        return HttpUrl(target);
    }
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> file;
    if (!LoadLink(path, link, file)) return {};
    link->GetPath(target, static_cast<int>(std::size(target)), nullptr, SLGP_RAWPATH);
    if (auto url = HttpUrl(target); !url.empty()) return url;
    const auto executable = std::filesystem::path(target).filename().wstring();
    // A URL argument to an arbitrary program need not be a website shortcut.
    constexpr std::array browsers{L"chrome.exe", L"msedge.exe", L"firefox.exe", L"brave.exe", L"vivaldi.exe", L"opera.exe"};
    if (std::none_of(browsers.begin(), browsers.end(), [&](auto name) { return rules::EqualsIgnoreCase(executable, name); })) return {};
    wchar_t arguments[32768]{};
    if (FAILED(link->GetArguments(arguments, static_cast<int>(std::size(arguments))))) return {};
    const std::wstring command = L"browser " + std::wstring(arguments);
    int count = 0;
    wchar_t** argv = CommandLineToArgvW(command.c_str(), &count);
    if (!argv) return {};
    std::wstring found;
    for (int i = 1; i < count; ++i)
    {
        std::wstring_view argument(argv[i]);
        if (argument.starts_with(L"--app=")) argument.remove_prefix(6);
        if (auto url = HttpUrl(argument); !url.empty())
        {
            if (!found.empty() && found != url) { found.clear(); break; }
            found = std::move(url);
        }
    }
    LocalFree(argv);
    return found;
}

std::optional<Shortcut> Capture(const std::filesystem::path& path)
{
    Shortcut result;
    result.path = path;
    if (!ReadContents(path, result.contents, result.identity) ||
        (result.identity.dwFileAttributes & FILE_ATTRIBUTE_READONLY)) return {};
    result.url = ReadUrl(path);
    if (result.url.empty() || !SameFile(result)) return {};
    return result;
}

std::vector<std::wstring> FindCandidates(std::string_view html, const std::wstring& pageUrl)
{
    struct Candidate { std::string href; unsigned score; };
    std::vector<Candidate> links;
    std::wstring base = pageUrl;
    bool baseSeen = false;
    html = html.substr(0, std::min(html.size(), maxShortcutBytes));
    const auto lower = Lower(html);
    size_t p = 0;
    while ((p = html.find('<', p)) != html.npos && links.size() < 32)
    {
        if (html.substr(p, 4) == "<!--")
        {
            const auto end = html.find("-->", p + 4);
            p = end == html.npos ? html.size() : end + 3;
            continue;
        }
        ++p;
        const auto start = p;
        while (p < html.size() && !Space(html[p]) && html[p] != '>') ++p;
        const auto tag = lower.substr(start, p - start);
        if (tag == "/head" || tag == "body") break;
        std::map<std::string, std::string> attributes;
        while (p < html.size() && html[p] != '>')
        {
            while (p < html.size() && (Space(html[p]) || html[p] == '/')) ++p;
            const auto nameStart = p;
            while (p < html.size() && !Space(html[p]) && html[p] != '=' && html[p] != '>') ++p;
            if (p == nameStart) break;
            auto name = lower.substr(nameStart, p - nameStart);
            while (p < html.size() && Space(html[p])) ++p;
            std::string value;
            if (p < html.size() && html[p] == '=')
            {
                ++p;
                while (p < html.size() && Space(html[p])) ++p;
                const char quote = p < html.size() && (html[p] == '\'' || html[p] == '"') ? html[p++] : 0;
                const auto valueStart = p;
                while (p < html.size() && (quote ? html[p] != quote : !Space(html[p]) && html[p] != '>')) ++p;
                value = html.substr(valueStart, p - valueStart);
                if (quote && p < html.size()) ++p;
            }
            attributes.emplace(std::move(name), std::move(value));
        }
        if (p < html.size()) ++p;
        if (tag == "script" || tag == "style" || tag == "title" || tag == "textarea")
        {
            const auto end = lower.find("</" + tag, p);
            p = end == lower.npos ? html.size() : end;
            continue;
        }
        if (tag == "base" && !baseSeen && attributes.contains("href"))
        {
            baseSeen = true;
            if (auto resolved = Combine(pageUrl, AttributeUrl(attributes["href"])); !resolved.empty()) base = std::move(resolved);
        }
        if (tag != "link" || !attributes.contains("href")) continue;
        const bool icon = Token(attributes["rel"], "icon");
        const bool touch = Token(attributes["rel"], "apple-touch-icon") || Token(attributes["rel"], "apple-touch-icon-precomposed");
        if (!icon && !touch) continue;
        if (Lower(attributes["type"]) == "image/svg+xml") continue; // WIC does not decode SVG.
        unsigned size = 0;
        for (const char c : attributes["sizes"])
        {
            if (c < '0' || c > '9') break;
            size = std::min(1024u, size * 10 + c - '0');
        }
        links.push_back({attributes["href"], (icon ? 2048u : 0u) + size});
    }
    std::stable_sort(links.begin(), links.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    std::vector<std::wstring> result;
    for (const auto& link : links)
    {
        auto url = Combine(base, AttributeUrl(link.href));
        if (!url.empty() && std::find(result.begin(), result.end(), url) == result.end()) result.push_back(std::move(url));
        if (result.size() == 7) break;
    }
    auto fallback = Combine(pageUrl, L"/favicon.ico");
    if (!fallback.empty() && std::find(result.begin(), result.end(), fallback) == result.end()) result.push_back(std::move(fallback));
    return result;
}

std::string ConvertToIco(std::span<const std::byte> image)
{
    if (image.empty() || image.size() > maxImageBytes) return {};
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromMemory(
            reinterpret_cast<BYTE*>(const_cast<std::byte*>(image.data())), static_cast<DWORD>(image.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) return {};
    GUID format{};
    if (FAILED(decoder->GetContainerFormat(&format)) ||
        (format != GUID_ContainerFormatPng && format != GUID_ContainerFormatIco &&
         format != GUID_ContainerFormatJpeg && format != GUID_ContainerFormatGif && format != GUID_ContainerFormatBmp)) return {};
    UINT frameCount = 0, bestEdge = 0;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrameCount(&frameCount))) return {};
    for (UINT i = 0; i < std::min(frameCount, format == GUID_ContainerFormatIco ? 256u : 1u); ++i)
    {
        ComPtr<IWICBitmapFrameDecode> candidate;
        UINT w = 0, h = 0;
        if (SUCCEEDED(decoder->GetFrame(i, &candidate)) && SUCCEEDED(candidate->GetSize(&w, &h)) &&
            w && h && std::uint64_t(w) * h <= 16 * 1024 * 1024 && std::max(w, h) > bestEdge)
        { frame = candidate; bestEdge = std::max(w, h); }
    }
    if (!frame) return {};
    UINT width = 0, height = 0;
    frame->GetSize(&width, &height);
    std::vector<std::pair<UINT, std::string>> frames;
    for (const UINT side : {16u, 32u, 48u, 64u, 128u, 256u})
    {
        if (side > bestEdge && !frames.empty()) break;
        const UINT w = std::max(1u, side * width / bestEdge), h = std::max(1u, side * height / bestEdge);
        ComPtr<IWICBitmapScaler> scaler;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(factory->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(frame.Get(), w, h, WICBitmapInterpolationModeFant)) ||
            FAILED(factory->CreateFormatConverter(&converter)) || FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return {};
        std::vector<std::uint32_t> pixels(static_cast<size_t>(w) * h);
        if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size() * 4), reinterpret_cast<BYTE*>(pixels.data())))) return {};
        if (std::none_of(pixels.begin(), pixels.end(), [](auto pixel) { return (pixel >> 24) != 0; })) return {};
        std::string dib;
        const UINT maskStride = ((side + 31) / 32) * 4;
        AppendLe(dib, 40, 4); AppendLe(dib, side, 4); AppendLe(dib, side * 2, 4);
        AppendLe(dib, 1, 2); AppendLe(dib, 32, 2); AppendLe(dib, 0, 4);
        AppendLe(dib, side * side * 4 + maskStride * side, 4);
        dib.append(16, '\0');
        std::string mask(maskStride * side, '\0');
        for (UINT row = 0; row < side; ++row)
            for (UINT x = 0; x < side; ++x)
            {
                const UINT y = side - row - 1, left = (side - w) / 2, top = (side - h) / 2;
                const auto pixel = x >= left && x < left + w && y >= top && y < top + h ? pixels[(y - top) * w + x - left] : 0;
                AppendLe(dib, pixel, 4);
                if (!(pixel >> 24)) mask[row * maskStride + x / 8] |= static_cast<char>(0x80 >> (x % 8));
            }
        dib += mask;
        frames.emplace_back(side, std::move(dib));
    }
    std::string result;
    AppendLe(result, 0, 2); AppendLe(result, 1, 2); AppendLe(result, static_cast<UINT>(frames.size()), 2);
    UINT offset = 6 + static_cast<UINT>(frames.size()) * 16;
    for (const auto& [side, bytes] : frames)
    {
        AppendLe(result, side == 256 ? 0 : side, 1); AppendLe(result, side == 256 ? 0 : side, 1);
        AppendLe(result, 0, 2); AppendLe(result, 1, 2); AppendLe(result, 32, 2);
        AppendLe(result, static_cast<UINT>(bytes.size()), 4); AppendLe(result, offset, 4);
        offset += static_cast<UINT>(bytes.size());
    }
    for (const auto& [_, bytes] : frames) result += bytes;
    return result;
}

std::filesystem::path Fetch(const std::wstring& url, const std::filesystem::path& directory, std::stop_token stop)
{
    if (HttpUrl(url).empty()) return {};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    const auto download = [&](const std::wstring& address, size_t limit, std::string& bytes, std::wstring& finalUrl) {
        bytes.clear();
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (stop.stop_requested() || remaining < 1000) return false;
        http_stream::Options options;
        options.url = address; options.maximumResponseBytes = limit;
        options.totalTimeoutMs = static_cast<int>(std::min<std::int64_t>(remaining, 6000));
        options.timeoutMs = std::min(options.totalTimeoutMs, 3000);
        const auto result = http_stream::StreamHttpGet(options, stop,
            [&](const auto& head) { finalUrl = head.finalUrl; return head.status == 200 && !HttpUrl(head.finalUrl).empty(); },
            [&](auto chunk) {
                if (chunk.size() > limit - bytes.size()) return false;
                bytes.append(reinterpret_cast<const char*>(chunk.data()), chunk.size()); return true;
            });
        if (!result.head.finalUrl.empty()) finalUrl = result.head.finalUrl;
        return result.responseAccepted && result.error.empty() && !result.cancelled;
    };
    std::string html;
    std::wstring pageUrl = url;
    if (!download(url, maxShortcutBytes, html, pageUrl)) html.clear();
    auto candidates = FindCandidates(html, pageUrl);
    // A failed redirect must not discard the original site's conventional icon.
    const auto originalFallback = Combine(url, L"/favicon.ico");
    if (!originalFallback.empty() && std::find(candidates.begin(), candidates.end(), originalFallback) == candidates.end()) candidates.push_back(originalFallback);
    for (const auto& candidate : candidates)
    {
        std::string bytes;
        std::wstring finalUrl;
        if (!download(candidate, maxImageBytes, bytes, finalUrl)) continue;
        auto ico = ConvertToIco(std::as_bytes(std::span(bytes)));
        if (ico.empty() || stop.stop_requested()) continue;
        const auto name = UniqueName();
        if (name.empty()) return {};
        const auto output = directory / (name + L".ico");
        if (atomic_file::WriteAll(output, ico)) return output;
        return {};
    }
    return {};
}

bool Apply(const Shortcut& shortcut, const std::filesystem::path& icon)
{
    if (!icon.is_absolute() || !SameFile(shortcut) || ReadUrl(shortcut.path) != shortcut.url) return false;
    const DWORD iconAttributes = GetFileAttributesW(icon.c_str());
    if (iconAttributes == INVALID_FILE_ATTRIBUTES || (iconAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    const auto name = UniqueName();
    if (name.empty()) return false;
    TemporaryFile temporary{shortcut.path.parent_path() / (L".snowdesktop-" + name + shortcut.path.extension().wstring())};
    if (!CopyFileW(shortcut.path.c_str(), temporary.path.c_str(), TRUE)) return false;
    if (rules::HasExtension(shortcut.path.wstring(), L".url"))
    {
        // Profile APIs preserve unrelated sections and the shortcut URL. Convert
        // ANSI/UTF-8 input to UTF-16 first so a Unicode icon path is not lossy.
        std::string input = shortcut.contents;
        if (!(input.size() >= 2 && static_cast<unsigned char>(input[0]) == 0xff && static_cast<unsigned char>(input[1]) == 0xfe))
        {
            if (input.starts_with("\xef\xbb\xbf")) input.erase(0, 3);
            auto wide = Wide(input);
            if (wide.empty())
            {
                const int count = MultiByteToWideChar(CP_ACP, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
                wide.resize(count);
                MultiByteToWideChar(CP_ACP, 0, input.data(), static_cast<int>(input.size()), wide.data(), count);
            }
            const std::string unicode = std::string("\xff\xfe", 2) + std::string(reinterpret_cast<const char*>(wide.data()), wide.size() * sizeof(wchar_t));
            if (!atomic_file::WriteAll(temporary.path, unicode)) return false;
        }
        if (!WritePrivateProfileStringW(L"InternetShortcut", L"IconFile", icon.c_str(), temporary.path.c_str()) ||
            !WritePrivateProfileStringW(L"InternetShortcut", L"IconIndex", L"0", temporary.path.c_str())) return false;
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, temporary.path.c_str());
    }
    else
    {
        ComPtr<IShellLinkW> link;
        ComPtr<IPersistFile> file;
        if (!LoadLink(temporary.path, link, file) || FAILED(link->SetIconLocation(icon.c_str(), 0)) ||
            FAILED(file->Save(temporary.path.c_str(), TRUE))) return false;
    }
    if (ReadUrl(temporary.path) != shortcut.url || !SameFile(shortcut)) return false;
    if (!ReplaceFileW(shortcut.path.c_str(), temporary.path.c_str(), nullptr, 0, nullptr, nullptr)) return false;
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, shortcut.path.c_str());
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, shortcut.path.c_str(), nullptr);
    return true;
}
}

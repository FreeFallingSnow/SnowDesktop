#include "website_icon.h"
#include "shortcut_icon_resource.h"
#include "atomic_file.h"
#include "preview_png_writer.h"
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <iostream>

namespace
{
int failures = 0;
void Check(bool passed, const char* reason)
{
    if (!passed) { ++failures; std::cerr << "FAILED: " << reason << '\n'; }
}
bool MakeLink(const std::filesystem::path& path, const wchar_t* executable,
    const wchar_t* arguments, const wchar_t* icon = L"")
{
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> file;
    return SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
        SUCCEEDED(link->SetPath(executable)) && SUCCEEDED(link->SetArguments(arguments)) &&
        SUCCEEDED(link->SetDescription(L"Keep this description")) && SUCCEEDED(link->SetIconLocation(icon, 0)) &&
        SUCCEEDED(link.As(&file)) && SUCCEEDED(file->Save(path.c_str(), TRUE));
}
}

int RunWebsiteIconTests()
{
    namespace fs = std::filesystem;
    namespace site = snowdesktop::website_icon;
    using namespace snowdesktop;
    const auto root = fs::temp_directory_path() / (L"SnowDesktop-website-icon-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(root);
    const auto url = root / L"bilibili.url";
    const std::string original = "[InternetShortcut]\r\nURL=https://www.bilibili.com/\r\nHotKey=0\r\n[Extra]\r\nKeep=yes\r\n";
    Check(atomic_file::WriteAll(url, original), "create website shortcut fixture");
    Check(site::ReadUrl(url) == L"https://www.bilibili.com/", "recognize a website shortcut with no explicit icon");
    const auto snapshot = site::Capture(url);
    Check(snapshot.has_value(), "capture the target and file identity before downloading");
    const auto nonWeb = root / L"game.url";
    atomic_file::WriteAll(nonWeb, "[InternetShortcut]\nURL=steam://rungameid/570\n");
    Check(site::ReadUrl(nonWeb).empty(), "do not offer website icons for Steam protocol shortcuts");

    const auto candidates = site::FindCandidates(R"html(
        <!-- <link rel=icon href='https://wrong.example/comment.ico'> -->
        <script>const fake = '<link rel=icon href="https://wrong.example/script.ico">';</script>
        <link REL='shortcut ICON' sizes='192x192' href='logo.png?a=1&amp;b=2'>
        <base href='/assets/'>
        <link rel='apple-touch-icon' href='//cdn.example/touch.png'>
        <link rel='icon' type='image/svg+xml' href='icon.svg'>
        <link rel='icon' href='javascript:alert(1)'>
        </head><link rel=icon href='/body.ico'>
    )html", L"https://example.com/start/page");
    Check(candidates == std::vector<std::wstring>{L"https://example.com/assets/logo.png?a=1&b=2", L"https://cdn.example/touch.png", L"https://example.com/favicon.ico"},
        "resolve HTML base, relative/CDN paths and entities while ignoring comments, script, SVG and unsafe schemes");
    Check(site::FindCandidates("<link rel=not-icon href='/wrong.ico'>", L"https://example.com/a") ==
        std::vector<std::wstring>{L"https://example.com/favicon.ico"}, "match rel tokens exactly and retain root fallback");
    Check(site::FindCandidates("<link rel=icon href='/favicon.ico'><link rel=ICON href='/favicon.ico'>", L"https://example.com").size() == 1,
        "deduplicate icon candidates and the fallback");
    Check(site::FindCandidates("<link rel=icon href='/i.png?a=1&#38;b=2'>", L"https://example.com").front() == L"https://example.com/i.png?a=1&b=2",
        "decode numeric entities in icon addresses");
    const std::string invalid = "<html>not an image</html>";
    Check(site::ConvertToIco(std::as_bytes(std::span(invalid))).empty(), "reject an HTML error response masquerading as an icon");
    std::stop_source cancelled;
    cancelled.request_stop();
    Check(site::Fetch(L"https://example.com", root, cancelled.get_token()).empty(), "cancel without downloading or modifying a shortcut");

    const auto png = root / L"source.png";
    std::string error, bytes;
    Check(preview_png::Save(png, 128, 64, std::vector<std::uint32_t>(128 * 64, 0xff44aa22), error), "create a non-square real PNG fixture");
    atomic_file::ReadAll(png, bytes);
    const auto ico = site::ConvertToIco(std::as_bytes(std::span(bytes)));
    const auto icon = root / L"网站图标.ico";
    Check(!ico.empty() && atomic_file::WriteAll(icon, ico), "convert PNG to a persistent multi-size ICO with a Unicode path");
    Microsoft::WRL::ComPtr<IWICImagingFactory> imaging;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&imaging))) &&
        SUCCEEDED(imaging->CreateDecoderFromFilename(icon.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)),
        "Windows decodes the generated ICO");
    if (decoder)
    {
        UINT count = 0;
        decoder->GetFrameCount(&count);
        Check(count == 5, "keep 16, 32, 48, 64 and 128 pixel frames without enlarging the source");
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        // ICO decoders may reorder directory entries. Validate the intended
        // dimensions instead of assuming the last frame is the largest.
        for (UINT i = 0; i < count; ++i)
        {
            Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> candidate;
            UINT w = 0, h = 0;
            if (SUCCEEDED(decoder->GetFrame(i, &candidate)) &&
                SUCCEEDED(candidate->GetSize(&w, &h)) && w == 128 && h == 128)
            { frame = candidate; break; }
        }
        Check(frame != nullptr, "the ICO contains the largest unscaled source frame");
        imaging->CreateFormatConverter(&converter);
        const HRESULT converted = frame ? converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom) : E_FAIL;
        std::vector<std::uint32_t> pixels(128 * 128);
        const bool copied = SUCCEEDED(converted) && SUCCEEDED(converter->CopyPixels(nullptr, 128 * 4,
            static_cast<UINT>(pixels.size() * 4), reinterpret_cast<BYTE*>(pixels.data())));
        if (!copied || pixels[0] != 0 || pixels[64 * 128 + 64] != 0xff44aa22)
            std::cerr << "ICO sample pixels: copied=" << copied << ", corner=0x" << std::hex << pixels[0]
                << ", center=0x" << pixels[64 * 128 + 64] << std::dec << '\n';
        Check(copied &&
            pixels[0] == 0 && pixels[64 * 128 + 64] == 0xff44aa22, "preserve color and transparent padding without distorting a rectangular logo");
    }
    decoder.Reset();
    Check(snapshot && site::Apply(*snapshot, icon), "replace URL icon only after successful download");
    const auto resource = shortcut_icon_resource::ReadInternetShortcutIconResource(url.wstring());
    Check(resource && resource->path == icon.wstring() && resource->index == 0 && site::ReadUrl(url) == L"https://www.bilibili.com/",
        "persist the exact Unicode icon path while keeping the original website URL");
    wchar_t value[32]{};
    GetPrivateProfileStringW(L"Extra", L"Keep", L"", value, 32, url.c_str());
    Check(std::wstring(value) == L"yes", "preserve unrelated shortcut metadata");
    Check(snapshot && !site::Apply(*snapshot, icon), "reject stale completion after a shortcut has changed");
    const auto current = site::Capture(url);
    WritePrivateProfileStringW(L"InternetShortcut", L"URL", L"https://example.org", url.c_str());
    Check(current && !site::Apply(*current, icon) && site::ReadUrl(url) == L"https://example.org", "a download cannot overwrite a concurrently changed target");
    const auto missing = site::Capture(url);
    fs::remove(url);
    Check(missing && !site::Apply(*missing, icon) && !fs::exists(url), "do not recreate a deleted shortcut when download completes");
    atomic_file::WriteAll(url, original);
    SetFileAttributesW(url.c_str(), FILE_ATTRIBUTE_READONLY);
    Check(!site::Capture(url), "fail safely on a read-only shortcut");
    SetFileAttributesW(url.c_str(), FILE_ATTRIBUTE_NORMAL);

    const auto linkPath = root / L"website.lnk";
    Check(MakeLink(linkPath, L"C:\\Browser\\chrome.exe", L"--profile-directory=Default --app=\"https://example.com/app\""), "create a browser web-app shortcut");
    Check(site::ReadUrl(linkPath) == L"https://example.com/app", "recognize a browser --app URL");
    const auto linkSnapshot = site::Capture(linkPath);
    Check(linkSnapshot && site::Apply(*linkSnapshot, icon), "set a browser link icon while preserving its command");
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> file;
    CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    link.As(&file); file->Load(linkPath.c_str(), STGM_READ);
    wchar_t arguments[512]{}, description[128]{};
    link->GetArguments(arguments, 512); link->GetDescription(description, 128);
    Check(std::wstring(arguments) == L"--profile-directory=Default --app=\"https://example.com/app\"" &&
        std::wstring(description) == L"Keep this description", "preserve browser arguments and the link description");
    const auto resources = shortcut_icon_resource::ReadShortcutIconResources(linkPath.wstring());
    Check(resources.size() == 2 && resources[0].path == icon.wstring(), "large icons prefer the link's explicit icon before its executable");
    file.Reset(); link.Reset();
    MakeLink(linkPath, L"C:\\Browser\\chrome.exe", L"https://one.example https://two.example");
    Check(site::ReadUrl(linkPath).empty(), "exclude ambiguous browser links that open multiple sites");
    MakeLink(linkPath, L"C:\\Tools\\tool.exe", L"https://example.com");
    Check(site::ReadUrl(linkPath).empty(), "do not treat arbitrary URL arguments as browser shortcuts");

    if (root.parent_path() == fs::temp_directory_path() && root.filename().wstring().starts_with(L"SnowDesktop-website-icon-")) fs::remove_all(root);
    return failures;
}

// Opt-in reproduction against a user's real shortcut. Only the copy in the
// supplied evidence directory is modified; this is never run by normal CTest.
int RunWebsiteIconProbe(const wchar_t* shortcutPath, const wchar_t* outputDirectory)
{
    namespace fs = std::filesystem;
    namespace site = snowdesktop::website_icon;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const fs::path output = fs::absolute(outputDirectory);
    fs::create_directories(output);
    const auto copy = output / (L"website" + fs::path(shortcutPath).extension().wstring());
    if (!CopyFileW(shortcutPath, copy.c_str(), TRUE)) return 1;
    const auto snapshot = site::Capture(copy);
    if (!snapshot) return 2;
    const auto icon = site::Fetch(snapshot->url, output, {});
    const bool applied = !icon.empty() && site::Apply(*snapshot, icon);
    std::wcout << L"Website: " << snapshot->url << L"\nCopy: " << copy.wstring()
        << L"\nIcon: " << icon.wstring() << L"\nApplied: " << applied << L"\n";
    if (SUCCEEDED(initialized)) CoUninitialize();
    return applied ? 0 : 3;
}
